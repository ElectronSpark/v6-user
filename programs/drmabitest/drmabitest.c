#include "kernel/inc/types.h"
#include "kernel/inc/errno.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/syscall.h"
#include "kernel/inc/uabi/drm.h"
#include "kernel/inc/uabi/fcntl.h"
#include "kernel/inc/uabi/poll.h"
#include "user/user.h"

#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define VIRGL_CCMD_NOP 0
#define VIRGL_CMD0(cmd, obj, len) ((cmd) | ((obj) << 8) | ((len) << 16))
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW (1u << 3)
#define VIRGL_BIND_DISPLAY_TARGET (1u << 7)
#define PIPE_TEXTURE_2D 2

struct drm_node {
    const char *name;
    const char *path;
    int fd;
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct cap_case {
    uint64 cap;
    const char *name;
};

struct simple_ioctl {
    uint64 request;
    const char *name;
    void *arg;
};

static int saved_errno(int ret)
{
    return ret < 0 ? -ret : 0;
}

static void print_ret(const char *node, const char *name, int ret)
{
    printf("%s:%s: ret=%d errno=%d\n", node, name, ret, saved_errno(ret));
}

static void print_ret_u64(const char *node, const char *name, int ret,
                          uint64 value)
{
    printf("%s:%s: ret=%d errno=%d value=%lu\n",
           node, name, ret, saved_errno(ret), value);
}

static void print_ret_u32(const char *node, const char *name, int ret,
                          uint32 value)
{
    printf("%s:%s: ret=%d errno=%d value=%u\n",
           node, name, ret, saved_errno(ret), value);
}

static int call_ioctl(int fd, uint64 request, void *arg)
{
    return ioctl(fd, (int)request, arg);
}

#if defined(__riscv)
static inline int64 raw_syscall2(int num, int64 a, int64 b)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
#elif defined(__x86_64__)
static inline int64 raw_syscall2(int num, int64 a, int64 b)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return ret;
}
#else
#error "raw_syscall3 is not defined for this architecture"
#endif

static int poll_raw(struct pollfd *fds, int nfds, int timeout)
{
    return (int)raw_syscall3(SYS_poll, (int64)fds, nfds, timeout);
}

static int eventfd2_raw(uint32 initval, int flags)
{
    return (int)raw_syscall2(SYS_eventfd2, initval, flags);
}

static void probe_version(struct drm_node *node)
{
    struct drm_version_compat ver;
    char name[64];
    char date[64];
    char desc[128];
    int ret;

    memset(&ver, 0, sizeof(ver));
    ret = call_ioctl(node->fd, DRM_IOCTL_VERSION, &ver);
    printf("%s:DRM_IOCTL_VERSION.probe: ret=%d errno=%d major=%d minor=%d "
           "patch=%d name_len=%lu date_len=%lu desc_len=%lu\n",
           node->name, ret, saved_errno(ret), ver.version_major,
           ver.version_minor, ver.version_patchlevel, ver.name_len,
           ver.date_len, ver.desc_len);

    memset(name, 0, sizeof(name));
    memset(date, 0, sizeof(date));
    memset(desc, 0, sizeof(desc));
    ver.name = (uint64)name;
    ver.name_len = sizeof(name);
    ver.date = (uint64)date;
    ver.date_len = sizeof(date);
    ver.desc = (uint64)desc;
    ver.desc_len = sizeof(desc);
    ret = call_ioctl(node->fd, DRM_IOCTL_VERSION, &ver);
    printf("%s:DRM_IOCTL_VERSION.fill: ret=%d errno=%d major=%d minor=%d "
           "patch=%d name=\"%s\" date=\"%s\" desc=\"%s\"\n",
           node->name, ret, saved_errno(ret), ver.version_major,
           ver.version_minor, ver.version_patchlevel, name, date, desc);
}

static void probe_unique(struct drm_node *node)
{
    struct drm_unique_compat req;
    char unique[128];
    int ret;

    memset(&req, 0, sizeof(req));
    ret = call_ioctl(node->fd, DRM_IOCTL_GET_UNIQUE, &req);
    print_ret_u64(node->name, "DRM_IOCTL_GET_UNIQUE.probe", ret,
                  req.unique_len);

    memset(unique, 0, sizeof(unique));
    req.unique = (uint64)unique;
    req.unique_len = sizeof(unique);
    ret = call_ioctl(node->fd, DRM_IOCTL_GET_UNIQUE, &req);
    printf("%s:DRM_IOCTL_GET_UNIQUE.fill: ret=%d errno=%d unique_len=%lu "
           "unique=\"%s\"\n",
           node->name, ret, saved_errno(ret), req.unique_len, unique);
}

static void probe_caps(struct drm_node *node)
{
    static const struct cap_case caps[] = {
        { DRM_CAP_DUMB_BUFFER, "DUMB_BUFFER" },
        { DRM_CAP_VBLANK_HIGH_CRTC, "VBLANK_HIGH_CRTC" },
        { DRM_CAP_DUMB_PREFERRED_DEPTH, "DUMB_PREFERRED_DEPTH" },
        { DRM_CAP_DUMB_PREFER_SHADOW, "DUMB_PREFER_SHADOW" },
        { DRM_CAP_PRIME, "PRIME" },
        { DRM_CAP_TIMESTAMP_MONOTONIC, "TIMESTAMP_MONOTONIC" },
        { DRM_CAP_ASYNC_PAGE_FLIP, "ASYNC_PAGE_FLIP" },
        { DRM_CAP_CURSOR_WIDTH, "CURSOR_WIDTH" },
        { DRM_CAP_CURSOR_HEIGHT, "CURSOR_HEIGHT" },
        { DRM_CAP_ADDFB2_MODIFIERS, "ADDFB2_MODIFIERS" },
        { DRM_CAP_PAGE_FLIP_TARGET, "PAGE_FLIP_TARGET" },
        { DRM_CAP_CRTC_IN_VBLANK_EVENT, "CRTC_IN_VBLANK_EVENT" },
        { DRM_CAP_SYNCOBJ, "SYNCOBJ" },
        { DRM_CAP_SYNCOBJ_TIMELINE, "SYNCOBJ_TIMELINE" },
        { DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, "ATOMIC_ASYNC_PAGE_FLIP" },
        { 0xffffffffULL, "INVALID" },
    };

    for (int i = 0; i < ARRAY_SIZE(caps); i++) {
        struct drm_get_cap_compat req;
        int ret;

        memset(&req, 0, sizeof(req));
        req.capability = caps[i].cap;
        ret = call_ioctl(node->fd, DRM_IOCTL_GET_CAP, &req);
        printf("%s:DRM_IOCTL_GET_CAP[%s]: ret=%d errno=%d value=%lu\n",
               node->name, caps[i].name, ret, saved_errno(ret), req.value);
    }
}

static void probe_client_caps(struct drm_node *node)
{
    static const uint64 client_caps[] = {
        DRM_CLIENT_CAP_UNIVERSAL_PLANES,
        DRM_CLIENT_CAP_ATOMIC,
        DRM_CLIENT_CAP_ASPECT_RATIO,
        DRM_CLIENT_CAP_WRITEBACK_CONNECTORS,
        0xffffffffULL,
    };

    for (int i = 0; i < ARRAY_SIZE(client_caps); i++) {
        struct drm_set_client_cap_compat req;
        int ret;

        memset(&req, 0, sizeof(req));
        req.capability = client_caps[i];
        req.value = 1;
        ret = call_ioctl(node->fd, DRM_IOCTL_SET_CLIENT_CAP, &req);
        printf("%s:DRM_IOCTL_SET_CLIENT_CAP[%lu]: ret=%d errno=%d\n",
               node->name, client_caps[i], ret, saved_errno(ret));
    }
}

static void probe_core_misc(struct drm_node *node)
{
    struct drm_auth_compat auth;
    struct drm_client_compat client;
    struct drm_set_version_compat set_version;
    struct drm_set_client_name_compat client_name;
    char name[] = "drmabitest";
    uint64 scratch[32];
    int ret;

    memset(&auth, 0, sizeof(auth));
    ret = call_ioctl(node->fd, DRM_IOCTL_GET_MAGIC, &auth);
    print_ret_u32(node->name, "DRM_IOCTL_GET_MAGIC", ret, auth.magic);
    ret = call_ioctl(node->fd, DRM_IOCTL_AUTH_MAGIC, &auth);
    print_ret(node->name, "DRM_IOCTL_AUTH_MAGIC.self", ret);

    memset(&client, 0, sizeof(client));
    client.idx = 0;
    ret = call_ioctl(node->fd, DRM_IOCTL_GET_CLIENT, &client);
    printf("%s:DRM_IOCTL_GET_CLIENT[0]: ret=%d errno=%d auth=%d pid=%lu "
           "uid=%lu magic=%lu iocs=%lu\n",
           node->name, ret, saved_errno(ret), client.auth, client.pid,
           client.uid, client.magic, client.iocs);

    memset(&set_version, 0, sizeof(set_version));
    set_version.drm_di_major = 1;
    set_version.drm_di_minor = 4;
    ret = call_ioctl(node->fd, DRM_IOCTL_SET_VERSION, &set_version);
    printf("%s:DRM_IOCTL_SET_VERSION: ret=%d errno=%d di=%d.%d dd=%d.%d\n",
           node->name, ret, saved_errno(ret), set_version.drm_di_major,
           set_version.drm_di_minor, set_version.drm_dd_major,
           set_version.drm_dd_minor);

    memset(&client_name, 0, sizeof(client_name));
    client_name.name = (uint64)name;
    client_name.name_len = strlen(name);
    print_ret(node->name, "DRM_IOCTL_SET_CLIENT_NAME",
              call_ioctl(node->fd, DRM_IOCTL_SET_CLIENT_NAME, &client_name));

    print_ret(node->name, "DRM_IOCTL_SET_MASTER",
              call_ioctl(node->fd, DRM_IOCTL_SET_MASTER, scratch));
    print_ret(node->name, "DRM_IOCTL_DROP_MASTER",
              call_ioctl(node->fd, DRM_IOCTL_DROP_MASTER, scratch));
}

static void probe_legacy_core_stubs(struct drm_node *node)
{
    static const struct simple_ioctl ioctls[] = {
        { DRM_IOCTL_GET_MAP, "DRM_IOCTL_GET_MAP", 0 },
        { DRM_IOCTL_GET_STATS, "DRM_IOCTL_GET_STATS", 0 },
        { DRM_IOCTL_ADD_MAP, "DRM_IOCTL_ADD_MAP", 0 },
        { DRM_IOCTL_ADD_BUFS, "DRM_IOCTL_ADD_BUFS", 0 },
        { DRM_IOCTL_MARK_BUFS, "DRM_IOCTL_MARK_BUFS", 0 },
        { DRM_IOCTL_INFO_BUFS, "DRM_IOCTL_INFO_BUFS", 0 },
        { DRM_IOCTL_MAP_BUFS, "DRM_IOCTL_MAP_BUFS", 0 },
        { DRM_IOCTL_FREE_BUFS, "DRM_IOCTL_FREE_BUFS", 0 },
        { DRM_IOCTL_RM_MAP, "DRM_IOCTL_RM_MAP", 0 },
        { DRM_IOCTL_SET_SAREA_CTX, "DRM_IOCTL_SET_SAREA_CTX", 0 },
        { DRM_IOCTL_GET_SAREA_CTX, "DRM_IOCTL_GET_SAREA_CTX", 0 },
        { DRM_IOCTL_ADD_CTX, "DRM_IOCTL_ADD_CTX", 0 },
        { DRM_IOCTL_RM_CTX, "DRM_IOCTL_RM_CTX", 0 },
        { DRM_IOCTL_MOD_CTX, "DRM_IOCTL_MOD_CTX", 0 },
        { DRM_IOCTL_GET_CTX, "DRM_IOCTL_GET_CTX", 0 },
        { DRM_IOCTL_SWITCH_CTX, "DRM_IOCTL_SWITCH_CTX", 0 },
        { DRM_IOCTL_NEW_CTX, "DRM_IOCTL_NEW_CTX", 0 },
        { DRM_IOCTL_RES_CTX, "DRM_IOCTL_RES_CTX", 0 },
        { DRM_IOCTL_DMA, "DRM_IOCTL_DMA", 0 },
        { DRM_IOCTL_LOCK, "DRM_IOCTL_LOCK", 0 },
        { DRM_IOCTL_UNLOCK, "DRM_IOCTL_UNLOCK", 0 },
        { DRM_IOCTL_FINISH, "DRM_IOCTL_FINISH", 0 },
        { DRM_IOCTL_AGP_ACQUIRE, "DRM_IOCTL_AGP_ACQUIRE", 0 },
        { DRM_IOCTL_AGP_RELEASE, "DRM_IOCTL_AGP_RELEASE", 0 },
        { DRM_IOCTL_AGP_ENABLE, "DRM_IOCTL_AGP_ENABLE", 0 },
        { DRM_IOCTL_AGP_INFO, "DRM_IOCTL_AGP_INFO", 0 },
        { DRM_IOCTL_AGP_ALLOC, "DRM_IOCTL_AGP_ALLOC", 0 },
        { DRM_IOCTL_AGP_FREE, "DRM_IOCTL_AGP_FREE", 0 },
        { DRM_IOCTL_AGP_BIND, "DRM_IOCTL_AGP_BIND", 0 },
        { DRM_IOCTL_AGP_UNBIND, "DRM_IOCTL_AGP_UNBIND", 0 },
        { DRM_IOCTL_SG_ALLOC, "DRM_IOCTL_SG_ALLOC", 0 },
        { DRM_IOCTL_SG_FREE, "DRM_IOCTL_SG_FREE", 0 },
    };
    uint64 scratch[32];

    memset(scratch, 0, sizeof(scratch));
    for (int i = 0; i < ARRAY_SIZE(ioctls); i++)
        print_ret(node->name, ioctls[i].name,
                  call_ioctl(node->fd, ioctls[i].request, scratch));
}

static uint32 first_id(uint32 *ids, uint32 count)
{
    return count > 0 ? ids[0] : 0;
}

static void probe_connector(struct drm_node *node, uint32 connector_id)
{
    struct drm_mode_get_connector_compat conn;
    uint32 encoders[8];
    uint32 props[32];
    uint64 values[32];
    struct drm_mode_modeinfo_compat modes[16];
    int ret;

    memset(&conn, 0, sizeof(conn));
    conn.connector_id = connector_id;
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
    printf("%s:DRM_IOCTL_MODE_GETCONNECTOR.probe: ret=%d errno=%d id=%u "
           "modes=%u props=%u encoders=%u connection=%u\n",
           node->name, ret, saved_errno(ret), connector_id, conn.count_modes,
           conn.count_props, conn.count_encoders, conn.connection);

    memset(encoders, 0, sizeof(encoders));
    memset(props, 0, sizeof(props));
    memset(values, 0, sizeof(values));
    memset(modes, 0, sizeof(modes));
    conn.encoders_ptr = (uint64)encoders;
    conn.count_encoders = ARRAY_SIZE(encoders);
    conn.props_ptr = (uint64)props;
    conn.prop_values_ptr = (uint64)values;
    conn.count_props = ARRAY_SIZE(props);
    conn.modes_ptr = (uint64)modes;
    conn.count_modes = ARRAY_SIZE(modes);
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
    printf("%s:DRM_IOCTL_MODE_GETCONNECTOR.fill: ret=%d errno=%d id=%u "
           "modes=%u props=%u encoders=%u encoder_id=%u mode0=\"%s\"\n",
           node->name, ret, saved_errno(ret), connector_id, conn.count_modes,
           conn.count_props, conn.count_encoders, conn.encoder_id,
           conn.count_modes > 0 ? modes[0].name : "");
    if (ret == 0) {
        uint32 mode_print_count = conn.count_modes;
        int preferred_count = 0;

        if (mode_print_count > ARRAY_SIZE(modes))
            mode_print_count = ARRAY_SIZE(modes);
        for (uint32 i = 0; i < mode_print_count; i++) {
            if ((modes[i].type & DRM_MODE_TYPE_PREFERRED) != 0)
                preferred_count++;
            printf("%s:DRM_IOCTL_MODE_GETCONNECTOR.mode%u: name=\"%s\" "
                   "size=%ux%u refresh=%u type=0x%x flags=0x%x\n",
                   node->name, i, modes[i].name, modes[i].hdisplay,
                   modes[i].vdisplay, modes[i].vrefresh, modes[i].type,
                   modes[i].flags);
        }
        printf("%s:DRM_CONNECTOR_MODES.valid: modes=%u preferred=%d\n",
               node->name, conn.count_modes, preferred_count);
    }
}

static void probe_property(struct drm_node *node, uint32 prop_id)
{
    struct drm_mode_get_property_compat prop;
    uint64 values[16];
    struct drm_mode_property_enum_compat enums[16];
    int ret;

    memset(&prop, 0, sizeof(prop));
    prop.prop_id = prop_id;
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETPROPERTY, &prop);
    printf("%s:DRM_IOCTL_MODE_GETPROPERTY.probe: ret=%d errno=%d id=%u "
           "flags=0x%x values=%u enums=%u name=\"%s\"\n",
           node->name, ret, saved_errno(ret), prop_id, prop.flags,
           prop.count_values, prop.count_enum_blobs, prop.name);

    memset(values, 0, sizeof(values));
    memset(enums, 0, sizeof(enums));
    prop.values_ptr = (uint64)values;
    prop.count_values = ARRAY_SIZE(values);
    prop.enum_blob_ptr = (uint64)enums;
    prop.count_enum_blobs = ARRAY_SIZE(enums);
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETPROPERTY, &prop);
    printf("%s:DRM_IOCTL_MODE_GETPROPERTY.fill: ret=%d errno=%d id=%u "
           "flags=0x%x values=%u enums=%u name=\"%s\"\n",
           node->name, ret, saved_errno(ret), prop_id, prop.flags,
           prop.count_values, prop.count_enum_blobs, prop.name);
}

static void probe_obj_props(struct drm_node *node, uint32 obj_id,
                            uint32 obj_type, const char *label)
{
    struct drm_mode_obj_get_properties_compat req;
    uint32 props[32];
    uint64 values[32];
    int ret;

    memset(&req, 0, sizeof(req));
    req.obj_id = obj_id;
    req.obj_type = obj_type;
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req);
    printf("%s:DRM_IOCTL_MODE_OBJ_GETPROPERTIES.%s.probe: ret=%d errno=%d "
           "id=%u count=%u\n",
           node->name, label, ret, saved_errno(ret), obj_id, req.count_props);

    memset(props, 0, sizeof(props));
    memset(values, 0, sizeof(values));
    req.props_ptr = (uint64)props;
    req.prop_values_ptr = (uint64)values;
    req.count_props = ARRAY_SIZE(props);
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req);
    printf("%s:DRM_IOCTL_MODE_OBJ_GETPROPERTIES.%s.fill: ret=%d errno=%d "
           "id=%u count=%u\n",
           node->name, label, ret, saved_errno(ret), obj_id, req.count_props);

    if (ret == 0 && req.count_props > 0)
        probe_property(node, props[0]);
}

static uint32 find_obj_prop_named(struct drm_node *node, uint32 obj_id,
                                  uint32 obj_type, const char *name)
{
    struct drm_mode_obj_get_properties_compat req;
    uint32 props[32];
    uint64 values[32];

    memset(&req, 0, sizeof(req));
    memset(props, 0, sizeof(props));
    memset(values, 0, sizeof(values));
    req.obj_id = obj_id;
    req.obj_type = obj_type;
    req.props_ptr = (uint64)props;
    req.prop_values_ptr = (uint64)values;
    req.count_props = ARRAY_SIZE(props);
    if (call_ioctl(node->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req) != 0)
        return 0;
    for (uint32 i = 0; i < req.count_props && i < ARRAY_SIZE(props); i++) {
        struct drm_mode_get_property_compat prop;

        memset(&prop, 0, sizeof(prop));
        prop.prop_id = props[i];
        if (call_ioctl(node->fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) == 0 &&
            strcmp(prop.name, name) == 0)
            return props[i];
    }
    return 0;
}

static int get_obj_prop_value_named(struct drm_node *node, uint32 obj_id,
                                    uint32 obj_type, const char *name,
                                    uint64 *value)
{
    struct drm_mode_obj_get_properties_compat req;
    uint32 props[32];
    uint64 values[32];

    if (value == NULL)
        return -EINVAL;
    memset(&req, 0, sizeof(req));
    memset(props, 0, sizeof(props));
    memset(values, 0, sizeof(values));
    req.obj_id = obj_id;
    req.obj_type = obj_type;
    req.props_ptr = (uint64)props;
    req.prop_values_ptr = (uint64)values;
    req.count_props = ARRAY_SIZE(props);
    if (call_ioctl(node->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req) != 0)
        return -1;
    for (uint32 i = 0; i < req.count_props && i < ARRAY_SIZE(props); i++) {
        struct drm_mode_get_property_compat prop;

        memset(&prop, 0, sizeof(prop));
        prop.prop_id = props[i];
        if (call_ioctl(node->fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) == 0 &&
            strcmp(prop.name, name) == 0) {
            *value = values[i];
            return 0;
        }
    }
    return -ENOENT;
}

static int atomic_commit_props(struct drm_node *node, uint32 *objs,
                               uint32 *counts, uint32 count_objs,
                               uint32 *props, uint64 *values,
                               uint32 flags)
{
    struct drm_mode_atomic_compat atomic;

    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = flags;
    atomic.count_objs = count_objs;
    atomic.objs_ptr = (uint64)objs;
    atomic.count_props_ptr = (uint64)counts;
    atomic.props_ptr = (uint64)props;
    atomic.prop_values_ptr = (uint64)values;
    return call_ioctl(node->fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
}

static int get_plane_fb_id(struct drm_node *node, uint32 plane_id,
                           uint32 *fb_id)
{
    struct drm_mode_get_plane_compat plane;

    if (fb_id == NULL)
        return -EINVAL;
    memset(&plane, 0, sizeof(plane));
    plane.plane_id = plane_id;
    if (call_ioctl(node->fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0)
        return -1;
    *fb_id = plane.fb_id;
    return 0;
}

static void probe_plane(struct drm_node *node, uint32 plane_id)
{
    struct drm_mode_get_plane_compat plane;
    uint32 formats[16];
    int ret;

    memset(&plane, 0, sizeof(plane));
    plane.plane_id = plane_id;
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETPLANE, &plane);
    printf("%s:DRM_IOCTL_MODE_GETPLANE.probe: ret=%d errno=%d id=%u "
           "formats=%u crtc=%u fb=%u possible=0x%x\n",
           node->name, ret, saved_errno(ret), plane_id,
           plane.count_format_types, plane.crtc_id, plane.fb_id,
           plane.possible_crtcs);

    memset(formats, 0, sizeof(formats));
    plane.format_type_ptr = (uint64)formats;
    plane.count_format_types = ARRAY_SIZE(formats);
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETPLANE, &plane);
    printf("%s:DRM_IOCTL_MODE_GETPLANE.fill: ret=%d errno=%d id=%u "
           "formats=%u format0=0x%x\n",
           node->name, ret, saved_errno(ret), plane_id,
           plane.count_format_types, plane.count_format_types > 0 ? formats[0] : 0);
}

static void probe_kms(struct drm_node *node)
{
    struct drm_mode_card_res_compat res;
    struct drm_mode_get_plane_res_compat plane_res;
    uint32 crtcs[8];
    uint32 connectors[8];
    uint32 encoders[8];
    uint32 fbs[16];
    uint32 planes[8];
    int ret;

    memset(&res, 0, sizeof(res));
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    printf("%s:DRM_IOCTL_MODE_GETRESOURCES.probe: ret=%d errno=%d "
           "fbs=%u crtcs=%u connectors=%u encoders=%u size=%ux%u..%ux%u\n",
           node->name, ret, saved_errno(ret), res.count_fbs, res.count_crtcs,
           res.count_connectors, res.count_encoders, res.min_width,
           res.min_height, res.max_width, res.max_height);

    memset(crtcs, 0, sizeof(crtcs));
    memset(connectors, 0, sizeof(connectors));
    memset(encoders, 0, sizeof(encoders));
    memset(fbs, 0, sizeof(fbs));
    res.fb_id_ptr = (uint64)fbs;
    res.count_fbs = ARRAY_SIZE(fbs);
    res.crtc_id_ptr = (uint64)crtcs;
    res.count_crtcs = ARRAY_SIZE(crtcs);
    res.connector_id_ptr = (uint64)connectors;
    res.count_connectors = ARRAY_SIZE(connectors);
    res.encoder_id_ptr = (uint64)encoders;
    res.count_encoders = ARRAY_SIZE(encoders);
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    printf("%s:DRM_IOCTL_MODE_GETRESOURCES.fill: ret=%d errno=%d "
           "fbs=%u crtcs=%u connectors=%u encoders=%u crtc0=%u conn0=%u "
           "enc0=%u fb0=%u\n",
           node->name, ret, saved_errno(ret), res.count_fbs, res.count_crtcs,
           res.count_connectors, res.count_encoders, first_id(crtcs, res.count_crtcs),
           first_id(connectors, res.count_connectors),
           first_id(encoders, res.count_encoders), first_id(fbs, res.count_fbs));

    if (res.count_crtcs > 0) {
        struct drm_mode_crtc_compat crtc;
        struct drm_crtc_get_sequence_compat seq;
        union drm_wait_vblank_compat wait_vblank;
        struct drm_crtc_queue_sequence_compat queue_seq;
        uint64 start_sequence;
        int64 start_ns;

        memset(&crtc, 0, sizeof(crtc));
        crtc.crtc_id = crtcs[0];
        ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETCRTC, &crtc);
        printf("%s:DRM_IOCTL_MODE_GETCRTC: ret=%d errno=%d id=%u fb=%u "
               "mode_valid=%u mode=\"%s\"\n",
               node->name, ret, saved_errno(ret), crtcs[0], crtc.fb_id,
               crtc.mode_valid, crtc.mode.name);

        memset(&seq, 0, sizeof(seq));
        seq.crtc_id = crtcs[0];
        ret = call_ioctl(node->fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &seq);
        printf("%s:DRM_IOCTL_CRTC_GET_SEQUENCE: ret=%d errno=%d id=%u "
               "active=%u sequence=%lu ns=%ld\n",
               node->name, ret, saved_errno(ret), crtcs[0], seq.active,
               seq.sequence, seq.sequence_ns);
        start_sequence = seq.sequence;
        start_ns = seq.sequence_ns;

        memset(&wait_vblank, 0, sizeof(wait_vblank));
        wait_vblank.request.sequence = (uint32)start_sequence;
        ret = call_ioctl(node->fd, DRM_IOCTL_WAIT_VBLANK, &wait_vblank);
        printf("%s:DRM_IOCTL_WAIT_VBLANK.real_present: ret=%d errno=%d "
               "start=%lu reply=%u advanced=%d start_ns=%ld reply_us=%ld "
               "monotonic=%d\n",
               node->name, ret, saved_errno(ret), start_sequence,
               wait_vblank.reply.sequence,
               ret == 0 && wait_vblank.reply.sequence > start_sequence,
               start_ns,
               (int64)wait_vblank.reply.tval_sec * 1000000LL +
                   wait_vblank.reply.tval_usec,
               ret == 0 &&
                   ((int64)wait_vblank.reply.tval_sec * 1000000000LL +
                    wait_vblank.reply.tval_usec * 1000LL) >= start_ns);

        memset(&queue_seq, 0, sizeof(queue_seq));
        queue_seq.crtc_id = crtcs[0];
        queue_seq.flags = DRM_CRTC_SEQUENCE_RELATIVE;
        queue_seq.sequence = 1;
        ret = call_ioctl(node->fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &queue_seq);
        print_ret_u64(node->name, "DRM_IOCTL_CRTC_QUEUE_SEQUENCE.relative",
                      ret, queue_seq.sequence);

        probe_obj_props(node, crtcs[0], DRM_MODE_OBJECT_CRTC, "crtc");
    }

    if (res.count_connectors > 0) {
        probe_connector(node, connectors[0]);
        probe_obj_props(node, connectors[0], DRM_MODE_OBJECT_CONNECTOR,
                        "connector");
    }

    if (res.count_encoders > 0) {
        struct drm_mode_get_encoder_compat enc;

        memset(&enc, 0, sizeof(enc));
        enc.encoder_id = encoders[0];
        ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETENCODER, &enc);
        printf("%s:DRM_IOCTL_MODE_GETENCODER: ret=%d errno=%d id=%u "
               "type=%u crtc=%u possible=0x%x\n",
               node->name, ret, saved_errno(ret), encoders[0],
               enc.encoder_type, enc.crtc_id, enc.possible_crtcs);
    }

    memset(&plane_res, 0, sizeof(plane_res));
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res);
    printf("%s:DRM_IOCTL_MODE_GETPLANERESOURCES.probe: ret=%d errno=%d "
           "planes=%u\n",
           node->name, ret, saved_errno(ret), plane_res.count_planes);

    memset(planes, 0, sizeof(planes));
    plane_res.plane_id_ptr = (uint64)planes;
    plane_res.count_planes = ARRAY_SIZE(planes);
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res);
    printf("%s:DRM_IOCTL_MODE_GETPLANERESOURCES.fill: ret=%d errno=%d "
           "planes=%u plane0=%u plane1=%u\n",
           node->name, ret, saved_errno(ret), plane_res.count_planes,
           first_id(planes, plane_res.count_planes),
           plane_res.count_planes > 1 ? planes[1] : 0);

    for (uint32 i = 0; ret == 0 && i < plane_res.count_planes &&
         i < ARRAY_SIZE(planes); i++) {
        probe_plane(node, planes[i]);
        probe_obj_props(node, planes[i], DRM_MODE_OBJECT_PLANE,
                        i == 0 ? "plane" : "plane_extra");
    }
}

static void probe_kms_invalids(struct drm_node *node)
{
    struct drm_mode_crtc_compat crtc;
    struct drm_mode_crtc_lut_compat gamma;
    struct drm_mode_get_blob_compat blob;
    struct drm_mode_get_property_compat prop;
    struct drm_mode_obj_get_properties_compat obj_props;
    struct drm_mode_obj_set_property_compat obj_set;
    struct drm_mode_fb_dirty_cmd_compat dirtyfb;
    struct drm_mode_create_lease_compat create_lease;
    struct drm_mode_list_lessees_compat list_lessees;
    struct drm_mode_get_lease_compat get_lease;
    struct drm_mode_revoke_lease_compat revoke_lease;
    struct drm_mode_closefb_compat closefb;

    memset(&crtc, 0, sizeof(crtc));
    print_ret(node->name, "DRM_IOCTL_MODE_SETCRTC.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_SETCRTC, &crtc));

    memset(&gamma, 0, sizeof(gamma));
    print_ret(node->name, "DRM_IOCTL_MODE_GETGAMMA.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_GETGAMMA, &gamma));
    print_ret(node->name, "DRM_IOCTL_MODE_SETGAMMA.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_SETGAMMA, &gamma));

    memset(&blob, 0, sizeof(blob));
    print_ret(node->name, "DRM_IOCTL_MODE_GETPROPBLOB.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob));

    memset(&prop, 0, sizeof(prop));
    print_ret(node->name, "DRM_IOCTL_MODE_GETPROPERTY.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_GETPROPERTY, &prop));

    memset(&obj_props, 0, sizeof(obj_props));
    print_ret(node->name, "DRM_IOCTL_MODE_OBJ_GETPROPERTIES.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES,
                         &obj_props));

    memset(&obj_set, 0, sizeof(obj_set));
    print_ret(node->name, "DRM_IOCTL_MODE_OBJ_SETPROPERTY.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &obj_set));

    memset(&dirtyfb, 0, sizeof(dirtyfb));
    print_ret(node->name, "DRM_IOCTL_MODE_DIRTYFB.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_DIRTYFB, &dirtyfb));

    memset(&create_lease, 0, sizeof(create_lease));
    print_ret(node->name, "DRM_IOCTL_MODE_CREATE_LEASE.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_CREATE_LEASE,
                         &create_lease));

    memset(&list_lessees, 0, sizeof(list_lessees));
    print_ret(node->name, "DRM_IOCTL_MODE_LIST_LESSEES.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_LIST_LESSEES,
                         &list_lessees));

    memset(&get_lease, 0, sizeof(get_lease));
    print_ret(node->name, "DRM_IOCTL_MODE_GET_LEASE.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_GET_LEASE, &get_lease));

    memset(&revoke_lease, 0, sizeof(revoke_lease));
    print_ret(node->name, "DRM_IOCTL_MODE_REVOKE_LEASE.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_REVOKE_LEASE,
                         &revoke_lease));

    memset(&closefb, 0, sizeof(closefb));
    print_ret(node->name, "DRM_IOCTL_MODE_CLOSEFB.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_CLOSEFB, &closefb));
}

static void probe_prop_blobs(struct drm_node *node)
{
    static const uint8 mode_blob_payload[64] = {
        0x21, 0x43, 0x65, 0x87, 0xaa, 0x55, 0x19, 0x83,
        0x10, 0x32, 0x54, 0x76, 0xfe, 0xdc, 0xba, 0x98,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
        0x42, 0x24, 0x66, 0x99, 0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88, 0x13, 0x57, 0x9b, 0xdf,
        0x20, 0x26, 0x06, 0x06, 0x12, 0x34, 0x56, 0x78,
        0x87, 0x65, 0x43, 0x21, 0xbe, 0xef, 0xca, 0xfe,
    };
    uint8 readback[sizeof(mode_blob_payload)];
    struct drm_mode_create_blob_compat create;
    struct drm_mode_get_blob_compat get_probe;
    struct drm_mode_get_blob_compat get_read;
    struct drm_mode_get_blob_compat get_after_destroy;
    struct drm_mode_destroy_blob_compat destroy;
    int create_ret;
    int probe_ret;
    int read_ret;
    int destroy_ret;
    int after_destroy_ret;
    int match = 0;

    memset(readback, 0, sizeof(readback));
    memset(&create, 0, sizeof(create));
    create.data = (uint64)mode_blob_payload;
    create.length = sizeof(mode_blob_payload);
    create_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &create);

    memset(&get_probe, 0, sizeof(get_probe));
    get_probe.blob_id = create.blob_id;
    probe_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETPROPBLOB, &get_probe);

    memset(&get_read, 0, sizeof(get_read));
    get_read.blob_id = create.blob_id;
    get_read.data = (uint64)readback;
    get_read.length = sizeof(readback);
    read_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETPROPBLOB, &get_read);
    if (read_ret == 0 && get_read.length == sizeof(mode_blob_payload) &&
        memcmp(readback, mode_blob_payload, sizeof(mode_blob_payload)) == 0)
        match = 1;

    memset(&destroy, 0, sizeof(destroy));
    destroy.blob_id = create.blob_id;
    destroy_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_DESTROYPROPBLOB,
                             &destroy);

    memset(&get_after_destroy, 0, sizeof(get_after_destroy));
    get_after_destroy.blob_id = create.blob_id;
    after_destroy_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETPROPBLOB,
                                   &get_after_destroy);

    printf("%s:DRM_IOCTL_MODE_CREATEPROPBLOB.mode_id_roundtrip: "
           "create=%d create_errno=%d blob=%u probe=%d probe_errno=%d "
           "probe_len=%u read=%d read_errno=%d read_len=%u match=%d "
           "destroy=%d destroy_errno=%d after_destroy=%d "
           "after_destroy_errno=%d\n",
           node->name, create_ret, saved_errno(create_ret), create.blob_id,
           probe_ret, saved_errno(probe_ret),
           probe_ret == 0 ? get_probe.length : 0,
           read_ret, saved_errno(read_ret),
           read_ret == 0 ? get_read.length : 0,
           match, destroy_ret, saved_errno(destroy_ret), after_destroy_ret,
           saved_errno(after_destroy_ret));
}

static void probe_syncobj(struct drm_node *node)
{
    struct drm_syncobj_create_compat create;
    struct drm_syncobj_array_compat array;
    struct drm_syncobj_timeline_array_compat timeline_array;
    struct drm_syncobj_wait_compat wait_req;
    struct drm_syncobj_timeline_wait_compat timeline_wait;
    struct drm_syncobj_handle_compat handle_fd;
    struct drm_syncobj_transfer_compat transfer;
    struct drm_syncobj_eventfd_compat eventfd;
    struct drm_syncobj_destroy_compat destroy;
    uint32 handles[1];
    uint64 points[1];
    int ret;

    memset(&create, 0, sizeof(create));
    create.flags = DRM_SYNCOBJ_CREATE_SIGNALED;
    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_CREATE, &create);
    print_ret_u32(node->name, "DRM_IOCTL_SYNCOBJ_CREATE.signaled", ret,
                  create.handle);
    if (ret < 0 || create.handle == 0)
        return;

    handles[0] = create.handle;
    points[0] = 1;

    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)handles;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req);
    print_ret_u32(node->name, "DRM_IOCTL_SYNCOBJ_WAIT.signaled", ret,
                  wait_req.first_signaled);

    memset(&timeline_wait, 0, sizeof(timeline_wait));
    timeline_wait.handles = (uint64)handles;
    timeline_wait.points = (uint64)points;
    timeline_wait.count_handles = 1;
    timeline_wait.timeout_nsec = 0;
    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &timeline_wait);
    print_ret_u32(node->name, "DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT.point1", ret,
                  timeline_wait.first_signaled);

    memset(&timeline_array, 0, sizeof(timeline_array));
    timeline_array.handles = (uint64)handles;
    timeline_array.points = (uint64)points;
    timeline_array.count_handles = 1;
    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_QUERY, &timeline_array);
    printf("%s:DRM_IOCTL_SYNCOBJ_QUERY: ret=%d errno=%d point0=%lu\n",
           node->name, ret, saved_errno(ret), points[0]);

    memset(&array, 0, sizeof(array));
    array.handles = (uint64)handles;
    array.count_handles = 1;
    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_RESET, &array);
    print_ret(node->name, "DRM_IOCTL_SYNCOBJ_RESET", ret);

    if (ret == 0) {
        int child = fork();

        if (child == 0) {
            sleep(50);
            ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &array);
            printf("%s:DRM_IOCTL_SYNCOBJ_SIGNAL.blocking_child: ret=%d "
                   "errno=%d\n",
                   node->name, ret, saved_errno(ret));
            exit(ret == 0 ? 0 : 1);
        } else if (child < 0) {
            printf("%s:DRM_IOCTL_SYNCOBJ_WAIT.blocking: ret=-1 errno=%d "
                   "child=-1\n",
                   node->name, EAGAIN);
        } else {
            int child_status = -1;

            memset(&wait_req, 0, sizeof(wait_req));
            wait_req.handles = (uint64)handles;
            wait_req.count_handles = 1;
            wait_req.timeout_nsec = 2000000000LL;
            ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req);
            wait(&child_status);
            printf("%s:DRM_IOCTL_SYNCOBJ_WAIT.blocking: ret=%d errno=%d "
                   "first=%u child_status=%d\n",
                   node->name, ret, saved_errno(ret),
                   wait_req.first_signaled, child_status);
        }
    }

    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &array);
    print_ret(node->name, "DRM_IOCTL_SYNCOBJ_SIGNAL", ret);

    memset(&timeline_array, 0, sizeof(timeline_array));
    points[0] = 2;
    timeline_array.handles = (uint64)handles;
    timeline_array.points = (uint64)points;
    timeline_array.count_handles = 1;
    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL,
                     &timeline_array);
    print_ret(node->name, "DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL.point2", ret);

    memset(&transfer, 0, sizeof(transfer));
    transfer.src_handle = create.handle;
    transfer.dst_handle = create.handle;
    transfer.src_point = 2;
    transfer.dst_point = 3;
    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer);
    print_ret(node->name, "DRM_IOCTL_SYNCOBJ_TRANSFER.self", ret);

    {
        struct drm_syncobj_create_compat pending_src;
        struct drm_syncobj_create_compat pending_dst;
        struct drm_syncobj_transfer_compat pending_transfer;
        struct drm_syncobj_timeline_array_compat pending_query;
        struct drm_syncobj_destroy_compat pending_destroy;
        uint32 pending_handles[1];
        uint64 pending_signaled_points[1];
        uint64 pending_submitted_points[1];
        int ret_src;
        int ret_dst;
        int ret_transfer = -1;
        int ret_signaled = -1;
        int ret_submitted = -1;

        memset(&pending_src, 0, sizeof(pending_src));
        memset(&pending_dst, 0, sizeof(pending_dst));
        pending_signaled_points[0] = 0;
        pending_submitted_points[0] = 0;
        ret_src = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_CREATE,
                             &pending_src);
        ret_dst = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_CREATE,
                             &pending_dst);
        if (ret_src == 0 && ret_dst == 0) {
            memset(&pending_transfer, 0, sizeof(pending_transfer));
            pending_transfer.src_handle = pending_src.handle;
            pending_transfer.dst_handle = pending_dst.handle;
            pending_transfer.src_point = 1;
            pending_transfer.dst_point = 5;
            ret_transfer = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_TRANSFER,
                                      &pending_transfer);
            if (ret_transfer == 0) {
                pending_handles[0] = pending_dst.handle;
                pending_signaled_points[0] = 99;
                memset(&pending_query, 0, sizeof(pending_query));
                pending_query.handles = (uint64)pending_handles;
                pending_query.points = (uint64)pending_signaled_points;
                pending_query.count_handles = 1;
                ret_signaled = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_QUERY,
                                          &pending_query);

                pending_submitted_points[0] = 0;
                memset(&pending_query, 0, sizeof(pending_query));
                pending_query.handles = (uint64)pending_handles;
                pending_query.points = (uint64)pending_submitted_points;
                pending_query.count_handles = 1;
                pending_query.flags =
                    DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED;
                ret_submitted = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_QUERY,
                                           &pending_query);
            }
        }
        printf("%s:DRM_IOCTL_SYNCOBJ_QUERY.pending_transfer: "
               "create_src=%d create_dst=%d transfer=%d signaled_ret=%d "
               "submitted_ret=%d signaled=%lu submitted=%lu "
               "expect_signaled=0 expect_submitted=5\n",
               node->name, ret_src, ret_dst, ret_transfer, ret_signaled,
               ret_submitted, pending_signaled_points[0],
               pending_submitted_points[0]);
        if (ret_signaled == 0 && ret_submitted == 0 &&
            (pending_signaled_points[0] != 0 ||
             pending_submitted_points[0] != 5))
            printf("%s:DRM_IOCTL_SYNCOBJ_QUERY.pending_transfer: FAIL\n",
                   node->name);
        if (ret_dst == 0) {
            memset(&pending_destroy, 0, sizeof(pending_destroy));
            pending_destroy.handle = pending_dst.handle;
            print_ret(node->name, "DRM_IOCTL_SYNCOBJ_DESTROY.pending_dst",
                      call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_DESTROY,
                                 &pending_destroy));
        }
        if (ret_src == 0) {
            memset(&pending_destroy, 0, sizeof(pending_destroy));
            pending_destroy.handle = pending_src.handle;
            print_ret(node->name, "DRM_IOCTL_SYNCOBJ_DESTROY.pending_src",
                      call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_DESTROY,
                                 &pending_destroy));
        }
    }

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = create.handle;
    handle_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd);
    printf("%s:DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD.sync_file: ret=%d errno=%d "
           "fd=%d\n",
           node->name, ret, saved_errno(ret), handle_fd.fd);
    if (ret == 0 && handle_fd.fd >= 0) {
        struct drm_syncobj_handle_compat import_fd;

        memset(&import_fd, 0, sizeof(import_fd));
        import_fd.fd = handle_fd.fd;
        import_fd.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
        ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import_fd);
        printf("%s:DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE.sync_file: ret=%d "
               "errno=%d handle=%u\n",
               node->name, ret, saved_errno(ret), import_fd.handle);
        if (ret == 0 && import_fd.handle != 0) {
            struct drm_syncobj_destroy_compat destroy_import;

            memset(&destroy_import, 0, sizeof(destroy_import));
            destroy_import.handle = import_fd.handle;
            print_ret(node->name, "DRM_IOCTL_SYNCOBJ_DESTROY.imported",
                      call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_DESTROY,
                                 &destroy_import));
        }
        close(handle_fd.fd);
    }

    {
        struct drm_syncobj_create_compat poll_create;
        struct drm_syncobj_array_compat poll_array;
        struct drm_syncobj_handle_compat poll_fd;
        struct drm_syncobj_destroy_compat poll_destroy;
        struct pollfd pfd;
        int create_ret;
        int export_ret = -1;
        int poll0_ret = -1;
        int poll_ret = -1;
        int child_status = -1;
        short poll0_revents = 0;

        memset(&poll_create, 0, sizeof(poll_create));
        create_ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_CREATE,
                                &poll_create);
        memset(&poll_fd, 0, sizeof(poll_fd));
        if (create_ret == 0) {
            poll_fd.handle = poll_create.handle;
            poll_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
            export_ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD,
                                    &poll_fd);
        }
        if (export_ret == 0 && poll_fd.fd >= 0) {
            memset(&pfd, 0, sizeof(pfd));
            pfd.fd = poll_fd.fd;
            pfd.events = POLLIN;
            poll0_ret = poll_raw(&pfd, 1, 0);
            poll0_revents = pfd.revents;

            int child = fork();

            if (child == 0) {
                sleep(50);
                memset(&poll_array, 0, sizeof(poll_array));
                handles[0] = poll_create.handle;
                poll_array.handles = (uint64)handles;
                poll_array.count_handles = 1;
                ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_SIGNAL,
                                 &poll_array);
                printf("%s:DRM_IOCTL_SYNCOBJ_SIGNAL.poll_child: ret=%d "
                       "errno=%d\n",
                       node->name, ret, saved_errno(ret));
                exit(ret == 0 ? 0 : 1);
            } else if (child >= 0) {
                memset(&pfd, 0, sizeof(pfd));
                pfd.fd = poll_fd.fd;
                pfd.events = POLLIN;
                poll_ret = poll_raw(&pfd, 1, 2000);
                wait(&child_status);
            } else {
                child_status = -EAGAIN;
            }
            printf("%s:DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD.poll_blocking: "
                   "create=%d export=%d poll0=%d poll0_revents=0x%x "
                   "poll=%d errno=%d revents=0x%x child_status=%d\n",
                   node->name, create_ret, export_ret, poll0_ret,
                   (uint32)poll0_revents, poll_ret, saved_errno(poll_ret),
                   (uint32)pfd.revents, child_status);
            close(poll_fd.fd);
        } else {
            printf("%s:DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD.poll_blocking: "
                   "create=%d export=%d fd=%d\n",
                   node->name, create_ret, export_ret, poll_fd.fd);
        }
        if (create_ret == 0) {
            memset(&poll_destroy, 0, sizeof(poll_destroy));
            poll_destroy.handle = poll_create.handle;
            print_ret(node->name, "DRM_IOCTL_SYNCOBJ_DESTROY.poll_source",
                      call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_DESTROY,
                                 &poll_destroy));
        }
    }

    {
        struct drm_syncobj_create_compat ev_create;
        struct drm_syncobj_eventfd_compat ev_req;
        struct drm_syncobj_array_compat ev_array;
        struct drm_syncobj_destroy_compat ev_destroy;
        struct pollfd pfd;
        uint64 event_value = 0;
        int create_ret;
        int event_fd = -1;
        int arm_ret = -1;
        int poll0_ret = -1;
        int poll_ret = -1;
        int read_ret = -1;
        int child_status = -1;
        short poll0_revents = 0;

        memset(&ev_create, 0, sizeof(ev_create));
        create_ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_CREATE,
                                &ev_create);
        if (create_ret == 0)
            event_fd = eventfd2_raw(0, 0);
        if (create_ret == 0 && event_fd >= 0) {
            memset(&ev_req, 0, sizeof(ev_req));
            ev_req.handle = ev_create.handle;
            ev_req.fd = event_fd;
            arm_ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_EVENTFD,
                                 &ev_req);
        }
        if (arm_ret == 0) {
            memset(&pfd, 0, sizeof(pfd));
            pfd.fd = event_fd;
            pfd.events = POLLIN;
            poll0_ret = poll_raw(&pfd, 1, 0);
            poll0_revents = pfd.revents;

            int child = fork();

            if (child == 0) {
                sleep(50);
                memset(&ev_array, 0, sizeof(ev_array));
                handles[0] = ev_create.handle;
                ev_array.handles = (uint64)handles;
                ev_array.count_handles = 1;
                ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_SIGNAL,
                                 &ev_array);
                printf("%s:DRM_IOCTL_SYNCOBJ_SIGNAL.eventfd_child: ret=%d "
                       "errno=%d\n",
                       node->name, ret, saved_errno(ret));
                exit(ret == 0 ? 0 : 1);
            } else if (child >= 0) {
                memset(&pfd, 0, sizeof(pfd));
                pfd.fd = event_fd;
                pfd.events = POLLIN;
                poll_ret = poll_raw(&pfd, 1, 2000);
                if (poll_ret > 0)
                    read_ret = read(event_fd, &event_value,
                                    sizeof(event_value));
                wait(&child_status);
            } else {
                child_status = -EAGAIN;
            }
            printf("%s:DRM_IOCTL_SYNCOBJ_EVENTFD.blocking: create=%d "
                   "eventfd=%d arm=%d poll0=%d poll0_revents=0x%x poll=%d "
                   "errno=%d revents=0x%x read=%d value=%lu "
                   "child_status=%d\n",
                   node->name, create_ret, event_fd, arm_ret, poll0_ret,
                   (uint32)poll0_revents, poll_ret, saved_errno(poll_ret),
                   (uint32)pfd.revents, read_ret, event_value, child_status);
        } else {
            printf("%s:DRM_IOCTL_SYNCOBJ_EVENTFD.blocking: create=%d "
                   "eventfd=%d arm=%d errno=%d\n",
                   node->name, create_ret, event_fd, arm_ret,
                   saved_errno(arm_ret));
        }
        if (event_fd >= 0)
            close(event_fd);
        if (create_ret == 0) {
            memset(&ev_destroy, 0, sizeof(ev_destroy));
            ev_destroy.handle = ev_create.handle;
            print_ret(node->name, "DRM_IOCTL_SYNCOBJ_DESTROY.eventfd_source",
                      call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_DESTROY,
                                 &ev_destroy));
        }
    }

    memset(&eventfd, 0, sizeof(eventfd));
    eventfd.handle = create.handle;
    eventfd.fd = -1;
    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &eventfd);
    print_ret(node->name, "DRM_IOCTL_SYNCOBJ_EVENTFD.invalid_fd", ret);

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    print_ret(node->name, "DRM_IOCTL_SYNCOBJ_DESTROY", ret);
}

static void probe_dumb_bo(struct drm_node *node)
{
    struct drm_mode_create_dumb_compat create;
    struct drm_mode_map_dumb_compat map;
    struct drm_prime_handle_compat prime;
    struct drm_gem_flink_compat flink;
    struct drm_gem_close_compat close_req;
    struct drm_mode_destroy_dumb_compat destroy;
    int prime_fd = -1;
    int ret;

    memset(&create, 0, sizeof(create));
    create.width = 64;
    create.height = 64;
    create.bpp = 32;
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
    printf("%s:DRM_IOCTL_MODE_CREATE_DUMB.valid: ret=%d errno=%d handle=%u "
           "pitch=%u size=%lu\n",
           node->name, ret, saved_errno(ret), create.handle, create.pitch,
           create.size);
    if (ret < 0 || create.handle == 0)
        return;

    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    print_ret_u64(node->name, "DRM_IOCTL_MODE_MAP_DUMB.valid", ret,
                  map.offset);

    memset(&prime, 0, sizeof(prime));
    prime.handle = create.handle;
    ret = call_ioctl(node->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
    printf("%s:DRM_IOCTL_PRIME_HANDLE_TO_FD.valid: ret=%d errno=%d fd=%d\n",
           node->name, ret, saved_errno(ret), prime.fd);
    if (ret == 0 && prime.fd >= 0)
        prime_fd = prime.fd;

    if (prime_fd >= 0) {
        struct drm_prime_handle_compat import_req;

        memset(&import_req, 0, sizeof(import_req));
        import_req.fd = prime_fd;
        ret = call_ioctl(node->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import_req);
        printf("%s:DRM_IOCTL_PRIME_FD_TO_HANDLE.valid: ret=%d errno=%d "
               "handle=%u\n",
               node->name, ret, saved_errno(ret), import_req.handle);
        if (import_req.handle != 0 && import_req.handle != create.handle) {
            memset(&close_req, 0, sizeof(close_req));
            close_req.handle = import_req.handle;
            print_ret(node->name, "DRM_IOCTL_GEM_CLOSE.imported",
                      call_ioctl(node->fd, DRM_IOCTL_GEM_CLOSE, &close_req));
        }
        close(prime_fd);
    }

    memset(&prime, 0, sizeof(prime));
    prime.handle = create.handle;
    ret = call_ioctl(node->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
    if (ret == 0 && prime.fd >= 0) {
        uint32 *parent_pixels;
        uint32 parent_first = 0;
        uint32 parent_last = 0;
        uint32 pattern_first = 0xff224466;
        uint32 pattern_last = 0xff6688aa;
        int child_status = -1;
        int child = fork();

        memset(&map, 0, sizeof(map));
        map.handle = create.handle;
        ret = call_ioctl(node->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
        parent_pixels = ret == 0 ?
            mmap(0, (int)create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                 node->fd, map.offset) : MAP_FAILED;
        if (parent_pixels != MAP_FAILED) {
            parent_pixels[0] = pattern_first;
            parent_pixels[(create.height - 1) * (create.pitch / 4) +
                          (create.width - 1)] = pattern_last;
        }

        if (child == 0) {
            int child_fd = open(node->path, 0);
            struct drm_mode_create_dumb_compat child_dummy;
            struct drm_mode_map_dumb_compat child_map;
            struct drm_prime_handle_compat child_import;
            struct drm_mode_destroy_dumb_compat child_destroy;
            struct drm_gem_close_compat child_close;
            uint32 *child_pixels = MAP_FAILED;
            uint32 child_first = 0;
            uint32 child_last = 0;
            int dummy_ret;
            int import_ret;
            int map_ret = -999;
            int close_ret = -999;
            int destroy_ret = -999;

            memset(&child_dummy, 0, sizeof(child_dummy));
            child_dummy.width = 16;
            child_dummy.height = 16;
            child_dummy.bpp = 32;
            dummy_ret = child_fd >= 0 ?
                call_ioctl(child_fd, DRM_IOCTL_MODE_CREATE_DUMB,
                           &child_dummy) : -EBADF;
            memset(&child_import, 0, sizeof(child_import));
            child_import.fd = prime.fd;
            import_ret = child_fd >= 0 ?
                call_ioctl(child_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE,
                           &child_import) : -EBADF;
            if (import_ret == 0) {
                memset(&child_map, 0, sizeof(child_map));
                child_map.handle = child_import.handle;
                map_ret = call_ioctl(child_fd, DRM_IOCTL_MODE_MAP_DUMB,
                                     &child_map);
                if (map_ret == 0) {
                    child_pixels = mmap(0, (int)create.size,
                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                        child_fd, child_map.offset);
                    if (child_pixels != MAP_FAILED) {
                        child_first = child_pixels[0];
                        child_last =
                            child_pixels[(create.height - 1) *
                                         (create.pitch / 4) +
                                         (create.width - 1)];
                        child_pixels[1] = 0xffabcdef;
                    }
                }
            }
            if (import_ret == 0) {
                memset(&child_close, 0, sizeof(child_close));
                child_close.handle = child_import.handle;
                close_ret = call_ioctl(child_fd, DRM_IOCTL_GEM_CLOSE,
                                       &child_close);
            }
            if (dummy_ret == 0) {
                memset(&child_destroy, 0, sizeof(child_destroy));
                child_destroy.handle = child_dummy.handle;
                destroy_ret = call_ioctl(child_fd,
                                         DRM_IOCTL_MODE_DESTROY_DUMB,
                                         &child_destroy);
            }
            printf("%s:DRM_IOCTL_PRIME_FD_TO_HANDLE.cross_owner.child: "
                   "open=%d dummy=%d dummy_handle=%u import=%d errno=%d "
                   "child_handle=%u parent_handle=%u different=%u "
                   "map=%d map_errno=%d same_pixels=%u child_first=0x%x "
                   "child_last=0x%x close=%d close_errno=%d destroy=%d "
                   "destroy_errno=%d\n",
                   node->name, child_fd, dummy_ret, child_dummy.handle,
                   import_ret, saved_errno(import_ret), child_import.handle,
                   create.handle, child_import.handle != create.handle,
                   map_ret, saved_errno(map_ret),
                   child_first == pattern_first && child_last == pattern_last,
                   child_first, child_last, close_ret, saved_errno(close_ret),
                   destroy_ret, saved_errno(destroy_ret));
            if (child_pixels != MAP_FAILED)
                munmap((void *)child_pixels, (int)create.size);
            if (child_fd >= 0)
                close(child_fd);
            exit(dummy_ret == 0 && import_ret == 0 && close_ret == 0 &&
                 destroy_ret == 0 && child_import.handle != 0 &&
                 child_import.handle != create.handle && map_ret == 0 &&
                 child_first == pattern_first &&
                 child_last == pattern_last ? 0 : 1);
        }
        if (child > 0)
            wait(&child_status);

        if (parent_pixels != MAP_FAILED) {
            parent_first = parent_pixels[0];
            parent_last = parent_pixels[1];
            munmap((void *)parent_pixels, (int)create.size);
        }
        memset(&map, 0, sizeof(map));
        map.handle = create.handle;
        ret = call_ioctl(node->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
        printf("%s:DRM_IOCTL_PRIME_FD_TO_HANDLE.cross_owner.parent: "
               "child_status=%d map=%d errno=%d offset=%lu "
               "parent_first=0x%x child_write=0x%x\n",
               node->name, child_status, ret, saved_errno(ret), map.offset,
               parent_first, parent_last);
        close(prime.fd);
    }

    memset(&flink, 0, sizeof(flink));
    flink.handle = create.handle;
    ret = call_ioctl(node->fd, DRM_IOCTL_GEM_FLINK, &flink);
    print_ret_u32(node->name, "DRM_IOCTL_GEM_FLINK.valid", ret, flink.name);
    if (ret == 0 && flink.name != 0) {
        uint32 *parent_pixels;
        uint32 parent_first = 0;
        uint32 parent_child_write = 0;
        uint32 pattern_first = 0xff13579b;
        uint32 pattern_last = 0xff2468ac;
        int child_status = -1;
        int child = fork();

        memset(&map, 0, sizeof(map));
        map.handle = create.handle;
        ret = call_ioctl(node->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
        parent_pixels = ret == 0 ?
            mmap(0, (int)create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                 node->fd, map.offset) : MAP_FAILED;
        if (parent_pixels != MAP_FAILED) {
            parent_pixels[0] = pattern_first;
            parent_pixels[(create.height - 1) * (create.pitch / 4) +
                          (create.width - 1)] = pattern_last;
        }

        if (child == 0) {
            int child_fd = open(node->path, 0);
            struct drm_mode_create_dumb_compat child_dummy;
            struct drm_mode_destroy_dumb_compat child_destroy;
            struct drm_mode_map_dumb_compat child_map;
            struct drm_gem_open_compat child_open;
            struct drm_gem_close_compat child_close;
            uint32 *child_pixels = MAP_FAILED;
            uint32 child_first = 0;
            uint32 child_last = 0;
            int dummy_ret;
            int open_ret;
            int map_ret = -999;
            int close_ret = -999;
            int destroy_ret = -999;

            memset(&child_dummy, 0, sizeof(child_dummy));
            child_dummy.width = 16;
            child_dummy.height = 16;
            child_dummy.bpp = 32;
            dummy_ret = child_fd >= 0 ?
                call_ioctl(child_fd, DRM_IOCTL_MODE_CREATE_DUMB,
                           &child_dummy) : -EBADF;
            memset(&child_open, 0, sizeof(child_open));
            child_open.name = flink.name;
            open_ret = child_fd >= 0 ?
                call_ioctl(child_fd, DRM_IOCTL_GEM_OPEN, &child_open) :
                -EBADF;
            if (open_ret == 0) {
                memset(&child_map, 0, sizeof(child_map));
                child_map.handle = child_open.handle;
                map_ret = call_ioctl(child_fd, DRM_IOCTL_MODE_MAP_DUMB,
                                     &child_map);
                if (map_ret == 0) {
                    child_pixels = mmap(0, (int)child_open.size,
                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                        child_fd, child_map.offset);
                    if (child_pixels != MAP_FAILED) {
                        child_first = child_pixels[0];
                        child_last =
                            child_pixels[(create.height - 1) *
                                         (create.pitch / 4) +
                                         (create.width - 1)];
                        child_pixels[2] = 0xff102030;
                    }
                }
                memset(&child_close, 0, sizeof(child_close));
                child_close.handle = child_open.handle;
                close_ret = call_ioctl(child_fd, DRM_IOCTL_GEM_CLOSE,
                                       &child_close);
            }
            if (dummy_ret == 0) {
                memset(&child_destroy, 0, sizeof(child_destroy));
                child_destroy.handle = child_dummy.handle;
                destroy_ret = call_ioctl(child_fd,
                                         DRM_IOCTL_MODE_DESTROY_DUMB,
                                         &child_destroy);
            }
            printf("%s:DRM_IOCTL_GEM_OPEN.cross_owner.child: open_fd=%d "
                   "dummy=%d dummy_handle=%u open=%d errno=%d "
                   "name=%u child_handle=%u parent_handle=%u size=%lu "
                   "different=%u map=%d map_errno=%d same_pixels=%u "
                   "child_first=0x%x child_last=0x%x close=%d "
                   "close_errno=%d destroy=%d destroy_errno=%d\n",
                   node->name, child_fd, dummy_ret, child_dummy.handle,
                   open_ret, saved_errno(open_ret), flink.name,
                   child_open.handle, create.handle, child_open.size,
                   child_open.handle != create.handle, map_ret,
                   saved_errno(map_ret),
                   child_first == pattern_first && child_last == pattern_last,
                   child_first, child_last, close_ret, saved_errno(close_ret),
                   destroy_ret, saved_errno(destroy_ret));
            if (child_pixels != MAP_FAILED)
                munmap((void *)child_pixels, (int)child_open.size);
            if (child_fd >= 0)
                close(child_fd);
            exit(dummy_ret == 0 && open_ret == 0 && map_ret == 0 &&
                 close_ret == 0 && destroy_ret == 0 &&
                 child_open.handle != 0 &&
                 child_first == pattern_first &&
                 child_last == pattern_last ? 0 : 1);
        }
        if (child > 0)
            wait(&child_status);
        if (parent_pixels != MAP_FAILED) {
            parent_first = parent_pixels[0];
            parent_child_write = parent_pixels[2];
            munmap((void *)parent_pixels, (int)create.size);
        }
        memset(&map, 0, sizeof(map));
        map.handle = create.handle;
        ret = call_ioctl(node->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
        printf("%s:DRM_IOCTL_GEM_OPEN.cross_owner.parent: child_status=%d "
               "map=%d errno=%d offset=%lu parent_first=0x%x "
               "child_write=0x%x\n",
               node->name, child_status, ret, saved_errno(ret), map.offset,
               parent_first, parent_child_write);
    }

    {
        struct drm_mode_fb_cmd_compat addfb;
        struct drm_mode_fb_cmd2_compat addfb2;
        struct drm_mode_fb_cmd_compat getfb;
        struct drm_mode_fb_cmd2_compat getfb2;
        struct drm_mode_closefb_compat closefb;
        uint32 rmfb;

        memset(&addfb, 0, sizeof(addfb));
        addfb.width = create.width;
        addfb.height = create.height;
        addfb.pitch = create.pitch;
        addfb.bpp = 32;
        addfb.depth = 24;
        addfb.handle = create.handle;
        ret = call_ioctl(node->fd, DRM_IOCTL_MODE_ADDFB, &addfb);
        print_ret_u32(node->name, "DRM_IOCTL_MODE_ADDFB.valid", ret,
                      addfb.fb_id);
        if (ret == 0 && addfb.fb_id != 0) {
            rmfb = addfb.fb_id;
            ret = call_ioctl(node->fd, DRM_IOCTL_MODE_RMFB, &rmfb);
            print_ret(node->name, "DRM_IOCTL_MODE_RMFB.legacy_addfb",
                      ret);
        }

        memset(&addfb2, 0, sizeof(addfb2));
        addfb2.width = create.width;
        addfb2.height = create.height;
        addfb2.pixel_format = DRM_FORMAT_XRGB8888;
        addfb2.handles[0] = create.handle;
        addfb2.pitches[0] = create.pitch;
        ret = call_ioctl(node->fd, DRM_IOCTL_MODE_ADDFB2, &addfb2);
        print_ret_u32(node->name, "DRM_IOCTL_MODE_ADDFB2.valid", ret,
                      addfb2.fb_id);
        if (ret == 0 && addfb2.fb_id != 0) {
            memset(&getfb, 0, sizeof(getfb));
            getfb.fb_id = addfb2.fb_id;
            ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETFB, &getfb);
            printf("%s:DRM_IOCTL_MODE_GETFB.valid: ret=%d errno=%d "
                   "fb=%u handle=%u %ux%u pitch=%u bpp=%u depth=%u\n",
                   node->name, ret, saved_errno(ret), getfb.fb_id,
                   getfb.handle, getfb.width, getfb.height, getfb.pitch,
                   getfb.bpp, getfb.depth);

            memset(&getfb2, 0, sizeof(getfb2));
            getfb2.fb_id = addfb2.fb_id;
            ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETFB2, &getfb2);
            printf("%s:DRM_IOCTL_MODE_GETFB2.valid: ret=%d errno=%d "
                   "fb=%u handle0=%u %ux%u format=0x%x\n",
                   node->name, ret, saved_errno(ret), getfb2.fb_id,
                   getfb2.handles[0], getfb2.width, getfb2.height,
                   getfb2.pixel_format);

            memset(&closefb, 0, sizeof(closefb));
            closefb.fb_id = addfb2.fb_id;
            ret = call_ioctl(node->fd, DRM_IOCTL_MODE_CLOSEFB, &closefb);
            print_ret(node->name, "DRM_IOCTL_MODE_CLOSEFB.valid", ret);

            rmfb = addfb2.fb_id;
            ret = call_ioctl(node->fd, DRM_IOCTL_MODE_RMFB, &rmfb);
            print_ret(node->name, "DRM_IOCTL_MODE_RMFB.after_closefb", ret);
        }
    }

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    print_ret(node->name, "DRM_IOCTL_MODE_DESTROY_DUMB.valid", ret);
}

static void probe_atomic_fences(struct drm_node *node)
{
    struct drm_mode_card_res_compat res;
    struct drm_mode_get_plane_res_compat plane_res;
    struct drm_mode_create_dumb_compat create;
    struct drm_mode_map_dumb_compat map;
    struct drm_mode_fb_cmd2_compat addfb2;
    struct drm_mode_destroy_dumb_compat destroy;
    struct drm_mode_fb_dirty_cmd_compat dirtyfb;
    struct drm_clip_rect_compat dirty_clip;
    struct drm_syncobj_create_compat sync_create;
    struct drm_syncobj_handle_compat sync_export;
    struct drm_syncobj_array_compat sync_signal;
    struct pollfd pfd;
    uint32 crtcs[4];
    uint32 planes[4];
    uint32 objs[2];
    uint32 counts[2];
    uint32 props[4];
    uint64 values[4];
    uint32 crtc_id = 0;
    uint32 plane_id = 0;
    uint32 crtc_out_fence_prop = 0;
    uint32 plane_crtc_prop = 0;
    uint32 plane_fb_prop = 0;
    uint32 plane_in_fence_prop = 0;
    uint32 rmfb = 0;
    int32 out_fence = -2;
    int sync_fd = -1;
    int child = -1;
    int child_status = -1;
    int create_ret;
    int addfb_ret = -999;
    int sync_create_ret = -999;
    int sync_export_ret = -999;
    int atomic_ret = -999;
    int poll_ret = -999;
    int signal_ret = -999;
    int dirty_ret = -999;
    uint32 rollback_before = 0;
    uint32 rollback_after_test = 0;
    uint32 rollback_after_real = 0;
    int rollback_query_before = -999;
    int rollback_query_after_test = -999;
    int rollback_query_after_real = -999;
    int rollback_test_ret = -999;
    int rollback_real_ret = -999;

    memset(&res, 0, sizeof(res));
    memset(crtcs, 0, sizeof(crtcs));
    res.crtc_id_ptr = (uint64)crtcs;
    res.count_crtcs = ARRAY_SIZE(crtcs);
    if (call_ioctl(node->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 &&
        res.count_crtcs > 0)
        crtc_id = crtcs[0];

    memset(&plane_res, 0, sizeof(plane_res));
    memset(planes, 0, sizeof(planes));
    plane_res.plane_id_ptr = (uint64)planes;
    plane_res.count_planes = ARRAY_SIZE(planes);
    if (call_ioctl(node->fd, DRM_IOCTL_MODE_GETPLANERESOURCES,
                   &plane_res) == 0 && plane_res.count_planes > 0)
        plane_id = planes[0];

    if (crtc_id != 0 && plane_id != 0) {
        crtc_out_fence_prop = find_obj_prop_named(
            node, crtc_id, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR");
        plane_crtc_prop = find_obj_prop_named(
            node, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        plane_fb_prop = find_obj_prop_named(
            node, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
        plane_in_fence_prop = find_obj_prop_named(
            node, plane_id, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD");
    }

    memset(&create, 0, sizeof(create));
    create.width = 64;
    create.height = 64;
    create.bpp = 32;
    create_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
    if (create_ret == 0 && create.handle != 0) {
        uint32 *pixels = MAP_FAILED;

        memset(&map, 0, sizeof(map));
        map.handle = create.handle;
        if (call_ioctl(node->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0) {
            pixels = mmap(0, (int)create.size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, node->fd, map.offset);
            if (pixels != MAP_FAILED) {
                for (uint32 y = 0; y < create.height; y++) {
                    for (uint32 x = 0; x < create.width; x++)
                        pixels[y * (create.pitch / 4) + x] =
                            0xff000000 | ((x * 3) << 16) |
                            ((y * 5) << 8) | 0x55;
                }
                munmap((void *)pixels, (int)create.size);
            }
        }

        memset(&addfb2, 0, sizeof(addfb2));
        addfb2.width = create.width;
        addfb2.height = create.height;
        addfb2.pixel_format = DRM_FORMAT_XRGB8888;
        addfb2.handles[0] = create.handle;
        addfb2.pitches[0] = create.pitch;
        addfb_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_ADDFB2, &addfb2);
        if (addfb_ret == 0)
            rmfb = addfb2.fb_id;
    }

    memset(&sync_create, 0, sizeof(sync_create));
    sync_create_ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_CREATE,
                                 &sync_create);
    if (sync_create_ret == 0) {
        memset(&sync_export, 0, sizeof(sync_export));
        sync_export.handle = sync_create.handle;
        sync_export.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
        sync_export_ret = call_ioctl(node->fd,
                                     DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD,
                                     &sync_export);
        if (sync_export_ret == 0)
            sync_fd = sync_export.fd;
    }

    if (crtc_id != 0 && plane_id != 0 && crtc_out_fence_prop != 0 &&
        plane_crtc_prop != 0 && plane_fb_prop != 0 &&
        plane_in_fence_prop != 0 && addfb_ret == 0 && sync_fd >= 0) {
        child = fork();
        if (child == 0) {
            sleep(50);
            memset(&sync_signal, 0, sizeof(sync_signal));
            sync_signal.handles = (uint64)&sync_create.handle;
            sync_signal.count_handles = 1;
            signal_ret = call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_SIGNAL,
                                    &sync_signal);
            exit(signal_ret == 0 ? 0 : 1);
        }

        objs[0] = crtc_id;
        counts[0] = 1;
        props[0] = crtc_out_fence_prop;
        values[0] = (uint64)&out_fence;
        objs[1] = plane_id;
        counts[1] = 3;
        props[1] = plane_crtc_prop;
        values[1] = crtc_id;
        props[2] = plane_fb_prop;
        values[2] = addfb2.fb_id;
        props[3] = plane_in_fence_prop;
        values[3] = (uint64)(uint32)sync_fd;
        atomic_ret = atomic_commit_props(node, objs, counts, 2, props,
                                         values, 0);
        if (child > 0)
            wait(&child_status);
        if (out_fence >= 0) {
            memset(&pfd, 0, sizeof(pfd));
            pfd.fd = out_fence;
            pfd.events = POLLIN | POLLOUT;
            poll_ret = poll_raw(&pfd, 1, 0);
        }

        memset(&dirty_clip, 0, sizeof(dirty_clip));
        dirty_clip.x1 = 4;
        dirty_clip.y1 = 4;
        dirty_clip.x2 = 20;
        dirty_clip.y2 = 12;
        memset(&dirtyfb, 0, sizeof(dirtyfb));
        dirtyfb.fb_id = addfb2.fb_id;
        dirtyfb.num_clips = 1;
        dirtyfb.clips_ptr = (uint64)&dirty_clip;
        dirty_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_DIRTYFB, &dirtyfb);

        rollback_query_before =
            get_plane_fb_id(node, plane_id, &rollback_before);
        objs[0] = crtc_id;
        counts[0] = 1;
        props[0] = crtc_out_fence_prop;
        values[0] = 0;
        objs[1] = plane_id;
        counts[1] = 1;
        props[1] = plane_crtc_prop;
        values[1] = 0xfeedfaceU;
        rollback_test_ret = atomic_commit_props(
            node, objs, counts, 2, props, values, DRM_MODE_ATOMIC_TEST_ONLY);
        rollback_query_after_test =
            get_plane_fb_id(node, plane_id, &rollback_after_test);
        rollback_real_ret = atomic_commit_props(
            node, objs, counts, 2, props, values, 0);
        rollback_query_after_real =
            get_plane_fb_id(node, plane_id, &rollback_after_real);
    }

    printf("%s:DRM_IOCTL_MODE_ATOMIC.fences: kms=%d crtc=%u plane=%u "
           "props=%u/%u/%u/%u create=%d addfb=%d fb=%u sync_create=%d "
           "sync_export=%d sync_fd=%d atomic=%d errno=%d out_fence=%d "
           "poll=%d revents=0x%x dirty=%d dirty_errno=%d "
           "child_status=%d\n",
           node->name, crtc_id != 0 && plane_id != 0, crtc_id, plane_id,
           crtc_out_fence_prop, plane_crtc_prop, plane_fb_prop,
           plane_in_fence_prop, create_ret, addfb_ret, addfb2.fb_id,
           sync_create_ret, sync_export_ret, sync_fd, atomic_ret,
           saved_errno(atomic_ret), out_fence, poll_ret,
           out_fence >= 0 ? pfd.revents : 0, dirty_ret,
           saved_errno(dirty_ret), child_status);
    printf("%s:DRM_IOCTL_MODE_ATOMIC.check_rollback: before_q=%d "
           "before=%u test_ret=%d test_errno=%d after_test_q=%d "
           "after_test=%u test_unchanged=%d real_ret=%d real_errno=%d "
           "after_real_q=%d after_real=%u real_unchanged=%d\n",
           node->name, rollback_query_before, rollback_before,
           rollback_test_ret, saved_errno(rollback_test_ret),
           rollback_query_after_test, rollback_after_test,
           rollback_query_before == 0 && rollback_query_after_test == 0 &&
               rollback_after_test == rollback_before,
           rollback_real_ret, saved_errno(rollback_real_ret),
           rollback_query_after_real, rollback_after_real,
           rollback_query_before == 0 && rollback_query_after_real == 0 &&
               rollback_after_real == rollback_before);

    if (out_fence >= 0)
        close(out_fence);
    if (sync_fd >= 0)
        close(sync_fd);
    if (sync_create_ret == 0) {
        struct drm_syncobj_destroy_compat sync_destroy;

        memset(&sync_destroy, 0, sizeof(sync_destroy));
        sync_destroy.handle = sync_create.handle;
        (void)call_ioctl(node->fd, DRM_IOCTL_SYNCOBJ_DESTROY,
                         &sync_destroy);
    }
    if (rmfb != 0)
        (void)call_ioctl(node->fd, DRM_IOCTL_MODE_RMFB, &rmfb);
    if (create_ret == 0 && create.handle != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = create.handle;
        (void)call_ioctl(node->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
}

static void probe_cursor_plane(struct drm_node *node)
{
    struct drm_mode_card_res_compat res;
    struct drm_mode_get_plane_res_compat plane_res;
    struct drm_mode_create_dumb_compat create;
    struct drm_mode_map_dumb_compat map;
    struct drm_mode_fb_cmd2_compat addfb2;
    struct drm_mode_cursor2_compat cursor2;
    struct drm_mode_set_plane_compat setplane;
    struct drm_mode_destroy_dumb_compat destroy;
    uint32 crtcs[4];
    uint32 planes[8];
    uint32 cursor_plane = 0;
    uint32 crtc_id = 0;
    uint32 rmfb = 0;
    int create_ret;
    int map_ret = -999;
    int cursor_set_ret = -999;
    int cursor_move_ret = -999;
    int addfb_ret = -999;
    int setplane_ret = -999;
    int atomic_ret = -999;
    int hide_ret = -999;

    memset(&addfb2, 0, sizeof(addfb2));
    memset(&res, 0, sizeof(res));
    memset(crtcs, 0, sizeof(crtcs));
    res.crtc_id_ptr = (uint64)crtcs;
    res.count_crtcs = ARRAY_SIZE(crtcs);
    if (call_ioctl(node->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 &&
        res.count_crtcs > 0)
        crtc_id = crtcs[0];

    memset(&plane_res, 0, sizeof(plane_res));
    memset(planes, 0, sizeof(planes));
    plane_res.plane_id_ptr = (uint64)planes;
    plane_res.count_planes = ARRAY_SIZE(planes);
    if (call_ioctl(node->fd, DRM_IOCTL_MODE_GETPLANERESOURCES,
                   &plane_res) == 0) {
        for (uint32 i = 0; i < plane_res.count_planes &&
             i < ARRAY_SIZE(planes); i++) {
            uint64 type = 0;

            if (get_obj_prop_value_named(node, planes[i],
                                         DRM_MODE_OBJECT_PLANE, "type",
                                         &type) == 0 &&
                type == DRM_PLANE_TYPE_CURSOR) {
                cursor_plane = planes[i];
                break;
            }
        }
    }

    memset(&create, 0, sizeof(create));
    create.width = 64;
    create.height = 64;
    create.bpp = 32;
    create_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
    if (create_ret == 0 && create.handle != 0) {
        uint32 *pixels = MAP_FAILED;

        memset(&map, 0, sizeof(map));
        map.handle = create.handle;
        map_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
        if (map_ret == 0) {
            pixels = mmap(0, (int)create.size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, node->fd, map.offset);
            if (pixels != MAP_FAILED) {
                for (uint32 y = 0; y < create.height; y++) {
                    for (uint32 x = 0; x < create.width; x++) {
                        uint32 alpha = (x < 8 || y < 8) ? 0x80 : 0xff;
                        pixels[y * (create.pitch / 4) + x] =
                            (alpha << 24) | 0x00ff2020 |
                            ((x & 0x3f) << 8) | (y & 0x3f);
                    }
                }
                munmap((void *)pixels, (int)create.size);
            }
        }
    }

    if (crtc_id != 0 && create_ret == 0 && create.handle != 0) {
        memset(&cursor2, 0, sizeof(cursor2));
        cursor2.flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE;
        cursor2.crtc_id = crtc_id;
        cursor2.x = 48;
        cursor2.y = 48;
        cursor2.width = create.width;
        cursor2.height = create.height;
        cursor2.handle = create.handle;
        cursor2.hot_x = 4;
        cursor2.hot_y = 4;
        cursor_set_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_CURSOR2,
                                    &cursor2);

        memset(&cursor2, 0, sizeof(cursor2));
        cursor2.flags = DRM_MODE_CURSOR_MOVE;
        cursor2.crtc_id = crtc_id;
        cursor2.x = 96;
        cursor2.y = 72;
        cursor_move_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_CURSOR2,
                                     &cursor2);
    }

    if (cursor_plane != 0 && crtc_id != 0 && create_ret == 0) {
        memset(&addfb2, 0, sizeof(addfb2));
        addfb2.width = create.width;
        addfb2.height = create.height;
        addfb2.pixel_format = DRM_FORMAT_ARGB8888;
        addfb2.handles[0] = create.handle;
        addfb2.pitches[0] = create.pitch;
        addfb_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_ADDFB2, &addfb2);
        if (addfb_ret == 0)
            rmfb = addfb2.fb_id;

        if (addfb_ret == 0) {
            memset(&setplane, 0, sizeof(setplane));
            setplane.plane_id = cursor_plane;
            setplane.crtc_id = crtc_id;
            setplane.fb_id = addfb2.fb_id;
            setplane.crtc_x = 128;
            setplane.crtc_y = 80;
            setplane.crtc_w = create.width;
            setplane.crtc_h = create.height;
            setplane.src_w = create.width << 16;
            setplane.src_h = create.height << 16;
            setplane_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_SETPLANE,
                                      &setplane);
        }

        if (addfb_ret == 0) {
            uint32 props[8];
            uint64 values[8];
            uint32 objs[1];
            uint32 counts[1];
            uint32 crtc_prop = find_obj_prop_named(
                node, cursor_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
            uint32 fb_prop = find_obj_prop_named(
                node, cursor_plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
            uint32 src_w_prop = find_obj_prop_named(
                node, cursor_plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
            uint32 src_h_prop = find_obj_prop_named(
                node, cursor_plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
            uint32 crtc_x_prop = find_obj_prop_named(
                node, cursor_plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
            uint32 crtc_y_prop = find_obj_prop_named(
                node, cursor_plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
            uint32 crtc_w_prop = find_obj_prop_named(
                node, cursor_plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
            uint32 crtc_h_prop = find_obj_prop_named(
                node, cursor_plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");

            if (crtc_prop != 0 && fb_prop != 0 && src_w_prop != 0 &&
                src_h_prop != 0 && crtc_x_prop != 0 &&
                crtc_y_prop != 0 && crtc_w_prop != 0 &&
                crtc_h_prop != 0) {
                objs[0] = cursor_plane;
                counts[0] = 8;
                props[0] = crtc_prop;
                values[0] = crtc_id;
                props[1] = fb_prop;
                values[1] = addfb2.fb_id;
                props[2] = src_w_prop;
                values[2] = create.width << 16;
                props[3] = src_h_prop;
                values[3] = create.height << 16;
                props[4] = crtc_x_prop;
                values[4] = 160;
                props[5] = crtc_y_prop;
                values[5] = 96;
                props[6] = crtc_w_prop;
                values[6] = create.width;
                props[7] = crtc_h_prop;
                values[7] = create.height;
                atomic_ret = atomic_commit_props(node, objs, counts, 1,
                                                 props, values, 0);
            }
        }
    }

    if (crtc_id != 0) {
        memset(&cursor2, 0, sizeof(cursor2));
        cursor2.flags = DRM_MODE_CURSOR_BO;
        cursor2.crtc_id = crtc_id;
        hide_ret = call_ioctl(node->fd, DRM_IOCTL_MODE_CURSOR2, &cursor2);
    }

    printf("%s:DRM_CURSOR_PLANE.valid: kms=%d crtc=%u cursor_plane=%u "
           "create=%d map=%d cursor_set=%d cursor_set_errno=%d "
           "cursor_move=%d setplane=%d setplane_errno=%d addfb=%d fb=%u "
           "atomic=%d atomic_errno=%d hide=%d hide_errno=%d\n",
           node->name, crtc_id != 0 && cursor_plane != 0, crtc_id,
           cursor_plane, create_ret, map_ret, cursor_set_ret,
           saved_errno(cursor_set_ret), cursor_move_ret, setplane_ret,
           saved_errno(setplane_ret), addfb_ret, addfb2.fb_id, atomic_ret,
           saved_errno(atomic_ret), hide_ret, saved_errno(hide_ret));

    if (rmfb != 0)
        (void)call_ioctl(node->fd, DRM_IOCTL_MODE_RMFB, &rmfb);
    if (create_ret == 0 && create.handle != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = create.handle;
        (void)call_ioctl(node->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
}

static void probe_virtgpu(struct drm_node *node)
{
    static const uint64 params[] = {
        VIRTGPU_PARAM_3D_FEATURES,
        VIRTGPU_PARAM_CAPSET_QUERY_FIX,
        VIRTGPU_PARAM_RESOURCE_BLOB,
        VIRTGPU_PARAM_HOST_VISIBLE,
        VIRTGPU_PARAM_CONTEXT_INIT,
        VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs,
        VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME,
        0xffffffffULL,
    };

    for (int i = 0; i < ARRAY_SIZE(params); i++) {
        struct drm_virtgpu_getparam_compat req;
        uint64 value = 0;
        int ret;

        memset(&req, 0, sizeof(req));
        req.param = params[i];
        req.value = (uint64)&value;
        ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_GETPARAM, &req);
        printf("%s:DRM_IOCTL_VIRTGPU_GETPARAM[%lu]: ret=%d errno=%d "
               "value=%lu\n",
               node->name, params[i], ret, saved_errno(ret), value);
    }
}

static int virtgpu_getparam_value(struct drm_node *node, uint64 param,
                                  uint64 *value)
{
    struct drm_virtgpu_getparam_compat req;

    if (value == NULL)
        return -EINVAL;
    *value = 0;
    memset(&req, 0, sizeof(req));
    req.param = param;
    req.value = (uint64)value;
    return call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_GETPARAM, &req);
}

static void probe_virtgpu_execbuffer_sync(struct drm_node *node)
{
    struct drm_virtgpu_resource_create_compat create;
    struct drm_virtgpu_execbuffer_compat exec;
    struct drm_virtgpu_3d_wait_compat wait_req;
    struct drm_gem_close_compat close_req;
    struct pollfd pfd;
    uint32 nop = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, 0);
    uint32 handles[1];
    int create_ret;
    int exec_out_ret = -999;
    int exec_inout_ret = -999;
    int wait_ret = -999;
    int nowait_ret = -999;
    int poll_ret = -999;
    int first_fd = -1;
    int second_fd = -1;

    memset(&create, 0, sizeof(create));
    create.target = PIPE_TEXTURE_2D;
    create.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    create.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW;
    create.width = 16;
    create.height = 16;
    create.depth = 1;
    create.array_size = 1;
    create.size = 16 * 16 * 4;
    create_ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,
                            &create);

    if (create_ret == 0 && create.bo_handle != 0) {
        handles[0] = create.bo_handle;

        memset(&exec, 0, sizeof(exec));
        exec.flags = VIRTGPU_EXECBUF_FENCE_FD_OUT;
        exec.size = sizeof(nop);
        exec.command = (uint64)&nop;
        exec.bo_handles = (uint64)handles;
        exec.num_bo_handles = ARRAY_SIZE(handles);
        exec.fence_fd = -1;
        exec_out_ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_EXECBUFFER,
                                  &exec);
        if (exec_out_ret == 0)
            first_fd = exec.fence_fd;

        if (first_fd >= 0) {
            memset(&pfd, 0, sizeof(pfd));
            pfd.fd = first_fd;
            pfd.events = POLLIN;
            poll_ret = poll(&pfd, 1, 0);

            memset(&exec, 0, sizeof(exec));
            exec.flags = VIRTGPU_EXECBUF_FENCE_FD_IN |
                         VIRTGPU_EXECBUF_FENCE_FD_OUT;
            exec.size = sizeof(nop);
            exec.command = (uint64)&nop;
            exec.bo_handles = (uint64)handles;
            exec.num_bo_handles = ARRAY_SIZE(handles);
            exec.fence_fd = first_fd;
            exec_inout_ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_EXECBUFFER,
                                        &exec);
            if (exec_inout_ret == 0)
                second_fd = exec.fence_fd;
        }

        memset(&wait_req, 0, sizeof(wait_req));
        wait_req.handle = create.bo_handle;
        wait_ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_WAIT, &wait_req);
        wait_req.flags = VIRTGPU_WAIT_NOWAIT;
        nowait_ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_WAIT, &wait_req);
    }

    printf("%s:DRM_IOCTL_VIRTGPU_EXECBUFFER.sync_fd: create=%d "
           "create_errno=%d handle=%u out=%d out_errno=%d first_fd=%d "
           "poll=%d inout=%d inout_errno=%d second_fd=%d wait=%d "
           "wait_errno=%d nowait=%d nowait_errno=%d\n",
           node->name, create_ret, saved_errno(create_ret), create.bo_handle,
           exec_out_ret, saved_errno(exec_out_ret), first_fd, poll_ret,
           exec_inout_ret, saved_errno(exec_inout_ret), second_fd, wait_ret,
           saved_errno(wait_ret), nowait_ret, saved_errno(nowait_ret));

    if (second_fd >= 0)
        close(second_fd);
    if (first_fd >= 0)
        close(first_fd);
    if (create_ret == 0 && create.bo_handle != 0) {
        memset(&close_req, 0, sizeof(close_req));
        close_req.handle = create.bo_handle;
        (void)call_ioctl(node->fd, DRM_IOCTL_GEM_CLOSE, &close_req);
    }
}

static void probe_virtgpu_blob_create(struct drm_node *node)
{
    struct drm_virtgpu_resource_create_blob_compat blob;
    struct drm_virtgpu_resource_info_compat info;
    struct drm_gem_close_compat close_req;
    int create_ret;
    int info_ret = -999;

    memset(&blob, 0, sizeof(blob));
    blob.blob_mem = VIRTGPU_BLOB_MEM_GUEST;
    blob.size = 4096;
    blob.blob_id = 0x587636626c6f6231ULL;
    create_ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB,
                            &blob);
    memset(&info, 0, sizeof(info));
    if (create_ret == 0 && blob.bo_handle != 0) {
        info.bo_handle = blob.bo_handle;
        info_ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO,
                              &info);
    }

    printf("%s:DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB.valid: create=%d "
           "create_errno=%d bo=%u res=%u size=%lu info=%d info_errno=%d "
           "info_res=%u info_size=%u blob_mem=%u\n",
           node->name, create_ret, saved_errno(create_ret), blob.bo_handle,
           blob.res_handle, blob.size, info_ret, saved_errno(info_ret),
           info.res_handle, info.size, info.blob_mem);

    if (create_ret == 0 && blob.bo_handle != 0) {
        memset(&close_req, 0, sizeof(close_req));
        close_req.handle = blob.bo_handle;
        (void)call_ioctl(node->fd, DRM_IOCTL_GEM_CLOSE, &close_req);
    }
}

static void probe_virtgpu_host_visible_blob(struct drm_node *node)
{
    struct drm_virtgpu_resource_create_blob_compat blob;
    struct drm_virtgpu_map_compat map;
    struct drm_gem_close_compat close_req;
    volatile uint32 *mapped = (volatile uint32 *)MAP_FAILED;
    uint64 host_visible = 0;
    uint32 sample = 0;
    int param_ret;
    int create_ret;
    int map_ret = -999;
    int mmap_ok = 0;

    memset(&map, 0, sizeof(map));
    param_ret = virtgpu_getparam_value(node, VIRTGPU_PARAM_HOST_VISIBLE,
                                       &host_visible);

    memset(&blob, 0, sizeof(blob));
    blob.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
    blob.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
    blob.size = 4096;
    blob.blob_id = 0x5876686f73747631ULL;
    create_ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB,
                            &blob);

    if (create_ret == 0 && blob.bo_handle != 0) {
        map.handle = blob.bo_handle;
        map_ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_MAP, &map);
        if (map_ret == 0) {
            mapped = (volatile uint32 *)mmap(0, (int)blob.size,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, node->fd, map.offset);
            if (mapped != (volatile uint32 *)MAP_FAILED) {
                mapped[0] = 0xfeed5035U;
                sample = mapped[0];
                mmap_ok = sample == 0xfeed5035U;
                munmap((void *)mapped, (int)blob.size);
            }
        }

        memset(&close_req, 0, sizeof(close_req));
        close_req.handle = blob.bo_handle;
        (void)call_ioctl(node->fd, DRM_IOCTL_GEM_CLOSE, &close_req);
    }

    printf("%s:DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB.host_visible: "
           "param=%d param_errno=%d advertised=%lu create=%d "
           "create_errno=%d bo=%u res=%u size=%lu map=%d map_errno=%d "
           "offset=0x%lx mmap_ok=%d sample=0x%x\n",
           node->name, param_ret, saved_errno(param_ret), host_visible,
           create_ret, saved_errno(create_ret), blob.bo_handle,
           blob.res_handle, blob.size, map_ret, saved_errno(map_ret),
           map.offset, mmap_ok, sample);
}

static void probe_virtgpu_invalids(struct drm_node *node)
{
    struct drm_virtgpu_resource_create_compat create;
    struct drm_virtgpu_resource_create_blob_compat blob;
    struct drm_virtgpu_3d_transfer_compat transfer;
    struct drm_virtgpu_context_init_compat context_init;

    memset(&context_init, 0, sizeof(context_init));
    print_ret(node->name, "DRM_IOCTL_VIRTGPU_CONTEXT_INIT.empty",
              call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT,
                         &context_init));

    memset(&create, 0, sizeof(create));
    print_ret_u32(node->name, "DRM_IOCTL_VIRTGPU_RESOURCE_CREATE.invalid",
                  call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,
                             &create),
                  create.bo_handle);

    memset(&blob, 0, sizeof(blob));
    print_ret_u32(node->name, "DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB.invalid",
                  call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB,
                             &blob),
                  blob.bo_handle);

    memset(&transfer, 0, sizeof(transfer));
    print_ret(node->name, "DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST.invalid",
              call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST,
                         &transfer));
    print_ret(node->name, "DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST.invalid",
              call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST,
                         &transfer));
}

static uint32 get_first_crtc_id(struct drm_node *node)
{
    struct drm_mode_card_res_compat res;
    uint32 crtcs[4];
    int ret;

    if (node == NULL || node->fd < 0)
        return 0;
    memset(&res, 0, sizeof(res));
    memset(crtcs, 0, sizeof(crtcs));
    res.crtc_id_ptr = (uint64)crtcs;
    res.count_crtcs = ARRAY_SIZE(crtcs);
    ret = call_ioctl(node->fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    if (ret != 0 || res.count_crtcs == 0)
        return 0;
    return crtcs[0];
}

static void probe_prime_virtgpu_sample_scanout(void)
{
    enum { SAMPLE_W = 128, SAMPLE_H = 128 };
    struct fb_gpu_scanout_read req;
    uint32 *sample;
    uint64 total = 0;
    uint64 nonzero = 0;
    uint64 nonblack = 0;
    uint64 sum_r = 0;
    uint64 sum_g = 0;
    uint64 sum_b = 0;
    uint64 hash = 1469598103934665603UL;
    uint32 center = 0;
    uint32 tl = 0;
    uint32 tr = 0;
    uint32 bl = 0;
    uint32 br = 0;
    int fd;
    int ret = -1;

    sample = malloc(SAMPLE_W * SAMPLE_H * sizeof(uint32));
    if (sample == NULL) {
        printf("cross:DRM_PRIME_VIRTGPU_RESOURCE.sample: ret=-1 "
               "errno=12\n");
        return;
    }

    fd = open("/dev/fb0", O_RDWR);
    if (fd >= 0) {
        memset(&req, 0, sizeof(req));
        req.x = 0;
        req.y = 0;
        req.w = SAMPLE_W;
        req.h = SAMPLE_H;
        req.pitch = SAMPLE_W * sizeof(uint32);
        req.pixels = (uint64)sample;
        ret = ioctl(fd, FB_GPU_SCANOUT_READ, &req);
        close(fd);
    }

    if (ret == 0) {
        for (uint32 row = 0; row < SAMPLE_H; row++) {
            for (uint32 col = 0; col < SAMPLE_W; col++) {
                uint32 px = sample[row * SAMPLE_W + col];
                uint32 rgb = px & 0x00ffffffU;

                total++;
                if (px != 0)
                    nonzero++;
                if (rgb != 0)
                    nonblack++;
                sum_r += rgb & 0xffU;
                sum_g += (rgb >> 8) & 0xffU;
                sum_b += (rgb >> 16) & 0xffU;
                hash ^= px;
                hash *= 1099511628211UL;
            }
        }
        tl = sample[0];
        tr = sample[SAMPLE_W - 1];
        bl = sample[(SAMPLE_H - 1) * SAMPLE_W];
        br = sample[(SAMPLE_H - 1) * SAMPLE_W + SAMPLE_W - 1];
        center = sample[(SAMPLE_H / 2) * SAMPLE_W + SAMPLE_W / 2];
        printf("cross:DRM_PRIME_VIRTGPU_RESOURCE.sample: ret=0 "
               "screen=%ux%u pitch=%u total=%lu nonzero=%lu "
               "nonblack=%lu avg_rgb=%lu,%lu,%lu hash=0x%lx "
               "center=0x%x corners=0x%x,0x%x,0x%x,0x%x\n",
               req.screen_width, req.screen_height, req.screen_pitch,
               total, nonzero, nonblack,
               total ? sum_r / total : 0,
               total ? sum_g / total : 0,
               total ? sum_b / total : 0,
               hash, center, tl, tr, bl, br);
    } else {
        printf("cross:DRM_PRIME_VIRTGPU_RESOURCE.sample: ret=-1 errno=1\n");
    }

    free(sample);
}

static void probe_prime_virtgpu_handoff(struct drm_node *card,
                                        struct drm_node *render)
{
    enum { W = 1280, H = 800 };
    struct drm_virtgpu_resource_create_compat create;
    struct drm_virtgpu_map_compat map;
    struct drm_virtgpu_3d_transfer_compat transfer;
    struct drm_prime_handle_compat prime;
    struct drm_prime_handle_compat import_req;
    struct drm_mode_fb_cmd2_compat addfb2;
    struct drm_mode_crtc_page_flip_compat flip;
    struct drm_gem_close_compat close_req;
    uint32 *pixels = (uint32 *)MAP_FAILED;
    int create_ret = -999;
    int map_ret = -999;
    int transfer_ret = -999;
    int export_ret = -999;
    int import_ret = -999;
    int addfb_ret = -999;
    int flip_ret = -999;
    int prime_fd = -1;
    uint32 card_handle = 0;
    uint32 fb_id = 0;
    uint32 crtc_id;

    if (card == NULL || render == NULL || card->fd < 0 || render->fd < 0)
        return;

    crtc_id = get_first_crtc_id(card);

    memset(&create, 0, sizeof(create));
    create.target = PIPE_TEXTURE_2D;
    create.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    create.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
                  VIRGL_BIND_DISPLAY_TARGET;
    create.width = W;
    create.height = H;
    create.depth = 1;
    create.array_size = 1;
    create.size = W * H * 4;
    create_ret = call_ioctl(render->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,
                            &create);

    if (create_ret == 0 && create.bo_handle != 0) {
        memset(&map, 0, sizeof(map));
        map.handle = create.bo_handle;
        map_ret = call_ioctl(render->fd, DRM_IOCTL_VIRTGPU_MAP, &map);
        if (map_ret == 0) {
            pixels = mmap(0, (int)create.size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, render->fd, map.offset);
            if (pixels != (uint32 *)MAP_FAILED) {
                for (uint32 y = 0; y < H; y++) {
                    for (uint32 x = 0; x < W; x++) {
                        uint8 r = (uint8)(0x20 + (x * 0x60) / W);
                        uint8 g = (uint8)(0x40 + (y * 0x80) / H);
                        uint8 b = (uint8)(0x90 + ((x + y) & 0x3f));

                        pixels[y * W + x] =
                            0xff000000U | ((uint32)r << 16) |
                            ((uint32)g << 8) | b;
                    }
                }
                munmap(pixels, (int)create.size);
                memset(&transfer, 0, sizeof(transfer));
                transfer.bo_handle = create.bo_handle;
                transfer.box.x = 0;
                transfer.box.y = 0;
                transfer.box.z = 0;
                transfer.box.w = W;
                transfer.box.h = H;
                transfer.box.d = 1;
                transfer.level = 0;
                transfer.offset = 0;
                transfer.stride = W * sizeof(uint32);
                transfer.layer_stride = H * W * sizeof(uint32);
                transfer_ret = call_ioctl(render->fd,
                                          DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST,
                                          &transfer);
            }
        }

        memset(&prime, 0, sizeof(prime));
        prime.handle = create.bo_handle;
        export_ret = call_ioctl(render->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD,
                                &prime);
        if (export_ret == 0 && prime.fd >= 0)
            prime_fd = prime.fd;
    }

    if (prime_fd >= 0) {
        memset(&import_req, 0, sizeof(import_req));
        import_req.fd = prime_fd;
        import_ret = call_ioctl(card->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE,
                                &import_req);
        if (import_ret == 0)
            card_handle = import_req.handle;
    }

    if (card_handle != 0) {
        memset(&addfb2, 0, sizeof(addfb2));
        addfb2.width = W;
        addfb2.height = H;
        addfb2.pixel_format = DRM_FORMAT_XRGB8888;
        addfb2.handles[0] = card_handle;
        addfb2.pitches[0] = W * 4;
        addfb_ret = call_ioctl(card->fd, DRM_IOCTL_MODE_ADDFB2, &addfb2);
        if (addfb_ret == 0)
            fb_id = addfb2.fb_id;
    }

    if (fb_id != 0 && crtc_id != 0) {
        memset(&flip, 0, sizeof(flip));
        flip.crtc_id = crtc_id;
        flip.fb_id = fb_id;
        flip_ret = call_ioctl(card->fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
        if (flip_ret == 0)
            probe_prime_virtgpu_sample_scanout();
    }

    printf("cross:DRM_PRIME_VIRTGPU_RESOURCE.valid: create=%d "
           "create_errno=%d render_handle=%u map=%d map_errno=%d "
           "transfer=%d transfer_errno=%d export=%d export_errno=%d "
           "prime_fd=%d import=%d import_errno=%d card_handle=%u "
           "addfb=%d addfb_errno=%d fb=%u crtc=%u flip=%d "
           "flip_errno=%d\n",
           create_ret, saved_errno(create_ret), create.bo_handle, map_ret,
           saved_errno(map_ret), transfer_ret, saved_errno(transfer_ret),
           export_ret, saved_errno(export_ret), prime_fd, import_ret,
           saved_errno(import_ret), card_handle, addfb_ret,
           saved_errno(addfb_ret), fb_id, crtc_id, flip_ret,
           saved_errno(flip_ret));

    if (fb_id != 0) {
        uint32 rmfb = fb_id;

        (void)call_ioctl(card->fd, DRM_IOCTL_MODE_RMFB, &rmfb);
    }
    if (card_handle != 0) {
        memset(&close_req, 0, sizeof(close_req));
        close_req.handle = card_handle;
        (void)call_ioctl(card->fd, DRM_IOCTL_GEM_CLOSE, &close_req);
    }
    if (prime_fd >= 0)
        close(prime_fd);
    if (create_ret == 0 && create.bo_handle != 0) {
        memset(&close_req, 0, sizeof(close_req));
        close_req.handle = create.bo_handle;
        (void)call_ioctl(render->fd, DRM_IOCTL_GEM_CLOSE, &close_req);
    }
}

static void probe_safe_invalids(struct drm_node *node)
{
    struct drm_gem_close_compat gem_close;
    struct drm_gem_flink_compat flink;
    struct drm_gem_open_compat gem_open;
    struct drm_mode_destroy_blob_compat destroy_blob;
    struct drm_mode_create_blob_compat create_blob;
    struct drm_mode_set_plane_compat set_plane;
    struct drm_mode_cursor_compat cursor;
    struct drm_mode_cursor2_compat cursor2;
    struct drm_mode_atomic_compat atomic;
    struct drm_mode_crtc_page_flip_compat flip;
    struct drm_mode_fb_cmd_compat addfb;
    struct drm_mode_fb_cmd2_compat addfb2;
    struct drm_mode_destroy_dumb_compat destroy_dumb;
    struct drm_virtgpu_3d_wait_compat virt_wait;
    struct drm_virtgpu_map_compat virt_map;
    struct drm_virtgpu_resource_info_compat virt_info;
    struct drm_virtgpu_get_caps_compat virt_caps;
    struct drm_virtgpu_execbuffer_compat execbuffer;
    union drm_wait_vblank_compat vblank;
    int ret;

    memset(&gem_close, 0, sizeof(gem_close));
    print_ret(node->name, "DRM_IOCTL_GEM_CLOSE.invalid",
              call_ioctl(node->fd, DRM_IOCTL_GEM_CLOSE, &gem_close));

    memset(&flink, 0, sizeof(flink));
    print_ret(node->name, "DRM_IOCTL_GEM_FLINK.invalid",
              call_ioctl(node->fd, DRM_IOCTL_GEM_FLINK, &flink));

    memset(&gem_open, 0, sizeof(gem_open));
    print_ret(node->name, "DRM_IOCTL_GEM_OPEN.invalid",
              call_ioctl(node->fd, DRM_IOCTL_GEM_OPEN, &gem_open));

    memset(&create_blob, 0, sizeof(create_blob));
    create_blob.length = 16;
    print_ret_u32(node->name, "DRM_IOCTL_MODE_CREATEPROPBLOB.invalid",
                  call_ioctl(node->fd, DRM_IOCTL_MODE_CREATEPROPBLOB,
                             &create_blob),
                  create_blob.blob_id);

    memset(&destroy_blob, 0, sizeof(destroy_blob));
    print_ret(node->name, "DRM_IOCTL_MODE_DESTROYPROPBLOB.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_DESTROYPROPBLOB,
                         &destroy_blob));

    memset(&set_plane, 0, sizeof(set_plane));
    print_ret(node->name, "DRM_IOCTL_MODE_SETPLANE.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_SETPLANE, &set_plane));

    memset(&cursor, 0, sizeof(cursor));
    print_ret(node->name, "DRM_IOCTL_MODE_CURSOR.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_CURSOR, &cursor));

    memset(&cursor2, 0, sizeof(cursor2));
    print_ret(node->name, "DRM_IOCTL_MODE_CURSOR2.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_CURSOR2, &cursor2));

    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = DRM_MODE_ATOMIC_TEST_ONLY;
    print_ret(node->name, "DRM_IOCTL_MODE_ATOMIC.empty_test_only",
              call_ioctl(node->fd, DRM_IOCTL_MODE_ATOMIC, &atomic));

    memset(&flip, 0, sizeof(flip));
    print_ret(node->name, "DRM_IOCTL_MODE_PAGE_FLIP.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip));

    memset(&addfb, 0, sizeof(addfb));
    print_ret(node->name, "DRM_IOCTL_MODE_ADDFB.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_ADDFB, &addfb));

    memset(&addfb2, 0, sizeof(addfb2));
    print_ret(node->name, "DRM_IOCTL_MODE_ADDFB2.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_ADDFB2, &addfb2));

    memset(&destroy_dumb, 0, sizeof(destroy_dumb));
    print_ret(node->name, "DRM_IOCTL_MODE_DESTROY_DUMB.invalid",
              call_ioctl(node->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb));

    memset(&virt_wait, 0, sizeof(virt_wait));
    print_ret(node->name, "DRM_IOCTL_VIRTGPU_WAIT.invalid",
              call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_WAIT, &virt_wait));

    memset(&virt_map, 0, sizeof(virt_map));
    print_ret(node->name, "DRM_IOCTL_VIRTGPU_MAP.invalid",
              call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_MAP, &virt_map));

    memset(&virt_info, 0, sizeof(virt_info));
    print_ret(node->name, "DRM_IOCTL_VIRTGPU_RESOURCE_INFO.invalid",
              call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &virt_info));

    memset(&virt_caps, 0, sizeof(virt_caps));
    ret = call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &virt_caps);
    print_ret(node->name, "DRM_IOCTL_VIRTGPU_GET_CAPS.invalid", ret);

    memset(&execbuffer, 0, sizeof(execbuffer));
    print_ret(node->name, "DRM_IOCTL_VIRTGPU_EXECBUFFER.invalid",
              call_ioctl(node->fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &execbuffer));

    memset(&vblank, 0, sizeof(vblank));
    print_ret(node->name, "DRM_IOCTL_WAIT_VBLANK.zero",
              call_ioctl(node->fd, DRM_IOCTL_WAIT_VBLANK, &vblank));
}

static void probe_node(struct drm_node *node)
{
    if (node->fd < 0) {
        printf("%s:open(%s): ret=%d errno=%d\n",
               node->name, node->path, node->fd, saved_errno(node->fd));
        return;
    }

    printf("%s:open(%s): ret=%d errno=0\n", node->name, node->path, node->fd);
    probe_version(node);
    probe_unique(node);
    probe_caps(node);
    probe_client_caps(node);
    probe_core_misc(node);
    probe_legacy_core_stubs(node);
    probe_kms(node);
    probe_kms_invalids(node);
    probe_prop_blobs(node);
    probe_syncobj(node);
    probe_dumb_bo(node);
    probe_atomic_fences(node);
    probe_cursor_plane(node);
    probe_virtgpu(node);
    probe_virtgpu_execbuffer_sync(node);
    probe_virtgpu_blob_create(node);
    probe_virtgpu_host_visible_blob(node);
    probe_virtgpu_invalids(node);
    probe_safe_invalids(node);
}

static void probe_fb0_sample(void)
{
    uint32 pixels[16];
    int fd;
    int n;

    memset(pixels, 0, sizeof(pixels));
    fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) {
        printf("fb0:sample: open=%d errno=%d\n", fd, saved_errno(fd));
        return;
    }

    n = read(fd, pixels, sizeof(pixels));
    printf("fb0:sample: read=%d errno=%d pixels=", n, saved_errno(n));
    if (n >= (int)sizeof(uint32)) {
        int count = n / (int)sizeof(uint32);

        if (count > (int)ARRAY_SIZE(pixels))
            count = ARRAY_SIZE(pixels);
        for (int i = 0; i < count; i++)
            printf("%s%08x", i == 0 ? "" : " ", pixels[i]);
    }
    printf("\n");
    close(fd);
}

int main(int argc, char **argv)
{
    struct drm_node nodes[] = {
        { "card0", "/dev/dri/card0", -1 },
        { "renderD128", "/dev/dri/renderD128", -1 },
    };

    (void)argc;
    (void)argv;

    printf("drmabitest: begin\n");
    for (int i = 0; i < ARRAY_SIZE(nodes); i++)
        nodes[i].fd = open(nodes[i].path, O_RDWR);

    for (int i = 0; i < ARRAY_SIZE(nodes); i++)
        probe_node(&nodes[i]);

    probe_prime_virtgpu_handoff(&nodes[0], &nodes[1]);
    probe_fb0_sample();

    for (int i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (nodes[i].fd >= 0)
            close(nodes[i].fd);
    }
    printf("drmabitest: end\n");
    return 0;
}

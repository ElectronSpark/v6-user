#include "kernel/inc/types.h"
#include "kernel/inc/uabi/drm.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

static int fail(const char *msg)
{
    printf("drmiftest: %s\n", msg);
    return 1;
}

static int get_cap(int fd, uint64 cap, uint64 *value)
{
    struct drm_get_cap_compat req;

    memset(&req, 0, sizeof(req));
    req.capability = cap;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &req) < 0)
        return -1;
    *value = req.value;
    return 0;
}

static int check_common(int fd, const char *node)
{
    char name[32];
    char unique[64];
    struct drm_version_compat ver;
    struct drm_unique_compat uniq;
    struct drm_set_client_cap_compat client_cap;
    uint64 value = 0;

    memset(name, 0, sizeof(name));
    memset(&ver, 0, sizeof(ver));
    ver.name_len = sizeof(name);
    ver.name = (uint64)name;
    if (ioctl(fd, DRM_IOCTL_VERSION, &ver) < 0 || ver.name_len == 0)
        return fail("VERSION failed");

    memset(unique, 0, sizeof(unique));
    memset(&uniq, 0, sizeof(uniq));
    uniq.unique_len = sizeof(unique);
    uniq.unique = (uint64)unique;
    if (ioctl(fd, DRM_IOCTL_GET_UNIQUE, &uniq) < 0 || uniq.unique_len == 0)
        return fail("GET_UNIQUE failed");

    memset(&client_cap, 0, sizeof(client_cap));
    client_cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    client_cap.value = 1;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &client_cap) < 0)
        return fail("SET_CLIENT_CAP failed");

    if (get_cap(fd, DRM_CAP_DUMB_BUFFER, &value) < 0 || value != 1)
        return fail("DUMB_BUFFER cap failed");
    if (get_cap(fd, DRM_CAP_PRIME, &value) < 0 || (value & 3) != 3)
        return fail("PRIME cap failed");
    if (get_cap(fd, DRM_CAP_SYNCOBJ, &value) < 0)
        return fail("SYNCOBJ cap query failed");
    if (get_cap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &value) < 0)
        return fail("SYNCOBJ_TIMELINE cap query failed");

    printf("drmiftest: common ok node=%s driver=%s unique=%s\n",
           node, name, unique);
    return 0;
}

static int check_primary(int fd)
{
    uint32 ids[4];
    struct drm_auth_compat magic;
    struct drm_client_compat client;
    struct drm_stats_compat stats;
    struct drm_mode_card_res_compat res;
    struct drm_mode_get_connector_compat conn;
    struct drm_mode_get_plane_res_compat plane_res;
    struct drm_mode_get_plane_compat plane;
    struct drm_mode_modeinfo_compat modes[2];
    uint32 plane_ids[2];
    uint32 formats[4];

    memset(&magic, 0, sizeof(magic));
    if (ioctl(fd, DRM_IOCTL_GET_MAGIC, &magic) < 0 || magic.magic == 0)
        return fail("primary GET_MAGIC failed");
    if (ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &magic) < 0)
        return fail("primary AUTH_MAGIC failed");
    if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0)
        return fail("primary SET_MASTER failed");

    memset(&client, 0, sizeof(client));
    if (ioctl(fd, DRM_IOCTL_GET_CLIENT, &client) < 0 ||
        client.auth != 1 || client.magic != magic.magic || client.iocs == 0)
        return fail("primary GET_CLIENT failed");

    memset(ids, 0, sizeof(ids));
    memset(&res, 0, sizeof(res));
    res.crtc_id_ptr = (uint64)&ids[0];
    res.connector_id_ptr = (uint64)&ids[1];
    res.encoder_id_ptr = (uint64)&ids[2];
    res.count_crtcs = 1;
    res.count_connectors = 1;
    res.count_encoders = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 ||
        res.count_crtcs != 1 || res.count_connectors != 1 ||
        res.count_encoders != 1 || ids[1] == 0)
        return fail("GETRESOURCES failed");

    memset(&conn, 0, sizeof(conn));
    memset(modes, 0, sizeof(modes));
    conn.connector_id = ids[1];
    conn.modes_ptr = (uint64)modes;
    conn.count_modes = 2;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 ||
        conn.count_modes == 0 || conn.connection != 1 ||
        modes[0].hdisplay == 0 || modes[0].vdisplay == 0)
        return fail("GETCONNECTOR failed");

    memset(plane_ids, 0, sizeof(plane_ids));
    memset(&plane_res, 0, sizeof(plane_res));
    plane_res.plane_id_ptr = (uint64)plane_ids;
    plane_res.count_planes = 2;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res) < 0 ||
        plane_res.count_planes != 1 || plane_ids[0] == 0)
        return fail("GETPLANERESOURCES failed");

    memset(formats, 0, sizeof(formats));
    memset(&plane, 0, sizeof(plane));
    plane.plane_id = plane_ids[0];
    plane.format_type_ptr = (uint64)formats;
    plane.count_format_types = 4;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) < 0 ||
        plane.crtc_id == 0 || plane.possible_crtcs == 0 ||
        plane.count_format_types < 2 ||
        formats[0] != DRM_FORMAT_XRGB8888 ||
        formats[1] != DRM_FORMAT_ARGB8888)
        return fail("GETPLANE failed");

    if (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) < 0)
        return fail("primary DROP_MASTER failed");

    memset(&stats, 0, sizeof(stats));
    if (ioctl(fd, DRM_IOCTL_GET_STATS, &stats) < 0 || stats.count < 4)
        return fail("GET_STATS failed");

    if (ioctl(fd, 0x12345678, 0) >= 0)
        return fail("unknown ioctl accepted");

    printf("drmiftest: primary ok mode=%ux%u ioctls=%lu stats=%lu\n",
           modes[0].hdisplay, modes[0].vdisplay, client.iocs, stats.count);
    return 0;
}

static int check_render_policy(int fd)
{
    struct drm_auth_compat magic;
    struct drm_mode_card_res_compat res;
    struct drm_mode_get_plane_compat plane;
    struct drm_mode_fb_cmd2_compat fb;

    memset(&magic, 0, sizeof(magic));
    if (ioctl(fd, DRM_IOCTL_GET_MAGIC, &magic) >= 0)
        return fail("render GET_MAGIC unexpectedly accepted");
    if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) >= 0 ||
        ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) >= 0)
        return fail("render master ioctl unexpectedly accepted");
    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) >= 0)
        return fail("render KMS ioctl unexpectedly accepted");
    memset(&plane, 0, sizeof(plane));
    plane.plane_id = 4;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) >= 0)
        return fail("render GETPLANE unexpectedly accepted");
    memset(&fb, 0, sizeof(fb));
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) >= 0)
        return fail("render ADDFB2 unexpectedly accepted");

    printf("drmiftest: render policy ok\n");
    return 0;
}

static int check_kms_fb(int fd)
{
    struct drm_mode_create_dumb_compat create;
    struct drm_mode_fb_cmd2_compat fb;
    struct drm_mode_crtc_page_flip_compat flip;
    struct drm_mode_atomic_compat atomic;
    struct drm_mode_destroy_dumb_compat destroy;
    uint32 fb_id = 0;

    memset(&create, 0, sizeof(create));
    create.width = 80;
    create.height = 48;
    create.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        create.handle == 0 || create.pitch < create.width * 4)
        return fail("primary CREATE_DUMB failed");

    memset(&fb, 0, sizeof(fb));
    fb.width = create.width;
    fb.height = create.height;
    fb.pixel_format = DRM_FORMAT_XRGB8888;
    fb.handles[0] = create.handle;
    fb.pitches[0] = create.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0 || fb.fb_id == 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = create.handle;
        (void)ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return fail("ADDFB2 failed");
    }
    fb_id = fb.fb_id;

    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = 1;
    flip.fb_id = fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0)
        return fail("PAGE_FLIP failed");

    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) < 0)
        return fail("ATOMIC test-only failed");

    if (ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id) < 0)
        return fail("RMFB failed");
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
        return fail("primary DESTROY_DUMB failed");

    printf("drmiftest: kms fb ok fb=%u handle=%u\n", fb_id, create.handle);
    return 0;
}

static int check_dumb(int fd)
{
    struct drm_mode_create_dumb_compat create;
    struct drm_mode_destroy_dumb_compat destroy;

    memset(&create, 0, sizeof(create));
    create.width = 64;
    create.height = 64;
    create.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        create.handle == 0 || create.pitch < create.width * 4 ||
        create.size == 0)
        return fail("CREATE_DUMB failed");
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
        return fail("DESTROY_DUMB failed");
    printf("drmiftest: dumb gem ok handle=%u size=%lu\n",
           create.handle, create.size);
    return 0;
}

static int check_gem_mmap_isolation(int owner_fd, int foreign_fd)
{
    struct drm_mode_create_dumb_compat create;
    struct drm_mode_map_dumb_compat map_req;
    struct drm_mode_destroy_dumb_compat destroy;

    memset(&create, 0, sizeof(create));
    create.width = 32;
    create.height = 32;
    create.bpp = 32;
    if (ioctl(owner_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        create.handle == 0)
        return fail("GEM isolation CREATE_DUMB failed");

    memset(&map_req, 0, sizeof(map_req));
    map_req.handle = create.handle;
    if (ioctl(owner_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0 ||
        map_req.offset == 0)
        return fail("GEM isolation owner MAP_DUMB failed");

    memset(&map_req, 0, sizeof(map_req));
    map_req.handle = create.handle;
    if (ioctl(foreign_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) >= 0)
        return fail("GEM isolation foreign MAP_DUMB accepted");

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    if (ioctl(owner_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
        return fail("GEM isolation DESTROY_DUMB failed");

    memset(&map_req, 0, sizeof(map_req));
    map_req.handle = create.handle;
    if (ioctl(owner_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) >= 0)
        return fail("GEM isolation stale MAP_DUMB accepted");

    printf("drmiftest: gem mmap isolation ok handle=%u\n", create.handle);
    return 0;
}

static int check_syncobj(int fd)
{
    struct drm_syncobj_create_compat create;
    struct drm_syncobj_destroy_compat destroy;
    struct drm_syncobj_handle_compat handle_fd;
    struct drm_syncobj_wait_compat wait_req;
    struct drm_syncobj_timeline_wait_compat timeline_wait;
    struct drm_syncobj_array_compat array_req;
    struct drm_syncobj_timeline_array_compat timeline_array;
    uint32 handles[2];
    uint64 points[2];
    int syncfd = -1;

    memset(&create, 0, sizeof(create));
    create.flags = DRM_SYNCOBJ_CREATE_SIGNALED;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0 ||
        create.handle == 0)
        return fail("SYNCOBJ_CREATE signaled failed");
    handles[0] = create.handle;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) < 0 ||
        handle_fd.fd < 0)
        return fail("SYNCOBJ_HANDLE_TO_FD failed");
    syncfd = handle_fd.fd;

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
        return fail("SYNCOBJ_DESTROY original failed");

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = syncfd;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) < 0 ||
        handle_fd.handle == 0)
        return fail("SYNCOBJ_FD_TO_HANDLE failed");
    handles[0] = handle_fd.handle;

    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)handles;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) < 0 ||
        wait_req.first_signaled != 0)
        return fail("SYNCOBJ_WAIT signaled failed");

    memset(&array_req, 0, sizeof(array_req));
    array_req.handles = (uint64)handles;
    array_req.count_handles = 1;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &array_req) < 0)
        return fail("SYNCOBJ_RESET failed");
    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)handles;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) >= 0)
        return fail("SYNCOBJ_WAIT accepted reset object");

    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &array_req) < 0)
        return fail("SYNCOBJ_SIGNAL failed");
    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)handles;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) < 0)
        return fail("SYNCOBJ_WAIT after signal failed");

    points[0] = 7;
    memset(&timeline_array, 0, sizeof(timeline_array));
    timeline_array.handles = (uint64)handles;
    timeline_array.points = (uint64)points;
    timeline_array.count_handles = 1;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &timeline_array) < 0)
        return fail("SYNCOBJ_TIMELINE_SIGNAL failed");

    points[0] = 0;
    memset(&timeline_array, 0, sizeof(timeline_array));
    timeline_array.handles = (uint64)handles;
    timeline_array.points = (uint64)points;
    timeline_array.count_handles = 1;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &timeline_array) < 0 ||
        points[0] != 7)
        return fail("SYNCOBJ_QUERY timeline mismatch");

    points[0] = 7;
    memset(&timeline_wait, 0, sizeof(timeline_wait));
    timeline_wait.handles = (uint64)handles;
    timeline_wait.points = (uint64)points;
    timeline_wait.count_handles = 1;
    timeline_wait.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &timeline_wait) < 0)
        return fail("SYNCOBJ_TIMELINE_WAIT failed");
    points[0] = 8;
    memset(&timeline_wait, 0, sizeof(timeline_wait));
    timeline_wait.handles = (uint64)handles;
    timeline_wait.points = (uint64)points;
    timeline_wait.count_handles = 1;
    timeline_wait.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &timeline_wait) >= 0)
        return fail("SYNCOBJ_TIMELINE_WAIT accepted unsignaled point");

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = handles[0];
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
        return fail("SYNCOBJ_DESTROY imported failed");
    close(syncfd);

    printf("drmiftest: syncobj ok handle=%u\n", handles[0]);
    return 0;
}

static int check_nouveau(int fd)
{
    struct drm_nouveau_getparam_compat getp;
    struct drm_nouveau_channel_alloc_compat chan;
    uint64 device;

    memset(&getp, 0, sizeof(getp));
    getp.param = NOUVEAU_GETPARAM_PCI_VENDOR;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GETPARAM, &getp) < 0) {
        printf("drmiftest: nouveau absent fail-closed ok\n");
        return 0;
    }
    if (getp.value != 0x10de)
        return fail("Nouveau PCI vendor mismatch");

    getp.param = NOUVEAU_GETPARAM_PCI_DEVICE;
    getp.value = 0;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GETPARAM, &getp) < 0 || getp.value == 0)
        return fail("Nouveau PCI device getparam failed");
    device = getp.value;
    getp.param = NOUVEAU_GETPARAM_HAS_BO_USAGE;
    getp.value = 0;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GETPARAM, &getp) < 0 || getp.value != 1)
        return fail("Nouveau BO usage getparam failed");

    memset(&chan, 0, sizeof(chan));
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, &chan) < 0)
        return fail("Nouveau channel alloc failed");
    printf("drmiftest: nouveau probe ok device=0x%lx channel=%d domains=0x%x\n",
           device, chan.channel, chan.pushbuf_domains);
    return 0;
}

int main(void)
{
    int primary = open("/dev/dri/card0", O_RDWR);
    int render = open("/dev/dri/renderD128", O_RDWR);
    int ret = 1;

    if (primary < 0 || render < 0) {
        printf("drmiftest: open card/render failed primary=%d render=%d\n",
               primary, render);
        goto out;
    }
    if (check_common(primary, "primary") != 0)
        goto out;
    if (check_primary(primary) != 0)
        goto out;
    if (check_kms_fb(primary) != 0)
        goto out;
    if (check_common(render, "render") != 0)
        goto out;
    if (check_render_policy(render) != 0)
        goto out;
    if (check_dumb(render) != 0)
        goto out;
    if (check_gem_mmap_isolation(render, primary) != 0)
        goto out;
    if (check_syncobj(render) != 0)
        goto out;
    if (check_nouveau(render) != 0)
        goto out;

    printf("drmiftest: ok\n");
    ret = 0;

out:
    if (primary >= 0)
        close(primary);
    if (render >= 0)
        close(render);
    return ret;
}

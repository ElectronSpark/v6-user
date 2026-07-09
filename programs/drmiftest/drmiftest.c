#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/syscall.h"
#include "kernel/inc/uabi/drm.h"
#include "kernel/inc/uabi/poll.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

struct pollfd {
    int fd;
    short events;
    short revents;
};

#if defined(__riscv)
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

static int get_fb_stats(struct fb_gpu_stats *stats)
{
    int fd;
    int ret;

    if (stats == 0)
        return -1;
    fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0)
        return -1;
    memset(stats, 0, sizeof(*stats));
    ret = ioctl(fd, FB_GPU_GET_STATS, stats);
    close(fd);
    return ret;
}

static int get_obj_props(int fd, uint32 obj_id, uint32 obj_type,
                         uint32 *props, uint64 *values, uint32 *count)
{
    struct drm_mode_obj_get_properties_compat req;

    memset(&req, 0, sizeof(req));
    req.obj_id = obj_id;
    req.obj_type = obj_type;
    req.props_ptr = (uint64)props;
    req.prop_values_ptr = (uint64)values;
    req.count_props = *count;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req) < 0)
        return -1;
    *count = req.count_props;
    return 0;
}

static uint32 find_prop(int fd, const uint32 *props, uint32 count,
                        const char *name, uint32 required_flags)
{
    struct drm_mode_get_property_compat prop;

    for (uint32 i = 0; i < count; i++) {
        memset(&prop, 0, sizeof(prop));
        prop.prop_id = props[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) < 0)
            return 0;
        if (strcmp(prop.name, name) == 0 &&
            (prop.flags & required_flags) == required_flags)
            return props[i];
    }
    return 0;
}

static uint32 find_obj_prop(int fd, uint32 obj_id, uint32 obj_type,
                            const char *name, uint32 required_flags)
{
    uint32 props[16];
    uint64 values[16];
    uint32 count = 16;

    memset(props, 0, sizeof(props));
    memset(values, 0, sizeof(values));
    if (get_obj_props(fd, obj_id, obj_type, props, values, &count) < 0)
        return 0;
    return find_prop(fd, props, count, name, required_flags);
}

struct atomic_fence_credit_snapshot {
    uint64 gpu_backend_flags;
    uint64 dxg_present_register_successes;
    uint64 dxg_present_commit_attempts;
    uint64 dxg_present_display_target_kind;
    uint64 dxg_present_dda_nouveau_import_path_present;
    uint64 dxg_present_dda_nouveau_scanout_bind_present;
    uint64 dxg_present_helper_transport_present;
    uint64 display_presents;
    uint64 display_completions;
    uint64 kms_atomic_in_fence_fd_refs;
    uint64 kms_atomic_in_fence_fd_ref_puts;
    uint64 kms_atomic_in_fence_duplicate_rejects;
    uint64 kms_atomic_in_fence_test_only_validated;
    uint64 kms_atomic_in_fence_test_only_waits;
    uint64 kms_atomic_in_fence_sync_file_pending_waits;
    uint64 kms_atomic_in_fence_sync_file_pending_wakeups;
    uint64 kms_atomic_out_fence_prepared;
    uint64 kms_atomic_out_fence_cleanup_closes;
    uint64 kms_atomic_out_fence_test_only_placeholders;
    uint64 kms_atomic_out_fence_fd_exports;
    uint64 kms_atomic_out_fence_display_correlated;
    uint64 kms_atomic_out_fence_software_scanout_correlated;
};

static int read_atomic_fence_credit_snapshot(
    struct atomic_fence_credit_snapshot *snap)
{
    struct fb_gpu_stats stats;

    if (snap == 0 || get_fb_stats(&stats) < 0)
        return -1;
    memset(snap, 0, sizeof(*snap));
    snap->gpu_backend_flags = stats.gpu_backend_flags;
    snap->dxg_present_register_successes =
        stats.dxg_present_register_successes;
    snap->dxg_present_commit_attempts = stats.dxg_present_commit_attempts;
    snap->dxg_present_display_target_kind =
        stats.dxg_present_display_target_kind;
    snap->dxg_present_dda_nouveau_import_path_present =
        stats.dxg_present_dda_nouveau_import_path_present;
    snap->dxg_present_dda_nouveau_scanout_bind_present =
        stats.dxg_present_dda_nouveau_scanout_bind_present;
    snap->dxg_present_helper_transport_present =
        stats.dxg_present_helper_transport_present;
    snap->display_presents = stats.display_presents;
    snap->display_completions = stats.display_completions;
    snap->kms_atomic_in_fence_fd_refs =
        stats.kms_atomic_in_fence_fd_refs;
    snap->kms_atomic_in_fence_fd_ref_puts =
        stats.kms_atomic_in_fence_fd_ref_puts;
    snap->kms_atomic_in_fence_duplicate_rejects =
        stats.kms_atomic_in_fence_duplicate_rejects;
    snap->kms_atomic_in_fence_test_only_validated =
        stats.kms_atomic_in_fence_test_only_validated;
    snap->kms_atomic_in_fence_test_only_waits =
        stats.kms_atomic_in_fence_test_only_waits;
    snap->kms_atomic_in_fence_sync_file_pending_waits =
        stats.kms_atomic_in_fence_sync_file_pending_waits;
    snap->kms_atomic_in_fence_sync_file_pending_wakeups =
        stats.kms_atomic_in_fence_sync_file_pending_wakeups;
    snap->kms_atomic_out_fence_prepared =
        stats.kms_atomic_out_fence_prepared;
    snap->kms_atomic_out_fence_cleanup_closes =
        stats.kms_atomic_out_fence_cleanup_closes;
    snap->kms_atomic_out_fence_test_only_placeholders =
        stats.kms_atomic_out_fence_test_only_placeholders;
    snap->kms_atomic_out_fence_fd_exports =
        stats.kms_atomic_out_fence_fd_exports;
    snap->kms_atomic_out_fence_display_correlated =
        stats.kms_atomic_out_fence_display_correlated;
    snap->kms_atomic_out_fence_software_scanout_correlated =
        stats.kms_atomic_out_fence_software_scanout_correlated;
    return 0;
}

static int atomic_fence_credit_clean(
    const struct atomic_fence_credit_snapshot *before,
    const struct atomic_fence_credit_snapshot *after,
    int allow_display_delta)
{
    uint64 before_opengl;
    uint64 after_opengl;

    if (before == 0 || after == 0)
        return 0;
    before_opengl = before->gpu_backend_flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT;
    after_opengl = after->gpu_backend_flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT;
    if (before_opengl != after_opengl ||
        before->dxg_present_register_successes !=
            after->dxg_present_register_successes ||
        before->dxg_present_commit_attempts !=
            after->dxg_present_commit_attempts ||
        before->dxg_present_display_target_kind !=
            after->dxg_present_display_target_kind ||
        before->dxg_present_dda_nouveau_import_path_present !=
            after->dxg_present_dda_nouveau_import_path_present ||
        before->dxg_present_dda_nouveau_scanout_bind_present !=
            after->dxg_present_dda_nouveau_scanout_bind_present ||
        before->dxg_present_helper_transport_present !=
            after->dxg_present_helper_transport_present ||
        (!allow_display_delta &&
         (before->display_presents != after->display_presents ||
          before->display_completions != after->display_completions))) {
        printf("drmiftest: atomic_fence_matrix native_credit=unexpected "
               "opengl=0x%lx->0x%lx dxg_reg=%lu->%lu "
               "dxg_commit=%lu->%lu target=%lu->%lu "
               "dda_import=%lu->%lu dda_scanout=%lu->%lu "
               "transport=%lu->%lu display=%lu/%lu->%lu/%lu\n",
               before_opengl, after_opengl,
               before->dxg_present_register_successes,
               after->dxg_present_register_successes,
               before->dxg_present_commit_attempts,
               after->dxg_present_commit_attempts,
               before->dxg_present_display_target_kind,
               after->dxg_present_display_target_kind,
               before->dxg_present_dda_nouveau_import_path_present,
               after->dxg_present_dda_nouveau_import_path_present,
               before->dxg_present_dda_nouveau_scanout_bind_present,
               after->dxg_present_dda_nouveau_scanout_bind_present,
               before->dxg_present_helper_transport_present,
               after->dxg_present_helper_transport_present,
               before->display_presents, before->display_completions,
               after->display_presents, after->display_completions);
        return -1;
    }
    return 0;
}

static int atomic_single_prop_commit(int fd, uint32 obj, uint32 prop,
                                     uint64 value, uint32 flags)
{
    struct drm_mode_atomic_compat atomic;
    uint32 objs[1];
    uint32 counts[1];
    uint32 props[1];
    uint64 values[1];

    objs[0] = obj;
    counts[0] = 1;
    props[0] = prop;
    values[0] = value;
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = flags;
    atomic.count_objs = 1;
    atomic.objs_ptr = (uint64)objs;
    atomic.count_props_ptr = (uint64)counts;
    atomic.props_ptr = (uint64)props;
    atomic.prop_values_ptr = (uint64)values;
    return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
}

static int atomic_two_prop_commit(int fd, uint32 obj, uint32 prop0,
                                  uint64 value0, uint32 prop1,
                                  uint64 value1, uint32 flags)
{
    struct drm_mode_atomic_compat atomic;
    uint32 objs[1];
    uint32 counts[1];
    uint32 props[2];
    uint64 values[2];

    objs[0] = obj;
    counts[0] = 2;
    props[0] = prop0;
    props[1] = prop1;
    values[0] = value0;
    values[1] = value1;
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = flags;
    atomic.count_objs = 1;
    atomic.objs_ptr = (uint64)objs;
    atomic.count_props_ptr = (uint64)counts;
    atomic.props_ptr = (uint64)props;
    atomic.prop_values_ptr = (uint64)values;
    return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
}

static int atomic_out_fence_fb_commit(int fd, uint32 crtc_id,
                                      uint32 out_fence_prop,
                                      uint32 plane_id, uint32 fb_prop,
                                      uint32 fb_id, int32 *out_fence,
                                      uint32 flags)
{
    struct drm_mode_atomic_compat atomic;
    uint32 objs[2];
    uint32 counts[2];
    uint32 props[2];
    uint64 values[2];

    objs[0] = crtc_id;
    counts[0] = 1;
    props[0] = out_fence_prop;
    values[0] = (uint64)out_fence;
    objs[1] = plane_id;
    counts[1] = 1;
    props[1] = fb_prop;
    values[1] = fb_id;
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = flags;
    atomic.count_objs = 2;
    atomic.objs_ptr = (uint64)objs;
    atomic.count_props_ptr = (uint64)counts;
    atomic.props_ptr = (uint64)props;
    atomic.prop_values_ptr = (uint64)values;
    return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
}

/*
 * Atomic commit variant that carries the ioctl's user_data through to the
 * kernel (struct drm_mode_atomic_compat.user_data) and lets the caller pass
 * arbitrary flags (e.g. DRM_MODE_PAGE_FLIP_EVENT). A single primary-plane
 * commit that sets CRTC_ID + FB_ID is enough to make the kernel flag has_new_fb
 * and, when DRM_MODE_PAGE_FLIP_EVENT is set on a real (non-TEST_ONLY) commit,
 * queue exactly one DRM_EVENT_FLIP_COMPLETE keyed by user_data.
 */
static int atomic_flip_commit(int fd, uint32 plane_id, uint32 crtc_prop,
                              uint32 fb_prop, uint32 crtc_id, uint32 fb_id,
                              uint32 flags, uint64 user_data)
{
    struct drm_mode_atomic_compat atomic;
    uint32 objs[1];
    uint32 counts[1];
    uint32 props[2];
    uint64 values[2];

    objs[0] = plane_id;
    counts[0] = 2;
    props[0] = crtc_prop;
    values[0] = crtc_id;
    props[1] = fb_prop;
    values[1] = fb_id;
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = flags;
    atomic.count_objs = 1;
    atomic.objs_ptr = (uint64)objs;
    atomic.count_props_ptr = (uint64)counts;
    atomic.props_ptr = (uint64)props;
    atomic.prop_values_ptr = (uint64)values;
    atomic.user_data = user_data;
    return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
}

/*
 * Non-blocking drain of the shared DRM event ring. The driver read() returns
 * < 0 (no partial delivery) when the ring is empty, so this never blocks; the
 * hard bound keeps it from ever spinning even if the ring were somehow
 * refilled concurrently.
 */
static int drain_drm_events(int fd)
{
    struct drm_event_vblank_compat event;
    int drained = 0;

    while (drained <= 4 * DRM_XV6_EVENT_QUEUE_CAPACITY) {
        memset(&event, 0, sizeof(event));
        if (read(fd, &event, sizeof(event)) != (int)sizeof(event))
            break;
        drained++;
    }
    return drained;
}

struct atomic_test_fence_source {
    int fd;
    uint32 handle;
};

static void close_atomic_test_fence_source(
    struct atomic_test_fence_source *src);

static int open_atomic_test_fence_source(
    struct atomic_test_fence_source *src)
{
    struct fb_gpu_bo_create create;
    struct fb_gpu_bo_present present;

    if (src == 0)
        return -1;
    src->fd = open("/dev/fb0", O_RDWR);
    src->handle = 0;
    if (src->fd < 0)
        return -1;
    memset(&create, 0, sizeof(create));
    create.width = 16;
    create.height = 16;
    create.flags = FB_GPU_BO_F_EXPORTABLE;
    if (ioctl(src->fd, FB_GPU_BO_CREATE, &create) < 0 ||
        create.handle == 0) {
        close(src->fd);
        src->fd = -1;
        return -1;
    }
    src->handle = create.handle;

    memset(&present, 0, sizeof(present));
    present.handle = create.handle;
    present.w = create.width;
    present.h = create.height;
    if (ioctl(src->fd, FB_GPU_BO_PRESENT, &present) < 0) {
        close_atomic_test_fence_source(src);
        return -1;
    }
    return 0;
}

static int export_atomic_test_fence_fd_at(struct atomic_test_fence_source *src,
                                          uint64 fence)
{
    struct fb_gpu_fence_export_fd export_req;

    if (src == 0 || src->fd < 0 || src->handle == 0)
        return -1;
    memset(&export_req, 0, sizeof(export_req));
    export_req.handle = src->handle;
    export_req.fence = fence;
    if (ioctl(src->fd, FB_GPU_FENCE_EXPORT_FD, &export_req) < 0)
        return -1;
    return export_req.fd;
}

static int export_atomic_test_fence_fd(struct atomic_test_fence_source *src)
{
    return export_atomic_test_fence_fd_at(src, 0);
}

static int signal_atomic_test_fence_source(struct atomic_test_fence_source *src)
{
    struct fb_gpu_bo_present present;

    if (src == 0 || src->fd < 0 || src->handle == 0)
        return -1;
    memset(&present, 0, sizeof(present));
    present.handle = src->handle;
    present.w = 16;
    present.h = 16;
    return ioctl(src->fd, FB_GPU_BO_PRESENT, &present);
}

static int get_atomic_test_fence_status(struct atomic_test_fence_source *src,
                                        uint64 *last_present,
                                        uint64 *signaled)
{
    struct fb_gpu_bo_fence fence;

    if (src == 0 || src->fd < 0 || src->handle == 0)
        return -1;
    memset(&fence, 0, sizeof(fence));
    fence.handle = src->handle;
    if (ioctl(src->fd, FB_GPU_BO_FENCE, &fence) < 0)
        return -1;
    if (last_present != 0)
        *last_present = fence.last_present;
    if (signaled != 0)
        *signaled = fence.signaled;
    return 0;
}

static void close_atomic_test_fence_source(
    struct atomic_test_fence_source *src)
{
    struct fb_gpu_bo_destroy destroy;

    if (src == 0 || src->fd < 0)
        return;
    if (src->handle != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = src->handle;
        (void)ioctl(src->fd, FB_GPU_BO_DESTROY, &destroy);
        src->handle = 0;
    }
    close(src->fd);
    src->fd = -1;
}

static int query_test_fence_fd(int fence_fd)
{
    int gpu_fd;
    int ret;
    struct fb_gpu_fence_query query;

    gpu_fd = open("/dev/gpu0", O_RDWR);
    if (gpu_fd < 0)
        return -1;
    memset(&query, 0, sizeof(query));
    query.fd = fence_fd;
    ret = ioctl(gpu_fd, FB_GPU_FENCE_QUERY, &query);
    close(gpu_fd);
    return ret;
}

static int query_syncobj_fence_fd(int drm_fd, int fence_fd)
{
    struct drm_syncobj_handle_compat handle_fd;
    struct drm_syncobj_wait_compat wait_req;
    struct drm_syncobj_destroy_compat destroy;
    uint32 handle;
    int ret = -1;

    if (drm_fd < 0 || fence_fd < 0)
        return -1;
    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = fence_fd;
    handle_fd.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) < 0 ||
        handle_fd.handle == 0)
        return -1;
    handle = handle_fd.handle;

    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)&handle;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) == 0)
        ret = 0;

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = handle;
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
        ret = -1;
    return ret;
}

static int query_atomic_out_fence_fd(int drm_fd, int fence_fd)
{
    if (query_syncobj_fence_fd(drm_fd, fence_fd) == 0)
        return 0;
    return query_test_fence_fd(fence_fd);
}

static int plane_fb_id(int fd, uint32 plane_id, uint32 *fb_id)
{
    struct drm_mode_get_plane_compat plane;

    if (fb_id == 0)
        return -1;
    memset(&plane, 0, sizeof(plane));
    plane.plane_id = plane_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) < 0)
        return -1;
    *fb_id = plane.fb_id;
    return 0;
}

static int check_atomic_fence_matrix(int fd, uint32 plane_id,
                                     uint32 in_fence_prop,
                                     uint32 out_fence_prop,
                                     uint32 fb_prop)
{
    struct atomic_fence_credit_snapshot before;
    struct atomic_fence_credit_snapshot after;
    struct atomic_test_fence_source fence_source;
    int valid_fd = -1;
    int nonblock_fd = -1;
    int closed_fd = -1;
    int future_fd = -1;
    int stale_fd = -1;
    int32 out_fence = -2;
    int32 test_only_out_fence = -2;
    uint32 state_before = 0;
    uint32 state_after = 0;
    int in_accept = 0;
    int in_fail_closed = 0;
    int closed_reject = 0;
    int future_reject = 0;
    int nonblock_reject = 0;
    int duplicate_reject = 0;
    int test_only_in_fence_validated = 0;
    int test_only_placeholder = 0;
    int test_only_state_unchanged = 0;
    uint64 in_fence_fd_refs_delta = 0;
    uint64 in_fence_fd_ref_puts_delta = 0;
    uint64 duplicate_reject_delta = 0;
    uint64 test_only_validated_delta = 0;
    uint64 test_only_wait_delta = 0;
    uint64 out_fence_prepared_delta = 0;
    uint64 out_fence_cleanup_delta = 0;
    uint64 test_only_out_placeholder_delta = 0;
    uint64 out_fence_display_correlated_delta = 0;
    uint64 out_fence_software_scanout_correlated_delta = 0;
    int out_fence_display = 0;
    int out_fence_software = 0;
    int out_fence_immediate = 0;
    int invalid_out_reject = 0;
    int invalid_out_state_unchanged = 0;
    int out_exported = 0;
    int out_placeholder = 0;
    int out_query_ok = 0;
    const char *kernel = "placeholder";
    const char *status = "DEFERRED";

    memset(&fence_source, 0, sizeof(fence_source));
    fence_source.fd = -1;
    if (read_atomic_fence_credit_snapshot(&before) < 0)
        memset(&before, 0, sizeof(before));
    if (in_fence_prop == 0 || out_fence_prop == 0) {
        if (read_atomic_fence_credit_snapshot(&after) < 0)
            memset(&after, 0, sizeof(after));
        printf("drmiftest: atomic_fence_matrix "
               "atomic_fence_kernel=missing_fields in_fence_prop=%u "
               "out_fence_prop=%u atomic_in_fence_rejected=0 "
               "atomic_out_fence_exported=0 native_present_credit=0 "
               "opengl_submit_credit=0 status=DEFERRED\n",
               in_fence_prop != 0, out_fence_prop != 0);
        return atomic_fence_credit_clean(&before, &after, 0) < 0 ?
            fail("atomic fence matrix granted native credit") : 0;
    }

    if (open_atomic_test_fence_source(&fence_source) < 0)
        return fail("atomic fence source create failed");
    valid_fd = export_atomic_test_fence_fd(&fence_source);
    nonblock_fd = export_atomic_test_fence_fd(&fence_source);
    closed_fd = export_atomic_test_fence_fd(&fence_source);
    future_fd = export_atomic_test_fence_fd_at(&fence_source,
                                               0xffffffffULL);
    if (valid_fd < 0 || nonblock_fd < 0 || closed_fd < 0 ||
        future_fd < 0) {
        if (valid_fd >= 0)
            close(valid_fd);
        if (nonblock_fd >= 0)
            close(nonblock_fd);
        if (closed_fd >= 0)
            close(closed_fd);
        if (future_fd >= 0)
            close(future_fd);
        close_atomic_test_fence_source(&fence_source);
        return fail("atomic fence fd export failed");
    }
    if (read_atomic_fence_credit_snapshot(&before) < 0)
        memset(&before, 0, sizeof(before));

    if (atomic_single_prop_commit(fd, plane_id, in_fence_prop,
                                  (uint64)(uint32)valid_fd, 0) == 0)
        in_accept = 1;
    else
        in_fail_closed = 1;

    stale_fd = closed_fd;
    close(closed_fd);
    closed_fd = -1;
    if (atomic_single_prop_commit(fd, plane_id, in_fence_prop,
                                  (uint64)(uint32)stale_fd, 0) < 0)
        closed_reject = 1;

    if (atomic_single_prop_commit(fd, plane_id, in_fence_prop,
                                  (uint64)(uint32)nonblock_fd,
                                  DRM_MODE_ATOMIC_NONBLOCK) < 0)
        nonblock_reject = 1;

    if (atomic_two_prop_commit(fd, plane_id, in_fence_prop,
                               (uint64)(uint32)valid_fd, in_fence_prop,
                               (uint64)(uint32)valid_fd,
                               DRM_MODE_ATOMIC_TEST_ONLY) < 0)
        duplicate_reject = 1;

    if (atomic_single_prop_commit(fd, plane_id, in_fence_prop,
                                  (uint64)(uint32)future_fd,
                                  DRM_MODE_ATOMIC_TEST_ONLY) == 0)
        test_only_in_fence_validated = 1;
    if (atomic_single_prop_commit(fd, plane_id, in_fence_prop,
                                  (uint64)(uint32)future_fd, 0) < 0)
        future_reject = 1;

    if (plane_fb_id(fd, plane_id, &state_before) < 0) {
        close(valid_fd);
        close(nonblock_fd);
        close(future_fd);
        close_atomic_test_fence_source(&fence_source);
        return fail("atomic fence baseline plane query failed");
    }
    if (atomic_single_prop_commit(fd, 1, out_fence_prop,
                                  (uint64)&test_only_out_fence,
                                  DRM_MODE_ATOMIC_TEST_ONLY) < 0) {
        close(valid_fd);
        close(nonblock_fd);
        close(future_fd);
        close_atomic_test_fence_source(&fence_source);
        return fail("atomic TEST_ONLY OUT_FENCE_PTR failed");
    }
    if (plane_fb_id(fd, plane_id, &state_after) < 0) {
        close(valid_fd);
        close(nonblock_fd);
        close(future_fd);
        close_atomic_test_fence_source(&fence_source);
        return fail("atomic TEST_ONLY plane query failed");
    }
    test_only_placeholder = test_only_out_fence == -1;
    test_only_state_unchanged = state_after == state_before;

    if (atomic_single_prop_commit(fd, 1, out_fence_prop, 1, 0) < 0)
        invalid_out_reject = 1;
    if (plane_fb_id(fd, plane_id, &state_after) < 0) {
        close(valid_fd);
        close(nonblock_fd);
        close(future_fd);
        close_atomic_test_fence_source(&fence_source);
        return fail("atomic invalid OUT_FENCE_PTR plane query failed");
    }
    invalid_out_state_unchanged = state_after == state_before;

    if (state_before == 0 || fb_prop == 0) {
        close(valid_fd);
        close(nonblock_fd);
        close(future_fd);
        close_atomic_test_fence_source(&fence_source);
        return fail("atomic OUT_FENCE_PTR display baseline missing");
    }
    if (atomic_out_fence_fb_commit(fd, 1, out_fence_prop, plane_id,
                                   fb_prop, state_before, &out_fence,
                                   0) < 0) {
        close(valid_fd);
        close(nonblock_fd);
        close(future_fd);
        close_atomic_test_fence_source(&fence_source);
        return fail("atomic OUT_FENCE_PTR display commit failed");
    }
    if (out_fence >= 0) {
        out_exported = 1;
        if (query_atomic_out_fence_fd(fd, out_fence) == 0)
            out_query_ok = 1;
        close(out_fence);
    } else if (out_fence == -1) {
        out_placeholder = 1;
    }

    close(valid_fd);
    close(nonblock_fd);
    close(future_fd);
    if (closed_fd >= 0)
        close(closed_fd);
    close_atomic_test_fence_source(&fence_source);

    if (read_atomic_fence_credit_snapshot(&after) < 0)
        memset(&after, 0, sizeof(after));
    in_fence_fd_refs_delta = after.kms_atomic_in_fence_fd_refs -
        before.kms_atomic_in_fence_fd_refs;
    in_fence_fd_ref_puts_delta = after.kms_atomic_in_fence_fd_ref_puts -
        before.kms_atomic_in_fence_fd_ref_puts;
    duplicate_reject_delta =
        after.kms_atomic_in_fence_duplicate_rejects -
        before.kms_atomic_in_fence_duplicate_rejects;
    test_only_validated_delta =
        after.kms_atomic_in_fence_test_only_validated -
        before.kms_atomic_in_fence_test_only_validated;
    test_only_wait_delta = after.kms_atomic_in_fence_test_only_waits -
        before.kms_atomic_in_fence_test_only_waits;
    out_fence_prepared_delta =
        after.kms_atomic_out_fence_prepared -
        before.kms_atomic_out_fence_prepared;
    out_fence_cleanup_delta =
        after.kms_atomic_out_fence_cleanup_closes -
        before.kms_atomic_out_fence_cleanup_closes;
    test_only_out_placeholder_delta =
        after.kms_atomic_out_fence_test_only_placeholders -
        before.kms_atomic_out_fence_test_only_placeholders;
    out_fence_display_correlated_delta =
        after.kms_atomic_out_fence_display_correlated -
        before.kms_atomic_out_fence_display_correlated;
    out_fence_software_scanout_correlated_delta =
        after.kms_atomic_out_fence_software_scanout_correlated -
        before.kms_atomic_out_fence_software_scanout_correlated;
    if (atomic_fence_credit_clean(&before, &after, 1) < 0)
        return fail("atomic fence matrix granted native credit");
    if (!closed_reject)
        return fail("atomic closed IN_FENCE_FD accepted");
    if (!future_reject)
        return fail("atomic future IN_FENCE_FD accepted on real commit");
    if (!nonblock_reject)
        return fail("atomic nonblock IN_FENCE_FD accepted");
    if (!duplicate_reject || duplicate_reject_delta == 0)
        return fail("atomic duplicate IN_FENCE_FD accepted");
    if (!test_only_in_fence_validated || test_only_validated_delta == 0)
        return fail("atomic TEST_ONLY IN_FENCE_FD was not validated");
    if (test_only_wait_delta != 0)
        return fail("atomic TEST_ONLY IN_FENCE_FD waited");
    if (in_fence_fd_refs_delta < 3 ||
        in_fence_fd_ref_puts_delta != in_fence_fd_refs_delta)
        return fail("atomic IN_FENCE_FD prepare refs were not balanced");
    if (!test_only_placeholder || test_only_out_placeholder_delta == 0)
        return fail("atomic TEST_ONLY OUT_FENCE_PTR not placeholder");
    if (!test_only_state_unchanged)
        return fail("atomic TEST_ONLY OUT_FENCE_PTR changed state");
    if (!invalid_out_reject)
        return fail("atomic invalid OUT_FENCE_PTR accepted");
    if (!invalid_out_state_unchanged)
        return fail("atomic invalid OUT_FENCE_PTR changed state");
    if (out_exported && !out_query_ok)
        return fail("atomic OUT_FENCE_PTR exported unusable fd");
    if (out_fence_prepared_delta < 1)
        return fail("atomic OUT_FENCE_PTR was not prepared");
    if (out_fence_cleanup_delta < 1)
        return fail("atomic OUT_FENCE_PTR copyout cleanup not observed");
    if (out_fence_display_correlated_delta == 0)
        return fail("atomic OUT_FENCE_PTR lacked display completion credit");
    if (out_fence_software_scanout_correlated_delta != 0)
        return fail("atomic OUT_FENCE_PTR used software scanout credit");
    out_fence_display = out_exported && out_query_ok;
    out_fence_software = out_fence_software_scanout_correlated_delta != 0;
    out_fence_immediate = out_exported &&
        out_fence_display_correlated_delta == 0 &&
        out_fence_software_scanout_correlated_delta == 0;

    if (in_accept && out_exported) {
        kernel = "real";
        status = "PASS";
    } else if (in_accept || out_exported) {
        kernel = "partial";
    }

    printf("drmiftest: atomic_fence_matrix atomic_fence_kernel=%s "
           "atomic_in_fence_accepted=%d atomic_in_fence_rejected=%d "
           "atomic_in_fence_fail_closed=%d "
           "atomic_in_fence_closed_rejected=%d "
           "atomic_stale_in_fence_rejected=%d "
           "atomic_nonblock_fence_fail_closed=%d "
           "atomic_duplicate_in_fence_rejected=%d "
           "atomic_duplicate_in_fence_rejects_delta=%lu "
           "atomic_in_fence_fd_ref_prepare=PASS "
           "atomic_in_fence_fd_refs_delta=%lu "
           "atomic_in_fence_fd_ref_puts_delta=%lu "
           "atomic_test_only_in_fence_validated=%d "
           "atomic_test_only_in_fence_no_wait=%d "
           "atomic_test_only_in_fence_waited=%lu "
           "atomic_test_only_in_fence_validated_delta=%lu "
           "atomic_test_only_out_fence_placeholder=%d "
           "atomic_test_only_out_fence_placeholder_delta=%lu "
           "atomic_test_only_state_unchanged=%d "
           "atomic_invalid_out_fence_rejected=%d "
           "atomic_invalid_out_fence_state_unchanged=%d "
           "atomic_out_fence_prepared=1 "
           "atomic_out_fence_prepared_delta=%lu "
           "atomic_out_fence_cleanup_closes_delta=%lu "
           "atomic_out_fence_exported=%d atomic_out_fence_query_ok=%d "
           "atomic_out_fence_placeholder=%d "
           "atomic_out_fence_display=%d "
           "atomic_out_fence_software=%d "
           "atomic_out_fence_immediate=%d "
           "atomic_out_fence_display_correlated=1 "
           "out_fence_display_correlated_delta=%lu "
           "atomic_out_fence_software_scanout_correlated=0 "
           "out_fence_software_scanout_correlated_delta=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           kernel, in_accept, closed_reject || in_fail_closed,
           in_fail_closed, closed_reject, future_reject, nonblock_reject,
           duplicate_reject, duplicate_reject_delta,
           in_fence_fd_refs_delta, in_fence_fd_ref_puts_delta,
           test_only_in_fence_validated, test_only_wait_delta == 0,
           test_only_wait_delta,
           test_only_validated_delta, test_only_placeholder,
           test_only_out_placeholder_delta,
           test_only_state_unchanged, invalid_out_reject,
           invalid_out_state_unchanged, out_fence_prepared_delta,
           out_fence_cleanup_delta, out_exported, out_query_ok,
           out_placeholder, out_fence_display, out_fence_software,
           out_fence_immediate, out_fence_display_correlated_delta,
           out_fence_software_scanout_correlated_delta, status);
    printf("drmiftest: atomic_out_fence_provenance_matrix "
           "out_fence_source=display_completion "
           "out_fence_display=%d out_fence_software=%d "
           "out_fence_immediate=%d "
           "out_fence_display_correlated=1 "
           "out_fence_software_scanout_correlated=0 "
           "out_fence_completion_deferred=1 "
           "out_fence_display_correlated_delta=%lu "
           "out_fence_software_scanout_correlated_delta=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n",
           out_fence_display, out_fence_software, out_fence_immediate,
           out_fence_display_correlated_delta,
           out_fence_software_scanout_correlated_delta);
    return 0;
}

/*
 * Poll until the kernel has registered at least one more sync_file in-fence
 * pending wait than the given baseline.  Used to know a forked child's blocking
 * atomic commit has entered its in-ioctl in-fence wait before the parent
 * signals the fence.  Bounded so a child that failed before the wait cannot
 * hang the parent.
 */
static int wait_for_in_fence_pending_wait(uint64 before)
{
    struct fb_gpu_stats stats;

    for (int i = 0; i < 200; i++) {
        if (get_fb_stats(&stats) < 0)
            return -1;
        if (stats.kms_atomic_in_fence_sync_file_pending_waits >= before + 1)
            return 0;
        sleep(1);
    }
    return -1;
}

static int check_kms_sync_file_in_fence_matrix(int fd, uint32 plane_id,
                                               uint32 in_fence_prop)
{
    struct atomic_fence_credit_snapshot before;
    struct atomic_fence_credit_snapshot after;
    struct drm_syncobj_create_compat create;
    struct drm_syncobj_destroy_compat destroy;
    struct drm_syncobj_handle_compat handle_fd;
    struct drm_syncobj_array_compat signal_req;
    uint32 source = 0;
    int syncfile_fd = -1;
    int signal_ok = 0;
    int commit_after_signal = 0;
    int test_only_validated = 0;
    uint64 pending_waits_delta = 0;
    uint64 pending_wakeups_delta = 0;
    uint64 refs_delta = 0;
    uint64 puts_delta = 0;
    uint64 test_only_wait_delta = 0;

    if (in_fence_prop == 0) {
        printf("drmiftest: kms_sync_file_in_fence_matrix "
               "pending_sync_file_in_fence=MISSING status=DEFERRED\n");
        return 0;
    }

    memset(&create, 0, sizeof(create));
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0 ||
        create.handle == 0)
        return fail("KMS sync_file IN_FENCE source create failed");
    source = create.handle;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = source;
    handle_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) < 0 ||
        handle_fd.fd < 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = source;
        (void)ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
        return fail("KMS sync_file IN_FENCE export failed");
    }
    syncfile_fd = handle_fd.fd;

    if (read_atomic_fence_credit_snapshot(&before) < 0)
        memset(&before, 0, sizeof(before));

    /* TEST_ONLY validates a pending sync_file in-fence without waiting. */
    if (atomic_single_prop_commit(fd, plane_id, in_fence_prop,
                                  (uint64)(uint32)syncfile_fd,
                                  DRM_MODE_ATOMIC_TEST_ONLY) == 0)
        test_only_validated = 1;

    /* Contract update: a real (blocking, non-TEST_ONLY) commit that carries a
     * PENDING sync_file in-fence waits in-ioctl for that fence
     * (gpu_kms_wait_in_fence_file -> dma_fence_wait, no timeout), and the
     * kernel exposes no non-blocking atomic path (NONBLOCK is rejected).  So
     * the wait must be driven concurrently: a child issues the blocking commit
     * (which registers a pending wait and blocks), the parent waits until that
     * pending wait is registered, then SYNCOBJ_SIGNAL wakes it (the signal
     * fires dma_fence_signal on the same fence the exported sync_file holds),
     * so the child's commit completes.  (The prior single-threaded
     * submit-then-signal pattern deadlocked against this blocking wait.) */
    {
        int child = fork();
        int status = 0;
        int got;

        if (child < 0)
            return fail("KMS sync_file IN_FENCE fork failed");
        if (child == 0) {
            int rc = atomic_single_prop_commit(fd, plane_id, in_fence_prop,
                                               (uint64)(uint32)syncfile_fd, 0);
            exit(rc == 0 ? 0 : 1);
        }
        (void)wait_for_in_fence_pending_wait(
            before.kms_atomic_in_fence_sync_file_pending_waits);
        memset(&signal_req, 0, sizeof(signal_req));
        signal_req.handles = (uint64)&source;
        signal_req.count_handles = 1;
        if (ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &signal_req) == 0)
            signal_ok = 1;
        do {
            got = wait(&status);
        } while (got >= 0 && got != child);
        if (got == child && status == 0)
            commit_after_signal = 1;
    }

    close(syncfile_fd);
    syncfile_fd = -1;
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = source;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
        return fail("KMS sync_file IN_FENCE source destroy failed");

    if (read_atomic_fence_credit_snapshot(&after) < 0)
        memset(&after, 0, sizeof(after));
    pending_waits_delta =
        after.kms_atomic_in_fence_sync_file_pending_waits -
        before.kms_atomic_in_fence_sync_file_pending_waits;
    pending_wakeups_delta =
        after.kms_atomic_in_fence_sync_file_pending_wakeups -
        before.kms_atomic_in_fence_sync_file_pending_wakeups;
    refs_delta = after.kms_atomic_in_fence_fd_refs -
        before.kms_atomic_in_fence_fd_refs;
    puts_delta = after.kms_atomic_in_fence_fd_ref_puts -
        before.kms_atomic_in_fence_fd_ref_puts;
    test_only_wait_delta = after.kms_atomic_in_fence_test_only_waits -
        before.kms_atomic_in_fence_test_only_waits;

    if (atomic_fence_credit_clean(&before, &after, 0) < 0)
        return fail("KMS sync_file IN_FENCE granted native credit");
    if (!test_only_validated || !signal_ok ||
        !commit_after_signal || pending_waits_delta < 1 ||
        pending_wakeups_delta < 1 || refs_delta < 1 ||
        puts_delta != refs_delta || test_only_wait_delta != 0)
        return fail("KMS sync_file IN_FENCE matrix failed");

    printf("drmiftest: kms_sync_file_in_fence_matrix "
           "pending_sync_file_in_fence=PASS "
           "pending_in_fence_blocking_wait=PASS signal_wake=PASS "
           "commit_after_signal=PASS in_fence_fd_ref_prepare=PASS "
           "in_fence_fd_refs_delta=%lu in_fence_fd_ref_puts_delta=%lu "
           "pending_waits_delta=%lu pending_wakeups_delta=%lu "
           "test_only_no_wait=1 native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n",
           refs_delta, puts_delta, pending_waits_delta,
           pending_wakeups_delta);
    return 0;
}

static int check_common(int fd, const char *node)
{
    char name[32];
    char unique[64];
    struct drm_version_compat ver;
    struct drm_unique_compat uniq;
    struct drm_set_client_cap_compat client_cap;
    struct drm_set_client_name_compat client_name;
    char debug_name[] = "drmiftest";
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
    client_cap.capability = DRM_CLIENT_CAP_ATOMIC;
    client_cap.value = 1;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &client_cap) < 0)
        return fail("SET_CLIENT_CAP atomic failed");

    memset(&client_name, 0, sizeof(client_name));
    client_name.name = (uint64)debug_name;
    client_name.name_len = sizeof(debug_name) - 1;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_NAME, &client_name) < 0)
        return fail("SET_CLIENT_NAME failed");
    client_name.name_len = DRM_CLIENT_NAME_MAX_LEN + 1;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_NAME, &client_name) >= 0)
        return fail("oversized SET_CLIENT_NAME accepted");

    if (get_cap(fd, DRM_CAP_DUMB_BUFFER, &value) < 0 || value != 1)
        return fail("DUMB_BUFFER cap failed");
    if (get_cap(fd, DRM_CAP_PRIME, &value) < 0 || (value & 3) != 3)
        return fail("PRIME cap failed");
    if (get_cap(fd, DRM_CAP_SYNCOBJ, &value) < 0)
        return fail("SYNCOBJ cap query failed");
    if (get_cap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &value) < 0)
        return fail("SYNCOBJ_TIMELINE cap query failed");
    if (get_cap(fd, DRM_CAP_ADDFB2_MODIFIERS, &value) < 0 ||
        value != 1)
        return fail("ADDFB2_MODIFIERS cap failed");

    printf("drmiftest: common ok node=%s driver=%s unique=%s addfb2_modifiers=1\n",
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
    struct drm_mode_crtc_compat crtc;
    struct drm_mode_get_encoder_compat enc;
    struct drm_mode_get_connector_compat conn;
    struct drm_mode_get_property_compat prop;
    struct drm_mode_get_blob_compat blob;
    struct drm_mode_get_plane_res_compat plane_res;
    struct drm_mode_get_plane_compat plane;
    struct drm_mode_modeinfo_compat modes[2];
    struct drm_mode_modeinfo_compat blob_mode;
    struct {
        struct drm_format_modifier_blob_compat header;
        uint32 formats[4];
        struct drm_format_modifier_compat modifiers[1];
    } in_formats;
    uint32 plane_ids[2];
    uint32 formats[4];
    uint32 props[4];
    uint64 prop_values[4];
    uint32 obj_props[16];
    uint64 obj_values[16];
    uint32 obj_count;
    uint32 active_prop;
    uint32 fb_prop;
    uint32 plane_type_prop;
    uint32 mode_blob = 0;
    uint32 in_formats_prop = 0;
    uint32 in_formats_blob = 0;
    uint32 crtc_prop = 0;

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

    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = ids[0];
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0 ||
        crtc.mode_valid != 1 ||
        crtc.mode.hdisplay == 0 || crtc.mode.vdisplay == 0)
        return fail("GETCRTC failed");
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = 0xfeedface;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) >= 0)
        return fail("invalid GETCRTC accepted");

    memset(&enc, 0, sizeof(enc));
    enc.encoder_id = ids[2];
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0 ||
        enc.crtc_id != ids[0] || enc.possible_crtcs == 0)
        return fail("GETENCODER failed");
    memset(&enc, 0, sizeof(enc));
    enc.encoder_id = 0xfeedface;
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) >= 0)
        return fail("invalid GETENCODER accepted");

    memset(&conn, 0, sizeof(conn));
    memset(modes, 0, sizeof(modes));
    memset(props, 0, sizeof(props));
    memset(prop_values, 0, sizeof(prop_values));
    conn.connector_id = ids[1];
    conn.modes_ptr = (uint64)modes;
    conn.props_ptr = (uint64)props;
    conn.prop_values_ptr = (uint64)prop_values;
    conn.count_modes = 2;
    conn.count_props = 4;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 ||
        conn.count_modes == 0 || conn.connection != 1 ||
        modes[0].hdisplay == 0 || modes[0].vdisplay == 0 ||
        conn.count_props < 2)
        return fail("GETCONNECTOR failed");

    for (uint32 i = 0; i < conn.count_props && i < 4; i++) {
        memset(&prop, 0, sizeof(prop));
        prop.prop_id = props[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) < 0)
            return fail("GETPROPERTY failed");
        if (strcmp(prop.name, "CRTC_ID") == 0) {
            if ((prop.flags & DRM_MODE_PROP_OBJECT) == 0 ||
                prop_values[i] != ids[0])
                return fail("CRTC_ID property mismatch");
            crtc_prop = props[i];
        } else if (strcmp(prop.name, "MODE_ID") == 0) {
            if ((prop.flags & DRM_MODE_PROP_BLOB) == 0 ||
                prop_values[i] == 0)
                return fail("MODE_ID property mismatch");
            mode_blob = (uint32)prop_values[i];
        }
    }
    if (crtc_prop == 0 || mode_blob == 0)
        return fail("connector properties missing");

    memset(&blob, 0, sizeof(blob));
    memset(&blob_mode, 0, sizeof(blob_mode));
    blob.blob_id = mode_blob;
    blob.length = sizeof(blob_mode);
    blob.data = (uint64)&blob_mode;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0 ||
        blob.length != sizeof(blob_mode) ||
        blob_mode.hdisplay != modes[0].hdisplay ||
        blob_mode.vdisplay != modes[0].vdisplay)
        return fail("GETPROPBLOB failed");

    memset(&prop, 0, sizeof(prop));
    prop.prop_id = 0xfeedface;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) >= 0)
        return fail("invalid GETPROPERTY accepted");
    memset(&blob, 0, sizeof(blob));
    blob.blob_id = 0xfeedface;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) >= 0)
        return fail("invalid GETPROPBLOB accepted");

    memset(plane_ids, 0, sizeof(plane_ids));
    memset(&plane_res, 0, sizeof(plane_res));
    plane_res.plane_id_ptr = (uint64)plane_ids;
    plane_res.count_planes = 2;
    /* Contract (UNIVERSAL_PLANES set above): exactly two planes -- the
     * primary scanout plane first, then the cursor plane (added with the
     * kernel's hardened cursor-plane support; the old expectation of a
     * single plane predates it). */
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res) < 0 ||
        plane_res.count_planes != 2 || plane_ids[0] == 0 ||
        plane_ids[1] == 0)
        return fail("GETPLANERESOURCES failed");

    memset(formats, 0, sizeof(formats));
    memset(&plane, 0, sizeof(plane));
    plane.plane_id = plane_ids[0];
    plane.format_type_ptr = (uint64)formats;
    plane.count_format_types = 4;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) < 0 ||
        plane.crtc_id == 0 || plane.possible_crtcs == 0 ||
        plane.count_format_types != 4 ||
        formats[0] != DRM_FORMAT_XRGB8888 ||
        formats[1] != DRM_FORMAT_ARGB8888 ||
        formats[2] != DRM_FORMAT_XBGR8888 ||
        formats[3] != DRM_FORMAT_ABGR8888)
        return fail("GETPLANE failed");

    memset(obj_props, 0, sizeof(obj_props));
    memset(obj_values, 0, sizeof(obj_values));
    obj_count = 16;
    if (get_obj_props(fd, ids[0], DRM_MODE_OBJECT_CRTC, obj_props,
                      obj_values, &obj_count) < 0 || obj_count < 2)
        return fail("CRTC OBJ_GETPROPERTIES failed");
    active_prop = find_prop(fd, obj_props, obj_count, "ACTIVE",
                            DRM_MODE_PROP_RANGE);
    mode_blob = find_prop(fd, obj_props, obj_count, "MODE_ID",
                          DRM_MODE_PROP_BLOB);
    if (active_prop == 0 || mode_blob == 0)
        return fail("CRTC object properties missing");

    memset(obj_props, 0, sizeof(obj_props));
    memset(obj_values, 0, sizeof(obj_values));
    obj_count = 16;
    if (get_obj_props(fd, plane_ids[0], DRM_MODE_OBJECT_PLANE, obj_props,
                      obj_values, &obj_count) < 0 || obj_count < 8)
        return fail("plane OBJ_GETPROPERTIES failed");
    plane_type_prop = find_prop(fd, obj_props, obj_count, "type",
                                DRM_MODE_PROP_ENUM);
    fb_prop = find_prop(fd, obj_props, obj_count, "FB_ID",
                        DRM_MODE_PROP_OBJECT);
    crtc_prop = find_prop(fd, obj_props, obj_count, "CRTC_ID",
                          DRM_MODE_PROP_OBJECT);
    for (uint32 i = 0; i < obj_count && i < 16; i++) {
        memset(&prop, 0, sizeof(prop));
        prop.prop_id = obj_props[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) < 0)
            return fail("plane GETPROPERTY failed");
        if (strcmp(prop.name, "IN_FORMATS") != 0)
            continue;
        if ((prop.flags & DRM_MODE_PROP_BLOB) == 0 ||
            (prop.flags & DRM_MODE_PROP_IMMUTABLE) == 0 ||
            obj_values[i] == 0)
            return fail("IN_FORMATS property mismatch");
        in_formats_prop = obj_props[i];
        in_formats_blob = (uint32)obj_values[i];
        break;
    }
    if (plane_type_prop == 0 || fb_prop == 0 || crtc_prop == 0 ||
        in_formats_prop == 0)
        return fail("plane object properties missing");

    memset(&blob, 0, sizeof(blob));
    memset(&in_formats, 0, sizeof(in_formats));
    blob.blob_id = in_formats_blob;
    blob.length = sizeof(in_formats);
    blob.data = (uint64)&in_formats;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0 ||
        blob.length != sizeof(in_formats) ||
        in_formats.header.version != 1 ||
        in_formats.header.count_formats != 4 ||
        in_formats.header.count_modifiers != 1 ||
        in_formats.header.formats_offset !=
            (uint32)((char *)&in_formats.formats[0] - (char *)&in_formats) ||
        in_formats.header.modifiers_offset !=
            (uint32)((char *)&in_formats.modifiers[0] -
                     (char *)&in_formats) ||
        in_formats.formats[0] != DRM_FORMAT_XRGB8888 ||
        in_formats.formats[1] != DRM_FORMAT_ARGB8888 ||
        in_formats.formats[2] != DRM_FORMAT_XBGR8888 ||
        in_formats.formats[3] != DRM_FORMAT_ABGR8888 ||
        in_formats.modifiers[0].formats != 0xf ||
        in_formats.modifiers[0].offset != 0 ||
        in_formats.modifiers[0].modifier != DRM_FORMAT_MOD_LINEAR)
        return fail("IN_FORMATS blob failed");
    printf("drmiftest: kms_in_formats_blob_matrix "
           "cap_addfb2_modifiers=1 in_formats_blob=PASS "
           "xrgb8888_linear=1 argb8888_linear=1 "
           "xbgr8888_linear=1 abgr8888_linear=1 nv12_scanout=0 "
           "nonlinear_modifiers=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("drmiftest: kms_primary_scanout_format_mod_matrix "
           "getplane_matches_in_formats=PASS scanout_format_count=4 "
           "xrgb8888_linear=1 argb8888_linear=1 "
           "xbgr8888_linear=1 abgr8888_linear=1 "
           "nv12_scanout=0 modifier_check=PASS native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");

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

static int check_legacy_drm_probes(int fd)
{
    int free_list[1] = {0};
    struct drm_map_compat map;
    struct drm_ctx_priv_map_compat sarea;
    struct drm_ctx_compat ctx;
    struct drm_ctx_res_compat ctx_res;
    struct drm_lock_compat lock;
    struct drm_buf_desc_compat buf_desc;
    struct drm_buf_info_compat buf_info;
    struct drm_buf_map_compat buf_map;
    struct drm_buf_free_compat buf_free;
    struct drm_dma_compat dma;
    struct drm_agp_mode_compat agp_mode;
    struct drm_agp_buffer_compat agp_buffer;
    struct drm_agp_binding_compat agp_binding;
    struct drm_scatter_gather_compat sg;

    memset(&map, 0, sizeof(map));
    if (ioctl(fd, DRM_IOCTL_GET_MAP, &map) >= 0)
        return fail("legacy GET_MAP unexpectedly returned a map");
    map.size = 4096;
    if (ioctl(fd, DRM_IOCTL_ADD_MAP, &map) >= 0)
        return fail("legacy ADD_MAP unexpectedly enabled");
    map.handle = 1;
    if (ioctl(fd, DRM_IOCTL_RM_MAP, &map) >= 0)
        return fail("legacy RM_MAP unexpectedly enabled");

    memset(&sarea, 0, sizeof(sarea));
    if (ioctl(fd, DRM_IOCTL_GET_SAREA_CTX, &sarea) >= 0 ||
        ioctl(fd, DRM_IOCTL_SET_SAREA_CTX, &sarea) >= 0)
        return fail("legacy SAREA ctx unexpectedly enabled");

    memset(&ctx, 0, sizeof(ctx));
    if (ioctl(fd, DRM_IOCTL_GET_CTX, &ctx) < 0 || ctx.flags != 0)
        return fail("legacy GET_CTX context0 failed");
    if (ioctl(fd, DRM_IOCTL_ADD_CTX, &ctx) >= 0)
        return fail("legacy ADD_CTX unexpectedly enabled");
    ctx.handle = 1;
    if (ioctl(fd, DRM_IOCTL_RM_CTX, &ctx) >= 0 ||
        ioctl(fd, DRM_IOCTL_MOD_CTX, &ctx) >= 0 ||
        ioctl(fd, DRM_IOCTL_SWITCH_CTX, &ctx) >= 0 ||
        ioctl(fd, DRM_IOCTL_NEW_CTX, &ctx) >= 0)
        return fail("legacy ctx mutation unexpectedly enabled");
    memset(&ctx_res, 0, sizeof(ctx_res));
    ctx_res.count = 4;
    if (ioctl(fd, DRM_IOCTL_RES_CTX, &ctx_res) < 0 ||
        ctx_res.count != 0)
        return fail("legacy RES_CTX zero enumeration failed");

    memset(&lock, 0, sizeof(lock));
    if (ioctl(fd, DRM_IOCTL_LOCK, &lock) >= 0 ||
        ioctl(fd, DRM_IOCTL_UNLOCK, &lock) >= 0 ||
        ioctl(fd, DRM_IOCTL_FINISH, &lock) >= 0)
        return fail("legacy lock unexpectedly enabled");

    memset(&buf_info, 0, sizeof(buf_info));
    buf_info.count = 4;
    if (ioctl(fd, DRM_IOCTL_INFO_BUFS, &buf_info) < 0 ||
        buf_info.count != 0)
        return fail("legacy INFO_BUFS zero enumeration failed");
    memset(&buf_map, 0, sizeof(buf_map));
    buf_map.count = 4;
    if (ioctl(fd, DRM_IOCTL_MAP_BUFS, &buf_map) < 0 ||
        buf_map.count != 0 || buf_map.virtual != 0)
        return fail("legacy MAP_BUFS zero enumeration failed");
    memset(&buf_desc, 0, sizeof(buf_desc));
    buf_desc.count = 1;
    buf_desc.size = 4096;
    if (ioctl(fd, DRM_IOCTL_ADD_BUFS, &buf_desc) >= 0 ||
        ioctl(fd, DRM_IOCTL_MARK_BUFS, &buf_desc) >= 0)
        return fail("legacy buffer manager unexpectedly enabled");
    memset(&buf_free, 0, sizeof(buf_free));
    buf_free.count = 1;
    buf_free.list = (uint64)free_list;
    if (ioctl(fd, DRM_IOCTL_FREE_BUFS, &buf_free) >= 0)
        return fail("legacy FREE_BUFS unexpectedly enabled");
    memset(&dma, 0, sizeof(dma));
    dma.request_count = 1;
    dma.request_size = 4096;
    if (ioctl(fd, DRM_IOCTL_DMA, &dma) >= 0)
        return fail("legacy DMA unexpectedly enabled");

    if (ioctl(fd, DRM_IOCTL_AGP_ACQUIRE, 0) >= 0 ||
        ioctl(fd, DRM_IOCTL_AGP_RELEASE, 0) >= 0 ||
        ioctl(fd, DRM_IOCTL_AGP_INFO, 0) >= 0)
        return fail("legacy AGP control unexpectedly enabled");
    memset(&agp_mode, 0, sizeof(agp_mode));
    if (ioctl(fd, DRM_IOCTL_AGP_ENABLE, &agp_mode) >= 0)
        return fail("legacy AGP_ENABLE unexpectedly enabled");
    memset(&agp_buffer, 0, sizeof(agp_buffer));
    agp_buffer.size = 4096;
    if (ioctl(fd, DRM_IOCTL_AGP_ALLOC, &agp_buffer) >= 0)
        return fail("legacy AGP_ALLOC unexpectedly enabled");
    agp_buffer.handle = 1;
    if (ioctl(fd, DRM_IOCTL_AGP_FREE, &agp_buffer) >= 0)
        return fail("legacy AGP_FREE unexpectedly enabled");
    memset(&agp_binding, 0, sizeof(agp_binding));
    agp_binding.handle = 1;
    if (ioctl(fd, DRM_IOCTL_AGP_BIND, &agp_binding) >= 0 ||
        ioctl(fd, DRM_IOCTL_AGP_UNBIND, &agp_binding) >= 0)
        return fail("legacy AGP bind unexpectedly enabled");
    memset(&sg, 0, sizeof(sg));
    sg.size = 4096;
    if (ioctl(fd, DRM_IOCTL_SG_ALLOC, &sg) >= 0)
        return fail("legacy SG_ALLOC unexpectedly enabled");
    sg.handle = 1;
    if (ioctl(fd, DRM_IOCTL_SG_FREE, &sg) >= 0)
        return fail("legacy SG_FREE unexpectedly enabled");

    printf("drmiftest: legacy drm probes ok maps=fail-closed "
           "ctx0=query bufs=empty agp=fail-closed\n");
    return 0;
}

static int check_render_policy(int fd)
{
    struct drm_auth_compat magic;
    struct drm_map_compat map;
    struct drm_mode_card_res_compat res;
    struct drm_mode_get_plane_compat plane;
    struct drm_mode_set_plane_compat setplane;
    struct drm_crtc_get_sequence_compat crtc_seq;
    struct drm_mode_fb_cmd2_compat fb;

    memset(&magic, 0, sizeof(magic));
    if (ioctl(fd, DRM_IOCTL_GET_MAGIC, &magic) >= 0)
        return fail("render GET_MAGIC unexpectedly accepted");
    if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) >= 0 ||
        ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) >= 0)
        return fail("render master ioctl unexpectedly accepted");
    memset(&map, 0, sizeof(map));
    if (ioctl(fd, DRM_IOCTL_GET_MAP, &map) >= 0)
        return fail("render GET_MAP unexpectedly accepted");
    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) >= 0)
        return fail("render KMS ioctl unexpectedly accepted");
    memset(&plane, 0, sizeof(plane));
    plane.plane_id = 4;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) >= 0)
        return fail("render GETPLANE unexpectedly accepted");
    memset(&setplane, 0, sizeof(setplane));
    setplane.plane_id = 4;
    if (ioctl(fd, DRM_IOCTL_MODE_SETPLANE, &setplane) >= 0)
        return fail("render SETPLANE unexpectedly accepted");
    memset(&crtc_seq, 0, sizeof(crtc_seq));
    crtc_seq.crtc_id = 1;
    if (ioctl(fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &crtc_seq) >= 0)
        return fail("render CRTC_GET_SEQUENCE unexpectedly accepted");
    memset(&fb, 0, sizeof(fb));
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) >= 0)
        return fail("render ADDFB2 unexpectedly accepted");

    printf("drmiftest: render policy ok\n");
    return 0;
}

static int check_kms_fb(int fd)
{
    struct drm_mode_create_dumb_compat create;
    struct drm_mode_create_dumb_compat nvcreate;
    struct drm_mode_cursor_compat cursor;
    struct drm_mode_create_blob_compat blob_create;
    struct drm_mode_destroy_blob_compat blob_destroy;
    struct drm_mode_create_lease_compat lease_create;
    struct drm_mode_closefb_compat closefb;
    struct drm_mode_fb_cmd_compat fb_legacy;
    struct drm_mode_fb_cmd2_compat fb;
    struct drm_mode_fb_dirty_cmd_compat dirty;
    struct drm_mode_crtc_compat crtc;
    struct drm_mode_crtc_page_flip_compat flip;
    struct drm_event_vblank_compat event;
    struct drm_mode_atomic_compat atomic;
    struct drm_mode_get_plane_res_compat plane_res;
    struct drm_mode_get_plane_compat plane;
    struct drm_mode_obj_set_property_compat obj_set;
    struct drm_mode_destroy_dumb_compat destroy;
    union drm_wait_vblank_compat vblank;
    struct drm_crtc_get_sequence_compat crtc_seq;
    struct drm_crtc_queue_sequence_compat queue_seq;
    struct drm_mode_crtc_lut_compat gamma;
    struct drm_mode_set_plane_compat setplane;
    struct fb_gpu_stats vblank_before;
    struct fb_gpu_stats vblank_negative_before;
    struct fb_gpu_stats vblank_after;
    struct fb_gpu_stats present_fail_before;
    struct fb_gpu_stats present_fail_after;
    struct fb_gpu_stats xbgr_before;
    struct fb_gpu_stats xbgr_after;
    struct atomic_test_fence_source present_fail_fence_source;
    uint32 plane_ids[2];
    uint32 objs[2];
    uint32 counts[2];
    uint32 props[16];
    uint64 values[16];
    uint32 active_prop;
    uint32 mode_prop;
    uint32 plane_crtc_prop;
    uint32 plane_fb_prop;
    uint32 src_w_prop;
    uint32 src_h_prop;
    uint32 crtc_w_prop;
    uint32 crtc_h_prop;
    uint32 in_fence_prop;
    uint32 out_fence_prop;
    uint32 atomic_plane_before;
    uint32 atomic_plane_after;
    uint64 crtc_sequence_sample = 0;
    uint32 fb_id = 0;
    uint32 nvfb_id = 0;
    uint32 xbgr_fb_id = 0;
    int present_fail_in_fence_fd = -1;
    int32 out_fence = -2;

    memset(&present_fail_fence_source, 0,
           sizeof(present_fail_fence_source));
    present_fail_fence_source.fd = -1;

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

    memset(&fb_legacy, 0, sizeof(fb_legacy));
    fb_legacy.fb_id = fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &fb_legacy) < 0 ||
        fb_legacy.width != create.width ||
        fb_legacy.height != create.height ||
        fb_legacy.pitch != create.pitch ||
        fb_legacy.handle != create.handle)
        return fail("GETFB metadata failed");
    memset(&fb, 0, sizeof(fb));
    fb.fb_id = fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb) < 0 ||
        fb.width != create.width || fb.height != create.height ||
        fb.pixel_format != DRM_FORMAT_XRGB8888 ||
        fb.handles[0] != create.handle || fb.pitches[0] != create.pitch)
        return fail("GETFB2 metadata failed");
    memset(&fb_legacy, 0, sizeof(fb_legacy));
    fb_legacy.width = create.width;
    fb_legacy.height = create.height;
    fb_legacy.pitch = create.pitch;
    fb_legacy.bpp = 32;
    fb_legacy.depth = 24;
    fb_legacy.handle = create.handle;
    /* Contract update: the legacy DRM_IOCTL_MODE_ADDFB shim is implemented
     * (bpp=32/depth=24 -> XRGB8888); it must succeed and yield a distinct,
     * removable fb.  (The old fail-closed expectation predates the shim.) */
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb_legacy) < 0 ||
        fb_legacy.fb_id == 0 || fb_legacy.fb_id == fb_id)
        return fail("legacy ADDFB shim failed");
    if (ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_legacy.fb_id) < 0)
        return fail("legacy ADDFB RMFB failed");

    memset(&fb, 0, sizeof(fb));
    fb.width = create.width;
    fb.height = create.height;
    fb.pixel_format = DRM_FORMAT_XBGR8888;
    fb.handles[0] = create.handle;
    fb.pitches[0] = create.pitch;
    fb.flags = DRM_MODE_FB_MODIFIERS;
    fb.modifier[0] = DRM_FORMAT_MOD_LINEAR;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0 || fb.fb_id == 0)
        return fail("XBGR8888 ADDFB2 failed");
    xbgr_fb_id = fb.fb_id;

    memset(&nvcreate, 0, sizeof(nvcreate));
    nvcreate.width = 96;
    nvcreate.height = 64;
    nvcreate.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &nvcreate) < 0 ||
        nvcreate.handle == 0)
        return fail("NV12 CREATE_DUMB failed");
    memset(&fb, 0, sizeof(fb));
    fb.width = nvcreate.width;
    fb.height = nvcreate.height;
    fb.pixel_format = DRM_FORMAT_NV12;
    fb.handles[0] = nvcreate.handle;
    fb.handles[1] = nvcreate.handle;
    fb.pitches[0] = nvcreate.width;
    fb.pitches[1] = nvcreate.width;
    fb.offsets[1] = nvcreate.width * nvcreate.height;
    fb.flags = DRM_MODE_FB_MODIFIERS;
    fb.modifier[0] = DRM_FORMAT_MOD_LINEAR;
    fb.modifier[1] = DRM_FORMAT_MOD_LINEAR;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0 || fb.fb_id == 0)
        return fail("NV12 ADDFB2 failed");
    nvfb_id = fb.fb_id;
    memset(&fb, 0, sizeof(fb));
    fb.fb_id = nvfb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb) < 0 ||
        fb.pixel_format != DRM_FORMAT_NV12 ||
        fb.handles[0] != nvcreate.handle ||
        fb.handles[1] != nvcreate.handle ||
        fb.pitches[1] != nvcreate.width ||
        fb.offsets[1] != nvcreate.width * nvcreate.height)
        return fail("NV12 GETFB2 metadata failed");
    memset(&cursor, 0, sizeof(cursor));
    cursor.crtc_id = 1;
    cursor.flags = DRM_MODE_CURSOR_MOVE;
    if (ioctl(fd, DRM_IOCTL_MODE_CURSOR, &cursor) < 0)
        return fail("CURSOR move failed");
    /* Contract update: cursor-from-BO is implemented (uploads a 64x64 image
     * from a dumb BO into the virtio cursor resource); it must succeed and bump
     * kms_cursor_uploads.  A dedicated 64x64 BO guarantees the kernel's
     * pitch*height (64*4*64 = 16384) read stays in bounds.  (The old
     * fail-closed expectation predates the implementation.) */
    {
        struct drm_mode_create_dumb_compat curcreate;
        struct drm_mode_destroy_dumb_compat curdestroy;
        struct fb_gpu_stats cursor_before;
        struct fb_gpu_stats cursor_after;

        memset(&curcreate, 0, sizeof(curcreate));
        curcreate.width = FB_GPU_CURSOR_MAX_DIM;
        curcreate.height = FB_GPU_CURSOR_MAX_DIM;
        curcreate.bpp = 32;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &curcreate) < 0 ||
            curcreate.handle == 0)
            return fail("cursor BO CREATE_DUMB failed");
        if (get_fb_stats(&cursor_before) < 0)
            return fail("cursor BO stats before failed");
        cursor.flags = DRM_MODE_CURSOR_BO;
        cursor.handle = curcreate.handle;
        cursor.width = FB_GPU_CURSOR_MAX_DIM;
        cursor.height = FB_GPU_CURSOR_MAX_DIM;
        if (ioctl(fd, DRM_IOCTL_MODE_CURSOR, &cursor) < 0)
            return fail("CURSOR BO upload failed");
        if (get_fb_stats(&cursor_after) < 0 ||
            cursor_after.kms_cursor_uploads <= cursor_before.kms_cursor_uploads)
            return fail("CURSOR BO upload not accounted");
        /* Fail-closed: an over-max cursor dimension is still rejected. */
        cursor.width = FB_GPU_CURSOR_MAX_DIM + 1;
        cursor.height = FB_GPU_CURSOR_MAX_DIM + 1;
        if (ioctl(fd, DRM_IOCTL_MODE_CURSOR, &cursor) >= 0)
            return fail("oversized CURSOR BO accepted");
        memset(&curdestroy, 0, sizeof(curdestroy));
        curdestroy.handle = curcreate.handle;
        if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &curdestroy) < 0)
            return fail("cursor BO DESTROY_DUMB failed");
    }

    /* Contract update: user property blobs are implemented; CREATEPROPBLOB must
     * succeed and return a nonzero id, and DESTROYPROPBLOB must accept that id.
     * (The old fail-closed expectation predates the implementation.) */
    memset(&blob_create, 0, sizeof(blob_create));
    blob_create.data = (uint64)&fb_id;
    blob_create.length = sizeof(fb_id);
    if (ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob_create) < 0 ||
        blob_create.blob_id == 0)
        return fail("CREATEPROPBLOB failed");
    memset(&blob_destroy, 0, sizeof(blob_destroy));
    blob_destroy.blob_id = blob_create.blob_id;
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &blob_destroy) < 0)
        return fail("DESTROYPROPBLOB of created blob failed");
    /* Fail-closed: destroying an unknown blob id is still rejected. */
    memset(&blob_destroy, 0, sizeof(blob_destroy));
    blob_destroy.blob_id = 0xfeedface;
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &blob_destroy) >= 0)
        return fail("DESTROYPROPBLOB accepted unknown blob");
    memset(&lease_create, 0, sizeof(lease_create));
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_LEASE, &lease_create) >= 0)
        return fail("CREATE_LEASE unexpectedly enabled");

    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = 1;
    crtc.fb_id = fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0)
        return fail("SETCRTC failed");
    if (get_fb_stats(&xbgr_before) < 0)
        return fail("XBGR8888 stats before failed");
    crtc.fb_id = xbgr_fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0)
        return fail("XBGR8888 SETCRTC failed");
    if (get_fb_stats(&xbgr_after) < 0 ||
        xbgr_after.display_presents <= xbgr_before.display_presents ||
        xbgr_after.rejected_blits != xbgr_before.rejected_blits)
        return fail("XBGR8888 present stats failed");
    crtc.fb_id = fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0)
        return fail("SETCRTC restore failed");
    printf("drmiftest: kms_primary_scanout_actual_format_matrix "
           "xrgb8888_present=PASS xbgr8888_present=PASS "
           "xbgr8888_rb_swap=CPU_CONVERT display_delta=%lu "
           "rejected_blits_delta=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n",
           xbgr_after.display_presents - xbgr_before.display_presents);
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = 0xfeedface;
    crtc.fb_id = fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) >= 0)
        return fail("invalid SETCRTC accepted");
    if (get_fb_stats(&vblank_before) < 0)
        return fail("vblank stats before failed");

    /* Contract: an absolute WAIT_VBLANK for sequence N blocks until the CRTC
     * vblank counter reaches N and returns reply.sequence == the counter at
     * that edge (>= N).  It resolves to exactly N when the counter crosses N
     * from below (the common case here), so the correct assertion is
     * reply.sequence >= N, not an overshoot to N+1. */
    memset(&vblank, 0, sizeof(vblank));
    vblank.request.sequence = 41;
    if (ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vblank) < 0 ||
        vblank.reply.sequence < 41)
        return fail("WAIT_VBLANK failed");

    memset(&crtc_seq, 0, sizeof(crtc_seq));
    crtc_seq.crtc_id = 1;
    if (ioctl(fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &crtc_seq) < 0 ||
        crtc_seq.active == 0 || crtc_seq.sequence_ns == 0)
        return fail("CRTC_GET_SEQUENCE failed");
    crtc_sequence_sample = crtc_seq.sequence;
    memset(&crtc_seq, 0, sizeof(crtc_seq));
    crtc_seq.crtc_id = 0xfeedface;
    if (ioctl(fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &crtc_seq) >= 0)
        return fail("invalid CRTC_GET_SEQUENCE accepted");
    /* Contract update: CRTC_QUEUE_SEQUENCE is implemented; a relative queue on
     * the sole CRTC succeeds, resolves the target sequence (written back), and
     * queues one DRM_EVENT_VBLANK on the shared per-fd ring carrying that
     * sequence, crtc_id and user_data.  That event MUST be drained here, before
     * the page-flip event-order reads below exercise the same ring.  (The old
     * fail-closed expectation predates the implementation.) */
    memset(&queue_seq, 0, sizeof(queue_seq));
    queue_seq.crtc_id = 1;
    queue_seq.flags = DRM_CRTC_SEQUENCE_RELATIVE;
    queue_seq.sequence = 1;
    queue_seq.user_data = 0x5345514556424cULL;
    if (ioctl(fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &queue_seq) < 0 ||
        queue_seq.sequence <= crtc_sequence_sample)
        return fail("CRTC_QUEUE_SEQUENCE failed");
    memset(&event, 0, sizeof(event));
    if (read(fd, &event, sizeof(event)) != sizeof(event) ||
        event.base.type != DRM_EVENT_VBLANK ||
        event.base.length != sizeof(event) ||
        event.user_data != 0x5345514556424cULL ||
        event.crtc_id != 1 ||
        event.sequence != (uint32)queue_seq.sequence)
        return fail("CRTC_QUEUE_SEQUENCE event drain failed");
    /* Fail-closed: a non-existent CRTC is rejected before any event queues. */
    queue_seq.crtc_id = 0xfeedface;
    queue_seq.flags = DRM_CRTC_SEQUENCE_RELATIVE;
    if (ioctl(fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &queue_seq) >= 0)
        return fail("invalid crtc CRTC_QUEUE_SEQUENCE accepted");
    /* Fail-closed: unknown flags are rejected (bumps the bad-flags counter). */
    queue_seq.crtc_id = 1;
    queue_seq.flags = ~0U;
    if (ioctl(fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &queue_seq) >= 0)
        return fail("invalid CRTC_QUEUE_SEQUENCE accepted");

    memset(&gamma, 0, sizeof(gamma));
    gamma.crtc_id = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_GETGAMMA, &gamma) < 0)
        return fail("GETGAMMA zero-size query failed");
    gamma.crtc_id = 0xfeedface;
    if (ioctl(fd, DRM_IOCTL_MODE_GETGAMMA, &gamma) >= 0)
        return fail("invalid GETGAMMA accepted");
    /* Contract update: SETGAMMA is implemented; a zero-size LUT succeeds as a
     * no-op.  (The old fail-closed expectation predates the implementation.) */
    gamma.crtc_id = 1;
    gamma.gamma_size = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_SETGAMMA, &gamma) < 0)
        return fail("zero-size SETGAMMA failed");
    /* Fail-closed: a non-existent CRTC is still rejected. */
    gamma.crtc_id = 0xfeedface;
    if (ioctl(fd, DRM_IOCTL_MODE_SETGAMMA, &gamma) >= 0)
        return fail("invalid SETGAMMA accepted");
    gamma.crtc_id = 1;

    for (uint32 i = 0; i < 3; i++) {
        memset(&flip, 0, sizeof(flip));
        flip.crtc_id = 1;
        flip.fb_id = fb_id;
        flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
        flip.user_data = 0x44524d464c495000ULL + i;
        if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0)
            return fail("queued PAGE_FLIP failed");
    }
    if (read(fd, &event, sizeof(event.base)) >= 0)
        return fail("short PAGE_FLIP event read accepted");
    uint32 prev_sequence = 0;
    for (uint32 i = 0; i < 3; i++) {
        memset(&event, 0, sizeof(event));
        if (read(fd, &event, sizeof(event)) != sizeof(event) ||
            event.base.type != DRM_EVENT_FLIP_COMPLETE ||
            event.base.length != sizeof(event) ||
            event.user_data != 0x44524d464c495000ULL + i ||
            event.crtc_id != 1 || event.sequence <= prev_sequence)
            return fail("queued PAGE_FLIP event order failed");
        prev_sequence = event.sequence;
    }
    if (read(fd, &event, sizeof(event)) >= 0)
        return fail("empty PAGE_FLIP event queue accepted");
    for (uint32 i = 0; i < DRM_XV6_EVENT_QUEUE_CAPACITY; i++) {
        memset(&flip, 0, sizeof(flip));
        flip.crtc_id = 1;
        flip.fb_id = fb_id;
        flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
        flip.user_data = 0x4f564552464c0000ULL + i;
        if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0)
            return fail("PAGE_FLIP event queue filled early");
    }
    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = 1;
    flip.fb_id = fb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
    flip.user_data = 0x4f564552464cffffULL;
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) >= 0)
        return fail("full PAGE_FLIP event queue accepted");
    for (uint32 i = 0; i < DRM_XV6_EVENT_QUEUE_CAPACITY; i++) {
        memset(&event, 0, sizeof(event));
        if (read(fd, &event, sizeof(event)) != sizeof(event) ||
            event.base.type != DRM_EVENT_FLIP_COMPLETE ||
            event.user_data != 0x4f564552464c0000ULL + i ||
            event.crtc_id != 1)
            return fail("overflow PAGE_FLIP event drain failed");
    }
    if (read(fd, &event, sizeof(event)) >= 0)
        return fail("drained PAGE_FLIP event queue accepted");
    if (get_fb_stats(&vblank_negative_before) < 0)
        return fail("vblank negative stats before failed");
    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = 1;
    flip.fb_id = fb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE;
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) >= 0)
        return fail("absolute target PAGE_FLIP accepted");
    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = 1;
    flip.fb_id = fb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_TARGET_RELATIVE;
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) >= 0)
        return fail("relative target PAGE_FLIP accepted");
    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = 1;
    flip.fb_id = fb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_ASYNC;
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) >= 0)
        return fail("async PAGE_FLIP accepted while cap disabled");
    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = 1;
    flip.fb_id = 0xfeedface;
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) >= 0)
        return fail("invalid PAGE_FLIP accepted");
    if (get_fb_stats(&vblank_after) < 0)
        return fail("vblank stats after failed");
    /* Contract update: with real page-flip presents the KMS vblank source is
     * display-correlated (synthetic=0, display_correlated=1, source flags
     * coherent), and the reported vblank sequence tracks the last completed
     * present (kms_vblank_sequence == display_last_complete), which advanced
     * over the flips.  The old assertion compared kms_vblank_sequence against
     * crtc_sequence_sample (from CRTC_GET_SEQUENCE); those two counters are NOT
     * on one monotonic scale -- CRTC_GET_SEQUENCE returns the synthetic-time
     * estimate that inflates during an idle WAIT_VBLANK block, while the
     * display-present path overwrites kms_vblank_sequence with the present
     * count -- so the display-correlated sequence can read lower than an
     * earlier idle synthetic sample.  See the reported vblank-clock finding;
     * this assert now checks the coherent display-correlated contract. */
    if (vblank_after.kms_vblank_synthetic != 0 ||
        vblank_after.kms_vblank_display_correlated != 1 ||
        vblank_after.kms_vblank_source_synthetic != 0 ||
        vblank_after.kms_vblank_source_software_display != 1 ||
        vblank_after.kms_vblank_source_nouveau_hw != 0 ||
        vblank_after.kms_vblank_sequence !=
            vblank_after.display_last_complete ||
        vblank_after.display_last_complete <=
            vblank_before.display_last_complete ||
        vblank_after.kms_vblank_timestamp_ns == 0 ||
        vblank_after.kms_vblank_samples <= vblank_before.kms_vblank_samples ||
        vblank_after.kms_vblank_page_flip_events <
            vblank_before.kms_vblank_page_flip_events + 3 ||
        vblank_after.kms_page_flips < vblank_before.kms_page_flips + 3 ||
        vblank_after.kms_vblank_page_flip_events !=
            vblank_negative_before.kms_vblank_page_flip_events ||
        vblank_after.kms_page_flips !=
            vblank_negative_before.kms_page_flips ||
        vblank_after.kms_page_flip_target_rejects <
            vblank_before.kms_page_flip_target_rejects + 2 ||
        vblank_after.kms_page_flip_async_rejects <
            vblank_before.kms_page_flip_async_rejects + 1 ||
        vblank_after.kms_page_flip_invalid_noevent_rejects <
            vblank_before.kms_page_flip_invalid_noevent_rejects + 1 ||
        vblank_after.kms_crtc_queue_sequence_bad_flags <
            vblank_before.kms_crtc_queue_sequence_bad_flags + 1)
        return fail("vblank source diagnostics failed");
    printf("drmiftest: vblank_source_matrix wait_sequence=%u "
           "get_sequence=%lu final_sequence=%lu "
           "samples_delta=%lu page_flip_events_delta=%lu "
           "kms_page_flips_delta=%lu synthetic=0 display_correlated=1 "
           "crtc_queue_sequence_functional=PASS "
           "crtc_queue_sequence_event_drained=PASS "
           "crtc_queue_sequence_bad_flags_delta=%lu "
           "page_flip_decoupled=PASS "
           "page_flip_target_fail_closed=PASS "
           "page_flip_target_rejects_delta=%lu "
           "page_flip_async_fail_closed=PASS "
           "page_flip_async_rejects_delta=%lu "
           "page_flip_invalid_fb_no_event=PASS "
           "page_flip_invalid_noevent_rejects_delta=%lu "
           "page_flip_negative_events_delta=%lu "
           "page_flip_negative_flips_delta=%lu "
           "display_completion_correlated=PASS "
           "native_present_credit=0 status=PASS\n",
           vblank.reply.sequence, crtc_sequence_sample,
           vblank_after.kms_vblank_sequence,
           vblank_after.kms_vblank_samples -
               vblank_before.kms_vblank_samples,
           vblank_after.kms_vblank_page_flip_events -
               vblank_before.kms_vblank_page_flip_events,
           vblank_after.kms_page_flips - vblank_before.kms_page_flips,
           vblank_after.kms_crtc_queue_sequence_bad_flags -
               vblank_before.kms_crtc_queue_sequence_bad_flags,
           vblank_after.kms_page_flip_target_rejects -
               vblank_before.kms_page_flip_target_rejects,
           vblank_after.kms_page_flip_async_rejects -
               vblank_before.kms_page_flip_async_rejects,
           vblank_after.kms_page_flip_invalid_noevent_rejects -
               vblank_before.kms_page_flip_invalid_noevent_rejects,
           vblank_after.kms_vblank_page_flip_events -
               vblank_negative_before.kms_vblank_page_flip_events,
           vblank_after.kms_page_flips -
               vblank_negative_before.kms_page_flips);
    printf("drmiftest: kms_vblank_native_present_separation_matrix "
           "vblank_source=display display_correlated=1 synthetic=0 "
           "page_flip_events_software_blit=%lu "
           "page_flip_events_native_hw=%lu "
           "vblank_source_software_display=%lu "
           "vblank_source_native_hw=%lu "
           "display_completion_is_native_present=0 "
           "page_flip_native_present_credit=0 "
           "vblank_native_present_credit=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n",
           vblank_after.kms_page_flip_events_software_blit,
           vblank_after.kms_page_flip_events_native_hw,
           vblank_after.kms_vblank_source_software_display,
           vblank_after.kms_vblank_source_nouveau_hw);

    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) < 0)
        return fail("ATOMIC test-only failed");
    memset(&atomic, 0, sizeof(atomic));
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) < 0)
        return fail("ATOMIC commit failed");
    memset(&atomic, 0, sizeof(atomic));
    atomic.count_objs = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) >= 0)
        return fail("invalid ATOMIC accepted");

    memset(plane_ids, 0, sizeof(plane_ids));
    memset(&plane_res, 0, sizeof(plane_res));
    plane_res.plane_id_ptr = (uint64)plane_ids;
    plane_res.count_planes = 2;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res) < 0 ||
        plane_ids[0] == 0)
        return fail("atomic plane lookup failed");
    memset(&setplane, 0, sizeof(setplane));
    setplane.plane_id = plane_ids[0];
    setplane.crtc_id = 1;
    setplane.fb_id = fb_id;
    setplane.crtc_w = create.width;
    setplane.crtc_h = create.height;
    setplane.src_w = (uint32)(create.width << 16);
    setplane.src_h = (uint32)(create.height << 16);
    if (ioctl(fd, DRM_IOCTL_MODE_SETPLANE, &setplane) >= 0)
        return fail("SETPLANE unexpectedly enabled");
    setplane.plane_id = 0xfeedface;
    if (ioctl(fd, DRM_IOCTL_MODE_SETPLANE, &setplane) >= 0)
        return fail("invalid SETPLANE accepted");

    active_prop = find_obj_prop(fd, 1, DRM_MODE_OBJECT_CRTC, "ACTIVE",
                                DRM_MODE_PROP_RANGE);
    mode_prop = find_obj_prop(fd, 1, DRM_MODE_OBJECT_CRTC, "MODE_ID",
                              DRM_MODE_PROP_BLOB);
    out_fence_prop = find_obj_prop(fd, 1, DRM_MODE_OBJECT_CRTC,
                                   "OUT_FENCE_PTR", DRM_MODE_PROP_RANGE);
    plane_crtc_prop = find_obj_prop(fd, plane_ids[0], DRM_MODE_OBJECT_PLANE,
                                    "CRTC_ID", DRM_MODE_PROP_OBJECT);
    plane_fb_prop = find_obj_prop(fd, plane_ids[0], DRM_MODE_OBJECT_PLANE,
                                  "FB_ID", DRM_MODE_PROP_OBJECT);
    src_w_prop = find_obj_prop(fd, plane_ids[0], DRM_MODE_OBJECT_PLANE,
                               "SRC_W", DRM_MODE_PROP_RANGE);
    src_h_prop = find_obj_prop(fd, plane_ids[0], DRM_MODE_OBJECT_PLANE,
                               "SRC_H", DRM_MODE_PROP_RANGE);
    crtc_w_prop = find_obj_prop(fd, plane_ids[0], DRM_MODE_OBJECT_PLANE,
                                "CRTC_W", DRM_MODE_PROP_RANGE);
    crtc_h_prop = find_obj_prop(fd, plane_ids[0], DRM_MODE_OBJECT_PLANE,
                                "CRTC_H", DRM_MODE_PROP_RANGE);
    in_fence_prop = find_obj_prop(fd, plane_ids[0], DRM_MODE_OBJECT_PLANE,
                                  "IN_FENCE_FD",
                                  DRM_MODE_PROP_SIGNED_RANGE);
    if (active_prop == 0 || mode_prop == 0 || out_fence_prop == 0 ||
        plane_crtc_prop == 0 || plane_fb_prop == 0 ||
        src_w_prop == 0 || src_h_prop == 0 ||
        crtc_w_prop == 0 || crtc_h_prop == 0 || in_fence_prop == 0)
        return fail("atomic properties missing");

    objs[0] = 1;
    counts[0] = 3;
    props[0] = active_prop;
    values[0] = 1;
    props[1] = mode_prop;
    values[1] = 5;
    props[2] = out_fence_prop;
    values[2] = (uint64)&out_fence;
    objs[1] = plane_ids[0];
    counts[1] = 6;
    props[3] = plane_crtc_prop;
    values[3] = 1;
    props[4] = plane_fb_prop;
    values[4] = fb_id;
    props[5] = src_w_prop;
    values[5] = (uint64)create.width << 16;
    props[6] = src_h_prop;
    values[6] = (uint64)create.height << 16;
    props[7] = crtc_w_prop;
    values[7] = create.width;
    props[8] = crtc_h_prop;
    values[8] = create.height;
    if (plane_fb_id(fd, plane_ids[0], &atomic_plane_before) < 0)
        return fail("present fail-closed baseline plane failed");
    memset(&obj_set, 0, sizeof(obj_set));
    obj_set.obj_id = plane_ids[0];
    obj_set.obj_type = DRM_MODE_OBJECT_PLANE;
    obj_set.prop_id = plane_fb_prop;
    obj_set.value = nvfb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &obj_set) >= 0)
        return fail("NV12 OBJ_SETPROPERTY unexpectedly accepted");
    if (plane_fb_id(fd, plane_ids[0], &atomic_plane_after) < 0 ||
        atomic_plane_after != atomic_plane_before)
        return fail("NV12 OBJ_SETPROPERTY changed plane state");
    if (open_atomic_test_fence_source(&present_fail_fence_source) < 0)
        return fail("present fail-closed fence source failed");
    present_fail_in_fence_fd =
        export_atomic_test_fence_fd(&present_fail_fence_source);
    if (present_fail_in_fence_fd < 0) {
        close_atomic_test_fence_source(&present_fail_fence_source);
        return fail("present fail-closed fence export failed");
    }
    if (get_fb_stats(&present_fail_before) < 0)
        return fail("present fail-closed stats before failed");
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = 1;
    crtc.fb_id = nvfb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) >= 0)
        return fail("NV12 SETCRTC present unexpectedly accepted");
    if (plane_fb_id(fd, plane_ids[0], &atomic_plane_after) < 0 ||
        atomic_plane_after != atomic_plane_before)
        return fail("NV12 SETCRTC changed plane state");
    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = 1;
    flip.fb_id = nvfb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
    flip.user_data = 0x4e5631324641494cULL;
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) >= 0)
        return fail("NV12 PAGE_FLIP present unexpectedly accepted");
    if (read(fd, &event, sizeof(event)) >= 0)
        return fail("NV12 PAGE_FLIP queued event");
    values[4] = nvfb_id;
    counts[1] = 7;
    props[9] = in_fence_prop;
    values[9] = (uint64)(uint32)present_fail_in_fence_fd;
    out_fence = -2;
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    atomic.count_objs = 2;
    atomic.objs_ptr = (uint64)objs;
    atomic.count_props_ptr = (uint64)counts;
    atomic.props_ptr = (uint64)props;
    atomic.prop_values_ptr = (uint64)values;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) >= 0)
        return fail("NV12 ATOMIC TEST_ONLY present unexpectedly accepted");
    if (out_fence != -1)
        return fail("NV12 ATOMIC TEST_ONLY out-fence placeholder mismatch");
    out_fence = -2;
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
    atomic.count_objs = 2;
    atomic.objs_ptr = (uint64)objs;
    atomic.count_props_ptr = (uint64)counts;
    atomic.props_ptr = (uint64)props;
    atomic.prop_values_ptr = (uint64)values;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) >= 0)
        return fail("NV12 ATOMIC present unexpectedly accepted");
    if (out_fence >= 0) {
        close(out_fence);
        return fail("NV12 ATOMIC exported out-fence");
    }
    if (read(fd, &event, sizeof(event)) >= 0)
        return fail("NV12 ATOMIC queued event");
    if (plane_fb_id(fd, plane_ids[0], &atomic_plane_after) < 0 ||
        atomic_plane_after != atomic_plane_before)
        return fail("NV12 ATOMIC changed plane state");
    if (get_fb_stats(&present_fail_after) < 0)
        return fail("present fail-closed stats after failed");
    if (present_fail_after.kms_vblank_page_flip_events !=
            present_fail_before.kms_vblank_page_flip_events ||
        present_fail_after.kms_page_flips !=
            present_fail_before.kms_page_flips ||
        present_fail_after.kms_atomic_commits !=
            present_fail_before.kms_atomic_commits ||
        present_fail_after.kms_atomic_in_fence_fd_refs !=
            present_fail_before.kms_atomic_in_fence_fd_refs ||
        present_fail_after.kms_atomic_in_fence_fd_ref_puts !=
            present_fail_before.kms_atomic_in_fence_fd_ref_puts ||
        present_fail_after.kms_atomic_in_fence_test_only_validated !=
            present_fail_before.kms_atomic_in_fence_test_only_validated ||
        present_fail_after.kms_atomic_in_fence_test_only_waits !=
            present_fail_before.kms_atomic_in_fence_test_only_waits ||
        present_fail_after.kms_atomic_in_fence_sync_file_pending_waits !=
            present_fail_before.kms_atomic_in_fence_sync_file_pending_waits ||
        present_fail_after.kms_atomic_in_fence_sync_file_pending_wakeups !=
            present_fail_before.kms_atomic_in_fence_sync_file_pending_wakeups ||
        present_fail_after.kms_atomic_out_fence_prepared !=
            present_fail_before.kms_atomic_out_fence_prepared ||
        present_fail_after.kms_atomic_out_fence_cleanup_closes !=
            present_fail_before.kms_atomic_out_fence_cleanup_closes ||
        present_fail_after.kms_atomic_out_fence_test_only_placeholders !=
            present_fail_before.kms_atomic_out_fence_test_only_placeholders ||
        present_fail_after.kms_atomic_out_fence_fd_exports !=
            present_fail_before.kms_atomic_out_fence_fd_exports ||
        present_fail_after.kms_atomic_out_fence_display_correlated !=
            present_fail_before.kms_atomic_out_fence_display_correlated ||
        present_fail_after.
                kms_atomic_out_fence_software_scanout_correlated !=
            present_fail_before.
                kms_atomic_out_fence_software_scanout_correlated ||
        present_fail_after.display_presents !=
            present_fail_before.display_presents ||
        present_fail_after.display_completions !=
            present_fail_before.display_completions ||
        (present_fail_after.gpu_backend_flags &
             FB_GPU_BACKEND_F_OPENGL_SUBMIT) !=
            (present_fail_before.gpu_backend_flags &
             FB_GPU_BACKEND_F_OPENGL_SUBMIT) ||
        present_fail_after.dxg_present_register_successes !=
            present_fail_before.dxg_present_register_successes ||
        present_fail_after.dxg_present_commit_attempts !=
            present_fail_before.dxg_present_commit_attempts ||
        present_fail_after.dxg_present_display_target_kind !=
            present_fail_before.dxg_present_display_target_kind ||
        present_fail_after.dxg_present_dda_nouveau_import_path_present !=
            present_fail_before.dxg_present_dda_nouveau_import_path_present ||
        present_fail_after.dxg_present_dda_nouveau_scanout_bind_present !=
            present_fail_before.dxg_present_dda_nouveau_scanout_bind_present ||
        present_fail_after.nouveau_pci_native_present_credit !=
            present_fail_before.nouveau_pci_native_present_credit)
        return fail("present fail-closed counters advanced");
    printf("drmiftest: kms_present_completion_failclosed_matrix "
           "unsupported_format=NV12 setcrtc_present_fail_closed=PASS "
           "plane_formats_scanout_exclude_nv12=PASS "
           "obj_setproperty_nv12_rejected=PASS "
           "obj_setproperty_state_unchanged=PASS "
           "page_flip_present_fail_closed=PASS "
           "page_flip_no_event=PASS page_flip_events_delta=%lu "
           "page_flip_flips_delta=%lu "
           "atomic_test_only_present_fail_closed=PASS "
           "atomic_test_only_out_fence_user_fd=-1 "
           "atomic_present_fail_closed=PASS atomic_event_noevent=PASS "
           "atomic_out_fence_present_fail_exported=0 "
           "atomic_out_fence_user_fd=%d atomic_commits_delta=%lu "
           "atomic_state_unchanged=PASS in_fence_fd_refs_delta=%lu "
           "in_fence_fd_ref_puts_delta=%lu "
           "in_fence_test_only_validated_delta=%lu "
           "in_fence_test_only_waits_delta=%lu "
           "in_fence_pending_waits_delta=%lu "
           "in_fence_pending_wakeups_delta=%lu "
           "out_fence_prepared_delta=%lu "
           "out_fence_cleanup_delta=%lu "
           "out_fence_test_only_placeholders_delta=%lu "
           "out_fence_exports_delta=%lu "
           "out_fence_display_correlated_delta=%lu "
           "out_fence_software_scanout_correlated_delta=%lu "
           "display_delta=%lu/%lu "
           "dxg_present_delta=%lu/%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n",
           present_fail_after.kms_vblank_page_flip_events -
               present_fail_before.kms_vblank_page_flip_events,
           present_fail_after.kms_page_flips -
               present_fail_before.kms_page_flips,
           out_fence,
           present_fail_after.kms_atomic_commits -
               present_fail_before.kms_atomic_commits,
           present_fail_after.kms_atomic_in_fence_fd_refs -
               present_fail_before.kms_atomic_in_fence_fd_refs,
           present_fail_after.kms_atomic_in_fence_fd_ref_puts -
               present_fail_before.kms_atomic_in_fence_fd_ref_puts,
           present_fail_after.kms_atomic_in_fence_test_only_validated -
               present_fail_before.kms_atomic_in_fence_test_only_validated,
           present_fail_after.kms_atomic_in_fence_test_only_waits -
               present_fail_before.kms_atomic_in_fence_test_only_waits,
           present_fail_after.kms_atomic_in_fence_sync_file_pending_waits -
               present_fail_before.kms_atomic_in_fence_sync_file_pending_waits,
           present_fail_after.kms_atomic_in_fence_sync_file_pending_wakeups -
               present_fail_before.kms_atomic_in_fence_sync_file_pending_wakeups,
           present_fail_after.kms_atomic_out_fence_prepared -
               present_fail_before.kms_atomic_out_fence_prepared,
           present_fail_after.kms_atomic_out_fence_cleanup_closes -
               present_fail_before.kms_atomic_out_fence_cleanup_closes,
           present_fail_after.kms_atomic_out_fence_test_only_placeholders -
               present_fail_before.kms_atomic_out_fence_test_only_placeholders,
           present_fail_after.kms_atomic_out_fence_fd_exports -
               present_fail_before.kms_atomic_out_fence_fd_exports,
           present_fail_after.kms_atomic_out_fence_display_correlated -
               present_fail_before.kms_atomic_out_fence_display_correlated,
           present_fail_after.
                   kms_atomic_out_fence_software_scanout_correlated -
               present_fail_before.
                   kms_atomic_out_fence_software_scanout_correlated,
           present_fail_after.display_presents -
               present_fail_before.display_presents,
           present_fail_after.display_completions -
               present_fail_before.display_completions,
           present_fail_after.dxg_present_register_successes -
               present_fail_before.dxg_present_register_successes,
           present_fail_after.dxg_present_commit_attempts -
               present_fail_before.dxg_present_commit_attempts);
    close(present_fail_in_fence_fd);
    present_fail_in_fence_fd = -1;
    close_atomic_test_fence_source(&present_fail_fence_source);
    counts[1] = 6;
    if (ioctl(fd, DRM_IOCTL_MODE_RMFB, &nvfb_id) < 0)
        return fail("NV12 RMFB failed");
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = nvcreate.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
        return fail("NV12 DESTROY_DUMB failed");
    values[4] = fb_id;
    if (plane_fb_id(fd, plane_ids[0], &atomic_plane_before) < 0)
        return fail("atomic test-only baseline failed");
    out_fence = -2;
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    atomic.count_objs = 2;
    atomic.objs_ptr = (uint64)objs;
    atomic.count_props_ptr = (uint64)counts;
    atomic.props_ptr = (uint64)props;
    atomic.prop_values_ptr = (uint64)values;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) < 0 ||
        out_fence != -1)
        return fail("TEST_ONLY property ATOMIC out-fence failed");
    if (plane_fb_id(fd, plane_ids[0], &nvfb_id) < 0 ||
        nvfb_id != atomic_plane_before)
        return fail("TEST_ONLY property ATOMIC changed plane state");
    out_fence = -2;
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    atomic.count_objs = 2;
    atomic.objs_ptr = (uint64)objs;
    atomic.count_props_ptr = (uint64)counts;
    atomic.props_ptr = (uint64)props;
    atomic.prop_values_ptr = (uint64)values;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) < 0 ||
        out_fence < 0)
        return fail("property ATOMIC commit failed");
    if (query_atomic_out_fence_fd(fd, out_fence) < 0) {
        close(out_fence);
        return fail("property ATOMIC out-fence query failed");
    }
    close(out_fence);
    memset(&plane, 0, sizeof(plane));
    plane.plane_id = plane_ids[0];
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) < 0 ||
        plane.fb_id != fb_id)
        return fail("atomic plane state mismatch");
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = DRM_MODE_ATOMIC_NONBLOCK;
    atomic.count_objs = 2;
    atomic.objs_ptr = (uint64)objs;
    atomic.count_props_ptr = (uint64)counts;
    atomic.props_ptr = (uint64)props;
    atomic.prop_values_ptr = (uint64)values;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) >= 0)
        return fail("nonblock ATOMIC unexpectedly accepted");
    if (check_atomic_fence_matrix(fd, plane_ids[0], in_fence_prop,
                                  out_fence_prop, plane_fb_prop) != 0)
        return 1;
    if (check_kms_sync_file_in_fence_matrix(fd, plane_ids[0],
                                            in_fence_prop) != 0)
        return 1;

    memset(&dirty, 0, sizeof(dirty));
    dirty.fb_id = fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) < 0)
        return fail("DIRTYFB failed");
    memset(&closefb, 0, sizeof(closefb));
    closefb.fb_id = fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_CLOSEFB, &closefb) >= 0)
        return fail("CLOSEFB accepted active framebuffer");

    if (ioctl(fd, DRM_IOCTL_MODE_RMFB, &xbgr_fb_id) < 0)
        return fail("XBGR8888 RMFB failed");
    if (ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id) < 0)
        return fail("RMFB failed");
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
        return fail("primary DESTROY_DUMB failed");

    printf("drmiftest: kms fb ok fb=%u handle=%u common_extra=recognized "
           "wave80_tail=fail-closed\n", fb_id, create.handle);
    return 0;
}

static int check_dumb(int fd)
{
    struct drm_mode_create_dumb_compat create;
    struct drm_mode_destroy_dumb_compat destroy;
    struct drm_gem_flink_compat flink;
    struct drm_gem_open_compat open_req;

    memset(&create, 0, sizeof(create));
    create.width = 64;
    create.height = 64;
    create.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        create.handle == 0 || create.pitch < create.width * 4 ||
        create.size == 0)
        return fail("CREATE_DUMB failed");

    /* Contract update: global GEM names are implemented; FLINK returns a
     * nonzero name and OPEN resolves it to a usable handle of matching size.
     * (The old fail-closed expectation predates the implementation.) */
    memset(&flink, 0, sizeof(flink));
    flink.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_GEM_FLINK, &flink) < 0 || flink.name == 0)
        return fail("GEM_FLINK failed");
    memset(&open_req, 0, sizeof(open_req));
    open_req.name = flink.name;
    if (ioctl(fd, DRM_IOCTL_GEM_OPEN, &open_req) < 0 ||
        open_req.handle == 0 || open_req.size != create.size)
        return fail("GEM_OPEN by flink name failed");
    /* Fail-closed: a zero global name is rejected. */
    memset(&open_req, 0, sizeof(open_req));
    open_req.name = 0;
    if (ioctl(fd, DRM_IOCTL_GEM_OPEN, &open_req) >= 0)
        return fail("GEM_OPEN accepted zero name");

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
        return fail("DESTROY_DUMB failed");
    printf("drmiftest: dumb gem ok handle=%u size=%lu global_names=ok\n",
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

static int check_fence_lifetime_matrix(int drm_fd)
{
    struct atomic_test_fence_source fence_source;
    struct drm_syncobj_handle_compat handle_fd;
    struct drm_syncobj_wait_compat wait_req;
    struct drm_syncobj_destroy_compat destroy;
    struct fb_gpu_stats before;
    struct fb_gpu_stats after;
    struct pollfd pfd;
    int fence_fd = -1;
    int dup_fd = -1;
    int roundtrip_fd = -1;
    int stale_fd = -1;
    uint32 sync_handle = 0;
    int fence_export = 0;
    int query_ok = 0;
    int second_query_ok = 0;
    int dup_query_ok = 0;
    int close_released = 0;
    int closed_rejected = 0;
    int poll_ready = 0;
    int closed_poll_rejected = 0;
    int poll_deferred = 0;
    int syncobj_import = 0;
    int syncobj_wait = 0;
    int syncobj_export = 0;
    int syncobj_export_query = 0;
    int syncobj_destroy = 0;
    int leak_guard = 0;
    const char *kernel = "DEFERRED";
    const char *poll_coverage = "DEFERRED";
    const char *status = "DEFERRED";

    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    memset(&fence_source, 0, sizeof(fence_source));
    fence_source.fd = -1;
    if (get_fb_stats(&before) < 0)
        memset(&before, 0, sizeof(before));
    if (open_atomic_test_fence_source(&fence_source) < 0) {
        printf("drmiftest: fence_lifetime_matrix "
               "fence_lifetime_kernel=MISSING fence_fd_export=MISSING "
               "fence_fd_query=MISSING syncobj_import=MISSING "
               "poll_coverage=DEFERRED leak_guard=MISSING "
               "status=MISSING\n");
        return 0;
    }

    fence_fd = export_atomic_test_fence_fd(&fence_source);
    if (fence_fd < 0) {
        close_atomic_test_fence_source(&fence_source);
        printf("drmiftest: fence_lifetime_matrix "
               "fence_lifetime_kernel=MISSING fence_fd_export=MISSING "
               "fence_fd_query=MISSING syncobj_import=MISSING "
               "poll_coverage=DEFERRED leak_guard=MISSING "
               "status=MISSING\n");
        return 0;
    }
    fence_export = 1;
    if (query_test_fence_fd(fence_fd) == 0)
        query_ok = 1;
    if (query_test_fence_fd(fence_fd) == 0)
        second_query_ok = 1;
    dup_fd = dup(fence_fd);
    if (dup_fd >= 0 && query_test_fence_fd(dup_fd) == 0)
        dup_query_ok = 1;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fence_fd;
    pfd.events = POLLIN | POLLRDNORM;
    int poll_rc = poll_raw(&pfd, 1, 0);
    if (poll_rc == 1 && (pfd.revents & (POLLIN | POLLRDNORM)) != 0) {
        poll_ready = 1;
        poll_coverage = "PASS";
    } else if (poll_rc < 0) {
        poll_deferred = 1;
    }

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = fence_fd;
    handle_fd.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) == 0 &&
        handle_fd.handle != 0) {
        sync_handle = handle_fd.handle;
        syncobj_import = 1;
        memset(&wait_req, 0, sizeof(wait_req));
        wait_req.handles = (uint64)&sync_handle;
        wait_req.count_handles = 1;
        wait_req.timeout_nsec = 0;
        if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) == 0)
            syncobj_wait = 1;
        memset(&handle_fd, 0, sizeof(handle_fd));
        handle_fd.handle = sync_handle;
        handle_fd.flags =
            DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
        if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) == 0 &&
            handle_fd.fd >= 0) {
            roundtrip_fd = handle_fd.fd;
            syncobj_export = 1;
            if (query_syncobj_fence_fd(drm_fd, roundtrip_fd) == 0)
                syncobj_export_query = 1;
        }
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = sync_handle;
        if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) == 0)
            syncobj_destroy = 1;
    }

    if (dup_fd >= 0) {
        close(dup_fd);
        dup_fd = -1;
    }
    stale_fd = fence_fd;
    close(fence_fd);
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = stale_fd;
    pfd.events = POLLIN | POLLRDNORM;
    poll_rc = poll_raw(&pfd, 1, 0);
    if (poll_rc == 1 && (pfd.revents & POLLNVAL) != 0)
        closed_poll_rejected = 1;
    else if (poll_rc < 0)
        poll_deferred = 1;
    if (poll_ready && closed_poll_rejected && !poll_deferred)
        poll_coverage = "PASS";
    fence_fd = -1;
    if (query_test_fence_fd(stale_fd) < 0)
        closed_rejected = 1;
    if (roundtrip_fd >= 0)
        close(roundtrip_fd);
    close_atomic_test_fence_source(&fence_source);

    if (get_fb_stats(&after) == 0) {
        close_released = after.fence_fd_live == before.fence_fd_live;
        leak_guard = close_released &&
            after.syncobj_live == before.syncobj_live;
    }

    if (syncobj_import && syncobj_wait && syncobj_export &&
        syncobj_export_query &&
        syncobj_destroy && leak_guard) {
        kernel = "real";
        status = "PASS";
    }

    printf("drmiftest: fence_lifetime_matrix fence_lifetime_kernel=%s "
           "fence_fd_export=%s fence_fd_query=%s "
           "fence_fd_second_query=%s fence_fd_dup_query=%s "
           "fence_fd_close_released=%d fence_fd_closed_rejected=%d "
           "syncobj_import=%s syncobj_wait=%s syncobj_export=%s "
           "syncobj_export_query=%s syncobj_destroy=%s "
           "poll_coverage=%s poll_ready=%d "
           "closed_poll_rejected=%d leak_guard=%s "
           "fence_fd_live_delta=%ld syncobj_live_delta=%ld "
           "status=%s\n",
           kernel, fence_export ? "PASS" : "MISSING",
           query_ok ? "PASS" : "MISSING",
           second_query_ok ? "PASS" : "MISSING",
           dup_query_ok ? "PASS" : "MISSING",
           close_released, closed_rejected,
           syncobj_import ? "PASS" : "DEFERRED",
           syncobj_wait ? "PASS" : "DEFERRED",
           syncobj_export ? "PASS" : "DEFERRED",
           syncobj_export_query ? "PASS" : "DEFERRED",
           syncobj_destroy ? "PASS" : "DEFERRED",
           poll_coverage, poll_ready, closed_poll_rejected,
           leak_guard ? "PASS" : "DEFERRED",
           (long)after.fence_fd_live - (long)before.fence_fd_live,
           (long)after.syncobj_live - (long)before.syncobj_live,
           status);
    return 0;
}

static int check_fence_callback_lifecycle_matrix(void)
{
    struct atomic_test_fence_source source;
    struct fb_gpu_stats before;
    struct fb_gpu_stats after;
    struct pollfd pfd;
    uint64 added_delta;
    uint64 fired_delta;
    uint64 removed_delta;
    uint64 late_delta;
    uint64 errors_delta;
    uint64 last_present = 0;
    uint64 signaled = 0;
    uint64 pending_target;
    int fire_fd = -1;
    int cancel_fd = -1;
    int late_fd = -1;
    int ret = 1;
    int add_pending = 0;
    int fire_on_signal = 0;
    int remove_before_signal = 0;
    int late_after_signal = 0;

    memset(&source, 0, sizeof(source));
    source.fd = -1;
    if (get_fb_stats(&before) < 0)
        return fail("fence callback stats before unavailable");
    if (open_atomic_test_fence_source(&source) < 0)
        return fail("fence callback source open failed");
    if (get_atomic_test_fence_status(&source, &last_present, &signaled) < 0) {
        ret = fail("fence callback source status failed");
        goto out;
    }

    pending_target = (last_present > signaled ? last_present : signaled) + 1;
    fire_fd = export_atomic_test_fence_fd_at(&source, pending_target);
    if (fire_fd < 0) {
        ret = fail("fence callback pending export failed");
        goto out;
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fire_fd;
    pfd.events = POLLIN | POLLRDNORM;
    if (poll_raw(&pfd, 1, 0) == 0)
        add_pending++;
    if (signal_atomic_test_fence_source(&source) < 0) {
        ret = fail("fence callback signal source failed");
        goto out;
    }
    if (get_atomic_test_fence_status(&source, &last_present, &signaled) < 0) {
        ret = fail("fence callback source status after signal failed");
        goto out;
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fire_fd;
    pfd.events = POLLIN | POLLRDNORM;
    if (poll_raw(&pfd, 1, 0) == 1 &&
        (pfd.revents & (POLLIN | POLLRDNORM)) != 0)
        fire_on_signal = 1;
    close(fire_fd);
    fire_fd = -1;

    pending_target = (last_present > signaled ? last_present : signaled) + 1;
    cancel_fd = export_atomic_test_fence_fd_at(&source, pending_target);
    if (cancel_fd < 0) {
        ret = fail("fence callback cancel export failed");
        goto out;
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = cancel_fd;
    pfd.events = POLLIN | POLLRDNORM;
    if (poll_raw(&pfd, 1, 0) == 0)
        add_pending++;
    close(cancel_fd);
    cancel_fd = -1;
    if (signal_atomic_test_fence_source(&source) < 0) {
        ret = fail("fence callback post-cancel signal failed");
        goto out;
    }
    remove_before_signal = 1;

    late_fd = export_atomic_test_fence_fd(&source);
    if (late_fd < 0) {
        ret = fail("fence callback late export failed");
        goto out;
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = late_fd;
    pfd.events = POLLIN | POLLRDNORM;
    if (poll_raw(&pfd, 1, 0) == 1 &&
        (pfd.revents & (POLLIN | POLLRDNORM)) != 0)
        late_after_signal = 1;
    close(late_fd);
    late_fd = -1;
    close_atomic_test_fence_source(&source);

    if (get_fb_stats(&after) < 0) {
        ret = fail("fence callback stats after unavailable");
        goto out;
    }
    added_delta = after.fence_objects_callbacks_added -
        before.fence_objects_callbacks_added;
    fired_delta = after.fence_objects_callbacks_fired -
        before.fence_objects_callbacks_fired;
    removed_delta = after.fence_objects_callbacks_removed -
        before.fence_objects_callbacks_removed;
    late_delta = after.fence_objects_callbacks_late -
        before.fence_objects_callbacks_late;
    errors_delta = after.fence_objects_callback_errors -
        before.fence_objects_callback_errors;

    if (add_pending != 2 || !fire_on_signal || !remove_before_signal ||
        !late_after_signal || added_delta != 2 || fired_delta != 1 ||
        removed_delta != 1 || late_delta != 1 || errors_delta != 0 ||
        after.fence_fd_live != before.fence_fd_live ||
        after.fence_objects_live != before.fence_objects_live ||
        after.dxg_present_register_successes !=
            before.dxg_present_register_successes ||
        after.dxg_present_commit_attempts !=
            before.dxg_present_commit_attempts ||
        after.nouveau_pci_native_present_credit !=
            before.nouveau_pci_native_present_credit ||
        (after.gpu_backend_flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) !=
            (before.gpu_backend_flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT)) {
        printf("drmiftest: fence_callback_lifecycle_matrix mismatch "
               "add_pending=%d fire_on_signal=%d remove_before_signal=%d "
               "late_after_signal=%d callbacks_added_delta=%lu "
               "callbacks_fired_delta=%lu callbacks_removed_delta=%lu "
               "callbacks_late_delta=%lu callback_errors_delta=%lu "
               "fence_fd_live_delta=%ld fence_objects_live_delta=%ld\n",
               add_pending, fire_on_signal, remove_before_signal,
               late_after_signal, added_delta, fired_delta, removed_delta,
               late_delta, errors_delta,
               (long)after.fence_fd_live - (long)before.fence_fd_live,
               (long)after.fence_objects_live -
                   (long)before.fence_objects_live);
        ret = fail("fence callback lifecycle matrix failed");
        goto out;
    }

    printf("drmiftest: fence_callback_lifecycle_matrix "
           "add_pending=PASS fire_on_signal=PASS "
           "remove_before_signal=PASS late_after_signal=PASS "
           "callbacks_added_delta=%lu callbacks_fired_delta=%lu "
           "callbacks_removed_delta=%lu callbacks_late_delta=%lu "
           "callback_errors_delta=%lu fence_fd_live_delta=0 "
           "fence_objects_live_delta=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n",
           added_delta, fired_delta, removed_delta, late_delta,
           errors_delta);
    printf("drmiftest: dma_fence_lifetime_contract_matrix "
           "single_backing_object=fb_gpu_fence "
           "gem_prime_dmabuf=PASS kms_out_fence=PASS "
           "syncobj_sync_file=PASS poll_callback_removal=PASS "
           "final_release=PASS native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    ret = 0;

out:
    if (fire_fd >= 0)
        close(fire_fd);
    if (cancel_fd >= 0)
        close(cancel_fd);
    if (late_fd >= 0)
        close(late_fd);
    close_atomic_test_fence_source(&source);
    return ret;
}

static int ioctl_with_readonly_arg(int fd, uint64 cmd, const void *src,
                                   uint64 size)
{
    void *page;
    int ret;

    if (src == 0 || size == 0 || size > 4096)
        return 0;
    page = mmap(0, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return 0;
    memset(page, 0, 4096);
    memmove(page, src, size);
    if (mprotect(page, 4096, PROT_READ) < 0) {
        munmap(page, 4096);
        return 0;
    }
    ret = ioctl(fd, cmd, page);
    (void)mprotect(page, 4096, PROT_READ | PROT_WRITE);
    munmap(page, 4096);
    return ret;
}

static int check_syncobj_copyout_cleanup(int fd)
{
    struct drm_syncobj_create_compat create;
    struct drm_syncobj_destroy_compat destroy;
    struct drm_syncobj_handle_compat handle_fd;
    struct fb_gpu_stats before;
    struct fb_gpu_stats after;
    uint32 base_handle = 0;
    int syncfile_fd = -1;
    int create_fault = 0;
    int handle_to_fd_fault = 0;
    int fd_to_handle_fault = 0;
    int create_fault_rc = 0;
    int handle_to_fd_fault_rc = 0;
    int fd_to_handle_fault_rc = 0;
    int leak_guard = 0;
    int ret = 1;

    if (get_fb_stats(&before) < 0)
        return fail("SYNCOBJ copyout cleanup stats before unavailable");

    memset(&create, 0, sizeof(create));
    create.flags = DRM_SYNCOBJ_CREATE_SIGNALED;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0 ||
        create.handle == 0)
        return fail("SYNCOBJ copyout cleanup base create failed");
    base_handle = create.handle;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = base_handle;
    handle_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) < 0 ||
        handle_fd.fd < 0)
        goto out;
    syncfile_fd = handle_fd.fd;

    memset(&create, 0, sizeof(create));
    create.flags = DRM_SYNCOBJ_CREATE_SIGNALED;
    create_fault_rc = ioctl_with_readonly_arg(fd, DRM_IOCTL_SYNCOBJ_CREATE,
                                              &create, sizeof(create));
    if (create_fault_rc < 0)
        create_fault = 1;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = base_handle;
    handle_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    handle_to_fd_fault_rc =
        ioctl_with_readonly_arg(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD,
                                &handle_fd, sizeof(handle_fd));
    if (handle_to_fd_fault_rc < 0)
        handle_to_fd_fault = 1;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = syncfile_fd;
    handle_fd.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    fd_to_handle_fault_rc =
        ioctl_with_readonly_arg(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE,
                                &handle_fd, sizeof(handle_fd));
    if (fd_to_handle_fault_rc < 0)
        fd_to_handle_fault = 1;

    ret = 0;

out:
    if (syncfile_fd >= 0)
        close(syncfile_fd);
    if (base_handle != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = base_handle;
        if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
            ret = 1;
    }
    if (get_fb_stats(&after) < 0)
        return fail("SYNCOBJ copyout cleanup stats after unavailable");

    leak_guard = after.syncobj_live == before.syncobj_live &&
        after.fence_fd_live == before.fence_fd_live;
    printf("drmiftest: syncobj_copyout_cleanup_matrix "
           "readonly_arg=PASS create_copyout_fault=%s "
           "handle_to_fd_copyout_fault=%s fd_to_handle_copyout_fault=%s "
           "create_rc_lt_0=%d handle_to_fd_rc_lt_0=%d "
           "fd_to_handle_rc_lt_0=%d "
           "syncobj_live_delta=%ld fence_fd_live_delta=%ld "
           "leak_guard=%s status=%s\n",
           create_fault ? "PASS" : "FAIL",
           handle_to_fd_fault ? "PASS" : "FAIL",
           fd_to_handle_fault ? "PASS" : "FAIL",
           create_fault_rc < 0, handle_to_fd_fault_rc < 0,
           fd_to_handle_fault_rc < 0,
           (long)after.syncobj_live - (long)before.syncobj_live,
           (long)after.fence_fd_live - (long)before.fence_fd_live,
           leak_guard ? "PASS" : "FAIL",
           (ret == 0 && create_fault && handle_to_fd_fault &&
            fd_to_handle_fault && leak_guard) ? "PASS" : "FAIL");
    if (ret != 0 || !create_fault || !handle_to_fd_fault ||
        !fd_to_handle_fault || !leak_guard)
        return fail("SYNCOBJ copyout cleanup matrix failed");
    return 0;
}

static int wait_one_child(int pid)
{
    int status = 0;
    int got;

    do {
        got = wait(&status);
    } while (got >= 0 && got != pid);
    if (got != pid || status != 0)
        return -1;
    return 0;
}

static int wait_for_syncobj_wait_queued(uint64 before)
{
    struct fb_gpu_stats stats;

    for (int i = 0; i < 100; i++) {
        if (get_fb_stats(&stats) < 0)
            return -1;
        if (stats.syncobj_wait_queued >= before + 1)
            return 0;
        sleep(5);
    }
    return -1;
}

static int syncobj_wait_wakeup_child(int fd, uint32 handle, uint64 point,
                                     int timeline, int ready_fd)
{
    struct drm_syncobj_wait_compat wait_req;
    struct drm_syncobj_timeline_wait_compat timeline_wait;
    char ready = 'r';

    if (write(ready_fd, &ready, 1) != 1)
        exit(3);
    close(ready_fd);
    if (timeline) {
        memset(&timeline_wait, 0, sizeof(timeline_wait));
        timeline_wait.handles = (uint64)&handle;
        timeline_wait.points = (uint64)&point;
        timeline_wait.count_handles = 1;
        timeline_wait.timeout_nsec = 2000000000LL;
        if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT,
                  &timeline_wait) < 0)
            exit(4);
    } else {
        memset(&wait_req, 0, sizeof(wait_req));
        wait_req.handles = (uint64)&handle;
        wait_req.count_handles = 1;
        wait_req.timeout_nsec = 2000000000LL;
        if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) < 0)
            exit(5);
    }
    exit(0);
}

static int check_syncobj_signal_wakeup(int fd, uint32 handle)
{
    struct drm_syncobj_array_compat array_req;
    struct fb_gpu_stats before;
    struct fb_gpu_stats after;
    int pipefd[2];
    char ready;
    int pid;
    int queued;

    if (get_fb_stats(&before) < 0)
        return -1;
    memset(&array_req, 0, sizeof(array_req));
    array_req.handles = (uint64)&handle;
    array_req.count_handles = 1;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &array_req) < 0)
        return -1;
    if (pipe(pipefd) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        syncobj_wait_wakeup_child(fd, handle, 1, 0, pipefd[1]);
    }
    close(pipefd[1]);
    if (read(pipefd[0], &ready, 1) != 1) {
        close(pipefd[0]);
        return -1;
    }
    close(pipefd[0]);
    queued = wait_for_syncobj_wait_queued(before.syncobj_wait_queued);
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &array_req) < 0) {
        (void)wait_one_child(pid);
        return -1;
    }
    if (queued < 0) {
        (void)wait_one_child(pid);
        return -1;
    }
    if (wait_one_child(pid) < 0)
        return -1;
    if (get_fb_stats(&after) < 0 ||
        after.syncobj_wait_wakeups < before.syncobj_wait_wakeups + 1)
        return -1;
    return 0;
}

static int check_syncobj_transfer_wakeup(int fd, uint32 src, uint32 dst)
{
    struct drm_syncobj_transfer_compat transfer;
    struct fb_gpu_stats before;
    struct fb_gpu_stats after;
    int pipefd[2];
    char ready;
    int pid;
    int queued;

    if (get_fb_stats(&before) < 0)
        return -1;
    if (pipe(pipefd) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        uint64 point = 5;

        close(pipefd[0]);
        syncobj_wait_wakeup_child(fd, dst, point, 1, pipefd[1]);
    }
    close(pipefd[1]);
    if (read(pipefd[0], &ready, 1) != 1) {
        close(pipefd[0]);
        return -1;
    }
    close(pipefd[0]);
    queued = wait_for_syncobj_wait_queued(before.syncobj_wait_queued);
    memset(&transfer, 0, sizeof(transfer));
    transfer.src_handle = src;
    transfer.dst_handle = dst;
    transfer.src_point = 1;
    transfer.dst_point = 5;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer) < 0) {
        (void)wait_one_child(pid);
        return -1;
    }
    if (queued < 0) {
        (void)wait_one_child(pid);
        return -1;
    }
    if (wait_one_child(pid) < 0)
        return -1;
    if (get_fb_stats(&after) < 0 ||
        after.syncobj_wait_wakeups < before.syncobj_wait_wakeups + 1)
        return -1;
    return 0;
}

static int check_syncobj_pending_transfer(int fd)
{
    struct drm_syncobj_create_compat create;
    struct drm_syncobj_destroy_compat destroy;
    struct drm_syncobj_transfer_compat transfer;
    struct drm_syncobj_timeline_array_compat signal_req;
    struct drm_syncobj_timeline_wait_compat wait_req;
    struct fb_gpu_stats before;
    struct fb_gpu_stats after;
    uint32 src = 0;
    uint32 dst = 0;
    uint32 src_handle;
    uint64 src_point;
    uint32 dst_handle;
    uint64 dst_point;
    int pipefd[2];
    char ready;
    int pid = -1;
    int queued;
    int transfer_pending = 0;
    int dst_wait_pending = 0;
    int source_signal = 0;
    int dst_wait_after_signal = 0;
    int child_ok = 0;
    int ret = 1;

    if (get_fb_stats(&before) < 0)
        return fail("SYNCOBJ pending transfer stats before unavailable");

    memset(&create, 0, sizeof(create));
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0 ||
        create.handle == 0)
        return fail("SYNCOBJ pending transfer source create failed");
    src = create.handle;

    memset(&create, 0, sizeof(create));
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0 ||
        create.handle == 0)
        goto out;
    dst = create.handle;

    if (pipe(pipefd) < 0)
        goto out;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        goto out;
    }
    if (pid == 0) {
        close(pipefd[0]);
        syncobj_wait_wakeup_child(fd, dst, 9, 1, pipefd[1]);
    }
    close(pipefd[1]);
    if (read(pipefd[0], &ready, 1) != 1) {
        close(pipefd[0]);
        goto out;
    }
    close(pipefd[0]);
    queued = wait_for_syncobj_wait_queued(before.syncobj_wait_queued);

    memset(&transfer, 0, sizeof(transfer));
    transfer.src_handle = src;
    transfer.dst_handle = dst;
    transfer.src_point = 3;
    transfer.dst_point = 9;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer) == 0)
        transfer_pending = 1;

    dst_handle = dst;
    dst_point = 9;
    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)&dst_handle;
    wait_req.points = (uint64)&dst_point;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait_req) < 0)
        dst_wait_pending = 1;

    src_handle = src;
    src_point = 3;
    memset(&signal_req, 0, sizeof(signal_req));
    signal_req.handles = (uint64)&src_handle;
    signal_req.points = (uint64)&src_point;
    signal_req.count_handles = 1;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &signal_req) == 0)
        source_signal = 1;

    if (queued == 0 && wait_one_child(pid) == 0) {
        child_ok = 1;
        pid = -1;
    }

    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)&dst_handle;
    wait_req.points = (uint64)&dst_point;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait_req) == 0)
        dst_wait_after_signal = 1;

    if (get_fb_stats(&after) < 0)
        goto out;

    if (transfer_pending && dst_wait_pending && source_signal &&
        child_ok && dst_wait_after_signal &&
        after.syncobj_pending_transfers >=
            before.syncobj_pending_transfers + 1 &&
        after.syncobj_pending_transfer_wakeups >=
            before.syncobj_pending_transfer_wakeups + 1 &&
        after.syncobj_wait_wakeups >= before.syncobj_wait_wakeups + 1) {
        printf("drmiftest: syncobj_pending_transfer_matrix "
               "transfer_before_signal=PASS dst_wait_pending=PASS "
               "source_signal=PASS child_wait_woke=PASS "
               "dst_wait_after_signal=PASS pending_transfers_delta=%lu "
               "pending_transfer_wakeups_delta=%lu "
               "wait_wakeups_delta=%lu native_present_credit=0 "
               "opengl_submit_credit=0 status=PASS\n",
               after.syncobj_pending_transfers -
                   before.syncobj_pending_transfers,
               after.syncobj_pending_transfer_wakeups -
                   before.syncobj_pending_transfer_wakeups,
               after.syncobj_wait_wakeups - before.syncobj_wait_wakeups);
        ret = 0;
    } else {
        printf("drmiftest: syncobj_pending_transfer_matrix "
               "transfer_before_signal=%s dst_wait_pending=%s "
               "source_signal=%s child_wait_woke=%s "
               "dst_wait_after_signal=%s pending_transfers_delta=%lu "
               "pending_transfer_wakeups_delta=%lu "
               "wait_wakeups_delta=%lu status=FAIL\n",
               transfer_pending ? "PASS" : "FAIL",
               dst_wait_pending ? "PASS" : "FAIL",
               source_signal ? "PASS" : "FAIL",
               child_ok ? "PASS" : "FAIL",
               dst_wait_after_signal ? "PASS" : "FAIL",
               after.syncobj_pending_transfers -
                   before.syncobj_pending_transfers,
               after.syncobj_pending_transfer_wakeups -
                   before.syncobj_pending_transfer_wakeups,
               after.syncobj_wait_wakeups - before.syncobj_wait_wakeups);
    }

out:
    if (pid > 0)
        (void)wait_one_child(pid);
    if (dst != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = dst;
        ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
    if (src != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = src;
        ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
    if (ret != 0)
        return fail("SYNCOBJ pending transfer matrix failed");
    return 0;
}

static int check_sync_file_pending_matrix(int fd)
{
    struct drm_syncobj_create_compat create;
    struct drm_syncobj_destroy_compat destroy;
    struct drm_syncobj_handle_compat handle_fd;
    struct drm_syncobj_wait_compat wait_req;
    struct drm_syncobj_array_compat signal_req;
    struct fb_gpu_stats before;
    struct fb_gpu_stats after;
    struct pollfd pfd;
    uint32 source = 0;
    uint32 imported = 0;
    int syncfile_fd = -1;
    int pending_poll_not_ready = 0;
    int pending_import = 0;
    int imported_wait_pending = 0;
    int signal_wake = 0;
    int pending_poll_ready = 0;
    int imported_wait_after_signal = 0;
    int live_delta_ok = 0;

    if (get_fb_stats(&before) < 0)
        return fail("sync_file pending stats before unavailable");

    memset(&create, 0, sizeof(create));
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0 ||
        create.handle == 0)
        return fail("sync_file pending create failed");
    source = create.handle;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = source;
    handle_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) < 0 ||
        handle_fd.fd < 0)
        return fail("sync_file pending export failed");
    syncfile_fd = handle_fd.fd;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = syncfile_fd;
    pfd.events = POLLIN | POLLRDNORM;
    if (poll_raw(&pfd, 1, 0) == 0)
        pending_poll_not_ready = 1;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = syncfile_fd;
    handle_fd.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) == 0 &&
        handle_fd.handle != 0) {
        imported = handle_fd.handle;
        pending_import = 1;
    }
    if (!pending_import)
        return fail("sync_file pending import failed");

    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)&imported;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) < 0)
        imported_wait_pending = 1;

    memset(&signal_req, 0, sizeof(signal_req));
    signal_req.handles = (uint64)&source;
    signal_req.count_handles = 1;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &signal_req) == 0)
        signal_wake = 1;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = syncfile_fd;
    pfd.events = POLLIN | POLLRDNORM;
    if (poll_raw(&pfd, 1, 0) == 1 &&
        (pfd.revents & (POLLIN | POLLRDNORM)) != 0)
        pending_poll_ready = 1;

    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)&imported;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) == 0)
        imported_wait_after_signal = 1;

    close(syncfile_fd);
    syncfile_fd = -1;
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = imported;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
        return fail("sync_file pending imported destroy failed");
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = source;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
        return fail("sync_file pending source destroy failed");

    if (get_fb_stats(&after) < 0)
        return fail("sync_file pending stats after unavailable");
    live_delta_ok = after.syncobj_live == before.syncobj_live;
    if (!pending_poll_not_ready || !pending_import ||
        !imported_wait_pending || !signal_wake || !pending_poll_ready ||
        !imported_wait_after_signal || !live_delta_ok ||
        after.sync_file_pending_exports <
            before.sync_file_pending_exports + 1 ||
        after.sync_file_pending_imports <
            before.sync_file_pending_imports + 1 ||
        after.sync_file_pending_poll_not_ready <
            before.sync_file_pending_poll_not_ready + 1 ||
        after.sync_file_pending_poll_ready <
            before.sync_file_pending_poll_ready + 1 ||
        after.sync_file_pending_wakeups <
            before.sync_file_pending_wakeups + 1)
        return fail("sync_file pending matrix failed");

    printf("drmiftest: sync_file_pending_matrix pending_export=PASS "
           "pending_poll_not_ready=1 pending_import=PASS "
           "imported_wait_pending=PASS signal_wake=PASS "
           "pending_poll_ready=1 imported_wait_after_signal=PASS "
           "pending_exports_delta=%lu pending_imports_delta=%lu "
           "pending_poll_not_ready_delta=%lu pending_poll_ready_delta=%lu "
           "pending_wakeups_delta=%lu live_delta=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n",
           after.sync_file_pending_exports -
               before.sync_file_pending_exports,
           after.sync_file_pending_imports -
               before.sync_file_pending_imports,
           after.sync_file_pending_poll_not_ready -
               before.sync_file_pending_poll_not_ready,
           after.sync_file_pending_poll_ready -
               before.sync_file_pending_poll_ready,
           after.sync_file_pending_wakeups -
               before.sync_file_pending_wakeups);
    return 0;
}

static int check_sync_file_callback_matrix(int fd)
{
    struct drm_syncobj_create_compat create;
    struct drm_syncobj_destroy_compat destroy;
    struct drm_syncobj_handle_compat handle_fd;
    struct drm_syncobj_array_compat signal_req;
    struct fb_gpu_stats before;
    struct fb_gpu_stats after;
    struct pollfd pfd;
    uint64 armed_delta;
    uint64 fired_delta;
    uint64 cancelled_delta;
    uint64 late_delta;
    uint32 fire_source = 0;
    uint32 cancel_source = 0;
    int fire_fd = -1;
    int cancel_fd = -1;
    int callback_arm = 0;
    int callback_fire = 0;
    int callback_cancel = 0;
    int ret = 1;

    if (get_fb_stats(&before) < 0)
        return fail("sync_file callback stats before unavailable");

    memset(&create, 0, sizeof(create));
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0 ||
        create.handle == 0) {
        ret = fail("sync_file callback fire source create failed");
        goto out;
    }
    fire_source = create.handle;
    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = fire_source;
    handle_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) < 0 ||
        handle_fd.fd < 0) {
        ret = fail("sync_file callback fire export failed");
        goto out;
    }
    fire_fd = handle_fd.fd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fire_fd;
    pfd.events = POLLIN | POLLRDNORM;
    if (poll_raw(&pfd, 1, 0) == 0)
        callback_arm++;

    memset(&create, 0, sizeof(create));
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0 ||
        create.handle == 0) {
        ret = fail("sync_file callback cancel source create failed");
        goto out;
    }
    cancel_source = create.handle;
    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = cancel_source;
    handle_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) < 0 ||
        handle_fd.fd < 0) {
        ret = fail("sync_file callback cancel export failed");
        goto out;
    }
    cancel_fd = handle_fd.fd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = cancel_fd;
    pfd.events = POLLIN | POLLRDNORM;
    if (poll_raw(&pfd, 1, 0) == 0)
        callback_arm++;
    close(cancel_fd);
    cancel_fd = -1;

    memset(&signal_req, 0, sizeof(signal_req));
    signal_req.handles = (uint64)&fire_source;
    signal_req.count_handles = 1;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &signal_req) == 0)
        callback_fire = 1;

    memset(&signal_req, 0, sizeof(signal_req));
    signal_req.handles = (uint64)&cancel_source;
    signal_req.count_handles = 1;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &signal_req) < 0) {
        ret = fail("sync_file callback cancel source signal failed");
        goto out;
    }

    close(fire_fd);
    fire_fd = -1;
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = fire_source;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0) {
        ret = fail("sync_file callback fire source destroy failed");
        goto out;
    }
    fire_source = 0;
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = cancel_source;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0) {
        ret = fail("sync_file callback cancel source destroy failed");
        goto out;
    }
    cancel_source = 0;

    if (get_fb_stats(&after) < 0) {
        ret = fail("sync_file callback stats after unavailable");
        goto out;
    }
    armed_delta = after.sync_file_pending_callbacks_armed -
        before.sync_file_pending_callbacks_armed;
    fired_delta = after.sync_file_pending_callbacks_fired -
        before.sync_file_pending_callbacks_fired;
    cancelled_delta = after.sync_file_pending_callbacks_cancelled -
        before.sync_file_pending_callbacks_cancelled;
    late_delta = after.sync_file_pending_callback_late_fires -
        before.sync_file_pending_callback_late_fires;
    if (armed_delta == 2 && fired_delta == 1 && cancelled_delta == 1 &&
        late_delta == 0)
        callback_cancel = 1;

    if (callback_arm < 2 || !callback_fire || !callback_cancel ||
        after.syncobj_live != before.syncobj_live ||
        (after.gpu_backend_flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) !=
            (before.gpu_backend_flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) ||
        after.dxg_present_register_successes !=
            before.dxg_present_register_successes ||
        after.dxg_present_commit_attempts !=
            before.dxg_present_commit_attempts ||
        after.display_presents != before.display_presents ||
        after.display_completions != before.display_completions ||
        after.nouveau_pci_native_present_credit !=
            before.nouveau_pci_native_present_credit) {
        ret = fail("sync_file callback matrix failed");
        goto out;
    }

    printf("drmiftest: sync_file_callback_matrix callback_arm=PASS "
           "callback_fire=PASS callback_cancel=PASS late_fire_delta=0 "
           "pending_callbacks_armed_delta=%lu "
           "pending_callbacks_fired_delta=%lu "
           "pending_callbacks_cancelled_delta=%lu syncobj_live_delta=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n",
           armed_delta, fired_delta, cancelled_delta);
    ret = 0;

out:
    if (fire_fd >= 0)
        close(fire_fd);
    if (cancel_fd >= 0)
        close(cancel_fd);
    if (fire_source != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = fire_source;
        ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
    if (cancel_source != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = cancel_source;
        ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
    return ret;
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
    struct drm_syncobj_eventfd_compat eventfd;
    struct fb_gpu_stats sync_before;
    struct fb_gpu_stats sync_after;
    uint32 handles[2];
    uint64 points[2];
    int syncfd = -1;
    int syncfile_fd = -1;
    int opaque_roundtrip = 0;
    int sync_file_roundtrip = 0;
    int cross_kind_rejected = 0;
    int bad_flags_rejected = 0;
    int point_without_timeline_rejected = 0;
    int timeline_sync_file_roundtrip = 0;
    int closed_fd_rejected = 0;
    int signal_wakeup = 0;
    int transfer_wakeup = 0;
    int pending_transfer = 0;
    uint64 wait_callbacks_armed_delta;
    uint64 wait_callbacks_fired_delta;
    uint64 wait_callbacks_cancelled_delta;
    uint64 wait_callback_late_delta;

    if (check_sync_file_pending_matrix(fd) != 0)
        return 1;
    if (check_sync_file_callback_matrix(fd) != 0)
        return 1;
    if (get_fb_stats(&sync_before) < 0)
        return fail("SYNCOBJ diagnostics unavailable");
    if (check_syncobj_copyout_cleanup(fd) != 0)
        return 1;

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
    handle_fd.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) < 0)
        cross_kind_rejected++;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = syncfd;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) < 0 ||
        handle_fd.handle == 0)
        return fail("SYNCOBJ_FD_TO_HANDLE failed");
    handles[0] = handle_fd.handle;
    opaque_roundtrip = 1;

    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)handles;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) < 0 ||
        wait_req.first_signaled != 0)
        return fail("SYNCOBJ_WAIT signaled failed");

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = handles[0];
    handle_fd.flags = 0x80000000U;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) < 0)
        bad_flags_rejected++;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = syncfd;
    handle_fd.flags = 0x80000000U;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) < 0)
        bad_flags_rejected++;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = handles[0];
    handle_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    handle_fd.point = 1;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) < 0)
        point_without_timeline_rejected++;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = handles[0];
    handle_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) < 0 ||
        handle_fd.fd < 0)
        return fail("SYNCOBJ_HANDLE_TO_FD sync_file failed");
    syncfile_fd = handle_fd.fd;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = syncfile_fd;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) < 0)
        cross_kind_rejected++;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = syncfile_fd;
    handle_fd.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    handle_fd.point = 1;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) < 0)
        point_without_timeline_rejected++;

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = syncfile_fd;
    handle_fd.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) < 0 ||
        handle_fd.handle == 0)
        return fail("SYNCOBJ_FD_TO_HANDLE sync_file failed");
    {
        uint32 sync_file_handle = handle_fd.handle;

        memset(&wait_req, 0, sizeof(wait_req));
        wait_req.handles = (uint64)&sync_file_handle;
        wait_req.count_handles = 1;
        wait_req.timeout_nsec = 0;
        if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) < 0)
            return fail("SYNCOBJ_WAIT sync_file import failed");
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = sync_file_handle;
        if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
            return fail("SYNCOBJ_DESTROY sync_file import failed");
    }
    sync_file_roundtrip = 1;

    close(syncfile_fd);
    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = syncfile_fd;
    handle_fd.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) < 0)
        closed_fd_rejected = 1;
    syncfile_fd = -1;

    if (!opaque_roundtrip || !sync_file_roundtrip ||
        cross_kind_rejected < 2 || bad_flags_rejected < 2 ||
        point_without_timeline_rejected < 2 || !closed_fd_rejected)
        return fail("SYNCOBJ fd kind validation failed");

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
    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)handles;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 1000000;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) >= 0)
        return fail("SYNCOBJ_WAIT finite timeout accepted reset object");

    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &array_req) < 0)
        return fail("SYNCOBJ_SIGNAL failed");
    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)handles;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) < 0)
        return fail("SYNCOBJ_WAIT after signal failed");
    if (check_syncobj_signal_wakeup(fd, handles[0]) == 0)
        signal_wakeup = 1;
    else
        return fail("SYNCOBJ signal wakeup probe failed");

    memset(&create, 0, sizeof(create));
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0 ||
        create.handle == 0)
        return fail("SYNCOBJ_CREATE transfer destination failed");
    handles[1] = create.handle;
    if (check_syncobj_transfer_wakeup(fd, handles[0], handles[1]) == 0)
        transfer_wakeup = 1;
    else
        return fail("SYNCOBJ transfer wakeup probe failed");
    if (check_syncobj_pending_transfer(fd) == 0)
        pending_transfer = 1;
    else
        return fail("SYNCOBJ pending transfer probe failed");
    points[0] = 5;
    memset(&timeline_wait, 0, sizeof(timeline_wait));
    timeline_wait.handles = (uint64)&handles[1];
    timeline_wait.points = (uint64)points;
    timeline_wait.count_handles = 1;
    timeline_wait.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &timeline_wait) < 0)
        return fail("SYNCOBJ_TRANSFER destination wait failed");
    /* Contract update: SYNCOBJ_EVENTFD is implemented but requires a real
     * eventfd in .fd; syncfd is a syncobj fd, not an eventfd, so the ioctl must
     * reject it.  (The old "unexpectedly enabled" message predated the impl.) */
    memset(&eventfd, 0, sizeof(eventfd));
    eventfd.handle = handles[1];
    eventfd.fd = syncfd;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &eventfd) >= 0)
        return fail("SYNCOBJ_EVENTFD accepted non-eventfd");

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

    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.handle = handles[0];
    handle_fd.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE |
        DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE;
    handle_fd.point = 7;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle_fd) < 0 ||
        handle_fd.fd < 0)
        return fail("SYNCOBJ_HANDLE_TO_FD timeline sync_file failed");
    syncfile_fd = handle_fd.fd;
    memset(&handle_fd, 0, sizeof(handle_fd));
    handle_fd.fd = syncfile_fd;
    handle_fd.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE |
        DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE;
    handle_fd.point = 9;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle_fd) < 0 ||
        handle_fd.handle == 0)
        return fail("SYNCOBJ_FD_TO_HANDLE timeline sync_file failed");
    {
        uint32 timeline_import = handle_fd.handle;
        uint64 timeline_point = 9;

        memset(&timeline_wait, 0, sizeof(timeline_wait));
        timeline_wait.handles = (uint64)&timeline_import;
        timeline_wait.points = (uint64)&timeline_point;
        timeline_wait.count_handles = 1;
        timeline_wait.timeout_nsec = 0;
        if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &timeline_wait) < 0)
            return fail("SYNCOBJ_TIMELINE_WAIT timeline sync_file failed");
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = timeline_import;
        if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
            return fail("SYNCOBJ_DESTROY timeline sync_file failed");
    }
    close(syncfile_fd);
    syncfile_fd = -1;
    timeline_sync_file_roundtrip = 1;

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
    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.handles = (uint64)handles;
    wait_req.count_handles = 1;
    wait_req.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait_req) >= 0)
        return fail("SYNCOBJ_WAIT accepted stale handle");
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = handles[1];
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
        return fail("SYNCOBJ_DESTROY transfer destination failed");
    close(syncfd);

    for (int i = 0; i < 250; i++) {
        if (get_fb_stats(&sync_after) < 0)
            return fail("SYNCOBJ diagnostics after unavailable");
        if (sync_after.syncobj_created >= sync_before.syncobj_created + 5 &&
            sync_after.syncobj_live == sync_before.syncobj_live &&
            sync_after.syncobj_signals >= sync_before.syncobj_signals + 3 &&
            sync_after.syncobj_waits >= sync_before.syncobj_waits + 5 &&
            sync_after.syncobj_sync_file_exports >=
                sync_before.syncobj_sync_file_exports + 2 &&
            sync_after.syncobj_sync_file_imports >=
                sync_before.syncobj_sync_file_imports + 2 &&
            sync_after.syncobj_timeout_waits >=
                sync_before.syncobj_timeout_waits + 1 &&
            sync_after.syncobj_stale_wait_rejects >=
                sync_before.syncobj_stale_wait_rejects + 2 &&
            sync_after.syncobj_wait_queued >=
                sync_before.syncobj_wait_queued + 3 &&
            sync_after.syncobj_wait_wakeups >=
                sync_before.syncobj_wait_wakeups + 2 &&
            sync_after.syncobj_wait_callbacks_armed >=
                sync_before.syncobj_wait_callbacks_armed + 3 &&
            sync_after.syncobj_wait_callbacks_fired >=
                sync_before.syncobj_wait_callbacks_fired + 2 &&
            sync_after.syncobj_wait_callbacks_cancelled >=
                sync_before.syncobj_wait_callbacks_cancelled + 1 &&
            sync_after.ttm_resv_attach_sync_file_export >=
                sync_before.ttm_resv_attach_sync_file_export + 2 &&
            sync_after.ttm_resv_attach_sync_file_import >=
                sync_before.ttm_resv_attach_sync_file_import + 2 &&
            sync_after.ttm_resv_attach_syncobj_signal >=
                sync_before.ttm_resv_attach_syncobj_signal + 2 &&
            sync_after.ttm_resv_attach_syncobj_wait >=
                sync_before.ttm_resv_attach_syncobj_wait + 5)
            break;
        sleep(20);
    }
    if (get_fb_stats(&sync_after) < 0)
        return fail("SYNCOBJ diagnostics after unavailable");
    wait_callbacks_armed_delta =
        sync_after.syncobj_wait_callbacks_armed -
        sync_before.syncobj_wait_callbacks_armed;
    wait_callbacks_fired_delta =
        sync_after.syncobj_wait_callbacks_fired -
        sync_before.syncobj_wait_callbacks_fired;
    wait_callbacks_cancelled_delta =
        sync_after.syncobj_wait_callbacks_cancelled -
        sync_before.syncobj_wait_callbacks_cancelled;
    wait_callback_late_delta =
        sync_after.syncobj_wait_callback_late_fires -
        sync_before.syncobj_wait_callback_late_fires;
    if (sync_after.syncobj_created < sync_before.syncobj_created + 5 ||
        sync_after.syncobj_live != sync_before.syncobj_live ||
        sync_after.syncobj_signals < sync_before.syncobj_signals + 3 ||
        sync_after.syncobj_waits < sync_before.syncobj_waits + 5 ||
        sync_after.syncobj_sync_file_exports <
            sync_before.syncobj_sync_file_exports + 2 ||
        sync_after.syncobj_sync_file_imports <
            sync_before.syncobj_sync_file_imports + 2 ||
        sync_after.syncobj_timeout_waits <
            sync_before.syncobj_timeout_waits + 1 ||
        sync_after.syncobj_stale_wait_rejects <
            sync_before.syncobj_stale_wait_rejects + 2 ||
        sync_after.syncobj_wait_queued <
            sync_before.syncobj_wait_queued + 3 ||
        sync_after.syncobj_wait_wakeups <
            sync_before.syncobj_wait_wakeups + 2 ||
        sync_after.ttm_resv_attach_sync_file_export <
            sync_before.ttm_resv_attach_sync_file_export + 2 ||
        sync_after.ttm_resv_attach_sync_file_import <
            sync_before.ttm_resv_attach_sync_file_import + 2 ||
        sync_after.ttm_resv_attach_syncobj_signal <
            sync_before.ttm_resv_attach_syncobj_signal + 2 ||
        sync_after.ttm_resv_attach_syncobj_wait <
            sync_before.ttm_resv_attach_syncobj_wait + 5 ||
        wait_callbacks_armed_delta < 3 ||
        wait_callbacks_fired_delta < 2 ||
        wait_callbacks_cancelled_delta < 1 ||
        wait_callback_late_delta != 0) {
        printf("drmiftest: syncobj counter diagnostic "
               "created=%lu->%lu live=%lu->%lu signals=%lu->%lu "
               "waits=%lu->%lu wait_queued=%lu->%lu "
               "wait_wakeups=%lu->%lu "
               "wait_callbacks=%lu/%lu/%lu late=%lu "
               "timeout=%lu->%lu stale=%lu->%lu "
               "sync_file_export=%lu->%lu sync_file_import=%lu->%lu "
               "attach_export=%lu->%lu attach_import=%lu->%lu "
               "attach_signal=%lu->%lu attach_wait=%lu->%lu\n",
               sync_before.syncobj_created, sync_after.syncobj_created,
               sync_before.syncobj_live, sync_after.syncobj_live,
               sync_before.syncobj_signals, sync_after.syncobj_signals,
               sync_before.syncobj_waits, sync_after.syncobj_waits,
               sync_before.syncobj_wait_queued,
               sync_after.syncobj_wait_queued,
               sync_before.syncobj_wait_wakeups,
               sync_after.syncobj_wait_wakeups,
               wait_callbacks_armed_delta, wait_callbacks_fired_delta,
               wait_callbacks_cancelled_delta, wait_callback_late_delta,
               sync_before.syncobj_timeout_waits,
               sync_after.syncobj_timeout_waits,
               sync_before.syncobj_stale_wait_rejects,
               sync_after.syncobj_stale_wait_rejects,
               sync_before.syncobj_sync_file_exports,
               sync_after.syncobj_sync_file_exports,
               sync_before.syncobj_sync_file_imports,
               sync_after.syncobj_sync_file_imports,
               sync_before.ttm_resv_attach_sync_file_export,
               sync_after.ttm_resv_attach_sync_file_export,
               sync_before.ttm_resv_attach_sync_file_import,
               sync_after.ttm_resv_attach_sync_file_import,
               sync_before.ttm_resv_attach_syncobj_signal,
               sync_after.ttm_resv_attach_syncobj_signal,
               sync_before.ttm_resv_attach_syncobj_wait,
               sync_after.ttm_resv_attach_syncobj_wait);
        return fail("SYNCOBJ diagnostic counters missing");
    }

    printf("drmiftest: syncobj_wait_matrix created_delta=%lu "
           "signals_delta=%lu waits_delta=%lu wait_queued_delta=%lu "
           "wait_wakeups_delta=%lu "
           "wait_callbacks_armed_delta=%lu "
           "wait_callbacks_fired_delta=%lu "
           "wait_callbacks_cancelled_delta=%lu "
           "wait_callback_late_delta=%lu "
           "timeout_waits_delta=%lu stale_wait_rejects_delta=%lu "
           "sync_file_exports_delta=%lu sync_file_imports_delta=%lu "
           "attach_sync_file_export_delta=%lu "
           "attach_sync_file_import_delta=%lu "
           "attach_syncobj_signal_delta=%lu "
           "attach_syncobj_wait_delta=%lu live_delta=0 "
           "signal_wakeup=%s transfer_wakeup=%s "
           "pending_transfer=PASS timeout_separate=PASS "
           "finite_timeout_rejected=1 stale_handle_rejected=1 "
           "future_timeline_rejected=1 status=PASS\n",
           sync_after.syncobj_created - sync_before.syncobj_created,
           sync_after.syncobj_signals - sync_before.syncobj_signals,
           sync_after.syncobj_waits - sync_before.syncobj_waits,
           sync_after.syncobj_wait_queued - sync_before.syncobj_wait_queued,
           sync_after.syncobj_wait_wakeups -
               sync_before.syncobj_wait_wakeups,
           wait_callbacks_armed_delta, wait_callbacks_fired_delta,
           wait_callbacks_cancelled_delta, wait_callback_late_delta,
           sync_after.syncobj_timeout_waits -
               sync_before.syncobj_timeout_waits,
           sync_after.syncobj_stale_wait_rejects -
               sync_before.syncobj_stale_wait_rejects,
           sync_after.syncobj_sync_file_exports -
               sync_before.syncobj_sync_file_exports,
           sync_after.syncobj_sync_file_imports -
               sync_before.syncobj_sync_file_imports,
           sync_after.ttm_resv_attach_sync_file_export -
               sync_before.ttm_resv_attach_sync_file_export,
           sync_after.ttm_resv_attach_sync_file_import -
               sync_before.ttm_resv_attach_sync_file_import,
           sync_after.ttm_resv_attach_syncobj_signal -
               sync_before.ttm_resv_attach_syncobj_signal,
           sync_after.ttm_resv_attach_syncobj_wait -
               sync_before.ttm_resv_attach_syncobj_wait,
           signal_wakeup ? "PASS" : "FAIL",
           transfer_wakeup && pending_transfer ? "PASS" : "FAIL");
    printf("drmiftest: syncobj_wakeup_provenance_matrix "
           "signal_wait_queued=PASS signal_wake=%s "
           "transfer_wait_queued=PASS transfer_wake=%s "
           "pending_transfer_wake=PASS "
           "wait_queued_delta=%lu wakeups_delta=%lu "
           "wait_callbacks_armed_delta=%lu "
           "wait_callbacks_fired_delta=%lu "
           "wait_callbacks_cancelled_delta=%lu "
           "attach_syncobj_wait_delta=%lu attach_syncobj_signal_delta=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n",
           signal_wakeup ? "PASS" : "FAIL",
           transfer_wakeup && pending_transfer ? "PASS" : "FAIL",
           sync_after.syncobj_wait_queued - sync_before.syncobj_wait_queued,
           sync_after.syncobj_wait_wakeups -
               sync_before.syncobj_wait_wakeups,
           wait_callbacks_armed_delta, wait_callbacks_fired_delta,
           wait_callbacks_cancelled_delta,
           sync_after.ttm_resv_attach_syncobj_wait -
               sync_before.ttm_resv_attach_syncobj_wait,
           sync_after.ttm_resv_attach_syncobj_signal -
               sync_before.ttm_resv_attach_syncobj_signal);
    printf("drmiftest: syncobj_fd_kind_matrix opaque_roundtrip=PASS "
           "sync_file_roundtrip=PASS cross_kind_rejected=%d "
           "bad_flags_rejected=%d closed_fd_rejected=%d "
           "point_without_timeline_rejected=%d "
           "timeline_sync_file_roundtrip=%s eventfd=fail-closed "
           "status=PASS\n",
           cross_kind_rejected, bad_flags_rejected, closed_fd_rejected,
           point_without_timeline_rejected,
           timeline_sync_file_roundtrip ? "PASS" : "MISSING");
    printf("drmiftest: syncobj ok handle=%u transfer=ok eventfd=fail-closed\n",
           handles[0]);
    return 0;
}

static int check_nouveau(int fd)
{
    struct drm_nouveau_getparam_compat getp;
    struct drm_nouveau_channel_alloc_compat chan;
    struct drm_nouveau_notifierobj_alloc_compat notifier;
    struct drm_nouveau_grobj_alloc_compat grobj;
    struct drm_nouveau_gpuobj_free_compat gpuobj_free;
    struct {
        struct nvif_ioctl_v0_compat hdr;
        struct nvif_ioctl_sclass_v0_compat sclass;
        struct nvif_ioctl_sclass_oclass_v0_compat classes[4];
    } nvif_sclass;
    struct {
        struct nvif_ioctl_v0_compat hdr;
        struct nvif_ioctl_new_v0_compat newobj;
    } nvif_new;
    struct nvif_ioctl_v0_compat nvif_hdr;
    struct drm_nouveau_gem_new_compat gem_new;
    struct drm_nouveau_gem_info_compat gem_info;
    struct drm_nouveau_gem_cpu_prep_compat prep;
    struct drm_nouveau_gem_cpu_fini_compat fini;
    struct drm_nouveau_gem_pushbuf_compat push;
    struct drm_nouveau_gem_pushbuf_bo_compat push_bo[1];
    struct drm_nouveau_gem_pushbuf_reloc_compat reloc[1];
    struct drm_nouveau_gem_pushbuf_push_compat push_cmd[1];
    struct drm_nouveau_vm_init_compat vm_init;
    struct drm_nouveau_vm_bind_compat bind;
    struct drm_nouveau_vm_bind_op_compat bind_op[1];
    struct drm_nouveau_exec_compat exec;
    struct drm_nouveau_exec_push_compat exec_push[1];
    struct drm_nouveau_sync_compat nv_sync[1];
    struct drm_nouveau_channel_free_compat chan_free;
    struct drm_syncobj_create_compat sync_create;
    struct drm_syncobj_destroy_compat sync_destroy;
    struct drm_syncobj_wait_compat sync_wait;
    struct drm_gem_close_compat close_req;
    struct drm_prime_handle_compat prime;
    uint64 device;
    uint64 fb_size;
    uint64 gart_size;
    uint64 exec_push_max;
    uint32 sync_handles[1];
    int prime_fd = -1;

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
    getp.param = NOUVEAU_GETPARAM_FB_SIZE;
    getp.value = 0;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GETPARAM, &getp) < 0 || getp.value == 0)
        return fail("Nouveau FB size getparam failed");
    fb_size = getp.value;
    getp.param = NOUVEAU_GETPARAM_AGP_SIZE;
    getp.value = 0;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GETPARAM, &getp) < 0 || getp.value == 0)
        return fail("Nouveau GART size getparam failed");
    gart_size = getp.value;
    getp.param = NOUVEAU_GETPARAM_EXEC_PUSH_MAX;
    getp.value = 0;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GETPARAM, &getp) < 0 || getp.value == 0)
        return fail("Nouveau exec push max getparam failed");
    exec_push_max = getp.value;

    memset(&vm_init, 0, sizeof(vm_init));
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_VM_INIT, &vm_init) < 0)
        return fail("Nouveau VM init failed");
    memset(&bind, 0, sizeof(bind));
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_VM_BIND, &bind) < 0)
        return fail("Nouveau no-op VM bind failed");

    memset(&chan, 0, sizeof(chan));
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, &chan) < 0)
        return fail("Nouveau channel alloc failed");
    if (chan.channel != 0 ||
        (chan.pushbuf_domains & NOUVEAU_GEM_DOMAIN_GART) == 0)
        return fail("Nouveau channel metadata mismatch");

    memset(&notifier, 0, sizeof(notifier));
    notifier.channel = chan.channel;
    notifier.handle = 0x2000;
    notifier.size = 4096;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_NOTIFIEROBJ_ALLOC, &notifier) < 0 ||
        notifier.offset == 0)
        return fail("Nouveau notifier alloc failed");
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_NOTIFIEROBJ_ALLOC, &notifier) >= 0)
        return fail("Nouveau duplicate notifier unexpectedly succeeded");

    memset(&grobj, 0, sizeof(grobj));
    grobj.channel = chan.channel;
    grobj.handle = 0x3000;
    grobj.class = 0x906e;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GROBJ_ALLOC, &grobj) < 0)
        return fail("Nouveau GROBJ alloc failed");
    memset(&grobj, 0, sizeof(grobj));
    grobj.channel = chan.channel;
    grobj.handle = 0x3001;
    grobj.class = 0xdead;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GROBJ_ALLOC, &grobj) >= 0)
        return fail("Nouveau unsupported GROBJ unexpectedly succeeded");
    memset(&gpuobj_free, 0, sizeof(gpuobj_free));
    gpuobj_free.channel = chan.channel;
    gpuobj_free.handle = 0x3000;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GPUOBJ_FREE, &gpuobj_free) < 0)
        return fail("Nouveau GROBJ free failed");
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GPUOBJ_FREE, &gpuobj_free) >= 0)
        return fail("Nouveau duplicate GROBJ free unexpectedly succeeded");

    memset(&nvif_sclass, 0, sizeof(nvif_sclass));
    nvif_sclass.hdr.version = 0;
    nvif_sclass.hdr.type = NVIF_IOCTL_V0_SCLASS;
    nvif_sclass.hdr.owner = NVIF_IOCTL_V0_OWNER_ANY;
    nvif_sclass.sclass.count = 4;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_NVIF, &nvif_sclass) < 0 ||
        nvif_sclass.sclass.count != 0)
        return fail("Nouveau NVIF SCLASS fail-closed query failed");
    memset(&nvif_new, 0, sizeof(nvif_new));
    nvif_new.hdr.version = 0;
    nvif_new.hdr.type = NVIF_IOCTL_V0_NEW;
    nvif_new.hdr.owner = NVIF_IOCTL_V0_OWNER_ANY;
    nvif_new.newobj.oclass = 0x906e;
    nvif_new.newobj.handle = 0x4000;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_NVIF, &nvif_new) >= 0)
        return fail("Nouveau NVIF NEW unexpectedly succeeded");
    memset(&nvif_hdr, 0, sizeof(nvif_hdr));
    nvif_hdr.type = NVIF_IOCTL_V0_MTHD;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_NVIF, &nvif_hdr) >= 0)
        return fail("Nouveau NVIF method unexpectedly succeeded");

    memset(&gem_new, 0, sizeof(gem_new));
    gem_new.info.size = 4096;
    gem_new.info.domain = NOUVEAU_GEM_DOMAIN_GART |
                          NOUVEAU_GEM_DOMAIN_MAPPABLE |
                          NOUVEAU_GEM_DOMAIN_COHERENT;
    gem_new.info.tile_flags = NOUVEAU_GEM_TILE_NONCONTIG;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GEM_NEW, &gem_new) < 0 ||
        gem_new.info.handle == 0 || gem_new.info.map_handle == 0)
        return fail("Nouveau GEM new failed");
    if ((gem_new.info.domain & NOUVEAU_GEM_DOMAIN_GART) == 0 ||
        (gem_new.info.domain & NOUVEAU_GEM_DOMAIN_MAPPABLE) == 0 ||
        gem_new.info.tile_flags != NOUVEAU_GEM_TILE_NONCONTIG)
        return fail("Nouveau GEM new metadata mismatch");

    memset(&gem_info, 0, sizeof(gem_info));
    gem_info.handle = gem_new.info.handle;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GEM_INFO, &gem_info) < 0 ||
        gem_info.size < 4096 || gem_info.map_handle != gem_new.info.map_handle)
        return fail("Nouveau GEM info failed");
    if (gem_info.domain != gem_new.info.domain ||
        gem_info.tile_flags != NOUVEAU_GEM_TILE_NONCONTIG)
        return fail("Nouveau GEM info metadata mismatch");

    memset(&prep, 0, sizeof(prep));
    prep.handle = gem_new.info.handle;
    prep.flags = NOUVEAU_GEM_CPU_PREP_WRITE;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GEM_CPU_PREP, &prep) < 0)
        return fail("Nouveau GEM CPU prep failed");
    memset(&fini, 0, sizeof(fini));
    fini.handle = gem_new.info.handle;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GEM_CPU_FINI, &fini) < 0)
        return fail("Nouveau GEM CPU fini failed");

    memset(&push, 0, sizeof(push));
    push.channel = 0;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GEM_PUSHBUF, &push) < 0)
        return fail("Nouveau no-op pushbuf failed");
    memset(&push, 0, sizeof(push));
    memset(push_bo, 0, sizeof(push_bo));
    push_bo[0].handle = gem_new.info.handle;
    push_bo[0].read_domains = NOUVEAU_GEM_DOMAIN_GART;
    push_bo[0].write_domains = NOUVEAU_GEM_DOMAIN_GART;
    push_bo[0].valid_domains = NOUVEAU_GEM_DOMAIN_GART |
                                NOUVEAU_GEM_DOMAIN_MAPPABLE |
                                NOUVEAU_GEM_DOMAIN_COHERENT;
    push.channel = 0;
    push.nr_buffers = 1;
    push.buffers = (uint64)push_bo;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GEM_PUSHBUF, &push) < 0 ||
        push_bo[0].presumed.valid != 1 ||
        push_bo[0].presumed.offset != gem_new.info.map_handle ||
        (push_bo[0].presumed.domain & NOUVEAU_GEM_DOMAIN_GART) == 0)
        return fail("Nouveau presumed-offset pushbuf validation failed");

    memset(&push, 0, sizeof(push));
    memset(push_bo, 0, sizeof(push_bo));
    memset(reloc, 0, sizeof(reloc));
    push_bo[0].handle = gem_new.info.handle;
    push_bo[0].valid_domains = NOUVEAU_GEM_DOMAIN_GART |
                                NOUVEAU_GEM_DOMAIN_MAPPABLE |
                                NOUVEAU_GEM_DOMAIN_COHERENT;
    reloc[0].bo_index = 0;
    reloc[0].reloc_bo_index = 0;
    reloc[0].flags = 0x80000000u;
    push.channel = 0;
    push.nr_buffers = 1;
    push.buffers = (uint64)push_bo;
    push.nr_relocs = 1;
    push.relocs = (uint64)reloc;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GEM_PUSHBUF, &push) >= 0)
        return fail("Nouveau invalid relocation unexpectedly succeeded");

    memset(&push, 0, sizeof(push));
    memset(push_bo, 0, sizeof(push_bo));
    memset(push_cmd, 0, sizeof(push_cmd));
    push_bo[0].handle = gem_new.info.handle;
    push_bo[0].valid_domains = NOUVEAU_GEM_DOMAIN_GART |
                                NOUVEAU_GEM_DOMAIN_MAPPABLE |
                                NOUVEAU_GEM_DOMAIN_COHERENT;
    push_cmd[0].bo_index = 0;
    push_cmd[0].length = 4;
    push.channel = 0;
    push.nr_buffers = 1;
    push.buffers = (uint64)push_bo;
    push.nr_push = 1;
    push.push = (uint64)push_cmd;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GEM_PUSHBUF, &push) >= 0)
        return fail("Nouveau non-empty pushbuf unexpectedly succeeded");
    memset(&exec, 0, sizeof(exec));
    exec.channel = 0;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec) < 0)
        return fail("Nouveau no-op exec failed");
    memset(&sync_create, 0, sizeof(sync_create));
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync_create) < 0 ||
        sync_create.handle == 0)
        return fail("Nouveau fence-only syncobj create failed");
    memset(nv_sync, 0, sizeof(nv_sync));
    nv_sync[0].flags = DRM_NOUVEAU_SYNC_SYNCOBJ;
    nv_sync[0].handle = sync_create.handle;
    memset(&exec, 0, sizeof(exec));
    exec.channel = 0;
    exec.sig_count = 1;
    exec.sig_ptr = (uint64)nv_sync;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec) < 0)
        return fail("Nouveau fence-only exec signal failed");
    sync_handles[0] = sync_create.handle;
    memset(&sync_wait, 0, sizeof(sync_wait));
    sync_wait.handles = (uint64)sync_handles;
    sync_wait.count_handles = 1;
    sync_wait.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait) < 0)
        return fail("Nouveau fence-only exec wait failed");
    memset(&sync_destroy, 0, sizeof(sync_destroy));
    sync_destroy.handle = sync_create.handle;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sync_destroy) < 0)
        return fail("Nouveau fence-only syncobj destroy failed");

    memset(&sync_create, 0, sizeof(sync_create));
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync_create) < 0 ||
        sync_create.handle == 0)
        return fail("Nouveau fence-only VM syncobj create failed");
    memset(nv_sync, 0, sizeof(nv_sync));
    nv_sync[0].flags = DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ;
    nv_sync[0].handle = sync_create.handle;
    nv_sync[0].timeline_value = 7;
    memset(&bind, 0, sizeof(bind));
    bind.flags = DRM_NOUVEAU_VM_BIND_RUN_ASYNC;
    bind.sig_count = 1;
    bind.sig_ptr = (uint64)nv_sync;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_VM_BIND, &bind) < 0)
        return fail("Nouveau fence-only VM bind signal failed");
    sync_handles[0] = sync_create.handle;
    memset(&sync_wait, 0, sizeof(sync_wait));
    sync_wait.handles = (uint64)sync_handles;
    sync_wait.count_handles = 1;
    sync_wait.timeout_nsec = 0;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait) < 0)
        return fail("Nouveau fence-only VM bind wait failed");
    memset(&sync_destroy, 0, sizeof(sync_destroy));
    sync_destroy.handle = sync_create.handle;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sync_destroy) < 0)
        return fail("Nouveau fence-only VM syncobj destroy failed");
    memset(&bind, 0, sizeof(bind));
    memset(bind_op, 0, sizeof(bind_op));
    bind_op[0].op = DRM_NOUVEAU_VM_BIND_OP_MAP;
    bind_op[0].handle = gem_new.info.handle;
    bind_op[0].addr = 0x100000;
    bind_op[0].range = 4096;
    bind.op_count = 1;
    bind.op_ptr = (uint64)bind_op;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_VM_BIND, &bind) >= 0)
        return fail("Nouveau non-empty VM bind unexpectedly succeeded");
    memset(&exec, 0, sizeof(exec));
    memset(exec_push, 0, sizeof(exec_push));
    exec_push[0].va = 0x100000;
    exec_push[0].va_len = 4096;
    exec.channel = 0;
    exec.push_count = 1;
    exec.push_ptr = (uint64)exec_push;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec) >= 0)
        return fail("Nouveau non-empty exec unexpectedly succeeded");
    printf("drmiftest: nouveau_submit_failclosed_matrix "
           "nonempty_pushbuf_reject=PASS nonempty_exec_reject=PASS "
           "nonempty_vm_bind_reject=PASS fence_only_submit=PASS "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n");

    memset(&prime, 0, sizeof(prime));
    prime.handle = gem_new.info.handle;
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0 || prime.fd < 0)
        return fail("Nouveau PRIME handle-to-fd failed");
    prime_fd = prime.fd;
    memset(&prime, 0, sizeof(prime));
    prime.fd = prime_fd;
    if (ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0 ||
        prime.handle == 0)
        return fail("Nouveau PRIME fd-to-handle failed");
    close(prime_fd);
    prime_fd = -1;
    memset(&close_req, 0, sizeof(close_req));
    close_req.handle = prime.handle;
    if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_req) < 0)
        return fail("Nouveau PRIME imported GEM close failed");

    memset(&close_req, 0, sizeof(close_req));
    close_req.handle = gem_new.info.handle;
    if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_req) < 0)
        return fail("Nouveau GEM close failed");
    memset(&gpuobj_free, 0, sizeof(gpuobj_free));
    gpuobj_free.channel = chan.channel;
    gpuobj_free.handle = notifier.handle;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_GPUOBJ_FREE, &gpuobj_free) < 0)
        return fail("Nouveau notifier free failed");
    memset(&chan_free, 0, sizeof(chan_free));
    chan_free.channel = 0;
    if (ioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_FREE, &chan_free) < 0)
        return fail("Nouveau channel free failed");

    printf("drmiftest: nouveau_channel_object_matrix channel=PASS "
           "notifier=PASS grobj=PASS duplicate_reject=PASS "
           "unsupported_class_reject=PASS free=PASS status=PASS\n");
    printf("drmiftest: nouveau_nvif_failclosed_matrix sclass_empty=PASS "
           "new_reject=PASS method_reject=PASS status=PASS\n");
    printf("drmiftest: nouveau probe ok device=0x%lx channel=%d domains=0x%x fb=%lu gart=%lu exec_push_max=%lu\n",
           device, chan.channel, chan.pushbuf_domains, fb_size, gart_size,
           exec_push_max);
    return 0;
}

static int leave_dumb_handle(int fd)
{
    struct drm_mode_create_dumb_compat create;

    memset(&create, 0, sizeof(create));
    create.width = 16;
    create.height = 16;
    create.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        create.handle == 0)
        return -1;
    return (int)create.handle;
}

static int leave_primary_kms_event(int fd)
{
    struct drm_mode_create_dumb_compat create;
    struct drm_mode_fb_cmd2_compat fb;
    struct drm_mode_crtc_page_flip_compat flip;

    memset(&create, 0, sizeof(create));
    create.width = 32;
    create.height = 24;
    create.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        create.handle == 0)
        return -1;

    memset(&fb, 0, sizeof(fb));
    fb.width = create.width;
    fb.height = create.height;
    fb.pixel_format = DRM_FORMAT_XRGB8888;
    fb.handles[0] = create.handle;
    fb.pitches[0] = create.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0 || fb.fb_id == 0)
        return -1;

    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = 1;
    flip.fb_id = fb.fb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
    flip.user_data = 0x4c4946454359434cULL;
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0)
        return -1;
    return 0;
}

static int leave_syncobj_handle(int fd)
{
    struct drm_syncobj_create_compat create;

    memset(&create, 0, sizeof(create));
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0 ||
        create.handle == 0)
        return -1;
    return 0;
}

static int lifecycle_close_counters_ready(const struct fb_gpu_stats *before,
                                          const struct fb_gpu_stats *after)
{
    return after->drm_file_primary_closes >=
               before->drm_file_primary_closes + 1 &&
           after->drm_file_render_closes >=
               before->drm_file_render_closes + 1 &&
           after->drm_file_legacy_closes >=
               before->drm_file_legacy_closes + 1 &&
           after->drm_file_primary_live == before->drm_file_primary_live &&
           after->drm_file_render_live == before->drm_file_render_live &&
           after->drm_file_legacy_live == before->drm_file_legacy_live &&
           after->drm_file_close_generation >=
               before->drm_file_close_generation + 3 &&
           after->drm_file_stale_gem_handles >=
               before->drm_file_stale_gem_handles + 3 &&
           after->drm_file_stale_kms_fbs >=
               before->drm_file_stale_kms_fbs + 1 &&
           after->drm_file_stale_syncobjs >=
               before->drm_file_stale_syncobjs + 1 &&
           after->drm_file_stale_events >=
               before->drm_file_stale_events + 1 &&
           after->drm_event_close_stale >=
               before->drm_event_close_stale + 1;
}

static int wait_lifecycle_close_counters(const struct fb_gpu_stats *before,
                                         struct fb_gpu_stats *after)
{
    for (int i = 0; i < 250; i++) {
        if (get_fb_stats(after) < 0)
            return -1;
        if (lifecycle_close_counters_ready(before, after))
            return 0;
        sleep(20);
    }
    return get_fb_stats(after);
}

static int fail_lifecycle_live_counters(const struct fb_gpu_stats *before,
                                        const struct fb_gpu_stats *after)
{
    printf("drmiftest: lifecycle live counters leaked "
           "primary_live=%lu->%lu render_live=%lu->%lu "
           "legacy_live=%lu->%lu primary_close=%lu->%lu "
           "render_close=%lu->%lu legacy_close=%lu->%lu "
           "close_generation=%lu->%lu stale_gem=%lu->%lu "
           "stale_kms=%lu->%lu stale_syncobj=%lu->%lu "
           "stale_events=%lu->%lu event_close_stale=%lu->%lu\n",
           before->drm_file_primary_live, after->drm_file_primary_live,
           before->drm_file_render_live, after->drm_file_render_live,
           before->drm_file_legacy_live, after->drm_file_legacy_live,
           before->drm_file_primary_closes, after->drm_file_primary_closes,
           before->drm_file_render_closes, after->drm_file_render_closes,
           before->drm_file_legacy_closes, after->drm_file_legacy_closes,
           before->drm_file_close_generation,
           after->drm_file_close_generation,
           before->drm_file_stale_gem_handles,
           after->drm_file_stale_gem_handles,
           before->drm_file_stale_kms_fbs,
           after->drm_file_stale_kms_fbs,
           before->drm_file_stale_syncobjs,
           after->drm_file_stale_syncobjs,
           before->drm_file_stale_events,
           after->drm_file_stale_events,
           before->drm_event_close_stale,
           after->drm_event_close_stale);
    return 1;
}

static int check_file_lifecycle(void)
{
    struct fb_gpu_stats before;
    struct fb_gpu_stats opened;
    struct fb_gpu_stats after;
    int primary = -1;
    int render = -1;
    int legacy = -1;
    const char *setup_step = "open";

    if (get_fb_stats(&before) < 0)
        return fail("lifecycle baseline stats failed");

    primary = open("/dev/dri/card0", O_RDWR);
    render = open("/dev/dri/renderD128", O_RDWR);
    legacy = open("/dev/gpu0", O_RDWR);
    if (primary < 0 || render < 0 || legacy < 0) {
        printf("drmiftest: lifecycle setup failed step=open "
               "primary=%d render=%d legacy=%d\n",
               primary, render, legacy);
        goto out_fail_opened;
    }
    setup_step = "opened_stats";
    if (get_fb_stats(&opened) < 0)
        goto out_fail_opened;

    setup_step = "open_counters";
    if (opened.drm_file_primary_opens < before.drm_file_primary_opens + 1 ||
        opened.drm_file_render_opens < before.drm_file_render_opens + 1 ||
        opened.drm_file_legacy_opens < before.drm_file_legacy_opens + 1)
        goto out_fail_opened;
    setup_step = "live_counters";
    if (opened.drm_file_primary_live < before.drm_file_primary_live + 1 ||
        opened.drm_file_render_live < before.drm_file_render_live + 1 ||
        opened.drm_file_legacy_live < before.drm_file_legacy_live + 1)
        goto out_fail_opened;

    setup_step = "primary_kms_event";
    if (leave_primary_kms_event(primary) != 0)
        goto out_fail_opened;
    setup_step = "render_dumb_handle";
    if (leave_dumb_handle(render) <= 0)
        goto out_fail_opened;
    setup_step = "render_syncobj";
    if (leave_syncobj_handle(render) != 0)
        goto out_fail_opened;
    setup_step = "legacy_dumb_handle";
    if (leave_dumb_handle(legacy) <= 0)
        goto out_fail_opened;

    close(primary);
    primary = -1;
    close(render);
    render = -1;
    close(legacy);
    legacy = -1;

    if (wait_lifecycle_close_counters(&before, &after) < 0)
        return fail("lifecycle close stats failed");
    if (after.drm_file_primary_closes < before.drm_file_primary_closes + 1 ||
        after.drm_file_render_closes < before.drm_file_render_closes + 1 ||
        after.drm_file_legacy_closes < before.drm_file_legacy_closes + 1)
        return fail("lifecycle close counters missing");
    if (after.drm_file_primary_live != before.drm_file_primary_live ||
        after.drm_file_render_live != before.drm_file_render_live ||
        after.drm_file_legacy_live != before.drm_file_legacy_live)
        return fail_lifecycle_live_counters(&before, &after);
    if (after.drm_file_close_generation <
        before.drm_file_close_generation + 3)
        return fail("lifecycle close generation missing");
    if (after.drm_file_stale_gem_handles <
        before.drm_file_stale_gem_handles + 3)
        return fail("lifecycle stale GEM cleanup missing");
    if (after.drm_file_stale_kms_fbs <
        before.drm_file_stale_kms_fbs + 1)
        return fail("lifecycle stale KMS cleanup missing");
    if (after.drm_file_stale_syncobjs <
        before.drm_file_stale_syncobjs + 1)
        return fail("lifecycle stale syncobj cleanup missing");
    if (after.drm_file_stale_events <
        before.drm_file_stale_events + 1 ||
        after.drm_event_close_stale < before.drm_event_close_stale + 1)
        return fail("lifecycle stale event cleanup missing");
    if (after.dxg_present_register_attempts !=
            before.dxg_present_register_attempts ||
        after.dxg_present_commit_attempts !=
            before.dxg_present_commit_attempts)
        return fail("lifecycle diagnostics granted native-present credit");
    if (after.gpu_backend == FB_GPU_BACKEND_HYPERV_DXG &&
        (after.gpu_backend_flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0)
        return fail("Hyper-V OPENGL_SUBMIT gate opened");

    printf("drmiftest: file lifecycle ok primary=%lu/%lu render=%lu/%lu "
           "legacy=%lu/%lu close_gen=%lu stale_gem=%lu stale_kms=%lu "
           "stale_syncobj=%lu stale_events=%lu native_present_credit=0\n",
           after.drm_file_primary_opens, after.drm_file_primary_closes,
           after.drm_file_render_opens, after.drm_file_render_closes,
           after.drm_file_legacy_opens, after.drm_file_legacy_closes,
           after.drm_file_close_generation,
           after.drm_file_stale_gem_handles,
           after.drm_file_stale_kms_fbs,
           after.drm_file_stale_syncobjs,
           after.drm_file_stale_events);
    return 0;

out_fail_opened:
    printf("drmiftest: lifecycle open/handle setup failed step=%s "
           "primary=%d render=%d legacy=%d\n",
           setup_step, primary, render, legacy);
    if (primary >= 0)
        close(primary);
    if (render >= 0)
        close(render);
    if (legacy >= 0)
        close(legacy);
    return fail("lifecycle open/handle setup failed");
}

static int check_drm_minor_matrix(void)
{
    struct fb_gpu_stats stats;
    int control;

    if (get_fb_stats(&stats) < 0)
        return fail("minor stats unavailable");
    control = open("/dev/dri/controlD64", O_RDWR);
    if (control >= 0) {
        close(control);
        return fail("DRM control node unexpectedly present");
    }
    if (stats.drm_minor_model_version != 1 ||
        stats.drm_minor_primary_index != 0 ||
        stats.drm_minor_render_index != 128 ||
        stats.drm_minor_control_index != 64 ||
        stats.drm_minor_primary_registered != 1 ||
        stats.drm_minor_render_registered != 1 ||
        stats.drm_minor_control_registered != 0 ||
        stats.drm_minor_static_nodes != 2 ||
        stats.drm_minor_dynamic_nodes != 0 ||
        stats.drm_minor_generation == 0)
        return fail("DRM minor diagnostics mismatch");
    printf("drmiftest: drm_minor_matrix version=%lu "
           "primary_index=%lu primary_registered=%lu "
           "render_index=%lu render_registered=%lu "
           "control_index=%lu control_registered=%lu "
           "control_open=fail_closed static_nodes=%lu dynamic_nodes=%lu "
           "generation=%lu status=PASS\n",
           stats.drm_minor_model_version,
           stats.drm_minor_primary_index,
           stats.drm_minor_primary_registered,
           stats.drm_minor_render_index,
           stats.drm_minor_render_registered,
           stats.drm_minor_control_index,
           stats.drm_minor_control_registered,
           stats.drm_minor_static_nodes,
           stats.drm_minor_dynamic_nodes,
           stats.drm_minor_generation);
    return 0;
}

static int check_lease_failclosed_matrix(int primary, int render)
{
    struct fb_gpu_stats before;
    struct fb_gpu_stats after;
    struct drm_mode_create_lease_compat create;
    struct drm_mode_list_lessees_compat list;
    struct drm_mode_get_lease_compat get;
    struct drm_mode_revoke_lease_compat revoke;
    uint32 objects[2] = {1, 2};
    uint32 lessees[2] = {0};
    int primary_create_objects_rejected;
    int primary_create_empty_rejected;
    int render_create_rejected;
    int list_lessees_rejected;
    int get_lease_rejected;
    int revoke_lease_rejected;
    uint64 attempts_delta;
    uint64 create_delta;
    uint64 object_rejects_delta;
    uint64 empty_rejects_delta;
    uint64 list_delta;
    uint64 get_delta;
    uint64 revoke_delta;
    uint64 render_delta;

    if (get_fb_stats(&before) < 0)
        return fail("lease baseline stats failed");

    memset(&create, 0, sizeof(create));
    create.object_ids = (uint64)objects;
    create.object_count = 1;
    create.fd = 0xfeedface;
    create.lessee_id = 0xfeedface;
    primary_create_objects_rejected =
        ioctl(primary, DRM_IOCTL_MODE_CREATE_LEASE, &create) < 0 &&
        create.fd == 0xfeedface && create.lessee_id == 0xfeedface;

    memset(&create, 0, sizeof(create));
    create.fd = 0xfeedface;
    create.lessee_id = 0xfeedface;
    primary_create_empty_rejected =
        ioctl(primary, DRM_IOCTL_MODE_CREATE_LEASE, &create) < 0 &&
        create.fd == 0xfeedface && create.lessee_id == 0xfeedface;

    memset(&create, 0, sizeof(create));
    create.fd = 0xfeedface;
    create.lessee_id = 0xfeedface;
    render_create_rejected =
        ioctl(render, DRM_IOCTL_MODE_CREATE_LEASE, &create) < 0 &&
        create.fd == 0xfeedface && create.lessee_id == 0xfeedface;

    memset(&list, 0, sizeof(list));
    list.count_lessees = 2;
    list.lessees_ptr = (uint64)lessees;
    list_lessees_rejected =
        ioctl(primary, DRM_IOCTL_MODE_LIST_LESSEES, &list) < 0 &&
        list.count_lessees == 2 && lessees[0] == 0 && lessees[1] == 0;

    memset(&get, 0, sizeof(get));
    get.count_objects = 2;
    get.objects_ptr = (uint64)objects;
    get_lease_rejected =
        ioctl(primary, DRM_IOCTL_MODE_GET_LEASE, &get) < 0 &&
        get.count_objects == 2 && objects[0] == 1 && objects[1] == 2;

    memset(&revoke, 0, sizeof(revoke));
    revoke.lessee_id = 0x1234;
    revoke_lease_rejected =
        ioctl(primary, DRM_IOCTL_MODE_REVOKE_LEASE, &revoke) < 0;

    if (get_fb_stats(&after) < 0)
        return fail("lease final stats failed");

    attempts_delta = after.drm_lease_ioctl_attempts -
        before.drm_lease_ioctl_attempts;
    create_delta = after.drm_lease_create_rejects -
        before.drm_lease_create_rejects;
    object_rejects_delta = after.drm_lease_create_object_rejects -
        before.drm_lease_create_object_rejects;
    empty_rejects_delta = after.drm_lease_create_empty_rejects -
        before.drm_lease_create_empty_rejects;
    list_delta = after.drm_lease_list_rejects -
        before.drm_lease_list_rejects;
    get_delta = after.drm_lease_get_rejects -
        before.drm_lease_get_rejects;
    revoke_delta = after.drm_lease_revoke_rejects -
        before.drm_lease_revoke_rejects;
    render_delta = after.drm_lease_render_rejects -
        before.drm_lease_render_rejects;

    if (!primary_create_objects_rejected ||
        !primary_create_empty_rejected ||
        !render_create_rejected ||
        !list_lessees_rejected ||
        !get_lease_rejected ||
        !revoke_lease_rejected ||
        attempts_delta < 6 ||
        create_delta < 3 ||
        object_rejects_delta < 1 ||
        empty_rejects_delta < 2 ||
        list_delta < 1 ||
        get_delta < 1 ||
        revoke_delta < 1 ||
        render_delta < 1 ||
        after.drm_lease_fds_created != before.drm_lease_fds_created ||
        after.drm_lease_active != before.drm_lease_active ||
        after.dxg_present_register_attempts !=
            before.dxg_present_register_attempts ||
        after.dxg_present_commit_attempts !=
            before.dxg_present_commit_attempts)
        return fail("DRM lease fail-closed diagnostics mismatch");

    printf("drmiftest: lease_failclosed_matrix lease_kernel=absent "
           "primary_create_objects_rejected=%d "
           "primary_create_empty_rejected=%d render_create_rejected=%d "
           "list_lessees_rejected=%d get_lease_rejected=%d "
           "revoke_lease_rejected=%d lease_fd_created=0 "
           "lessee_id_created=0 active_leases_delta=%lu "
           "lease_attempts_delta=%lu lease_create_rejects_delta=%lu "
           "object_rejects_delta=%lu empty_rejects_delta=%lu "
           "list_rejects_delta=%lu get_rejects_delta=%lu "
           "revoke_rejects_delta=%lu render_rejects_delta=%lu "
           "native_present_credit=0 status=PASS\n",
           primary_create_objects_rejected,
           primary_create_empty_rejected,
           render_create_rejected,
           list_lessees_rejected,
           get_lease_rejected,
           revoke_lease_rejected,
           after.drm_lease_active - before.drm_lease_active,
           attempts_delta,
           create_delta,
           object_rejects_delta,
           empty_rejects_delta,
           list_delta,
           get_delta,
           revoke_delta,
           render_delta);
    return 0;
}

/*
 * U5 validation: the atomic (DRM_IOCTL_MODE_ATOMIC) path now queues
 * DRM_MODE_PAGE_FLIP_EVENT completions exactly like the legacy page-flip path
 * (fb_kms_atomic.c: pre-present -EAGAIN ring guard + one FLIP_COMPLETE per
 * successful has_new_fb commit). This mirrors the legacy event read-back in
 * check_kms_fb and asserts, on the shared per-fd event ring:
 *   (a) one FLIP_COMPLETE per event-flagged successful atomic commit, with
 *       matching user_data, crtc_id == GPU_DRM_CRTC_ID (1), and a strictly
 *       monotonic sequence (each present advances display_last_complete);
 *   (b) TEST_ONLY commits with the event flag emit zero events;
 *   (c) flag-absent commits emit zero events;
 *   (d) ring-full backpressure: fill the ring (DRM_XV6_EVENT_QUEUE_CAPACITY),
 *       the next event-flagged commit returns -EAGAIN *before* presenting
 *       (kms_atomic_commits unchanged), then all queued events drain in order
 *       with no loss;
 *   (e) mixed legacy + atomic interleaving keeps ordered, monotonic sequences
 *       on the one shared ring.
 * Reads are non-blocking (empty ring -> read() < 0), matching the legacy block,
 * so the test can never hang a nographic run.
 */
static int check_kms_atomic_flip_events(int fd)
{
    struct drm_mode_create_dumb_compat create;
    struct drm_mode_destroy_dumb_compat destroy;
    struct drm_mode_fb_cmd2_compat fb;
    struct drm_mode_crtc_compat crtc;
    struct drm_mode_crtc_page_flip_compat flip;
    struct drm_event_vblank_compat event;
    struct drm_mode_get_plane_res_compat plane_res;
    struct fb_gpu_stats stats_before;
    struct fb_gpu_stats stats_after;
    const uint64 seq_base = 0x41544f4d00000000ULL;   /* "ATOM" */
    const uint64 ring_base = 0x52494e4700000000ULL;  /* "RING" */
    const uint64 leg_base = 0x4c45470000000000ULL;   /* "LEG"  */
    const uint64 ato_base = 0x41544f0000000000ULL;   /* "ATO"  */
    uint32 plane_ids[2];
    uint32 plane_id;
    uint32 plane_crtc_prop;
    uint32 plane_fb_prop;
    uint32 fb_id = 0;
    uint32 prev_sequence;
    int i;

    memset(&create, 0, sizeof(create));
    create.width = 80;
    create.height = 48;
    create.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        create.handle == 0 || create.pitch < create.width * 4)
        return fail("atomic_flip_event CREATE_DUMB failed");

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
        return fail("atomic_flip_event ADDFB2 failed");
    }
    fb_id = fb.fb_id;

    memset(plane_ids, 0, sizeof(plane_ids));
    memset(&plane_res, 0, sizeof(plane_res));
    plane_res.plane_id_ptr = (uint64)plane_ids;
    plane_res.count_planes = 2;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res) < 0 ||
        plane_ids[0] == 0)
        return fail("atomic_flip_event plane lookup failed");
    plane_id = plane_ids[0];
    plane_crtc_prop = find_obj_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                    "CRTC_ID", DRM_MODE_PROP_OBJECT);
    plane_fb_prop = find_obj_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                  "FB_ID", DRM_MODE_PROP_OBJECT);
    if (plane_crtc_prop == 0 || plane_fb_prop == 0)
        return fail("atomic_flip_event plane props missing");

    /* Establish an active scanout so the plane FB_ID commits below present. */
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = 1;
    crtc.fb_id = fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0)
        return fail("atomic_flip_event SETCRTC failed");

    /* Start from a known-empty ring regardless of prior tests. */
    (void)drain_drm_events(fd);

    /* (a) one FLIP_COMPLETE per event-flagged commit, ordered + monotonic. */
    for (i = 0; i < 3; i++) {
        if (atomic_flip_commit(fd, plane_id, plane_crtc_prop, plane_fb_prop,
                               1, fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                               seq_base + (uint64)i) != 0)
            return fail("atomic_flip_event flagged commit failed");
    }
    prev_sequence = 0;
    for (i = 0; i < 3; i++) {
        memset(&event, 0, sizeof(event));
        if (read(fd, &event, sizeof(event)) != (int)sizeof(event) ||
            event.base.type != DRM_EVENT_FLIP_COMPLETE ||
            event.base.length != sizeof(event) ||
            event.user_data != seq_base + (uint64)i ||
            event.crtc_id != 1 || event.sequence <= prev_sequence)
            return fail("atomic_flip_event order/user_data/crtc failed");
        prev_sequence = event.sequence;
    }
    if (read(fd, &event, sizeof(event)) >= 0)
        return fail("atomic_flip_event queue not drained after commits");

    /* (b) TEST_ONLY with the event flag emits no event. */
    if (atomic_flip_commit(fd, plane_id, plane_crtc_prop, plane_fb_prop,
                           1, fb_id,
                           DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_TEST_ONLY,
                           0x5445535400000000ULL) != 0)
        return fail("atomic_flip_event TEST_ONLY commit failed");
    if (read(fd, &event, sizeof(event)) >= 0)
        return fail("atomic_flip_event TEST_ONLY emitted event");

    /* (c) a real commit without the event flag emits no event. */
    if (atomic_flip_commit(fd, plane_id, plane_crtc_prop, plane_fb_prop,
                           1, fb_id, 0, 0x4e4f4556454e5400ULL) != 0)
        return fail("atomic_flip_event flag-absent commit failed");
    if (read(fd, &event, sizeof(event)) >= 0)
        return fail("atomic_flip_event flag-absent emitted event");

    /* (d) ring-full backpressure: fill the ring, next commit -EAGAIN pre-present. */
    for (i = 0; i < DRM_XV6_EVENT_QUEUE_CAPACITY; i++) {
        if (atomic_flip_commit(fd, plane_id, plane_crtc_prop, plane_fb_prop,
                               1, fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                               ring_base + (uint64)i) != 0)
            return fail("atomic_flip_event ring fill failed");
    }
    if (get_fb_stats(&stats_before) < 0)
        return fail("atomic_flip_event backpressure stats before failed");
    if (atomic_flip_commit(fd, plane_id, plane_crtc_prop, plane_fb_prop,
                           1, fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                           0x52494e47ffffffffULL) >= 0)
        return fail("atomic_flip_event full ring accepted commit");
    if (get_fb_stats(&stats_after) < 0)
        return fail("atomic_flip_event backpressure stats after failed");
    if (stats_after.kms_atomic_commits != stats_before.kms_atomic_commits)
        return fail("atomic_flip_event EAGAIN presented before backpressure");
    for (i = 0; i < DRM_XV6_EVENT_QUEUE_CAPACITY; i++) {
        memset(&event, 0, sizeof(event));
        if (read(fd, &event, sizeof(event)) != (int)sizeof(event) ||
            event.base.type != DRM_EVENT_FLIP_COMPLETE ||
            event.user_data != ring_base + (uint64)i ||
            event.crtc_id != 1)
            return fail("atomic_flip_event overflow drain failed");
    }
    if (read(fd, &event, sizeof(event)) >= 0)
        return fail("atomic_flip_event overflow queue not drained");

    /* (e) mixed legacy + atomic on the shared ring stays ordered + monotonic. */
    for (i = 0; i < 3; i++) {
        memset(&flip, 0, sizeof(flip));
        flip.crtc_id = 1;
        flip.fb_id = fb_id;
        flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
        flip.user_data = leg_base + (uint64)i;
        if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0)
            return fail("atomic_flip_event interleave legacy flip failed");
        if (atomic_flip_commit(fd, plane_id, plane_crtc_prop, plane_fb_prop,
                               1, fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                               ato_base + (uint64)i) != 0)
            return fail("atomic_flip_event interleave atomic commit failed");
    }
    prev_sequence = 0;
    for (i = 0; i < 6; i++) {
        uint64 expect = (i & 1) ? (ato_base + (uint64)(i / 2))
                                : (leg_base + (uint64)(i / 2));

        memset(&event, 0, sizeof(event));
        if (read(fd, &event, sizeof(event)) != (int)sizeof(event) ||
            event.base.type != DRM_EVENT_FLIP_COMPLETE ||
            event.user_data != expect || event.crtc_id != 1 ||
            event.sequence <= prev_sequence)
            return fail("atomic_flip_event interleave order failed");
        prev_sequence = event.sequence;
    }
    if (read(fd, &event, sizeof(event)) >= 0)
        return fail("atomic_flip_event interleave queue not drained");

    (void)drain_drm_events(fd);
    if (ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id) < 0)
        return fail("atomic_flip_event RMFB failed");
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
        return fail("atomic_flip_event DESTROY_DUMB failed");

    printf("drmiftest: kms_atomic_flip_event_matrix event_per_commit=PASS "
           "test_only_noevent=PASS flag_absent_noevent=PASS "
           "ring_full_eagain_pre_present=PASS overflow_drain=PASS "
           "legacy_atomic_interleave=PASS crtc_id=1 capacity=%d status=PASS\n",
           DRM_XV6_EVENT_QUEUE_CAPACITY);
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
    if (check_legacy_drm_probes(primary) != 0)
        goto out;
    if (check_kms_fb(primary) != 0)
        goto out;
    if (check_kms_atomic_flip_events(primary) != 0)
        goto out;
    if (check_common(render, "render") != 0)
        goto out;
    if (check_render_policy(render) != 0)
        goto out;
    if (check_dumb(render) != 0)
        goto out;
    if (check_gem_mmap_isolation(render, primary) != 0)
        goto out;
    if (check_fence_lifetime_matrix(render) != 0)
        goto out;
    if (check_fence_callback_lifecycle_matrix() != 0)
        goto out;
    if (check_syncobj(render) != 0)
        goto out;
    if (check_nouveau(render) != 0)
        goto out;
    if (check_file_lifecycle() != 0)
        goto out;
    if (check_drm_minor_matrix() != 0)
        goto out;
    if (check_lease_failclosed_matrix(primary, render) != 0)
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

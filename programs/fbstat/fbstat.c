#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/uabi/drm.h"
#include "kernel/inc/uabi/fcntl.h"
#include "user/user.h"

static int parse_mode(const char *s, uint32 *w, uint32 *h)
{
    uint32 x = 0;
    uint32 y = 0;

    while (*s >= '0' && *s <= '9')
        x = x * 10 + (uint32)(*s++ - '0');
    if (*s != 'x' && *s != 'X')
        return -1;
    s++;
    while (*s >= '0' && *s <= '9')
        y = y * 10 + (uint32)(*s++ - '0');
    if (*s != '\0' || x == 0 || y == 0)
        return -1;
    *w = x;
    *h = y;
    return 0;
}

static int print_probe(void);

static const char *backend_name(uint32 backend)
{
    switch (backend) {
    case FB_GPU_BACKEND_VIRGL:
        return "virgl";
    case FB_GPU_BACKEND_HYPERV_DXG:
        return "hyperv-dxg";
    case FB_GPU_BACKEND_DUMB:
    default:
        return "dumb";
    }
}

struct named_bit {
    uint64 bit;
    const char *name;
};

static void print_named_mask(const char *key, uint64 value,
                             const struct named_bit *bits, uint32 count)
{
    int printed = 0;

    printf("%s", key);
    if (value == 0) {
        printf(" none\n");
        return;
    }
    for (uint32 i = 0; i < count; i++) {
        if ((value & bits[i].bit) == 0)
            continue;
        printf("%s%s", printed ? "," : " ", bits[i].name);
        printed = 1;
    }
    if (!printed)
        printf(" unknown");
    printf("\n");
}

static const char *adapter_identity_name(uint64 value)
{
    switch (value) {
    case FB_GPU_DXG_PRESENT_ADAPTER_UNKNOWN:
        return "unknown";
    case FB_GPU_DXG_PRESENT_ADAPTER_UNVERIFIED:
        return "unverified";
    case FB_GPU_DXG_PRESENT_ADAPTER_MATCH:
        return "match";
    case FB_GPU_DXG_PRESENT_ADAPTER_MISMATCH:
        return "mismatch";
    default:
        return "unknown_value";
    }
}

static const char *present_lane_name(uint64 value)
{
    switch (value) {
    case FB_GPU_DXG_PRESENT_LANE_NONE:
        return "none";
    case FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND:
        return "gpup_dxg_scanout_bind";
    case FB_GPU_DXG_PRESENT_LANE_DDA_NOUVEAU:
        return "dda_nouveau";
    default:
        return "unknown_value";
    }
}

static const char *display_target_kind_name(uint64 value)
{
    switch (value) {
    case FB_GPU_DXG_DISPLAY_TARGET_NONE:
        return "none";
    case FB_GPU_DXG_DISPLAY_TARGET_SYNTHVID_VRAM_D3D12_EXISTING_SYSMEM:
        return "synthvid_vram_d3d12_existing_sysmem";
    case FB_GPU_DXG_DISPLAY_TARGET_RUNTIME_D3D12_RESOURCE:
        return "runtime_d3d12_resource";
    case FB_GPU_DXG_DISPLAY_TARGET_DDA_NOUVEAU_SCANOUT:
        return "dda_nouveau_scanout";
    default:
        return "unknown_value";
    }
}

static int print_mode(void)
{
    return print_probe();
}

static void print_refresh(uint32 millihz)
{
    if (millihz == 0) {
        printf("unknown");
        return;
    }
    printf("%u.%03uHz", millihz / 1000, millihz % 1000);
}

static int print_probe(void)
{
    struct fb_var_screeninfo info;
    struct fb_gpu_display_probe probe;
    int fd = open("/dev/fb0", O_RDONLY);

    if (fd < 0) {
        fprintf(2, "fbstat: open /dev/fb0 failed\n");
        return 1;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) < 0) {
        fprintf(2, "fbstat: FBIOGET_VSCREENINFO failed\n");
        close(fd);
        return 1;
    }
    memset(&probe, 0, sizeof(probe));
    if (ioctl(fd, FB_GPU_DISPLAY_PROBE, &probe) < 0) {
        fprintf(2, "fbstat: FB_GPU_DISPLAY_PROBE failed\n");
        close(fd);
        return 1;
    }
    printf("current %ux%u pitch %u bpp %u refresh ",
           info.xres, info.yres, info.pitch, info.bits_per_pixel);
    print_refresh(probe.current_refresh_millihz);
    printf("\n");
    if (probe.flags & FB_GPU_DISPLAY_F_EDID) {
        printf("preferred_edid %ux%u@", probe.preferred_width,
               probe.preferred_height);
        print_refresh(probe.preferred_refresh_millihz);
        printf("\n");
    }
    if (probe.flags & FB_GPU_DISPLAY_F_HOST_SCANOUT) {
        printf("host_scanout_raw %ux%u", probe.host_width, probe.host_height);
        if (probe.flags & FB_GPU_DISPLAY_F_HOST_SCALED)
            printf(" scaled-suspect");
        printf("\n");
    }
    close(fd);
    return 0;
}

static int set_mode(const char *mode)
{
    struct fb_var_screeninfo info;
    uint32 w, h;
    int fd;

    if (parse_mode(mode, &w, &h) != 0) {
        fprintf(2, "usage: fbstat [mode|probe|<width>x<height>]\n");
        return 1;
    }

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        fprintf(2, "fbstat: open /dev/fb0 failed\n");
        return 1;
    }
    memset(&info, 0, sizeof(info));
    info.xres = w;
    info.yres = h;
    info.bits_per_pixel = 32;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &info) < 0) {
        fprintf(2, "fbstat: FBIOPUT_VSCREENINFO %ux%u failed\n", w, h);
        close(fd);
        return 1;
    }
    close(fd);
    return print_mode();
}

static int drm_cap_value(int fd, uint64 cap, uint64 *value)
{
    struct drm_get_cap_compat req;

    memset(&req, 0, sizeof(req));
    req.capability = cap;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &req) < 0)
        return -1;
    *value = req.value;
    return 0;
}

static void print_drm_node_diag(const char *path, const char *node)
{
    char name[32];
    char desc[96];
    char unique[96];
    struct drm_version_compat ver;
    struct drm_unique_compat uniq;
    struct drm_client_compat client;
    struct drm_auth_compat magic;
    uint64 dumb = 0;
    uint64 prime = 0;
    uint64 syncobj = 0;
    uint64 timeline = 0;
    int fd;
    int auth_after = -1;
    int master_after = 0;

    fd = open(path, O_RDWR);
    if (fd < 0) {
        printf("drm_node %s path=%s open=failed\n", node, path);
        return;
    }

    memset(name, 0, sizeof(name));
    memset(desc, 0, sizeof(desc));
    memset(unique, 0, sizeof(unique));
    memset(&ver, 0, sizeof(ver));
    ver.name_len = sizeof(name);
    ver.name = (uint64)name;
    ver.desc_len = sizeof(desc);
    ver.desc = (uint64)desc;
    if (ioctl(fd, DRM_IOCTL_VERSION, &ver) < 0)
        strcpy(name, "unknown");

    memset(&uniq, 0, sizeof(uniq));
    uniq.unique_len = sizeof(unique);
    uniq.unique = (uint64)unique;
    if (ioctl(fd, DRM_IOCTL_GET_UNIQUE, &uniq) < 0)
        strcpy(unique, "unknown");

    memset(&client, 0, sizeof(client));
    if (ioctl(fd, DRM_IOCTL_GET_CLIENT, &client) < 0)
        memset(&client, 0, sizeof(client));

    (void)drm_cap_value(fd, DRM_CAP_DUMB_BUFFER, &dumb);
    (void)drm_cap_value(fd, DRM_CAP_PRIME, &prime);
    (void)drm_cap_value(fd, DRM_CAP_SYNCOBJ, &syncobj);
    (void)drm_cap_value(fd, DRM_CAP_SYNCOBJ_TIMELINE, &timeline);

    printf("drm_node %s path=%s driver=%s unique=%s auth=%d magic=%lu iocs=%lu\n",
           node, path, name, unique, client.auth, client.magic, client.iocs);
    printf("drm_caps %s dumb=%lu prime=0x%lx syncobj=%lu timeline=%lu\n",
           node, dumb, prime, syncobj, timeline);

    memset(&magic, 0, sizeof(magic));
    if (strcmp(node, "primary") == 0 &&
        ioctl(fd, DRM_IOCTL_GET_MAGIC, &magic) == 0 &&
        ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &magic) == 0 &&
        ioctl(fd, DRM_IOCTL_SET_MASTER, 0) == 0) {
        memset(&client, 0, sizeof(client));
        if (ioctl(fd, DRM_IOCTL_GET_CLIENT, &client) == 0)
            auth_after = client.auth;
        master_after = 1;
        (void)ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
    } else if (strcmp(node, "render") == 0) {
        int magic_rejected = ioctl(fd, DRM_IOCTL_GET_MAGIC, &magic) < 0;
        int master_rejected = ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0;
        printf("drm_node_policy render magic_rejected=%d master_rejected=%d\n",
               magic_rejected, master_rejected);
    }

    if (strcmp(node, "primary") == 0)
        printf("drm_node_state primary auth_after=%d master_after=%d\n",
               auth_after, master_after);
    close(fd);
}

int main(int argc, char *argv[])
{
    static const struct named_bit present_provenance_bits[] = {
        { FB_GPU_DXG_PRESENT_PROV_DXG_FD, "dxg_fd" },
        { FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD, "resource_fd" },
        { FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES, "d3dkmt_handles" },
        { FB_GPU_DXG_PRESENT_PROV_DIMENSIONS, "dimensions" },
        { FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID, "adapter_luid" },
    };
    static const struct named_bit helper_block_bits[] = {
        { FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT, "no_transport" },
        { FB_GPU_DXG_PRESENT_BLOCK_SYNTHVID_GPA_ONLY,
          "synthvid_gpa_only" },
        { FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND,
          "dxg_no_display_bind" },
        { FB_GPU_DXG_PRESENT_BLOCK_LUID_UNVERIFIED, "luid_unverified" },
        { FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE,
          "no_registered_source" },
        { FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED,
          "resource_fd_unverified" },
        { FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH,
          "adapter_mismatch" },
        { FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION, "no_completion" },
        { FB_GPU_DXG_PRESENT_BLOCK_DDA_NO_IMPORT_PATH,
          "dda_no_import_path" },
    };
    static const struct named_bit required_metadata_bits[] = {
        { FB_GPU_DXG_PRESENT_META_DEVICE, "device" },
        { FB_GPU_DXG_PRESENT_META_RESOURCE, "resource" },
        { FB_GPU_DXG_PRESENT_META_ALLOCATION, "allocation" },
        { FB_GPU_DXG_PRESENT_META_DIMENSIONS, "dimensions" },
        { FB_GPU_DXG_PRESENT_META_FORMAT, "format" },
        { FB_GPU_DXG_PRESENT_META_MODIFIER, "modifier" },
        { FB_GPU_DXG_PRESENT_META_SYNC_OBJECT, "sync_object" },
        { FB_GPU_DXG_PRESENT_META_FENCE_VALUE, "fence_value" },
        { FB_GPU_DXG_PRESENT_META_ADAPTER_LUID, "adapter_luid" },
    };
    static const struct named_bit helper_lifetime_bits[] = {
        { FB_GPU_DXG_PRESENT_LIFE_SOURCE_REGISTERED,
          "source_registered" },
        { FB_GPU_DXG_PRESENT_LIFE_HANDLES_VALID, "handles_valid" },
        { FB_GPU_DXG_PRESENT_LIFE_SYNC_VALID, "sync_valid" },
        { FB_GPU_DXG_PRESENT_LIFE_HOST_COMPLETION, "host_completion" },
        { FB_GPU_DXG_PRESENT_LIFE_NO_CPU_READBACK, "no_cpu_readback" },
    };
    static const struct named_bit host_candidate_bits[] = {
        { FB_GPU_DXG_PRESENT_HOST_SYNTHVID, "synthvid" },
        { FB_GPU_DXG_PRESENT_HOST_DXG, "dxg" },
        { FB_GPU_DXG_PRESENT_HOST_DDA_NOUVEAU, "dda_nouveau" },
    };
    static const struct named_bit host_reject_bits[] = {
        { FB_GPU_DXG_PRESENT_REJECT_SYNTHVID_GPA_ONLY,
          "synthvid_gpa_only" },
        { FB_GPU_DXG_PRESENT_REJECT_DXG_NO_DISPLAY_BIND,
          "dxg_no_display_bind" },
        { FB_GPU_DXG_PRESENT_REJECT_DDA_ABSENT, "dda_absent" },
        { FB_GPU_DXG_PRESENT_REJECT_DDA_NO_IMPORT_PATH,
          "dda_no_import_path" },
    };
    static const struct named_bit dxg_state_bits[] = {
        { FB_GPU_DXG_STATE_GLOBAL_PRESENT, "global_present" },
        { FB_GPU_DXG_STATE_GLOBAL_OPEN, "global_open" },
        { FB_GPU_DXG_STATE_VGPU_PRESENT, "vgpu_present" },
        { FB_GPU_DXG_STATE_VGPU_OPEN, "vgpu_open" },
        { FB_GPU_DXG_STATE_D3DKMT_READY, "d3dkmt_ready" },
        { FB_GPU_DXG_STATE_PARAVIRTUALIZED, "paravirtualized" },
        { FB_GPU_DXG_STATE_NO_DISPLAY, "no_display" },
        { FB_GPU_DXG_STATE_NO_SOURCES, "no_sources" },
    };
    struct fb_gpu_stats stats;
    struct fb_gpu_backend_info backend;
    int have_backend = 0;
    int fd;

    if (argc == 2) {
        if (strcmp(argv[1], "mode") == 0)
            return print_mode();
        if (strcmp(argv[1], "probe") == 0)
            return print_probe();
        return set_mode(argv[1]);
    }

    fd = open("/dev/gpu0", O_RDONLY);
    if (fd < 0) {
        fd = open("/dev/fb0", O_RDONLY);
        if (fd < 0) {
            fprintf(2, "fbstat: open /dev/gpu0 and /dev/fb0 failed\n");
            return 1;
        }
    }
    if (ioctl(fd, FB_GPU_GET_STATS, &stats) < 0) {
        fprintf(2, "fbstat: FB_GPU_GET_STATS failed\n");
        close(fd);
        return 1;
    }
    memset(&backend, 0, sizeof(backend));
    if (ioctl(fd, FB_GPU_BACKEND_QUERY, &backend) == 0)
        have_backend = 1;

    if (have_backend) {
        printf("backend %s flags 0x%x renderer %s\n",
               backend.name[0] ? backend.name : backend_name(backend.backend),
               backend.flags,
               backend.renderer[0] ? backend.renderer : "unknown");
        printf("backend_id %u\n", backend.backend);
        printf("backend_opengl_submit %u\n",
               (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0);
        printf("backend_opengl_submit_gate %s\n",
               (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0 ?
               "open" : "closed");
        printf("backend_virgl_opengl %u\n",
               (backend.flags & FB_GPU_BACKEND_F_VIRGL_OPENGL) != 0);
        printf("backend_dxg_transport %u\n",
               (backend.flags & FB_GPU_BACKEND_F_DXG_TRANSPORT) != 0);
        printf("backend_d3dkmt %u\n",
               (backend.flags & FB_GPU_BACKEND_F_D3DKMT) != 0);
        printf("backend_dda_nouveau %u\n",
               (backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) != 0);
        printf("dxg_global_open %u\n", backend.dxg_global_open);
        printf("dxg_vgpu_open %u\n", backend.dxg_vgpu_open);
        printf("dxg_global_status %u\n", backend.dxg_global_status);
        printf("dxg_vgpu_status %u\n", backend.dxg_vgpu_status);
    }
    print_drm_node_diag("/dev/dri/card0", "primary");
    print_drm_node_diag("/dev/dri/renderD128", "render");
    printf("full_blits %lu\n", stats.full_blits);
    printf("partial_blits %lu\n", stats.partial_blits);
    printf("clipped_blits %lu\n", stats.clipped_blits);
    printf("rejected_blits %lu\n", stats.rejected_blits);
    printf("fill_rects %lu\n", stats.fill_rects);
    printf("copy_rects %lu\n", stats.copy_rects);
    printf("blit_bytes %lu\n", stats.blit_bytes);
    printf("bo_allocs %lu\n", stats.bo_allocs);
    printf("bo_bytes %lu\n", stats.bo_bytes);
    printf("bo_presents %lu\n", stats.bo_presents);
    printf("bo_handles %lu\n", stats.bo_handles);
    printf("bo_live_bytes %lu\n", stats.bo_live_bytes);
    printf("bo_peak_handles %lu\n", stats.bo_peak_handles);
    printf("bo_peak_bytes %lu\n", stats.bo_peak_bytes);
    printf("bo_imports %lu\n", stats.bo_imports);
    printf("bo_fd_exports %lu\n", stats.bo_fd_exports);
    printf("bo_fd_imports %lu\n", stats.bo_fd_imports);
    printf("bo_fd_live %lu\n", stats.bo_fd_live);
    printf("bo_fd_peak %lu\n", stats.bo_fd_peak);
    printf("dmabuf_exports %lu\n", stats.dmabuf_exports);
    printf("dmabuf_imports %lu\n", stats.dmabuf_imports);
    printf("dmabuf_attachments %lu\n", stats.dmabuf_attachments);
    printf("dmabuf_live_attachments %lu\n",
           stats.dmabuf_live_attachments);
    printf("dmabuf_peak_attachments %lu\n",
           stats.dmabuf_peak_attachments);
    printf("dmabuf_live %lu\n", stats.dmabuf_live);
    printf("dmabuf_peak %lu\n", stats.dmabuf_peak);
    printf("dmabuf_releases %lu\n", stats.dmabuf_releases);
    printf("dmabuf_bad_fd_rejects %lu\n", stats.dmabuf_bad_fd_rejects);
    printf("dmabuf_foreign_fd_rejects %lu\n",
           stats.dmabuf_foreign_fd_rejects);
    printf("dmabuf_resv_snapshots %lu\n", stats.dmabuf_resv_snapshots);
    printf("dmabuf_last_exporter_tag %lu\n",
           stats.dmabuf_last_exporter_tag);
    printf("dmabuf_last_importer_tag %lu\n",
           stats.dmabuf_last_importer_tag);
    printf("dmabuf_last_ttm_resv_seq %lu\n",
           stats.dmabuf_last_ttm_resv_seq);
    printf("dmabuf_last_ttm_resv_exclusive_fence %lu\n",
           stats.dmabuf_last_ttm_resv_exclusive_fence);
    printf("dmabuf_poll_semantics %lu\n", stats.dmabuf_poll_semantics);
    printf("dmabuf_poll_attempts %lu\n", stats.dmabuf_poll_attempts);
    printf("dmabuf_poll_ready %lu\n", stats.dmabuf_poll_ready);
    printf("dmabuf_poll_not_ready %lu\n", stats.dmabuf_poll_not_ready);
    printf("dmabuf_poll_errors %lu\n", stats.dmabuf_poll_errors);
    printf("dmabuf_poll_last_target_fence %lu\n",
           stats.dmabuf_poll_last_target_fence);
    printf("dmabuf_poll_last_signaled_fence %lu\n",
           stats.dmabuf_poll_last_signaled_fence);
    printf("dmabuf_poll_read_ready %lu\n",
           stats.dmabuf_poll_read_ready);
    printf("dmabuf_poll_write_ready %lu\n",
           stats.dmabuf_poll_write_ready);
    printf("dmabuf_poll_pending %lu\n", stats.dmabuf_poll_pending);
    printf("dmabuf_poll_last_read_fence %lu\n",
           stats.dmabuf_poll_last_read_fence);
    printf("dmabuf_poll_last_write_fence %lu\n",
           stats.dmabuf_poll_last_write_fence);
    printf("dmabuf_poll_real_fence_ready %lu\n",
           stats.dmabuf_poll_real_fence_ready);
    printf("dmabuf_poll_callbacks_armed %lu\n",
           stats.dmabuf_poll_callbacks_armed);
    printf("dmabuf_poll_callbacks_fired %lu\n",
           stats.dmabuf_poll_callbacks_fired);
    printf("dmabuf_poll_pending_to_ready %lu\n",
           stats.dmabuf_poll_pending_to_ready);
    printf("dmabuf_poll_last_callback_source %lu\n",
           stats.dmabuf_poll_last_callback_source);
    printf("dmabuf_poll_last_callback_target_fence %lu\n",
           stats.dmabuf_poll_last_callback_target_fence);
    printf("dmabuf_poll_last_callback_wakeup_seq %lu\n",
           stats.dmabuf_poll_last_callback_wakeup_seq);
    printf("dmabuf_shared_fence_semantics %lu\n",
           stats.dmabuf_shared_fence_semantics);
    printf("dmabuf_wait_queue_semantics %lu\n",
           stats.dmabuf_wait_queue_semantics);
    printf("dmabuf_last_ttm_resv_shared_fence %lu\n",
           stats.dmabuf_last_ttm_resv_shared_fence);
    printf("dmabuf_last_ttm_resv_shared_count %lu\n",
           stats.dmabuf_last_ttm_resv_shared_count);
    printf("dmabuf_resv_matrix stats snapshots=%lu last_seq=%lu "
           "last_exclusive_fence=%lu shared_fence_semantics=%lu "
           "wait_queue_semantics=%lu poll_semantics=%lu "
           "last_shared_fence=%lu last_shared_count=%lu\n",
           stats.dmabuf_resv_snapshots,
           stats.dmabuf_last_ttm_resv_seq,
           stats.dmabuf_last_ttm_resv_exclusive_fence,
           stats.dmabuf_shared_fence_semantics,
           stats.dmabuf_wait_queue_semantics,
           stats.dmabuf_poll_semantics,
           stats.dmabuf_last_ttm_resv_shared_fence,
           stats.dmabuf_last_ttm_resv_shared_count);
    printf("dmabuf_poll_readiness_matrix stats attempts=%lu ready=%lu "
           "not_ready=%lu read_ready=%lu write_ready=%lu pending=%lu "
           "errors=%lu target_fence=%lu read_fence=%lu write_fence=%lu "
           "signaled_fence=%lu real_fence_ready=%lu callback_armed=%lu "
           "callback_fired=%lu pending_to_ready=%lu wake_source=%lu "
           "wake_target=%lu wake_seq=%lu\n",
           stats.dmabuf_poll_attempts,
           stats.dmabuf_poll_ready,
           stats.dmabuf_poll_not_ready,
           stats.dmabuf_poll_read_ready,
           stats.dmabuf_poll_write_ready,
           stats.dmabuf_poll_pending,
           stats.dmabuf_poll_errors,
           stats.dmabuf_poll_last_target_fence,
           stats.dmabuf_poll_last_read_fence,
           stats.dmabuf_poll_last_write_fence,
           stats.dmabuf_poll_last_signaled_fence,
           stats.dmabuf_poll_real_fence_ready,
           stats.dmabuf_poll_callbacks_armed,
           stats.dmabuf_poll_callbacks_fired,
           stats.dmabuf_poll_pending_to_ready,
           stats.dmabuf_poll_last_callback_source,
           stats.dmabuf_poll_last_callback_target_fence,
           stats.dmabuf_poll_last_callback_wakeup_seq);
    printf("bo_fences %lu\n", stats.bo_fences);
    printf("bo_fence_waits %lu\n", stats.bo_fence_waits);
    printf("fence_fd_exports %lu\n", stats.fence_fd_exports);
    printf("fence_fd_queries %lu\n", stats.fence_fd_queries);
    printf("fence_fd_live %lu\n", stats.fence_fd_live);
    printf("fence_fd_peak %lu\n", stats.fence_fd_peak);
    printf("fence_fd_polls %lu\n", stats.fence_fd_polls);
    printf("fence_fd_poll_ready %lu\n", stats.fence_fd_poll_ready);
    printf("fence_objects_callbacks_added %lu\n",
           stats.fence_objects_callbacks_added);
    printf("fence_objects_callbacks_removed %lu\n",
           stats.fence_objects_callbacks_removed);
    printf("fence_objects_callbacks_fired %lu\n",
           stats.fence_objects_callbacks_fired);
    printf("fence_objects_callbacks_late %lu\n",
           stats.fence_objects_callbacks_late);
    printf("fence_objects_callback_errors %lu\n",
           stats.fence_objects_callback_errors);
    printf("fence_wait_wakeup_stats bo_fence_waits=%lu "
           "fence_fd_queries=%lu fence_fd_polls=%lu "
           "fence_fd_poll_ready=%lu fence_fd_live=%lu "
           "callbacks=%lu/%lu/%lu late=%lu errors=%lu\n",
           stats.bo_fence_waits,
           stats.fence_fd_queries,
           stats.fence_fd_polls,
           stats.fence_fd_poll_ready,
           stats.fence_fd_live,
           stats.fence_objects_callbacks_added,
           stats.fence_objects_callbacks_fired,
           stats.fence_objects_callbacks_removed,
           stats.fence_objects_callbacks_late,
           stats.fence_objects_callback_errors);
    printf("gpu_opens %lu\n", stats.gpu_opens);
    printf("gpu_live_opens %lu\n", stats.gpu_live_opens);
    printf("gpu_ioctls %lu\n", stats.gpu_ioctls);
    printf("drm_primary_opens %lu\n", stats.drm_primary_opens);
    printf("drm_render_opens %lu\n", stats.drm_render_opens);
    printf("drm_primary_live %lu\n", stats.drm_primary_live);
    printf("drm_render_live %lu\n", stats.drm_render_live);
    printf("drm_ioctls %lu\n", stats.drm_ioctls);
    printf("drm_unknown_ioctls %lu\n", stats.drm_unknown_ioctls);
    printf("drm_auths %lu\n", stats.drm_auths);
    printf("drm_master_sets %lu\n", stats.drm_master_sets);
    printf("drm_master_drops %lu\n", stats.drm_master_drops);
    printf("drm_events_queued %lu\n", stats.drm_events_queued);
    printf("drm_events_read %lu\n", stats.drm_events_read);
    printf("drm_event_queue_depth %lu\n",
           stats.drm_event_queue_depth);
    printf("drm_event_queue_high_water %lu\n",
           stats.drm_event_queue_high_water);
    printf("drm_event_file_high_water %lu\n",
           stats.drm_event_file_high_water);
    printf("drm_event_queue_overflows %lu\n",
           stats.drm_event_queue_overflows);
    printf("drm_event_queue_dropped %lu\n",
           stats.drm_event_queue_dropped);
    printf("drm_event_close_stale %lu\n",
           stats.drm_event_close_stale);
    printf("drm_file_legacy_opens %lu\n", stats.drm_file_legacy_opens);
    printf("drm_file_primary_opens %lu\n", stats.drm_file_primary_opens);
    printf("drm_file_render_opens %lu\n", stats.drm_file_render_opens);
    printf("drm_file_legacy_closes %lu\n", stats.drm_file_legacy_closes);
    printf("drm_file_primary_closes %lu\n", stats.drm_file_primary_closes);
    printf("drm_file_render_closes %lu\n", stats.drm_file_render_closes);
    printf("drm_file_legacy_live %lu\n", stats.drm_file_legacy_live);
    printf("drm_file_primary_live %lu\n", stats.drm_file_primary_live);
    printf("drm_file_render_live %lu\n", stats.drm_file_render_live);
    printf("drm_file_close_generation %lu\n",
           stats.drm_file_close_generation);
    printf("drm_file_stale_gem_handles %lu\n",
           stats.drm_file_stale_gem_handles);
    printf("drm_file_stale_kms_fbs %lu\n",
           stats.drm_file_stale_kms_fbs);
    printf("drm_file_stale_syncobjs %lu\n",
           stats.drm_file_stale_syncobjs);
    printf("drm_file_stale_events %lu\n",
           stats.drm_file_stale_events);
    printf("drm_minor_model_version %lu\n",
           stats.drm_minor_model_version);
    printf("drm_minor_primary_index %lu\n",
           stats.drm_minor_primary_index);
    printf("drm_minor_render_index %lu\n",
           stats.drm_minor_render_index);
    printf("drm_minor_control_index %lu\n",
           stats.drm_minor_control_index);
    printf("drm_minor_primary_registered %lu\n",
           stats.drm_minor_primary_registered);
    printf("drm_minor_render_registered %lu\n",
           stats.drm_minor_render_registered);
    printf("drm_minor_control_registered %lu\n",
           stats.drm_minor_control_registered);
    printf("drm_minor_static_nodes %lu\n",
           stats.drm_minor_static_nodes);
    printf("drm_minor_dynamic_nodes %lu\n",
           stats.drm_minor_dynamic_nodes);
    printf("drm_minor_generation %lu\n",
           stats.drm_minor_generation);
    printf("drm_minor_matrix stats version=%lu primary=%lu/%lu "
           "render=%lu/%lu control=%lu/%lu static=%lu dynamic=%lu "
           "generation=%lu\n",
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
    printf("drm_lease_ioctl_attempts %lu\n",
           stats.drm_lease_ioctl_attempts);
    printf("drm_lease_create_rejects %lu\n",
           stats.drm_lease_create_rejects);
    printf("drm_lease_create_object_rejects %lu\n",
           stats.drm_lease_create_object_rejects);
    printf("drm_lease_create_empty_rejects %lu\n",
           stats.drm_lease_create_empty_rejects);
    printf("drm_lease_list_rejects %lu\n",
           stats.drm_lease_list_rejects);
    printf("drm_lease_get_rejects %lu\n",
           stats.drm_lease_get_rejects);
    printf("drm_lease_revoke_rejects %lu\n",
           stats.drm_lease_revoke_rejects);
    printf("drm_lease_render_rejects %lu\n",
           stats.drm_lease_render_rejects);
    printf("drm_lease_fds_created %lu\n",
           stats.drm_lease_fds_created);
    printf("drm_lease_active %lu\n", stats.drm_lease_active);
    printf("drm_lease_failclosed_matrix stats attempts=%lu create=%lu "
           "objects=%lu empty=%lu list=%lu get=%lu revoke=%lu render=%lu "
           "fds=%lu active=%lu\n",
           stats.drm_lease_ioctl_attempts,
           stats.drm_lease_create_rejects,
           stats.drm_lease_create_object_rejects,
           stats.drm_lease_create_empty_rejects,
           stats.drm_lease_list_rejects,
           stats.drm_lease_get_rejects,
           stats.drm_lease_revoke_rejects,
           stats.drm_lease_render_rejects,
           stats.drm_lease_fds_created,
           stats.drm_lease_active);
    printf("nouveau_ioctl_entries %lu\n", stats.nouveau_ioctl_entries);
    printf("nouveau_fail_closed %lu\n", stats.nouveau_fail_closed);
    printf("nouveau_getparams %lu\n", stats.nouveau_getparams);
    printf("nouveau_getparam_dda_facts %lu\n",
           stats.nouveau_getparam_dda_facts);
    printf("nouveau_getparam_synthetic_facts %lu\n",
           stats.nouveau_getparam_synthetic_facts);
    printf("nouveau_getparam_fail_closed %lu\n",
           stats.nouveau_getparam_fail_closed);
    printf("nouveau_getparam_last_source %lu\n",
           stats.nouveau_getparam_last_source);
    printf("nouveau_channel_allocs %lu\n", stats.nouveau_channel_allocs);
    printf("nouveau_channel_frees %lu\n", stats.nouveau_channel_frees);
    printf("nouveau_gem_news %lu\n", stats.nouveau_gem_news);
    printf("nouveau_gem_infos %lu\n", stats.nouveau_gem_infos);
    printf("nouveau_cpu_preps %lu\n", stats.nouveau_cpu_preps);
    printf("nouveau_cpu_finis %lu\n", stats.nouveau_cpu_finis);
    printf("nouveau_vm_inits %lu\n", stats.nouveau_vm_inits);
    printf("nouveau_vm_bind_noops %lu\n", stats.nouveau_vm_bind_noops);
    printf("nouveau_pushbuf_noops %lu\n", stats.nouveau_pushbuf_noops);
    printf("nouveau_exec_noops %lu\n", stats.nouveau_exec_noops);
    printf("nouveau_unsupported %lu\n", stats.nouveau_unsupported);
    printf("nouveau_pci_registered %lu\n", stats.nouveau_pci_registered);
    printf("nouveau_pci_probes %lu\n", stats.nouveau_pci_probes);
    printf("nouveau_pci_probe_failures %lu\n",
           stats.nouveau_pci_probe_failures);
    printf("nouveau_pci_probe_reject_dxg_present %lu\n",
           stats.nouveau_pci_probe_reject_dxg_present);
    printf("nouveau_pci_probe_reject_class %lu\n",
           stats.nouveau_pci_probe_reject_class);
    printf("nouveau_pci_probe_reject_no_bars %lu\n",
           stats.nouveau_pci_probe_reject_no_bars);
    printf("nouveau_pci_probe_enable_failures %lu\n",
           stats.nouveau_pci_probe_enable_failures);
    printf("nouveau_pci_probe_accepts %lu\n",
           stats.nouveau_pci_probe_accepts);
    printf("nouveau_pci_removes %lu\n", stats.nouveau_pci_removes);
    printf("nouveau_pci_suspends %lu\n", stats.nouveau_pci_suspends);
    printf("nouveau_pci_resumes %lu\n", stats.nouveau_pci_resumes);
    printf("nouveau_pci_enable_count %lu\n",
           stats.nouveau_pci_enable_count);
    printf("nouveau_pci_master_enabled %lu\n",
           stats.nouveau_pci_master_enabled);
    printf("nouveau_pci_irq_vectors %lu\n",
           stats.nouveau_pci_irq_vectors);
    printf("nouveau_pci_runtime_suspended %lu\n",
           stats.nouveau_pci_runtime_suspended);
    printf("nouveau_pci_suspend_count %lu\n",
           stats.nouveau_pci_suspend_count);
    printf("nouveau_pci_resume_count %lu\n", stats.nouveau_pci_resume_count);
    printf("nouveau_pci_runtime_pm_balanced %lu\n",
           stats.nouveau_pci_runtime_pm_balanced);
    printf("nouveau_pci_remove_runtime_suspended %lu\n",
           stats.nouveau_pci_remove_runtime_suspended);
    printf("nouveau_pci_bar0_len %lu\n", stats.nouveau_pci_bar0_len);
    printf("nouveau_pci_bar1_len %lu\n", stats.nouveau_pci_bar1_len);
    printf("nouveau_pci_irq %lu\n", stats.nouveau_pci_irq);
    printf("nouveau_pci_irq_pin %lu\n", stats.nouveau_pci_irq_pin);
    printf("nouveau_pci_msi_cap %lu\n", stats.nouveau_pci_msi_cap);
    printf("nouveau_pci_msix_cap %lu\n", stats.nouveau_pci_msix_cap);
    printf("nouveau_pci_dma_mask_configured %lu\n",
           stats.nouveau_pci_dma_mask_configured);
    printf("nouveau_pci_dma_mask_bits %lu\n",
           stats.nouveau_pci_dma_mask_bits);
    printf("nouveau_pci_coherent_dma_mask_configured %lu\n",
           stats.nouveau_pci_coherent_dma_mask_configured);
    printf("nouveau_pci_coherent_dma_mask_bits %lu\n",
           stats.nouveau_pci_coherent_dma_mask_bits);
    printf("nouveau_pci_bar0_claimed %lu\n",
           stats.nouveau_pci_bar0_claimed);
    printf("nouveau_pci_bar1_claimed %lu\n",
           stats.nouveau_pci_bar1_claimed);
    printf("nouveau_pci_bar_claim_failures %lu\n",
           stats.nouveau_pci_bar_claim_failures);
    printf("nouveau_pci_bar_releases %lu\n",
           stats.nouveau_pci_bar_releases);
    printf("nouveau_pci_irq_request_failures %lu\n",
           stats.nouveau_pci_irq_request_failures);
    printf("nouveau_pci_irq_mode %lu\n", stats.nouveau_pci_irq_mode);
    printf("nouveau_pci_msi_requested %lu\n",
           stats.nouveau_pci_msi_requested);
    printf("nouveau_pci_msi_fail_closed %lu\n",
           stats.nouveau_pci_msi_fail_closed);
    printf("nouveau_pci_irq_vector_valid %lu\n",
           stats.nouveau_pci_irq_vector_valid);
    printf("nouveau_pci_irq_handler_registered %lu\n",
           stats.nouveau_pci_irq_handler_registered);
    printf("nouveau_pci_irq_delivery_enabled %lu\n",
           stats.nouveau_pci_irq_delivery_enabled);
    printf("nouveau_pci_irq_delivery_claimed %lu\n",
           stats.nouveau_pci_irq_delivery_claimed);
    printf("nouveau_pci_legacy_irq_fallback %lu\n",
           stats.nouveau_pci_legacy_irq_fallback);
    printf("nouveau_pci_native_present_credit %lu\n",
           stats.nouveau_pci_native_present_credit);
    printf("nouveau_pci_dma_resource_matrix stats "
           "registered=%lu accepts=%lu reject_dxg_present=%lu "
           "reject_no_bars=%lu dma_mask_configured=%lu "
           "dma_mask_bits=%lu coherent_configured=%lu "
           "coherent_bits=%lu bar0_len=%lu bar1_len=%lu "
           "bar0_claimed=%lu bar1_claimed=%lu claim_failures=%lu "
           "releases=%lu irq_mode=%lu irq_failures=%lu "
           "msi_requested=%lu msi_fail_closed=%lu "
           "irq_vector_valid=%lu irq_handler_registered=%lu "
           "irq_delivery_enabled=%lu irq_delivery_claimed=%lu "
           "legacy_irq_fallback=%lu "
           "suspend_count=%lu resume_count=%lu pm_balanced=%lu "
           "remove_while_suspended=%lu "
           "native_present_credit=%lu\n",
           stats.nouveau_pci_registered,
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_probe_reject_dxg_present,
           stats.nouveau_pci_probe_reject_no_bars,
           stats.nouveau_pci_dma_mask_configured,
           stats.nouveau_pci_dma_mask_bits,
           stats.nouveau_pci_coherent_dma_mask_configured,
           stats.nouveau_pci_coherent_dma_mask_bits,
           stats.nouveau_pci_bar0_len,
           stats.nouveau_pci_bar1_len,
           stats.nouveau_pci_bar0_claimed,
           stats.nouveau_pci_bar1_claimed,
           stats.nouveau_pci_bar_claim_failures,
           stats.nouveau_pci_bar_releases,
           stats.nouveau_pci_irq_mode,
           stats.nouveau_pci_irq_request_failures,
           stats.nouveau_pci_msi_requested,
           stats.nouveau_pci_msi_fail_closed,
           stats.nouveau_pci_irq_vector_valid,
           stats.nouveau_pci_irq_handler_registered,
           stats.nouveau_pci_irq_delivery_enabled,
           stats.nouveau_pci_irq_delivery_claimed,
           stats.nouveau_pci_legacy_irq_fallback,
           stats.nouveau_pci_suspend_count,
           stats.nouveau_pci_resume_count,
           stats.nouveau_pci_runtime_pm_balanced,
           stats.nouveau_pci_remove_runtime_suspended,
           stats.nouveau_pci_native_present_credit);
    printf("nouveau_getparam_provenance_matrix stats "
           "getparams=%lu dda_facts=%lu synthetic_facts=%lu "
           "fail_closed=%lu last_source=%lu accepts=%lu "
           "native_present_credit=%lu\n",
           stats.nouveau_getparams,
           stats.nouveau_getparam_dda_facts,
           stats.nouveau_getparam_synthetic_facts,
           stats.nouveau_getparam_fail_closed,
           stats.nouveau_getparam_last_source,
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_native_present_credit);
    printf("kms_framebuffers %lu\n", stats.kms_framebuffers);
    printf("kms_page_flips %lu\n", stats.kms_page_flips);
    printf("kms_page_flip_target_rejects %lu\n",
           stats.kms_page_flip_target_rejects);
    printf("kms_page_flip_async_rejects %lu\n",
           stats.kms_page_flip_async_rejects);
    printf("kms_page_flip_invalid_noevent_rejects %lu\n",
           stats.kms_page_flip_invalid_noevent_rejects);
    printf("kms_crtc_queue_sequence_rejects %lu\n",
           stats.kms_crtc_queue_sequence_rejects);
    printf("kms_crtc_queue_sequence_bad_flags %lu\n",
           stats.kms_crtc_queue_sequence_bad_flags);
    printf("kms_crtc_queue_sequence_noevent_rejects %lu\n",
           stats.kms_crtc_queue_sequence_noevent_rejects);
    printf("kms_atomic_commits %lu\n", stats.kms_atomic_commits);
    printf("kms_atomic_in_fence_accepted %lu\n",
           stats.kms_atomic_in_fence_accepted);
    printf("kms_atomic_in_fence_rejected %lu\n",
           stats.kms_atomic_in_fence_rejected);
    printf("kms_atomic_in_fence_fd_refs %lu\n",
           stats.kms_atomic_in_fence_fd_refs);
    printf("kms_atomic_in_fence_fd_ref_puts %lu\n",
           stats.kms_atomic_in_fence_fd_ref_puts);
    printf("kms_atomic_in_fence_duplicate_rejects %lu\n",
           stats.kms_atomic_in_fence_duplicate_rejects);
    printf("kms_atomic_in_fence_test_only_validated %lu\n",
           stats.kms_atomic_in_fence_test_only_validated);
    printf("kms_atomic_in_fence_test_only_waits %lu\n",
           stats.kms_atomic_in_fence_test_only_waits);
    printf("kms_atomic_in_fence_sync_file_pending_waits %lu\n",
           stats.kms_atomic_in_fence_sync_file_pending_waits);
    printf("kms_atomic_in_fence_sync_file_pending_wakeups %lu\n",
           stats.kms_atomic_in_fence_sync_file_pending_wakeups);
    printf("kms_atomic_out_fence_prepared %lu\n",
           stats.kms_atomic_out_fence_prepared);
    printf("kms_atomic_out_fence_cleanup_closes %lu\n",
           stats.kms_atomic_out_fence_cleanup_closes);
    printf("kms_atomic_out_fence_test_only_placeholders %lu\n",
           stats.kms_atomic_out_fence_test_only_placeholders);
    printf("kms_atomic_out_fence_fd_exports %lu\n",
           stats.kms_atomic_out_fence_fd_exports);
    printf("kms_atomic_out_fence_display_correlated %lu\n",
           stats.kms_atomic_out_fence_display_correlated);
    printf("kms_atomic_fence_matrix stats fd_refs=%lu fd_ref_puts=%lu "
           "duplicate_rejects=%lu test_only_validated=%lu "
           "test_only_waits=%lu sync_file_pending_waits=%lu "
           "sync_file_pending_wakeups=%lu out_fence_prepared=%lu "
           "out_fence_cleanup_closes=%lu "
           "test_only_out_fence_placeholders=%lu out_fence_exports=%lu "
           "out_fence_display_correlated=%lu\n",
           stats.kms_atomic_in_fence_fd_refs,
           stats.kms_atomic_in_fence_fd_ref_puts,
           stats.kms_atomic_in_fence_duplicate_rejects,
           stats.kms_atomic_in_fence_test_only_validated,
           stats.kms_atomic_in_fence_test_only_waits,
           stats.kms_atomic_in_fence_sync_file_pending_waits,
           stats.kms_atomic_in_fence_sync_file_pending_wakeups,
           stats.kms_atomic_out_fence_prepared,
           stats.kms_atomic_out_fence_cleanup_closes,
           stats.kms_atomic_out_fence_test_only_placeholders,
           stats.kms_atomic_out_fence_fd_exports,
           stats.kms_atomic_out_fence_display_correlated);
    printf("ttm_system_bytes %lu\n", stats.ttm_system_bytes);
    printf("ttm_tt_bytes %lu\n", stats.ttm_tt_bytes);
    printf("ttm_vram_bytes %lu\n", stats.ttm_vram_bytes);
    printf("ttm_stolen_bytes %lu\n", stats.ttm_stolen_bytes);
    printf("ttm_pinned_bytes %lu\n", stats.ttm_pinned_bytes);
    printf("ttm_validate_failures %lu\n", stats.ttm_validate_failures);
    printf("ttm_metadata_only_moves %lu\n", stats.ttm_metadata_only_moves);
    printf("ttm_real_copy_moves %lu\n", stats.ttm_real_copy_moves);
    printf("ttm_move_bytes %lu\n", stats.ttm_move_bytes);
    printf("ttm_native_accel_credit %lu\n",
           stats.ttm_native_accel_credit);
    printf("ttm_move_path_matrix stats aggregate_metadata_only=%lu "
           "aggregate_real_copy=%lu aggregate_move_bytes=%lu "
           "aggregate_native_accel_credit=%lu "
           "cpu_copy_by_domain=%lu/%lu/%lu/%lu "
           "metadata_noop_by_domain=%lu/%lu/%lu/%lu "
           "unsupported_hw_copy_by_domain=%lu/%lu/%lu/%lu "
           "real_copy_by_domain=%lu/%lu/%lu/%lu "
           "per_domain_fields=present\n",
           stats.ttm_metadata_only_moves,
           stats.ttm_real_copy_moves,
           stats.ttm_move_bytes,
           stats.ttm_native_accel_credit,
           stats.ttm_cpu_copy_fallback_moves[0],
           stats.ttm_cpu_copy_fallback_moves[1],
           stats.ttm_cpu_copy_fallback_moves[2],
           stats.ttm_cpu_copy_fallback_moves[3],
           stats.ttm_metadata_noop_moves[0],
           stats.ttm_metadata_noop_moves[1],
           stats.ttm_metadata_noop_moves[2],
           stats.ttm_metadata_noop_moves[3],
           stats.ttm_unsupported_hw_copy_moves[0],
           stats.ttm_unsupported_hw_copy_moves[1],
           stats.ttm_unsupported_hw_copy_moves[2],
           stats.ttm_unsupported_hw_copy_moves[3],
           stats.ttm_real_copy_moves_by_domain[0],
           stats.ttm_real_copy_moves_by_domain[1],
           stats.ttm_real_copy_moves_by_domain[2],
           stats.ttm_real_copy_moves_by_domain[3]);
    printf("ttm_resv_acquires %lu\n", stats.ttm_resv_acquires);
    printf("ttm_resv_releases %lu\n", stats.ttm_resv_releases);
    printf("ttm_resv_waits %lu\n", stats.ttm_resv_waits);
    printf("ttm_resv_conflicts %lu\n", stats.ttm_resv_conflicts);
    printf("ttm_resv_exclusive_fences %lu\n",
           stats.ttm_resv_exclusive_fences);
    printf("ttm_resv_shared_slots %lu\n", stats.ttm_resv_shared_slots);
    printf("ttm_resv_shared_used %lu\n", stats.ttm_resv_shared_used);
    printf("ttm_resv_shared_fences %lu\n", stats.ttm_resv_shared_fences);
    printf("ttm_resv_shared_replaced %lu\n",
           stats.ttm_resv_shared_replaced);
    printf("ttm_resv_wait_queued %lu\n", stats.ttm_resv_wait_queued);
    printf("ttm_resv_wait_wakeups %lu\n", stats.ttm_resv_wait_wakeups);
    printf("ttm_resv_stale_fence_rejects %lu\n",
           stats.ttm_resv_stale_fence_rejects);
    printf("ttm_resv_attach_prime_export %lu\n",
           stats.ttm_resv_attach_prime_export);
    printf("ttm_resv_attach_prime_import %lu\n",
           stats.ttm_resv_attach_prime_import);
    printf("ttm_resv_attach_dmabuf_export %lu\n",
           stats.ttm_resv_attach_dmabuf_export);
    printf("ttm_resv_attach_dmabuf_import %lu\n",
           stats.ttm_resv_attach_dmabuf_import);
    printf("ttm_resv_attach_kms_pin %lu\n",
           stats.ttm_resv_attach_kms_pin);
    printf("ttm_resv_attach_kms_unpin %lu\n",
           stats.ttm_resv_attach_kms_unpin);
    printf("ttm_resv_attach_syncobj_signal %lu\n",
           stats.ttm_resv_attach_syncobj_signal);
    printf("ttm_resv_attach_syncobj_wait %lu\n",
           stats.ttm_resv_attach_syncobj_wait);
    printf("ttm_resv_attach_sync_file_export %lu\n",
           stats.ttm_resv_attach_sync_file_export);
    printf("ttm_resv_attach_sync_file_import %lu\n",
           stats.ttm_resv_attach_sync_file_import);
    printf("ttm_resv_last_attach_point %lu\n",
           stats.ttm_resv_last_attach_point);
    printf("ttm_resv_last_shared_fence %lu\n",
           stats.ttm_resv_last_shared_fence);
    printf("ttm_resv_evict_pinned_rejects %lu\n",
           stats.ttm_resv_evict_pinned_rejects);
    printf("ttm_resv_evict_busy_rejects %lu\n",
           stats.ttm_resv_evict_busy_rejects);
    printf("ttm_resv_wait_matrix stats acquires=%lu releases=%lu "
           "waits=%lu conflicts=%lu exclusive_fences=%lu "
           "shared_slots=%lu shared_used=%lu shared_fences=%lu "
           "wait_queued=%lu wait_wakeups=%lu stale_fence_rejects=%lu "
           "attach_prime_export=%lu attach_prime_import=%lu "
           "attach_dmabuf_export=%lu attach_dmabuf_import=%lu "
           "attach_kms_pin=%lu attach_kms_unpin=%lu "
           "attach_syncobj_signal=%lu attach_syncobj_wait=%lu "
           "attach_sync_file_export=%lu attach_sync_file_import=%lu "
           "last_attach_point=%lu last_shared_fence=%lu "
           "evict_pinned_rejects=%lu evict_busy_rejects=%lu\n",
           stats.ttm_resv_acquires,
           stats.ttm_resv_releases,
           stats.ttm_resv_waits,
           stats.ttm_resv_conflicts,
           stats.ttm_resv_exclusive_fences,
           stats.ttm_resv_shared_slots,
           stats.ttm_resv_shared_used,
           stats.ttm_resv_shared_fences,
           stats.ttm_resv_wait_queued,
           stats.ttm_resv_wait_wakeups,
           stats.ttm_resv_stale_fence_rejects,
           stats.ttm_resv_attach_prime_export,
           stats.ttm_resv_attach_prime_import,
           stats.ttm_resv_attach_dmabuf_export,
           stats.ttm_resv_attach_dmabuf_import,
           stats.ttm_resv_attach_kms_pin,
           stats.ttm_resv_attach_kms_unpin,
           stats.ttm_resv_attach_syncobj_signal,
           stats.ttm_resv_attach_syncobj_wait,
           stats.ttm_resv_attach_sync_file_export,
           stats.ttm_resv_attach_sync_file_import,
           stats.ttm_resv_last_attach_point,
           stats.ttm_resv_last_shared_fence,
           stats.ttm_resv_evict_pinned_rejects,
           stats.ttm_resv_evict_busy_rejects);
    printf("syncobj_created %lu\n", stats.syncobj_created);
    printf("syncobj_live %lu\n", stats.syncobj_live);
    printf("syncobj_signals %lu\n", stats.syncobj_signals);
    printf("syncobj_waits %lu\n", stats.syncobj_waits);
    printf("syncobj_resv_attach %lu\n", stats.syncobj_resv_attach);
    printf("syncobj_sync_file_exports %lu\n",
           stats.syncobj_sync_file_exports);
    printf("syncobj_sync_file_imports %lu\n",
           stats.syncobj_sync_file_imports);
    printf("syncobj_wait_queued %lu\n", stats.syncobj_wait_queued);
    printf("syncobj_wait_wakeups %lu\n", stats.syncobj_wait_wakeups);
    printf("syncobj_wait_callbacks_armed %lu\n",
           stats.syncobj_wait_callbacks_armed);
    printf("syncobj_wait_callbacks_fired %lu\n",
           stats.syncobj_wait_callbacks_fired);
    printf("syncobj_wait_callbacks_cancelled %lu\n",
           stats.syncobj_wait_callbacks_cancelled);
    printf("syncobj_wait_callback_late_fires %lu\n",
           stats.syncobj_wait_callback_late_fires);
    printf("syncobj_timeout_waits %lu\n", stats.syncobj_timeout_waits);
    printf("syncobj_stale_wait_rejects %lu\n",
           stats.syncobj_stale_wait_rejects);
    printf("sync_file_pending_exports %lu\n",
           stats.sync_file_pending_exports);
    printf("sync_file_pending_imports %lu\n",
           stats.sync_file_pending_imports);
    printf("sync_file_pending_poll_not_ready %lu\n",
           stats.sync_file_pending_poll_not_ready);
    printf("sync_file_pending_poll_ready %lu\n",
           stats.sync_file_pending_poll_ready);
    printf("sync_file_pending_wakeups %lu\n",
           stats.sync_file_pending_wakeups);
    printf("sync_file_pending_import_rejects %lu\n",
           stats.sync_file_pending_import_rejects);
    printf("sync_file_pending_callbacks_armed %lu\n",
           stats.sync_file_pending_callbacks_armed);
    printf("sync_file_pending_callbacks_fired %lu\n",
           stats.sync_file_pending_callbacks_fired);
    printf("sync_file_pending_callbacks_cancelled %lu\n",
           stats.sync_file_pending_callbacks_cancelled);
    printf("sync_file_pending_callback_late_fires %lu\n",
           stats.sync_file_pending_callback_late_fires);
    printf("virtio_commands %lu\n", stats.virtio_commands);
    printf("virtio_failures %lu\n", stats.virtio_failures);
    printf("virtio_timeouts %lu\n", stats.virtio_timeouts);
    printf("virtio_resources %lu\n", stats.virtio_resources);
    printf("virtio_resource_bytes %lu\n", stats.virtio_resource_bytes);
    printf("virtio_transfers %lu\n", stats.virtio_transfers);
    printf("virtio_flushes %lu\n", stats.virtio_flushes);
    printf("virtio_scanouts %lu\n", stats.virtio_scanouts);
    printf("virtio_capsets %lu\n", stats.virtio_capsets);
    printf("virtio_virgl %lu\n", stats.virtio_virgl);
    printf("virtio_virgl_version %lu\n", stats.virtio_virgl_version);
    printf("virtio_virgl_size %lu\n", stats.virtio_virgl_size);
    printf("virtio_contexts %lu\n", stats.virtio_contexts);
    printf("virtio_context_failed %lu\n", stats.virtio_context_failed);
    printf("virtio_context_failures %lu\n", stats.virtio_context_failures);
    printf("virtio_submits %lu\n", stats.virtio_submits);
    printf("virtio_fences %lu\n", stats.virtio_fences);
    printf("virtio_last_fence %lu\n", stats.virtio_last_fence);
    printf("virtio_irq_completions %lu\n", stats.virtio_irq_completions);
    printf("virtio_poll_fallbacks %lu\n", stats.virtio_poll_fallbacks);
    printf("gpu_backend %lu\n", stats.gpu_backend);
    printf("gpu_backend_flags %lu\n", stats.gpu_backend_flags);
    printf("dxg_global_open_stat %lu\n", stats.dxg_global_open);
    printf("dxg_vgpu_open_stat %lu\n", stats.dxg_vgpu_open);
    printf("dxg_d3dkmt %lu\n", stats.dxg_d3dkmt);
    printf("dxg_global_rx %lu\n", stats.dxg_global_rx);
    printf("dxg_vgpu_rx %lu\n", stats.dxg_vgpu_rx);
    printf("dxg_present_register_ioctl_entries %lu\n",
           stats.dxg_present_register_ioctl_entries);
    printf("dxg_present_commit_ioctl_entries %lu\n",
           stats.dxg_present_commit_ioctl_entries);
    printf("dxg_present_query_ioctl_entries %lu\n",
           stats.dxg_present_query_ioctl_entries);
    printf("dxg_present_register_copyin_failures %lu\n",
           stats.dxg_present_register_copyin_failures);
    printf("dxg_present_commit_copyin_failures %lu\n",
           stats.dxg_present_commit_copyin_failures);
    printf("dxg_present_query_copyin_failures %lu\n",
           stats.dxg_present_query_copyin_failures);
    printf("dxg_present_query_copyout_failures %lu\n",
           stats.dxg_present_query_copyout_failures);
    printf("dxg_present_register_attempts %lu\n",
           stats.dxg_present_register_attempts);
    printf("dxg_present_register_successes %lu\n",
           stats.dxg_present_register_successes);
    printf("dxg_present_register_rejects %lu\n",
           stats.dxg_present_register_rejects);
    printf("dxg_present_commit_attempts %lu\n",
           stats.dxg_present_commit_attempts);
    printf("dxg_present_commit_rejects %lu\n",
           stats.dxg_present_commit_rejects);
    printf("dxg_present_query_attempts %lu\n",
           stats.dxg_present_query_attempts);
    printf("dxg_present_query_rejects %lu\n",
           stats.dxg_present_query_rejects);
    printf("dxg_present_host_handoff_missing %lu\n",
           stats.dxg_present_host_handoff_missing);
    printf("dxg_present_last_source %lu\n", stats.dxg_present_last_source);
    printf("dxg_present_last_ret %lu\n", stats.dxg_present_last_ret);
    printf("dxg_present_last_device 0x%lx\n", stats.dxg_present_last_device);
    printf("dxg_present_last_resource 0x%lx\n", stats.dxg_present_last_resource);
    printf("dxg_present_last_allocation 0x%lx\n",
           stats.dxg_present_last_allocation);
    printf("dxg_present_last_sync 0x%lx\n", stats.dxg_present_last_sync);
    printf("dxg_present_last_flags 0x%lx\n", stats.dxg_present_last_flags);
    printf("dxg_present_last_fence_value %lu\n",
           stats.dxg_present_last_fence_value);
    printf("dxg_present_last_width %lu\n", stats.dxg_present_last_width);
    printf("dxg_present_last_height %lu\n", stats.dxg_present_last_height);
    printf("dxg_present_last_pitch %lu\n", stats.dxg_present_last_pitch);
    printf("dxg_present_last_format 0x%lx\n", stats.dxg_present_last_format);
    printf("dxg_present_last_allocation_count %lu\n",
           stats.dxg_present_last_allocation_count);
    printf("dxg_present_last_dxg_fd %ld\n",
           (int64)stats.dxg_present_last_dxg_fd);
    printf("dxg_present_last_resource_fd %ld\n",
           (int64)stats.dxg_present_last_resource_fd);
    printf("dxg_present_last_provenance 0x%lx\n",
           stats.dxg_present_last_provenance);
    print_named_mask("dxg_present_last_provenance_names",
                     stats.dxg_present_last_provenance,
                     present_provenance_bits,
                     sizeof(present_provenance_bits) /
                     sizeof(present_provenance_bits[0]));
    printf("dxg_present_last_adapter_luid_low 0x%lx\n",
           stats.dxg_present_last_adapter_luid_low);
    printf("dxg_present_last_adapter_luid_high 0x%lx\n",
           stats.dxg_present_last_adapter_luid_high);
    printf("dxg_present_last_adapter_luid 0x%08lx:0x%08lx\n",
           stats.dxg_present_last_adapter_luid_high & 0xffffffffUL,
           stats.dxg_present_last_adapter_luid_low & 0xffffffffUL);
    printf("dxg_present_last_adapter_identity 0x%lx\n",
           stats.dxg_present_last_adapter_identity);
    printf("dxg_present_last_adapter_identity_name %s\n",
           adapter_identity_name(stats.dxg_present_last_adapter_identity));
    printf("dxg_present_selected_lane %lu\n",
           stats.dxg_present_selected_lane);
    printf("dxg_present_selected_lane_name %s\n",
           present_lane_name(stats.dxg_present_selected_lane));
    printf("dxg_present_gpu_p_or_dda_block_reason %lu\n",
           stats.dxg_present_helper_block_reason);
    print_named_mask("dxg_present_gpu_p_or_dda_block_reason_names",
                     stats.dxg_present_helper_block_reason,
                     helper_block_bits,
                     sizeof(helper_block_bits) /
                     sizeof(helper_block_bits[0]));
    printf("dxg_present_display_target_kind %lu\n",
           stats.dxg_present_display_target_kind);
    printf("dxg_present_display_target_kind_name %s\n",
           display_target_kind_name(stats.dxg_present_display_target_kind));
    printf("dxg_present_requires_host_protocol %lu\n",
           stats.dxg_present_requires_host_protocol);
    printf("dxg_present_missing_host_abi %lu\n",
           stats.dxg_present_missing_host_abi);
    printf("dxg_present_host_candidates 0x%lx\n",
           stats.dxg_present_host_candidates);
    print_named_mask("dxg_present_host_candidates_names",
                     stats.dxg_present_host_candidates,
                     host_candidate_bits,
                     sizeof(host_candidate_bits) /
                     sizeof(host_candidate_bits[0]));
    printf("dxg_present_host_rejects 0x%lx\n",
           stats.dxg_present_host_rejects);
    print_named_mask("dxg_present_host_rejects_names",
                     stats.dxg_present_host_rejects,
                     host_reject_bits,
                     sizeof(host_reject_bits) /
                     sizeof(host_reject_bits[0]));
    printf("dxg_present_synthvid_state 0x%lx\n",
           stats.dxg_present_synthvid_state);
    printf("dxg_present_synthvid_vram_gpa 0x%lx\n",
           stats.dxg_present_synthvid_vram_gpa);
    printf("dxg_present_dxg_state 0x%lx\n", stats.dxg_present_dxg_state);
    print_named_mask("dxg_present_dxg_state_names",
                     stats.dxg_present_dxg_state,
                     dxg_state_bits,
                     sizeof(dxg_state_bits) / sizeof(dxg_state_bits[0]));
    printf("dxg_present_dxg_adapter_type_raw 0x%lx\n",
           stats.dxg_present_dxg_adapter_type_raw);
    printf("dxg_present_dxg_adapter_type_wsl 0x%lx\n",
           stats.dxg_present_dxg_adapter_type_wsl);
    printf("dxg_present_dxg_adapter_type_rewrites %lu\n",
           stats.dxg_present_dxg_adapter_type_rewrites);
    printf("dxg_present_dxg_adapter_sources %lu\n",
           stats.dxg_present_dxg_adapter_sources);
    printf("dxg_present_dxg_adapter_render_supported %lu\n",
           stats.dxg_present_dxg_adapter_render_supported);
    printf("dxg_present_dxg_adapter_display_supported %lu\n",
           stats.dxg_present_dxg_adapter_display_supported);
    printf("dxg_present_dxg_adapter_paravirtualized %lu\n",
           stats.dxg_present_dxg_adapter_paravirtualized);
    printf("dxg_present_dxg_adapter_compute_only %lu\n",
           stats.dxg_present_dxg_adapter_compute_only);
    printf("dxg_present_dxg_adapter_sources_known %lu\n",
           stats.dxg_present_dxg_adapter_sources_known);
    printf("dxg_present_dxg_enum_adapter_count %lu\n",
           stats.dxg_present_dxg_enum_adapter_count);
    printf("dxg_present_dxg_enum_adapter_handle 0x%lx\n",
           stats.dxg_present_dxg_enum_adapter_handle);
    printf("dxg_present_dxg_enum_adapter_luid_low 0x%lx\n",
           stats.dxg_present_dxg_enum_adapter_luid_low);
    printf("dxg_present_dxg_enum_adapter_luid_high 0x%lx\n",
           stats.dxg_present_dxg_enum_adapter_luid_high);
    printf("dxg_present_dxg_enum_adapter_luid 0x%08lx:0x%08lx\n",
           stats.dxg_present_dxg_enum_adapter_luid_high & 0xffffffffUL,
           stats.dxg_present_dxg_enum_adapter_luid_low & 0xffffffffUL);
    printf("dxg_present_dxg_user_luid_low 0x%lx\n",
           stats.dxg_present_dxg_user_luid_low);
    printf("dxg_present_dxg_user_luid_high 0x%lx\n",
           stats.dxg_present_dxg_user_luid_high);
    printf("dxg_present_dxg_user_luid 0x%08lx:0x%08lx\n",
           stats.dxg_present_dxg_user_luid_high & 0xffffffffUL,
           stats.dxg_present_dxg_user_luid_low & 0xffffffffUL);
    printf("dxg_present_dda_nouveau_present %lu\n",
           stats.dxg_present_dda_nouveau_present);
    printf("dxg_present_dda_nouveau_import_path_present %lu\n",
           stats.dxg_present_dda_nouveau_import_path_present);
    printf("dxg_present_dda_nouveau_scanout_bind_present %lu\n",
           stats.dxg_present_dda_nouveau_scanout_bind_present);
    printf("dxg_present_gpu_p_or_dda_contract_version %lu\n",
           stats.dxg_present_helper_contract_version);
    printf("dxg_present_gpu_p_or_dda_required_metadata 0x%lx\n",
           stats.dxg_present_helper_required_metadata);
    print_named_mask("dxg_present_gpu_p_or_dda_required_metadata_names",
                     stats.dxg_present_helper_required_metadata,
                     required_metadata_bits,
                     sizeof(required_metadata_bits) /
                     sizeof(required_metadata_bits[0]));
    printf("dxg_present_gpu_p_or_dda_transport %lu\n",
           stats.dxg_present_helper_transport);
    printf("dxg_present_gpu_p_or_dda_transport_present %lu\n",
           stats.dxg_present_helper_transport_present);
    printf("dxg_present_gpu_p_or_dda_operation %lu\n",
           stats.dxg_present_helper_operation);
    printf("dxg_present_gpu_p_or_dda_lifetime 0x%lx\n",
           stats.dxg_present_helper_lifetime);
    print_named_mask("dxg_present_gpu_p_or_dda_lifetime_names",
                     stats.dxg_present_helper_lifetime,
                     helper_lifetime_bits,
                     sizeof(helper_lifetime_bits) /
                     sizeof(helper_lifetime_bits[0]));
    printf("dxg_present_gpu_p_or_dda_source_live %lu\n",
           stats.dxg_present_helper_source_live);
    printf("dxg_present_gpu_p_or_dda_requires_completion %lu\n",
           stats.dxg_present_helper_requires_completion);
    printf("dxg_present_bind_contract_queries %lu\n",
           stats.dxg_present_bind_contract_queries);
    printf("dxg_present_bind_contract_rejects %lu\n",
           stats.dxg_present_bind_contract_rejects);
    printf("dxg_present_bind_contract_successes %lu\n",
           stats.dxg_present_bind_contract_successes);
    printf("dxg_present_release_sources %lu\n",
           stats.dxg_present_release_sources);
    printf("display_presents %lu\n", stats.display_presents);
    printf("display_completions %lu\n", stats.display_completions);
    printf("display_last_present %lu\n", stats.display_last_present);
    printf("display_last_complete %lu\n", stats.display_last_complete);
    printf("dxg_present_completion_summary source=%lu lane=%s provenance=0x%lx adapter=%s display_present=%lu display_complete=%lu complete_ge_present=%u gpu_p_or_dda_block=0x%lx\n",
           stats.dxg_present_last_source,
           present_lane_name(stats.dxg_present_selected_lane),
           stats.dxg_present_last_provenance,
           adapter_identity_name(stats.dxg_present_last_adapter_identity),
           stats.display_last_present, stats.display_last_complete,
           stats.display_last_complete >= stats.display_last_present,
           stats.dxg_present_helper_block_reason);
    close(fd);
    return 0;
}

#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
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
    case FB_GPU_DXG_PRESENT_LANE_HELPER_SCANOUT_BIND:
        return "helper_scanout_bind";
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
    };
    static const struct named_bit host_reject_bits[] = {
        { FB_GPU_DXG_PRESENT_REJECT_SYNTHVID_GPA_ONLY,
          "synthvid_gpa_only" },
        { FB_GPU_DXG_PRESENT_REJECT_DXG_NO_DISPLAY_BIND,
          "dxg_no_display_bind" },
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
        printf("dxg_global_open %u\n", backend.dxg_global_open);
        printf("dxg_vgpu_open %u\n", backend.dxg_vgpu_open);
        printf("dxg_global_status %u\n", backend.dxg_global_status);
        printf("dxg_vgpu_status %u\n", backend.dxg_vgpu_status);
    }
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
    printf("bo_fences %lu\n", stats.bo_fences);
    printf("bo_fence_waits %lu\n", stats.bo_fence_waits);
    printf("fence_fd_exports %lu\n", stats.fence_fd_exports);
    printf("fence_fd_queries %lu\n", stats.fence_fd_queries);
    printf("fence_fd_live %lu\n", stats.fence_fd_live);
    printf("fence_fd_peak %lu\n", stats.fence_fd_peak);
    printf("fence_fd_polls %lu\n", stats.fence_fd_polls);
    printf("fence_fd_poll_ready %lu\n", stats.fence_fd_poll_ready);
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
    printf("kms_framebuffers %lu\n", stats.kms_framebuffers);
    printf("kms_page_flips %lu\n", stats.kms_page_flips);
    printf("kms_atomic_commits %lu\n", stats.kms_atomic_commits);
    printf("ttm_system_bytes %lu\n", stats.ttm_system_bytes);
    printf("ttm_tt_bytes %lu\n", stats.ttm_tt_bytes);
    printf("ttm_vram_bytes %lu\n", stats.ttm_vram_bytes);
    printf("ttm_stolen_bytes %lu\n", stats.ttm_stolen_bytes);
    printf("ttm_pinned_bytes %lu\n", stats.ttm_pinned_bytes);
    printf("ttm_validate_failures %lu\n", stats.ttm_validate_failures);
    printf("syncobj_created %lu\n", stats.syncobj_created);
    printf("syncobj_live %lu\n", stats.syncobj_live);
    printf("syncobj_signals %lu\n", stats.syncobj_signals);
    printf("syncobj_waits %lu\n", stats.syncobj_waits);
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
    printf("dxg_present_helper_block_reason %lu\n",
           stats.dxg_present_helper_block_reason);
    print_named_mask("dxg_present_helper_block_reason_names",
                     stats.dxg_present_helper_block_reason,
                     helper_block_bits,
                     sizeof(helper_block_bits) /
                     sizeof(helper_block_bits[0]));
    printf("dxg_present_display_target_kind %lu\n",
           stats.dxg_present_display_target_kind);
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
    printf("dxg_present_helper_contract_version %lu\n",
           stats.dxg_present_helper_contract_version);
    printf("dxg_present_helper_required_metadata 0x%lx\n",
           stats.dxg_present_helper_required_metadata);
    print_named_mask("dxg_present_helper_required_metadata_names",
                     stats.dxg_present_helper_required_metadata,
                     required_metadata_bits,
                     sizeof(required_metadata_bits) /
                     sizeof(required_metadata_bits[0]));
    printf("dxg_present_helper_transport %lu\n",
           stats.dxg_present_helper_transport);
    printf("dxg_present_helper_transport_present %lu\n",
           stats.dxg_present_helper_transport_present);
    printf("dxg_present_helper_operation %lu\n",
           stats.dxg_present_helper_operation);
    printf("dxg_present_helper_lifetime 0x%lx\n",
           stats.dxg_present_helper_lifetime);
    print_named_mask("dxg_present_helper_lifetime_names",
                     stats.dxg_present_helper_lifetime,
                     helper_lifetime_bits,
                     sizeof(helper_lifetime_bits) /
                     sizeof(helper_lifetime_bits[0]));
    printf("dxg_present_helper_source_live %lu\n",
           stats.dxg_present_helper_source_live);
    printf("dxg_present_helper_requires_completion %lu\n",
           stats.dxg_present_helper_requires_completion);
    printf("display_presents %lu\n", stats.display_presents);
    printf("display_completions %lu\n", stats.display_completions);
    printf("display_last_present %lu\n", stats.display_last_present);
    printf("display_last_complete %lu\n", stats.display_last_complete);
    printf("dxg_present_completion_summary source=%lu lane=%s provenance=0x%lx adapter=%s display_present=%lu display_complete=%lu complete_ge_present=%u helper_block=0x%lx\n",
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

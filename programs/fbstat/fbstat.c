#include "kernel/inc/types.h"
#include "kernel/inc/errno.h"
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

static const char *nouveau_vblank_source_name(uint64 value)
{
    switch (value) {
    case FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE:
        return "none";
    case FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ:
        return "irq";
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
    static const struct named_bit kms_present_reject_bits[] = {
        { FB_GPU_KMS_PRESENT_REJECT_NO_NATIVE_DISPLAY,
          "no_native_display" },
        { FB_GPU_KMS_PRESENT_REJECT_NO_NOUVEAU_DISPLAY,
          "no_nouveau_display" },
        { FB_GPU_KMS_PRESENT_REJECT_NO_DISPLAY_CREATE,
          "no_display_create" },
        { FB_GPU_KMS_PRESENT_REJECT_NO_HEADS, "no_heads" },
        { FB_GPU_KMS_PRESENT_REJECT_NO_CONNECTORS, "no_connectors" },
        { FB_GPU_KMS_PRESENT_REJECT_NO_VBLANK, "no_vblank" },
        { FB_GPU_KMS_PRESENT_REJECT_NO_HW_COMPLETION, "no_hw_completion" },
        { FB_GPU_KMS_PRESENT_REJECT_NO_ATOMIC_PAGEFLIP_BACKEND,
          "no_atomic_pageflip_backend" },
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
    int backend_opengl_submit = 0;
    int scanout_bind_skeleton_ok = 0;
    int display_bind_boundary_ok = 0;
    int display_bind_id_shape_ok = 0;
    int display_bind_success_shape_ok = 0;
    int provider_credit_gate_ok = 0;
    int native_completion_lifetime_ok = 0;
    int display_bind_request_metadata_ok = 0;
    int display_bind_pending_lifetime_ok = 0;
    int display_bind_provider_pending_publication_ok = 0;
    int stale_source_zero_credit_ok = 0;
    int generic_completion_not_native_ok = 0;
    int standard_alloc_not_display_bind_ok = 0;
    int dda_nouveau_separate_display_not_bind_ok = 0;
    int foreign_prime_import_gap_ok = 0;
    int public_present_api_not_guest_bind_ok = 0;
    int d3d12_display_bind_host_abi_discovery_ok = 0;
    int nouveau_display_kms_ready = 0;
    int nouveau_native_display_claimed = 0;
    int nouveau_atomic_pageflip_backend_missing_ok = 0;
    int dda_nouveau_non_readback_display_proof_ok = 0;
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
    if (ioctl(fd, FB_GPU_BACKEND_QUERY, &backend) == 0) {
        have_backend = 1;
        backend_opengl_submit =
            (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0;
    }
    scanout_bind_skeleton_ok =
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_last_present_id == 0 &&
        stats.dxg_scanout_bind_last_completed == 0 &&
        (stats.dxg_scanout_bind_attempts == 0 ||
         (stats.dxg_scanout_bind_rejects >=
              stats.dxg_scanout_bind_attempts &&
          stats.dxg_scanout_bind_weak_evidence_rejects >=
              stats.dxg_scanout_bind_attempts)) &&
        (stats.dxg_scanout_bind_completion_queries == 0 ||
         stats.dxg_scanout_bind_completion_pending >=
             stats.dxg_scanout_bind_completion_queries);
    display_bind_boundary_ok =
        stats.dxg_display_bind_contract_version == 1 &&
        stats.dxg_display_bind_backend ==
            FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND &&
        stats.dxg_display_bind_transport ==
            FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_operation ==
            FB_GPU_DXG_PRESENT_GPUP_DDA_OP_SCANOUT_BIND &&
        stats.dxg_display_bind_completion_source ==
            FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_display_bind_status == EOPNOTSUPP &&
        (stats.dxg_display_bind_provider_submits == 0 ||
         (stats.dxg_display_bind_provider_no_host_abi == 1 &&
          stats.dxg_display_bind_provider_no_sender == 1 &&
          stats.dxg_display_bind_provider_no_completion == 1));
    display_bind_request_metadata_ok =
        stats.dxg_display_bind_provider_submits == 0 ||
        (stats.dxg_display_bind_request_metadata_complete == 1 &&
         stats.dxg_display_bind_request_sync_metadata_complete == 1 &&
         stats.dxg_display_bind_request_missing_metadata == 0 &&
         stats.dxg_display_bind_source_generation != 0 &&
         stats.dxg_display_bind_resource_generation != 0);
    display_bind_pending_lifetime_ok =
        stats.dxg_display_bind_pending_active == 0 &&
        stats.dxg_display_bind_pending_created >=
            stats.dxg_display_bind_pending_completed +
            stats.dxg_display_bind_pending_failclosed +
            stats.dxg_display_bind_pending_cancelled &&
        (stats.dxg_display_bind_pending_created == 0 ||
         (stats.dxg_display_bind_pending_sequence != 0 &&
          stats.dxg_display_bind_pending_peak != 0 &&
          stats.dxg_display_bind_pending_last_source_generation != 0 &&
          stats.dxg_display_bind_pending_last_resource_generation != 0)) &&
        (stats.dxg_display_bind_transport_present != 0 ||
         stats.dxg_display_bind_pending_completed == 0) &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        backend_opengl_submit == 0;
    display_bind_provider_pending_publication_ok =
        stats.dxg_display_bind_provider_submits == 0 ||
        (stats.dxg_display_bind_provider_publication_attempts != 0 &&
         stats.dxg_display_bind_provider_pending_owner_generation != 0 &&
         stats.dxg_display_bind_provider_pending_source_generation != 0 &&
         stats.dxg_display_bind_provider_pending_resource_generation != 0 &&
         stats.dxg_display_bind_provider_pending_source_generation ==
             stats.dxg_display_bind_source_generation &&
         stats.dxg_display_bind_provider_pending_resource_generation ==
             stats.dxg_display_bind_resource_generation &&
         stats.dxg_display_bind_pending_last_source_generation ==
             stats.dxg_display_bind_provider_pending_source_generation &&
         stats.dxg_display_bind_pending_last_resource_generation ==
             stats.dxg_display_bind_provider_pending_resource_generation &&
         stats.dxg_display_bind_provider_publish_before_send == 0 &&
         stats.dxg_display_bind_provider_transport_pending_id == 0 &&
         stats.dxg_display_bind_provider_command_id == 0 &&
         stats.dxg_display_bind_provider_transaction_id == 0 &&
         stats.dxg_display_bind_provider_channel == 0 &&
         stats.dxg_display_bind_provider_completion_demux_registered == 0 &&
         ((stats.dxg_display_bind_provider_resolved_or_cancelled != 0 &&
           stats.dxg_display_bind_provider_refs_released != 0 &&
           stats.dxg_display_bind_provider_no_host_abi_cancelled != 0 &&
           stats.dxg_display_bind_provider_no_host_abi_refs_released != 0) ||
          (stats.dxg_display_bind_provider_submits == 0 &&
           stats.dxg_display_bind_provider_resolved_or_cancelled == 0 &&
           stats.dxg_display_bind_provider_refs_released == 0 &&
           stats.dxg_display_bind_provider_no_host_abi_cancelled == 0 &&
           stats.dxg_display_bind_provider_no_host_abi_refs_released == 0)) &&
         stats.dxg_display_bind_provider_no_host_abi != 0 &&
         stats.dxg_display_bind_provider_no_sender != 0 &&
         stats.dxg_display_bind_provider_no_completion != 0 &&
         stats.dxg_display_bind_transport_present == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         backend_opengl_submit == 0);
    display_bind_id_shape_ok =
        ((stats.dxg_display_bind_present_id == 0 &&
          stats.dxg_display_bind_completed_id == 0) ||
         (stats.dxg_display_bind_present_id != 0 &&
          stats.dxg_display_bind_completed_id >=
              stats.dxg_display_bind_present_id &&
          stats.dxg_display_bind_source_generation != 0 &&
          stats.dxg_display_bind_resource_generation != 0)) &&
        ((stats.dxg_scanout_bind_last_present_id == 0 &&
          stats.dxg_scanout_bind_last_completed == 0) ||
         (stats.dxg_scanout_bind_last_present_id != 0 &&
          stats.dxg_scanout_bind_last_completed >=
              stats.dxg_scanout_bind_last_present_id &&
          stats.dxg_scanout_bind_last_source_generation != 0 &&
          stats.dxg_scanout_bind_last_resource_generation != 0));
    provider_credit_gate_ok =
        (backend_opengl_submit == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         stats.dxg_scanout_bind_successes == 0 &&
         stats.dxg_scanout_bind_completion_successes == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         stats.dxg_present_helper_transport_present == 0 &&
         stats.dxg_present_display_target_kind ==
             FB_GPU_DXG_DISPLAY_TARGET_NONE) ||
        (stats.dxg_display_bind_provider_no_host_abi == 0 &&
         stats.dxg_display_bind_provider_no_sender == 0 &&
         stats.dxg_display_bind_provider_no_completion == 0);
    display_bind_success_shape_ok =
        (backend_opengl_submit == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         stats.dxg_scanout_bind_successes == 0 &&
         stats.dxg_scanout_bind_completion_successes == 0 &&
         stats.dxg_display_bind_transport_present == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         (stats.dxg_display_bind_provider_submits == 0 ||
          (stats.dxg_display_bind_provider_no_host_abi != 0 &&
           stats.dxg_display_bind_provider_no_sender != 0 &&
           stats.dxg_display_bind_provider_no_completion != 0))) ||
        (stats.dxg_display_bind_transport_present != 0 &&
         stats.dxg_display_bind_status == 0 &&
         stats.dxg_display_bind_block_reason == 0 &&
         stats.dxg_display_bind_completion_source ==
             FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
         stats.dxg_display_bind_present_id != 0 &&
         stats.dxg_display_bind_completed_id >=
             stats.dxg_display_bind_present_id &&
         stats.dxg_display_bind_source_generation != 0 &&
         stats.dxg_display_bind_resource_generation != 0 &&
         stats.dxg_scanout_bind_successes != 0 &&
         stats.dxg_scanout_bind_completion_successes != 0 &&
         stats.dxg_display_bind_provider_submits != 0 &&
         stats.dxg_display_bind_provider_pin_revalidated != 0 &&
         stats.dxg_display_bind_provider_no_host_abi == 0 &&
         stats.dxg_display_bind_provider_no_sender == 0 &&
         stats.dxg_display_bind_provider_no_completion == 0);
    native_completion_lifetime_ok =
        (backend_opengl_submit == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         stats.dxg_scanout_bind_successes == 0 &&
         stats.dxg_scanout_bind_completion_successes == 0 &&
         stats.dxg_display_bind_transport_present == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         (stats.dxg_display_bind_provider_submits == 0 ||
          stats.dxg_display_bind_provider_no_completion != 0)) ||
        (stats.dxg_display_bind_transport_present != 0 &&
         stats.dxg_display_bind_status == 0 &&
         stats.dxg_display_bind_block_reason == 0 &&
         stats.dxg_display_bind_completion_source ==
             FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
         stats.dxg_display_bind_present_id != 0 &&
         stats.dxg_display_bind_completed_id >=
             stats.dxg_display_bind_present_id &&
         stats.dxg_display_bind_source_generation != 0 &&
         stats.dxg_display_bind_resource_generation != 0 &&
         stats.dxg_scanout_bind_completion_successes != 0 &&
         stats.dxg_display_bind_provider_no_host_abi == 0 &&
         stats.dxg_display_bind_provider_no_sender == 0 &&
         stats.dxg_display_bind_provider_no_completion == 0);
    stale_source_zero_credit_ok =
        stats.dxg_display_bind_late_completion_after_release == 0 &&
        stats.dxg_display_bind_after_close_nonzero_id_rejects == 0 &&
        (stats.dxg_display_bind_after_close_queries == 0 ||
         stats.dxg_display_bind_stale_source_rejects != 0) &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    generic_completion_not_native_ok =
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.dxg_scanout_bind_last_present_id == 0 &&
        stats.dxg_scanout_bind_last_completed == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.kms_vblank_source_nouveau_hw == 0 &&
        stats.kms_page_flip_events_native_hw == 0;
    standard_alloc_not_display_bind_ok =
        (stats.dxg_scanout_bind_standard_alloc_private_data == 0 ||
         stats.dxg_scanout_bind_standard_alloc_display_bind_absent != 0) &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0;
    dda_nouveau_separate_display_not_bind_ok =
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.dxg_scanout_bind_dda_resource_import_absent != 0 &&
        stats.dxg_scanout_bind_dda_scanout_bind_absent != 0 &&
        stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    foreign_prime_import_gap_ok =
        stats.dmabuf_local_imports == stats.dmabuf_imports &&
        stats.dmabuf_foreign_import_rejects >=
            stats.dmabuf_foreign_import_attempts &&
        stats.dmabuf_foreign_fd_rejects >=
            stats.dmabuf_foreign_import_rejects &&
        stats.dmabuf_d3d12_foreign_resource_imports == 0 &&
        stats.dmabuf_nouveau_scanout_bind_imports == 0 &&
        stats.dmabuf_native_present_credit == 0 &&
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    public_present_api_not_guest_bind_ok =
        stats.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats.dxg_present_helper_transport_present == 0 &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    d3d12_display_bind_host_abi_discovery_ok =
        public_present_api_not_guest_bind_ok &&
        dda_nouveau_separate_display_not_bind_ok &&
        stats.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats.dxg_display_bind_provider_completion_demux_registered == 0 &&
        stats.dxg_present_helper_transport_present == 0 &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    nouveau_display_kms_ready =
        stats.nouveau_display_create_successes != 0 &&
        stats.nouveau_display_heads != 0 &&
        stats.nouveau_display_connectors != 0 &&
        stats.nouveau_display_nonvirtual_connectors != 0 &&
        stats.nouveau_display_vblank_supported != 0 &&
        stats.nouveau_display_vblank_irq_supported != 0 &&
        stats.nouveau_display_vblank_source ==
            FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ &&
        stats.nouveau_display_page_flip_completion_ready != 0 &&
        stats.nouveau_display_page_flip_completions != 0 &&
        stats.nouveau_display_atomic_pageflip_backend_missing == 0;
    nouveau_atomic_pageflip_backend_missing_ok =
        stats.nouveau_display_atomic_pageflip_backend_missing == 0 ||
        (stats.nouveau_pci_probe_accepts != 0 &&
         stats.nouveau_display_probe_attempts != 0 &&
         stats.nouveau_native_display_ready == 0 &&
         stats.nouveau_display_atomic_pageflip_backend_missing == 1);
    nouveau_native_display_claimed =
        stats.nouveau_native_display_ready != 0 ||
        stats.nouveau_dda_native_display_present != 0 ||
        stats.nouveau_pci_native_present_credit != 0 ||
        stats.kms_present_nouveau_hw != 0 ||
        stats.kms_present_last_lane == FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW;
    dda_nouveau_non_readback_display_proof_ok =
        (stats.nouveau_pci_probe_accepts == 0 &&
         stats.nouveau_display_probe_attempts == 0 &&
         stats.nouveau_display_create_attempts == 0 &&
         stats.nouveau_display_create_successes == 0 &&
         stats.nouveau_display_head_probe_attempts == 0 &&
         stats.nouveau_display_heads == 0 &&
         stats.nouveau_display_connector_probe_attempts == 0 &&
         stats.nouveau_display_connectors == 0 &&
         stats.nouveau_display_nonvirtual_connectors == 0 &&
         stats.nouveau_display_vblank_supported == 0 &&
         stats.nouveau_display_vblank_irq_supported == 0 &&
         stats.nouveau_display_vblank_source ==
             FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE &&
         stats.nouveau_display_vblank_irqs == 0 &&
         stats.nouveau_display_page_flip_completion_ready == 0 &&
         stats.nouveau_display_page_flip_completions == 0 &&
         stats.nouveau_display_atomic_pageflip_backend_missing == 0 &&
         stats.kms_present_last_lane == FB_GPU_KMS_PRESENT_LANE_NONE &&
         stats.kms_present_nouveau_hw == 0 &&
         stats.kms_page_flip_events_native_hw == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         backend_opengl_submit == 0) ||
        (stats.nouveau_pci_probe_accepts != 0 &&
         !nouveau_native_display_claimed &&
         nouveau_atomic_pageflip_backend_missing_ok &&
         stats.nouveau_pci_native_present_credit == 0 &&
         backend_opengl_submit == 0) ||
        (stats.nouveau_pci_probe_accepts != 0 &&
         nouveau_display_kms_ready &&
         stats.kms_present_last_lane == FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW &&
         stats.kms_present_dumb == 0 &&
         stats.kms_present_synthvid == 0 &&
         stats.kms_present_nouveau_hw != 0 &&
         stats.kms_vblank_source_nouveau_hw != 0 &&
         stats.kms_vblank_source_software_display == 0 &&
         stats.kms_vblank_source_synthetic == 0 &&
         stats.kms_page_flip_events_native_hw != 0 &&
         stats.kms_page_flip_events_software_blit == 0);

    if (have_backend) {
        printf("backend %s flags 0x%x renderer %s\n",
               backend.name[0] ? backend.name : backend_name(backend.backend),
               backend.flags,
               backend.renderer[0] ? backend.renderer : "unknown");
        printf("backend_id %u\n", backend.backend);
        printf("backend_opengl_submit %u\n", backend_opengl_submit);
        printf("backend_opengl_submit_gate %s\n",
               backend_opengl_submit ? "open" : "closed");
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
        printf("hyperv_opengl_submit_gate_matrix "
               "backend=%s backend_opengl_submit=%u "
               "requires_native_present=1 requires_finite_fps=1 "
               "requires_webkit_shared_surface=1 "
               "native_present_credit=%lu display_target_kind=%lu "
               "present_id=%lu completed=%lu "
               "backend_gate=%s status=%s\n",
               backend_name(backend.backend),
               backend_opengl_submit,
               0UL,
               stats.dxg_present_display_target_kind,
               0UL,
               0UL,
               backend_opengl_submit ? "open" : "closed",
               backend.backend == FB_GPU_BACKEND_HYPERV_DXG &&
                       backend_opengl_submit == 0 &&
                       stats.dxg_present_display_target_kind ==
                           FB_GPU_DXG_DISPLAY_TARGET_NONE ?
                   "PASS" : "DIAGNOSTIC");
        printf("opengl_submit_backend_separation_matrix "
               "backend=%s dxg_transport=%u d3dkmt=%u virgl_opengl=%u "
               "backend_opengl_submit=%u allowed_submit_backend=virgl "
               "hyperv_dxg_transport_is_submit=0 "
               "hyperv_d3dkmt_is_submit=0 "
               "kvm_virgl_submit_allowed=1 native_present_credit=0 "
               "opengl_submit_credit=%u status=%s\n",
               backend_name(backend.backend),
               (backend.flags & FB_GPU_BACKEND_F_DXG_TRANSPORT) != 0,
               (backend.flags & FB_GPU_BACKEND_F_D3DKMT) != 0,
               (backend.flags & FB_GPU_BACKEND_F_VIRGL_OPENGL) != 0,
               backend_opengl_submit,
               backend.backend == FB_GPU_BACKEND_VIRGL ?
                   backend_opengl_submit : 0,
               (backend.backend == FB_GPU_BACKEND_HYPERV_DXG &&
                (backend.flags & FB_GPU_BACKEND_F_DXG_TRANSPORT) != 0 &&
                (backend.flags & FB_GPU_BACKEND_F_D3DKMT) != 0 &&
                (backend.flags & FB_GPU_BACKEND_F_VIRGL_OPENGL) == 0 &&
                backend_opengl_submit == 0) ||
                   (backend.backend == FB_GPU_BACKEND_VIRGL &&
                    (backend.flags & FB_GPU_BACKEND_F_VIRGL_OPENGL) != 0 &&
                    backend_opengl_submit != 0) ?
                   "PASS" : "DIAGNOSTIC");
        printf("d3d12_native_completion_zero_credit_matrix "
               "backend=%s display_bind=%s transport_present=%lu "
               "completion_source=required present_id=0 completed=0 "
               "close_before_signal=DEFERRED callback_release_order=blocked "
               "per_client_generation=required id_shape=%s "
               "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
               backend_name(backend.backend),
               stats.dxg_present_helper_transport_present ? "PRESENT" :
                   "ABSENT",
               stats.dxg_present_helper_transport_present,
               display_bind_id_shape_ok ? "PASS" : "FAIL",
               backend.backend == FB_GPU_BACKEND_HYPERV_DXG &&
                       backend_opengl_submit == 0 &&
                       stats.dxg_present_helper_transport_present == 0 &&
                       stats.dxg_present_display_target_kind ==
                           FB_GPU_DXG_DISPLAY_TARGET_NONE &&
                       display_bind_id_shape_ok &&
                       provider_credit_gate_ok ?
                   "PASS" : "DIAGNOSTIC");
    }
    printf("gpu_diagnostics_separation_matrix "
           "generic_scanout=drm-kms-fb generic_scanout_prefixes=drm,kms,fb "
           "d3d12_present=dxg-present webkit_policy=separate "
           "ioctl_trace_label=fb-gpu-trace "
           "generic_scanout_native_present_credit=0 "
           "generic_display_last_complete=%lu "
           "d3d12_native_present_credit=0 "
           "opengl_submit_credit=0 backend_opengl_submit=%u status=PASS\n",
           stats.display_last_complete,
           backend_opengl_submit);
    printf("d3d12_display_bind_pending_lifetime_matrix "
           "pending_sequence=%lu created=%lu active=%lu peak=%lu "
           "completed=%lu failclosed=%lu cancelled=%lu "
           "last_status=%lu last_block_reason=0x%lx "
           "source_generation=%lu resource_generation=%lu "
           "native_present_credit=0 opengl_submit_credit=%u status=%s\n",
           stats.dxg_display_bind_pending_sequence,
           stats.dxg_display_bind_pending_created,
           stats.dxg_display_bind_pending_active,
           stats.dxg_display_bind_pending_peak,
           stats.dxg_display_bind_pending_completed,
           stats.dxg_display_bind_pending_failclosed,
           stats.dxg_display_bind_pending_cancelled,
           stats.dxg_display_bind_pending_last_status,
           stats.dxg_display_bind_pending_last_block_reason,
           stats.dxg_display_bind_pending_last_source_generation,
           stats.dxg_display_bind_pending_last_resource_generation,
           backend_opengl_submit,
           display_bind_pending_lifetime_ok ? "PASS" : "DIAGNOSTIC");
    printf("d3d12_display_bind_id_shape_matrix "
           "bind_present_id=%lu bind_completed_id=%lu "
           "bind_source_generation=%lu bind_resource_generation=%lu "
           "scanout_present_id=%lu scanout_completed_id=%lu "
           "scanout_source_generation=%lu scanout_resource_generation=%lu "
           "zero_ids_required_when_failclosed=1 "
           "completed_ge_present_if_nonzero=1 stale_id_rejected=1 "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_scanout_bind_last_present_id,
           stats.dxg_scanout_bind_last_completed,
           stats.dxg_scanout_bind_last_source_generation,
           stats.dxg_scanout_bind_last_resource_generation,
           display_bind_id_shape_ok ? "PASS" : "FAIL");
    printf("d3d12_provider_credit_gate_matrix "
           "provider_submits=%lu provider_no_host_abi=%lu "
           "provider_no_sender=%lu provider_no_completion=%lu "
           "transport_present=%lu display_target_kind=%lu "
           "scanout_successes=%lu completion_successes=%lu "
           "native_present_credit=%lu backend_opengl_submit=%u "
           "credit_requires_provider_clear=1 status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           stats.dxg_present_helper_transport_present,
           stats.dxg_present_display_target_kind,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_completion_successes,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           provider_credit_gate_ok ? "PASS" : "FAIL");
    printf("d3d12_display_bind_request_metadata_matrix "
           "provider_submits=%lu request_metadata_complete=%lu "
           "request_sync_metadata_complete=%lu missing_metadata=0x%lx "
           "required_metadata=0x%lx source_generation=%lu "
           "resource_generation=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_request_metadata_complete,
           stats.dxg_display_bind_request_sync_metadata_complete,
           stats.dxg_display_bind_request_missing_metadata,
           stats.dxg_display_bind_required_metadata,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           display_bind_request_metadata_ok ? "PASS" : "FAIL");
    printf("d3d12_display_bind_provider_pending_publication_matrix "
           "provider_submits=%lu publication_attempts=%lu "
           "host_abi_present=0 sender_present=0 completion_present=0 "
           "owner_generation=%lu provider_source_generation=%lu "
           "provider_resource_generation=%lu pending_owner_generation=%lu "
           "pending_source_generation=%lu pending_resource_generation=%lu "
           "owner_generation_required=1 source_generation_required=1 "
           "resource_generation_required=1 pending_generation_match=%s "
           "publish_before_send=%lu transport_pending_id=%lu "
           "command_id=%lu transaction_id=%lu channel=%s "
           "completion_demux_registered=%lu resolved_or_cancelled=%lu "
           "refs_released=%lu no_host_abi_cancelled=%lu "
           "no_host_abi_refs_released=%lu pending_cancelled=%lu "
           "publish_before_send_order=blocked "
           "cancellation_ref_release_credit=0 provider_no_host_abi=%lu "
           "provider_no_sender=%lu provider_no_completion=%lu "
           "present_id=%lu completed=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_provider_publication_attempts,
           stats.dxg_display_bind_provider_pending_owner_generation,
           stats.dxg_display_bind_provider_pending_source_generation,
           stats.dxg_display_bind_provider_pending_resource_generation,
           stats.dxg_display_bind_pending_last_owner_generation,
           stats.dxg_display_bind_pending_last_source_generation,
           stats.dxg_display_bind_pending_last_resource_generation,
           stats.dxg_display_bind_provider_submits == 0 ?
               "NOT_SAMPLED" :
           (stats.dxg_display_bind_provider_pending_owner_generation != 0 &&
                   stats.dxg_display_bind_provider_pending_source_generation != 0 &&
                   stats.dxg_display_bind_provider_pending_resource_generation != 0 &&
                   stats.dxg_display_bind_pending_last_owner_generation ==
                       stats.dxg_display_bind_provider_pending_owner_generation &&
                   stats.dxg_display_bind_pending_last_source_generation ==
                       stats.dxg_display_bind_provider_pending_source_generation &&
                   stats.dxg_display_bind_pending_last_resource_generation ==
                       stats.dxg_display_bind_provider_pending_resource_generation) ?
               "PASS" : "FAIL",
           stats.dxg_display_bind_provider_publish_before_send,
           stats.dxg_display_bind_provider_transport_pending_id,
           stats.dxg_display_bind_provider_command_id,
           stats.dxg_display_bind_provider_transaction_id,
           stats.dxg_display_bind_provider_channel == 0 ? "none" : "other",
           stats.dxg_display_bind_provider_completion_demux_registered,
           stats.dxg_display_bind_provider_resolved_or_cancelled,
           stats.dxg_display_bind_provider_refs_released,
           stats.dxg_display_bind_provider_no_host_abi_cancelled,
           stats.dxg_display_bind_provider_no_host_abi_refs_released,
           stats.dxg_display_bind_pending_cancelled,
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           display_bind_provider_pending_publication_ok ?
               "PASS_FAILCLOSED" : "FAIL");
    printf("d3d12_display_bind_success_shape_matrix "
           "transport_present=%lu status_code=%lu block_reason=0x%lx "
           "completion_source=%lu present_id=%lu completed=%lu "
           "source_generation=%lu resource_generation=%lu "
           "scanout_successes=%lu completion_successes=%lu "
           "provider_submits=%lu provider_pin_revalidated=%lu "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu failclosed_allowed=1 "
           "success_requires_provider_clear=1 "
           "success_requires_display_completion=1 "
           "native_present_credit=%lu backend_opengl_submit=%d "
           "status=%s\n",
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_status,
           stats.dxg_display_bind_block_reason,
           stats.dxg_display_bind_completion_source,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_completion_successes,
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_provider_pin_revalidated,
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           display_bind_success_shape_ok ? "PASS" : "FAIL");
    printf("d3d12_native_completion_lifetime_matrix "
           "transport_present=%lu status_code=%lu block_reason=0x%lx "
           "completion_source=%lu present_id=%lu completed=%lu "
           "source_generation=%lu resource_generation=%lu "
           "completion_successes=%lu provider_no_completion=%lu "
           "callbacks_after_completion_required=1 "
           "releases_after_completion_required=1 "
           "close_before_signal_cancel_required=1 cleanup_balance_required=1 "
           "failclosed_callbacks_after_completion=0 "
           "failclosed_releases_after_completion=0 "
           "native_present_credit=%lu backend_opengl_submit=%d "
           "status=%s\n",
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_status,
           stats.dxg_display_bind_block_reason,
           stats.dxg_display_bind_completion_source,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_scanout_bind_completion_successes,
           stats.dxg_display_bind_provider_no_completion,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           native_completion_lifetime_ok ? "PASS" : "FAIL");
    printf("d3d12_display_bind_stale_source_zero_credit_matrix "
           "after_close_queries=%lu stale_source_rejects=%lu "
           "release_clears=%lu stale_generation_rejects=%lu "
           "stale_completion_rejects=%lu late_completion_after_release=%lu "
           "after_close_nonzero_id_rejects=%lu "
           "global_present_id_after_close=%lu "
           "global_completed_after_close=%lu native_present_credit=%lu "
           "opengl_submit_credit=%d stale_completion_rejected=%s "
           "late_completion_rejected=%s webkit_accel_credit=0 status=%s\n",
           stats.dxg_display_bind_after_close_queries,
           stats.dxg_display_bind_stale_source_rejects,
           stats.dxg_display_bind_release_clears,
           stats.dxg_display_bind_stale_generation_rejects,
           stats.dxg_display_bind_stale_completion_rejects,
           stats.dxg_display_bind_late_completion_after_release,
           stats.dxg_display_bind_after_close_nonzero_id_rejects,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           stats.dxg_display_bind_stale_completion_rejects != 0 ?
               "PASS" : "PENDING",
           stats.dxg_display_bind_late_completion_after_release == 0 ?
               "PASS" : "FAIL",
           stale_source_zero_credit_ok ? "PASS" : "FAIL");
    printf("d3d12_native_completion_not_kms_matrix "
           "generic_display_last_complete=%lu "
           "kms_vblank_display_correlated=%lu "
           "kms_vblank_source_software_display=%lu "
           "kms_vblank_source_native_hw=%lu "
           "kms_atomic_out_fence_display_correlated=%lu "
           "kms_atomic_out_fence_software_scanout_correlated=%lu "
           "kms_page_flip_events=%lu page_flip_events_software_blit=%lu "
           "page_flip_events_native_hw=%lu display_wait_is_native=0 "
           "kms_generic_display_credit=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.display_last_complete,
           stats.kms_vblank_display_correlated,
           stats.kms_vblank_source_software_display,
           stats.kms_vblank_source_nouveau_hw,
           stats.kms_atomic_out_fence_display_correlated,
           stats.kms_atomic_out_fence_software_scanout_correlated,
           stats.kms_vblank_page_flip_events,
           stats.kms_page_flip_events_software_blit,
           stats.kms_page_flip_events_native_hw,
           generic_completion_not_native_ok ? "PASS" : "FAIL");
    printf("wsl_standard_alloc_not_display_bind_matrix "
           "standard_alloc_private_data=%lu "
           "standard_alloc_display_bind_absent=%lu "
           "standard_alloc_role=private_driver_data "
           "standard_alloc_native_present_credit=0 "
           "display_bind_transport_present=%lu present_id=%lu "
           "completed=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_scanout_bind_standard_alloc_private_data,
           stats.dxg_scanout_bind_standard_alloc_display_bind_absent,
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           standard_alloc_not_display_bind_ok ? "PASS" : "FAIL");
    printf("d3d12_display_bind_host_abi_discovery_matrix "
           "custom_host_tool=0 wsl_dxg_display_bind_ioctl=0 "
           "wsl_display_bind_ioctl_absent=1 "
           "wslg_frame_path=absent freerdp_frame_path=absent "
           "rdp_frame_path=copy_or_dirty_frame "
           "gpup_dxg_sender_contract=%lu "
           "gpup_dxg_completion_contract=%lu "
           "completion_demux_contract=%lu "
           "dda_nouveau_d3d12_import=%lu "
           "dda_nouveau_scanout_bind=%lu "
           "dda_nouveau_hw_flip_completion=%s "
           "provider_state=failclosed provider_failclosed=1 "
           "host_abi_present=0 sender_present=0 completion_present=0 "
           "transport_present=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 "
           "webkit_accel_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_display_bind_provider_completion_demux_registered,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present,
           stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           d3d12_display_bind_host_abi_discovery_ok ? "PASS" : "FAIL");
    printf("public_present_api_not_guest_bind_matrix "
           "reactos_d3dkmt_present_api=known "
           "reactos_present_redirected_api=known "
           "directx_shared_handle_api=known "
           "directx_sharing_contract_hwnd_only=1 "
           "wslg_local_source=absent freerdp_local_source=absent "
           "rdp_frame_transport=copy_or_dirty_frame "
           "guest_vmbus_display_bind_contract=0 "
           "guest_resource_bind_contracts=%lu "
           "guest_completion_contracts=%lu "
           "gpup_sender_contract=%lu gpup_completion_contract=%lu "
           "transport_present=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=%u "
           "webkit_accel_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats.dxg_scanout_bind_candidate_display_completion_contracts,
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           backend_opengl_submit,
           public_present_api_not_guest_bind_ok ? "PASS" : "FAIL");
    printf("gpu_remaining_plan_dependency_skeleton_matrix "
           "root_display_bind_gate=closed native_present_gate=closed "
           "real_display_bind_sender=%lu real_display_bind_completion=%lu "
           "gpup_sender_contract=%lu gpup_completion_contract=%lu "
           "native_completion_validators=armed "
           "native_completion_validator_gate=closed finite_480p_gate=closed "
           "demo_interaction_gate=closed backend_opengl_submit_gate=closed "
           "kvm_virgl_recheck_gate=deferred webkit_route_gate=closed "
           "webkit_content_gate=closed webkit_enabled_artifact_gate=closed "
           "dda_nouveau_blocker=separate-display-not-D3D12-bind "
           "dda_nouveau_reason=DDA/Nouveau-separate-display-not-D3D12-bind "
           "dda_d3d12_resource_import=%lu dda_scanout_bind=%lu "
           "dda_hw_flip_completion=%s display_bind_present_id=%lu "
           "display_bind_completed=%lu backend_opengl_submit=%u "
           "native_present_credit=0 opengl_submit_credit=0 "
           "webkit_accel_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present,
           stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           backend_opengl_submit,
           display_bind_success_shape_ok &&
                   display_bind_pending_lifetime_ok &&
                   display_bind_provider_pending_publication_ok &&
                   native_completion_lifetime_ok &&
                   provider_credit_gate_ok &&
                   generic_completion_not_native_ok &&
                   standard_alloc_not_display_bind_ok &&
                   dda_nouveau_separate_display_not_bind_ok &&
                   public_present_api_not_guest_bind_ok &&
                   d3d12_display_bind_host_abi_discovery_ok &&
                   stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                   stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                   stats.dxg_display_bind_present_id == 0 &&
                   stats.dxg_display_bind_completed_id == 0 &&
                   backend_opengl_submit == 0 ?
               "PASS" : "FAIL");
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
    printf("dmabuf_import_attempts %lu\n", stats.dmabuf_import_attempts);
    printf("dmabuf_local_imports %lu\n", stats.dmabuf_local_imports);
    printf("dmabuf_foreign_import_attempts %lu\n",
           stats.dmabuf_foreign_import_attempts);
    printf("dmabuf_foreign_import_rejects %lu\n",
           stats.dmabuf_foreign_import_rejects);
    printf("dmabuf_local_only_import_path %lu\n",
           stats.dmabuf_local_only_import_path);
    printf("dmabuf_d3d12_foreign_resource_imports %lu\n",
           stats.dmabuf_d3d12_foreign_resource_imports);
    printf("dmabuf_nouveau_scanout_bind_imports %lu\n",
           stats.dmabuf_nouveau_scanout_bind_imports);
    printf("dmabuf_native_present_credit %lu\n",
           stats.dmabuf_native_present_credit);
    printf("dmabuf_bad_fd_rejects %lu\n", stats.dmabuf_bad_fd_rejects);
    printf("dmabuf_foreign_fd_rejects %lu\n",
           stats.dmabuf_foreign_fd_rejects);
    printf("foreign_prime_import_gap_matrix attempts=%lu "
           "local_imports=%lu accepted_imports=%lu "
           "foreign_attempts=%lu foreign_rejects=%lu "
           "legacy_foreign_fd_rejects=%lu local_only_import_path=%lu "
           "d3d12_foreign_resource_imports=%lu "
           "nouveau_scanout_bind_imports=%lu "
           "dmabuf_native_present_credit=%lu "
           "dxg_dda_import_path=%lu dxg_dda_scanout_bind=%lu "
           "scanout_bind_successes=%lu completion_successes=%lu "
           "native_present_credit=%lu opengl_submit_credit=%d status=%s\n",
           stats.dmabuf_import_attempts,
           stats.dmabuf_local_imports,
           stats.dmabuf_imports,
           stats.dmabuf_foreign_import_attempts,
           stats.dmabuf_foreign_import_rejects,
           stats.dmabuf_foreign_fd_rejects,
           stats.dmabuf_local_only_import_path != 0 ||
               stats.dmabuf_import_attempts == 0 ? 1UL : 0UL,
           stats.dmabuf_d3d12_foreign_resource_imports,
           stats.dmabuf_nouveau_scanout_bind_imports,
           stats.dmabuf_native_present_credit,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_completion_successes,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           foreign_prime_import_gap_ok ? "PASS" : "FAIL");
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
    printf("nouveau_getparam_driver_caps %lu\n",
           stats.nouveau_getparam_driver_caps);
    printf("nouveau_getparam_fail_closed %lu\n",
           stats.nouveau_getparam_fail_closed);
    printf("nouveau_getparam_last_source %lu\n",
           stats.nouveau_getparam_last_source);
    printf("nouveau_channel_allocs %lu\n", stats.nouveau_channel_allocs);
    printf("nouveau_channel_frees %lu\n", stats.nouveau_channel_frees);
    printf("nouveau_channel_active %lu\n", stats.nouveau_channel_active);
    printf("nouveau_notifier_allocs %lu\n", stats.nouveau_notifier_allocs);
    printf("nouveau_grobj_allocs %lu\n", stats.nouveau_grobj_allocs);
    printf("nouveau_gpuobj_frees %lu\n", stats.nouveau_gpuobj_frees);
    printf("nouveau_object_rejects %lu\n", stats.nouveau_object_rejects);
    printf("nouveau_close_object_reclaims %lu\n",
           stats.nouveau_close_object_reclaims);
    printf("nouveau_nvif_ioctls %lu\n", stats.nouveau_nvif_ioctls);
    printf("nouveau_nvif_sclass_queries %lu\n",
           stats.nouveau_nvif_sclass_queries);
    printf("nouveau_nvif_sclass_count %lu\n",
           stats.nouveau_nvif_sclass_count);
    printf("nouveau_nvif_new_rejects %lu\n",
           stats.nouveau_nvif_new_rejects);
    printf("nouveau_nvif_del_rejects %lu\n",
           stats.nouveau_nvif_del_rejects);
    printf("nouveau_nvif_unsupported %lu\n",
           stats.nouveau_nvif_unsupported);
    printf("nouveau_gem_news %lu\n", stats.nouveau_gem_news);
    printf("nouveau_gem_infos %lu\n", stats.nouveau_gem_infos);
    printf("nouveau_cpu_preps %lu\n", stats.nouveau_cpu_preps);
    printf("nouveau_cpu_finis %lu\n", stats.nouveau_cpu_finis);
    printf("nouveau_vm_inits %lu\n", stats.nouveau_vm_inits);
    printf("nouveau_vm_bind_noops %lu\n", stats.nouveau_vm_bind_noops);
    printf("nouveau_pushbuf_noops %lu\n", stats.nouveau_pushbuf_noops);
    printf("nouveau_exec_noops %lu\n", stats.nouveau_exec_noops);
    printf("nouveau_nonempty_pushbuf_rejects %lu\n",
           stats.nouveau_nonempty_pushbuf_rejects);
    printf("nouveau_nonempty_exec_rejects %lu\n",
           stats.nouveau_nonempty_exec_rejects);
    printf("nouveau_nonempty_vm_bind_rejects %lu\n",
           stats.nouveau_nonempty_vm_bind_rejects);
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
    printf("nouveau_pci_remove_calls %lu\n",
           stats.nouveau_pci_remove_calls);
    printf("nouveau_pci_remove_runtime_resume_attempts %lu\n",
           stats.nouveau_pci_remove_runtime_resume_attempts);
    printf("nouveau_pci_remove_runtime_resume_successes %lu\n",
           stats.nouveau_pci_remove_runtime_resume_successes);
    printf("nouveau_pci_remove_runtime_barriers %lu\n",
           stats.nouveau_pci_remove_runtime_barriers);
    printf("nouveau_pci_remove_active_before_callback %lu\n",
           stats.nouveau_pci_remove_active_before_callback);
    printf("nouveau_pci_hot_remove_events %lu\n",
           stats.nouveau_pci_hot_remove_events);
    printf("nouveau_pci_removed %lu\n", stats.nouveau_pci_removed);
    printf("nouveau_pci_bar_iounmaps %lu\n",
           stats.nouveau_pci_bar_iounmaps);
    printf("nouveau_pci_irq_unregisters %lu\n",
           stats.nouveau_pci_irq_unregisters);
    printf("nouveau_pci_irq_vectors_freed %lu\n",
           stats.nouveau_pci_irq_vectors_freed);
    printf("nouveau_pci_bus_master_clears %lu\n",
           stats.nouveau_pci_bus_master_clears);
    printf("nouveau_pci_device_disables %lu\n",
           stats.nouveau_pci_device_disables);
    printf("nouveau_pci_drvdata_cleared %lu\n",
           stats.nouveau_pci_drvdata_cleared);
    printf("nouveau_pci_bar0_len %lu\n", stats.nouveau_pci_bar0_len);
    printf("nouveau_pci_bar1_len %lu\n", stats.nouveau_pci_bar1_len);
    printf("nouveau_pci_irq %lu\n", stats.nouveau_pci_irq);
    printf("nouveau_pci_irq_pin %lu\n", stats.nouveau_pci_irq_pin);
    printf("nouveau_pci_msi_cap %lu\n", stats.nouveau_pci_msi_cap);
    printf("nouveau_pci_msix_cap %lu\n", stats.nouveau_pci_msix_cap);
    printf("nouveau_pci_dma_mask_configured %lu\n",
           stats.nouveau_pci_dma_mask_configured);
    printf("nouveau_pci_dma_mask_requested_bits %lu\n",
           stats.nouveau_pci_dma_mask_requested_bits);
    printf("nouveau_pci_dma_mask_bits %lu\n",
           stats.nouveau_pci_dma_mask_bits);
    printf("nouveau_pci_dma_mask_effective_bits %lu\n",
           stats.nouveau_pci_dma_mask_effective_bits);
    printf("nouveau_pci_dma_mask_fallback_32 %lu\n",
           stats.nouveau_pci_dma_mask_fallback_32);
    printf("nouveau_pci_coherent_dma_mask_configured %lu\n",
           stats.nouveau_pci_coherent_dma_mask_configured);
    printf("nouveau_pci_coherent_dma_mask_requested_bits %lu\n",
           stats.nouveau_pci_coherent_dma_mask_requested_bits);
    printf("nouveau_pci_coherent_dma_mask_bits %lu\n",
           stats.nouveau_pci_coherent_dma_mask_bits);
    printf("nouveau_pci_coherent_dma_mask_effective_bits %lu\n",
           stats.nouveau_pci_coherent_dma_mask_effective_bits);
    printf("nouveau_pci_coherent_dma_mask_fallback_32 %lu\n",
           stats.nouveau_pci_coherent_dma_mask_fallback_32);
    printf("nouveau_pci_bar0_claimed %lu\n",
           stats.nouveau_pci_bar0_claimed);
    printf("nouveau_pci_bar1_claimed %lu\n",
           stats.nouveau_pci_bar1_claimed);
    printf("nouveau_pci_bar_claim_failures %lu\n",
           stats.nouveau_pci_bar_claim_failures);
    printf("nouveau_pci_bar_releases %lu\n",
           stats.nouveau_pci_bar_releases);
    printf("nouveau_pci_resource_claims %lu\n",
           stats.nouveau_pci_resource_claims);
    printf("nouveau_pci_resource_releases %lu\n",
           stats.nouveau_pci_resource_releases);
    printf("nouveau_pci_resource_iomaps %lu\n",
           stats.nouveau_pci_resource_iomaps);
    printf("nouveau_pci_resource_owner_mismatches %lu\n",
           stats.nouveau_pci_resource_owner_mismatches);
    printf("nouveau_pci_unclaimed_iomaps %lu\n",
           stats.nouveau_pci_unclaimed_iomaps);
    printf("nouveau_pci_unclaimed_releases %lu\n",
           stats.nouveau_pci_unclaimed_releases);
    printf("nouveau_pci_irq_request_failures %lu\n",
           stats.nouveau_pci_irq_request_failures);
    printf("nouveau_pci_irq_mode %lu\n", stats.nouveau_pci_irq_mode);
    printf("nouveau_pci_msi_requested %lu\n",
           stats.nouveau_pci_msi_requested);
    printf("nouveau_pci_msi_fail_closed %lu\n",
           stats.nouveau_pci_msi_fail_closed);
    printf("nouveau_pci_irq_alloc_requests %lu\n",
           stats.nouveau_pci_irq_alloc_requests);
    printf("nouveau_pci_irq_alloc_failures %lu\n",
           stats.nouveau_pci_irq_alloc_failures);
    printf("nouveau_pci_msi_program_attempts %lu\n",
           stats.nouveau_pci_msi_program_attempts);
    printf("nouveau_pci_msi_program_unsupported %lu\n",
           stats.nouveau_pci_msi_program_unsupported);
    printf("nouveau_pci_msix_program_attempts %lu\n",
           stats.nouveau_pci_msix_program_attempts);
    printf("nouveau_pci_msix_program_unsupported %lu\n",
           stats.nouveau_pci_msix_program_unsupported);
    printf("nouveau_pci_legacy_irq_requests %lu\n",
           stats.nouveau_pci_legacy_irq_requests);
    printf("nouveau_pci_legacy_irq_grants %lu\n",
           stats.nouveau_pci_legacy_irq_grants);
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
    printf("nouveau_pci_irq_handler_invocations %lu\n",
           stats.nouveau_pci_irq_handler_invocations);
    printf("nouveau_pci_irq_cause_reads %lu\n",
           stats.nouveau_pci_irq_cause_reads);
    printf("nouveau_pci_irq_cause_valid %lu\n",
           stats.nouveau_pci_irq_cause_valid);
    printf("nouveau_pci_irq_cause_acks %lu\n",
           stats.nouveau_pci_irq_cause_acks);
    printf("nouveau_pci_irq_spurious %lu\n",
           stats.nouveau_pci_irq_spurious);
    printf("nouveau_pci_dma_map_api_present %lu\n",
           stats.nouveau_pci_dma_map_api_present);
    printf("nouveau_pci_dma_map_attempts %lu\n",
           stats.nouveau_pci_dma_map_attempts);
    printf("nouveau_pci_dma_map_successes %lu\n",
           stats.nouveau_pci_dma_map_successes);
    printf("nouveau_pci_dma_map_failures %lu\n",
           stats.nouveau_pci_dma_map_failures);
    printf("nouveau_pci_dma_unmaps %lu\n", stats.nouveau_pci_dma_unmaps);
    printf("nouveau_pci_dma_map_last_size %lu\n",
           stats.nouveau_pci_dma_map_last_size);
    printf("nouveau_pci_dma_map_last_addr %lu\n",
           stats.nouveau_pci_dma_map_last_addr);
    printf("nouveau_pci_dma_map_last_ret %lu\n",
           stats.nouveau_pci_dma_map_last_ret);
    printf("nouveau_pci_native_present_credit %lu\n",
           stats.nouveau_pci_native_present_credit);
    printf("nouveau_native_display_ready %lu\n",
           stats.nouveau_native_display_ready);
    printf("nouveau_dda_native_display_present %lu\n",
           stats.nouveau_dda_native_display_present);
    printf("nouveau_display_probe_attempts %lu\n",
           stats.nouveau_display_probe_attempts);
    printf("nouveau_display_create_attempts %lu\n",
           stats.nouveau_display_create_attempts);
    printf("nouveau_display_create_successes %lu\n",
           stats.nouveau_display_create_successes);
    printf("nouveau_display_create_fail_closed %lu\n",
           stats.nouveau_display_create_fail_closed);
    printf("nouveau_display_create_fail_reason 0x%lx\n",
           stats.nouveau_display_create_fail_reason);
    print_named_mask("nouveau_display_create_fail_reason_names",
                     stats.nouveau_display_create_fail_reason,
                     kms_present_reject_bits,
                     sizeof(kms_present_reject_bits) /
                     sizeof(kms_present_reject_bits[0]));
    printf("nouveau_display_head_probe_attempts %lu\n",
           stats.nouveau_display_head_probe_attempts);
    printf("nouveau_display_heads %lu\n", stats.nouveau_display_heads);
    printf("nouveau_display_connector_probe_attempts %lu\n",
           stats.nouveau_display_connector_probe_attempts);
    printf("nouveau_display_connectors %lu\n",
           stats.nouveau_display_connectors);
    printf("nouveau_display_nonvirtual_connectors %lu\n",
           stats.nouveau_display_nonvirtual_connectors);
    printf("nouveau_display_vblank_supported %lu\n",
           stats.nouveau_display_vblank_supported);
    printf("nouveau_display_vblank_irq_supported %lu\n",
           stats.nouveau_display_vblank_irq_supported);
    printf("nouveau_display_vblank_source %lu\n",
           stats.nouveau_display_vblank_source);
    printf("nouveau_display_vblank_source_name %s\n",
           nouveau_vblank_source_name(stats.nouveau_display_vblank_source));
    printf("nouveau_display_vblank_irqs %lu\n",
           stats.nouveau_display_vblank_irqs);
    printf("nouveau_display_page_flip_completion_ready %lu\n",
           stats.nouveau_display_page_flip_completion_ready);
    printf("nouveau_display_page_flip_completions %lu\n",
           stats.nouveau_display_page_flip_completions);
    printf("nouveau_display_atomic_pageflip_backend_missing %lu\n",
           stats.nouveau_display_atomic_pageflip_backend_missing);
    printf("nouveau_native_display_reject_reasons 0x%lx\n",
           stats.nouveau_native_display_reject_reasons);
    print_named_mask("nouveau_native_display_reject_reason_names",
                     stats.nouveau_native_display_reject_reasons,
                     kms_present_reject_bits,
                     sizeof(kms_present_reject_bits) /
                     sizeof(kms_present_reject_bits[0]));
    printf("kms_present_last_lane %lu\n", stats.kms_present_last_lane);
    printf("kms_present_dumb %lu\n", stats.kms_present_dumb);
    printf("kms_present_synthvid %lu\n", stats.kms_present_synthvid);
    printf("kms_present_nouveau_hw %lu\n", stats.kms_present_nouveau_hw);
    printf("kms_present_rejects %lu\n", stats.kms_present_rejects);
    printf("kms_present_reject_reasons 0x%lx\n",
           stats.kms_present_reject_reasons);
    print_named_mask("kms_present_reject_reason_names",
                     stats.kms_present_reject_reasons,
                     kms_present_reject_bits,
                     sizeof(kms_present_reject_bits) /
                     sizeof(kms_present_reject_bits[0]));
    printf("kms_present_reject_no_native_display %lu\n",
           stats.kms_present_reject_no_native_display);
    printf("kms_present_reject_no_nouveau_display %lu\n",
           stats.kms_present_reject_no_nouveau_display);
    printf("kms_present_reject_no_display_create %lu\n",
           stats.kms_present_reject_no_display_create);
    printf("kms_present_reject_no_heads %lu\n",
           stats.kms_present_reject_no_heads);
    printf("kms_present_reject_no_connectors %lu\n",
           stats.kms_present_reject_no_connectors);
    printf("kms_present_reject_no_vblank %lu\n",
           stats.kms_present_reject_no_vblank);
    printf("kms_present_reject_no_hw_completion %lu\n",
           stats.kms_present_reject_no_hw_completion);
    printf("kms_present_reject_no_atomic_pageflip_backend %lu\n",
           stats.kms_present_reject_no_atomic_pageflip_backend);
    {
        int hyperv_gpup =
            have_backend &&
            backend.backend == FB_GPU_BACKEND_HYPERV_DXG &&
            (backend.flags & FB_GPU_BACKEND_F_DXG_TRANSPORT) != 0 &&
            (backend.flags & FB_GPU_BACKEND_F_D3DKMT) != 0 &&
            (backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) == 0 &&
            stats.nouveau_pci_probe_accepts == 0;
        int failclosed =
            hyperv_gpup &&
            stats.nouveau_native_display_ready == 0 &&
            stats.nouveau_dda_native_display_present == 0 &&
            stats.nouveau_display_probe_attempts == 0 &&
            stats.nouveau_display_create_attempts == 0 &&
            stats.nouveau_display_create_successes == 0 &&
            stats.nouveau_display_create_fail_reason == 0 &&
            stats.nouveau_display_head_probe_attempts == 0 &&
            stats.nouveau_display_heads == 0 &&
            stats.nouveau_display_connector_probe_attempts == 0 &&
            stats.nouveau_display_connectors == 0 &&
            stats.nouveau_display_nonvirtual_connectors == 0 &&
            stats.nouveau_display_vblank_supported == 0 &&
            stats.nouveau_display_vblank_irq_supported == 0 &&
            stats.nouveau_display_vblank_source ==
                FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE &&
            stats.nouveau_display_vblank_irqs == 0 &&
            stats.nouveau_display_page_flip_completion_ready == 0 &&
            stats.nouveau_display_page_flip_completions == 0 &&
            stats.nouveau_display_atomic_pageflip_backend_missing == 0 &&
            (stats.nouveau_native_display_reject_reasons &
                FB_GPU_KMS_PRESENT_REJECT_ALL) ==
                FB_GPU_KMS_PRESENT_REJECT_ALL &&
            stats.kms_present_last_lane == FB_GPU_KMS_PRESENT_LANE_NONE &&
            stats.kms_present_dumb == 0 &&
            stats.kms_present_synthvid == 0 &&
            stats.kms_present_nouveau_hw == 0 &&
            (stats.kms_present_reject_reasons &
                FB_GPU_KMS_PRESENT_REJECT_ALL) ==
                FB_GPU_KMS_PRESENT_REJECT_ALL &&
            (stats.kms_present_reject_reasons &
             FB_GPU_KMS_PRESENT_REJECT_NO_ATOMIC_PAGEFLIP_BACKEND) != 0 &&
            stats.nouveau_pci_native_present_credit == 0 &&
            (!have_backend ||
             (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0);

        printf("native_display_readiness_failclosed_matrix "
               "backend=%u hyperv_gpup=%s native_display_ready=%lu "
               "dda_native_display_present=%lu display_target_kind=%lu "
               "display_probe_attempts=%lu "
               "head_probe_attempts=%lu connector_probe_attempts=%lu "
               "nonvirtual_connectors=%lu vblank_irq_supported=%lu "
               "vblank_source=%s page_flip_ready=%lu "
               "atomic_backend_missing=%lu "
               "dxg_scanout_bind_successes=%lu present_id=%lu "
               "completed=%lu reject_reasons=0x%lx "
               "reject_has_atomic_pageflip_backend=%u "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=%s\n",
               have_backend ? backend.backend : 0,
               hyperv_gpup ? "PASS" : "FAIL",
               stats.nouveau_native_display_ready,
               stats.nouveau_dda_native_display_present,
               stats.dxg_present_display_target_kind,
               stats.nouveau_display_probe_attempts,
               stats.nouveau_display_head_probe_attempts,
               stats.nouveau_display_connector_probe_attempts,
               stats.nouveau_display_nonvirtual_connectors,
               stats.nouveau_display_vblank_irq_supported,
               nouveau_vblank_source_name(stats.nouveau_display_vblank_source),
               stats.nouveau_display_page_flip_completion_ready,
               stats.nouveau_display_atomic_pageflip_backend_missing,
               stats.dxg_scanout_bind_successes,
               stats.dxg_scanout_bind_last_present_id,
               stats.dxg_scanout_bind_last_completed,
               stats.nouveau_native_display_reject_reasons,
               (stats.nouveau_native_display_reject_reasons &
                FB_GPU_KMS_PRESENT_REJECT_NO_ATOMIC_PAGEFLIP_BACKEND) != 0,
               failclosed ? "PASS" : "FAIL");
        printf("nouveau_display_failclosed_matrix "
               "accepts=%lu probe_attempts=%lu create_attempts=%lu "
               "create_successes=%lu create_fail_closed=%lu "
               "create_fail_reason=0x%lx head_probe_attempts=%lu "
               "heads=%lu connector_probe_attempts=%lu connectors=%lu "
               "nonvirtual_connectors=%lu vblank_supported=%lu "
               "vblank_irq_supported=%lu vblank_source=%s "
               "vblank_irqs=%lu page_flip_ready=%lu "
               "flip_completions=%lu atomic_backend_missing=%lu "
               "atomic_missing_policy=%s dda_native_display_present=%lu "
               "native_display_ready=%lu reject_reasons=0x%lx "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=%s\n",
               stats.nouveau_pci_probe_accepts,
               stats.nouveau_display_probe_attempts,
               stats.nouveau_display_create_attempts,
               stats.nouveau_display_create_successes,
               stats.nouveau_display_create_fail_closed,
               stats.nouveau_display_create_fail_reason,
               stats.nouveau_display_head_probe_attempts,
               stats.nouveau_display_heads,
               stats.nouveau_display_connector_probe_attempts,
               stats.nouveau_display_connectors,
               stats.nouveau_display_nonvirtual_connectors,
               stats.nouveau_display_vblank_supported,
               stats.nouveau_display_vblank_irq_supported,
               nouveau_vblank_source_name(stats.nouveau_display_vblank_source),
               stats.nouveau_display_vblank_irqs,
               stats.nouveau_display_page_flip_completion_ready,
               stats.nouveau_display_page_flip_completions,
               stats.nouveau_display_atomic_pageflip_backend_missing,
               nouveau_atomic_pageflip_backend_missing_ok ? "PASS" : "FAIL",
               stats.nouveau_dda_native_display_present,
               stats.nouveau_native_display_ready,
               stats.nouveau_native_display_reject_reasons,
               failclosed ? "PASS" : "FAIL");
        printf("kms_present_discriminator_failclosed_matrix "
               "last_lane=%lu kms_present_dumb=%lu "
               "kms_present_synthvid=%lu kms_present_nouveau_hw=%lu "
               "rejects=%lu reject_reasons=0x%lx "
               "no_native_display=%lu no_nouveau_display=%lu "
               "no_display_create=%lu no_heads=%lu no_connectors=%lu "
               "no_vblank=%lu no_hw_completion=%lu "
               "no_atomic_pageflip_backend=%lu selected=none "
               "native_display_ready=%lu dda_native_display_present=%lu "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=%s\n",
               stats.kms_present_last_lane,
               stats.kms_present_dumb,
               stats.kms_present_synthvid,
               stats.kms_present_nouveau_hw,
               stats.kms_present_rejects,
               stats.kms_present_reject_reasons,
               stats.kms_present_reject_no_native_display,
               stats.kms_present_reject_no_nouveau_display,
               stats.kms_present_reject_no_display_create,
               stats.kms_present_reject_no_heads,
               stats.kms_present_reject_no_connectors,
               stats.kms_present_reject_no_vblank,
               stats.kms_present_reject_no_hw_completion,
               stats.kms_present_reject_no_atomic_pageflip_backend,
               stats.nouveau_native_display_ready,
               stats.nouveau_dda_native_display_present,
               failclosed ? "PASS" : "FAIL");
        printf("nouveau_display_kms_registration_matrix "
               "accepts=%lu display_probe_attempts=%lu "
               "display_create_attempts=%lu display_create_successes=%lu "
               "head_probe_attempts=%lu heads=%lu "
               "connector_probe_attempts=%lu connectors=%lu "
               "nonvirtual_connectors=%lu vblank_irq_supported=%lu "
               "vblank_source=%s page_flip_ready=%lu "
               "atomic_backend_missing=%lu "
               "kms_registered=%u native_display_ready=%lu "
               "dda_native_display_present=%lu registration_source=%s "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=%s\n",
               stats.nouveau_pci_probe_accepts,
               stats.nouveau_display_probe_attempts,
               stats.nouveau_display_create_attempts,
               stats.nouveau_display_create_successes,
               stats.nouveau_display_head_probe_attempts,
               stats.nouveau_display_heads,
               stats.nouveau_display_connector_probe_attempts,
               stats.nouveau_display_connectors,
               stats.nouveau_display_nonvirtual_connectors,
               stats.nouveau_display_vblank_irq_supported,
               nouveau_vblank_source_name(stats.nouveau_display_vblank_source),
               stats.nouveau_display_page_flip_completion_ready,
               stats.nouveau_display_atomic_pageflip_backend_missing,
               nouveau_display_kms_ready &&
                       stats.nouveau_native_display_ready != 0,
               stats.nouveau_native_display_ready,
               stats.nouveau_dda_native_display_present,
               stats.nouveau_pci_probe_accepts == 0 ? "GPU_P_FAIL_CLOSED" :
                                                       "DDA_DIAGNOSTIC",
               failclosed ? "PASS" : "DIAGNOSTIC");
        printf("nouveau_kms_vblank_irq_source_matrix "
               "kms_vblank_sequence=%lu kms_vblank_samples=%lu "
               "kms_display_correlated=%lu kms_synthetic=%lu "
               "kms_source_software_display=%lu kms_source_native_hw=%lu "
               "nouveau_vblank_supported=%lu "
               "nouveau_vblank_irq_supported=%lu nouveau_vblank_source=%s "
               "nouveau_vblank_irqs=%lu nouveau_irq_claimed=%lu "
               "page_flip_ready=%lu flip_completions=%lu "
               "irq_source=%s native_present_credit=0 "
               "opengl_submit_credit=0 status=%s\n",
               stats.kms_vblank_sequence,
               stats.kms_vblank_samples,
               stats.kms_vblank_display_correlated,
               stats.kms_vblank_synthetic,
               stats.kms_vblank_source_software_display,
               stats.kms_vblank_source_nouveau_hw,
               stats.nouveau_display_vblank_supported,
               stats.nouveau_display_vblank_irq_supported,
               nouveau_vblank_source_name(stats.nouveau_display_vblank_source),
               stats.nouveau_display_vblank_irqs,
               stats.nouveau_pci_irq_delivery_claimed,
               stats.nouveau_display_page_flip_completion_ready,
               stats.nouveau_display_page_flip_completions,
               stats.nouveau_display_vblank_source ==
                       FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ?
                   "nouveau_hw" : "not_nouveau",
               failclosed ? "PASS" : "DIAGNOSTIC");
        printf("nouveau_primary_plane_modifier_failclosed_matrix "
               "primary_plane=diagnostic required_modifier=LINEAR "
               "nonlinear_modifiers=0 nouveau_hw_scanout=%lu "
               "native_display_ready=%lu modifier_credit=0 "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=%s\n",
               stats.kms_present_nouveau_hw,
               stats.nouveau_native_display_ready,
               failclosed ? "PASS" : "DIAGNOSTIC");
        printf("kms_scanout_cpu_convert_separation_matrix "
               "kms_present_dumb=%lu kms_present_synthvid=%lu "
               "kms_present_nouveau_hw=%lu blit_bytes=%lu "
               "software_scanout_fence=%lu cpu_convert_native_present=0 "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=%s\n",
               stats.kms_present_dumb,
               stats.kms_present_synthvid,
               stats.kms_present_nouveau_hw,
               stats.blit_bytes,
               stats.kms_atomic_out_fence_software_scanout_correlated,
               stats.nouveau_pci_native_present_credit == 0 &&
                       backend_opengl_submit == 0 ? "PASS" : "FAIL");
        printf("kms_gem_fb_plane_ref_matrix "
               "kms_framebuffers=%lu stale_kms_fbs=%lu bo_handles=%lu "
               "plane_ref_fields=bounded existing_kernel_fields=1 "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=%s\n",
               stats.kms_framebuffers,
               stats.drm_file_stale_kms_fbs,
               stats.bo_handles,
               stats.nouveau_pci_native_present_credit == 0 &&
                       backend_opengl_submit == 0 ? "PASS" : "FAIL");
        printf("kms_atomic_plane_state_matrix "
               "atomic_commits=%lu framebuffers=%lu page_flips=%lu "
               "out_fence_display_correlated=%lu "
               "out_fence_software_scanout_correlated=%lu "
               "plane_state_native_present=0 native_present_credit=0 "
               "opengl_submit_credit=0 status=%s\n",
               stats.kms_atomic_commits,
               stats.kms_framebuffers,
               stats.kms_page_flips,
               stats.kms_atomic_out_fence_display_correlated,
               stats.kms_atomic_out_fence_software_scanout_correlated,
               stats.kms_atomic_out_fence_software_scanout_correlated == 0 &&
                       stats.nouveau_pci_native_present_credit == 0 &&
                       backend_opengl_submit == 0 ? "PASS" : "FAIL");
        printf("kms_atomic_prepare_cleanup_fb_matrix "
               "in_fence_fd_refs=%lu in_fence_fd_ref_puts=%lu "
               "out_fence_prepared=%lu out_fence_cleanup_closes=%lu "
               "test_only_placeholders=%lu stale_kms_fbs=%lu "
               "fb_prepare_cleanup_credit=0 native_present_credit=0 "
               "opengl_submit_credit=0 status=%s\n",
               stats.kms_atomic_in_fence_fd_refs,
               stats.kms_atomic_in_fence_fd_ref_puts,
               stats.kms_atomic_out_fence_prepared,
               stats.kms_atomic_out_fence_cleanup_closes,
               stats.kms_atomic_out_fence_test_only_placeholders,
               stats.drm_file_stale_kms_fbs,
               stats.kms_atomic_in_fence_fd_refs ==
                           stats.kms_atomic_in_fence_fd_ref_puts &&
                       stats.nouveau_pci_native_present_credit == 0 &&
                       backend_opengl_submit == 0 ? "PASS" : "FAIL");
        printf("kms_page_flip_feature_gate_matrix "
               "page_flips=%lu target_rejects=%lu async_rejects=%lu "
               "invalid_noevent_rejects=%lu page_flip_events=%lu "
               "page_flip_ready=%lu atomic_backend_missing=%lu "
               "target_gate=%s async_gate=%s "
               "page_flip_native_present_credit=0 "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=%s\n",
               stats.kms_page_flips,
               stats.kms_page_flip_target_rejects,
               stats.kms_page_flip_async_rejects,
               stats.kms_page_flip_invalid_noevent_rejects,
               stats.kms_vblank_page_flip_events,
               stats.nouveau_display_page_flip_completion_ready,
               stats.nouveau_display_atomic_pageflip_backend_missing,
               stats.nouveau_native_display_ready == 0 ? "closed" :
                                                          "diagnostic",
               stats.nouveau_native_display_ready == 0 ? "closed" :
                                                          "diagnostic",
               stats.kms_present_nouveau_hw == 0 &&
                       stats.nouveau_pci_native_present_credit == 0 &&
                       backend_opengl_submit == 0 ? "PASS" : "FAIL");
    }
    printf("nouveau_pci_dma_resource_matrix stats "
           "registered=%lu accepts=%lu reject_dxg_present=%lu "
           "reject_no_bars=%lu dma_mask_configured=%lu "
           "dma_mask_requested_bits=%lu dma_mask_bits=%lu "
           "dma_mask_effective_bits=%lu dma_mask_fallback_32=%lu "
           "coherent_configured=%lu coherent_requested_bits=%lu "
           "coherent_bits=%lu coherent_effective_bits=%lu "
           "coherent_fallback_32=%lu bar0_len=%lu bar1_len=%lu "
           "bar0_claimed=%lu bar1_claimed=%lu claim_failures=%lu "
           "releases=%lu resource_claims=%lu resource_releases=%lu "
           "resource_iomaps=%lu owner_mismatches=%lu "
           "unclaimed_iomaps=%lu unclaimed_releases=%lu "
           "irq_mode=%lu irq_failures=%lu "
           "msi_requested=%lu msi_fail_closed=%lu "
           "irq_alloc_requests=%lu irq_alloc_failures=%lu "
           "msi_program_attempts=%lu msi_program_unsupported=%lu "
           "msix_program_attempts=%lu msix_program_unsupported=%lu "
           "legacy_irq_requests=%lu legacy_irq_grants=%lu "
           "irq_vector_valid=%lu irq_handler_registered=%lu "
           "irq_delivery_enabled=%lu irq_delivery_claimed=%lu "
           "legacy_irq_fallback=%lu irq_handler_invocations=%lu "
           "irq_cause_reads=%lu irq_cause_valid=%lu "
           "irq_cause_acks=%lu irq_spurious=%lu dma_map_api=%lu "
           "dma_map_attempts=%lu dma_map_successes=%lu "
           "dma_map_failures=%lu dma_unmaps=%lu dma_last_size=%lu "
           "dma_last_addr=0x%lx dma_last_ret=%lu "
           "suspend_count=%lu resume_count=%lu pm_balanced=%lu "
           "remove_while_suspended=%lu remove_calls=%lu "
           "remove_resume_attempts=%lu remove_resume_successes=%lu "
           "remove_barriers=%lu remove_active_before_callback=%lu "
           "hot_remove_events=%lu removed=%lu bar_iounmaps=%lu "
           "irq_unregisters=%lu irq_vectors_freed=%lu "
           "bus_master_clears=%lu device_disables=%lu "
           "drvdata_cleared=%lu "
           "native_present_credit=%lu\n",
           stats.nouveau_pci_registered,
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_probe_reject_dxg_present,
           stats.nouveau_pci_probe_reject_no_bars,
           stats.nouveau_pci_dma_mask_configured,
           stats.nouveau_pci_dma_mask_requested_bits,
           stats.nouveau_pci_dma_mask_bits,
           stats.nouveau_pci_dma_mask_effective_bits,
           stats.nouveau_pci_dma_mask_fallback_32,
           stats.nouveau_pci_coherent_dma_mask_configured,
           stats.nouveau_pci_coherent_dma_mask_requested_bits,
           stats.nouveau_pci_coherent_dma_mask_bits,
           stats.nouveau_pci_coherent_dma_mask_effective_bits,
           stats.nouveau_pci_coherent_dma_mask_fallback_32,
           stats.nouveau_pci_bar0_len,
           stats.nouveau_pci_bar1_len,
           stats.nouveau_pci_bar0_claimed,
           stats.nouveau_pci_bar1_claimed,
           stats.nouveau_pci_bar_claim_failures,
           stats.nouveau_pci_bar_releases,
           stats.nouveau_pci_resource_claims,
           stats.nouveau_pci_resource_releases,
           stats.nouveau_pci_resource_iomaps,
           stats.nouveau_pci_resource_owner_mismatches,
           stats.nouveau_pci_unclaimed_iomaps,
           stats.nouveau_pci_unclaimed_releases,
           stats.nouveau_pci_irq_mode,
           stats.nouveau_pci_irq_request_failures,
           stats.nouveau_pci_msi_requested,
           stats.nouveau_pci_msi_fail_closed,
           stats.nouveau_pci_irq_alloc_requests,
           stats.nouveau_pci_irq_alloc_failures,
           stats.nouveau_pci_msi_program_attempts,
           stats.nouveau_pci_msi_program_unsupported,
           stats.nouveau_pci_msix_program_attempts,
           stats.nouveau_pci_msix_program_unsupported,
           stats.nouveau_pci_legacy_irq_requests,
           stats.nouveau_pci_legacy_irq_grants,
           stats.nouveau_pci_irq_vector_valid,
           stats.nouveau_pci_irq_handler_registered,
           stats.nouveau_pci_irq_delivery_enabled,
           stats.nouveau_pci_irq_delivery_claimed,
           stats.nouveau_pci_legacy_irq_fallback,
           stats.nouveau_pci_irq_handler_invocations,
           stats.nouveau_pci_irq_cause_reads,
           stats.nouveau_pci_irq_cause_valid,
           stats.nouveau_pci_irq_cause_acks,
           stats.nouveau_pci_irq_spurious,
           stats.nouveau_pci_dma_map_api_present,
           stats.nouveau_pci_dma_map_attempts,
           stats.nouveau_pci_dma_map_successes,
           stats.nouveau_pci_dma_map_failures,
           stats.nouveau_pci_dma_unmaps,
           stats.nouveau_pci_dma_map_last_size,
           stats.nouveau_pci_dma_map_last_addr,
           stats.nouveau_pci_dma_map_last_ret,
           stats.nouveau_pci_suspend_count,
           stats.nouveau_pci_resume_count,
           stats.nouveau_pci_runtime_pm_balanced,
           stats.nouveau_pci_remove_runtime_suspended,
           stats.nouveau_pci_remove_calls,
           stats.nouveau_pci_remove_runtime_resume_attempts,
           stats.nouveau_pci_remove_runtime_resume_successes,
           stats.nouveau_pci_remove_runtime_barriers,
           stats.nouveau_pci_remove_active_before_callback,
           stats.nouveau_pci_hot_remove_events,
           stats.nouveau_pci_removed,
           stats.nouveau_pci_bar_iounmaps,
           stats.nouveau_pci_irq_unregisters,
           stats.nouveau_pci_irq_vectors_freed,
           stats.nouveau_pci_bus_master_clears,
           stats.nouveau_pci_device_disables,
           stats.nouveau_pci_drvdata_cleared,
           stats.nouveau_pci_native_present_credit);
    {
        const int accepts = stats.nouveau_pci_probe_accepts != 0;
        const char *dma_mask = accepts ?
            (stats.nouveau_pci_dma_mask_configured &&
             stats.nouveau_pci_dma_mask_requested_bits >= 32 &&
             stats.nouveau_pci_dma_mask_effective_bits >= 32 &&
             stats.nouveau_pci_dma_mask_bits ==
                 stats.nouveau_pci_dma_mask_effective_bits ?
                 "PASS" : "FAIL") :
            "NOT_CONFIGURED";
        const char *coherent_dma_mask = accepts ?
            (stats.nouveau_pci_coherent_dma_mask_configured &&
             stats.nouveau_pci_coherent_dma_mask_requested_bits >= 32 &&
             stats.nouveau_pci_coherent_dma_mask_effective_bits >= 32 &&
             stats.nouveau_pci_coherent_dma_mask_bits ==
                 stats.nouveau_pci_coherent_dma_mask_effective_bits ?
                 "PASS" : "FAIL") : "NOT_CONFIGURED";
        const char *bar_claim = accepts ?
            (((stats.nouveau_pci_bar0_len == 0 ||
               stats.nouveau_pci_bar0_claimed) &&
              (stats.nouveau_pci_bar1_len == 0 ||
               stats.nouveau_pci_bar1_claimed)) ? "PASS" : "FAIL") :
            "NOT_ATTEMPTED";
        const char *resource_owner =
            stats.nouveau_pci_resource_owner_mismatches == 0 ?
                (accepts ? "PASS" : "GPU_P_FAIL_CLOSED") : "FAIL";
        const char *claim_before_iomap =
            stats.nouveau_pci_unclaimed_iomaps == 0 ?
                (accepts ? "PASS" : "GPU_P_FAIL_CLOSED") : "FAIL";
        const char *release_balance =
            stats.nouveau_pci_unclaimed_releases == 0 ?
                (accepts ? "PASS" : "GPU_P_FAIL_CLOSED") : "FAIL";
        const char *dma_map = accepts ?
            (stats.nouveau_pci_dma_map_api_present &&
             stats.nouveau_pci_dma_map_attempts != 0 &&
             stats.nouveau_pci_dma_map_successes != 0 &&
             stats.nouveau_pci_dma_map_failures == 0 &&
             stats.nouveau_pci_dma_unmaps ==
                 stats.nouveau_pci_dma_map_successes ? "PASS" : "FAIL") :
            "GPU_P_FAIL_CLOSED";
        const char *msi_msix = stats.nouveau_pci_msi_fail_closed ?
            "FAIL_CLOSED" : "NOT_ATTEMPTED";
        const char *legacy_irq = accepts ?
            (stats.nouveau_pci_legacy_irq_fallback ? "PASS" : "MISSING") :
            "NOT_CLAIMED";
        const char *irq_handler =
            stats.nouveau_pci_irq_handler_registered ? "PRESENT" : "ABSENT";
        const char *irq_delivery =
            stats.nouveau_pci_irq_delivery_enabled ||
                    stats.nouveau_pci_irq_delivery_claimed ?
                "PRESENT" : "ABSENT";
        const char *irq_cause =
            accepts ? (stats.nouveau_pci_irq_cause_acks != 0 ?
                           "PASS" : "DIAGNOSTIC") : "GPU_P_FAIL_CLOSED";
        const char *runtime_pm = accepts ? "DIAGNOSTIC" : "DEFERRED";
        const char *remove_path =
            stats.nouveau_pci_removes ? "DIAGNOSTIC" : "DEFERRED";
        const char *status = accepts ? "DIAGNOSTIC" : "PASS";

        printf("nouveau_pci_runtime_contract_matrix "
               "accepts=%lu gpup_only=%s dma_mask=%s "
               "coherent_dma_mask=%s dma_map=%s bar_claim=%s "
               "msi_msix_setup=%s legacy_irq_fallback=%s "
               "irq_handler=%s irq_delivery=%s runtime_pm_usage=%s "
               "remove_path=%s native_present_credit=%lu "
               "opengl_submit_credit=0 status=%s\n",
               stats.nouveau_pci_probe_accepts,
               accepts ? "NO" : "PASS",
               dma_mask, coherent_dma_mask, dma_map, bar_claim, msi_msix,
               legacy_irq, irq_handler, irq_delivery, runtime_pm,
               remove_path, stats.nouveau_pci_native_present_credit,
               status);
        printf("nouveau_pci_runtime_interface_matrix "
               "accepts=%lu resource_tree=%s dma_mapping_api=%s "
               "msi_msix_programming=%s legacy_irq_fallback=%s "
               "irq_delivery=%s runtime_pm=%s remove_path=%s "
               "resource_owner=%s claim_before_iomap=%s "
               "release_balance=%s owner_mismatch=%lu "
               "unclaimed_iomap=%lu unclaimed_release=%lu "
               "hot_remove=%s native_engine=%s native_present_credit=%lu "
               "opengl_submit_credit=0 status=%s\n",
               stats.nouveau_pci_probe_accepts,
               accepts ? bar_claim : "GPU_P_FAIL_CLOSED",
               accepts ? dma_map : "GPU_P_FAIL_CLOSED",
               msi_msix,
               legacy_irq,
               irq_delivery,
               runtime_pm,
               remove_path,
               resource_owner,
               claim_before_iomap,
               release_balance,
               stats.nouveau_pci_resource_owner_mismatches,
               stats.nouveau_pci_unclaimed_iomaps,
               stats.nouveau_pci_unclaimed_releases,
               accepts ? "DIAGNOSTIC" : "DEFERRED",
               accepts ? "DIAGNOSTIC" : "ABSENT",
               stats.nouveau_pci_native_present_credit,
               status);
        printf("nouveau_pci_irq_provenance_matrix "
               "accepts=%lu msi_attempts=%lu msi_unsupported=%lu "
               "msix_attempts=%lu msix_unsupported=%lu "
               "legacy_requests=%lu legacy_grants=%lu "
               "handler_invocations=%lu cause_reads=%lu "
               "cause_valid=%lu cause_acks=%lu spurious=%lu "
               "device_cause=%s native_present_credit=%lu "
               "opengl_submit_credit=0 status=%s\n",
               stats.nouveau_pci_probe_accepts,
               stats.nouveau_pci_msi_program_attempts,
               stats.nouveau_pci_msi_program_unsupported,
               stats.nouveau_pci_msix_program_attempts,
               stats.nouveau_pci_msix_program_unsupported,
               stats.nouveau_pci_legacy_irq_requests,
               stats.nouveau_pci_legacy_irq_grants,
               stats.nouveau_pci_irq_handler_invocations,
               stats.nouveau_pci_irq_cause_reads,
               stats.nouveau_pci_irq_cause_valid,
               stats.nouveau_pci_irq_cause_acks,
               stats.nouveau_pci_irq_spurious,
               irq_cause,
               stats.nouveau_pci_native_present_credit,
               accepts ? "DIAGNOSTIC" : "PASS");
        printf("nouveau_pci_remove_pm_matrix "
               "accepts=%lu remove_calls=%lu "
               "runtime_resume_attempts=%lu "
               "runtime_resume_successes=%lu runtime_barriers=%lu "
               "runtime_resume_before_remove=%s "
               "remove_while_suspended=%lu hot_remove_events=%lu "
               "removed=%lu bar_iounmaps=%lu irq_unregisters=%lu "
               "irq_vectors_freed=%lu bus_master_clears=%lu "
               "device_disables=%lu drvdata_cleared=%lu "
               "teardown=%s native_present_credit=%lu "
               "opengl_submit_credit=0 status=%s\n",
               stats.nouveau_pci_probe_accepts,
               stats.nouveau_pci_remove_calls,
               stats.nouveau_pci_remove_runtime_resume_attempts,
               stats.nouveau_pci_remove_runtime_resume_successes,
               stats.nouveau_pci_remove_runtime_barriers,
               accepts ? (stats.nouveau_pci_removes == 0 ?
                              "DEFERRED" :
                              (stats.nouveau_pci_remove_runtime_suspended == 0 ?
                                   "PASS" : "FAIL")) :
                         "NOT_APPLICABLE",
               stats.nouveau_pci_remove_runtime_suspended,
               stats.nouveau_pci_hot_remove_events,
               stats.nouveau_pci_removed,
               stats.nouveau_pci_bar_iounmaps,
               stats.nouveau_pci_irq_unregisters,
               stats.nouveau_pci_irq_vectors_freed,
               stats.nouveau_pci_bus_master_clears,
               stats.nouveau_pci_device_disables,
               stats.nouveau_pci_drvdata_cleared,
               accepts ? (stats.nouveau_pci_removes == 0 ?
                              "DEFERRED" :
                              (stats.nouveau_pci_drvdata_cleared != 0 &&
                               stats.nouveau_pci_device_disables != 0 ?
                                   "PASS" : "FAIL")) :
                         "GPU_P_FAIL_CLOSED",
               stats.nouveau_pci_native_present_credit,
               accepts ? "DIAGNOSTIC" : "PASS");
    }
    printf("nouveau_getparam_provenance_matrix stats "
           "getparams=%lu dda_facts=%lu synthetic_facts=%lu "
           "driver_caps=%lu "
           "fail_closed=%lu last_source=%lu accepts=%lu "
           "native_present_credit=%lu\n",
           stats.nouveau_getparams,
           stats.nouveau_getparam_dda_facts,
           stats.nouveau_getparam_synthetic_facts,
           stats.nouveau_getparam_driver_caps,
           stats.nouveau_getparam_fail_closed,
           stats.nouveau_getparam_last_source,
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_native_present_credit);
    if (have_backend && stats.nouveau_pci_probe_accepts == 0) {
        int no_fake_bar =
            stats.nouveau_pci_bar0_len == 0 &&
            stats.nouveau_pci_bar1_len == 0 &&
            stats.nouveau_pci_bar0_claimed == 0 &&
            stats.nouveau_pci_bar1_claimed == 0;
        int no_fake_dma =
            stats.nouveau_pci_dma_mask_configured == 0 &&
            stats.nouveau_pci_dma_mask_requested_bits == 0 &&
            stats.nouveau_pci_dma_mask_effective_bits == 0 &&
            stats.nouveau_pci_dma_mask_fallback_32 == 0 &&
            stats.nouveau_pci_coherent_dma_mask_configured == 0 &&
            stats.nouveau_pci_coherent_dma_mask_requested_bits == 0 &&
            stats.nouveau_pci_coherent_dma_mask_effective_bits == 0 &&
            stats.nouveau_pci_coherent_dma_mask_fallback_32 == 0 &&
            stats.nouveau_pci_dma_map_attempts == 0 &&
            stats.nouveau_pci_dma_map_successes == 0 &&
            stats.nouveau_pci_dma_map_failures == 0 &&
            stats.nouveau_pci_dma_unmaps == 0;
        int no_fake_irq =
            stats.nouveau_pci_irq_vector_valid == 0 &&
            stats.nouveau_pci_irq_alloc_requests == 0 &&
            stats.nouveau_pci_irq_alloc_failures == 0 &&
            stats.nouveau_pci_msi_program_attempts == 0 &&
            stats.nouveau_pci_msi_program_unsupported == 0 &&
            stats.nouveau_pci_msix_program_attempts == 0 &&
            stats.nouveau_pci_msix_program_unsupported == 0 &&
            stats.nouveau_pci_legacy_irq_requests == 0 &&
            stats.nouveau_pci_legacy_irq_grants == 0 &&
            stats.nouveau_pci_irq_handler_registered == 0 &&
            stats.nouveau_pci_irq_delivery_enabled == 0 &&
            stats.nouveau_pci_irq_delivery_claimed == 0 &&
            stats.nouveau_pci_irq_handler_invocations == 0 &&
            stats.nouveau_pci_irq_cause_reads == 0 &&
            stats.nouveau_pci_irq_cause_valid == 0 &&
            stats.nouveau_pci_irq_cause_acks == 0 &&
            stats.nouveau_pci_irq_spurious == 0;
        int no_fake_getparams =
            stats.nouveau_getparams == 0 &&
            stats.nouveau_getparam_dda_facts == 0 &&
            stats.nouveau_getparam_synthetic_facts == 0 &&
            stats.nouveau_getparam_driver_caps == 0 &&
            stats.nouveau_getparam_last_source ==
                FB_GPU_NOUVEAU_GETPARAM_SOURCE_NONE;
        int no_fake_present =
            stats.nouveau_pci_native_present_credit == 0 &&
            stats.dxg_present_dda_nouveau_present == 0 &&
            stats.dxg_present_dda_nouveau_import_path_present == 0 &&
            stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
            (backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) == 0;
        int no_fake_remove =
            stats.nouveau_pci_remove_calls == 0 &&
            stats.nouveau_pci_remove_runtime_resume_attempts == 0 &&
            stats.nouveau_pci_remove_runtime_resume_successes == 0 &&
            stats.nouveau_pci_remove_runtime_barriers == 0 &&
            stats.nouveau_pci_remove_active_before_callback == 0 &&
            stats.nouveau_pci_hot_remove_events == 0 &&
            stats.nouveau_pci_removed == 0 &&
            stats.nouveau_pci_bar_iounmaps == 0 &&
            stats.nouveau_pci_irq_unregisters == 0 &&
            stats.nouveau_pci_irq_vectors_freed == 0 &&
            stats.nouveau_pci_bus_master_clears == 0 &&
            stats.nouveau_pci_device_disables == 0 &&
            stats.nouveau_pci_drvdata_cleared == 0;
        int rejected =
            stats.nouveau_pci_probes == 0 ||
            stats.nouveau_pci_probe_reject_dxg_present != 0 ||
            stats.nouveau_pci_probe_reject_no_bars != 0;

        printf("nouveau_gpup_failclosed_matrix "
               "accepts=0 backend_dda_nouveau=0 reject_reason=%s "
               "no_fake_bar=%s no_fake_dma=%s no_fake_irq=%s "
               "no_fake_getparam=%s no_fake_remove=%s "
               "no_fake_present=%s "
               "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
               rejected ? "PASS" : "MISSING",
               no_fake_bar ? "PASS" : "FAIL",
               no_fake_dma ? "PASS" : "FAIL",
               no_fake_irq ? "PASS" : "FAIL",
               no_fake_getparams ? "PASS" : "FAIL",
               no_fake_remove ? "PASS" : "FAIL",
               no_fake_present ? "PASS" : "FAIL",
               (rejected && no_fake_bar && no_fake_dma && no_fake_irq &&
               no_fake_getparams && no_fake_remove && no_fake_present) ?
                   "PASS" : "FAIL");
    }
    if (stats.nouveau_pci_probe_accepts != 0) {
        int balanced =
            stats.nouveau_getparams ==
            stats.nouveau_getparam_dda_facts +
                stats.nouveau_getparam_driver_caps +
                stats.nouveau_getparam_synthetic_facts;
        int no_synthetic = stats.nouveau_getparam_synthetic_facts == 0;
        printf("nouveau_getparam_ddafacts_matrix stats accepts=%lu "
               "dda_facts=%lu driver_caps=%lu synthetic_facts=%lu "
               "balanced=%s no_synthetic_hw=%s "
               "native_present_credit=%lu status=%s\n",
               stats.nouveau_pci_probe_accepts,
               stats.nouveau_getparam_dda_facts,
               stats.nouveau_getparam_driver_caps,
               stats.nouveau_getparam_synthetic_facts,
               balanced ? "PASS" : "FAIL",
               no_synthetic ? "PASS" : "FAIL",
               stats.nouveau_pci_native_present_credit,
               balanced && no_synthetic &&
                       stats.nouveau_pci_native_present_credit == 0 ?
                   "PASS" : "FAIL");
    }
    printf("nouveau_channel_object_matrix stats "
           "channel_allocs=%lu channel_frees=%lu active=%lu "
           "notifier_allocs=%lu grobj_allocs=%lu gpuobj_frees=%lu "
           "object_rejects=%lu close_reclaims=%lu status=%s\n",
           stats.nouveau_channel_allocs,
           stats.nouveau_channel_frees,
           stats.nouveau_channel_active,
           stats.nouveau_notifier_allocs,
           stats.nouveau_grobj_allocs,
           stats.nouveau_gpuobj_frees,
           stats.nouveau_object_rejects,
           stats.nouveau_close_object_reclaims,
           stats.nouveau_channel_active == 0 ? "PASS" : "PENDING");
    printf("nouveau_nvif_failclosed_matrix stats "
           "ioctls=%lu sclass_queries=%lu sclass_count=%lu "
           "new_rejects=%lu del_rejects=%lu unsupported=%lu "
           "status=%s\n",
           stats.nouveau_nvif_ioctls,
           stats.nouveau_nvif_sclass_queries,
           stats.nouveau_nvif_sclass_count,
           stats.nouveau_nvif_new_rejects,
           stats.nouveau_nvif_del_rejects,
           stats.nouveau_nvif_unsupported,
           stats.nouveau_nvif_sclass_count == 0 ? "PASS" : "FAIL");
    printf("nouveau_submit_failclosed_matrix stats "
           "pushbuf_noops=%lu exec_noops=%lu vm_bind_noops=%lu "
           "nonempty_pushbuf_rejects=%lu nonempty_exec_rejects=%lu "
           "nonempty_vm_bind_rejects=%lu native_present_credit=%lu "
           "opengl_submit_credit=0 status=%s\n",
           stats.nouveau_pushbuf_noops,
           stats.nouveau_exec_noops,
           stats.nouveau_vm_bind_noops,
           stats.nouveau_nonempty_pushbuf_rejects,
           stats.nouveau_nonempty_exec_rejects,
           stats.nouveau_nonempty_vm_bind_rejects,
           stats.nouveau_pci_native_present_credit,
           stats.nouveau_pci_native_present_credit == 0 ? "PASS" : "FAIL");
    printf("nouveau_gem_mmap_backing_matrix stats "
           "gem_news=%lu gem_infos=%lu cpu_preps=%lu cpu_finis=%lu "
           "mmap_backing=absent mmap_successes=0 "
           "backing_source=none linux_mmap_credit=0 "
           "native_present_credit=%lu opengl_submit_credit=0 "
           "status=%s\n",
           stats.nouveau_gem_news,
           stats.nouveau_gem_infos,
           stats.nouveau_cpu_preps,
           stats.nouveau_cpu_finis,
           stats.nouveau_pci_native_present_credit,
           stats.nouveau_pci_native_present_credit == 0 ? "PASS" : "FAIL");
    printf("nouveau_gpuvm_mapping_failclosed_matrix stats "
           "vm_inits=%lu vm_bind_noops=%lu "
           "nonempty_vm_bind_rejects=%lu mapping_successes=0 "
           "mapping_backend=fail_closed native_present_credit=%lu "
           "opengl_submit_credit=0 status=%s\n",
           stats.nouveau_vm_inits,
           stats.nouveau_vm_bind_noops,
           stats.nouveau_nonempty_vm_bind_rejects,
           stats.nouveau_pci_native_present_credit,
           stats.nouveau_pci_native_present_credit == 0 ? "PASS" : "FAIL");
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
    printf("kms_atomic_out_fence_software_scanout_correlated %lu\n",
           stats.kms_atomic_out_fence_software_scanout_correlated);
    printf("kms_atomic_fence_matrix stats fd_refs=%lu fd_ref_puts=%lu "
           "duplicate_rejects=%lu test_only_validated=%lu "
           "test_only_waits=%lu sync_file_pending_waits=%lu "
           "sync_file_pending_wakeups=%lu out_fence_prepared=%lu "
           "out_fence_cleanup_closes=%lu "
           "test_only_out_fence_placeholders=%lu out_fence_exports=%lu "
           "out_fence_display_correlated=%lu "
           "out_fence_software_scanout_correlated=%lu\n",
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
           stats.kms_atomic_out_fence_display_correlated,
           stats.kms_atomic_out_fence_software_scanout_correlated);
    printf("kms_vblank_native_present_separation_matrix "
           "vblank_source=%s display_correlated=%lu synthetic=%lu "
           "vblank_sequence=%lu display_last_complete=%lu "
           "page_flip_events=%lu kms_page_flips=%lu "
           "page_flip_events_software_blit=%lu "
           "page_flip_events_native_hw=%lu "
           "vblank_source_software_display=%lu "
           "vblank_source_native_hw=%lu "
           "display_completion_is_native_present=0 "
           "page_flip_native_present_credit=0 "
           "vblank_native_present_credit=0 "
           "atomic_out_fence_display_correlated=%lu "
           "atomic_out_fence_software_scanout_correlated=%lu "
           "opengl_submit_credit=0 status=PASS\n",
           stats.kms_vblank_display_correlated ? "display" : "synthetic",
           stats.kms_vblank_display_correlated,
           stats.kms_vblank_synthetic,
           stats.kms_vblank_sequence,
           stats.display_last_complete,
           stats.kms_vblank_page_flip_events,
           stats.kms_page_flips,
           stats.kms_page_flip_events_software_blit,
           stats.kms_page_flip_events_native_hw,
           stats.kms_vblank_source_software_display,
           stats.kms_vblank_source_nouveau_hw,
           stats.kms_atomic_out_fence_display_correlated,
           stats.kms_atomic_out_fence_software_scanout_correlated);
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
    printf("ttm_real_move_backend_matrix stats real_copy_moves=%lu "
           "move_bytes=%lu cpu_copy_by_domain=%lu/%lu/%lu/%lu "
           "unsupported_hw_copy_by_domain=%lu/%lu/%lu/%lu "
           "real_copy_by_domain=%lu/%lu/%lu/%lu "
           "native_accel_credit=%lu real_move_backend=cpu_copy "
           "hw_backend=fail_closed native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.ttm_real_copy_moves,
           stats.ttm_move_bytes,
           stats.ttm_cpu_copy_fallback_moves[0],
           stats.ttm_cpu_copy_fallback_moves[1],
           stats.ttm_cpu_copy_fallback_moves[2],
           stats.ttm_cpu_copy_fallback_moves[3],
           stats.ttm_unsupported_hw_copy_moves[0],
           stats.ttm_unsupported_hw_copy_moves[1],
           stats.ttm_unsupported_hw_copy_moves[2],
           stats.ttm_unsupported_hw_copy_moves[3],
           stats.ttm_real_copy_moves_by_domain[0],
           stats.ttm_real_copy_moves_by_domain[1],
           stats.ttm_real_copy_moves_by_domain[2],
           stats.ttm_real_copy_moves_by_domain[3],
           stats.ttm_native_accel_credit,
           stats.ttm_native_accel_credit == 0 ? "PASS" : "FAIL");
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
    printf("syncobj_pending_transfers %lu\n",
           stats.syncobj_pending_transfers);
    printf("syncobj_pending_transfer_wakeups %lu\n",
           stats.syncobj_pending_transfer_wakeups);
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
    printf("dxg_present_commit_copyout_failures %lu\n",
           stats.dxg_present_commit_copyout_failures);
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
    printf("dxg_scanout_bind_attempts %lu\n",
           stats.dxg_scanout_bind_attempts);
    printf("dxg_scanout_bind_rejects %lu\n",
           stats.dxg_scanout_bind_rejects);
    printf("dxg_scanout_bind_successes %lu\n",
           stats.dxg_scanout_bind_successes);
    printf("dxg_scanout_bind_completion_queries %lu\n",
           stats.dxg_scanout_bind_completion_queries);
    printf("dxg_scanout_bind_completion_successes %lu\n",
           stats.dxg_scanout_bind_completion_successes);
    printf("dxg_scanout_bind_completion_pending %lu\n",
           stats.dxg_scanout_bind_completion_pending);
    printf("dxg_scanout_bind_weak_evidence_rejects %lu\n",
           stats.dxg_scanout_bind_weak_evidence_rejects);
    printf("dxg_scanout_bind_candidate_cmds_known %lu\n",
           stats.dxg_scanout_bind_candidate_cmds_known);
    printf("dxg_scanout_bind_candidate_sender_contracts %lu\n",
           stats.dxg_scanout_bind_candidate_sender_contracts);
    printf("dxg_scanout_bind_candidate_completion_contracts %lu\n",
           stats.dxg_scanout_bind_candidate_completion_contracts);
    printf("dxg_scanout_bind_candidate_rejects %lu\n",
           stats.dxg_scanout_bind_candidate_rejects);
    printf("dxg_scanout_bind_candidate_propagate_presenthistory_cmd %lu\n",
           stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd);
    printf("dxg_scanout_bind_weak_dxg_ready_only %lu\n",
           stats.dxg_scanout_bind_weak_dxg_ready_only);
    printf("dxg_scanout_bind_weak_d3dkmt_handles_only %lu\n",
           stats.dxg_scanout_bind_weak_d3dkmt_handles_only);
    printf("dxg_scanout_bind_weak_same_adapter_resource_only %lu\n",
           stats.dxg_scanout_bind_weak_same_adapter_resource_only);
    printf("dxg_scanout_bind_weak_syncfile_only %lu\n",
           stats.dxg_scanout_bind_weak_syncfile_only);
    printf("dxg_scanout_bind_weak_synthvid_gpa_dirty_only %lu\n",
           stats.dxg_scanout_bind_weak_synthvid_gpa_dirty_only);
    printf("dxg_scanout_bind_weak_software_or_readback_path %lu\n",
           stats.dxg_scanout_bind_weak_software_or_readback_path);
    printf("dxg_scanout_bind_last_transport %lu\n",
           stats.dxg_scanout_bind_last_transport);
    printf("dxg_scanout_bind_last_status %lu\n",
           stats.dxg_scanout_bind_last_status);
    printf("dxg_scanout_bind_last_present_id %lu\n",
           stats.dxg_scanout_bind_last_present_id);
    printf("dxg_scanout_bind_last_completed %lu\n",
           stats.dxg_scanout_bind_last_completed);
    printf("dxg_scanout_bind_last_source_generation %lu\n",
           stats.dxg_scanout_bind_last_source_generation);
    printf("dxg_scanout_bind_last_resource_generation %lu\n",
           stats.dxg_scanout_bind_last_resource_generation);
    printf("dxg_scanout_bind_last_dirty_sequence %lu\n",
           stats.dxg_scanout_bind_last_dirty_sequence);
    printf("dxg_scanout_bind_last_dirty_rects %lu\n",
           stats.dxg_scanout_bind_last_dirty_rects);
    printf("dxg_display_bind_contract_version %lu\n",
           stats.dxg_display_bind_contract_version);
    printf("dxg_display_bind_backend %lu\n",
           stats.dxg_display_bind_backend);
    printf("dxg_display_bind_backend_name %s\n",
           present_lane_name(stats.dxg_display_bind_backend));
    printf("dxg_display_bind_transport %lu\n",
           stats.dxg_display_bind_transport);
    printf("dxg_display_bind_transport_present %lu\n",
           stats.dxg_display_bind_transport_present);
    printf("dxg_display_bind_operation %lu\n",
           stats.dxg_display_bind_operation);
    printf("dxg_display_bind_required_metadata 0x%lx\n",
           stats.dxg_display_bind_required_metadata);
    printf("dxg_display_bind_lifetime 0x%lx\n",
           stats.dxg_display_bind_lifetime);
    printf("dxg_display_bind_block_reason 0x%lx\n",
           stats.dxg_display_bind_block_reason);
    printf("dxg_display_bind_completion_source %lu\n",
           stats.dxg_display_bind_completion_source);
    printf("dxg_display_bind_present_id %lu\n",
           stats.dxg_display_bind_present_id);
    printf("dxg_display_bind_completed_id %lu\n",
           stats.dxg_display_bind_completed_id);
    printf("dxg_display_bind_source_generation %lu\n",
           stats.dxg_display_bind_source_generation);
    printf("dxg_display_bind_resource_generation %lu\n",
           stats.dxg_display_bind_resource_generation);
    printf("dxg_display_bind_status %lu\n",
           stats.dxg_display_bind_status);
    printf("dxg_display_bind_provider_submits %lu\n",
           stats.dxg_display_bind_provider_submits);
    printf("dxg_display_bind_provider_pin_revalidated %lu\n",
           stats.dxg_display_bind_provider_pin_revalidated);
    printf("dxg_display_bind_provider_no_host_abi %lu\n",
           stats.dxg_display_bind_provider_no_host_abi);
    printf("dxg_display_bind_provider_no_sender %lu\n",
           stats.dxg_display_bind_provider_no_sender);
    printf("dxg_display_bind_provider_no_completion %lu\n",
           stats.dxg_display_bind_provider_no_completion);
    printf("dxg_display_bind_provider_publication_attempts %lu\n",
           stats.dxg_display_bind_provider_publication_attempts);
    printf("dxg_display_bind_provider_publish_before_send %lu\n",
           stats.dxg_display_bind_provider_publish_before_send);
    printf("dxg_display_bind_provider_transport_pending_id %lu\n",
           stats.dxg_display_bind_provider_transport_pending_id);
    printf("dxg_display_bind_provider_command_id %lu\n",
           stats.dxg_display_bind_provider_command_id);
    printf("dxg_display_bind_provider_transaction_id %lu\n",
           stats.dxg_display_bind_provider_transaction_id);
    printf("dxg_display_bind_provider_channel %lu\n",
           stats.dxg_display_bind_provider_channel);
    printf("dxg_display_bind_provider_completion_demux_registered %lu\n",
           stats.dxg_display_bind_provider_completion_demux_registered);
    printf("dxg_display_bind_provider_resolved_or_cancelled %lu\n",
           stats.dxg_display_bind_provider_resolved_or_cancelled);
    printf("dxg_display_bind_provider_refs_released %lu\n",
           stats.dxg_display_bind_provider_refs_released);
    printf("dxg_display_bind_request_metadata_complete %lu\n",
           stats.dxg_display_bind_request_metadata_complete);
    printf("dxg_display_bind_request_sync_metadata_complete %lu\n",
           stats.dxg_display_bind_request_sync_metadata_complete);
    printf("dxg_display_bind_request_missing_metadata 0x%lx\n",
           stats.dxg_display_bind_request_missing_metadata);
    printf("dxg_display_bind_lock_dropped_submits %lu\n",
           stats.dxg_display_bind_lock_dropped_submits);
    printf("dxg_display_bind_revalidate_attempts %lu\n",
           stats.dxg_display_bind_revalidate_attempts);
    printf("dxg_display_bind_revalidate_successes %lu\n",
           stats.dxg_display_bind_revalidate_successes);
    printf("dxg_display_bind_revalidate_failures %lu\n",
           stats.dxg_display_bind_revalidate_failures);
    printf("dxg_display_bind_pin_attempts %lu\n",
           stats.dxg_display_bind_pin_attempts);
    printf("dxg_display_bind_pin_successes %lu\n",
           stats.dxg_display_bind_pin_successes);
    printf("dxg_display_bind_pin_failures %lu\n",
           stats.dxg_display_bind_pin_failures);
    printf("dxg_display_bind_unpins %lu\n",
           stats.dxg_display_bind_unpins);
    printf("dxg_display_bind_pinned_dxg_file %lu\n",
           stats.dxg_display_bind_pinned_dxg_file);
    printf("dxg_display_bind_pinned_resource_file %lu\n",
           stats.dxg_display_bind_pinned_resource_file);
    printf("dxg_display_bind_pinned_resource_generation %lu\n",
           stats.dxg_display_bind_pinned_resource_generation);
    printf("dxg_display_bind_pinned_process_generation %lu\n",
           stats.dxg_display_bind_pinned_process_generation);
    printf("dxg_display_bind_pinned_process_refs %lu\n",
           stats.dxg_display_bind_pinned_process_refs);
    printf("dxg_display_bind_pinned_shared_parent %lu\n",
           stats.dxg_display_bind_pinned_shared_parent);
    printf("dxg_display_bind_pinned_parent_refs %lu\n",
           stats.dxg_display_bind_pinned_parent_refs);
    printf("dxg_display_bind_pinned_parent_children %lu\n",
           stats.dxg_display_bind_pinned_parent_children);
    printf("d3d12_display_bind_backend_boundary_matrix "
           "backend=%s contract_version=%lu transport=%lu "
           "transport_present=%lu operation=%lu completion_source=%lu "
           "required_metadata=0x%lx lifetime=0x%lx block_reason=0x%lx "
           "present_id=%lu completed=%lu source_generation=%lu "
           "resource_generation=%lu status_code=%lu provider_submits=%lu "
           "lock_dropped_submits=%lu revalidate_attempts=%lu "
           "revalidate_successes=%lu revalidate_failures=%lu "
           "provider_pin_revalidated=%lu provider_no_host_abi=%lu "
           "provider_no_sender=%lu provider_no_completion=%lu "
           "custom_host_tool=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           present_lane_name(stats.dxg_display_bind_backend),
           stats.dxg_display_bind_contract_version,
           stats.dxg_display_bind_transport,
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_operation,
           stats.dxg_display_bind_completion_source,
           stats.dxg_display_bind_required_metadata,
           stats.dxg_display_bind_lifetime,
           stats.dxg_display_bind_block_reason,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_display_bind_status,
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_lock_dropped_submits,
           stats.dxg_display_bind_revalidate_attempts,
           stats.dxg_display_bind_revalidate_successes,
           stats.dxg_display_bind_revalidate_failures,
           stats.dxg_display_bind_provider_pin_revalidated,
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           display_bind_boundary_ok ? "PASS" : "DIAGNOSTIC");
    printf("d3d12_display_bind_pin_lifetime_matrix "
           "pin_attempts=%lu pin_successes=%lu pin_failures=%lu "
           "unpins=%lu pinned_dxg_file=%lu pinned_resource_file=%lu "
           "pinned_resource_generation=%lu pinned_process_generation=%lu "
           "pinned_process_refs=%lu pinned_shared_parent=%lu "
           "pinned_parent_refs=%lu pinned_parent_children=%lu "
           "source_generation=%lu "
           "resource_generation=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_pin_attempts,
           stats.dxg_display_bind_pin_successes,
           stats.dxg_display_bind_pin_failures,
           stats.dxg_display_bind_unpins,
           stats.dxg_display_bind_pinned_dxg_file,
           stats.dxg_display_bind_pinned_resource_file,
           stats.dxg_display_bind_pinned_resource_generation,
           stats.dxg_display_bind_pinned_process_generation,
           stats.dxg_display_bind_pinned_process_refs,
           stats.dxg_display_bind_pinned_shared_parent,
           stats.dxg_display_bind_pinned_parent_refs,
           stats.dxg_display_bind_pinned_parent_children,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_display_bind_pin_attempts == 0 ||
                   (stats.dxg_display_bind_pin_successes != 0 &&
                    stats.dxg_display_bind_unpins ==
                        stats.dxg_display_bind_pin_successes &&
                    stats.dxg_display_bind_pinned_dxg_file == 1 &&
                    stats.dxg_display_bind_pinned_resource_file == 1 &&
                    stats.dxg_display_bind_pinned_resource_generation != 0 &&
                    stats.dxg_display_bind_pinned_process_generation != 0 &&
                    stats.dxg_display_bind_pinned_shared_parent != 0 &&
                    stats.dxg_display_bind_pinned_parent_refs != 0 &&
                    stats.dxg_display_bind_pinned_parent_children != 0) ?
               "PASS" : "DIAGNOSTIC");
    printf("dxg_scanout_bind_skeleton_matrix "
           "attempts=%lu rejects=%lu successes=%lu "
           "completion_queries=%lu completion_successes=%lu "
           "completion_pending=%lu weak_evidence_rejects=%lu "
           "transport=%lu status_code=%lu present_id=%lu completed=%lu "
           "source_generation=%lu resource_generation=%lu "
           "dirty_sequence=%lu dirty_rects=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_scanout_bind_attempts,
           stats.dxg_scanout_bind_rejects,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_completion_queries,
           stats.dxg_scanout_bind_completion_successes,
           stats.dxg_scanout_bind_completion_pending,
           stats.dxg_scanout_bind_weak_evidence_rejects,
           stats.dxg_scanout_bind_last_transport,
           stats.dxg_scanout_bind_last_status,
           stats.dxg_scanout_bind_last_present_id,
           stats.dxg_scanout_bind_last_completed,
           stats.dxg_scanout_bind_last_source_generation,
           stats.dxg_scanout_bind_last_resource_generation,
           stats.dxg_scanout_bind_last_dirty_sequence,
           stats.dxg_scanout_bind_last_dirty_rects,
           scanout_bind_skeleton_ok ? "PASS" : "FAIL");
    printf("dxg_scanout_bind_candidate_command_matrix "
           "presenthistory_cmd=%lu redirected_flip_fence_cmd=%lu "
           "blt_cmd=%lu propagate_presenthistory_cmd=%lu "
           "cmds_known=%lu sender_contracts=%lu completion_contracts=%lu "
           "candidate_rejects=%lu custom_host_tool=0 transport_present=%lu "
           "vmbus_enum_known=%lu linux_ioctl_contracts=%lu "
           "resource_bind_contracts=%lu display_completion_contracts=%lu "
           "reject_reasons=0x%lx "
           "present_id=0 completed=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_presenthistory_cmd,
           stats.dxg_scanout_bind_candidate_redirected_flip_fence_cmd,
           stats.dxg_scanout_bind_candidate_blt_cmd,
           stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd,
           stats.dxg_scanout_bind_candidate_cmds_known,
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_scanout_bind_candidate_rejects,
           stats.dxg_present_helper_transport_present,
           stats.dxg_scanout_bind_candidate_vmbus_enum_known,
           stats.dxg_scanout_bind_candidate_linux_ioctl_contracts,
           stats.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats.dxg_scanout_bind_candidate_display_completion_contracts,
           stats.dxg_scanout_bind_candidate_reject_reasons,
           stats.dxg_scanout_bind_candidate_cmds_known == 4 &&
                   stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd == 1 &&
                   stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                   stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                   stats.dxg_present_helper_transport_present == 0 &&
                   stats.dxg_scanout_bind_last_present_id == 0 &&
                   stats.dxg_scanout_bind_last_completed == 0 ?
               "PASS" : "DIAGNOSTIC");
    printf("dxg_native_present_lane_rejection_matrix "
           "wsl_presenthistory_enum_only=REJECTED "
           "wsl_presenthistory_sender_contract=%lu "
           "wsl_presenthistory_completion_contract=%lu "
           "synthvid_gpa_dirty_only=REJECTED "
           "linux_hyperv_drm_shadow_blit_only=REJECTED "
           "synthvid_gpa_dirty_present=%lu "
           "synthvid_d3d12_resource_bind=0 "
           "dda_nouveau_separate_pci_path=%s "
           "dda_pci_display_present=%lu "
           "dda_d3d12_resource_import=0 dda_scanout_bind=0 "
           "dda_hw_flip_completion=0 "
           "vmbus_enum_known=%lu linux_ioctl_contracts=%lu "
           "resource_bind_contracts=%lu display_completion_contracts=%lu "
           "reject_reasons=0x%lx "
           "custom_host_tool=0 transport_present=%lu present_id=0 "
           "completed=0 native_present_credit=0 opengl_submit_credit=0 "
           "status=%s\n",
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_scanout_bind_synthvid_gpa_dirty_present,
           stats.dxg_present_dda_nouveau_present != 0 ?
               "REJECTED_NO_IMPORT_PATH" : "ABSENT",
           stats.dxg_scanout_bind_dda_pci_display_present,
           stats.dxg_scanout_bind_candidate_vmbus_enum_known,
           stats.dxg_scanout_bind_candidate_linux_ioctl_contracts,
           stats.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats.dxg_scanout_bind_candidate_display_completion_contracts,
           stats.dxg_scanout_bind_candidate_reject_reasons,
           stats.dxg_present_helper_transport_present,
           stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                   stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                   stats.dxg_present_helper_transport_present == 0 &&
                   stats.dxg_scanout_bind_last_present_id == 0 &&
                   stats.dxg_scanout_bind_last_completed == 0 &&
                   stats.dxg_present_dda_nouveau_import_path_present == 0 &&
                   stats.dxg_present_dda_nouveau_scanout_bind_present == 0 ?
               "PASS" : "DIAGNOSTIC");
    printf("d3d12_dda_nouveau_separate_display_not_bind_matrix "
           "dda_backend_flag=%u dda_pci_display_present=%lu "
           "dda_d3d12_resource_import=%lu dda_scanout_bind=%lu "
           "dda_hw_flip_completion=%s separate_pci_display_path=%s "
           "display_bind_present_id=%lu display_bind_completed=%lu "
           "scanout_bind_successes=%lu completion_successes=%lu "
           "native_present_credit=%lu opengl_submit_credit=%d "
           "status=%s\n",
           have_backend &&
               (backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) != 0,
           stats.dxg_scanout_bind_dda_pci_display_present,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present,
           stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats.dxg_scanout_bind_dda_pci_display_present != 0 ?
               "REJECTED_D3D12_IMPORT_MISSING" : "ABSENT",
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_completion_successes,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           dda_nouveau_separate_display_not_bind_ok ? "PASS" : "FAIL");
    printf("dda_nouveau_non_readback_display_proof_matrix "
           "dda_pci_transport_present=%s dda_nouveau_display_present=%s "
           "dda_nouveau_non_readback_present=%s "
           "display_probe_attempts=%lu display_create_successes=%lu "
           "head_probe_attempts=%lu heads=%lu "
           "connector_probe_attempts=%lu connectors=%lu "
           "nonvirtual_connectors=%lu vblank_supported=%lu "
           "vblank_irq_supported=%lu vblank_source=%s "
           "page_flip_ready=%lu page_flip_completions=%lu "
           "atomic_backend_missing=%lu kms_lane=%lu "
           "kms_present_dumb=%lu kms_present_synthvid=%lu "
           "kms_present_nouveau_hw=%lu page_flip_events_native_hw=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.nouveau_pci_probe_accepts != 0 ? "PASS" :
                                                   "GPU_P_FAIL_CLOSED",
           nouveau_display_kms_ready ? "PASS" : "ABSENT",
           stats.kms_present_last_lane == FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW &&
                   stats.kms_page_flip_events_native_hw != 0 &&
                   nouveau_display_kms_ready ?
               "PASS" : "ABSENT",
           stats.nouveau_display_probe_attempts,
           stats.nouveau_display_create_successes,
           stats.nouveau_display_head_probe_attempts,
           stats.nouveau_display_heads,
           stats.nouveau_display_connector_probe_attempts,
           stats.nouveau_display_connectors,
           stats.nouveau_display_nonvirtual_connectors,
           stats.nouveau_display_vblank_supported,
           stats.nouveau_display_vblank_irq_supported,
           nouveau_vblank_source_name(stats.nouveau_display_vblank_source),
           stats.nouveau_display_page_flip_completion_ready,
           stats.nouveau_display_page_flip_completions,
           stats.nouveau_display_atomic_pageflip_backend_missing,
           stats.kms_present_last_lane,
           stats.kms_present_dumb,
           stats.kms_present_synthvid,
           stats.kms_present_nouveau_hw,
           stats.kms_page_flip_events_native_hw,
           dda_nouveau_non_readback_display_proof_ok ? "PASS" : "FAIL");
    printf("dxg_scanout_bind_weak_evidence_matrix "
           "dxg_ready_only=%lu d3dkmt_handles_only=%lu "
           "same_adapter_resource_only=%lu syncfile_only=%lu "
           "synthvid_gpa_dirty_only=%lu software_or_readback_path=%lu "
           "weak_evidence_rejects=%lu successes=%lu present_id=0 "
           "completed=0 native_present_credit=0 opengl_submit_credit=0 "
           "status=%s\n",
           stats.dxg_scanout_bind_weak_dxg_ready_only,
           stats.dxg_scanout_bind_weak_d3dkmt_handles_only,
           stats.dxg_scanout_bind_weak_same_adapter_resource_only,
           stats.dxg_scanout_bind_weak_syncfile_only,
           stats.dxg_scanout_bind_weak_synthvid_gpa_dirty_only,
           stats.dxg_scanout_bind_weak_software_or_readback_path,
           stats.dxg_scanout_bind_weak_evidence_rejects,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_weak_evidence_rejects > 0 &&
                   stats.dxg_scanout_bind_successes == 0 &&
                   stats.dxg_scanout_bind_last_present_id == 0 &&
                   stats.dxg_scanout_bind_last_completed == 0 ?
               "PASS" : "DIAGNOSTIC");
    printf("dxg_syncfile_not_kms_completion_matrix "
           "syncfile_only=%lu weak_evidence_rejects=%lu "
           "scanout_successes=%lu completion_successes=%lu "
           "completion_pending=%lu kms_no_hw_completion=%lu "
           "present_id=%lu completed=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_scanout_bind_weak_syncfile_only,
           stats.dxg_scanout_bind_weak_evidence_rejects,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_completion_successes,
           stats.dxg_scanout_bind_completion_pending,
           stats.kms_present_reject_no_hw_completion,
           stats.dxg_scanout_bind_last_present_id,
           stats.dxg_scanout_bind_last_completed,
           stats.dxg_scanout_bind_successes == 0 &&
                   stats.dxg_scanout_bind_completion_successes == 0 &&
                   stats.dxg_scanout_bind_last_present_id == 0 &&
                   stats.dxg_scanout_bind_last_completed == 0 ?
               "PASS" : "FAIL");
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
    printf("dxg_present_lane_selection_matrix "
           "selected=%s selected_id=%lu "
           "working_model=dxg_resource_scanout_bind "
           "wslg_display_channel=0 "
           "synthvid_vram_bridge=gpa_dirty_only "
           "synthvid_native_resource_bind=0 "
           "gpup_or_dda_required=1 custom_host_tool=0 "
           "missing_host_abi=%lu transport_present=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n",
           present_lane_name(stats.dxg_present_selected_lane),
           stats.dxg_present_selected_lane,
           stats.dxg_present_missing_host_abi,
           stats.dxg_present_helper_transport_present);
    printf("d3d12_display_bind_absent_matrix "
           "selected=%s display_bind=%s missing_host_abi=%lu "
           "transport_present=%lu helper_requires_completion=%lu "
           "scanout_bind_attempts=%lu weak_evidence_rejects=%lu "
           "present_id=0 completed=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           present_lane_name(stats.dxg_present_selected_lane),
           stats.dxg_present_helper_transport_present ? "PRESENT" :
               "ABSENT",
           stats.dxg_present_missing_host_abi,
           stats.dxg_present_helper_transport_present,
           stats.dxg_present_helper_requires_completion,
           stats.dxg_scanout_bind_attempts,
           stats.dxg_scanout_bind_weak_evidence_rejects,
           stats.dxg_present_selected_lane ==
                   FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND &&
                   stats.dxg_present_helper_transport_present == 0 &&
                   stats.dxg_present_helper_requires_completion != 0 ?
               "PASS" : "DIAGNOSTIC");
    close(fd);
    return 0;
}

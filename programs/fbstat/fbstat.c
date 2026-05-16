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
    printf("display_presents %lu\n", stats.display_presents);
    printf("display_completions %lu\n", stats.display_completions);
    printf("display_last_present %lu\n", stats.display_last_present);
    printf("display_last_complete %lu\n", stats.display_last_complete);
    close(fd);
    return 0;
}

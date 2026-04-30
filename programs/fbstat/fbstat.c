#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/uabi/fcntl.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    struct fb_gpu_stats stats;
    int fd;

    (void)argc;
    (void)argv;

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
    close(fd);
    return 0;
}

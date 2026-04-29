#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/uabi/fcntl.h"
#include "user/user.h"

#define VIRGL_CCMD_NOP 0
#define VIRGL_CMD0(cmd, obj, len) ((cmd) | ((obj) << 8) | ((len) << 16))

int main(int argc, char **argv)
{
    int fd;
    uint32 nop = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, 0);
    struct fb_gpu_virgl_ctx ctx = {0};
    struct fb_gpu_virgl_submit submit = {0};
    struct fb_gpu_virgl_fence fence = {0};

    (void)argc;
    (void)argv;

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        printf("virgltest: open /dev/fb0 failed\n");
        return 1;
    }

    strcpy(ctx.debug_name, "virgltest");
    if (ioctl(fd, FB_GPU_VIRGL_CTX_CREATE, &ctx) < 0 || ctx.ctx_id == 0) {
        printf("virgltest: FB_GPU_VIRGL_CTX_CREATE failed\n");
        close(fd);
        return 1;
    }

    submit.ctx_id = ctx.ctx_id;
    submit.cmd_size = sizeof(nop);
    submit.cmd = (uint64)&nop;
    if (ioctl(fd, FB_GPU_VIRGL_SUBMIT, &submit) < 0 ||
        submit.fence == 0 || submit.signaled < submit.fence) {
        printf("virgltest: FB_GPU_VIRGL_SUBMIT failed ctx=%u fence=%lu signaled=%lu\n",
               ctx.ctx_id, submit.fence, submit.signaled);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }

    fence.flags = FB_GPU_VIRGL_FENCE_WAIT;
    fence.wait_for = submit.fence;
    if (ioctl(fd, FB_GPU_VIRGL_FENCE, &fence) < 0 ||
        fence.signaled < submit.fence) {
        printf("virgltest: FB_GPU_VIRGL_FENCE failed fence=%lu signaled=%lu\n",
               submit.fence, fence.signaled);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }

    if (ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx) < 0) {
        printf("virgltest: FB_GPU_VIRGL_CTX_DESTROY failed ctx=%u\n",
               ctx.ctx_id);
        close(fd);
        return 1;
    }

    printf("virgltest: ctx=%u fence=%lu signaled=%lu\n",
           ctx.ctx_id, submit.fence, fence.signaled);
    close(fd);
    return 0;
}

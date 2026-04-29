#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/uabi/fcntl.h"
#include "user/user.h"

#define VIRGL_CCMD_NOP 0
#define VIRGL_CMD0(cmd, obj, len) ((cmd) | ((obj) << 8) | ((len) << 16))
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#define VIRGL_BIND_DISPLAY_TARGET (1u << 7)
#define PIPE_TEXTURE_2D 2

int main(int argc, char **argv)
{
    int fd;
    uint32 nop = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, 0);
    uint8 caps_buf[512];
    struct fb_gpu_virgl_caps caps = {0};
    struct fb_gpu_virgl_ctx ctx = {0};
    struct fb_gpu_virgl_resource_create res = {0};
    struct fb_gpu_virgl_resource_destroy res_destroy = {0};
    struct fb_gpu_virgl_transfer transfer = {0};
    struct fb_gpu_virgl_submit submit = {0};
    struct fb_gpu_virgl_fence fence = {0};
    uint32 pattern = 0xff336699;

    (void)argc;
    (void)argv;

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        printf("virgltest: open /dev/fb0 failed\n");
        return 1;
    }

    caps.data = (uint64)caps_buf;
    caps.size = sizeof(caps_buf);
    if (ioctl(fd, FB_GPU_VIRGL_GET_CAPS, &caps) < 0 || caps.size == 0) {
        printf("virgltest: FB_GPU_VIRGL_GET_CAPS failed size=%u\n",
               caps.size);
        close(fd);
        return 1;
    }

    strcpy(ctx.debug_name, "virgltest");
    if (ioctl(fd, FB_GPU_VIRGL_CTX_CREATE, &ctx) < 0 || ctx.ctx_id == 0) {
        printf("virgltest: FB_GPU_VIRGL_CTX_CREATE failed\n");
        close(fd);
        return 1;
    }

    res.ctx_id = ctx.ctx_id;
    res.target = PIPE_TEXTURE_2D;
    res.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    res.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_DISPLAY_TARGET;
    res.width = 16;
    res.height = 16;
    res.depth = 1;
    res.array_size = 1;
    if (ioctl(fd, FB_GPU_VIRGL_RESOURCE_CREATE, &res) < 0 ||
        res.resource_id == 0 || res.addr == 0 || res.size == 0) {
        printf("virgltest: FB_GPU_VIRGL_RESOURCE_CREATE failed ctx=%u res=%u addr=%lu size=%lu\n",
               ctx.ctx_id, res.resource_id, res.addr, res.size);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }

    ((volatile uint32 *)res.addr)[0] = pattern;
    transfer.resource_id = res.resource_id;
    transfer.w = res.width;
    transfer.h = res.height;
    transfer.d = 1;
    transfer.stride = res.width * sizeof(uint32);
    transfer.layer_stride = transfer.stride * res.height;
    if (ioctl(fd, FB_GPU_VIRGL_TRANSFER_TO_HOST, &transfer) < 0) {
        printf("virgltest: FB_GPU_VIRGL_TRANSFER_TO_HOST failed res=%u\n",
               res.resource_id);
        (void)munmap((void *)res.addr, res.size);
        res_destroy.resource_id = res.resource_id;
        (void)ioctl(fd, FB_GPU_VIRGL_RESOURCE_DESTROY, &res_destroy);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }
    ((volatile uint32 *)res.addr)[0] = 0;
    if (ioctl(fd, FB_GPU_VIRGL_TRANSFER_FROM_HOST, &transfer) < 0 ||
        ((volatile uint32 *)res.addr)[0] != pattern) {
        printf("virgltest: FB_GPU_VIRGL_TRANSFER_FROM_HOST failed res=%u pixel=0x%x\n",
               res.resource_id, ((volatile uint32 *)res.addr)[0]);
        (void)munmap((void *)res.addr, res.size);
        res_destroy.resource_id = res.resource_id;
        (void)ioctl(fd, FB_GPU_VIRGL_RESOURCE_DESTROY, &res_destroy);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
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
        (void)munmap((void *)res.addr, res.size);
        res_destroy.resource_id = res.resource_id;
        (void)ioctl(fd, FB_GPU_VIRGL_RESOURCE_DESTROY, &res_destroy);
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
        (void)munmap((void *)res.addr, res.size);
        res_destroy.resource_id = res.resource_id;
        (void)ioctl(fd, FB_GPU_VIRGL_RESOURCE_DESTROY, &res_destroy);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }

    res_destroy.resource_id = res.resource_id;
    (void)munmap((void *)res.addr, res.size);
    if (ioctl(fd, FB_GPU_VIRGL_RESOURCE_DESTROY, &res_destroy) < 0) {
        printf("virgltest: FB_GPU_VIRGL_RESOURCE_DESTROY failed res=%u\n",
               res.resource_id);
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

    printf("virgltest: ctx=%u res=%u map=%lu fence=%lu signaled=%lu capset=%u version=%u size=%u\n",
           ctx.ctx_id, res.resource_id, res.addr, submit.fence,
           fence.signaled, caps.capset_id, caps.capset_version, caps.size);
    close(fd);
    return 0;
}

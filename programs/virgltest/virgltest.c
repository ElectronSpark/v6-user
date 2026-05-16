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

static int submit_nop(int fd, uint32 ctx_id, uint64 *fence_out,
                      uint64 *signaled_out)
{
    uint32 nop = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, 0);
    struct fb_gpu_virgl_submit submit = {0};

    submit.ctx_id = ctx_id;
    submit.cmd_size = sizeof(nop);
    submit.cmd = (uint64)&nop;
    if (ioctl(fd, FB_GPU_VIRGL_SUBMIT, &submit) < 0 ||
        submit.fence == 0 || submit.signaled < submit.fence)
        return -1;
    if (fence_out)
        *fence_out = submit.fence;
    if (signaled_out)
        *signaled_out = submit.signaled;
    return 0;
}

static int async_submit_test(void)
{
    int fd;
    uint32 nop = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, 0);
    struct fb_gpu_virgl_ctx ctx = {0};
    struct fb_gpu_virgl_submit submit = {0};
    struct fb_gpu_virgl_fence fence = {0};
    uint64 initial_signaled;
    uint64 final_signaled;

    fd = open("/dev/gpu0", O_RDWR);
    if (fd < 0) {
        printf("virgltest: open /dev/gpu0 failed\n");
        return 1;
    }

    strcpy(ctx.debug_name, "virgltest-async");
    if (ioctl(fd, FB_GPU_VIRGL_CTX_CREATE, &ctx) < 0 || ctx.ctx_id == 0) {
        printf("virgltest: async-submit ctx create failed\n");
        close(fd);
        return 1;
    }

    submit.ctx_id = ctx.ctx_id;
    submit.flags = FB_GPU_VIRGL_SUBMIT_ASYNC;
    submit.cmd_size = sizeof(nop);
    submit.cmd = (uint64)&nop;
    if (ioctl(fd, FB_GPU_VIRGL_SUBMIT, &submit) < 0 ||
        submit.fence == 0) {
        printf("virgltest: async-submit failed ctx=%u fence=%lu signaled=%lu\n",
               ctx.ctx_id, submit.fence, submit.signaled);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }

    initial_signaled = submit.signaled;
    fence.flags = FB_GPU_VIRGL_FENCE_WAIT;
    fence.wait_for = submit.fence;
    if (ioctl(fd, FB_GPU_VIRGL_FENCE, &fence) < 0 ||
        fence.signaled < submit.fence) {
        printf("virgltest: async-submit fence wait failed fence=%lu signaled=%lu\n",
               submit.fence, fence.signaled);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }
    final_signaled = fence.signaled;

    if (submit_nop(fd, ctx.ctx_id, NULL, NULL) < 0) {
        printf("virgltest: sync submit after async failed ctx=%u\n", ctx.ctx_id);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }

    if (ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx) < 0) {
        printf("virgltest: async-submit ctx destroy failed ctx=%u\n",
               ctx.ctx_id);
        close(fd);
        return 1;
    }

    printf("virgltest: async-submit queued ctx=%u fence=%lu initial_signaled=%lu final_signaled=%lu completed_inline=%d\n",
           ctx.ctx_id, submit.fence, initial_signaled, final_signaled,
           initial_signaled >= submit.fence);
    close(fd);
    return 0;
}

static int bad_submit_test(void)
{
    int fd;
    uint32 bad = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, 0);
    struct fb_gpu_virgl_ctx ctx = {0};
    struct fb_gpu_virgl_ctx fresh = {0};
    struct fb_gpu_virgl_submit submit = {0};
    struct fb_gpu_virgl_resource_create res = {0};
    uint64 fence = 0;
    uint64 signaled = 0;

    fd = open("/dev/gpu0", O_RDWR);
    if (fd < 0) {
        printf("virgltest: open /dev/gpu0 failed\n");
        return 1;
    }

    strcpy(ctx.debug_name, "virgltest-bad");
    if (ioctl(fd, FB_GPU_VIRGL_CTX_CREATE, &ctx) < 0 || ctx.ctx_id == 0) {
        printf("virgltest: bad-submit ctx create failed\n");
        close(fd);
        return 1;
    }

    submit.ctx_id = ctx.ctx_id;
    submit.flags = FB_GPU_VIRGL_SUBMIT_FORCE_FAIL;
    submit.cmd_size = sizeof(bad);
    submit.cmd = (uint64)&bad;
    if (ioctl(fd, FB_GPU_VIRGL_SUBMIT, &submit) >= 0) {
        printf("virgltest: bad submit unexpectedly succeeded ctx=%u fence=%lu\n",
               ctx.ctx_id, submit.fence);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }

    if (submit_nop(fd, ctx.ctx_id, NULL, NULL) >= 0) {
        printf("virgltest: failed context accepted later submit ctx=%u\n",
               ctx.ctx_id);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
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
    if (ioctl(fd, FB_GPU_VIRGL_RESOURCE_CREATE, &res) >= 0) {
        struct fb_gpu_virgl_resource_destroy destroy = {
            .resource_id = res.resource_id,
        };
        printf("virgltest: failed context accepted resource create ctx=%u res=%u\n",
               ctx.ctx_id, res.resource_id);
        if (res.addr != 0 && res.size != 0)
            (void)munmap((void *)res.addr, res.size);
        (void)ioctl(fd, FB_GPU_VIRGL_RESOURCE_DESTROY, &destroy);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }

    if (ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx) < 0) {
        printf("virgltest: failed context destroy failed ctx=%u\n",
               ctx.ctx_id);
        close(fd);
        return 1;
    }

    strcpy(fresh.debug_name, "virgltest-fresh");
    if (ioctl(fd, FB_GPU_VIRGL_CTX_CREATE, &fresh) < 0 ||
        fresh.ctx_id == 0 || submit_nop(fd, fresh.ctx_id, &fence, &signaled) < 0) {
        printf("virgltest: fresh context did not recover after bad submit\n");
        if (fresh.ctx_id != 0)
            (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &fresh);
        close(fd);
        return 1;
    }
    if (ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &fresh) < 0) {
        printf("virgltest: fresh context destroy failed ctx=%u\n",
               fresh.ctx_id);
        close(fd);
        return 1;
    }

    printf("virgltest: bad-submit isolated ctx=%u fresh=%u fence=%lu signaled=%lu\n",
           ctx.ctx_id, fresh.ctx_id, fence, signaled);
    close(fd);
    return 0;
}

static int expect_submit_fail(int fd, const char *name,
                              struct fb_gpu_virgl_submit *submit)
{
    if (ioctl(fd, FB_GPU_VIRGL_SUBMIT, submit) >= 0) {
        printf("virgltest: invalid-submit %s unexpectedly succeeded fence=%lu signaled=%lu\n",
               name, submit->fence, submit->signaled);
        return 1;
    }
    printf("virgltest: invalid-submit %s rejected\n", name);
    return 0;
}

static int invalid_submit_test(void)
{
    int fd;
    uint32 nop = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, 0);
    struct fb_gpu_virgl_ctx ctx = {0};
    struct fb_gpu_virgl_ctx bad_ctx = {0};
    struct fb_gpu_virgl_submit submit = {0};
    uint64 fence = 0;
    uint64 signaled = 0;
    int ret = 1;

    fd = open("/dev/gpu0", O_RDWR);
    if (fd < 0) {
        printf("virgltest: open /dev/gpu0 failed\n");
        return 1;
    }

    bad_ctx.flags = 1;
    strcpy(bad_ctx.debug_name, "virgltest-invalid");
    if (ioctl(fd, FB_GPU_VIRGL_CTX_CREATE, &bad_ctx) >= 0) {
        printf("virgltest: invalid ctx flags unexpectedly succeeded ctx=%u\n",
               bad_ctx.ctx_id);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &bad_ctx);
        goto out;
    }
    printf("virgltest: invalid ctx flags rejected\n");

    strcpy(ctx.debug_name, "virgltest-invalid-ok");
    if (ioctl(fd, FB_GPU_VIRGL_CTX_CREATE, &ctx) < 0 || ctx.ctx_id == 0) {
        printf("virgltest: invalid-submit ctx create failed\n");
        goto out;
    }

    memset(&submit, 0, sizeof(submit));
    submit.cmd = (uint64)&nop;
    submit.cmd_size = sizeof(nop);
    if (expect_submit_fail(fd, "zero_ctx", &submit) != 0)
        goto out_ctx;

    memset(&submit, 0, sizeof(submit));
    submit.ctx_id = ctx.ctx_id;
    submit.cmd_size = sizeof(nop);
    if (expect_submit_fail(fd, "null_cmd", &submit) != 0)
        goto out_ctx;

    memset(&submit, 0, sizeof(submit));
    submit.ctx_id = ctx.ctx_id;
    submit.cmd = (uint64)&nop;
    submit.cmd_size = 2;
    if (expect_submit_fail(fd, "unaligned_size", &submit) != 0)
        goto out_ctx;

    memset(&submit, 0, sizeof(submit));
    submit.ctx_id = ctx.ctx_id;
    submit.cmd = (uint64)&nop;
    submit.cmd_size = 262148;
    if (expect_submit_fail(fd, "oversize", &submit) != 0)
        goto out_ctx;

    memset(&submit, 0, sizeof(submit));
    submit.ctx_id = ctx.ctx_id + 1000;
    submit.cmd = (uint64)&nop;
    submit.cmd_size = sizeof(nop);
    if (expect_submit_fail(fd, "foreign_ctx", &submit) != 0)
        goto out_ctx;

    if (submit_nop(fd, ctx.ctx_id, &fence, &signaled) < 0) {
        printf("virgltest: valid submit failed after invalid-submit rejects ctx=%u\n",
               ctx.ctx_id);
        goto out_ctx;
    }

    printf("virgltest: invalid-submit rejected invalid ioctls ctx=%u fence=%lu signaled=%lu\n",
           ctx.ctx_id, fence, signaled);
    ret = 0;

out_ctx:
    if (ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx) < 0) {
        printf("virgltest: invalid-submit ctx destroy failed ctx=%u\n",
               ctx.ctx_id);
        ret = 1;
    }
out:
    close(fd);
    return ret;
}

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
    struct fb_gpu_virgl_fence_export_fd fence_export = {0};
    struct fb_gpu_virgl_fence_query_fd fence_query = {0};
    uint32 pattern = 0xff336699;

    if (argc > 1 && strcmp(argv[1], "--bad-submit") == 0)
        return bad_submit_test();
    if (argc > 1 && strcmp(argv[1], "--async-submit") == 0)
        return async_submit_test();
    if (argc > 1 && strcmp(argv[1], "--invalid-submit") == 0)
        return invalid_submit_test();

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

    fence_export.fence = submit.fence;
    if (ioctl(fd, FB_GPU_VIRGL_FENCE_EXPORT_FD, &fence_export) < 0 ||
        fence_export.fd < 0 || fence_export.fence != submit.fence ||
        fence_export.signaled < submit.fence) {
        printf("virgltest: FB_GPU_VIRGL_FENCE_EXPORT_FD failed fence=%lu signaled=%lu\n",
               submit.fence, fence_export.signaled);
        (void)munmap((void *)res.addr, res.size);
        res_destroy.resource_id = res.resource_id;
        (void)ioctl(fd, FB_GPU_VIRGL_RESOURCE_DESTROY, &res_destroy);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }
    fence_query.fd = fence_export.fd;
    fence_query.flags = FB_GPU_VIRGL_FENCE_WAIT;
    if (ioctl(fd, FB_GPU_VIRGL_FENCE_QUERY_FD, &fence_query) < 0 ||
        fence_query.fence != submit.fence ||
        fence_query.signaled < submit.fence) {
        printf("virgltest: FB_GPU_VIRGL_FENCE_QUERY_FD failed fence=%lu signaled=%lu\n",
               fence_query.fence, fence_query.signaled);
        close(fence_export.fd);
        (void)munmap((void *)res.addr, res.size);
        res_destroy.resource_id = res.resource_id;
        (void)ioctl(fd, FB_GPU_VIRGL_RESOURCE_DESTROY, &res_destroy);
        (void)ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
        close(fd);
        return 1;
    }
    close(fence_export.fd);

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

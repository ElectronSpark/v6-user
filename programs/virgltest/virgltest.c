#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/uabi/fcntl.h"
#include "user/user.h"

#define VIRGL_CCMD_NOP 0
#define VIRGL_CCMD_CLEAR_TEXTURE 47
#define VIRGL_CCMD_RESOURCE_COPY_REGION 17
#define VIRGL_CMD0(cmd, obj, len) ((cmd) | ((obj) << 8) | ((len) << 16))
#define VIRGL_CLEAR_TEXTURE_SIZE 12
#define VIRGL_CMD_RESOURCE_COPY_REGION_SIZE 13
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#define VIRGL_BIND_DISPLAY_TARGET (1u << 7)
#define VIRGL_BIND_SAMPLER_VIEW (1u << 3)
#define VIRGL_BIND_SHARED (1u << 20)
#define VIRGL_BIND_LINEAR (1u << 22)
#define VIRGL_BIND_SCANOUT (1u << 18)
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

static void fill_copy_pattern(volatile uint32 *pixels, uint32 width,
                              uint32 height)
{
    uint32 colors[5] = {
        0xff336699, 0xffcc5522, 0xff22aa66, 0xff8833cc, 0xffe0d050,
    };
    uint32 xs[5] = {
        width / 2, width / 4, (width * 3) / 4, width / 4,
        (width * 3) / 4,
    };
    uint32 ys[5] = {
        height / 2, height / 4, height / 4, (height * 3) / 4,
        (height * 3) / 4,
    };

    for (uint32 y = 0; y < height; y++) {
        for (uint32 x = 0; x < width; x++)
            pixels[y * width + x] = 0xff101820;
    }
    for (int i = 0; i < 5; i++)
        pixels[ys[i] * width + xs[i]] = colors[i];
}

static int check_copy_pattern(volatile uint32 *src, volatile uint32 *dst,
                              uint32 width, uint32 height)
{
    uint32 xs[5] = {
        width / 2, width / 4, (width * 3) / 4, width / 4,
        (width * 3) / 4,
    };
    uint32 ys[5] = {
        height / 2, height / 4, height / 4, (height * 3) / 4,
        (height * 3) / 4,
    };
    int matches = 0;

    for (int i = 0; i < 5; i++) {
        uint32 off = ys[i] * width + xs[i];
        if ((src[off] & 0x00ffffffu) != 0 &&
            (src[off] & 0x00ffffffu) == (dst[off] & 0x00ffffffu))
            matches++;
    }
    return matches;
}

static void destroy_resource_if_created(int fd,
                                        struct fb_gpu_virgl_resource_create *r)
{
    if (r->addr != 0 && r->size != 0)
        (void)munmap((void *)r->addr, r->size);
    if (r->resource_id != 0) {
        struct fb_gpu_virgl_resource_destroy destroy = {
            .resource_id = r->resource_id,
        };

        (void)ioctl(fd, FB_GPU_VIRGL_RESOURCE_DESTROY, &destroy);
    }
}

static int dmabuf_resource_import_test(void)
{
    int fd;
    struct fb_gpu_virgl_ctx ctx = {0};
    struct fb_gpu_virgl_resource_create res = {0};
    struct fb_gpu_virgl_resource_export_fd export_fd = {0};
    struct fb_gpu_bo_import_fd import_fd = {0};
    struct fb_gpu_bo_info info = {0};
    struct fb_gpu_bo_destroy destroy_bo = {0};
    int ret = 1;

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        printf("virgltest: dmabuf import open /dev/fb0 failed\n");
        return 1;
    }

    strcpy(ctx.debug_name, "virgltest-dmabuf");
    if (ioctl(fd, FB_GPU_VIRGL_CTX_CREATE, &ctx) < 0 || ctx.ctx_id == 0) {
        printf("virgltest: dmabuf import ctx create failed\n");
        goto out;
    }

    export_fd.fd = -1;
    res.ctx_id = ctx.ctx_id;
    res.target = PIPE_TEXTURE_2D;
    res.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    res.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
               VIRGL_BIND_SHARED | VIRGL_BIND_LINEAR;
    res.width = 64;
    res.height = 64;
    res.depth = 1;
    res.array_size = 1;
    if (ioctl(fd, FB_GPU_VIRGL_RESOURCE_CREATE, &res) < 0 ||
        res.resource_id == 0 || res.addr == 0 || res.size == 0) {
        printf("virgltest: dmabuf import resource create failed ctx=%u res=%u addr=%lu size=%lu\n",
               ctx.ctx_id, res.resource_id, res.addr, res.size);
        goto out_ctx;
    }

    export_fd.resource_id = res.resource_id;
    if (ioctl(fd, FB_GPU_VIRGL_RESOURCE_EXPORT_FD, &export_fd) < 0 ||
        export_fd.fd < 0 || export_fd.width != res.width ||
        export_fd.height != res.height || export_fd.pitch == 0 ||
        export_fd.size == 0) {
        printf("virgltest: dmabuf import export failed res=%u fd=%d size=%lu pitch=%u\n",
               res.resource_id, export_fd.fd, export_fd.size,
               export_fd.pitch);
        goto out_res;
    }

    import_fd.fd = export_fd.fd;
    if (ioctl(fd, FB_GPU_BO_IMPORT_FD, &import_fd) < 0 ||
        import_fd.handle == 0 || import_fd.addr == 0 ||
        import_fd.size == 0) {
        printf("virgltest: dmabuf import fd failed fd=%d handle=%u addr=%lu size=%lu\n",
               export_fd.fd, import_fd.handle, import_fd.addr,
               import_fd.size);
        goto out_export_fd;
    }

    info.handle = import_fd.handle;
    if (ioctl(fd, FB_GPU_BO_INFO, &info) < 0) {
        printf("virgltest: dmabuf import info failed handle=%u\n",
               import_fd.handle);
        goto out_import_bo;
    }
    if (info.virtio_resource_id != res.resource_id ||
        info.width != res.width || info.height != res.height ||
        info.pitch != import_fd.pitch || info.size != import_fd.size) {
        printf("virgltest: dmabuf import mismatch res=%u imported_res=%u size=%lu/%lu pitch=%u/%u wh=%ux%u/%ux%u\n",
               res.resource_id, info.virtio_resource_id, info.size,
               import_fd.size, info.pitch, import_fd.pitch, info.width,
               info.height, res.width, res.height);
        goto out_import_bo;
    }

    printf("virgltest: dmabuf-resource-import ok ctx=%u res=%u fd=%d bo=%u imported_resource=%u size=%lu pitch=%u\n",
           ctx.ctx_id, res.resource_id, export_fd.fd, import_fd.handle,
           info.virtio_resource_id, info.size, info.pitch);
    ret = 0;

out_import_bo:
    destroy_bo.handle = import_fd.handle;
    if (destroy_bo.handle != 0 &&
        ioctl(fd, FB_GPU_BO_DESTROY, &destroy_bo) < 0) {
        printf("virgltest: dmabuf import bo destroy failed handle=%u\n",
               destroy_bo.handle);
        ret = 1;
    }
out_export_fd:
    if (export_fd.fd >= 0)
        close(export_fd.fd);
out_res:
    destroy_resource_if_created(fd, &res);
out_ctx:
    if (ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx) < 0) {
        printf("virgltest: dmabuf import ctx destroy failed ctx=%u\n",
               ctx.ctx_id);
        ret = 1;
    }
out:
    close(fd);
    return ret;
}

static int submit_clear_texture(int fd, uint32 ctx_id, uint32 resource_id,
                                uint32 width, uint32 height,
                                uint64 *fence_out, uint64 *signaled_out)
{
    uint32 cmds[VIRGL_CLEAR_TEXTURE_SIZE + 1];
    struct fb_gpu_virgl_submit submit = {0};
    struct fb_gpu_virgl_fence fence = {0};

    memset(cmds, 0, sizeof(cmds));
    cmds[0] = VIRGL_CMD0(VIRGL_CCMD_CLEAR_TEXTURE, 0,
                         VIRGL_CLEAR_TEXTURE_SIZE);
    cmds[1] = resource_id;
    cmds[2] = 0;
    cmds[3] = 0;
    cmds[4] = 0;
    cmds[5] = 0;
    cmds[6] = width;
    cmds[7] = height;
    cmds[8] = 1;
    cmds[9] = 0xff2a7bd8;
    cmds[10] = 0;
    cmds[11] = 0;
    cmds[12] = 0;

    submit.ctx_id = ctx_id;
    submit.cmd_size = sizeof(cmds);
    submit.cmd = (uint64)cmds;
    if (ioctl(fd, FB_GPU_VIRGL_SUBMIT, &submit) < 0 ||
        submit.fence == 0)
        return -1;
    fence.flags = FB_GPU_VIRGL_FENCE_WAIT;
    fence.wait_for = submit.fence;
    if (ioctl(fd, FB_GPU_VIRGL_FENCE, &fence) < 0 ||
        fence.signaled < submit.fence)
        return -1;
    if (fence_out)
        *fence_out = submit.fence;
    if (signaled_out)
        *signaled_out = fence.signaled;
    return 0;
}

static int resource_copy_test(int scanout_dst, int clear_src, int render_bind)
{
    int fd;
    struct fb_gpu_virgl_ctx ctx = {0};
    struct fb_gpu_virgl_resource_create src = {0};
    struct fb_gpu_virgl_resource_create dst = {0};
    struct fb_gpu_virgl_transfer transfer = {0};
    struct fb_gpu_virgl_submit submit = {0};
    struct fb_gpu_virgl_fence fence = {0};
    uint32 cmds[VIRGL_CMD_RESOURCE_COPY_REGION_SIZE + 1];
    uint32 bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
                  VIRGL_BIND_SHARED;
    int matches;
    int ret = 1;

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        printf("virgltest: copy open /dev/fb0 failed\n");
        return 1;
    }

    strcpy(ctx.debug_name, "virgltest-copy");
    if (ioctl(fd, FB_GPU_VIRGL_CTX_CREATE, &ctx) < 0 || ctx.ctx_id == 0) {
        printf("virgltest: copy ctx create failed\n");
        goto out;
    }

    src.ctx_id = ctx.ctx_id;
    src.target = PIPE_TEXTURE_2D;
    src.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    if (!render_bind)
        bind |= VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_LINEAR;
    src.bind = bind;
    src.width = 64;
    src.height = 64;
    src.depth = 1;
    src.array_size = 1;
    if (ioctl(fd, FB_GPU_VIRGL_RESOURCE_CREATE, &src) < 0 ||
        src.resource_id == 0 || src.addr == 0 || src.size == 0) {
        printf("virgltest: copy src create failed ctx=%u res=%u addr=%lu size=%lu\n",
               ctx.ctx_id, src.resource_id, src.addr, src.size);
        goto out_ctx;
    }

    dst = src;
    if (scanout_dst)
        dst.bind |= VIRGL_BIND_SCANOUT;
    dst.resource_id = 0;
    dst.addr = 0;
    dst.size = 0;
    if (ioctl(fd, FB_GPU_VIRGL_RESOURCE_CREATE, &dst) < 0 ||
        dst.resource_id == 0 || dst.addr == 0 || dst.size == 0) {
        printf("virgltest: copy dst create failed ctx=%u res=%u addr=%lu size=%lu\n",
               ctx.ctx_id, dst.resource_id, dst.addr, dst.size);
        goto out_resources;
    }

    transfer.resource_id = src.resource_id;
    transfer.w = src.width;
    transfer.h = src.height;
    transfer.d = 1;
    transfer.stride = src.width * sizeof(uint32);
    transfer.layer_stride = transfer.stride * src.height;
    if (clear_src) {
        memset((void *)src.addr, 0, src.size);
        if (ioctl(fd, FB_GPU_VIRGL_TRANSFER_TO_HOST, &transfer) < 0) {
            printf("virgltest: copy clear-src zero upload failed src=%u\n",
                   src.resource_id);
            goto out_resources;
        }
        if (submit_clear_texture(fd, ctx.ctx_id, src.resource_id,
                                 src.width, src.height, &submit.fence,
                                 &fence.signaled) != 0) {
            printf("virgltest: copy clear-src submit failed ctx=%u src=%u\n",
                   ctx.ctx_id, src.resource_id);
            goto out_resources;
        }
        if (ioctl(fd, FB_GPU_VIRGL_TRANSFER_FROM_HOST, &transfer) < 0) {
            printf("virgltest: copy clear-src download failed src=%u\n",
                   src.resource_id);
            goto out_resources;
        }
    } else {
        fill_copy_pattern((volatile uint32 *)src.addr, src.width, src.height);
    }
    memset((void *)dst.addr, 0, dst.size);
    if (!clear_src && ioctl(fd, FB_GPU_VIRGL_TRANSFER_TO_HOST, &transfer) < 0) {
        printf("virgltest: copy upload failed src=%u\n", src.resource_id);
        goto out_resources;
    }

    memset(cmds, 0, sizeof(cmds));
    cmds[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0,
                         VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
    cmds[1] = dst.resource_id;
    cmds[2] = 0;
    cmds[3] = 0;
    cmds[4] = 0;
    cmds[5] = 0;
    cmds[6] = src.resource_id;
    cmds[7] = 0;
    cmds[8] = 0;
    cmds[9] = 0;
    cmds[10] = 0;
    cmds[11] = src.width;
    cmds[12] = src.height;
    cmds[13] = 1;

    submit.ctx_id = ctx.ctx_id;
    submit.cmd_size = sizeof(cmds);
    submit.cmd = (uint64)cmds;
    if (ioctl(fd, FB_GPU_VIRGL_SUBMIT, &submit) < 0 ||
        submit.fence == 0) {
        printf("virgltest: copy submit failed ctx=%u src=%u dst=%u fence=%lu signaled=%lu\n",
               ctx.ctx_id, src.resource_id, dst.resource_id, submit.fence,
               submit.signaled);
        goto out_resources;
    }
    fence.flags = FB_GPU_VIRGL_FENCE_WAIT;
    fence.wait_for = submit.fence;
    if (ioctl(fd, FB_GPU_VIRGL_FENCE, &fence) < 0 ||
        fence.signaled < submit.fence) {
        printf("virgltest: copy fence wait failed fence=%lu signaled=%lu\n",
               submit.fence, fence.signaled);
        goto out_resources;
    }

    transfer.resource_id = dst.resource_id;
    if (ioctl(fd, FB_GPU_VIRGL_TRANSFER_FROM_HOST, &transfer) < 0) {
        printf("virgltest: copy download failed dst=%u\n", dst.resource_id);
        goto out_resources;
    }

    matches = check_copy_pattern((volatile uint32 *)src.addr,
                                 (volatile uint32 *)dst.addr,
                                 src.width, src.height);
    if (matches < 5) {
        printf("virgltest: copy mismatch matches=%d src=%08x,%08x,%08x,%08x,%08x dst=%08x,%08x,%08x,%08x,%08x\n",
               matches,
               ((volatile uint32 *)src.addr)[(src.height / 2) * src.width + src.width / 2],
               ((volatile uint32 *)src.addr)[(src.height / 4) * src.width + src.width / 4],
               ((volatile uint32 *)src.addr)[(src.height / 4) * src.width + (src.width * 3) / 4],
               ((volatile uint32 *)src.addr)[((src.height * 3) / 4) * src.width + src.width / 4],
               ((volatile uint32 *)src.addr)[((src.height * 3) / 4) * src.width + (src.width * 3) / 4],
               ((volatile uint32 *)dst.addr)[(dst.height / 2) * dst.width + dst.width / 2],
               ((volatile uint32 *)dst.addr)[(dst.height / 4) * dst.width + dst.width / 4],
               ((volatile uint32 *)dst.addr)[(dst.height / 4) * dst.width + (dst.width * 3) / 4],
               ((volatile uint32 *)dst.addr)[((dst.height * 3) / 4) * dst.width + dst.width / 4],
               ((volatile uint32 *)dst.addr)[((dst.height * 3) / 4) * dst.width + (dst.width * 3) / 4]);
        goto out_resources;
    }

    printf("virgltest: copy-region%s%s%s ok ctx=%u src=%u dst=%u fence=%lu signaled=%lu matches=%d\n",
           clear_src ? "-clear-src" : "",
           render_bind ? "-render-bind" : "",
           scanout_dst ? "-scanout" : "",
           ctx.ctx_id, src.resource_id, dst.resource_id, submit.fence,
           fence.signaled, matches);
    ret = 0;

out_resources:
    destroy_resource_if_created(fd, &dst);
    destroy_resource_if_created(fd, &src);
out_ctx:
    if (ioctl(fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx) < 0) {
        printf("virgltest: copy ctx destroy failed ctx=%u\n", ctx.ctx_id);
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
    if (argc > 1 && strcmp(argv[1], "--copy-region") == 0)
        return resource_copy_test(0, 0, 0);
    if (argc > 1 && strcmp(argv[1], "--copy-region-scanout") == 0)
        return resource_copy_test(1, 0, 0);
    if (argc > 1 && strcmp(argv[1], "--copy-region-clear-src") == 0)
        return resource_copy_test(0, 1, 0);
    if (argc > 1 && strcmp(argv[1], "--copy-region-clear-src-scanout") == 0)
        return resource_copy_test(1, 1, 0);
    if (argc > 1 && strcmp(argv[1], "--copy-region-clear-src-render-bind") == 0)
        return resource_copy_test(0, 1, 1);
    if (argc > 1 && strcmp(argv[1], "--dmabuf-resource-import") == 0)
        return dmabuf_resource_import_test();

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

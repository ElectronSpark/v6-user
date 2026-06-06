#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/syscall.h"
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

static void fill_pattern(uint32 *pixels, uint32 width, uint32 height,
                         uint32 pitch, int loop)
{
    uint32 stride = pitch / 4;

    for (uint32 y = 0; y < height; y++) {
        for (uint32 x = 0; x < width; x++) {
            uint32 r = (x + loop * 17) & 0xff;
            uint32 g = (y * 2 + loop * 29) & 0xff;
            uint32 b = ((x ^ y) + loop * 41) & 0xff;

            pixels[y * stride + x] = 0xff000000 | (r << 16) | (g << 8) | b;
        }
    }
}

static uint32 pattern_pixel(uint32 x, uint32 y, int loop)
{
    uint32 r = (x + loop * 17) & 0xff;
    uint32 g = (y * 2 + loop * 29) & 0xff;
    uint32 b = ((x ^ y) + loop * 41) & 0xff;

    return 0xff000000 | (r << 16) | (g << 8) | b;
}

static int verify_fullscreen_bo_present(int fd)
{
    struct fb_var_screeninfo info;
    struct fb_fix_screeninfo fix;
    struct fb_gpu_bo_create create;
    struct fb_gpu_bo_present present;
    uint32 *readback;
    uint32 *pixels;
    uint32 stride;
    uint32 samples[][2] = {
        {0, 0}, {1, 0}, {31, 0}, {0, 1}, {0, 47},
        {0, 48}, {127, 63}, {319, 127}, {511, 255},
    };
    int nsamples = sizeof(samples) / sizeof(samples[0]);
    int ret = 1;

    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
        info.xres == 0 || info.yres == 0 || fix.line_length == 0) {
        printf("gpubuftest: FBIOGET_VSCREENINFO failed\n");
        return 1;
    }

    memset(&create, 0, sizeof(create));
    create.width = info.xres;
    create.height = info.yres;
    create.flags = FB_GPU_BO_F_EXPORTABLE;
    if (ioctl(fd, FB_GPU_BO_CREATE, &create) < 0) {
        printf("gpubuftest: fullscreen BO_CREATE failed\n");
        return 1;
    }

    pixels = (uint32 *)create.addr;
    stride = create.pitch / 4;
    fill_pattern(pixels, create.width, create.height, create.pitch, 23);

    memset(&present, 0, sizeof(present));
    present.w = create.width;
    present.h = create.height;
    present.handle = create.handle;
    if (ioctl(fd, FB_GPU_BO_PRESENT, &present) < 0) {
        printf("gpubuftest: fullscreen BO_PRESENT failed\n");
        goto out;
    }

    readback = malloc((uint)(fix.line_length * info.yres));
    if (!readback) {
        printf("gpubuftest: readback malloc failed\n");
        goto out;
    }
    if (read(fd, readback, (int)(fix.line_length * info.yres)) !=
        (int)(fix.line_length * info.yres)) {
        printf("gpubuftest: framebuffer readback failed\n");
        free(readback);
        goto out;
    }

    for (int i = 0; i < nsamples; i++) {
        uint32 x = samples[i][0];
        uint32 y = samples[i][1];
        uint32 got;
        uint32 want;

        if (x >= info.xres || y >= info.yres)
            continue;
        got = readback[y * (fix.line_length / 4) + x];
        want = pattern_pixel(x, y, 23);
        if (got != want) {
            printf("gpubuftest: fullscreen mismatch x=%u y=%u got=%x want=%x\n",
                   x, y, got, want);
            free(readback);
            goto out;
        }
    }

    free(readback);
    ret = 0;
    printf("gpubuftest: fullscreen BO present verified %ux%u stride=%u\n",
           create.width, create.height, stride);

out:
    {
        struct fb_gpu_bo_destroy destroy = {
            .handle = create.handle,
        };
        (void)ioctl(fd, FB_GPU_BO_DESTROY, &destroy);
        (void)munmap((void *)create.addr, (int)create.size);
    }
    return ret;
}

static int verify_render_fd_ownership(void)
{
    int fd1 = open("/dev/gpu0", O_RDWR);
    int fd2 = open("/dev/gpu0", O_RDWR);
    struct fb_gpu_bo_create create = {
        .width = 64,
        .height = 64,
        .flags = FB_GPU_BO_F_EXPORTABLE,
    };
    struct fb_gpu_bo_export_fd export_fd;
    struct fb_gpu_bo_export_fd test_export_fd;
    struct fb_gpu_bo_destroy destroy;
    struct fb_gpu_bo_import_fd import_fd;
    struct fb_gpu_bo_import_fd test_import_fd;
    int ret = 1;

    if (fd1 < 0 || fd2 < 0) {
        printf("gpubuftest: open /dev/gpu0 failed\n");
        goto out;
    }
    if (ioctl(fd1, FB_GPU_BO_CREATE, &create) < 0 ||
        create.handle == 0 || create.addr == 0) {
        printf("gpubuftest: gpu0 BO_CREATE failed\n");
        goto out;
    }

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    if (ioctl(fd2, FB_GPU_BO_DESTROY, &destroy) >= 0) {
        printf("gpubuftest: second gpu0 fd destroyed first fd BO\n");
        goto out_unmap;
    }

    memset(&export_fd, 0, sizeof(export_fd));
    export_fd.handle = create.handle;
    if (ioctl(fd1, FB_GPU_BO_EXPORT_FD, &export_fd) < 0 ||
        export_fd.fd < 0) {
        printf("gpubuftest: gpu0 BO_EXPORT_FD failed\n");
        goto out_unmap;
    }

    memset(&test_export_fd, 0, sizeof(test_export_fd));
    test_export_fd.handle = create.handle;
    if (ioctl(fd1, FB_GPU_TEST_DMABUF_EXPORT_FD, &test_export_fd) < 0 ||
        test_export_fd.fd < 0) {
        printf("gpubuftest: TEST_DMABUF_EXPORT_FD failed\n");
        close(export_fd.fd);
        goto out_unmap;
    }

    memset(&test_import_fd, 0, sizeof(test_import_fd));
    test_import_fd.fd = test_export_fd.fd;
    uint32 *test_pixels = (uint32 *)mmap(0, (int)create.size,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED, test_export_fd.fd, 0);
    if (test_pixels == MAP_FAILED) {
        printf("gpubuftest: generic test dma_buf mmap failed\n");
        close(test_export_fd.fd);
        close(export_fd.fd);
        goto out_unmap;
    }
    test_pixels[0] = 0xff314159U;
    test_pixels[(create.size / sizeof(uint32)) - 1] = 0xff271828U;

    if (ioctl(fd2, FB_GPU_BO_IMPORT_FD, &test_import_fd) < 0 ||
        test_import_fd.handle == 0 || test_import_fd.addr == 0 ||
        test_import_fd.width != create.width ||
        test_import_fd.height != create.height) {
        printf("gpubuftest: generic test dma_buf import failed\n");
        munmap((void *)test_pixels, (int)create.size);
        close(test_export_fd.fd);
        close(export_fd.fd);
        goto out_unmap;
    }
    uint32 *import_pixels = (uint32 *)(uintptr_t)test_import_fd.addr;
    if (import_pixels[0] != 0xff314159U ||
        import_pixels[(test_import_fd.size / sizeof(uint32)) - 1] !=
            0xff271828U) {
        printf("gpubuftest: generic test dma_buf mmap pixels mismatch\n");
        munmap((void *)test_pixels, (int)create.size);
        munmap((void *)test_import_fd.addr, (int)test_import_fd.size);
        close(test_export_fd.fd);
        close(export_fd.fd);
        goto out_unmap;
    }
    import_pixels[1] = 0xff123456U;
    if (test_pixels[1] != 0xff123456U) {
        printf("gpubuftest: generic test dma_buf mmap writeback failed\n");
        munmap((void *)test_pixels, (int)create.size);
        munmap((void *)test_import_fd.addr, (int)test_import_fd.size);
        close(test_export_fd.fd);
        close(export_fd.fd);
        goto out_unmap;
    }
    munmap((void *)test_pixels, (int)create.size);
    close(test_export_fd.fd);
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = test_import_fd.handle;
    if (ioctl(fd2, FB_GPU_BO_DESTROY, &destroy) < 0) {
        printf("gpubuftest: generic test dma_buf destroy failed\n");
        munmap((void *)test_import_fd.addr, (int)test_import_fd.size);
        close(export_fd.fd);
        goto out_unmap;
    }
    munmap((void *)test_import_fd.addr, (int)test_import_fd.size);
    printf("gpubuftest: generic test dma_buf mmap/import verified handle=%u\n",
           test_import_fd.handle);

    close(fd1);
    fd1 = -1;

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = create.handle;
    if (ioctl(fd2, FB_GPU_BO_DESTROY, &destroy) >= 0) {
        printf("gpubuftest: stale gpu0 handle survived render fd close\n");
        close(export_fd.fd);
        goto out_unmap;
    }

    memset(&import_fd, 0, sizeof(import_fd));
    import_fd.fd = export_fd.fd;
    if (ioctl(fd2, FB_GPU_BO_IMPORT_FD, &import_fd) < 0 ||
        import_fd.handle == 0 || import_fd.addr == 0 ||
        import_fd.width != create.width || import_fd.height != create.height) {
        printf("gpubuftest: exported BO fd did not survive render fd close\n");
        close(export_fd.fd);
        goto out_unmap;
    }
    close(export_fd.fd);

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = import_fd.handle;
    if (ioctl(fd2, FB_GPU_BO_DESTROY, &destroy) < 0) {
        printf("gpubuftest: imported gpu0 BO_DESTROY failed\n");
        munmap((void *)import_fd.addr, (int)import_fd.size);
        goto out_unmap;
    }
    munmap((void *)import_fd.addr, (int)import_fd.size);
    ret = 0;
    printf("gpubuftest: render fd ownership verified handle=%u imported=%u\n",
           create.handle, import_fd.handle);

out_unmap:
    if (create.addr != 0 && create.size != 0)
        (void)munmap((void *)create.addr, (int)create.size);
out:
    if (fd1 >= 0)
        close(fd1);
    if (fd2 >= 0)
        close(fd2);
    return ret;
}

int main(int argc, char **argv)
{
    int loops = 4;
    int fd = open("/dev/fb0", O_RDWR);

    if (argc > 1) {
        if (strcmp(argv[1], "--fullscreen") == 0 ||
            strcmp(argv[1], "fullscreen") == 0)
            loops = 0;
        else if (strcmp(argv[1], "--render-owner") == 0 ||
                 strcmp(argv[1], "render-owner") == 0)
            return verify_render_fd_ownership();
        else
            loops = atoi(argv[1]);
        if (loops <= 0)
            loops = 0;
    }

    if (fd < 0) {
        printf("gpubuftest: open /dev/fb0 failed\n");
        return 1;
    }

    if (loops == 0) {
        int ret = verify_fullscreen_bo_present(fd);
        close(fd);
        return ret;
    }

    for (int i = 0; i < loops; i++) {
        struct fb_gpu_bo_create create = {
            .width = 160,
            .height = 96,
            .flags = FB_GPU_BO_F_EXPORTABLE,
        };

        if (ioctl(fd, FB_GPU_BO_CREATE, &create) < 0) {
            printf("gpubuftest: FB_GPU_BO_CREATE failed at loop %d\n", i);
            close(fd);
            return 1;
        }
        if (create.addr == 0 || create.size == 0 ||
            create.pitch < create.width * 4 || create.handle == 0) {
            printf("gpubuftest: invalid buffer addr=%p size=%lu pitch=%u handle=%u\n",
                   (void *)create.addr, create.size, create.pitch,
                   create.handle);
            close(fd);
            return 1;
        }

        fill_pattern((uint32 *)create.addr, create.width, create.height,
                     create.pitch, i);

        struct fb_gpu_bo_present present = {
            .x = (uint32)(32 + i * 24),
            .y = (uint32)(48 + i * 16),
            .w = create.width,
            .h = create.height,
            .handle = create.handle,
        };

        struct fb_gpu_bo_import import = {
            .handle = create.handle,
        };
        if (ioctl(fd, FB_GPU_BO_IMPORT, &import) < 0 ||
            import.width != create.width || import.height != create.height ||
            import.pitch != create.pitch || import.size != create.size ||
            import.addr == 0 || import.addr == create.addr) {
            printf("gpubuftest: FB_GPU_BO_IMPORT failed at loop %d\n", i);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        fill_pattern((uint32 *)import.addr, import.width, import.height,
                     import.pitch, i + 17);

        struct fb_gpu_bo_export_fd export_fd = {
            .handle = create.handle,
        };
        if (ioctl(fd, FB_GPU_BO_EXPORT_FD, &export_fd) < 0 ||
            export_fd.fd < 0) {
            printf("gpubuftest: FB_GPU_BO_EXPORT_FD failed at loop %d\n", i);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }

        struct fb_gpu_bo_import_fd import_fd = {
            .fd = export_fd.fd,
        };
        if (ioctl(fd, FB_GPU_BO_IMPORT_FD, &import_fd) < 0 ||
            import_fd.width != create.width ||
            import_fd.height != create.height ||
            import_fd.pitch != create.pitch ||
            import_fd.size != create.size ||
            import_fd.handle == 0 ||
            import_fd.addr == 0 || import_fd.addr == create.addr ||
            import_fd.addr == import.addr) {
            printf("gpubuftest: FB_GPU_BO_IMPORT_FD failed at loop %d\n", i);
            close(export_fd.fd);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        fill_pattern((uint32 *)import_fd.addr, import_fd.width,
                     import_fd.height, import_fd.pitch, i + 31);
        if (close(export_fd.fd) < 0) {
            printf("gpubuftest: BO fd close failed at loop %d\n", i);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }

        if (ioctl(fd, FB_GPU_BO_PRESENT, &present) < 0) {
            printf("gpubuftest: FB_GPU_BO_PRESENT failed at loop %d\n", i);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        if (present.fence == 0) {
            printf("gpubuftest: missing present fence at loop %d\n", i);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }

        struct fb_gpu_fence_export_fd fence_export = {
            .handle = create.handle,
            .fence = present.fence,
        };
        if (ioctl(fd, FB_GPU_FENCE_EXPORT_FD, &fence_export) < 0 ||
            fence_export.fd < 0 ||
            fence_export.fence != present.fence ||
            fence_export.signaled < present.fence) {
            printf("gpubuftest: FB_GPU_FENCE_EXPORT_FD failed at loop %d\n", i);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }

        struct fb_gpu_fence_query fence_query = {
            .fd = fence_export.fd,
            .flags = FB_GPU_FENCE_WAIT,
        };
        if (ioctl(fd, FB_GPU_FENCE_QUERY, &fence_query) < 0 ||
            fence_query.fence != present.fence ||
            fence_query.signaled < present.fence) {
            printf("gpubuftest: FB_GPU_FENCE_QUERY failed at loop %d\n", i);
            close(fence_export.fd);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        struct pollfd pfd = {
            .fd = fence_export.fd,
            .events = POLLIN | POLLRDNORM,
        };
        if (poll_raw(&pfd, 1, 0) != 1 ||
            (pfd.revents & (POLLIN | POLLRDNORM)) == 0) {
            printf("gpubuftest: fence fd poll failed at loop %d revents=%x\n",
                   i, pfd.revents);
            close(fence_export.fd);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        if (close(fence_export.fd) < 0) {
            printf("gpubuftest: fence fd close failed at loop %d\n", i);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        struct pollfd closed_pfd = {
            .fd = fence_export.fd,
            .events = POLLIN | POLLRDNORM,
        };
        if (poll_raw(&closed_pfd, 1, 0) != 1 ||
            (closed_pfd.revents & POLLNVAL) == 0) {
            printf("gpubuftest: closed fence fd poll did not report POLLNVAL at loop %d revents=%x\n",
                   i, closed_pfd.revents);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        struct fb_gpu_fence_query closed_fence_query = {
            .fd = fence_export.fd,
        };
        if (ioctl(fd, FB_GPU_FENCE_QUERY, &closed_fence_query) >= 0) {
            printf("gpubuftest: closed fence fd query succeeded at loop %d\n",
                   i);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        printf("gpubuftest: closed fence fd rejected at loop %d\n", i);

        struct fb_gpu_fence_export_fd future_fence_export = {
            .handle = create.handle,
            .fence = present.fence + 1000000,
        };
        if (ioctl(fd, FB_GPU_FENCE_EXPORT_FD, &future_fence_export) < 0 ||
            future_fence_export.fd < 0 ||
            future_fence_export.fence != present.fence + 1000000) {
            printf("gpubuftest: future FB_GPU_FENCE_EXPORT_FD failed at loop %d\n",
                   i);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        struct pollfd future_pfd = {
            .fd = future_fence_export.fd,
            .events = POLLIN | POLLRDNORM,
        };
        if (poll_raw(&future_pfd, 1, 0) != 0 || future_pfd.revents != 0) {
            printf("gpubuftest: future fence fd became ready at loop %d revents=%x\n",
                   i, future_pfd.revents);
            close(future_fence_export.fd);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        struct fb_gpu_fence_query future_nowait_query = {
            .fd = future_fence_export.fd,
        };
        if (ioctl(fd, FB_GPU_FENCE_QUERY, &future_nowait_query) < 0 ||
            future_nowait_query.fence != present.fence + 1000000 ||
            future_nowait_query.signaled >= future_nowait_query.fence) {
            printf("gpubuftest: pending fence zero-timeout query failed at loop %d fence=%lu signaled=%lu\n",
                   i, future_nowait_query.fence,
                   future_nowait_query.signaled);
            close(future_fence_export.fd);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        printf("gpubuftest: pending fence query ok at loop %d fence=%lu signaled=%lu\n",
               i, future_nowait_query.fence, future_nowait_query.signaled);
        struct fb_gpu_fence_query future_fence_query = {
            .fd = future_fence_export.fd,
            .flags = FB_GPU_FENCE_WAIT,
        };
        if (ioctl(fd, FB_GPU_FENCE_QUERY, &future_fence_query) >= 0 ||
            future_fence_query.fence != present.fence + 1000000 ||
            future_fence_query.signaled < present.fence) {
            printf("gpubuftest: future fence wait did not fail cleanly at loop %d fence=%lu signaled=%lu\n",
                   i, future_fence_query.fence, future_fence_query.signaled);
            close(future_fence_export.fd);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        if (close(future_fence_export.fd) < 0) {
            printf("gpubuftest: future fence fd close failed at loop %d\n", i);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }

        struct fb_gpu_bo_fence fence = {
            .handle = create.handle,
            .flags = FB_GPU_BO_FENCE_WAIT,
            .wait_for = present.fence,
        };
        if (ioctl(fd, FB_GPU_BO_FENCE, &fence) < 0 ||
            fence.signaled < present.fence ||
            fence.last_present != present.fence) {
            printf("gpubuftest: FB_GPU_BO_FENCE failed at loop %d fence=%lu signaled=%lu last=%lu\n",
                   i, present.fence, fence.signaled, fence.last_present);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }

        struct fb_gpu_bo_destroy destroy = {
            .handle = create.handle,
        };
        struct fb_gpu_bo_destroy import_fd_destroy = {
            .handle = import_fd.handle,
        };
        if (ioctl(fd, FB_GPU_BO_DESTROY, &import_fd_destroy) < 0) {
            printf("gpubuftest: FB_GPU_BO_DESTROY import-fd failed at loop %d\n", i);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        if (ioctl(fd, FB_GPU_BO_DESTROY, &destroy) < 0) {
            printf("gpubuftest: FB_GPU_BO_DESTROY failed at loop %d\n", i);
            munmap((void *)import_fd.addr, (int)import_fd.size);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }

        if (munmap((void *)import_fd.addr, (int)import_fd.size) < 0) {
            printf("gpubuftest: import-fd munmap failed at loop %d\n", i);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }

        if (munmap((void *)import.addr, (int)import.size) < 0) {
            printf("gpubuftest: import munmap failed at loop %d\n", i);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }

        if (munmap((void *)create.addr, (int)create.size) < 0) {
            printf("gpubuftest: munmap failed at loop %d\n", i);
            close(fd);
            return 1;
        }
    }

    printf("gpubuftest: completed %d buffer cycles\n", loops);
    close(fd);
    return 0;
}

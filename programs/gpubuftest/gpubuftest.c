#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

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
        info.xres == 0 || info.yres == 0 || info.pitch == 0) {
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

    readback = malloc((uint)(info.pitch * info.yres));
    if (!readback) {
        printf("gpubuftest: readback malloc failed\n");
        goto out;
    }
    if (read(fd, readback, (int)(info.pitch * info.yres)) !=
        (int)(info.pitch * info.yres)) {
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
        got = readback[y * (info.pitch / 4) + x];
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

int main(int argc, char **argv)
{
    int loops = 4;
    int fd = open("/dev/fb0", O_RDWR);

    if (argc > 1) {
        if (strcmp(argv[1], "--fullscreen") == 0 ||
            strcmp(argv[1], "fullscreen") == 0)
            loops = 0;
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

        if (ioctl(fd, FB_GPU_BO_PRESENT, &present) < 0) {
            printf("gpubuftest: FB_GPU_BO_PRESENT failed at loop %d\n", i);
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }
        if (present.fence == 0) {
            printf("gpubuftest: missing present fence at loop %d\n", i);
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
            munmap((void *)import.addr, (int)import.size);
            munmap((void *)create.addr, (int)create.size);
            close(fd);
            return 1;
        }

        struct fb_gpu_bo_destroy destroy = {
            .handle = create.handle,
        };
        if (ioctl(fd, FB_GPU_BO_DESTROY, &destroy) < 0) {
            printf("gpubuftest: FB_GPU_BO_DESTROY failed at loop %d\n", i);
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

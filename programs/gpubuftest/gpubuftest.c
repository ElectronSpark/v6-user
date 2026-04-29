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

int main(int argc, char **argv)
{
    int loops = 4;
    int fd = open("/dev/fb0", O_RDWR);

    if (argc > 1) {
        loops = atoi(argv[1]);
        if (loops <= 0)
            loops = 1;
    }

    if (fd < 0) {
        printf("gpubuftest: open /dev/fb0 failed\n");
        return 1;
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

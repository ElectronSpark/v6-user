#include "kernel/inc/types.h"
#include "kernel/inc/uabi/drm.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

static int check_prime_cap(int fd)
{
    struct drm_get_cap_compat cap = {
        .capability = DRM_CAP_PRIME,
    };

    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0) {
        printf("drmprimeprobe: DRM_CAP_PRIME failed\n");
        return 1;
    }
    if ((cap.value & (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) !=
        (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) {
        printf("drmprimeprobe: PRIME cap missing value=0x%lx\n", cap.value);
        return 1;
    }
    printf("drmprimeprobe: prime cap value=0x%lx\n", cap.value);
    return 0;
}

int main(void)
{
    int fd1 = open("/dev/dri/renderD128", O_RDWR);
    int fd2 = open("/dev/dri/renderD128", O_RDWR);
    struct drm_mode_create_dumb_compat create = {
        .width = 80,
        .height = 48,
        .bpp = 32,
    };
    struct drm_prime_handle_compat prime;
    struct drm_gem_close_compat close_req;
    struct drm_mode_map_dumb_compat map_req;
    struct drm_mode_destroy_dumb_compat destroy_req;
    uint32 *pixels;
    uint32 imported_handle = 0;
    int prime_fd = -1;
    int ret = 1;

    if (fd1 < 0 || fd2 < 0) {
        printf("drmprimeprobe: open render node failed\n");
        goto out;
    }
    if (check_prime_cap(fd1) != 0)
        goto out;
    if (ioctl(fd1, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        create.handle == 0 || create.pitch < create.width * 4 ||
        create.size == 0) {
        printf("drmprimeprobe: create dumb failed handle=%u pitch=%u size=%lu\n",
               create.handle, create.pitch, create.size);
        goto out;
    }

    memset(&prime, 0, sizeof(prime));
    prime.handle = create.handle;
    if (ioctl(fd1, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0 ||
        prime.fd < 0) {
        printf("drmprimeprobe: PRIME_HANDLE_TO_FD failed handle=%u\n",
               create.handle);
        goto out_destroy_fd1;
    }
    prime_fd = prime.fd;
    printf("drmprimeprobe: exported handle=%u fd=%d\n",
           create.handle, prime_fd);

    close(fd1);
    fd1 = -1;

    memset(&close_req, 0, sizeof(close_req));
    close_req.handle = create.handle;
    if (ioctl(fd2, DRM_IOCTL_GEM_CLOSE, &close_req) >= 0) {
        printf("drmprimeprobe: stale handle closed on another render fd\n");
        goto out;
    }
    printf("drmprimeprobe: stale handle rejected on second fd\n");

    memset(&prime, 0, sizeof(prime));
    prime.fd = prime_fd;
    if (ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0 ||
        prime.handle == 0) {
        printf("drmprimeprobe: PRIME_FD_TO_HANDLE failed fd=%d\n", prime_fd);
        goto out;
    }
    imported_handle = prime.handle;
    printf("drmprimeprobe: imported fd=%d handle=%u\n",
           prime_fd, imported_handle);

    close(prime_fd);
    prime_fd = -1;

    memset(&prime, 0, sizeof(prime));
    prime.fd = prime_fd;
    if (ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) >= 0) {
        printf("drmprimeprobe: closed PRIME fd imported unexpectedly\n");
        goto out_destroy_import;
    }
    printf("drmprimeprobe: closed PRIME fd rejected\n");

    memset(&map_req, 0, sizeof(map_req));
    map_req.handle = imported_handle;
    if (ioctl(fd2, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0 ||
        map_req.offset == 0) {
        printf("drmprimeprobe: MAP_DUMB failed handle=%u offset=0x%lx\n",
               imported_handle, map_req.offset);
        goto out_destroy_import;
    }
    pixels = mmap(0, (int)create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd2, map_req.offset);
    if (pixels == MAP_FAILED) {
        printf("drmprimeprobe: mmap imported dumb failed offset=0x%lx size=%lu\n",
               map_req.offset, create.size);
        goto out_destroy_import;
    }
    pixels[0] = 0xff336699;
    pixels[(create.height - 1) * (create.pitch / 4) + (create.width - 1)] =
        0xff996633;
    if (pixels[0] != 0xff336699 ||
        pixels[(create.height - 1) * (create.pitch / 4) + (create.width - 1)] !=
            0xff996633) {
        printf("drmprimeprobe: imported mapping readback mismatch\n");
        munmap((void *)pixels, (int)create.size);
        goto out_destroy_import;
    }
    munmap((void *)pixels, (int)create.size);

    ret = 0;
    printf("drmprimeprobe: ok size=%lu pitch=%u imported=%u\n",
           create.size, create.pitch, imported_handle);

out_destroy_import:
    if (imported_handle != 0) {
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = imported_handle;
        (void)ioctl(fd2, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    }
    goto out;

out_destroy_fd1:
    memset(&destroy_req, 0, sizeof(destroy_req));
    destroy_req.handle = create.handle;
    (void)ioctl(fd1, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

out:
    if (prime_fd >= 0)
        close(prime_fd);
    if (fd1 >= 0)
        close(fd1);
    if (fd2 >= 0)
        close(fd2);
    return ret;
}

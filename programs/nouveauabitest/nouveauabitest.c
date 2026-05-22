#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <nouveau.h>
#include <drm/nouveau_drm.h>

static int
fail(const char *msg)
{
    printf("nouveauabitest: %s\n", msg);
    return 1;
}

static const char *
version_name(drmVersionPtr ver)
{
    if (ver == NULL || ver->name == NULL)
        return "(unknown)";
    return ver->name;
}

static int
check_getparam(struct nouveau_device *dev, uint64_t param, uint64_t *value,
               const char *name)
{
    int ret = nouveau_getparam(dev, param, value);
    if (ret != 0) {
        printf("nouveauabitest: getparam %s failed ret=%d\n", name, ret);
        return 1;
    }
    return 0;
}

static int
check_present_nouveau(int fd, struct nouveau_device *dev)
{
    struct nouveau_client *client = NULL;
    struct nouveau_bo *bo = NULL;
    struct nouveau_bo *prime_bo = NULL;
    uint64_t vendor = 0;
    uint64_t device = 0;
    uint64_t chipset = 0;
    uint64_t has_bo_usage = 0;
    uint64_t fb_size = 0;
    uint64_t gart_size = 0;
    int prime_fd = -1;
    int ret;

    (void)fd;
    if (check_getparam(dev, NOUVEAU_GETPARAM_PCI_VENDOR, &vendor,
                       "PCI_VENDOR") != 0 ||
        check_getparam(dev, NOUVEAU_GETPARAM_PCI_DEVICE, &device,
                       "PCI_DEVICE") != 0 ||
        check_getparam(dev, NOUVEAU_GETPARAM_CHIPSET_ID, &chipset,
                       "CHIPSET_ID") != 0 ||
        check_getparam(dev, NOUVEAU_GETPARAM_HAS_BO_USAGE, &has_bo_usage,
                       "HAS_BO_USAGE") != 0 ||
        check_getparam(dev, NOUVEAU_GETPARAM_FB_SIZE, &fb_size,
                       "FB_SIZE") != 0 ||
        check_getparam(dev, NOUVEAU_GETPARAM_AGP_SIZE, &gart_size,
                       "AGP_SIZE") != 0)
        return 1;

    if (vendor != 0x10de || device == 0 || has_bo_usage != 1)
        return fail("unexpected Nouveau device parameters");

    ret = nouveau_client_new(dev, &client);
    if (ret != 0 || client == NULL)
        return fail("nouveau_client_new failed");

    ret = nouveau_bo_new(dev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP |
                              NOUVEAU_BO_COHERENT, 4096, 4096, NULL, &bo);
    if (ret != 0 || bo == NULL || bo->handle == 0 || bo->size < 4096) {
        nouveau_client_del(&client);
        return fail("nouveau_bo_new failed");
    }

    ret = nouveau_bo_wait(bo, NOUVEAU_BO_RDWR | NOUVEAU_BO_NOBLOCK, client);
    if (ret != 0) {
        nouveau_bo_ref(NULL, &bo);
        nouveau_client_del(&client);
        return fail("nouveau_bo_wait failed");
    }

    ret = nouveau_bo_map(bo, NOUVEAU_BO_RDWR, client);
    if (ret != 0 || bo->map == MAP_FAILED || bo->map == NULL) {
        nouveau_bo_ref(NULL, &bo);
        nouveau_client_del(&client);
        return fail("nouveau_bo_map failed");
    }
    memset(bo->map, 0x5a, 4096);

    ret = nouveau_bo_set_prime(bo, &prime_fd);
    if (ret != 0 || prime_fd < 0) {
        nouveau_bo_ref(NULL, &bo);
        nouveau_client_del(&client);
        return fail("nouveau_bo_set_prime failed");
    }

    ret = nouveau_bo_prime_handle_ref(dev, prime_fd, &prime_bo);
    close(prime_fd);
    prime_fd = -1;
    if (ret != 0 || prime_bo == NULL) {
        nouveau_bo_ref(NULL, &bo);
        nouveau_client_del(&client);
        return fail("nouveau_bo_prime_handle_ref failed");
    }

    nouveau_bo_ref(NULL, &prime_bo);
    nouveau_bo_ref(NULL, &bo);
    nouveau_client_del(&client);
    printf("nouveauabitest: present ok vendor=0x%" PRIx64
           " device=0x%" PRIx64 " chipset=0x%" PRIx64
           " fb=%" PRIu64 " gart=%" PRIu64 "\n",
           vendor, device, chipset, fb_size, gart_size);
    return 0;
}

int
main(void)
{
    struct nouveau_device *dev = NULL;
    drmVersionPtr ver;
    int fd;
    int ret;

    fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return fail("open renderD128 failed");

    ver = drmGetVersion(fd);
    if (ver == NULL) {
        close(fd);
        return fail("drmGetVersion failed");
    }

    ret = nouveau_device_wrap(fd, 0, &dev);
    if (ret != 0) {
        printf("nouveauabitest: nouveau absent fail-closed ok driver=%s ret=%d\n",
               version_name(ver), ret);
        drmFreeVersion(ver);
        close(fd);
        return 0;
    }

    ret = check_present_nouveau(fd, dev);
    nouveau_device_del(&dev);
    drmFreeVersion(ver);
    close(fd);
    if (ret != 0)
        return ret;

    printf("nouveauabitest: ok\n");
    return 0;
}

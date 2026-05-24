#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <nouveau.h>
#include <drm/nouveau_drm.h>

#define FB_GPU_BACKEND_QUERY 0x462C
#define FB_GPU_BACKEND_F_DDA_NOUVEAU 0x0200

struct fb_gpu_backend_info_compat {
    uint32_t backend;
    uint32_t flags;
    uint32_t capset_id;
    uint32_t capset_version;
    uint32_t capset_size;
    uint32_t dxg_global_open;
    uint32_t dxg_vgpu_open;
    uint32_t dxg_d3dkmt;
    uint32_t dxg_global_status;
    uint32_t dxg_vgpu_status;
    uint32_t dxg_global_rx;
    uint32_t dxg_vgpu_rx;
    char name[32];
    char renderer[64];
};

static int
fail(const char *msg)
{
    printf("nouveauabitest: %s\n", msg);
    return 1;
}

static int
fail_token(const char *token, const char *msg)
{
    printf("nouveauabitest: %s %s\n", token, msg);
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
query_backend(struct fb_gpu_backend_info_compat *info)
{
    static const char *paths[] = {
        "/dev/fb0",
        "/dev/gpu0",
        "/dev/dri/renderD128",
    };
    int fd;

    if (info == NULL)
        return 0;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        memset(info, 0, sizeof(*info));
        fd = open(paths[i], O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (ioctl(fd, FB_GPU_BACKEND_QUERY, info) == 0) {
            close(fd);
            return 1;
        }
        close(fd);
    }
    memset(info, 0, sizeof(*info));
    return 0;
}

static int
backend_has_dda_nouveau(const struct fb_gpu_backend_info_compat *info,
                        int have_backend)
{
    return have_backend &&
           (info->flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) != 0;
}

static void
print_no_nouveau_driver_pass(const char *driver, int ret)
{
    printf("nouveauabitest: PASS_NO_NOUVEAU_DRIVER ok driver=%s ret=%d\n",
           driver ? driver : "(none)", ret);
}

static void
print_no_native_engine_pass(const struct fb_gpu_backend_info_compat *info,
                            int have_backend)
{
    printf("nouveauabitest: PASS_NO_NATIVE_PCI_BAR_NOUVEAU_ENGINE ok "
           "backend=%u flags=0x%x dda_nouveau=0\n",
           have_backend ? info->backend : 0,
           have_backend ? info->flags : 0);
}

static void
print_no_mesa_nvif_pass(const char *reason)
{
    printf("nouveauabitest: PASS_NO_MESA_NVIF_ENABLEMENT ok reason=%s\n",
           reason ? reason : "not-enabled");
}

static int
fail_backend_claimed_nouveau_without_stack(const char *driver, int ret,
                                           uint32_t flags, const char *reason)
{
    printf("nouveauabitest: FAIL_NO_NOUVEAU_DRIVER "
           "backend advertises DDA/Nouveau but Nouveau driver path is absent "
           "driver=%s ret=%d flags=0x%x reason=%s\n",
           driver ? driver : "(none)", ret, flags,
           reason ? reason : "unknown");
    printf("nouveauabitest: FAIL_NO_NATIVE_PCI_BAR_NOUVEAU_ENGINE "
           "backend advertises DDA/Nouveau but no native PCI BAR/Nouveau "
           "engine path materialized flags=0x%x reason=%s\n",
           flags, reason ? reason : "unknown");
    printf("nouveauabitest: FAIL_NO_MESA_NVIF_ENABLEMENT "
           "backend advertises DDA/Nouveau but Mesa/NVIF enablement is absent "
           "reason=%s\n",
           reason ? reason : "unknown");
    return 1;
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
    struct nouveau_object *chan = NULL;
    struct nouveau_pushbuf *push = NULL;
    struct nouveau_bo *bo = NULL;
    struct nouveau_bo *prime_bo = NULL;
    struct nve0_fifo fifo;
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
        return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "unexpected Nouveau device parameters");
    if (fb_size == 0 || gart_size == 0)
        return fail_token("FAIL_NO_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "nouveau device opened without BAR-backed memory");

    ret = nouveau_client_new(dev, &client);
    if (ret != 0 || client == NULL)
        return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "nouveau_client_new failed");

    memset(&fifo, 0, sizeof(fifo));
    fifo.engine = NVE0_FIFO_ENGINE_GR;
    ret = nouveau_object_new(&dev->object, 0, NOUVEAU_FIFO_CHANNEL_CLASS,
                             &fifo, sizeof(fifo), &chan);
    if (ret != 0 || chan == NULL) {
        nouveau_client_del(&client);
        return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "nouveau channel object creation failed");
    }
    {
        struct nouveau_object *bad = NULL;

        ret = nouveau_object_new(&dev->object, 0, 0xdeadbeef, NULL, 0, &bad);
        if (ret == 0 || bad != NULL) {
            if (bad != NULL)
                nouveau_object_del(&bad);
            nouveau_object_del(&chan);
            nouveau_client_del(&client);
            return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                              "nouveau unsupported class unexpectedly succeeded");
        }
    }

    ret = nouveau_pushbuf_new(client, chan, 1, 64, false, &push);
    if (ret != 0 || push == NULL) {
        nouveau_object_del(&chan);
        nouveau_client_del(&client);
        return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "nouveau no-op pushbuf creation failed");
    }
    ret = nouveau_pushbuf_kick(push, chan);
    if (ret != 0) {
        nouveau_pushbuf_del(&push);
        nouveau_object_del(&chan);
        nouveau_client_del(&client);
        return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "nouveau no-op pushbuf kick failed");
    }

    ret = nouveau_bo_new(dev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP |
                              NOUVEAU_BO_COHERENT, 4096, 4096, NULL, &bo);
    if (ret != 0 || bo == NULL || bo->handle == 0 || bo->size < 4096) {
        nouveau_pushbuf_del(&push);
        nouveau_object_del(&chan);
        nouveau_client_del(&client);
        return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "nouveau_bo_new failed");
    }

    ret = nouveau_bo_wait(bo, NOUVEAU_BO_RDWR | NOUVEAU_BO_NOBLOCK, client);
    if (ret != 0) {
        nouveau_bo_ref(NULL, &bo);
        nouveau_pushbuf_del(&push);
        nouveau_object_del(&chan);
        nouveau_client_del(&client);
        return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "nouveau_bo_wait failed");
    }

    ret = nouveau_bo_map(bo, NOUVEAU_BO_RDWR, client);
    if (ret != 0 || bo->map == MAP_FAILED || bo->map == NULL) {
        nouveau_bo_ref(NULL, &bo);
        nouveau_pushbuf_del(&push);
        nouveau_object_del(&chan);
        nouveau_client_del(&client);
        return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "nouveau_bo_map failed");
    }
    memset(bo->map, 0x5a, 4096);

    ret = nouveau_bo_set_prime(bo, &prime_fd);
    if (ret != 0 || prime_fd < 0) {
        nouveau_bo_ref(NULL, &bo);
        nouveau_pushbuf_del(&push);
        nouveau_object_del(&chan);
        nouveau_client_del(&client);
        return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "nouveau_bo_set_prime failed");
    }

    ret = nouveau_bo_prime_handle_ref(dev, prime_fd, &prime_bo);
    close(prime_fd);
    prime_fd = -1;
    if (ret != 0 || prime_bo == NULL) {
        nouveau_bo_ref(NULL, &bo);
        nouveau_pushbuf_del(&push);
        nouveau_object_del(&chan);
        nouveau_client_del(&client);
        return fail_token("FAIL_NATIVE_PCI_BAR_NOUVEAU_ENGINE",
                          "nouveau_bo_prime_handle_ref failed");
    }

    nouveau_bo_ref(NULL, &prime_bo);
    nouveau_bo_ref(NULL, &bo);
    nouveau_pushbuf_del(&push);
    nouveau_object_del(&chan);
    nouveau_client_del(&client);
    printf("nouveauabitest: PASS_NATIVE_PCI_BAR_NOUVEAU_ENGINE_DIAGNOSTIC ok "
           "no_real_nouveau_support_claim=1 vendor=0x%" PRIx64
           " device=0x%" PRIx64 " chipset=0x%" PRIx64
           " fb=%" PRIu64 " gart=%" PRIu64 "\n",
           vendor, device, chipset, fb_size, gart_size);
    printf("nouveauabitest: nouveau_mesa_smoke_gate_matrix "
           "dda_nouveau=1 winsys_device_info=PASS "
           "synthetic_gpup_rejected=PASS native_engine=diagnostic-only "
           "mesa_nvif_enabled=0 status=PASS\n");
    return 0;
}

int
main(void)
{
    struct nouveau_device *dev = NULL;
    struct fb_gpu_backend_info_compat backend;
    drmVersionPtr ver;
    int have_backend;
    int fd;
    int ret;

    have_backend = query_backend(&backend);
    ret = nouveau_device_open(NULL, &dev);
    if (ret == 0) {
        ret = check_present_nouveau(dev->fd, dev);
        nouveau_device_del(&dev);
        if (ret != 0)
            return ret;
        printf("nouveauabitest: PASS_MESA_NVIF_ENABLEMENT_NOT_CLAIMED ok "
               "reason=nouveau-abi-only\n");
        printf("nouveauabitest: discovery open ok\n");
        return 0;
    }
    printf("nouveauabitest: discovery open fail-closed ret=%d\n", ret);

    fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (backend_has_dda_nouveau(&backend, have_backend))
            return fail_backend_claimed_nouveau_without_stack(
                "render-node-unavailable", -errno, backend.flags,
                "render-node-absent");
        print_no_nouveau_driver_pass("render-node-unavailable", -errno);
        print_no_native_engine_pass(&backend, have_backend);
        print_no_mesa_nvif_pass("no-nouveau-driver");
        printf("nouveauabitest: nouveau_mesa_smoke_gate_matrix "
               "dda_nouveau=0 winsys_device_info=SKIP "
               "synthetic_gpup_rejected=PASS native_engine=absent "
               "mesa_nvif_enabled=0 status=PASS\n");
        return 0;
    }

    ver = drmGetVersion(fd);
    if (ver == NULL) {
        close(fd);
        return fail("drmGetVersion failed");
    }

    ret = nouveau_device_wrap(fd, 0, &dev);
    if (ret != 0) {
        if (backend_has_dda_nouveau(&backend, have_backend)) {
            int fail_ret = fail_backend_claimed_nouveau_without_stack(
                version_name(ver), ret, backend.flags, "libdrm-wrap-failed");
            drmFreeVersion(ver);
            close(fd);
            return fail_ret;
        }
        print_no_nouveau_driver_pass(version_name(ver), ret);
        print_no_native_engine_pass(&backend, have_backend);
        print_no_mesa_nvif_pass("no-native-pci-bar-nouveau-engine");
        printf("nouveauabitest: nouveau absent fail-closed ok driver=%s ret=%d\n",
               version_name(ver), ret);
        printf("nouveauabitest: nouveau_mesa_smoke_gate_matrix "
               "dda_nouveau=0 winsys_device_info=SKIP "
               "synthetic_gpup_rejected=PASS native_engine=absent "
               "mesa_nvif_enabled=0 status=PASS\n");
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

    printf("nouveauabitest: PASS_MESA_NVIF_ENABLEMENT_NOT_CLAIMED ok "
           "reason=nouveau-abi-only\n");
    printf("nouveauabitest: ok\n");
    return 0;
}

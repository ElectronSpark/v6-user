#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

static int fail(const char *msg)
{
    printf("ttmtest: %s\n", msg);
    return 1;
}

static int create_bo(int fd, uint32 width, uint32 height,
                     struct fb_gpu_bo_create *bo)
{
    memset(bo, 0, sizeof(*bo));
    bo->width = width;
    bo->height = height;
    bo->flags = FB_GPU_BO_F_EXPORTABLE;
    if (ioctl(fd, FB_GPU_BO_CREATE, bo) < 0 || bo->handle == 0 ||
        bo->addr == 0 || bo->size == 0)
        return -1;
    return 0;
}

static int destroy_bo(int fd, struct fb_gpu_bo_create *bo)
{
    struct fb_gpu_bo_destroy destroy;

    if (bo->addr != 0 && bo->size != 0)
        (void)munmap((void *)bo->addr, (int)bo->size);
    if (bo->handle == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = bo->handle;
    if (ioctl(fd, FB_GPU_BO_DESTROY, &destroy) < 0)
        return -1;
    bo->handle = 0;
    return 0;
}

static int ttm_ioctl(int fd, struct fb_gpu_ttm_validate *ttm)
{
    return ioctl(fd, FB_GPU_TTM_VALIDATE, ttm);
}

static int query_ttm(int fd, uint32 handle, struct fb_gpu_ttm_validate *ttm)
{
    memset(ttm, 0, sizeof(*ttm));
    ttm->handle = handle;
    return ttm_ioctl(fd, ttm);
}

static int set_ttm(int fd, uint32 handle, uint32 placement,
                   struct fb_gpu_ttm_validate *ttm)
{
    memset(ttm, 0, sizeof(*ttm));
    ttm->handle = handle;
    ttm->flags = FB_GPU_TTM_F_SET_PLACEMENT;
    ttm->placement = placement;
    return ttm_ioctl(fd, ttm);
}

int main(int argc, char **argv)
{
    struct fb_gpu_bo_create bo1;
    struct fb_gpu_bo_create bo2;
    struct fb_gpu_ttm_validate ttm;
    struct fb_gpu_ttm_validate ttm2;
    uint64 moves;
    uint64 evictions;
    int fd;
    int ret = 1;

    (void)argc;
    (void)argv;
    memset(&bo1, 0, sizeof(bo1));
    memset(&bo2, 0, sizeof(bo2));

    fd = open("/dev/gpu0", O_RDWR);
    if (fd < 0)
        return fail("open /dev/gpu0 failed");
    if (create_bo(fd, 64, 64, &bo1) < 0)
        goto out_fail_create1;
    if (create_bo(fd, 32, 32, &bo2) < 0)
        goto out_fail_create2;

    if (set_ttm(fd, bo1.handle, FB_GPU_TTM_PL_TT, &ttm) < 0)
        goto out_fail;
    if (ttm.mem_type != 1 || ttm.tt_populated == 0 ||
        ttm.sg_nents == 0 || ttm.dma_addr_base == 0 ||
        ttm.manager_bytes[1] < bo1.size)
        goto out_fail;
    moves = ttm.move_count;

    memset(&ttm, 0, sizeof(ttm));
    ttm.handle = bo1.handle;
    ttm.flags = FB_GPU_TTM_F_PIN;
    if (ttm_ioctl(fd, &ttm) < 0 || ttm.pin_count != 1)
        goto out_fail;
    memset(&ttm, 0, sizeof(ttm));
    ttm.handle = bo1.handle;
    ttm.flags = FB_GPU_TTM_F_UNPIN;
    if (ttm_ioctl(fd, &ttm) < 0 || ttm.pin_count != 0)
        goto out_fail;

    if (set_ttm(fd, bo1.handle, FB_GPU_TTM_PL_VRAM, &ttm) < 0)
        goto out_fail;
    if (ttm.mem_type != 2 || ttm.move_count <= moves ||
        ttm.manager_bytes[2] < bo1.size)
        goto out_fail;
    moves = ttm.move_count;
    if (set_ttm(fd, bo1.handle, FB_GPU_TTM_PL_VRAM, &ttm) < 0)
        goto out_fail;
    if (ttm.mem_type != 2 || ttm.move_count != moves)
        goto out_fail;

    if (set_ttm(fd, bo2.handle, FB_GPU_TTM_PL_TT, &ttm2) < 0 ||
        ttm2.mem_type != 1)
        goto out_fail;
    evictions = ttm2.evictions;
    memset(&ttm2, 0, sizeof(ttm2));
    ttm2.handle = bo2.handle;
    ttm2.flags = FB_GPU_TTM_F_FORCE_EVICT;
    ttm2.placement = FB_GPU_TTM_PL_TT;
    if (ttm_ioctl(fd, &ttm2) < 0)
        goto out_fail;
    if (ttm2.evictions <= evictions)
        goto out_fail;
    if (query_ttm(fd, bo2.handle, &ttm2) < 0)
        goto out_fail;
    if (ttm2.mem_type != 0)
        goto out_fail;

    memset(&ttm, 0, sizeof(ttm));
    ttm.handle = bo1.handle;
    ttm.flags = FB_GPU_TTM_F_SET_PLACEMENT;
    ttm.placement = 0;
    if (ttm_ioctl(fd, &ttm) >= 0)
        goto out_fail;

    ret = 0;
    printf("ttmtest: ok tt_bytes=%lu vram_bytes=%lu evictions=%lu\n",
           ttm2.manager_bytes[1], ttm2.manager_bytes[2], ttm2.evictions);

out_fail:
    (void)destroy_bo(fd, &bo2);
out_fail_create2:
    (void)destroy_bo(fd, &bo1);
out_fail_create1:
    close(fd);
    if (ret != 0)
        return fail("validation failed");
    return 0;
}

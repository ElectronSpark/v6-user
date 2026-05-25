#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

static int fail(const char *msg)
{
    printf("ttmtest: %s\n", msg);
    return 1;
}

struct ttm_resv_stats {
    uint64 acquires;
    uint64 releases;
    uint64 waits;
    uint64 conflicts;
    uint64 exclusive_fences;
    uint64 shared_slots;
    uint64 shared_used;
    uint64 shared_fences;
    uint64 wait_queued;
    uint64 wait_wakeups;
    uint64 stale_fence_rejects;
    uint64 attach_dmabuf_export;
    uint64 attach_dmabuf_import;
    uint64 last_attach_point;
    uint64 evict_pinned_rejects;
    uint64 evict_busy_rejects;
    uint64 validate_failures;
    uint64 pinned_bytes;
    uint64 native_accel_credit;
    uint64 ww_contexts;
    uint64 ww_ordered_acquires;
    uint64 ww_deadlock_retries;
    uint64 ww_wound_backoffs;
    uint64 ww_multi_object;
    uint64 ww_release_balance;
    uint64 ww_max_acquired;
    uint64 ww_validate_failures;
};

struct ttm_move_path_stats {
    uint64 move_count;
    uint64 metadata_only_moves;
    uint64 real_copy_moves;
    uint64 move_bytes;
    uint64 native_accel_credit;
    uint64 manager_moves[4];
    uint64 cpu_copy_fallback_moves[4];
    uint64 metadata_noop_moves[4];
    uint64 unsupported_hw_copy_moves[4];
    uint64 real_copy_moves_by_domain[4];
};

static int read_ttm_resv_stats(int fd, struct ttm_resv_stats *snap)
{
    struct fb_gpu_stats stats;

    memset(&stats, 0, sizeof(stats));
    if (ioctl(fd, FB_GPU_GET_STATS, &stats) < 0)
        return -1;
    snap->acquires = stats.ttm_resv_acquires;
    snap->releases = stats.ttm_resv_releases;
    snap->waits = stats.ttm_resv_waits;
    snap->conflicts = stats.ttm_resv_conflicts;
    snap->exclusive_fences = stats.ttm_resv_exclusive_fences;
    snap->shared_slots = stats.ttm_resv_shared_slots;
    snap->shared_used = stats.ttm_resv_shared_used;
    snap->shared_fences = stats.ttm_resv_shared_fences;
    snap->wait_queued = stats.ttm_resv_wait_queued;
    snap->wait_wakeups = stats.ttm_resv_wait_wakeups;
    snap->stale_fence_rejects = stats.ttm_resv_stale_fence_rejects;
    snap->attach_dmabuf_export = stats.ttm_resv_attach_dmabuf_export;
    snap->attach_dmabuf_import = stats.ttm_resv_attach_dmabuf_import;
    snap->last_attach_point = stats.ttm_resv_last_attach_point;
    snap->evict_pinned_rejects = stats.ttm_resv_evict_pinned_rejects;
    snap->evict_busy_rejects = stats.ttm_resv_evict_busy_rejects;
    snap->validate_failures = stats.ttm_validate_failures;
    snap->pinned_bytes = stats.ttm_pinned_bytes;
    snap->native_accel_credit = stats.ttm_native_accel_credit;
    snap->ww_contexts = stats.ttm_resv_ww_contexts;
    snap->ww_ordered_acquires = stats.ttm_resv_ww_ordered_acquires;
    snap->ww_deadlock_retries = stats.ttm_resv_ww_deadlock_retries;
    snap->ww_wound_backoffs = stats.ttm_resv_ww_wound_backoffs;
    snap->ww_multi_object = stats.ttm_resv_ww_multi_object;
    snap->ww_release_balance = stats.ttm_resv_ww_release_balance;
    snap->ww_max_acquired = stats.ttm_resv_ww_max_acquired;
    snap->ww_validate_failures = stats.ttm_resv_ww_validate_failures;
    return 0;
}

static void capture_ttm_move_path(const struct fb_gpu_ttm_validate *ttm,
                                  struct ttm_move_path_stats *snap)
{
    snap->move_count = ttm->move_count;
    snap->metadata_only_moves = ttm->metadata_only_moves;
    snap->real_copy_moves = ttm->real_copy_moves;
    snap->move_bytes = ttm->move_bytes;
    snap->native_accel_credit = ttm->native_accel_credit;
    for (int i = 0; i < 4; i++) {
        snap->manager_moves[i] = ttm->manager_moves[i];
        snap->cpu_copy_fallback_moves[i] =
            ttm->cpu_copy_fallback_moves[i];
        snap->metadata_noop_moves[i] = ttm->metadata_noop_moves[i];
        snap->unsupported_hw_copy_moves[i] =
            ttm->unsupported_hw_copy_moves[i];
        snap->real_copy_moves_by_domain[i] =
            ttm->real_copy_moves_by_domain[i];
    }
}

static int ttm_cpu_copy_move_observed(
    const struct ttm_move_path_stats *before,
    const struct fb_gpu_ttm_validate *after,
    uint32 mem_type, uint32 manager, uint64 size)
{
    if (after->mem_type != mem_type)
        return 0;
    if (after->move_count <= before->move_count)
        return 0;
    if (after->metadata_only_moves <= before->metadata_only_moves)
        return 0;
    if (after->move_bytes < before->move_bytes + size)
        return 0;
    if (after->manager_moves[manager] <= before->manager_moves[manager])
        return 0;
    if (after->cpu_copy_fallback_moves[manager] <=
        before->cpu_copy_fallback_moves[manager])
        return 0;
    if (after->metadata_noop_moves[manager] !=
        before->metadata_noop_moves[manager])
        return 0;
    if (after->unsupported_hw_copy_moves[manager] !=
        before->unsupported_hw_copy_moves[manager])
        return 0;
    if (after->real_copy_moves_by_domain[manager] !=
        before->real_copy_moves_by_domain[manager])
        return 0;
    if (after->real_copy_moves < before->real_copy_moves)
        return 0;
    if (after->native_accel_credit != before->native_accel_credit)
        return 0;
    return 1;
}

static int ttm_metadata_noop_observed(const struct ttm_move_path_stats *before,
                                      const struct fb_gpu_ttm_validate *after,
                                      uint32 mem_type)
{
    if (after->move_count != before->move_count)
        return 0;
    if (after->metadata_only_moves != before->metadata_only_moves)
        return 0;
    if (after->move_bytes != before->move_bytes)
        return 0;
    if (after->real_copy_moves != before->real_copy_moves)
        return 0;
    if (after->native_accel_credit != before->native_accel_credit)
        return 0;
    if (after->metadata_noop_moves[mem_type] <=
        before->metadata_noop_moves[mem_type])
        return 0;
    if (after->cpu_copy_fallback_moves[mem_type] !=
        before->cpu_copy_fallback_moves[mem_type])
        return 0;
    if (after->unsupported_hw_copy_moves[mem_type] !=
        before->unsupported_hw_copy_moves[mem_type])
        return 0;
    return 1;
}

static int ttm_unsupported_hw_copy_observed(
    const struct ttm_move_path_stats *before,
    const struct fb_gpu_ttm_validate *after,
    uint32 mem_type, uint32 manager, uint64 size)
{
    if (after->mem_type != mem_type)
        return 0;
    if (after->move_count <= before->move_count)
        return 0;
    if (after->metadata_only_moves <= before->metadata_only_moves)
        return 0;
    if (after->move_bytes < before->move_bytes + size)
        return 0;
    if (after->manager_moves[manager] <= before->manager_moves[manager])
        return 0;
    if (after->unsupported_hw_copy_moves[manager] <=
        before->unsupported_hw_copy_moves[manager])
        return 0;
    if (after->cpu_copy_fallback_moves[manager] !=
        before->cpu_copy_fallback_moves[manager])
        return 0;
    if (after->metadata_noop_moves[manager] !=
        before->metadata_noop_moves[manager])
        return 0;
    if (after->real_copy_moves_by_domain[manager] !=
        before->real_copy_moves_by_domain[manager])
        return 0;
    if (after->real_copy_moves != before->real_copy_moves)
        return 0;
    if (after->native_accel_credit != before->native_accel_credit)
        return 0;
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

static int reserve_ttm(int fd, uint32 handle, struct fb_gpu_ttm_validate *ttm)
{
    memset(ttm, 0, sizeof(*ttm));
    ttm->handle = handle;
    ttm->flags = FB_GPU_TTM_F_RESERVE;
    return ttm_ioctl(fd, ttm);
}

static int unreserve_ttm(int fd, uint32 handle,
                         struct fb_gpu_ttm_validate *ttm)
{
    memset(ttm, 0, sizeof(*ttm));
    ttm->handle = handle;
    ttm->flags = FB_GPU_TTM_F_UNRESERVE;
    return ttm_ioctl(fd, ttm);
}

static uchar ttm_pattern_byte(uint64 off)
{
    return (uchar)((off * 131 + (off >> 7) + 0x5d) & 0xff);
}

static void write_ttm_pattern(uint64 addr, uint64 size)
{
    volatile uchar *p = (volatile uchar *)addr;

    for (uint64 i = 0; i < size; i++)
        p[i] = ttm_pattern_byte(i);
}

static int check_ttm_pattern(uint64 addr, uint64 size)
{
    volatile uchar *p = (volatile uchar *)addr;

    for (uint64 i = 0; i < size; i++) {
        if (p[i] != ttm_pattern_byte(i))
            return -1;
    }
    return 0;
}

static int ttm_no_native_credit(const struct fb_gpu_ttm_validate *ttm)
{
    return ttm->native_accel_credit == 0;
}

static int set_ttm_check_content(int fd, uint32 handle, uint32 placement,
                                 uint32 mem_type, uint64 addr, uint64 size,
                                 struct fb_gpu_ttm_validate *ttm)
{
    if (set_ttm(fd, handle, placement, ttm) < 0)
        return -1;
    if (ttm->mem_type != mem_type || !ttm_no_native_credit(ttm))
        return -1;
    if (check_ttm_pattern(addr, size) < 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    struct fb_gpu_bo_create bo1;
    struct fb_gpu_bo_create bo2;
    struct fb_gpu_bo_export_fd export_fd;
    struct fb_gpu_bo_import_fd import_fd;
    struct fb_gpu_bo_destroy destroy;
    struct fb_gpu_ttm_validate ttm;
    struct fb_gpu_ttm_validate ttm2;
    struct ttm_resv_stats resv_before;
    struct ttm_resv_stats resv_after;
    struct ttm_resv_stats ww_before;
    struct ttm_resv_stats ww_after;
    struct ttm_move_path_stats move_start;
    struct ttm_move_path_stats move_before;
    uint64 moves;
    uint64 evictions;
    uint64 metadata_moves;
    uint64 move_bytes;
    uint64 system_manager_moves;
    uint64 tt_manager_moves;
    uint64 vram_manager_moves;
    uint64 resv_conflicts;
    uint64 resv_fence;
    uint64 resv_acquires_delta = 0;
    uint64 resv_releases_delta = 0;
    uint64 resv_waits_delta = 0;
    uint64 resv_conflicts_delta = 0;
    uint64 resv_exclusive_fences_delta = 0;
    uint64 resv_shared_fences_delta = 0;
    uint64 resv_validate_failures_delta = 0;
    uint64 resv_evict_pinned_rejects_delta = 0;
    uint64 resv_evict_busy_rejects_delta = 0;
    uint64 resv_pinned_before_evict = 0;
    uint64 ww_contexts_delta = 0;
    uint64 ww_ordered_acquires_delta = 0;
    uint64 ww_deadlock_retries_delta = 0;
    uint64 ww_wound_backoffs_delta = 0;
    uint64 ww_multi_object_delta = 0;
    uint64 ww_release_balance_delta = 0;
    uint64 content_migrations = 0;
    uint64 cpu_copy_fallback_compatible = 0;
    uint64 metadata_noop_checks = 0;
    uint64 hardware_copy_unsupported = 0;
    uint64 real_copy_path_moves = 0;
    int fd;
    int fd2 = -1;
    int bo_cap_fd = -1;
    int ret = 1;

    (void)argc;
    (void)argv;
    memset(&bo1, 0, sizeof(bo1));
    memset(&bo2, 0, sizeof(bo2));
    memset(&import_fd, 0, sizeof(import_fd));

    fd = open("/dev/gpu0", O_RDWR);
    if (fd < 0)
        return fail("open /dev/gpu0 failed");
    if (create_bo(fd, 64, 64, &bo1) < 0)
        goto out_fail_create1;
    if (create_bo(fd, 32, 32, &bo2) < 0)
        goto out_fail_create2;
    if (query_ttm(fd, bo1.handle, &ttm) < 0)
        goto out_fail;
    if (ttm.mem_type != 0 || !ttm_no_native_credit(&ttm))
        goto out_fail;
    write_ttm_pattern(bo1.addr, bo1.size);
    if (check_ttm_pattern(bo1.addr, bo1.size) < 0)
        goto out_fail;
    metadata_moves = ttm.metadata_only_moves;
    move_bytes = ttm.move_bytes;
    system_manager_moves = ttm.manager_moves[0];
    tt_manager_moves = ttm.manager_moves[1];
    vram_manager_moves = ttm.manager_moves[2];
    resv_conflicts = ttm.resv_conflicts;
    capture_ttm_move_path(&ttm, &move_start);
    move_before = move_start;

    if (set_ttm_check_content(fd, bo1.handle, FB_GPU_TTM_PL_TT, 1,
                              bo1.addr, bo1.size, &ttm) < 0)
        goto out_fail;
    if (!ttm_cpu_copy_move_observed(&move_before, &ttm, 1, 1, bo1.size))
        goto out_fail;
    content_migrations++;
    cpu_copy_fallback_compatible++;
    if (ttm.mem_type != 1 || ttm.tt_populated == 0 ||
        ttm.sg_nents == 0 || ttm.dma_addr_base == 0 ||
        ttm.manager_bytes[1] < bo1.size ||
        ttm.metadata_only_moves <= metadata_moves ||
        ttm.native_accel_credit != 0 ||
        ttm.move_bytes < move_bytes + bo1.size ||
        ttm.manager_moves[1] <= tt_manager_moves)
        goto out_fail;
    metadata_moves = ttm.metadata_only_moves;
    move_bytes = ttm.move_bytes;
    tt_manager_moves = ttm.manager_moves[1];
    moves = ttm.move_count;
    capture_ttm_move_path(&ttm, &move_before);

    if (read_ttm_resv_stats(fd, &resv_before) < 0)
        goto out_fail;

    memset(&export_fd, 0, sizeof(export_fd));
    export_fd.handle = bo1.handle;
    if (ioctl(fd, FB_GPU_BO_EXPORT_FD, &export_fd) < 0 || export_fd.fd < 0)
        goto out_fail;
    bo_cap_fd = export_fd.fd;
    fd2 = open("/dev/gpu0", O_RDWR);
    if (fd2 < 0)
        goto out_fail;
    memset(&import_fd, 0, sizeof(import_fd));
    import_fd.fd = bo_cap_fd;
    if (ioctl(fd2, FB_GPU_BO_IMPORT_FD, &import_fd) < 0 ||
        import_fd.handle == 0)
        goto out_fail;

    if (reserve_ttm(fd, bo1.handle, &ttm) < 0 ||
        ttm.resv_count == 0 || ttm.resv_exclusive_fence == 0)
        goto out_fail;
    resv_fence = ttm.resv_exclusive_fence;
    if (reserve_ttm(fd2, import_fd.handle, &ttm2) >= 0)
        goto out_fail;
    if (query_ttm(fd, bo1.handle, &ttm) < 0 ||
        ttm.resv_conflicts <= resv_conflicts ||
        ttm.resv_exclusive_fence != resv_fence)
        goto out_fail;
    resv_conflicts = ttm.resv_conflicts;
    if (set_ttm(fd2, import_fd.handle, FB_GPU_TTM_PL_TT, &ttm2) >= 0)
        goto out_fail;
    if (query_ttm(fd, bo1.handle, &ttm) < 0 ||
        ttm.resv_conflicts <= resv_conflicts)
        goto out_fail;
    memset(&ttm2, 0, sizeof(ttm2));
    ttm2.handle = import_fd.handle;
    ttm2.flags = FB_GPU_TTM_F_FORCE_EVICT;
    ttm2.placement = FB_GPU_TTM_PL_TT;
    if (ttm_ioctl(fd2, &ttm2) >= 0)
        goto out_fail;
    if (unreserve_ttm(fd, bo1.handle, &ttm) < 0 ||
        ttm.resv_count != 0 ||
        ttm.resv_exclusive_fence <= resv_fence)
        goto out_fail;
    if (reserve_ttm(fd2, import_fd.handle, &ttm2) < 0 ||
        ttm2.resv_count == 0)
        goto out_fail;
    if (unreserve_ttm(fd2, import_fd.handle, &ttm2) < 0 ||
        ttm2.resv_count != 0)
        goto out_fail;
    if (read_ttm_resv_stats(fd, &resv_after) < 0)
        goto out_fail;
    if (resv_after.acquires < resv_before.acquires + 2 ||
        resv_after.releases < resv_before.releases + 2 ||
        resv_after.waits < resv_before.waits + 2 ||
        resv_after.conflicts < resv_before.conflicts + 2 ||
        resv_after.exclusive_fences < resv_before.exclusive_fences + 2 ||
        resv_after.shared_slots == 0 ||
        resv_after.shared_used == 0 ||
        resv_after.shared_fences < resv_before.shared_fences + 2 ||
        resv_after.attach_dmabuf_export <
            resv_before.attach_dmabuf_export + 1 ||
        resv_after.attach_dmabuf_import <
            resv_before.attach_dmabuf_import + 1 ||
        resv_after.native_accel_credit != resv_before.native_accel_credit)
        goto out_fail;
    resv_acquires_delta = resv_after.acquires - resv_before.acquires;
    resv_releases_delta = resv_after.releases - resv_before.releases;
    resv_waits_delta = resv_after.waits - resv_before.waits;
    resv_conflicts_delta = resv_after.conflicts - resv_before.conflicts;
    resv_exclusive_fences_delta =
        resv_after.exclusive_fences - resv_before.exclusive_fences;
    resv_shared_fences_delta =
        resv_after.shared_fences - resv_before.shared_fences;

    ww_before = resv_after;
    memset(&ttm2, 0, sizeof(ttm2));
    ttm2.handle = bo2.handle;
    ttm2.flags = FB_GPU_TTM_F_WW_VALIDATE;
    ttm2.peer_handle = bo1.handle;
    if (ttm_ioctl(fd, &ttm2) < 0)
        goto out_fail;
    if (read_ttm_resv_stats(fd, &ww_after) < 0)
        goto out_fail;
    if (ww_after.ww_contexts < ww_before.ww_contexts + 1 ||
        ww_after.ww_ordered_acquires < ww_before.ww_ordered_acquires + 2 ||
        ww_after.ww_deadlock_retries < ww_before.ww_deadlock_retries + 1 ||
        ww_after.ww_wound_backoffs < ww_before.ww_wound_backoffs + 1 ||
        ww_after.ww_multi_object < ww_before.ww_multi_object + 1 ||
        ww_after.ww_release_balance < ww_before.ww_release_balance + 2 ||
        ww_after.ww_max_acquired < 2 ||
        ww_after.ww_validate_failures != ww_before.ww_validate_failures ||
        ww_after.native_accel_credit != ww_before.native_accel_credit)
        goto out_fail;
    ww_contexts_delta = ww_after.ww_contexts - ww_before.ww_contexts;
    ww_ordered_acquires_delta =
        ww_after.ww_ordered_acquires - ww_before.ww_ordered_acquires;
    ww_deadlock_retries_delta =
        ww_after.ww_deadlock_retries - ww_before.ww_deadlock_retries;
    ww_wound_backoffs_delta =
        ww_after.ww_wound_backoffs - ww_before.ww_wound_backoffs;
    ww_multi_object_delta =
        ww_after.ww_multi_object - ww_before.ww_multi_object;
    ww_release_balance_delta =
        ww_after.ww_release_balance - ww_before.ww_release_balance;

    if (set_ttm_check_content(fd, bo1.handle, FB_GPU_TTM_PL_SYSTEM, 0,
                              bo1.addr, bo1.size, &ttm) < 0)
        goto out_fail;
    if (!ttm_cpu_copy_move_observed(&move_before, &ttm, 0, 0, bo1.size))
        goto out_fail;
    if (ttm.mem_type != 0 || ttm.move_count <= moves ||
        ttm.metadata_only_moves <= metadata_moves ||
        ttm.native_accel_credit != 0 ||
        ttm.move_bytes < move_bytes + bo1.size ||
        ttm.manager_moves[0] <= system_manager_moves)
        goto out_fail;
    content_migrations++;
    cpu_copy_fallback_compatible++;
    metadata_moves = ttm.metadata_only_moves;
    move_bytes = ttm.move_bytes;
    system_manager_moves = ttm.manager_moves[0];
    moves = ttm.move_count;
    capture_ttm_move_path(&ttm, &move_before);

    if (set_ttm_check_content(fd, bo1.handle, FB_GPU_TTM_PL_TT, 1,
                              bo1.addr, bo1.size, &ttm) < 0)
        goto out_fail;
    if (!ttm_cpu_copy_move_observed(&move_before, &ttm, 1, 1, bo1.size))
        goto out_fail;
    content_migrations++;
    cpu_copy_fallback_compatible++;
    if (ttm.mem_type != 1 || ttm.tt_populated == 0 ||
        ttm.sg_nents == 0 || ttm.dma_addr_base == 0 ||
        ttm.manager_bytes[1] < bo1.size)
        goto out_fail;
    if (ttm.metadata_only_moves <= metadata_moves ||
        ttm.native_accel_credit != 0 ||
        ttm.move_bytes < move_bytes + bo1.size ||
        ttm.manager_moves[1] <= tt_manager_moves)
        goto out_fail;
    metadata_moves = ttm.metadata_only_moves;
    move_bytes = ttm.move_bytes;
    tt_manager_moves = ttm.manager_moves[1];
    moves = ttm.move_count;
    capture_ttm_move_path(&ttm, &move_before);

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

    if (set_ttm_check_content(fd, bo1.handle, FB_GPU_TTM_PL_VRAM, 2,
                              bo1.addr, bo1.size, &ttm) < 0)
        goto out_fail;
    if (!ttm_cpu_copy_move_observed(&move_before, &ttm, 2, 2, bo1.size))
        goto out_fail;
    content_migrations++;
    cpu_copy_fallback_compatible++;
    if (ttm.mem_type != 2 || ttm.move_count <= moves ||
        ttm.manager_bytes[2] < bo1.size)
        goto out_fail;
    if (ttm.metadata_only_moves <= metadata_moves ||
        ttm.native_accel_credit != 0 ||
        ttm.move_bytes < move_bytes + bo1.size ||
        ttm.manager_moves[2] <= vram_manager_moves)
        goto out_fail;
    metadata_moves = ttm.metadata_only_moves;
    move_bytes = ttm.move_bytes;
    vram_manager_moves = ttm.manager_moves[2];
    moves = ttm.move_count;
    capture_ttm_move_path(&ttm, &move_before);
    if (set_ttm_check_content(fd, bo1.handle, FB_GPU_TTM_PL_VRAM, 2,
                              bo1.addr, bo1.size, &ttm) < 0)
        goto out_fail;
    if (!ttm_metadata_noop_observed(&move_before, &ttm, 2))
        goto out_fail;
    metadata_noop_checks++;
    if (ttm.mem_type != 2 || ttm.move_count != moves ||
        ttm.metadata_only_moves != metadata_moves ||
        ttm.move_bytes != move_bytes ||
        ttm.native_accel_credit != 0)
        goto out_fail;

    capture_ttm_move_path(&ttm, &move_before);
    if (set_ttm_check_content(fd, bo1.handle, FB_GPU_TTM_PL_STOLEN, 3,
                              bo1.addr, bo1.size, &ttm) < 0)
        goto out_fail;
    if (!ttm_unsupported_hw_copy_observed(&move_before, &ttm, 3, 3,
                                          bo1.size))
        goto out_fail;
    hardware_copy_unsupported++;
    content_migrations++;
    metadata_moves = ttm.metadata_only_moves;
    move_bytes = ttm.move_bytes;
    moves = ttm.move_count;
    capture_ttm_move_path(&ttm, &move_before);

    if (set_ttm_check_content(fd, bo1.handle, FB_GPU_TTM_PL_SYSTEM, 0,
                              bo1.addr, bo1.size, &ttm) < 0)
        goto out_fail;
    if (!ttm_cpu_copy_move_observed(&move_before, &ttm, 0, 0, bo1.size))
        goto out_fail;
    content_migrations++;
    cpu_copy_fallback_compatible++;
    if (ttm.move_count <= moves || ttm.manager_bytes[0] < bo1.size)
        goto out_fail;
    if (ttm.metadata_only_moves <= metadata_moves ||
        ttm.native_accel_credit != 0 ||
        ttm.move_bytes < move_bytes + bo1.size ||
        ttm.manager_moves[0] <= system_manager_moves)
        goto out_fail;
    metadata_moves = ttm.metadata_only_moves;
    move_bytes = ttm.move_bytes;
    system_manager_moves = ttm.manager_moves[0];
    capture_ttm_move_path(&ttm, &move_before);

    if (set_ttm(fd, bo2.handle, FB_GPU_TTM_PL_TT, &ttm2) < 0 ||
        ttm2.mem_type != 1)
        goto out_fail;
    capture_ttm_move_path(&ttm2, &move_before);
    memset(&ttm2, 0, sizeof(ttm2));
    ttm2.handle = bo2.handle;
    ttm2.flags = FB_GPU_TTM_F_PIN;
    if (ttm_ioctl(fd, &ttm2) < 0 || ttm2.pin_count != 1)
        goto out_fail;
    resv_pinned_before_evict = ttm2.pin_count;
    memset(&ttm2, 0, sizeof(ttm2));
    ttm2.handle = bo2.handle;
    ttm2.flags = FB_GPU_TTM_F_FORCE_EVICT;
    ttm2.placement = FB_GPU_TTM_PL_TT;
    if (ttm_ioctl(fd, &ttm2) >= 0)
        goto out_fail;
    memset(&ttm2, 0, sizeof(ttm2));
    ttm2.handle = bo2.handle;
    ttm2.flags = FB_GPU_TTM_F_UNPIN;
    if (ttm_ioctl(fd, &ttm2) < 0 || ttm2.pin_count != 0)
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
    if (!ttm_cpu_copy_move_observed(&move_before, &ttm2, 0, 0, bo2.size))
        goto out_fail;
    cpu_copy_fallback_compatible++;
    if (ttm2.metadata_only_moves <= metadata_moves ||
        ttm2.native_accel_credit != 0 ||
        ttm2.manager_moves[0] <= system_manager_moves)
        goto out_fail;
    if (query_ttm(fd, bo2.handle, &ttm2) < 0)
        goto out_fail;
    if (ttm2.mem_type != 0)
        goto out_fail;
    if (read_ttm_resv_stats(fd, &resv_after) < 0 ||
        resv_after.validate_failures < resv_before.validate_failures + 4 ||
        resv_after.evict_pinned_rejects <
            resv_before.evict_pinned_rejects + 1 ||
        resv_after.evict_busy_rejects <
            resv_before.evict_busy_rejects + 1 ||
        resv_after.pinned_bytes != resv_before.pinned_bytes)
        goto out_fail;
    resv_validate_failures_delta =
        resv_after.validate_failures - resv_before.validate_failures;
    resv_evict_pinned_rejects_delta =
        resv_after.evict_pinned_rejects -
            resv_before.evict_pinned_rejects;
    resv_evict_busy_rejects_delta =
        resv_after.evict_busy_rejects - resv_before.evict_busy_rejects;

    memset(&ttm, 0, sizeof(ttm));
    ttm.handle = bo1.handle;
    ttm.flags = FB_GPU_TTM_F_SET_PLACEMENT;
    ttm.placement = 0;
    if (ttm_ioctl(fd, &ttm) >= 0)
        goto out_fail;

    if (ttm2.real_copy_moves < move_start.real_copy_moves ||
        ttm2.native_accel_credit < move_start.native_accel_credit)
        goto out_fail;
    real_copy_path_moves = ttm2.real_copy_moves - move_start.real_copy_moves;
    if (hardware_copy_unsupported == 0 ||
        real_copy_path_moves != 0 ||
        ttm2.native_accel_credit != move_start.native_accel_credit)
        goto out_fail;

    ret = 0;
    printf("ttmtest: ttm_move_path_matrix "
           "cpu_copy_fallback_compatible=%lu metadata_noop_checks=%lu "
           "hardware_copy_unsupported=%lu real_copy_path_moves=%lu "
           "aggregate_metadata_only_delta=%lu "
           "aggregate_move_bytes_delta=%lu "
           "cpu_copy_by_domain=%lu/%lu/%lu/%lu "
           "metadata_noop_by_domain=%lu/%lu/%lu/%lu "
           "unsupported_hw_copy_by_domain=%lu/%lu/%lu/%lu "
           "real_copy_by_domain=%lu/%lu/%lu/%lu "
           "per_domain_fields=present status=PASS\n",
           cpu_copy_fallback_compatible, metadata_noop_checks,
           hardware_copy_unsupported, real_copy_path_moves,
           ttm2.metadata_only_moves - move_start.metadata_only_moves,
           ttm2.move_bytes - move_start.move_bytes,
           ttm2.cpu_copy_fallback_moves[0] -
               move_start.cpu_copy_fallback_moves[0],
           ttm2.cpu_copy_fallback_moves[1] -
               move_start.cpu_copy_fallback_moves[1],
           ttm2.cpu_copy_fallback_moves[2] -
               move_start.cpu_copy_fallback_moves[2],
           ttm2.cpu_copy_fallback_moves[3] -
               move_start.cpu_copy_fallback_moves[3],
           ttm2.metadata_noop_moves[0] -
               move_start.metadata_noop_moves[0],
           ttm2.metadata_noop_moves[1] -
               move_start.metadata_noop_moves[1],
           ttm2.metadata_noop_moves[2] -
               move_start.metadata_noop_moves[2],
           ttm2.metadata_noop_moves[3] -
               move_start.metadata_noop_moves[3],
           ttm2.unsupported_hw_copy_moves[0] -
               move_start.unsupported_hw_copy_moves[0],
           ttm2.unsupported_hw_copy_moves[1] -
               move_start.unsupported_hw_copy_moves[1],
           ttm2.unsupported_hw_copy_moves[2] -
               move_start.unsupported_hw_copy_moves[2],
           ttm2.unsupported_hw_copy_moves[3] -
               move_start.unsupported_hw_copy_moves[3],
           ttm2.real_copy_moves_by_domain[0] -
               move_start.real_copy_moves_by_domain[0],
           ttm2.real_copy_moves_by_domain[1] -
               move_start.real_copy_moves_by_domain[1],
           ttm2.real_copy_moves_by_domain[2] -
               move_start.real_copy_moves_by_domain[2],
           ttm2.real_copy_moves_by_domain[3] -
               move_start.real_copy_moves_by_domain[3]);
    printf("ttmtest: ttm_resv_wait_matrix acquires_delta=%lu "
           "releases_delta=%lu waits_delta=%lu conflicts_delta=%lu "
           "exclusive_fences_delta=%lu imported_conflict_rejected=1 "
           "reserved_set_placement_rejected=1 "
           "reserved_force_evict_rejected=1 "
           "shared_slots=%lu shared_used=%lu shared_fences_delta=%lu "
           "dmabuf_attach_export_delta=%lu dmabuf_attach_import_delta=%lu "
           "last_attach_point=%lu native_accel_credit_delta=0 "
           "status=PASS\n",
           resv_acquires_delta, resv_releases_delta, resv_waits_delta,
           resv_conflicts_delta, resv_exclusive_fences_delta,
           resv_after.shared_slots, resv_after.shared_used,
           resv_shared_fences_delta,
           resv_after.attach_dmabuf_export -
               resv_before.attach_dmabuf_export,
           resv_after.attach_dmabuf_import -
               resv_before.attach_dmabuf_import,
           resv_after.last_attach_point);
    printf("ttmtest: ttm_dma_resv_ww_mutex_matrix "
           "ww_contexts_delta=%lu ordered_acquires_delta=%lu "
           "deadlock_retries_delta=%lu wound_backoffs_delta=%lu "
           "multi_object_delta=%lu release_balance_delta=%lu "
           "max_acquired=%lu validate_failures_delta=0 "
           "native_accel_credit_delta=0 status=PASS\n",
           ww_contexts_delta, ww_ordered_acquires_delta,
           ww_deadlock_retries_delta, ww_wound_backoffs_delta,
           ww_multi_object_delta, ww_release_balance_delta,
           ww_after.ww_max_acquired);
    printf("ttmtest: ttm_eviction_negative_matrix "
           "pinned_evict_rejected=1 pinned_before=%lu "
           "pinned_bytes_restored=1 validate_failures_delta=%lu "
           "evict_pinned_rejects_delta=%lu "
           "evict_busy_rejects_delta=%lu "
           "native_accel_credit_delta=0 status=PASS\n",
           resv_pinned_before_evict, resv_validate_failures_delta,
           resv_evict_pinned_rejects_delta,
           resv_evict_busy_rejects_delta);
    printf("ttmtest: ok tt_bytes=%lu vram_bytes=%lu evictions=%lu "
           "metadata_moves=%lu real_copy_moves=%lu native_accel_credit=%lu "
           "resv_conflicts=%lu content_migrations=%lu\n",
           ttm2.manager_bytes[1], ttm2.manager_bytes[2], ttm2.evictions,
           ttm2.metadata_only_moves, ttm2.real_copy_moves,
           ttm2.native_accel_credit, ttm2.resv_conflicts,
           content_migrations);

out_fail:
    if (import_fd.handle != 0) {
        if (import_fd.addr != 0 && import_fd.size != 0)
            (void)munmap((void *)import_fd.addr, (int)import_fd.size);
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = import_fd.handle;
        if (fd2 >= 0)
            (void)ioctl(fd2, FB_GPU_BO_DESTROY, &destroy);
    }
    if (bo_cap_fd >= 0)
        close(bo_cap_fd);
    if (fd2 >= 0)
        close(fd2);
    (void)destroy_bo(fd, &bo2);
out_fail_create2:
    (void)destroy_bo(fd, &bo1);
out_fail_create1:
    close(fd);
    if (ret != 0)
        return fail("validation failed");
    return 0;
}

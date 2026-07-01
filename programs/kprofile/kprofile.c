#include "kernel/inc/types.h"
#include "kernel/inc/kstats.h"
#include "user/user.h"

static void print_delta(const char *name, uint64 after, uint64 before)
{
    printf("%-28s %lu\n", name, (unsigned long)(after - before));
}

static void print_tick_delta_ms(const char *name, uint64 after, uint64 before,
                                uint64 timebase_freq)
{
    uint64 delta = after - before;
    uint64 ms = timebase_freq ? (delta * 1000ULL) / timebase_freq : 0;
    printf("%-28s %lu\n", name, (unsigned long)ms);
}

static int read_kstats(struct kstats *ks)
{
    return kstats2(ks, sizeof(*ks));
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: kprofile <command> [args...]\n");
        exit(1);
    }

    struct kstats before;
    struct kstats after;

    if (kstatsctl(1) < 0) {
        printf("kprofile: kstatsctl(enable) failed\n");
        exit(1);
    }

    if (read_kstats(&before) < 0) {
        printf("kprofile: kstats(before) failed\n");
        kstatsctl(0);
        exit(1);
    }

    int pid = fork();
    if (pid < 0) {
        printf("kprofile: fork failed\n");
        kstatsctl(0);
        exit(1);
    }

    if (pid == 0) {
        exec(argv[1], &argv[1]);
        printf("kprofile: exec %s failed\n", argv[1]);
        exit(1);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (read_kstats(&after) < 0) {
        printf("kprofile: kstats(after) failed\n");
        kstatsctl(0);
        exit(1);
    }
    kstatsctl(0);

    printf("elapsed_ms                  %lu\n",
           (unsigned long)(after.uptime_ms - before.uptime_ms));
    print_delta("vfs_lookup_calls", after.vfs_lookup_calls,
                before.vfs_lookup_calls);
    print_delta("vfs_lookup_dcache_hits", after.vfs_lookup_dcache_hits,
                before.vfs_lookup_dcache_hits);
    print_delta("vfs_lookup_negative_hits", after.vfs_lookup_negative_hits,
                before.vfs_lookup_negative_hits);
    print_delta("vfs_lookup_cache_misses", after.vfs_lookup_cache_misses,
                before.vfs_lookup_cache_misses);
    print_delta("vfs_lookup_driver_calls", after.vfs_lookup_driver_calls,
                before.vfs_lookup_driver_calls);
    print_tick_delta_ms("vfs_lookup_driver_ms",
                        after.vfs_lookup_driver_ticks,
                        before.vfs_lookup_driver_ticks,
                        after.timebase_freq);
    print_delta("vfs_dentry_inode_calls", after.vfs_dentry_inode_calls,
                before.vfs_dentry_inode_calls);
    print_tick_delta_ms("vfs_dentry_inode_ms",
                        after.vfs_dentry_inode_ticks,
                        before.vfs_dentry_inode_ticks,
                        after.timebase_freq);
    print_delta("vfs_dentry_inode_retries",
                after.vfs_dentry_inode_retries,
                before.vfs_dentry_inode_retries);
    print_delta("vfs_dentry_inode_self_hits",
                after.vfs_dentry_inode_self_hits,
                before.vfs_dentry_inode_self_hits);
    print_delta("vfs_dentry_inode_rlock_calls",
                after.vfs_dentry_inode_rlock_calls,
                before.vfs_dentry_inode_rlock_calls);
    print_tick_delta_ms("vfs_dentry_inode_rlock_ms",
                        after.vfs_dentry_inode_rlock_ticks,
                        before.vfs_dentry_inode_rlock_ticks,
                        after.timebase_freq);
    print_delta("vfs_dentry_inode_upgrade_calls",
                after.vfs_dentry_inode_upgrade_calls,
                before.vfs_dentry_inode_upgrade_calls);
    print_tick_delta_ms("vfs_dentry_inode_upgrade_ms",
                        after.vfs_dentry_inode_upgrade_ticks,
                        before.vfs_dentry_inode_upgrade_ticks,
                        after.timebase_freq);
    print_delta("vfs_inode_cache_calls", after.vfs_inode_cache_calls,
                before.vfs_inode_cache_calls);
    print_delta("vfs_inode_cache_hits", after.vfs_inode_cache_hits,
                before.vfs_inode_cache_hits);
    print_delta("vfs_inode_cache_misses", after.vfs_inode_cache_misses,
                before.vfs_inode_cache_misses);
    print_delta("vfs_inode_cache_eagain", after.vfs_inode_cache_eagain,
                before.vfs_inode_cache_eagain);
    print_delta("vfs_inode_cache_miss_hash",
                after.vfs_inode_cache_miss_hash,
                before.vfs_inode_cache_miss_hash);
    print_delta("vfs_inode_cache_miss_revive_without_wlock",
                after.vfs_inode_cache_miss_revive_without_wlock,
                before.vfs_inode_cache_miss_revive_without_wlock);
    print_delta("vfs_inode_cache_miss_dying",
                after.vfs_inode_cache_miss_dying,
                before.vfs_inode_cache_miss_dying);
    print_delta("vfs_inode_cache_miss_invalid_destroying",
                after.vfs_inode_cache_miss_invalid_destroying,
                before.vfs_inode_cache_miss_invalid_destroying);
    print_delta("vfs_inode_cache_read_revive_attempts",
                after.vfs_inode_cache_read_revive_attempts,
                before.vfs_inode_cache_read_revive_attempts);
    print_delta("vfs_inode_cache_read_revive_success",
                after.vfs_inode_cache_read_revive_success,
                before.vfs_inode_cache_read_revive_success);
    print_delta("vfs_inode_cache_read_revive_lock_fail",
                after.vfs_inode_cache_read_revive_lock_fail,
                before.vfs_inode_cache_read_revive_lock_fail);
    print_delta("vfs_inode_cache_read_revive_stale",
                after.vfs_inode_cache_read_revive_stale,
                before.vfs_inode_cache_read_revive_stale);
    print_tick_delta_ms("vfs_inode_cache_ms",
                        after.vfs_inode_cache_ticks,
                        before.vfs_inode_cache_ticks,
                        after.timebase_freq);
    print_delta("vfs_inode_load_calls", after.vfs_inode_load_calls,
                before.vfs_inode_load_calls);
    print_delta("vfs_inode_load_success", after.vfs_inode_load_success,
                before.vfs_inode_load_success);
    print_tick_delta_ms("vfs_inode_load_ms",
                        after.vfs_inode_load_ticks,
                        before.vfs_inode_load_ticks,
                        after.timebase_freq);
    print_delta("vm_copyin_calls", after.vm_copyin_calls,
                before.vm_copyin_calls);
    print_delta("vm_copyout_calls", after.vm_copyout_calls,
                before.vm_copyout_calls);
    print_delta("vm_copyin_bytes", after.vm_copyin_bytes,
                before.vm_copyin_bytes);
    print_delta("vm_copyout_bytes", after.vm_copyout_bytes,
                before.vm_copyout_bytes);
    print_delta("vm_copyout_fast_hits", after.vm_copyout_fast_hits,
                before.vm_copyout_fast_hits);
    print_delta("vm_copyout_fast_bytes", after.vm_copyout_fast_bytes,
                before.vm_copyout_fast_bytes);
    print_delta("vm_copyin_fast_hits", after.vm_copyin_fast_hits,
                before.vm_copyin_fast_hits);
    print_delta("vm_copyin_fast_bytes", after.vm_copyin_fast_bytes,
                before.vm_copyin_fast_bytes);
    print_delta("vm_copyout_present_skip_hits",
                after.vm_copyout_present_skip_hits,
                before.vm_copyout_present_skip_hits);
    print_delta("vm_copyout_present_skip_bytes",
                after.vm_copyout_present_skip_bytes,
                before.vm_copyout_present_skip_bytes);
    print_delta("vm_copyin_present_skip_hits",
                after.vm_copyin_present_skip_hits,
                before.vm_copyin_present_skip_hits);
    print_delta("vm_copyin_present_skip_bytes",
                after.vm_copyin_present_skip_bytes,
                before.vm_copyin_present_skip_bytes);
    print_delta("vm_vma_validate_calls", after.vm_vma_validate_calls,
                before.vm_vma_validate_calls);
    print_tick_delta_ms("vm_vma_validate_ms",
                        after.vm_vma_validate_ticks,
                        before.vm_vma_validate_ticks,
                        after.timebase_freq);
    print_delta("vm_file_faults", after.vm_file_faults,
                before.vm_file_faults);
    print_tick_delta_ms("vm_validate_batch_ms",
                        after.vm_validate_batch_ticks,
                        before.vm_validate_batch_ticks,
                        after.timebase_freq);
    print_tick_delta_ms("vm_validate_fallback_ms",
                        after.vm_validate_fallback_ticks,
                        before.vm_validate_fallback_ticks,
                        after.timebase_freq);
    print_tick_delta_ms("vm_validate_hugepage_ms",
                        after.vm_validate_hugepage_ticks,
                        before.vm_validate_hugepage_ticks,
                        after.timebase_freq);
    print_tick_delta_ms("vm_validate_pte_check_ms",
                        after.vm_validate_pte_check_ticks,
                        before.vm_validate_pte_check_ticks,
                        after.timebase_freq);
    print_tick_delta_ms("vm_copyin_ms", after.vm_copyin_ticks,
                        before.vm_copyin_ticks, after.timebase_freq);
    print_tick_delta_ms("vm_copyout_ms", after.vm_copyout_ticks,
                        before.vm_copyout_ticks, after.timebase_freq);
    print_delta("ext4_pcache_read_page_calls",
                after.ext4_pcache_read_page_calls,
                before.ext4_pcache_read_page_calls);
    print_delta("ext4_pcache_pages_filled",
                after.ext4_pcache_pages_filled,
                before.ext4_pcache_pages_filled);
    print_delta("ext4_pcache_readahead_pages",
                after.ext4_pcache_readahead_pages,
                before.ext4_pcache_readahead_pages);
    print_tick_delta_ms("ext4_pcache_read_page_ms",
                        after.ext4_pcache_read_page_ticks,
                        before.ext4_pcache_read_page_ticks,
                        after.timebase_freq);
    print_delta("ext4_fault_calls", after.ext4_fault_calls,
                before.ext4_fault_calls);
    print_delta("ext4_fault_zero_copy", after.ext4_fault_zero_copy,
                before.ext4_fault_zero_copy);
    print_delta("ext4_fault_partial_copy", after.ext4_fault_partial_copy,
                before.ext4_fault_partial_copy);
    print_tick_delta_ms("ext4_fault_ms", after.ext4_fault_ticks,
                        before.ext4_fault_ticks, after.timebase_freq);
    print_delta("ext4_lookup_calls", after.ext4_lookup_calls,
                before.ext4_lookup_calls);
    print_tick_delta_ms("ext4_lookup_lock_wait_ms",
                        after.ext4_lookup_lock_wait_ticks,
                        before.ext4_lookup_lock_wait_ticks,
                        after.timebase_freq);
    print_tick_delta_ms("ext4_lookup_lock_hold_ms",
                        after.ext4_lookup_lock_hold_ticks,
                        before.ext4_lookup_lock_hold_ticks,
                        after.timebase_freq);
    print_tick_delta_ms("ext4_lookup_parent_ref_ms",
                        after.ext4_lookup_parent_ref_ticks,
                        before.ext4_lookup_parent_ref_ticks,
                        after.timebase_freq);
    print_tick_delta_ms("ext4_lookup_dir_find_ms",
                        after.ext4_lookup_dir_find_ticks,
                        before.ext4_lookup_dir_find_ticks,
                        after.timebase_freq);
    print_delta("ext4_lookup_found", after.ext4_lookup_found,
                before.ext4_lookup_found);
    print_delta("ext4_lookup_enoent", after.ext4_lookup_enoent,
                before.ext4_lookup_enoent);
    print_delta("ext4_lookup_errors", after.ext4_lookup_errors,
                before.ext4_lookup_errors);
    print_delta("sys_open_calls", after.sys_open_calls,
                before.sys_open_calls);
    print_tick_delta_ms("sys_open_ms", after.sys_open_ticks,
                        before.sys_open_ticks, after.timebase_freq);
    print_delta("sys_fstat_calls", after.sys_fstat_calls,
                before.sys_fstat_calls);
    print_tick_delta_ms("sys_fstat_ms", after.sys_fstat_ticks,
                        before.sys_fstat_ticks, after.timebase_freq);
    print_delta("sys_lseek_calls", after.sys_lseek_calls,
                before.sys_lseek_calls);
    print_tick_delta_ms("sys_lseek_ms", after.sys_lseek_ticks,
                        before.sys_lseek_ticks, after.timebase_freq);
    print_delta("sys_pread64_calls", after.sys_pread64_calls,
                before.sys_pread64_calls);
    print_tick_delta_ms("sys_pread64_ms", after.sys_pread64_ticks,
                        before.sys_pread64_ticks, after.timebase_freq);
    print_delta("sys_openat_calls", after.sys_openat_calls,
                before.sys_openat_calls);
    print_tick_delta_ms("sys_openat_ms", after.sys_openat_ticks,
                        before.sys_openat_ticks, after.timebase_freq);
    print_delta("sys_openat_path_copy_calls",
                after.sys_openat_path_copy_calls,
                before.sys_openat_path_copy_calls);
    print_tick_delta_ms("sys_openat_path_copy_ms",
                        after.sys_openat_path_copy_ticks,
                        before.sys_openat_path_copy_ticks,
                        after.timebase_freq);
    print_delta("sys_openat_dirfd_calls", after.sys_openat_dirfd_calls,
                before.sys_openat_dirfd_calls);
    print_tick_delta_ms("sys_openat_dirfd_ms",
                        after.sys_openat_dirfd_ticks,
                        before.sys_openat_dirfd_ticks,
                        after.timebase_freq);
    print_delta("sys_openat_lookup_calls", after.sys_openat_lookup_calls,
                before.sys_openat_lookup_calls);
    print_tick_delta_ms("sys_openat_lookup_ms",
                        after.sys_openat_lookup_ticks,
                        before.sys_openat_lookup_ticks,
                        after.timebase_freq);
    print_delta("sys_openat_fileopen_calls",
                after.sys_openat_fileopen_calls,
                before.sys_openat_fileopen_calls);
    print_tick_delta_ms("sys_openat_fileopen_ms",
                        after.sys_openat_fileopen_ticks,
                        before.sys_openat_fileopen_ticks,
                        after.timebase_freq);
    print_delta("sys_openat_fdalloc_calls",
                after.sys_openat_fdalloc_calls,
                before.sys_openat_fdalloc_calls);
    print_tick_delta_ms("sys_openat_fdalloc_ms",
                        after.sys_openat_fdalloc_ticks,
                        before.sys_openat_fdalloc_ticks,
                        after.timebase_freq);
    print_delta("sys_poll_calls", after.sys_poll_calls,
                before.sys_poll_calls);
    print_tick_delta_ms("sys_poll_ms", after.sys_poll_ticks,
                        before.sys_poll_ticks, after.timebase_freq);
    print_delta("sys_ppoll_calls", after.sys_ppoll_calls,
                before.sys_ppoll_calls);
    print_tick_delta_ms("sys_ppoll_ms", after.sys_ppoll_ticks,
                        before.sys_ppoll_ticks, after.timebase_freq);
    print_delta("sys_poll_blocking_calls",
                after.sys_poll_blocking_calls,
                before.sys_poll_blocking_calls);
    print_tick_delta_ms("sys_poll_blocking_ms",
                        after.sys_poll_blocking_ticks,
                        before.sys_poll_blocking_ticks,
                        after.timebase_freq);
    print_delta("sys_poll_wait_unix_calls",
                after.sys_poll_wait_unix_calls,
                before.sys_poll_wait_unix_calls);
    print_tick_delta_ms("sys_poll_wait_unix_ms",
                        after.sys_poll_wait_unix_ticks,
                        before.sys_poll_wait_unix_ticks,
                        after.timebase_freq);
    print_delta("sys_poll_wait_eventfd_calls",
                after.sys_poll_wait_eventfd_calls,
                before.sys_poll_wait_eventfd_calls);
    print_tick_delta_ms("sys_poll_wait_eventfd_ms",
                        after.sys_poll_wait_eventfd_ticks,
                        before.sys_poll_wait_eventfd_ticks,
                        after.timebase_freq);
    print_delta("sys_poll_wait_pipe_calls",
                after.sys_poll_wait_pipe_calls,
                before.sys_poll_wait_pipe_calls);
    print_tick_delta_ms("sys_poll_wait_pipe_ms",
                        after.sys_poll_wait_pipe_ticks,
                        before.sys_poll_wait_pipe_ticks,
                        after.timebase_freq);
    print_delta("sys_poll_wait_other_calls",
                after.sys_poll_wait_other_calls,
                before.sys_poll_wait_other_calls);
    print_tick_delta_ms("sys_poll_wait_other_ms",
                        after.sys_poll_wait_other_ticks,
                        before.sys_poll_wait_other_ticks,
                        after.timebase_freq);
    print_delta("sys_poll_wait_notify_calls",
                after.sys_poll_wait_notify_calls,
                before.sys_poll_wait_notify_calls);
    print_tick_delta_ms("sys_poll_wait_notify_ms",
                        after.sys_poll_wait_notify_ticks,
                        before.sys_poll_wait_notify_ticks,
                        after.timebase_freq);
    print_delta("sys_poll_wait_rescan_calls",
                after.sys_poll_wait_rescan_calls,
                before.sys_poll_wait_rescan_calls);
    print_tick_delta_ms("sys_poll_wait_rescan_ms",
                        after.sys_poll_wait_rescan_ticks,
                        before.sys_poll_wait_rescan_ticks,
                        after.timebase_freq);
    print_delta("sys_poll_wait_ready_calls",
                after.sys_poll_wait_ready_calls,
                before.sys_poll_wait_ready_calls);
    print_tick_delta_ms("sys_poll_wait_ready_ms",
                        after.sys_poll_wait_ready_ticks,
                        before.sys_poll_wait_ready_ticks,
                        after.timebase_freq);
    print_delta("sys_poll_wait_timeout_calls",
                after.sys_poll_wait_timeout_calls,
                before.sys_poll_wait_timeout_calls);
    print_tick_delta_ms("sys_poll_wait_timeout_ms",
                        after.sys_poll_wait_timeout_ticks,
                        before.sys_poll_wait_timeout_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_poll_total_calls",
                after.konsole_prepty_poll_total_calls,
                before.konsole_prepty_poll_total_calls);
    print_tick_delta_ms("konsole_prepty_poll_total_ms",
                        after.konsole_prepty_poll_total_ticks,
                        before.konsole_prepty_poll_total_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_poll_wayland_calls",
                after.konsole_prepty_poll_wayland_calls,
                before.konsole_prepty_poll_wayland_calls);
    print_tick_delta_ms("konsole_prepty_poll_wayland_ms",
                        after.konsole_prepty_poll_wayland_ticks,
                        before.konsole_prepty_poll_wayland_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_poll_qdbus_calls",
                after.konsole_prepty_poll_qdbus_calls,
                before.konsole_prepty_poll_qdbus_calls);
    print_tick_delta_ms("konsole_prepty_poll_qdbus_ms",
                        after.konsole_prepty_poll_qdbus_ticks,
                        before.konsole_prepty_poll_qdbus_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_poll_unix_other_calls",
                after.konsole_prepty_poll_unix_other_calls,
                before.konsole_prepty_poll_unix_other_calls);
    print_tick_delta_ms("konsole_prepty_poll_unix_other_ms",
                        after.konsole_prepty_poll_unix_other_ticks,
                        before.konsole_prepty_poll_unix_other_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_poll_eventfd_calls",
                after.konsole_prepty_poll_eventfd_calls,
                before.konsole_prepty_poll_eventfd_calls);
    print_tick_delta_ms("konsole_prepty_poll_eventfd_ms",
                        after.konsole_prepty_poll_eventfd_ticks,
                        before.konsole_prepty_poll_eventfd_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_poll_pipe_calls",
                after.konsole_prepty_poll_pipe_calls,
                before.konsole_prepty_poll_pipe_calls);
    print_tick_delta_ms("konsole_prepty_poll_pipe_ms",
                        after.konsole_prepty_poll_pipe_ticks,
                        before.konsole_prepty_poll_pipe_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_poll_other_calls",
                after.konsole_prepty_poll_other_calls,
                before.konsole_prepty_poll_other_calls);
    print_tick_delta_ms("konsole_prepty_poll_other_ms",
                        after.konsole_prepty_poll_other_ticks,
                        before.konsole_prepty_poll_other_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_poll_ready_calls",
                after.konsole_prepty_poll_ready_calls,
                before.konsole_prepty_poll_ready_calls);
    print_tick_delta_ms("konsole_prepty_poll_ready_ms",
                        after.konsole_prepty_poll_ready_ticks,
                        before.konsole_prepty_poll_ready_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_poll_timeout_calls",
                after.konsole_prepty_poll_timeout_calls,
                before.konsole_prepty_poll_timeout_calls);
    print_tick_delta_ms("konsole_prepty_poll_timeout_ms",
                        after.konsole_prepty_poll_timeout_ticks,
                        before.konsole_prepty_poll_timeout_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_futex_wait_calls",
                after.konsole_prepty_futex_wait_calls,
                before.konsole_prepty_futex_wait_calls);
    print_tick_delta_ms("konsole_prepty_futex_wait_ms",
                        after.konsole_prepty_futex_wait_ticks,
                        before.konsole_prepty_futex_wait_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_futex_woken_calls",
                after.konsole_prepty_futex_woken_calls,
                before.konsole_prepty_futex_woken_calls);
    print_tick_delta_ms("konsole_prepty_futex_woken_ms",
                        after.konsole_prepty_futex_woken_ticks,
                        before.konsole_prepty_futex_woken_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_futex_timeout_calls",
                after.konsole_prepty_futex_timeout_calls,
                before.konsole_prepty_futex_timeout_calls);
    print_tick_delta_ms("konsole_prepty_futex_timeout_ms",
                        after.konsole_prepty_futex_timeout_ticks,
                        before.konsole_prepty_futex_timeout_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_futex_signal_calls",
                after.konsole_prepty_futex_signal_calls,
                before.konsole_prepty_futex_signal_calls);
    print_tick_delta_ms("konsole_prepty_futex_signal_ms",
                        after.konsole_prepty_futex_signal_ticks,
                        before.konsole_prepty_futex_signal_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_futex_other_calls",
                after.konsole_prepty_futex_other_calls,
                before.konsole_prepty_futex_other_calls);
    print_tick_delta_ms("konsole_prepty_futex_other_ms",
                        after.konsole_prepty_futex_other_ticks,
                        before.konsole_prepty_futex_other_ticks,
                        after.timebase_freq);
    print_delta("konsole_prepty_pty_seen", after.konsole_prepty_pty_seen,
                before.konsole_prepty_pty_seen);
#define PRINT_PREPTY_POLL_PAIR(field)                                      \
    do {                                                                   \
        print_delta("konsole_prepty_poll_" #field "_calls",                \
                    after.konsole_prepty_poll_##field##_calls,             \
                    before.konsole_prepty_poll_##field##_calls);           \
        print_tick_delta_ms("konsole_prepty_poll_" #field "_ms",           \
                            after.konsole_prepty_poll_##field##_ticks,     \
                            before.konsole_prepty_poll_##field##_ticks,    \
                            after.timebase_freq);                          \
    } while (0)
    PRINT_PREPTY_POLL_PAIR(wayland_pipe);
    PRINT_PREPTY_POLL_PAIR(wayland_eventfd);
    PRINT_PREPTY_POLL_PAIR(qdbus_pipe);
    PRINT_PREPTY_POLL_PAIR(qdbus_eventfd);
    PRINT_PREPTY_POLL_PAIR(unix_other_pipe);
    PRINT_PREPTY_POLL_PAIR(unix_other_eventfd);
    PRINT_PREPTY_POLL_PAIR(eventfd_pipe);
    PRINT_PREPTY_POLL_PAIR(wayland_only);
    PRINT_PREPTY_POLL_PAIR(qdbus_only);
    PRINT_PREPTY_POLL_PAIR(unix_other_only);
    PRINT_PREPTY_POLL_PAIR(eventfd_only);
    PRINT_PREPTY_POLL_PAIR(pipe_only);
    PRINT_PREPTY_POLL_PAIR(kqueue_wake);
    PRINT_PREPTY_POLL_PAIR(timed_rescan);
    PRINT_PREPTY_POLL_PAIR(rescan_ready);
    PRINT_PREPTY_POLL_PAIR(event_ready);
    PRINT_PREPTY_POLL_PAIR(ready_wayland);
    PRINT_PREPTY_POLL_PAIR(ready_qdbus);
    PRINT_PREPTY_POLL_PAIR(ready_unix_other);
    PRINT_PREPTY_POLL_PAIR(ready_eventfd);
    PRINT_PREPTY_POLL_PAIR(ready_pipe);
    PRINT_PREPTY_POLL_PAIR(rescan_ready_wayland);
    PRINT_PREPTY_POLL_PAIR(rescan_ready_qdbus);
    PRINT_PREPTY_POLL_PAIR(rescan_ready_unix_other);
    PRINT_PREPTY_POLL_PAIR(rescan_ready_eventfd);
    PRINT_PREPTY_POLL_PAIR(rescan_ready_pipe);
#undef PRINT_PREPTY_POLL_PAIR
    print_delta("sys_ioctl_calls", after.sys_ioctl_calls,
                before.sys_ioctl_calls);
    print_tick_delta_ms("sys_ioctl_ms", after.sys_ioctl_ticks,
                        before.sys_ioctl_ticks, after.timebase_freq);
    print_delta("sys_ioctl_tty_tcgets_calls",
                after.sys_ioctl_tty_tcgets_calls,
                before.sys_ioctl_tty_tcgets_calls);
    print_tick_delta_ms("sys_ioctl_tty_tcgets_ms",
                        after.sys_ioctl_tty_tcgets_ticks,
                        before.sys_ioctl_tty_tcgets_ticks,
                        after.timebase_freq);
    print_delta("sys_ioctl_tty_tcsets_calls",
                after.sys_ioctl_tty_tcsets_calls,
                before.sys_ioctl_tty_tcsets_calls);
    print_tick_delta_ms("sys_ioctl_tty_tcsets_ms",
                        after.sys_ioctl_tty_tcsets_ticks,
                        before.sys_ioctl_tty_tcsets_ticks,
                        after.timebase_freq);
    print_delta("sys_ioctl_tty_winsz_calls",
                after.sys_ioctl_tty_winsz_calls,
                before.sys_ioctl_tty_winsz_calls);
    print_tick_delta_ms("sys_ioctl_tty_winsz_ms",
                        after.sys_ioctl_tty_winsz_ticks,
                        before.sys_ioctl_tty_winsz_ticks,
                        after.timebase_freq);
    print_delta("sys_ioctl_tty_pgrp_calls",
                after.sys_ioctl_tty_pgrp_calls,
                before.sys_ioctl_tty_pgrp_calls);
    print_tick_delta_ms("sys_ioctl_tty_pgrp_ms",
                        after.sys_ioctl_tty_pgrp_ticks,
                        before.sys_ioctl_tty_pgrp_ticks,
                        after.timebase_freq);
    print_delta("sys_ioctl_tty_ptmx_calls",
                after.sys_ioctl_tty_ptmx_calls,
                before.sys_ioctl_tty_ptmx_calls);
    print_tick_delta_ms("sys_ioctl_tty_ptmx_ms",
                        after.sys_ioctl_tty_ptmx_ticks,
                        before.sys_ioctl_tty_ptmx_ticks,
                        after.timebase_freq);
    print_delta("sys_ioctl_tty_ctty_calls",
                after.sys_ioctl_tty_ctty_calls,
                before.sys_ioctl_tty_ctty_calls);
    print_tick_delta_ms("sys_ioctl_tty_ctty_ms",
                        after.sys_ioctl_tty_ctty_ticks,
                        before.sys_ioctl_tty_ctty_ticks,
                        after.timebase_freq);
    print_delta("sys_futex_calls", after.sys_futex_calls,
                before.sys_futex_calls);
    print_tick_delta_ms("sys_futex_ms", after.sys_futex_ticks,
                        before.sys_futex_ticks, after.timebase_freq);
    print_delta("sys_futex_wait_calls", after.sys_futex_wait_calls,
                before.sys_futex_wait_calls);
    print_tick_delta_ms("sys_futex_wait_ms", after.sys_futex_wait_ticks,
                        before.sys_futex_wait_ticks, after.timebase_freq);
    print_delta("sys_futex_wake_calls", after.sys_futex_wake_calls,
                before.sys_futex_wake_calls);
    print_tick_delta_ms("sys_futex_wake_ms", after.sys_futex_wake_ticks,
                        before.sys_futex_wake_ticks, after.timebase_freq);
    print_delta("sys_fstatat_calls", after.sys_fstatat_calls,
                before.sys_fstatat_calls);
    print_tick_delta_ms("sys_fstatat_ms", after.sys_fstatat_ticks,
                        before.sys_fstatat_ticks, after.timebase_freq);
    print_delta("sys_faccessat_calls", after.sys_faccessat_calls,
                before.sys_faccessat_calls);
    print_tick_delta_ms("sys_faccessat_ms", after.sys_faccessat_ticks,
                        before.sys_faccessat_ticks, after.timebase_freq);
    print_delta("sys_read_calls", after.sys_read_calls,
                before.sys_read_calls);
    print_tick_delta_ms("sys_read_ms", after.sys_read_ticks,
                        before.sys_read_ticks, after.timebase_freq);
    print_delta("sys_readv_calls", after.sys_readv_calls,
                before.sys_readv_calls);
    print_tick_delta_ms("sys_readv_ms", after.sys_readv_ticks,
                        before.sys_readv_ticks, after.timebase_freq);
    print_delta("sys_getdents_calls", after.sys_getdents_calls,
                before.sys_getdents_calls);
    print_tick_delta_ms("sys_getdents_ms", after.sys_getdents_ticks,
                        before.sys_getdents_ticks, after.timebase_freq);
    print_delta("sys_readlinkat_calls", after.sys_readlinkat_calls,
                before.sys_readlinkat_calls);
    print_tick_delta_ms("sys_readlinkat_ms", after.sys_readlinkat_ticks,
                        before.sys_readlinkat_ticks, after.timebase_freq);
    print_delta("sys_mmap_calls", after.sys_mmap_calls,
                before.sys_mmap_calls);
    print_tick_delta_ms("sys_mmap_ms", after.sys_mmap_ticks,
                        before.sys_mmap_ticks, after.timebase_freq);
    print_delta("sys_munmap_calls", after.sys_munmap_calls,
                before.sys_munmap_calls);
    print_tick_delta_ms("sys_munmap_ms", after.sys_munmap_ticks,
                        before.sys_munmap_ticks, after.timebase_freq);
    print_delta("vm_munmap_pages_freed", after.vm_munmap_pages_freed,
                before.vm_munmap_pages_freed);
    print_tick_delta_ms("vm_munmap_pte_walk_ms",
                        after.vm_munmap_pte_walk_ticks,
                        before.vm_munmap_pte_walk_ticks,
                        after.timebase_freq);
    print_tick_delta_ms("vm_munmap_page_release_ms",
                        after.vm_munmap_page_release_ticks,
                        before.vm_munmap_page_release_ticks,
                        after.timebase_freq);
    print_delta("vm_munmap_anon_pages", after.vm_munmap_anon_pages,
                before.vm_munmap_anon_pages);
    print_delta("sys_mprotect_calls", after.sys_mprotect_calls,
                before.sys_mprotect_calls);
    print_tick_delta_ms("sys_mprotect_ms", after.sys_mprotect_ticks,
                        before.sys_mprotect_ticks, after.timebase_freq);
    print_delta("sys_brk_calls", after.sys_brk_calls,
                before.sys_brk_calls);
    print_tick_delta_ms("sys_brk_ms", after.sys_brk_ticks,
                        before.sys_brk_ticks, after.timebase_freq);
    print_delta("sys_clock_gettime_calls", after.sys_clock_gettime_calls,
                before.sys_clock_gettime_calls);
    print_tick_delta_ms("sys_clock_gettime_ms",
                        after.sys_clock_gettime_ticks,
                        before.sys_clock_gettime_ticks,
                        after.timebase_freq);
    print_delta("sys_clock_gettime_monotonic_calls",
                after.sys_clock_gettime_monotonic_calls,
                before.sys_clock_gettime_monotonic_calls);
    print_tick_delta_ms("sys_clock_gettime_monotonic_ms",
                        after.sys_clock_gettime_monotonic_ticks,
                        before.sys_clock_gettime_monotonic_ticks,
                        after.timebase_freq);
    print_delta("sys_clock_gettime_monotonic_coarse_calls",
                after.sys_clock_gettime_monotonic_coarse_calls,
                before.sys_clock_gettime_monotonic_coarse_calls);
    print_tick_delta_ms("sys_clock_gettime_monotonic_coarse_ms",
                        after.sys_clock_gettime_monotonic_coarse_ticks,
                        before.sys_clock_gettime_monotonic_coarse_ticks,
                        after.timebase_freq);
    print_delta("sys_clock_gettime_realtime_calls",
                after.sys_clock_gettime_realtime_calls,
                before.sys_clock_gettime_realtime_calls);
    print_tick_delta_ms("sys_clock_gettime_realtime_ms",
                        after.sys_clock_gettime_realtime_ticks,
                        before.sys_clock_gettime_realtime_ticks,
                        after.timebase_freq);
    print_delta("sys_clock_gettime_process_calls",
                after.sys_clock_gettime_process_calls,
                before.sys_clock_gettime_process_calls);
    print_tick_delta_ms("sys_clock_gettime_process_ms",
                        after.sys_clock_gettime_process_ticks,
                        before.sys_clock_gettime_process_ticks,
                        after.timebase_freq);
    print_delta("sys_clock_gettime_thread_calls",
                after.sys_clock_gettime_thread_calls,
                before.sys_clock_gettime_thread_calls);
    print_tick_delta_ms("sys_clock_gettime_thread_ms",
                        after.sys_clock_gettime_thread_ticks,
                        before.sys_clock_gettime_thread_ticks,
                        after.timebase_freq);
    print_delta("sys_clock_gettime_other_calls",
                after.sys_clock_gettime_other_calls,
                before.sys_clock_gettime_other_calls);
    print_tick_delta_ms("sys_clock_gettime_other_ms",
                        after.sys_clock_gettime_other_ticks,
                        before.sys_clock_gettime_other_ticks,
                        after.timebase_freq);
    print_delta("sys_gettimeofday_calls", after.sys_gettimeofday_calls,
                before.sys_gettimeofday_calls);
    print_tick_delta_ms("sys_gettimeofday_ms",
                        after.sys_gettimeofday_ticks,
                        before.sys_gettimeofday_ticks,
                        after.timebase_freq);
    print_delta("sys_getrandom_calls", after.sys_getrandom_calls,
                before.sys_getrandom_calls);
    print_tick_delta_ms("sys_getrandom_ms", after.sys_getrandom_ticks,
                        before.sys_getrandom_ticks, after.timebase_freq);
    print_delta("exec_calls", after.exec_calls,
                before.exec_calls);
    print_tick_delta_ms("exec_ms", after.exec_ticks,
                        before.exec_ticks, after.timebase_freq);

    exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}

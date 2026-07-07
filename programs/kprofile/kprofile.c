#include "kernel/inc/types.h"
#include "kernel/inc/kstats.h"
#include "kernel/inc/signo.h"
#include "kernel/inc/timer/timer.h"
#include "user/user.h"

#define KPROFILE_UINT64_MAX ((uint64)~0ULL)
#define KPROFILE_USERPC_PERIOD_MAX 2147483647ULL
#define KPROFILE_O_RDONLY 0

#define KPROFILE_USERPC_MAP_PID_CAP 64
#define KPROFILE_USERPC_MAP_CAP 512
#define KPROFILE_USERPC_MAP_PATH_LEN 160
#define KPROFILE_USERPC_MAP_LINE_MAX 512
#define KPROFILE_USERPC_MAP_READ_BUF 1024
#define KPROFILE_USERPC_MAP_SNAPSHOTS_PER_PID 4
#define KPROFILE_USERPC_MODULE_PRINT_CAP 64

struct userpc_maps;
static void collect_userpc_maps_from_snapshot(
    struct userpc_maps *maps, struct kprofile_userpc_snapshot *snap);

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

static int read_pgroup(int pgid, struct kprofile_pgroup *kp)
{
    return kprofile_pgroup(pgid, kp, sizeof(*kp));
}

static int read_prepty_ring(struct konsole_prepty_wake_snapshot *snap)
{
    return kprofile_prepty_ring(snap, sizeof(*snap));
}

static int userpc_ctl(int enable, int pgid, uint64 period_ticks)
{
    struct kprofile_userpc_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = KPROFILE_USERPC_ABI_VERSION;
    cfg.flags = enable ? KPROFILE_USERPC_CTL_F_ENABLE : 0;
    cfg.target_pgid = enable ? pgid : 0;
    cfg.period_ticks = period_ticks != 0 ? period_ticks : 1;
    return kprofile_userpc_ctl(&cfg, sizeof(cfg));
}

static int read_userpc_ring(struct kprofile_userpc_snapshot *snap)
{
    return kprofile_userpc_snapshot(snap, sizeof(*snap));
}

static int read_vfs_enoent_snapshot(struct kprofile_vfs_enoent_snapshot *snap)
{
    return kprofile_vfs_enoent_snapshot(snap, sizeof(*snap));
}

static void capture_userpc_snapshot(struct kprofile_userpc_snapshot **snap,
                                    int *have)
{
    if (snap == 0 || have == 0)
        return;
    if (*snap == 0)
        *snap = malloc(sizeof(**snap));
    if (*snap != 0 && read_userpc_ring(*snap) == 0)
        *have = 1;
}

static void capture_userpc_state(struct kprofile_userpc_snapshot **snap,
                                 int *have, struct userpc_maps *maps)
{
    capture_userpc_snapshot(snap, have);
    if (maps != 0 && have != 0 && *have && snap != 0 && *snap != 0)
        collect_userpc_maps_from_snapshot(maps, *snap);
}

static void disarm_userpc_if_armed(int *armed, uint64 period_ticks)
{
    if (armed != 0 && *armed) {
        userpc_ctl(0, 0, period_ticks);
        *armed = 0;
    }
}

static void profile_sleep(void)
{
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 20 * 1000 * 1000;
    nanosleep(&ts, 0);
}

static void print_tick_value_ms(const char *name, uint64 ticks,
                                uint64 timebase_freq)
{
    uint64 ms = timebase_freq ? (ticks * 1000ULL) / timebase_freq : 0;
    printf("%-28s %lu\n", name, (unsigned long)ms);
}

static void print_sched_tick_value_ms(const char *name, uint64 ticks)
{
    uint64 ms = (ticks * 1000ULL) / HZ;
    printf("%-28s %lu\n", name, (unsigned long)ms);
}

static void print_cpu_metrics(struct kstats *after, struct kstats *before)
{
    uint64 busy_ticks = 0;
    uint64 total_ticks = 0;
    uint64 final_running = 0;
    uint64 final_idle = 0;
    uint64 final_util_1s = 0;
    int ncpus = after->ncpus;

    if (ncpus > before->ncpus)
        ncpus = before->ncpus;
    if (ncpus > KSTATS_MAX_CPUS)
        ncpus = KSTATS_MAX_CPUS;

    for (int i = 0; i < ncpus; i++) {
        busy_ticks += after->cpu[i].busy_ticks - before->cpu[i].busy_ticks;
        total_ticks += after->cpu[i].total_ticks - before->cpu[i].total_ticks;
        final_running += after->cpu[i].nr_running;
        final_idle += after->cpu[i].idle ? 1 : 0;
        final_util_1s += after->cpu[i].util_1s;
    }

    print_sched_tick_value_ms("cpu_busy_ms", busy_ticks);
    print_sched_tick_value_ms("cpu_total_ms", total_ticks);
    print_delta("cpu_final_nr_running", final_running, 0);
    print_delta("cpu_final_idle", final_idle, 0);
    print_delta("cpu_final_util_1s_fp", final_util_1s, 0);
}

static void print_pgroup_metrics(struct kprofile_pgroup *after,
                                 struct kprofile_pgroup *before)
{
    printf("%-28s %s\n", "pgroup_scope", "process-group-only");
    printf("%-28s %s\n", "pgroup_descendant_tracking", "none");
    printf("%-28s %ld\n", "pgroup_pgid", (long)after->pgid);
    print_delta("pgroup_processes_final", after->processes, 0);
    print_delta("pgroup_threads_final", after->threads, 0);
    print_delta("pgroup_live_threads_final", after->live_threads, 0);
    print_delta("pgroup_running_final", after->state_running, 0);
    print_delta("pgroup_runnable_final", after->state_runnable, 0);
    print_delta("pgroup_sleeping_final", after->state_sleeping, 0);
    print_delta("pgroup_unintr_final", after->state_uninterruptible, 0);
    print_delta("pgroup_wakening_final", after->state_wakening, 0);
    print_delta("pgroup_stopped_final", after->state_stopped, 0);
    print_delta("pgroup_zombie_final", after->state_zombie, 0);
    print_delta("pgroup_on_cpu_final", after->on_cpu, 0);
    print_delta("pgroup_on_rq_final", after->on_rq, 0);
    print_tick_delta_ms("pgroup_cpu_runtime_ms",
                        after->cpu_runtime_ticks,
                        before->cpu_runtime_ticks,
                        after->timebase_freq);
    print_tick_value_ms("pgroup_max_thread_cpu_ms",
                        after->max_thread_runtime_ticks,
                        after->timebase_freq);
    print_delta("pgroup_peak_vm_kb_final", after->peak_vm_bytes / 1024, 0);
    print_delta("pgroup_peak_vm_kb_delta",
                after->peak_vm_bytes / 1024,
                before->peak_vm_bytes / 1024);
    print_delta("pgroup_rss_pages_final", after->rss_pages, 0);
    print_delta("pgroup_fs_opens", after->fs_opens, before->fs_opens);
    print_delta("pgroup_fs_closes", after->fs_closes, before->fs_closes);
    print_delta("pgroup_fs_bytes_read",
                after->fs_bytes_read, before->fs_bytes_read);
    print_delta("pgroup_fs_bytes_written",
                after->fs_bytes_written, before->fs_bytes_written);
    print_delta("pgroup_bio_reads", after->bio_reads, before->bio_reads);
    print_delta("pgroup_bio_writes", after->bio_writes, before->bio_writes);
    print_delta("pgroup_net_sockets",
                after->net_sockets, before->net_sockets);
    print_delta("pgroup_net_connects",
                after->net_connects, before->net_connects);
    print_delta("pgroup_net_accepts", after->net_accepts, before->net_accepts);
    print_delta("pgroup_net_bytes_sent",
                after->net_bytes_sent, before->net_bytes_sent);
    print_delta("pgroup_net_bytes_recv",
                after->net_bytes_recv, before->net_bytes_recv);
    print_delta("pgroup_mm_mmap_count",
                after->mm_mmap_count, before->mm_mmap_count);
    print_delta("pgroup_mm_munmap_count",
                after->mm_munmap_count, before->mm_munmap_count);
    printf("%-28s %ld\n", "pgroup_mm_brk_delta",
           (long)(after->mm_brk_delta - before->mm_brk_delta));
    print_delta("pgroup_sched_forks",
                after->sched_forks, before->sched_forks);
    print_delta("pgroup_sched_execs",
                after->sched_execs, before->sched_execs);
    print_delta("pgroup_sched_exits",
                after->sched_exits, before->sched_exits);
}

static const char *prepty_event_name(int event)
{
    switch (event) {
    case KONSOLE_PREPTY_WAKE_EVENT_NOTIFY:
        return "notify";
    case KONSOLE_PREPTY_WAKE_EVENT_PIPE_WRITE:
        return "pipe_write";
    case KONSOLE_PREPTY_WAKE_EVENT_POLL_WAIT:
        return "poll_wait";
    case KONSOLE_PREPTY_WAKE_EVENT_FD_LIFECYCLE:
        return "fd_lifecycle";
    case KONSOLE_PREPTY_WAKE_EVENT_EVENTFD_OP:
        return "eventfd_op";
    case KONSOLE_PREPTY_WAKE_EVENT_POLL_FD:
        return "poll_fd";
    default:
        return "unknown";
    }
}

static const char *prepty_role_name(int role)
{
    switch (role) {
    case KONSOLE_PREPTY_WAKE_ROLE_WAYLAND:
        return "wayland";
    case KONSOLE_PREPTY_WAKE_ROLE_QDBUS:
        return "qdbus";
    case KONSOLE_PREPTY_WAKE_ROLE_EVENTFD:
        return "eventfd";
    case KONSOLE_PREPTY_WAKE_ROLE_PIPE:
        return "pipe";
    case KONSOLE_PREPTY_WAKE_ROLE_KQUEUE:
        return "kqueue";
    case KONSOLE_PREPTY_WAKE_ROLE_UNIX_OTHER:
        return "unix_other";
    case KONSOLE_PREPTY_WAKE_ROLE_OTHER:
    default:
        return "other";
    }
}

static const char *prepty_eventfd_op_name(int op)
{
    switch (op) {
    case KONSOLE_PREPTY_EVENTFD_OP_SIGNAL:
        return "signal";
    case KONSOLE_PREPTY_EVENTFD_OP_WRITE:
        return "write";
    case KONSOLE_PREPTY_EVENTFD_OP_READ:
        return "read";
    case KONSOLE_PREPTY_EVENTFD_OP_NONE:
    default:
        return "none";
    }
}

static const char *prepty_fd_lifecycle_name(int op)
{
    switch (op) {
    case KONSOLE_PREPTY_FD_LIFECYCLE_OPEN:
        return "open";
    case KONSOLE_PREPTY_FD_LIFECYCLE_CLOSE:
        return "close";
    case KONSOLE_PREPTY_FD_LIFECYCLE_DUP:
        return "dup";
    case KONSOLE_PREPTY_FD_LIFECYCLE_REPLACE:
        return "replace";
    case KONSOLE_PREPTY_FD_LIFECYCLE_EVENTFD_CREATE:
        return "eventfd_create";
    case KONSOLE_PREPTY_FD_LIFECYCLE_PIPE:
        return "pipe";
    case KONSOLE_PREPTY_FD_LIFECYCLE_NONE:
    default:
        return "none";
    }
}

static const char *prepty_poll_op_name(int op)
{
    switch (op) {
    case KONSOLE_PREPTY_POLL_OP_POLL:
        return "poll";
    case KONSOLE_PREPTY_POLL_OP_PPOLL:
        return "ppoll";
    case KONSOLE_PREPTY_POLL_OP_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *prepty_disarm_name(uint64 reason)
{
    switch (reason) {
    case 1:
        return "pty";
    case 2:
        return "limit";
    default:
        return "none";
    }
}

static const char *prepty_str(const char *s)
{
    return s != 0 && s[0] != '\0' ? s : "-";
}

static uint64 prepty_wait_ms(const struct konsole_prepty_wake_record *r)
{
    if (r->waiter_run_ms >= r->wait_start_ms)
        return r->waiter_run_ms - r->wait_start_ms;
    return 0;
}

enum prepty_thread_role {
    PREPTY_THREAD_WAYLAND = 0,
    PREPTY_THREAD_QDBUS,
    PREPTY_THREAD_MAIN,
    PREPTY_THREAD_OTHER,
    PREPTY_THREAD_COUNT,
};

static const char *prepty_thread_role_name(int role)
{
    switch (role) {
    case PREPTY_THREAD_WAYLAND:
        return "WaylandEventThr";
    case PREPTY_THREAD_QDBUS:
        return "QDBusConnection";
    case PREPTY_THREAD_MAIN:
        return "main";
    case PREPTY_THREAD_OTHER:
    default:
        return "other";
    }
}

static int prepty_thread_role(const char *comm)
{
    if (comm == 0)
        return PREPTY_THREAD_OTHER;
    if (strncmp(comm, "WaylandEventThr", 15) == 0)
        return PREPTY_THREAD_WAYLAND;
    if (strncmp(comm, "QDBusConnection", 15) == 0)
        return PREPTY_THREAD_QDBUS;
    if (strncmp(comm, "konsole", 7) == 0)
        return PREPTY_THREAD_MAIN;
    return PREPTY_THREAD_OTHER;
}

struct prepty_wait_bucket {
    uint64 calls;
    uint64 ms;
};

static void prepty_bucket_add(struct prepty_wait_bucket *bucket, uint64 ms)
{
    bucket->calls++;
    bucket->ms += ms;
}

#define PREPTY_FD_TARGET_CAP 96

struct prepty_fd_target_summary {
    int used;
    int fd;
    int tgid;
    uint64 file;
    int role;
    uint64 poll_records;
    uint64 ready_records;
    uint64 revents_or;
    uint64 last_seq;
    char comm[KONSOLE_PREPTY_WAKE_COMM_LEN];
    char kind[KONSOLE_PREPTY_WAKE_KIND_LEN];
    char target[KONSOLE_PREPTY_WAKE_TARGET_LEN];
    char path[KONSOLE_PREPTY_WAKE_PATH_LEN];
    char peer_path[KONSOLE_PREPTY_WAKE_PATH_LEN];
};

static int prepty_fd_target_match(const struct prepty_fd_target_summary *s,
                                  const struct konsole_prepty_wake_record *r)
{
    return s->used && s->fd == r->fd && s->tgid == r->tgid &&
           s->file == r->file && s->role == r->role &&
           strcmp(s->kind, r->kind) == 0 &&
           strcmp(s->target, r->target) == 0 &&
           strcmp(s->path, r->path) == 0 &&
           strcmp(s->peer_path, r->peer_path) == 0 &&
           strcmp(s->comm, r->comm) == 0;
}

static void prepty_fd_target_note(struct prepty_fd_target_summary *rows,
                                  const struct konsole_prepty_wake_record *r)
{
    int slot = -1;

    if (r->event != KONSOLE_PREPTY_WAKE_EVENT_POLL_FD)
        return;
    for (int i = 0; i < PREPTY_FD_TARGET_CAP; i++) {
        if (prepty_fd_target_match(&rows[i], r)) {
            slot = i;
            break;
        }
        if (slot < 0 && !rows[i].used)
            slot = i;
    }
    if (slot < 0)
        return;

    struct prepty_fd_target_summary *s = &rows[slot];
    if (!s->used) {
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->fd = r->fd;
        s->tgid = r->tgid;
        s->file = r->file;
        s->role = r->role;
        snprintf(s->comm, sizeof(s->comm), "%s", r->comm);
        snprintf(s->kind, sizeof(s->kind), "%s", r->kind);
        snprintf(s->target, sizeof(s->target), "%s", r->target);
        snprintf(s->path, sizeof(s->path), "%s", r->path);
        snprintf(s->peer_path, sizeof(s->peer_path), "%s", r->peer_path);
    }
    s->poll_records++;
    if (r->poll_revents != 0)
        s->ready_records++;
    s->revents_or |= (uint64)(uint32)r->poll_revents;
    s->last_seq = r->seq;
}

static void print_prepty_role_summary(
    struct konsole_prepty_wake_snapshot *snap, uint64 count)
{
    struct prepty_wait_bucket thread[PREPTY_THREAD_COUNT];
    struct prepty_wait_bucket wayland = {0};
    struct prepty_wait_bucket qdbus = {0};
    struct prepty_wait_bucket eventfd = {0};
    struct prepty_wait_bucket pipe = {0};
    struct prepty_wait_bucket unix_other = {0};
    struct prepty_wait_bucket other = {0};
    struct prepty_wait_bucket ready = {0};
    struct prepty_wait_bucket timeout_rescan = {0};
    struct prepty_wait_bucket timeout_notify = {0};
    struct prepty_wait_bucket event_ready = {0};
    struct prepty_wait_bucket timed_rescan = {0};
    struct prepty_wait_bucket main_ppoll = {0};
    struct prepty_fd_target_summary fd_rows[PREPTY_FD_TARGET_CAP];
    const char *dominant = "none";
    uint64 dominant_ms = 0;

    memset(thread, 0, sizeof(thread));
    memset(fd_rows, 0, sizeof(fd_rows));

    for (uint64 i = 0; i < count; i++) {
        const struct konsole_prepty_wake_record *r = &snap->records[i];

        prepty_fd_target_note(fd_rows, r);
        if (r->event != KONSOLE_PREPTY_WAKE_EVENT_POLL_WAIT)
            continue;

        uint64 ms = prepty_wait_ms(r);
        int role = prepty_thread_role(r->comm);

        prepty_bucket_add(&thread[role], ms);
        if (r->role_mask & KONSOLE_PREPTY_WAKE_ROLE_BIT(KONSOLE_PREPTY_WAKE_ROLE_WAYLAND))
            prepty_bucket_add(&wayland, ms);
        if (r->role_mask & KONSOLE_PREPTY_WAKE_ROLE_BIT(KONSOLE_PREPTY_WAKE_ROLE_QDBUS))
            prepty_bucket_add(&qdbus, ms);
        if (r->role_mask & KONSOLE_PREPTY_WAKE_ROLE_BIT(KONSOLE_PREPTY_WAKE_ROLE_EVENTFD))
            prepty_bucket_add(&eventfd, ms);
        if (r->role_mask & KONSOLE_PREPTY_WAKE_ROLE_BIT(KONSOLE_PREPTY_WAKE_ROLE_PIPE))
            prepty_bucket_add(&pipe, ms);
        if (r->role_mask & KONSOLE_PREPTY_WAKE_ROLE_BIT(KONSOLE_PREPTY_WAKE_ROLE_UNIX_OTHER))
            prepty_bucket_add(&unix_other, ms);
        if (r->role_mask & KONSOLE_PREPTY_WAKE_ROLE_BIT(KONSOLE_PREPTY_WAKE_ROLE_OTHER))
            prepty_bucket_add(&other, ms);
        if (r->ready > 0)
            prepty_bucket_add(&ready, ms);
        else if (r->has_unnotified_fds)
            prepty_bucket_add(&timeout_rescan, ms);
        else
            prepty_bucket_add(&timeout_notify, ms);
        if (r->ready > 0 && r->nevents > 0)
            prepty_bucket_add(&event_ready, ms);
        if (r->nevents == 0 && r->timeout_ms > 0)
            prepty_bucket_add(&timed_rescan, ms);
        if (role == PREPTY_THREAD_MAIN &&
            r->poll_op == KONSOLE_PREPTY_POLL_OP_PPOLL)
            prepty_bucket_add(&main_ppoll, ms);
    }

#define PREPTY_DOMINANT(bucket, name)                                      \
    do {                                                                   \
        if ((bucket).ms > dominant_ms) {                                    \
            dominant_ms = (bucket).ms;                                      \
            dominant = (name);                                             \
        }                                                                  \
    } while (0)
    PREPTY_DOMINANT(wayland, "wayland");
    PREPTY_DOMINANT(qdbus, "qdbus");
    PREPTY_DOMINANT(eventfd, "eventfd");
    PREPTY_DOMINANT(pipe, "pipe");
    PREPTY_DOMINANT(unix_other, "unix_other");
    PREPTY_DOMINANT(other, "other");
    PREPTY_DOMINANT(main_ppoll, "main_ppoll");
#undef PREPTY_DOMINANT

    printf("konsole_prepty_role_summary poll_wait_calls=%lu poll_wait_ms=%lu "
           "thread_wayland=%lu/%lu thread_qdbus=%lu/%lu "
           "thread_main=%lu/%lu thread_other=%lu/%lu "
           "wayland=%lu/%lu qdbus=%lu/%lu eventfd=%lu/%lu pipe=%lu/%lu "
           "unix_other=%lu/%lu other=%lu/%lu main_ppoll=%lu/%lu "
           "ready=%lu/%lu timeout_rescan=%lu/%lu timeout_notify=%lu/%lu "
           "event_ready=%lu/%lu timed_rescan=%lu/%lu dominant=%s "
           "dominant_ms=%lu\n",
           thread[0].calls + thread[1].calls + thread[2].calls +
               thread[3].calls,
           thread[0].ms + thread[1].ms + thread[2].ms + thread[3].ms,
           thread[PREPTY_THREAD_WAYLAND].calls,
           thread[PREPTY_THREAD_WAYLAND].ms,
           thread[PREPTY_THREAD_QDBUS].calls,
           thread[PREPTY_THREAD_QDBUS].ms,
           thread[PREPTY_THREAD_MAIN].calls,
           thread[PREPTY_THREAD_MAIN].ms,
           thread[PREPTY_THREAD_OTHER].calls,
           thread[PREPTY_THREAD_OTHER].ms,
           wayland.calls, wayland.ms, qdbus.calls, qdbus.ms,
           eventfd.calls, eventfd.ms, pipe.calls, pipe.ms,
           unix_other.calls, unix_other.ms, other.calls, other.ms,
           main_ppoll.calls, main_ppoll.ms, ready.calls, ready.ms,
           timeout_rescan.calls, timeout_rescan.ms,
           timeout_notify.calls, timeout_notify.ms,
           event_ready.calls, event_ready.ms, timed_rescan.calls,
           timed_rescan.ms, dominant, dominant_ms);

    int emitted[PREPTY_FD_TARGET_CAP];
    memset(emitted, 0, sizeof(emitted));
    for (int rank = 0; rank < 8; rank++) {
        int best = -1;
        for (int i = 0; i < PREPTY_FD_TARGET_CAP; i++) {
            if (!fd_rows[i].used || emitted[i])
                continue;
            if (best < 0 ||
                fd_rows[i].poll_records > fd_rows[best].poll_records ||
                (fd_rows[i].poll_records == fd_rows[best].poll_records &&
                 fd_rows[i].ready_records > fd_rows[best].ready_records))
                best = i;
        }
        if (best < 0)
            break;
        emitted[best] = 1;
        printf("konsole_prepty_fd_target_summary rank=%d tgid=%d fd=%d "
               "role=%s thread_role=%s comm=%s kind=%s target=%s "
               "path=%s peer_path=%s file=0x%lx poll_records=%lu "
               "ready_records=%lu revents_or=0x%lx last_seq=%lu\n",
               rank + 1, fd_rows[best].tgid, fd_rows[best].fd,
               prepty_role_name(fd_rows[best].role),
               prepty_thread_role_name(prepty_thread_role(fd_rows[best].comm)),
               prepty_str(fd_rows[best].comm), prepty_str(fd_rows[best].kind),
               prepty_str(fd_rows[best].target),
               prepty_str(fd_rows[best].path),
               prepty_str(fd_rows[best].peer_path),
               (unsigned long)fd_rows[best].file,
               (unsigned long)fd_rows[best].poll_records,
               (unsigned long)fd_rows[best].ready_records,
               (unsigned long)fd_rows[best].revents_or,
               (unsigned long)fd_rows[best].last_seq);
    }
}

static uint64 prepty_record_start_ms(
    const struct konsole_prepty_wake_record *rec)
{
    if (rec->wait_start_ms != 0)
        return rec->wait_start_ms;
    return rec->notify_ms;
}

static uint64 prepty_record_end_ms(
    const struct konsole_prepty_wake_record *rec)
{
    uint64 end = rec->notify_ms;

    if (rec->waiter_run_ms > end)
        end = rec->waiter_run_ms;
    return end;
}

struct prepty_fd_summary {
    int fd;
    int tgid;
    uint64 file;
    uint64 eventfd_id;
    uint64 eventfd_object;
    uint64 counter;
    uint64 last_poll_seq;
    uint64 last_write_seq;
    uint64 last_read_seq;
    uint64 last_close_seq;
    uint64 last_lifecycle_seq;
    uint64 poll_readable_mismatch_count;
    int last_events;
    int last_revents;
    int last_lifecycle_op;
};

static void prepty_summary_note(struct prepty_fd_summary *s,
                                const struct konsole_prepty_wake_record *r)
{
    if (s->tgid > 0 && r->tgid != s->tgid)
        return;
    if (r->fd != s->fd && r->newfd != s->fd && r->oldfd != s->fd)
        return;

    if (r->file != 0)
        s->file = r->file;
    if (r->eventfd_id != 0)
        s->eventfd_id = r->eventfd_id;
    if (r->eventfd_object != 0)
        s->eventfd_object = r->eventfd_object;
    if (r->eventfd_count != 0 || r->eventfd_readable ||
        r->eventfd_writable)
        s->counter = r->eventfd_count;

    if (r->event == KONSOLE_PREPTY_WAKE_EVENT_POLL_FD) {
        s->last_poll_seq = r->seq;
        s->last_events = r->poll_events;
        s->last_revents = r->poll_revents;
        if (r->poll_readable_mismatch)
            s->poll_readable_mismatch_count++;
    }
    if (r->event == KONSOLE_PREPTY_WAKE_EVENT_EVENTFD_OP ||
        r->eventfd_op != KONSOLE_PREPTY_EVENTFD_OP_NONE) {
        if (r->eventfd_op == KONSOLE_PREPTY_EVENTFD_OP_WRITE ||
            r->eventfd_op == KONSOLE_PREPTY_EVENTFD_OP_SIGNAL)
            s->last_write_seq = r->seq;
        if (r->eventfd_op == KONSOLE_PREPTY_EVENTFD_OP_READ)
            s->last_read_seq = r->seq;
    }
    if (r->event == KONSOLE_PREPTY_WAKE_EVENT_FD_LIFECYCLE) {
        s->last_lifecycle_seq = r->seq;
        s->last_lifecycle_op = r->fd_lifecycle_op;
        if (r->fd_lifecycle_op == KONSOLE_PREPTY_FD_LIFECYCLE_CLOSE)
            s->last_close_seq = r->seq;
    }
}

static int prepty_find_konsole_tgid(struct konsole_prepty_wake_snapshot *snap,
                                    uint64 count)
{
    for (uint64 i = 0; i < count; i++) {
        const struct konsole_prepty_wake_record *r = &snap->records[i];

        if (strncmp(r->comm, "konsole", 7) == 0)
            return r->tgid;
    }
    return 0;
}

static void print_prepty_fd_summaries(
    struct konsole_prepty_wake_snapshot *snap, uint64 count)
{
    int konsole_tgid = prepty_find_konsole_tgid(snap, count);
    struct prepty_fd_summary fd4 = {.fd = 4, .tgid = konsole_tgid};
    struct prepty_fd_summary fd15 = {.fd = 15, .tgid = konsole_tgid};

    for (uint64 i = 0; i < count; i++) {
        prepty_summary_note(&fd4, &snap->records[i]);
        prepty_summary_note(&fd15, &snap->records[i]);
    }

    printf("konsole_prepty_fd_summary scope_tgid=%d fd=4 "
           "file=0x%lx eventfd_id=%lu "
           "eventfd_object=0x%lx counter=%lu last_poll_seq=%lu "
           "last_events=0x%x last_revents=0x%x last_write_seq=%lu "
           "last_read_seq=%lu last_close_seq=%lu last_lifecycle_seq=%lu "
           "last_lifecycle_op=%s poll_readable_mismatch_count=%lu\n",
           konsole_tgid,
           (unsigned long)fd4.file, (unsigned long)fd4.eventfd_id,
           (unsigned long)fd4.eventfd_object, (unsigned long)fd4.counter,
           (unsigned long)fd4.last_poll_seq, fd4.last_events,
           fd4.last_revents, (unsigned long)fd4.last_write_seq,
           (unsigned long)fd4.last_read_seq,
           (unsigned long)fd4.last_close_seq,
           (unsigned long)fd4.last_lifecycle_seq,
           prepty_fd_lifecycle_name(fd4.last_lifecycle_op),
           (unsigned long)fd4.poll_readable_mismatch_count);
    printf("konsole_prepty_fd_summary scope_tgid=%d fd=15 "
           "file=0x%lx eventfd_id=%lu "
           "eventfd_object=0x%lx counter=%lu last_poll_seq=%lu "
           "last_events=0x%x last_revents=0x%x last_write_seq=%lu "
           "last_read_seq=%lu last_close_seq=%lu last_lifecycle_seq=%lu "
           "last_lifecycle_op=%s poll_readable_mismatch_count=%lu\n",
           konsole_tgid,
           (unsigned long)fd15.file, (unsigned long)fd15.eventfd_id,
           (unsigned long)fd15.eventfd_object, (unsigned long)fd15.counter,
           (unsigned long)fd15.last_poll_seq, fd15.last_events,
           fd15.last_revents, (unsigned long)fd15.last_write_seq,
           (unsigned long)fd15.last_read_seq,
           (unsigned long)fd15.last_close_seq,
           (unsigned long)fd15.last_lifecycle_seq,
           prepty_fd_lifecycle_name(fd15.last_lifecycle_op),
           (unsigned long)fd15.poll_readable_mismatch_count);
}

static void print_prepty_ring(struct konsole_prepty_wake_snapshot *snap)
{
    uint64 count;
    uint64 total_seen;
    uint64 stored;
    uint64 overwritten;
    uint64 first_seq;
    uint64 last_seq;
    uint64 first_ms;
    uint64 last_ms;
    uint64 span_ms;

    if (snap == 0)
        return;

    count = snap->count;
    if (count > snap->capacity)
        count = snap->capacity;
    if (count > KONSOLE_PREPTY_WAKE_RING_CAP)
        count = KONSOLE_PREPTY_WAKE_RING_CAP;
    stored = snap->stored != 0 || snap->abi_version >= 2 ?
        snap->stored : count;
    if (stored > count)
        stored = count;
    total_seen = snap->total_seen != 0 || snap->abi_version >= 2 ?
        snap->total_seen : (snap->next_seq > 0 ? snap->next_seq - 1 : count);
    overwritten = snap->overwritten;
    first_seq = snap->first_seq;
    last_seq = snap->last_seq;
    first_ms = snap->first_ms;
    last_ms = snap->last_ms;
    span_ms = snap->span_ms;
    for (uint64 i = 0; i < count; i++) {
        struct konsole_prepty_wake_record *r = &snap->records[i];
        uint64 rec_start = prepty_record_start_ms(r);
        uint64 rec_end = prepty_record_end_ms(r);

        if (i == 0 && first_seq == 0)
            first_seq = r->seq;
        if (r->seq != 0)
            last_seq = r->seq;
        if (rec_start != 0 && (first_ms == 0 || rec_start < first_ms))
            first_ms = rec_start;
        if (rec_end > last_ms)
            last_ms = rec_end;
    }
    if (span_ms == 0 && last_ms >= first_ms)
        span_ms = last_ms - first_ms;

    printf("%-28s %lu\n", "konsole_prepty_ring_abi",
           (unsigned long)snap->abi_version);
    printf("%-28s %lu\n", "konsole_prepty_ring_capacity",
           (unsigned long)snap->capacity);
    printf("%-28s %lu\n", "konsole_prepty_ring_limit",
           (unsigned long)snap->configured_limit);
    printf("%-28s %lu\n", "konsole_prepty_ring_total_seen",
           (unsigned long)total_seen);
    printf("%-28s %lu\n", "konsole_prepty_ring_stored",
           (unsigned long)stored);
    printf("%-28s %lu\n", "konsole_prepty_ring_count",
           (unsigned long)count);
    printf("%-28s %lu\n", "konsole_prepty_ring_overwritten",
           (unsigned long)overwritten);
    printf("%-28s %lu\n", "konsole_prepty_ring_dropped",
           (unsigned long)snap->dropped);
    printf("%-28s %lu\n", "konsole_prepty_ring_armed",
           (unsigned long)snap->armed);
    printf("%-28s %s\n", "konsole_prepty_ring_disarm",
           prepty_disarm_name(snap->disarm_reason));
    printf("%-28s %lu\n", "konsole_prepty_ring_first_seq",
           (unsigned long)first_seq);
    printf("%-28s %lu\n", "konsole_prepty_ring_last_seq",
           (unsigned long)last_seq);
    printf("%-28s %lu\n", "konsole_prepty_ring_first_ms",
           (unsigned long)first_ms);
    printf("%-28s %lu\n", "konsole_prepty_ring_last_ms",
           (unsigned long)last_ms);
    printf("%-28s %lu\n", "konsole_prepty_ring_span_ms",
           (unsigned long)span_ms);
    print_prepty_role_summary(snap, count);

    for (uint64 i = 0; i < count; i++) {
        struct konsole_prepty_wake_record *r = &snap->records[i];
        uint64 wait_ms = 0;

        if (r->waiter_run_ms >= r->wait_start_ms)
            wait_ms = r->waiter_run_ms - r->wait_start_ms;
        printf("konsole_prepty_ring seq=%lu event=%s role=%s "
               "role_mask=0x%lx notify_ms=%lu wait_start_ms=%lu "
               "waiter_run_ms=%lu wait_ms=%lu pid=%ld tgid=%ld "
               "comm=%s kind=%s target=%s path=%s peer_path=%s "
               "filter=%ld fd=%ld nfds=%ld ready=%ld nevents=%ld "
               "timeout_ms=%ld has_unnotified_fds=%ld matched=%ld "
               "enqueued_new=%ld already_queued=%ld propagated=%ld "
               "data=%ld bytes=%ld nread=%lu nwrite=%lu "
               "readable_before=%lu readable_after=%lu file=0x%lx "
               "eventfd_id=%lu eventfd_object=0x%lx eventfd_count=%lu "
               "eventfd_op=%s eventfd_counter_before=%lu "
               "eventfd_counter_after=%lu eventfd_value=%lu "
               "eventfd_read_value=%lu eventfd_ret=%ld "
               "eventfd_caller=0x%lx eventfd_caller_sym=%s "
               "eventfd_caller_off=%ld eventfd_caller_file=%s "
               "eventfd_caller_line=%lu "
               "fd_lifecycle_op=%s oldfd=%ld newfd=%ld "
               "first_visible=%ld fd_visible_refs=%ld file_ref_count=%ld "
               "fdtable=0x%lx replaced_file=0x%lx poll_op=%s "
               "poll_events=0x%lx "
               "poll_revents=0x%lx poll_file_count=%lu "
               "eventfd_readable=%ld eventfd_writable=%ld "
               "poll_readable_mismatch=%ld file_ops=0x%lx "
               "file_poll=0x%lx first_kq_waiters=%ld "
               "last_kq_waiters=%ld "
               "first_ident=%lu last_ident=%lu first_udata=%lu "
               "last_udata=%lu first_kq=0x%lx last_kq=0x%lx "
               "unix_ino=%lu unix_peer_ino=%lu unix_type=%ld "
               "unix_state=%ld unix_shutdown=%ld origin=0x%lx "
               "origin_sym=%s origin_off=%ld origin_file=%s "
               "origin_line=%lu\n",
               (unsigned long)r->seq, prepty_event_name(r->event),
               prepty_role_name(r->role), (unsigned long)r->role_mask,
               (unsigned long)r->notify_ms,
               (unsigned long)r->wait_start_ms,
               (unsigned long)r->waiter_run_ms, (unsigned long)wait_ms,
               (long)r->pid, (long)r->tgid, prepty_str(r->comm),
               prepty_str(r->kind), prepty_str(r->target),
               prepty_str(r->path), prepty_str(r->peer_path),
               (long)r->filter, (long)r->fd, (long)r->nfds,
               (long)r->ready, (long)r->nevents, (long)r->timeout_ms,
               (long)r->has_unnotified_fds, (long)r->matched,
               (long)r->enqueued_new, (long)r->already_queued,
               (long)r->propagated, (long)r->data, (long)r->bytes,
               (unsigned long)r->nread, (unsigned long)r->nwrite,
               (unsigned long)r->readable_before,
               (unsigned long)r->readable_after, (unsigned long)r->file,
               (unsigned long)r->eventfd_id,
               (unsigned long)r->eventfd_object,
               (unsigned long)r->eventfd_count,
               prepty_eventfd_op_name(r->eventfd_op),
               (unsigned long)r->eventfd_counter_before,
               (unsigned long)r->eventfd_counter_after,
               (unsigned long)r->eventfd_value,
               (unsigned long)r->eventfd_read_value,
               (long)r->eventfd_ret,
               (unsigned long)r->eventfd_caller,
               prepty_str(r->eventfd_caller_sym),
               (long)r->eventfd_caller_off,
               prepty_str(r->eventfd_caller_file),
               (unsigned long)r->eventfd_caller_line,
               prepty_fd_lifecycle_name(r->fd_lifecycle_op),
               (long)r->oldfd, (long)r->newfd,
               (long)r->first_visible, (long)r->fd_visible_refs,
               (long)r->file_ref_count, (unsigned long)r->fdtable,
               (unsigned long)r->replaced_file,
               prepty_poll_op_name(r->poll_op),
               (unsigned long)r->poll_events,
               (unsigned long)r->poll_revents,
               (unsigned long)r->poll_file_count,
               (long)r->eventfd_readable, (long)r->eventfd_writable,
               (long)r->poll_readable_mismatch,
               (unsigned long)r->file_ops, (unsigned long)r->file_poll,
               (long)r->first_kq_waiters, (long)r->last_kq_waiters,
               (unsigned long)r->first_ident,
               (unsigned long)r->last_ident,
               (unsigned long)r->first_udata,
               (unsigned long)r->last_udata,
               (unsigned long)r->first_kq, (unsigned long)r->last_kq,
               (unsigned long)r->unix_ino,
               (unsigned long)r->unix_peer_ino, (long)r->unix_type,
               (long)r->unix_state, (long)r->unix_shutdown,
               (unsigned long)r->origin, prepty_str(r->origin_sym),
               (long)r->origin_off, prepty_str(r->origin_file),
               (unsigned long)r->origin_line);
    }
    print_prepty_fd_summaries(snap, count);
}

static const char *userpc_str(const char *s)
{
    return s != 0 && s[0] != '\0' ? s : "-";
}

struct userpc_pid_map_status {
    int used;
    int tgid;
    uint64 sampled_records;
    uint64 last_counted_seq;
    uint64 last_sample_seq;
    uint64 last_scan_seq;
    uint64 snapshots;
    uint64 open_failures;
    uint64 read_failures;
    uint64 lines_seen;
    uint64 lines_stored;
    int last_error;
};

struct userpc_map_entry {
    int used;
    int tgid;
    uint64 start;
    uint64 end;
    uint64 offset;
    uint64 dev_major;
    uint64 dev_minor;
    uint64 inode;
    char perms[5];
    char path[KPROFILE_USERPC_MAP_PATH_LEN];
    uint64 samples;
    uint64 first_seq;
    uint64 last_seq;
};

struct userpc_maps {
    struct userpc_pid_map_status pids[KPROFILE_USERPC_MAP_PID_CAP];
    struct userpc_map_entry entries[KPROFILE_USERPC_MAP_CAP];
    uint64 pid_dropped;
    uint64 map_dropped;
    uint64 path_truncated;
    uint64 line_truncated;
    uint64 parse_failures;
    uint64 attempted;
    uint64 succeeded;
    uint64 open_failed;
    uint64 read_failed;
};

static void userpc_maps_init(struct userpc_maps *maps)
{
    if (maps != 0)
        memset(maps, 0, sizeof(*maps));
}

static int userpc_map_pid_slot(struct userpc_maps *maps, int tgid)
{
    int free_slot = -1;

    if (maps == 0 || tgid <= 0)
        return -1;
    for (int i = 0; i < KPROFILE_USERPC_MAP_PID_CAP; i++) {
        if (maps->pids[i].used && maps->pids[i].tgid == tgid)
            return i;
        if (free_slot < 0 && !maps->pids[i].used)
            free_slot = i;
    }
    if (free_slot < 0) {
        maps->pid_dropped++;
        return -1;
    }
    memset(&maps->pids[free_slot], 0, sizeof(maps->pids[free_slot]));
    maps->pids[free_slot].used = 1;
    maps->pids[free_slot].tgid = tgid;
    return free_slot;
}

static int userpc_hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int userpc_parse_hex_until(const char **pp, char delim, uint64 *out)
{
    const char *p = *pp;
    uint64 v = 0;
    int digits = 0;

    while (*p != '\0' && *p != delim) {
        int d = userpc_hex_digit(*p);

        if (d < 0)
            return -1;
        if (v > (KPROFILE_UINT64_MAX - (uint64)d) / 16)
            return -1;
        v = v * 16 + (uint64)d;
        digits++;
        p++;
    }
    if (digits == 0 || *p != delim)
        return -1;
    *out = v;
    *pp = p + 1;
    return 0;
}

static int userpc_parse_dec_until_space(const char **pp, uint64 *out)
{
    const char *p = *pp;
    uint64 v = 0;
    int digits = 0;

    while (*p >= '0' && *p <= '9') {
        uint64 d = (uint64)(*p - '0');

        if (v > (KPROFILE_UINT64_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
        digits++;
        p++;
    }
    if (digits == 0)
        return -1;
    *out = v;
    *pp = p;
    return 0;
}

static void userpc_skip_spaces(const char **pp)
{
    while (**pp == ' ' || **pp == '\t')
        (*pp)++;
}

static void userpc_copy_sanitized_path(char *dst, uint dst_len,
                                       const char *src, int *truncated)
{
    uint i = 0;

    if (dst_len == 0)
        return;
    if (src == 0) {
        dst[0] = '\0';
        return;
    }
    while (*src == ' ' || *src == '\t')
        src++;
    for (; *src != '\0' && *src != '\n' && *src != '\r'; src++) {
        char c = *src;

        if (i + 1 >= dst_len) {
            if (truncated != 0)
                *truncated = 1;
            break;
        }
        if (c == ' ' || c == '\t')
            c = '_';
        dst[i++] = c;
    }
    dst[i] = '\0';
}

static int userpc_parse_maps_line(const char *line,
                                  struct userpc_map_entry *out,
                                  int *path_truncated)
{
    const char *p = line;
    uint64 start;
    uint64 end;
    uint64 offset;
    uint64 dev_major;
    uint64 dev_minor;
    uint64 inode;

    memset(out, 0, sizeof(*out));
    if (userpc_parse_hex_until(&p, '-', &start) < 0 ||
        userpc_parse_hex_until(&p, ' ', &end) < 0)
        return -1;
    userpc_skip_spaces(&p);
    for (int i = 0; i < 4; i++) {
        if (p[i] == '\0' || p[i] == ' ' || p[i] == '\t')
            return -1;
        out->perms[i] = p[i];
    }
    out->perms[4] = '\0';
    p += 4;
    userpc_skip_spaces(&p);
    if (userpc_parse_hex_until(&p, ' ', &offset) < 0)
        return -1;
    userpc_skip_spaces(&p);
    if (userpc_parse_hex_until(&p, ':', &dev_major) < 0 ||
        userpc_parse_hex_until(&p, ' ', &dev_minor) < 0)
        return -1;
    userpc_skip_spaces(&p);
    if (userpc_parse_dec_until_space(&p, &inode) < 0)
        return -1;
    userpc_skip_spaces(&p);

    out->start = start;
    out->end = end;
    out->offset = offset;
    out->dev_major = dev_major;
    out->dev_minor = dev_minor;
    out->inode = inode;
    userpc_copy_sanitized_path(out->path, sizeof(out->path), p,
                               path_truncated);
    return start < end ? 0 : -1;
}

static int userpc_map_entry_same(const struct userpc_map_entry *a,
                                 const struct userpc_map_entry *b)
{
    return a->used && a->tgid == b->tgid && a->start == b->start &&
           a->end == b->end && a->offset == b->offset &&
           a->dev_major == b->dev_major && a->dev_minor == b->dev_minor &&
           a->inode == b->inode && strcmp(a->perms, b->perms) == 0 &&
           strcmp(a->path, b->path) == 0;
}

static void userpc_store_map_entry(struct userpc_maps *maps,
                                   struct userpc_pid_map_status *pid_status,
                                   const struct userpc_map_entry *entry)
{
    int free_slot = -1;

    for (int i = 0; i < KPROFILE_USERPC_MAP_CAP; i++) {
        if (userpc_map_entry_same(&maps->entries[i], entry))
            return;
        if (free_slot < 0 && !maps->entries[i].used)
            free_slot = i;
    }
    if (free_slot < 0) {
        maps->map_dropped++;
        return;
    }
    maps->entries[free_slot] = *entry;
    maps->entries[free_slot].used = 1;
    pid_status->lines_stored++;
}

static void userpc_read_maps_for_pid(struct userpc_maps *maps,
                                     struct userpc_pid_map_status *pid_status)
{
    char path[64];
    char read_buf[KPROFILE_USERPC_MAP_READ_BUF];
    char line[KPROFILE_USERPC_MAP_LINE_MAX];
    int line_len = 0;
    int discarding = 0;
    int fd;

    if (maps == 0 || pid_status == 0 || pid_status->tgid <= 0)
        return;
    if (pid_status->snapshots >= KPROFILE_USERPC_MAP_SNAPSHOTS_PER_PID)
        return;
    if (pid_status->last_scan_seq == pid_status->last_sample_seq)
        return;

    snprintf(path, sizeof(path), "/proc/%d/maps", pid_status->tgid);
    maps->attempted++;
    pid_status->snapshots++;
    fd = open(path, KPROFILE_O_RDONLY);
    if (fd < 0) {
        maps->open_failed++;
        pid_status->open_failures++;
        pid_status->last_error = fd;
        return;
    }

    for (;;) {
        int n = read(fd, read_buf, sizeof(read_buf));

        if (n < 0) {
            maps->read_failed++;
            pid_status->read_failures++;
            pid_status->last_error = n;
            break;
        }
        if (n == 0)
            break;
        for (int i = 0; i < n; i++) {
            char c = read_buf[i];

            if (discarding) {
                if (c == '\n')
                    discarding = 0;
                continue;
            }
            if (line_len + 1 >= (int)sizeof(line)) {
                maps->line_truncated++;
                discarding = 1;
                line_len = 0;
                continue;
            }
            line[line_len++] = c;
            if (c == '\n') {
                struct userpc_map_entry entry;
                int path_truncated = 0;

                line[line_len] = '\0';
                pid_status->lines_seen++;
                if (userpc_parse_maps_line(line, &entry,
                                           &path_truncated) == 0) {
                    entry.tgid = pid_status->tgid;
                    userpc_store_map_entry(maps, pid_status, &entry);
                    if (path_truncated)
                        maps->path_truncated++;
                } else {
                    maps->parse_failures++;
                }
                line_len = 0;
            }
        }
    }
    if (line_len > 0 && !discarding) {
        struct userpc_map_entry entry;
        int path_truncated = 0;

        line[line_len] = '\0';
        pid_status->lines_seen++;
        if (userpc_parse_maps_line(line, &entry, &path_truncated) == 0) {
            entry.tgid = pid_status->tgid;
            userpc_store_map_entry(maps, pid_status, &entry);
            if (path_truncated)
                maps->path_truncated++;
        } else {
            maps->parse_failures++;
        }
    }
    close(fd);
    pid_status->last_scan_seq = pid_status->last_sample_seq;
    maps->succeeded++;
}

static void collect_userpc_maps_from_snapshot(
    struct userpc_maps *maps, struct kprofile_userpc_snapshot *snap)
{
    uint64 count;

    if (maps == 0 || snap == 0)
        return;
    count = snap->stored;
    if (count > snap->capacity)
        count = snap->capacity;
    if (count > KPROFILE_USERPC_RING_CAP)
        count = KPROFILE_USERPC_RING_CAP;

    for (uint64 i = 0; i < count; i++) {
        struct kprofile_userpc_record *r = &snap->records[i];
        int slot;

        if (r->tgid <= 0 || r->seq == 0)
            continue;
        slot = userpc_map_pid_slot(maps, r->tgid);
        if (slot < 0)
            continue;
        if (r->seq > maps->pids[slot].last_counted_seq) {
            maps->pids[slot].sampled_records++;
            maps->pids[slot].last_counted_seq = r->seq;
        }
        if (r->seq > maps->pids[slot].last_sample_seq)
            maps->pids[slot].last_sample_seq = r->seq;
    }

    for (int i = 0; i < KPROFILE_USERPC_MAP_PID_CAP; i++) {
        if (maps->pids[i].used)
            userpc_read_maps_for_pid(maps, &maps->pids[i]);
    }
}

static int userpc_map_contains(const struct userpc_map_entry *entry,
                               const struct kprofile_userpc_record *rec)
{
    return entry->used && entry->tgid == rec->tgid &&
           rec->rip >= entry->start && rec->rip < entry->end;
}

static void userpc_maps_attribute(struct userpc_maps *maps,
                                  struct kprofile_userpc_snapshot *snap,
                                  uint64 *unmapped_samples)
{
    uint64 count;

    if (unmapped_samples != 0)
        *unmapped_samples = 0;
    if (maps == 0 || snap == 0)
        return;
    for (int i = 0; i < KPROFILE_USERPC_MAP_CAP; i++) {
        maps->entries[i].samples = 0;
        maps->entries[i].first_seq = 0;
        maps->entries[i].last_seq = 0;
    }

    count = snap->stored;
    if (count > snap->capacity)
        count = snap->capacity;
    if (count > KPROFILE_USERPC_RING_CAP)
        count = KPROFILE_USERPC_RING_CAP;

    for (uint64 i = 0; i < count; i++) {
        struct kprofile_userpc_record *r = &snap->records[i];
        int found = 0;

        if (r->tgid <= 0 || r->seq == 0)
            continue;
        for (int j = 0; j < KPROFILE_USERPC_MAP_CAP; j++) {
            if (!userpc_map_contains(&maps->entries[j], r))
                continue;
            maps->entries[j].samples++;
            if (maps->entries[j].first_seq == 0 ||
                r->seq < maps->entries[j].first_seq)
                maps->entries[j].first_seq = r->seq;
            if (r->seq > maps->entries[j].last_seq)
                maps->entries[j].last_seq = r->seq;
            found = 1;
            break;
        }
        if (!found && unmapped_samples != 0)
            (*unmapped_samples)++;
    }
}

static const char *userpc_map_path(const struct userpc_map_entry *entry)
{
    return entry->path[0] != '\0' ? entry->path : "-";
}

static void print_userpc_maps(struct userpc_maps *maps,
                              struct kprofile_userpc_snapshot *snap)
{
    uint64 pid_count = 0;
    uint64 map_count = 0;
    uint64 sampled_pids = 0;
    uint64 unmapped_samples = 0;
    int emitted[KPROFILE_USERPC_MAP_CAP];

    if (maps == 0) {
        printf("kprofile-userpc-maps: status=unavailable reason=no_cache\n");
        return;
    }

    userpc_maps_attribute(maps, snap, &unmapped_samples);
    for (int i = 0; i < KPROFILE_USERPC_MAP_PID_CAP; i++) {
        if (maps->pids[i].used) {
            pid_count++;
            if (maps->pids[i].sampled_records != 0)
                sampled_pids++;
        }
    }
    for (int i = 0; i < KPROFILE_USERPC_MAP_CAP; i++) {
        if (maps->entries[i].used)
            map_count++;
    }

    printf("kprofile-userpc-maps: status=%s sampled_pids=%lu pids=%lu "
           "pid_dropped=%lu maps=%lu map_dropped=%lu attempts=%lu "
           "succeeded=%lu open_failed=%lu read_failed=%lu "
           "parse_failures=%lu path_truncated=%lu line_truncated=%lu "
           "unmapped_samples=%lu source=proc_tgid_maps\n",
           map_count != 0 ? "available" : "unavailable",
           (unsigned long)sampled_pids,
           (unsigned long)pid_count,
           (unsigned long)maps->pid_dropped,
           (unsigned long)map_count,
           (unsigned long)maps->map_dropped,
           (unsigned long)maps->attempted,
           (unsigned long)maps->succeeded,
           (unsigned long)maps->open_failed,
           (unsigned long)maps->read_failed,
           (unsigned long)maps->parse_failures,
           (unsigned long)maps->path_truncated,
           (unsigned long)maps->line_truncated,
           (unsigned long)unmapped_samples);

    for (int i = 0; i < KPROFILE_USERPC_MAP_PID_CAP; i++) {
        struct userpc_pid_map_status *p = &maps->pids[i];

        if (!p->used)
            continue;
        printf("kprofile-userpc-map-pid: tgid=%ld sampled_records=%lu "
               "last_sample_seq=%lu snapshots=%lu open_failures=%lu "
               "read_failures=%lu last_error=%ld lines_seen=%lu "
               "lines_stored=%lu\n",
               (long)p->tgid,
               (unsigned long)p->sampled_records,
               (unsigned long)p->last_sample_seq,
               (unsigned long)p->snapshots,
               (unsigned long)p->open_failures,
               (unsigned long)p->read_failures,
               (long)p->last_error,
               (unsigned long)p->lines_seen,
               (unsigned long)p->lines_stored);
    }

    for (int i = 0; i < KPROFILE_USERPC_MAP_CAP; i++) {
        struct userpc_map_entry *m = &maps->entries[i];

        if (!m->used)
            continue;
        printf("kprofile-userpc-map: tgid=%ld start=0x%lx end=0x%lx "
               "perms=%s offset=0x%lx dev=%02lx:%02lx inode=%lu "
               "samples=%lu first_seq=%lu last_seq=%lu path=%s\n",
               (long)m->tgid,
               (unsigned long)m->start,
               (unsigned long)m->end,
               m->perms,
               (unsigned long)m->offset,
               (unsigned long)m->dev_major,
               (unsigned long)m->dev_minor,
               (unsigned long)m->inode,
               (unsigned long)m->samples,
               (unsigned long)m->first_seq,
               (unsigned long)m->last_seq,
               userpc_map_path(m));
    }

    memset(emitted, 0, sizeof(emitted));
    for (int rank = 1; rank <= KPROFILE_USERPC_MODULE_PRINT_CAP; rank++) {
        int best = -1;
        struct userpc_map_entry *m;
        const char *module;
        char module_key[192];
        const char *module_key_value;

        for (int i = 0; i < KPROFILE_USERPC_MAP_CAP; i++) {
            if (!maps->entries[i].used || emitted[i] ||
                maps->entries[i].samples == 0)
                continue;
            if (best < 0 ||
                maps->entries[i].samples > maps->entries[best].samples ||
                (maps->entries[i].samples == maps->entries[best].samples &&
                 maps->entries[i].start < maps->entries[best].start))
                best = i;
        }
        if (best < 0)
            break;
        emitted[best] = 1;
        m = &maps->entries[best];
        module = userpc_map_path(m);
        module_key_value = module;

        if (strcmp(module, "-") == 0) {
            snprintf(module_key, sizeof(module_key),
                     "map:%ld/0x%lx-0x%lx/%s/0x%lx/%02lx:%02lx/%lu",
                     (long)m->tgid, (unsigned long)m->start,
                     (unsigned long)m->end, m->perms,
                     (unsigned long)m->offset, (unsigned long)m->dev_major,
                     (unsigned long)m->dev_minor, (unsigned long)m->inode);
            module_key_value = module_key;
        }
        printf("kprofile-userpc-module: rank=%d tgid=%ld module=%s "
               "range=0x%lx-0x%lx perms=%s offset=0x%lx dev=%02lx:%02lx "
               "inode=%lu samples=%lu first_seq=%lu last_seq=%lu "
               "module_key=%s\n",
               rank,
               (long)m->tgid,
               module,
               (unsigned long)m->start,
               (unsigned long)m->end,
               m->perms,
               (unsigned long)m->offset,
               (unsigned long)m->dev_major,
               (unsigned long)m->dev_minor,
               (unsigned long)m->inode,
               (unsigned long)m->samples,
               (unsigned long)m->first_seq,
               (unsigned long)m->last_seq,
               module_key_value);
    }
}

static void print_userpc_ring(struct kprofile_userpc_snapshot *snap,
                              uint64 limit, struct userpc_maps *maps)
{
    uint64 count;
    uint64 rows;

    if (snap == 0)
        return;

    count = snap->stored;
    if (count > snap->capacity)
        count = snap->capacity;
    if (count > KPROFILE_USERPC_RING_CAP)
        count = KPROFILE_USERPC_RING_CAP;
    rows = count;
    if (limit != 0 && rows > limit)
        rows = limit;

    printf("kprofile-userpc-summary: abi=%lu enabled=%lu target_pgid=%ld "
           "period_ticks=%lu capacity=%lu stored=%lu total_seen=%lu "
           "overwritten=%lu dropped=%lu first_seq=%lu last_seq=%lu "
           "printed=%lu limit=%lu\n",
           (unsigned long)snap->abi_version,
           (unsigned long)snap->enabled,
           (long)snap->target_pgid,
           (unsigned long)snap->period_ticks,
           (unsigned long)snap->capacity,
           (unsigned long)count,
           (unsigned long)snap->total_seen,
           (unsigned long)snap->overwritten,
           (unsigned long)snap->dropped,
           (unsigned long)snap->first_seq,
           (unsigned long)snap->last_seq,
           (unsigned long)rows,
           (unsigned long)limit);
    print_userpc_maps(maps, snap);

    for (uint64 i = 0; i < rows; i++) {
        struct kprofile_userpc_record *r = &snap->records[i];

        printf("kprofile-userpc: seq=%lu timestamp=%lu uptime_ms=%lu "
               "cpu=%ld pid=%ld tgid=%ld pgid=%ld comm=%s rip=0x%lx "
               "rsp=0x%lx rbp=0x%lx trapno=%lu flags=0x%lx\n",
               (unsigned long)r->seq,
               (unsigned long)r->timestamp,
               (unsigned long)r->uptime_ms,
               (long)r->cpu,
               (long)r->pid,
               (long)r->tgid,
               (long)r->pgid,
               userpc_str(r->comm),
               (unsigned long)r->rip,
               (unsigned long)r->rsp,
               (unsigned long)r->rbp,
               (unsigned long)r->trapno,
               (unsigned long)r->flags);
    }
}

static const char *vfs_enoent_source_name(int source)
{
    switch (source) {
    case KPROFILE_VFS_ENOENT_SOURCE_OPENAT_RET:
        return "openat_ret";
    case KPROFILE_VFS_ENOENT_SOURCE_EXT4_LOOKUP:
        return "ext4_lookup";
    default:
        return "unknown";
    }
}

static const char *vfs_enoent_dirfd_class_name(int dirfd_class)
{
    switch (dirfd_class) {
    case KPROFILE_VFS_ENOENT_DIRFD_ABSOLUTE:
        return "absolute";
    case KPROFILE_VFS_ENOENT_DIRFD_AT_FDCWD:
        return "at_fdcwd";
    case KPROFILE_VFS_ENOENT_DIRFD_FD:
        return "fd";
    case KPROFILE_VFS_ENOENT_DIRFD_UNKNOWN:
    default:
        return "unknown";
    }
}

static void print_escaped_quoted(const char *s)
{
    printf("\"");
    if (s != 0) {
        for (; *s != '\0'; s++) {
            unsigned char c = (unsigned char)*s;

            if (c == '\\')
                printf("\\\\");
            else if (c == '"')
                printf("\\\"");
            else if (c == ' ')
                printf("\\s");
            else if (c == '\t')
                printf("\\t");
            else if (c == '\n')
                printf("\\n");
            else if (c == '\r')
                printf("\\r");
            else if (c < 32 || c >= 127)
                printf("?");
            else
                printf("%c", c);
        }
    }
    printf("\"");
}

static void print_vfs_enoent_snapshot(
    struct kprofile_vfs_enoent_snapshot *snap)
{
    uint64 count;
    int emitted[KPROFILE_VFS_ENOENT_TABLE_CAP];

    if (snap == 0)
        return;

    count = snap->stored;
    if (count > snap->capacity)
        count = snap->capacity;
    if (count > KPROFILE_VFS_ENOENT_TABLE_CAP)
        count = KPROFILE_VFS_ENOENT_TABLE_CAP;

    printf("kprofile-vfs-enoent-summary scope=global enabled=%lu total=%lu stored=%lu "
           "dropped=%lu capacity=%lu abi=%lu\n",
           (unsigned long)snap->enabled,
           (unsigned long)snap->total,
           (unsigned long)count,
           (unsigned long)snap->dropped,
           (unsigned long)snap->capacity,
           (unsigned long)snap->abi_version);

    memset(emitted, 0, sizeof(emitted));
    for (uint64 rank = 1; rank <= count; rank++) {
        int best = -1;

        for (uint64 i = 0; i < count; i++) {
            if (emitted[i] || snap->rows[i].count == 0)
                continue;
            if (best < 0 ||
                snap->rows[i].count > snap->rows[best].count ||
                (snap->rows[i].count == snap->rows[best].count &&
                 snap->rows[i].first_ms < snap->rows[best].first_ms))
                best = (int)i;
        }
        if (best < 0)
            break;
        emitted[best] = 1;

        struct kprofile_vfs_enoent_row *r = &snap->rows[best];
        printf("kprofile-vfs-enoent-row scope=global rank=%lu source=%s tgid=%ld "
               "first_pid=%ld comm=",
               (unsigned long)rank, vfs_enoent_source_name(r->source),
               (long)r->tgid, (long)r->pid);
        print_escaped_quoted(r->comm);
        printf(" count=%lu dirfd_class=%s first_dirfd=%ld flags_class=0x%lx "
               "first_flags=0x%lx first_ms=%lu last_ms=%lu prefix=",
               (unsigned long)r->count,
               vfs_enoent_dirfd_class_name(r->dirfd_class),
               (long)r->dirfd,
               (unsigned long)r->flags_class,
               (unsigned long)r->flags,
               (unsigned long)r->first_ms,
               (unsigned long)r->last_ms);
        print_escaped_quoted(r->prefix);
        printf(" basename=");
        print_escaped_quoted(r->basename);
        printf("\n");
    }
}

static int parse_uint_arg(const char *s, uint64 *value)
{
    uint64 v = 0;

    if (!s || !*s)
        return -1;
    for (; *s; s++) {
        uint64 digit;

        if (*s < '0' || *s > '9')
            return -1;
        digit = (uint64)(*s - '0');
        if (v > (KPROFILE_UINT64_MAX - digit) / 10)
            return -1;
        v = v * 10 + digit;
    }
    *value = v;
    return 0;
}

static int parse_options(int argc, char **argv, int *cmd_index,
                         int *profile_pgroup, uint64 *timeout_ms,
                         int *userpc, uint64 *userpc_period,
                         uint64 *userpc_limit, int *vfs_enoent_attr)
{
    int i = 1;

    *profile_pgroup = 1;
    *timeout_ms = 0;
    *userpc = 0;
    *userpc_period = 1;
    *userpc_limit = 0;
    *vfs_enoent_attr = 0;
    while (i < argc) {
        if (strcmp(argv[i], "--system-only") == 0 ||
            strcmp(argv[i], "--no-pgroup") == 0) {
            *profile_pgroup = 0;
            i++;
        } else if (strcmp(argv[i], "--pgroup") == 0) {
            *profile_pgroup = 1;
            i++;
        } else if (strcmp(argv[i], "--timeout-ms") == 0) {
            if (i + 1 >= argc || parse_uint_arg(argv[i + 1], timeout_ms) < 0)
                return -1;
            i += 2;
        } else if (strcmp(argv[i], "--userpc") == 0) {
            *userpc = 1;
            *profile_pgroup = 1;
            i++;
        } else if (strcmp(argv[i], "--userpc-period") == 0) {
            if (i + 1 >= argc ||
                parse_uint_arg(argv[i + 1], userpc_period) < 0 ||
                *userpc_period == 0 ||
                *userpc_period > KPROFILE_USERPC_PERIOD_MAX)
                return -1;
            *userpc = 1;
            *profile_pgroup = 1;
            i += 2;
        } else if (strcmp(argv[i], "--userpc-limit") == 0) {
            if (i + 1 >= argc ||
                parse_uint_arg(argv[i + 1], userpc_limit) < 0 ||
                *userpc_limit == 0 ||
                *userpc_limit > KPROFILE_USERPC_RING_CAP)
                return -1;
            *userpc = 1;
            *profile_pgroup = 1;
            i += 2;
        } else if (strcmp(argv[i], "--vfs-enoent-attr") == 0) {
            *vfs_enoent_attr = 1;
            i++;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            break;
        }
    }
    if (*userpc)
        *profile_pgroup = 1;
    *cmd_index = i;
    return i < argc ? 0 : -1;
}

int main(int argc, char *argv[])
{
    int cmd_index;
    int profile_pgroup;
    int userpc;
    int vfs_enoent_attr;
    int userpc_armed = 0;
    uint64 timeout_ms;
    uint64 userpc_period;
    uint64 userpc_limit;
    uint64 child_start_ms = 0;
    int timeout_hit = 0;
    int term_sent = 0;
    int kill_sent = 0;
    int status = 0;
    int ready_pipe[2] = {-1, -1};
    int go_pipe[2] = {-1, -1};
    struct kprofile_pgroup pg_before;
    struct kprofile_pgroup pg_after;
    int have_pg_before = 0;
    int have_pg_after = 0;
    struct konsole_prepty_wake_snapshot *prepty_after = 0;
    int have_prepty_after = 0;
    struct kprofile_userpc_snapshot *userpc_after = 0;
    int have_userpc_after = 0;
    struct userpc_maps *userpc_maps = 0;
    struct kprofile_vfs_enoent_snapshot *vfs_enoent_after = 0;
    int have_vfs_enoent_after = 0;

    if (parse_options(argc, argv, &cmd_index, &profile_pgroup,
                      &timeout_ms, &userpc, &userpc_period,
                      &userpc_limit, &vfs_enoent_attr) < 0) {
        printf("usage: kprofile [--system-only|--pgroup] [--timeout-ms N] [--userpc] [--userpc-period N] [--userpc-limit N] [--vfs-enoent-attr] [--] <command> [args...]\n");
        exit(1);
    }
    if (userpc) {
        userpc_maps = malloc(sizeof(*userpc_maps));
        if (userpc_maps != 0)
            userpc_maps_init(userpc_maps);
    }

    struct kstats before;
    struct kstats after;

    int kstats_flags = KSTATS_PROFILE_F_ENABLE;
    if (vfs_enoent_attr)
        kstats_flags |= KSTATS_PROFILE_F_VFS_ENOENT_ATTR;

    if (kstatsctl(kstats_flags) < 0) {
        printf("kprofile: kstatsctl(enable) failed\n");
        exit(1);
    }

    if (read_kstats(&before) < 0) {
        printf("kprofile: kstats(before) failed\n");
        kstatsctl(0);
        exit(1);
    }

    if (profile_pgroup) {
        if (pipe(ready_pipe) < 0 || pipe(go_pipe) < 0) {
            printf("kprofile: pipe failed\n");
            kstatsctl(0);
            exit(1);
        }
    }

    int pid = fork();
    if (pid < 0) {
        printf("kprofile: fork failed\n");
        kstatsctl(0);
        exit(1);
    }

    if (pid == 0) {
        if (profile_pgroup) {
            char ch = 'r';

            close(ready_pipe[0]);
            close(go_pipe[1]);
            setpgid(0, 0);
            if (write(ready_pipe[1], &ch, 1) != 1)
                exit(1);
            close(ready_pipe[1]);
            if (read(go_pipe[0], &ch, 1) != 1)
                exit(1);
            close(go_pipe[0]);
        }
        exec(argv[cmd_index], &argv[cmd_index]);
        printf("kprofile: exec %s failed\n", argv[cmd_index]);
        exit(1);
    }

    if (profile_pgroup) {
        char ch = 0;

        close(ready_pipe[1]);
        close(go_pipe[0]);
        if (read(ready_pipe[0], &ch, 1) != 1) {
            printf("kprofile: child setup failed\n");
            kstatsctl(0);
            exit(1);
        }
        close(ready_pipe[0]);
        setpgid(pid, pid);
        if (read_pgroup(pid, &pg_before) == 0) {
            pg_after = pg_before;
            have_pg_before = 1;
            have_pg_after = 1;
        }
        if (userpc) {
            if (userpc_ctl(1, pid, userpc_period) < 0) {
                printf("kprofile: kprofile_userpc_ctl(enable) failed\n");
                close(go_pipe[1]);
                waitpid(pid, &status, 0);
                kstatsctl(0);
                exit(1);
            }
            userpc_armed = 1;
        }
        ch = 'g';
        if (write(go_pipe[1], &ch, 1) != 1) {
            printf("kprofile: child start failed\n");
            if (userpc_armed)
                userpc_ctl(0, 0, userpc_period);
            kstatsctl(0);
            exit(1);
        }
        close(go_pipe[1]);
    }
    child_start_ms = (uint64)uptime();

    if (profile_pgroup) {
        for (;;) {
            struct kprofile_pgroup sample;
            int w;
            uint64 now_ms;

            if (read_pgroup(pid, &sample) == 0) {
                pg_after = sample;
                have_pg_after = 1;
            }
            if (userpc)
                capture_userpc_state(&userpc_after, &have_userpc_after,
                                     userpc_maps);
            w = waitpid(pid, &status, WNOHANG);
            if (w == pid)
                break;
            if (w < 0) {
                status = 1 << 8;
                break;
            }
            now_ms = (uint64)uptime();
            if (timeout_ms && now_ms - child_start_ms >= timeout_ms) {
                timeout_hit = 1;
                if (!term_sent) {
                    if (userpc) {
                        capture_userpc_state(&userpc_after,
                                             &have_userpc_after,
                                             userpc_maps);
                        disarm_userpc_if_armed(&userpc_armed, userpc_period);
                    }
                    kill(-pid, SIGTERM);
                    term_sent = 1;
                } else if (!kill_sent &&
                           now_ms - child_start_ms >= timeout_ms + 2000) {
                    if (userpc) {
                        capture_userpc_state(&userpc_after,
                                             &have_userpc_after,
                                             userpc_maps);
                        disarm_userpc_if_armed(&userpc_armed, userpc_period);
                    }
                    kill(-pid, SIGKILL);
                    kill_sent = 1;
                } else if (kill_sent &&
                           now_ms - child_start_ms >= timeout_ms + 4000) {
                    break;
                }
            }
            profile_sleep();
        }
    } else {
        waitpid(pid, &status, 0);
    }

    if (read_kstats(&after) < 0) {
        printf("kprofile: kstats(after) failed\n");
        if (userpc_armed)
            userpc_ctl(0, 0, userpc_period);
        kstatsctl(0);
        exit(1);
    }
    if (userpc) {
        disarm_userpc_if_armed(&userpc_armed, userpc_period);
        if (!have_userpc_after)
            capture_userpc_state(&userpc_after, &have_userpc_after,
                                 userpc_maps);
        else
            collect_userpc_maps_from_snapshot(userpc_maps, userpc_after);
    }
    if (userpc) {
        prepty_after = malloc(sizeof(*prepty_after));
        if (prepty_after != 0 && read_prepty_ring(prepty_after) == 0)
            have_prepty_after = 1;
    }
    if (vfs_enoent_attr) {
        vfs_enoent_after = malloc(sizeof(*vfs_enoent_after));
        if (vfs_enoent_after != 0 &&
            read_vfs_enoent_snapshot(vfs_enoent_after) == 0)
            have_vfs_enoent_after = 1;
    }
    kstatsctl(0);

    printf("elapsed_ms                  %lu\n",
           (unsigned long)(after.uptime_ms - before.uptime_ms));
    printf("%-28s %lu\n", "kprofile_timeout_ms",
           (unsigned long)timeout_ms);
    printf("%-28s %d\n", "kprofile_timeout_hit", timeout_hit);
    print_cpu_metrics(&after, &before);
    print_delta("sched_starve_probe_snapshots",
                after.sched_starve_probe_snapshots,
                before.sched_starve_probe_snapshots);
    print_delta("sched_starve_idle_needs_resched",
                after.sched_starve_probe_idle_needs_resched_samples,
                before.sched_starve_probe_idle_needs_resched_samples);
    if (profile_pgroup) {
        if (have_pg_before && have_pg_after) {
            print_pgroup_metrics(&pg_after, &pg_before);
        } else {
            printf("%-28s %s\n", "pgroup_status", "unavailable");
        }
    }
    if (have_prepty_after)
        print_prepty_ring(prepty_after);
    if (prepty_after != 0)
        free(prepty_after);
    if (userpc) {
        if (have_userpc_after)
            print_userpc_ring(userpc_after, userpc_limit, userpc_maps);
        else
            printf("kprofile-userpc-summary: status=unavailable\n");
    }
    if (userpc_after != 0)
        free(userpc_after);
    if (userpc_maps != 0)
        free(userpc_maps);
    if (vfs_enoent_attr) {
        if (have_vfs_enoent_after)
            print_vfs_enoent_snapshot(vfs_enoent_after);
        else
            printf("kprofile-vfs-enoent-summary scope=global enabled=0 total=0 stored=0 dropped=0 status=unavailable\n");
    }
    if (vfs_enoent_after != 0)
        free(vfs_enoent_after);
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

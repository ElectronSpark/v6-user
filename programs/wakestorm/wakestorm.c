#ifdef HOST_LIBC_PROGRAM
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

#include "kernel/inc/kstats.h"
#ifndef HOST_LIBC_PROGRAM
#include "kernel/inc/clone_flags.h"
#include "kernel/inc/syscall.h"
#include "kernel/inc/types.h"
#include "user/user.h"
#endif

#define CLOCK_MONOTONIC_ID 1
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define XV6_SYS_KSTATS2 1368

#define MODE_HD "hd"
#define MODE_GUIHD "guihd"
#define DEFAULT_WORKERS 256
#define DEFAULT_GUI_LEAVES 240
#define MIN_WORKERS 200
#define MAX_WORKERS 384
#define DEFAULT_ROUNDS 5
#define HD_MAX_ROUNDS 20
#define MAX_ROUNDS 240
#define DEFAULT_GUI_ROUNDS 60
#define DEFAULT_ACTIVE_MS 2600
#define MAX_ACTIVE_MS 10000
#define DEFAULT_GUI_WINDOW_MS 3200
#define MAX_GUI_WINDOW_MS 10000
#define DEFAULT_TARGET_RUNNABLE 32
#define MAX_TARGET_RUNNABLE 96
#define LEADER_THREADS 2
#define GUI_PRODUCER_THREADS 2
#define DEFAULT_GUI_REACTORS 8
#define MAX_GUI_REACTORS 16
#define DEFAULT_CHURN_THREADS 24
#define MAX_CHURN_THREADS 32
#define STACK_SIZE 16384
#define ANCHOR_MS 140
#define LEADER_WAIT_MS 1500
#define QUIET_GRACE_MS 10000
#define STOP_WAIT_MS 4000
#define MAX_GUI_STALL_SAMPLES 65536

static volatile int start_flag;
static volatile int stop_flag;
static volatile int ready_count;
static volatile int leader_cmd;
static volatile int leaders_done;
static volatile int anchor_cmd;
static volatile int gui_round_cmd;
static volatile int gui_round_done;
static volatile int quiet_count;
static volatile int worker_seq[MAX_WORKERS];
static volatile int worker_runs[MAX_WORKERS];
static volatile int worker_done[MAX_WORKERS];
static volatile int leader_done[LEADER_THREADS];
static volatile int gui_reactor_seq[MAX_GUI_REACTORS];
static volatile int gui_reactor_done[MAX_GUI_REACTORS];
static volatile int gui_producer_done[GUI_PRODUCER_THREADS];
static volatile int gui_churn_done[MAX_CHURN_THREADS];
static volatile int anchor_runs[MAX_CHURN_THREADS];
static uint64 wake_stamp_ns[MAX_WORKERS];
static uint64 gui_reactor_wake_stamp_ns[MAX_GUI_REACTORS];
static uint64 max_stall_ns[MAX_WORKERS];
static uint64 gui_stall_samples[MAX_GUI_STALL_SAMPLES];
static uint64 burst_end_ns[MAX_ROUNDS + 2];
static uint64 leader_issue_ns[MAX_ROUNDS + 2];
static int pipefd[MAX_WORKERS][2];
static int gui_reactor_pipefd[MAX_GUI_REACTORS][2];
static char worker_stacks[MAX_WORKERS][STACK_SIZE];
static char leader_stacks[LEADER_THREADS][STACK_SIZE];
static char gui_producer_stacks[GUI_PRODUCER_THREADS][STACK_SIZE];
static char gui_reactor_stacks[MAX_GUI_REACTORS][STACK_SIZE];
static char churn_stacks[MAX_CHURN_THREADS][STACK_SIZE];
static int worker_count;
static int round_count;
static int active_ms;
static int gui_window_ms;
static int gui_target_runnable;
static int gui_reactor_count;
static int churn_count;
static int kstats_ok = 1;
static volatile uint64 gui_round_end_ns;
static volatile uint64 gui_leaf_cursor;
static volatile uint64 gui_stall_sample_count;
static volatile uint64 gui_max_wake_to_run_ns;
static volatile uint64 gui_max_issue_ns;
static volatile uint64 gui_reactor_runs;
static volatile uint64 gui_leaf_runs;
static volatile uint64 gui_idle_churn_runs;
static volatile uint64 futex_wake_calls;
static volatile uint64 pipe_wake_calls;
static volatile uint64 spin_iterations;

#ifdef HOST_LIBC_PROGRAM
static inline int64 raw_syscall0(int n)
{
    return (int64)syscall(n);
}

static inline int64 raw_syscall2(int n, int64 a, int64 b)
{
    return (int64)syscall(n, a, b);
}

static inline int64 raw_syscall6(int n, int64 a, int64 b, int64 c, int64 d,
                                 int64 e, int64 f)
{
    return (int64)syscall(n, a, b, c, d, e, f);
}
#elif defined(__riscv)
static inline int64 raw_syscall0(int n)
{
    register int64 a7 asm("a7") = n;
    register int64 a0 asm("a0");
    asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_syscall2(int n, int64 a, int64 b)
{
    register int64 a7 asm("a7") = n;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_syscall6(int n, int64 a, int64 b, int64 c, int64 d,
                                 int64 e, int64 f)
{
    register int64 a7 asm("a7") = n;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    register int64 a3 asm("a3") = d;
    register int64 a4 asm("a4") = e;
    register int64 a5 asm("a5") = f;
    asm volatile("ecall" : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                 : "memory");
    return a0;
}
#elif defined(__x86_64__)
static inline int64 raw_syscall0(int n)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_syscall2(int n, int64 a, int64 b)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n), "D"(a), "S"(b)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_syscall6(int n, int64 a, int64 b, int64 c, int64 d,
                                 int64 e, int64 f)
{
    int64 ret;
    register int64 r10 asm("r10") = d;
    register int64 r8 asm("r8") = e;
    register int64 r9 asm("r9") = f;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n), "D"(a), "S"(b), "d"(c), "r"(r10),
                   "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}
#else
#error unsupported architecture
#endif

static uint64 atomic_load_u64(volatile uint64 *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
}

static int atomic_load_int(volatile int *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
}

static int atomic_add_int(volatile int *ptr, int val)
{
    return __atomic_add_fetch(ptr, val, __ATOMIC_SEQ_CST);
}

static void atomic_store_int(volatile int *ptr, int val)
{
    __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST);
}

static void atomic_inc_u64(volatile uint64 *ptr)
{
    __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST);
}

static uint64 atomic_fetch_add_u64(volatile uint64 *ptr, uint64 val)
{
    return __atomic_fetch_add(ptr, val, __ATOMIC_SEQ_CST);
}

static void atomic_max_u64(volatile uint64 *ptr, uint64 val)
{
    uint64 old = __atomic_load_n(ptr, __ATOMIC_SEQ_CST);

    while (old < val &&
           !__atomic_compare_exchange_n(ptr, &old, val, 0,
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
        ;
}

static uint64 now_ns(void)
{
    struct timespec ts;

#ifdef HOST_LIBC_PROGRAM
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
#else
    if (raw_syscall2(SYS_clock_gettime, CLOCK_MONOTONIC_ID,
                     (int64)&ts) < 0)
        return 0;
#endif
    return (uint64)ts.tv_sec * 1000000000ULL + (uint64)ts.tv_nsec;
}

static int futex_wait(volatile int *addr, int val)
{
    return (int)raw_syscall6(SYS_futex, (int64)addr,
                             FUTEX_WAIT | FUTEX_PRIVATE_FLAG, val, 0, 0, 0);
}

static int futex_wake(volatile int *addr, int nr)
{
    atomic_inc_u64(&futex_wake_calls);
    return (int)raw_syscall6(SYS_futex, (int64)addr,
                             FUTEX_WAKE | FUTEX_PRIVATE_FLAG, nr, 0, 0, 0);
}

static void cpu_yield(void)
{
    (void)raw_syscall0(SYS_sched_yield);
}

static void sleep_ms(int ms)
{
#ifdef HOST_LIBC_PROGRAM
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, 0);
#else
    sleep(ms);
#endif
}

static void burn_until(uint64 end_ns, int yield_shift);

static void burn_for_us(int usec, int yield_shift)
{
    burn_until(now_ns() + (uint64)usec * 1000ULL, yield_shift);
}

static void signal_ready(void)
{
    atomic_add_int(&ready_count, 1);
    futex_wake(&ready_count, 1);
}

static int kstats_full(struct kstats *ks)
{
#ifdef HOST_LIBC_PROGRAM
    return (int)syscall(XV6_SYS_KSTATS2, ks, sizeof(*ks));
#else
    return kstats2(ks, sizeof(*ks));
#endif
}

static void thread_exit(int rc)
{
#ifdef HOST_LIBC_PROGRAM
    (void)syscall(SYS_exit, rc);
    for (;;)
        ;
#else
    exit(rc);
#endif
}

static void process_exit(int rc)
{
#ifdef HOST_LIBC_PROGRAM
    (void)syscall(SYS_exit_group, rc);
    _exit(rc);
#else
    exit_group(rc);
#endif
}

static int positive_arg(const char *s)
{
    if (s == 0 || *s < '0' || *s > '9')
        return 0;
    return 1;
}

static int parse_positive(const char *s, int fallback, int max)
{
    int val;

    if (s == 0)
        return fallback;
    val = atoi(s);
    if (val <= 0)
        return fallback;
    if (val > max)
        return max;
    return val;
}

static int clamp_int(int val, int min, int max)
{
    if (val < min)
        return min;
    if (val > max)
        return max;
    return val;
}

static int identify_stack(char stacks[][STACK_SIZE], int count, uint64 sp)
{
    for (int i = 0; i < count; i++) {
        uint64 base = (uint64)&stacks[i][0];
        uint64 end = (uint64)&stacks[i][STACK_SIZE];
        if (sp >= base && sp < end)
            return i;
    }
    return -1;
}

static void burn_until(uint64 end_ns, int yield_shift)
{
    uint64 loops = 0;
    volatile uint64 mix = 0x9e3779b97f4a7c15ULL;

    while (!atomic_load_int(&stop_flag)) {
        for (int i = 0; i < 512; i++) {
            mix ^= mix << 7;
            mix ^= mix >> 9;
            mix += (uint64)i + 0x100000001b3ULL;
        }
        loops++;
        if ((loops & ((1U << yield_shift) - 1U)) == 0)
            cpu_yield();
        if ((loops & 31U) == 0 && now_ns() >= end_ns)
            break;
    }
    __atomic_add_fetch(&spin_iterations, loops, __ATOMIC_SEQ_CST);
}

static void record_gui_stall(uint64 sent, uint64 started)
{
    uint64 stall;
    uint64 idx;

    if (sent == 0 || started < sent)
        return;
    stall = started - sent;
    atomic_max_u64(&gui_max_wake_to_run_ns, stall);
    idx = atomic_fetch_add_u64(&gui_stall_sample_count, 1);
    if (idx < MAX_GUI_STALL_SAMPLES)
        gui_stall_samples[idx] = stall;
}

static uint64 select_p99_ns(uint64 count)
{
    uint64 left = 0;
    uint64 right;
    uint64 want;

    if (count == 0)
        return 0;
    if (count > MAX_GUI_STALL_SAMPLES)
        count = MAX_GUI_STALL_SAMPLES;
    right = count - 1;
    want = ((count * 99ULL) + 99ULL) / 100ULL;
    if (want == 0)
        want = 1;
    want--;

    while (left < right) {
        uint64 pivot = gui_stall_samples[(left + right) / 2];
        uint64 i = left;
        uint64 j = right;

        while (i <= j) {
            while (gui_stall_samples[i] < pivot)
                i++;
            while (gui_stall_samples[j] > pivot) {
                if (j == 0)
                    break;
                j--;
            }
            if (i <= j) {
                uint64 tmp = gui_stall_samples[i];
                gui_stall_samples[i] = gui_stall_samples[j];
                gui_stall_samples[j] = tmp;
                i++;
                if (j == 0)
                    break;
                j--;
            }
        }
        if (want <= j)
            right = j;
        else if (want >= i)
            left = i;
        else
            break;
    }

    return gui_stall_samples[want];
}

static void wake_worker(int id, int round)
{
    char byte = (char)(round & 0x7f);

    wake_stamp_ns[id] = now_ns();
    if (id & 1) {
        if (write(pipefd[id][1], &byte, 1) == 1)
            atomic_inc_u64(&pipe_wake_calls);
    } else {
        atomic_store_int(&worker_seq[id], round);
        futex_wake(&worker_seq[id], 1);
    }
}

static void wake_gui_reactor(int id)
{
    char byte = 1;

    gui_reactor_wake_stamp_ns[id] = now_ns();
    if (id & 1) {
        if (write(gui_reactor_pipefd[id][1], &byte, 1) == 1)
            atomic_inc_u64(&pipe_wake_calls);
    } else {
        atomic_add_int(&gui_reactor_seq[id], 1);
        futex_wake(&gui_reactor_seq[id], 1);
    }
}

static void wake_gui_leaf(int id)
{
    char byte = 1;

    wake_stamp_ns[id] = now_ns();
    if (id & 1) {
        if (write(pipefd[id][1], &byte, 1) == 1)
            atomic_inc_u64(&pipe_wake_calls);
    } else {
        atomic_add_int(&worker_seq[id], 1);
        futex_wake(&worker_seq[id], 1);
    }
}

static void worker_entry(void)
{
    int id;
    int seen = 0;
    char byte = 0;
    uint64 sp = (uint64)&id;

    id = identify_stack(worker_stacks, worker_count, sp);
    if (id < 0)
        thread_exit(2);

    signal_ready();
    while (!atomic_load_int(&start_flag))
        cpu_yield();

    while (!atomic_load_int(&stop_flag)) {
        int round;

        if (id & 1) {
            if (read(pipefd[id][0], &byte, 1) != 1) {
                cpu_yield();
                continue;
            }
            round = (int)(byte & 0x7f);
            if (round == 0)
                break;
        } else {
            while (!atomic_load_int(&stop_flag) &&
                   atomic_load_int(&worker_seq[id]) <= seen)
                futex_wait(&worker_seq[id], seen);
            if (atomic_load_int(&stop_flag))
                break;
            round = atomic_load_int(&worker_seq[id]);
        }

        if (round <= seen || round > MAX_ROUNDS)
            continue;
        seen = round;

        uint64 started = now_ns();
        uint64 sent = wake_stamp_ns[id];
        if (sent != 0 && started >= sent) {
            uint64 stall = started - sent;
            if (stall > max_stall_ns[id])
                max_stall_ns[id] = stall;
        }

        atomic_add_int(&worker_runs[id], 1);
        burn_until(burst_end_ns[round], 3);
        atomic_add_int(&quiet_count, 1);
        futex_wake(&quiet_count, 1);
    }

    atomic_store_int(&worker_done[id], 1);
    thread_exit(0);
}

static void gui_leaf_entry(void)
{
    int id;
    int seen = 0;
    char byte = 0;
    uint64 sp = (uint64)&id;

    id = identify_stack(worker_stacks, worker_count, sp);
    if (id < 0)
        thread_exit(2);

    signal_ready();
    while (!atomic_load_int(&start_flag))
        cpu_yield();

    while (!atomic_load_int(&stop_flag)) {
        uint64 started;
        uint64 run;

        if (id & 1) {
            if (read(pipefd[id][0], &byte, 1) != 1) {
                cpu_yield();
                continue;
            }
            if (atomic_load_int(&stop_flag) || byte == 0)
                break;
        } else {
            while (!atomic_load_int(&stop_flag) &&
                   atomic_load_int(&worker_seq[id]) <= seen)
                futex_wait(&worker_seq[id], seen);
            if (atomic_load_int(&stop_flag))
                break;
            seen = atomic_load_int(&worker_seq[id]);
        }

        started = now_ns();
        record_gui_stall(wake_stamp_ns[id], started);
        run = atomic_fetch_add_u64(&gui_leaf_runs, 1) + 1;
        atomic_add_int(&worker_runs[id], 1);
        burn_for_us(100 + (int)((run * 53ULL + (uint64)id * 37ULL) % 701ULL),
                    5);
    }

    atomic_store_int(&worker_done[id], 1);
    thread_exit(0);
}

static void leader_entry(void)
{
    int id;
    int seen = 0;
    uint64 sp = (uint64)&id;

    id = identify_stack(leader_stacks, LEADER_THREADS, sp);
    if (id < 0)
        thread_exit(2);

    signal_ready();
    while (!atomic_load_int(&start_flag))
        cpu_yield();

    while (!atomic_load_int(&stop_flag)) {
        int round;

        while (!atomic_load_int(&stop_flag) &&
               atomic_load_int(&leader_cmd) <= seen)
            futex_wait(&leader_cmd, seen);
        if (atomic_load_int(&stop_flag))
            break;

        round = atomic_load_int(&leader_cmd);
        if (round <= seen || round > MAX_ROUNDS)
            continue;
        seen = round;

        for (int i = id; i < worker_count; i += LEADER_THREADS)
            wake_worker(i, round);

        atomic_store_int(&leader_done[id], round);
        if (atomic_add_int(&leaders_done, 1) >= LEADER_THREADS)
            futex_wake(&leaders_done, 1);
        burn_until(now_ns() + 2000000ULL, 4);
    }

    thread_exit(0);
}

static void gui_reactor_entry(void)
{
    int id;
    int seen = 0;
    char byte = 0;
    uint64 sp = (uint64)&id;
    int leaves_per_wake;

    id = identify_stack(gui_reactor_stacks, gui_reactor_count, sp);
    if (id < 0)
        thread_exit(2);

    leaves_per_wake = clamp_int((gui_target_runnable + 11) / 12, 2, 4);

    signal_ready();
    while (!atomic_load_int(&start_flag))
        cpu_yield();

    while (!atomic_load_int(&stop_flag)) {
        uint64 started;
        uint64 run;
        uint64 base;

        if (id & 1) {
            if (read(gui_reactor_pipefd[id][0], &byte, 1) != 1) {
                cpu_yield();
                continue;
            }
            if (atomic_load_int(&stop_flag) || byte == 0)
                break;
        } else {
            while (!atomic_load_int(&stop_flag) &&
                   atomic_load_int(&gui_reactor_seq[id]) <= seen)
                futex_wait(&gui_reactor_seq[id], seen);
            if (atomic_load_int(&stop_flag))
                break;
            seen = atomic_load_int(&gui_reactor_seq[id]);
        }

        started = now_ns();
        record_gui_stall(gui_reactor_wake_stamp_ns[id], started);
        run = atomic_fetch_add_u64(&gui_reactor_runs, 1) + 1;
        burn_for_us(100 + (int)((run * 29ULL + (uint64)id * 47ULL) % 301ULL),
                    5);

        base = atomic_fetch_add_u64(&gui_leaf_cursor, (uint64)leaves_per_wake);
        for (int i = 0; i < leaves_per_wake && !atomic_load_int(&stop_flag);
             i++) {
            int leaf = (int)((base + (uint64)i * 17ULL + (uint64)id * 7ULL) %
                             (uint64)worker_count);

            wake_gui_leaf(leaf);
        }
    }

    atomic_store_int(&gui_reactor_done[id], 1);
    thread_exit(0);
}

static void gui_producer_entry(void)
{
    int id;
    int seen = 0;
    uint64 sp = (uint64)&id;
    int reactors_per_burst;

    id = identify_stack(gui_producer_stacks, GUI_PRODUCER_THREADS, sp);
    if (id < 0)
        thread_exit(2);

    reactors_per_burst = clamp_int((gui_target_runnable + 7) / 8, 2, 4);

    signal_ready();
    while (!atomic_load_int(&start_flag))
        cpu_yield();

    while (!atomic_load_int(&stop_flag)) {
        int round;
        uint64 burst = 0;

        while (!atomic_load_int(&stop_flag) &&
               atomic_load_int(&gui_round_cmd) <= seen)
            futex_wait(&gui_round_cmd, seen);
        if (atomic_load_int(&stop_flag))
            break;

        round = atomic_load_int(&gui_round_cmd);
        if (round <= seen || round > MAX_ROUNDS)
            continue;
        seen = round;

        while (!atomic_load_int(&stop_flag) &&
               now_ns() < atomic_load_u64(&gui_round_end_ns)) {
            uint64 issue_start = now_ns();
            int base = (int)(((uint64)round + burst * 5ULL +
                              (uint64)id * 3ULL) %
                             (uint64)gui_reactor_count);

            for (int i = 0; i < reactors_per_burst; i++) {
                int reactor = (base + i * 3) % gui_reactor_count;

                wake_gui_reactor(reactor);
            }
            atomic_max_u64(&gui_max_issue_ns, now_ns() - issue_start);
            burst++;
            sleep_ms(1 + (int)((burst + (uint64)id + (uint64)round) & 1ULL));
        }

        if (atomic_add_int(&gui_round_done, 1) >= GUI_PRODUCER_THREADS)
            futex_wake(&gui_round_done, 1);
    }

    atomic_store_int(&gui_producer_done[id], 1);
    thread_exit(0);
}

static void churn_entry(void)
{
    int id;
    int seen = 0;
    uint64 sp = (uint64)&id;

    id = identify_stack(churn_stacks, churn_count, sp);
    if (id < 0)
        thread_exit(2);

    signal_ready();
    while (!atomic_load_int(&start_flag))
        cpu_yield();

    while (!atomic_load_int(&stop_flag)) {
        int round;

        while (!atomic_load_int(&stop_flag) &&
               atomic_load_int(&anchor_cmd) <= seen)
            futex_wait(&anchor_cmd, seen);
        if (atomic_load_int(&stop_flag))
            break;

        round = atomic_load_int(&anchor_cmd);
        if (round <= seen)
            continue;
        seen = round;
        atomic_add_int(&anchor_runs[id], 1);
        burn_until(now_ns() + (uint64)ANCHOR_MS * 1000000ULL, 4);
        sleep_ms(1);
    }

    thread_exit(0);
}

static void gui_churn_entry(void)
{
    int id;
    uint64 runs = 0;
    uint64 sp = (uint64)&id;

    id = identify_stack(churn_stacks, churn_count, sp);
    if (id < 0)
        thread_exit(2);

    signal_ready();
    while (!atomic_load_int(&start_flag))
        cpu_yield();

    while (!atomic_load_int(&stop_flag)) {
        sleep_ms(1 + (int)(((uint64)id * 3ULL + runs) % 8ULL));
        if (atomic_load_int(&stop_flag))
            break;
        runs++;
        atomic_inc_u64(&gui_idle_churn_runs);
        burn_for_us(20 + (int)(((uint64)id * 11ULL + runs * 13ULL) % 61ULL),
                    6);
    }

    atomic_store_int(&gui_churn_done[id], 1);
    thread_exit(0);
}

#ifdef HOST_LIBC_PROGRAM
static int clone_trampoline(void *arg)
{
    void (*entry)(void) = arg;

    entry();
    thread_exit(0);
    return 0;
}
#endif

static int create_thread(void (*entry)(void), char *stack)
{
#ifdef HOST_LIBC_PROGRAM
    int flags = CLONE_VM | CLONE_THREAD | CLONE_SIGHAND | CLONE_FILES |
                CLONE_FS | SIGCHLD;

    return clone(clone_trampoline, stack + STACK_SIZE, flags, entry);
#else
    struct clone_args args = {
        .flags = CLONE_VM | CLONE_THREAD | CLONE_SIGHAND | CLONE_FILES |
                 CLONE_FS | SIGCHLD,
        .stack = (uint64)stack,
        .stack_size = STACK_SIZE,
        .entry = (uint64)entry,
    };

    return clone(&args);
#endif
}

static int wait_until_at_least(volatile int *word, int target, int timeout_ms)
{
    uint64 deadline = now_ns() + (uint64)timeout_ms * 1000000ULL;
    int spins = 0;

    /* Control waits must respect their timeout even if a futex wake is missed. */
    while (atomic_load_int(word) < target) {
        if (now_ns() >= deadline)
            return -1;
        if ((spins++ & 63) == 0)
            sleep_ms(1);
        else
            cpu_yield();
    }
    return 0;
}

static void stop_all_threads(void)
{
    atomic_store_int(&stop_flag, 1);
    futex_wake(&leader_cmd, LEADER_THREADS);
    futex_wake(&anchor_cmd, churn_count);
    futex_wake(&quiet_count, worker_count);
    for (int i = 0; i < worker_count; i++) {
        if (i & 1) {
            char zero = 0;
            if (write(pipefd[i][1], &zero, 1) < 0) {
                /* Best-effort shutdown wake; exit_group bounds cleanup. */
            }
        } else {
            atomic_store_int(&worker_seq[i], round_count + 1);
            futex_wake(&worker_seq[i], 1);
        }
    }
}

static void stop_gui_threads(void)
{
    atomic_store_int(&stop_flag, 1);
    futex_wake(&gui_round_cmd, GUI_PRODUCER_THREADS);
    futex_wake(&gui_round_done, GUI_PRODUCER_THREADS);
    for (int i = 0; i < gui_reactor_count; i++) {
        if (i & 1) {
            char zero = 0;

            if (gui_reactor_pipefd[i][1] >= 0 &&
                write(gui_reactor_pipefd[i][1], &zero, 1) < 0) {
                /* Best-effort shutdown wake; exit_group bounds cleanup. */
            }
        } else {
            atomic_add_int(&gui_reactor_seq[i], 1);
            futex_wake(&gui_reactor_seq[i], 1);
        }
    }
    for (int i = 0; i < worker_count; i++) {
        if (i & 1) {
            char zero = 0;

            if (pipefd[i][1] >= 0 && write(pipefd[i][1], &zero, 1) < 0) {
                /* Best-effort shutdown wake; exit_group bounds cleanup. */
            }
        } else {
            atomic_add_int(&worker_seq[i], 1);
            futex_wake(&worker_seq[i], 1);
        }
    }
}

static int run_guihd(int argc, char **argv, int arg, const char *mode)
{
    struct kstats before;
    struct kstats after;
    int expected_ready;
    int completed_rounds = 0;
    int failed = 0;
    int unfinished = 0;
    int created_leaves = 0;
    int created_reactors = 0;
    int created_producers = 0;
    int created_churn = 0;
    uint64 probe_delta = 0;
    uint64 idle_needs_delta = 0;
    uint64 p99_ns;
    uint64 sample_count;
    int rc;
    const char *status;

    worker_count = parse_positive(argc > arg ? argv[arg] : 0,
                                  DEFAULT_GUI_LEAVES, MAX_WORKERS);
    round_count = parse_positive(argc > arg + 1 ? argv[arg + 1] : 0,
                                 DEFAULT_GUI_ROUNDS, MAX_ROUNDS);
    gui_window_ms = parse_positive(argc > arg + 2 ? argv[arg + 2] : 0,
                                   DEFAULT_GUI_WINDOW_MS, MAX_GUI_WINDOW_MS);
    gui_target_runnable = parse_positive(argc > arg + 3 ? argv[arg + 3] : 0,
                                         DEFAULT_TARGET_RUNNABLE,
                                         MAX_TARGET_RUNNABLE);
    if (worker_count < MIN_WORKERS)
        worker_count = MIN_WORKERS;
    gui_reactor_count = DEFAULT_GUI_REACTORS;
    for (int i = 0; i < MAX_WORKERS; i++) {
        pipefd[i][0] = -1;
        pipefd[i][1] = -1;
    }
    for (int i = 0; i < MAX_GUI_REACTORS; i++) {
        gui_reactor_pipefd[i][0] = -1;
        gui_reactor_pipefd[i][1] = -1;
    }

    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    if (kstats_full(&before) < 0) {
        printf("wakestorm: kstats2 before failed\n");
        kstats_ok = 0;
    }
    churn_count = before.ncpus > 0 ? before.ncpus * 4 : DEFAULT_CHURN_THREADS;
    if (churn_count < DEFAULT_CHURN_THREADS)
        churn_count = DEFAULT_CHURN_THREADS;
    if (churn_count > MAX_CHURN_THREADS)
        churn_count = MAX_CHURN_THREADS;

    for (int i = 0; i < worker_count; i++) {
        pipefd[i][0] = -1;
        pipefd[i][1] = -1;
        if ((i & 1) && pipe(pipefd[i]) < 0) {
            printf("wakestorm: pipe failed leaf=%d\n", i);
            failed = 1;
            goto out_stop;
        }
    }
    for (int i = 0; i < gui_reactor_count; i++) {
        gui_reactor_pipefd[i][0] = -1;
        gui_reactor_pipefd[i][1] = -1;
        if ((i & 1) && pipe(gui_reactor_pipefd[i]) < 0) {
            printf("wakestorm: pipe failed reactor=%d\n", i);
            failed = 1;
            goto out_stop;
        }
    }

    for (int i = 0; i < worker_count; i++) {
        int tid = create_thread(gui_leaf_entry, worker_stacks[i]);

        if (tid < 0) {
            printf("wakestorm: clone leaf failed i=%d ret=%d\n", i, tid);
            failed = 1;
            goto out_stop;
        }
        created_leaves++;
    }
    for (int i = 0; i < gui_reactor_count; i++) {
        int tid = create_thread(gui_reactor_entry, gui_reactor_stacks[i]);

        if (tid < 0) {
            printf("wakestorm: clone reactor failed i=%d ret=%d\n", i, tid);
            failed = 1;
            goto out_stop;
        }
        created_reactors++;
    }
    for (int i = 0; i < GUI_PRODUCER_THREADS; i++) {
        int tid = create_thread(gui_producer_entry, gui_producer_stacks[i]);

        if (tid < 0) {
            printf("wakestorm: clone producer failed i=%d ret=%d\n", i, tid);
            failed = 1;
            goto out_stop;
        }
        created_producers++;
    }
    for (int i = 0; i < churn_count; i++) {
        int tid = create_thread(gui_churn_entry, churn_stacks[i]);

        if (tid < 0) {
            printf("wakestorm: clone churn failed i=%d ret=%d\n", i, tid);
            failed = 1;
            goto out_stop;
        }
        created_churn++;
    }

    expected_ready = worker_count + gui_reactor_count + GUI_PRODUCER_THREADS +
                     churn_count;
    if (wait_until_at_least(&ready_count, expected_ready, 12000) < 0) {
        printf("wakestorm: ready timeout ready=%d expected=%d\n",
               atomic_load_int(&ready_count), expected_ready);
        failed = 1;
        goto out_stop;
    }

    atomic_store_int(&start_flag, 1);
    sleep_ms(120);

    for (int round = 1; round <= round_count; round++) {
        uint64 deadline = now_ns() + (uint64)gui_window_ms * 1000000ULL;

        __atomic_store_n(&gui_round_end_ns, deadline, __ATOMIC_SEQ_CST);
        atomic_store_int(&gui_round_done, 0);
        atomic_store_int(&gui_round_cmd, round);
        futex_wake(&gui_round_cmd, GUI_PRODUCER_THREADS);

        if (wait_until_at_least(&gui_round_done, GUI_PRODUCER_THREADS,
                                gui_window_ms + 2500) < 0) {
            printf("wakestorm: producer timeout round=%d done=%d\n", round,
                   atomic_load_int(&gui_round_done));
            failed = 1;
            break;
        }
        completed_rounds = round;
        sleep_ms(15);
    }

out_stop:
    stop_gui_threads();

    uint64 stop_deadline = now_ns() + (uint64)STOP_WAIT_MS * 1000000ULL;
    while (now_ns() < stop_deadline) {
        int done = 0;
        int expected = created_leaves + created_reactors + created_producers +
                       created_churn;

        for (int i = 0; i < created_leaves; i++)
            done += atomic_load_int(&worker_done[i]) ? 1 : 0;
        for (int i = 0; i < created_reactors; i++)
            done += atomic_load_int(&gui_reactor_done[i]) ? 1 : 0;
        for (int i = 0; i < created_producers; i++)
            done += atomic_load_int(&gui_producer_done[i]) ? 1 : 0;
        for (int i = 0; i < created_churn; i++)
            done += atomic_load_int(&gui_churn_done[i]) ? 1 : 0;
        if (done == expected)
            break;
        cpu_yield();
    }

    for (int i = 0; i < created_leaves; i++)
        unfinished += atomic_load_int(&worker_done[i]) ? 0 : 1;
    for (int i = 0; i < created_reactors; i++)
        unfinished += atomic_load_int(&gui_reactor_done[i]) ? 0 : 1;
    for (int i = 0; i < created_producers; i++)
        unfinished += atomic_load_int(&gui_producer_done[i]) ? 0 : 1;
    for (int i = 0; i < created_churn; i++)
        unfinished += atomic_load_int(&gui_churn_done[i]) ? 0 : 1;

    if (kstats_full(&after) < 0) {
        printf("wakestorm: kstats2 after failed\n");
        kstats_ok = 0;
    }
    if (kstats_ok) {
        probe_delta = after.sched_starve_probe_snapshots -
                      before.sched_starve_probe_snapshots;
        idle_needs_delta =
            after.sched_starve_probe_idle_needs_resched_samples -
            before.sched_starve_probe_idle_needs_resched_samples;
    }

    sample_count = atomic_load_u64(&gui_stall_sample_count);
    p99_ns = select_p99_ns(sample_count);

    if (failed || unfinished || completed_rounds != round_count || !kstats_ok) {
        status = "FAIL";
        rc = 1;
    } else if (probe_delta > 0) {
        status = "SIGNAL";
        rc = 2;
    } else {
        status = "PASS";
        rc = 0;
    }

    printf("wakestorm: mode=%s workers=%d reactors=%d leaves=%d churn=%d "
           "rounds=%d window_ms=%d target_runnable=%d "
           "max_wake_to_run_us=%lu p99_wake_to_run_us=%lu max_issue_us=%lu "
           "futex_wakes=%lu pipe_wakes=%lu reactor_runs=%lu leaf_runs=%lu "
           "idle_churn_runs=%lu probe_snapshots_delta=%lu "
           "idle_needs_resched_delta=%lu completed_rounds=%d "
           "unfinished_threads=%d status=%s\n",
           mode, worker_count, gui_reactor_count, worker_count, churn_count,
           round_count, gui_window_ms, gui_target_runnable,
           atomic_load_u64(&gui_max_wake_to_run_ns) / 1000, p99_ns / 1000,
           atomic_load_u64(&gui_max_issue_ns) / 1000,
           atomic_load_u64(&futex_wake_calls), atomic_load_u64(&pipe_wake_calls),
           atomic_load_u64(&gui_reactor_runs), atomic_load_u64(&gui_leaf_runs),
           atomic_load_u64(&gui_idle_churn_runs), probe_delta,
           idle_needs_delta, completed_rounds, unfinished, status);

    process_exit(rc);
    return rc;
}

int main(int argc, char **argv)
{
    struct kstats before;
    struct kstats after;
    const char *mode = MODE_HD;
    int arg = 1;
    int expected_ready;
    int completed_rounds = 0;
    int failed = 0;
    int unfinished = 0;
    uint64 max_ns = 0;
    uint64 max_issue_ns = 0;
    uint64 worker_run_total = 0;
    uint64 expected_worker_runs;
    uint64 probe_delta = 0;
    uint64 idle_needs_delta = 0;
    int rc;
    const char *status;

    if (argc > 1 && !positive_arg(argv[1])) {
        mode = argv[1];
        arg = 2;
    }
    if (strcmp(mode, MODE_GUIHD) == 0)
        return run_guihd(argc, argv, arg, mode);
    if (strcmp(mode, MODE_HD) != 0) {
        printf("wakestorm: unknown mode=%s\n", mode);
        return 1;
    }

    worker_count = parse_positive(argc > arg ? argv[arg] : 0,
                                  DEFAULT_WORKERS, MAX_WORKERS);
    round_count = parse_positive(argc > arg + 1 ? argv[arg + 1] : 0,
                                 DEFAULT_ROUNDS, HD_MAX_ROUNDS);
    active_ms = parse_positive(argc > arg + 2 ? argv[arg + 2] : 0,
                               DEFAULT_ACTIVE_MS, MAX_ACTIVE_MS);
    if (worker_count < MIN_WORKERS)
        worker_count = MIN_WORKERS;

    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    if (kstats_full(&before) < 0) {
        printf("wakestorm: kstats2 before failed\n");
        kstats_ok = 0;
    }
    churn_count = before.ncpus > 0 ? before.ncpus * 4 : DEFAULT_CHURN_THREADS;
    if (churn_count < DEFAULT_CHURN_THREADS)
        churn_count = DEFAULT_CHURN_THREADS;
    if (churn_count > MAX_CHURN_THREADS)
        churn_count = MAX_CHURN_THREADS;

    for (int i = 0; i < worker_count; i++) {
        pipefd[i][0] = -1;
        pipefd[i][1] = -1;
        if ((i & 1) && pipe(pipefd[i]) < 0) {
            printf("wakestorm: pipe failed worker=%d\n", i);
            return 1;
        }
    }

    for (int i = 0; i < worker_count; i++) {
        int tid = create_thread(worker_entry, worker_stacks[i]);
        if (tid < 0) {
            printf("wakestorm: clone worker failed i=%d ret=%d\n", i, tid);
            return 1;
        }
    }
    for (int i = 0; i < LEADER_THREADS; i++) {
        int tid = create_thread(leader_entry, leader_stacks[i]);
        if (tid < 0) {
            printf("wakestorm: clone leader failed i=%d ret=%d\n", i, tid);
            return 1;
        }
    }
    for (int i = 0; i < churn_count; i++) {
        int tid = create_thread(churn_entry, churn_stacks[i]);
        if (tid < 0) {
            printf("wakestorm: clone churn failed i=%d ret=%d\n", i, tid);
            return 1;
        }
    }

    expected_ready = worker_count + LEADER_THREADS + churn_count;
    if (wait_until_at_least(&ready_count, expected_ready, 8000) < 0) {
        printf("wakestorm: ready timeout ready=%d expected=%d\n",
               atomic_load_int(&ready_count), expected_ready);
        failed = 1;
        goto out_stop;
    }

    atomic_store_int(&start_flag, 1);
    sleep_ms(120);

    for (int round = 1; round <= round_count; round++) {
        int quiet_base = atomic_load_int(&quiet_count);
        uint64 wave_start;

        atomic_store_int(&leaders_done, 0);
        for (int i = 0; i < LEADER_THREADS; i++)
            atomic_store_int(&leader_done[i], 0);

        atomic_store_int(&anchor_cmd, round);
        futex_wake(&anchor_cmd, churn_count);
        sleep_ms(12);

        wave_start = now_ns();
        burst_end_ns[round] =
            wave_start + (uint64)active_ms * 1000000ULL;
        atomic_store_int(&leader_cmd, round);
        futex_wake(&leader_cmd, LEADER_THREADS);

        if (wait_until_at_least(&leaders_done, LEADER_THREADS,
                                LEADER_WAIT_MS) < 0) {
            printf("wakestorm: leader timeout round=%d done=%d\n", round,
                   atomic_load_int(&leaders_done));
            failed = 1;
            break;
        }
        leader_issue_ns[round] = now_ns() - wave_start;
        if (leader_issue_ns[round] > max_issue_ns)
            max_issue_ns = leader_issue_ns[round];

        if (wait_until_at_least(&quiet_count, quiet_base + worker_count,
                                active_ms + QUIET_GRACE_MS) < 0) {
            printf("wakestorm: quiet timeout round=%d quiet=%d target=%d\n",
                   round, atomic_load_int(&quiet_count),
                   quiet_base + worker_count);
            failed = 1;
            break;
        }
        completed_rounds = round;
        sleep_ms(60);
    }

out_stop:
    stop_all_threads();

    uint64 stop_deadline = now_ns() + (uint64)STOP_WAIT_MS * 1000000ULL;
    while (now_ns() < stop_deadline) {
        int done = 0;

        for (int i = 0; i < worker_count; i++)
            done += atomic_load_int(&worker_done[i]) ? 1 : 0;
        if (done == worker_count)
            break;
        cpu_yield();
    }

    for (int i = 0; i < worker_count; i++) {
        int runs = atomic_load_int(&worker_runs[i]);

        worker_run_total += (uint64)runs;
        if (runs < completed_rounds || !atomic_load_int(&worker_done[i]))
            unfinished++;
        if (max_stall_ns[i] > max_ns)
            max_ns = max_stall_ns[i];
    }

    if (kstats_full(&after) < 0) {
        printf("wakestorm: kstats2 after failed\n");
        kstats_ok = 0;
    }
    if (kstats_ok) {
        probe_delta = after.sched_starve_probe_snapshots -
                      before.sched_starve_probe_snapshots;
        idle_needs_delta =
            after.sched_starve_probe_idle_needs_resched_samples -
            before.sched_starve_probe_idle_needs_resched_samples;
    }

    expected_worker_runs = (uint64)worker_count * (uint64)round_count;
    if (failed || unfinished || completed_rounds != round_count || !kstats_ok) {
        status = "FAIL";
        rc = 1;
    } else if (probe_delta > 0) {
        status = "SIGNAL";
        rc = 2;
    } else {
        status = "PASS";
        rc = 0;
    }

    printf("wakestorm: mode=%s workers=%d churn=%d leaders=%d rounds=%d "
           "bursts=%d active_ms=%d max_wake_to_run_us=%lu "
           "max_wave_issue_us=%lu worker_runs=%lu expected_worker_runs=%lu "
           "futex_wakes=%lu pipe_wakes=%lu spin_iterations=%lu "
           "probe_snapshots_delta=%lu idle_needs_resched_delta=%lu "
           "unfinished_workers=%d completed_rounds=%d status=%s\n",
           mode, worker_count, churn_count, LEADER_THREADS, round_count,
           round_count, active_ms, max_ns / 1000, max_issue_ns / 1000,
           worker_run_total, expected_worker_runs,
           atomic_load_u64(&futex_wake_calls), atomic_load_u64(&pipe_wake_calls),
           atomic_load_u64(&spin_iterations), probe_delta, idle_needs_delta,
           unfinished, completed_rounds, status);

    process_exit(rc);
}

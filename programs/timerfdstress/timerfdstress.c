#include "kernel/inc/syscall.h"
#include "kernel/inc/types.h"
#include "kernel/inc/uabi/poll.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

#define CLOCK_MONOTONIC 1
#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC
#define EPOLLIN 0x001
#define EPOLL_CTL_ADD 1
#define EPOLL_CLOEXEC O_CLOEXEC
#define SIGTERM 15

#ifndef HOST_LIBC_PROGRAM
struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};
#endif

struct epoll_event_abi {
    uint32 events;
#if !defined(__x86_64__)
    uint32 __pad;
#endif
    uint64 data;
}
#if defined(__x86_64__)
__attribute__((packed))
#endif
;

#if defined(__riscv)
static inline int64 raw_syscall1(int num, int64 a)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_syscall2(int num, int64 a, int64 b)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_syscall4(int num, int64 a, int64 b, int64 c, int64 d)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    register int64 a3 asm("a3") = d;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_syscall6(int num, int64 a, int64 b, int64 c,
                                 int64 d, int64 e, int64 f)
{
    register int64 a7 asm("a7") = num;
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
static inline int64 raw_syscall1(int num, int64 a)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_syscall2(int num, int64 a, int64 b)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_syscall4(int num, int64 a, int64 b, int64 c, int64 d)
{
    int64 ret;
    register int64 r10 asm("r10") = d;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_syscall6(int num, int64 a, int64 b, int64 c,
                                 int64 d, int64 e, int64 f)
{
    int64 ret;
    register int64 r10 asm("r10") = d;
    register int64 r8 asm("r8") = e;
    register int64 r9 asm("r9") = f;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}
#else
#error unsupported architecture
#endif

static int timerfd_create_raw(int clockid, int flags)
{
    return (int)raw_syscall2(SYS_timerfd_create, clockid, flags);
}

static int timerfd_settime_raw(int fd, const struct itimerspec *value)
{
    return (int)raw_syscall4(SYS_timerfd_settime, fd, 0, (int64)value, 0);
}

static int epoll_create1_raw(int flags)
{
    return (int)raw_syscall1(SYS_epoll_create1, flags);
}

static int epoll_ctl_raw(int epfd, int op, int fd, struct epoll_event_abi *ev)
{
    return (int)raw_syscall4(SYS_epoll_ctl, epfd, op, fd, (int64)ev);
}

static int epoll_pwait_raw(int epfd, struct epoll_event_abi *events,
                           int maxevents, int timeout)
{
    return (int)raw_syscall6(SYS_epoll_pwait, epfd, (int64)events,
                             maxevents, timeout, 0, 0);
}

static int clock_gettime_raw(struct timespec *ts)
{
    return (int)raw_syscall2(SYS_clock_gettime, CLOCK_MONOTONIC, (int64)ts);
}

static uint64 ns_now(void)
{
    struct timespec ts;
    if (clock_gettime_raw(&ts) < 0)
        return 0;
    return (uint64)ts.tv_sec * 1000000000ULL + (uint64)ts.tv_nsec;
}

static void busy_child(void)
{
    volatile uint64 x = 1;
    for (;;) {
        for (int i = 0; i < 200000; i++)
            x = x * 1103515245ULL + 12345ULL;
        sleep(0);
    }
}

int main(int argc, char **argv)
{
    int interval_ms = argc > 1 ? atoi(argv[1]) : 20;
    int iterations = argc > 2 ? atoi(argv[2]) : 200;
    int pressure = argc > 3 ? atoi(argv[3]) : 0;
    int children[32];
    int nchildren = pressure;
    if (nchildren > 32)
        nchildren = 32;

    printf("timerfdstress: interval_ms=%d iterations=%d pressure=%d\n",
           interval_ms, iterations, nchildren);

    for (int i = 0; i < nchildren; i++) {
        int pid = fork();
        if (pid == 0)
            busy_child();
        if (pid > 0)
            children[i] = pid;
        else
            children[i] = -1;
    }

    int fd = timerfd_create_raw(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    int epfd = epoll_create1_raw(EPOLL_CLOEXEC);
    if (fd < 0 || epfd < 0) {
        printf("timerfdstress: RESULT fail create fd=%d epfd=%d\n", fd, epfd);
        goto out_children;
    }

    struct epoll_event_abi ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data = 0x54464d52ULL;
    if (epoll_ctl_raw(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        printf("timerfdstress: RESULT fail epoll_ctl\n");
        goto out_fds;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = (int64)interval_ms * 1000000LL;
    its.it_interval.tv_nsec = (int64)interval_ms * 1000000LL;
    if (timerfd_settime_raw(fd, &its) < 0) {
        printf("timerfdstress: RESULT fail settime\n");
        goto out_fds;
    }

    uint64 start = ns_now();
    uint64 expirations_total = 0;
    uint64 max_gap_ns = 0;
    uint64 last = start;
    int waits = 0;
    int timeouts = 0;

    while (expirations_total < (uint64)iterations) {
        struct epoll_event_abi out;
        memset(&out, 0, sizeof(out));
        int rc = epoll_pwait_raw(epfd, &out, 1, 1000);
        uint64 now = ns_now();
        if (now > last && now - last > max_gap_ns)
            max_gap_ns = now - last;
        last = now;
        waits++;
        if (rc == 0) {
            timeouts++;
            printf("timerfdstress: timeout waits=%d total=%lu\n",
                   waits, expirations_total);
            continue;
        }
        if (rc < 0) {
            printf("timerfdstress: RESULT fail epoll rc=%d total=%lu\n",
                   rc, expirations_total);
            goto out_fds;
        }
        uint64 count = 0;
        int n = read(fd, &count, sizeof(count));
        if (n != (int)sizeof(count) || count == 0) {
            printf("timerfdstress: RESULT fail read n=%d count=%lu total=%lu\n",
                   n, count, expirations_total);
            goto out_fds;
        }
        expirations_total += count;
        if ((expirations_total % 50) == 0 || expirations_total >= (uint64)iterations)
            printf("timerfdstress: tick total=%lu waits=%d max_gap_ms=%lu\n",
                   expirations_total, waits,
                   (uint64)(max_gap_ns / 1000000ULL));
    }

    uint64 elapsed_ns = ns_now() - start;
    uint64 expected_ns = (uint64)iterations * (uint64)interval_ms * 1000000ULL;
    uint64 elapsed_ms = elapsed_ns / 1000000ULL;
    uint64 expected_ms = expected_ns / 1000000ULL;
    uint64 max_gap_ms = max_gap_ns / 1000000ULL;
    int pass = timeouts == 0 &&
        elapsed_ns + expected_ns / 2 >= expected_ns &&
        elapsed_ns <= expected_ns * 2 &&
        max_gap_ms <= (uint64)interval_ms * 20ULL;

    printf("timerfdstress: elapsed_ms=%lu expected_ms=%lu total=%lu waits=%d timeouts=%d max_gap_ms=%lu\n",
           elapsed_ms, expected_ms, expirations_total, waits, timeouts,
           max_gap_ms);
    printf("timerfdstress: RESULT %s\n", pass ? "pass" : "fail");

out_fds:
    if (fd >= 0)
        close(fd);
    if (epfd >= 0)
        close(epfd);
out_children:
    for (int i = 0; i < nchildren; i++) {
        if (children[i] > 0)
            kill(children[i], SIGTERM);
    }
    for (int i = 0; i < nchildren; i++) {
        int status;
        if (children[i] > 0)
            waitpid(children[i], &status, 0);
    }
    return 0;
}

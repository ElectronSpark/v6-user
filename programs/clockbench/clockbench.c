#ifdef HOST_LIBC_PROGRAM
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
typedef uint64_t uint64;
typedef int64_t int64;
#else
#include "kernel/inc/kstats.h"
#include "kernel/inc/syscall.h"
#include "kernel/inc/types.h"
#include "user/user.h"
#endif

#define CLOCK_MONOTONIC_ID 1

#ifdef HOST_LIBC_PROGRAM
static int64
raw_syscall0(int num)
{
    return (int64)syscall(num);
}

static int64
raw_syscall2(int num, int64 a, int64 b)
{
    return (int64)syscall(num, a, b);
}

#else
#if defined(__riscv)
static inline int64
raw_syscall0(int num)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0");
    asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int64
raw_syscall1(int num, int64 a)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int64
raw_syscall2(int num, int64 a, int64 b)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}
#elif defined(__x86_64__)
static inline int64
raw_syscall0(int num)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64
raw_syscall1(int num, int64 a)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64
raw_syscall2(int num, int64 a, int64 b)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b)
                 : "rcx", "r11", "memory");
    return ret;
}
#else
#error unsupported architecture
#endif

static int
xv6_kstats(struct kstats *ks)
{
    return (int)raw_syscall1(SYS_kstats, (int64)ks);
}
#endif

static int64
bench_clock_gettime(struct timespec *ts)
{
#ifdef HOST_LIBC_PROGRAM
    return raw_syscall2(SYS_clock_gettime, CLOCK_MONOTONIC, (int64)ts);
#else
    return raw_syscall2(SYS_clock_gettime, CLOCK_MONOTONIC_ID, (int64)ts);
#endif
}

static int64
bench_clock_getres(struct timespec *ts)
{
#ifdef HOST_LIBC_PROGRAM
    return raw_syscall2(SYS_clock_getres, CLOCK_MONOTONIC, (int64)ts);
#else
    return raw_syscall2(SYS_clock_getres, CLOCK_MONOTONIC_ID, (int64)ts);
#endif
}

static int64
bench_getpid(void)
{
#ifdef HOST_LIBC_PROGRAM
    return raw_syscall0(SYS_getpid);
#else
    return raw_syscall0(SYS_getpid);
#endif
}

static uint64
ns_from_ts(const struct timespec *ts)
{
    return (uint64)ts->tv_sec * 1000000000ULL + (uint64)ts->tv_nsec;
}

static uint64
now_ns(void)
{
    struct timespec ts;
    if (bench_clock_gettime(&ts) < 0)
        return 0;
    return ns_from_ts(&ts);
}

#ifndef HOST_LIBC_PROGRAM
static uint64
ticks_to_ms(uint64 ticks, uint64 freq)
{
    if (freq == 0)
        return 0;
    return (ticks * 1000ULL) / freq;
}

static void
print_kstats_delta(const struct kstats *before, const struct kstats *after)
{
    uint64 freq = after->timebase_freq;

    printf("clockbench kstats clock_gettime_calls=%lu\n",
           after->sys_clock_gettime_calls - before->sys_clock_gettime_calls);
    printf("clockbench kstats clock_gettime_ms=%lu\n",
           ticks_to_ms(after->sys_clock_gettime_ticks -
                       before->sys_clock_gettime_ticks, freq));
    printf("clockbench kstats monotonic_calls=%lu\n",
           after->sys_clock_gettime_monotonic_calls -
           before->sys_clock_gettime_monotonic_calls);
    printf("clockbench kstats monotonic_ms=%lu\n",
           ticks_to_ms(after->sys_clock_gettime_monotonic_ticks -
                       before->sys_clock_gettime_monotonic_ticks, freq));
    printf("clockbench kstats copyout_calls=%lu\n",
           after->vm_copyout_calls - before->vm_copyout_calls);
    printf("clockbench kstats copyout_ms=%lu\n",
           ticks_to_ms(after->vm_copyout_ticks - before->vm_copyout_ticks,
                       freq));
    printf("clockbench kstats copyout_fast_hits=%lu\n",
           after->vm_copyout_fast_hits - before->vm_copyout_fast_hits);
}
#endif

int
main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "clock";
    int loops = argc > 2 ? atoi(argv[2]) : 100000;
    if (loops <= 0)
        loops = 100000;

#ifndef HOST_LIBC_PROGRAM
    struct kstats before;
    struct kstats after;
    int have_kstats = xv6_kstats(&before) == 0;
#endif
    struct timespec ts;
    volatile uint64 sink = 0;
    uint64 start = now_ns();

    for (int i = 0; i < loops; i++) {
        if (strcmp(mode, "clock") == 0) {
            if (bench_clock_gettime(&ts) < 0) {
                printf("clockbench RESULT fail mode=clock iter=%d\n", i);
                return 1;
            }
            sink += (uint64)ts.tv_nsec;
        } else if (strcmp(mode, "getpid") == 0) {
            int64 pid = bench_getpid();
            if (pid < 0) {
                printf("clockbench RESULT fail mode=getpid iter=%d ret=%ld\n",
                       i, pid);
                return 1;
            }
            sink += (uint64)pid;
        } else if (strcmp(mode, "clockres") == 0) {
            if (bench_clock_getres(&ts) < 0) {
                printf("clockbench RESULT fail mode=clockres iter=%d\n", i);
                return 1;
            }
            sink += (uint64)ts.tv_nsec;
        } else {
            printf("usage: clockbench [clock|getpid|clockres] [loops]\n");
            return 1;
        }
    }

    uint64 elapsed = now_ns() - start;
    uint64 per_call = loops > 0 ? elapsed / (uint64)loops : 0;

    printf("clockbench RESULT pass mode=%s loops=%d elapsed_ns=%lu "
           "per_call_ns=%lu sink=%lu\n",
           mode, loops, elapsed, per_call, sink);

 #ifndef HOST_LIBC_PROGRAM
    if (have_kstats && xv6_kstats(&after) == 0)
        print_kstats_delta(&before, &after);
#endif

    return 0;
}

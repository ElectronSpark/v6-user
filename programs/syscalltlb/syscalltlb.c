/*
 * syscalltlb — measure per-syscall TLB amplification cost.
 *
 * Theory under test: the x86_64 syscall path reloads CR3 twice per
 * syscall with noflush=0 (usertrap_syscall entry -> kernel table,
 * usertrapret/userret exit -> user table), discarding every non-global
 * TLB entry.  The direct syscall cost is visible in kstats, but the
 * *hidden* cost is user code restarting with a cold TLB after every
 * syscall.  This probe quantifies that hidden cost:
 *
 *   sweep  = read 1 byte from each of N pages (TLB-warm after warmup)
 *   mode A = sweeps only                       (baseline, TLB stays warm)
 *   mode B = getpid() + sweep per iteration    (syscall flushes user TLB)
 *   mode C = getpid() only                     (pure syscall cost)
 *
 *   amplification per syscall = B/iter - A/iter - C/iter
 *
 * If amplification >> 0, each syscall taxes user execution with N TLB
 * refills, and syscall-heavy phases (Qt/KIO app startup, compositor
 * frame loops) pay far more than the visible syscall time.
 *
 * Dual-mode like clockbench: builds for xv6 natively, and for the host
 * with -DHOST_LIBC_PROGRAM as a Linux control.
 */

#ifdef HOST_LIBC_PROGRAM
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
#include "kernel/inc/syscall.h"
#include "kernel/inc/types.h"
#include "user/user.h"
#endif

#define MAX_PAGES 1024
#define PAGE_BYTES 4096

/* Static working set: MAX_PAGES pages in BSS. */
static unsigned char workset[MAX_PAGES * PAGE_BYTES]
    __attribute__((aligned(PAGE_BYTES)));

#ifdef HOST_LIBC_PROGRAM

static int64
raw_getpid(void)
{
    return (int64)syscall(SYS_getpid);
}

static uint64
now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64)ts.tv_sec * 1000000000ULL + (uint64)ts.tv_nsec;
}

#else /* xv6 native */

#if defined(__x86_64__)
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
raw_syscall2(int num, int64 a, int64 b)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b)
                 : "rcx", "r11", "memory");
    return ret;
}
#elif defined(__riscv)
static inline int64
raw_syscall0(int num)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0");
    asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
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
#else
#error unsupported architecture
#endif

#define CLOCK_MONOTONIC_ID 1

static int64
raw_getpid(void)
{
    return raw_syscall0(SYS_getpid);
}

static uint64
now_ns(void)
{
    struct timespec ts;
    if (raw_syscall2(SYS_clock_gettime, CLOCK_MONOTONIC_ID, (int64)&ts) < 0)
        return 0;
    return (uint64)ts.tv_sec * 1000000000ULL + (uint64)ts.tv_nsec;
}

#endif /* HOST_LIBC_PROGRAM */

static volatile uint64 g_sink;

static void
touch_pages(int npages)
{
    uint64 sum = 0;
    for (int p = 0; p < npages; p++)
        sum += workset[(uint64)p * PAGE_BYTES];
    g_sink += sum;
}

static void
run_case(int npages, int iters)
{
    /* Warm: fault in all pages and load the TLB. */
    for (int w = 0; w < 4; w++)
        touch_pages(npages);

    /* Mode C: pure syscall loop. */
    uint64 c0 = now_ns();
    for (int i = 0; i < iters; i++)
        g_sink += (uint64)raw_getpid();
    uint64 c1 = now_ns();
    uint64 c_ns = (c1 - c0) / (uint64)iters;

    /* Mode A: sweeps only (TLB stays warm). */
    touch_pages(npages);
    uint64 a0 = now_ns();
    for (int i = 0; i < iters; i++)
        touch_pages(npages);
    uint64 a1 = now_ns();
    uint64 a_ns = (a1 - a0) / (uint64)iters;

    /* Mode B: one syscall then a sweep, per iteration. */
    touch_pages(npages);
    uint64 b0 = now_ns();
    for (int i = 0; i < iters; i++) {
        g_sink += (uint64)raw_getpid();
        touch_pages(npages);
    }
    uint64 b1 = now_ns();
    uint64 b_ns = (b1 - b0) / (uint64)iters;

    uint64 amp = 0;
    if (b_ns > a_ns + c_ns)
        amp = b_ns - a_ns - c_ns;

    printf("syscalltlb RESULT npages=%d iters=%d "
           "sweep_ns=%lu getpid_ns=%lu syscall_plus_sweep_ns=%lu "
           "tlb_amplification_ns=%lu amp_per_page_ns=%lu\n",
           npages, iters,
           (unsigned long)a_ns, (unsigned long)c_ns, (unsigned long)b_ns,
           (unsigned long)amp,
           (unsigned long)(npages > 0 ? amp / (uint64)npages : 0));
}

int
main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 2000;
    if (iters <= 0)
        iters = 2000;

    /* Ensure the whole working set is resident before any timing. */
    memset(workset, 1, sizeof(workset));

    static const int cases[] = { 64, 256, 512, 1024 };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(cases[i], iters);

    printf("syscalltlb DONE sink=%lu\n", (unsigned long)g_sink);
#ifndef HOST_LIBC_PROGRAM
    exit(0);
#endif
    return 0;
}

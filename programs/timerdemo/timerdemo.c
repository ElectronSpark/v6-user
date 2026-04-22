/*
 * timerdemo.c — Display seconds elapsed according to CLOCK_MONOTONIC
 *               (timer/jiffies) and CLOCK_REALTIME (RTC-seeded TSC).
 *
 * Usage: timerdemo [seconds]    (default 30)
 */
#include "user/user.h"

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* clock_gettime is provided by musl libc but not declared in user.h */
int clock_gettime(int clockid, struct timespec *tp);

int main(int argc, char **argv)
{
    int duration = 30;
    if (argc > 1)
        duration = atoi(argv[1]);
    if (duration <= 0)
        duration = 30;

    /* Snapshot the initial values of both clocks */
    struct timespec mono0, real0;
    if (clock_gettime(CLOCK_MONOTONIC, &mono0) < 0) {
        fprintf(2, "timerdemo: clock_gettime(MONOTONIC) failed\n");
        exit(1);
    }
    if (clock_gettime(CLOCK_REALTIME, &real0) < 0) {
        fprintf(2, "timerdemo: clock_gettime(REALTIME) failed\n");
        exit(1);
    }

    printf("timerdemo: showing elapsed seconds for %d seconds\n", duration);
    printf("  RTC epoch at start : %d.%09d\n",
           (int)real0.tv_sec, (int)real0.tv_nsec);
    printf("  Monotonic at start : %d.%09d\n",
           (int)mono0.tv_sec, (int)mono0.tv_nsec);
    printf("\n");
    printf("  %8s  %14s  %14s  %10s\n",
           "tick", "monotonic(ms)", "realtime(ms)", "drift(ms)");
    printf("  %8s  %14s  %14s  %10s\n",
           "--------", "--------------", "--------------", "----------");

    for (int i = 0; i <= duration; i++) {
        struct timespec mono, real;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        clock_gettime(CLOCK_REALTIME, &real);

        /* Elapsed since start, in milliseconds */
        int64 mono_ms = (mono.tv_sec - mono0.tv_sec) * 1000
                      + (mono.tv_nsec - mono0.tv_nsec) / 1000000;
        int64 real_ms = (real.tv_sec - real0.tv_sec) * 1000
                      + (real.tv_nsec - real0.tv_nsec) / 1000000;
        int64 drift   = real_ms - mono_ms;

        printf("  %8d  %14d  %14d  %10d\n",
               i, (int)mono_ms, (int)real_ms, (int)drift);

        if (i < duration) {
            struct timespec req = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&req, 0);
        }
    }

    printf("\ntimerdemo: done.\n");
    exit(0);
}

#include "kernel/inc/types.h"
#ifdef HOST_LIBC_PROGRAM
#include <sched.h>
#else
#include "kernel/inc/clone_flags.h"
#endif
#include "user/user.h"

#define STACK_SIZE (4096 * 4)
#define GROUP_THREADS 4
#define SIMPLE_ROUNDS 256
#define GROUP_ROUNDS 96

static volatile int group_ready;

static void sleep_one_ms(void)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
    nanosleep(&ts, NULL);
}

static void group_worker(void)
{
    __atomic_add_fetch(&group_ready, 1, __ATOMIC_SEQ_CST);
    for (;;)
        sleep_one_ms();
}

#ifdef HOST_LIBC_PROGRAM
static int group_worker_trampoline(void *arg)
{
    (void)arg;
    group_worker();
    return 0;
}
#endif

static int create_group_thread(void)
{
    char *stack = sbrk(STACK_SIZE);
    if (stack == (char *)-1)
        return -1;

#ifdef HOST_LIBC_PROGRAM
    int flags = CLONE_VM | CLONE_THREAD | CLONE_SIGHAND | CLONE_FILES |
                CLONE_FS | SIGCHLD;
    return clone(group_worker_trampoline, stack + STACK_SIZE, flags, NULL);
#else
    struct clone_args args = {
        .flags = CLONE_VM | CLONE_THREAD | CLONE_SIGHAND | CLONE_FILES |
                 CLONE_FS | SIGCHLD,
        .stack = (uint64)stack,
        .stack_size = STACK_SIZE,
        .entry = (uint64)group_worker,
    };
    return clone(&args);
#endif
}

static void run_simple_round(int round)
{
    int pid = fork();
    if (pid < 0) {
        printf("waitreapstorm: FAIL simple fork round=%d ret=%d\n", round,
               pid);
        exit(1);
    }
    if (pid == 0) {
        if ((round & 3) == 0)
            sleep_one_ms();
        exit(round & 7);
    }

    int status = -1;
    int got = waitpid(pid, &status, 0);
    if (got != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != (round & 7)) {
        printf("waitreapstorm: FAIL simple wait round=%d child=%d got=%d status=0x%x\n",
               round, pid, got, status);
        exit(1);
    }
}

static void run_delayed_group_round(int round)
{
    int pid = fork();
    if (pid < 0) {
        printf("waitreapstorm: FAIL group fork round=%d ret=%d\n", round,
               pid);
        exit(1);
    }
    if (pid == 0) {
        group_ready = 0;
        for (int i = 0; i < GROUP_THREADS; i++) {
            int tid = create_group_thread();
            if (tid < 0) {
                printf("waitreapstorm: FAIL clone round=%d index=%d ret=%d\n",
                       round, i, tid);
                exit(31);
            }
        }
        while (__atomic_load_n(&group_ready, __ATOMIC_ACQUIRE) <
               GROUP_THREADS)
            sleep_one_ms();

        /*
         * The leader exits while sibling threads are still sleeping.  The
         * leader becomes a delayed zombie; only the last sibling makes it
         * reapable.  This is the Chromium utility-process wait4 shape.
         */
        exit(round & 7);
    }

    int status = -1;
    int got = waitpid(pid, &status, 0);
    if (got != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != (round & 7)) {
        printf("waitreapstorm: FAIL group wait round=%d child=%d got=%d status=0x%x\n",
               round, pid, got, status);
        exit(1);
    }
}

int main(void)
{
    printf("waitreapstorm: start simple=%d delayed_group=%d threads=%d\n",
           SIMPLE_ROUNDS, GROUP_ROUNDS, GROUP_THREADS);

    for (int round = 0; round < SIMPLE_ROUNDS; round++)
        run_simple_round(round);
    printf("waitreapstorm: simple PASS rounds=%d\n", SIMPLE_ROUNDS);

    for (int round = 0; round < GROUP_ROUNDS; round++)
        run_delayed_group_round(round);
    printf("waitreapstorm: delayed-group PASS rounds=%d threads=%d\n",
           GROUP_ROUNDS, GROUP_THREADS);
    printf("waitreapstorm: PASS\n");
    exit(0);
}

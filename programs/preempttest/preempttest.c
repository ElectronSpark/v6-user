#include "kernel/inc/types.h"
#include "kernel/inc/signo.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int pid = fork();
    if (pid < 0) {
        printf("preempttest: fork failed\n");
        exit(1);
    }

    if (pid == 0) {
        volatile uint64 sink = 0;
        for (;;) {
            sink++;
        }
    }

    /*
     * On a single-vCPU VM this parent can only wake and kill the child if
     * timer interrupts force the spinning child back into the scheduler.
     */
    sleep(200);

    if (kill(pid, SIGKILL) < 0) {
        printf("preempttest: kill failed\n");
        exit(1);
    }

    int status = 0;
    int waited = waitpid(pid, &status, 0);
    if (waited != pid) {
        printf("preempttest: waitpid failed got=%d want=%d\n", waited, pid);
        exit(1);
    }

    printf("preempttest: PASS timer preempted user spin\n");
    exit(0);
}

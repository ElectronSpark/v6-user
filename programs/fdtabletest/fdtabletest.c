#include "kernel/inc/types.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

#define TEST_OPEN_COUNT 80

static void
fail(const char *msg, int expected, int actual)
{
    printf("fdtabletest: %s (expected=%d actual=%d)\n", msg, expected,
           actual);
    exit(1);
}

static void
check_fd(const char *msg, int actual, int expected)
{
    if (actual != expected)
        fail(msg, expected, actual);
}

static void
close_checked(int fd)
{
    if (close(fd) < 0)
        fail("close failed", 0, -1);
}

int
main(void)
{
    int fds[TEST_OPEN_COUNT];
    int reopened[4];

    for (int i = 0; i < TEST_OPEN_COUNT; i++) {
        fds[i] = open("/dev/null", O_RDONLY);
        if (fds[i] < 0)
            fail("open failed", 0, fds[i]);
        check_fd("sequential open returned wrong fd", fds[i], i + 3);
    }

    close_checked(fds[7]);
    close_checked(fds[60]);
    close_checked(fds[61]);
    close_checked(fds[76]);
    fds[7] = -1;
    fds[60] = -1;
    fds[61] = -1;
    fds[76] = -1;

    reopened[0] = open("/dev/null", O_RDONLY);
    reopened[1] = open("/dev/null", O_RDONLY);
    reopened[2] = open("/dev/null", O_RDONLY);
    reopened[3] = open("/dev/null", O_RDONLY);

    check_fd("lowest free fd was not reused first", reopened[0], 10);
    check_fd("fd allocation skipped the next hole", reopened[1], 63);
    check_fd("fd allocation skipped the 64-boundary hole", reopened[2], 64);
    check_fd("fd allocation skipped a later free slot", reopened[3], 79);

    for (int i = 0; i < 4; i++)
        close_checked(reopened[i]);

    for (int i = 0; i < TEST_OPEN_COUNT; i++) {
        if (fds[i] >= 0)
            close_checked(fds[i]);
    }

    printf("fdtabletest: ok\n");
    exit(0);
}
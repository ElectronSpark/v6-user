#include "kernel/inc/syscall.h"
#include "kernel/inc/types.h"
#include "kernel/inc/signo.h"
#include "user/user.h"

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_CLOEXEC 0x80000

struct sockaddr_in {
    uint16 sin_family;
    uint16 sin_port;
    uint32 sin_addr;
    char sin_zero[8];
};

#if defined(__riscv)
static inline int64
raw_syscall1(int n, int64 a)
{
    register int64 a7 asm("a7") = n;
    register int64 a0 asm("a0") = a;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int64
raw_syscall2(int n, int64 a, int64 b)
{
    register int64 a7 asm("a7") = n;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline int64
raw_syscall3(int n, int64 a, int64 b, int64 c)
{
    register int64 a7 asm("a7") = n;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int64
raw_syscall6(int n, int64 a, int64 b, int64 c, int64 d, int64 e, int64 f)
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
static inline int64
raw_syscall1(int n, int64 a)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n), "D"(a)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64
raw_syscall2(int n, int64 a, int64 b)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n), "D"(a), "S"(b)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64
raw_syscall3(int n, int64 a, int64 b, int64 c)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64
raw_syscall6(int n, int64 a, int64 b, int64 c, int64 d, int64 e, int64 f)
{
    int64 ret;
    register int64 r10 asm("r10") = d;
    register int64 r8 asm("r8") = e;
    register int64 r9 asm("r9") = f;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n), "D"(a), "S"(b), "d"(c),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}
#endif

static uint16
bswap16(uint16 x)
{
    return (uint16)((x << 8) | (x >> 8));
}

static uint32
loopback_addr(void)
{
    return ((uint32)127) | ((uint32)1 << 24);
}

static int
socket_raw(int domain, int type, int protocol)
{
    return (int)raw_syscall3(SYS_socket, domain, type, protocol);
}

static int
bind_raw(int fd, const struct sockaddr_in *sa)
{
    return (int)raw_syscall3(SYS_bind, fd, (int64)sa, sizeof(*sa));
}

static int
listen_raw(int fd, int backlog)
{
    return (int)raw_syscall2(SYS_listen, fd, backlog);
}

static int
accept_raw(int fd)
{
    return (int)raw_syscall3(SYS_accept, fd, 0, 0);
}

static int
connect_raw(int fd, const struct sockaddr_in *sa)
{
    return (int)raw_syscall3(SYS_connect, fd, (int64)sa, sizeof(*sa));
}

static int
sendto_raw(int fd, const void *buf, int len, int flags)
{
    return (int)raw_syscall6(SYS_sendto, fd, (int64)buf, len, flags, 0, 0);
}

static int
read_raw(int fd, void *buf, int len)
{
    return (int)raw_syscall3(SYS_read, fd, (int64)buf, len);
}

static int
close_raw(int fd)
{
    return (int)raw_syscall1(SYS_close, fd);
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int listener = socket_raw(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        printf("blocksendwake: listener socket failed %d\n", listener);
        exit(1);
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = bswap16(19192);
    sa.sin_addr = loopback_addr();

    if (bind_raw(listener, &sa) < 0 || listen_raw(listener, 1) < 0) {
        printf("blocksendwake: bind/listen failed\n");
        exit(1);
    }

    int child = fork();
    if (child < 0) {
        printf("blocksendwake: fork failed\n");
        exit(1);
    }
    if (child == 0) {
        int cfd = accept_raw(listener);
        if (cfd < 0) {
            printf("blocksendwake: accept failed %d\n", cfd);
            exit(1);
        }
        sleep(1500);

        char rbuf[4096];
        uint64 total = 0;
        for (;;) {
            int n = read_raw(cfd, rbuf, sizeof(rbuf));
            if (n == 0)
                break;
            if (n < 0) {
                printf("blocksendwake: server read failed %d total=%lu\n",
                       n, total);
                exit(1);
            }
            total += (uint64)n;
            if (total >= 1024 * 1024)
                break;
        }
        printf("blocksendwake: server read total=%lu\n", total);
        close_raw(cfd);
        exit(total >= 1024 * 1024 ? 0 : 1);
    }

    int fd = socket_raw(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect_raw(fd, &sa) < 0) {
        printf("blocksendwake: client socket/connect failed fd=%d\n", fd);
        kill(child, SIGKILL);
        exit(1);
    }

    char buf[4096];
    memset(buf, 0x42, sizeof(buf));

    uint64 total = 0;
    while (total < 1024 * 1024) {
        int todo = sizeof(buf);
        if (total + (uint64)todo > 1024 * 1024)
            todo = (int)(1024 * 1024 - total);
        int n = sendto_raw(fd, buf, todo, 0);
        if (n <= 0) {
            printf("blocksendwake: send failed %d total=%lu\n", n, total);
            kill(child, SIGKILL);
            exit(1);
        }
        total += (uint64)n;
    }
    close_raw(fd);

    int status = 0;
    waitpid(child, &status, 0);
    if (status != 0) {
        printf("blocksendwake: server failed status=%d\n", status);
        exit(1);
    }

    printf("blocksendwake: PASS blocking TCP write woke total=%lu\n", total);
    exit(0);
}

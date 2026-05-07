#include "kernel/inc/syscall.h"
#include "kernel/inc/types.h"
#include "kernel/inc/uabi/poll.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000
#define SOL_SOCKET 1
#define SO_ERROR 4
#define MSG_DONTWAIT 0x40
#define EAGAIN 11
#define EINPROGRESS 115
#define LOCAL_TEST_PORT_BASE 18080

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct sockaddr_in {
    uint16 sin_family;
    uint16 sin_port;
    uint32 sin_addr;
    char sin_zero[8];
};

struct msghdr {
    void *msg_name;
    uint32 msg_namelen;
    struct iovec *msg_iov;
    uint64 msg_iovlen;
    void *msg_control;
    uint64 msg_controllen;
    int msg_flags;
};

#if defined(__riscv)
static inline int64 raw_syscall2(int num, int64 a, int64 b)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_syscall5(int num, int64 a, int64 b, int64 c, int64 d, int64 e)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    register int64 a3 asm("a3") = d;
    register int64 a4 asm("a4") = e;
    asm volatile("ecall" : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
                 : "memory");
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
static inline int64 raw_syscall2(int num, int64 a, int64 b)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_syscall5(int num, int64 a, int64 b, int64 c, int64 d, int64 e)
{
    int64 ret;
    register int64 r10 asm("r10") = d;
    register int64 r8 asm("r8") = e;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c),
                   "r"(r10), "r"(r8)
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
#endif

static uint16 bswap16(uint16 x)
{
    return (uint16)((x << 8) | (x >> 8));
}

static uint32 parse_ipv4(const char *s)
{
    int parts[4] = {0, 0, 0, 0};
    int idx = 0;
    int value = 0;

    for (int i = 0;; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (value > 255)
                return 0;
        } else if (c == '.' || c == 0) {
            if (idx >= 4)
                return 0;
            parts[idx++] = value;
            value = 0;
            if (c == 0)
                break;
        } else {
            return 0;
        }
    }
    if (idx != 4)
        return 0;

    return ((uint32)parts[0] << 0) |
           ((uint32)parts[1] << 8) |
           ((uint32)parts[2] << 16) |
           ((uint32)parts[3] << 24);
}

static int socket_raw(int domain, int type, int protocol)
{
    return (int)raw_syscall3(SYS_socket, domain, type, protocol);
}

static int connect_raw(int fd, const struct sockaddr_in *sa)
{
    return (int)raw_syscall3(SYS_connect, fd, (int64)sa, sizeof(*sa));
}

static int bind_raw(int fd, const struct sockaddr_in *sa)
{
    return (int)raw_syscall3(SYS_bind, fd, (int64)sa, sizeof(*sa));
}

static int listen_raw(int fd, int backlog)
{
    return (int)raw_syscall2(SYS_listen, fd, backlog);
}

static int accept_raw(int fd)
{
    return (int)raw_syscall3(SYS_accept, fd, 0, 0);
}

static int poll_raw(struct pollfd *fds, int nfds, int timeout)
{
    return (int)raw_syscall3(SYS_poll, (int64)fds, nfds, timeout);
}

static int getsockopt_raw(int fd, int level, int optname, void *optval, int *optlen)
{
    return (int)raw_syscall5(SYS_getsockopt, fd, level, optname,
                             (int64)optval, (int64)optlen);
}

static int sendto_raw(int fd, const void *buf, int len, int flags)
{
    return (int)raw_syscall6(SYS_sendto, fd, (int64)buf, len, flags, 0, 0);
}

static int recvfrom_raw(int fd, void *buf, int len, int flags)
{
    return (int)raw_syscall6(SYS_recvfrom, fd, (int64)buf, len, flags, 0, 0);
}

static int recvmsg_raw(int fd, struct msghdr *msg, int flags)
{
    return (int)raw_syscall3(SYS_recvmsg, fd, (int64)msg, flags);
}

static int connect_one(const char *ip, int port, int send_http)
{
    struct sockaddr_in sa;
    struct pollfd pfd;
    int fd;
    int rc;
    int so_error = 0;
    int optlen = sizeof(so_error);

    fd = socket_raw(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        printf("webkitnettest: socket failed\n");
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = bswap16((uint16)port);
    sa.sin_addr = parse_ipv4(ip);
    if (sa.sin_addr == 0) {
        printf("webkitnettest: invalid IPv4 %s\n", ip);
        close(fd);
        return -1;
    }

    rc = connect_raw(fd, &sa);
    if (rc < 0 && rc != -EINPROGRESS) {
        printf("webkitnettest: connect immediate failure %d\n", rc);
        close(fd);
        return -1;
    }

    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    rc = poll_raw(&pfd, 1, 5000);
    if (rc <= 0 || !(pfd.revents & (POLLOUT | POLLERR | POLLHUP))) {
        printf("webkitnettest: poll connect timeout/failure rc=%d revents=0x%x\n",
               rc, pfd.revents);
        close(fd);
        return -1;
    }

    if (getsockopt_raw(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) < 0) {
        printf("webkitnettest: getsockopt(SO_ERROR) failed\n");
        close(fd);
        return -1;
    }
    if (so_error != 0) {
        printf("webkitnettest: SO_ERROR=%d\n", so_error);
        close(fd);
        return -1;
    }

    if (send_http) {
        const char req[] = "GET / HTTP/1.0\r\nHost: example\r\n\r\n";
        char buf[128];
        int n = sendto_raw(fd, req, sizeof(req) - 1, MSG_DONTWAIT);
        if (n <= 0) {
            printf("webkitnettest: short send %d\n", n);
            close(fd);
            return -1;
        }
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll_raw(&pfd, 1, 5000) <= 0 || !(pfd.revents & (POLLIN | POLLHUP))) {
            printf("webkitnettest: response poll failed revents=0x%x\n", pfd.revents);
            close(fd);
            return -1;
        }
        n = recvfrom_raw(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0) {
            printf("webkitnettest: recv failed %d\n", n);
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

static int http_download_one(const char *ip, int port, const char *path,
                             const char *host, int use_recvmsg)
{
    struct sockaddr_in sa;
    struct pollfd pfd;
    char req[512];
    char buf[8192];
    int fd;
    int rc;
    int so_error = 0;
    int optlen = sizeof(so_error);
    int header_done = 0;
    int status = 0;
    uint64 total = 0;
    int start_tick;
    int end_tick;
    int idle_polls = 0;

    fd = socket_raw(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        printf("webkitnettest: download socket failed\n");
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = bswap16((uint16)port);
    sa.sin_addr = parse_ipv4(ip);
    if (sa.sin_addr == 0) {
        printf("webkitnettest: invalid IPv4 %s\n", ip);
        close(fd);
        return -1;
    }

    rc = connect_raw(fd, &sa);
    if (rc < 0 && rc != -EINPROGRESS) {
        printf("webkitnettest: download connect immediate failure %d\n", rc);
        close(fd);
        return -1;
    }

    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    rc = poll_raw(&pfd, 1, 10000);
    if (rc <= 0 || !(pfd.revents & (POLLOUT | POLLERR | POLLHUP))) {
        printf("webkitnettest: download connect poll failed rc=%d revents=0x%x\n",
               rc, pfd.revents);
        close(fd);
        return -1;
    }

    if (getsockopt_raw(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) < 0 ||
        so_error != 0) {
        printf("webkitnettest: download SO_ERROR=%d\n", so_error);
        close(fd);
        return -1;
    }

    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "User-Agent: xv6-webkitnettest/1\r\n"
             "\r\n",
             path, host);
    rc = sendto_raw(fd, req, strlen(req), 0);
    if (rc != strlen(req)) {
        printf("webkitnettest: download request short send %d want %d\n",
               rc, (int)strlen(req));
        close(fd);
        return -1;
    }

    start_tick = uptime();
    for (;;) {
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rc = poll_raw(&pfd, 1, 10000);
        if (rc == 0) {
            idle_polls++;
            printf("webkitnettest: download idle poll bytes=%lu ticks=%d\n",
                   total, uptime() - start_tick);
            if (idle_polls >= 3) {
                close(fd);
                return -1;
            }
            continue;
        }
        if (rc < 0) {
            printf("webkitnettest: download poll failed %d\n", rc);
            close(fd);
            return -1;
        }

        for (;;) {
            int n;
            if (use_recvmsg) {
                struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
                struct msghdr msg;
                memset(&msg, 0, sizeof(msg));
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                n = recvmsg_raw(fd, &msg, MSG_DONTWAIT);
            } else {
                n = recvfrom_raw(fd, buf, sizeof(buf), MSG_DONTWAIT);
            }
            if (n == 0)
                goto done;
            if (n == -EAGAIN)
                break;
            if (n < 0) {
                printf("webkitnettest: download recv failed %d\n", n);
                close(fd);
                return -1;
            }
            idle_polls = 0;
            if (!header_done) {
                for (int i = 0; i + 3 < n; i++) {
                    if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                        buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                        header_done = 1;
                        if (n >= 12 && buf[0] == 'H' && buf[9] >= '0' && buf[9] <= '9')
                            status = (buf[9] - '0') * 100 +
                                     (buf[10] - '0') * 10 +
                                     (buf[11] - '0');
                        total += n - (i + 4);
                        break;
                    }
                }
            } else {
                total += n;
            }
        }
    }

done:
    end_tick = uptime();
    close(fd);
    printf("webkitnettest: download api=%s status=%d bytes=%lu ticks=%d bytes_per_tick=%lu\n",
           use_recvmsg ? "recvmsg" : "recvfrom", status, total, end_tick - start_tick,
           (end_tick > start_tick) ? total / (uint64)(end_tick - start_tick) : total);
    return (status >= 200 && status < 300 && total > 0) ? 0 : -1;
}

static void run_local_server(int port, int accept_count)
{
    struct sockaddr_in sa;
    int fd = socket_raw(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const char reply[] =
        "HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";

    if (fd < 0)
        exit(2);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = bswap16((uint16)port);
    sa.sin_addr = 0;

    if (bind_raw(fd, &sa) < 0 || listen_raw(fd, accept_count) < 0) {
        close(fd);
        exit(3);
    }

    for (int i = 0; i < accept_count; i++) {
        char buf[128];
        int cfd = accept_raw(fd);
        if (cfd < 0) {
            close(fd);
            exit(4);
        }
        if (i == 0) {
            struct pollfd pfd;
            pfd.fd = cfd;
            pfd.events = POLLIN | POLLHUP;
            pfd.revents = 0;
            poll_raw(&pfd, 1, 3000);
        } else {
            sleep(1);
        }
        recvfrom_raw(cfd, buf, sizeof(buf), MSG_DONTWAIT);
        sendto_raw(cfd, reply, sizeof(reply) - 1, 0);
        close(cfd);
    }

    close(fd);
    exit(0);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && (strcmp(argv[1], "download") == 0 ||
                      strcmp(argv[1], "download-recvmsg") == 0)) {
        const char *ip = argc > 2 ? argv[2] : "10.0.2.2";
        int port = argc > 3 ? atoi(argv[3]) : 18081;
        const char *path = argc > 4 ? argv[4] : "/";
        const char *host = argc > 5 ? argv[5] : "10.0.2.2";
        int use_recvmsg = strcmp(argv[1], "download-recvmsg") == 0;

        printf("webkitnettest: download target %s:%d%s host=%s api=%s\n",
               ip, port, path, host, use_recvmsg ? "recvmsg" : "recvfrom");
        exit(http_download_one(ip, port, path, host, use_recvmsg) == 0 ? 0 : 1);
    }

    const char *ip = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : LOCAL_TEST_PORT_BASE + (getpid() % 1000);
    int parallel = argc > 3 ? atoi(argv[3]) : 4;
    int server_pid = -1;

    printf("webkitnettest: target %s:%d parallel=%d\n", ip, port, parallel);

    if (argc == 1) {
        server_pid = fork();
        if (server_pid < 0)
            exit(1);
        if (server_pid == 0)
            run_local_server(port, parallel + 1);
        sleep(1);
    }

    if (connect_one(ip, port, 1) < 0)
        exit(1);

    if (parallel < 1)
        parallel = 1;
    if (parallel > 8)
        parallel = 8;

    for (int i = 0; i < parallel; i++) {
        int pid = fork();
        if (pid < 0)
            exit(1);
        if (pid == 0)
            exit(connect_one(ip, port, 0) == 0 ? 0 : 1);
    }

    for (int i = 0; i < parallel; i++) {
        int status = 0;
        if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            exit(1);
    }

    if (server_pid > 0) {
        int status = 0;
        if (waitpid(server_pid, &status, 0) < 0 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            exit(1);
    }

    printf("webkitnettest: ok\n");
    exit(0);
}

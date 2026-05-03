#include "kernel/inc/syscall.h"
#include "kernel/inc/types.h"
#include "kernel/inc/uabi/poll.h"
#include "kernel/inc/uabi/statfs.h"
#include "kernel/inc/vfs/fcntl.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_PASSCRED 16
#define SO_PEERCRED 17
#define SO_PROTOCOL 38
#define SO_DOMAIN 39
#define SCM_RIGHTS 1
#define MSG_DONTWAIT 0x40
#define MSG_TRUNC 0x20
#define MSG_CMSG_CLOEXEC 0x40000000
#define MFD_CLOEXEC 0x0001
#define MLOCK_ONFAULT 0x01
#define MCL_CURRENT 0x01
#define MCL_FUTURE 0x02
#define MCL_ONFAULT 0x04
#define CLOCK_MONOTONIC 1
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu
#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC
#define EPOLLIN 0x001
#define EPOLLOUT 0x004
#define EPOLL_CTL_ADD 1
#define EPOLL_CLOEXEC O_CLOEXEC
#define LARGE_SHM_SIZE (1024 * 1024)
#define SHAREABLE_RESOURCE_SIZE (10 * 1024 * 1024)
#define WEBKIT_YT_RESOURCE_SIZE 9701939u
#define WEBKIT_YT_APP_JS_SIZE 9701895u
#define WEBKIT_YT_BASE_JS_SIZE 2461301u
#define WEBKIT_YT_BASE_JS_CORRUPT_OFFSET 114688u
#define WEBKIT_PAGE_CHUNK 4096u
#define IPC_STRESS_MAX_PAYLOAD 8192
#define IPC_STRESS_MESSAGES 2500
#define IPC_STRESS_MAGIC 0x574b4950u
#define WEBKIT_IPC_DATA_CHUNK 2048u
#if defined(__x86_64__)
#define SYS_memfd_create_native 319
#else
#define SYS_memfd_create_native 279
#endif
#define EAGAIN 11
#define EINVAL 22
#define ETIMEDOUT 110
#define EINPROGRESS 115

struct pollfd {
    int fd;
    short events;
    short revents;
};

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

struct sockaddr_un {
    uint16 sun_family;
    char sun_path[108];
};

struct cmsghdr {
    uint32 cmsg_len;
    uint32 __pad1;
    int cmsg_level;
    int cmsg_type;
};

struct msghdr {
    void *msg_name;
    uint32 msg_namelen;
    uint32 __pad0;
    struct iovec *msg_iov;
    int msg_iovlen;
    int __pad1;
    void *msg_control;
    uint32 msg_controllen;
    uint32 __pad2;
    int msg_flags;
};

struct mmsghdr {
    struct msghdr msg_hdr;
    uint32 msg_len;
    uint32 __pad;
};

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

struct ucred {
    int pid;
    uint32 uid;
    uint32 gid;
};

#define CMSG_ALIGN(n) (((n) + sizeof(uint64) - 1) & ~(sizeof(uint64) - 1))
#define CMSG_LEN(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_SPACE(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(n))
#define CMSG_DATA(cmsg) ((unsigned char *)((struct cmsghdr *)(cmsg) + 1))

static int passed;
static int failed;
static int skipped;

static void pass(const char *name)
{
    printf("  PASS: %s\n", name);
    passed++;
}

static void fail(const char *name, const char *why)
{
    printf("  FAIL: %s - %s\n", name, why);
    failed++;
}

static void skip(const char *name, const char *why)
{
    printf("  SKIP: %s - %s\n", name, why);
    skipped++;
}

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

static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
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

static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c)
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

static int socketpair_raw(int type, int sv[2])
{
    return (int)raw_syscall4(SYS_socketpair, AF_UNIX, type, 0, (int64)sv);
}

static int socket_raw(int domain, int type, int protocol)
{
    return (int)raw_syscall3(SYS_socket, domain, type, protocol);
}

static int bind_unix_raw(int fd, const struct sockaddr_un *sa)
{
    return (int)raw_syscall3(SYS_bind, fd, (int64)sa, sizeof(*sa));
}

static int listen_raw(int fd, int backlog)
{
    return (int)raw_syscall2(SYS_listen, fd, backlog);
}

static int accept4_raw(int fd, int flags)
{
    return (int)raw_syscall4(SYS_accept4, fd, 0, 0, flags);
}

static int connect_unix_raw(int fd, const struct sockaddr_un *sa)
{
    return (int)raw_syscall3(SYS_sconnect, fd, (int64)sa, sizeof(*sa));
}

static int sendmsg_raw(int fd, struct msghdr *msg, int flags)
{
    return (int)raw_syscall3(SYS_sendmsg, fd, (int64)msg, flags);
}

static int recvmsg_raw(int fd, struct msghdr *msg, int flags)
{
    return (int)raw_syscall3(SYS_recvmsg, fd, (int64)msg, flags);
}

static int recvmmsg_raw(int fd, struct mmsghdr *msgvec, int vlen, int flags)
{
    return (int)raw_syscall6(SYS_recvmmsg_time64, fd, (int64)msgvec, vlen,
                             flags, 0, 0);
}

static int poll_raw(struct pollfd *fds, int nfds, int timeout)
{
    return (int)raw_syscall3(SYS_poll, (int64)fds, nfds, timeout);
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

static int fsync_raw(int fd)
{
    return (int)raw_syscall2(SYS_fsync, fd, 0);
}

static int fdatasync_raw(int fd)
{
    return (int)raw_syscall2(SYS_fdatasync, fd, 0);
}

static int memfd_create_raw(const char *name, uint flags)
{
    return (int)raw_syscall2(SYS_memfd_create, (int64)name, flags);
}

static int memfd_create_native_raw(const char *name, uint flags)
{
    return (int)raw_syscall2(SYS_memfd_create_native, (int64)name, flags);
}

static int mlock2_raw(const void *addr, uint64 len, int flags)
{
    return (int)raw_syscall3(SYS_mlock2, (int64)addr, len, flags);
}

static int mlockall_raw(int flags)
{
    return (int)raw_syscall2(SYS_mlockall, flags, 0);
}

static int munlock_raw(const void *addr, uint64 len)
{
    return (int)raw_syscall2(SYS_munlock, (int64)addr, len);
}

static int munlockall_raw(void)
{
    return (int)raw_syscall2(SYS_munlockall, 0, 0);
}

static int timerfd_create_raw(int clockid, int flags)
{
    return (int)raw_syscall2(SYS_timerfd_create, clockid, flags);
}

static int clock_gettime_raw(int clockid, struct timespec *ts)
{
    return (int)raw_syscall2(SYS_clock_gettime, clockid, (int64)ts);
}

static int futex_raw(uint32 *addr, int op, uint32 val, const struct timespec *timeout,
                     uint32 *addr2, uint32 val3)
{
    return (int)raw_syscall6(SYS_futex, (int64)addr, op, val, (int64)timeout,
                             (int64)addr2, val3);
}

static int timerfd_settime_raw(int fd, int flags, const struct itimerspec *new_value,
                               struct itimerspec *old_value)
{
    return (int)raw_syscall4(SYS_timerfd_settime, fd, flags,
                             (int64)new_value, (int64)old_value);
}

static int statfs_raw(const char *path, struct statfs *st)
{
    return (int)raw_syscall2(SYS_statfs, (int64)path, (int64)st);
}

static int fstatfs_raw(int fd, struct statfs *st)
{
    return (int)raw_syscall2(SYS_fstatfs, fd, (int64)st);
}

static int fcntl_addr_raw(int fd, int cmd, void *arg)
{
    return (int)raw_syscall3(SYS_fcntl, fd, cmd, (int64)arg);
}

static int setsockopt_raw(int fd, int level, int optname, const void *optval, int optlen)
{
    return (int)raw_syscall6(SYS_setsockopt, fd, level, optname,
                             (int64)optval, optlen, 0);
}

static int getsockopt_raw(int fd, int level, int optname, void *optval, int *optlen)
{
    return (int)raw_syscall6(SYS_getsockopt, fd, level, optname,
                             (int64)optval, (int64)optlen, 0);
}

static int check_fd_closed_after_exec(int fd)
{
    char ch;
    if (read(fd, &ch, 1) >= 0) {
        fprintf(2, "webkitabitest: fd %d survived exec\n", fd);
        return -1;
    }
    return 0;
}

static int exec_check_closed(int fd)
{
    char fdstr[16];
    char *argv[4];

    snprintf(fdstr, sizeof(fdstr), "%d", fd);
    argv[0] = "webkitabitest";
    argv[1] = "checkclosed";
    argv[2] = fdstr;
    argv[3] = 0;
    exec("/bin/webkitabitest", argv);
    return -1;
}

static void test_socketpair_stream(void)
{
    const char *name = "AF_UNIX SOCK_STREAM socketpair";
    int sv[2];
    char buf[8];

    if (socketpair_raw(SOCK_STREAM, sv) < 0) {
        fail(name, "socketpair failed");
        return;
    }
    if (write(sv[0], "wk", 2) != 2 || read(sv[1], buf, 2) != 2 ||
        buf[0] != 'w' || buf[1] != 'k') {
        close(sv[0]);
        close(sv[1]);
        fail(name, "stream bytes did not round-trip");
        return;
    }

    close(sv[0]);
    close(sv[1]);
    pass(name);
}

static void test_socketpair_seqpacket_policy(void)
{
    const char *name = "AF_UNIX SOCK_SEQPACKET socketpair";
    int sv[2];
    int type;
    int optlen = sizeof(type);
    char a[] = "abc";
    char b[] = "de";
    char c[] = "truncate";
    char buf[8];
    struct iovec iov;
    struct msghdr msg;

    if (socketpair_raw(SOCK_SEQPACKET | SOCK_CLOEXEC, sv) < 0) {
        fail(name, "socketpair failed");
        return;
    }

    if (getsockopt_raw(sv[0], SOL_SOCKET, SO_TYPE, &type, &optlen) < 0 ||
        optlen != sizeof(type) || type != SOCK_SEQPACKET) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "SO_TYPE did not report SOCK_SEQPACKET");
        return;
    }

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = a;
    iov.iov_len = sizeof(a) - 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (sendmsg_raw(sv[0], &msg, 0) != (int)iov.iov_len) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "first sendmsg failed");
        return;
    }
    iov.iov_base = b;
    iov.iov_len = sizeof(b) - 1;
    if (sendmsg_raw(sv[0], &msg, 0) != (int)iov.iov_len) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "second sendmsg failed");
        return;
    }

    memset(buf, 0, sizeof(buf));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (recvmsg_raw(sv[1], &msg, 0) != 3 ||
        memcmp(buf, "abc", 3) != 0 || (msg.msg_flags & MSG_TRUNC)) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "first packet boundary was not preserved");
        return;
    }
    memset(buf, 0, sizeof(buf));
    if (recvmsg_raw(sv[1], &msg, 0) != 2 ||
        memcmp(buf, "de", 2) != 0 || (msg.msg_flags & MSG_TRUNC)) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "second packet boundary was not preserved");
        return;
    }

    iov.iov_base = c;
    iov.iov_len = sizeof(c) - 1;
    if (sendmsg_raw(sv[0], &msg, 0) != (int)iov.iov_len) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "truncate sendmsg failed");
        return;
    }

    memset(buf, 0, sizeof(buf));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = 4;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (recvmsg_raw(sv[1], &msg, 0) != 4 ||
        memcmp(buf, "trun", 4) != 0 || !(msg.msg_flags & MSG_TRUNC)) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "short receive did not truncate one packet");
        return;
    }

    close(sv[0]);
    close(sv[1]);
    pass(name);
}

static void test_socket_nonblock_poll(void)
{
    const char *name = "AF_UNIX nonblock poll readiness";
    int sv[2];
    struct pollfd pfd;
    char c;

    if (socketpair_raw(SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, sv) < 0) {
        fail(name, "socketpair nonblock/cloexec failed");
        return;
    }

    pfd.fd = sv[1];
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;
    if (poll_raw(&pfd, 1, 0) < 0 || !(pfd.revents & POLLOUT)) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "empty socket was not writable");
        return;
    }

    if (read(sv[1], &c, 1) >= 0) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "empty nonblocking read succeeded");
        return;
    }
    if (write(sv[0], "x", 1) != 1) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "write failed");
        return;
    }
    pfd.revents = 0;
    if (poll_raw(&pfd, 1, 1000) <= 0 || !(pfd.revents & POLLIN)) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "readable socket did not report POLLIN");
        return;
    }
    if (read(sv[1], &c, 1) != 1 || c != 'x') {
        close(sv[0]);
        close(sv[1]);
        fail(name, "readable byte mismatch");
        return;
    }

    close(sv[0]);
    pfd.revents = 0;
    poll_raw(&pfd, 1, 0);
    if (!(pfd.revents & (POLLHUP | POLLIN))) {
        close(sv[1]);
        fail(name, "peer close did not produce HUP/read readiness");
        return;
    }

    close(sv[1]);
    pass(name);
}

static void test_socket_nonblock_connect(void)
{
    const char *name = "AF_UNIX nonblock connect readiness";
    struct sockaddr_un sa;
    struct pollfd pfd;
    int listener = -1;
    int client = -1;
    int server = -1;
    int val = -1;
    int len = sizeof(val);
    int rc;

    listener = socket_raw(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    client = socket_raw(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0 || client < 0) {
        fail(name, "socket creation failed");
        goto out;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "webkitabitest-nb-connect");

    if (bind_unix_raw(listener, &sa) < 0 || listen_raw(listener, 1) < 0) {
        fail(name, "bind/listen failed");
        goto out;
    }

    rc = connect_unix_raw(client, &sa);
    if (rc != -EINPROGRESS) {
        fail(name, "nonblocking connect did not return EINPROGRESS");
        goto out;
    }

    pfd.fd = client;
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;
    if (poll_raw(&pfd, 1, 0) < 0 || (pfd.revents & (POLLIN | POLLOUT))) {
        fail(name, "pending connect reported data or write readiness");
        goto out;
    }

    len = sizeof(val);
    if (getsockopt_raw(client, SOL_SOCKET, SO_ERROR, &val, &len) < 0 ||
        len != sizeof(val) || val != EINPROGRESS) {
        fail(name, "pending SO_ERROR was not EINPROGRESS");
        goto out;
    }

    server = accept4_raw(listener, SOCK_CLOEXEC);
    if (server < 0) {
        fail(name, "accept4 failed");
        goto out;
    }

    pfd.revents = 0;
    if (poll_raw(&pfd, 1, 1000) <= 0 || !(pfd.revents & POLLOUT)) {
        fail(name, "accepted connect did not become writable");
        goto out;
    }

    len = sizeof(val);
    val = -1;
    if (getsockopt_raw(client, SOL_SOCKET, SO_ERROR, &val, &len) < 0 ||
        len != sizeof(val) || val != 0) {
        fail(name, "accepted SO_ERROR was not zero");
        goto out;
    }

    pass(name);

out:
    if (server >= 0)
        close(server);
    if (client >= 0)
        close(client);
    if (listener >= 0)
        close(listener);
}

static void test_socket_nonblock_connect_epoll(void)
{
    const char *name = "AF_UNIX nonblock connect epoll readiness";
    struct sockaddr_un sa;
    struct epoll_event_abi ev;
    struct epoll_event_abi out;
    int listener = -1;
    int client = -1;
    int server = -1;
    int epfd = -1;
    int rc;

    listener = socket_raw(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    client = socket_raw(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    epfd = epoll_create1_raw(EPOLL_CLOEXEC);
    if (listener < 0 || client < 0 || epfd < 0) {
        fail(name, "socket or epoll creation failed");
        goto out;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "webkitabitest-nb-epoll");

    if (bind_unix_raw(listener, &sa) < 0 || listen_raw(listener, 1) < 0) {
        fail(name, "bind/listen failed");
        goto out;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data = 0x45504f4c4cULL;
    if (epoll_ctl_raw(epfd, EPOLL_CTL_ADD, client, &ev) < 0) {
        fail(name, "epoll_ctl add failed");
        goto out;
    }

    rc = connect_unix_raw(client, &sa);
    if (rc != -EINPROGRESS) {
        fail(name, "nonblocking connect did not return EINPROGRESS");
        goto out;
    }

    memset(&out, 0, sizeof(out));
    if (epoll_pwait_raw(epfd, &out, 1, 0) != 0) {
        fail(name, "pending connect reported epoll readiness");
        goto out;
    }

    server = accept4_raw(listener, SOCK_CLOEXEC);
    if (server < 0) {
        fail(name, "accept4 failed");
        goto out;
    }

    memset(&out, 0, sizeof(out));
    rc = epoll_pwait_raw(epfd, &out, 1, 1000);
    if (rc <= 0 || !(out.events & EPOLLOUT) || out.data != ev.data) {
        fail(name, "accepted connect did not wake epoll writable");
        goto out;
    }

    pass(name);

out:
    if (server >= 0)
        close(server);
    if (epfd >= 0)
        close(epfd);
    if (client >= 0)
        close(client);
    if (listener >= 0)
        close(listener);
}

static void test_socket_sol_options(void)
{
    const char *name = "AF_UNIX SOL_SOCKET compatibility";
    int sv[2];
    int val;
    int len;
    struct ucred cred;

    if (socketpair_raw(SOCK_STREAM | SOCK_CLOEXEC, sv) < 0) {
        fail(name, "socketpair failed");
        return;
    }

    val = 1;
    if (setsockopt_raw(sv[0], SOL_SOCKET, SO_PASSCRED, &val, sizeof(val)) < 0 ||
        setsockopt_raw(sv[0], SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "setsockopt compatibility option failed");
        return;
    }

    len = sizeof(val);
    if (getsockopt_raw(sv[0], SOL_SOCKET, SO_DOMAIN, &val, &len) < 0 ||
        len != sizeof(val) || val != AF_UNIX) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "SO_DOMAIN failed");
        return;
    }

    len = sizeof(val);
    if (getsockopt_raw(sv[0], SOL_SOCKET, SO_PROTOCOL, &val, &len) < 0 ||
        len != sizeof(val) || val != 0) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "SO_PROTOCOL failed");
        return;
    }

    len = sizeof(val);
    if (getsockopt_raw(sv[0], SOL_SOCKET, SO_TYPE, &val, &len) < 0 ||
        len != sizeof(val) || val != SOCK_STREAM) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "SO_TYPE failed");
        return;
    }

    len = sizeof(val);
    if (getsockopt_raw(sv[0], SOL_SOCKET, SO_PASSCRED, &val, &len) < 0 ||
        len != sizeof(val)) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "SO_PASSCRED failed");
        return;
    }

    len = sizeof(cred);
    if (getsockopt_raw(sv[0], SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 ||
        len != sizeof(cred)) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "SO_PEERCRED failed");
        return;
    }

    len = sizeof(val);
    if (getsockopt_raw(sv[0], SOL_SOCKET, SO_SNDBUF, &val, &len) < 0 ||
        len != sizeof(val) || val <= 0) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "SO_SNDBUF failed");
        return;
    }

    len = sizeof(val);
    if (getsockopt_raw(sv[0], SOL_SOCKET, SO_RCVBUF, &val, &len) < 0 ||
        len != sizeof(val) || val <= 0) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "SO_RCVBUF failed");
        return;
    }

    close(sv[0]);
    close(sv[1]);
    pass(name);
}

static void test_socket_cloexec_exec(void)
{
    const char *name = "SOCK_CLOEXEC survives exec check";
    int sv[2];
    int pid;
    int status = 0;

    if (socketpair_raw(SOCK_STREAM | SOCK_CLOEXEC, sv) < 0) {
        fail(name, "socketpair CLOEXEC failed");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "fork failed");
        return;
    }
    if (pid == 0) {
        close(sv[1]);
        exit(exec_check_closed(sv[0]) == 0 ? 0 : 1);
    }

    close(sv[0]);
    close(sv[1]);
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "child observed socket fd after exec");
        return;
    }
    pass(name);
}

static void test_scm_rights_batch(void)
{
    const char *name = "SCM_RIGHTS multiple fd batch";
    int sv[2];
    int fd1 = -1;
    int fd2 = -1;
    int sent_fds[2];
    int got_fds[2];
    char control[CMSG_SPACE(sizeof(sent_fds))];
    char recv_control[CMSG_SPACE(sizeof(got_fds))];
    char byte = 'F';
    char got = 0;
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;

    unlink("__webkit_fd1");
    unlink("__webkit_fd2");
    fd1 = open("__webkit_fd1", O_CREAT | O_RDWR);
    fd2 = open("__webkit_fd2", O_CREAT | O_RDWR);
    if (fd1 < 0 || fd2 < 0 || socketpair_raw(SOCK_STREAM, sv) < 0) {
        if (fd1 >= 0) close(fd1);
        if (fd2 >= 0) close(fd2);
        fail(name, "setup failed");
        return;
    }

    write(fd1, "A", 1);
    write(fd2, "B", 1);
    lseek(fd1, 0, SEEK_SET);
    lseek(fd2, 0, SEEK_SET);

    memset(control, 0, sizeof(control));
    sent_fds[0] = fd1;
    sent_fds[1] = fd2;
    cmsg = (struct cmsghdr *)control;
    cmsg->cmsg_len = CMSG_LEN(sizeof(sent_fds));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), sent_fds, sizeof(sent_fds));

    iov.iov_base = &byte;
    iov.iov_len = 1;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    if (sendmsg_raw(sv[0], &msg, MSG_DONTWAIT) != 1) {
        close(fd1); close(fd2); close(sv[0]); close(sv[1]);
        fail(name, "sendmsg failed");
        return;
    }
    close(fd1);
    close(fd2);

    memset(recv_control, 0, sizeof(recv_control));
    got = 0;
    iov.iov_base = &got;
    iov.iov_len = 1;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = recv_control;
    msg.msg_controllen = sizeof(recv_control);

    if (recvmsg_raw(sv[1], &msg, MSG_CMSG_CLOEXEC) != 1 || got != 'F') {
        close(sv[0]); close(sv[1]);
        fail(name, "recvmsg payload failed");
        return;
    }

    cmsg = (struct cmsghdr *)recv_control;
    if (msg.msg_controllen < CMSG_LEN(sizeof(got_fds)) ||
        cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        close(sv[0]); close(sv[1]);
        fail(name, "control message missing");
        return;
    }
    memcpy(got_fds, CMSG_DATA(cmsg), sizeof(got_fds));
    if (read(got_fds[0], &got, 1) != 1 || got != 'A' ||
        read(got_fds[1], &got, 1) != 1 || got != 'B') {
        close(got_fds[0]); close(got_fds[1]); close(sv[0]); close(sv[1]);
        fail(name, "received fd contents wrong");
        return;
    }

    close(got_fds[0]);
    close(got_fds[1]);
    close(sv[0]);
    close(sv[1]);
    unlink("__webkit_fd1");
    unlink("__webkit_fd2");
    pass(name);
}

static void test_scm_rights_stream_first_byte_barrier(void)
{
    const char *name = "SCM_RIGHTS stream first-byte barrier";
    int sv[2];
    int fd = -1;
    int sent_fd;
    int got_fd = -1;
    char control[CMSG_SPACE(sizeof(sent_fd))];
    char recv_control[CMSG_SPACE(sizeof(got_fd))];
    char bytes[8] = "ABCDEFG";
    char got = 0;
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;

    unlink("__webkit_barrier_fd");
    fd = open("__webkit_barrier_fd", O_CREAT | O_RDWR);
    if (fd < 0 || socketpair_raw(SOCK_STREAM, sv) < 0) {
        if (fd >= 0)
            close(fd);
        fail(name, "setup failed");
        return;
    }

    write(fd, "Z", 1);
    lseek(fd, 0, SEEK_SET);

    memset(control, 0, sizeof(control));
    sent_fd = fd;
    cmsg = (struct cmsghdr *)control;
    cmsg->cmsg_len = CMSG_LEN(sizeof(sent_fd));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &sent_fd, sizeof(sent_fd));

    iov.iov_base = bytes;
    iov.iov_len = sizeof(bytes);
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    if (sendmsg_raw(sv[0], &msg, MSG_DONTWAIT) != (int)sizeof(bytes)) {
        close(fd); close(sv[0]); close(sv[1]);
        fail(name, "sendmsg failed");
        return;
    }
    close(fd);

    memset(recv_control, 0, sizeof(recv_control));
    got = 0;
    iov.iov_base = &got;
    iov.iov_len = 1;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = recv_control;
    msg.msg_controllen = sizeof(recv_control);

    if (recvmsg_raw(sv[1], &msg, MSG_CMSG_CLOEXEC) != 1 || got != 'A') {
        close(sv[0]); close(sv[1]);
        fail(name, "first-byte recv failed");
        return;
    }

    cmsg = (struct cmsghdr *)recv_control;
    if (msg.msg_controllen < CMSG_LEN(sizeof(got_fd)) ||
        cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        close(sv[0]); close(sv[1]);
        fail(name, "fd was not delivered with first byte");
        return;
    }

    memcpy(&got_fd, CMSG_DATA(cmsg), sizeof(got_fd));
    if (read(got_fd, &got, 1) != 1 || got != 'Z') {
        close(got_fd); close(sv[0]); close(sv[1]);
        fail(name, "received fd content wrong");
        return;
    }

    close(got_fd);
    close(sv[0]);
    close(sv[1]);
    unlink("__webkit_barrier_fd");
    pass(name);
}

static void test_scm_rights_recvmmsg_batch(void)
{
    const char *name = "SCM_RIGHTS recvmmsg multiple fd batch";
    int sv[2];
    int fd1 = -1;
    int fd2 = -1;
    int sent_fds[2];
    int got_fds[2];
    char control[CMSG_SPACE(sizeof(sent_fds))];
    char recv_control[CMSG_SPACE(sizeof(got_fds))];
    char byte = 'R';
    char got = 0;
    struct iovec send_iov;
    struct iovec recv_iov;
    struct msghdr send_msg;
    struct mmsghdr recv_msg;
    struct cmsghdr *cmsg;

    unlink("__webkit_rfd1");
    unlink("__webkit_rfd2");
    fd1 = open("__webkit_rfd1", O_CREAT | O_RDWR);
    fd2 = open("__webkit_rfd2", O_CREAT | O_RDWR);
    if (fd1 < 0 || fd2 < 0 || socketpair_raw(SOCK_STREAM, sv) < 0) {
        if (fd1 >= 0) close(fd1);
        if (fd2 >= 0) close(fd2);
        fail(name, "setup failed");
        return;
    }

    write(fd1, "X", 1);
    write(fd2, "Y", 1);
    lseek(fd1, 0, SEEK_SET);
    lseek(fd2, 0, SEEK_SET);

    memset(control, 0, sizeof(control));
    sent_fds[0] = fd1;
    sent_fds[1] = fd2;
    cmsg = (struct cmsghdr *)control;
    cmsg->cmsg_len = CMSG_LEN(sizeof(sent_fds));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), sent_fds, sizeof(sent_fds));

    send_iov.iov_base = &byte;
    send_iov.iov_len = 1;
    memset(&send_msg, 0, sizeof(send_msg));
    send_msg.msg_iov = &send_iov;
    send_msg.msg_iovlen = 1;
    send_msg.msg_control = control;
    send_msg.msg_controllen = sizeof(control);

    if (sendmsg_raw(sv[0], &send_msg, MSG_DONTWAIT) != 1) {
        close(fd1); close(fd2); close(sv[0]); close(sv[1]);
        fail(name, "sendmsg failed");
        return;
    }
    close(fd1);
    close(fd2);

    memset(recv_control, 0, sizeof(recv_control));
    recv_iov.iov_base = &got;
    recv_iov.iov_len = 1;
    memset(&recv_msg, 0, sizeof(recv_msg));
    recv_msg.msg_hdr.msg_iov = &recv_iov;
    recv_msg.msg_hdr.msg_iovlen = 1;
    recv_msg.msg_hdr.msg_control = recv_control;
    recv_msg.msg_hdr.msg_controllen = sizeof(recv_control);

    if (recvmmsg_raw(sv[1], &recv_msg, 1, MSG_CMSG_CLOEXEC) != 1 ||
        recv_msg.msg_len != 1 || got != 'R') {
        close(sv[0]); close(sv[1]);
        fail(name, "recvmmsg payload failed");
        return;
    }

    cmsg = (struct cmsghdr *)recv_control;
    if (recv_msg.msg_hdr.msg_controllen < CMSG_LEN(sizeof(got_fds)) ||
        cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        close(sv[0]); close(sv[1]);
        fail(name, "control message missing");
        return;
    }

    memcpy(got_fds, CMSG_DATA(cmsg), sizeof(got_fds));
    if (read(got_fds[0], &got, 1) != 1 || got != 'X' ||
        read(got_fds[1], &got, 1) != 1 || got != 'Y') {
        close(got_fds[0]); close(got_fds[1]); close(sv[0]); close(sv[1]);
        fail(name, "received fd contents wrong");
        return;
    }

    close(got_fds[0]);
    close(got_fds[1]);
    close(sv[0]);
    close(sv[1]);
    unlink("__webkit_rfd1");
    unlink("__webkit_rfd2");
    pass(name);
}

static void test_scm_rights_process_lifetime(void)
{
    const char *name = "SCM_RIGHTS lifetime across sender exit";
    int sv[2];
    int pid;
    int status = 0;
    char recv_control[CMSG_SPACE(sizeof(int))];
    char got = 0;
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    int gotfd = -1;

    unlink("__webkit_life");
    if (socketpair_raw(SOCK_STREAM, sv) < 0) {
        fail(name, "socketpair failed");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(sv[0]); close(sv[1]);
        fail(name, "fork failed");
        return;
    }
    if (pid == 0) {
        int fd = open("__webkit_life", O_CREAT | O_RDWR);
        int passfd;
        char control[CMSG_SPACE(sizeof(int))];
        char byte = 'L';
        struct cmsghdr *scmsg;

        close(sv[1]);
        if (fd < 0)
            exit(2);
        write(fd, "Z", 1);
        lseek(fd, 0, SEEK_SET);
        passfd = fd;
        memset(control, 0, sizeof(control));
        scmsg = (struct cmsghdr *)control;
        scmsg->cmsg_len = CMSG_LEN(sizeof(passfd));
        scmsg->cmsg_level = SOL_SOCKET;
        scmsg->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(scmsg), &passfd, sizeof(passfd));

        iov.iov_base = &byte;
        iov.iov_len = 1;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        if (sendmsg_raw(sv[0], &msg, 0) != 1)
            exit(3);
        close(fd);
        close(sv[0]);
        exit(0);
    }

    close(sv[0]);
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        close(sv[1]);
        fail(name, "sender failed before receiver used fd");
        return;
    }

    memset(recv_control, 0, sizeof(recv_control));
    iov.iov_base = &got;
    iov.iov_len = 1;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = recv_control;
    msg.msg_controllen = sizeof(recv_control);
    if (recvmsg_raw(sv[1], &msg, 0) != 1 || got != 'L') {
        close(sv[1]);
        fail(name, "receiver did not get payload after sender exit");
        return;
    }
    cmsg = (struct cmsghdr *)recv_control;
    if (msg.msg_controllen < CMSG_LEN(sizeof(int)) ||
        cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        close(sv[1]);
        fail(name, "receiver did not get fd after sender exit");
        return;
    }
    memcpy(&gotfd, CMSG_DATA(cmsg), sizeof(gotfd));
    got = 0;
    if (read(gotfd, &got, 1) != 1 || got != 'Z') {
        close(gotfd);
        close(sv[1]);
        fail(name, "received fd was not usable");
        return;
    }

    close(gotfd);
    close(sv[1]);
    unlink("__webkit_life");
    pass(name);
}

struct ipc_stress_header {
    uint magic;
    uint seq;
    uint len;
    uint checksum;
};

static uint ipc_stress_byte(uint seq, uint index)
{
    uint x = seq * 1103515245u + index * 2654435761u + 0x9e3779b9u;
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    return x & 0xff;
}

static uint ipc_stress_checksum(const uchar *buf, uint len)
{
    uint h = 2166136261u;
    for (uint i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 16777619u;
    }
    return h;
}

static uint ipc_stress_len(uint seq)
{
    uint len = 1 + ((seq * 7919u) % IPC_STRESS_MAX_PAYLOAD);
    if ((seq % 31) == 0)
        len = 2048 + (seq % 7);
    if ((seq % 127) == 0)
        len = IPC_STRESS_MAX_PAYLOAD - (seq % 113);
    return len;
}

static void ipc_stress_fill(uchar *buf, uint len, uint seq)
{
    for (uint i = 0; i < len; i++)
        buf[i] = (uchar)ipc_stress_byte(seq, i);
}

static int ipc_stress_send_message(int fd, uint seq, int pass_fd)
{
    static uchar payload[IPC_STRESS_MAX_PAYLOAD];
    struct ipc_stress_header hdr;
    struct iovec iov[3];
    struct msghdr msg;
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;
    uint len = ipc_stress_len(seq);
    int ret;

    ipc_stress_fill(payload, len, seq);
    hdr.magic = IPC_STRESS_MAGIC;
    hdr.seq = seq;
    hdr.len = len;
    hdr.checksum = ipc_stress_checksum(payload, len);

    iov[0].iov_base = &hdr;
    iov[0].iov_len = 7;
    iov[1].iov_base = ((char *)&hdr) + 7;
    iov[1].iov_len = sizeof(hdr) - 7;
    iov[2].iov_base = payload;
    iov[2].iov_len = len;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 3;

    if (pass_fd >= 0) {
        memset(control, 0, sizeof(control));
        cmsg = (struct cmsghdr *)control;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(cmsg), &pass_fd, sizeof(pass_fd));
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
    }

    ret = sendmsg_raw(fd, &msg, 0);
    return ret == (int)(sizeof(hdr) + len) ? 0 : ret;
}

static int ipc_stress_send_message_retry(int fd, uint seq, int pass_fd)
{
    for (int tries = 0; tries < 1000; tries++) {
        int ret = ipc_stress_send_message(fd, seq, pass_fd);
        if (ret == 0)
            return 0;
        if (ret != -EAGAIN)
            return ret;
        sleep(1);
    }
    return -EAGAIN;
}

static int ipc_stress_recv_some(int fd, uchar *buf, uint want, int *received_fd)
{
    struct iovec iov[3];
    struct msghdr msg;
    char control[CMSG_SPACE(sizeof(int))];
    uint a = want > 5 ? 5 : want;
    uint b = want > a ? ((want - a) > 251 ? 251 : (want - a)) : 0;
    uint c = want - a - b;
    int ret;

    iov[0].iov_base = buf;
    iov[0].iov_len = a;
    iov[1].iov_base = buf + a;
    iov[1].iov_len = b;
    iov[2].iov_base = buf + a + b;
    iov[2].iov_len = c;

    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 3;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ret = recvmsg_raw(fd, &msg, MSG_CMSG_CLOEXEC);
    if (ret > 0 && msg.msg_controllen >= CMSG_LEN(sizeof(int))) {
        struct cmsghdr *cmsg = (struct cmsghdr *)control;
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int fdtmp = -1;
            memcpy(&fdtmp, CMSG_DATA(cmsg), sizeof(fdtmp));
            if (fdtmp >= 0) {
                if (*received_fd >= 0)
                    close(*received_fd);
                *received_fd = fdtmp;
            }
        }
    }
    return ret;
}

static int ipc_stress_read_exact(int fd, uchar *buf, uint len, int *received_fd)
{
    uint got = 0;
    while (got < len) {
        int n = ipc_stress_recv_some(fd, buf + got, len - got, received_fd);
        if (n <= 0)
            return -1;
        got += (uint)n;
    }
    return 0;
}

static int ipc_stress_recv_message(int fd, uint seq, int *received_fd)
{
    static uchar payload[IPC_STRESS_MAX_PAYLOAD];
    struct ipc_stress_header hdr;
    char why[128];

    if (ipc_stress_read_exact(fd, (uchar *)&hdr, sizeof(hdr), received_fd) < 0)
        return -1;
    if (hdr.magic != IPC_STRESS_MAGIC || hdr.seq != seq ||
        hdr.len != ipc_stress_len(seq) || hdr.len > IPC_STRESS_MAX_PAYLOAD)
        return -2;
    if (ipc_stress_read_exact(fd, payload, hdr.len, received_fd) < 0)
        return -3;
    if (ipc_stress_checksum(payload, hdr.len) != hdr.checksum)
        return -4;
    for (uint i = 0; i < hdr.len; i++) {
        if (payload[i] != (uchar)ipc_stress_byte(seq, i)) {
            snprintf(why, sizeof(why), "seq=%u byte=%u got=%u", seq, i, payload[i]);
            fprintf(2, "webkitabitest: IPC stream byte mismatch %s\n", why);
            return -5;
        }
    }
    return 0;
}

static void test_ipc_stream_stress(void)
{
    const char *name = "AF_UNIX WebKit IPC stream stress";
    int sv[2];
    int pid;
    int status = 0;

    unlink("__webkit_ipc_fd");
    if (socketpair_raw(SOCK_STREAM | SOCK_CLOEXEC, sv) < 0) {
        fail(name, "socketpair failed");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "fork failed");
        return;
    }

    if (pid == 0) {
        int fd = open("__webkit_ipc_fd", O_CREAT | O_RDWR);
        close(sv[0]);
        if (fd < 0)
            exit(2);
        write(fd, "S", 1);
        lseek(fd, 0, SEEK_SET);
        for (uint seq = 0; seq < IPC_STRESS_MESSAGES; seq++) {
            int pass_fd = (seq % 101) == 17 ? fd : -1;
            int send_rc = ipc_stress_send_message_retry(sv[1], seq, pass_fd);
            if (send_rc != 0) {
                fprintf(2, "webkitabitest: IPC stress send failed seq=%u rc=%d\n",
                        seq, send_rc);
                exit(3);
            }
        }
        close(fd);
        close(sv[1]);
        exit(0);
    }

    close(sv[1]);
    int received_fd = -1;
    for (uint seq = 0; seq < IPC_STRESS_MESSAGES; seq++) {
        int rc = ipc_stress_recv_message(sv[0], seq, &received_fd);
        if (rc < 0) {
            char why[96];
            snprintf(why, sizeof(why), "message %u failed rc=%d", seq, rc);
            close(sv[0]);
            if (received_fd >= 0)
                close(received_fd);
            waitpid(pid, &status, 0);
            fail(name, why);
            return;
        }
    }

    close(sv[0]);
    waitpid(pid, &status, 0);
    if (received_fd < 0) {
        fail(name, "SCM_RIGHTS fd was not delivered");
        return;
    }
    char c = 0;
    lseek(received_fd, 0, SEEK_SET);
    if (read(received_fd, &c, 1) != 1 || c != 'S') {
        close(received_fd);
        fail(name, "delivered fd content mismatch");
        return;
    }
    close(received_fd);
    unlink("__webkit_ipc_fd");

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "sender failed");
        return;
    }
    pass(name);
}

static int seqpacket_send_chunk(int fd, uint seq, int pass_fd)
{
    struct ipc_stress_header hdr;
    static uchar payload[2048];
    struct iovec iov[2];
    struct msghdr msg;
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;

    for (uint i = 0; i < sizeof(payload); i++)
        payload[i] = (uchar)ipc_stress_byte(seq, i);

    hdr.magic = IPC_STRESS_MAGIC;
    hdr.seq = seq;
    hdr.len = sizeof(payload);
    hdr.checksum = ipc_stress_checksum(payload, sizeof(payload));

    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = payload;
    iov[1].iov_len = sizeof(payload);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    if (pass_fd >= 0) {
        memset(control, 0, sizeof(control));
        cmsg = (struct cmsghdr *)control;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(cmsg), &pass_fd, sizeof(pass_fd));
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
    }

    return sendmsg_raw(fd, &msg, 0);
}

static int seqpacket_recv_chunk(int fd, uint seq, int *received_fd)
{
    struct ipc_stress_header hdr;
    static uchar payload[2048];
    struct iovec iov[2];
    struct msghdr msg;
    char control[CMSG_SPACE(sizeof(int))];
    int ret;

    memset(&hdr, 0, sizeof(hdr));
    memset(payload, 0, sizeof(payload));
    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));

    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = payload;
    iov[1].iov_len = sizeof(payload);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ret = recvmsg_raw(fd, &msg, MSG_CMSG_CLOEXEC);
    if (ret != (int)(sizeof(hdr) + sizeof(payload))) {
        fprintf(2, "webkitabitest: seqpacket recv seq=%u ret=%d flags=0x%x controllen=%u\n",
                seq, ret, msg.msg_flags, msg.msg_controllen);
        return -1;
    }
    if (msg.msg_flags & MSG_TRUNC)
        return -2;
    if (hdr.magic != IPC_STRESS_MAGIC || hdr.seq != seq ||
        hdr.len != sizeof(payload) ||
        hdr.checksum != ipc_stress_checksum(payload, sizeof(payload)))
        return -3;
    for (uint i = 0; i < sizeof(payload); i++) {
        if (payload[i] != (uchar)ipc_stress_byte(seq, i))
            return -4;
    }

    if (msg.msg_controllen >= CMSG_LEN(sizeof(int))) {
        struct cmsghdr *cmsg = (struct cmsghdr *)control;
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int fdtmp = -1;
            memcpy(&fdtmp, CMSG_DATA(cmsg), sizeof(fdtmp));
            if (fdtmp >= 0) {
                if (*received_fd >= 0)
                    close(*received_fd);
                *received_fd = fdtmp;
            }
        }
    }
    return 0;
}

static void test_ipc_seqpacket_stress(void)
{
    const char *name = "AF_UNIX WebKit IPC seqpacket stress";
    enum { messages = 1400 };
    int sv[2];
    int pid;
    int status = 0;

    unlink("__webkit_seqpacket_fd");
    if (socketpair_raw(SOCK_SEQPACKET | SOCK_CLOEXEC, sv) < 0) {
        fail(name, "socketpair failed");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "fork failed");
        return;
    }

    if (pid == 0) {
        int fd = open("__webkit_seqpacket_fd", O_CREAT | O_RDWR);
        close(sv[0]);
        if (fd < 0)
            exit(2);
        write(fd, "Q", 1);
        lseek(fd, 0, SEEK_SET);
        for (uint seq = 0; seq < messages; seq++) {
            int pass_fd = (seq % 173) == 41 ? fd : -1;
            int ret = seqpacket_send_chunk(sv[1], seq, pass_fd);
            if (ret != (int)(sizeof(struct ipc_stress_header) + 2048)) {
                fprintf(2, "webkitabitest: seqpacket send failed seq=%u ret=%d\n",
                        seq, ret);
                exit(3);
            }
        }
        close(fd);
        close(sv[1]);
        exit(0);
    }

    close(sv[1]);
    int received_fd = -1;
    for (uint seq = 0; seq < messages; seq++) {
        int rc = seqpacket_recv_chunk(sv[0], seq, &received_fd);
        if (rc < 0) {
            char why[96];
            snprintf(why, sizeof(why), "message %u failed rc=%d", seq, rc);
            close(sv[0]);
            if (received_fd >= 0)
                close(received_fd);
            waitpid(pid, &status, 0);
            fail(name, why);
            return;
        }
    }

    close(sv[0]);
    waitpid(pid, &status, 0);
    if (received_fd < 0) {
        fail(name, "SCM_RIGHTS fd was not delivered");
        return;
    }
    char c = 0;
    lseek(received_fd, 0, SEEK_SET);
    if (read(received_fd, &c, 1) != 1 || c != 'Q') {
        close(received_fd);
        fail(name, "delivered fd content mismatch");
        return;
    }
    close(received_fd);
    unlink("__webkit_seqpacket_fd");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "sender process failed");
        return;
    }
    pass(name);
}

static uchar yt_inline_byte(uint index)
{
    uint x = index * 2654435761u + 0x9e3779b9u;
    x ^= x >> 17;
    x *= 2246822519u;
    x ^= x >> 15;
    x += index >> 3;
    return (uchar)x;
}

static uint yt_inline_checksum(const uchar *buf, uint len)
{
    uint h = 2166136261u;

    for (uint i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 16777619u;
    }
    return h;
}

static int fill_yt_inline_payload(uchar *buf, uint len)
{
    for (uint i = 0; i < len; i++)
        buf[i] = yt_inline_byte(i);
    return 0;
}

static int check_yt_inline_payload(const uchar *buf, uint len, const char *tag)
{
    for (uint i = 0; i < len; i++) {
        uchar want = yt_inline_byte(i);
        if (buf[i] != want) {
            fprintf(2, "webkitabitest: %s mismatch at %u got=%u want=%u\n",
                    tag, i, buf[i], want);
            return -1;
        }
    }
    return 0;
}

static int send_yt_inline_packet(int fd, int type, const uchar *payload)
{
    struct {
        uint magic;
        uint size;
        uint checksum;
    } hdr;
    struct iovec iov[4];
    struct msghdr msg;
    uint split0 = WEBKIT_YT_BASE_JS_CORRUPT_OFFSET;
    uint split1 = 2058;
    uint split2 = 7312;
    uint off = 0;

    hdr.magic = IPC_STRESS_MAGIC;
    hdr.size = WEBKIT_YT_BASE_JS_SIZE;
    hdr.checksum = yt_inline_checksum(payload, WEBKIT_YT_BASE_JS_SIZE);

    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = (void *)(payload + off);
    iov[1].iov_len = split0;
    off += split0;
    iov[2].iov_base = (void *)(payload + off);
    iov[2].iov_len = split1 + split2;
    off += split1 + split2;
    iov[3].iov_base = (void *)(payload + off);
    iov[3].iov_len = WEBKIT_YT_BASE_JS_SIZE - off;
    msg.msg_iov = iov;
    msg.msg_iovlen = 4;

    int ret = sendmsg_raw(fd, &msg, 0);
    int want = (int)(sizeof(hdr) + WEBKIT_YT_BASE_JS_SIZE);
    if (type == SOCK_STREAM)
        return ret == want ? 0 : -1;
    return ret == want ? 0 : -2;
}

static int recv_yt_inline_packet(int fd, int type, uchar *payload)
{
    struct {
        uint magic;
        uint size;
        uint checksum;
    } hdr;
    struct iovec iov[8];
    struct msghdr msg;
    uint off = 0;
    uint chunks[7] = {
        WEBKIT_YT_BASE_JS_CORRUPT_OFFSET - 64,
        64,
        2058,
        7312,
        65536,
        1048576,
        0,
    };

    chunks[6] = WEBKIT_YT_BASE_JS_SIZE -
        (chunks[0] + chunks[1] + chunks[2] + chunks[3] + chunks[4] + chunks[5]);

    memset(&hdr, 0, sizeof(hdr));
    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    for (uint i = 0; i < 7; i++) {
        iov[i + 1].iov_base = payload + off;
        iov[i + 1].iov_len = chunks[i];
        off += chunks[i];
    }
    msg.msg_iov = iov;
    msg.msg_iovlen = 8;

    int ret = recvmsg_raw(fd, &msg, 0);
    int want = (int)(sizeof(hdr) + WEBKIT_YT_BASE_JS_SIZE);
    if (ret != want || (type == SOCK_SEQPACKET && (msg.msg_flags & MSG_TRUNC))) {
        fprintf(2, "webkitabitest: inline recv type=%d ret=%d want=%d flags=0x%x\n",
                type, ret, want, msg.msg_flags);
        return -1;
    }
    if (hdr.magic != IPC_STRESS_MAGIC || hdr.size != WEBKIT_YT_BASE_JS_SIZE)
        return -2;
    if (hdr.checksum != yt_inline_checksum(payload, WEBKIT_YT_BASE_JS_SIZE))
        return -3;
    return check_yt_inline_payload(payload, WEBKIT_YT_BASE_JS_SIZE,
                                   type == SOCK_SEQPACKET ? "seqpacket inline" :
                                   "stream inline");
}

static void test_webkit_large_inline_ipc(int type)
{
    const char *name = type == SOCK_SEQPACKET
        ? "AF_UNIX large inline seqpacket integrity"
        : "AF_UNIX large inline stream integrity";
    int sv[2];
    int pid;
    int status = 0;
    uchar *payload = malloc(WEBKIT_YT_BASE_JS_SIZE);

    if (payload == NULL) {
        fail(name, "malloc failed");
        return;
    }
    fill_yt_inline_payload(payload, WEBKIT_YT_BASE_JS_SIZE);

    if (socketpair_raw(type | SOCK_CLOEXEC, sv) < 0) {
        free(payload);
        fail(name, "socketpair failed");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        free(payload);
        fail(name, "fork failed");
        return;
    }

    if (pid == 0) {
        close(sv[0]);
        int rc = send_yt_inline_packet(sv[1], type, payload);
        close(sv[1]);
        exit(rc == 0 ? 0 : 31);
    }

    close(sv[1]);
    memset(payload, 0, WEBKIT_YT_BASE_JS_SIZE);
    int rc = recv_yt_inline_packet(sv[0], type, payload);
    close(sv[0]);
    waitpid(pid, &status, 0);
    free(payload);
    if (rc < 0) {
        fail(name, "payload mismatch");
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "sender process failed");
        return;
    }
    pass(name);
}

struct webkit_chunk_header {
    uint magic;
    uint seq;
    uint offset;
    uint len;
    uint total;
    uint checksum;
};

static int webkit_chunk_send(int fd, uint seq, uint offset, uint len)
{
    struct webkit_chunk_header hdr;
    uchar payload[WEBKIT_IPC_DATA_CHUNK];
    struct iovec iov[2];
    struct msghdr msg;

    for (uint i = 0; i < len; i++)
        payload[i] = yt_inline_byte(offset + i);

    hdr.magic = IPC_STRESS_MAGIC;
    hdr.seq = seq;
    hdr.offset = offset;
    hdr.len = len;
    hdr.total = WEBKIT_YT_APP_JS_SIZE;
    hdr.checksum = yt_inline_checksum(payload, len);

    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = payload;
    iov[1].iov_len = len;
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    for (;;) {
        int ret = sendmsg_raw(fd, &msg, MSG_DONTWAIT);
        if (ret == (int)(sizeof(hdr) + len))
            return 0;
        if (ret == -EAGAIN) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll_raw(&pfd, 1, 1000) <= 0)
                return -2;
            continue;
        }
        fprintf(2, "webkitabitest: chunk seqpacket send seq=%u off=%u len=%u ret=%d\n",
                seq, offset, len, ret);
        return -1;
    }
}

static int webkit_chunk_send_nowait(int fd, uint seq, uint offset, uint len)
{
    struct webkit_chunk_header hdr;
    uchar payload[WEBKIT_IPC_DATA_CHUNK];
    struct iovec iov[2];
    struct msghdr msg;

    for (uint i = 0; i < len; i++)
        payload[i] = yt_inline_byte(offset + i);

    hdr.magic = IPC_STRESS_MAGIC;
    hdr.seq = seq;
    hdr.offset = offset;
    hdr.len = len;
    hdr.total = WEBKIT_YT_APP_JS_SIZE;
    hdr.checksum = yt_inline_checksum(payload, len);

    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = payload;
    iov[1].iov_len = len;
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    return sendmsg_raw(fd, &msg, MSG_DONTWAIT);
}

static int webkit_chunk_recv(int fd, uint seq, uint offset, uint len)
{
    struct webkit_chunk_header hdr;
    uchar payload[WEBKIT_IPC_DATA_CHUNK];
    struct iovec iov[2];
    struct msghdr msg;
    int ret;

    memset(&hdr, 0, sizeof(hdr));
    memset(payload, 0, sizeof(payload));
    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = payload;
    iov[1].iov_len = sizeof(payload);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    ret = recvmsg_raw(fd, &msg, 0);
    if (ret != (int)(sizeof(hdr) + len)) {
        fprintf(2, "webkitabitest: chunk seqpacket recv seq=%u ret=%d want=%u flags=0x%x\n",
                seq, ret, (uint)(sizeof(hdr) + len), msg.msg_flags);
        return -1;
    }
    if (msg.msg_flags & MSG_TRUNC)
        return -2;
    if (hdr.magic != IPC_STRESS_MAGIC || hdr.seq != seq ||
        hdr.offset != offset || hdr.len != len ||
        hdr.total != WEBKIT_YT_APP_JS_SIZE)
        return -3;
    if (hdr.checksum != yt_inline_checksum(payload, len))
        return -4;
    for (uint i = 0; i < len; i++) {
        uchar want = yt_inline_byte(offset + i);
        if (payload[i] != want) {
            fprintf(2, "webkitabitest: chunk seqpacket mismatch seq=%u byte=%u got=%u want=%u\n",
                    seq, i, payload[i], want);
            return -5;
        }
    }
    return 0;
}

static void test_webkit_seqpacket_chunk_transfer(void)
{
    const char *name = "AF_UNIX seqpacket WebKit 2048 chunk transfer";
    int sv[2];
    int pid;
    int status = 0;

    if (socketpair_raw(SOCK_SEQPACKET | SOCK_CLOEXEC, sv) < 0) {
        fail(name, "socketpair failed");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        fail(name, "fork failed");
        return;
    }

    if (pid == 0) {
        uint off = 0;
        uint seq = 0;
        close(sv[0]);
        while (off < WEBKIT_YT_APP_JS_SIZE) {
            uint chunk = WEBKIT_YT_APP_JS_SIZE - off;
            if (chunk > WEBKIT_IPC_DATA_CHUNK)
                chunk = WEBKIT_IPC_DATA_CHUNK;
            if (webkit_chunk_send(sv[1], seq, off, chunk) < 0)
                exit(51);
            off += chunk;
            seq++;
        }
        close(sv[1]);
        exit(0);
    }

    close(sv[1]);
    uint off = 0;
    uint seq = 0;
    while (off < WEBKIT_YT_APP_JS_SIZE) {
        uint chunk = WEBKIT_YT_APP_JS_SIZE - off;
        if (chunk > WEBKIT_IPC_DATA_CHUNK)
            chunk = WEBKIT_IPC_DATA_CHUNK;
        int rc = webkit_chunk_recv(sv[0], seq, off, chunk);
        if (rc < 0) {
            char why[96];
            snprintf(why, sizeof(why), "message %u failed rc=%d", seq, rc);
            close(sv[0]);
            waitpid(pid, &status, 0);
            fail(name, why);
            return;
        }
        off += chunk;
        seq++;
    }
    close(sv[0]);
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "sender process failed");
        return;
    }
    pass(name);
}

static void test_webkit_seqpacket_chunk_burst_queue(void)
{
    const char *name = "AF_UNIX seqpacket WebKit full burst queue";
    int sv[2];
    uint off = 0;
    uint seq = 0;

    if (socketpair_raw(SOCK_SEQPACKET | SOCK_CLOEXEC, sv) < 0) {
        fail(name, "socketpair failed");
        return;
    }

    while (off < WEBKIT_YT_APP_JS_SIZE) {
        uint chunk = WEBKIT_YT_APP_JS_SIZE - off;
        if (chunk > WEBKIT_IPC_DATA_CHUNK)
            chunk = WEBKIT_IPC_DATA_CHUNK;
        int ret = webkit_chunk_send_nowait(sv[1], seq, off, chunk);
        if (ret != (int)(sizeof(struct webkit_chunk_header) + chunk)) {
            char why[96];
            snprintf(why, sizeof(why), "send seq=%u ret=%d", seq, ret);
            close(sv[0]);
            close(sv[1]);
            fail(name, why);
            return;
        }
        off += chunk;
        seq++;
    }

    off = 0;
    seq = 0;
    while (off < WEBKIT_YT_APP_JS_SIZE) {
        uint chunk = WEBKIT_YT_APP_JS_SIZE - off;
        if (chunk > WEBKIT_IPC_DATA_CHUNK)
            chunk = WEBKIT_IPC_DATA_CHUNK;
        int rc = webkit_chunk_recv(sv[0], seq, off, chunk);
        if (rc < 0) {
            char why[96];
            snprintf(why, sizeof(why), "recv seq=%u rc=%d", seq, rc);
            close(sv[0]);
            close(sv[1]);
            fail(name, why);
            return;
        }
        off += chunk;
        seq++;
    }

    close(sv[0]);
    close(sv[1]);
    pass(name);
}

static void test_webkit_stream_page_chunk_transfer(void)
{
    const char *name = "AF_UNIX stream WebKit page chunk transfer";
    int sv[2];
    int pid;
    int status = 0;
    uchar *payload = malloc(WEBKIT_YT_APP_JS_SIZE);

    if (payload == NULL) {
        fail(name, "malloc failed");
        return;
    }
    fill_yt_inline_payload(payload, WEBKIT_YT_APP_JS_SIZE);

    if (socketpair_raw(SOCK_STREAM | SOCK_CLOEXEC, sv) < 0) {
        free(payload);
        fail(name, "socketpair failed");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        free(payload);
        fail(name, "fork failed");
        return;
    }

    if (pid == 0) {
        uint off = 0;
        close(sv[0]);
        while (off < WEBKIT_YT_APP_JS_SIZE) {
            uint chunk = WEBKIT_YT_APP_JS_SIZE - off;
            if (chunk > WEBKIT_PAGE_CHUNK)
                chunk = WEBKIT_PAGE_CHUNK;
            int n = write(sv[1], payload + off, chunk);
            if (n != (int)chunk) {
                fprintf(2, "webkitabitest: stream chunk write off=%u n=%d chunk=%u\n",
                        off, n, chunk);
                exit(41);
            }
            off += chunk;
        }
        close(sv[1]);
        exit(0);
    }

    close(sv[1]);
    memset(payload, 0, WEBKIT_YT_APP_JS_SIZE);
    uint off = 0;
    while (off < WEBKIT_YT_APP_JS_SIZE) {
        uint chunk = WEBKIT_YT_APP_JS_SIZE - off;
        if (chunk > WEBKIT_PAGE_CHUNK)
            chunk = WEBKIT_PAGE_CHUNK;
        int n = read(sv[0], payload + off, chunk);
        if (n <= 0) {
            close(sv[0]);
            waitpid(pid, &status, 0);
            free(payload);
            fail(name, "short read before EOF");
            return;
        }
        off += n;
    }
    close(sv[0]);
    waitpid(pid, &status, 0);

    if (check_yt_inline_payload(payload, WEBKIT_YT_APP_JS_SIZE, "stream page chunks") < 0) {
        free(payload);
        fail(name, "payload mismatch");
        return;
    }
    free(payload);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "sender process failed");
        return;
    }
    pass(name);
}

static void test_parent_child_socket_handoff(void)
{
    const char *name = "parent/child AF_UNIX socket handoff";
    int sv[2];
    int pid;
    int status = 0;
    char c = 0;

    if (socketpair_raw(SOCK_STREAM, sv) < 0) {
        fail(name, "socketpair failed");
        return;
    }
    pid = fork();
    if (pid < 0) {
        close(sv[0]); close(sv[1]);
        fail(name, "fork failed");
        return;
    }
    if (pid == 0) {
        close(sv[0]);
        if (read(sv[1], &c, 1) != 1 || c != 'Q')
            exit(2);
        c = 'R';
        if (write(sv[1], &c, 1) != 1)
            exit(3);
        close(sv[1]);
        exit(0);
    }

    close(sv[1]);
    c = 'Q';
    if (write(sv[0], &c, 1) != 1 || read(sv[0], &c, 1) != 1 || c != 'R') {
        close(sv[0]);
        fail(name, "handoff ping-pong failed");
        return;
    }
    waitpid(pid, &status, 0);
    close(sv[0]);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "child handoff exit failed");
        return;
    }
    pass(name);
}

static void test_fd_pressure_cleanup(void)
{
    const char *name = "fd pressure create/close cleanup";
    int sv[8][2];
    int made = 0;

    for (int i = 0; i < 8; i++) {
        if (socketpair_raw(SOCK_STREAM | SOCK_CLOEXEC, sv[i]) < 0)
            break;
        made++;
    }
    if (made < 2) {
        fail(name, "could not allocate enough socketpairs");
        for (int i = 0; i < made; i++) {
            close(sv[i][0]);
            close(sv[i][1]);
        }
        return;
    }
    for (int i = 0; i < made; i++) {
        close(sv[i][0]);
        close(sv[i][1]);
    }

    if (socketpair_raw(SOCK_STREAM, sv[0]) < 0) {
        fail(name, "socketpair failed after cleanup");
        return;
    }
    close(sv[0][0]);
    close(sv[0][1]);
    pass(name);
}

static void test_memfd_shared_mapping(void)
{
    const char *name = "memfd ftruncate MAP_SHARED";
    int fd = memfd_create_raw("webkit-shm", MFD_CLOEXEC);
    char *p;
    int status = 0;
    int pid;

    if (fd < 0) {
        fail(name, "memfd_create failed");
        return;
    }
    if (ftruncate(fd, 4096) < 0) {
        close(fd);
        fail(name, "ftruncate failed");
        return;
    }
    p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        fail(name, "mmap failed");
        return;
    }
    p[0] = 'p';
    pid = fork();
    if (pid < 0) {
        munmap(p, 4096);
        close(fd);
        fail(name, "fork failed");
        return;
    }
    if (pid == 0) {
        p[1] = (p[0] == 'p') ? 'c' : 'x';
        exit(0);
    }
    waitpid(pid, &status, 0);
    if (p[1] != 'c') {
        munmap(p, 4096);
        close(fd);
        fail(name, "child update not visible");
        return;
    }
    if (mprotect(p, 4096, PROT_READ | PROT_WRITE) < 0) {
        munmap(p, 4096);
        close(fd);
        fail(name, "mprotect RW failed");
        return;
    }

    munmap(p, 4096);
    close(fd);
    pass(name);
}

static void test_native_memfd_syscall_alias(void)
{
    const char *name = "native memfd_create syscall alias";
    int fd = memfd_create_native_raw("webkit-native-shm", MFD_CLOEXEC);
    char *p;

    if (fd < 0) {
        fail(name, "native-number memfd_create failed");
        return;
    }
    if (ftruncate(fd, 4096) < 0) {
        close(fd);
        fail(name, "ftruncate failed");
        return;
    }
    p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        fail(name, "mmap failed");
        return;
    }
    p[0] = 'N';
    if (p[0] != 'N') {
        munmap(p, 4096);
        close(fd);
        fail(name, "mapping content mismatch");
        return;
    }
    munmap(p, 4096);
    close(fd);
    pass(name);
}

static void test_large_memfd_shared_mapping(void)
{
    const char *name = "large memfd shared-memory object";
    int fd = memfd_create_raw("webkit-large-shm", MFD_CLOEXEC);
    char *p;

    if (fd < 0) {
        fail(name, "memfd_create failed");
        return;
    }
    if (ftruncate(fd, LARGE_SHM_SIZE) < 0) {
        close(fd);
        fail(name, "large ftruncate failed");
        return;
    }
    p = mmap(0, LARGE_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        fail(name, "large mmap failed");
        return;
    }
    p[0] = 'A';
    p[LARGE_SHM_SIZE / 2] = 'B';
    p[LARGE_SHM_SIZE - 1] = 'C';
    if (p[0] != 'A' || p[LARGE_SHM_SIZE / 2] != 'B' ||
        p[LARGE_SHM_SIZE - 1] != 'C') {
        munmap(p, LARGE_SHM_SIZE);
        close(fd);
        fail(name, "large mapping content mismatch");
        return;
    }
    munmap(p, LARGE_SHM_SIZE);
    close(fd);
    pass(name);
}

static uchar shared_resource_byte(uint index)
{
    uint x = index * 1103515245u + 12345u;
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    return (uchar)x;
}

static void test_large_memfd_scm_resource_mapping(void)
{
    const char *name = "large memfd SCM_RIGHTS shared resource";
    int fd = memfd_create_raw("webkit-shareable-resource", MFD_CLOEXEC);
    int sv[2];
    int pid;
    int status = 0;
    char *p;

    if (fd < 0) {
        fail(name, "memfd_create failed");
        return;
    }
    if (ftruncate(fd, SHAREABLE_RESOURCE_SIZE) < 0) {
        close(fd);
        fail(name, "ftruncate failed");
        return;
    }
    p = mmap(0, SHAREABLE_RESOURCE_SIZE, PROT_READ | PROT_WRITE,
             MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        fail(name, "writer mmap failed");
        return;
    }
    for (uint i = 0; i < SHAREABLE_RESOURCE_SIZE; i++)
        p[i] = (char)shared_resource_byte(i);

    if (socketpair_raw(SOCK_STREAM | SOCK_CLOEXEC, sv) < 0) {
        munmap(p, SHAREABLE_RESOURCE_SIZE);
        close(fd);
        fail(name, "socketpair failed");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        munmap(p, SHAREABLE_RESOURCE_SIZE);
        close(fd);
        fail(name, "fork failed");
        return;
    }
    if (pid == 0) {
        char byte = 0;
        char control[CMSG_SPACE(sizeof(int))];
        struct cmsghdr *cmsg;
        struct iovec iov;
        struct msghdr msg;
        int got_fd = -1;
        char *rp;

        close(sv[0]);
        memset(control, 0, sizeof(control));
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = &byte;
        iov.iov_len = sizeof(byte);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        if (recvmsg_raw(sv[1], &msg, MSG_CMSG_CLOEXEC) != 1 || byte != 'R')
            exit(11);
        if (msg.msg_controllen < CMSG_LEN(sizeof(int)))
            exit(12);
        cmsg = (struct cmsghdr *)control;
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            exit(13);
        memcpy(&got_fd, CMSG_DATA(cmsg), sizeof(got_fd));
        if (got_fd < 0)
            exit(14);
        rp = mmap(0, SHAREABLE_RESOURCE_SIZE, PROT_READ, MAP_SHARED,
                  got_fd, 0);
        if (rp == MAP_FAILED)
            exit(15);
        for (uint i = 0; i < SHAREABLE_RESOURCE_SIZE; i++) {
            if ((uchar)rp[i] != shared_resource_byte(i)) {
                fprintf(2, "webkitabitest: shared resource mismatch at %u got=%u want=%u\n",
                        i, (uchar)rp[i], shared_resource_byte(i));
                exit(16);
            }
        }
        munmap(rp, SHAREABLE_RESOURCE_SIZE);
        close(got_fd);
        close(sv[1]);
        exit(0);
    }

    close(sv[1]);
    char byte = 'R';
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg = (struct cmsghdr *)control;
    struct iovec iov;
    struct msghdr msg;
    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = &byte;
    iov.iov_len = sizeof(byte);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    if (sendmsg_raw(sv[0], &msg, 0) != 1) {
        close(sv[0]);
        waitpid(pid, &status, 0);
        munmap(p, SHAREABLE_RESOURCE_SIZE);
        close(fd);
        fail(name, "sendmsg failed");
        return;
    }

    close(sv[0]);
    waitpid(pid, &status, 0);
    munmap(p, SHAREABLE_RESOURCE_SIZE);
    close(fd);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "reader process failed");
        return;
    }
    pass(name);
}

static uchar yt_resource_byte(uint index)
{
    uint x = index * 1664525u + 1013904223u;
    x ^= x >> 15;
    x *= 3266489917u;
    x ^= x >> 16;
    return (uchar)x;
}

static void test_webkit_ool_seqpacket_resource_mapping(void)
{
    const char *name = "WebKit OOL seqpacket shared resource";
    int fd = memfd_create_raw("webkit-ool-resource", MFD_CLOEXEC);
    int sv[2];
    int pid;
    int status = 0;
    char *p;

    if (fd < 0) {
        fail(name, "memfd_create failed");
        return;
    }
    if (ftruncate(fd, WEBKIT_YT_RESOURCE_SIZE) < 0) {
        close(fd);
        fail(name, "ftruncate failed");
        return;
    }
    p = mmap(0, WEBKIT_YT_RESOURCE_SIZE, PROT_READ | PROT_WRITE,
             MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        fail(name, "writer mmap failed");
        return;
    }
    for (uint i = 0; i < WEBKIT_YT_RESOURCE_SIZE; i++)
        p[i] = (char)yt_resource_byte(i);

    if (socketpair_raw(SOCK_SEQPACKET | SOCK_CLOEXEC, sv) < 0) {
        munmap(p, WEBKIT_YT_RESOURCE_SIZE);
        close(fd);
        fail(name, "socketpair failed");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        munmap(p, WEBKIT_YT_RESOURCE_SIZE);
        close(fd);
        fail(name, "fork failed");
        return;
    }
    if (pid == 0) {
        struct {
            uint magic;
            uint size;
        } hdr;
        char control[CMSG_SPACE(sizeof(int))];
        struct iovec iov;
        struct msghdr msg;
        int got_fd = -1;
        char *rp;

        close(sv[0]);
        memset(&hdr, 0, sizeof(hdr));
        memset(control, 0, sizeof(control));
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = &hdr;
        iov.iov_len = sizeof(hdr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        if (recvmsg_raw(sv[1], &msg, MSG_CMSG_CLOEXEC) != (int)sizeof(hdr))
            exit(21);
        if ((msg.msg_flags & MSG_TRUNC) || hdr.magic != IPC_STRESS_MAGIC ||
            hdr.size != WEBKIT_YT_RESOURCE_SIZE)
            exit(22);
        if (msg.msg_controllen < CMSG_LEN(sizeof(int)))
            exit(23);
        struct cmsghdr *cmsg = (struct cmsghdr *)control;
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            exit(24);
        memcpy(&got_fd, CMSG_DATA(cmsg), sizeof(got_fd));
        if (got_fd < 0)
            exit(25);

        rp = mmap(0, WEBKIT_YT_RESOURCE_SIZE, PROT_READ, MAP_SHARED,
                  got_fd, 0);
        if (rp == MAP_FAILED)
            exit(26);
        for (uint i = 0; i < WEBKIT_YT_RESOURCE_SIZE; i++) {
            if ((uchar)rp[i] != yt_resource_byte(i)) {
                fprintf(2, "webkitabitest: OOL resource mismatch at %u got=%u want=%u\n",
                        i, (uchar)rp[i], yt_resource_byte(i));
                exit(27);
            }
        }
        munmap(rp, WEBKIT_YT_RESOURCE_SIZE);
        close(got_fd);
        close(sv[1]);
        exit(0);
    }

    close(sv[1]);
    struct {
        uint magic;
        uint size;
    } hdr;
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg = (struct cmsghdr *)control;
    struct iovec iov;
    struct msghdr msg;

    hdr.magic = IPC_STRESS_MAGIC;
    hdr.size = WEBKIT_YT_RESOURCE_SIZE;
    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = &hdr;
    iov.iov_len = sizeof(hdr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    if (sendmsg_raw(sv[0], &msg, 0) != (int)sizeof(hdr)) {
        close(sv[0]);
        waitpid(pid, &status, 0);
        munmap(p, WEBKIT_YT_RESOURCE_SIZE);
        close(fd);
        fail(name, "sendmsg failed");
        return;
    }

    munmap(p, WEBKIT_YT_RESOURCE_SIZE);
    close(fd);
    close(sv[0]);
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "reader process failed");
        return;
    }
    pass(name);
}

static void test_vfs_cache_shape(void)
{
    const char *name = "VFS cache-shape operations";
    int fd;
    struct stat st;
    struct statfs sfs;
    char buf[4];

    printf("TRACE: vfs-cache cleanup\n");
    unlink("__webkit_cache/a");
    unlink("__webkit_cache/b");
    unlink("__webkit_cache/renamed");
    unlink("__webkit_cache");
    printf("TRACE: vfs-cache mkdir\n");
    mkdir("__webkit_cache");

    printf("TRACE: vfs-cache open a\n");
    fd = open("__webkit_cache/a", O_CREAT | O_RDWR);
    if (fd < 0) {
        fail(name, "open nested file failed");
        return;
    }
    printf("TRACE: vfs-cache write\n");
    if (write(fd, "cache", 5) != 5) {
        close(fd);
        fail(name, "write failed");
        return;
    }
    printf("TRACE: vfs-cache fsync\n");
    if (fsync_raw(fd) < 0) {
        close(fd);
        fail(name, "fsync failed");
        return;
    }
    printf("TRACE: vfs-cache fdatasync\n");
    if (fdatasync_raw(fd) < 0) {
        close(fd);
        fail(name, "fdatasync failed");
        return;
    }
    printf("TRACE: vfs-cache ftruncate\n");
    if (ftruncate(fd, 8192) < 0) {
        close(fd);
        fail(name, "ftruncate failed");
        return;
    }
    printf("TRACE: vfs-cache fstat\n");
    if (fstat(fd, &st) < 0 || st.st_size < 8192) {
        close(fd);
        fail(name, "fstat failed");
        return;
    }
    printf("TRACE: vfs-cache fstatfs\n");
    if (fstatfs_raw(fd, &sfs) < 0) {
        close(fd);
        fail(name, "fstatfs failed");
        return;
    }
    printf("TRACE: vfs-cache statfs\n");
    if (statfs_raw("__webkit_cache", &sfs) < 0) {
        close(fd);
        fail(name, "statfs failed");
        return;
    }
    printf("TRACE: vfs-cache unlink-open\n");
    if (unlink("__webkit_cache/a") < 0) {
        close(fd);
        fail(name, "unlink while open failed");
        return;
    }
    printf("TRACE: vfs-cache read-unlinked\n");
    lseek(fd, 0, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 4) != 4 || memcmp(buf, "cach", 4) != 0) {
        close(fd);
        fail(name, "open unlinked file lost contents");
        return;
    }
    printf("TRACE: vfs-cache close-unlinked\n");
    close(fd);

    printf("TRACE: vfs-cache open b\n");
    fd = open("__webkit_cache/b", O_CREAT | O_RDWR);
    if (fd < 0) {
        fail(name, "open rename source failed");
        return;
    }
    close(fd);
    printf("TRACE: vfs-cache open renamed\n");
    fd = open("__webkit_cache/renamed", O_CREAT | O_RDWR);
    if (fd < 0) {
        fail(name, "open rename destination failed");
        return;
    }
    close(fd);
    printf("TRACE: vfs-cache rename-over\n");
    if (rename("__webkit_cache/b", "__webkit_cache/renamed") < 0) {
        fail(name, "rename over existing failed");
        return;
    }

    printf("TRACE: vfs-cache final cleanup\n");
    unlink("__webkit_cache/renamed");
    unlink("__webkit_cache");
    pass(name);
}

static void test_advisory_locks(void)
{
    const char *name = "advisory file locks";
    int fd;
    struct flock fl;

    unlink("__webkit_lock");
    fd = open("__webkit_lock", O_CREAT | O_RDWR);
    if (fd < 0) {
        fail(name, "open failed");
        return;
    }
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 1;
    if (fcntl_addr_raw(fd, F_SETLK, &fl) < 0) {
        close(fd);
        unlink("__webkit_lock");
        fail(name, "F_SETLK write lock failed");
        return;
    }
    fl.l_type = F_UNLCK;
    if (fcntl_addr_raw(fd, F_SETLK, &fl) < 0) {
        close(fd);
        unlink("__webkit_lock");
        fail(name, "F_SETLK unlock failed");
        return;
    }
    close(fd);
    unlink("__webkit_lock");
    pass(name);
}

static void test_mmap_file_truncate(void)
{
    const char *name = "mmap-backed file writes and truncate";
    int fd;
    char *p;
    struct stat st;

    unlink("__webkit_mmap_file");
    fd = open("__webkit_mmap_file", O_CREAT | O_RDWR);
    if (fd < 0) {
        fail(name, "open failed");
        return;
    }
    if (ftruncate(fd, 8192) < 0) {
        close(fd);
        fail(name, "initial ftruncate failed");
        return;
    }
    p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        fail(name, "mmap failed");
        return;
    }
    p[0] = 'M';
    p[4096] = 'N';
    if (msync(p, 8192, MS_SYNC) < 0) {
        munmap(p, 8192);
        close(fd);
        fail(name, "msync failed");
        return;
    }
    if (ftruncate(fd, 4096) < 0 || fstat(fd, &st) < 0 || st.st_size != 4096) {
        munmap(p, 8192);
        close(fd);
        fail(name, "truncate-after-mmap failed");
        return;
    }
    munmap(p, 8192);
    close(fd);
    unlink("__webkit_mmap_file");
    pass(name);
}

static void test_waitpid_reap(void)
{
    const char *name = "waitpid WNOHANG child cleanup";
    int pid = fork();
    int status = 0;
    int spins = 0;
    int got;

    if (pid < 0) {
        fail(name, "fork failed");
        return;
    }
    if (pid == 0)
        exit(7);

    do {
        got = waitpid(pid, &status, WNOHANG);
        if (got == 0)
            sleep(1);
        spins++;
    } while (got == 0 && spins < 50);

    if (got != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
        char why[96];
        snprintf(why, sizeof(why), "got=%d pid=%d status=0x%x exit=%d",
                 got, pid, status, WEXITSTATUS(status));
        fail(name, why);
        return;
    }
    pass(name);
}

static void test_timerfd_poll(void)
{
    const char *name = "timerfd poll timeout";
    int fd = timerfd_create_raw(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec its;
    struct pollfd pfd;
    uint64 expirations = 0;

    if (fd < 0) {
        fail(name, "timerfd_create failed");
        return;
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 20 * 1000 * 1000;
    if (timerfd_settime_raw(fd, 0, &its, 0) < 0) {
        close(fd);
        fail(name, "timerfd_settime failed");
        return;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll_raw(&pfd, 1, 1000) <= 0 || !(pfd.revents & POLLIN)) {
        close(fd);
        fail(name, "timerfd did not become readable");
        return;
    }
    if (read(fd, &expirations, sizeof(expirations)) != (int)sizeof(expirations) ||
        expirations == 0) {
        close(fd);
        fail(name, "timerfd read failed");
        return;
    }
    close(fd);
    pass(name);
}

static void test_futex_timeout(void)
{
    const char *name = "futex timed waits";
    volatile uint32 word = 1;
    struct timespec rel = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
    struct timespec abs;
    int rc;

    rc = futex_raw((uint32 *)&word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                   2, &rel, 0, 0);
    if (rc != -EAGAIN) {
        fail(name, "relative wait did not reject mismatched value");
        return;
    }

    rc = futex_raw((uint32 *)&word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                   1, &rel, 0, 0);
    if (rc != -ETIMEDOUT) {
        fail(name, "relative wait did not time out");
        return;
    }

    if (clock_gettime_raw(CLOCK_MONOTONIC, &abs) < 0) {
        fail(name, "clock_gettime failed");
        return;
    }
    abs.tv_nsec += 20 * 1000 * 1000;
    if (abs.tv_nsec >= 1000000000LL) {
        abs.tv_sec++;
        abs.tv_nsec -= 1000000000LL;
    }

    rc = futex_raw((uint32 *)&word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                   1, &abs, 0, FUTEX_BITSET_MATCH_ANY);
    if (rc != -ETIMEDOUT) {
        fail(name, "absolute bitset wait did not time out");
        return;
    }

    rc = futex_raw((uint32 *)&word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                   1, 0, 0, 0);
    if (rc != 0) {
        fail(name, "wake on empty queue failed");
        return;
    }

    pass(name);
}

static void test_random_devices(void)
{
    const char *name = "/dev/random and /dev/urandom";
    char buf[8];
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd < 0 || read(fd, buf, sizeof(buf)) != (int)sizeof(buf)) {
        if (fd >= 0) close(fd);
        fail(name, "/dev/urandom unreadable");
        return;
    }
    close(fd);

    fd = open("/dev/random", O_RDONLY);
    if (fd < 0 || read(fd, buf, sizeof(buf)) != (int)sizeof(buf)) {
        if (fd >= 0) close(fd);
        fail(name, "/dev/random unreadable");
        return;
    }
    close(fd);
    pass(name);
}

static void test_executable_memory_policy(void)
{
    const char *name = "executable memory policy";
    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *x;

    if (p == MAP_FAILED) {
        fail(name, "anonymous RW mmap failed");
        return;
    }
    p[0] = 0;
    if (mprotect(p, 4096, PROT_READ | PROT_EXEC) < 0) {
        munmap(p, 4096);
        fail(name, "RW-to-RX mprotect failed");
        return;
    }
    munmap(p, 4096);

    x = mmap(0, 4096, PROT_READ | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (x == MAP_FAILED) {
        skip(name, "direct PROT_EXEC mmap rejected by kernel policy");
        return;
    }
    munmap(x, 4096);
    pass(name);
}

static void test_memory_locking_abi(void)
{
    const char *name = "memory locking ABI";
    char *p = mmap(0, 8192, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED) {
        fail(name, "anonymous mmap failed");
        return;
    }
    p[0] = 'L';

    if (mlock2_raw(p + 17, 4096, 0) < 0 ||
        mlock2_raw(p, 4096, MLOCK_ONFAULT) < 0 ||
        mlockall_raw(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) < 0 ||
        munlock_raw(p + 33, 1024) < 0 ||
        munlockall_raw() < 0) {
        munmap(p, 8192);
        fail(name, "expected lock/unlock call failed");
        return;
    }
    if (mlock2_raw(p, 4096, 0x80) != -EINVAL ||
        mlockall_raw(0x80) != -EINVAL) {
        munmap(p, 8192);
        fail(name, "invalid flags were accepted");
        return;
    }

    munmap(p, 8192);
    pass(name);
}

static void test_procfs_meminfo_webkit_parse(void)
{
    const char *name = "procfs meminfo WebKit parse";
    char buf[4096];
    int fd = open("/proc/meminfo", O_RDONLY);
    int n;
    size_t memory_available = (size_t)-1;
    size_t memory_total = (size_t)-1;
    size_t memory_free = (size_t)-1;
    size_t active_file = (size_t)-1;
    size_t inactive_file = (size_t)-1;
    size_t slab_reclaimable = (size_t)-1;

    if (fd < 0) {
        fail(name, "open /proc/meminfo failed");
        return;
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        fail(name, "lseek /proc/meminfo failed");
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        fail(name, "read /proc/meminfo failed");
        return;
    }
    buf[n] = '\0';

    char *line = buf;
    while (*line != '\0') {
        char *next = strchr(line, '\n');
        if (next != 0)
            *next++ = '\0';
        char *colon = strchr(line, ':');
        if (colon != 0) {
            char *p = colon + 1;
            size_t amount = 0;
            while (*p == ' ' || *p == '\t')
                p++;
            while (*p >= '0' && *p <= '9') {
                amount = amount * 10 + (size_t)(*p - '0');
                p++;
            }
            if (memcmp(line, "MemTotal:", 9) == 0)
                memory_total = amount;
            else if (memcmp(line, "MemFree:", 8) == 0)
                memory_free = amount;
            else if (memcmp(line, "MemAvailable:", 13) == 0)
                memory_available = amount;
            else if (memcmp(line, "Active(file):", 13) == 0)
                active_file = amount;
            else if (memcmp(line, "Inactive(file):", 15) == 0)
                inactive_file = amount;
            else if (memcmp(line, "SReclaimable:", 13) == 0)
                slab_reclaimable = amount;
        }
        if (next == 0)
            break;
        line = next;
    }

    if (memory_total == 0 || memory_total == (size_t)-1) {
        fail(name, "missing MemTotal");
        return;
    }
    if (memory_free == (size_t)-1 || memory_available == (size_t)-1 ||
        active_file == (size_t)-1 || inactive_file == (size_t)-1 ||
        slab_reclaimable == (size_t)-1) {
        fail(name, "missing WebKit meminfo token");
        return;
    }
    if (memory_available > memory_total) {
        fail(name, "MemAvailable exceeds MemTotal");
        return;
    }
    pass(name);
}

static int text_contains(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);

    if (needle_len == 0)
        return 1;
    for (const char *p = haystack; *p != '\0'; p++) {
        if (memcmp(p, needle, needle_len) == 0)
            return 1;
    }
    return 0;
}

static void test_procfs_status_linux_shape(void)
{
    const char *name = "procfs status Linux shape";
    char buf[4096];
    int fd = open("/proc/self/status", O_RDONLY);
    int n;

    if (fd < 0) {
        fail(name, "open /proc/self/status failed");
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        fail(name, "read /proc/self/status failed");
        return;
    }
    buf[n] = '\0';

    if (!text_contains(buf, "Tgid:\t") ||
        !text_contains(buf, "VmRSS:\t") ||
        !text_contains(buf, "RssAnon:\t") ||
        !text_contains(buf, "Threads:\t") ||
        !text_contains(buf, "SigPnd:\t") ||
        !text_contains(buf, "CapEff:\t") ||
        !text_contains(buf, "Cpus_allowed_list:\t")) {
        fail(name, "missing Linux status token");
        return;
    }
    pass(name);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "checkclosed") == 0) {
        int fd = atoi(argv[2]);
        exit(check_fd_closed_after_exec(fd) == 0 ? 0 : 1);
    }

    printf("webkitabitest: WebKit-shaped xv6 ABI checks\n");

    test_socketpair_stream();
    test_socketpair_seqpacket_policy();
    test_socket_nonblock_poll();
    test_socket_nonblock_connect();
    test_socket_nonblock_connect_epoll();
    test_socket_sol_options();
    test_socket_cloexec_exec();
    test_scm_rights_batch();
    test_scm_rights_stream_first_byte_barrier();
    test_scm_rights_recvmmsg_batch();
    test_scm_rights_process_lifetime();
    test_ipc_stream_stress();
    test_ipc_seqpacket_stress();
    test_webkit_large_inline_ipc(SOCK_SEQPACKET);
    test_webkit_large_inline_ipc(SOCK_STREAM);
    test_webkit_seqpacket_chunk_transfer();
    test_webkit_seqpacket_chunk_burst_queue();
    test_webkit_stream_page_chunk_transfer();
    test_parent_child_socket_handoff();
    test_fd_pressure_cleanup();
    test_memfd_shared_mapping();
    test_native_memfd_syscall_alias();
    test_large_memfd_shared_mapping();
    test_large_memfd_scm_resource_mapping();
    test_webkit_ool_seqpacket_resource_mapping();
    test_vfs_cache_shape();
    test_advisory_locks();
    test_mmap_file_truncate();
    test_waitpid_reap();
    test_timerfd_poll();
    test_futex_timeout();
    test_random_devices();
    test_executable_memory_policy();
    test_memory_locking_abi();
    test_procfs_meminfo_webkit_parse();
    test_procfs_status_linux_shape();

    printf("webkitabitest: %d passed, %d skipped, %d failed\n",
           passed, skipped, failed);
    exit(failed == 0 ? 0 : 1);
}

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
#define SCM_RIGHTS 1
#define MSG_DONTWAIT 0x40
#define MSG_CMSG_CLOEXEC 0x40000000
#define MFD_CLOEXEC 0x0001
#define CLOCK_MONOTONIC 1
#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC
#define LARGE_SHM_SIZE (1024 * 1024)

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct cmsghdr {
    uint64 cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

struct msghdr {
    void *msg_name;
    uint32 msg_namelen;
    uint32 __pad0;
    struct iovec *msg_iov;
    uint64 msg_iovlen;
    void *msg_control;
    uint64 msg_controllen;
    int msg_flags;
};

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
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

static int sendmsg_raw(int fd, struct msghdr *msg, int flags)
{
    return (int)raw_syscall3(SYS_sendmsg, fd, (int64)msg, flags);
}

static int recvmsg_raw(int fd, struct msghdr *msg, int flags)
{
    return (int)raw_syscall3(SYS_recvmsg, fd, (int64)msg, flags);
}

static int poll_raw(struct pollfd *fds, int nfds, int timeout)
{
    return (int)raw_syscall3(SYS_poll, (int64)fds, nfds, timeout);
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

static int timerfd_create_raw(int clockid, int flags)
{
    return (int)raw_syscall2(SYS_timerfd_create, clockid, flags);
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
    const char *name = "AF_UNIX SOCK_SEQPACKET policy";
    int sv[2];
    int rc = socketpair_raw(SOCK_SEQPACKET, sv);

    if (rc == 0) {
        close(sv[0]);
        close(sv[1]);
        pass(name);
        return;
    }

    skip(name, "not implemented; WebKit must keep SOCK_STREAM IPC override");
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

static void test_vfs_cache_shape(void)
{
    const char *name = "VFS cache-shape operations";
    int fd;
    struct stat st;
    struct statfs sfs;
    char buf[4];

    unlink("__webkit_cache/a");
    unlink("__webkit_cache/b");
    unlink("__webkit_cache/renamed");
    unlink("__webkit_cache");
    mkdir("__webkit_cache");

    fd = open("__webkit_cache/a", O_CREAT | O_RDWR);
    if (fd < 0) {
        fail(name, "open nested file failed");
        return;
    }
    if (write(fd, "cache", 5) != 5 || fsync_raw(fd) < 0 ||
        fdatasync_raw(fd) < 0 || ftruncate(fd, 8192) < 0 ||
        fstat(fd, &st) < 0 || st.st_size < 8192 ||
        fstatfs_raw(fd, &sfs) < 0 || statfs_raw("__webkit_cache", &sfs) < 0) {
        close(fd);
        fail(name, "write/sync/truncate/stat failed");
        return;
    }
    if (unlink("__webkit_cache/a") < 0) {
        close(fd);
        fail(name, "unlink while open failed");
        return;
    }
    lseek(fd, 0, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 4) != 4 || memcmp(buf, "cach", 4) != 0) {
        close(fd);
        fail(name, "open unlinked file lost contents");
        return;
    }
    close(fd);

    fd = open("__webkit_cache/b", O_CREAT | O_RDWR);
    if (fd < 0) {
        fail(name, "open rename source failed");
        return;
    }
    close(fd);
    fd = open("__webkit_cache/renamed", O_CREAT | O_RDWR);
    if (fd < 0) {
        fail(name, "open rename destination failed");
        return;
    }
    close(fd);
    if (rename("__webkit_cache/b", "__webkit_cache/renamed") < 0) {
        fail(name, "rename over existing failed");
        return;
    }

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
    test_socket_cloexec_exec();
    test_scm_rights_batch();
    test_scm_rights_process_lifetime();
    test_parent_child_socket_handoff();
    test_fd_pressure_cleanup();
    test_memfd_shared_mapping();
    test_large_memfd_shared_mapping();
    test_vfs_cache_shape();
    test_advisory_locks();
    test_mmap_file_truncate();
    test_waitpid_reap();
    test_timerfd_poll();
    test_random_devices();
    test_executable_memory_policy();

    printf("webkitabitest: %d passed, %d skipped, %d failed\n",
           passed, skipped, failed);
    exit(failed == 0 ? 0 : 1);
}

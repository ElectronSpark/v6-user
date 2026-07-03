// linuxsyscallabitest.c - raw Linux-number syscall compatibility tests.
#include "kernel/inc/types.h"
#ifdef HOST_LIBC_PROGRAM
#include <sched.h>
#if defined(__has_include)
#if __has_include(<sys/rseq.h>)
#include <sys/rseq.h>
#define HAVE_GLIBC_RSEQ 1
#endif
#endif
#else
#include "kernel/inc/clone_flags.h"
#endif
#include "kernel/inc/elf.h"
#include "kernel/inc/uabi/fcntl.h"
#include "kernel/inc/uabi/stat.h"
#include "kernel/inc/uabi/statfs.h"
#include "user/user.h"

#define STACK_SIZE (4096 * 4)
#define LINUX_KERNEL_SIGSET_SIZE 8

#if defined(__x86_64__)
#define LINUX_NR_READ 0
#define LINUX_NR_OPEN 2
#define LINUX_NR_CLOSE 3
#define LINUX_NR_STAT 4
#define LINUX_NR_FSTAT 5
#define LINUX_NR_LSTAT 6
#define LINUX_NR_FCNTL 72
#define LINUX_NR_RT_SIGACTION 13
#define LINUX_NR_RT_SIGPROCMASK 14
#define LINUX_NR_MUNMAP 11
#define LINUX_NR_LSEEK 8
#define LINUX_NR_IOCTL 16
#define LINUX_NR_EXIT 60
#define LINUX_NR_MREMAP 25
#define LINUX_NR_MSYNC 26
#define LINUX_NR_MINCORE 27
#define LINUX_NR_MADVISE 28
#define LINUX_NR_PWRITE64 18
#define LINUX_NR_ACCESS 21
#define LINUX_NR_CLONE 56
#define LINUX_NR_ARCH_PRCTL 158
#define LINUX_NR_GETGID 104
#define LINUX_NR_GETEUID 107
#define LINUX_NR_GETEGID 108
#define LINUX_NR_SETREUID 113
#define LINUX_NR_SETREGID 114
#define LINUX_NR_GETRESUID 118
#define LINUX_NR_GETRESGID 120
#define LINUX_NR_RT_SIGPENDING 127
#define LINUX_NR_RT_SIGTIMEDWAIT 128
#define LINUX_NR_RT_SIGSUSPEND 130
#define LINUX_NR_GETUID 102
#define LINUX_NR_GETPPID 110
#define LINUX_NR_STATFS 137
#define LINUX_NR_FSTATFS 138
#define LINUX_NR_MLOCK 149
#define LINUX_NR_MUNLOCK 150
#define LINUX_NR_MLOCKALL 151
#define LINUX_NR_MUNLOCKALL 152
#define LINUX_NR_PRCTL 157
#define LINUX_NR_SYSINFO 99
#define LINUX_NR_READV 19
#define LINUX_NR_WRITEV 20
#define LINUX_NR_GETDENTS 78
#define LINUX_NR_GETCWD 79
#define LINUX_NR_GETTID 186
#define LINUX_NR_READAHEAD 187
#define LINUX_NR_SCHED_SETAFFINITY 203
#define LINUX_NR_SCHED_GETAFFINITY 204
#define LINUX_NR_GETDENTS64 217
#define LINUX_NR_SET_TID_ADDRESS 218
#define LINUX_NR_CLOCK_SETTIME 227
#define LINUX_NR_CLOCK_GETTIME 228
#define LINUX_NR_CLOCK_GETRES 229
#define LINUX_NR_OPENAT 257
#define LINUX_NR_NEWFSTATAT 262
#define LINUX_NR_READLINKAT 267
#define LINUX_NR_FACCESSAT 269
#define LINUX_NR_PSELECT6 270
#define LINUX_NR_PPOLL 271
#define LINUX_NR_PIPE2 293
#define LINUX_NR_SELECT 23
#define LINUX_NR_SOCKET 41
#define LINUX_NR_CONNECT 42
#define LINUX_NR_BIND 49
#define LINUX_NR_LISTEN 50
#define LINUX_NR_FORK 57
#define LINUX_NR_FSYNC 74
#define LINUX_NR_FDATASYNC 75
#define LINUX_NR_TRUNCATE 76
#define LINUX_NR_FTRUNCATE 77
#define LINUX_NR_CHDIR 80
#define LINUX_NR_FCHDIR 81
#define LINUX_NR_RENAME 82
#define LINUX_NR_MKDIR 83
#define LINUX_NR_RMDIR 84
#define LINUX_NR_CREAT 85
#define LINUX_NR_LINK 86
#define LINUX_NR_UNLINK 87
#define LINUX_NR_SYMLINK 88
#define LINUX_NR_READLINK 89
#define LINUX_NR_CHMOD 90
#define LINUX_NR_CHOWN 92
#define LINUX_NR_TIMES 100
#define LINUX_NR_GETPGRP 111
#define LINUX_NR_SETFSUID 122
#define LINUX_NR_SETFSGID 123
#define LINUX_NR_CAPGET 125
#define LINUX_NR_CAPSET 126
#define LINUX_NR_UTIME 132
#define LINUX_NR_SETXATTR 188
#define LINUX_NR_GETXATTR 191
#define LINUX_NR_LISTXATTR 194
#define LINUX_NR_REMOVEXATTR 197
#define LINUX_NR_TIME 201
#define LINUX_NR_UTIMES 235
#define LINUX_NR_WAITID 247
#define LINUX_NR_IOPRIO_SET 251
#define LINUX_NR_IOPRIO_GET 252
#define LINUX_NR_INOTIFY_INIT 253
#define LINUX_NR_INOTIFY_ADD_WATCH 254
#define LINUX_NR_INOTIFY_RM_WATCH 255
#define LINUX_NR_SYNC_FILE_RANGE 277
#define LINUX_NR_SIGNALFD 282
#define LINUX_NR_EVENTFD 284
#define LINUX_NR_ACCEPT4 288
#define LINUX_NR_SIGNALFD4 289
#define LINUX_NR_INOTIFY_INIT1 294
#define LINUX_NR_GETCPU 309
#define LINUX_NR_SCHED_SETATTR 314
#define LINUX_NR_SCHED_GETATTR 315
#define LINUX_NR_COPY_FILE_RANGE 326
#define LINUX_NR_RSEQ 334
#define LINUX_NR_CLOSE_RANGE 436
#define LINUX_NR_OPENAT2 437
#define LINUX_NR_FCHMODAT2 452
#define LINUX_NR_DUP3 292
#define LINUX_NR_PREADV 295
#define LINUX_NR_PWRITEV 296
#define LINUX_NR_PREADV2 327
#define LINUX_NR_PWRITEV2 328
#define LINUX_NR_PRLIMIT64 302
#define LINUX_NR_SYNCFS 306
#define LINUX_NR_MEMBARRIER 324
#define LINUX_NR_MLOCK2 325
#define LINUX_NR_STATX 332
#define LINUX_NR_MOVE_PAGES 279
#else
#define LINUX_NR_MUNMAP 215
#define LINUX_NR_EXIT 93
#define LINUX_NR_IOCTL 29
#define LINUX_NR_CLOCK_GETTIME 113
#define LINUX_NR_CLOCK_GETRES 114
#define LINUX_NR_GETTIMEOFDAY 169
#define LINUX_NR_GETTID 178
#define LINUX_NR_PRCTL 167
#define LINUX_NR_SYSINFO 179
#define LINUX_NR_MMAP 222
#define LINUX_NR_MLOCK 228
#define LINUX_NR_MUNLOCK 229
#define LINUX_NR_MLOCKALL 230
#define LINUX_NR_MUNLOCKALL 231
#define LINUX_NR_PRLIMIT64 261
#define LINUX_NR_MEMBARRIER 283
#define LINUX_NR_MLOCK2 284
#define LINUX_NR_STATX 291
#endif

#define EPERM 1
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define EFAULT 14
#define ENOTDIR 20
#define EISDIR 21
#define ENOSYS 38
#define EINVAL 22
#define ENODEV 19
#define ELOOP 40
#ifndef ENOTSUP
#define ENOTSUP 95
#endif
#define EINPROGRESS 115
#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#define RLIMIT_NOFILE 7
#define MEMBARRIER_CMD_QUERY 0
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif
#ifndef MLOCK_ONFAULT
#define MLOCK_ONFAULT 0x1
#endif
#ifndef MCL_CURRENT
#define MCL_CURRENT 0x1
#endif
#ifndef MCL_FUTURE
#define MCL_FUTURE 0x2
#endif
#ifndef MCL_ONFAULT
#define MCL_ONFAULT 0x4
#endif
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#ifndef O_DIRECT
#define O_DIRECT 040000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0200000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0400000
#endif
#ifndef O_NOATIME
#define O_NOATIME 01000000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef O_TMPFILE
#define O_TMPFILE 020200000
#endif
#define O_RDWR 02
#define O_WRONLY 01
#define O_RDONLY 00
#define LINUX_SIG_SETMASK 2

#define P_ALL 0
#define P_PID 1
#define WEXITED 4
#define WNOHANG 1
#define RSEQ_FLAG_UNREGISTER 1
#define _LINUX_CAPABILITY_VERSION_3 0x20080522
#define LINUX_IN_MODIFY 0x00000002
#define LINUX_IN_NONBLOCK 00004000
#define LINUX_IN_CLOEXEC 02000000
#define LINUX_FIONREAD 0x541B
#define LINUX_SFD_NONBLOCK 00004000
#define LINUX_SFD_CLOEXEC 02000000
#define LINUX_SOCK_NONBLOCK 00004000
#define LINUX_SOCK_CLOEXEC 02000000
#define AF_UNIX 1
#define SOCK_STREAM 1
#define SYNC_FILE_RANGE_WAIT_BEFORE 1
#define SYNC_FILE_RANGE_WRITE 2
#define SYNC_FILE_RANGE_WAIT_AFTER 4
#define IOPRIO_WHO_PROCESS 1
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define X86_NONCANONICAL_LOW 0x0000800000000000ULL

struct linux_timeval {
    int64 tv_sec;
    int64 tv_usec;
};

struct linux_utimbuf {
    int64 actime;
    int64 modtime;
};

struct linux_tms {
    int64 tms_utime;
    int64 tms_stime;
    int64 tms_cutime;
    int64 tms_cstime;
};

struct linux_inotify_event_abi {
    int wd;
    uint32 mask;
    uint32 cookie;
    uint32 len;
};

struct linux_open_how {
    uint64 flags;
    uint64 mode;
    uint64 resolve;
};

static inline int64 raw_linux_syscall2(int64 num, int64 a0, int64 a1);
static inline int64 raw_linux_syscall3(int64 num, int64 a0, int64 a1,
                                       int64 a2);
static inline int64 raw_linux_syscall4(int64 num, int64 a0, int64 a1,
                                       int64 a2, int64 a3);

static void test_linux_inotify_fionread_reducer(void)
{
#if defined(__x86_64__)
    const char *path = "linuxabi.inotify.fionread";
    char b = 'x';
    char inotify_buf[sizeof(struct linux_inotify_event_abi) + 16];
    int fd = -1;
    int ifd = -1;
    int wd = -1;
    int inotify_empty_avail = -1;
    int inotify_avail = -1;
    int inotify_avail_after = -1;

    memset(inotify_buf, 0, sizeof(inotify_buf));
    unlink(path);

    fd = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD, (int64)path,
                            O_CREAT | O_RDWR | O_TRUNC, 0600);
    int create_write_ok = fd >= 0 && write(fd, &b, 1) == 1;
    if (fd >= 0)
        close(fd);

    ifd = raw_linux_syscall2(LINUX_NR_INOTIFY_INIT1,
                             LINUX_IN_NONBLOCK | LINUX_IN_CLOEXEC, 0);
    if (ifd >= 0)
        wd = raw_linux_syscall3(LINUX_NR_INOTIFY_ADD_WATCH, ifd,
                                (int64)path, LINUX_IN_MODIFY);

    int inotify_empty_ioctl =
        raw_linux_syscall3(LINUX_NR_IOCTL, ifd, LINUX_FIONREAD,
                           (int64)&inotify_empty_avail);
    int inotify_empty_read =
        raw_linux_syscall3(LINUX_NR_READ, ifd, (int64)inotify_buf,
                           sizeof(inotify_buf));

    fd = open(path, O_WRONLY);
    int inotify_write_ok = fd >= 0 && write(fd, &b, 1) == 1;
    if (fd >= 0)
        close(fd);

    int inotify_ready_ioctl =
        raw_linux_syscall3(LINUX_NR_IOCTL, ifd, LINUX_FIONREAD,
                           (int64)&inotify_avail);
    int inotify_read =
        raw_linux_syscall3(LINUX_NR_READ, ifd, (int64)inotify_buf,
                           sizeof(inotify_buf));
    struct linux_inotify_event_abi *iev =
        (struct linux_inotify_event_abi *)inotify_buf;
    int inotify_drained_ioctl =
        raw_linux_syscall3(LINUX_NR_IOCTL, ifd, LINUX_FIONREAD,
                           (int64)&inotify_avail_after);
    int inotify_rm = wd > 0
        ? raw_linux_syscall2(LINUX_NR_INOTIFY_RM_WATCH, ifd, wd)
        : -EINVAL;

    if (!create_write_ok || ifd < 0 || wd <= 0 ||
        inotify_empty_ioctl != 0 || inotify_empty_avail != 0 ||
        inotify_empty_read != -EAGAIN || !inotify_write_ok ||
        inotify_ready_ioctl != 0 ||
        inotify_avail < (int)sizeof(struct linux_inotify_event_abi) ||
        inotify_avail > (int)sizeof(inotify_buf) ||
        inotify_read != inotify_avail || iev->wd != wd ||
        (iev->mask & LINUX_IN_MODIFY) == 0 || inotify_drained_ioctl != 0 ||
        inotify_avail_after != 0 || inotify_rm != 0) {
        printf("linuxsyscallabitest: inotify FIONREAD reducer failed: "
               "create_write_ok=%d ifd=%d wd=%d empty_ioctl=%d "
               "empty_avail=%d empty_read=%d write_ok=%d ready_ioctl=%d "
               "ready_avail=%d read=%d ev_wd=%d ev_mask=0x%x "
               "drained_ioctl=%d drained_avail=%d rm=%d\n",
               create_write_ok, ifd, wd, inotify_empty_ioctl,
               inotify_empty_avail, inotify_empty_read, inotify_write_ok,
               inotify_ready_ioctl, inotify_avail, inotify_read, iev->wd,
               iev->mask, inotify_drained_ioctl, inotify_avail_after,
               inotify_rm);
        if (ifd >= 0)
            close(ifd);
        unlink(path);
        exit(1);
    }

    close(ifd);
    unlink(path);
    printf("linuxsyscallabitest: inotify FIONREAD reducer OK: "
           "ready_avail=%d read=%d mask=0x%x\n",
           inotify_avail, inotify_read, iev->mask);
#else
    printf("linuxsyscallabitest: inotify FIONREAD reducer skipped\n");
#endif
}

struct linux_pselect6_sigmask {
    uint64 ss;
    uint64 ss_len;
};

struct linux_cap_header {
    uint32 version;
    int pid;
};

struct linux_cap_data {
    uint32 effective;
    uint32 permitted;
    uint32 inheritable;
};

struct linux_sched_attr {
    uint32 size;
    uint32 sched_policy;
    uint64 sched_flags;
    int32 sched_nice;
    uint32 sched_priority;
    uint64 sched_runtime;
    uint64 sched_deadline;
    uint64 sched_period;
    uint32 sched_util_min;
    uint32 sched_util_max;
};

struct linux_timespec {
    int64 tv_sec;
    int64 tv_nsec;
};

struct linux_sockaddr_un {
    uint16 sun_family;
    char sun_path[108];
};

struct linux_dirent_compat {
    uint64 d_ino;
    uint64 d_off;
    uint16 d_reclen;
    char d_name[];
};

struct linux_dirent64_abi {
    uint64 d_ino;
    int64 d_off;
    uint16 d_reclen;
    uint8 d_type;
    char d_name[];
};

static volatile int child_entered;

#ifdef HAVE_GLIBC_RSEQ
static uint64 glibc_rseq_addr(void)
{
    if (__rseq_size == 0)
        return 0;
    return (uint64)((char *)__builtin_thread_pointer() + __rseq_offset);
}
#endif
static volatile int child_failed;
static char *child_stack;

#if defined(__x86_64__)
static inline int64 raw_linux_syscall0(int64 num)
{
    int64 ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_linux_syscall2(int64 num, int64 a0, int64 a1)
{
    int64 ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a0), "S"(a1)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_linux_syscall5(int64 num, int64 a0, int64 a1, int64 a2,
                                       int64 a3, int64 a4)
{
    int64 ret;
    register int64 r10 asm("r10") = a3;
    register int64 r8 asm("r8") = a4;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_linux_syscall6(int64 num, int64 a0, int64 a1, int64 a2,
                                       int64 a3, int64 a4, int64 a5)
{
    int64 ret;
    register int64 r10 asm("r10") = a3;
    register int64 r8 asm("r8") = a4;
    register int64 r9 asm("r9") = a5;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a0), "S"(a1), "d"(a2),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_linux_syscall3(int64 num, int64 a0, int64 a1, int64 a2)
{
    int64 ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a0), "S"(a1), "d"(a2)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_linux_syscall4(int64 num, int64 a0, int64 a1, int64 a2,
                                       int64 a3)
{
    int64 ret;
    register int64 r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

__attribute__((noreturn, noinline))
static void raw_linux_unmapself_child(void)
{
    child_entered = 1;
    asm volatile(
        "movq %[addr], %%rdi\n"
        "movq %[len], %%rsi\n"
        "movl %[linux_munmap], %%eax\n"
        "syscall\n"
        "xorq %%rdi, %%rdi\n"
        "movl %[linux_exit], %%eax\n"
        "syscall\n"
        "movl $1, child_failed(%%rip)\n"
        "xorq %%rdi, %%rdi\n"
        "movl $3, %%eax\n"
        "syscall\n"
        "1: jmp 1b\n"
        :
        : [addr] "r"(child_stack),
          [len] "r"((uint64)STACK_SIZE),
          [linux_munmap] "i"(LINUX_NR_MUNMAP),
          [linux_exit] "i"(LINUX_NR_EXIT)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory");
    for (;;)
        ;
}
#else
static inline int64 raw_linux_syscall0(int64 num)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = 0;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_linux_syscall2(int64 num, int64 a0v, int64 a1v)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a0v;
    register int64 a1 asm("a1") = a1v;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_linux_syscall3(int64 num, int64 a0v, int64 a1v, int64 a2v)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a0v;
    register int64 a1 asm("a1") = a1v;
    register int64 a2 asm("a2") = a2v;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_linux_syscall5(int64 num, int64 a0v, int64 a1v,
                                       int64 a2v, int64 a3v, int64 a4v)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a0v;
    register int64 a1 asm("a1") = a1v;
    register int64 a2 asm("a2") = a2v;
    register int64 a3 asm("a3") = a3v;
    register int64 a4 asm("a4") = a4v;
    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
                 : "memory");
    return a0;
}

static inline int64 raw_linux_syscall4(int64 num, int64 a0v, int64 a1v,
                                       int64 a2v, int64 a3v)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a0v;
    register int64 a1 asm("a1") = a1v;
    register int64 a2 asm("a2") = a2v;
    register int64 a3 asm("a3") = a3v;
    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a3), "r"(a7)
                 : "memory");
    return a0;
}

static inline int64 raw_linux_syscall6(int64 num, int64 a0v, int64 a1v,
                                       int64 a2v, int64 a3v, int64 a4v,
                                       int64 a5v)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a0v;
    register int64 a1 asm("a1") = a1v;
    register int64 a2 asm("a2") = a2v;
    register int64 a3 asm("a3") = a3v;
    register int64 a4 asm("a4") = a4v;
    register int64 a5 asm("a5") = a5v;
    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                 : "memory");
    return a0;
}

__attribute__((noreturn, noinline))
static void raw_linux_unmapself_child(void)
{
    child_entered = 1;
    asm volatile(
        "mv a0, %[addr]\n"
        "mv a1, %[len]\n"
        "li a7, %[linux_munmap]\n"
        "ecall\n"
        "li a0, 0\n"
        "li a7, %[linux_exit]\n"
        "ecall\n"
        "la t0, child_failed\n"
        "li t1, 1\n"
        "sw t1, 0(t0)\n"
        "li a0, 0\n"
        "li a7, 3\n"
        "ecall\n"
        "1: j 1b\n"
        :
        : [addr] "r"(child_stack),
          [len] "r"((uint64)STACK_SIZE),
          [linux_munmap] "i"(LINUX_NR_MUNMAP),
          [linux_exit] "i"(LINUX_NR_EXIT)
        : "a0", "a1", "a7", "t0", "t1", "memory");
    for (;;)
        ;
}
#endif

static inline int64 raw_linux_munmap(void *addr, uint64 len)
{
    return raw_linux_syscall2(LINUX_NR_MUNMAP, (int64)addr, (int64)len);
}

static void test_linux_arch_prctl_tls_rejects(void)
{
#if defined(__x86_64__)
    uint64 before = 0;
    uint64 after = 0;
    int64 get_before;
    int64 set_bad;
    int64 get_after;
    char *stack;
    int64 clone_bad;

    get_before =
        raw_linux_syscall2(LINUX_NR_ARCH_PRCTL, ARCH_GET_FS, (int64)&before);
    set_bad = raw_linux_syscall2(LINUX_NR_ARCH_PRCTL, ARCH_SET_FS,
                                 (int64)X86_NONCANONICAL_LOW);
    get_after =
        raw_linux_syscall2(LINUX_NR_ARCH_PRCTL, ARCH_GET_FS, (int64)&after);

    stack = mmap(0, STACK_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        printf("linuxsyscallabitest: tls reject stack mmap failed\n");
        exit(1);
    }
    clone_bad = raw_linux_syscall5(
        LINUX_NR_CLONE,
        CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
            CLONE_SETTLS,
        (int64)(stack + STACK_SIZE), 0, 0, (int64)X86_NONCANONICAL_LOW);
    munmap(stack, STACK_SIZE);

    if (get_before != 0 || set_bad != -EPERM || get_after != 0 ||
        before != after || clone_bad != -EPERM) {
        printf("linuxsyscallabitest: x86 TLS reject failed: get_before=%ld "
               "set_bad=%ld get_after=%ld before=0x%lx after=0x%lx "
               "clone_bad=%ld\n",
               get_before, set_bad, get_after, before, after, clone_bad);
        exit(1);
    }

    printf("linuxsyscallabitest: x86 TLS reject OK\n");
#else
    printf("linuxsyscallabitest: x86 TLS reject skipped\n");
#endif
}

static void test_linux_munmap_number(void)
{
    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("linuxsyscallabitest: mmap setup failed\n");
        exit(1);
    }

    p[0] = 42;
    int64 ret = raw_linux_munmap(p, 4096);
    if (ret != 0) {
        printf("linuxsyscallabitest: Linux munmap number failed: %ld\n", ret);
        exit(1);
    }

    printf("linuxsyscallabitest: Linux munmap number OK\n");
}

static void test_linux_unmapself_sequence(void)
{
    child_entered = 0;
    child_failed = 0;
    child_stack = mmap(0, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (child_stack == MAP_FAILED) {
        printf("linuxsyscallabitest: child stack mmap failed\n");
        exit(1);
    }

#ifdef HOST_LIBC_PROGRAM
    int pid = clone((int (*)(void *))raw_linux_unmapself_child,
                    (char *)child_stack + STACK_SIZE,
                    CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                    SIGCHLD,
                    0);
#else
    struct clone_args args = {
        .flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                 CLONE_THREAD | SIGCHLD,
        .stack = (uint64)child_stack,
        .stack_size = STACK_SIZE,
        .entry = (uint64)raw_linux_unmapself_child,
    };

    int pid = clone(&args);
#endif
    if (pid < 0) {
        printf("linuxsyscallabitest: clone failed: %d\n", pid);
        exit(1);
    }

    for (int i = 0; i < 10000000 && !child_entered; i++)
        ;
    for (int i = 0; i < 10000000 && !child_failed; i++)
        ;

    if (!child_entered) {
        printf("linuxsyscallabitest: child never entered\n");
        exit(1);
    }
    if (child_failed) {
        printf("linuxsyscallabitest: Linux exit number returned in child\n");
        exit(1);
    }

#ifdef HOST_LIBC_PROGRAM
    int status = 0;
    int waited = raw_linux_syscall4(61, pid, (int64)&status, 0, 0);
    if (waited != pid || status != 0) {
        printf("linuxsyscallabitest: Linux __unmapself child wait failed pid=%d status=%d\n",
               waited, status);
        exit(1);
    }
#endif

    printf("linuxsyscallabitest: Linux __unmapself syscall sequence OK\n");
}

static void test_linux_memory_locking_numbers(void)
{
    char *p = mmap(0, 8192, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("linuxsyscallabitest: memory locking mmap failed\n");
        exit(1);
    }

    p[0] = 42;
    if (raw_linux_syscall2(LINUX_NR_MLOCK, (int64)(p + 17), 4096) != 0 ||
        raw_linux_syscall3(LINUX_NR_MLOCK2, (int64)p, 4096, MLOCK_ONFAULT) != 0 ||
        raw_linux_syscall2(LINUX_NR_MLOCKALL, MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT, 0) != 0 ||
        raw_linux_syscall2(LINUX_NR_MUNLOCK, (int64)(p + 33), 1024) != 0 ||
        raw_linux_syscall2(LINUX_NR_MUNLOCKALL, 0, 0) != 0) {
        printf("linuxsyscallabitest: Linux memory locking numbers failed\n");
        munmap(p, 8192);
        exit(1);
    }

    if (raw_linux_syscall3(LINUX_NR_MLOCK2, (int64)p, 4096, 0x80) != -EINVAL ||
        raw_linux_syscall2(LINUX_NR_MLOCKALL, 0x80, 0) != -EINVAL) {
        printf("linuxsyscallabitest: Linux memory locking invalid flags accepted\n");
        munmap(p, 8192);
        exit(1);
    }

    munmap(p, 8192);
    printf("linuxsyscallabitest: Linux memory locking numbers OK\n");
}

static void test_linux_time_numbers(void)
{
    struct timespec ts;
    struct timespec res;

    if (raw_linux_syscall2(LINUX_NR_CLOCK_GETTIME, CLOCK_MONOTONIC, (int64)&ts) != 0 ||
        raw_linux_syscall2(LINUX_NR_CLOCK_GETRES, CLOCK_REALTIME, (int64)&res) != 0) {
        printf("linuxsyscallabitest: Linux clock numbers failed\n");
        exit(1);
    }
    if (res.tv_nsec <= 0 || res.tv_nsec >= 1000000000) {
        printf("linuxsyscallabitest: Linux clock_getres returned invalid nsec %ld\n",
               res.tv_nsec);
        exit(1);
    }

    printf("linuxsyscallabitest: Linux clock numbers OK\n");
}

static void test_linux_misc_numbers(void)
{
    char new_name[16] = "linuxabi";
    char got_name[16];
    char sysinfo_buf[128];

    if (raw_linux_syscall5(LINUX_NR_PRCTL, PR_SET_NAME, (int64)new_name,
                           0, 0, 0) != 0 ||
        raw_linux_syscall5(LINUX_NR_PRCTL, PR_GET_NAME, (int64)got_name,
                           0, 0, 0) != 0) {
        printf("linuxsyscallabitest: Linux prctl number failed\n");
        exit(1);
    }
    if (strcmp(got_name, new_name) != 0) {
        printf("linuxsyscallabitest: Linux prctl name mismatch\n");
        exit(1);
    }

    if (raw_linux_syscall2(LINUX_NR_SYSINFO, (int64)sysinfo_buf, 0) != 0) {
        printf("linuxsyscallabitest: Linux sysinfo number failed\n");
        exit(1);
    }

#if defined(__x86_64__)
    int clear_tid = 0;
    if (raw_linux_syscall2(LINUX_NR_SET_TID_ADDRESS, (int64)&clear_tid, 0) <= 0) {
        printf("linuxsyscallabitest: Linux set_tid_address number failed\n");
        exit(1);
    }
#endif

    printf("linuxsyscallabitest: Linux misc numbers OK\n");
}

static void test_linux_more_native_numbers(void)
{
    if (raw_linux_syscall0(LINUX_NR_GETTID) != gettid()) {
        printf("linuxsyscallabitest: Linux gettid number failed\n");
        exit(1);
    }

    if (raw_linux_syscall5(LINUX_NR_PRLIMIT64, 0, -1, 0, 0, 0) != -EINVAL) {
        printf("linuxsyscallabitest: Linux prlimit64 number failed\n");
        exit(1);
    }

    if (raw_linux_syscall2(LINUX_NR_MEMBARRIER, MEMBARRIER_CMD_QUERY, 0) < 0) {
        printf("linuxsyscallabitest: Linux membarrier number failed\n");
        exit(1);
    }

    if (raw_linux_syscall2(LINUX_NR_IOPRIO_GET, IOPRIO_WHO_PROCESS, 0) < 0 ||
        raw_linux_syscall3(LINUX_NR_IOPRIO_SET, IOPRIO_WHO_PROCESS, 0, 0) != 0) {
        printf("linuxsyscallabitest: Linux ioprio numbers failed\n");
        exit(1);
    }

    if (raw_linux_syscall5(LINUX_NR_STATX, AT_FDCWD, 0, 0, 0, 0) != -EFAULT) {
        printf("linuxsyscallabitest: Linux statx number failed\n");
        exit(1);
    }

#if defined(__x86_64__)
    char cwd[128];
    char dirents[512];
    char mask[8] = {0};
    struct timespec ts = {0};

    if (raw_linux_syscall2(LINUX_NR_GETCWD, (int64)cwd, sizeof(cwd)) !=
            (int64)cwd ||
        cwd[0] != '/') {
        printf("linuxsyscallabitest: Linux getcwd number failed\n");
        exit(1);
    }

    int fd = open("/", 0);
    if (fd < 0 || raw_linux_syscall3(LINUX_NR_GETDENTS, fd, (int64)dirents,
                                     sizeof(dirents)) <= 0) {
        printf("linuxsyscallabitest: Linux getdents number failed\n");
        if (fd >= 0)
            close(fd);
        exit(1);
    }
    close(fd);

    fd = open("/", 0);
    if (fd < 0 || raw_linux_syscall3(LINUX_NR_GETDENTS64, fd, (int64)dirents,
                                     sizeof(dirents)) <= 0) {
        printf("linuxsyscallabitest: Linux getdents64 number failed\n");
        if (fd >= 0)
            close(fd);
        exit(1);
    }
    close(fd);

    if (raw_linux_syscall3(LINUX_NR_SCHED_GETAFFINITY, 0, sizeof(mask),
                           (int64)mask) <= 0 ||
        (mask[0] & 1) == 0 ||
        raw_linux_syscall3(LINUX_NR_SCHED_SETAFFINITY, 0, sizeof(mask),
                           (int64)mask) != 0) {
        printf("linuxsyscallabitest: Linux sched affinity numbers failed\n");
        exit(1);
    }

    struct linux_sched_attr sa;
    memset(&sa, 0, sizeof(sa));
    if (raw_linux_syscall4(LINUX_NR_SCHED_GETATTR, 0, (int64)&sa,
                           sizeof(sa), 0) != 0 ||
        sa.size < 48 || sa.sched_policy != 0 || sa.sched_priority != 0) {
        printf("linuxsyscallabitest: Linux sched_getattr number failed\n");
        exit(1);
    }
    sa.size = sizeof(sa);
    if (raw_linux_syscall3(LINUX_NR_SCHED_SETATTR, 0, (int64)&sa, 0) != 0) {
        printf("linuxsyscallabitest: Linux sched_setattr number failed\n");
        exit(1);
    }

    if (raw_linux_syscall2(LINUX_NR_CLOCK_SETTIME, CLOCK_REALTIME,
                           (int64)&ts) != -EPERM) {
        printf("linuxsyscallabitest: Linux clock_settime number failed\n");
        exit(1);
    }
#else
    struct timeval tv;
    if (raw_linux_syscall2(LINUX_NR_GETTIMEOFDAY, (int64)&tv, 0) != 0) {
        printf("linuxsyscallabitest: Linux gettimeofday number failed\n");
        exit(1);
    }

    int64 mapped = raw_linux_syscall6(LINUX_NR_MMAP, 0, 4096,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped < 0) {
        printf("linuxsyscallabitest: Linux mmap number failed\n");
        exit(1);
    }
    ((char *)mapped)[0] = 7;
    if (raw_linux_munmap((void *)mapped, 4096) != 0) {
        printf("linuxsyscallabitest: Linux generic munmap cleanup failed\n");
        exit(1);
    }
#endif

    printf("linuxsyscallabitest: more Linux native numbers OK\n");
}

#if defined(__x86_64__)
static void test_linux_identity_native_numbers(void)
{
    uint32 ids[3];

    if (raw_linux_syscall0(LINUX_NR_GETUID) != getuid() ||
        raw_linux_syscall0(LINUX_NR_GETGID) < 0 ||
        raw_linux_syscall0(LINUX_NR_GETEUID) != geteuid() ||
        raw_linux_syscall0(LINUX_NR_GETEGID) < 0) {
        printf("linuxsyscallabitest: Linux uid/gid native numbers failed\n");
        exit(1);
    }

    if (raw_linux_syscall2(LINUX_NR_SETREUID, -1, -1) != 0 ||
        raw_linux_syscall2(LINUX_NR_SETREGID, -1, -1) != 0) {
        printf("linuxsyscallabitest: Linux setreuid/setregid no-op failed\n");
        exit(1);
    }

    memset(ids, 0, sizeof(ids));
    if (raw_linux_syscall3(LINUX_NR_GETRESUID, (int64)&ids[0],
                           (int64)&ids[1], (int64)&ids[2]) != 0 ||
        ids[0] != (uint32)getuid() || ids[1] != (uint32)geteuid()) {
        printf("linuxsyscallabitest: Linux getresuid number failed\n");
        exit(1);
    }

    memset(ids, 0, sizeof(ids));
    if (raw_linux_syscall3(LINUX_NR_GETRESGID, (int64)&ids[0],
                           (int64)&ids[1], (int64)&ids[2]) != 0) {
        printf("linuxsyscallabitest: Linux getresgid number failed\n");
        exit(1);
    }

    printf("linuxsyscallabitest: Linux identity native numbers OK\n");
}

static void test_linux_memory_native_numbers(void)
{
    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char vec[2] = {0};

    if (p == MAP_FAILED) {
        printf("linuxsyscallabitest: memory native mmap setup failed\n");
        exit(1);
    }
    p[0] = 7;

    if (raw_linux_syscall3(LINUX_NR_MSYNC, (int64)p, 4096, 0) != 0 ||
        raw_linux_syscall3(LINUX_NR_MINCORE, (int64)p, 4096, (int64)vec) != 0 ||
        raw_linux_syscall3(LINUX_NR_MADVISE, (int64)p, 4096, 0) != 0) {
        printf("linuxsyscallabitest: Linux memory native numbers failed\n");
        munmap(p, 4096);
        exit(1);
    }

    int64 moved = raw_linux_syscall5(LINUX_NR_MREMAP, (int64)p, 4096, 8192,
                                     MREMAP_MAYMOVE, 0);
    if (moved < 0) {
        printf("linuxsyscallabitest: Linux mremap native number failed: %ld\n",
               moved);
        munmap(p, 4096);
        exit(1);
    }

    ((char *)moved)[4096] = 9;
    if (munmap((void *)moved, 8192) != 0) {
        printf("linuxsyscallabitest: Linux mremap cleanup failed\n");
        exit(1);
    }

    printf("linuxsyscallabitest: Linux memory native numbers OK\n");
}

static void test_linux_at_fd_native_numbers(void)
{
    struct stat st;
    char linkbuf[16];
    int pipefd[2];
    char write_byte = 'Z';
    char read_byte = 0;
    struct iovec wiov = { &write_byte, 1 };
    struct iovec riov = { &read_byte, 1 };
    char write_pair[2] = {'A', 'B'};
    char read_pair[2] = {0, 0};
    struct iovec writev_iov[2] = {
        { &write_pair[0], 1 },
        { &write_pair[1], 1 },
    };
    struct iovec readv_iov[2] = {
        { &read_pair[0], 1 },
        { &read_pair[1], 1 },
    };
    struct f_owner_ex owner = {0};
    struct flock fl = {0};
    struct flock probe_fl = {0};

    int fd = raw_linux_syscall5(LINUX_NR_OPENAT, AT_FDCWD,
                                (int64)"linuxabi.tmp",
                                O_CREAT | O_TRUNC | O_RDWR, 0644, 0);
    if (fd < 0) {
        printf("linuxsyscallabitest: Linux openat number failed: %d\n", fd);
        exit(1);
    }

    if (raw_linux_syscall4(LINUX_NR_PWRITE64, fd, (int64)&write_byte, 1,
                           0) != 1 ||
        raw_linux_syscall2(LINUX_NR_FSYNC, fd, 0) != 0 ||
        raw_linux_syscall2(LINUX_NR_FDATASYNC, fd, 0) != 0 ||
        raw_linux_syscall3(LINUX_NR_LSEEK, fd, 0, SEEK_SET) != 0 ||
        raw_linux_syscall3(LINUX_NR_READ, fd, (int64)&read_byte, 1) != 1 ||
        read_byte != write_byte ||
        raw_linux_syscall2(LINUX_NR_FTRUNCATE, fd, 0) != 0 ||
        raw_linux_syscall3(LINUX_NR_LSEEK, fd, 0, SEEK_SET) != 0) {
        printf("linuxsyscallabitest: Linux pwrite/fsync numbers failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }

    if (raw_linux_syscall3(LINUX_NR_WRITEV, fd, (int64)writev_iov, 2) != 2 ||
        raw_linux_syscall3(LINUX_NR_LSEEK, fd, 0, SEEK_SET) != 0 ||
        raw_linux_syscall3(LINUX_NR_READV, fd, (int64)readv_iov, 2) != 2 ||
        read_pair[0] != 'A' || read_pair[1] != 'B') {
        printf("linuxsyscallabitest: Linux readv/writev numbers failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    if (raw_linux_syscall2(LINUX_NR_FTRUNCATE, fd, 0) != 0) {
        printf("linuxsyscallabitest: Linux ftruncate number failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }

    if (raw_linux_syscall5(LINUX_NR_PWRITEV, fd, (int64)&wiov, 1, 0, 0) != 1 ||
        raw_linux_syscall5(LINUX_NR_PREADV, fd, (int64)&riov, 1, 0, 0) != 1 ||
        read_byte != write_byte) {
        printf("linuxsyscallabitest: Linux preadv/pwritev numbers failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }

    read_byte = 0;
    if (raw_linux_syscall5(LINUX_NR_PWRITEV2, fd, (int64)&wiov, 1, 0, 0) != 1 ||
        raw_linux_syscall5(LINUX_NR_PREADV2, fd, (int64)&riov, 1, 0, 0) != 1 ||
        read_byte != write_byte) {
        printf("linuxsyscallabitest: Linux preadv2/pwritev2 numbers failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }

    if (raw_linux_syscall5(LINUX_NR_NEWFSTATAT, AT_FDCWD,
                           (int64)"linuxabi.tmp", (int64)&st, 0, 0) != 0 ||
        raw_linux_syscall5(LINUX_NR_NEWFSTATAT, fd, (int64)"",
                           (int64)&st, AT_EMPTY_PATH, 0) != 0 ||
        raw_linux_syscall5(LINUX_NR_NEWFSTATAT, fd, (int64)"",
                           (int64)&st, 0x40000000, 0) != -EINVAL ||
        raw_linux_syscall2(LINUX_NR_ACCESS, (int64)"linuxabi.tmp", 0) != 0 ||
        raw_linux_syscall5(LINUX_NR_FACCESSAT, AT_FDCWD,
                           (int64)"linuxabi.tmp", 0, 0, 0) != 0 ||
        raw_linux_syscall5(LINUX_NR_READLINKAT, AT_FDCWD,
                           (int64)"linuxabi.tmp", (int64)linkbuf,
                           sizeof(linkbuf), 0) != -EINVAL) {
        printf("linuxsyscallabitest: Linux at-family numbers failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    int abs_dir_fd = raw_linux_syscall4(LINUX_NR_OPENAT, fd, (int64)"/",
                                        O_RDONLY | O_DIRECTORY, 0);
    if (abs_dir_fd < 0) {
        printf("linuxsyscallabitest: Linux openat absolute dirfd ignore failed: %d\n",
               abs_dir_fd);
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    close(abs_dir_fd);

    unlink("linuxabi.link");
    unlink("linuxabi.renamed");
    unlink("linuxabi.symlink");
    raw_linux_syscall2(LINUX_NR_RMDIR, (int64)"linuxabi.dir", 0);
    memset(linkbuf, 0, sizeof(linkbuf));
    int64 path_ret;
#define CHECK_PATH_RET(expr, what)                                            \
    do {                                                                      \
        path_ret = (expr);                                                    \
        if (path_ret != 0) {                                                  \
            printf("linuxsyscallabitest: Linux %s failed: %ld\n", what,      \
                   path_ret);                                                 \
            close(fd);                                                        \
            unlink("linuxabi.link");                                         \
            unlink("linuxabi.renamed");                                      \
            unlink("linuxabi.symlink");                                      \
            unlink("linuxabi.tmp");                                          \
            raw_linux_syscall2(LINUX_NR_RMDIR, (int64)"linuxabi.dir", 0);     \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

    CHECK_PATH_RET(raw_linux_syscall2(LINUX_NR_MKDIR, (int64)"linuxabi.dir",
                                      0755), "mkdir");
    CHECK_PATH_RET(raw_linux_syscall2(LINUX_NR_CHDIR, (int64)"linuxabi.dir",
                                      0), "chdir child");
    CHECK_PATH_RET(raw_linux_syscall2(LINUX_NR_CHDIR, (int64)"..", 0),
                   "chdir parent");
    CHECK_PATH_RET(raw_linux_syscall2(LINUX_NR_RMDIR, (int64)"linuxabi.dir",
                                      0), "rmdir");
    CHECK_PATH_RET(raw_linux_syscall2(LINUX_NR_LINK, (int64)"linuxabi.tmp",
                                      (int64)"linuxabi.link"), "link");
    CHECK_PATH_RET(raw_linux_syscall2(LINUX_NR_RENAME, (int64)"linuxabi.link",
                                      (int64)"linuxabi.renamed"), "rename");
    CHECK_PATH_RET(raw_linux_syscall2(LINUX_NR_SYMLINK,
                                      (int64)"linuxabi.renamed",
                                      (int64)"linuxabi.symlink"), "symlink");
    path_ret = raw_linux_syscall3(LINUX_NR_READLINK,
                                  (int64)"linuxabi.symlink",
                                  (int64)linkbuf, sizeof(linkbuf));
    if (path_ret <= 0 || linkbuf[0] != 'l') {
        printf("linuxsyscallabitest: Linux readlink failed: %ld first=%d\n",
               path_ret, linkbuf[0]);
        close(fd);
        unlink("linuxabi.link");
        unlink("linuxabi.renamed");
        unlink("linuxabi.symlink");
        unlink("linuxabi.tmp");
        raw_linux_syscall2(LINUX_NR_RMDIR, (int64)"linuxabi.dir", 0);
        exit(1);
    }
    CHECK_PATH_RET(raw_linux_syscall2(LINUX_NR_UNLINK,
                                      (int64)"linuxabi.symlink", 0),
                   "unlink symlink");
    CHECK_PATH_RET(raw_linux_syscall2(LINUX_NR_UNLINK,
                                      (int64)"linuxabi.renamed", 0),
                   "unlink renamed");
#undef CHECK_PATH_RET

    int dupfd = raw_linux_syscall3(LINUX_NR_DUP3, fd, fd + 10, O_CLOEXEC);
    if (dupfd != fd + 10) {
        printf("linuxsyscallabitest: Linux dup3 number failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    close(dupfd);
    dupfd = raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_DUPFD_CLOEXEC, fd + 10);
    if (dupfd < fd + 10 ||
        raw_linux_syscall3(LINUX_NR_FCNTL, fd, 14, 0) != -EINVAL) {
        printf("linuxsyscallabitest: Linux F_DUPFD_CLOEXEC number failed\n");
        if (dupfd >= 0)
            close(dupfd);
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    close(dupfd);

    owner.type = F_OWNER_PID;
    owner.pid = getpid();
    if (raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_SETOWN, owner.pid) != 0 ||
        raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_GETOWN, 0) != owner.pid ||
        raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_SETSIG, 10) != 0 ||
        raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_GETSIG, 0) != 10 ||
        raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_SETOWN_EX,
                           (int64)&owner) != 0) {
        printf("linuxsyscallabitest: Linux fcntl owner failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    memset(&owner, 0, sizeof(owner));
    if (raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_GETOWN_EX,
                           (int64)&owner) != 0 ||
        owner.type != F_OWNER_PID || owner.pid != getpid()) {
        printf("linuxsyscallabitest: Linux fcntl owner_ex failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }

    if (raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_GETLEASE, 0) != F_UNLCK ||
        raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_SETLEASE, F_UNLCK) != 0 ||
        raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_SETLEASE, F_WRLCK) != -EAGAIN ||
        raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_NOTIFY, 0) != 0 ||
        raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_NOTIFY, DN_MODIFY) != -ENOTDIR) {
        printf("linuxsyscallabitest: Linux fcntl lease/notify failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }

    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 1;
    if (raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_OFD_SETLK,
                           (int64)&fl) != 0) {
        printf("linuxsyscallabitest: Linux fcntl OFD setlk failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    int lock_fd = raw_linux_syscall5(LINUX_NR_OPENAT, AT_FDCWD,
                                     (int64)"linuxabi.tmp", O_RDWR, 0, 0);
    probe_fl = fl;
    if (lock_fd < 0 ||
        raw_linux_syscall3(LINUX_NR_FCNTL, lock_fd, F_OFD_GETLK,
                           (int64)&probe_fl) != 0 ||
        probe_fl.l_type != F_WRLCK || probe_fl.l_pid != -1) {
        printf("linuxsyscallabitest: Linux fcntl OFD getlk failed\n");
        if (lock_fd >= 0)
            close(lock_fd);
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    fl.l_type = F_UNLCK;
    if (raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_OFD_SETLK,
                           (int64)&fl) != 0) {
        printf("linuxsyscallabitest: Linux fcntl OFD unlock failed\n");
        close(lock_fd);
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    close(lock_fd);

    if (raw_linux_syscall2(LINUX_NR_PIPE2, (int64)pipefd, O_CLOEXEC) != 0) {
        printf("linuxsyscallabitest: Linux pipe2 number failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    int pipe_size = raw_linux_syscall3(LINUX_NR_FCNTL, pipefd[0],
                                       F_GETPIPE_SZ, 0);
    if (pipe_size <= 0 ||
        raw_linux_syscall3(LINUX_NR_FCNTL, pipefd[1], F_SETPIPE_SZ,
                           pipe_size) != pipe_size ||
        raw_linux_syscall3(LINUX_NR_FCNTL, fd, F_GETPIPE_SZ, 0) != -EBADF) {
        printf("linuxsyscallabitest: Linux fcntl pipe size failed\n");
        close(pipefd[0]);
        close(pipefd[1]);
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    close(pipefd[0]);
    close(pipefd[1]);

    close(fd);
    unlink("linuxabi.tmp");
    printf("linuxsyscallabitest: Linux at/fd native numbers OK\n");
}

static void test_linux_stat_dirent_layouts(void)
{
    struct stat st;
    struct stat st2;
    char dirents[512];

    memset(&st, 0, sizeof(st));
    if (raw_linux_syscall2(LINUX_NR_STAT, (int64)"/", (int64)&st) != 0 ||
        st.st_ino == 0 || st.st_blksize == 0) {
        printf("linuxsyscallabitest: Linux stat layout failed\n");
        exit(1);
    }

    int fd = open("/", 0);
    if (fd < 0) {
        printf("linuxsyscallabitest: open / failed for stat layout\n");
        exit(1);
    }

    memset(&st2, 0, sizeof(st2));
    if (raw_linux_syscall2(LINUX_NR_FSTAT, fd, (int64)&st2) != 0 ||
        st2.st_ino != st.st_ino) {
        printf("linuxsyscallabitest: Linux fstat layout failed\n");
        close(fd);
        exit(1);
    }
    close(fd);

    memset(&st2, 0, sizeof(st2));
    if (raw_linux_syscall2(LINUX_NR_LSTAT, (int64)"/", (int64)&st2) != 0 ||
        st2.st_ino != st.st_ino) {
        printf("linuxsyscallabitest: Linux lstat layout failed\n");
        exit(1);
    }

    fd = open("/", 0);
    if (fd < 0) {
        printf("linuxsyscallabitest: open / failed for getdents layout\n");
        exit(1);
    }
    memset(dirents, 0, sizeof(dirents));
    int n = raw_linux_syscall3(LINUX_NR_GETDENTS, fd, (int64)dirents,
                               sizeof(dirents));
    if (n <= 0) {
        printf("linuxsyscallabitest: Linux getdents layout call failed\n");
        close(fd);
        exit(1);
    }
    struct linux_dirent_compat *de = (struct linux_dirent_compat *)dirents;
    if (de->d_reclen < sizeof(*de) + 2 ||
        de->d_name[0] != '.' || de->d_name[1] != '\0' ||
        ((uint8 *)de)[de->d_reclen - 1] != 4) {
        printf("linuxsyscallabitest: Linux getdents compat layout failed\n");
        close(fd);
        exit(1);
    }
    close(fd);

    fd = open("/", 0);
    if (fd < 0) {
        printf("linuxsyscallabitest: open / failed for getdents64 layout\n");
        exit(1);
    }
    memset(dirents, 0, sizeof(dirents));
    n = raw_linux_syscall3(LINUX_NR_GETDENTS64, fd, (int64)dirents,
                           sizeof(dirents));
    if (n <= 0) {
        printf("linuxsyscallabitest: Linux getdents64 layout call failed\n");
        close(fd);
        exit(1);
    }
    struct linux_dirent64_abi *de64 = (struct linux_dirent64_abi *)dirents;
    if (de64->d_reclen < sizeof(*de64) + 1 ||
        de64->d_type != 4 ||
        de64->d_name[0] != '.' || de64->d_name[1] != '\0') {
        printf("linuxsyscallabitest: Linux getdents64 layout failed\n");
        close(fd);
        exit(1);
    }
    close(fd);

    printf("linuxsyscallabitest: Linux stat/dirent layouts OK\n");
}

static void test_linux_rt_signal_abi(void)
{
    struct sigaction oldact;
    sigset_t mask = {0};
    sigset_t oldmask = {0};
    sigset_t pending = {0};
    siginfo_t info;
    struct linux_timespec zero = {0, 0};

    memset(&oldact, 0, sizeof(oldact));
    if (raw_linux_syscall4(LINUX_NR_RT_SIGACTION, SIGUSR1, 0,
                           (int64)&oldact, LINUX_KERNEL_SIGSET_SIZE) != 0) {
        printf("linuxsyscallabitest: Linux rt_sigaction query failed\n");
        exit(1);
    }

    if (raw_linux_syscall4(LINUX_NR_RT_SIGACTION, SIGUSR1, 0,
                           (int64)&oldact, 4) != -EINVAL) {
        printf("linuxsyscallabitest: Linux rt_sigaction bad sigsetsize accepted\n");
        exit(1);
    }

    if (raw_linux_syscall4(LINUX_NR_RT_SIGPROCMASK, LINUX_SIG_SETMASK, 0,
                           (int64)&oldmask, LINUX_KERNEL_SIGSET_SIZE) != 0 ||
        raw_linux_syscall4(LINUX_NR_RT_SIGPROCMASK, LINUX_SIG_SETMASK,
                           (int64)&mask, (int64)&oldmask, 4) != -EINVAL) {
        printf("linuxsyscallabitest: Linux rt_sigprocmask ABI failed\n");
        exit(1);
    }

    if (raw_linux_syscall2(LINUX_NR_RT_SIGPENDING, (int64)&pending,
                           LINUX_KERNEL_SIGSET_SIZE) != 0 ||
        raw_linux_syscall2(LINUX_NR_RT_SIGPENDING, (int64)&pending, 4) !=
            -EINVAL) {
        printf("linuxsyscallabitest: Linux rt_sigpending ABI failed\n");
        exit(1);
    }

    if (raw_linux_syscall2(LINUX_NR_RT_SIGSUSPEND, (int64)&mask, 4) !=
        -EINVAL) {
        printf("linuxsyscallabitest: Linux rt_sigsuspend bad sigsetsize accepted\n");
        exit(1);
    }

    struct linux_pselect6_sigmask psig = {
        .ss = (uint64)&mask,
        .ss_len = LINUX_KERNEL_SIGSET_SIZE,
    };
    if (raw_linux_syscall6(LINUX_NR_PSELECT6, 0, 0, 0, 0, (int64)&zero,
                           (int64)&psig) != 0) {
        printf("linuxsyscallabitest: Linux pselect6 sigmask ABI failed\n");
        exit(1);
    }
    psig.ss_len = 4;
    if (raw_linux_syscall6(LINUX_NR_PSELECT6, 0, 0, 0, 0, (int64)&zero,
                           (int64)&psig) != -EINVAL) {
        printf("linuxsyscallabitest: Linux pselect6 short sigset accepted\n");
        exit(1);
    }
    psig.ss_len = LINUX_KERNEL_SIGSET_SIZE + 4;
    if (raw_linux_syscall6(LINUX_NR_PSELECT6, 0, 0, 0, 0, (int64)&zero,
                           (int64)&psig) != -EINVAL) {
        printf("linuxsyscallabitest: Linux pselect6 oversized sigset accepted\n");
        exit(1);
    }
    if (raw_linux_syscall5(LINUX_NR_PPOLL, 0, 0, (int64)&zero, (int64)&mask,
                           LINUX_KERNEL_SIGSET_SIZE) != 0 ||
        raw_linux_syscall5(LINUX_NR_PPOLL, 0, 0, (int64)&zero, (int64)&mask,
                           4) != -EINVAL ||
        raw_linux_syscall5(LINUX_NR_PPOLL, 0, 0, (int64)&zero, (int64)&mask,
                           LINUX_KERNEL_SIGSET_SIZE + 4) != -EINVAL) {
        printf("linuxsyscallabitest: Linux ppoll sigmask ABI failed\n");
        exit(1);
    }

    memset(&info, 0, sizeof(info));
    if (raw_linux_syscall4(LINUX_NR_RT_SIGTIMEDWAIT, (int64)&mask,
                           (int64)&info, (int64)&zero,
                           LINUX_KERNEL_SIGSET_SIZE) !=
            -EAGAIN ||
        raw_linux_syscall4(LINUX_NR_RT_SIGTIMEDWAIT, (int64)&mask,
                           (int64)&info, (int64)&zero, 4) != -EINVAL) {
        printf("linuxsyscallabitest: Linux rt_sigtimedwait ABI failed\n");
        exit(1);
    }

    printf("linuxsyscallabitest: Linux rt signal ABI OK\n");
}

static void test_linux_process_runtime_numbers(void)
{
    struct linux_tms tms;
    uint32 cpu = 99;
    uint32 node = 99;
    char rseq_area[32] __attribute__((aligned(32)));
    struct linux_cap_header cap_hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0,
    };
    struct linux_cap_data cap_data[2];

    memset(&tms, 0, sizeof(tms));
    if (raw_linux_syscall2(LINUX_NR_TIMES, (int64)&tms, 0) < 0 ||
        raw_linux_syscall0(LINUX_NR_GETPGRP) < 0 ||
        raw_linux_syscall3(LINUX_NR_GETCPU, (int64)&cpu,
                           (int64)&node, 0) != 0 ||
        cpu == 99 || node != 0) {
        printf("linuxsyscallabitest: Linux process runtime numbers failed\n");
        exit(1);
    }

    int old_fsuid = raw_linux_syscall2(LINUX_NR_SETFSUID, -1, 0);
    int old_fsgid = raw_linux_syscall2(LINUX_NR_SETFSGID, -1, 0);
    if (old_fsuid < 0 || old_fsgid < 0) {
        printf("linuxsyscallabitest: Linux setfsuid/setfsgid query failed\n");
        exit(1);
    }

    memset(cap_data, 0xcc, sizeof(cap_data));
    if (raw_linux_syscall2(LINUX_NR_CAPGET, (int64)&cap_hdr,
                           (int64)cap_data) != 0 ||
        cap_data[0].effective != 0 || cap_data[1].permitted != 0 ||
        raw_linux_syscall2(LINUX_NR_CAPSET, (int64)&cap_hdr,
                           (int64)cap_data) != 0) {
        printf("linuxsyscallabitest: Linux capability model failed\n");
        exit(1);
    }

    memset(rseq_area, 0, sizeof(rseq_area));
    int64 rseq_ret = raw_linux_syscall4(LINUX_NR_RSEQ, (int64)rseq_area,
                                        sizeof(rseq_area), 0, 0x53053053);
#ifdef HAVE_GLIBC_RSEQ
    uint64 saved_glibc_rseq = 0;
    if (rseq_ret == -EBUSY) {
        saved_glibc_rseq = glibc_rseq_addr();
        if (saved_glibc_rseq != 0 &&
            raw_linux_syscall4(LINUX_NR_RSEQ, (int64)saved_glibc_rseq,
                               __rseq_size, RSEQ_FLAG_UNREGISTER,
                               0x53053053) == 0) {
            rseq_ret = raw_linux_syscall4(LINUX_NR_RSEQ, (int64)rseq_area,
                                          sizeof(rseq_area), 0, 0x53053053);
        }
    }
#endif
    if (rseq_ret != 0 ||
        raw_linux_syscall4(LINUX_NR_RSEQ, (int64)rseq_area,
                           sizeof(rseq_area), RSEQ_FLAG_UNREGISTER,
                           0x53053053) != 0) {
        printf("linuxsyscallabitest: Linux rseq registration failed\n");
        exit(1);
    }
#ifdef HAVE_GLIBC_RSEQ
    if (saved_glibc_rseq != 0 &&
        raw_linux_syscall4(LINUX_NR_RSEQ, (int64)saved_glibc_rseq,
                           __rseq_size, __rseq_flags, 0x53053053) != 0) {
        printf("linuxsyscallabitest: Linux glibc rseq restore failed\n");
        exit(1);
    }
#endif

    printf("linuxsyscallabitest: Linux process runtime numbers OK\n");
}

static void test_linux_wait_fork_numbers(void)
{
    int status = 0;
    int pid = raw_linux_syscall0(LINUX_NR_FORK);
    if (pid < 0) {
        printf("linuxsyscallabitest: Linux fork number failed: %d\n", pid);
        exit(1);
    }
    if (pid == 0) {
        raw_linux_syscall2(LINUX_NR_EXIT, 7, 0);
        for (;;)
            ;
    }
    int waited = raw_linux_syscall4(61, pid, (int64)&status, 0, 0);
    if (waited != pid || status != (7 << 8)) {
        printf("linuxsyscallabitest: Linux wait4 after fork failed pid=%d status=%d\n",
               waited, status);
        exit(1);
    }

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int waitid_empty = raw_linux_syscall5(LINUX_NR_WAITID, P_ALL, 0,
                                          (int64)&info, WEXITED | WNOHANG, 0);
    if (waitid_empty != -ECHILD) {
        int any_status = 0;
        int wait4_any = raw_linux_syscall4(61, -1, (int64)&any_status,
                                           WNOHANG, 0);
        printf("linuxsyscallabitest: Linux waitid empty-children failed ret=%d si_pid=%d wait4_any=%d\n",
               waitid_empty, info.si_pid, wait4_any);
        exit(1);
    }

    printf("linuxsyscallabitest: Linux fork/wait numbers OK\n");
}

static void test_linux_vfs_new_numbers(void)
{
    struct linux_timeval tv = {0, 0};
    struct linux_open_how how;
    int fd;
    int fd2;
    int64 in_off = 0;
    int64 out_off = 0;
    char b = 'Q';
    char out = 0;
    uint64 now = 0;

    printf("linuxsyscallabitest: VFS new select/time...\n");
    if (raw_linux_syscall5(LINUX_NR_SELECT, 0, 0, 0, 0, (int64)&tv) != 0 ||
        raw_linux_syscall2(LINUX_NR_TIME, (int64)&now, 0) <= 0 || now == 0) {
        printf("linuxsyscallabitest: Linux select/time numbers failed\n");
        exit(1);
    }
    printf("linuxsyscallabitest: VFS new eventfd...\n");
    int efd = raw_linux_syscall2(LINUX_NR_EVENTFD, 2, 0);
    uint64 ev = 0;
    if (efd < 0 ||
        raw_linux_syscall3(LINUX_NR_READ, efd, (int64)&ev, sizeof(ev)) !=
            (int64)sizeof(ev) ||
        ev != 2) {
        printf("linuxsyscallabitest: Linux eventfd number failed\n");
        if (efd >= 0)
            close(efd);
        exit(1);
    }
    close(efd);

    printf("linuxsyscallabitest: VFS new signalfd...\n");
    uint64 sigmask = 0;
    int sfd = raw_linux_syscall4(LINUX_NR_SIGNALFD4, -1, (int64)&sigmask,
                                 sizeof(sigmask),
                                 LINUX_SFD_NONBLOCK | LINUX_SFD_CLOEXEC);
    char siginfo[128];
    if (sfd < 0 ||
        raw_linux_syscall3(LINUX_NR_READ, sfd, (int64)siginfo,
                           sizeof(siginfo)) != -EAGAIN) {
        printf("linuxsyscallabitest: Linux signalfd4 number failed: %d\n", sfd);
        if (sfd >= 0)
            close(sfd);
        exit(1);
    }
    if (raw_linux_syscall3(LINUX_NR_SIGNALFD, sfd, (int64)&sigmask,
                           sizeof(sigmask)) != sfd) {
        printf("linuxsyscallabitest: Linux signalfd update failed\n");
        close(sfd);
        exit(1);
    }
    close(sfd);

    printf("linuxsyscallabitest: VFS new accept4...\n");
    struct linux_sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, "linuxabi.accept4.sock");
    int sun_len = sizeof(sun.sun_family) + strlen(sun.sun_path) + 1;
    int lfd = raw_linux_syscall3(LINUX_NR_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    int cfd = raw_linux_syscall3(LINUX_NR_SOCKET, AF_UNIX,
                                 SOCK_STREAM | LINUX_SOCK_NONBLOCK, 0);
    if (lfd < 0 || cfd < 0 ||
        raw_linux_syscall3(LINUX_NR_BIND, lfd, (int64)&sun, sun_len) != 0 ||
        raw_linux_syscall2(LINUX_NR_LISTEN, lfd, 1) != 0 ||
        raw_linux_syscall4(LINUX_NR_ACCEPT4, lfd, 0, 0, 0x40000000) !=
            -EINVAL) {
        printf("linuxsyscallabitest: Linux accept4 setup/flags failed\n");
        if (lfd >= 0)
            close(lfd);
        if (cfd >= 0)
            close(cfd);
        exit(1);
    }
    int conn_ret =
        raw_linux_syscall3(LINUX_NR_CONNECT, cfd, (int64)&sun, sun_len);
    if (conn_ret != 0 && conn_ret != -EINPROGRESS) {
        printf("linuxsyscallabitest: Linux accept4 connect failed: %d\n",
               conn_ret);
        close(lfd);
        close(cfd);
        exit(1);
    }
    int afd = raw_linux_syscall4(LINUX_NR_ACCEPT4, lfd, 0, 0,
                                 LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC);
    if (afd < 0 ||
        (raw_linux_syscall3(LINUX_NR_FCNTL, afd, F_GETFD, 0) &
         FD_CLOEXEC) == 0 ||
        (raw_linux_syscall3(LINUX_NR_FCNTL, afd, F_GETFL, 0) &
         O_NONBLOCK) == 0) {
        printf("linuxsyscallabitest: Linux accept4 fd flags failed: %d\n",
               afd);
        if (afd >= 0)
            close(afd);
        close(lfd);
        close(cfd);
        exit(1);
    }
    close(afd);
    close(cfd);
    close(lfd);

    printf("linuxsyscallabitest: VFS new creat/metadata...\n");
    fd = raw_linux_syscall2(LINUX_NR_CREAT, (int64)"linuxabi.vfs.a", 0644);
    if (fd < 0) {
        printf("linuxsyscallabitest: Linux creat number failed: %d\n", fd);
        exit(1);
    }
    printf("linuxsyscallabitest: VFS new write/close...\n");
    if (write(fd, &b, 1) != 1 || close(fd) != 0 ||
        raw_linux_syscall2(LINUX_NR_TRUNCATE, (int64)"linuxabi.vfs.a", 1) != 0) {
        printf("linuxsyscallabitest: Linux write/truncate numbers failed\n");
        unlink("linuxabi.vfs.a");
        exit(1);
    }
    printf("linuxsyscallabitest: VFS new chmod/chown...\n");
    int chmod_ret =
        raw_linux_syscall2(LINUX_NR_CHMOD, (int64)"linuxabi.vfs.a", 0600);
    int chown_ret =
        raw_linux_syscall3(LINUX_NR_CHOWN, (int64)"linuxabi.vfs.a", 0, 0);
    if (chmod_ret != 0 || chown_ret != 0) {
        printf("linuxsyscallabitest: Linux metadata numbers failed: chmod=%d chown=%d uid=%d euid=%d\n",
               chmod_ret, chown_ret, getuid(), geteuid());
        unlink("linuxabi.vfs.a");
        exit(1);
    }
    printf("linuxsyscallabitest: VFS new utime/utimes...\n");
    struct linux_utimbuf ub = {123, 456};
    struct linux_timeval tvs[2] = {{789, 1000}, {790, 2000}};
    if (raw_linux_syscall2(LINUX_NR_UTIME, (int64)"linuxabi.vfs.a",
                           (int64)&ub) != 0 ||
        raw_linux_syscall2(LINUX_NR_UTIMES, (int64)"linuxabi.vfs.a",
                           (int64)tvs) != 0) {
        printf("linuxsyscallabitest: Linux utime/utimes numbers failed\n");
        unlink("linuxabi.vfs.a");
        exit(1);
    }

    printf("linuxsyscallabitest: VFS new openat2...\n");
    memset(&how, 0, sizeof(how));
    how.flags = O_CREAT | O_RDWR | O_TRUNC;
    how.mode = 0644;
    fd2 = raw_linux_syscall4(LINUX_NR_OPENAT2, AT_FDCWD,
                             (int64)"linuxabi.vfs.b", (int64)&how,
                             sizeof(how));
    if (fd2 < 0) {
        printf("linuxsyscallabitest: Linux openat2 number failed: %d\n", fd2);
        unlink("linuxabi.vfs.a");
        exit(1);
    }
    close(fd2);

    printf("linuxsyscallabitest: VFS new copy_file_range...\n");
    fd = open("linuxabi.vfs.a", O_RDONLY);
    fd2 = open("linuxabi.vfs.b", O_RDWR);
    if (fd < 0 || fd2 < 0 ||
        raw_linux_syscall6(LINUX_NR_COPY_FILE_RANGE, fd, (int64)&in_off,
                           fd2, (int64)&out_off, 1, 0) != 1) {
        printf("linuxsyscallabitest: Linux copy_file_range number failed\n");
        if (fd >= 0) close(fd);
        if (fd2 >= 0) close(fd2);
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }
    close(fd);
    lseek(fd2, 0, 0);
    if (read(fd2, &out, 1) != 1 || out != b) {
        printf("linuxsyscallabitest: Linux copy_file_range data mismatch\n");
        close(fd2);
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }

    printf("linuxsyscallabitest: VFS new close_range...\n");
    int dupfd = raw_linux_syscall3(LINUX_NR_DUP3, fd2, fd2 + 20, 0);
    if (dupfd != fd2 + 20 ||
        raw_linux_syscall3(LINUX_NR_CLOSE_RANGE, dupfd, dupfd, 0) != 0 ||
        raw_linux_syscall3(LINUX_NR_READ, dupfd, (int64)&out, 1) != -EBADF) {
        printf("linuxsyscallabitest: Linux close_range number failed\n");
        close(fd2);
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }
    close(fd2);

    printf("linuxsyscallabitest: VFS open flags...\n");
    struct stat st;
    unlink("linuxabi.vfs.mode");
    unlink("linuxabi.vfs.link");
    raw_linux_syscall2(LINUX_NR_SYMLINK, (int64)"linuxabi.vfs.a",
                       (int64)"linuxabi.vfs.link");
    int pfd = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD,
                                 (int64)"linuxabi.vfs.a",
                                 O_PATH | O_CLOEXEC, 0);
    if (pfd < 0 ||
        raw_linux_syscall2(LINUX_NR_FSTAT, pfd, (int64)&st) != 0 ||
        raw_linux_syscall3(LINUX_NR_READ, pfd, (int64)&out, 1) != -EBADF) {
        printf("linuxsyscallabitest: Linux O_PATH behavior failed: %d\n", pfd);
        if (pfd >= 0)
            close(pfd);
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }
    close(pfd);
    pfd = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD,
                             (int64)"linuxabi.vfs.link",
                             O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
    if (pfd < 0 ||
        raw_linux_syscall2(LINUX_NR_FSTAT, pfd, (int64)&st) != 0 ||
        !S_ISLNK(st.st_mode)) {
        printf("linuxsyscallabitest: Linux O_PATH|O_NOFOLLOW failed: %d\n",
               pfd);
        if (pfd >= 0)
            close(pfd);
        unlink("linuxabi.vfs.link");
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }
    close(pfd);
    int nofollow_ret = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD,
                                          (int64)"linuxabi.vfs.link",
                                          O_NOFOLLOW, 0);
    int old_open_ret = raw_linux_syscall3(LINUX_NR_OPEN,
                                          (int64)"linuxabi.vfs.link",
                                          O_NOFOLLOW, 0);
    int direct_open_ret = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD,
                                            (int64)"linuxabi.vfs.link",
                                            O_RDONLY | O_NOATIME | O_DIRECT, 0);
    if (nofollow_ret != -ELOOP || old_open_ret != -ELOOP ||
        direct_open_ret < 0) {
        printf("linuxsyscallabitest: Linux symlink/open flag matrix failed\n");
        if (direct_open_ret >= 0)
            close(direct_open_ret);
        unlink("linuxabi.vfs.link");
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }
    close(direct_open_ret);
    fd = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD,
                            (int64)"linuxabi.vfs.mode",
                            O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 ||
        raw_linux_syscall2(LINUX_NR_FSTAT, fd, (int64)&st) != 0 ||
        (st.st_mode & 0777) != 0600) {
        printf("linuxsyscallabitest: Linux open create mode failed: %d mode=%o\n",
               fd, st.st_mode & 0777);
        if (fd >= 0)
            close(fd);
        unlink("linuxabi.vfs.mode");
        unlink("linuxabi.vfs.link");
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }
    close(fd);
    int odir_ret = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD,
                                      (int64)"linuxabi.vfs.a", O_DIRECTORY, 0);
    int otmp_ret = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD, (int64)"/",
                                      O_TMPFILE | O_RDWR, 0600);
    int slash_ret = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD,
                                       (int64)"linuxabi.vfs.a/", O_RDONLY, 0);
    int creat_dir_ret = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD,
                                           (int64)"/", O_CREAT | O_RDWR, 0600);
    if (odir_ret != -ENOTDIR || otmp_ret != -ENOTSUP ||
        slash_ret != -ENOTDIR || creat_dir_ret != -EISDIR) {
        printf("linuxsyscallabitest: Linux open flag errno matrix failed: "
               "odir=%d otmp=%d slash=%d creatdir=%d\n",
               odir_ret, otmp_ret, slash_ret, creat_dir_ret);
        unlink("linuxabi.vfs.mode");
        unlink("linuxabi.vfs.link");
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }
    unlink("linuxabi.vfs.mode");
    unlink("linuxabi.vfs.link");

    printf("linuxsyscallabitest: VFS cache/sync hints...\n");
    fd = open("linuxabi.vfs.a", O_RDONLY);
    if (fd < 0 ||
        raw_linux_syscall3(LINUX_NR_READAHEAD, fd, 0, 4096) != 0 ||
        raw_linux_syscall4(LINUX_NR_SYNC_FILE_RANGE, fd, 0, 1,
                           SYNC_FILE_RANGE_WAIT_BEFORE |
                               SYNC_FILE_RANGE_WAIT_AFTER) != 0 ||
        raw_linux_syscall2(LINUX_NR_SYNCFS, fd, 0) != 0) {
        printf("linuxsyscallabitest: Linux cache/sync hint syscalls failed\n");
        if (fd >= 0)
            close(fd);
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }
    close(fd);

    printf("linuxsyscallabitest: VFS new inotify...\n");
    int ifd = raw_linux_syscall2(LINUX_NR_INOTIFY_INIT1,
                                 LINUX_IN_NONBLOCK | LINUX_IN_CLOEXEC, 0);
    if (ifd < 0) {
        printf("linuxsyscallabitest: Linux inotify_init1 number failed: %d\n",
               ifd);
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }
    int wd = raw_linux_syscall3(LINUX_NR_INOTIFY_ADD_WATCH, ifd,
                                (int64)"linuxabi.vfs.a", LINUX_IN_MODIFY);
    int inotify_empty_avail = -1;
    int inotify_avail = -1;
    int inotify_avail_after = -1;
    char inotify_buf[sizeof(struct linux_inotify_event_abi) + 16];
    memset(inotify_buf, 0, sizeof(inotify_buf));
    int inotify_empty_ioctl =
        raw_linux_syscall3(LINUX_NR_IOCTL, ifd, LINUX_FIONREAD,
                           (int64)&inotify_empty_avail);
    int inotify_empty_read =
        raw_linux_syscall3(LINUX_NR_READ, ifd, (int64)inotify_buf,
                           sizeof(inotify_buf));
    fd = open("linuxabi.vfs.a", O_WRONLY);
    int inotify_write_ok = fd >= 0 && write(fd, &b, 1) == 1;
    if (fd >= 0)
        close(fd);
    int inotify_ready_ioctl =
        raw_linux_syscall3(LINUX_NR_IOCTL, ifd, LINUX_FIONREAD,
                           (int64)&inotify_avail);
    int inotify_read =
        raw_linux_syscall3(LINUX_NR_READ, ifd, (int64)inotify_buf,
                           sizeof(inotify_buf));
    struct linux_inotify_event_abi *iev =
        (struct linux_inotify_event_abi *)inotify_buf;
    int inotify_drained_ioctl =
        raw_linux_syscall3(LINUX_NR_IOCTL, ifd, LINUX_FIONREAD,
                           (int64)&inotify_avail_after);
    int inotify_rm =
        raw_linux_syscall2(LINUX_NR_INOTIFY_RM_WATCH, ifd, wd);
    if (wd <= 0 || inotify_empty_ioctl != 0 || inotify_empty_avail != 0 ||
        inotify_empty_read != -EAGAIN || !inotify_write_ok ||
        inotify_ready_ioctl != 0 ||
        inotify_avail < (int)sizeof(struct linux_inotify_event_abi) ||
        inotify_avail > (int)sizeof(inotify_buf) ||
        inotify_read != inotify_avail || iev->wd != wd ||
        (iev->mask & LINUX_IN_MODIFY) == 0 || inotify_drained_ioctl != 0 ||
        inotify_avail_after != 0 || inotify_rm != 0) {
        printf("linuxsyscallabitest: Linux inotify numbers failed: "
               "wd=%d empty_ioctl=%d empty_avail=%d empty_read=%d "
               "write_ok=%d ready_ioctl=%d ready_avail=%d read=%d "
               "ev_wd=%d ev_mask=0x%x drained_ioctl=%d drained_avail=%d "
               "rm=%d\n",
               wd, inotify_empty_ioctl, inotify_empty_avail, inotify_empty_read,
               inotify_write_ok, inotify_ready_ioctl, inotify_avail,
               inotify_read, iev->wd, iev->mask, inotify_drained_ioctl,
               inotify_avail_after, inotify_rm);
        close(ifd);
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }
    close(ifd);

    printf("linuxsyscallabitest: VFS new xattr stubs...\n");
    if (raw_linux_syscall5(LINUX_NR_SETXATTR, (int64)"linuxabi.vfs.a",
                           (int64)"user.none", 0, 0, 0) != -ENOTSUP ||
        raw_linux_syscall4(LINUX_NR_GETXATTR, (int64)"linuxabi.vfs.a",
                           (int64)"user.none", 0, 0) != -ENOTSUP ||
        raw_linux_syscall3(LINUX_NR_LISTXATTR, (int64)"linuxabi.vfs.a",
                           0, 0) != -ENOTSUP ||
        raw_linux_syscall2(LINUX_NR_REMOVEXATTR, (int64)"linuxabi.vfs.a",
                           (int64)"user.none") != -ENOTSUP) {
        printf("linuxsyscallabitest: Linux xattr stub numbers failed\n");
        unlink("linuxabi.vfs.a");
        unlink("linuxabi.vfs.b");
        exit(1);
    }

    unlink("linuxabi.vfs.a");
    unlink("linuxabi.vfs.b");
    printf("linuxsyscallabitest: Linux VFS new numbers OK\n");
}

static void test_linux_optional_compat_numbers(void)
{
    static const struct {
        int nr;
        int expect;
    } compat[] = {
        { 101, -EPERM },  /* ptrace */
        { 103, -EPERM },  /* syslog */
        { 134, -EINVAL }, /* uselib */
        { 135, 0 },       /* personality query */
        { 136, -EINVAL }, /* ustat */
        { 139, -EINVAL }, /* sysfs */
        { 167, -EPERM },  /* swapon, collides with old generic prctl */
        { 178, -EINVAL }, /* query_module, collides with old generic gettid */
        { 179, -ENODEV }, /* quotactl, collides with old generic sysinfo */
        { 214, -EINVAL }, /* epoll_ctl_old, collides with old generic brk */
        { 215, -EINVAL }, /* epoll_wait_old, collides with old generic munmap */
        { 216, -EINVAL }, /* remap_file_pages, collides with old generic mremap */
        { 279, -EINVAL }, /* move_pages, collides with old memfd_create_generic */
    };

    for (uint64 i = 0; i < sizeof(compat) / sizeof(compat[0]); i++) {
        int got = raw_linux_syscall6(compat[i].nr, 0, 0, 0, 0, 0, 0);
        if (got != compat[i].expect) {
            printf("linuxsyscallabitest: Linux optional number %d got %d want %d\n",
                   compat[i].nr, got, compat[i].expect);
            exit(1);
        }
    }

    printf("linuxsyscallabitest: optional compatibility numbers OK\n");
}

static void test_linux_wrong_dispatch_regressions(void)
{
    char read_buf;
    if (raw_linux_syscall3(LINUX_NR_READ, -1, (int64)&read_buf, 1) != -EBADF) {
        printf("linuxsyscallabitest: Linux read(0) number misdispatched\n");
        exit(1);
    }

    if (raw_linux_syscall0(LINUX_NR_GETUID) != getuid()) {
        printf("linuxsyscallabitest: Linux getuid(102) number misdispatched\n");
        exit(1);
    }

    if (raw_linux_syscall0(LINUX_NR_GETPPID) != getppid()) {
        printf("linuxsyscallabitest: Linux getppid(110) number misdispatched\n");
        exit(1);
    }

    struct statfs sfs;
    memset(&sfs, 0, sizeof(sfs));
    if (raw_linux_syscall2(LINUX_NR_STATFS, (int64)"/", (int64)&sfs) != 0 ||
        sfs.f_bsize == 0) {
        printf("linuxsyscallabitest: Linux statfs(137) number misdispatched\n");
        exit(1);
    }
    memset(&sfs, 0, sizeof(sfs));
    int rootfd = raw_linux_syscall4(LINUX_NR_OPENAT, AT_FDCWD, (int64)"/",
                                    O_RDONLY | O_DIRECTORY, 0);
    if (rootfd < 0 ||
        raw_linux_syscall2(LINUX_NR_FSTATFS, rootfd, (int64)&sfs) != 0 ||
        sfs.f_type != 0xEF53 || sfs.f_bsize == 0 ||
        raw_linux_syscall2(LINUX_NR_FSTATFS, -1, (int64)&sfs) != -EBADF) {
        printf("linuxsyscallabitest: Linux fstatfs(138) number/layout failed\n");
        if (rootfd >= 0)
            close(rootfd);
        exit(1);
    }
    close(rootfd);

    if (raw_linux_syscall6(LINUX_NR_MOVE_PAGES, 0, 0, 0, 0, 0, 0) != -EINVAL) {
        printf("linuxsyscallabitest: Linux move_pages(279) collision misdispatched\n");
        exit(1);
    }

    printf("linuxsyscallabitest: wrong-dispatch regressions OK\n");
}

static int auxv_has_type(uint64 *auxv, int pairs, uint64 type)
{
    for (int i = 0; i < pairs; i++) {
        if (auxv[i * 2] == AT_NULL)
            break;
        if (auxv[i * 2] == type)
            return 1;
    }
    return 0;
}

static void test_linux_proc_exec_snapshot(void)
{
    char cmdline[384];
    uint64 auxv[96];

    int fd = raw_linux_syscall3(LINUX_NR_OPEN, (int64)"/proc/self/cmdline",
                                0, 0);
    if (fd < 0) {
        printf("linuxsyscallabitest: /proc/self/cmdline open failed\n");
        exit(1);
    }
    int n = raw_linux_syscall3(LINUX_NR_READ, fd, (int64)cmdline,
                               sizeof(cmdline));
    raw_linux_syscall2(LINUX_NR_CLOSE, fd, 0);
    if (n <= 0 || cmdline[n - 1] != '\0') {
        printf("linuxsyscallabitest: /proc/self/cmdline is not NUL-separated\n");
        exit(1);
    }

    fd = raw_linux_syscall3(LINUX_NR_OPEN, (int64)"/proc/self/environ", 0, 0);
    if (fd < 0) {
        printf("linuxsyscallabitest: /proc/self/environ open failed\n");
        exit(1);
    }
    n = raw_linux_syscall3(LINUX_NR_READ, fd, (int64)cmdline,
                           sizeof(cmdline));
    raw_linux_syscall2(LINUX_NR_CLOSE, fd, 0);
    if (n < 0) {
        printf("linuxsyscallabitest: /proc/self/environ read failed\n");
        exit(1);
    }

    fd = raw_linux_syscall3(LINUX_NR_OPEN, (int64)"/proc/self/auxv", 0, 0);
    if (fd < 0) {
        printf("linuxsyscallabitest: /proc/self/auxv open failed\n");
        exit(1);
    }
    n = raw_linux_syscall3(LINUX_NR_READ, fd, (int64)auxv, sizeof(auxv));
    raw_linux_syscall2(LINUX_NR_CLOSE, fd, 0);
    if (n <= 0 || (n & 15) != 0) {
        printf("linuxsyscallabitest: /proc/self/auxv size invalid\n");
        exit(1);
    }

    int pairs = n / (int)(sizeof(uint64) * 2);
    if (!auxv_has_type(auxv, pairs, AT_RANDOM) ||
        !auxv_has_type(auxv, pairs, AT_EXECFN) ||
        !auxv_has_type(auxv, pairs, AT_PLATFORM) ||
        !auxv_has_type(auxv, pairs, AT_CLKTCK) ||
        !auxv_has_type(auxv, pairs, AT_HWCAP) ||
        !auxv_has_type(auxv, pairs, AT_HWCAP2)) {
        printf("linuxsyscallabitest: /proc/self/auxv missing startup entries\n");
        exit(1);
    }

    printf("linuxsyscallabitest: Linux proc exec snapshot OK\n");
}
#endif

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "inotify-fionread") == 0) {
        test_linux_inotify_fionread_reducer();
        exit(0);
    }
    if (argc > 1 && strcmp(argv[1], "tls-reject") == 0) {
        test_linux_arch_prctl_tls_rejects();
        exit(0);
    }

    test_linux_munmap_number();
    test_linux_unmapself_sequence();
    test_linux_arch_prctl_tls_rejects();
    test_linux_memory_locking_numbers();
    test_linux_time_numbers();
    test_linux_misc_numbers();
    test_linux_more_native_numbers();
#if defined(__x86_64__)
    test_linux_identity_native_numbers();
    test_linux_memory_native_numbers();
    test_linux_at_fd_native_numbers();
    test_linux_stat_dirent_layouts();
    test_linux_rt_signal_abi();
    test_linux_process_runtime_numbers();
    test_linux_wait_fork_numbers();
    test_linux_vfs_new_numbers();
    test_linux_optional_compat_numbers();
    test_linux_wrong_dispatch_regressions();
    test_linux_proc_exec_snapshot();
#endif
    printf("linuxsyscallabitest: all tests passed\n");
    exit(0);
}

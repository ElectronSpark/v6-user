// linuxsyscallabitest.c - raw Linux-number syscall compatibility tests.
#include "kernel/inc/types.h"
#include "kernel/inc/clone_flags.h"
#include "kernel/inc/uabi/stat.h"
#include "kernel/inc/uabi/statfs.h"
#include "user/user.h"

#define STACK_SIZE (4096 * 4)

#if defined(__x86_64__)
#define LINUX_NR_READ 0
#define LINUX_NR_STAT 4
#define LINUX_NR_FSTAT 5
#define LINUX_NR_LSTAT 6
#define LINUX_NR_RT_SIGACTION 13
#define LINUX_NR_RT_SIGPROCMASK 14
#define LINUX_NR_MUNMAP 11
#define LINUX_NR_EXIT 60
#define LINUX_NR_MREMAP 25
#define LINUX_NR_MSYNC 26
#define LINUX_NR_MINCORE 27
#define LINUX_NR_MADVISE 28
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
#define LINUX_NR_MLOCK 149
#define LINUX_NR_MUNLOCK 150
#define LINUX_NR_MLOCKALL 151
#define LINUX_NR_MUNLOCKALL 152
#define LINUX_NR_PRCTL 157
#define LINUX_NR_SYSINFO 99
#define LINUX_NR_GETDENTS 78
#define LINUX_NR_GETCWD 79
#define LINUX_NR_GETTID 186
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
#define LINUX_NR_PIPE2 293
#define LINUX_NR_DUP3 292
#define LINUX_NR_PREADV 295
#define LINUX_NR_PWRITEV 296
#define LINUX_NR_PREADV2 327
#define LINUX_NR_PWRITEV2 328
#define LINUX_NR_PRLIMIT64 302
#define LINUX_NR_MEMBARRIER 324
#define LINUX_NR_MLOCK2 325
#define LINUX_NR_STATX 332
#define LINUX_NR_MOVE_PAGES 279
#else
#define LINUX_NR_MUNMAP 215
#define LINUX_NR_EXIT 93
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
#define EAGAIN 11
#define EFAULT 14
#define ENOSYS 38
#define EINVAL 22
#define AT_FDCWD -100
#define AT_EMPTY_PATH 0x1000
#define RLIMIT_NOFILE 7
#define MEMBARRIER_CMD_QUERY 0
#define MREMAP_MAYMOVE 1
#define MLOCK_ONFAULT 0x1
#define MCL_CURRENT 0x1
#define MCL_FUTURE 0x2
#define MCL_ONFAULT 0x4
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_CLOEXEC 02000000
#define O_RDWR 02
#define LINUX_SIG_SETMASK 2

struct linux_timespec {
    int64 tv_sec;
    int64 tv_nsec;
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

    struct clone_args args = {
        .flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                 CLONE_THREAD | SIGCHLD,
        .stack = (uint64)child_stack,
        .stack_size = STACK_SIZE,
        .entry = (uint64)raw_linux_unmapself_child,
    };

    int pid = clone(&args);
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

    int fd = raw_linux_syscall5(LINUX_NR_OPENAT, AT_FDCWD,
                                (int64)"linuxabi.tmp",
                                O_CREAT | O_TRUNC | O_RDWR, 0644, 0);
    if (fd < 0) {
        printf("linuxsyscallabitest: Linux openat number failed: %d\n", fd);
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

    int dupfd = raw_linux_syscall3(LINUX_NR_DUP3, fd, fd + 10, O_CLOEXEC);
    if (dupfd != fd + 10) {
        printf("linuxsyscallabitest: Linux dup3 number failed\n");
        close(fd);
        unlink("linuxabi.tmp");
        exit(1);
    }
    close(dupfd);

    if (raw_linux_syscall2(LINUX_NR_PIPE2, (int64)pipefd, O_CLOEXEC) != 0) {
        printf("linuxsyscallabitest: Linux pipe2 number failed\n");
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
    sigset_t mask = 0;
    sigset_t oldmask = 0;
    sigset_t pending = 0;
    siginfo_t info;
    struct linux_timespec zero = {0, 0};

    memset(&oldact, 0, sizeof(oldact));
    if (raw_linux_syscall4(LINUX_NR_RT_SIGACTION, SIGUSR1, 0,
                           (int64)&oldact, sizeof(sigset_t)) != 0) {
        printf("linuxsyscallabitest: Linux rt_sigaction query failed\n");
        exit(1);
    }

    if (raw_linux_syscall4(LINUX_NR_RT_SIGACTION, SIGUSR1, 0,
                           (int64)&oldact, 4) != -EINVAL) {
        printf("linuxsyscallabitest: Linux rt_sigaction bad sigsetsize accepted\n");
        exit(1);
    }

    if (raw_linux_syscall4(LINUX_NR_RT_SIGPROCMASK, LINUX_SIG_SETMASK, 0,
                           (int64)&oldmask, sizeof(sigset_t)) != 0 ||
        raw_linux_syscall4(LINUX_NR_RT_SIGPROCMASK, LINUX_SIG_SETMASK,
                           (int64)&mask, (int64)&oldmask, 4) != -EINVAL) {
        printf("linuxsyscallabitest: Linux rt_sigprocmask ABI failed\n");
        exit(1);
    }

    if (raw_linux_syscall2(LINUX_NR_RT_SIGPENDING, (int64)&pending,
                           sizeof(sigset_t)) != 0 ||
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

    memset(&info, 0, sizeof(info));
    if (raw_linux_syscall4(LINUX_NR_RT_SIGTIMEDWAIT, (int64)&mask,
                           (int64)&info, (int64)&zero, sizeof(sigset_t)) !=
            -EAGAIN ||
        raw_linux_syscall4(LINUX_NR_RT_SIGTIMEDWAIT, (int64)&mask,
                           (int64)&info, (int64)&zero, 4) != -EINVAL) {
        printf("linuxsyscallabitest: Linux rt_sigtimedwait ABI failed\n");
        exit(1);
    }

    printf("linuxsyscallabitest: Linux rt signal ABI OK\n");
}

static void test_linux_legacy_range_unsupported_numbers(void)
{
    static const int unsupported[] = {
        100, 101, 103, 111, 122, 123, 125, 126, 132, 134, 135, 136, 139,
        142, 143,
    };

    for (uint64 i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        int nr = unsupported[i];
        if (raw_linux_syscall6(nr, 0, 0, 0, 0, 0, 0) != -ENOSYS) {
            printf("linuxsyscallabitest: Linux unsupported number %d misdispatched\n",
                   nr);
            exit(1);
        }
    }

    printf("linuxsyscallabitest: legacy-range unsupported numbers OK\n");
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

    if (raw_linux_syscall6(LINUX_NR_MOVE_PAGES, 0, 0, 0, 0, 0, 0) != -ENOSYS) {
        printf("linuxsyscallabitest: Linux move_pages(279) collision misdispatched\n");
        exit(1);
    }

    printf("linuxsyscallabitest: wrong-dispatch regressions OK\n");
}
#endif

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_linux_munmap_number();
    test_linux_unmapself_sequence();
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
    test_linux_legacy_range_unsupported_numbers();
    test_linux_wrong_dispatch_regressions();
#endif
    printf("linuxsyscallabitest: all tests passed\n");
    exit(0);
}

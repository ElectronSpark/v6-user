// linuxsyscallabitest.c - raw Linux-number syscall compatibility tests.
#include "kernel/inc/types.h"
#include "kernel/inc/clone_flags.h"
#include "user/user.h"

#define STACK_SIZE (4096 * 4)

#if defined(__x86_64__)
#define LINUX_NR_MUNMAP 11
#define LINUX_NR_EXIT 60
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
#define LINUX_NR_PRLIMIT64 302
#define LINUX_NR_MEMBARRIER 324
#define LINUX_NR_MLOCK2 325
#define LINUX_NR_STATX 332
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
#define EFAULT 14
#define EINVAL 22
#define AT_FDCWD -100
#define RLIMIT_NOFILE 7
#define MEMBARRIER_CMD_QUERY 0
#define MLOCK_ONFAULT 0x1
#define MCL_CURRENT 0x1
#define MCL_FUTURE 0x2
#define MCL_ONFAULT 0x4
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define PR_SET_NAME 15
#define PR_GET_NAME 16

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
    printf("linuxsyscallabitest: all tests passed\n");
    exit(0);
}

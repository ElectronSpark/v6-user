#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

//
// wrapper so that it's OK if main() does not call exit().
// On RISC-V a0 serves as both the exec return (argc) and the first function
// argument, so main() automatically receives argc.  On x86_64 the exec return
// is in RAX while the first function argument is in RDI — exec sets both
// RDI=argc and RSI=argv in the trap frame, so start() must forward them.
//
// On x86_64 start() is the ELF entry point, so the kernel hands us a
// 16-byte-aligned RSP. The SysV AMD64 ABI then requires that RSP at
// `call main` be (16N+8) mod 16 (i.e. 8 mod 16) so that the callee's
// `push %rbp` re-aligns RSP to 16. Without that, GCC's `movaps` against
// stack slots in main() / fork() / etc. faults #GP. A C `start()` would
// emit its own `push %rbp` prologue and break this invariant, so on
// x86_64 we use a naked asm trampoline instead.
//
#if defined(CONFIG_ARCH_X86_64)
extern int main(int, char **);
__attribute__((naked, noreturn)) void start(int argc, char *argv[]) {
    __asm__ volatile(
        "xorl  %%ebp, %%ebp\n\t"     /* ABI: clear frame pointer */
        "andq  $-16, %%rsp\n\t"      /* RSP 16-aligned; the CALL  */
                                     /* below pushes 8 bytes so   */
                                     /* main() enters at 8 mod 16 */
        "callq main\n\t"
        "movl  %%eax, %%edi\n\t"
        "callq exit\n\t"
        "ud2\n\t"
        ::: "memory");
}
#else
void start(int argc, char *argv[]) {
    extern int main(int, char **);
    main(argc, argv);
    exit(0);
}
#endif

// fork() wrapper - calls clone with default fork args
int fork(void) {
    struct clone_args args = {
        .flags = 0,
        .stack = 0,
        .stack_size = 0,
        .entry = 0,
        .esignal = SIGCHLD,
        .tls = 0,
        .ctid = 0,
        .ptid = 0,
    };
    return clone(&args);
}

// vfork() is now a direct syscall (SYS_vfork) with a pure assembly stub
// in usys.S. This avoids stack corruption: the C wrapper would save
// callee-saved registers on the shared user stack, which the child would
// overwrite when calling functions before exec/exit.

char *strcpy(char *s, const char *t) {
    char *os;

    os = s;
    while ((*s++ = *t++) != 0)
        ;
    return os;
}

int strcmp(const char *p, const char *q) {
    while (*p && *p == *q)
        p++, q++;
    return (uchar)*p - (uchar)*q;
}

uint strlen(const char *s) {
    int n;

    for (n = 0; s[n]; n++)
        ;
    return n;
}

void *memset(void *dst, int c, uint n) {
    char *cdst = (char *)dst;
    int i;
    for (i = 0; i < n; i++) {
        cdst[i] = c;
    }
    return dst;
}

char *strchr(const char *s, char c) {
    for (; *s; s++)
        if (*s == c)
            return (char *)s;
    return 0;
}

char *gets(char *buf, int max) {
    int i, cc;
    char c;

    for (i = 0; i + 1 < max;) {
        cc = read(0, &c, 1);
        if (cc < 1)
            break;
        buf[i++] = c;
        if (c == '\n' || c == '\r')
            break;
    }
    buf[i] = '\0';
    return buf;
}

int atoi(const char *s) {
    int n;

    n = 0;
    while ('0' <= *s && *s <= '9')
        n = n * 10 + *s++ - '0';
    return n;
}

void *memmove(void *vdst, const void *vsrc, int n) {
    char *dst;
    const char *src;

    dst = vdst;
    src = vsrc;
    if (src > dst) {
        while (n-- > 0)
            *dst++ = *src++;
    } else {
        dst += n;
        src += n;
        while (n-- > 0)
            *--dst = *--src;
    }
    return vdst;
}

int memcmp(const void *s1, const void *s2, uint n) {
    const char *p1 = s1, *p2 = s2;
    while (n-- > 0) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

void *memcpy(void *dst, const void *src, uint n) {
    return memmove(dst, src, n);
}

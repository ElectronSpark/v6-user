#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct regs {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
};

static const struct regs expected = {
    .rbx = 0x1111222233334444ULL,
    .rbp = 0x5555666677778888ULL,
    .r12 = 0x1212121212121212ULL,
    .r13 = 0x1313131313131313ULL,
    .r14 = 0x1414141414141414ULL,
    .r15 = 0x1515151515151515ULL,
};

static void fill_bad(struct regs *out)
{
    memset(out, 0xa5, sizeof(*out));
}

static void check_regs(const char *phase, const struct regs *got)
{
    if (memcmp(got, &expected, sizeof(expected)) == 0) {
        printf("regpreservetest: %s OK\n", phase);
        return;
    }

    printf("regpreservetest: %s FAILED\n", phase);
    printf("  rbx got=0x%016llx want=0x%016llx\n",
           (unsigned long long)got->rbx, (unsigned long long)expected.rbx);
    printf("  rbp got=0x%016llx want=0x%016llx\n",
           (unsigned long long)got->rbp, (unsigned long long)expected.rbp);
    printf("  r12 got=0x%016llx want=0x%016llx\n",
           (unsigned long long)got->r12, (unsigned long long)expected.r12);
    printf("  r13 got=0x%016llx want=0x%016llx\n",
           (unsigned long long)got->r13, (unsigned long long)expected.r13);
    printf("  r14 got=0x%016llx want=0x%016llx\n",
           (unsigned long long)got->r14, (unsigned long long)expected.r14);
    printf("  r15 got=0x%016llx want=0x%016llx\n",
           (unsigned long long)got->r15, (unsigned long long)expected.r15);
    exit(1);
}

static __attribute__((noinline, used)) void record_regs(struct regs *out)
{
    asm volatile(
        "movq %%rbx, 0(%0)\n\t"
        "movq %%rbp, 8(%0)\n\t"
        "movq %%r12, 16(%0)\n\t"
        "movq %%r13, 24(%0)\n\t"
        "movq %%r14, 32(%0)\n\t"
        "movq %%r15, 40(%0)\n\t"
        :
        : "r"(out)
        : "memory");
}

static __attribute__((noinline, used)) void set_sentinels(void)
{
    asm volatile(
        "movabsq $0x1111222233334444, %%rbx\n\t"
        "movabsq $0x5555666677778888, %%rbp\n\t"
        "movabsq $0x1212121212121212, %%r12\n\t"
        "movabsq $0x1313131313131313, %%r13\n\t"
        "movabsq $0x1414141414141414, %%r14\n\t"
        "movabsq $0x1515151515151515, %%r15\n\t"
        :
        :
        : "memory");
}

static void test_raw_syscall(struct regs *out)
{
    fill_bad(out);
    asm volatile(
        "pushq %%rbp\n\t"
        "pushq %%rbx\n\t"
        "pushq %%r12\n\t"
        "pushq %%r13\n\t"
        "pushq %%r14\n\t"
        "pushq %%r15\n\t"
        "pushq %[out]\n\t"
        "call set_sentinels\n\t"
        "movq $39, %%rax\n\t"
        "syscall\n\t"
        "movq (%%rsp), %%rdi\n\t"
        "call record_regs\n\t"
        "addq $8, %%rsp\n\t"
        "popq %%r15\n\t"
        "popq %%r14\n\t"
        "popq %%r13\n\t"
        "popq %%r12\n\t"
        "popq %%rbx\n\t"
        "popq %%rbp\n\t"
        :
        : [out] "r"(out)
        : "rax", "rcx", "rdi", "r11", "memory", "cc");
    check_regs("raw-syscall", out);
}

static void test_read_fault(struct regs *out, volatile unsigned char *addr)
{
    fill_bad(out);
    asm volatile(
        "pushq %%rbp\n\t"
        "pushq %%rbx\n\t"
        "pushq %%r12\n\t"
        "pushq %%r13\n\t"
        "pushq %%r14\n\t"
        "pushq %%r15\n\t"
        "pushq %[out]\n\t"
        "pushq %[addr]\n\t"
        "call set_sentinels\n\t"
        "movq (%%rsp), %%rsi\n\t"
        "movzbq (%%rsi), %%rax\n\t"
        "movq 8(%%rsp), %%rdi\n\t"
        "call record_regs\n\t"
        "addq $16, %%rsp\n\t"
        "popq %%r15\n\t"
        "popq %%r14\n\t"
        "popq %%r13\n\t"
        "popq %%r12\n\t"
        "popq %%rbx\n\t"
        "popq %%rbp\n\t"
        :
        : [out] "r"(out), [addr] "r"(addr)
        : "rax", "rdi", "rsi", "memory", "cc");
    check_regs("demand-read-fault", out);
}

static void test_write_fault(struct regs *out, volatile unsigned char *addr)
{
    fill_bad(out);
    asm volatile(
        "pushq %%rbp\n\t"
        "pushq %%rbx\n\t"
        "pushq %%r12\n\t"
        "pushq %%r13\n\t"
        "pushq %%r14\n\t"
        "pushq %%r15\n\t"
        "pushq %[out]\n\t"
        "pushq %[addr]\n\t"
        "call set_sentinels\n\t"
        "movq (%%rsp), %%rsi\n\t"
        "movb $0x5a, (%%rsi)\n\t"
        "movq 8(%%rsp), %%rdi\n\t"
        "call record_regs\n\t"
        "addq $16, %%rsp\n\t"
        "popq %%r15\n\t"
        "popq %%r14\n\t"
        "popq %%r13\n\t"
        "popq %%r12\n\t"
        "popq %%rbx\n\t"
        "popq %%rbp\n\t"
        :
        : [out] "r"(out), [addr] "r"(addr)
        : "rdi", "rsi", "memory", "cc");
    check_regs("demand-write-fault", out);
}

static void test_timer_preempt(struct regs *out)
{
    fill_bad(out);
    asm volatile(
        "pushq %%rbp\n\t"
        "pushq %%rbx\n\t"
        "pushq %%r12\n\t"
        "pushq %%r13\n\t"
        "pushq %%r14\n\t"
        "pushq %%r15\n\t"
        "pushq %[out]\n\t"
        "call set_sentinels\n\t"
        "movq $25000000, %%rax\n\t"
        "1:\n\t"
        "subq $1, %%rax\n\t"
        "jnz 1b\n\t"
        "movq (%%rsp), %%rdi\n\t"
        "call record_regs\n\t"
        "addq $8, %%rsp\n\t"
        "popq %%r15\n\t"
        "popq %%r14\n\t"
        "popq %%r13\n\t"
        "popq %%r12\n\t"
        "popq %%rbx\n\t"
        "popq %%rbp\n\t"
        :
        : [out] "r"(out)
        : "rax", "rdi", "memory", "cc");
    check_regs("timer-preempt", out);
}

int main(void)
{
    struct regs got;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    volatile unsigned char *read_page =
        mmap(NULL, page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    volatile unsigned char *write_page =
        mmap(NULL, page, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (read_page == MAP_FAILED || write_page == MAP_FAILED) {
        printf("regpreservetest: mmap failed errno=%d\n", errno);
        return 1;
    }

    test_raw_syscall(&got);
    test_read_fault(&got, read_page);
    test_write_fault(&got, write_page);
    test_timer_preempt(&got);

    munmap((void *)read_page, page);
    munmap((void *)write_page, page);
    printf("regpreservetest: all tests passed\n");
    return 0;
}

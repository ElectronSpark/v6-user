/**
 * @file vforktest.c
 * @brief Test vfork() implementation
 *
 * Tests that:
 * 1. vfork() creates a child that shares address space with parent
 * 2. Parent blocks until child calls exec or exit
 * 3. Child can modify parent's globals before exit/exec
 */

#include "user.h"
#include "kernel/inc/syscall.h"

volatile int shared_var = 0;
volatile int sequence = 0;
char *echo_argv[] = {"echo", "Child exec'd successfully", 0};
extern char **environ;

static inline __attribute__((always_inline)) long
raw_exec_no_stack(const char *path, char *const argv[])
{
    long ret;
    register long rax asm("rax") = SYS_execve;
    register long rdi asm("rdi") = (long)path;
    register long rsi asm("rsi") = (long)argv;
    register long rdx asm("rdx") = (long)environ;

    asm volatile("syscall"
                 : "+r"(rax)
                 : "r"(rdi), "r"(rsi), "r"(rdx)
                 : "rcx", "r11", "memory");
    ret = rax;
    return ret;
}

static inline __attribute__((always_inline, noreturn)) void
raw_exit_no_stack(int status)
{
    register long rax asm("rax") = SYS_exit;
    register long rdi asm("rdi") = status;

    asm volatile("syscall\n\tud2"
                 :
                 : "r"(rax), "r"(rdi)
                 : "rcx", "r11", "memory");
    __builtin_unreachable();
}

void test_vforkexit(void) {
    printf("=== Test 1: vfork with exit ===\n");

    int before = shared_var;
    printf("Before vfork: shared_var = %d\n", before);

    int pid = vfork();
    if (pid < 0) {
        printf("FAIL: vfork failed\n");
        exit(1);
    }

    if (pid == 0) {
        shared_var = 42;
        raw_exit_no_stack(0);
    }

    // Parent: should see child's modification
    printf("Parent resumed: shared_var = %d\n", shared_var);
    if (shared_var == 42) {
        printf("PASS: Parent sees child's modification\n");
    } else {
        printf("FAIL: shared_var should be 42, got %d\n", shared_var);
        exit(1);
    }

    // Wait for child
    int status;
    wait(&status);
    printf("Child exited with status %d\n", status);
    printf("Test 1 passed!\n\n");
}

void test_vfork_exec(void) {
    printf("=== Test 2: vfork with exec ===\n");

    shared_var = 100;
    printf("Before vfork: shared_var = %d\n", shared_var);

    int pid = vfork();
    if (pid < 0) {
        printf("FAIL: vfork failed\n");
        exit(1);
    }

    if (pid == 0) {
        shared_var = 200;
        raw_exec_no_stack("/bin/echo", echo_argv);
        raw_exit_no_stack(1);
    }

    // Parent: should see child's modification before exec
    printf("Parent resumed: shared_var = %d\n", shared_var);
    if (shared_var == 200) {
        printf("PASS: Parent sees child's modification before exec\n");
    } else {
        printf("FAIL: shared_var should be 200, got %d\n", shared_var);
        exit(1);
    }

    // Wait for child
    int status;
    wait(&status);
    printf("Child exited with status %d\n", status);
    if (status != 0) {
        printf("FAIL: exec child should exit 0\n");
        exit(1);
    }
    printf("Test 2 passed!\n\n");
}

void test_vfork_ordering(void) {
    printf("=== Test 3: vfork parent blocks until child finishes ===\n");

    sequence = 0;

    int pid = vfork();
    if (pid < 0) {
        printf("FAIL: vfork failed\n");
        exit(1);
    }

    if (pid == 0) {
        sequence = 1;
        raw_exit_no_stack(0);
    }

    // Parent: sequence should already be 1 because parent was blocked
    printf("Parent: sequence = %d\n", sequence);
    if (sequence == 1) {
        printf("PASS: Parent correctly blocked until child finished\n");
    } else {
        printf("FAIL: sequence should be 1, got %d (parent ran before child "
               "finished)\n",
               sequence);
        exit(1);
    }

    wait(0);
    printf("Test 3 passed!\n\n");
}

int main(int argc, char *argv[]) {
    printf("vforktest: starting\n\n");

    test_vforkexit();
    test_vfork_exec();
    test_vfork_ordering();

    printf("All vfork tests passed!\n");
    exit(0);
}

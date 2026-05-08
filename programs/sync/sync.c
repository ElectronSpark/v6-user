#ifdef HOST_LIBC_PROGRAM
#include "host_compat.h"
#else
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#endif

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    sync();
    exit(0);
}

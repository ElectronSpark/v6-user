#ifdef HOST_LIBC_PROGRAM
#include "host_compat.h"
#else
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#endif

int main(int argc, char *argv[]) {
    int i;

    for (i = 1; i < argc; i++) {
        if (write(1, argv[i], strlen(argv[i])) < 0)
            exit(1);
        if (i + 1 < argc) {
            if (write(1, " ", 1) < 0)
                exit(1);
        } else {
            if (write(1, "\n", 1) < 0)
                exit(1);
        }
    }
    exit(0);
}

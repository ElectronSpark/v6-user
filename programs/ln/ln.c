#ifdef HOST_LIBC_PROGRAM
#include "host_compat.h"
#else
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#endif

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(2, "Usage: ln old new\n");
        exit(1);
    }
    if (link(argv[1], argv[2]) < 0)
        fprintf(2, "link %s %s: failed\n", argv[1], argv[2]);
    exit(0);
}

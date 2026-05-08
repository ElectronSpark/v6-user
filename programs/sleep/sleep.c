#ifdef HOST_LIBC_PROGRAM
#include "host_compat.h"
#else
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#endif

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("sleep must receive the number of miniseconds as parameter\n");
        exit(1);
    }
    int secs = atoi(argv[1]);
    sleep(secs);
    exit(0);
}

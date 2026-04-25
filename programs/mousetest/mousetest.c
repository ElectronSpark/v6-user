//
// mousetest: read up to 20 events from /dev/mouse and print them.
// Useful to confirm the kernel side delivers PS/2 mouse data to
// userspace independently of any compositor.
//
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/vfs/fcntl.h"
#include "kernel/inc/dev/ps2mouse.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    int fd = open("/dev/mouse", O_RDONLY);
    if (fd < 0) {
        printf("mousetest: open /dev/mouse failed\n");
        exit(1);
    }
    printf("mousetest: /dev/mouse fd=%d - move the mouse...\n", fd);

    struct mouse_event ev;
    int got = 0;
    int spins = 0;
    while (got < 20 && spins < 500) {
        int n = read(fd, &ev, sizeof(ev));
        if (n == (int)sizeof(ev)) {
            got++;
            printf("mousetest: ev #%d dx=%d dy=%d btn=0x%x dz=%d\n",
                   got, ev.dx, ev.dy, ev.buttons, ev.dz);
            spins = 0;
            continue;
        }
        // -EAGAIN or partial; sleep a tick.
        spins++;
        sleep(1);
    }
    printf("mousetest: done (%d events)\n", got);
    close(fd);
    exit(0);
}

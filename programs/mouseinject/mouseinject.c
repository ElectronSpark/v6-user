#include "kernel/inc/types.h"
#include "kernel/inc/vfs/fcntl.h"
#include "kernel/inc/dev/ps2mouse.h"
#include "user/user.h"

static int parse_coord(const char *s, int fallback)
{
    int v;

    if (!s)
        return fallback;
    v = atoi(s);
    if (v < 0)
        return 0;
    if (v > 65535)
        return 65535;
    return v;
}

int main(int argc, char **argv)
{
    struct mouse_event ev;
    int fd;
    int n;

    memset(&ev, 0, sizeof(ev));
    ev.flags = MOUSE_EVENT_F_ABSOLUTE;
    ev.dx = (int16)parse_coord(argc > 1 ? argv[1] : NULL, 32768);
    ev.dy = (int16)parse_coord(argc > 2 ? argv[2] : NULL, 32768);
    ev.buttons = (uint8)parse_coord(argc > 3 ? argv[3] : NULL, 0);

    fd = open("/dev/mouse", O_RDWR);
    if (fd < 0) {
        printf("mouseinject: open /dev/mouse failed\n");
        return 1;
    }
    n = write(fd, &ev, sizeof(ev));
    if (n != (int)sizeof(ev)) {
        printf("mouseinject: write failed n=%d size=%d\n", n, (int)sizeof(ev));
        close(fd);
        return 1;
    }
    close(fd);
    printf("mouseinject: absolute x=%d y=%d buttons=0x%x\n",
           (uint16)ev.dx, (uint16)ev.dy, ev.buttons);
    return 0;
}

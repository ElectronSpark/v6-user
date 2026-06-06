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

static int parse_int(const char *s, int fallback, int min, int max)
{
    int v;

    if (!s)
        return fallback;
    v = atoi(s);
    if (v < min)
        return min;
    if (v > max)
        return max;
    return v;
}

static int write_event(int fd, int x, int y, int buttons)
{
    struct mouse_event ev;
    int n;

    memset(&ev, 0, sizeof(ev));
    ev.flags = MOUSE_EVENT_F_ABSOLUTE;
    ev.dx = (int16)parse_coord(0, x);
    ev.dy = (int16)parse_coord(0, y);
    ev.buttons = (uint8)buttons;
    n = write(fd, &ev, sizeof(ev));
    if (n != (int)sizeof(ev)) {
        printf("mouseinject: write failed n=%d size=%d\n",
               n, (int)sizeof(ev));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct mouse_event ev;
    int fd;
    int n;

    if (argc > 1 && strcmp(argv[1], "drag") == 0) {
        int x0 = parse_coord(argc > 2 ? argv[2] : NULL, 32768);
        int y0 = parse_coord(argc > 3 ? argv[3] : NULL, 32768);
        int x1 = parse_coord(argc > 4 ? argv[4] : NULL, x0);
        int y1 = parse_coord(argc > 5 ? argv[5] : NULL, y0);
        int steps = parse_int(argc > 6 ? argv[6] : NULL, 48, 1, 512);
        int delay_us = parse_int(argc > 7 ? argv[7] : NULL, 1000, 0, 1000000);

        fd = open("/dev/mouse", O_RDWR);
        if (fd < 0) {
            printf("mouseinject: open /dev/mouse failed\n");
            return 1;
        }
        if (write_event(fd, x0, y0, 0) < 0 ||
            write_event(fd, x0, y0, 1) < 0) {
            close(fd);
            return 1;
        }
        for (int i = 1; i <= steps; i++) {
            int x = x0 + (int)(((long)(x1 - x0) * i) / steps);
            int y = y0 + (int)(((long)(y1 - y0) * i) / steps);

            if (write_event(fd, x, y, 1) < 0) {
                close(fd);
                return 1;
            }
            if (delay_us > 0)
                usleep(delay_us);
        }
        if (write_event(fd, x1, y1, 0) < 0) {
            close(fd);
            return 1;
        }
        close(fd);
        printf("mouseinject: drag x0=%d y0=%d x1=%d y1=%d steps=%d delay_us=%d\n",
               x0, y0, x1, y1, steps, delay_us);
        return 0;
    }

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

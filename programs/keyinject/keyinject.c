#include "kernel/inc/types.h"
#include "kernel/inc/vfs/fcntl.h"
#include "kernel/inc/dev/ps2kbd.h"
#include "user/user.h"

struct key_map {
    char ch;
    uint8 code;
    uint8 shift;
};

static const struct key_map key_map[] = {
    { '1', 2, 0 }, { '2', 3, 0 }, { '3', 4, 0 }, { '4', 5, 0 },
    { '5', 6, 0 }, { '6', 7, 0 }, { '7', 8, 0 }, { '8', 9, 0 },
    { '9', 10, 0 }, { '0', 11, 0 }, { '-', 12, 0 }, { '=', 13, 0 },
    { 'q', 16, 0 }, { 'w', 17, 0 }, { 'e', 18, 0 }, { 'r', 19, 0 },
    { 't', 20, 0 }, { 'y', 21, 0 }, { 'u', 22, 0 }, { 'i', 23, 0 },
    { 'o', 24, 0 }, { 'p', 25, 0 }, { '[', 26, 0 }, { ']', 27, 0 },
    { 'a', 30, 0 }, { 's', 31, 0 }, { 'd', 32, 0 }, { 'f', 33, 0 },
    { 'g', 34, 0 }, { 'h', 35, 0 }, { 'j', 36, 0 }, { 'k', 37, 0 },
    { 'l', 38, 0 }, { ';', 39, 0 }, { '\'', 40, 0 }, { '`', 41, 0 },
    { '\\', 43, 0 }, { 'z', 44, 0 }, { 'x', 45, 0 }, { 'c', 46, 0 },
    { 'v', 47, 0 }, { 'b', 48, 0 }, { 'n', 49, 0 }, { 'm', 50, 0 },
    { ',', 51, 0 }, { '.', 52, 0 }, { '/', 53, 0 }, { ' ', 57, 0 },
    { '!', 2, 1 }, { '@', 3, 1 }, { '#', 4, 1 }, { '$', 5, 1 },
    { '%', 6, 1 }, { '^', 7, 1 }, { '&', 8, 1 }, { '*', 9, 1 },
    { '(', 10, 1 }, { ')', 11, 1 }, { '_', 12, 1 }, { '+', 13, 1 },
    { '{', 26, 1 }, { '}', 27, 1 }, { ':', 39, 1 }, { '"', 40, 1 },
    { '~', 41, 1 }, { '|', 43, 1 }, { '<', 51, 1 }, { '>', 52, 1 },
    { '?', 53, 1 },
};

static int write_key(int fd, uint8 code, uint8 pressed, uint8 modifiers)
{
    struct kbd_event ev;
    int n;

    memset(&ev, 0, sizeof(ev));
    ev.scancode = code;
    ev.pressed = pressed;
    ev.modifiers = modifiers;
    n = write(fd, &ev, sizeof(ev));
    if (n != (int)sizeof(ev)) {
        printf("keyinject: write failed n=%d errno=%d size=%d\n",
               n, n < 0 ? -n : 0, (int)sizeof(ev));
        return -1;
    }
    return 0;
}

static const struct key_map *lookup_char(char ch)
{
    char lower = ch;

    if (ch >= 'A' && ch <= 'Z')
        lower = (char)(ch - 'A' + 'a');
    for (int i = 0; i < (int)(sizeof(key_map) / sizeof(key_map[0])); i++) {
        if (key_map[i].ch == lower)
            return &key_map[i];
    }
    return 0;
}

static int tap_key(int fd, uint8 code, uint8 modifiers)
{
    if (write_key(fd, code, 1, modifiers) < 0)
        return -1;
    usleep(12000);
    if (write_key(fd, code, 0, modifiers) < 0)
        return -1;
    usleep(12000);
    return 0;
}

static int type_char(int fd, char ch)
{
    const struct key_map *km = lookup_char(ch);
    uint8 shift;

    if (!km) {
        printf("keyinject: unsupported char 0x%x\n", (uint8)ch);
        return -1;
    }
    shift = (uint8)(km->shift || (ch >= 'A' && ch <= 'Z'));
    if (shift && write_key(fd, 42, 1, KBD_MOD_SHIFT) < 0)
        return -1;
    if (tap_key(fd, km->code, shift ? KBD_MOD_SHIFT : 0) < 0)
        return -1;
    if (shift && write_key(fd, 42, 0, 0) < 0)
        return -1;
    return 0;
}

static int key_code(const char *name)
{
    if (strcmp(name, "enter") == 0)
        return 28;
    if (strcmp(name, "tab") == 0)
        return 15;
    if (strcmp(name, "backspace") == 0)
        return 14;
    if (strcmp(name, "delete") == 0)
        return 111;
    if (strcmp(name, "esc") == 0 || strcmp(name, "escape") == 0)
        return 1;
    if (strlen(name) == 1) {
        const struct key_map *km = lookup_char(name[0]);
        return km ? km->code : 0;
    }
    return 0;
}

static int chord_ctrl(int fd, const char *key)
{
    int code = key_code(key);

    if (code == 0) {
        printf("keyinject: unsupported ctrl chord %s\n", key);
        return -1;
    }
    if (write_key(fd, 29, 1, KBD_MOD_CTRL) < 0)
        return -1;
    if (tap_key(fd, (uint8)code, KBD_MOD_CTRL) < 0)
        return -1;
    if (write_key(fd, 29, 0, 0) < 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    int fd;
    int rc = 0;

    if (argc < 3) {
        printf("usage: keyinject text STRING | keyinject key NAME | keyinject chord ctrl+KEY\n");
        return 1;
    }

    fd = open("/dev/kbd", O_WRONLY);
    if (fd < 0) {
        printf("keyinject: open /dev/kbd failed\n");
        return 1;
    }

    if (strcmp(argv[1], "text") == 0) {
        for (const char *p = argv[2]; *p; p++) {
            if (type_char(fd, *p) < 0) {
                rc = 1;
                break;
            }
        }
        if (rc == 0)
            printf("keyinject: text len=%d\n", (int)strlen(argv[2]));
    } else if (strcmp(argv[1], "key") == 0) {
        int code = key_code(argv[2]);

        if (code == 0 || tap_key(fd, (uint8)code, 0) < 0)
            rc = 1;
        else
            printf("keyinject: key %s\n", argv[2]);
    } else if (strcmp(argv[1], "chord") == 0 &&
               strncmp(argv[2], "ctrl+", 5) == 0) {
        rc = chord_ctrl(fd, argv[2] + 5) < 0 ? 1 : 0;
        if (rc == 0)
            printf("keyinject: chord %s\n", argv[2]);
    } else {
        printf("keyinject: unsupported mode\n");
        rc = 1;
    }

    close(fd);
    return rc;
}

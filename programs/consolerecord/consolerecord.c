#include "user/user.h"
#include "kernel/inc/dev/console.h"
#include "kernel/inc/vfs/fcntl.h"

/*
 * Emit exactly one bounded console record.  Diagnostics intentionally travel
 * only through the exit status: stdout/stderr fallback would corrupt the
 * transport this tool exists to protect.
 */
int main(int argc, char **argv)
{
    char record[CONSOLE_RECORD_MAX_INPUT_BYTES];
    int fd;
    int input_len = 0;

    if (argc != 2 || argv[1] == 0)
        return 2;

    while (argv[1][input_len] != '\0') {
        unsigned char c = (unsigned char)argv[1][input_len];
        if (input_len >= (int)CONSOLE_RECORD_MAX_INPUT_BYTES - 1 ||
            c < 0x20 || c > 0x7e)
            return 2;
        record[input_len++] = (char)c;
    }
    if (input_len == 0)
        return 2;

    record[input_len++] = '\n';
    if (!console_record_wire_text_valid(record, (console_u32)input_len))
        return 2;

    fd = open("/dev/console", O_WRONLY);
    if (fd < 0)
        return 3;

    struct console_record_write_v1 request = {
        .version = CONSOLE_RECORD_ABI_VERSION,
        .flags = 0,
        .data_ptr = (console_u64)(uint64)record,
        .data_len = (console_u32)input_len,
        .reserved = 0,
    };
    int ret = ioctl(fd, CONSOLE_IOC_WRITE_RECORD, &request);
    (void)close(fd);
    return ret == input_len ? 0 : 4;
}

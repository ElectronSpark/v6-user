#include "user/user.h"
#include "kernel/inc/dev/console.h"
#include "kernel/inc/vfs/fcntl.h"

#ifdef HOST_LIBC_PROGRAM
static int read_batch_file(const char *path, char **data_out,
                           console_u32 *data_len_out,
                           console_u32 *record_count_out)
{
    struct stat path_stat;
    struct stat file_stat;
    char *data;
    int fd;
    int offset = 0;
    int record_bytes = 0;
    console_u32 record_count = 0;

    if (path == 0 || path[0] == '\0' || lstat(path, &path_stat) < 0 ||
        !S_ISREG(path_stat.st_mode) || path_stat.st_size <= 0 ||
        (console_u64)path_stat.st_size >
            CONSOLE_RECORD_BATCH_MAX_LOGICAL_BYTES)
        return -1;

    fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &file_stat) < 0 || !S_ISREG(file_stat.st_mode) ||
        file_stat.st_dev != path_stat.st_dev ||
        file_stat.st_ino != path_stat.st_ino ||
        file_stat.st_size != path_stat.st_size) {
        (void)close(fd);
        return -1;
    }

    data = malloc((uint)file_stat.st_size);
    if (data == 0) {
        (void)close(fd);
        return -1;
    }

    while (offset < file_stat.st_size) {
        int n = read(fd, data + offset, (int)file_stat.st_size - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            free(data);
            (void)close(fd);
            return -1;
        }
        offset += n;
    }

    /* Reject a file that grew after the bounded identity/size check. */
    for (;;) {
        char extra;
        int n = read(fd, &extra, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n != 0 || fstat(fd, &file_stat) < 0 ||
            file_stat.st_dev != path_stat.st_dev ||
            file_stat.st_ino != path_stat.st_ino ||
            file_stat.st_size != path_stat.st_size) {
            free(data);
            (void)close(fd);
            return -1;
        }
        break;
    }
    (void)close(fd);

    for (int i = 0; i < offset; i++) {
        unsigned char c = (unsigned char)data[i];

        record_bytes++;
        if (record_bytes > (int)CONSOLE_RECORD_MAX_INPUT_BYTES) {
            free(data);
            return -1;
        }
        if (c == '\n') {
            if (record_bytes == 1 ||
                !console_record_wire_text_valid(
                    data + i + 1 - record_bytes,
                    (console_u32)record_bytes) ||
                record_count == CONSOLE_RECORD_BATCH_MAX_RECORDS) {
                free(data);
                return -1;
            }
            record_count++;
            record_bytes = 0;
        } else if (c < 0x20 || c > 0x7e) {
            free(data);
            return -1;
        }
    }

    if (record_bytes != 0 || record_count == 0 ||
        !console_record_batch_wire_text_valid(
            data, (console_u32)offset, record_count)) {
        free(data);
        return -1;
    }

    *data_out = data;
    *data_len_out = (console_u32)offset;
    *record_count_out = record_count;
    return 0;
}
#endif

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

#ifdef HOST_LIBC_PROGRAM
    if (argc == 3 && argv[1] != 0 && argv[2] != 0 &&
        strcmp(argv[1], "--batch-file") == 0) {
        char *batch_data = 0;
        console_u32 batch_len = 0;
        console_u32 record_count = 0;

        if (read_batch_file(argv[2], &batch_data, &batch_len,
                            &record_count) < 0)
            return 2;

        fd = open("/dev/console", O_WRONLY);
        if (fd < 0) {
            free(batch_data);
            return 3;
        }

        struct console_record_batch_write_v1 request = {
            .version = CONSOLE_RECORD_BATCH_ABI_VERSION,
            .flags = 0,
            .data_ptr = (console_u64)(uint64)batch_data,
            .data_len = batch_len,
            .record_count = record_count,
            .reserved0 = 0,
            .reserved1 = 0,
        };
        int ret = ioctl(fd, CONSOLE_IOC_WRITE_RECORD_BATCH, &request);
        (void)close(fd);
        free(batch_data);
        return ret == (int)batch_len ? 0 : 4;
    }
#endif

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

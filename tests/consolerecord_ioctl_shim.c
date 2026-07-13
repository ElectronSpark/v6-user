#define _GNU_SOURCE 1
#define HOST_LIBC_PROGRAM 1

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "dev/console.h"

static int console_fd = -1;

static int raw_open(const char *path, int flags, mode_t mode)
{
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;

    if ((flags & O_CREAT) != 0) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (strcmp(path, "/dev/console") == 0) {
        console_fd = raw_open("/dev/null", O_WRONLY | O_CLOEXEC, 0);
        return console_fd;
    }
    return raw_open(path, flags, mode);
}

static int expected_payload_matches(const void *data, uint32_t data_len)
{
    const char *path = getenv("CONSOLE_RECORD_EXPECT_FILE");
    unsigned char buf[4096];
    uint32_t offset = 0;
    int fd;

    if (path == NULL)
        return 0;
    fd = raw_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    while (offset < data_len) {
        size_t want = data_len - offset;
        if (want > sizeof(buf))
            want = sizeof(buf);
        ssize_t n = syscall(SYS_read, fd, buf, want);
        if (n <= 0 || memcmp((const unsigned char *)data + offset, buf,
                             (size_t)n) != 0) {
            (void)syscall(SYS_close, fd);
            return 0;
        }
        offset += (uint32_t)n;
    }
    unsigned char extra;
    ssize_t n = syscall(SYS_read, fd, &extra, 1);
    (void)syscall(SYS_close, fd);
    return offset == data_len && n == 0;
}

static int record_call(void)
{
    const char *path = getenv("CONSOLE_RECORD_TRACE_FILE");
    int fd;

    if (path == NULL)
        return 0;
    fd = raw_open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return 0;
    char marker = 'I';
    int ok = syscall(SYS_write, fd, &marker, 1) == 1;
    (void)syscall(SYS_close, fd);
    return ok;
}

int ioctl(int fd, unsigned long command, ...)
{
    va_list ap;
    void *arg;
    const char *mode;
    const void *data;
    uint32_t data_len;

    va_start(ap, command);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (fd != console_fd || arg == NULL || !record_call()) {
        errno = EINVAL;
        return -1;
    }

    mode = getenv("CONSOLE_RECORD_EXPECT_MODE");
    if (mode != NULL && strcmp(mode, "legacy") == 0 &&
        command == CONSOLE_IOC_WRITE_RECORD) {
        const struct console_record_write_v1 *request = arg;
        if (request->version != CONSOLE_RECORD_ABI_VERSION ||
            request->flags != 0 || request->reserved != 0 ||
            request->data_ptr == 0 || request->data_len == 0) {
            errno = EINVAL;
            return -1;
        }
        data = (const void *)(uintptr_t)request->data_ptr;
        data_len = request->data_len;
    } else if (mode != NULL && strcmp(mode, "batch") == 0 &&
               command == CONSOLE_IOC_WRITE_RECORD_BATCH) {
        const struct console_record_batch_write_v1 *request = arg;
        const char *count_text = getenv("CONSOLE_RECORD_EXPECT_COUNT");
        unsigned long expected_count =
            count_text == NULL ? 0 : strtoul(count_text, NULL, 10);
        if (request->version != CONSOLE_RECORD_BATCH_ABI_VERSION ||
            request->flags != 0 || request->reserved0 != 0 ||
            request->reserved1 != 0 || request->data_ptr == 0 ||
            request->data_len == 0 ||
            request->record_count != expected_count) {
            errno = EINVAL;
            return -1;
        }
        data = (const void *)(uintptr_t)request->data_ptr;
        data_len = request->data_len;
    } else {
        errno = ENOTTY;
        return -1;
    }

    if (!expected_payload_matches(data, data_len)) {
        errno = EINVAL;
        return -1;
    }
    if (getenv("CONSOLE_RECORD_IOCTL_FAIL") != NULL) {
        errno = EIO;
        return -1;
    }
    return (int)data_len;
}

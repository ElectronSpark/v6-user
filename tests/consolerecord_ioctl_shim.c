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
#include <sys/stat.h>
#include <unistd.h>

#include "dev/console.h"

static int console_fd = -1;
static int input_fd = -1;
static int source_unlinked;
static int source_read_seen;
static int input_close_failed;

static int raw_open(const char *path, int flags, mode_t mode)
{
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

static int terminal_mode(void)
{
    const char *mode = getenv("CONSOLE_RECORD_EXPECT_MODE");
    return mode != NULL && strcmp(mode, "terminal") == 0;
}

static int record_marker(char marker)
{
    const char *path = getenv("CONSOLE_RECORD_TRACE_FILE");
    int fd;

    if (path == NULL)
        return 1;
    fd = raw_open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return 0;
    int ok = syscall(SYS_write, fd, &marker, 1) == 1;
    (void)syscall(SYS_close, fd);
    return ok;
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
        if (terminal_mode()) {
            (void)record_marker('C');
            if (getenv("CONSOLE_RECORD_CONSOLE_OPEN_FAIL") != NULL) {
                errno = EIO;
                return -1;
            }
        }
        console_fd = raw_open("/dev/null", O_WRONLY | O_CLOEXEC, 0);
        return console_fd;
    }
    int fd = raw_open(path, flags, mode);
    const char *source = getenv("CONSOLE_RECORD_SOURCE_FILE");
    if (fd >= 0 && terminal_mode() && source != NULL &&
        strcmp(path, source) == 0 && (flags & O_ACCMODE) == O_RDONLY) {
        input_fd = fd;
        (void)record_marker('O');
    }
    return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{
    if (terminal_mode() && fd == input_fd &&
        getenv("CONSOLE_RECORD_SHORT_READ") != NULL) {
        return 0;
    }
    ssize_t n = syscall(SYS_read, fd, buf, count);
    if (terminal_mode() && fd == input_fd && n > 0 && !source_read_seen) {
        const char *source = getenv("CONSOLE_RECORD_SOURCE_FILE");
        source_read_seen = 1;
        if (source != NULL &&
            getenv("CONSOLE_RECORD_GROW_AFTER_READ") != NULL) {
            int append_fd = raw_open(source, O_WRONLY | O_APPEND | O_CLOEXEC,
                                     0);
            if (append_fd >= 0) {
                char extra = 'x';
                (void)syscall(SYS_write, append_fd, &extra, 1);
                (void)syscall(SYS_close, append_fd);
            }
        }
        if (source != NULL &&
            getenv("CONSOLE_RECORD_REPLACE_AFTER_READ") != NULL) {
            char moved[4096];
            size_t len = strlen(source);
            if (len + 7 < sizeof(moved)) {
                memcpy(moved, source, len);
                memcpy(moved + len, ".moved", 7);
                (void)syscall(SYS_renameat, AT_FDCWD, source, AT_FDCWD,
                              moved);
                int replacement = raw_open(
                    source, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                if (replacement >= 0)
                    (void)syscall(SYS_close, replacement);
            }
        }
    }
    return n;
}

int unlink(const char *path)
{
    const char *source = getenv("CONSOLE_RECORD_SOURCE_FILE");

    if (!terminal_mode() || source == NULL || strcmp(path, source) != 0)
        return (int)syscall(SYS_unlinkat, AT_FDCWD, path, 0);
    (void)record_marker('U');
    if (getenv("CONSOLE_RECORD_UNLINK_FAIL") != NULL) {
        errno = EACCES;
        return -1;
    }
    int ret = (int)syscall(SYS_unlinkat, AT_FDCWD, path, 0);
    if (ret == 0) {
        source_unlinked = 1;
        if (getenv("CONSOLE_RECORD_RECREATE_AFTER_UNLINK") != NULL) {
            int replacement = raw_open(
                path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (replacement >= 0)
                (void)syscall(SYS_close, replacement);
        }
    }
    return ret;
}

int fstat(int fd, struct stat *st)
{
    int ret = (int)syscall(SYS_fstat, fd, st);
    if (ret == 0 && terminal_mode() && fd == input_fd && source_unlinked &&
        getenv("CONSOLE_RECORD_NLINK_ZERO_FAIL") != NULL)
        st->st_nlink = 1;
    return ret;
}

int close(int fd)
{
    if (terminal_mode() && fd == input_fd) {
        (void)record_marker('X');
        if (!input_close_failed &&
            getenv("CONSOLE_RECORD_INPUT_CLOSE_FAIL") != NULL) {
            input_close_failed = 1;
            errno = EIO;
            return -1;
        }
        input_fd = -1;
        return (int)syscall(SYS_close, fd);
    }
    if (terminal_mode() && fd == console_fd) {
        (void)record_marker('Z');
        console_fd = -1;
        int ret = (int)syscall(SYS_close, fd);
        if (getenv("CONSOLE_RECORD_CONSOLE_CLOSE_FAIL") != NULL) {
            errno = EIO;
            return -1;
        }
        return ret;
    }
    return (int)syscall(SYS_close, fd);
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
    if (fd != console_fd || arg == NULL || !record_marker('I')) {
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
    } else if (mode != NULL && strcmp(mode, "terminal") == 0 &&
               command == CONSOLE_IOC_WRITE_RECORD_BATCH) {
        const struct console_record_batch_write_v1 *request = arg;
        if (request->version != CONSOLE_RECORD_BATCH_ABI_VERSION ||
            request->flags != CONSOLE_RECORD_BATCH_F_TERMINAL_COMMIT ||
            request->reserved0 != 0 || request->reserved1 != 0 ||
            request->data_ptr == 0 || request->data_len == 0 ||
            request->record_count != 2 || !source_unlinked || input_fd >= 0) {
            errno = EINVAL;
            return -1;
        }
        const char *source = getenv("CONSOLE_RECORD_SOURCE_FILE");
        struct stat st;
        if (source == NULL ||
            syscall(SYS_newfstatat, AT_FDCWD, source, &st,
                    AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
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

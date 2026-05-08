#ifndef XV6_HOST_COMPAT_H
#define XV6_HOST_COMPAT_H

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef uint8_t uchar;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int64_t int64;

#ifndef MAXPATH
#define MAXPATH 512
#endif

#ifndef NSIG
#define NSIG 65
#endif

static inline int host_compat_errno_ret(long ret) {
    return ret < 0 ? -errno : (int)ret;
}

static inline int host_compat_open2(const char *path, int flags) {
    int fd;

    if (flags & O_CREAT)
        fd = open(path, flags, 0666);
    else
        fd = open(path, flags);
    return host_compat_errno_ret(fd);
}

static inline int host_compat_mkdir1(const char *path) {
    return host_compat_errno_ret(mkdir(path, 0777));
}

static inline int host_compat_stat(const char *path, struct stat *st) {
    return host_compat_errno_ret(stat(path, st));
}

static inline int host_compat_lstat(const char *path, struct stat *st) {
    return host_compat_errno_ret(lstat(path, st));
}

static inline int host_compat_fstat(int fd, struct stat *st) {
    return host_compat_errno_ret(fstat(fd, st));
}

static inline int host_compat_unlink(const char *path) {
    return host_compat_errno_ret(unlink(path));
}

static inline int host_compat_link(const char *oldpath, const char *newpath) {
    return host_compat_errno_ret(link(oldpath, newpath));
}

static inline int host_compat_symlink(const char *target, const char *linkpath) {
    return host_compat_errno_ret(symlink(target, linkpath));
}

static inline int host_compat_rename(const char *oldpath, const char *newpath) {
    return host_compat_errno_ret(rename(oldpath, newpath));
}

static inline int host_compat_fchmodat(int dirfd, const char *path, int mode,
                                       int flags) {
    return host_compat_errno_ret(fchmodat(dirfd, path, (mode_t)mode, flags));
}

static inline int host_compat_readlink(const char *path, char *buf, int bufsiz) {
    return host_compat_errno_ret(readlink(path, buf, (size_t)bufsiz));
}

static inline int host_compat_getdents(int fd, void *dirp, int count) {
    return host_compat_errno_ret(
        syscall(SYS_getdents64, fd, dirp, (size_t)count));
}

static inline uint strlen_local(const char *s) {
    return (uint)strlen(s);
}

static inline int atoi_local(const char *s) {
    return atoi(s);
}

static inline int dumpproc(int mode, int id) {
    DIR *dir;
    struct dirent *de;

    (void)mode;
    dir = opendir("/proc");
    if (dir == 0)
        return -1;

    printf("PID CMD\n");
    while ((de = readdir(dir)) != 0) {
        char path[MAXPATH];
        char cmd[128];
        FILE *fp;
        int pid;
        size_t n;
        int numeric = 1;

        for (const char *p = de->d_name; *p; p++) {
            if (!isdigit((unsigned char)*p)) {
                numeric = 0;
                break;
            }
        }
        if (!numeric)
            continue;

        pid = atoi(de->d_name);
        if (id >= 0 && pid != id)
            continue;

        snprintf(path, sizeof(path), "/proc/%s/cmdline", de->d_name);
        fp = fopen(path, "rb");
        if (fp == 0)
            continue;
        n = fread(cmd, 1, sizeof(cmd) - 1, fp);
        fclose(fp);
        if (n == 0)
            snprintf(cmd, sizeof(cmd), "?");
        else {
            for (size_t i = 0; i < n; i++) {
                if (cmd[i] == '\0')
                    cmd[i] = ' ';
            }
            cmd[n] = '\0';
        }
        printf("%d %s\n", pid, cmd);
    }
    closedir(dir);
    return 0;
}

#define open(path, flags) host_compat_open2((path), (flags))
#define mkdir(path) host_compat_mkdir1((path))
#define stat(path, st) host_compat_stat((path), (st))
#define lstat(path, st) host_compat_lstat((path), (st))
#define fstat(fd, st) host_compat_fstat((fd), (st))
#define unlink(path) host_compat_unlink((path))
#define link(oldpath, newpath) host_compat_link((oldpath), (newpath))
#define symlink(target, linkpath) host_compat_symlink((target), (linkpath))
#define rename(oldpath, newpath) host_compat_rename((oldpath), (newpath))
#define fchmodat(dirfd, path, mode, flags) \
    host_compat_fchmodat((dirfd), (path), (mode), (flags))
#define readlink(path, buf, bufsiz) host_compat_readlink((path), (buf), (bufsiz))
#define getdents(fd, dirp, count) host_compat_getdents((fd), (dirp), (count))
#define fprintf(fd, ...) dprintf((fd), __VA_ARGS__)
#define strlen strlen_local
#define atoi atoi_local

#endif

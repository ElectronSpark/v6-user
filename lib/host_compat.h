#ifndef XV6_HOST_COMPAT_H
#define XV6_HOST_COMPAT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
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

struct clone_args;
struct kstats;
struct kprofile_pgroup;
struct konsole_prepty_wake_snapshot;
struct kprofile_userpc_config;
struct kprofile_userpc_snapshot;
struct kprofile_vfs_enoent_snapshot;
struct kevent;
struct netconf_req;

#ifndef XV6_SYS_kstats
#define XV6_SYS_kstats 1360
#endif

#ifndef XV6_SYS_kstatsctl
#define XV6_SYS_kstatsctl 1367
#endif

#ifndef XV6_SYS_kstats2
#define XV6_SYS_kstats2 1368
#endif

#ifndef XV6_SYS_kprofile_pgroup
#define XV6_SYS_kprofile_pgroup 1370
#endif

#ifndef XV6_SYS_kprofile_prepty_ring
#define XV6_SYS_kprofile_prepty_ring 1371
#endif

#ifndef XV6_SYS_kprofile_userpc_ctl
#define XV6_SYS_kprofile_userpc_ctl 1372
#endif

#ifndef XV6_SYS_kprofile_userpc_snapshot
#define XV6_SYS_kprofile_userpc_snapshot 1373
#endif

#ifndef XV6_SYS_kprofile_vfs_enoent_snapshot
#define XV6_SYS_kprofile_vfs_enoent_snapshot 1374
#endif

#ifndef XV6_SYS_netconf
#define XV6_SYS_netconf 1361
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

static inline int host_compat_exec(const char *path, char **argv) {
    extern char **environ;
    return host_compat_errno_ret(execve(path, argv, environ));
}

static inline int host_compat_mknod4(const char *path, int mode, int major,
                                     int minor) {
    return host_compat_errno_ret(
        mknod(path, (mode_t)mode, makedev((unsigned)major, (unsigned)minor)));
}

static inline int host_compat_mount5(const char *source, const char *target,
                                     const char *fstype, unsigned long flags,
                                     const void *data) {
    return host_compat_errno_ret(
        syscall(SYS_mount, source, target, fstype, flags, data));
}

static inline int host_compat_umount1(const char *target) {
#ifdef SYS_umount2
    return host_compat_errno_ret(syscall(SYS_umount2, target, 0));
#else
    return -ENOSYS;
#endif
}

static inline int host_compat_sleep_ms(int millis) {
    if (millis < 0)
        millis = 0;
    struct timespec ts = {
        .tv_sec = millis / 1000,
        .tv_nsec = (long)(millis % 1000) * 1000000L,
    };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
    return 0;
}

static inline int host_compat_uptime(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return -errno;
    return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static inline int host_compat_getrandom(void *buf, int len) {
#ifdef SYS_getrandom
    return host_compat_errno_ret(syscall(SYS_getrandom, buf, (size_t)len, 0));
#else
    FILE *fp = fopen("/dev/urandom", "rb");
    if (fp == 0)
        return -errno;
    size_t n = fread(buf, 1, (size_t)len, fp);
    int saved = errno;
    fclose(fp);
    errno = saved;
    return n == (size_t)len ? (int)n : -errno;
#endif
}

static inline uint64 memstat(uint64 flags) {
    (void)flags;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp == 0)
        return (uint64)-errno;
    char line[160];
    while (fgets(line, sizeof(line), fp) != 0)
        dprintf(1, "%s", line);
    fclose(fp);
    return 0;
}

static inline int dumpchan(void) {
    return 0;
}

static inline int dumppcache(void) {
    return 0;
}

static inline int dumprq(void) {
    return 0;
}

static inline int dumpinode(const char *path) {
    struct stat st;
    if (stat(path ? path : ".", &st) < 0)
        return -errno;
    dprintf(1, "%s: dev=%lu ino=%lu mode=%o nlink=%lu size=%ld\n",
            path ? path : ".", (unsigned long)st.st_dev,
            (unsigned long)st.st_ino, (unsigned)st.st_mode,
            (unsigned long)st.st_nlink, (long)st.st_size);
    return 0;
}

static inline int dumpblk(int mode) {
    (void)mode;
    FILE *fp = fopen("/proc/partitions", "r");
    if (fp == 0)
        return -errno;
    char line[160];
    while (fgets(line, sizeof(line), fp) != 0)
        dprintf(1, "%s", line);
    fclose(fp);
    return 0;
}

static inline int losetup(int cmd, int loop_num, const char *path) {
    (void)cmd;
    (void)loop_num;
    (void)path;
    return -ENOSYS;
}

static inline int kstats(struct kstats *ks) {
    return host_compat_errno_ret(syscall(XV6_SYS_kstats, ks));
}

static inline int kstats2(struct kstats *ks, size_t size) {
    return host_compat_errno_ret(syscall(XV6_SYS_kstats2, ks, size));
}

static inline int kstatsctl(int enabled) {
    long ret = syscall(XV6_SYS_kstatsctl, enabled);
    if (ret < 0 && errno == ENOSYS)
        return 0;
    return host_compat_errno_ret(ret);
}

static inline int kprofile_pgroup(int pgid, struct kprofile_pgroup *kp,
                                  size_t size) {
    long ret = syscall(XV6_SYS_kprofile_pgroup, pgid, kp, size);
    return host_compat_errno_ret(ret);
}

static inline int kprofile_prepty_ring(
    struct konsole_prepty_wake_snapshot *snap, size_t size) {
    long ret = syscall(XV6_SYS_kprofile_prepty_ring, snap, size);
    return host_compat_errno_ret(ret);
}

static inline int kprofile_userpc_ctl(struct kprofile_userpc_config *cfg,
                                      size_t size) {
    long ret = syscall(XV6_SYS_kprofile_userpc_ctl, cfg, size);
    return host_compat_errno_ret(ret);
}

static inline int kprofile_userpc_snapshot(
    struct kprofile_userpc_snapshot *snap, size_t size) {
    long ret = syscall(XV6_SYS_kprofile_userpc_snapshot, snap, size);
    return host_compat_errno_ret(ret);
}

static inline int kprofile_vfs_enoent_snapshot(
    struct kprofile_vfs_enoent_snapshot *snap, size_t size) {
    long ret = syscall(XV6_SYS_kprofile_vfs_enoent_snapshot, snap, size);
    return host_compat_errno_ret(ret);
}

static inline int netconf(const struct netconf_req *req) {
    return host_compat_errno_ret(syscall(XV6_SYS_netconf, req));
}

static inline int kqueue(void) {
#ifdef SYS_eventfd2
    return host_compat_errno_ret(syscall(SYS_eventfd2, 0, 0));
#else
    return -ENOSYS;
#endif
}

static inline int kevent_register(int kqfd, struct kevent *changelist,
                                  int nchanges) {
    (void)kqfd;
    (void)changelist;
    (void)nchanges;
    return 0;
}

static inline int kevent_wait(int kqfd, struct kevent *eventlist, int nevents,
                              int timeout_ms) {
    (void)kqfd;
    (void)eventlist;
    (void)nevents;
    host_compat_sleep_ms(timeout_ms);
    return 0;
}

static inline int poweroff(void) {
    return host_compat_errno_ret(
        syscall(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0));
}

static inline int reboot(void) {
    return host_compat_errno_ret(
        syscall(SYS_reboot, 0xfee1dead, 672274793, 0x1234567, 0));
}

static inline void waitgdb(void) {
    raise(SIGTRAP);
}

static inline void waitgdb_stopentry(void) {
    raise(SIGTRAP);
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
#define exec(path, argv) host_compat_exec((path), (argv))
#define mknod(path, mode, major, minor) \
    host_compat_mknod4((path), (mode), (major), (minor))
#define mount(source, target, fstype, flags, data) \
    host_compat_mount5((source), (target), (fstype), (flags), (data))
#define umount(target) host_compat_umount1((target))
#define sleep(millis) host_compat_sleep_ms((millis))
#define uptime() host_compat_uptime()
#define getrandom(buf, len) host_compat_getrandom((buf), (len))
#define fprintf(fd, ...) dprintf((fd), __VA_ARGS__)
#define strlen strlen_local
#define atoi atoi_local

#endif

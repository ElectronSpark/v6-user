#ifdef HOST_LIBC_PROGRAM
#include "host_compat.h"
#else
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#include "kernel/inc/vfs/xv6fs/ondisk.h"
#include "kernel/inc/vfs/fcntl.h"
#endif

int find(char *path, char *name) {
#ifdef HOST_LIBC_PROGRAM
    DIR *dir;
    struct dirent *de;
    struct stat st;
    int status = 0;

    if (lstat(path, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(2, "find: %s is not a directory\n", path);
        return -1;
    }

    dir = opendir(path);
    if (dir == 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return -1;
    }

    while ((de = readdir(dir)) != 0) {
        char child[MAXPATH];

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >=
            (int)sizeof(child)) {
            fprintf(2, "find: path too long: %s/%s\n", path, de->d_name);
            status = -1;
            continue;
        }
        if (lstat(child, &st) < 0) {
            fprintf(2, "find: cannot stat %s\n", child);
            status = -1;
            continue;
        }
        if (strcmp(de->d_name, name) == 0)
            printf("%s\n", child);
        if (S_ISDIR(st.st_mode) && find(child, name) < 0)
            status = -1;
    }

    closedir(dir);
    return status;
#else
    char buf[512], *p;
    int fd;
    int path_length;
    struct dirent de;
    struct stat st;

    if ((fd = open(path, O_RDONLY)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return -1;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(2, "find: %s is not a directory\n", path);
        close(fd);
        return -1;
    }

    path_length = strlen(path);
    memcpy(buf, path, path_length);
    p = buf + path_length;
    *p++ = '/';
    *p = '\0';

    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0)
            continue;
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;
        if (stat(buf, &st) < 0) {
            printf("find: cannot stat %s\n", buf);
            continue;
        }
        if (strcmp(p, name) == 0) {
            printf("%s\n", buf);
        }
        if (S_ISDIR(st.st_mode) && strcmp(p, ".") && strcmp(p, "..")) {
            find(buf, name);
        }
    }

    return 0;
#endif
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: find [path] [name]\n");
        exit(1);
    }

    int ret = find(argv[1], argv[2]);

    exit(ret);
}

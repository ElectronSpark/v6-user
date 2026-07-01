// init: The initial user-level program

#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/lock/spinlock.h"
#include "kernel/inc/lock/mutex_types.h"
#include "user/user.h"
#include "kernel/inc/vfs/fcntl.h"
#include "kernel/inc/dev/netconf.h"

#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif

#ifndef CONSOLE_MAJOR
#define CONSOLE_MAJOR 1
#endif
#ifndef CONSOLE_MINOR
#define CONSOLE_MINOR 1
#endif
#ifndef NULL_MAJOR
#define NULL_MAJOR 1
#endif
#ifndef NULL_MINOR
#define NULL_MINOR 2
#endif
#ifndef RANDOM_MAJOR
#define RANDOM_MAJOR 1
#endif
#ifndef RANDOM_MINOR
#define RANDOM_MINOR 3
#endif
#ifndef TTY_DEV_MAJOR
#define TTY_DEV_MAJOR 5
#endif
#ifndef TTY_DEV_MINOR
#define TTY_DEV_MINOR 1
#endif

// Second virtio disk: major=2, minor=2
#ifndef DISK1_MAJOR
#define DISK1_MAJOR 2
#endif
#ifndef DISK1_MINOR
#define DISK1_MINOR 2
#endif

char *bash_argv[] = {"bash", "-i", 0};
char *sh_argv[] = {"sh", 0};
char *console_env[] = {
    "HOME=/root",
    "USER=root",
    "LOGNAME=root",
    "SHELL=/bin/bash",
    "PATH=/usr/local/bin:/usr/bin:/bin:/",
    "TERM=xterm-256color",
    "LANG=C",
    "LC_ALL=C",
    "PYTHONUTF8=1",
    "PYTHONIOENCODING=utf-8",
    "PYTHONHOME=/",
    "PYTHONPATH=/lib/python3.12:/lib/python3.12/site-packages",
    "PYTHONDONTWRITEBYTECODE=1",
    0,
};

/* --------------------------------------------------------------------------
 * Network configuration from /etc/network.conf
 * -------------------------------------------------------------------------- */

#define NET_CONF_PATH "/etc/network.conf"

/* Local strncmp - xv6 userlib only provides strcmp */
static int my_strncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

/* Parse a dotted-quad IP string into a uint32 in network byte order.
 * Returns 0 on success, -1 on failure. */
static int parse_ip4(const char *s, uint32 *out)
{
    int parts[4] = {0, 0, 0, 0};
    int pi = 0, val = 0, digits = 0;

    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            digits++;
        } else if (*p == '.' || *p == '\0' || *p == '\n' ||
                   *p == '\r' || *p == ' ' || *p == '\t' || *p == '#') {
            if (digits == 0 || val > 255 || pi > 3)
                return -1;
            parts[pi++] = val;
            val = 0;
            digits = 0;
            if (*p != '.')
                break;
        } else {
            return -1;
        }
    }
    if (pi != 4)
        return -1;
    /* Network byte order (big-endian) */
    *out = (uint32)parts[0] | ((uint32)parts[1] << 8) |
           ((uint32)parts[2] << 16) | ((uint32)parts[3] << 24);
    return 0;
}

/* Skip leading whitespace */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* Copy value string into dst, stripping trailing whitespace/comments */
static void copy_value(char *dst, int maxlen, const char *src)
{
    int i = 0;
    while (i < maxlen - 1 && src[i] != '\0' && src[i] != '\n' &&
           src[i] != '\r' && src[i] != '#')
        i++;
    while (i > 0 && (src[i-1] == ' ' || src[i-1] == '\t'))
        i--;
    memmove(dst, src, i);
    dst[i] = '\0';
}

/* Read /etc/network.conf, parse it, and write to /dev/netconf device */
static void configure_network(void)
{
    struct netconf_req req;
    memset(&req, 0, sizeof(req));
    req.mode = NETCONF_MODE_DHCP;  /* default */
    strcpy(req.hostname, "xv6");

    int fd = open(NET_CONF_PATH, O_RDONLY);
    if (fd >= 0) {
        char buf[1024];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (n > 0) {
            buf[n] = '\0';

            /* Parse line by line */
            char *line = buf;
            while (*line) {
                char *eol = line;
                while (*eol && *eol != '\n')
                    eol++;

                char saved = *eol;
                *eol = '\0';

                const char *p = skip_ws(line);

                /* Skip empty lines and comments */
                if (*p != '\0' && *p != '#') {
                    if (my_strncmp(p, "mode=", 5) == 0) {
                        const char *val = skip_ws(p + 5);
                        if (my_strncmp(val, "static", 6) == 0)
                            req.mode = NETCONF_MODE_STATIC;
                        else
                            req.mode = NETCONF_MODE_DHCP;
                    } else if (my_strncmp(p, "ip=", 3) == 0) {
                        parse_ip4(skip_ws(p + 3), &req.ip);
                    } else if (my_strncmp(p, "netmask=", 8) == 0) {
                        parse_ip4(skip_ws(p + 8), &req.netmask);
                    } else if (my_strncmp(p, "gateway=", 8) == 0) {
                        parse_ip4(skip_ws(p + 8), &req.gateway);
                    } else if (my_strncmp(p, "dns=", 4) == 0) {
                        parse_ip4(skip_ws(p + 4), &req.dns);
                    } else if (my_strncmp(p, "hostname=", 9) == 0) {
                        copy_value(req.hostname, NETCONF_HOSTNAME_MAX,
                                   skip_ws(p + 9));
                    }
                }

                *eol = saved;
                if (saved == '\0')
                    break;
                line = eol + 1;
            }
        }
    }

    /* Apply configuration via /dev/netconf device (created by devtmpfs) */
    int nfd = open("/dev/netconf", O_WRONLY);
    if (nfd < 0) {
        printf("init: /dev/netconf unavailable\n");
        return;
    }

    int ret = write(nfd, &req, sizeof(req));
    close(nfd);

    if (ret < 0)
        printf("init: /dev/netconf write failed\n");
    else
        printf("init: network configured (%s)\n",
               req.mode == NETCONF_MODE_DHCP ? "dhcp" : "static");
}

static int is_env_assignment(const char *s)
{
    const char *p = s;

    if (*p == '\0' || *p == '=')
        return 0;
    while (*p && *p != '=')
        p++;
    return *p == '=';
}

static void redirect_startup_stdin_to_null(void)
{
    int nfd = open("/dev/null", O_RDONLY);

    if (nfd < 0)
        return;
    if (nfd != 0) {
        dup2(nfd, 0);
        close(nfd);
    }
}

int main(void) {
    int pid, wpid;

    // Open console as stdin (fd 0), stdout (fd 1), stderr (fd 2)
    if (open("/dev/console", O_RDWR) < 0) {
        mknod("/dev/console", S_IFCHR | 0666, CONSOLE_MAJOR, CONSOLE_MINOR);
        open("/dev/console", O_RDWR);
    }
    dup(0); // stdout (fd 1)
    dup(0); // stderr (fd 2)

    // Set the console as the controlling terminal for this session.
    // The kernel already created session 1 for init during boot
    // (session_init_first), but the console tty is not yet attached.
    // This enables job control (tcgetpgrp/tcsetpgrp) for shells like dash.
    if (ioctl(0, TIOCSCTTY, (void *)0) < 0)
        printf("init: warning: TIOCSCTTY failed\n");

    // Ensure device nodes exist (devtmpfs creates them automatically
    // when drivers register with devname/devmode; these mknod calls are
    // kept only as a safety net - errors are silently ignored)
    mknod("/dev/null", S_IFCHR | 0666, NULL_MAJOR, NULL_MINOR);
    mknod("/dev/random", S_IFCHR | 0666, RANDOM_MAJOR, RANDOM_MINOR);
    mknod("/dev/tty", S_IFCHR | 0666, TTY_DEV_MAJOR, TTY_DEV_MINOR);
    mkdir("/tmp");
    if (mount("none", "/tmp", "tmpfs", 0, 0) < 0)
        printf("init: warning: mount /tmp failed\n");
    mkdir("/proc");
    if (mount("none", "/proc", "procfs", 0, 0) < 0)
        printf("init: warning: mount /proc failed\n");
    mkdir("/sys");
    if (mount("none", "/sys", "sysfs", 0, 0) < 0)
        printf("init: warning: mount /sys failed\n");

    // The ext4 rootfs already contains /usr with Python stdlib.
    // No separate disk mount needed.

    // Configure network through /dev/netconf.
    configure_network();

    // Launch background services listed in /etc/startup. Each non-empty,
    // non-comment line is forked as a separate process. Tokens are split
    // on spaces/tabs (no quoting). Leading NAME=value tokens become the
    // exec environment. Services run in their own sessions and keep
    // stdout/stderr on the console for logs, but stdin comes from /dev/null so
    // daemons cannot consume shell input or take foreground terminal control.
    {
        int sfd = open("/etc/startup", O_RDONLY);
        if (sfd >= 0) {
            char buf[1024];
            int n = read(sfd, buf, sizeof(buf) - 1);
            close(sfd);
            if (n > 0) {
                buf[n] = '\0';
                char *line = buf;
                while (*line) {
                    char *eol = line;
                    while (*eol && *eol != '\n') eol++;
                    char saved = *eol;
                    *eol = '\0';

                    // skip leading whitespace
                    char *p = line;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p && *p != '#') {
                        static char *tokens[32];
                        static char *senv[16];
                        static char *sargv[16];
                        int ntokens = 0;
                        char *q = p;
                        while (*q && ntokens < 31) {
                            tokens[ntokens++] = q;
                            while (*q && *q != ' ' && *q != '\t') q++;
                            if (!*q) break;
                            *q++ = '\0';
                            while (*q == ' ' || *q == '\t') q++;
                        }
                        tokens[ntokens] = 0;

                        int envc = 0;
                        int cmdi = 0;
                        while (cmdi < ntokens && envc < 15 &&
                               is_env_assignment(tokens[cmdi])) {
                            senv[envc++] = tokens[cmdi++];
                        }
                        senv[envc] = 0;
                        if (cmdi >= ntokens)
                            goto next_startup_line;

                        int sargc = 0;
                        while (cmdi < ntokens && sargc < 15)
                            sargv[sargc++] = tokens[cmdi++];
                        sargv[sargc] = 0;

                        int dpid = fork();
                        if (dpid == 0) {
                            if (setsid() < 0)
                                (void)setpgid(0, 0);
                            redirect_startup_stdin_to_null();
                            if (envc > 0)
                                execve(sargv[0], sargv, senv);
                            else
                                exec(sargv[0], sargv);
                            printf("init: exec %s failed\n", sargv[0]);
                            exit(1);
                        } else if (dpid > 0) {
                            printf("init: started %s (pid %d)\n",
                                   sargv[0], dpid);
                        }
                    }

next_startup_line:
                    *eol = saved;
                    line = (saved == '\0') ? eol : eol + 1;
                }
            }
        }
    }

    // Userspace telnet daemon disabled - using kernel telnetd instead.
    // sleep(10);
    // pid = fork();
    // if (pid == 0) {
    //     char *telnetd_argv[] = {"utelnetd", 0};
    //     exec("/bin/utelnetd", telnetd_argv);
    //     printf("init: exec utelnetd failed\n");
    //     exit(1);
    // } else if (pid < 0) {
    //     printf("init: fork for utelnetd failed\n");
    // } else {
    //     printf("init: utelnetd started (pid %d)\n", pid);
    // }

    for (;;) {
        printf("init: starting bash\n");
        pid = fork(); // Use fork instead of vfork to debug OrangePi hang
        if (pid < 0) {
            printf("init: fork failed\n");
            exit(1);
        }
        if (pid == 0) {
            execve("/bin/bash", bash_argv, console_env);
            printf("init: exec bash failed, falling back to sh\n");
            execve("/bin/sh", sh_argv, console_env);
            printf("init: exec sh failed\n");
            exit(1);
        }

        for (;;) {
            // this call to wait() returns if the shell exits,
            // or if a parentless process exits.
            wpid = wait((int *)0);
            if (wpid == pid) {
                // the shell exited; restart it.
                break;
            } else if (wpid < 0) {
                printf("init: wait returned an error\n");
                exit(1);
            } else {
                // it was a parentless process; do nothing.
            }
        }
    }
}

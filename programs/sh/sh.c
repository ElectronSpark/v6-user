// Enhanced Shell with line editing, history, tab completion, and job control
//
// Features:
// - Command history (up/down arrows, ring buffer)
// - Tab completion for commands (PATH) and files
// - Cursor movement (left/right) and mid-line editing
// - Ctrl+C/D/U/K/A/E/L key bindings
// - Raw terminal mode for character-by-character input
// - Environment variables ($VAR, ${VAR}, export/unset/env)
// - PATH-based command lookup
// - Foreground process group management (TIOCSPGRP)
// - Enhanced ls with permissions, types, sizes

#ifdef USE_NCURSES_SHELL
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <curses.h>
#include <pwd.h>
static int getdents(int fd, void *dirp, int count) {
    return (int)syscall(SYS_getdents64, fd, dirp, (size_t)count);
}
static int shell_open(const char *path, int flags) {
    if (flags & O_CREAT)
        return open(path, flags, 0666);
    return open(path, flags);
}
static inline void waitgdb(void) {
#if defined(__riscv)
    asm volatile("li a0, 0\n\tebreak" ::: "a0", "memory");
#endif
}
static inline void waitgdb_stopentry(void) {
#if defined(__riscv)
    asm volatile("li a0, 1\n\tebreak" ::: "a0", "memory");
#endif
}
#else
#include "user.h"
#include "kernel/inc/vfs/fcntl.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/tty/termios.h"
#define shell_open(path, flags) open((path), (flags))
// Stubs — xv6 userlib has no real environ; the shell's internal
// env_vars[] table is the authoritative store.
static int setenv(const char *name, const char *value, int overwrite) {
    (void)name; (void)value; (void)overwrite;
    return 0;
}
static int unsetenv(const char *name) {
    (void)name;
    return 0;
}
#endif

// Linux-compatible dirent structure for getdents
struct linux_dirent64 {
#ifdef USE_NCURSES_SHELL
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
#else
    uint64 d_ino;
    int64 d_off;
    uint16 d_reclen;
    uint8 d_type;
#endif
    char d_name[];
};

#define NAME_MAX 255

// =====================================================================
// Parsed command representation (same as original xv6 shell)
// =====================================================================
#define EXEC 1
#define REDIR 2
#define PIPE 3
#define LIST 4
#define BACK 5

#define MAXARGS 32

struct cmd {
    int type;
};

struct execcmd {
    int type;
    char *argv[MAXARGS];
    char *eargv[MAXARGS];
};

struct redircmd {
    int type;
    struct cmd *cmd;
    char *file;
    char *efile;
    int mode;
    int fd;
};

struct pipecmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

struct listcmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

struct backcmd {
    int type;
    struct cmd *cmd;
};

// Forward declarations
void panic(char *);
struct cmd *parsecmd(char *);
void runcmd(struct cmd *) __attribute__((noreturn));

// =====================================================================
// Utility helpers
// =====================================================================

static int strncmp_local(const char *a, const char *b, int n) {
    while (n > 0) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (*a == 0)
            return 0;
        a++;
        b++;
        n--;
    }
    return 0;
}

static int contains_local(const char *haystack, const char *needle) {
    int needle_len = strlen(needle);
    if (needle_len == 0)
        return 1;
    for (const char *p = haystack; *p; p++) {
        if (strncmp_local(p, needle, needle_len) == 0)
            return 1;
    }
    return 0;
}

static char *strcat_local(char *dest, const char *src) {
    char *d = dest;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

// Simple itoa into caller buffer, returns length written.
static int itoa_local(int val, char *buf, int bufsz) {
    if (bufsz < 2)
        return 0;
    if (val < 0) {
        buf[0] = '-';
        int n = itoa_local(-val, buf + 1, bufsz - 1);
        return n + 1;
    }
    if (val == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return 1;
    }
    char tmp[16];
    int i = 0;
    while (val && i < 15) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int len = i;
    if (len >= bufsz)
        len = bufsz - 1;
    for (int j = 0; j < len; j++)
        buf[j] = tmp[len - 1 - j];
    buf[len] = 0;
    return len;
}

// =====================================================================
// Line editing state
// =====================================================================

#define LINE_BUF_SIZE 1024
#define HISTORY_SIZE 32

static char line_buf[LINE_BUF_SIZE];
static int line_len;
static int cursor_pos;

// History ring buffer
static char history[HISTORY_SIZE][LINE_BUF_SIZE];
static int history_count; // total entries stored
static int history_idx;   // browsing index
static int history_start; // oldest entry position in ring

// Terminal state
static struct termios orig_termios;
static int orig_termios_saved;
static int raw_mode;
#ifdef USE_NCURSES_SHELL
static int ncurses_active;
#endif

// Current working directory (for prompt)
static char cwd_path[512] = "/";

// Current user info (for prompt)
static char user_name[64] = "?";
static int  user_uid = -1;
static int  gui_session_shell = 0;

#ifdef USE_NCURSES_SHELL
#define errprintf(...) fprintf(stderr, __VA_ARGS__)
#define ST_MODE(x) ((x).st_mode)
#define ST_MODE_P(x) ((x)->st_mode)
#define ST_NLINK_P(x) ((x)->st_nlink)
#define ST_SIZE_P(x) ((x)->st_size)
#else
#define errprintf(...) fprintf(2, __VA_ARGS__)
#define ST_MODE(x) ((x).st_mode)
#define ST_MODE_P(x) ((x)->st_mode)
#define ST_NLINK_P(x) ((x)->st_nlink)
#define ST_SIZE_P(x) ((x)->st_size)
#endif

// =====================================================================
// Environment variables
// =====================================================================

#define MAX_ENV_VARS 64
#define MAX_ENV_NAME 64
#define MAX_ENV_VALUE 256

struct env_var {
    char name[MAX_ENV_NAME];
    char value[MAX_ENV_VALUE];
    int used;
};

static struct env_var env_vars[MAX_ENV_VARS];

static int env_set(const char *name, const char *value);

static int env_name_start(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int env_name_char(int c) {
    return env_name_start(c) || (c >= '0' && c <= '9');
}

static int env_assignment_name_len(const char *word) {
    int i;

    if (word == 0 || !env_name_start((unsigned char)word[0]))
        return 0;
    for (i = 1; word[i] && word[i] != '='; i++) {
        if (!env_name_char((unsigned char)word[i]))
            return 0;
    }
    if (word[i] != '=')
        return 0;
    return i;
}

static int is_env_assignment_word(const char *word) {
    return env_assignment_name_len(word) > 0;
}

static int assignment_matches_name(const char *assignment, const char *name) {
    int alen = env_assignment_name_len(assignment);
    int nlen;

    if (alen == 0)
        return 0;
    nlen = strlen(name);
    return alen == nlen && strncmp_local(assignment, name, alen) == 0;
}

static int env_set_assignment(const char *assignment) {
    char name[MAX_ENV_NAME];
    int nlen = env_assignment_name_len(assignment);

    if (nlen == 0)
        return -1;
    if (nlen >= MAX_ENV_NAME)
        nlen = MAX_ENV_NAME - 1;
    memcpy(name, assignment, nlen);
    name[nlen] = 0;
    return env_set(name, assignment + env_assignment_name_len(assignment) + 1);
}

static void env_init(void) {
    for (int i = 0; i < MAX_ENV_VARS; i++)
        env_vars[i].used = 0;
    env_set("PATH", "/usr/local/bin:/usr/bin:/bin:/");
    env_set("HOME", "/root");
    env_set("TERM", "xterm");
    env_set("SHELL", "/bin/bash");
    env_set("LANG", "C");
    env_set("LC_ALL", "C");
    env_set("PYTHONUTF8", "1");
    env_set("PYTHONIOENCODING", "utf-8");
    env_set("PYTHONHOME", "/");
    env_set("PYTHONPATH", "/lib/python3.12:/lib/python3.12/site-packages");
    env_set("PYTHONDONTWRITEBYTECODE", "1");
}

static char *env_get(const char *name) {
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].name, name) == 0)
            return env_vars[i].value;
    }
    return 0;
}

static int env_set(const char *name, const char *value) {
    // Keep standard environ in sync so exec'd children inherit env
    setenv(name, value, 1);
    // Update existing
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].name, name) == 0) {
            int vlen = strlen(value);
            if (vlen >= MAX_ENV_VALUE)
                vlen = MAX_ENV_VALUE - 1;
            memcpy(env_vars[i].value, value, vlen);
            env_vars[i].value[vlen] = 0;
            return 0;
        }
    }
    // Find empty slot
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (!env_vars[i].used) {
            int nlen = strlen(name);
            int vlen = strlen(value);
            if (nlen >= MAX_ENV_NAME)
                nlen = MAX_ENV_NAME - 1;
            if (vlen >= MAX_ENV_VALUE)
                vlen = MAX_ENV_VALUE - 1;
            memcpy(env_vars[i].name, name, nlen);
            env_vars[i].name[nlen] = 0;
            memcpy(env_vars[i].value, value, vlen);
            env_vars[i].value[vlen] = 0;
            env_vars[i].used = 1;
            return 0;
        }
    }
    return -1;
}

static int env_unset(const char *name) {
    unsetenv(name);
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].name, name) == 0) {
            env_vars[i].used = 0;
            return 0;
        }
    }
    return -1;
}

/*
 * Detect the available GPU transport and export the Mesa driver-selection
 * environment so that *every* program launched from this shell renders on the
 * real GPU without any per-launch configuration.  This is deliberately limited
 * to the rendering-driver + loader-path variables (no Wayland/EGL platform
 * vars), so it is safe to apply to plain terminal/serial shells as well as the
 * GUI session.  env_set() mirrors into the standard environ, so exec'd children
 * inherit these automatically.
 *
 *   - Hyper-V/WSL /dev/dxg present -> GALLIUM_DRIVER=d3d12 pinned to the
 *       discrete NVIDIA adapter (libdxcore + libd3d12 over /dev/dxg, the same
 *       path d3d12probe proved).  The WSL GPU-PV user-mode runtime lives in
 *       /usr/lib/wsl/lib, so it is added to the loader search path.  This is
 *       checked FIRST: on a GPU-P guest the /dev/dri/renderD128 node also
 *       exists but is only a software dumb-buffer node (no virgl), so it must
 *       NOT shadow the real d3d12 GPU path.
 *   - virtio render node present  -> GALLIUM_DRIVER=virgl (host GL via virgl)
 *   - neither                     -> software rendering.
 *
 * Presentation is still a CPU framebuffer blit; only *rendering* is GPU-backed.
 */
static void env_enable_gpu_defaults(void) {
    int dxg_fd = open("/dev/dxg", O_RDONLY);

    if (dxg_fd >= 0) {
        close(dxg_fd);
        env_set("LIBGL_ALWAYS_SOFTWARE", "0");
        env_set("GALLIUM_DRIVER", "d3d12");
        env_set("MESA_D3D12_DEFAULT_ADAPTER_NAME", "NVIDIA");
        env_set("LIBGL_DRIVERS_PATH", "/lib/dri");

        /*
         * The host D3D12 runtime (libd3d12*.so) and the NVIDIA UMD live in
         * /usr/lib/wsl/lib; make sure the dynamic loader finds them without
         * the caller exporting LD_LIBRARY_PATH by hand.
         */
        const char *cur = env_get("LD_LIBRARY_PATH");
        if (cur && cur[0]) {
            if (strstr(cur, "/usr/lib/wsl/lib") == 0) {
                char joined[MAX_ENV_VALUE];
                snprintf(joined, sizeof(joined), "/usr/lib/wsl/lib:%s", cur);
                env_set("LD_LIBRARY_PATH", joined);
            }
        } else {
            env_set("LD_LIBRARY_PATH",
                    "/usr/lib/wsl/lib:/lib:/usr/lib:"
                    "/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu");
        }
        return;
    }

    int render_fd = open("/dev/dri/renderD128", O_RDONLY);
    if (render_fd >= 0) {
        close(render_fd);
        env_set("LIBGL_ALWAYS_SOFTWARE", "0");
        env_set("GALLIUM_DRIVER", "virgl");
        env_set("LIBGL_DRIVERS_PATH", "/lib/dri");
        return;
    }

    env_set("LIBGL_ALWAYS_SOFTWARE", "1");
    env_unset("GALLIUM_DRIVER");
    env_unset("MESA_D3D12_DEFAULT_ADAPTER_NAME");
}

static void env_enable_gui_session(void) {
    env_set("HOME", "/root");
    env_set("PATH", "/usr/local/bin:/usr/bin:/bin:/");
    env_set("TERM", "dumb");
    env_set("PS1", "\\w# ");
    env_set("XDG_RUNTIME_DIR", "/tmp");
    env_set("XDG_CACHE_HOME", "/tmp/.cache");
    env_set("WAYLAND_DISPLAY", "wayland-0");
    env_set("GDK_BACKEND", "wayland");
    env_set("GDK_GL", "gles");
    /*
     * Hyper-V GPU-P / WSL guest: no virtio render node, but /dev/dxg exposes
     * the D3DKMT transport to the host GPU.  Mesa's d3d12 Gallium driver
     * translates OpenGL ES to Direct3D 12 and runs it on the real adapter.
     * Shared with plain terminal shells via env_enable_gpu_defaults().
     */
    env_enable_gpu_defaults();
    env_set("EGL_PLATFORM", "wayland");
    env_set("XCURSOR_PATH", "/share/icons");
    env_set("XCURSOR_THEME", "Adwaita");
    env_set("SSL_CERT_FILE", "/share/netsurf/ca-bundle");
    env_set("XV6_GUI_SESSION", "wayland");
}

static int parent_is_gui_session(void) {
    char path[64];
    char pidbuf[16];
    char buf[256];
    int n = itoa_local(getppid(), pidbuf, sizeof(pidbuf));
    int off = 0;

    memcpy(path + off, "/proc/", 6);
    off += 6;
    memcpy(path + off, pidbuf, n);
    off += n;
    memcpy(path + off, "/status", 8);
    off += 8;
    path[off] = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    int total = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (total <= 0)
        return 0;
    buf[total] = 0;
    return contains_local(buf, "Name:\tweston-session\n") ||
           contains_local(buf, "Name:\tweston\n");
}

static int can_enable_gui_session(void) {
    return parent_is_gui_session();
}

static const char *path_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            base = p + 1;
    }
    return base;
}

static int is_gui_only_command(const char *cmd) {
    if (!cmd || !cmd[0])
        return 0;

    const char *base = path_basename(cmd);

    return strcmp(base, "netsurf") == 0 ||
           strcmp(base, "MiniBrowser") == 0 ||
           strcmp(base, "weston") == 0 ||
           strcmp(base, "weston-session") == 0;
}

static int is_wayland_client_command(const char *cmd) {
    if (!cmd || !cmd[0])
        return 0;

    const char *base = path_basename(cmd);

    return strcmp(base, "weston-terminal") == 0 ||
           strcmp(base, "filemgr") == 0 ||
           strcmp(base, "glmaze") == 0 ||
           strcmp(base, "glsmoke") == 0 ||
           strcmp(base, "mesaglsmoke") == 0 ||
           strcmp(base, "mesawlegl") == 0 ||
           strcmp(base, "mesademo") == 0 ||
           strcmp(base, "peanutgb") == 0 ||
           strcmp(base, "netsurf") == 0 ||
           strcmp(base, "MiniBrowser") == 0;
}

static int has_gui_session(void) {
    const char *session = env_get("XV6_GUI_SESSION");
    const char *runtime = env_get("XDG_RUNTIME_DIR");
    const char *display = env_get("WAYLAND_DISPLAY");

    return gui_session_shell &&
           session && strcmp(session, "wayland") == 0 &&
           runtime && runtime[0] &&
           display && display[0];
}

static int refuse_gui_only_without_session(const char *cmd) {
    if (!is_gui_only_command(cmd) || has_gui_session())
        return 0;

    errprintf("%s: GUI session required; refusing to launch from serial/ssh/telnet\n",
              path_basename(cmd));
    return 1;
}

static void env_list(void) {
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used)
            printf("%s=%s\n", env_vars[i].name, env_vars[i].value);
    }
}

// Expand $VAR, ${VAR}, $$, $? in a string.
// Returns pointer to a static buffer.
static char expand_buf[LINE_BUF_SIZE * 2];
static int expand_truncated;

static char *expand_env_vars(const char *input) {
    char *out = expand_buf;
    char *out_end = expand_buf + sizeof(expand_buf) - 1;
    const char *p = input;

    expand_truncated = 0;
    while (*p && out < out_end) {
        if (*p == '\'') {
            *out++ = *p++;
            while (*p && out < out_end) {
                *out++ = *p;
                if (*p++ == '\'')
                    break;
            }
        } else if (*p == '$') {
            p++;
            if (*p == '{') {
                // ${VAR}
                p++;
                const char *start = p;
                while (*p && *p != '}')
                    p++;
                int len = p - start;
                if (*p == '}')
                    p++;
                char varname[MAX_ENV_NAME];
                if (len >= MAX_ENV_NAME)
                    len = MAX_ENV_NAME - 1;
                memcpy(varname, start, len);
                varname[len] = 0;
                char *val = env_get(varname);
                if (val) {
                    while (*val && out < out_end)
                        *out++ = *val++;
                }
            } else if (*p == '$') {
                // $$ = PID
                p++;
                char pidbuf[16];
                int n = itoa_local(getpid(), pidbuf, sizeof(pidbuf));
                for (int j = 0; j < n && out < out_end; j++)
                    *out++ = pidbuf[j];
            } else if (*p == '?') {
                // $? – not tracked, just emit '0'
                p++;
                if (out < out_end)
                    *out++ = '0';
            } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       *p == '_') {
                // $VAR
                const char *start = p;
                while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_')
                    p++;
                int len = p - start;
                char varname[MAX_ENV_NAME];
                if (len >= MAX_ENV_NAME)
                    len = MAX_ENV_NAME - 1;
                memcpy(varname, start, len);
                varname[len] = 0;
                char *val = env_get(varname);
                if (val) {
                    while (*val && out < out_end)
                        *out++ = *val++;
                }
            } else {
                // Lone $
                *out++ = '$';
            }
        } else {
            *out++ = *p++;
        }
    }
    if (*p)
        expand_truncated = 1;
    *out = 0;
    return expand_buf;
}

// =====================================================================
// Terminal control
// =====================================================================

static void enable_raw_mode(void) {
    if (raw_mode)
        return;
    struct termios raw;
    if (!orig_termios_saved) {
        if (tcgetattr(0, &orig_termios) < 0)
            return;
        orig_termios_saved = 1;
    }
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ISIG);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSAFLUSH, &raw) < 0)
        return;
    raw_mode = 1;
}

static void disable_raw_mode(void) {
    if (!raw_mode)
        return;
    tcsetattr(0, TCSAFLUSH, &orig_termios);
    raw_mode = 0;
}

static void term_write(const char *s, int n) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        for (int i = 0; i < n; i++) {
            char ch = s[i];
            if (ch == '\r') {
                int y, x;
                getyx(stdscr, y, x);
                (void)x;
                move(y, 0);
            } else if (ch == '\b') {
                int y, x;
                getyx(stdscr, y, x);
                if (x > 0)
                    move(y, x - 1);
            } else {
                addch(ch);
            }
        }
        refresh();
        return;
    }
#endif
    if (write(1, s, n) < 0) {
    }
}

static void term_putc(char c) { term_write(&c, 1); }

static void term_cursor_left(int n) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        int y, x;
        getyx(stdscr, y, x);
        x -= n;
        if (x < 0)
            x = 0;
        move(y, x);
        refresh();
        return;
    }
#endif
    while (n-- > 0)
        term_write("\x1b[D", 3);
}

static void term_cursor_right(int n) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        int y, x;
        getyx(stdscr, y, x);
        x += n;
        move(y, x);
        refresh();
        return;
    }
#endif
    while (n-- > 0)
        term_write("\x1b[C", 3);
}

static void term_clear_to_eol(void) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        clrtoeol();
        refresh();
        return;
    }
#endif
    term_write("\x1b[K", 3);
}

static void term_bell(void) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        beep();
        return;
    }
#endif
    term_putc('\x07');
}

static void term_clear_screen(void) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        clear();
        move(0, 0);
        refresh();
        return;
    }
#endif
    term_write("\x1b[2J\x1b[H", 7);
}

static void term_show_prompt(void) {
    term_write(user_name, strlen(user_name));
    term_write(":", 1);
    term_write(cwd_path, strlen(cwd_path));
    if (user_uid == 0)
        term_write("# ", 2);
    else
        term_write("$ ", 2);
}

// =====================================================================
// Line buffer operations
// =====================================================================

static void line_clear(void) {
    line_buf[0] = 0;
    line_len = 0;
    cursor_pos = 0;
}

static void line_insert_char(char c) {
    if (line_len >= LINE_BUF_SIZE - 1) {
        term_bell();
        return;
    }
    // Shift right
    for (int i = line_len; i > cursor_pos; i--)
        line_buf[i] = line_buf[i - 1];
    line_buf[cursor_pos] = c;
    line_len++;
    line_buf[line_len] = 0;

    // Write from cursor to end
    term_write(&line_buf[cursor_pos], line_len - cursor_pos);
    cursor_pos++;
    // Move cursor back
    if (cursor_pos < line_len)
        term_cursor_left(line_len - cursor_pos);
}

static void line_delete_char(void) {
    if (cursor_pos == 0) {
        term_bell();
        return;
    }
    cursor_pos--;
    for (int i = cursor_pos; i < line_len - 1; i++)
        line_buf[i] = line_buf[i + 1];
    line_len--;
    line_buf[line_len] = 0;

    term_write("\x08", 1);
    term_write(&line_buf[cursor_pos], line_len - cursor_pos);
    term_clear_to_eol();
    if (cursor_pos < line_len)
        term_cursor_left(line_len - cursor_pos);
}

static void line_delete_forward(void) {
    if (cursor_pos >= line_len) {
        term_bell();
        return;
    }
    for (int i = cursor_pos; i < line_len - 1; i++)
        line_buf[i] = line_buf[i + 1];
    line_len--;
    line_buf[line_len] = 0;

    term_write(&line_buf[cursor_pos], line_len - cursor_pos);
    term_clear_to_eol();
    if (cursor_pos < line_len)
        term_cursor_left(line_len - cursor_pos);
}

static void line_move_left(void) {
    if (cursor_pos > 0) {
        cursor_pos--;
        term_cursor_left(1);
    }
}

static void line_move_right(void) {
    if (cursor_pos < line_len) {
        cursor_pos++;
        term_cursor_right(1);
    }
}

static void line_move_home(void) {
    if (cursor_pos > 0) {
        term_cursor_left(cursor_pos);
        cursor_pos = 0;
    }
}

static void line_move_end(void) {
    if (cursor_pos < line_len) {
        term_cursor_right(line_len - cursor_pos);
        cursor_pos = line_len;
    }
}

static void line_kill_to_end(void) {
    term_clear_to_eol();
    line_len = cursor_pos;
    line_buf[line_len] = 0;
}

static void line_kill_all(void) {
    if (cursor_pos > 0)
        term_cursor_left(cursor_pos);
    term_clear_to_eol();
    line_clear();
}

static void line_set(const char *s) {
    if (cursor_pos > 0)
        term_cursor_left(cursor_pos);
    term_clear_to_eol();

    int n = strlen(s);
    if (n >= LINE_BUF_SIZE)
        n = LINE_BUF_SIZE - 1;
    memcpy(line_buf, s, n);
    line_buf[n] = 0;
    line_len = n;
    cursor_pos = n;
    term_write(line_buf, line_len);
}

// =====================================================================
// History management
// =====================================================================

static void history_add(const char *line) {
    if (line[0] == 0)
        return;
    // Skip duplicate of last entry
    if (history_count > 0) {
        int last = (history_start + history_count - 1) % HISTORY_SIZE;
        if (strcmp(history[last], line) == 0)
            return;
    }
    int idx;
    if (history_count < HISTORY_SIZE) {
        idx = history_count;
        history_count++;
    } else {
        idx = history_start;
        history_start = (history_start + 1) % HISTORY_SIZE;
    }
    int n = strlen(line);
    if (n >= LINE_BUF_SIZE)
        n = LINE_BUF_SIZE - 1;
    memcpy(history[idx], line, n);
    history[idx][n] = 0;
}

static void history_prev(void) {
    if (history_count == 0 || history_idx <= 0) {
        term_bell();
        return;
    }
    history_idx--;
    int idx = (history_start + history_idx) % HISTORY_SIZE;
    line_set(history[idx]);
}

static void history_next(void) {
    if (history_idx < history_count - 1) {
        history_idx++;
        int idx = (history_start + history_idx) % HISTORY_SIZE;
        line_set(history[idx]);
    } else if (history_idx == history_count - 1) {
        history_idx = history_count;
        line_set("");
    } else {
        term_bell();
    }
}

static void history_reset(void) { history_idx = history_count; }

// =====================================================================
// Tab completion
// =====================================================================

#define MAX_MATCHES 64
static char matches[MAX_MATCHES][NAME_MAX + 1];
static int match_count;

static int match_exists(const char *name) {
    for (int i = 0; i < match_count; i++) {
        if (strcmp(matches[i], name) == 0)
            return 1;
    }
    return 0;
}

// Return non-zero if c is a shell word-boundary character.
static int is_word_boundary(char c) {
    return c == ' ' || c == '\t' || c == '|' || c == ';' || c == '&' ||
           c == '>' || c == '<' || c == '(' || c == ')';
}

// Determine word at cursor and whether it is a command position.
static int get_completion_word(char *word, int maxlen, int *is_first_word) {
    int start = cursor_pos;
    while (start > 0 && !is_word_boundary(line_buf[start - 1]))
        start--;

    // Walk everything before start to decide whether this is a command
    // (first-word) position.  Pipe, semicolon, and ampersand reset to
    // command position; redirection operators and normal characters
    // indicate argument position.
    int first = 1;
    for (int i = 0; i < start; i++) {
        char c = line_buf[i];
        if (c == ' ' || c == '\t')
            continue;
        if (c == '|' || c == ';' || c == '&')
            first = 1; // next word is a command
        else
            first = 0; // next word is an argument / filename
    }
    if (is_first_word)
        *is_first_word = first;

    int len = cursor_pos - start;
    if (len >= maxlen)
        len = maxlen - 1;
    memcpy(word, &line_buf[start], len);
    word[len] = 0;
    return start;
}

static void get_completion_dir(const char *word, char *dir, char *prefix,
                               int maxdir) {
    const char *slash = 0;
    for (const char *p = word; *p; p++) {
        if (*p == '/')
            slash = p;
    }
    if (slash) {
        int dlen = slash - word + 1;
        if (dlen >= maxdir)
            dlen = maxdir - 1;
        memcpy(dir, word, dlen);
        dir[dlen] = 0;
        strcpy(prefix, slash + 1);
    } else {
        strcpy(dir, ".");
        strcpy(prefix, word);
    }
}

static void add_matches_from_dir(const char *dir, const char *prefix,
                                 int executables_only) {
    int prefix_len = strlen(prefix);
    int fd = open(dir, O_RDONLY);
    if (fd < 0)
        return;

    char dirent_buf[1024];
    int nread;

    while ((nread = getdents(fd, dirent_buf, sizeof(dirent_buf))) > 0) {
        int pos = 0;
        while (pos < nread && match_count < MAX_MATCHES) {
            struct linux_dirent64 *de =
                (struct linux_dirent64 *)(dirent_buf + pos);
            if (de->d_ino != 0) {
                if (prefix_len == 0 ||
                    strncmp_local(de->d_name, prefix, prefix_len) == 0) {
                    if (strcmp(de->d_name, ".") != 0 &&
                        strcmp(de->d_name, "..") != 0 &&
                        !match_exists(de->d_name)) {
                        if (executables_only) {
                            char path[512];
                            int dlen = strlen(dir);
                            memcpy(path, dir, dlen);
                            if (dlen > 0 && dir[dlen - 1] != '/')
                                path[dlen++] = '/';
                            strcpy(path + dlen, de->d_name);
                            struct stat st;
                            if (stat(path, &st) == 0 && S_ISREG(ST_MODE(st)))
                                strcpy(matches[match_count++], de->d_name);
                        } else {
                            strcpy(matches[match_count++], de->d_name);
                        }
                    }
                }
            }
            pos += de->d_reclen;
        }
    }
    close(fd);
}

static void collect_path_matches(const char *prefix) {
    match_count = 0;

    // Files in current directory
    add_matches_from_dir(".", prefix, 0);

    // Executables from PATH
    char *path = env_get("PATH");
    if (path) {
        char pathcopy[MAX_ENV_VALUE];
        int plen = strlen(path);
        if (plen >= MAX_ENV_VALUE)
            plen = MAX_ENV_VALUE - 1;
        memcpy(pathcopy, path, plen);
        pathcopy[plen] = 0;

        char *p = pathcopy;
        while (*p && match_count < MAX_MATCHES) {
            char *start = p;
            while (*p && *p != ':')
                p++;
            int dlen = p - start;
            if (dlen > 0 && dlen < 256) {
                char dir[256];
                memcpy(dir, start, dlen);
                dir[dlen] = 0;
                add_matches_from_dir(dir, prefix, 1);
            }
            if (*p == ':')
                p++;
        }
    }

    // Built-in commands
    static const char *builtins[] = {
        "cd", "ls", "echo", "exit", "export", "unset", "env", "history",
        "waitgdb", 0};
    int prefix_len = strlen(prefix);
    for (int i = 0; builtins[i] && match_count < MAX_MATCHES; i++) {
        if (prefix_len == 0 ||
            strncmp_local(builtins[i], prefix, prefix_len) == 0) {
            if (!match_exists(builtins[i]))
                strcpy(matches[match_count++], builtins[i]);
        }
    }
}

static int common_prefix_len(void) {
    if (match_count <= 1)
        return match_count ? (int)strlen(matches[0]) : 0;
    int len = 0;
    while (1) {
        char c = matches[0][len];
        if (c == 0)
            break;
        int ok = 1;
        for (int i = 1; i < match_count; i++) {
            if (matches[i][len] != c) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            break;
        len++;
    }
    return len;
}

static void do_tab_completion(void) {
    char word[NAME_MAX + 1];
    char dir[256];
    char prefix[NAME_MAX + 1];
    int is_first_word = 0;

    (void)get_completion_word(word, sizeof(word), &is_first_word);
    get_completion_dir(word, dir, prefix, sizeof(dir));

    if (is_first_word && strcmp(dir, ".") == 0)
        collect_path_matches(prefix);
    else {
        match_count = 0;
        add_matches_from_dir(dir, prefix, 0);
    }

    if (match_count == 0) {
        term_bell();
        return;
    }

    int prefix_len = strlen(prefix);
    int common = common_prefix_len();

    if (common > prefix_len) {
        for (int i = prefix_len; i < common; i++)
            line_insert_char(matches[0][i]);

        if (match_count == 1) {
            // Append / for dirs, space otherwise
            char path[512];
            if (strcmp(dir, ".") == 0)
                strcpy(path, matches[0]);
            else {
                strcpy(path, dir);
                strcat_local(path, matches[0]);
            }
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(ST_MODE(st)))
                line_insert_char('/');
            else
                line_insert_char(' ');
        }
    } else if (match_count > 1) {
        // Show candidates
        term_write("\r\n", 2);

        int maxlen = 0;
        for (int i = 0; i < match_count; i++) {
            int len = strlen(matches[i]);
            if (len > maxlen)
                maxlen = len;
        }
        int colwidth = maxlen + 2;
        if (colwidth < 8)
            colwidth = 8;
        int cols = 80 / colwidth;
        if (cols < 1)
            cols = 1;

        for (int i = 0; i < match_count; i++) {
            int len = strlen(matches[i]);
            term_write(matches[i], len);
            if ((i + 1) % cols == 0 || i == match_count - 1) {
                term_write("\r\n", 2);
            } else {
                for (int j = len; j < colwidth; j++)
                    term_putc(' ');
            }
        }

        // Re-display prompt + current line
        term_show_prompt();
        term_write(line_buf, line_len);
        if (cursor_pos < line_len)
            term_cursor_left(line_len - cursor_pos);
    }
}

// =====================================================================
// Escape sequence parsing
// =====================================================================

static int read_char(void) {
    char c;
    for (;;) {
        int n = read(0, &c, 1);
        if (n == 1)
            return (unsigned char)c;
        if (n == 0)
            return -1; // EOF
        // On xv6 console, raw reads can transiently return <0 during
        // mode transitions; keep retrying.
    }
}

#define KEY_EXT_BASE 0x100
#define KEY_EXT_UP (KEY_EXT_BASE + 1)
#define KEY_EXT_DOWN (KEY_EXT_BASE + 2)
#define KEY_EXT_LEFT (KEY_EXT_BASE + 3)
#define KEY_EXT_RIGHT (KEY_EXT_BASE + 4)
#define KEY_EXT_HOME (KEY_EXT_BASE + 5)
#define KEY_EXT_END (KEY_EXT_BASE + 6)
#define KEY_EXT_DELETE (KEY_EXT_BASE + 7)

static int read_key(void) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        int c = getch();
        switch (c) {
        case KEY_UP:
            return KEY_EXT_UP;
        case KEY_DOWN:
            return KEY_EXT_DOWN;
        case KEY_LEFT:
            return KEY_EXT_LEFT;
        case KEY_RIGHT:
            return KEY_EXT_RIGHT;
        case KEY_HOME:
            return KEY_EXT_HOME;
        case KEY_END:
            return KEY_EXT_END;
        case KEY_DC:
            return KEY_EXT_DELETE;
        case KEY_BACKSPACE:
            return 0x7f;
        case KEY_ENTER:
            return '\n';
        default:
            return c;
        }
    }
#endif

    int c = read_char();
    if (c != 0x1b)
        return c;

    int c1 = read_char();
    if (c1 < 0)
        return 0x1b;
    if (c1 == '[') {
        int c2 = read_char();
        if (c2 < 0)
            return 0x1b;
        switch (c2) {
        case 'A':
            return KEY_EXT_UP;
        case 'B':
            return KEY_EXT_DOWN;
        case 'C':
            return KEY_EXT_RIGHT;
        case 'D':
            return KEY_EXT_LEFT;
        case 'H':
            return KEY_EXT_HOME;
        case 'F':
            return KEY_EXT_END;
        case '3':
            if (read_char() == '~')
                return KEY_EXT_DELETE;
            break;
        case '1':
        case '7':
            if (read_char() == '~')
                return KEY_EXT_HOME;
            break;
        case '4':
        case '8':
            if (read_char() == '~')
                return KEY_EXT_END;
            break;
        }
        return 0x1b;
    }
    if (c1 == 'O') {
        int c2 = read_char();
        if (c2 == 'A')
            return KEY_EXT_UP;
        if (c2 == 'B')
            return KEY_EXT_DOWN;
        if (c2 == 'C')
            return KEY_EXT_RIGHT;
        if (c2 == 'D')
            return KEY_EXT_LEFT;
        if (c2 == 'H')
            return KEY_EXT_HOME;
        if (c2 == 'F')
            return KEY_EXT_END;
    }
    return 0x1b;
}

// =====================================================================
// readline – main line reading loop
// =====================================================================

static int do_readline(char *buf, int nbuf) {
    line_clear();
    history_reset();
    enable_raw_mode();
    term_show_prompt();

    while (1) {
        int c = read_key();
        if (c < 0) {
            disable_raw_mode();
            return -1;
        }

        switch (c) {
        case '\r':
        case '\n':
            term_write("\r\n", 2);
            disable_raw_mode();
            if (line_len > 0)
                history_add(line_buf);
            if (line_len >= nbuf)
                line_len = nbuf - 1;
            memcpy(buf, line_buf, line_len);
            buf[line_len] = '\n';
            buf[line_len + 1] = 0;
            return line_len + 1;

        case 0x04: // Ctrl+D
            if (line_len == 0) {
                term_write("\r\n", 2);
                disable_raw_mode();
                return -1;
            }
            line_delete_forward();
            break;

        case 0x03: // Ctrl+C
            term_write("^C\r\n", 4);
            line_clear();
            disable_raw_mode();
            enable_raw_mode();
            term_show_prompt();
            break;

        case 0x15: // Ctrl+U
            line_kill_all();
            break;

        case 0x0b: // Ctrl+K
            line_kill_to_end();
            break;

        case 0x01: // Ctrl+A
            line_move_home();
            break;

        case 0x05: // Ctrl+E
            line_move_end();
            break;

        case 0x08: // Ctrl+H / backspace
        case 0x7f: // DEL
            line_delete_char();
            break;

        case '\t':
            do_tab_completion();
            break;

        case KEY_EXT_UP:
            history_prev();
            break;

        case KEY_EXT_DOWN:
            history_next();
            break;

        case KEY_EXT_LEFT:
            line_move_left();
            break;

        case KEY_EXT_RIGHT:
            line_move_right();
            break;

        case KEY_EXT_HOME:
            line_move_home();
            break;

        case KEY_EXT_END:
            line_move_end();
            break;

        case KEY_EXT_DELETE:
            line_delete_forward();
            break;

        case 0x0c: // Ctrl+L – clear screen
            term_clear_screen();
            term_show_prompt();
            term_write(line_buf, line_len);
            if (cursor_pos < line_len)
                term_cursor_left(line_len - cursor_pos);
            break;

        default:
            if (c >= 32 && c < 127)
                line_insert_char(c);
            break;
        }
    }
}

// =====================================================================
// Built-in commands
// =====================================================================

static void update_cwd(void) {
    if (getcwd(cwd_path, sizeof(cwd_path)) == 0)
        strcpy(cwd_path, "?");
}

// Resolve username from /etc/passwd for the current uid.
static void update_user_info(void) {
#ifdef USE_NCURSES_SHELL
    user_uid = (int)getuid();
    struct passwd *pw = getpwuid(user_uid);
    if (pw && pw->pw_name) {
        int n = strlen(pw->pw_name);
        if (n >= (int)sizeof(user_name))
            n = sizeof(user_name) - 1;
        memcpy(user_name, pw->pw_name, n);
        user_name[n] = 0;
    } else {
        itoa_local(user_uid, user_name, sizeof(user_name));
    }
#else
    user_uid = getuid();
    // Parse /etc/passwd: each line is  name:x:uid:gid:...
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0) {
        itoa_local(user_uid, user_name, sizeof(user_name));
        return;
    }
    char pbuf[1024];
    int total = 0, n;
    while ((n = read(fd, pbuf + total, sizeof(pbuf) - total - 1)) > 0)
        total += n;
    close(fd);
    pbuf[total] = 0;

    char *p = pbuf;
    while (*p) {
        // Parse one line: name:x:uid:...
        char *line_start = p;
        // Find first colon (end of name)
        char *c1 = 0;
        for (char *q = p; *q && *q != '\n'; q++) {
            if (*q == ':' && !c1) { c1 = q; break; }
        }
        if (!c1) { while (*p && *p != '\n') p++; if (*p) p++; continue; }
        // Skip password field → find second colon
        char *c2 = 0;
        for (char *q = c1 + 1; *q && *q != '\n'; q++) {
            if (*q == ':') { c2 = q; break; }
        }
        if (!c2) { while (*p && *p != '\n') p++; if (*p) p++; continue; }
        // Parse uid number after second colon
        int pw_uid = 0;
        for (char *q = c2 + 1; *q >= '0' && *q <= '9'; q++)
            pw_uid = pw_uid * 10 + (*q - '0');
        if (pw_uid == user_uid) {
            int namelen = c1 - line_start;
            if (namelen >= (int)sizeof(user_name))
                namelen = sizeof(user_name) - 1;
            memcpy(user_name, line_start, namelen);
            user_name[namelen] = 0;
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    // Not found — use numeric uid
    itoa_local(user_uid, user_name, sizeof(user_name));
#endif
}

// ---- Enhanced ls ----

static char mode_type_char(mode_t m) {
    if (S_ISDIR(m))
        return 'd';
    if (S_ISLNK(m))
        return 'l';
    if (S_ISCHR(m))
        return 'c';
    if (S_ISBLK(m))
        return 'b';
    if (S_ISFIFO(m))
        return 'p';
    if (S_ISSOCK(m))
        return 's';
    return '-';
}

static void format_permissions(mode_t m, char *buf) {
    buf[0] = (m & S_IRUSR) ? 'r' : '-';
    buf[1] = (m & S_IWUSR) ? 'w' : '-';
    buf[2] = (m & S_IXUSR) ? 'x' : '-';
    buf[3] = (m & S_IRGRP) ? 'r' : '-';
    buf[4] = (m & S_IWGRP) ? 'w' : '-';
    buf[5] = (m & S_IXGRP) ? 'x' : '-';
    buf[6] = (m & S_IROTH) ? 'r' : '-';
    buf[7] = (m & S_IWOTH) ? 'w' : '-';
    buf[8] = (m & S_IXOTH) ? 'x' : '-';
    buf[9] = 0;
}

static char name_indicator(mode_t m) {
    if (S_ISDIR(m))
        return '/';
    if (S_ISLNK(m))
        return '@';
    if (S_ISSOCK(m))
        return '=';
    if (S_ISFIFO(m))
        return '|';
    if (m & (S_IXUSR | S_IXGRP | S_IXOTH))
        return '*';
    return 0;
}

static void ls_print_entry(char *path, struct stat *st) {
    char perms[10];
    format_permissions(ST_MODE_P(st), perms);

    // Extract basename
    char *name;
    for (name = path + strlen(path); name >= path && *name != '/'; name--)
        ;
    name++;

    char ind = name_indicator(ST_MODE_P(st));

    if (ind)
         printf("%c%s %3lu %7ld %s%c\n", mode_type_char(ST_MODE_P(st)), perms,
             (unsigned long)ST_NLINK_P(st), (long)ST_SIZE_P(st), name, ind);
    else
         printf("%c%s %3lu %7ld %s\n", mode_type_char(ST_MODE_P(st)), perms,
             (unsigned long)ST_NLINK_P(st), (long)ST_SIZE_P(st), name);
}

static void builtin_ls(char *path) {
    int fd;
    struct stat st;

    if ((fd = open(path, O_RDONLY)) < 0) {
        errprintf("ls: cannot open %s\n", path);
        return;
    }
    if (fstat(fd, &st) < 0) {
        errprintf("ls: cannot stat %s\n", path);
        close(fd);
        return;
    }

    if (!S_ISDIR(ST_MODE(st))) {
        // Single file
        ls_print_entry(path, &st);
        close(fd);
        return;
    }

    // Directory listing
    char buf[512], *p;
    if (strlen(path) + 1 + NAME_MAX + 1 > sizeof(buf)) {
        printf("ls: path too long\n");
        close(fd);
        return;
    }
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';

    char dirent_buf[1024];
    int nread;

    while ((nread = getdents(fd, dirent_buf, sizeof(dirent_buf))) > 0) {
        int pos = 0;
        while (pos < nread) {
            struct linux_dirent64 *de =
                (struct linux_dirent64 *)(dirent_buf + pos);
            if (de->d_ino == 0) {
                pos += de->d_reclen;
                continue;
            }
            strcpy(p, de->d_name);
            if (stat(buf, &st) < 0) {
                printf("ls: cannot stat %s\n", buf);
                pos += de->d_reclen;
                continue;
            }
            ls_print_entry(buf, &st);
            pos += de->d_reclen;
        }
    }
    close(fd);
}

static void builtin_history(void) {
    for (int i = 0; i < history_count; i++) {
        int idx = (history_start + i) % HISTORY_SIZE;
        printf("%3d  %s\n", i + 1, history[idx]);
    }
}

static void copy_assignment_env(char *dst, const char *assignment) {
    int nlen = env_assignment_name_len(assignment);
    int vlen;

    if (nlen >= MAX_ENV_NAME)
        nlen = MAX_ENV_NAME - 1;
    vlen = strlen(assignment + env_assignment_name_len(assignment) + 1);
    if (vlen >= MAX_ENV_VALUE)
        vlen = MAX_ENV_VALUE - 1;
    memcpy(dst, assignment, nlen);
    dst[nlen] = '=';
    memcpy(dst + nlen + 1, assignment + env_assignment_name_len(assignment) + 1, vlen);
    dst[nlen + 1 + vlen] = 0;
}

static int assignment_has_later_override(char **assignv, int assignc, int idx) {
    for (int j = idx + 1; j < assignc; j++) {
        int nlen = env_assignment_name_len(assignv[idx]);
        if (nlen == env_assignment_name_len(assignv[j]) &&
            strncmp_local(assignv[idx], assignv[j], nlen) == 0)
            return 1;
    }
    return 0;
}

static char **build_exec_envp_with_assignments(char **assignv, int assignc) {
    static char env_storage[MAX_ENV_VARS][MAX_ENV_NAME + MAX_ENV_VALUE + 2];
    static char *envp[MAX_ENV_VARS + 1];
    int out = 0;

    for (int i = 0; i < MAX_ENV_VARS && out < MAX_ENV_VARS; i++) {
        int match = -1;

        if (!env_vars[i].used)
            continue;
        for (int j = 0; j < assignc; j++) {
            if (assignment_matches_name(assignv[j], env_vars[i].name))
                match = j;
        }
        if (match >= 0) {
            copy_assignment_env(env_storage[out], assignv[match]);
            envp[out] = env_storage[out];
            out++;
            continue;
        }
        int nlen = strlen(env_vars[i].name);
        int vlen = strlen(env_vars[i].value);
        if (nlen >= MAX_ENV_NAME)
            nlen = MAX_ENV_NAME - 1;
        if (vlen >= MAX_ENV_VALUE)
            vlen = MAX_ENV_VALUE - 1;

        memcpy(env_storage[out], env_vars[i].name, nlen);
        env_storage[out][nlen] = '=';
        memcpy(env_storage[out] + nlen + 1, env_vars[i].value, vlen);
        env_storage[out][nlen + 1 + vlen] = 0;
        envp[out] = env_storage[out];
        out++;
    }
    for (int j = 0; j < assignc && out < MAX_ENV_VARS; j++) {
        int exists = 0;

        if (!is_env_assignment_word(assignv[j]) ||
            assignment_has_later_override(assignv, assignc, j))
            continue;
        for (int i = 0; i < MAX_ENV_VARS; i++) {
            if (env_vars[i].used && assignment_matches_name(assignv[j], env_vars[i].name)) {
                exists = 1;
                break;
            }
        }
        if (exists)
            continue;
        copy_assignment_env(env_storage[out], assignv[j]);
        envp[out] = env_storage[out];
        out++;
    }
    envp[out] = 0;
    return envp;
}

static char **build_exec_envp(void) {
    return build_exec_envp_with_assignments(0, 0);
}

static int shell_exec(char *path, char **argv) {
    return execve(path, argv, build_exec_envp());
}

static int shell_exec_with_assignments(char *path, char **argv,
                                       char **assignv, int assignc) {
    return execve(path, argv, build_exec_envp_with_assignments(assignv, assignc));
}

static int shell_exec_env(char *path, char **argv, char **assignv, int assignc) {
    if (assignc == 0)
        return shell_exec(path, argv);
    return shell_exec_with_assignments(path, argv, assignv, assignc);
}

static int shell_fork(void) {
#ifdef USE_NCURSES_SHELL
    return fork();
#else
    return vfork();
#endif
}

static int maybe_add_wayland_client_env(char *cmd, char **assignv, int assignc,
                                        char **merged, int merged_cap) {
    static char *gui_defaults[] = {
        "XDG_RUNTIME_DIR=/tmp",
        "XDG_CACHE_HOME=/tmp/.cache",
        "WAYLAND_DISPLAY=wayland-0",
        "GDK_BACKEND=wayland",
        "GDK_GL=gles",
        "EGL_PLATFORM=wayland",
        "XCURSOR_PATH=/share/icons",
        "XCURSOR_THEME=Adwaita",
        "SSL_CERT_FILE=/share/netsurf/ca-bundle",
        "XV6_GUI_SESSION=wayland",
    };
    int out = 0;

    if (is_wayland_client_command(cmd) && !has_gui_session()) {
        int count = sizeof(gui_defaults) / sizeof(gui_defaults[0]);
        for (int i = 0; i < count && out < merged_cap; i++)
            merged[out++] = gui_defaults[i];
    }

    for (int i = 0; i < assignc && out < merged_cap; i++)
        merged[out++] = assignv[i];

    return out;
}

// =====================================================================
// PATH-based exec
// =====================================================================

static void exec_with_path_env(char *cmd, char **argv, char **assignv, int assignc) {
    char *merged_assignv[MAX_ENV_VARS];
    int merged_assignc = maybe_add_wayland_client_env(
        cmd, assignv, assignc, merged_assignv, MAX_ENV_VARS);
    assignv = merged_assignv;
    assignc = merged_assignc;

    // If contains '/', use directly
    for (char *p = cmd; *p; p++) {
        if (*p == '/') {
            shell_exec_env(cmd, argv, assignv, assignc);
            return;
        }
    }

    // Try as-is (current dir)
    shell_exec_env(cmd, argv, assignv, assignc);

    // Search PATH
    char *path = env_get("PATH");
    if (!path)
        return;

    char pathcopy[MAX_ENV_VALUE];
    int plen = strlen(path);
    if (plen >= MAX_ENV_VALUE)
        plen = MAX_ENV_VALUE - 1;
    memcpy(pathcopy, path, plen);
    pathcopy[plen] = 0;

    char *p = pathcopy;
    while (*p) {
        char *start = p;
        while (*p && *p != ':')
            p++;
        int dlen = p - start;
        if (dlen > 0) {
            char fullpath[512];
            memcpy(fullpath, start, dlen);
            if (fullpath[dlen - 1] != '/')
                fullpath[dlen++] = '/';
            strcpy(fullpath + dlen, cmd);
            shell_exec_env(fullpath, argv, assignv, assignc);
        }
        if (*p == ':')
            p++;
    }
}

// =====================================================================
// Pipe helpers (vfork-safe wrappers)
// =====================================================================

static void run_pipe_left(struct cmd *cmd, int *p) __attribute__((noreturn));
static void run_pipe_left(struct cmd *cmd, int *p) {
    close(1);
    if (dup(p[1]) < 0)
        panic("dup");
    close(p[0]);
    close(p[1]);
    runcmd(cmd);
}

static void run_pipe_right(struct cmd *cmd, int *p) __attribute__((noreturn));
static void run_pipe_right(struct cmd *cmd, int *p) {
    close(0);
    if (dup(p[0]) < 0)
        panic("dup");
    close(p[0]);
    close(p[1]);
    runcmd(cmd);
}

// =====================================================================
// Command execution (recursive walk of the parse tree)
// =====================================================================

void runcmd(struct cmd *cmd) {
    int p[2];
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;
    int pid;

    if (cmd == 0)
        exit(1);

    switch (cmd->type) {
    default:
        panic("runcmd");
        break;

    case EXEC: {
        ecmd = (struct execcmd *)cmd;
        if (ecmd->argv[0] == 0)
            exit(1);
        int assignc = 0;
        while (ecmd->argv[assignc] && is_env_assignment_word(ecmd->argv[assignc]))
            assignc++;
        if (ecmd->argv[assignc] == 0) {
            for (int i = 0; i < assignc; i++)
                env_set_assignment(ecmd->argv[i]);
            exit(0);
        }
        char **exec_argv = &ecmd->argv[assignc];

        // waitgdb: pause for debugger, then exec the real command
        // waitgdb -e <cmd>: also stop at entry point after exec
        if (strcmp(exec_argv[0], "waitgdb") == 0) {
            int stop_entry = 0;
            int cmd_idx = 1;
            if (exec_argv[1] && strcmp(exec_argv[1], "-e") == 0) {
                stop_entry = 1;
                cmd_idx = 2;
            }
            if (exec_argv[cmd_idx] == 0) {
                errprintf("usage: waitgdb [-e] <command> [args...]\n");
                exit(1);
            }
            if (refuse_gui_only_without_session(exec_argv[cmd_idx]))
                exit(126);
            if (stop_entry)
                waitgdb_stopentry();
            else
                waitgdb();
            exec_with_path_env(exec_argv[cmd_idx], &exec_argv[cmd_idx],
                               ecmd->argv, assignc);
            errprintf("waitgdb: exec %s failed\n", exec_argv[cmd_idx]);
            exit(127);
        }
        if (refuse_gui_only_without_session(exec_argv[0]))
            exit(126);
        exec_with_path_env(exec_argv[0], exec_argv, ecmd->argv, assignc);
        errprintf("exec %s failed\n", exec_argv[0]);
        exit(127);
    }

    case REDIR:
        rcmd = (struct redircmd *)cmd;
        close(rcmd->fd);
        if (shell_open(rcmd->file, rcmd->mode) < 0) {
            errprintf("open %s failed\n", rcmd->file);
            exit(1);
        }
        runcmd(rcmd->cmd);
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;
        pid = shell_fork();
        if (pid < 0)
            panic("fork");
        if (pid == 0)
            runcmd(lcmd->left);
        wait(0);
        runcmd(lcmd->right);
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        if (pipe(p) < 0)
            panic("pipe");
        pid = shell_fork();
        if (pid < 0)
            panic("fork");
        if (pid == 0)
            run_pipe_left(pcmd->left, p);
        pid = shell_fork();
        if (pid < 0)
            panic("fork");
        if (pid == 0)
            run_pipe_right(pcmd->right, p);
        close(p[0]);
        close(p[1]);
        wait(0);
        wait(0);
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;
        pid = shell_fork();
        if (pid < 0)
            panic("fork");
        if (pid == 0)
            runcmd(bcmd->cmd);
        break;
    }
    exit(0);
}

// =====================================================================
// getcmd – prompt + readline
// =====================================================================

int getcmd(char *buf, int nbuf) {
    memset(buf, 0, nbuf);
    int n = do_readline(buf, nbuf);
    if (n < 0)
        return -1;
    return 0;
}

// =====================================================================
// run_line – execute a single command line (builtins + externals)
//
// Returns:  0 = keep going,  1 = "exit" was requested,  -1 = error
// =====================================================================

static int line_has_control_operator(const char *s) {
    int quote = 0;

    for (; *s; s++) {
        if (*s == '\\' && s[1] != 0) {
            s++;
            continue;
        }
        if (quote) {
            if (*s == quote)
                quote = 0;
            continue;
        }
        if (*s == '\'' || *s == '"') {
            quote = *s;
            continue;
        }
        if (strchr("|&;<>()", *s) != 0)
            return 1;
    }
    return 0;
}

static int run_assignment_only_line(char *buf) {
    char tmp[LINE_BUF_SIZE];
    int len = strlen(buf);
    char *p;
    int seen = 0;

    if (len >= (int)sizeof(tmp))
        len = sizeof(tmp) - 1;
    memcpy(tmp, buf, len);
    tmp[len] = 0;

    p = tmp;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (*p == 0)
            break;
        char *word = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
            p++;
        char saved = *p;
        *p = 0;
        if (!is_env_assignment_word(word))
            return 0;
        seen = 1;
        *p = saved;
    }
    if (!seen)
        return 0;

    p = tmp;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (*p == 0)
            break;
        char *word = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
            p++;
        char saved = *p;
        *p = 0;
        if (env_set_assignment(word) < 0)
            errprintf("sh: bad assignment: %s\n", word);
        *p = saved;
    }
    return 1;
}

static int run_line(char *buf, int interactive) {
    static char expanded_buf[LINE_BUF_SIZE * 2];
    int simple_builtin_line;

    int len = strlen(buf);
    // Ensure the line ends with '\n' (parsecmd and builtins expect it)
    if (len > 0 && buf[len - 1] != '\n') {
        if (len < LINE_BUF_SIZE - 1) {
            buf[len] = '\n';
            buf[len + 1] = 0;
        } else {
            errprintf("sh: command line too long\n");
            return -1;
        }
    }

    // Skip empty or comment-only lines
    {
        const char *p = buf;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '#')
            return 0;
    }

    simple_builtin_line = !line_has_control_operator(buf);

    if (simple_builtin_line && run_assignment_only_line(buf))
        return 0;

    // ---- Built-in: cd ----
    if (simple_builtin_line && buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' ') {
        buf[strlen(buf) - 1] = 0; // chop \n
        char *path = buf + 3;
        char *expanded = expand_env_vars(path);
        if (expand_truncated) {
            errprintf("sh: expanded command too long\n");
            return -1;
        }
        if (chdir(expanded) < 0)
            errprintf("cannot cd %s\n", expanded);
        else
            update_cwd();
        return 0;
    }

    // ---- Built-in: ls ----
    if (simple_builtin_line && buf[0] == 'l' && buf[1] == 's' &&
        (buf[2] == '\n' || buf[2] == ' ')) {
        buf[strlen(buf) - 1] = 0;
        if (buf[2] == 0 || buf[3] == 0)
            builtin_ls(".");
        else
            builtin_ls(buf + 3);
        return 0;
    }

    // ---- Built-in: history ----
    if (simple_builtin_line && strncmp_local(buf, "history", 7) == 0 &&
        (buf[7] == '\n' || buf[7] == 0)) {
        builtin_history();
        return 0;
    }

    // ---- Built-in: env ----
    if (simple_builtin_line && strncmp_local(buf, "env", 3) == 0 &&
        (buf[3] == '\n' || buf[3] == 0)) {
        env_list();
        return 0;
    }

    // ---- Built-in: export VAR=value ----
    if (simple_builtin_line && strncmp_local(buf, "export ", 7) == 0) {
        buf[strlen(buf) - 1] = 0;
        char *arg = buf + 7;
        while (*arg == ' ')
            arg++;
        char *eq = strchr(arg, '=');
        if (eq) {
            *eq = 0;
            char *name = arg;
            char *value = eq + 1;
            int vlen = strlen(value);
            if (vlen >= 2 &&
                ((value[0] == '"' && value[vlen - 1] == '"') ||
                 (value[0] == '\'' && value[vlen - 1] == '\''))) {
                value[vlen - 1] = 0;
                value++;
            }
            if (env_set(name, value) < 0)
                errprintf("export: too many variables\n");
        } else {
            errprintf("export: usage: export VAR=value\n");
        }
        return 0;
    }

    // ---- Built-in: unset ----
    if (simple_builtin_line && strncmp_local(buf, "unset ", 6) == 0) {
        buf[strlen(buf) - 1] = 0;
        char *name = buf + 6;
        while (*name == ' ')
            name++;
        env_unset(name);
        return 0;
    }

    // ---- Built-in: echo (with expansion) ----
    if (simple_builtin_line && strncmp_local(buf, "echo ", 5) == 0) {
        buf[strlen(buf) - 1] = 0;
        char *expanded = expand_env_vars(buf + 5);
        if (expand_truncated) {
            errprintf("sh: expanded command too long\n");
            return -1;
        }
        printf("%s\n", expanded);
        return 0;
    }

    // ---- Built-in: exit ----
    if (simple_builtin_line && strncmp_local(buf, "exit", 4) == 0 &&
        (buf[4] == '\n' || buf[4] == 0))
        return 1;

    // ---- External command ----
    // Expand env vars
    char *expanded = expand_env_vars(buf);
    if (expand_truncated) {
        errprintf("sh: expanded command too long\n");
        return -1;
    }
    int elen = strlen(expanded);
    memcpy(expanded_buf, expanded, elen);
    expanded_buf[elen] = 0;

    struct cmd *cmd = parsecmd(expanded_buf);
    if (cmd == 0)
        return 0;

    // Skip empty commands (e.g. bare Enter)
    if (cmd->type == EXEC && ((struct execcmd *)cmd)->argv[0] == 0)
        return 0;

    int pid;

    if (interactive) {
        // Use fork (not vfork) for interactive commands — vfork shares
        // the parent's address space, so the child's stack operations
        // (setpgid, tcgetattr, tcsetattr, runcmd) can corrupt the
        // parent's local variables including our saved termios state.
        pid = fork();
        if (pid < 0)
            panic("fork");
        if (pid == 0) {
            setpgid(0, 0); // New process group (pgid = own pid)

            // Child side: ensure we own foreground tty and restore sane mode
            // before launching interactive programs like vim, python.
            int child_pgid = getpgid(0);
            ioctl(0, TIOCSPGRP, &child_pgid);

            // Set a fully sane terminal state for the child
            struct termios child_termios;
            if (tcgetattr(0, &child_termios) == 0) {
                child_termios.c_lflag |= (ICANON | ECHO | ISIG | ECHOE | ECHOK);
                child_termios.c_iflag |= (ICRNL | IXON);
                child_termios.c_oflag |= (OPOST | ONLCR);
                child_termios.c_cc[VMIN] = 1;
                child_termios.c_cc[VTIME] = 0;
                tcsetattr(0, TCSANOW, &child_termios);
            }

            runcmd(cmd);
        }

        // Also set from parent side to avoid race.
        (void)setpgid(pid, pid);

        int child_pgid = pid;
        ioctl(0, TIOCSPGRP, &child_pgid);

        int status = 0;
        waitpid(pid, &status, WUNTRACED);

        // Restore shell as foreground
        {
            int shell_pgid = getpgid(0);
            ioctl(0, TIOCSPGRP, &shell_pgid);
        }

        // Restore the pristine terminal state (captured once at shell
        // startup).  orig_termios is guaranteed to have OPOST|ONLCR
        // and all other sane flags — it cannot be corrupted by children.
        tcsetattr(0, TCSANOW, &orig_termios);

        if (WIFSTOPPED(status)) {
            printf("[suspended] pid %d\n", pid);
        }
    } else {
        // Non-interactive: simpler fork+exec without job control
        pid = fork();
        if (pid < 0)
            panic("fork");
        if (pid == 0)
            runcmd(cmd);

        int status = 0;
        waitpid(pid, &status, 0);
    }

    return 0;
}

// =====================================================================
// run_script – execute commands from a file, line by line
// =====================================================================

static int run_script(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        errprintf("sh: cannot open %s\n", path);
        return 1;
    }

    // Read the entire file (up to 16KB)
    #define SCRIPT_MAX 16384
    char *filebuf = malloc(SCRIPT_MAX);
    if (filebuf == 0) {
        errprintf("sh: out of memory\n");
        close(fd);
        return 1;
    }

    int total = 0;
    int n;
    while ((n = read(fd, filebuf + total, SCRIPT_MAX - total - 1)) > 0)
        total += n;
    close(fd);
    filebuf[total] = '\0';

    // Execute line by line
    char *line = filebuf;
    int ret = 0;
    while (*line) {
        char *eol = line;
        while (*eol && *eol != '\n')
            eol++;

        char saved = *eol;
        *eol = '\0';

        // Copy to a mutable buffer for run_line
        char linebuf[LINE_BUF_SIZE];
        int llen = strlen(line);
        if (llen >= (int)sizeof(linebuf))
            llen = sizeof(linebuf) - 1;
        memcpy(linebuf, line, llen);
        linebuf[llen] = 0;

        ret = run_line(linebuf, 0);
        if (ret == 1)
            break; // "exit" encountered

        if (saved == '\0')
            break;
        line = eol + 1;
    }

    free(filebuf);
    return 0;
}

static int shell_option_cluster_is_supported(const char *arg) {
    if (arg == 0 || arg[0] != '-' || arg[1] == 0)
        return 0;
    for (const char *p = arg + 1; *p; p++) {
        if (*p != 'c' && *p != 'l')
            return 0;
    }
    return 1;
}

static int shell_option_has(const char *arg, char opt) {
    if (!shell_option_cluster_is_supported(arg))
        return 0;
    for (const char *p = arg + 1; *p; p++) {
        if (*p == opt)
            return 1;
    }
    return 0;
}

// =====================================================================
// =====================================================================
// main
// =====================================================================

int main(int argc, char *argv[]) {
    static char buf[LINE_BUF_SIZE];
    int fd;

    // Ensure three file descriptors are open.
    while ((fd = open("/dev/console", O_RDWR)) >= 0) {
        if (fd >= 3) {
            close(fd);
            break;
        }
    }

    env_init();

    /*
     * Make the host-GPU rendering environment a default for *every* shell
     * (serial, terminal, GUI) whenever the transport device exists, so
     * manually launched GL/D3D12 programs use the real GPU without the user
     * exporting GALLIUM_DRIVER / MESA_D3D12_DEFAULT_ADAPTER_NAME /
     * LD_LIBRARY_PATH by hand each time.  The GUI session layers its own
     * Wayland/EGL vars on top via env_enable_gui_session().
     */
    env_enable_gpu_defaults();

    int argi = 1;
    if (argc >= 2 && strcmp(argv[1], "--gui-session") == 0) {
        if (can_enable_gui_session()) {
            gui_session_shell = 1;
            env_enable_gui_session();
        } else {
            errprintf("sh: refusing --gui-session outside Weston session\n");
        }
        argi = 2;
    }

    update_cwd();
    update_user_info();

#ifdef USE_NCURSES_SHELL
    setenv("TERMINFO", "/usr/share/terminfo", 1);
    if (getenv("TERM") == 0)
        setenv("TERM", "xterm", 1);
#endif

    while (argi < argc && strcmp(argv[argi], "-l") == 0)
        argi++;

    // ---- sh -c "command" / sh -lc "command" ----
    if (argc >= argi + 2 &&
        (strcmp(argv[argi], "-c") == 0 || shell_option_has(argv[argi], 'c'))) {
        // glibc system() may invoke "sh -c -- command" so that command
        // strings beginning with '-' are not parsed as shell options.
        int cmd_argi = argi + 1;
        if (cmd_argi < argc && strcmp(argv[cmd_argi], "--") == 0)
            cmd_argi++;

        // Concatenate all remaining args with spaces (sh -c "cmd" arg0 arg1)
        char cmdbuf[LINE_BUF_SIZE];
        int pos = 0;
        for (int i = cmd_argi; i < argc && pos < (int)sizeof(cmdbuf) - 2; i++) {
            if (i > cmd_argi && pos < (int)sizeof(cmdbuf) - 1)
                cmdbuf[pos++] = ' ';
            int alen = strlen(argv[i]);
            if (alen > (int)sizeof(cmdbuf) - pos - 1)
                alen = sizeof(cmdbuf) - pos - 1;
            memcpy(cmdbuf + pos, argv[i], alen);
            pos += alen;
        }
        cmdbuf[pos] = 0;
        run_line(cmdbuf, 0);
        exit(0);
    }

    // ---- sh script.sh [args...] ----
    if (argc >= argi + 1 && argv[argi][0] != '-') {
        int ret = run_script(argv[argi]);
        exit(ret);
    }

    // ---- Interactive mode ----

    // Capture the pristine terminal state before entering the main loop.
    // This is the known-good state we restore to after every child exits.
    if (!orig_termios_saved) {
        if (tcgetattr(0, &orig_termios) == 0)
            orig_termios_saved = 1;
    }

    for (;;) {
        if (getcmd(buf, sizeof(buf)) < 0)
            break; // EOF (e.g. PTY closed) or Ctrl-D

        int ret = run_line(buf, 1);
        if (ret == 1)
            break; // exit
    }

    disable_raw_mode();
    exit(0);
}

void panic(char *s) {
    errprintf("%s\n", s);
    exit(1);
}

// =====================================================================
// Command constructors
// =====================================================================

struct cmd *execcmd(void) {
    struct execcmd *cmd;
    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;
    return (struct cmd *)cmd;
}

struct cmd *redircmd(struct cmd *subcmd, char *file, char *efile, int mode,
                     int fd) {
    struct redircmd *cmd;
    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDIR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->mode = mode;
    cmd->fd = fd;
    return (struct cmd *)cmd;
}

struct cmd *pipecmd(struct cmd *left, struct cmd *right) {
    struct pipecmd *cmd;
    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd *)cmd;
}

struct cmd *listcmd(struct cmd *left, struct cmd *right) {
    struct listcmd *cmd;
    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd *)cmd;
}

struct cmd *backcmd(struct cmd *subcmd) {
    struct backcmd *cmd;
    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;
    return (struct cmd *)cmd;
}

// =====================================================================
// Tokeniser & parser (standard xv6 parser)
// =====================================================================

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int gettoken(char **ps, char *es, char **q, char **eq) {
    char *s;
    int ret;

    s = *ps;
    while (s < es && strchr(whitespace, *s))
        s++;
    if (q)
        *q = s;
    ret = *s;
    switch (*s) {
    case 0:
        break;
    case '|':
    case '(':
    case ')':
    case ';':
    case '&':
    case '<':
        s++;
        break;
    case '>':
        s++;
        if (*s == '>') {
            ret = '+';
            s++;
        }
        break;
    default:
        ret = 'a';
        while (s < es && !strchr(whitespace, *s) && !strchr(symbols, *s)) {
            if (*s == '\\' && s + 1 < es) {
                /* Backslash escape — skip the next character */
                s += 2;
            } else if (*s == '\'') {
                /* Single-quoted string — consume until unescaped closing
                 * single quote.  Double quotes inside are literal chars.
                 * Backslash-single-quote (\') is an escape. */
                s++;
                while (s < es && *s != '\'') {
                    if (*s == '\\' && s + 1 < es && s[1] == '\'')
                        s += 2;   /* \' — escaped single quote */
                    else
                        s++;
                }
                if (s < es)
                    s++; /* skip closing quote */
            } else if (*s == '"') {
                /* Double-quoted string — consume until unescaped closing
                 * double quote.  Single quotes inside are literal chars.
                 * Backslash escapes: \" \\  (all others literal). */
                s++;
                while (s < es && *s != '"') {
                    if (*s == '\\' && s + 1 < es)
                        s += 2;
                    else
                        s++;
                }
                if (s < es)
                    s++; /* skip closing quote */
            } else {
                s++;
            }
        }
        break;
    }
    if (eq)
        *eq = s;

    while (s < es && strchr(whitespace, *s))
        s++;
    *ps = s;
    return ret;
}

int peek(char **ps, char *es, char *toks) {
    char *s;
    s = *ps;
    while (s < es && strchr(whitespace, *s))
        s++;
    *ps = s;
    return *s && strchr(toks, *s);
}

struct cmd *parseline(char **, char *);
struct cmd *parsepipe(char **, char *);
struct cmd *parseexec(char **, char *);
struct cmd *nulterminate(struct cmd *);

struct cmd *parsecmd(char *s) {
    char *es;
    struct cmd *cmd;

    es = s + strlen(s);
    cmd = parseline(&s, es);
    peek(&s, es, "");
    if (s != es) {
        errprintf("syntax error near: %s\n", s);
        return 0;
    }
    nulterminate(cmd);
    return cmd;
}

struct cmd *parseline(char **ps, char *es) {
    struct cmd *cmd;
    cmd = parsepipe(ps, es);
    while (peek(ps, es, "&")) {
        gettoken(ps, es, 0, 0);
        cmd = backcmd(cmd);
        if (*ps < es && !peek(ps, es, ";&")) {
            cmd = listcmd(cmd, parseline(ps, es));
            return cmd;
        }
    }
    if (peek(ps, es, ";")) {
        gettoken(ps, es, 0, 0);
        cmd = listcmd(cmd, parseline(ps, es));
    }
    return cmd;
}

struct cmd *parsepipe(char **ps, char *es) {
    struct cmd *cmd;
    cmd = parseexec(ps, es);
    if (peek(ps, es, "|")) {
        gettoken(ps, es, 0, 0);
        cmd = pipecmd(cmd, parsepipe(ps, es));
    }
    return cmd;
}

struct cmd *parseredirs(struct cmd *cmd, char **ps, char *es) {
    int tok;
    char *q, *eq;

    while (peek(ps, es, "<>")) {
        tok = gettoken(ps, es, 0, 0);
        if (gettoken(ps, es, &q, &eq) != 'a')
            panic("missing file for redirection");
        switch (tok) {
        case '<':
            cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
            break;
        case '>':
            cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREAT | O_TRUNC, 1);
            break;
        case '+':
            cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREAT | O_APPEND, 1);
            break;
        }
    }
    return cmd;
}

struct cmd *parseblock(char **ps, char *es) {
    struct cmd *cmd;

    if (!peek(ps, es, "("))
        panic("parseblock");
    gettoken(ps, es, 0, 0);
    cmd = parseline(ps, es);
    if (!peek(ps, es, ")"))
        panic("syntax - missing )");
    gettoken(ps, es, 0, 0);
    cmd = parseredirs(cmd, ps, es);
    return cmd;
}

struct cmd *parseexec(char **ps, char *es) {
    char *q, *eq;
    int tok, argc;
    struct execcmd *cmd;
    struct cmd *ret;

    if (peek(ps, es, "("))
        return parseblock(ps, es);

    ret = execcmd();
    cmd = (struct execcmd *)ret;

    argc = 0;
    ret = parseredirs(ret, ps, es);
    while (!peek(ps, es, "|)&;")) {
        if ((tok = gettoken(ps, es, &q, &eq)) == 0)
            break;
        if (tok != 'a')
            panic("syntax");
        cmd->argv[argc] = q;
        cmd->eargv[argc] = eq;
        argc++;
        if (argc >= MAXARGS)
            panic("too many args");
        ret = parseredirs(ret, ps, es);
    }
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;
    return ret;
}

/*
 * dequote - strip quotes and process backslash escapes in a token, in-place.
 *
 * Rules:
 *   Outside quotes:  \ escapes the next character.
 *   Inside "...":   \" → "   \\ → \   all other \ are literal.
 *                    Single quotes inside are literal characters.
 *   Inside '...':   \' → '   \\ → \   all other \ are literal.
 *                    Double quotes inside are literal characters.
 *
 * Examples:
 *   "hello world"        →  hello world
 *   'hello world'        →  hello world
 *   hello\ world         →  hello world
 *   "say \"hi\""         →  say "hi"
 *   "it's fine"          →  it's fine
 *   'he said "hello"'    →  he said "hello"
 *   'can\'t stop'        →  can't stop
 */
static void dequote(char *s) {
    char *dst = s;
    while (*s) {
        if (*s == '\\' && s[1] != '\0') {
            /* Backslash escape outside quotes — copy next char literally */
            s++;
            *dst++ = *s++;
        } else if (*s == '\'') {
            /* Single-quoted region — copy until unescaped closing '.
             * Double quotes and most backslashes are literal.
             * Only \' and \\ are escape sequences. */
            s++;
            while (*s && *s != '\'') {
                if (*s == '\\' && s[1] == '\'') {
                    s++;            /* skip backslash */
                    *dst++ = *s++;  /* copy the literal ' */
                } else if (*s == '\\' && s[1] == '\\') {
                    s++;            /* skip first backslash */
                    *dst++ = *s++;  /* copy the second as literal \ */
                } else {
                    *dst++ = *s++;  /* everything else is literal */
                }
            }
            if (*s == '\'')
                s++;
        } else if (*s == '"') {
            /* Double-quoted region — copy until unescaped closing ".
             * Single quotes are literal characters.
             * Only \" and \\ are escape sequences. */
            s++;
            while (*s && *s != '"') {
                if (*s == '\\' && s[1] == '"') {
                    s++;            /* skip backslash */
                    *dst++ = *s++;  /* copy the literal " */
                } else if (*s == '\\' && s[1] == '\\') {
                    s++;            /* skip first backslash */
                    *dst++ = *s++;  /* copy the second as literal \ */
                } else {
                    *dst++ = *s++;  /* everything else is literal (incl. \n etc.) */
                }
            }
            if (*s == '"')
                s++;
        } else {
            *dst++ = *s++;
        }
    }
    *dst = '\0';
}

struct cmd *nulterminate(struct cmd *cmd) {
    int i;
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if (cmd == 0)
        return 0;

    switch (cmd->type) {
    case EXEC:
        ecmd = (struct execcmd *)cmd;
        for (i = 0; ecmd->argv[i]; i++) {
            *ecmd->eargv[i] = 0;
            dequote(ecmd->argv[i]);
        }
        break;
    case REDIR:
        rcmd = (struct redircmd *)cmd;
        nulterminate(rcmd->cmd);
        *rcmd->efile = 0;
        dequote(rcmd->file);
        break;
    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        nulterminate(pcmd->left);
        nulterminate(pcmd->right);
        break;
    case LIST:
        lcmd = (struct listcmd *)cmd;
        nulterminate(lcmd->left);
        nulterminate(lcmd->right);
        break;
    case BACK:
        bcmd = (struct backcmd *)cmd;
        nulterminate(bcmd->cmd);
        break;
    }
    return cmd;
}

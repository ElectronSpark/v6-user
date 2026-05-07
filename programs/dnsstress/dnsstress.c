#include "kernel/inc/syscall.h"
#include "kernel/inc/types.h"
#include "kernel/inc/uabi/poll.h"
#include "user/user.h"

#define AF_INET 2
#define SOCK_DGRAM 2
#define SOCK_CLOEXEC 0x80000
#define MSG_DONTWAIT 0x40
#define EAGAIN 11
#define DNS_PORT 53

enum dns_fail_reason {
    DNS_FAIL_NONE = 0,
    DNS_FAIL_BUILD,
    DNS_FAIL_SOCKET,
    DNS_FAIL_SERVER,
    DNS_FAIL_SEND4,
    DNS_FAIL_SEND6,
    DNS_FAIL_POLL,
    DNS_FAIL_RECV,
    DNS_FAIL_BADSRC,
    DNS_FAIL_BAD4,
    DNS_FAIL_TIMEOUT4,
};

struct dns_result {
    int reason;
    int detail;
    int got4;
    int got6;
    uint32 bad_src_addr;
    uint16 bad_src_port;
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct sockaddr_in {
    uint16 sin_family;
    uint16 sin_port;
    uint32 sin_addr;
    char sin_zero[8];
};

#if defined(__x86_64__)
static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_syscall6(int num, int64 a, int64 b, int64 c,
                                 int64 d, int64 e, int64 f)
{
    int64 ret;
    register int64 r10 asm("r10") = d;
    register int64 r8 asm("r8") = e;
    register int64 r9 asm("r9") = f;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}
#else
static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_syscall6(int num, int64 a, int64 b, int64 c,
                                 int64 d, int64 e, int64 f)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    register int64 a3 asm("a3") = d;
    register int64 a4 asm("a4") = e;
    register int64 a5 asm("a5") = f;
    asm volatile("ecall" : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                 : "memory");
    return a0;
}
#endif

static int socket_raw(int domain, int type, int protocol)
{
    return (int)raw_syscall3(SYS_socket, domain, type, protocol);
}

static int poll_raw(struct pollfd *fds, int nfds, int timeout)
{
    return (int)raw_syscall3(SYS_poll, (int64)fds, nfds, timeout);
}

static int sendto_raw(int fd, const void *buf, int len, int flags,
                      const struct sockaddr_in *sa)
{
    return (int)raw_syscall6(SYS_sendto, fd, (int64)buf, len, flags,
                             (int64)sa, sizeof(*sa));
}

static int recvfrom_raw(int fd, void *buf, int len, int flags,
                        struct sockaddr_in *sa)
{
    int alen = sizeof(*sa);
    return (int)raw_syscall6(SYS_recvfrom, fd, (int64)buf, len, flags,
                             (int64)sa, (int64)&alen);
}

static uint16 bswap16(uint16 x)
{
    return (uint16)((x << 8) | (x >> 8));
}

static uint16 get16(const uchar *p)
{
    return ((uint16)p[0] << 8) | p[1];
}

static uint32 parse_ipv4(const char *s)
{
    int parts[4] = {0, 0, 0, 0};
    int idx = 0;
    int value = 0;

    for (int i = 0;; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (value > 255)
                return 0;
        } else if (c == '.' || c == 0) {
            if (idx >= 4)
                return 0;
            parts[idx++] = value;
            value = 0;
            if (c == 0)
                break;
        } else {
            return 0;
        }
    }
    if (idx != 4)
        return 0;
    return ((uint32)parts[0] << 0) |
           ((uint32)parts[1] << 8) |
           ((uint32)parts[2] << 16) |
           ((uint32)parts[3] << 24);
}

static int put_qname(uchar *buf, int off, int cap, const char *host)
{
    int label_start = off++;
    int label_len = 0;

    for (int i = 0;; i++) {
        char c = host[i];
        if (c == '.' || c == 0) {
            if (label_len == 0 || label_len > 63 || label_start >= cap)
                return -1;
            buf[label_start] = (uchar)label_len;
            if (c == 0) {
                if (off >= cap)
                    return -1;
                buf[off++] = 0;
                return off;
            }
            label_start = off++;
            label_len = 0;
        } else {
            if (off >= cap)
                return -1;
            buf[off++] = (uchar)c;
            label_len++;
        }
    }
}

static int make_query(uchar *buf, int cap, uint16 id, const char *host,
                      uint16 qtype)
{
    memset(buf, 0, cap);
    buf[0] = (uchar)(id >> 8);
    buf[1] = (uchar)id;
    buf[2] = 0x01; /* recursion desired */
    buf[5] = 0x01; /* one question */

    int off = put_qname(buf, 12, cap, host);
    if (off < 0 || off + 4 > cap)
        return -1;
    buf[off++] = (uchar)(qtype >> 8);
    buf[off++] = (uchar)qtype;
    buf[off++] = 0;
    buf[off++] = 1; /* IN */
    return off;
}

static int skip_name(const uchar *buf, int len, int off)
{
    for (;;) {
        if (off >= len)
            return -1;
        uchar n = buf[off++];
        if (n == 0)
            return off;
        if ((n & 0xc0) == 0xc0) {
            if (off >= len)
                return -1;
            return off + 1;
        }
        if ((n & 0xc0) != 0 || off + n > len)
            return -1;
        off += n;
    }
}

static int response_usable(const uchar *buf, int len, uint16 id, uint16 qtype)
{
    if (len < 12 || get16(buf) != id)
        return -1;
    if ((buf[3] & 0x0f) != 0)
        return 0;

    int qd = get16(buf + 4);
    int an = get16(buf + 6);
    int off = 12;
    for (int i = 0; i < qd; i++) {
        off = skip_name(buf, len, off);
        if (off < 0 || off + 4 > len)
            return -1;
        off += 4;
    }

    for (int i = 0; i < an; i++) {
        off = skip_name(buf, len, off);
        if (off < 0 || off + 10 > len)
            return -1;
        uint16 typ = get16(buf + off);
        uint16 cls = get16(buf + off + 2);
        uint16 rdlen = get16(buf + off + 8);
        off += 10;
        if (off + rdlen > len)
            return -1;
        if (cls == 1 && typ == qtype &&
            ((qtype == 1 && rdlen == 4) || (qtype == 28 && rdlen == 16)))
            return 1;
        off += rdlen;
    }
    return 0;
}

static void dns_result_init(struct dns_result *res)
{
    if (res == NULL)
        return;
    memset(res, 0, sizeof(*res));
}

static const char *dns_fail_reason_name(int reason)
{
    switch (reason) {
    case DNS_FAIL_BUILD:    return "build";
    case DNS_FAIL_SOCKET:   return "socket";
    case DNS_FAIL_SERVER:   return "server";
    case DNS_FAIL_SEND4:    return "send4";
    case DNS_FAIL_SEND6:    return "send6";
    case DNS_FAIL_POLL:     return "poll";
    case DNS_FAIL_RECV:     return "recv";
    case DNS_FAIL_BADSRC:   return "badsrc";
    case DNS_FAIL_BAD4:     return "bad4";
    case DNS_FAIL_TIMEOUT4: return "timeout4";
    default:                return "unknown";
    }
}

static int query_pair(const char *server, const char *host, int iter,
                      struct dns_result *res)
{
    uchar q4[512], q6[512], reply[1536];
    struct sockaddr_in dst, src;
    uint16 id4 = (uint16)(0x4000 + (getpid() * 37 + iter * 2));
    uint16 id6 = (uint16)(id4 + 1);
    int q4len = make_query(q4, sizeof(q4), id4, host, 1);
    int q6len = make_query(q6, sizeof(q6), id6, host, 28);
    int got4 = 0;
    int got6 = 0;
    int fd;

    dns_result_init(res);

    if (q4len < 0 || q6len < 0) {
        if (res != NULL)
            res->reason = DNS_FAIL_BUILD;
        return -1;
    }
    fd = socket_raw(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (res != NULL) {
            res->reason = DNS_FAIL_SOCKET;
            res->detail = fd;
        }
        return -1;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = bswap16(DNS_PORT);
    dst.sin_addr = parse_ipv4(server);
    if (dst.sin_addr == 0) {
        if (res != NULL)
            res->reason = DNS_FAIL_SERVER;
        close(fd);
        return -1;
    }

    int sn = sendto_raw(fd, q4, q4len, 0, &dst);
    if (sn != q4len) {
        if (res != NULL) {
            res->reason = DNS_FAIL_SEND4;
            res->detail = sn;
        }
        close(fd);
        return -1;
    }
    sn = sendto_raw(fd, q6, q6len, 0, &dst);
    if (sn != q6len) {
        if (res != NULL) {
            res->reason = DNS_FAIL_SEND6;
            res->detail = sn;
        }
        close(fd);
        return -1;
    }

    for (int wait = 0; wait < 8 && (!got4 || !got6); wait++) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int prc = poll_raw(&pfd, 1, 1000);
        if (prc < 0) {
            if (res != NULL) {
                res->reason = DNS_FAIL_POLL;
                res->detail = prc;
            }
            close(fd);
            return -1;
        }
        if (prc == 0)
            continue;
        for (;;) {
            int n;
            memset(&src, 0, sizeof(src));
            n = recvfrom_raw(fd, reply, sizeof(reply), MSG_DONTWAIT, &src);
            if (n == -EAGAIN)
                break;
            if (n <= 0) {
                if (res != NULL) {
                    res->reason = DNS_FAIL_RECV;
                    res->detail = n;
                }
                close(fd);
                return -1;
            }
            if (src.sin_addr != dst.sin_addr || src.sin_port != dst.sin_port) {
                if (res != NULL) {
                    res->reason = DNS_FAIL_BADSRC;
                    res->bad_src_addr = src.sin_addr;
                    res->bad_src_port = src.sin_port;
                }
                continue;
            }
            if (get16(reply) == id4) {
                int ok = response_usable(reply, n, id4, 1);
                if (ok > 0)
                    got4 = 1;
                else if (ok < 0) {
                    if (res != NULL) {
                        res->reason = DNS_FAIL_BAD4;
                        res->detail = n;
                    }
                    close(fd);
                    return -1;
                } else {
                    got4 = -1;
                }
            } else if (get16(reply) == id6) {
                int ok = response_usable(reply, n, id6, 28);
                got6 = ok > 0 ? 1 : -1;
            }
        }
    }

    close(fd);
    if (got4 == 1)
        return 0;
    if (res != NULL) {
        if (res->reason == DNS_FAIL_NONE)
            res->reason = DNS_FAIL_TIMEOUT4;
        res->got4 = got4;
        res->got6 = got6;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "www.youtube.com";
    const char *server = argc > 2 ? argv[2] : "10.0.2.3";
    int iterations = argc > 3 ? atoi(argv[3]) : 30;
    int parallel = argc > 4 ? atoi(argv[4]) : 6;
    int failed = 0;

    if (iterations < 1)
        iterations = 1;
    if (parallel < 1)
        parallel = 1;
    if (parallel > 16)
        parallel = 16;

    printf("dnsstress: host=%s server=%s iterations=%d parallel=%d\n",
           host, server, iterations, parallel);

    for (int i = 0; i < parallel; i++) {
        int pid = fork();
        if (pid < 0)
            exit(1);
        if (pid == 0) {
            int child_fail = 0;
            for (int j = 0; j < iterations; j++) {
                struct dns_result res;
                if (query_pair(server, host, j, &res) != 0) {
                    printf("dnsstress: child=%d iter=%d fail reason=%s "
                           "detail=%d got4=%d got6=%d badsrc=%08x:%04x\n",
                           i, j, dns_fail_reason_name(res.reason),
                           res.detail, res.got4, res.got6,
                           res.bad_src_addr, res.bad_src_port);
                    child_fail++;
                }
            }
            exit(child_fail == 0 ? 0 : 1);
        }
    }

    for (int i = 0; i < parallel; i++) {
        int status = 0;
        if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            failed++;
    }

    printf("dnsstress: RESULT %s failed_children=%d\n",
           failed == 0 ? "pass" : "fail", failed);
    exit(failed == 0 ? 0 : 1);
}

/*
 * tcpstress — guest-side TCP concurrency stress test.
 *
 * Spawns N parallel forked clients that each open one TCP connection to a
 * host server (default 10.0.2.2:5001 — SLIRP host gateway), request a
 * deterministic payload, and validate every byte of the response using the
 * same xorshift32 generator the host uses (see scripts/tcpstress-host-server.py).
 *
 * Each client uses recvmmsg() (a multi-iovec single syscall) for every read,
 * so the run exercises the lwIP recvmmsg STREAM path that the WebKit network
 * process hits during a real browse.  Optional pacing on the host side
 * (--delay-ms) keeps connections open long enough to expose receive-window
 * starvation, futex/condvar wakeup races, and SMP scheduler bugs without
 * needing to drive WebKit.
 *
 * Usage:
 *   tcpstress [host_ipv4] [port] [parallel] [payload_size] [iters_per_child]
 * Defaults: 10.0.2.2 5001 8 1048576 1
 *
 * Exits 0 only if every client validated every byte.  Prints a one-line
 * summary "tcpstress: ok parallel=N total_bytes=B elapsed_ms=T" on success.
 */

#include "kernel/inc/syscall.h"
#include "kernel/inc/types.h"
#include "kernel/inc/uabi/poll.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

#define AF_INET         2
#define SOCK_STREAM     1
#define SOCK_CLOEXEC    0x80000
#define SOL_SOCKET      1
#define SO_ERROR        4
#define MSG_DONTWAIT    0x40
#define MSG_WAITFORONE  0x10000

#define REQ_MAGIC 0xCA11AB1Eu
#define RSP_MAGIC 0x10AD0FABu

#define DEFAULT_HOST     "10.0.2.2"
#define DEFAULT_PORT     5001
#define DEFAULT_PARALLEL 8
#define DEFAULT_PAYLOAD  (1 * 1024 * 1024)
#define DEFAULT_ITERS    1
#define MAX_PARALLEL     64

struct sockaddr_in {
    uint16 sin_family;
    uint16 sin_port;
    uint32 sin_addr;
    char   sin_zero[8];
};

struct msghdr {
    void   *msg_name;
    uint32  msg_namelen;
    uint32  __pad0;
    struct iovec *msg_iov;
    int     msg_iovlen;
    int     __pad1;
    void   *msg_control;
    uint32  msg_controllen;
    uint32  __pad2;
    int     msg_flags;
};

struct mmsghdr {
    struct msghdr msg_hdr;
    uint32 msg_len;
    uint32 __pad;
};

#if defined(__riscv)
static inline int64 raw_syscall2(int n, int64 a, int64 b) {
    register int64 a7 asm("a7") = n;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}
static inline int64 raw_syscall3(int n, int64 a, int64 b, int64 c) {
    register int64 a7 asm("a7") = n;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
static inline int64 raw_syscall5(int n, int64 a, int64 b, int64 c, int64 d, int64 e) {
    register int64 a7 asm("a7") = n;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    register int64 a3 asm("a3") = d;
    register int64 a4 asm("a4") = e;
    asm volatile("ecall" : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
                 : "memory");
    return a0;
}
static inline int64 raw_syscall6(int n, int64 a, int64 b, int64 c,
                                 int64 d, int64 e, int64 f) {
    register int64 a7 asm("a7") = n;
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
#elif defined(__x86_64__)
static inline int64 raw_syscall2(int n, int64 a, int64 b) {
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n), "D"(a), "S"(b)
                 : "rcx", "r11", "memory");
    return ret;
}
static inline int64 raw_syscall3(int n, int64 a, int64 b, int64 c) {
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return ret;
}
static inline int64 raw_syscall5(int n, int64 a, int64 b, int64 c, int64 d, int64 e) {
    int64 ret;
    register int64 r10 asm("r10") = d;
    register int64 r8  asm("r8")  = e;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return ret;
}
static inline int64 raw_syscall6(int n, int64 a, int64 b, int64 c,
                                 int64 d, int64 e, int64 f) {
    int64 ret;
    register int64 r10 asm("r10") = d;
    register int64 r8  asm("r8")  = e;
    register int64 r9  asm("r9")  = f;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)n), "D"(a), "S"(b), "d"(c),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}
#endif

static int socket_raw(int domain, int type, int proto) {
    return (int)raw_syscall3(SYS_socket, domain, type, proto);
}
static int connect_raw(int fd, const struct sockaddr_in *sa) {
    return (int)raw_syscall3(SYS_connect, fd, (int64)sa, sizeof(*sa));
}
static int sendto_raw(int fd, const void *buf, int len, int flags) {
    return (int)raw_syscall6(SYS_sendto, fd, (int64)buf, len, flags, 0, 0);
}
static int recvmmsg_raw(int fd, struct mmsghdr *vec, int vlen, int flags) {
    return (int)raw_syscall5(SYS_recvmmsg, fd, (int64)vec, vlen, flags, 0);
}

static uint16 bswap16(uint16 x) { return (uint16)((x << 8) | (x >> 8)); }

static uint32 parse_ipv4(const char *s) {
    int parts[4] = {0};
    int idx = 0;
    int v = 0;
    for (int i = 0;; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            if (v > 255) return 0;
        } else if (c == '.' || c == 0) {
            if (idx >= 4) return 0;
            parts[idx++] = v; v = 0;
            if (c == 0) break;
        } else {
            return 0;
        }
    }
    if (idx != 4) return 0;
    return ((uint32)parts[0])       |
           ((uint32)parts[1] << 8)  |
           ((uint32)parts[2] << 16) |
           ((uint32)parts[3] << 24);
}

/* Same xorshift32 as scripts/tcpstress-host-server.py:make_pattern.
 * Stream-friendly: tracks a partial 4-byte word across calls so the kernel
 * can deliver bytes in arbitrarily-sized recv chunks. */
struct vstream {
    uint32 state;   /* xorshift32 state after the most-recently expanded word */
    uint8  word[4]; /* the most-recently expanded want-bytes */
    uint8  pos;     /* 0..4 — bytes already consumed from word */
};

static void vstream_init(struct vstream *v, uint32 seed) {
    v->state = (seed == 0) ? 0xDEADBEEFu : seed;
    /* Force a refill on first byte: pos == 4 means "need next word". */
    v->pos = 4;
    v->word[0] = v->word[1] = v->word[2] = v->word[3] = 0;
}

static int validate_pattern(const uint8 *buf, uint64 size, uint32 seed,
                            uint64 base_off, struct vstream *v)
{
    (void)seed;
    for (uint64 i = 0; i < size; i++) {
        if (v->pos == 4) {
            v->state ^= (v->state << 13);
            v->state ^= (v->state >> 17);
            v->state ^= (v->state << 5);
            v->word[0] = (uint8)(v->state);
            v->word[1] = (uint8)(v->state >> 8);
            v->word[2] = (uint8)(v->state >> 16);
            v->word[3] = (uint8)(v->state >> 24);
            v->pos = 0;
        }
        if (buf[i] != v->word[v->pos]) {
            printf("tcpstress: MISMATCH at offset %lu got=0x%x want=0x%x\n",
                   (unsigned long)(base_off + i), buf[i], v->word[v->pos]);
            return -1;
        }
        v->pos++;
    }
    return 0;
}

static int64 now_ms(void) {
    struct timeval tv = {0};
    if (gettimeofday(&tv, 0) < 0) return 0;
    return (int64)tv.tv_sec * 1000 + (int64)tv.tv_usec / 1000;
}

#define RECV_CHUNK   16384
#define RECV_VEC_LEN 4

static int run_one_iter(uint32 host_addr, int port, uint32 conn_id,
                        uint32 payload, uint32 seed)
{
    int fd = socket_raw(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        printf("tcpstress: socket failed (%d)\n", fd);
        return -1;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = bswap16((uint16)port);
    sa.sin_addr   = host_addr;

    int rc = connect_raw(fd, &sa);
    if (rc < 0) {
        printf("tcpstress: conn_id=%u connect failed (%d)\n", conn_id, rc);
        close(fd);
        return -1;
    }

    uint32 req[4];
    req[0] = REQ_MAGIC;
    req[1] = conn_id;
    req[2] = payload;
    req[3] = seed;
    int sent = sendto_raw(fd, req, sizeof(req), 0);
    if (sent != (int)sizeof(req)) {
        printf("tcpstress: conn_id=%u short send %d\n", conn_id, sent);
        close(fd);
        return -1;
    }

    /* Read response header (16 bytes), then validate the body using one
     * recvmmsg() per loop iteration.  We split each call into RECV_VEC_LEN
     * iovecs of RECV_CHUNK bytes so the kernel's stream recvmmsg loop is
     * actually exercised end-to-end. */
    uint8 hdr[16];
    int got = 0;
    while (got < (int)sizeof(hdr)) {
        struct iovec  iv = { .iov_base = hdr + got, .iov_len = sizeof(hdr) - got };
        struct mmsghdr m = {0};
        m.msg_hdr.msg_iov    = &iv;
        m.msg_hdr.msg_iovlen = 1;
        int n = recvmmsg_raw(fd, &m, 1, MSG_WAITFORONE);
        if (n <= 0 || m.msg_len == 0) {
            printf("tcpstress: conn_id=%u hdr recvmmsg=%d msg_len=%u\n",
                   conn_id, n, m.msg_len);
            close(fd);
            return -1;
        }
        got += (int)m.msg_len;
    }
    uint32 rmagic = (uint32)hdr[0] | ((uint32)hdr[1] << 8) |
                    ((uint32)hdr[2] << 16) | ((uint32)hdr[3] << 24);
    uint32 rconn  = (uint32)hdr[4] | ((uint32)hdr[5] << 8) |
                    ((uint32)hdr[6] << 16) | ((uint32)hdr[7] << 24);
    uint32 rsize  = (uint32)hdr[8] | ((uint32)hdr[9] << 8) |
                    ((uint32)hdr[10] << 16) | ((uint32)hdr[11] << 24);
    uint32 rseed  = (uint32)hdr[12] | ((uint32)hdr[13] << 8) |
                    ((uint32)hdr[14] << 16) | ((uint32)hdr[15] << 24);
    if (rmagic != RSP_MAGIC || rconn != conn_id ||
        rsize != payload || rseed != seed) {
        printf("tcpstress: conn_id=%u bad hdr magic=0x%x conn=%u size=%u seed=0x%x\n",
               conn_id, rmagic, rconn, rsize, rseed);
        close(fd);
        return -1;
    }

    static uint8 buf[RECV_VEC_LEN * RECV_CHUNK];
    uint64 received = 0;
    struct vstream vs;
    vstream_init(&vs, seed);

    while (received < payload) {
        struct iovec iv[RECV_VEC_LEN];
        for (int i = 0; i < RECV_VEC_LEN; i++) {
            iv[i].iov_base = buf + i * RECV_CHUNK;
            uint64 remain = payload - received - (uint64)i * RECV_CHUNK;
            if ((int64)remain <= 0) {
                iv[i].iov_len = 0;
            } else {
                iv[i].iov_len = remain > RECV_CHUNK ? RECV_CHUNK : remain;
            }
        }
        struct mmsghdr m = {0};
        m.msg_hdr.msg_iov    = iv;
        m.msg_hdr.msg_iovlen = RECV_VEC_LEN;
        int n = recvmmsg_raw(fd, &m, 1, MSG_WAITFORONE);
        if (n <= 0) {
            printf("tcpstress: conn_id=%u recvmmsg=%d after %lu/%u bytes\n",
                   conn_id, n, (unsigned long)received, payload);
            close(fd);
            return -1;
        }
        if (m.msg_len == 0) {
            printf("tcpstress: conn_id=%u EOF after %lu/%u bytes\n",
                   conn_id, (unsigned long)received, payload);
            close(fd);
            return -1;
        }
        /* Validate each iovec slice that the kernel filled. */
        uint32 left = m.msg_len;
        for (int i = 0; i < RECV_VEC_LEN && left > 0; i++) {
            uint32 take = left < iv[i].iov_len ? left : (uint32)iv[i].iov_len;
            if (take == 0) continue;
            if (validate_pattern((uint8 *)iv[i].iov_base, take, seed,
                                 received, &vs) != 0) {
                printf("tcpstress: conn_id=%u validation FAIL at byte %lu\n",
                       conn_id, (unsigned long)received);
                close(fd);
                return -1;
            }
            received += take;
            left -= take;
        }
    }

    close(fd);
    return 0;
}

static int run_child(uint32 host_addr, int port, uint32 conn_id,
                     uint32 payload, int iters)
{
    for (int it = 0; it < iters; it++) {
        uint32 seed = conn_id * 2654435761u + (uint32)it * 0x9E3779B9u;
        if (run_one_iter(host_addr, port, conn_id, payload, seed) != 0)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : DEFAULT_HOST;
    int port      = argc > 2 ? atoi(argv[2]) : DEFAULT_PORT;
    int parallel  = argc > 3 ? atoi(argv[3]) : DEFAULT_PARALLEL;
    int payload   = argc > 4 ? atoi(argv[4]) : DEFAULT_PAYLOAD;
    int iters     = argc > 5 ? atoi(argv[5]) : DEFAULT_ITERS;

    if (parallel < 1) parallel = 1;
    if (parallel > MAX_PARALLEL) parallel = MAX_PARALLEL;
    if (payload < 16) payload = 16;
    if (iters < 1) iters = 1;

    uint32 host_addr = parse_ipv4(host);
    if (host_addr == 0) {
        printf("tcpstress: invalid host '%s'\n", host);
        exit(1);
    }

    printf("tcpstress: target=%s:%d parallel=%d payload=%d iters=%d\n",
           host, port, parallel, payload, iters);

    int64 t0 = now_ms();

    int pids[MAX_PARALLEL];
    for (int i = 0; i < parallel; i++) {
        int pid = fork();
        if (pid < 0) {
            printf("tcpstress: fork failed at child %d\n", i);
            for (int j = 0; j < i; j++)
                kill(pids[j], 9);
            exit(1);
        }
        if (pid == 0) {
            uint32 conn_id = (uint32)((getpid() << 8) ^ (i + 1));
            int rc = run_child(host_addr, port, conn_id,
                               (uint32)payload, iters);
            exit(rc);
        }
        pids[i] = pid;
    }

    int failed = 0;
    for (int i = 0; i < parallel; i++) {
        int status = 0;
        int wp = waitpid(pids[i], &status, 0);
        if (wp < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("tcpstress: child %d (pid=%d) FAILED status=0x%x\n",
                   i, pids[i], status);
            failed++;
        }
    }

    int64 elapsed = now_ms() - t0;
    uint64 total_bytes = (uint64)parallel * (uint64)iters * (uint64)payload;

    if (failed != 0) {
        printf("tcpstress: FAIL parallel=%d failed=%d elapsed_ms=%ld\n",
               parallel, failed, (long)elapsed);
        exit(2);
    }
    printf("tcpstress: ok parallel=%d total_bytes=%lu elapsed_ms=%ld\n",
           parallel, (unsigned long)total_bytes, (long)elapsed);
    exit(0);
}

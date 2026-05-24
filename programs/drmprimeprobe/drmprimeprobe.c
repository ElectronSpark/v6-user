#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/syscall.h"
#include "kernel/inc/uabi/drm.h"
#include "kernel/inc/uabi/poll.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

#define DRMPRIMEPROBE_TEST_MOD_NONLINEAR 0x0100000000000001ULL
#define DRMPRIMEPROBE_PAGE_SIZE 4096

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define DRMPRIMEPROBE_AF_UNIX 1
#define DRMPRIMEPROBE_SOCK_DGRAM 2
#define DRMPRIMEPROBE_SOL_SOCKET 1
#define DRMPRIMEPROBE_SCM_RIGHTS 1
#define DRMPRIMEPROBE_MSG_CMSG_CLOEXEC 0x40000000

struct cmsghdr {
    uint32 cmsg_len;
    uint32 __pad1;
    int cmsg_level;
    int cmsg_type;
};

struct msghdr {
    void *msg_name;
    uint32 msg_namelen;
    uint32 __pad0;
    struct iovec *msg_iov;
    int msg_iovlen;
    int __pad1;
    void *msg_control;
    uint32 msg_controllen;
    uint32 __pad2;
    int msg_flags;
};

#define CMSG_ALIGN(n) (((n) + sizeof(uint64) - 1) & ~(sizeof(uint64) - 1))
#define CMSG_LEN(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_SPACE(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(n))
#define CMSG_DATA(cmsg) ((unsigned char *)((struct cmsghdr *)(cmsg) + 1))

#if defined(__riscv)
static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int64 raw_syscall4(int num, int64 a, int64 b, int64 c, int64 d)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    register int64 a3 asm("a3") = d;
    asm volatile("ecall" : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}
#elif defined(__x86_64__)
static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline int64 raw_syscall4(int num, int64 a, int64 b, int64 c, int64 d)
{
    int64 ret;
    register int64 r10 asm("r10") = d;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}
#else
#error "raw_syscall3 is not defined for this architecture"
#endif

static int poll_raw(struct pollfd *fds, int nfds, int timeout)
{
    return (int)raw_syscall3(SYS_poll, (int64)fds, nfds, timeout);
}

static int socketpair_raw(int sv[2])
{
    return (int)raw_syscall4(SYS_socketpair, DRMPRIMEPROBE_AF_UNIX,
                             DRMPRIMEPROBE_SOCK_DGRAM, 0, (int64)sv);
}

static int sendmsg_raw(int fd, struct msghdr *msg, int flags)
{
    return (int)raw_syscall3(SYS_sendmsg, fd, (int64)msg, flags);
}

static int recvmsg_raw(int fd, struct msghdr *msg, int flags)
{
    return (int)raw_syscall3(SYS_recvmsg, fd, (int64)msg, flags);
}

static void *readonly_arg_page(const void *src, int size)
{
    char *page;

    if (size <= 0 || size > DRMPRIMEPROBE_PAGE_SIZE)
        return MAP_FAILED;
    page = mmap(0, DRMPRIMEPROBE_PAGE_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return MAP_FAILED;
    memset(page, 0, DRMPRIMEPROBE_PAGE_SIZE);
    memmove(page, src, size);
    if (mprotect(page, DRMPRIMEPROBE_PAGE_SIZE, PROT_READ) < 0) {
        munmap(page, DRMPRIMEPROBE_PAGE_SIZE);
        return MAP_FAILED;
    }
    return page;
}

static void free_arg_page(void *page)
{
    if (page == MAP_FAILED || page == 0)
        return;
    (void)mprotect(page, DRMPRIMEPROBE_PAGE_SIZE,
                   PROT_READ | PROT_WRITE);
    munmap(page, DRMPRIMEPROBE_PAGE_SIZE);
}

struct kms_credit_snapshot {
    uint64 bo_presents;
    uint64 kms_page_flips;
    uint64 kms_atomic_commits;
    uint64 display_presented;
    uint64 display_completed;
    uint64 gpu_backend_flags;
    uint64 dxg_present_register_successes;
    uint64 dxg_present_commit_attempts;
    uint64 dxg_present_commit_rejects;
    uint64 dxg_present_display_target_kind;
    uint64 dxg_present_dda_nouveau_import_path_present;
    uint64 dxg_present_dda_nouveau_scanout_bind_present;
    uint64 dxg_present_helper_transport_present;
    uint64 bo_fd_live;
    uint64 dmabuf_exports;
    uint64 dmabuf_imports;
    uint64 dmabuf_attachments;
    uint64 dmabuf_live_attachments;
    uint64 dmabuf_live;
    uint64 dmabuf_releases;
    uint64 dmabuf_bad_fd_rejects;
    uint64 dmabuf_foreign_fd_rejects;
    uint64 dmabuf_resv_snapshots;
    uint64 dmabuf_last_exporter_tag;
    uint64 dmabuf_last_importer_tag;
    uint64 dmabuf_last_ttm_resv_seq;
    uint64 dmabuf_last_ttm_resv_exclusive_fence;
    uint64 dmabuf_poll_semantics;
    uint64 dmabuf_poll_attempts;
    uint64 dmabuf_poll_ready;
    uint64 dmabuf_poll_not_ready;
    uint64 dmabuf_poll_errors;
    uint64 dmabuf_poll_last_target_fence;
    uint64 dmabuf_poll_last_signaled_fence;
    uint64 dmabuf_poll_read_ready;
    uint64 dmabuf_poll_write_ready;
    uint64 dmabuf_poll_pending;
    uint64 dmabuf_poll_last_read_fence;
    uint64 dmabuf_poll_last_write_fence;
    uint64 dmabuf_poll_real_fence_ready;
    uint64 dmabuf_poll_callbacks_armed;
    uint64 dmabuf_poll_callbacks_fired;
    uint64 dmabuf_poll_pending_to_ready;
    uint64 dmabuf_poll_last_callback_source;
    uint64 dmabuf_poll_last_callback_target_fence;
    uint64 dmabuf_poll_last_callback_wakeup_seq;
    uint64 dmabuf_shared_fence_semantics;
    uint64 dmabuf_wait_queue_semantics;
    uint64 dmabuf_last_ttm_resv_shared_fence;
    uint64 dmabuf_last_ttm_resv_shared_count;
    uint64 bo_fence_waits;
    uint64 fence_fd_exports;
    uint64 fence_fd_queries;
    uint64 fence_fd_live;
    uint64 fence_fd_polls;
    uint64 fence_fd_poll_ready;
    uint64 ttm_resv_shared_slots;
    uint64 ttm_resv_shared_used;
    uint64 ttm_resv_shared_fences;
    uint64 ttm_resv_wait_queued;
    uint64 ttm_resv_wait_wakeups;
    uint64 ttm_resv_stale_fence_rejects;
    uint64 ttm_resv_attach_prime_export;
    uint64 ttm_resv_attach_prime_import;
    uint64 ttm_resv_attach_dmabuf_export;
    uint64 ttm_resv_attach_dmabuf_import;
    uint64 ttm_resv_attach_kms_pin;
    uint64 ttm_resv_attach_kms_unpin;
    uint64 ttm_resv_last_attach_point;
    uint64 ttm_resv_last_shared_fence;
};

static int check_prime_cap(int fd)
{
    struct drm_get_cap_compat cap = {
        .capability = DRM_CAP_PRIME,
    };

    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0) {
        printf("drmprimeprobe: DRM_CAP_PRIME failed\n");
        return 1;
    }
    if ((cap.value & (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) !=
        (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) {
        printf("drmprimeprobe: PRIME cap missing value=0x%lx\n", cap.value);
        return 1;
    }
    printf("drmprimeprobe: prime cap value=0x%lx\n", cap.value);
    return 0;
}

static int open_gpu_stats_node(void)
{
    int fd = open("/dev/fb0", O_RDONLY);

    if (fd < 0)
        fd = open("/dev/gpu0", O_RDONLY);
    return fd;
}

static int read_credit_snapshot(struct kms_credit_snapshot *snap)
{
    struct fb_gpu_stats stats;
    struct fb_gpu_display_wait display;
    int fd;

    memset(snap, 0, sizeof(*snap));
    fd = open_gpu_stats_node();
    if (fd < 0)
        return -1;
    memset(&stats, 0, sizeof(stats));
    memset(&display, 0, sizeof(display));
    if (ioctl(fd, FB_GPU_GET_STATS, &stats) < 0 ||
        ioctl(fd, FB_GPU_DISPLAY_WAIT, &display) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    snap->bo_presents = stats.bo_presents;
    snap->kms_page_flips = stats.kms_page_flips;
    snap->kms_atomic_commits = stats.kms_atomic_commits;
    snap->display_presented = display.presented;
    snap->display_completed = display.completed;
    snap->gpu_backend_flags = stats.gpu_backend_flags;
    snap->dxg_present_register_successes =
        stats.dxg_present_register_successes;
    snap->dxg_present_commit_attempts = stats.dxg_present_commit_attempts;
    snap->dxg_present_commit_rejects = stats.dxg_present_commit_rejects;
    snap->dxg_present_display_target_kind =
        stats.dxg_present_display_target_kind;
    snap->dxg_present_dda_nouveau_import_path_present =
        stats.dxg_present_dda_nouveau_import_path_present;
    snap->dxg_present_dda_nouveau_scanout_bind_present =
        stats.dxg_present_dda_nouveau_scanout_bind_present;
    snap->dxg_present_helper_transport_present =
        stats.dxg_present_helper_transport_present;
    snap->bo_fd_live = stats.bo_fd_live;
    snap->dmabuf_exports = stats.dmabuf_exports;
    snap->dmabuf_imports = stats.dmabuf_imports;
    snap->dmabuf_attachments = stats.dmabuf_attachments;
    snap->dmabuf_live_attachments = stats.dmabuf_live_attachments;
    snap->dmabuf_live = stats.dmabuf_live;
    snap->dmabuf_releases = stats.dmabuf_releases;
    snap->dmabuf_bad_fd_rejects = stats.dmabuf_bad_fd_rejects;
    snap->dmabuf_foreign_fd_rejects = stats.dmabuf_foreign_fd_rejects;
    snap->dmabuf_resv_snapshots = stats.dmabuf_resv_snapshots;
    snap->dmabuf_last_exporter_tag = stats.dmabuf_last_exporter_tag;
    snap->dmabuf_last_importer_tag = stats.dmabuf_last_importer_tag;
    snap->dmabuf_last_ttm_resv_seq = stats.dmabuf_last_ttm_resv_seq;
    snap->dmabuf_last_ttm_resv_exclusive_fence =
        stats.dmabuf_last_ttm_resv_exclusive_fence;
    snap->dmabuf_poll_semantics = stats.dmabuf_poll_semantics;
    snap->dmabuf_poll_attempts = stats.dmabuf_poll_attempts;
    snap->dmabuf_poll_ready = stats.dmabuf_poll_ready;
    snap->dmabuf_poll_not_ready = stats.dmabuf_poll_not_ready;
    snap->dmabuf_poll_errors = stats.dmabuf_poll_errors;
    snap->dmabuf_poll_last_target_fence =
        stats.dmabuf_poll_last_target_fence;
    snap->dmabuf_poll_last_signaled_fence =
        stats.dmabuf_poll_last_signaled_fence;
    snap->dmabuf_poll_read_ready = stats.dmabuf_poll_read_ready;
    snap->dmabuf_poll_write_ready = stats.dmabuf_poll_write_ready;
    snap->dmabuf_poll_pending = stats.dmabuf_poll_pending;
    snap->dmabuf_poll_last_read_fence =
        stats.dmabuf_poll_last_read_fence;
    snap->dmabuf_poll_last_write_fence =
        stats.dmabuf_poll_last_write_fence;
    snap->dmabuf_poll_real_fence_ready =
        stats.dmabuf_poll_real_fence_ready;
    snap->dmabuf_poll_callbacks_armed =
        stats.dmabuf_poll_callbacks_armed;
    snap->dmabuf_poll_callbacks_fired =
        stats.dmabuf_poll_callbacks_fired;
    snap->dmabuf_poll_pending_to_ready =
        stats.dmabuf_poll_pending_to_ready;
    snap->dmabuf_poll_last_callback_source =
        stats.dmabuf_poll_last_callback_source;
    snap->dmabuf_poll_last_callback_target_fence =
        stats.dmabuf_poll_last_callback_target_fence;
    snap->dmabuf_poll_last_callback_wakeup_seq =
        stats.dmabuf_poll_last_callback_wakeup_seq;
    snap->dmabuf_shared_fence_semantics =
        stats.dmabuf_shared_fence_semantics;
    snap->dmabuf_wait_queue_semantics = stats.dmabuf_wait_queue_semantics;
    snap->dmabuf_last_ttm_resv_shared_fence =
        stats.dmabuf_last_ttm_resv_shared_fence;
    snap->dmabuf_last_ttm_resv_shared_count =
        stats.dmabuf_last_ttm_resv_shared_count;
    snap->bo_fence_waits = stats.bo_fence_waits;
    snap->fence_fd_exports = stats.fence_fd_exports;
    snap->fence_fd_queries = stats.fence_fd_queries;
    snap->fence_fd_live = stats.fence_fd_live;
    snap->fence_fd_polls = stats.fence_fd_polls;
    snap->fence_fd_poll_ready = stats.fence_fd_poll_ready;
    snap->ttm_resv_shared_slots = stats.ttm_resv_shared_slots;
    snap->ttm_resv_shared_used = stats.ttm_resv_shared_used;
    snap->ttm_resv_shared_fences = stats.ttm_resv_shared_fences;
    snap->ttm_resv_wait_queued = stats.ttm_resv_wait_queued;
    snap->ttm_resv_wait_wakeups = stats.ttm_resv_wait_wakeups;
    snap->ttm_resv_stale_fence_rejects =
        stats.ttm_resv_stale_fence_rejects;
    snap->ttm_resv_attach_prime_export =
        stats.ttm_resv_attach_prime_export;
    snap->ttm_resv_attach_prime_import =
        stats.ttm_resv_attach_prime_import;
    snap->ttm_resv_attach_dmabuf_export =
        stats.ttm_resv_attach_dmabuf_export;
    snap->ttm_resv_attach_dmabuf_import =
        stats.ttm_resv_attach_dmabuf_import;
    snap->ttm_resv_attach_kms_pin = stats.ttm_resv_attach_kms_pin;
    snap->ttm_resv_attach_kms_unpin = stats.ttm_resv_attach_kms_unpin;
    snap->ttm_resv_last_attach_point = stats.ttm_resv_last_attach_point;
    snap->ttm_resv_last_shared_fence = stats.ttm_resv_last_shared_fence;
    return 0;
}

static int wait_dmabuf_releases_since(const struct kms_credit_snapshot *before,
                                      struct kms_credit_snapshot *after,
                                      uint64 releases, const char *label)
{
    for (int i = 0; i < 50; i++) {
        if (read_credit_snapshot(after) < 0) {
            printf("drmprimeprobe: %s snapshot=failed\n", label);
            return -1;
        }
        if (after->dmabuf_releases >= before->dmabuf_releases + releases)
            return 0;
        sleep(20);
    }
    printf("drmprimeprobe: %s release_wait=timeout releases=%lu->%lu "
           "needed=%lu\n", label, before->dmabuf_releases,
           after->dmabuf_releases, releases);
    return -1;
}

static int wait_dmabuf_visible_close_since(
    const struct kms_credit_snapshot *before,
    struct kms_credit_snapshot *after,
    const char *label)
{
    for (int i = 0; i < 50; i++) {
        if (read_credit_snapshot(after) < 0) {
            printf("drmprimeprobe: %s visible_close_snapshot=failed\n",
                   label);
            return -1;
        }
        if (after->bo_fd_live == before->bo_fd_live &&
            after->dmabuf_live == before->dmabuf_live)
            return 0;
        sleep(20);
    }
    printf("drmprimeprobe: %s visible_close_wait=timeout "
           "bo_fd_live=%lu->%lu dmabuf_live=%lu->%lu\n",
           label, before->bo_fd_live, after->bo_fd_live,
           before->dmabuf_live, after->dmabuf_live);
    return -1;
}

static int wait_fence_live_since(const struct kms_credit_snapshot *before,
                                 struct kms_credit_snapshot *after,
                                 const char *label)
{
    for (int i = 0; i < 50; i++) {
        if (read_credit_snapshot(after) < 0) {
            printf("drmprimeprobe: %s snapshot=failed\n", label);
            return -1;
        }
        if (after->fence_fd_live == before->fence_fd_live)
            return 0;
        sleep(20);
    }
    printf("drmprimeprobe: %s fence_live_wait=timeout live=%lu->%lu\n",
           label, before->fence_fd_live, after->fence_fd_live);
    return -1;
}

static int credit_unchanged(const struct kms_credit_snapshot *before,
                            const struct kms_credit_snapshot *after)
{
    return before->bo_presents == after->bo_presents &&
           before->kms_page_flips == after->kms_page_flips &&
           before->kms_atomic_commits == after->kms_atomic_commits &&
           before->display_presented == after->display_presented &&
           before->display_completed == after->display_completed;
}

static int native_credit_unchanged_since(const struct kms_credit_snapshot *a,
                                         const struct kms_credit_snapshot *b,
                                         const char *token)
{
    if (a->gpu_backend_flags == b->gpu_backend_flags &&
        a->dxg_present_register_successes ==
            b->dxg_present_register_successes &&
        a->dxg_present_commit_attempts == b->dxg_present_commit_attempts &&
        a->dxg_present_commit_rejects == b->dxg_present_commit_rejects &&
        a->dxg_present_display_target_kind ==
            b->dxg_present_display_target_kind &&
        a->dxg_present_dda_nouveau_import_path_present ==
            b->dxg_present_dda_nouveau_import_path_present &&
        a->dxg_present_dda_nouveau_scanout_bind_present ==
            b->dxg_present_dda_nouveau_scanout_bind_present &&
        a->dxg_present_helper_transport_present ==
            b->dxg_present_helper_transport_present)
        return 0;
    printf("drmprimeprobe: %s native_present_credit=unexpected "
           "backend=0x%lx->0x%lx dxg_reg=%lu->%lu "
           "dxg_commit=%lu/%lu->%lu/%lu target=%lu->%lu "
           "dda_import=%lu->%lu dda_scanout=%lu->%lu "
           "transport=%lu->%lu context_bo_display=%lu/%lu/%lu/%lu/%lu->"
           "%lu/%lu/%lu/%lu/%lu\n",
           token, a->gpu_backend_flags, b->gpu_backend_flags,
           a->dxg_present_register_successes,
           b->dxg_present_register_successes,
           a->dxg_present_commit_attempts, a->dxg_present_commit_rejects,
           b->dxg_present_commit_attempts, b->dxg_present_commit_rejects,
           a->dxg_present_display_target_kind,
           b->dxg_present_display_target_kind,
           a->dxg_present_dda_nouveau_import_path_present,
           b->dxg_present_dda_nouveau_import_path_present,
           a->dxg_present_dda_nouveau_scanout_bind_present,
           b->dxg_present_dda_nouveau_scanout_bind_present,
           a->dxg_present_helper_transport_present,
           b->dxg_present_helper_transport_present,
           a->bo_presents, a->kms_page_flips, a->kms_atomic_commits,
           a->display_presented, a->display_completed, b->bo_presents,
           b->kms_page_flips, b->kms_atomic_commits, b->display_presented,
           b->display_completed);
    return -1;
}

static int native_handoff_absent_since(const struct kms_credit_snapshot *a,
                                       const struct kms_credit_snapshot *b,
                                       const char *token)
{
    uint64 opengl_submit =
        b->gpu_backend_flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT;
    uint64 dda_backend =
        b->gpu_backend_flags & FB_GPU_BACKEND_F_DDA_NOUVEAU;

    if (opengl_submit != 0 ||
        b->dxg_present_display_target_kind !=
            FB_GPU_DXG_DISPLAY_TARGET_NONE ||
        b->dxg_present_dda_nouveau_import_path_present != 0 ||
        b->dxg_present_dda_nouveau_scanout_bind_present != 0 ||
        dda_backend != 0) {
        printf("drmprimeprobe: %s native_handoff_credit=unexpected "
               "backend_flags=0x%lx->0x%lx display_target=%lu->%lu "
               "dda_import=%lu->%lu dda_scanout=%lu->%lu "
               "ordinary_bo_display_delta=%lu/%lu/%lu\n",
               token, a->gpu_backend_flags, b->gpu_backend_flags,
               a->dxg_present_display_target_kind,
               b->dxg_present_display_target_kind,
               a->dxg_present_dda_nouveau_import_path_present,
               b->dxg_present_dda_nouveau_import_path_present,
               a->dxg_present_dda_nouveau_scanout_bind_present,
               b->dxg_present_dda_nouveau_scanout_bind_present,
               b->bo_presents - a->bo_presents,
               b->display_presented - a->display_presented,
               b->display_completed - a->display_completed);
        return -1;
    }
    return 0;
}

static int check_dmabuf_resv_matrix(const struct kms_credit_snapshot *before,
                                    const struct kms_credit_snapshot *after,
                                    uint64 exporter_tag,
                                    const char *path)
{
    if (after->dmabuf_exports < before->dmabuf_exports + 1 ||
        after->dmabuf_imports < before->dmabuf_imports + 1 ||
        after->dmabuf_attachments < before->dmabuf_attachments + 1 ||
        after->dmabuf_resv_snapshots < before->dmabuf_resv_snapshots + 2 ||
        after->dmabuf_bad_fd_rejects <
            before->dmabuf_bad_fd_rejects + 1 ||
        after->dmabuf_foreign_fd_rejects <
            before->dmabuf_foreign_fd_rejects + 1 ||
        after->dmabuf_releases < before->dmabuf_releases + 1) {
        printf("drmprimeprobe: dmabuf_diag counters=missing "
               "exports=%lu->%lu imports=%lu->%lu attachments=%lu->%lu "
               "snapshots=%lu->%lu badfd=%lu->%lu foreign=%lu->%lu "
               "releases=%lu->%lu\n",
               before->dmabuf_exports, after->dmabuf_exports,
               before->dmabuf_imports, after->dmabuf_imports,
               before->dmabuf_attachments, after->dmabuf_attachments,
               before->dmabuf_resv_snapshots, after->dmabuf_resv_snapshots,
               before->dmabuf_bad_fd_rejects,
               after->dmabuf_bad_fd_rejects,
               before->dmabuf_foreign_fd_rejects,
               after->dmabuf_foreign_fd_rejects,
               before->dmabuf_releases, after->dmabuf_releases);
        return -1;
    }
    if (after->dmabuf_last_exporter_tag != exporter_tag ||
        after->dmabuf_last_importer_tag != exporter_tag ||
        after->dmabuf_last_ttm_resv_seq == 0) {
        printf("drmprimeprobe: dmabuf_diag tags_or_resv=missing "
               "exporter=%lu importer=%lu resv_seq=%lu fence=%lu\n",
               after->dmabuf_last_exporter_tag,
               after->dmabuf_last_importer_tag,
               after->dmabuf_last_ttm_resv_seq,
               after->dmabuf_last_ttm_resv_exclusive_fence);
        return -1;
    }
    if (after->dmabuf_poll_semantics == 0 ||
        after->dmabuf_shared_fence_semantics == 0 ||
        after->dmabuf_wait_queue_semantics == 0 ||
        after->dmabuf_last_ttm_resv_shared_fence == 0 ||
        after->dmabuf_last_ttm_resv_shared_count == 0 ||
        after->ttm_resv_shared_slots == 0 ||
        after->ttm_resv_shared_fences < before->ttm_resv_shared_fences + 2) {
        printf("drmprimeprobe: dmabuf_resv_matrix linux_wait_semantics=missing "
               "poll=%lu shared=%lu waitq=%lu shared_fence=%lu "
               "shared_count=%lu shared_slots=%lu shared_fences=%lu->%lu\n",
               after->dmabuf_poll_semantics,
               after->dmabuf_shared_fence_semantics,
               after->dmabuf_wait_queue_semantics,
               after->dmabuf_last_ttm_resv_shared_fence,
               after->dmabuf_last_ttm_resv_shared_count,
               after->ttm_resv_shared_slots,
               before->ttm_resv_shared_fences,
               after->ttm_resv_shared_fences);
        return -1;
    }
    if (exporter_tag == FB_GPU_DMABUF_TAG_DRM_PRIME) {
        if (after->ttm_resv_attach_prime_export <
                before->ttm_resv_attach_prime_export + 1 ||
            after->ttm_resv_attach_prime_import <
                before->ttm_resv_attach_prime_import + 1 ||
            after->ttm_resv_last_attach_point !=
                FB_GPU_RESV_ATTACH_PRIME_IMPORT) {
            printf("drmprimeprobe: dmabuf_resv_matrix "
                   "prime_attach=missing export=%lu->%lu import=%lu->%lu "
                   "last_attach=%lu\n",
                   before->ttm_resv_attach_prime_export,
                   after->ttm_resv_attach_prime_export,
                   before->ttm_resv_attach_prime_import,
                   after->ttm_resv_attach_prime_import,
                   after->ttm_resv_last_attach_point);
            return -1;
        }
    } else if (exporter_tag == FB_GPU_DMABUF_TAG_FB_BO) {
        if (after->ttm_resv_attach_dmabuf_export <
                before->ttm_resv_attach_dmabuf_export + 1 ||
            after->ttm_resv_attach_dmabuf_import <
                before->ttm_resv_attach_dmabuf_import + 1 ||
            after->ttm_resv_last_attach_point !=
                FB_GPU_RESV_ATTACH_DMABUF_IMPORT) {
            printf("drmprimeprobe: dmabuf_resv_matrix "
                   "fb_bo_attach=missing export=%lu->%lu import=%lu->%lu "
                   "last_attach=%lu\n",
                   before->ttm_resv_attach_dmabuf_export,
                   after->ttm_resv_attach_dmabuf_export,
                   before->ttm_resv_attach_dmabuf_import,
                   after->ttm_resv_attach_dmabuf_import,
                   after->ttm_resv_last_attach_point);
            return -1;
        }
    }
    printf("drmprimeprobe: dmabuf_diag exporter_tag=%lu importer_tag=%lu "
           "exports_delta=%lu imports_delta=%lu attachments_delta=%lu "
           "live=%lu live_attachments=%lu releases_delta=%lu "
           "bad_fd_rejects_delta=%lu foreign_fd_rejects_delta=%lu "
           "resv_snapshots_delta=%lu resv_seq=%lu exclusive_fence=%lu "
           "poll_semantics=%lu shared_fence_semantics=%lu "
           "wait_queue_semantics=%lu shared_fence=%lu shared_count=%lu "
           "shared_fences_delta=%lu\n",
           after->dmabuf_last_exporter_tag,
           after->dmabuf_last_importer_tag,
           after->dmabuf_exports - before->dmabuf_exports,
           after->dmabuf_imports - before->dmabuf_imports,
           after->dmabuf_attachments - before->dmabuf_attachments,
           after->dmabuf_live, after->dmabuf_live_attachments,
           after->dmabuf_releases - before->dmabuf_releases,
           after->dmabuf_bad_fd_rejects - before->dmabuf_bad_fd_rejects,
           after->dmabuf_foreign_fd_rejects -
           before->dmabuf_foreign_fd_rejects,
           after->dmabuf_resv_snapshots - before->dmabuf_resv_snapshots,
           after->dmabuf_last_ttm_resv_seq,
           after->dmabuf_last_ttm_resv_exclusive_fence,
           after->dmabuf_poll_semantics,
           after->dmabuf_shared_fence_semantics,
           after->dmabuf_wait_queue_semantics,
           after->dmabuf_last_ttm_resv_shared_fence,
           after->dmabuf_last_ttm_resv_shared_count,
           after->ttm_resv_shared_fences - before->ttm_resv_shared_fences);
    printf("drmprimeprobe: dmabuf_resv_matrix path=%s exporter_tag=%lu "
           "importer_tag=%lu exports_delta=%lu imports_delta=%lu "
           "attachments_delta=%lu bad_fd_rejects_delta=%lu "
           "foreign_fd_rejects_delta=%lu releases_delta=%lu "
           "resv_snapshots_delta=%lu resv_seq=%lu exclusive_fence=%lu "
           "shared_fence_semantics=%lu wait_queue_semantics=%lu "
           "poll_semantics=%lu shared_fence=%lu shared_count=%lu "
           "shared_fences_delta=%lu attach_export_delta=%lu "
           "attach_import_delta=%lu last_attach_point=%lu "
           "native_present_credit=0 status=PASS\n",
           path,
           after->dmabuf_last_exporter_tag,
           after->dmabuf_last_importer_tag,
           after->dmabuf_exports - before->dmabuf_exports,
           after->dmabuf_imports - before->dmabuf_imports,
           after->dmabuf_attachments - before->dmabuf_attachments,
           after->dmabuf_bad_fd_rejects - before->dmabuf_bad_fd_rejects,
           after->dmabuf_foreign_fd_rejects -
               before->dmabuf_foreign_fd_rejects,
           after->dmabuf_releases - before->dmabuf_releases,
           after->dmabuf_resv_snapshots - before->dmabuf_resv_snapshots,
           after->dmabuf_last_ttm_resv_seq,
           after->dmabuf_last_ttm_resv_exclusive_fence,
           after->dmabuf_shared_fence_semantics,
           after->dmabuf_wait_queue_semantics,
           after->dmabuf_poll_semantics,
           after->dmabuf_last_ttm_resv_shared_fence,
           after->dmabuf_last_ttm_resv_shared_count,
           after->ttm_resv_shared_fences - before->ttm_resv_shared_fences,
           exporter_tag == FB_GPU_DMABUF_TAG_DRM_PRIME ?
                after->ttm_resv_attach_prime_export -
                    before->ttm_resv_attach_prime_export :
                after->ttm_resv_attach_dmabuf_export -
                    before->ttm_resv_attach_dmabuf_export,
           exporter_tag == FB_GPU_DMABUF_TAG_DRM_PRIME ?
                after->ttm_resv_attach_prime_import -
                    before->ttm_resv_attach_prime_import :
                after->ttm_resv_attach_dmabuf_import -
                    before->ttm_resv_attach_dmabuf_import,
           after->ttm_resv_last_attach_point);
    return 0;
}

static void destroy_dumb_if_valid(int fd, uint32 handle)
{
    struct drm_mode_destroy_dumb_compat destroy_req;

    if (fd < 0 || handle == 0)
        return;
    memset(&destroy_req, 0, sizeof(destroy_req));
    destroy_req.handle = handle;
    (void)ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
}

static int create_dumb_checked(int fd, uint32 width, uint32 height,
                               uint32 bpp,
                               struct drm_mode_create_dumb_compat *create,
                               const char *name)
{
    memset(create, 0, sizeof(*create));
    create->width = width;
    create->height = height;
    create->bpp = bpp;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, create) < 0 ||
        create->handle == 0 || create->size == 0) {
        printf("drmprimeprobe: %s CREATE_DUMB failed handle=%u size=%lu\n",
               name, create->handle, create->size);
        return -1;
    }
    return 0;
}

static void fill_nv12_fb(struct drm_mode_fb_cmd2_compat *fb, uint32 width,
                         uint32 height, uint32 y_handle, uint32 uv_handle,
                         uint64 mod0, uint64 mod1)
{
    memset(fb, 0, sizeof(*fb));
    fb->width = width;
    fb->height = height;
    fb->pixel_format = DRM_FORMAT_NV12;
    fb->handles[0] = y_handle;
    fb->handles[1] = uv_handle;
    fb->pitches[0] = width;
    fb->pitches[1] = width;
    fb->offsets[1] = width * height;
    fb->flags = DRM_MODE_FB_MODIFIERS;
    fb->modifier[0] = mod0;
    fb->modifier[1] = mod1;
}

static int remove_fb_if_valid(int fd, uint32 *fb_id)
{
    if (*fb_id == 0)
        return 0;
    if (ioctl(fd, DRM_IOCTL_MODE_RMFB, fb_id) < 0)
        return -1;
    *fb_id = 0;
    return 0;
}

static int check_linear_nv12_roundtrip(int fd, uint32 handle,
                                       uint32 width, uint32 height)
{
    struct drm_mode_fb_cmd2_compat fb;
    uint32 fb_id;

    fill_nv12_fb(&fb, width, height, handle, handle,
                 DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_LINEAR);
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0 || fb.fb_id == 0) {
        printf("drmprimeprobe: kms_modifier_matrix linear_nv12=unsupported\n");
        return -1;
    }
    fb_id = fb.fb_id;
    memset(&fb, 0, sizeof(fb));
    fb.fb_id = fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb) < 0 ||
        fb.width != width || fb.height != height ||
        fb.pixel_format != DRM_FORMAT_NV12 ||
        fb.handles[0] != handle || fb.handles[1] != handle ||
        fb.pitches[0] != width || fb.pitches[1] != width ||
        fb.offsets[1] != width * height ||
        fb.modifier[0] != DRM_FORMAT_MOD_LINEAR ||
        fb.modifier[1] != DRM_FORMAT_MOD_LINEAR) {
        (void)remove_fb_if_valid(fd, &fb_id);
        printf("drmprimeprobe: kms_modifier_matrix getfb2_metadata=mismatch\n");
        return -1;
    }
    if (remove_fb_if_valid(fd, &fb_id) < 0) {
        printf("drmprimeprobe: kms_modifier_matrix linear_rmfb=failed\n");
        return -1;
    }
    printf("drmprimeprobe: kms_modifier_matrix linear_nv12=accepted "
           "getfb2_nv12_metadata=ok\n");
    return 0;
}

static int check_rejected_nv12_addfb2(int fd,
                                      struct drm_mode_fb_cmd2_compat *fb,
                                      const char *token,
                                      const char *bad_token)
{
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, fb) >= 0 || fb->fb_id != 0) {
        printf("drmprimeprobe: %s=unexpected_accept fb_id=%u\n",
               bad_token, fb->fb_id);
        if (fb->fb_id != 0)
            (void)remove_fb_if_valid(fd, &fb->fb_id);
        return -1;
    }
    printf("drmprimeprobe: %s\n", token);
    return 0;
}

static int check_kms_prime_matrix(void)
{
    int fd = open("/dev/dri/card0", O_RDWR);
    struct drm_mode_create_dumb_compat y_create;
    struct drm_mode_create_dumb_compat uv_create;
    struct drm_mode_fb_cmd2_compat fb;
    struct kms_credit_snapshot before;
    struct kms_credit_snapshot after;
    int ret = 1;

    if (fd < 0) {
        printf("drmprimeprobe: open primary node failed\n");
        return 1;
    }
    if (read_credit_snapshot(&before) < 0) {
        printf("drmprimeprobe: kms_credit_snapshot before=failed\n");
        goto out;
    }
    if (create_dumb_checked(fd, 96, 96, 32, &y_create, "y-plane") < 0)
        goto out;
    if (create_dumb_checked(fd, 96, 48, 32, &uv_create, "uv-plane") < 0)
        goto out_destroy_y;

    fill_nv12_fb(&fb, 96, 64, y_create.handle, y_create.handle,
                 DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_LINEAR);
    if (check_linear_nv12_roundtrip(fd, y_create.handle, 96, 64) < 0)
        goto out_destroy_uv;

    fill_nv12_fb(&fb, 96, 64, y_create.handle, y_create.handle,
                 DRMPRIMEPROBE_TEST_MOD_NONLINEAR,
                 DRMPRIMEPROBE_TEST_MOD_NONLINEAR);
    if (check_rejected_nv12_addfb2(fd, &fb,
            "kms_modifier_matrix nonlinear_modifier=rejected",
            "kms_modifier_matrix nonlinear_modifier") < 0)
        goto out_destroy_uv;

    fill_nv12_fb(&fb, 96, 64, y_create.handle, y_create.handle,
                 DRM_FORMAT_MOD_LINEAR, DRMPRIMEPROBE_TEST_MOD_NONLINEAR);
    if (check_rejected_nv12_addfb2(fd, &fb,
            "kms_modifier_matrix mixed_plane_modifier=rejected",
            "kms_modifier_matrix mixed_plane_modifier") < 0)
        goto out_destroy_uv;

    fill_nv12_fb(&fb, 96, 64, y_create.handle, uv_create.handle,
                 DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_LINEAR);
    if (check_rejected_nv12_addfb2(fd, &fb,
            "kms_multiplane_matrix separate_plane_nv12=fail_closed",
            "kms_multiplane_matrix separate_plane_nv12") < 0)
        goto out_destroy_uv;

    if (read_credit_snapshot(&after) < 0) {
        printf("drmprimeprobe: kms_credit_snapshot after=failed\n");
        goto out_destroy_uv;
    }
    if (after.ttm_resv_attach_kms_pin < before.ttm_resv_attach_kms_pin + 1 ||
        after.ttm_resv_attach_kms_unpin <
            before.ttm_resv_attach_kms_unpin + 1 ||
        after.ttm_resv_last_attach_point != FB_GPU_RESV_ATTACH_KMS_UNPIN ||
        after.ttm_resv_last_shared_fence == 0) {
        printf("drmprimeprobe: kms_resv_attach_matrix counters=missing "
               "pin=%lu->%lu unpin=%lu->%lu last_attach=%lu "
               "last_shared_fence=%lu\n",
               before.ttm_resv_attach_kms_pin,
               after.ttm_resv_attach_kms_pin,
               before.ttm_resv_attach_kms_unpin,
               after.ttm_resv_attach_kms_unpin,
               after.ttm_resv_last_attach_point,
               after.ttm_resv_last_shared_fence);
        goto out_destroy_uv;
    }
    if (!credit_unchanged(&before, &after)) {
        printf("drmprimeprobe: kms_multiplane_matrix "
               "native_present_credit=unexpected before=%lu/%lu/%lu/%lu/%lu "
               "after=%lu/%lu/%lu/%lu/%lu\n",
               before.bo_presents, before.kms_page_flips,
               before.kms_atomic_commits, before.display_presented,
               before.display_completed, after.bo_presents,
               after.kms_page_flips, after.kms_atomic_commits,
               after.display_presented, after.display_completed);
        goto out_destroy_uv;
    }
    printf("drmprimeprobe: kms_multiplane_matrix "
           "separate_plane_native_present_credit=0 "
           "separate_plane_scanout_credit=0 status=PASS\n");
    printf("drmprimeprobe: kms_resv_attach_matrix kms_pin_delta=%lu "
           "kms_unpin_delta=%lu last_attach_point=%lu "
           "last_shared_fence=%lu native_present_credit=0 status=PASS\n",
           after.ttm_resv_attach_kms_pin - before.ttm_resv_attach_kms_pin,
           after.ttm_resv_attach_kms_unpin -
               before.ttm_resv_attach_kms_unpin,
           after.ttm_resv_last_attach_point,
           after.ttm_resv_last_shared_fence);
    printf("drmprimeprobe: kms_modifier_matrix status=PASS\n");
    ret = 0;

out_destroy_uv:
    destroy_dumb_if_valid(fd, uv_create.handle);
out_destroy_y:
    destroy_dumb_if_valid(fd, y_create.handle);
out:
    close(fd);
    return ret;
}

static int check_fb_bo_dmabuf_matrix(void)
{
    int fd1 = open("/dev/gpu0", O_RDWR);
    int fd2 = open("/dev/gpu0", O_RDWR);
    struct fb_gpu_bo_create create = {
        .width = 32,
        .height = 32,
        .flags = FB_GPU_BO_F_EXPORTABLE,
    };
    struct fb_gpu_bo_export_fd export_fd;
    struct fb_gpu_bo_import_fd import_fd;
    struct fb_gpu_bo_import_fd bad_import_fd;
    struct fb_gpu_bo_destroy destroy;
    struct kms_credit_snapshot before;
    struct kms_credit_snapshot after_visible_close;
    struct kms_credit_snapshot after_import;
    struct kms_credit_snapshot after_destroy;
    int exported_fd = -1;
    int ret = 1;

    if (fd1 < 0 || fd2 < 0) {
        printf("drmprimeprobe: fb_bo_dmabuf open_gpu0=failed\n");
        goto out;
    }
    if (read_credit_snapshot(&before) < 0) {
        printf("drmprimeprobe: fb_bo_dmabuf before=failed\n");
        goto out;
    }
    if (ioctl(fd1, FB_GPU_BO_CREATE, &create) < 0 ||
        create.handle == 0 || create.addr == 0 || create.size == 0) {
        printf("drmprimeprobe: fb_bo_dmabuf create=failed\n");
        goto out;
    }
    memset(&export_fd, 0, sizeof(export_fd));
    export_fd.handle = create.handle;
    if (ioctl(fd1, FB_GPU_BO_EXPORT_FD, &export_fd) < 0 ||
        export_fd.fd < 0) {
        printf("drmprimeprobe: fb_bo_dmabuf export=failed handle=%u\n",
               create.handle);
        goto out_destroy_create;
    }
    exported_fd = export_fd.fd;

    memset(&import_fd, 0, sizeof(import_fd));
    import_fd.fd = exported_fd;
    if (ioctl(fd2, FB_GPU_BO_IMPORT_FD, &import_fd) < 0 ||
        import_fd.handle == 0 || import_fd.addr == 0 ||
        import_fd.width != create.width || import_fd.height != create.height) {
        printf("drmprimeprobe: fb_bo_dmabuf import=failed fd=%d\n",
               exported_fd);
        goto out_close_export;
    }
    close(exported_fd);
    exported_fd = -1;
    if (wait_dmabuf_visible_close_since(&before, &after_visible_close,
                                        "fb_bo_dmabuf") < 0)
        goto out_destroy_import;
    printf("drmprimeprobe: dmabuf_visible_close_matrix path=fb_bo "
           "bo_fd_live=%lu->%lu dmabuf_live=%lu->%lu "
           "status=PASS\n",
           before.bo_fd_live, after_visible_close.bo_fd_live,
           before.dmabuf_live, after_visible_close.dmabuf_live);
    memset(&bad_import_fd, 0, sizeof(bad_import_fd));
    bad_import_fd.fd = exported_fd;
    if (ioctl(fd2, FB_GPU_BO_IMPORT_FD, &bad_import_fd) >= 0) {
        printf("drmprimeprobe: fb_bo_dmabuf closed_fd=unexpected_accept\n");
        goto out_destroy_import;
    }
    memset(&bad_import_fd, 0, sizeof(bad_import_fd));
    bad_import_fd.fd = fd1;
    if (ioctl(fd2, FB_GPU_BO_IMPORT_FD, &bad_import_fd) >= 0) {
        printf("drmprimeprobe: fb_bo_dmabuf foreign_fd=unexpected_accept\n");
        goto out_destroy_import;
    }

    if (wait_dmabuf_releases_since(&before, &after_import, 1,
                                   "fb_bo_dmabuf after_import") < 0) {
        goto out_destroy_import;
    }
    if (check_dmabuf_resv_matrix(&before, &after_import,
                                 FB_GPU_DMABUF_TAG_FB_BO, "fb_bo") < 0)
        goto out_destroy_import;
    if (native_credit_unchanged_since(&before, &after_import,
                                      "fb_bo_dmabuf") < 0)
        goto out_destroy_import;

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = import_fd.handle;
    if (ioctl(fd2, FB_GPU_BO_DESTROY, &destroy) < 0) {
        printf("drmprimeprobe: fb_bo_dmabuf destroy_import=failed\n");
        goto out_destroy_import;
    }
    munmap((void *)import_fd.addr, (int)import_fd.size);
    import_fd.handle = 0;
    import_fd.addr = 0;
    if (read_credit_snapshot(&after_destroy) < 0) {
        printf("drmprimeprobe: fb_bo_dmabuf after_destroy=failed\n");
        goto out_destroy_create;
    }
    if (after_destroy.dmabuf_live_attachments >
        after_import.dmabuf_live_attachments) {
        printf("drmprimeprobe: fb_bo_dmabuf live_attachment_release=failed "
               "before=%lu after=%lu\n",
               after_import.dmabuf_live_attachments,
               after_destroy.dmabuf_live_attachments);
        goto out_destroy_create;
    }
    if (native_credit_unchanged_since(&before, &after_destroy,
                                      "fb_bo_dmabuf_destroy") < 0)
        goto out_destroy_create;
    printf("drmprimeprobe: fb_bo_dmabuf export_import=PASS "
           "live_attachment_release=ok native_present_credit=0\n");
    ret = 0;

out_destroy_import:
    if (import_fd.handle != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = import_fd.handle;
        (void)ioctl(fd2, FB_GPU_BO_DESTROY, &destroy);
    }
    if (import_fd.addr != 0 && import_fd.size != 0)
        munmap((void *)import_fd.addr, (int)import_fd.size);
out_close_export:
    if (exported_fd >= 0)
        close(exported_fd);
out_destroy_create:
    if (create.handle != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = create.handle;
        (void)ioctl(fd1, FB_GPU_BO_DESTROY, &destroy);
    }
    if (create.addr != 0 && create.size != 0)
        munmap((void *)create.addr, (int)create.size);
out:
    if (fd1 >= 0)
        close(fd1);
    if (fd2 >= 0)
        close(fd2);
    return ret;
}

static int check_stale_fence_wait_matrix(void)
{
    int fd = open("/dev/gpu0", O_RDWR);
    struct fb_gpu_bo_create create = {
        .width = 16,
        .height = 16,
        .flags = FB_GPU_BO_F_EXPORTABLE,
    };
    struct fb_gpu_fence_export_fd fence_export;
    struct fb_gpu_fence_query fence_query;
    struct fb_gpu_fence_query fence_wait;
    struct fb_gpu_fence_query closed_query;
    struct fb_gpu_bo_fence bo_wait;
    struct fb_gpu_bo_destroy destroy;
    struct kms_credit_snapshot before;
    struct kms_credit_snapshot after;
    struct pollfd pfd;
    struct pollfd closed_pfd;
    int fence_fd = -1;
    int ret = 1;

    if (fd < 0) {
        printf("drmprimeprobe: stale_fence_wait_matrix open_gpu0=failed\n");
        return 1;
    }
    if (read_credit_snapshot(&before) < 0) {
        printf("drmprimeprobe: stale_fence_wait_matrix before=failed\n");
        goto out;
    }
    if (ioctl(fd, FB_GPU_BO_CREATE, &create) < 0 ||
        create.handle == 0 || create.addr == 0 || create.size == 0) {
        printf("drmprimeprobe: stale_fence_wait_matrix create=failed\n");
        goto out;
    }

    memset(&fence_export, 0, sizeof(fence_export));
    fence_export.handle = create.handle;
    fence_export.fence = 1;
    if (ioctl(fd, FB_GPU_FENCE_EXPORT_FD, &fence_export) < 0 ||
        fence_export.fd < 0 || fence_export.fence != 1 ||
        fence_export.signaled != 0) {
        printf("drmprimeprobe: stale_fence_wait_matrix export_future=failed "
               "fd=%d fence=%lu signaled=%lu\n",
               fence_export.fd, fence_export.fence, fence_export.signaled);
        goto out_destroy;
    }
    fence_fd = fence_export.fd;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fence_fd;
    pfd.events = POLLIN | POLLRDNORM;
    if (poll_raw(&pfd, 1, 0) != 0 || pfd.revents != 0) {
        printf("drmprimeprobe: stale_fence_wait_matrix "
               "pending_poll_ready=unexpected revents=0x%x\n",
               pfd.revents);
        goto out_close_fence;
    }

    memset(&fence_query, 0, sizeof(fence_query));
    fence_query.fd = fence_fd;
    if (ioctl(fd, FB_GPU_FENCE_QUERY, &fence_query) < 0 ||
        fence_query.fence != 1 || fence_query.signaled >= fence_query.fence) {
        printf("drmprimeprobe: stale_fence_wait_matrix pending_query=failed "
               "fence=%lu signaled=%lu\n",
               fence_query.fence, fence_query.signaled);
        goto out_close_fence;
    }

    memset(&fence_wait, 0, sizeof(fence_wait));
    fence_wait.fd = fence_fd;
    fence_wait.flags = FB_GPU_FENCE_WAIT;
    if (ioctl(fd, FB_GPU_FENCE_QUERY, &fence_wait) >= 0) {
        printf("drmprimeprobe: stale_fence_wait_matrix "
               "future_wait_reject=unexpected_accept fence=%lu signaled=%lu\n",
               fence_wait.fence, fence_wait.signaled);
        goto out_close_fence;
    }

    memset(&bo_wait, 0, sizeof(bo_wait));
    bo_wait.handle = create.handle;
    bo_wait.flags = FB_GPU_BO_FENCE_WAIT;
    bo_wait.wait_for = 1;
    if (ioctl(fd, FB_GPU_BO_FENCE, &bo_wait) >= 0) {
        printf("drmprimeprobe: stale_fence_wait_matrix "
               "bo_future_wait=unexpected_accept\n");
        goto out_close_fence;
    }

    close(fence_fd);
    fence_fd = -1;

    memset(&closed_pfd, 0, sizeof(closed_pfd));
    closed_pfd.fd = fence_export.fd;
    closed_pfd.events = POLLIN | POLLRDNORM;
    if (poll_raw(&closed_pfd, 1, 0) != 1 ||
        (closed_pfd.revents & POLLNVAL) == 0) {
        printf("drmprimeprobe: stale_fence_wait_matrix "
               "closed_poll_reject=failed revents=0x%x\n",
               closed_pfd.revents);
        goto out_destroy;
    }

    memset(&closed_query, 0, sizeof(closed_query));
    closed_query.fd = fence_export.fd;
    if (ioctl(fd, FB_GPU_FENCE_QUERY, &closed_query) >= 0) {
        printf("drmprimeprobe: stale_fence_wait_matrix "
               "closed_query=unexpected_accept\n");
        goto out_destroy;
    }

    if (wait_fence_live_since(&before, &after,
                              "stale_fence_wait_matrix") < 0) {
        printf("drmprimeprobe: stale_fence_wait_matrix after=failed\n");
        goto out_destroy;
    }
    if (after.fence_fd_exports < before.fence_fd_exports + 1 ||
        after.fence_fd_queries < before.fence_fd_queries + 2 ||
        after.fence_fd_polls < before.fence_fd_polls + 1 ||
        after.bo_fence_waits < before.bo_fence_waits + 1 ||
        after.fence_fd_live != before.fence_fd_live ||
        after.ttm_resv_stale_fence_rejects <
            before.ttm_resv_stale_fence_rejects + 2) {
        printf("drmprimeprobe: stale_fence_wait_matrix counters=missing "
               "exports=%lu->%lu queries=%lu->%lu polls=%lu->%lu "
               "bo_waits=%lu->%lu live=%lu->%lu stale_rejects=%lu->%lu\n",
               before.fence_fd_exports, after.fence_fd_exports,
               before.fence_fd_queries, after.fence_fd_queries,
               before.fence_fd_polls, after.fence_fd_polls,
               before.bo_fence_waits, after.bo_fence_waits,
               before.fence_fd_live, after.fence_fd_live,
               before.ttm_resv_stale_fence_rejects,
               after.ttm_resv_stale_fence_rejects);
        goto out_destroy;
    }
    if (native_handoff_absent_since(&before, &after,
                                    "stale_fence_wait_matrix") < 0)
        goto out_destroy;
    printf("drmprimeprobe: stale_fence_wait_matrix future_fence=1 "
           "pending_query=ok future_wait_rejected=1 "
           "bo_future_wait_rejected=1 closed_fd_rejected=1 "
           "fence_fd_exports_delta=%lu fence_fd_queries_delta=%lu "
           "fence_fd_polls_delta=%lu bo_fence_waits_delta=%lu "
           "poll_ready_delta=%lu stale_fence_rejects_delta=%lu "
           "ordinary_bo_presents_delta=%lu ordinary_display_presented_delta=%lu "
           "ordinary_display_completed_delta=%lu native_handoff_credit=0 "
           "native_present_credit=0 status=PASS\n",
           after.fence_fd_exports - before.fence_fd_exports,
           after.fence_fd_queries - before.fence_fd_queries,
           after.fence_fd_polls - before.fence_fd_polls,
           after.bo_fence_waits - before.bo_fence_waits,
           after.fence_fd_poll_ready - before.fence_fd_poll_ready,
           after.ttm_resv_stale_fence_rejects -
               before.ttm_resv_stale_fence_rejects,
           after.bo_presents - before.bo_presents,
           after.display_presented - before.display_presented,
           after.display_completed - before.display_completed);
    ret = 0;

out_close_fence:
    if (fence_fd >= 0)
        close(fence_fd);
out_destroy:
    if (create.handle != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = create.handle;
        (void)ioctl(fd, FB_GPU_BO_DESTROY, &destroy);
    }
    if (create.addr != 0 && create.size != 0)
        munmap((void *)create.addr, (int)create.size);
out:
    close(fd);
    return ret;
}

static int check_dmabuf_poll_readiness_matrix(void)
{
    int fd = open("/dev/gpu0", O_RDWR);
    struct fb_gpu_bo_create create = {
        .width = 16,
        .height = 16,
        .flags = FB_GPU_BO_F_EXPORTABLE,
    };
    struct fb_gpu_ttm_validate reserve;
    struct fb_gpu_ttm_validate unreserve;
    struct fb_gpu_bo_export_fd export_fd;
    struct fb_gpu_bo_present present;
    struct fb_gpu_bo_destroy destroy;
    struct kms_credit_snapshot before;
    struct kms_credit_snapshot after;
    struct pollfd pfd;
    uint64 reserved_fence = 0;
    int dmabuf_fd = -1;
    int reserved = 0;
    int ret = 1;

    if (fd < 0) {
        printf("drmprimeprobe: dmabuf_poll_readiness_matrix "
               "open_gpu0=failed\n");
        return 1;
    }
    if (read_credit_snapshot(&before) < 0) {
        printf("drmprimeprobe: dmabuf_poll_readiness_matrix before=failed\n");
        goto out;
    }
    if (ioctl(fd, FB_GPU_BO_CREATE, &create) < 0 ||
        create.handle == 0 || create.addr == 0 || create.size == 0) {
        printf("drmprimeprobe: dmabuf_poll_readiness_matrix create=failed\n");
        goto out;
    }

    memset((void *)create.addr, 0x5a, (int)create.size);

    memset(&reserve, 0, sizeof(reserve));
    reserve.handle = create.handle;
    reserve.flags = FB_GPU_TTM_F_RESERVE;
    if (ioctl(fd, FB_GPU_TTM_VALIDATE, &reserve) < 0 ||
        reserve.resv_count == 0 || reserve.resv_exclusive_fence == 0) {
        printf("drmprimeprobe: dmabuf_poll_readiness_matrix "
               "reserve=failed count=%lu fence=%lu\n",
               reserve.resv_count, reserve.resv_exclusive_fence);
        goto out_destroy;
    }
    reserved = 1;
    reserved_fence = reserve.resv_exclusive_fence;

    memset(&export_fd, 0, sizeof(export_fd));
    export_fd.handle = create.handle;
    if (ioctl(fd, FB_GPU_BO_EXPORT_FD, &export_fd) < 0 ||
        export_fd.fd < 0) {
        printf("drmprimeprobe: dmabuf_poll_readiness_matrix export=failed "
               "fd=%d\n", export_fd.fd);
        goto out_unreserve;
    }
    dmabuf_fd = export_fd.fd;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = dmabuf_fd;
    pfd.events = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
    if (poll_raw(&pfd, 1, 0) != 0 || pfd.revents != 0) {
        printf("drmprimeprobe: dmabuf_poll_readiness_matrix "
               "pending_poll_ready=unexpected revents=0x%x\n",
               pfd.revents);
        goto out_close_dmabuf;
    }

    memset(&present, 0, sizeof(present));
    present.handle = create.handle;
    present.w = create.width;
    present.h = create.height;
    present.src_pitch = create.pitch;
    present.pixels = 0;
    if (ioctl(fd, FB_GPU_BO_PRESENT, &present) < 0 ||
        present.fence < reserved_fence) {
        printf("drmprimeprobe: dmabuf_poll_readiness_matrix present=failed "
               "present_fence=%lu reserved_fence=%lu\n",
               present.fence, reserved_fence);
        goto out_close_dmabuf;
    }

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = dmabuf_fd;
    pfd.events = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
    if (poll_raw(&pfd, 1, 0) != 1 ||
        (pfd.revents & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM)) == 0) {
        printf("drmprimeprobe: dmabuf_poll_readiness_matrix "
               "ready_poll=failed revents=0x%x\n", pfd.revents);
        goto out_close_dmabuf;
    }

    if (read_credit_snapshot(&after) < 0) {
        printf("drmprimeprobe: dmabuf_poll_readiness_matrix after=failed\n");
        goto out_close_dmabuf;
    }
    if (after.dmabuf_poll_attempts < before.dmabuf_poll_attempts + 2 ||
        after.dmabuf_poll_not_ready < before.dmabuf_poll_not_ready + 1 ||
        after.dmabuf_poll_ready < before.dmabuf_poll_ready + 1 ||
        after.dmabuf_poll_read_ready <
            before.dmabuf_poll_read_ready + 1 ||
        after.dmabuf_poll_write_ready <
            before.dmabuf_poll_write_ready + 1 ||
        after.dmabuf_poll_pending < before.dmabuf_poll_pending + 1 ||
        after.dmabuf_poll_real_fence_ready <
            before.dmabuf_poll_real_fence_ready + 2 ||
        after.dmabuf_poll_callbacks_armed <
            before.dmabuf_poll_callbacks_armed + 1 ||
        after.dmabuf_poll_callbacks_fired <
            before.dmabuf_poll_callbacks_fired + 1 ||
        after.dmabuf_poll_pending_to_ready <
            before.dmabuf_poll_pending_to_ready + 1 ||
        after.dmabuf_poll_errors != before.dmabuf_poll_errors ||
        after.dmabuf_poll_last_target_fence < reserved_fence ||
        after.dmabuf_poll_last_read_fence < reserved_fence ||
        after.dmabuf_poll_last_write_fence < reserved_fence ||
        after.dmabuf_poll_last_signaled_fence <
            after.dmabuf_poll_last_target_fence ||
        after.dmabuf_poll_last_callback_source !=
            FB_GPU_DMABUF_POLL_WAKE_PRESENT_SIGNAL ||
        after.dmabuf_poll_last_callback_target_fence < reserved_fence ||
        after.dmabuf_poll_last_callback_wakeup_seq == 0 ||
        after.dmabuf_poll_semantics == 0 ||
        after.dmabuf_last_ttm_resv_exclusive_fence < reserved_fence) {
        printf("drmprimeprobe: dmabuf_poll_readiness_matrix "
               "counters=missing attempts=%lu->%lu ready=%lu->%lu "
               "not_ready=%lu->%lu read_ready=%lu->%lu "
               "write_ready=%lu->%lu pending=%lu->%lu "
               "real_ready=%lu->%lu callback_armed=%lu->%lu "
               "callback_fired=%lu->%lu pending_to_ready=%lu->%lu "
               "errors=%lu->%lu target=%lu "
               "read_fence=%lu write_fence=%lu signaled=%lu "
               "wake_source=%lu wake_target=%lu wake_seq=%lu "
               "reserved_fence=%lu last_exclusive=%lu\n",
               before.dmabuf_poll_attempts, after.dmabuf_poll_attempts,
               before.dmabuf_poll_ready, after.dmabuf_poll_ready,
               before.dmabuf_poll_not_ready, after.dmabuf_poll_not_ready,
               before.dmabuf_poll_read_ready,
               after.dmabuf_poll_read_ready,
               before.dmabuf_poll_write_ready,
               after.dmabuf_poll_write_ready,
               before.dmabuf_poll_pending, after.dmabuf_poll_pending,
               before.dmabuf_poll_real_fence_ready,
               after.dmabuf_poll_real_fence_ready,
               before.dmabuf_poll_callbacks_armed,
               after.dmabuf_poll_callbacks_armed,
               before.dmabuf_poll_callbacks_fired,
               after.dmabuf_poll_callbacks_fired,
               before.dmabuf_poll_pending_to_ready,
               after.dmabuf_poll_pending_to_ready,
               before.dmabuf_poll_errors, after.dmabuf_poll_errors,
               after.dmabuf_poll_last_target_fence,
               after.dmabuf_poll_last_read_fence,
               after.dmabuf_poll_last_write_fence,
               after.dmabuf_poll_last_signaled_fence,
               after.dmabuf_poll_last_callback_source,
               after.dmabuf_poll_last_callback_target_fence,
               after.dmabuf_poll_last_callback_wakeup_seq,
               reserved_fence, after.dmabuf_last_ttm_resv_exclusive_fence);
        goto out_close_dmabuf;
    }
    if (native_handoff_absent_since(&before, &after,
                                    "dmabuf_poll_readiness_matrix") < 0)
        goto out_close_dmabuf;

    printf("drmprimeprobe: dmabuf_poll_readiness_matrix "
           "reserve_pending=PASS pending_poll_not_ready=1 "
           "present_signal=PASS ready_poll=1 read_ready=1 write_ready=1 "
           "attempts_delta=%lu ready_delta=%lu not_ready_delta=%lu "
           "read_ready_delta=%lu write_ready_delta=%lu pending_delta=%lu "
           "real_fence_ready_delta=%lu callback_armed_delta=%lu "
           "callback_fired_delta=%lu pending_to_ready_delta=%lu "
           "wakeup_source=present_signal wakeup_source_value=%lu "
           "wakeup_target_fence=%lu wakeup_seq=%lu errors_delta=%lu "
           "reserved_fence=%lu target_fence=%lu read_fence=%lu "
           "write_fence=%lu signaled_fence=%lu "
           "exclusive_fence_snapshot=%lu ordinary_bo_presents_delta=%lu "
           "ordinary_display_presented_delta=%lu "
           "ordinary_display_completed_delta=%lu native_handoff_credit=0 "
           "native_present_credit=0 status=PASS\n",
           after.dmabuf_poll_attempts - before.dmabuf_poll_attempts,
           after.dmabuf_poll_ready - before.dmabuf_poll_ready,
           after.dmabuf_poll_not_ready - before.dmabuf_poll_not_ready,
           after.dmabuf_poll_read_ready - before.dmabuf_poll_read_ready,
           after.dmabuf_poll_write_ready - before.dmabuf_poll_write_ready,
           after.dmabuf_poll_pending - before.dmabuf_poll_pending,
           after.dmabuf_poll_real_fence_ready -
               before.dmabuf_poll_real_fence_ready,
           after.dmabuf_poll_callbacks_armed -
               before.dmabuf_poll_callbacks_armed,
           after.dmabuf_poll_callbacks_fired -
               before.dmabuf_poll_callbacks_fired,
           after.dmabuf_poll_pending_to_ready -
               before.dmabuf_poll_pending_to_ready,
           after.dmabuf_poll_last_callback_source,
           after.dmabuf_poll_last_callback_target_fence,
           after.dmabuf_poll_last_callback_wakeup_seq,
           after.dmabuf_poll_errors - before.dmabuf_poll_errors,
           reserved_fence, after.dmabuf_poll_last_target_fence,
           after.dmabuf_poll_last_read_fence,
           after.dmabuf_poll_last_write_fence,
           after.dmabuf_poll_last_signaled_fence,
           after.dmabuf_last_ttm_resv_exclusive_fence,
           after.bo_presents - before.bo_presents,
           after.display_presented - before.display_presented,
           after.display_completed - before.display_completed);
    ret = 0;

out_close_dmabuf:
    if (dmabuf_fd >= 0)
        close(dmabuf_fd);
out_unreserve:
    if (reserved) {
        memset(&unreserve, 0, sizeof(unreserve));
        unreserve.handle = create.handle;
        unreserve.flags = FB_GPU_TTM_F_UNRESERVE;
        (void)ioctl(fd, FB_GPU_TTM_VALIDATE, &unreserve);
        reserved = 0;
    }
out_destroy:
    if (create.handle != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = create.handle;
        (void)ioctl(fd, FB_GPU_BO_DESTROY, &destroy);
    }
    if (create.addr != 0 && create.size != 0)
        munmap((void *)create.addr, (int)create.size);
out:
    close(fd);
    return ret;
}

static int check_fd_copyout_fault_matrix(void)
{
    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    int gpu_fd = open("/dev/gpu0", O_RDWR);
    struct drm_mode_create_dumb_compat dumb;
    struct drm_mode_destroy_dumb_compat destroy_dumb;
    struct drm_prime_handle_compat prime;
    struct fb_gpu_bo_create bo_create = {
        .width = 16,
        .height = 16,
        .flags = FB_GPU_BO_F_EXPORTABLE,
    };
    struct fb_gpu_bo_export_fd bo_export;
    struct fb_gpu_fence_export_fd fence_export;
    struct fb_gpu_bo_destroy bo_destroy;
    struct kms_credit_snapshot before;
    struct kms_credit_snapshot after;
    void *ro = MAP_FAILED;
    int prime_fd = -1;
    int rc;
    int ret = 1;

    if (drm_fd < 0 || gpu_fd < 0) {
        printf("drmprimeprobe: fd_export_copyout_fault_matrix open=failed\n");
        goto out;
    }

    memset(&dumb, 0, sizeof(dumb));
    dumb.width = 32;
    dumb.height = 32;
    dumb.bpp = 32;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0 ||
        dumb.handle == 0) {
        printf("drmprimeprobe: fd_export_copyout_fault_matrix dumb=failed\n");
        goto out;
    }

    memset(&prime, 0, sizeof(prime));
    prime.handle = dumb.handle;
    if (read_credit_snapshot(&before) < 0)
        goto out_destroy_dumb;
    ro = readonly_arg_page(&prime, sizeof(prime));
    if (ro == MAP_FAILED)
        goto out_destroy_dumb;
    rc = ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, ro);
    free_arg_page(ro);
    ro = MAP_FAILED;
    if (rc >= 0 ||
        wait_dmabuf_visible_close_since(&before, &after,
            "copyout_fault drm_prime_handle_to_fd") < 0 ||
        after.dmabuf_live != before.dmabuf_live ||
        after.bo_fd_live != before.bo_fd_live) {
        printf("drmprimeprobe: fd_export_copyout_fault_matrix "
               "path=drm_prime_handle_to_fd readonly_arg=FAIL rc=%d "
               "bo_fd_live=%lu->%lu dmabuf_live=%lu->%lu\n",
               rc, before.bo_fd_live, after.bo_fd_live,
               before.dmabuf_live, after.dmabuf_live);
        goto out_destroy_dumb;
    }
    printf("drmprimeprobe: fd_export_copyout_fault_matrix "
           "path=drm_prime_handle_to_fd readonly_arg=PASS rc=%d "
           "rc_negative=1 "
           "bo_fd_live_delta=0 dmabuf_live_delta=0 status=PASS\n",
           rc);

    memset(&prime, 0, sizeof(prime));
    prime.handle = dumb.handle;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0 ||
        prime.fd < 0) {
        printf("drmprimeprobe: fd_import_copyout_fault_matrix "
               "prime_export=failed\n");
        goto out_destroy_dumb;
    }
    prime_fd = prime.fd;
    memset(&prime, 0, sizeof(prime));
    prime.fd = prime_fd;
    if (read_credit_snapshot(&before) < 0)
        goto out_destroy_dumb;
    ro = readonly_arg_page(&prime, sizeof(prime));
    if (ro == MAP_FAILED)
        goto out_destroy_dumb;
    rc = ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, ro);
    free_arg_page(ro);
    ro = MAP_FAILED;
    if (read_credit_snapshot(&after) < 0 ||
        rc >= 0 ||
        after.dmabuf_live_attachments != before.dmabuf_live_attachments) {
        printf("drmprimeprobe: fd_import_copyout_fault_matrix "
               "path=drm_prime_fd_to_handle readonly_arg=FAIL rc=%d "
               "live_attachments=%lu->%lu\n",
               rc, before.dmabuf_live_attachments,
               after.dmabuf_live_attachments);
        goto out_destroy_dumb;
    }
    printf("drmprimeprobe: fd_import_copyout_fault_matrix "
           "path=drm_prime_fd_to_handle readonly_arg=PASS rc=%d "
           "rc_negative=1 "
           "dmabuf_live_attachments_delta=0 status=PASS\n",
           rc);

    if (ioctl(gpu_fd, FB_GPU_BO_CREATE, &bo_create) < 0 ||
        bo_create.handle == 0) {
        printf("drmprimeprobe: fd_export_copyout_fault_matrix bo=failed\n");
        goto out_destroy_dumb;
    }

    memset(&bo_export, 0, sizeof(bo_export));
    bo_export.handle = bo_create.handle;
    if (read_credit_snapshot(&before) < 0)
        goto out_destroy_bo;
    ro = readonly_arg_page(&bo_export, sizeof(bo_export));
    if (ro == MAP_FAILED)
        goto out_destroy_bo;
    rc = ioctl(gpu_fd, FB_GPU_BO_EXPORT_FD, ro);
    free_arg_page(ro);
    ro = MAP_FAILED;
    if (rc >= 0 ||
        wait_dmabuf_visible_close_since(&before, &after,
            "copyout_fault fb_bo_export_fd") < 0 ||
        after.dmabuf_live != before.dmabuf_live ||
        after.bo_fd_live != before.bo_fd_live) {
        printf("drmprimeprobe: fd_export_copyout_fault_matrix "
               "path=fb_bo_export_fd readonly_arg=FAIL rc=%d "
               "bo_fd_live=%lu->%lu dmabuf_live=%lu->%lu\n",
               rc, before.bo_fd_live, after.bo_fd_live,
               before.dmabuf_live, after.dmabuf_live);
        goto out_destroy_bo;
    }
    printf("drmprimeprobe: fd_export_copyout_fault_matrix "
           "path=fb_bo_export_fd readonly_arg=PASS rc=%d rc_negative=1 "
           "bo_fd_live_delta=0 dmabuf_live_delta=0 status=PASS\n",
           rc);

    memset(&fence_export, 0, sizeof(fence_export));
    fence_export.handle = bo_create.handle;
    fence_export.fence = 1;
    if (read_credit_snapshot(&before) < 0)
        goto out_destroy_bo;
    ro = readonly_arg_page(&fence_export, sizeof(fence_export));
    if (ro == MAP_FAILED)
        goto out_destroy_bo;
    rc = ioctl(gpu_fd, FB_GPU_FENCE_EXPORT_FD, ro);
    free_arg_page(ro);
    ro = MAP_FAILED;
    if (rc >= 0 ||
        wait_fence_live_since(&before, &after,
            "copyout_fault fb_fence_export_fd") < 0 ||
        after.fence_fd_live != before.fence_fd_live) {
        printf("drmprimeprobe: fd_export_copyout_fault_matrix "
               "path=fb_fence_export_fd readonly_arg=FAIL rc=%d "
               "fence_fd_live=%lu->%lu\n",
               rc, before.fence_fd_live, after.fence_fd_live);
        goto out_destroy_bo;
    }
    printf("drmprimeprobe: fd_export_copyout_fault_matrix "
           "path=fb_fence_export_fd readonly_arg=PASS rc=%d rc_negative=1 "
           "fence_fd_live_delta=0 status=PASS\n",
           rc);

    ret = 0;

out_destroy_bo:
    if (bo_create.handle != 0) {
        memset(&bo_destroy, 0, sizeof(bo_destroy));
        bo_destroy.handle = bo_create.handle;
        (void)ioctl(gpu_fd, FB_GPU_BO_DESTROY, &bo_destroy);
    }
    if (bo_create.addr != 0 && bo_create.size != 0)
        munmap((void *)bo_create.addr, (int)bo_create.size);
out_destroy_dumb:
    if (dumb.handle != 0) {
        memset(&destroy_dumb, 0, sizeof(destroy_dumb));
        destroy_dumb.handle = dumb.handle;
        (void)ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
    }
out:
    free_arg_page(ro);
    if (prime_fd >= 0)
        close(prime_fd);
    if (drm_fd >= 0)
        close(drm_fd);
    if (gpu_fd >= 0)
        close(gpu_fd);
    return ret;
}

static int send_fd_rights(int sock, int fd)
{
    char byte = 'B';
    struct iovec iov;
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    struct cmsghdr *cmsg;

    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    cmsg = (struct cmsghdr *)control;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    cmsg->cmsg_level = DRMPRIMEPROBE_SOL_SOCKET;
    cmsg->cmsg_type = DRMPRIMEPROBE_SCM_RIGHTS;
    memmove(CMSG_DATA(cmsg), &fd, sizeof(fd));

    return sendmsg_raw(sock, &msg, 0);
}

static int recv_fd_rights(int sock, int *fd_out)
{
    char byte = 0;
    struct iovec iov;
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    struct cmsghdr *cmsg;
    int got_fd = -1;
    int ret;

    if (fd_out == 0)
        return -1;
    *fd_out = -1;
    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ret = recvmsg_raw(sock, &msg, DRMPRIMEPROBE_MSG_CMSG_CLOEXEC);
    if (ret != 1 || byte != 'B')
        return -1;
    cmsg = (struct cmsghdr *)control;
    if (msg.msg_controllen < CMSG_LEN(sizeof(got_fd)) ||
        cmsg->cmsg_level != DRMPRIMEPROBE_SOL_SOCKET ||
        cmsg->cmsg_type != DRMPRIMEPROBE_SCM_RIGHTS)
        return -1;
    memmove(&got_fd, CMSG_DATA(cmsg), sizeof(got_fd));
    if (got_fd < 0)
        return -1;
    *fd_out = got_fd;
    return 0;
}

static int check_scm_rights_hidden_ref_lifetime_matrix(void)
{
    int fd1 = open("/dev/gpu0", O_RDWR);
    int fd2 = open("/dev/gpu0", O_RDWR);
    int sv[2] = {-1, -1};
    struct fb_gpu_bo_create create = {
        .width = 16,
        .height = 16,
        .flags = FB_GPU_BO_F_EXPORTABLE,
    };
    struct fb_gpu_bo_export_fd export_fd;
    struct fb_gpu_bo_import_fd import_fd;
    struct fb_gpu_bo_destroy destroy;
    struct kms_credit_snapshot before;
    struct kms_credit_snapshot after_hidden_close;
    struct kms_credit_snapshot after_recv_visible;
    struct kms_credit_snapshot after_final_close;
    int exported_fd = -1;
    int received_fd = -1;
    int ret = 1;

    memset(&import_fd, 0, sizeof(import_fd));
    if (fd1 < 0 || fd2 < 0) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime "
               "open_gpu0=failed\n");
        goto out;
    }
    if (socketpair_raw(sv) < 0) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime "
               "missing_api=SYS_socketpair(AF_UNIX,SOCK_DGRAM) "
               "status=FAIL\n");
        goto out;
    }
    if (read_credit_snapshot(&before) < 0) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime before=failed\n");
        goto out;
    }
    if (ioctl(fd1, FB_GPU_BO_CREATE, &create) < 0 ||
        create.handle == 0 || create.addr == 0 || create.size == 0) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime create=failed\n");
        goto out;
    }

    memset(&export_fd, 0, sizeof(export_fd));
    export_fd.handle = create.handle;
    if (ioctl(fd1, FB_GPU_BO_EXPORT_FD, &export_fd) < 0 ||
        export_fd.fd < 0) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime export=failed "
               "handle=%u\n", create.handle);
        goto out_destroy_create;
    }
    exported_fd = export_fd.fd;

    if (send_fd_rights(sv[0], exported_fd) != 1) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime "
               "missing_api=SYS_sendmsg(SCM_RIGHTS) status=FAIL\n");
        goto out_close_export;
    }

    close(exported_fd);
    exported_fd = -1;
    if (wait_dmabuf_visible_close_since(&before, &after_hidden_close,
            "scm_rights_hidden_ref_lifetime hidden") < 0)
        goto out_destroy_create;

    if (recv_fd_rights(sv[1], &received_fd) < 0) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime "
               "missing_api=SYS_recvmsg(SCM_RIGHTS) status=FAIL\n");
        goto out_destroy_create;
    }
    if (read_credit_snapshot(&after_recv_visible) < 0) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime "
               "after_recv=failed\n");
        goto out_close_received;
    }
    if (after_recv_visible.bo_fd_live < before.bo_fd_live + 1 ||
        after_recv_visible.dmabuf_live < before.dmabuf_live + 1) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime "
               "revisible_accounting=missing bo_fd_live=%lu->%lu "
               "dmabuf_live=%lu->%lu\n",
               before.bo_fd_live, after_recv_visible.bo_fd_live,
               before.dmabuf_live, after_recv_visible.dmabuf_live);
        goto out_close_received;
    }

    memset(&import_fd, 0, sizeof(import_fd));
    import_fd.fd = received_fd;
    if (ioctl(fd2, FB_GPU_BO_IMPORT_FD, &import_fd) < 0 ||
        import_fd.handle == 0 || import_fd.addr == 0 ||
        import_fd.width != create.width || import_fd.height != create.height) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime "
               "revisible_import=failed fd=%d\n", received_fd);
        goto out_close_received;
    }

    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = import_fd.handle;
    if (ioctl(fd2, FB_GPU_BO_DESTROY, &destroy) < 0) {
        printf("drmprimeprobe: scm_rights_hidden_ref_lifetime "
               "destroy_import=failed\n");
        goto out_destroy_import;
    }
    munmap((void *)import_fd.addr, (int)import_fd.size);
    import_fd.handle = 0;
    import_fd.addr = 0;

    close(received_fd);
    received_fd = -1;
    if (wait_dmabuf_visible_close_since(&before, &after_final_close,
            "scm_rights_hidden_ref_lifetime final") < 0)
        goto out_destroy_create;
    printf("drmprimeprobe: scm_rights_hidden_ref_lifetime path=fb_bo "
           "transport=AF_UNIX_SOCK_DGRAM hidden_ref_transfer=PASS "
           "hidden_visible_close=PASS revisible_fd=PASS "
           "revisible_import=PASS final_visible_close=PASS "
           "bo_fd_live=%lu->%lu->%lu->%lu "
           "dmabuf_live=%lu->%lu->%lu->%lu status=PASS\n",
           before.bo_fd_live, after_hidden_close.bo_fd_live,
           after_recv_visible.bo_fd_live, after_final_close.bo_fd_live,
           before.dmabuf_live, after_hidden_close.dmabuf_live,
           after_recv_visible.dmabuf_live, after_final_close.dmabuf_live);
    ret = 0;
    goto out_destroy_create;

out_destroy_import:
    if (import_fd.handle != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = import_fd.handle;
        (void)ioctl(fd2, FB_GPU_BO_DESTROY, &destroy);
    }
    if (import_fd.addr != 0 && import_fd.size != 0)
        munmap((void *)import_fd.addr, (int)import_fd.size);
out_close_received:
    if (received_fd >= 0)
        close(received_fd);
out_close_export:
    if (exported_fd >= 0)
        close(exported_fd);
out_destroy_create:
    if (create.handle != 0) {
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = create.handle;
        (void)ioctl(fd1, FB_GPU_BO_DESTROY, &destroy);
    }
    if (create.addr != 0 && create.size != 0)
        munmap((void *)create.addr, (int)create.size);
out:
    if (sv[0] >= 0)
        close(sv[0]);
    if (sv[1] >= 0)
        close(sv[1]);
    if (fd1 >= 0)
        close(fd1);
    if (fd2 >= 0)
        close(fd2);
    return ret;
}

int main(void)
{
    int fd1 = open("/dev/dri/renderD128", O_RDWR);
    int fd2 = open("/dev/dri/renderD128", O_RDWR);
    struct drm_mode_create_dumb_compat create = {
        .width = 80,
        .height = 48,
        .bpp = 32,
    };
    struct drm_prime_handle_compat prime;
    struct drm_gem_close_compat close_req;
    struct drm_mode_map_dumb_compat map_req;
    struct drm_mode_destroy_dumb_compat destroy_req;
    struct kms_credit_snapshot diag_before;
    struct kms_credit_snapshot diag_after_visible_close;
    struct kms_credit_snapshot diag_after_import;
    struct kms_credit_snapshot diag_after_destroy;
    uint32 *pixels;
    uint32 imported_handle = 0;
    uint32 imported_handle_seen = 0;
    int prime_fd = -1;
    int ret = 1;

    if (fd1 < 0 || fd2 < 0) {
        printf("drmprimeprobe: open render node failed\n");
        goto out;
    }
    if (read_credit_snapshot(&diag_before) < 0) {
        printf("drmprimeprobe: dmabuf_diag before=failed\n");
        goto out;
    }
    if (check_prime_cap(fd1) != 0)
        goto out;
    if (ioctl(fd1, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        create.handle == 0 || create.pitch < create.width * 4 ||
        create.size == 0) {
        printf("drmprimeprobe: create dumb failed handle=%u pitch=%u size=%lu\n",
               create.handle, create.pitch, create.size);
        goto out;
    }

    memset(&prime, 0, sizeof(prime));
    prime.handle = create.handle;
    if (ioctl(fd1, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0 ||
        prime.fd < 0) {
        printf("drmprimeprobe: PRIME_HANDLE_TO_FD failed handle=%u\n",
               create.handle);
        goto out_destroy_fd1;
    }
    prime_fd = prime.fd;
    printf("drmprimeprobe: exported handle=%u fd=%d\n",
           create.handle, prime_fd);

    close(fd1);
    fd1 = -1;

    memset(&close_req, 0, sizeof(close_req));
    close_req.handle = create.handle;
    if (ioctl(fd2, DRM_IOCTL_GEM_CLOSE, &close_req) >= 0) {
        printf("drmprimeprobe: stale handle closed on another render fd\n");
        goto out;
    }
    printf("drmprimeprobe: stale handle rejected on second fd\n");

    memset(&prime, 0, sizeof(prime));
    prime.fd = prime_fd;
    if (ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0 ||
        prime.handle == 0) {
        printf("drmprimeprobe: PRIME_FD_TO_HANDLE failed fd=%d\n", prime_fd);
        goto out;
    }
    imported_handle = prime.handle;
    imported_handle_seen = imported_handle;
    printf("drmprimeprobe: imported fd=%d handle=%u\n",
           prime_fd, imported_handle);

    close(prime_fd);
    prime_fd = -1;
    if (wait_dmabuf_visible_close_since(&diag_before,
                                        &diag_after_visible_close,
                                        "drm_prime") < 0)
        goto out_destroy_import;
    printf("drmprimeprobe: dmabuf_visible_close_matrix path=drm_prime "
           "bo_fd_live=%lu->%lu dmabuf_live=%lu->%lu "
           "status=PASS\n",
           diag_before.bo_fd_live, diag_after_visible_close.bo_fd_live,
           diag_before.dmabuf_live, diag_after_visible_close.dmabuf_live);

    memset(&prime, 0, sizeof(prime));
    prime.fd = prime_fd;
    if (ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) >= 0) {
        printf("drmprimeprobe: closed PRIME fd imported unexpectedly\n");
        goto out_destroy_import;
    }
    printf("drmprimeprobe: closed PRIME fd rejected\n");
    memset(&prime, 0, sizeof(prime));
    prime.fd = fd2;
    if (ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) >= 0) {
        printf("drmprimeprobe: foreign PRIME fd imported unexpectedly\n");
        goto out_destroy_import;
    }
    printf("drmprimeprobe: foreign PRIME fd rejected\n");

    if (wait_dmabuf_releases_since(&diag_before, &diag_after_import, 1,
                                   "dmabuf_diag after_import") < 0) {
        goto out_destroy_import;
    }
    if (check_dmabuf_resv_matrix(&diag_before, &diag_after_import,
                                 FB_GPU_DMABUF_TAG_DRM_PRIME, "drm_prime") < 0)
        goto out_destroy_import;
    if (native_credit_unchanged_since(&diag_before, &diag_after_import,
                                      "dmabuf_diag") < 0)
        goto out_destroy_import;

    memset(&map_req, 0, sizeof(map_req));
    map_req.handle = imported_handle;
    if (ioctl(fd2, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0 ||
        map_req.offset == 0) {
        printf("drmprimeprobe: MAP_DUMB failed handle=%u offset=0x%lx\n",
               imported_handle, map_req.offset);
        goto out_destroy_import;
    }
    pixels = mmap(0, (int)create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd2, map_req.offset);
    if (pixels == MAP_FAILED) {
        printf("drmprimeprobe: mmap imported dumb failed offset=0x%lx size=%lu\n",
               map_req.offset, create.size);
        goto out_destroy_import;
    }
    pixels[0] = 0xff336699;
    pixels[(create.height - 1) * (create.pitch / 4) + (create.width - 1)] =
        0xff996633;
    if (pixels[0] != 0xff336699 ||
        pixels[(create.height - 1) * (create.pitch / 4) + (create.width - 1)] !=
            0xff996633) {
        printf("drmprimeprobe: imported mapping readback mismatch\n");
        munmap((void *)pixels, (int)create.size);
        goto out_destroy_import;
    }
    munmap((void *)pixels, (int)create.size);

    if (check_kms_prime_matrix() != 0)
        goto out_destroy_import;
    if (check_fb_bo_dmabuf_matrix() != 0)
        goto out_destroy_import;
    if (check_stale_fence_wait_matrix() != 0)
        goto out_destroy_import;
    if (check_dmabuf_poll_readiness_matrix() != 0)
        goto out_destroy_import;
    if (check_fd_copyout_fault_matrix() != 0)
        goto out_destroy_import;
    if (check_scm_rights_hidden_ref_lifetime_matrix() != 0)
        goto out_destroy_import;

    memset(&destroy_req, 0, sizeof(destroy_req));
    destroy_req.handle = imported_handle;
    if (ioctl(fd2, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req) < 0) {
        printf("drmprimeprobe: imported destroy failed handle=%u\n",
               imported_handle);
        goto out_destroy_import;
    }
    imported_handle = 0;
    if (read_credit_snapshot(&diag_after_destroy) < 0) {
        printf("drmprimeprobe: dmabuf_diag after_destroy=failed\n");
        goto out_destroy_import;
    }
    if (diag_after_destroy.dmabuf_live_attachments >
        diag_after_import.dmabuf_live_attachments) {
        printf("drmprimeprobe: dmabuf_diag live_attachment_release=failed "
               "before=%lu after=%lu\n",
               diag_after_import.dmabuf_live_attachments,
               diag_after_destroy.dmabuf_live_attachments);
        goto out_destroy_import;
    }
    if (native_credit_unchanged_since(&diag_before, &diag_after_destroy,
                                      "dmabuf_destroy_diag") < 0)
        goto out_destroy_import;
    printf("drmprimeprobe: dmabuf_diag live_attachment_release=ok "
           "live_attachments=%lu\n",
           diag_after_destroy.dmabuf_live_attachments);

    ret = 0;
    printf("drmprimeprobe: ok size=%lu pitch=%u imported=%u\n",
           create.size, create.pitch, imported_handle_seen);

out_destroy_import:
    if (imported_handle != 0) {
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = imported_handle;
        (void)ioctl(fd2, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    }
    goto out;

out_destroy_fd1:
    memset(&destroy_req, 0, sizeof(destroy_req));
    destroy_req.handle = create.handle;
    (void)ioctl(fd1, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

out:
    if (prime_fd >= 0)
        close(prime_fd);
    if (fd1 >= 0)
        close(fd1);
    if (fd2 >= 0)
        close(fd2);
    return ret;
}

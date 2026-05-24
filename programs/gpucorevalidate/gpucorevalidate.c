#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

static int failures;

static int contains(const char *haystack, const char *needle)
{
    uint nlen = strlen(needle);

    if (nlen == 0)
        return 1;
    for (const char *p = haystack; *p; p++) {
        if (memcmp(p, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static void note_fail(const char *step, const char *why)
{
    failures++;
    printf("gpu_core_c_validator step=%s status=FAIL reason=%s\n",
           step, why);
}

static int read_text_file(const char *path, char *buf, int size)
{
    int fd;
    int n;

    if (size <= 0)
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return n;
}

static int wait_for_child(int pid, int *status)
{
    int got;

    for (;;) {
        got = wait(status);
        if (got < 0)
            return -1;
        if (got == pid)
            return 0;
    }
}

static int run_child(const char *name, char **argv)
{
    char path[128];
    int status = 0;
    int pid;
    int rc;

    printf("gpu_core_c_validator step=%s status=RUN cmd=%s", name, argv[0]);
    for (int i = 1; argv[i] != 0; i++)
        printf(" %s", argv[i]);
    printf("\n");

    pid = fork();
    if (pid < 0) {
        note_fail(name, "fork_failed");
        return -1;
    }
    if (pid == 0) {
        snprintf(path, sizeof(path), "/bin/%s", argv[0]);
        exec(path, argv);
        exec(argv[0], argv);
        printf("gpu_core_c_validator child_exec_failed cmd=%s\n", argv[0]);
        exit(127);
    }
    if (wait_for_child(pid, &status) < 0) {
        note_fail(name, "wait_failed");
        return -1;
    }
    if (!WIFEXITED(status)) {
        note_fail(name, "child_not_exited");
        return -1;
    }
    rc = WEXITSTATUS(status);
    if (rc != 0) {
        printf("gpu_core_c_validator step=%s status=FAIL exit=%d raw=0x%x\n",
               name, rc, status);
        failures++;
        return -1;
    }
    printf("gpu_core_c_validator step=%s status=PASS exit=0\n", name);
    return 0;
}

static int run_child_capture(const char *name, char **argv,
                             char *out, int out_size)
{
    char path[128];
    char tmp[256];
    int pipefd[2];
    int status = 0;
    int pid;
    int off = 0;
    int n;
    int rc;

    printf("gpu_core_c_validator step=%s status=RUN_CAPTURE cmd=%s",
           name, argv[0]);
    for (int i = 1; argv[i] != 0; i++)
        printf(" %s", argv[i]);
    printf("\n");

    if (out_size > 0)
        out[0] = 0;
    if (pipe(pipefd) < 0) {
        note_fail(name, "pipe_failed");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        note_fail(name, "fork_failed");
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        close(1);
        dup(pipefd[1]);
        close(2);
        dup(pipefd[1]);
        close(pipefd[1]);
        snprintf(path, sizeof(path), "/bin/%s", argv[0]);
        exec(path, argv);
        exec(argv[0], argv);
        printf("gpu_core_c_validator child_exec_failed cmd=%s\n", argv[0]);
        exit(127);
    }

    close(pipefd[1]);
    for (;;) {
        n = read(pipefd[0], tmp, sizeof(tmp));
        if (n < 0) {
            close(pipefd[0]);
            note_fail(name, "read_failed");
            return -1;
        }
        if (n == 0)
            break;
        write(1, tmp, n);
        if (out_size > 1 && off < out_size - 1) {
            int copy = n;
            if (copy > out_size - 1 - off)
                copy = out_size - 1 - off;
            memmove(out + off, tmp, copy);
            off += copy;
            out[off] = 0;
        }
    }
    close(pipefd[0]);

    if (wait_for_child(pid, &status) < 0) {
        note_fail(name, "wait_failed");
        return -1;
    }
    if (!WIFEXITED(status)) {
        note_fail(name, "child_not_exited");
        return -1;
    }
    rc = WEXITSTATUS(status);
    if (rc != 0) {
        printf("gpu_core_c_validator step=%s status=FAIL exit=%d raw=0x%x\n",
               name, rc, status);
        failures++;
        return -1;
    }
    printf("gpu_core_c_validator step=%s status=PASS exit=0 captured=%d\n",
           name, off);
    return 0;
}

static int require_output_token(const char *step, const char *output,
                                const char *token)
{
    if (contains(output, token))
        return 0;
    failures++;
    printf("gpu_core_c_validator step=%s status=FAIL missing_token=%s\n",
           step, token);
    return -1;
}

static int reject_output_token(const char *step, const char *output,
                               const char *token)
{
    if (!contains(output, token))
        return 0;
    failures++;
    printf("gpu_core_c_validator step=%s status=FAIL unexpected_token=%s\n",
           step, token);
    return -1;
}

static int output_line_has_token(const char *output, const char *anchor,
                                 const char *token)
{
    const char *line = output;

    while (*line) {
        const char *end = line;
        int has_anchor;
        int has_token;

        while (*end && *end != '\n')
            end++;
        has_anchor = 0;
        has_token = 0;
        for (const char *p = line; p < end; p++) {
            uint alen = strlen(anchor);
            uint tlen = strlen(token);
            if (!has_anchor && p + alen <= end &&
                memcmp(p, anchor, alen) == 0)
                has_anchor = 1;
            if (!has_token && p + tlen <= end &&
                memcmp(p, token, tlen) == 0)
                has_token = 1;
        }
        if (has_anchor && has_token)
            return 1;
        line = *end == '\n' ? end + 1 : end;
    }
    return 0;
}

static int require_output_line_token(const char *step, const char *output,
                                     const char *anchor, const char *token)
{
    if (output_line_has_token(output, anchor, token))
        return 0;
    failures++;
    printf("gpu_core_c_validator step=%s status=FAIL "
           "missing_line_token=%s anchor=%s\n",
           step, token, anchor);
    return -1;
}

static int reject_output_line_token(const char *step, const char *output,
                                    const char *anchor, const char *token)
{
    if (!output_line_has_token(output, anchor, token))
        return 0;
    failures++;
    printf("gpu_core_c_validator step=%s status=FAIL "
           "unexpected_line_token=%s anchor=%s\n",
           step, token, anchor);
    return -1;
}

static int parse_u64(const char *p, const char *end, uint64 *value)
{
    uint64 v = 0;
    int digits = 0;

    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (digits == 0)
        return -1;
    *value = v;
    return 0;
}

static int read_line_counter(const char *output, const char *anchor,
                             const char *key, uint64 *value)
{
    const char *line = output;
    uint klen = strlen(key);

    while (*line) {
        const char *end = line;
        int has_anchor = 0;

        while (*end && *end != '\n')
            end++;
        for (const char *p = line; p < end; p++) {
            uint alen = strlen(anchor);

            if (p + alen <= end && memcmp(p, anchor, alen) == 0) {
                has_anchor = 1;
                break;
            }
        }
        if (has_anchor) {
            for (const char *p = line; p + klen + 1 <= end; p++) {
                int boundary = p == line || p[-1] == ' ';

                if (boundary && memcmp(p, key, klen) == 0 &&
                    p[klen] == '=')
                    return parse_u64(p + klen + 1, end, value);
            }
            return -1;
        }
        line = *end == '\n' ? end + 1 : end;
    }
    return -1;
}

static int require_counter(const char *step, const char *output,
                           const char *anchor, const char *key,
                           uint64 *value)
{
    if (read_line_counter(output, anchor, key, value) == 0)
        return 0;
    failures++;
    printf("gpu_core_c_validator step=%s status=FAIL "
           "missing_counter=%s anchor=%s\n", step, key, anchor);
    return -1;
}

static int require_counter_min(const char *step, const char *key,
                               uint64 value, uint64 min)
{
    if (value >= min)
        return 0;
    failures++;
    printf("gpu_core_c_validator step=%s status=FAIL counter=%s "
           "value=%lu expected_min=%lu\n", step, key, value, min);
    return -1;
}

static int require_counter_eq(const char *step, const char *key,
                              uint64 value, uint64 expected)
{
    if (value == expected)
        return 0;
    failures++;
    printf("gpu_core_c_validator step=%s status=FAIL counter=%s "
           "value=%lu expected=%lu\n", step, key, value, expected);
    return -1;
}

static int validate_fbstat_aggregate_matrix(void)
{
    char *fbstat[] = { "fbstat", 0 };
    static char output[65536];
    const char *poll_anchor = "dmabuf_poll_readiness_matrix stats";
    const char *atomic_anchor = "kms_atomic_fence_matrix stats";
    uint64 attempts = 0;
    uint64 ready = 0;
    uint64 not_ready = 0;
    uint64 read_ready = 0;
    uint64 write_ready = 0;
    uint64 pending = 0;
    uint64 errors = 0;
    uint64 target_fence = 0;
    uint64 read_fence = 0;
    uint64 write_fence = 0;
    uint64 signaled_fence = 0;
    uint64 real_fence_ready = 0;
    uint64 callback_armed = 0;
    uint64 callback_fired = 0;
    uint64 pending_to_ready = 0;
    uint64 wake_source = 0;
    uint64 wake_target = 0;
    uint64 wake_seq = 0;
    uint64 fd_refs = 0;
    uint64 fd_ref_puts = 0;
    uint64 duplicate_rejects = 0;
    uint64 test_only_validated = 0;
    uint64 test_only_waits = 0;
    uint64 sync_file_pending_waits = 0;
    uint64 sync_file_pending_wakeups = 0;
    uint64 out_fence_prepared = 0;
    uint64 out_fence_cleanup_closes = 0;
    uint64 test_only_out_fence_placeholders = 0;
    uint64 out_fence_exports = 0;
    uint64 out_fence_display_correlated = 0;
    uint64 out_fence_software_scanout_correlated = 0;
    int before = failures;

    if (run_child_capture("fbstat_aggregate", fbstat, output,
                          sizeof(output)) < 0)
        return -1;

    require_output_line_token("fbstat_dmabuf_poll_readiness_stats", output,
                              poll_anchor, "attempts=");
    require_output_line_token("fbstat_kms_atomic_fence_stats", output,
                              atomic_anchor, "fd_refs=");
    require_output_token("fbstat_backend_gating", output,
                         "backend_opengl_submit 0");
    require_output_token("fbstat_backend_gating", output,
                         "backend_opengl_submit_gate closed");
    require_output_token("fbstat_backend_gating", output,
                         "nouveau_pci_native_present_credit 0");

    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "attempts", &attempts);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "ready", &ready);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "not_ready", &not_ready);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "read_ready", &read_ready);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "write_ready", &write_ready);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "pending", &pending);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "errors", &errors);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "target_fence", &target_fence);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "read_fence", &read_fence);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "write_fence", &write_fence);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "signaled_fence", &signaled_fence);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "real_fence_ready", &real_fence_ready);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "callback_armed", &callback_armed);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "callback_fired", &callback_fired);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "pending_to_ready", &pending_to_ready);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "wake_source", &wake_source);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "wake_target", &wake_target);
    require_counter("fbstat_dmabuf_poll_readiness_stats", output,
                    poll_anchor, "wake_seq", &wake_seq);

    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "fd_refs", &fd_refs);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "fd_ref_puts", &fd_ref_puts);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "duplicate_rejects", &duplicate_rejects);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "test_only_validated", &test_only_validated);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "test_only_waits", &test_only_waits);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "sync_file_pending_waits", &sync_file_pending_waits);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "sync_file_pending_wakeups", &sync_file_pending_wakeups);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "out_fence_prepared", &out_fence_prepared);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "out_fence_cleanup_closes", &out_fence_cleanup_closes);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "test_only_out_fence_placeholders",
                    &test_only_out_fence_placeholders);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "out_fence_exports", &out_fence_exports);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "out_fence_display_correlated",
                    &out_fence_display_correlated);
    require_counter("fbstat_kms_atomic_fence_stats", output, atomic_anchor,
                    "out_fence_software_scanout_correlated",
                    &out_fence_software_scanout_correlated);

    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "attempts",
                        attempts, 2);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "ready",
                        ready, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "not_ready",
                        not_ready, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "read_ready",
                        read_ready, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "write_ready",
                        write_ready, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "pending",
                        pending, 1);
    require_counter_eq("fbstat_dmabuf_poll_readiness_stats", "errors",
                       errors, 0);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "target_fence",
                        target_fence, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "read_fence",
                        read_fence, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "write_fence",
                        write_fence, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats",
                        "signaled_fence", signaled_fence, target_fence);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats",
                        "real_fence_ready", real_fence_ready, 2);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats",
                        "callback_armed", callback_armed, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats",
                        "callback_fired", callback_fired, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats",
                        "pending_to_ready", pending_to_ready, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "wake_source",
                        wake_source, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "wake_target",
                        wake_target, 1);
    require_counter_min("fbstat_dmabuf_poll_readiness_stats", "wake_seq",
                        wake_seq, 1);

    require_counter_min("fbstat_kms_atomic_fence_stats", "fd_refs",
                        fd_refs, 1);
    require_counter_eq("fbstat_kms_atomic_fence_stats", "fd_ref_puts",
                       fd_ref_puts, fd_refs);
    require_counter_min("fbstat_kms_atomic_fence_stats", "duplicate_rejects",
                        duplicate_rejects, 1);
    require_counter_min("fbstat_kms_atomic_fence_stats",
                        "test_only_validated", test_only_validated, 1);
    require_counter_eq("fbstat_kms_atomic_fence_stats", "test_only_waits",
                       test_only_waits, 0);
    require_counter_min("fbstat_kms_atomic_fence_stats",
                        "sync_file_pending_waits",
                        sync_file_pending_waits, 1);
    require_counter_min("fbstat_kms_atomic_fence_stats",
                        "sync_file_pending_wakeups",
                        sync_file_pending_wakeups, 1);
    require_counter_min("fbstat_kms_atomic_fence_stats",
                        "out_fence_prepared", out_fence_prepared, 1);
    require_counter_min("fbstat_kms_atomic_fence_stats",
                        "out_fence_cleanup_closes",
                        out_fence_cleanup_closes, 1);
    require_counter_min("fbstat_kms_atomic_fence_stats",
                        "test_only_out_fence_placeholders",
                        test_only_out_fence_placeholders, 1);
    require_counter_min("fbstat_kms_atomic_fence_stats", "out_fence_exports",
                        out_fence_exports, 1);
    require_counter_eq("fbstat_kms_atomic_fence_stats",
                       "out_fence_display_correlated",
                       out_fence_display_correlated, 0);
    require_counter_min("fbstat_kms_atomic_fence_stats",
                        "out_fence_software_scanout_correlated",
                        out_fence_software_scanout_correlated, 1);

    if (failures != before)
        return -1;

    printf("gpu_core_c_validator fbstat_dmabuf_poll_readiness_stats "
           "attempts=%lu ready=%lu not_ready=%lu read_ready=%lu "
           "write_ready=%lu pending=%lu errors=%lu target_fence=%lu "
           "read_fence=%lu write_fence=%lu signaled_fence=%lu "
           "real_fence_ready=%lu callback_armed=%lu callback_fired=%lu "
           "pending_to_ready=%lu wake_source=%lu wake_target=%lu "
           "wake_seq=%lu status=PASS\n",
           attempts, ready, not_ready, read_ready, write_ready, pending,
           errors, target_fence, read_fence, write_fence, signaled_fence,
           real_fence_ready, callback_armed, callback_fired,
           pending_to_ready, wake_source, wake_target, wake_seq);
    printf("gpu_core_c_validator fbstat_kms_atomic_fence_stats "
           "fd_refs=%lu fd_ref_puts=%lu fd_refs_balanced=PASS "
           "duplicate_rejects=%lu test_only_validated=%lu "
           "test_only_waits=%lu sync_file_pending_waits=%lu "
           "sync_file_pending_wakeups=%lu out_fence_prepared=%lu "
           "out_fence_cleanup_closes=%lu "
           "test_only_out_fence_placeholders=%lu out_fence_exports=%lu "
           "out_fence_display_correlated=%lu "
           "out_fence_software_scanout_correlated=%lu "
           "native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n",
           fd_refs, fd_ref_puts, duplicate_rejects, test_only_validated,
           test_only_waits, sync_file_pending_waits,
           sync_file_pending_wakeups, out_fence_prepared,
           out_fence_cleanup_closes, test_only_out_fence_placeholders,
           out_fence_exports, out_fence_display_correlated,
           out_fence_software_scanout_correlated);
    return 0;
}

static int validate_sync_fd_close_lifetime(void)
{
    char *dxg_import[] = { "dxgprobe", "--import-negative-validate", 0 };
    static char output[32768];
    int before = failures;

    if (run_child_capture("dxg_sync_fd_close_lifetime",
                          dxg_import, output, sizeof(output)) < 0)
        return -1;

    require_output_token("sync_fd_stale_matrix", output,
                         "sync_import_negative_matrix");
    require_output_token("sync_fd_stale_matrix", output,
                         "sync_returned_fd_valid=1");
    require_output_token("sync_fd_stale_matrix", output,
                         "sync_fd_open_after_close=0");
    require_output_token("sync_fd_stale_matrix", output,
                         "stale_sync_fd_valid=1");
    require_output_token("sync_fd_stale_matrix", output,
                         "stale_sync_fd_expected_closed=1");
    require_output_token("sync_fd_stale_matrix", output,
                         "missing_open_rc=-1");
    require_output_token("sync_fd_stale_matrix", output,
                         "wrong_fd_kind=resource_fd");
    require_output_token("sync_fd_stale_matrix", output,
                         "wrong_kind_open_rc=-1");
    require_output_token("sync_fd_stale_matrix", output,
                         "zero_device_open_rc=-1");
    require_output_token("sync_fd_stale_matrix", output,
                         "stale_open_rc=-1");
    require_output_token("sync_fd_stale_matrix", output,
                         "child_parent_device_control=expected_reject");
    require_output_token("sync_fd_stale_matrix", output,
                         "child_parent_device_rc=-1");
    require_output_token("sync_fd_stale_matrix", output,
                         "child_status=0");
    require_output_token("sync_fd_stale_matrix", output,
                         "present_attempted=0");
    require_output_token("sync_fd_stale_matrix", output,
                         "native_present_claim=0");
    require_output_token("sync_fd_stale_matrix", output, "status=PASS");

    require_output_token("opensync_process_namespace_matrix", output,
                         "opensyncobject_source_matrix");
    require_output_token("opensync_process_namespace_matrix", output,
                         "opensync_child_own_dxg_matrix");
    require_output_token("opensync_process_namespace_matrix", output,
                         "expected=allow_same_numeric_child_device");
    require_output_token("opensync_process_namespace_matrix", output,
                         "reason=wsl_consistent_same_numeric_child_device");
    require_output_token("opensync_process_namespace_matrix", output,
                         "used_parent_device=0");
    require_output_token("opensync_process_namespace_matrix", output,
                         "used_child_device=1");
    require_output_token("opensync_process_namespace_matrix", output,
                         "opensync_child_inherited_parent_dxg_matrix");
    require_output_token("opensync_process_namespace_matrix", output,
                         "kernel_namespace_diag_present=1");
    require_output_token("opensync_process_namespace_matrix", output,
                         "expected=reject_inherited_parent_dxg_fd_tgid_guard");

    require_output_token("sync_nt_fd_close_lifetime_matrix", output,
                         "ntshared_close_behavior_matrix");
    require_output_token("sync_nt_fd_close_lifetime_matrix", output,
                         "sync_fd_open_after_close=0");
    require_output_token("sync_nt_fd_close_lifetime_matrix", output,
                         "sync_close_attempted=1");
    require_output_token("sync_nt_fd_close_lifetime_matrix", output,
                         "close_is_user_trigger=1");
    require_output_token("sync_nt_fd_close_lifetime_matrix", output,
                         "destroy_separation_matrix");
    require_output_token("sync_nt_fd_close_lifetime_matrix", output,
                         "sync_fd_close_attempted=1");
    require_output_token("sync_nt_fd_close_lifetime_matrix", output,
                         "sync_d3dkmt_destroy_rc=0");
    require_output_token("sync_nt_fd_close_lifetime_matrix", output,
                         "nt_fd_close_is_separate_from_d3dkmt_destroy=1");

    if (failures != before)
        return -1;

    printf("gpu_core_c_validator sync_fd_stale_matrix "
           "sync_returned_fd_valid=1 sync_fd_open_after_close=0 "
           "stale_sync_fd_valid=1 stale_sync_fd_expected_closed=1 "
           "missing_open_rc=-1 wrong_kind_resource_fd_rc=-1 "
           "zero_device_open_rc=-1 stale_open_rc=-1 "
           "present_attempted=0 native_present_claim=0 "
           "resource_fd_scope=separate status=PASS\n");
    printf("gpu_core_c_validator opensync_process_namespace_matrix "
           "own_dxg=PASS inherited_parent_dxg=PASS child_open_rc_own=0 "
           "child_open_rc_inherited=-1 kernel_namespace_diag_present=1 "
           "used_parent_device_split=1 used_child_device_split=1 "
           "resource_fd_scope=separate status=PASS\n");
    printf("gpu_core_c_validator sync_nt_fd_close_lifetime_matrix "
           "sync_fd_close_attempted=1 sync_fd_open_after_close=0 "
           "sync_d3dkmt_destroy_rc=0 "
           "nt_fd_close_is_separate_from_d3dkmt_destroy=1 "
           "close_is_user_trigger=1 resource_fd_scope=separate "
           "status=PASS\n");
    return 0;
}

static int validate_prime_kms_matrix(void)
{
    char *prime[] = { "drmprimeprobe", 0 };
    static char output[32768];
    int before = failures;

    if (run_child_capture("prime", prime, output, sizeof(output)) < 0)
        return -1;

    require_output_token("prime_kms_multiplane_matrix", output,
                         "kms_multiplane_matrix separate_plane_nv12=fail_closed");
    require_output_token("prime_kms_multiplane_matrix", output,
                         "separate_plane_native_present_credit=0");
    require_output_token("prime_kms_multiplane_matrix", output,
                         "separate_plane_scanout_credit=0");
    require_output_token("prime_kms_multiplane_matrix", output,
                         "kms_multiplane_matrix separate_plane_native_present_credit=0");
    require_output_token("prime_kms_modifier_matrix", output,
                         "kms_modifier_matrix linear_nv12=accepted");
    require_output_token("prime_kms_modifier_matrix", output,
                         "getfb2_nv12_metadata=ok");
    require_output_token("prime_kms_modifier_matrix", output,
                         "kms_modifier_matrix nonlinear_modifier=rejected");
    require_output_token("prime_kms_modifier_matrix", output,
                         "kms_modifier_matrix mixed_plane_modifier=rejected");
    require_output_token("prime_kms_modifier_matrix", output,
                         "kms_modifier_matrix status=PASS");
    require_output_token("prime_dmabuf_resv_matrix", output,
                         "dmabuf_resv_matrix path=drm_prime");
    require_output_token("prime_dmabuf_resv_matrix", output,
                         "dmabuf_resv_matrix path=fb_bo");
    require_output_token("prime_dmabuf_visible_close_matrix", output,
                         "dmabuf_visible_close_matrix path=drm_prime");
    require_output_token("prime_dmabuf_visible_close_matrix", output,
                         "dmabuf_visible_close_matrix path=fb_bo");
    require_output_token("prime_dmabuf_visible_close_matrix", output,
                         "bo_fd_live=");
    require_output_token("prime_dmabuf_visible_close_matrix", output,
                         "dmabuf_live=");
    require_output_token("prime_dmabuf_visible_close_matrix", output,
                         "status=PASS");
    require_output_token("prime_dmabuf_resv_matrix", output,
                         "shared_fence_semantics=");
    require_output_token("prime_dmabuf_resv_matrix", output,
                         "wait_queue_semantics=");
    require_output_token("prime_dmabuf_resv_matrix", output,
                         "shared_fence=");
    require_output_token("prime_dmabuf_resv_matrix", output,
                         "shared_count=");
    require_output_token("prime_dmabuf_resv_matrix", output,
                         "attach_export_delta=");
    require_output_token("prime_dmabuf_resv_matrix", output,
                         "attach_import_delta=");
    require_output_token("prime_dmabuf_resv_matrix", output,
                         "native_present_credit=0 status=PASS");
    require_output_token("prime_kms_resv_attach_matrix", output,
                         "kms_resv_attach_matrix");
    require_output_token("prime_kms_resv_attach_matrix", output,
                         "kms_pin_delta=");
    require_output_token("prime_kms_resv_attach_matrix", output,
                         "kms_unpin_delta=");
    require_output_token("prime_kms_resv_attach_matrix", output,
                         "native_present_credit=0 status=PASS");
    require_output_token("prime_stale_fence_wait_matrix", output,
                         "stale_fence_wait_matrix");
    require_output_token("prime_stale_fence_wait_matrix", output,
                         "future_wait_rejected=1");
    require_output_token("prime_stale_fence_wait_matrix", output,
                         "closed_fd_rejected=1");
    require_output_token("prime_stale_fence_wait_matrix", output,
                         "stale_fence_rejects_delta=");
    require_output_token("prime_stale_fence_wait_matrix", output,
                         "native_present_credit=0 status=PASS");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "dmabuf_poll_readiness_matrix");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "reserve_pending=PASS");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "pending_poll_not_ready=1");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "present_signal=PASS");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "ready_poll=1");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "read_ready=1");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "write_ready=1");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "attempts_delta=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "not_ready_delta=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "ready_delta=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "read_ready_delta=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "write_ready_delta=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "pending_delta=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "real_fence_ready_delta=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "callback_armed_delta=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "callback_fired_delta=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "pending_to_ready_delta=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "wakeup_source=present_signal");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "wakeup_target_fence=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "wakeup_seq=");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "errors_delta=0");
    require_output_token("prime_dmabuf_poll_readiness_matrix", output,
                         "native_present_credit=0 status=PASS");
    require_output_token("prime_fd_copyout_cleanup_matrix", output,
                         "fd_export_copyout_fault_matrix "
                         "path=drm_prime_handle_to_fd");
    require_output_token("prime_fd_copyout_cleanup_matrix", output,
                         "fd_import_copyout_fault_matrix "
                         "path=drm_prime_fd_to_handle");
    require_output_token("prime_fd_copyout_cleanup_matrix", output,
                         "fd_export_copyout_fault_matrix "
                         "path=fb_bo_export_fd");
    require_output_token("prime_fd_copyout_cleanup_matrix", output,
                         "fd_export_copyout_fault_matrix "
                         "path=fb_fence_export_fd");
    require_output_token("prime_fd_copyout_cleanup_matrix", output,
                         "readonly_arg=PASS");
    require_output_token("prime_fd_copyout_cleanup_matrix", output,
                         "dmabuf_live_delta=0");
    require_output_token("prime_fd_copyout_cleanup_matrix", output,
                         "fence_fd_live_delta=0");
    require_output_token("prime_scm_rights_hidden_ref_lifetime", output,
                         "scm_rights_hidden_ref_lifetime path=fb_bo");
    require_output_line_token("prime_scm_rights_hidden_ref_lifetime", output,
                              "scm_rights_hidden_ref_lifetime",
                              "transport=AF_UNIX_SOCK_DGRAM");
    require_output_line_token("prime_scm_rights_hidden_ref_lifetime", output,
                              "scm_rights_hidden_ref_lifetime",
                              "hidden_ref_transfer=PASS");
    require_output_line_token("prime_scm_rights_hidden_ref_lifetime", output,
                              "scm_rights_hidden_ref_lifetime",
                              "hidden_visible_close=PASS");
    require_output_line_token("prime_scm_rights_hidden_ref_lifetime", output,
                              "scm_rights_hidden_ref_lifetime",
                              "revisible_fd=PASS");
    require_output_line_token("prime_scm_rights_hidden_ref_lifetime", output,
                              "scm_rights_hidden_ref_lifetime",
                              "revisible_import=PASS");
    require_output_line_token("prime_scm_rights_hidden_ref_lifetime", output,
                              "scm_rights_hidden_ref_lifetime",
                              "final_visible_close=PASS");
    require_output_line_token("prime_scm_rights_hidden_ref_lifetime", output,
                              "scm_rights_hidden_ref_lifetime",
                              "status=PASS");
    require_output_token("prime_kms_matrix", output,
                         "drmprimeprobe: ok");

    if (failures != before)
        return -1;

    printf("gpu_core_c_validator prime_kms_matrix "
           "separate_plane_nv12=fail_closed "
           "separate_plane_native_present_credit=0 "
           "separate_plane_scanout_credit=0 "
           "linear_modifier=accepted nonlinear_modifier=rejected "
           "mixed_plane_modifier=rejected getfb2_nv12_metadata=ok "
           "dmabuf_visible_close=PASS dmabuf_resv_attach=PASS "
           "kms_resv_attach=PASS fd_copyout_cleanup=PASS "
           "stale_fence_reject=PASS dmabuf_poll_readiness=PASS "
           "scm_rights_hidden_ref=PASS "
           "status=PASS\n");
    printf("gpu_core_c_validator prime_dmabuf_poll_readiness_matrix "
           "reserve_pending=PASS pending_poll_not_ready=1 "
           "present_signal=PASS ready_poll=1 read_ready=1 write_ready=1 "
           "callback_armed=PASS callback_fired=PASS "
           "pending_to_ready=PASS wakeup_source=present_signal errors_delta=0 "
           "native_present_credit=0 status=PASS\n");
    printf("gpu_core_c_validator prime_scm_rights_hidden_ref_lifetime "
           "transport=AF_UNIX_SOCK_DGRAM hidden_ref_transfer=PASS "
           "hidden_visible_close=PASS revisible_fd=PASS "
           "revisible_import=PASS final_visible_close=PASS status=PASS\n");
    return 0;
}

static int validate_drm_syncobj_matrix(void)
{
    char *drm[] = { "drmiftest", 0 };
    static char output[49152];
    int before = failures;

    if (run_child_capture("drm_gem_kms", drm, output, sizeof(output)) < 0)
        return -1;

    require_output_token("drm_syncobj_wait_matrix", output,
                         "syncobj_wait_matrix");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "finite_timeout_rejected=1");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "stale_handle_rejected=1");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "future_timeline_rejected=1");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "sync_file_exports_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "sync_file_imports_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "attach_sync_file_export_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "attach_sync_file_import_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "wait_queued_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "wait_wakeups_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "wait_callbacks_armed_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "wait_callbacks_fired_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "wait_callbacks_cancelled_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "wait_callback_late_delta=0");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "signal_wakeup=PASS");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "transfer_wakeup=PASS");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "pending_transfer=PASS");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "timeout_separate=PASS");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "timeout_waits_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "stale_wait_rejects_delta=");
    require_output_token("drm_syncobj_wait_matrix", output,
                         "status=PASS");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "syncobj_wakeup_provenance_matrix");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "signal_wait_queued=PASS");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "signal_wake=PASS");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "transfer_wait_queued=PASS");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "transfer_wake=PASS");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "pending_transfer_wake=PASS");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "wait_queued_delta=");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "wakeups_delta=");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "wait_callbacks_armed_delta=");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "wait_callbacks_fired_delta=");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "wait_callbacks_cancelled_delta=");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_syncobj_wakeup_provenance_matrix", output,
                         "opengl_submit_credit=0 status=PASS");
    require_output_token("drm_sync_file_pending_matrix", output,
                         "sync_file_pending_matrix");
    require_output_token("drm_sync_file_pending_matrix", output,
                         "pending_export=PASS");
    require_output_token("drm_sync_file_pending_matrix", output,
                         "pending_poll_not_ready=1");
    require_output_token("drm_sync_file_pending_matrix", output,
                         "pending_import=PASS");
    require_output_token("drm_sync_file_pending_matrix", output,
                         "imported_wait_pending=PASS");
    require_output_token("drm_sync_file_pending_matrix", output,
                         "pending_poll_ready=1");
    require_output_token("drm_sync_file_pending_matrix", output,
                         "imported_wait_after_signal=PASS");
    require_output_token("drm_sync_file_pending_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_sync_file_pending_matrix", output,
                         "opengl_submit_credit=0 status=PASS");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "sync_file_callback_matrix");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "callback_arm=PASS");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "callback_fire=PASS");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "callback_cancel=PASS");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "late_fire_delta=0");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "pending_callbacks_armed_delta=2");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "pending_callbacks_fired_delta=1");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "pending_callbacks_cancelled_delta=1");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "syncobj_live_delta=0");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_sync_file_callback_matrix", output,
                         "opengl_submit_credit=0 status=PASS");
    require_output_token("drm_syncobj_pending_transfer_matrix", output,
                         "syncobj_pending_transfer_matrix");
    require_output_token("drm_syncobj_pending_transfer_matrix", output,
                         "transfer_before_signal=PASS");
    require_output_token("drm_syncobj_pending_transfer_matrix", output,
                         "dst_wait_pending=PASS");
    require_output_token("drm_syncobj_pending_transfer_matrix", output,
                         "source_signal=PASS");
    require_output_token("drm_syncobj_pending_transfer_matrix", output,
                         "child_wait_woke=PASS");
    require_output_token("drm_syncobj_pending_transfer_matrix", output,
                         "dst_wait_after_signal=PASS");
    require_output_token("drm_syncobj_pending_transfer_matrix", output,
                         "pending_transfers_delta=");
    require_output_token("drm_syncobj_pending_transfer_matrix", output,
                         "pending_transfer_wakeups_delta=");
    require_output_token("drm_syncobj_pending_transfer_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_syncobj_pending_transfer_matrix", output,
                         "opengl_submit_credit=0 status=PASS");
    require_output_token("drm_syncobj_fd_kind_matrix", output,
                         "syncobj_fd_kind_matrix");
    require_output_token("drm_syncobj_fd_kind_matrix", output,
                         "opaque_roundtrip=PASS");
    require_output_token("drm_syncobj_fd_kind_matrix", output,
                         "sync_file_roundtrip=PASS");
    require_output_token("drm_syncobj_fd_kind_matrix", output,
                         "cross_kind_rejected=2");
    require_output_token("drm_syncobj_fd_kind_matrix", output,
                         "bad_flags_rejected=2");
    require_output_token("drm_syncobj_fd_kind_matrix", output,
                         "closed_fd_rejected=1");
    require_output_token("drm_syncobj_fd_kind_matrix", output,
                         "point_without_timeline_rejected=2");
    require_output_token("drm_syncobj_fd_kind_matrix", output,
                         "timeline_sync_file_roundtrip=PASS");
    require_output_token("drm_syncobj_fd_kind_matrix", output,
                         "eventfd=fail-closed");
    require_output_token("drm_syncobj_fd_kind_matrix", output,
                         "status=PASS");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "syncobj_copyout_cleanup_matrix");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "readonly_arg=PASS");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "create_copyout_fault=PASS");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "handle_to_fd_copyout_fault=PASS");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "fd_to_handle_copyout_fault=PASS");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "create_rc_lt_0=1");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "handle_to_fd_rc_lt_0=1");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "fd_to_handle_rc_lt_0=1");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "syncobj_live_delta=0");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "fence_fd_live_delta=0");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "leak_guard=PASS");
    require_output_token("drm_syncobj_copyout_cleanup_matrix", output,
                         "status=PASS");
    require_output_token("drm_vblank_source_matrix", output,
                         "vblank_source_matrix");
    require_output_token("drm_vblank_source_matrix", output,
                         "synthetic=0");
    require_output_token("drm_vblank_source_matrix", output,
                         "display_correlated=1");
    require_output_token("drm_vblank_source_matrix", output,
                         "display_completion_correlated=PASS");
    require_output_token("drm_vblank_source_matrix", output,
                         "crtc_queue_sequence_fail_closed=PASS");
    require_output_token("drm_vblank_source_matrix", output,
                         "crtc_queue_sequence_rejects_delta=");
    require_output_token("drm_vblank_source_matrix", output,
                         "crtc_queue_sequence_bad_flags_delta=");
    require_output_token("drm_vblank_source_matrix", output,
                         "crtc_queue_sequence_noevent_delta=");
    require_output_token("drm_vblank_source_matrix", output,
                         "page_flip_decoupled=PASS");
    require_output_token("drm_vblank_source_matrix", output,
                         "page_flip_target_fail_closed=PASS");
    require_output_token("drm_vblank_source_matrix", output,
                         "page_flip_target_rejects_delta=");
    require_output_token("drm_vblank_source_matrix", output,
                         "page_flip_async_fail_closed=PASS");
    require_output_token("drm_vblank_source_matrix", output,
                         "page_flip_async_rejects_delta=");
    require_output_token("drm_vblank_source_matrix", output,
                         "page_flip_invalid_fb_no_event=PASS");
    require_output_token("drm_vblank_source_matrix", output,
                         "page_flip_invalid_noevent_rejects_delta=");
    require_output_token("drm_vblank_source_matrix", output,
                         "page_flip_negative_events_delta=0");
    require_output_token("drm_vblank_source_matrix", output,
                         "page_flip_negative_flips_delta=0");
    require_output_token("drm_vblank_source_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_vblank_source_matrix", output,
                         "status=PASS");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output,
                         "kms_present_completion_failclosed_matrix");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "unsupported_format=NV12");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "setcrtc_present_fail_closed=PASS");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "plane_formats_scanout_exclude_nv12=PASS");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "page_flip_present_fail_closed=PASS");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "page_flip_no_event=PASS");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "page_flip_events_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "page_flip_flips_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output,
                         "atomic_test_only_present_fail_closed=PASS");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "atomic_test_only_out_fence_user_fd=-1");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "atomic_present_fail_closed=PASS");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "atomic_event_noevent=PASS");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "atomic_out_fence_present_fail_exported=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "atomic_out_fence_user_fd=-1");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "atomic_commits_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "atomic_state_unchanged=PASS");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "in_fence_fd_refs_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "in_fence_fd_ref_puts_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "in_fence_test_only_validated_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "in_fence_test_only_waits_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "in_fence_pending_waits_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "in_fence_pending_wakeups_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "out_fence_prepared_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "out_fence_cleanup_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "out_fence_test_only_placeholders_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "out_fence_exports_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "out_fence_display_correlated_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output,
                         "out_fence_software_scanout_correlated_delta=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "display_delta=0/0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "dxg_present_delta=0/0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "native_present_credit=0");
    require_output_token("drm_kms_present_completion_failclosed_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("drm_kms_present_completion_failclosed_matrix",
                              output,
                              "kms_present_completion_failclosed_matrix",
                              "status=PASS");
    require_output_token("drm_fence_lifetime_matrix", output,
                         "fence_lifetime_matrix");
    if (contains(output, "fence_lifetime_kernel=real")) {
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_lifetime_kernel=real");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_export=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_query=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_second_query=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_dup_query=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_close_released=1");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_closed_rejected=1");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "syncobj_import=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "syncobj_wait=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "syncobj_export=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "syncobj_export_query=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "syncobj_destroy=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "leak_guard=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "poll_coverage=");
        require_output_line_token("drm_fence_lifetime_matrix", output,
                                  "fence_lifetime_matrix", "status=PASS");
        reject_output_line_token("drm_fence_lifetime_matrix", output,
                                 "fence_lifetime_matrix",
                                 "status=DEFERRED");
        reject_output_line_token("drm_fence_lifetime_matrix", output,
                                 "fence_lifetime_matrix", "status=MISSING");
    } else if (contains(output, "fence_lifetime_kernel=MISSING")) {
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_lifetime_kernel=MISSING");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_export=MISSING");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "syncobj_import=MISSING");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "poll_coverage=DEFERRED");
        require_output_line_token("drm_fence_lifetime_matrix", output,
                                  "fence_lifetime_matrix", "status=MISSING");
    } else {
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_lifetime_kernel=DEFERRED");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_export=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_query=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_second_query=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_dup_query=PASS");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_close_released=1");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "fence_fd_closed_rejected=1");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "syncobj_import=DEFERRED");
        require_output_token("drm_fence_lifetime_matrix", output,
                             "poll_coverage=");
        require_output_line_token("drm_fence_lifetime_matrix", output,
                                  "fence_lifetime_matrix",
                                  "status=DEFERRED");
    }
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "fence_callback_lifecycle_matrix");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "add_pending=PASS");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "fire_on_signal=PASS");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "remove_before_signal=PASS");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "late_after_signal=PASS");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "callbacks_added_delta=2");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "callbacks_fired_delta=1");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "callbacks_removed_delta=1");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "callbacks_late_delta=1");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "callback_errors_delta=0");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "fence_fd_live_delta=0");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "fence_objects_live_delta=0");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_fence_callback_lifecycle_matrix", output,
                         "opengl_submit_credit=0 status=PASS");
    require_output_token("drm_atomic_fence_matrix", output,
                         "atomic_fence_matrix");
    require_output_token("drm_atomic_fence_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_atomic_fence_matrix", output,
                         "opengl_submit_credit=0");
    require_output_token("drm_minor_matrix", output,
                         "drm_minor_matrix");
    require_output_token("drm_minor_matrix", output,
                         "primary_index=0");
    require_output_token("drm_minor_matrix", output,
                         "render_index=128");
    require_output_token("drm_minor_matrix", output,
                         "control_index=64");
    require_output_token("drm_minor_matrix", output,
                         "control_registered=0");
    require_output_token("drm_minor_matrix", output,
                         "control_open=fail_closed");
    require_output_token("drm_minor_matrix", output,
                         "static_nodes=2");
    require_output_token("drm_minor_matrix", output,
                         "dynamic_nodes=0");
    require_output_token("drm_minor_matrix", output,
                         "status=PASS");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "lease_failclosed_matrix");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "lease_kernel=absent");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "primary_create_objects_rejected=1");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "primary_create_empty_rejected=1");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "render_create_rejected=1");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "list_lessees_rejected=1");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "get_lease_rejected=1");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "revoke_lease_rejected=1");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "lease_fd_created=0");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "lessee_id_created=0");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "active_leases_delta=0");
    require_output_token("drm_lease_failclosed_matrix", output,
                         "native_present_credit=0");
    require_output_line_token("drm_lease_failclosed_matrix", output,
                              "lease_failclosed_matrix", "status=PASS");
    if (contains(output, "atomic_fence_kernel=real")) {
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_in_fence_accepted=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_exported=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_query_ok=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_software=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_immediate=0");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_stale_in_fence_rejected=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_duplicate_in_fence_rejected=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_duplicate_in_fence_rejects_delta=");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_in_fence_fd_ref_prepare=PASS");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_in_fence_fd_refs_delta=");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_in_fence_fd_ref_puts_delta=");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_test_only_in_fence_validated=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_test_only_in_fence_no_wait=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_test_only_in_fence_waited=0");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_test_only_in_fence_validated_delta=");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_test_only_out_fence_placeholder=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_test_only_out_fence_placeholder_delta=");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_test_only_state_unchanged=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_invalid_out_fence_rejected=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_invalid_out_fence_state_unchanged=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_prepared=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_prepared_delta=");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_cleanup_closes_delta=");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_display_correlated=0");
        require_output_token("drm_atomic_fence_matrix", output,
                             "out_fence_display_correlated_delta=0");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_software_scanout_correlated=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "out_fence_software_scanout_correlated_delta=");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_fence_kernel=real");
        require_output_line_token("drm_atomic_fence_matrix", output,
                                  "atomic_fence_matrix", "status=PASS");
        reject_output_token("drm_atomic_fence_matrix", output,
                            "atomic_out_fence_placeholder=1");
        reject_output_token("drm_atomic_fence_matrix", output,
                            "atomic_fence_kernel=placeholder");
        reject_output_token("drm_atomic_fence_matrix", output,
                            "atomic_fence_kernel=missing_fields");
        reject_output_line_token("drm_atomic_fence_matrix", output,
                                 "atomic_fence_matrix", "status=DEFERRED");
    } else if (contains(output, "atomic_fence_kernel=missing_fields")) {
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_fence_kernel=missing_fields");
        require_output_line_token("drm_atomic_fence_matrix", output,
                                  "atomic_fence_matrix",
                                  "status=DEFERRED");
        reject_output_token("drm_atomic_fence_matrix", output,
                            "atomic_out_fence_exported=1");
    } else {
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_in_fence_rejected=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_in_fence_closed_rejected=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_stale_in_fence_rejected=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_nonblock_fence_fail_closed=1");
        require_output_line_token("drm_atomic_fence_matrix", output,
                                  "atomic_fence_matrix",
                                  "status=DEFERRED");
        reject_output_token("drm_atomic_fence_matrix", output,
                            "atomic_out_fence_exported=1");
    }
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "atomic_out_fence_provenance_matrix");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_source=software_scanout_commit");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_software=1");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_immediate=0");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_display_correlated=0");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_software_scanout_correlated=1");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_completion_deferred=0");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_display_correlated_delta=0");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_software_scanout_correlated_delta=");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "opengl_submit_credit=0");
    require_output_line_token("drm_atomic_out_fence_provenance_matrix",
                              output, "atomic_out_fence_provenance_matrix",
                              "status=PASS");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "kms_sync_file_in_fence_matrix");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "pending_sync_file_in_fence=PASS");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "pending_in_fence_rejected=PASS");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "signal_wake=PASS");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "commit_after_signal=PASS");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "in_fence_fd_ref_prepare=PASS");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "pending_waits_delta=");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "pending_wakeups_delta=");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "test_only_no_wait=1");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_kms_sync_file_in_fence_matrix", output,
                         "opengl_submit_credit=0");
    require_output_line_token("drm_kms_sync_file_in_fence_matrix", output,
                              "kms_sync_file_in_fence_matrix",
                              "status=PASS");
    require_output_token("drm_syncobj", output, "drmiftest: syncobj ok");

    if (failures != before)
        return -1;

    printf("gpu_core_c_validator drm_syncobj_wait_matrix "
           "finite_timeout_rejected=1 stale_handle_rejected=1 "
           "future_timeline_rejected=1 sync_file_resv_attach=PASS "
           "wait_queue_timeout_diag=PASS signal_wakeup=PASS "
           "transfer_wakeup=PASS pending_transfer=PASS "
           "wait_callback_lifecycle=PASS "
           "timeout_separate=PASS status=PASS\n");
    printf("gpu_core_c_validator drm_syncobj_wakeup_provenance_matrix "
           "signal_wait_queued=PASS signal_wake=PASS "
           "transfer_wait_queued=PASS transfer_wake=PASS "
           "pending_transfer_wake=PASS "
           "wait_callback_lifecycle=PASS "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator drm_syncobj_pending_transfer_matrix "
           "transfer_before_signal=PASS dst_wait_pending=PASS "
           "source_signal=PASS child_wait_woke=PASS "
           "dst_wait_after_signal=PASS native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator drm_sync_file_pending_matrix "
           "pending_export=PASS pending_poll_not_ready=1 "
           "pending_import=PASS imported_wait_pending=PASS "
           "pending_poll_ready=1 imported_wait_after_signal=PASS "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator drm_sync_file_callback_matrix "
           "callback_arm=PASS callback_fire=PASS callback_cancel=PASS "
           "late_fire_delta=0 pending_callbacks_armed_delta=2 "
           "pending_callbacks_fired_delta=1 "
           "pending_callbacks_cancelled_delta=1 syncobj_live_delta=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator drm_syncobj_fd_kind_matrix "
           "opaque_roundtrip=PASS sync_file_roundtrip=PASS "
           "cross_kind_rejected=2 bad_flags_rejected=2 "
           "closed_fd_rejected=1 point_without_timeline_rejected=2 "
           "timeline_sync_file_roundtrip=PASS eventfd=fail-closed "
           "status=PASS\n");
    printf("gpu_core_c_validator drm_syncobj_copyout_cleanup_matrix "
           "readonly_arg=PASS create_copyout_fault=PASS "
           "handle_to_fd_copyout_fault=PASS fd_to_handle_copyout_fault=PASS "
           "create_rc_lt_0=1 handle_to_fd_rc_lt_0=1 "
           "fd_to_handle_rc_lt_0=1 "
           "syncobj_live_delta=0 fence_fd_live_delta=0 "
           "leak_guard=PASS status=PASS\n");
    printf("gpu_core_c_validator drm_vblank_source_matrix "
           "synthetic=0 display_correlated=1 "
           "display_completion_correlated=PASS page_flip_decoupled=PASS "
           "crtc_queue_sequence_fail_closed=PASS "
           "page_flip_target_fail_closed=PASS "
           "page_flip_async_fail_closed=PASS "
           "page_flip_invalid_fb_no_event=PASS "
           "native_present_credit=0 status=PASS\n");
    printf("gpu_core_c_validator drm_minor_matrix "
           "primary_index=0 render_index=128 control_index=64 "
           "control_registered=0 control_open=fail_closed "
           "static_nodes=2 dynamic_nodes=0 status=PASS\n");
    printf("gpu_core_c_validator drm_lease_failclosed_matrix "
           "lease_kernel=absent primary_create_objects_rejected=1 "
           "primary_create_empty_rejected=1 render_create_rejected=1 "
           "list_lessees_rejected=1 get_lease_rejected=1 "
           "revoke_lease_rejected=1 lease_fd_created=0 "
           "lessee_id_created=0 active_leases_delta=0 "
           "native_present_credit=0 status=PASS\n");
    if (contains(output, "fence_lifetime_kernel=real")) {
        printf("gpu_core_c_validator drm_fence_lifetime_matrix "
               "fence_fd_lifetime=PASS syncobj_bridge=PASS "
               "leak_guard=PASS status=PASS\n");
    } else if (contains(output, "fence_lifetime_kernel=MISSING")) {
        printf("gpu_core_c_validator drm_fence_lifetime_matrix "
               "fence_fd_lifetime=MISSING syncobj_bridge=MISSING "
               "status=MISSING\n");
    } else {
        printf("gpu_core_c_validator drm_fence_lifetime_matrix "
               "fence_fd_lifetime=PASS syncobj_bridge=DEFERRED "
               "status=DEFERRED\n");
    }
    printf("gpu_core_c_validator drm_fence_callback_lifecycle_matrix "
           "add_pending=PASS fire_on_signal=PASS "
           "remove_before_signal=PASS late_after_signal=PASS "
           "callbacks_added_delta=2 callbacks_fired_delta=1 "
           "callbacks_removed_delta=1 callbacks_late_delta=1 "
           "callback_errors_delta=0 fence_fd_live_delta=0 "
           "fence_objects_live_delta=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    if (contains(output, "atomic_fence_kernel=real")) {
        printf("gpu_core_c_validator drm_atomic_fence_matrix "
               "atomic_in_fence_accepted=1 atomic_in_fence_rejected=1 "
               "atomic_stale_in_fence_rejected=1 "
               "atomic_duplicate_in_fence_rejected=1 "
               "atomic_in_fence_fd_ref_prepare=PASS "
               "atomic_test_only_in_fence_validated=1 "
               "atomic_test_only_in_fence_no_wait=1 "
               "atomic_test_only_in_fence_waited=0 "
               "atomic_out_fence_exported=1 "
               "atomic_test_only_out_fence_placeholder=1 "
               "atomic_out_fence_prepared=1 "
               "atomic_out_fence_cleanup=PASS "
               "atomic_invalid_out_fence_rejected=1 "
               "atomic_out_fence_software=1 "
               "atomic_out_fence_immediate=0 "
               "atomic_out_fence_display_correlated=0 "
               "atomic_out_fence_software_scanout_correlated=1 "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=PASS\n");
    } else if (contains(output, "atomic_fence_kernel=missing_fields")) {
        printf("gpu_core_c_validator drm_atomic_fence_matrix "
               "kernel_fields=missing native_present_credit=0 "
               "opengl_submit_credit=0 status=DEFERRED\n");
    } else {
        printf("gpu_core_c_validator drm_atomic_fence_matrix "
               "atomic_in_fence_rejected=1 "
               "atomic_in_fence_closed_rejected=1 "
               "atomic_nonblock_fence_fail_closed=1 "
               "atomic_out_fence=deferred native_present_credit=0 "
               "opengl_submit_credit=0 status=DEFERRED\n");
    }
    printf("gpu_core_c_validator drm_kms_sync_file_in_fence_matrix "
           "pending_sync_file_in_fence=PASS "
           "pending_in_fence_rejected=PASS signal_wake=PASS "
           "commit_after_signal=PASS in_fence_fd_ref_prepare=PASS "
           "test_only_no_wait=1 native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator drm_kms_present_completion_failclosed_matrix "
           "unsupported_format=NV12 setcrtc_failclosed=PASS "
           "plane_formats_scanout_exclude_nv12=PASS "
           "page_flip_failclosed=PASS "
           "atomic_test_only_failclosed=PASS atomic_failclosed=PASS "
           "no_event=PASS no_out_fence=PASS no_in_fence_refs=PASS "
           "state_unchanged=PASS dxg_present_delta=0/0 "
           "native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator drm_atomic_out_fence_provenance_matrix "
           "out_fence_source=software_scanout_commit out_fence_software=1 "
           "out_fence_immediate=0 out_fence_display_correlated=0 "
           "out_fence_software_scanout_correlated=1 "
           "out_fence_completion_deferred=0 "
           "out_fence_display_correlated_delta=0 "
           "out_fence_software_scanout_correlated_delta=1 "
           "native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    return 0;
}

static int validate_ttm_resv_matrix(void)
{
    char *ttm[] = { "ttmtest", 0 };
    static char output[32768];
    int before = failures;

    if (run_child_capture("ttm", ttm, output, sizeof(output)) < 0)
        return -1;

    require_output_token("ttm_resv_wait_matrix", output,
                         "ttm_resv_wait_matrix");
    require_output_token("ttm_resv_wait_matrix", output,
                         "imported_conflict_rejected=1");
    require_output_token("ttm_resv_wait_matrix", output,
                         "reserved_set_placement_rejected=1");
    require_output_token("ttm_resv_wait_matrix", output,
                         "reserved_force_evict_rejected=1");
    require_output_token("ttm_resv_wait_matrix", output,
                         "shared_fences_delta=");
    require_output_token("ttm_resv_wait_matrix", output,
                         "dmabuf_attach_export_delta=");
    require_output_token("ttm_resv_wait_matrix", output,
                         "dmabuf_attach_import_delta=");
    require_output_token("ttm_resv_wait_matrix", output,
                         "native_accel_credit_delta=0");
    require_output_token("ttm_resv_wait_matrix", output,
                         "status=PASS");
    require_output_token("ttm_eviction_negative_matrix", output,
                         "ttm_eviction_negative_matrix");
    require_output_token("ttm_eviction_negative_matrix", output,
                         "pinned_evict_rejected=1");
    require_output_token("ttm_eviction_negative_matrix", output,
                         "evict_pinned_rejects_delta=");
    require_output_token("ttm_eviction_negative_matrix", output,
                         "evict_busy_rejects_delta=");
    require_output_token("ttm_eviction_negative_matrix", output,
                         "native_accel_credit_delta=0 status=PASS");
    require_output_token("ttm_move_path_matrix", output,
                         "ttm_move_path_matrix");
    require_output_token("ttm_move_path_matrix", output,
                         "cpu_copy_fallback_compatible=");
    require_output_token("ttm_move_path_matrix", output,
                         "metadata_noop_checks=");
    require_output_token("ttm_move_path_matrix", output,
                         "hardware_copy_unsupported=");
    require_output_token("ttm_move_path_matrix", output,
                         "real_copy_path_moves=");
    require_output_token("ttm_move_path_matrix", output,
                         "cpu_copy_by_domain=");
    require_output_token("ttm_move_path_matrix", output,
                         "metadata_noop_by_domain=");
    require_output_token("ttm_move_path_matrix", output,
                         "unsupported_hw_copy_by_domain=");
    require_output_token("ttm_move_path_matrix", output,
                         "real_copy_by_domain=");
    require_output_token("ttm_move_path_matrix", output,
                         "per_domain_fields=present");
    reject_output_token("ttm_move_path_matrix", output,
                        "per_domain_fields=missing");
    reject_output_token("ttm_move_path_matrix", output,
                        "missing_fields=");
    require_output_token("ttm_move_path_matrix", output,
                         "status=PASS");
    require_output_token("ttm_matrix", output, "ttmtest: ok");

    if (failures != before)
        return -1;

    printf("gpu_core_c_validator ttm_resv_wait_matrix "
           "imported_conflict_rejected=1 "
           "reserved_set_placement_rejected=1 "
           "reserved_force_evict_rejected=1 "
           "pinned_evict_rejected=1 native_accel_credit_delta=0 "
           "ttm_move_path_matrix=PASS "
           "status=PASS\n");
    return 0;
}

static int validate_present_source_matrix(void)
{
    char *dxg_present[] = {
        "dxgprobe", "--present-source-failclosed-validate", 0
    };
    static char output[16384];
    int before = failures;

    if (run_child_capture("dxg_present_source_failclosed",
                          dxg_present, output, sizeof(output)) < 0)
        return -1;

    require_output_token("present_source_failclosed_matrix", output,
                         "present_source_failclosed_matrix");
    require_output_token("present_source_failclosed_matrix", output,
                         "failclosed=1");
    require_output_token("present_source_failclosed_matrix", output,
                         "no_present_credit=1");
    require_output_token("present_source_failclosed_matrix", output,
                         "owner_cleanup=1");
    require_output_token("present_source_failclosed_matrix", output,
                         "native_present_claim=0");
    require_output_line_token("present_source_failclosed_matrix", output,
                              "present_source_failclosed_matrix",
                              "status=PASS");

    require_output_token("present_source_waitsync_failclosed_matrix",
                         output,
                         "present_source_waitsync_failclosed_matrix");
    require_output_token("present_source_waitsync_failclosed_matrix",
                         output, "create_resource=PASS");
    require_output_token("present_source_waitsync_failclosed_matrix",
                         output, "share_resource=PASS");
    require_output_token("present_source_waitsync_failclosed_matrix",
                         output, "register=PASS");
    require_output_token("present_source_waitsync_failclosed_matrix",
                         output, "wait_sync_metadata=PASS");
    require_output_token("present_source_waitsync_failclosed_matrix",
                         output, "present_id=0");
    require_output_token("present_source_waitsync_failclosed_matrix",
                         output, "completed=0");
    require_output_token("present_source_waitsync_failclosed_matrix",
                         output, "no_present_credit=1");
    require_output_token("present_source_waitsync_failclosed_matrix",
                         output, "native_present_claim=0");
    require_output_line_token("present_source_waitsync_failclosed_matrix",
                              output,
                              "present_source_waitsync_failclosed_matrix",
                              "status=PASS");

    require_output_token("present_source_negative_metadata_matrix",
                         output,
                         "present_source_negative_metadata_matrix");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "zero_dimensions=PASS");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "bad_pitch=PASS");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "invalid_resource_fd=PASS");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "reserved_register_flags=PASS");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "no_source_commit=PASS");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "wait_sync_missing_sync=PASS");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "sync_without_wait_flag=PASS");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "unverified_resource_fd=PASS");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "adapter_mismatch=PASS");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "no_present_credit=1");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "native_present_claim=0");
    require_output_token("present_source_negative_metadata_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("present_source_negative_metadata_matrix",
                              output,
                              "present_source_negative_metadata_matrix",
                              "status=PASS");

    if (failures != before)
        return -1;

    printf("gpu_core_c_validator present_source_failclosed_matrix "
           "failclosed=PASS owner_cleanup=PASS no_present_credit=1 "
           "native_present_claim=0 status=PASS\n");
    printf("gpu_core_c_validator present_source_waitsync_failclosed_matrix "
           "wait_sync_metadata=PASS missing_host_abi=scanout_bind "
           "transport_present=0 present_id=0 completed=0 "
           "no_present_credit=1 native_present_claim=0 status=PASS\n");
    printf("gpu_core_c_validator present_source_negative_metadata_matrix "
           "register_rejects=PASS commit_sync_rules=PASS "
           "unverified_resource_fd=PASS adapter_mismatch=PASS "
           "no_present_credit=1 native_present_claim=0 "
           "opengl_submit_credit=0 status=PASS\n");
    return 0;
}

static int segment_done(const char *segment, int before)
{
    if (failures == before) {
        printf("gpu_core_c_validator section=%s status=PASS\n", segment);
        printf("gpu_core_c_validator segment=%s status=PASS\n", segment);
        return 0;
    }
    printf("gpu_core_c_validator section=%s status=FAIL failures=%d\n",
           segment, failures - before);
    printf("gpu_core_c_validator segment=%s status=FAIL failures=%d\n",
           segment, failures - before);
    return -1;
}

static int validate_cmdline(void)
{
    char buf[512];
    int ok = 1;

    if (read_text_file("/proc/cmdline", buf, sizeof(buf)) < 0) {
        note_fail("cmdline", "read_failed");
        return -1;
    }
    printf("gpu_core_c_validator cmdline=%s", buf);
    if (!contains(buf, "netsurf=0")) {
        note_fail("cmdline", "netsurf_not_disabled");
        ok = 0;
    }
    if (!contains(buf, "webkit=0")) {
        note_fail("cmdline", "webkit_not_disabled");
        ok = 0;
    }
    if (!contains(buf, "glsmoke=0")) {
        note_fail("cmdline", "glsmoke_not_disabled");
        ok = 0;
    }
    if (!contains(buf, "acpi_cpus=6")) {
        note_fail("cmdline", "missing_6vcpu_cmdline");
        ok = 0;
    }
    if (ok)
        printf("gpu_core_c_validator step=cmdline status=PASS\n");
    return ok ? 0 : -1;
}

static int validate_backend(void)
{
    struct fb_gpu_backend_info backend;
    struct fb_gpu_stats stats;
    int fd;
    int ok = 1;

    fd = open("/dev/gpu0", O_RDONLY);
    if (fd < 0)
        fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) {
        note_fail("backend", "open_gpu_or_fb_failed");
        return -1;
    }
    memset(&backend, 0, sizeof(backend));
    memset(&stats, 0, sizeof(stats));
    if (ioctl(fd, FB_GPU_BACKEND_QUERY, &backend) < 0) {
        close(fd);
        note_fail("backend", "backend_query_failed");
        return -1;
    }
    if (ioctl(fd, FB_GPU_GET_STATS, &stats) < 0) {
        close(fd);
        note_fail("backend", "stats_query_failed");
        return -1;
    }
    close(fd);

    printf("gpu_core_c_validator backend id=%u flags=0x%x name=%s renderer=%s "
           "dxg_global_open=%u dxg_vgpu_open=%u dxg_d3dkmt=%u\n",
           backend.backend, backend.flags,
           backend.name[0] ? backend.name : "(unnamed)",
           backend.renderer[0] ? backend.renderer : "(unknown)",
           backend.dxg_global_open, backend.dxg_vgpu_open,
           backend.dxg_d3dkmt);
    printf("gpu_core_c_validator stats bo_fd_live=%lu fence_fd_live=%lu "
           "gpu_backend=%lu gpu_backend_flags=0x%lx nouveau_registered=%lu "
           "nouveau_accepts=%lu reject_dxg_present=%lu reject_no_bars=%lu "
           "bar0=%lu bar1=%lu bar0_claimed=%lu bar1_claimed=%lu "
           "dma=%lu/%lu coherent=%lu/%lu irq_mode=%lu "
           "irq_handler=%lu irq_delivery=%lu irq_claimed=%lu "
           "pm=%lu/%lu balanced=%lu remove_suspended=%lu "
           "native_present_credit=%lu dxg_present_transport=%lu "
           "dxg_render=%lu dxg_display=%lu dxg_sources=%lu "
           "dxg_sources_known=%lu dda_present=%lu dda_import=%lu "
           "dda_scanout=%lu\n",
           stats.bo_fd_live, stats.fence_fd_live, stats.gpu_backend,
           stats.gpu_backend_flags, stats.nouveau_pci_registered,
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_probe_reject_dxg_present,
           stats.nouveau_pci_probe_reject_no_bars,
           stats.nouveau_pci_bar0_len, stats.nouveau_pci_bar1_len,
           stats.nouveau_pci_bar0_claimed, stats.nouveau_pci_bar1_claimed,
           stats.nouveau_pci_dma_mask_configured,
           stats.nouveau_pci_dma_mask_bits,
           stats.nouveau_pci_coherent_dma_mask_configured,
           stats.nouveau_pci_coherent_dma_mask_bits,
           stats.nouveau_pci_irq_mode,
           stats.nouveau_pci_irq_handler_registered,
           stats.nouveau_pci_irq_delivery_enabled,
           stats.nouveau_pci_irq_delivery_claimed,
           stats.nouveau_pci_suspend_count,
           stats.nouveau_pci_resume_count,
           stats.nouveau_pci_runtime_pm_balanced,
           stats.nouveau_pci_remove_runtime_suspended,
           stats.nouveau_pci_native_present_credit,
           stats.dxg_present_helper_transport_present,
           stats.dxg_present_dxg_adapter_render_supported,
           stats.dxg_present_dxg_adapter_display_supported,
           stats.dxg_present_dxg_adapter_sources,
           stats.dxg_present_dxg_adapter_sources_known,
           stats.dxg_present_dda_nouveau_present,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present);
    printf("gpu_core_c_validator nouveau_pci_dma_resource_matrix "
           "registered=%lu accepts=%lu reject_dxg_present=%lu "
           "reject_no_bars=%lu dma_mask_configured=%lu "
           "dma_mask_bits=%lu coherent_configured=%lu coherent_bits=%lu "
           "bar0_len=%lu bar1_len=%lu bar0_claimed=%lu "
           "bar1_claimed=%lu claim_failures=%lu releases=%lu "
           "irq_mode=%lu irq_failures=%lu msi_requested=%lu "
           "msi_fail_closed=%lu irq_vector_valid=%lu "
           "irq_handler_registered=%lu irq_delivery_enabled=%lu "
           "irq_delivery_claimed=%lu legacy_irq_fallback=%lu "
           "suspend_count=%lu resume_count=%lu "
           "pm_balanced=%lu remove_while_suspended=%lu "
           "native_present_credit=%lu status=PENDING\n",
           stats.nouveau_pci_registered,
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_probe_reject_dxg_present,
           stats.nouveau_pci_probe_reject_no_bars,
           stats.nouveau_pci_dma_mask_configured,
           stats.nouveau_pci_dma_mask_bits,
           stats.nouveau_pci_coherent_dma_mask_configured,
           stats.nouveau_pci_coherent_dma_mask_bits,
           stats.nouveau_pci_bar0_len,
           stats.nouveau_pci_bar1_len,
           stats.nouveau_pci_bar0_claimed,
           stats.nouveau_pci_bar1_claimed,
           stats.nouveau_pci_bar_claim_failures,
           stats.nouveau_pci_bar_releases,
           stats.nouveau_pci_irq_mode,
           stats.nouveau_pci_irq_request_failures,
           stats.nouveau_pci_msi_requested,
           stats.nouveau_pci_msi_fail_closed,
           stats.nouveau_pci_irq_vector_valid,
           stats.nouveau_pci_irq_handler_registered,
           stats.nouveau_pci_irq_delivery_enabled,
           stats.nouveau_pci_irq_delivery_claimed,
           stats.nouveau_pci_legacy_irq_fallback,
           stats.nouveau_pci_suspend_count,
           stats.nouveau_pci_resume_count,
           stats.nouveau_pci_runtime_pm_balanced,
           stats.nouveau_pci_remove_runtime_suspended,
           stats.nouveau_pci_native_present_credit);
    printf("gpu_core_c_validator nouveau_getparam_provenance_matrix "
           "getparams=%lu dda_facts=%lu synthetic_facts=%lu "
           "driver_caps=%lu "
           "fail_closed=%lu last_source=%lu accepts=%lu "
           "native_present_credit=%lu status=PENDING\n",
           stats.nouveau_getparams,
           stats.nouveau_getparam_dda_facts,
           stats.nouveau_getparam_synthetic_facts,
           stats.nouveau_getparam_driver_caps,
           stats.nouveau_getparam_fail_closed,
           stats.nouveau_getparam_last_source,
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_native_present_credit);
    printf("gpu_core_c_validator nouveau_channel_object_matrix "
           "channel_allocs=%lu channel_frees=%lu active=%lu "
           "notifier_allocs=%lu grobj_allocs=%lu gpuobj_frees=%lu "
           "object_rejects=%lu close_reclaims=%lu status=PENDING\n",
           stats.nouveau_channel_allocs,
           stats.nouveau_channel_frees,
           stats.nouveau_channel_active,
           stats.nouveau_notifier_allocs,
           stats.nouveau_grobj_allocs,
           stats.nouveau_gpuobj_frees,
           stats.nouveau_object_rejects,
           stats.nouveau_close_object_reclaims);

    if (backend.backend != FB_GPU_BACKEND_HYPERV_DXG) {
        note_fail("backend", "not_hyperv_dxg");
        ok = 0;
    }
    if ((backend.flags & FB_GPU_BACKEND_F_DXG_TRANSPORT) == 0 ||
        backend.dxg_global_open == 0 || backend.dxg_vgpu_open == 0) {
        note_fail("backend", "dxg_transport_not_ready");
        ok = 0;
    }
    if ((backend.flags & FB_GPU_BACKEND_F_D3DKMT) == 0 ||
        backend.dxg_d3dkmt == 0) {
        note_fail("backend", "d3dkmt_not_ready");
        ok = 0;
    }
    if ((backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0) {
        note_fail("backend", "hyperv_opengl_submit_ungated");
        ok = 0;
    }
    if ((backend.flags & FB_GPU_BACKEND_F_VIRGL_OPENGL) != 0) {
        note_fail("backend", "hyperv_advertises_virgl_opengl");
        ok = 0;
    }
    if (stats.bo_fd_live != 0) {
        note_fail("backend", "bo_fd_leak");
        ok = 0;
    }
    if (stats.fence_fd_live != 0) {
        note_fail("backend", "fence_fd_leak");
        ok = 0;
    }
    if (stats.nouveau_pci_registered == 0) {
        note_fail("backend", "nouveau_pci_not_registered");
        ok = 0;
    }
    if (stats.nouveau_pci_probe_accepts > 0) {
        if ((backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) == 0) {
            note_fail("backend", "dda_nouveau_accept_without_backend_flag");
            ok = 0;
        }
        if (stats.nouveau_pci_bar0_len == 0 &&
            stats.nouveau_pci_bar1_len == 0) {
            note_fail("backend", "dda_nouveau_accept_without_bar");
            ok = 0;
        }
        if (stats.nouveau_pci_dma_mask_configured == 0 ||
            stats.nouveau_pci_dma_mask_bits < 32 ||
            stats.nouveau_pci_coherent_dma_mask_configured == 0 ||
            stats.nouveau_pci_coherent_dma_mask_bits < 32) {
            note_fail("backend", "dda_nouveau_dma_mask_not_configured");
            ok = 0;
        }
        if (stats.nouveau_pci_bar0_len != 0 &&
            stats.nouveau_pci_bar0_claimed == 0) {
            note_fail("backend", "dda_nouveau_bar0_not_claimed");
            ok = 0;
        }
        if (stats.nouveau_pci_bar1_len != 0 &&
            stats.nouveau_pci_bar1_claimed == 0) {
            note_fail("backend", "dda_nouveau_bar1_not_claimed");
            ok = 0;
        }
        if (stats.nouveau_pci_runtime_suspended != 0 ||
            stats.nouveau_pci_runtime_pm_balanced == 0 ||
            stats.nouveau_pci_suspend_count !=
                stats.nouveau_pci_resume_count) {
            note_fail("backend", "dda_nouveau_runtime_pm_unbalanced");
            ok = 0;
        }
        if (stats.nouveau_pci_irq_delivery_claimed != 0 &&
            (stats.nouveau_pci_irq_handler_registered == 0 ||
             stats.nouveau_pci_irq_delivery_enabled == 0)) {
            note_fail("backend", "dda_nouveau_irq_delivery_without_handler");
            ok = 0;
        }
        if (stats.nouveau_getparams !=
            stats.nouveau_getparam_dda_facts +
                stats.nouveau_getparam_driver_caps +
                stats.nouveau_getparam_synthetic_facts) {
            note_fail("backend", "nouveau_getparam_provenance_unbalanced");
            ok = 0;
        }
        if (stats.nouveau_getparam_synthetic_facts != 0) {
            note_fail("backend", "nouveau_getparam_synthetic_hardware_facts");
            ok = 0;
        }
        if (stats.nouveau_getparams != 0 &&
            stats.nouveau_getparam_last_source ==
                FB_GPU_NOUVEAU_GETPARAM_SOURCE_NONE) {
            note_fail("backend", "nouveau_getparam_missing_last_source");
            ok = 0;
        }
    } else {
        if ((backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) != 0) {
            note_fail("backend", "dda_backend_flag_without_accept");
            ok = 0;
        }
        if (stats.nouveau_pci_probe_reject_dxg_present == 0 &&
            stats.nouveau_pci_probe_reject_no_bars == 0) {
            note_fail("backend", "gpu_p_fail_closed_reason_missing");
            ok = 0;
        }
        if (stats.dxg_present_dda_nouveau_present != 0) {
            note_fail("backend", "dda_present_without_accept");
            ok = 0;
        }
        if (stats.nouveau_pci_dma_mask_configured != 0 ||
            stats.nouveau_pci_coherent_dma_mask_configured != 0 ||
            stats.nouveau_pci_bar0_claimed != 0 ||
            stats.nouveau_pci_bar1_claimed != 0 ||
            stats.nouveau_pci_irq_vector_valid != 0 ||
            stats.nouveau_pci_irq_handler_registered != 0 ||
            stats.nouveau_pci_irq_delivery_enabled != 0 ||
            stats.nouveau_pci_irq_delivery_claimed != 0 ||
            stats.nouveau_pci_legacy_irq_fallback != 0 ||
            stats.nouveau_pci_suspend_count != 0 ||
            stats.nouveau_pci_resume_count != 0 ||
            stats.nouveau_pci_runtime_suspended != 0) {
            note_fail("backend", "nouveau_pci_resources_without_accept");
            ok = 0;
        }
        if (stats.nouveau_getparam_dda_facts != 0 ||
            stats.nouveau_getparam_synthetic_facts != 0 ||
            stats.nouveau_getparam_driver_caps != 0 ||
            stats.nouveau_getparams != 0 ||
            stats.nouveau_getparam_last_source !=
                FB_GPU_NOUVEAU_GETPARAM_SOURCE_NONE) {
            note_fail("backend", "nouveau_getparam_facts_without_accept");
            ok = 0;
        }
    }
    if (stats.nouveau_pci_irq_delivery_enabled != 0 &&
        stats.nouveau_pci_irq_handler_registered == 0) {
        note_fail("backend", "nouveau_pci_irq_delivery_enabled_without_handler");
        ok = 0;
    }
    if (stats.nouveau_pci_irq_delivery_claimed != 0 &&
        stats.nouveau_pci_irq_delivery_enabled == 0) {
        note_fail("backend", "nouveau_pci_irq_delivery_claimed_without_enable");
        ok = 0;
    }
    if (stats.nouveau_pci_remove_runtime_suspended != 0) {
        note_fail("backend", "nouveau_pci_removed_while_suspended");
        ok = 0;
    }
    if (stats.nouveau_pci_native_present_credit != 0) {
        note_fail("backend", "nouveau_pci_fabricated_native_present");
        ok = 0;
    }
    if (stats.nouveau_channel_active != 0) {
        note_fail("backend", "nouveau_channel_lifetime_leak");
        ok = 0;
    }
    if (stats.dxg_present_dda_nouveau_import_path_present != 0 ||
        stats.dxg_present_dda_nouveau_scanout_bind_present != 0) {
        note_fail("backend", "dda_d3d12_present_path_fabricated");
        ok = 0;
    }
    if (stats.dxg_present_dxg_adapter_type_wsl != 0 &&
        stats.dxg_present_dxg_adapter_display_supported != 0) {
        note_fail("backend", "wsl_dxg_display_bit_not_suppressed");
        ok = 0;
    }
    if (ok) {
        printf("gpu_core_c_validator nouveau_pci_dma_resource_matrix "
               "bar_claim=PASS dma_mask=PASS irq_diagnostics=PASS "
               "irq_handler_registered=%lu irq_delivery_enabled=%lu "
               "irq_delivery_claimed=%lu "
               "runtime_pm=PASS "
               "native_present_credit=0 status=PASS\n",
               stats.nouveau_pci_irq_handler_registered,
               stats.nouveau_pci_irq_delivery_enabled,
               stats.nouveau_pci_irq_delivery_claimed);
        if (stats.nouveau_pci_probe_accepts == 0) {
            printf("gpu_core_c_validator nouveau_gpup_failclosed_matrix "
                   "accepts=0 backend_dda_nouveau=0 reject_reason=PASS "
                   "no_fake_bar=PASS no_fake_dma=PASS no_fake_irq=PASS "
                   "no_fake_getparam=PASS no_fake_present=PASS "
                   "native_present_credit=0 opengl_submit_credit=0 "
                   "status=PASS\n");
        }
        printf("gpu_core_c_validator nouveau_getparam_provenance_matrix "
               "getparams=%lu dda_facts=%lu synthetic_facts=%lu "
               "driver_caps=%lu "
               "fail_closed=%lu last_source=%lu accepts=%lu "
               "synthetic_not_dda=PASS native_present_credit=0 "
               "status=PASS\n",
               stats.nouveau_getparams,
               stats.nouveau_getparam_dda_facts,
               stats.nouveau_getparam_synthetic_facts,
               stats.nouveau_getparam_driver_caps,
               stats.nouveau_getparam_fail_closed,
               stats.nouveau_getparam_last_source,
               stats.nouveau_pci_probe_accepts);
        if (stats.nouveau_pci_probe_accepts != 0) {
            printf("gpu_core_c_validator nouveau_getparam_ddafacts_matrix "
                   "dda_facts=%lu driver_caps=%lu synthetic_facts=0 "
                   "balanced=PASS no_synthetic_hw=PASS "
                   "native_present_credit=0 status=PASS\n",
                   stats.nouveau_getparam_dda_facts,
                   stats.nouveau_getparam_driver_caps);
        }
        printf("gpu_core_c_validator nouveau_channel_object_matrix "
               "channel_allocs=%lu channel_frees=%lu active=0 "
               "notifier_allocs=%lu grobj_allocs=%lu gpuobj_frees=%lu "
               "object_rejects=%lu close_reclaims=%lu "
               "lifetime=PASS status=PASS\n",
               stats.nouveau_channel_allocs,
               stats.nouveau_channel_frees,
               stats.nouveau_notifier_allocs,
               stats.nouveau_grobj_allocs,
               stats.nouveau_gpuobj_frees,
               stats.nouveau_object_rejects,
               stats.nouveau_close_object_reclaims);
    }
    if (ok)
        printf("gpu_core_c_validator step=backend status=PASS\n");
    return ok ? 0 : -1;
}

static int run_segment(const char *segment)
{
    char *dxg_qai[] = { "dxgprobe", "--qai-admission-validate", 0 };
    char *dxg_import[] = { "dxgprobe", "--import-negative-validate", 0 };
    char *dxg_resource[] = { "dxgprobe", "--resource-nt-validate", 0 };
    char *dxg_exporter[] = {
        "dxgprobe", "--shared-exporter-close-lifetime-validate", 0
    };
    char *dxg_seal[] = {
        "dxgprobe", "--shared-seal-provenance-validate", 0
    };
    char *dxg_lifetime[] = { "dxgprobe", "--shared-lifetime-validate", 0 };
    char *dxg_sync[] = { "dxgprobe", "--sync-only", 0 };
    char *gpubuf[] = { "gpubuftest", "3", 0 };
    char *owner[] = { "gpubuftest", "--render-owner", 0 };
    char *nouveau[] = { "nouveauabitest", 0 };
    int before = failures;

    printf("gpu_core_c_validator segment=%s status=RUN\n", segment);
    if (strcmp(segment, "preflight") == 0) {
        validate_cmdline();
        validate_backend();
        return segment_done(segment, before);
    }
    if (strcmp(segment, "drm") == 0) {
        validate_ttm_resv_matrix();
        validate_drm_syncobj_matrix();
        validate_prime_kms_matrix();
        validate_fbstat_aggregate_matrix();
        validate_backend();
        return segment_done(segment, before);
    }
    if (strcmp(segment, "dxg-share") == 0) {
        run_child("dxg_qai_admission", dxg_qai);
        run_child("dxg_import_negative", dxg_import);
        run_child("dxg_resource_nt", dxg_resource);
        run_child("dxg_exporter_close_lifetime", dxg_exporter);
        run_child("dxg_seal_provenance", dxg_seal);
        run_child("dxg_lifetime", dxg_lifetime);
        validate_backend();
        return segment_done(segment, before);
    }
    if (strcmp(segment, "dxg-sync") == 0) {
        run_child("dxg_sync_nt", dxg_sync);
        validate_sync_fd_close_lifetime();
        validate_backend();
        return segment_done(segment, before);
    }
    if (strcmp(segment, "present-source") == 0) {
        validate_present_source_matrix();
        validate_backend();
        return segment_done(segment, before);
    }
    if (strcmp(segment, "buffers") == 0) {
        run_child("gpubuf", gpubuf);
        run_child("render_owner", owner);
        run_child("nouveau", nouveau);
        validate_backend();
        return segment_done(segment, before);
    }
    if (strcmp(segment, "final") == 0) {
        validate_backend();
        return segment_done(segment, before);
    }

    note_fail("segment", "unknown_segment");
    printf("gpu_core_c_validator segment=%s status=FAIL reason=unknown\n",
           segment);
    return -1;
}

int main(int argc, char **argv)
{
    const char *segment = 0;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--segment") == 0 ||
             strcmp(argv[i], "--section") == 0) && i + 1 < argc) {
            segment = argv[++i];
        } else if (strcmp(argv[i], "--list-segments") == 0 ||
                   strcmp(argv[i], "--list-sections") == 0) {
            printf("preflight drm dxg-share dxg-sync present-source buffers final\n");
            return 0;
        } else {
            printf("gpu_core_c_validator status=FAIL unknown_arg=%s\n",
                   argv[i]);
            return 1;
        }
    }

    printf("gpu_core_c_validator status=RUN\n");
    if (segment != 0) {
        if (run_segment(segment) < 0) {
            printf("gpu_core_c_validator status=FAIL failures=%d\n",
                   failures);
            return 1;
        }
        printf("gpu_core_c_validator status=PASS section=%s\n", segment);
        printf("gpu_core_c_validator status=PASS segment=%s\n", segment);
        return 0;
    }

    run_segment("preflight");
    run_segment("drm");
    run_segment("dxg-share");
    run_segment("dxg-sync");
    run_segment("present-source");
    run_segment("buffers");
    run_segment("final");

    if (failures != 0) {
        printf("gpu_core_c_validator status=FAIL failures=%d\n", failures);
        return 1;
    }
    printf("gpu_core_c_validator status=PASS\n");
    return 0;
}

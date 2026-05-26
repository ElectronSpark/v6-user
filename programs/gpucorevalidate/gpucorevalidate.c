#include "kernel/inc/types.h"
#include "kernel/inc/errno.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

static int failures;

static const char *
display_bind_transport_source_name(uint64 value)
{
    switch (value) {
    case FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE:
        return "none";
    case FB_GPU_DXG_DISPLAY_BIND_SOURCE_NON_WSL_DXGKRNL_EXTENSION:
        return "non_wsl_linux_dxgkrnl_extension";
    case FB_GPU_DXG_DISPLAY_BIND_SOURCE_DDA_NOUVEAU_NATIVE_DISPLAY:
        return "dda_nouveau_native_display";
    default:
        return "unknown_value";
    }
}

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

static int token_boundary(char c)
{
    return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int output_token_match_at(const char *start, const char *end,
                                 const char *p, const char *token, uint len)
{
    if (p != start && !token_boundary(p[-1]))
        return 0;
    if (memcmp(p, token, len) != 0)
        return 0;
    if (p + len == end || token_boundary(p[len]))
        return 1;
    return len > 0 && token[len - 1] == '=';
}

static int output_has_token(const char *start, const char *end,
                            const char *token)
{
    uint len = strlen(token);
    const char *last;

    if (len == 0)
        return 1;
    if ((uint)(end - start) < len)
        return 0;
    last = end - len;
    for (const char *p = start; p <= last; p++) {
        if (output_token_match_at(start, end, p, token, len))
            return 1;
    }
    return 0;
}

static int output_contains_token(const char *output, const char *token)
{
    return output_has_token(output, output + strlen(output), token);
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
    if (output_contains_token(output, token))
        return 0;
    failures++;
    printf("gpu_core_c_validator step=%s status=FAIL missing_token=%s\n",
           step, token);
    return -1;
}

static int reject_output_token(const char *step, const char *output,
                               const char *token)
{
    if (!output_contains_token(output, token))
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
        has_anchor = output_has_token(line, end, anchor);
        has_token = output_has_token(line, end, token);
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
    static char output[131072];
    const char *poll_anchor = "dmabuf_poll_readiness_matrix stats";
    const char *atomic_anchor = "kms_atomic_fence_matrix stats";
    const char *backend_separation_anchor =
        "opengl_submit_backend_separation_matrix";
    const char *id_shape_anchor = "d3d12_display_bind_id_shape_matrix";
    const char *success_shape_anchor =
        "d3d12_display_bind_success_shape_matrix";
    const char *provider_gate_anchor = "d3d12_provider_credit_gate_matrix";
    const char *request_metadata_anchor =
        "d3d12_display_bind_request_metadata_matrix";
    const char *pending_lifetime_anchor =
        "d3d12_display_bind_pending_lifetime_matrix";
    const char *provider_publication_anchor =
        "d3d12_display_bind_provider_pending_publication_matrix";
    const char *provider_packet_lifetime_anchor =
        "d3d12_display_bind_provider_packet_lifetime_matrix";
    const char *provider_no_send_preflight_anchor =
        "d3d12_display_bind_provider_no_send_preflight_matrix";
    const char *completion_lifetime_anchor =
        "d3d12_native_completion_lifetime_matrix";
    const char *stale_source_anchor =
        "d3d12_display_bind_stale_source_zero_credit_matrix";
    const char *stale_async_anchor =
        "d3d12_display_bind_stale_async_completion_contract_matrix";
    const char *not_kms_anchor = "d3d12_native_completion_not_kms_matrix";
    const char *standard_alloc_anchor =
        "wsl_standard_alloc_not_display_bind_matrix";
    const char *foreign_prime_anchor =
        "foreign_prime_import_gap_matrix";
    const char *public_present_anchor =
        "public_present_api_not_guest_bind_matrix";
    const char *host_abi_discovery_anchor =
        "d3d12_display_bind_host_abi_discovery_matrix";
    const char *negative_abi_manifest_anchor =
        "d3d12_negative_abi_manifest_matrix";
    const char *authority_chain_anchor =
        "d3d12_display_bind_authority_chain_matrix";
    const char *dda_bridge_anchor =
        "dda_nouveau_d3d12_bridge_disjoint_matrix";
    const char *plan_dependency_anchor =
        "gpu_remaining_plan_dependency_skeleton_matrix";
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
    require_output_line_token("fbstat_backend_gating", output,
                              backend_separation_anchor,
                              "backend=hyperv-dxg");
    require_output_line_token("fbstat_backend_gating", output,
                              backend_separation_anchor,
                              "dxg_transport=1");
    require_output_line_token("fbstat_backend_gating", output,
                              backend_separation_anchor,
                              "d3dkmt=1");
    require_output_line_token("fbstat_backend_gating", output,
                              backend_separation_anchor,
                              "virgl_opengl=0");
    require_output_line_token("fbstat_backend_gating", output,
                              backend_separation_anchor,
                              "backend_opengl_submit=0");
    require_output_line_token("fbstat_backend_gating", output,
                              backend_separation_anchor,
                              "allowed_submit_backend=virgl");
    require_output_line_token("fbstat_backend_gating", output,
                              backend_separation_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_backend_gating", output,
                              backend_separation_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_native_completion_shape", output,
                              id_shape_anchor,
                              "zero_ids_required_when_failclosed=1");
    require_output_line_token("fbstat_native_completion_shape", output,
                              id_shape_anchor,
                              "completed_ge_present_if_nonzero=1");
    require_output_line_token("fbstat_native_completion_shape", output,
                              id_shape_anchor,
                              "stale_id_rejected=1");
    require_output_line_token("fbstat_native_completion_shape", output,
                              id_shape_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_display_bind_success_shape", output,
                              success_shape_anchor,
                              "success_requires_provider_clear=1");
    require_output_line_token("fbstat_display_bind_success_shape", output,
                              success_shape_anchor,
                              "success_requires_display_completion=1");
    require_output_line_token("fbstat_display_bind_success_shape", output,
                              success_shape_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_provider_credit_gate", output,
                              provider_gate_anchor,
                              "credit_requires_provider_clear=1");
    require_output_line_token("fbstat_provider_credit_gate", output,
                              provider_gate_anchor,
                              "backend_opengl_submit=0");
    require_output_line_token("fbstat_provider_credit_gate", output,
                              provider_gate_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_display_bind_request_metadata", output,
                              request_metadata_anchor,
                              "request_metadata_complete=");
    require_output_line_token("fbstat_display_bind_request_metadata", output,
                              request_metadata_anchor,
                              "missing_metadata=0x0");
    require_output_line_token("fbstat_display_bind_request_metadata", output,
                              request_metadata_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_display_bind_request_metadata", output,
                              request_metadata_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_display_bind_pending_lifetime", output,
                              pending_lifetime_anchor,
                              "active=0");
    require_output_line_token("fbstat_display_bind_pending_lifetime", output,
                              pending_lifetime_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_display_bind_pending_lifetime", output,
                              pending_lifetime_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_display_bind_pending_lifetime", output,
                              pending_lifetime_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "publication_attempts=");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "owner_generation_required=1");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "owner_generation=");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "source_generation=");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "resource_generation=");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "source_generation_required=1");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "resource_generation_required=1");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "pending_generation_match=");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "process_namespace_valid=1");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "device_hmgr_index_unique_valid=1");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "resource_hmgr_index_unique_valid=1");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "allocation_hmgr_index_unique_valid=1");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "shared_parent_global_share_match=1");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "syncobject_fence_map_size=");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "publish_before_send=0");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "publish_before_send_order=blocked");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "transport_pending_id=0");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "completion_demux_registered=0");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "resolved_or_cancelled=");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "refs_released=");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "pending_cancelled=");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "cancellation_ref_release_credit=0");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_display_bind_provider_publication",
                              output, provider_publication_anchor,
                              "status=PASS_FAILCLOSED");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "packet_listed=0");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "request_id=0");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "transport_pending_id=0");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "packet_completed=0");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "wait_cancelled=0");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "packet_removed_on_cancel=0");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "completion_demux_registered=0");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "host_saw_display_bind_packet=0");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "display_bind_transport_source=none");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_display_bind_provider_packet_lifetime",
                              output, provider_packet_lifetime_anchor,
                              "status=PASS_FAILCLOSED");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "request_metadata_complete=1");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "provider_pin_revalidated=1");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "source_generation_match=PASS");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "resource_generation_match=PASS");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "preflight_ready=1");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "send_attempts=0");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "send_blocked_no_host_abi=");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "completion_demux_attempts=0");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "completion_demux_blocked_no_contract=");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "host_saw_display_bind_packet=0");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "display_bind_transport_source=none");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "provider_no_host_abi=1");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "provider_no_sender=1");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "provider_no_completion=1");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_display_bind_provider_no_send_preflight",
                              output, provider_no_send_preflight_anchor,
                              "status=PASS_FAILCLOSED");
    require_output_line_token("fbstat_native_completion_lifetime", output,
                              completion_lifetime_anchor,
                              "callbacks_after_completion_required=1");
    require_output_line_token("fbstat_native_completion_lifetime", output,
                              completion_lifetime_anchor,
                              "close_before_signal_cancel_required=1");
    require_output_line_token("fbstat_native_completion_lifetime", output,
                              completion_lifetime_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_display_bind_stale_source", output,
                              stale_source_anchor,
                              "late_completion_after_release=0");
    require_output_line_token("fbstat_display_bind_stale_source", output,
                              stale_source_anchor,
                              "stale_completion_rejects=");
    require_output_line_token("fbstat_display_bind_stale_source", output,
                              stale_source_anchor,
                              "stale_completion_rejected=");
    require_output_line_token("fbstat_display_bind_stale_source", output,
                              stale_source_anchor,
                              "late_completion_rejected=PASS");
    require_output_line_token("fbstat_display_bind_stale_source", output,
                              stale_source_anchor,
                              "global_present_id_after_close=0");
    require_output_line_token("fbstat_display_bind_stale_source", output,
                              stale_source_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_display_bind_stale_source", output,
                              stale_source_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_display_bind_stale_source", output,
                              stale_source_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "real_sender=0");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "completion_demux_registered=0");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "transport_pending_id=0");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "owner_close_cancel_required_after_send=1");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "owner_close_cancel_deferred=1");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "late_completion_reject_required=1");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "late_completion_rejected=1");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "host_saw_display_bind_packet=0");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "display_bind_transport_source=none");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_display_bind_stale_async", output,
                              stale_async_anchor,
                              "status=PASS_FAILCLOSED");
    require_output_line_token("fbstat_native_completion_not_kms", output,
                              not_kms_anchor,
                              "display_wait_is_native=0");
    require_output_line_token("fbstat_native_completion_not_kms", output,
                              not_kms_anchor,
                              "kms_generic_display_credit=0");
    require_output_line_token("fbstat_native_completion_not_kms", output,
                              not_kms_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_standard_alloc_not_display_bind",
                              output, standard_alloc_anchor,
                              "standard_alloc_native_present_credit=0");
    require_output_line_token("fbstat_standard_alloc_not_display_bind",
                              output, standard_alloc_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "foreign_attempts=");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "foreign_rejects=");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "local_only_import_path=1");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "d3d12_foreign_resource_imports=0");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "nouveau_scanout_bind_imports=0");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "dmabuf_native_present_credit=0");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "dxg_dda_import_path=0");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "dxg_dda_scanout_bind=0");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "scanout_bind_successes=0");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "completion_successes=0");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_foreign_prime_import_gap", output,
                              foreign_prime_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "reactos_d3dkmt_present_api=known");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "directx_sharing_contract_hwnd_only=1");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "wslg_local_source=absent");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "freerdp_local_source=absent");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "rdp_frame_transport=copy_or_dirty_frame");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "guest_vmbus_display_bind_contract=0");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "gpup_sender_contract=0");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "gpup_completion_contract=0");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "transport_present=0");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "webkit_accel_credit=0");
    require_output_line_token("fbstat_public_present_api_not_guest_bind",
                              output, public_present_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "custom_host_tool=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "wsl_dxg_display_bind_ioctl=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "wsl_display_bind_ioctl_absent=1");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "wslg_frame_path=absent");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "freerdp_frame_path=absent");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "rdp_frame_path=copy_or_dirty_frame");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "hvsock_display_bind_service=absent");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "gpup_dxg_sender_contract=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "gpup_dxg_completion_contract=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "completion_demux_contract=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "dda_nouveau_d3d12_import=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "dda_nouveau_scanout_bind=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "dda_nouveau_hw_flip_completion=ABSENT");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "provider_state=failclosed");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "transport_present=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "present_id=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "completed=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "webkit_accel_credit=0");
    require_output_line_token("fbstat_display_bind_host_abi_discovery",
                              output, host_abi_discovery_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "wsl_uapi_header=d3dkmthk.h");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "wsl_display_bind_ioctl=0");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "wsl_vmbus_file=dxgvmbus.c");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "presenthistory_cmd=34");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "redirected_flip_fence_cmd=35");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "blt_cmd=38");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "submit_hwqueue_cmd=52");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "propagate_presenthistory_cmd=1");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "hvsock_display_bind_service=absent");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "resource_scanout_bind_sender=0");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "display_completion_demux=0");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "synthvid_path=gpa_dirty_rect_only");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "dda_nouveau_path=separate_pci_display");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "host_saw_display_bind_packet=0");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "display_bind_transport_source=none");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_negative_abi_manifest", output,
                              negative_abi_manifest_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "chain_version=1");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "selected_lane=gpup_dxg_scanout_bind");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "host_abi_gate=closed");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "provider_send_gate=closed");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "host_packet_gate=closed");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "completion_demux_gate=closed");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "display_completion_gate=closed");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "consumer_credit_gate=closed");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "display_bind_transport_source=none");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "host_saw_display_bind_packet=0");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "completion_demux_registered=0");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "dda_native_display_is_d3d12_bridge=0");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "kms_completion_is_d3d12=0");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "syncfile_is_display_completion=0");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_display_bind_authority_chain", output,
                              authority_chain_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_dda_nouveau_d3d12_bridge", output,
                              dda_bridge_anchor,
                              "dda_d3d12_import_path=0");
    require_output_line_token("fbstat_dda_nouveau_d3d12_bridge", output,
                              dda_bridge_anchor,
                              "dda_d3d12_scanout_bind=0");
    require_output_line_token("fbstat_dda_nouveau_d3d12_bridge", output,
                              dda_bridge_anchor,
                              "dda_hw_flip_completion_for_d3d12=0");
    require_output_line_token("fbstat_dda_nouveau_d3d12_bridge", output,
                              dda_bridge_anchor,
                              "kms_lane_is_d3d12=0");
    require_output_line_token("fbstat_dda_nouveau_d3d12_bridge", output,
                              dda_bridge_anchor,
                              "display_bind_present_id=0");
    require_output_line_token("fbstat_dda_nouveau_d3d12_bridge", output,
                              dda_bridge_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_dda_nouveau_d3d12_bridge", output,
                              dda_bridge_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_dda_nouveau_d3d12_bridge", output,
                              dda_bridge_anchor,
                              "status=PASS_FAILCLOSED");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "real_display_bind_sender=0");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "real_display_bind_completion=0");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "native_completion_validator_gate=closed");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "finite_480p_gate=closed");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "backend_opengl_submit_gate=closed");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "webkit_enabled_artifact_gate=closed");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "dda_nouveau_blocker="
                              "separate-display-not-D3D12-bind");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "dda_nouveau_reason="
                              "DDA/Nouveau-separate-display-not-D3D12-bind");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "native_present_credit=0");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "webkit_accel_credit=0");
    require_output_line_token("fbstat_plan_dependency_blockers", output,
                              plan_dependency_anchor,
                              "status=PASS");
    require_output_line_token("fbstat_native_display_readiness", output,
                              "native_display_readiness_failclosed_matrix",
                              "display_probe_attempts=0");
    require_output_line_token("fbstat_native_display_readiness", output,
                              "native_display_readiness_failclosed_matrix",
                              "vblank_source=none");
    require_output_line_token("fbstat_native_display_readiness", output,
                              "native_display_readiness_failclosed_matrix",
                              "atomic_backend_missing=0");
    require_output_line_token("fbstat_native_display_readiness", output,
                              "native_display_readiness_failclosed_matrix",
                              "reject_has_atomic_pageflip_backend=");
    require_output_line_token("fbstat_nouveau_display_failclosed", output,
                              "nouveau_display_failclosed_matrix",
                              "vblank_irq_supported=0");
    require_output_line_token("fbstat_nouveau_display_failclosed", output,
                              "nouveau_display_failclosed_matrix",
                              "atomic_missing_policy=PASS");
    require_output_line_token("fbstat_kms_present_discriminator", output,
                              "kms_present_discriminator_failclosed_matrix",
                              "no_atomic_pageflip_backend=");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_display_kms_registration_matrix",
                              "nonvirtual_connectors=");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_display_kms_registration_matrix",
                              "atomic_backend_missing=");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_display_kms_registration_matrix",
                              "native_present_credit=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_display_kms_registration_matrix",
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_display_kms_registration_matrix",
                              "status=PASS");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_kms_vblank_irq_source_matrix",
                              "nouveau_vblank_source=none");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_kms_vblank_irq_source_matrix",
                              "irq_source=not_nouveau");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_kms_vblank_irq_source_matrix",
                              "native_present_credit=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_kms_vblank_irq_source_matrix",
                              "status=PASS");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_primary_plane_modifier_failclosed_matrix",
                              "modifier_credit=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_primary_plane_modifier_failclosed_matrix",
                              "status=PASS");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_linux_display_readiness_matrix",
                              "display_engine_object=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_linux_display_readiness_matrix",
                              "linear_required=1");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_linux_display_readiness_matrix",
                              "vblank_event_registered=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_linux_display_readiness_matrix",
                              "page_flip_event_source=none");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_linux_display_readiness_matrix",
                              "status=PASS_FAILCLOSED");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_kms_acceptance_shape_matrix",
                              "kernel_gate=full_linux_shape");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_kms_acceptance_shape_matrix",
                              "vblank_source=none");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_kms_acceptance_shape_matrix",
                              "page_flip_event_source=none");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_kms_acceptance_shape_matrix",
                              "native_present_credit=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_kms_acceptance_shape_matrix",
                              "status=PASS_FAILCLOSED");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_dda_display_positive_shape_matrix",
                              "dda_positive=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_dda_display_positive_shape_matrix",
                              "d3d12_native_present_credit=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_dda_display_positive_shape_matrix",
                              "opengl_submit_credit=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "nouveau_dda_display_positive_shape_matrix",
                              "status=PASS_FAILCLOSED");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_scanout_cpu_convert_separation_matrix",
                              "cpu_convert_native_present=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_scanout_cpu_convert_separation_matrix",
                              "status=PASS");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_gem_fb_plane_ref_matrix",
                              "plane_ref_fields=bounded");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_gem_fb_plane_ref_matrix",
                              "status=PASS");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_atomic_plane_state_matrix",
                              "plane_state_native_present=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_atomic_plane_state_matrix",
                              "status=PASS");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_atomic_prepare_cleanup_fb_matrix",
                              "fb_prepare_cleanup_credit=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_atomic_prepare_cleanup_fb_matrix",
                              "status=PASS");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_page_flip_feature_gate_matrix",
                              "atomic_backend_missing=");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_page_flip_feature_gate_matrix",
                              "target_gate=closed");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_page_flip_feature_gate_matrix",
                              "page_flip_native_present_credit=0");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "kms_page_flip_feature_gate_matrix",
                              "status=PASS");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "dda_nouveau_non_readback_display_proof_matrix",
                              "nonvirtual_connectors=");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "dda_nouveau_non_readback_display_proof_matrix",
                              "vblank_irq_supported=");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "dda_nouveau_non_readback_display_proof_matrix",
                              "atomic_backend_missing=");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "dda_nouveau_non_readback_display_proof_matrix",
                              "dda_native_display_credit=");
    require_output_line_token("fbstat_linux_kms_nouveau_audit", output,
                              "dda_nouveau_non_readback_display_proof_matrix",
                              "d3d12_native_present_credit=0");

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
    require_counter_min("fbstat_kms_atomic_fence_stats",
                        "out_fence_display_correlated",
                        out_fence_display_correlated, 1);
    require_counter_eq("fbstat_kms_atomic_fence_stats",
                       "out_fence_software_scanout_correlated",
                       out_fence_software_scanout_correlated, 0);

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
    require_output_token("drm_vblank_native_present_separation_matrix",
                         output,
                         "kms_vblank_native_present_separation_matrix");
    require_output_token("drm_vblank_native_present_separation_matrix",
                         output, "display_completion_is_native_present=0");
    require_output_token("drm_vblank_native_present_separation_matrix",
                         output, "page_flip_native_present_credit=0");
    require_output_token("drm_vblank_native_present_separation_matrix",
                         output, "page_flip_events_native_hw=0");
    require_output_token("drm_vblank_native_present_separation_matrix",
                         output, "vblank_source_native_hw=0");
    require_output_token("drm_vblank_native_present_separation_matrix",
                         output, "vblank_native_present_credit=0");
    require_output_token("drm_vblank_native_present_separation_matrix",
                         output, "opengl_submit_credit=0");
    require_output_token("drm_vblank_native_present_separation_matrix",
                         output, "status=PASS");
    require_output_token("drm_kms_in_formats_blob_matrix", output,
                         "kms_in_formats_blob_matrix");
    require_output_token("drm_kms_in_formats_blob_matrix", output,
                         "cap_addfb2_modifiers=1");
    require_output_token("drm_kms_in_formats_blob_matrix", output,
                         "in_formats_blob=PASS");
    require_output_token("drm_kms_in_formats_blob_matrix", output,
                         "xrgb8888_linear=1");
    require_output_token("drm_kms_in_formats_blob_matrix", output,
                         "argb8888_linear=1");
    require_output_token("drm_kms_in_formats_blob_matrix", output,
                         "nv12_scanout=0");
    require_output_token("drm_kms_in_formats_blob_matrix", output,
                         "nonlinear_modifiers=0");
    require_output_token("drm_kms_in_formats_blob_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_kms_in_formats_blob_matrix", output,
                         "opengl_submit_credit=0");
    require_output_line_token("drm_kms_in_formats_blob_matrix", output,
                              "kms_in_formats_blob_matrix", "status=PASS");
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
    require_output_token("drm_dma_fence_lifetime_contract_matrix", output,
                         "dma_fence_lifetime_contract_matrix");
    require_output_token("drm_dma_fence_lifetime_contract_matrix", output,
                         "single_backing_object=fb_gpu_fence");
    require_output_token("drm_dma_fence_lifetime_contract_matrix", output,
                         "gem_prime_dmabuf=PASS");
    require_output_token("drm_dma_fence_lifetime_contract_matrix", output,
                         "kms_out_fence=PASS");
    require_output_token("drm_dma_fence_lifetime_contract_matrix", output,
                         "syncobj_sync_file=PASS");
    require_output_token("drm_dma_fence_lifetime_contract_matrix", output,
                         "poll_callback_removal=PASS");
    require_output_token("drm_dma_fence_lifetime_contract_matrix", output,
                         "final_release=PASS");
    require_output_token("drm_dma_fence_lifetime_contract_matrix", output,
                         "native_present_credit=0");
    require_output_token("drm_dma_fence_lifetime_contract_matrix", output,
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
                             "atomic_out_fence_display=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_software=0");
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
                             "atomic_out_fence_display_correlated=1");
        require_output_token("drm_atomic_fence_matrix", output,
                             "out_fence_display_correlated_delta=");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_out_fence_software_scanout_correlated=0");
        require_output_token("drm_atomic_fence_matrix", output,
                             "out_fence_software_scanout_correlated_delta=0");
        require_output_token("drm_atomic_fence_matrix", output,
                             "atomic_fence_kernel=real");
        require_output_line_token("drm_atomic_fence_matrix", output,
                                  "atomic_fence_matrix", "status=PASS");
        reject_output_token("drm_atomic_fence_matrix", output,
                            "atomic_out_fence_placeholder=1");
        reject_output_token("drm_atomic_fence_matrix", output,
                            "out_fence_placeholder=1");
        reject_output_token("drm_atomic_fence_matrix", output,
                            "atomic_fence_kernel=placeholder");
        reject_output_token("drm_atomic_fence_matrix", output,
                            "atomic_fence_kernel=missing_fields");
        reject_output_line_token("drm_atomic_fence_matrix", output,
                                 "atomic_fence_matrix",
                                 "out_fence_placeholder=1");
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
                         "out_fence_source=display_completion");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_display=1");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_software=0");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_immediate=0");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_display_correlated=1");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_software_scanout_correlated=0");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_completion_deferred=1");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_display_correlated_delta=");
    require_output_token("drm_atomic_out_fence_provenance_matrix", output,
                         "out_fence_software_scanout_correlated_delta=0");
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
    printf("gpu_core_c_validator "
           "drm_vblank_native_present_separation_matrix "
           "page_flip_events_software_blit=1 "
           "page_flip_events_native_hw=0 "
           "vblank_source_software_display=1 "
           "vblank_source_native_hw=0 "
           "display_completion_is_native_present=0 "
           "page_flip_native_present_credit=0 "
           "vblank_native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
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
    printf("gpu_core_c_validator "
           "drm_dma_fence_lifetime_contract_matrix "
           "single_backing_object=fb_gpu_fence "
           "gem_prime_dmabuf=PASS kms_out_fence=PASS "
           "syncobj_sync_file=PASS poll_callback_removal=PASS "
           "final_release=PASS native_present_credit=0 "
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
    printf("gpu_core_c_validator drm_kms_in_formats_blob_matrix "
           "cap_addfb2_modifiers=1 in_formats_blob=PASS "
           "xrgb8888_linear=1 argb8888_linear=1 nv12_scanout=0 "
           "nonlinear_modifiers=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator drm_atomic_out_fence_provenance_matrix "
           "out_fence_source=display_completion out_fence_display=1 "
           "out_fence_software=0 out_fence_immediate=0 "
           "out_fence_display_correlated=1 "
           "out_fence_software_scanout_correlated=0 "
           "out_fence_completion_deferred=1 "
           "out_fence_display_correlated_delta=1 "
           "out_fence_software_scanout_correlated_delta=0 "
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
    require_output_token("ttm_dma_resv_ww_mutex_matrix", output,
                         "ttm_dma_resv_ww_mutex_matrix");
    require_output_token("ttm_dma_resv_ww_mutex_matrix", output,
                         "ww_contexts_delta=");
    require_output_token("ttm_dma_resv_ww_mutex_matrix", output,
                         "ordered_acquires_delta=");
    require_output_token("ttm_dma_resv_ww_mutex_matrix", output,
                         "deadlock_retries_delta=");
    require_output_token("ttm_dma_resv_ww_mutex_matrix", output,
                         "wound_backoffs_delta=");
    require_output_token("ttm_dma_resv_ww_mutex_matrix", output,
                         "multi_object_delta=");
    require_output_token("ttm_dma_resv_ww_mutex_matrix", output,
                         "release_balance_delta=");
    require_output_token("ttm_dma_resv_ww_mutex_matrix", output,
                         "max_acquired=");
    require_output_token("ttm_dma_resv_ww_mutex_matrix", output,
                         "validate_failures_delta=0");
    require_output_token("ttm_dma_resv_ww_mutex_matrix", output,
                         "native_accel_credit_delta=0 status=PASS");
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
           "ttm_dma_resv_ww_mutex_matrix=PASS "
           "ttm_move_path_matrix=PASS "
           "status=PASS\n");
    printf("gpu_core_c_validator ttm_real_move_backend_matrix "
           "real_move_backend=cpu_copy hw_backend=fail_closed "
           "native_accel_credit_delta=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    return 0;
}

static int validate_present_source_matrix(void)
{
    char *dxg_present[] = {
        "dxgprobe", "--present-source-failclosed-validate", 0
    };
    static char output[32768];
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
    require_output_token("present_source_software_path_rejection_matrix",
                         output,
                         "present_source_software_path_rejection_matrix");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "framebuffer_blit=REJECTED");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "cpu_map_readback=REJECTED");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "dri_software_present=REJECTED");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "copy_export_fallback=REJECTED");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "callback_only=REJECTED");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "release_only=REJECTED");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "display_target_kind=0");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "present_id=0");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "completed=0");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "native_present_claim=0");
    require_output_token("present_source_software_path_rejection_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("present_source_software_path_rejection_matrix",
                              output,
                              "present_source_software_path_rejection_matrix",
                              "status=PASS");
    require_output_token("d3d12_shared_resource_fd_lifetime_matrix", output,
                         "d3d12_shared_resource_fd_lifetime_matrix");
    require_output_token("d3d12_shared_resource_fd_lifetime_matrix", output,
                         "register_live_fd=PASS");
    require_output_token("d3d12_shared_resource_fd_lifetime_matrix", output,
                         "invalid_fd_rejected=PASS");
    require_output_token("d3d12_shared_resource_fd_lifetime_matrix", output,
                         "unverified_resource_fd=PASS");
    require_output_token("d3d12_shared_resource_fd_lifetime_matrix", output,
                         "stale_source_after_owner_close=PASS");
    require_output_token("d3d12_shared_resource_fd_lifetime_matrix", output,
                         "cleanup_balance=PASS");
    require_output_token("d3d12_shared_resource_fd_lifetime_matrix", output,
                         "native_present_credit=0");
    require_output_token("d3d12_shared_resource_fd_lifetime_matrix", output,
                         "opengl_submit_credit=0");
    require_output_line_token("d3d12_shared_resource_fd_lifetime_matrix",
                              output,
                              "d3d12_shared_resource_fd_lifetime_matrix",
                              "status=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "d3d12_present_source_admission_matrix");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "same_adapter_luid=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "resource_fd=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "d3dkmt_handles=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "dimensions=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "format_modifier=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "wait_sync_metadata=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "unverified_resource_fd=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "adapter_mismatch=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "source_owner=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "failclosed=PASS");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "native_present_credit=0");
    require_output_token("d3d12_present_source_admission_matrix", output,
                         "opengl_submit_credit=0");
    require_output_line_token("d3d12_present_source_admission_matrix",
                              output,
                              "d3d12_present_source_admission_matrix",
                              "status=PASS");
    require_output_token("d3d12_acquire_fence_lifetime_matrix", output,
                         "d3d12_acquire_fence_lifetime_matrix");
    require_output_token("d3d12_acquire_fence_lifetime_matrix", output,
                         "monitored_fence=PASS");
    require_output_token("d3d12_acquire_fence_lifetime_matrix", output,
                         "wait_metadata=PASS");
    require_output_token("d3d12_acquire_fence_lifetime_matrix", output,
                         "wait_commit_failclosed=PASS");
    require_output_token("d3d12_acquire_fence_lifetime_matrix", output,
                         "query_sync_matches=PASS");
    require_output_token("d3d12_acquire_fence_lifetime_matrix", output,
                         "stale_source_cleanup=PASS");
    require_output_token("d3d12_acquire_fence_lifetime_matrix", output,
                         "native_present_credit=0");
    require_output_token("d3d12_acquire_fence_lifetime_matrix", output,
                         "opengl_submit_credit=0");
    require_output_line_token("d3d12_acquire_fence_lifetime_matrix",
                              output,
                              "d3d12_acquire_fence_lifetime_matrix",
                              "status=PASS");
    require_output_token("d3d12_present_syncfile_preopen_matrix", output,
                         "d3d12_present_syncfile_preopen_matrix");
    require_output_token("d3d12_present_syncfile_preopen_matrix", output,
                         "sync_file=");
    require_output_token("d3d12_present_syncfile_preopen_matrix", output,
                         "opened_sync=");
    require_output_token("d3d12_present_syncfile_preopen_matrix", output,
                         "wrong_fd_kind_rejected=PASS");
    require_output_token("d3d12_present_syncfile_preopen_matrix", output,
                         "wait_commit_failclosed=PASS");
    require_output_token("d3d12_present_syncfile_preopen_matrix", output,
                         "query_sync_matches=PASS");
    require_output_token("d3d12_present_syncfile_preopen_matrix", output,
                         "fence_value_preserved=PASS");
    require_output_token("d3d12_present_syncfile_preopen_matrix", output,
                         "stale_source_cleanup=PASS");
    require_output_token("d3d12_present_syncfile_preopen_matrix", output,
                         "native_present_credit=0");
    require_output_token("d3d12_present_syncfile_preopen_matrix", output,
                         "opengl_submit_credit=0");
    require_output_line_token("d3d12_present_syncfile_preopen_matrix",
                              output,
                              "d3d12_present_syncfile_preopen_matrix",
                              "status=PASS");
    require_output_token("d3d12_present_bind_contract_failclosed_matrix",
                         output,
                         "d3d12_present_bind_contract_failclosed_matrix");
    require_output_token("d3d12_present_bind_contract_failclosed_matrix",
                         output, "foreign_source=PASS");
    require_output_token("d3d12_present_bind_contract_failclosed_matrix",
                         output, "stale_source=PASS");
    require_output_token("d3d12_present_bind_contract_failclosed_matrix",
                         output, "software_paths_rejected=PASS");
    require_output_token("d3d12_present_bind_contract_failclosed_matrix",
                         output, "transport_present=0");
    require_output_token("d3d12_present_bind_contract_failclosed_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_present_bind_contract_failclosed_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_present_bind_contract_failclosed_matrix",
                              output,
                              "d3d12_present_bind_contract_failclosed_matrix",
                              "status=PASS");
    require_output_token("d3d12_present_resource_fd_typed_admission_matrix",
                         output,
                         "d3d12_present_resource_fd_typed_admission_matrix");
    require_output_token("d3d12_present_resource_fd_typed_admission_matrix",
                         output, "typed_resource_fd=PASS");
    require_output_token("d3d12_present_resource_fd_typed_admission_matrix",
                         output, "sealed_before_admit=PASS");
    require_output_token("d3d12_present_resource_fd_typed_admission_matrix",
                         output, "shared_records_valid=PASS");
    require_output_token("d3d12_present_resource_fd_typed_admission_matrix",
                         output, "allocation_match=PASS");
    require_output_token("d3d12_present_resource_fd_typed_admission_matrix",
                         output, "generation_from_shared=PASS");
    require_output_token("d3d12_present_resource_fd_typed_admission_matrix",
                         output, "invalid_fd_rejected=PASS");
    require_output_token("d3d12_present_resource_fd_typed_admission_matrix",
                         output, "stale_source_cleanup=PASS");
    require_output_token("d3d12_present_resource_fd_typed_admission_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_present_resource_fd_typed_admission_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_present_resource_fd_typed_admission_matrix",
                              output,
                              "d3d12_present_resource_fd_typed_admission_matrix",
                              "status=PASS");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output,
                         "d3d12_native_completion_zero_credit_matrix");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output, "display_bind=ABSENT");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output, "transport_present=0");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output, "completion_source=");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output, "present_id=0");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output, "completed=0");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output, "callbacks_after_completion=0");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output, "releases_after_completion=0");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output, "per_client_generation=required");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_native_completion_zero_credit_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_native_completion_zero_credit_matrix",
                              output,
                              "d3d12_native_completion_zero_credit_matrix",
                              "status=PASS");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output,
                         "d3d12_display_bind_backend_boundary_matrix");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "backend=gpup_dxg_scanout_bind");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "contract_version=1");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "transport=0");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "transport_present=0");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "operation=1");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "completion_source=3");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "present_id=0");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "completed=0");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "provider_submits=");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "lock_dropped_submits=");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "revalidate_attempts=");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "revalidate_successes=");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "revalidate_failures=0");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "provider_pin_revalidated=1");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "provider_no_host_abi=1");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "provider_no_sender=1");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "provider_no_completion=1");
    require_output_token("d3d12_display_bind_request_metadata_matrix",
                         output,
                         "d3d12_display_bind_request_metadata_matrix");
    require_output_token("d3d12_display_bind_request_metadata_matrix",
                         output, "request_metadata_complete=1");
    require_output_token("d3d12_display_bind_request_metadata_matrix",
                         output, "request_sync_metadata_complete=1");
    require_output_token("d3d12_display_bind_request_metadata_matrix",
                         output, "missing_metadata=0x0");
    require_output_token("d3d12_display_bind_request_metadata_matrix",
                         output, "present_id=0");
    require_output_token("d3d12_display_bind_request_metadata_matrix",
                         output, "completed=0");
    require_output_token("d3d12_display_bind_request_metadata_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_display_bind_request_metadata_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_display_bind_request_metadata_matrix",
                              output,
                              "d3d12_display_bind_request_metadata_matrix",
                              "status=PASS");
    require_output_token("d3d12_display_bind_pending_lifetime_matrix",
                         output,
                         "d3d12_display_bind_pending_lifetime_matrix");
    require_output_token("d3d12_display_bind_pending_lifetime_matrix",
                         output, "created_delta=");
    require_output_token("d3d12_display_bind_pending_lifetime_matrix",
                         output, "active=0");
    require_output_token("d3d12_display_bind_pending_lifetime_matrix",
                         output, "completed_delta=0");
    require_output_token("d3d12_display_bind_pending_lifetime_matrix",
                         output, "failclosed_delta=");
    require_output_token("d3d12_display_bind_pending_lifetime_matrix",
                         output, "cancelled_delta=0");
    require_output_token("d3d12_display_bind_pending_lifetime_matrix",
                         output, "last_status=95");
    require_output_token("d3d12_display_bind_pending_lifetime_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_display_bind_pending_lifetime_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_display_bind_pending_lifetime_matrix",
                              output,
                              "d3d12_display_bind_pending_lifetime_matrix",
                              "status=PASS");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output,
                         "d3d12_display_bind_generation_revalidation_matrix");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "provider_submits_delta=");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "lock_dropped_submits_delta=");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "revalidate_attempts_delta=");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "revalidate_successes_delta=");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "revalidate_failures_delta=0");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "provider_pin_revalidated=1");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "source_generation_match=PASS");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "resource_generation_match=PASS");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "pinned_generation_match=PASS");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "present_id=0");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "completed=0");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_display_bind_generation_revalidation_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_display_bind_generation_revalidation_matrix",
                              output,
                              "d3d12_display_bind_generation_revalidation_matrix",
                              "status=PASS");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output,
                         "d3d12_display_bind_provider_pending_publication_matrix");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "provider_submits_delta=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "publication_attempts_delta=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "owner_generation_required=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "owner_generation=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "query_source_generation=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "display_bind_source_generation=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "query_resource_generation=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "display_bind_resource_generation=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "source_generation_required=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "resource_generation_required=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "pending_generation_match=PASS");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "dxgprocess_generation=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "process_adapter_generation=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "process_namespace_valid=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "hmgr_index_unique_valid=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "device_hmgr_index_unique_valid=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "resource_hmgr_index_unique_valid=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "allocation_hmgr_index_unique_valid=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "device_object_ref_active=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "resource_object_ref_active=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "allocation_object_ref_active=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "shared_parent_id=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "shared_parent_refs=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "shared_parent_children=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "shared_parent_fd_refs=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "shared_parent_host_nt_refs=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "shared_parent_child_refs=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "shared_parent_global_share=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "shared_parent_host_nt=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "opened_child_parent_id_match=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "opened_child_global_share_match=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "opened_child_sealed_generation_match=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "shared_parent_snapshot_valid=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "opened_child_snapshot_valid=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "shared_parent_global_share_match=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "syncobject_object_ref_active=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "syncobject_shared_owner_present=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "syncobject_monitored_fence=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "syncobject_fence_value=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "syncobject_fence_cpu_va_present=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "syncobject_fence_gpu_va_present=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "syncobject_fence_kva_present=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "syncobject_fence_gpu_va_alias_gap=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "syncobject_fence_map_size=");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "owner_close_cancelled=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "publish_before_send=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "publish_before_send_order=blocked");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "transport_pending_id=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "completion_demux_registered=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "host_saw_display_bind_packet=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "display_bind_transport_source=none");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "wsl_presenthistory_completion_credit=0");
    require_output_token("d3d12_display_bind_provider_shared_parent_retention_matrix",
                         output, "opened_child_parent_id_match=1");
    require_output_token("d3d12_display_bind_provider_shared_parent_retention_matrix",
                         output, "opened_child_global_share_match=1");
    require_output_token("d3d12_display_bind_provider_shared_parent_retention_matrix",
                         output, "opened_child_sealed_generation_match=1");
    require_output_token("d3d12_display_bind_provider_shared_parent_retention_matrix",
                         output, "host_saw_display_bind_packet=0");
    require_output_token("d3d12_display_bind_provider_sync_fence_alias_matrix",
                         output, "kva_is_real_gpu_va=0");
    require_output_token("d3d12_display_bind_provider_sync_fence_alias_matrix",
                         output, "real_fence_gpu_va_present=");
    require_output_token("d3d12_display_bind_provider_sync_fence_alias_matrix",
                         output, "gpu_va_source=");
    require_output_token("d3d12_display_bind_provider_sync_fence_alias_matrix",
                         output, "syncfile_dma_fence_display_completion_credit=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "resolved_or_cancelled=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "refs_released=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "no_host_abi_cancelled=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "no_host_abi_refs_released=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "pending_cancelled_delta=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "cancellation_ref_release_credit=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "provider_no_host_abi=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "provider_no_sender=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "provider_no_completion=1");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "present_id=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "completed=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_display_bind_provider_pending_publication_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_display_bind_provider_pending_publication_matrix",
                              output,
                              "d3d12_display_bind_provider_pending_publication_matrix",
                              "status=PASS_FAILCLOSED");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output,
                         "d3d12_display_bind_provider_no_send_preflight_matrix");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "request_metadata_complete=1");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "provider_pin_revalidated=1");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "source_generation_match=PASS");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "resource_generation_match=PASS");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "preflight_ready=1");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "send_attempts=0");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "send_blocked_no_host_abi=");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "completion_demux_attempts=0");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "completion_demux_blocked_no_contract=");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "host_saw_display_bind_packet=0");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "display_bind_transport_source=none");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "provider_no_host_abi=1");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "provider_no_sender=1");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "provider_no_completion=1");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "present_id=0");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "completed=0");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_display_bind_provider_no_send_preflight_matrix",
                              output,
                              "d3d12_display_bind_provider_no_send_preflight_matrix",
                              "status=PASS_FAILCLOSED");
    require_output_token("d3d12_display_bind_pin_lifetime_matrix",
                         output,
                         "d3d12_display_bind_pin_lifetime_matrix");
    require_output_token("d3d12_display_bind_pin_lifetime_matrix",
                         output, "pinned_dxg_file=1");
    require_output_token("d3d12_display_bind_pin_lifetime_matrix",
                         output, "pinned_resource_file=1");
    require_output_token("d3d12_display_bind_pin_lifetime_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_display_bind_pin_lifetime_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_display_bind_pin_lifetime_matrix",
                              output,
                              "d3d12_display_bind_pin_lifetime_matrix",
                              "status=PASS");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "custom_host_tool=0");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_display_bind_backend_boundary_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_display_bind_backend_boundary_matrix",
                              output,
                              "d3d12_display_bind_backend_boundary_matrix",
                              "status=PASS");
    require_output_token("d3d12_display_bind_success_shape_matrix",
                         output,
                         "d3d12_display_bind_success_shape_matrix");
    require_output_token("d3d12_display_bind_success_shape_matrix",
                         output, "failclosed_allowed=1");
    require_output_token("d3d12_display_bind_success_shape_matrix",
                         output, "success_requires_provider_clear=1");
    require_output_token("d3d12_display_bind_success_shape_matrix",
                         output,
                         "success_requires_display_completion=1");
    require_output_token("d3d12_display_bind_success_shape_matrix",
                         output, "success_requires_source_authority=1");
    require_output_token("d3d12_display_bind_success_shape_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_display_bind_success_shape_matrix",
                         output, "backend_opengl_submit=0");
    require_output_line_token("d3d12_display_bind_success_shape_matrix",
                              output,
                              "d3d12_display_bind_success_shape_matrix",
                              "status=PASS");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output,
                         "d3d12_display_bind_query_fields_matrix");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "query_status=95");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "provider_status=95");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "query_host_abi_present=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "query_sender_present=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "query_completion_present=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "contract_host_abi_present=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "contract_sender_present=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "contract_completion_present=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "query_no_host_abi=1");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "contract_no_host_abi=1");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "query_pin_revalidated=1");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "contract_pin_revalidated=1");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "query_transport_source=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "contract_transport_source=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "query_host_saw_packet=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "contract_host_saw_packet=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "query_wsl_presenthistory_completion_credit=0");
    require_output_token("d3d12_display_bind_query_fields_matrix",
                         output, "contract_wsl_presenthistory_completion_credit=0");
    require_output_line_token("d3d12_display_bind_query_fields_matrix",
                              output,
                              "d3d12_display_bind_query_fields_matrix",
                              "status=PASS");
    require_output_token("d3d12_native_completion_lifetime_matrix",
                         output,
                         "d3d12_native_completion_lifetime_matrix");
    require_output_token("d3d12_native_completion_lifetime_matrix",
                         output, "callbacks_after_completion_required=1");
    require_output_token("d3d12_native_completion_lifetime_matrix",
                         output, "releases_after_completion_required=1");
    require_output_token("d3d12_native_completion_lifetime_matrix",
                         output, "close_before_signal_cancel_required=1");
    require_output_token("d3d12_native_completion_lifetime_matrix",
                         output, "cleanup_balance_required=1");
    require_output_token("d3d12_native_completion_lifetime_matrix",
                         output, "failclosed_callbacks_after_completion=0");
    require_output_token("d3d12_native_completion_lifetime_matrix",
                         output, "failclosed_releases_after_completion=0");
    require_output_token("d3d12_native_completion_lifetime_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_native_completion_lifetime_matrix",
                         output, "backend_opengl_submit=0");
    require_output_line_token("d3d12_native_completion_lifetime_matrix",
                              output,
                              "d3d12_native_completion_lifetime_matrix",
                              "status=PASS");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output,
                         "d3d12_native_completion_consumer_escrow_matrix");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "display_bind_gate=closed");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "provider_completion_present=0");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "callback_release_credit=0");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "frame_callback_credit=0");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "final_handoff_credit=0");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "fps_visible_credit=0");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "content_progress_credit=0");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "webkit_accel_credit=0");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "callback_release_order=blocked");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "consumer_visible_credit=blocked");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_native_completion_consumer_escrow_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_native_completion_consumer_escrow_matrix",
                              output,
                              "d3d12_native_completion_consumer_escrow_matrix",
                              "status=PASS_FAILCLOSED");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output,
                         "d3d12_display_bind_stale_source_zero_credit_matrix");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "release_sources_delta=");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "after_close_queries=");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "stale_source_rejects=");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "release_clears=");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "stale_generation_rejected=PASS");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "stale_completion_rejected=PASS");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "late_completion_after_release=0");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "late_completion_rejected=PASS");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "global_present_id_after_close=0");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "global_completed_after_close=0");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("d3d12_display_bind_stale_source_zero_credit_matrix",
                              output,
                              "d3d12_display_bind_stale_source_zero_credit_matrix",
                              "status=PASS");
    require_output_token("d3d12_present_commit_result_copyout_contract_matrix",
                         output,
                         "d3d12_present_commit_result_copyout_contract_matrix");
    require_output_token("d3d12_present_commit_result_copyout_contract_matrix",
                         output, "copyout_on_success=IMPLEMENTED");
    require_output_token("d3d12_present_commit_result_copyout_contract_matrix",
                         output, "failure_returns_errno=PASS");
    require_output_token("d3d12_present_commit_result_copyout_contract_matrix",
                         output, "failure_preserves_present_id=0");
    require_output_token("d3d12_present_commit_result_copyout_contract_matrix",
                         output, "failure_preserves_completed=0");
    require_output_token("d3d12_present_commit_result_copyout_contract_matrix",
                         output, "native_present_credit=0");
    require_output_token("d3d12_present_commit_result_copyout_contract_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token(
        "d3d12_present_commit_result_copyout_contract_matrix", output,
        "d3d12_present_commit_result_copyout_contract_matrix",
        "status=PASS");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output,
                         "dxg_resource_scanout_bind_host_abi_matrix");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "selected_lane=gpup_dxg_scanout_bind");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "custom_host_tool=0");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "wsl_dxg_display_bind_ioctl=0");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "wsl_ioctl_namespace_checked=1");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "wsl_display_bind_ioctl_absent=1");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "synthvid_vram_bridge=gpa_dirty_only");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output,
                         "standard_alloc_role=private_driver_data");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "standard_alloc_display_bind_absent=1");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "dxg_resource_fd=PASS");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "d3dkmt_handles=PASS");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "same_adapter_luid=PASS");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "missing_host_abi=1");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "transport_present=0");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "display_target_kind=0");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "present_id=0");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "completed=0");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "native_present_credit=0");
    require_output_token("dxg_resource_scanout_bind_host_abi_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("dxg_resource_scanout_bind_host_abi_matrix",
                              output,
                              "dxg_resource_scanout_bind_host_abi_matrix",
                              "status=PASS");
    require_output_token("dxg_scanout_bind_skeleton_matrix",
                         output, "dxg_scanout_bind_skeleton_matrix");
    require_output_token("dxg_scanout_bind_skeleton_matrix",
                         output, "attempts=");
    require_output_token("dxg_scanout_bind_skeleton_matrix",
                         output, "successes=0");
    require_output_token("dxg_scanout_bind_skeleton_matrix",
                         output, "weak_evidence_rejects=");
    require_output_token("dxg_scanout_bind_skeleton_matrix",
                         output, "present_id=0");
    require_output_token("dxg_scanout_bind_skeleton_matrix",
                         output, "completed=0");
    require_output_token("dxg_scanout_bind_skeleton_matrix",
                         output, "native_present_credit=0");
    require_output_token("dxg_scanout_bind_skeleton_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("dxg_scanout_bind_skeleton_matrix",
                              output,
                              "dxg_scanout_bind_skeleton_matrix",
                              "status=PASS");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output,
                         "dxg_scanout_bind_candidate_command_matrix");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "cmds_known=4");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "propagate_presenthistory_cmd=1");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "sender_contracts=0");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "completion_contracts=0");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "vmbus_enum_known=1");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "linux_ioctl_contracts=0");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "resource_bind_contracts=0");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "display_completion_contracts=0");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "reject_reasons=0x7f");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "custom_host_tool=0");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "present_id=0");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "completed=0");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "native_present_credit=0");
    require_output_token("dxg_scanout_bind_candidate_command_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("dxg_scanout_bind_candidate_command_matrix",
                              output,
                              "dxg_scanout_bind_candidate_command_matrix",
                              "status=PASS");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "wsl_dxg_ioctl_namespace_probe_matrix");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "last_known_ioctl_nr=0x49");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "display_bind_probe_nr=0x4a");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "wrong_type_rc=-1");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "wrong_size_rc=-1");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "wrong_dir_rc=-1");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "after_last_rc=-1");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "future_high_rc=-1");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "accepted_ioctls=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "display_bind_ioctl_present=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "present_source_ioctl_present=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "present_completion_ioctl_present=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "linux_ioctl_contracts=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "resource_bind_contracts=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "display_completion_contracts=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "transport_present=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "present_id=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "completed=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "native_present_credit=0");
    require_output_token("wsl_dxg_ioctl_namespace_probe_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("wsl_dxg_ioctl_namespace_probe_matrix",
                              output,
                              "wsl_dxg_ioctl_namespace_probe_matrix",
                              "status=PASS");
    require_output_token("dxg_host_to_vm_presenthistory_completion_matrix",
                         output,
                         "dxg_host_to_vm_presenthistory_completion_matrix");
    require_output_token("dxg_host_to_vm_presenthistory_completion_matrix",
                         output, "propagate_presenthistory_cmd=1");
    require_output_token("dxg_host_to_vm_presenthistory_completion_matrix",
                         output, "presenthistory_packets=");
    require_output_token("dxg_host_to_vm_presenthistory_completion_matrix",
                         output, "presenthistory_head_len=");
    require_output_token("dxg_host_to_vm_presenthistory_completion_matrix",
                         output, "completion_contracts=0");
    require_output_token("dxg_host_to_vm_presenthistory_completion_matrix",
                         output, "completion_successes_delta=0");
    require_output_token("dxg_host_to_vm_presenthistory_completion_matrix",
                         output, "present_id=0");
    require_output_token("dxg_host_to_vm_presenthistory_completion_matrix",
                         output, "completed=0");
    require_output_token("dxg_host_to_vm_presenthistory_completion_matrix",
                         output, "native_present_credit=0");
    require_output_token("dxg_host_to_vm_presenthistory_completion_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token(
        "dxg_host_to_vm_presenthistory_completion_matrix", output,
        "dxg_host_to_vm_presenthistory_completion_matrix", "status=PASS");
    require_output_token("dxg_presenthistory_orphan_completion_rejection_matrix",
                         output,
                         "dxg_presenthistory_orphan_completion_rejection_matrix");
    require_output_token("dxg_presenthistory_orphan_completion_rejection_matrix",
                         output, "propagate_presenthistory_cmd=1");
    require_output_token("dxg_presenthistory_orphan_completion_rejection_matrix",
                         output, "provider_pending_match=0");
    require_output_token("dxg_presenthistory_orphan_completion_rejection_matrix",
                         output, "completion_demux_registered=0");
    require_output_token("dxg_presenthistory_orphan_completion_rejection_matrix",
                         output, "display_bind_present_id=0");
    require_output_token("dxg_presenthistory_orphan_completion_rejection_matrix",
                         output, "display_bind_completed_id=0");
    require_output_token("dxg_presenthistory_orphan_completion_rejection_matrix",
                         output, "orphan_completion_rejected=1");
    require_output_token("dxg_presenthistory_orphan_completion_rejection_matrix",
                         output, "native_present_credit=0");
    require_output_token("dxg_presenthistory_orphan_completion_rejection_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token(
        "dxg_presenthistory_orphan_completion_rejection_matrix", output,
        "dxg_presenthistory_orphan_completion_rejection_matrix",
        "status=PASS");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output,
                         "dxg_native_present_lane_rejection_matrix");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "wsl_presenthistory_enum_only=REJECTED");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "wsl_presenthistory_sender_contract=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "wsl_presenthistory_completion_contract=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "vmbus_enum_known=1");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "linux_ioctl_contracts=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "resource_bind_contracts=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "display_completion_contracts=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "reject_reasons=0x7f");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "synthvid_gpa_dirty_only=REJECTED");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output,
                         "linux_hyperv_drm_shadow_blit_only=REJECTED");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "synthvid_d3d12_resource_bind=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "dda_pci_display_present=");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "dda_d3d12_resource_import=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "dda_scanout_bind=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "dda_hw_flip_completion=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "custom_host_tool=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "transport_present=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "present_id=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "completed=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "native_present_credit=0");
    require_output_token("dxg_native_present_lane_rejection_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("dxg_native_present_lane_rejection_matrix",
                              output,
                              "dxg_native_present_lane_rejection_matrix",
                              "status=PASS");
    require_output_token("dxg_scanout_bind_weak_evidence_matrix",
                         output, "dxg_scanout_bind_weak_evidence_matrix");
    require_output_token("dxg_scanout_bind_weak_evidence_matrix",
                         output, "weak_evidence_rejects=");
    require_output_token("dxg_scanout_bind_weak_evidence_matrix",
                         output, "successes=0");
    require_output_token("dxg_scanout_bind_weak_evidence_matrix",
                         output, "present_id=0");
    require_output_token("dxg_scanout_bind_weak_evidence_matrix",
                         output, "completed=0");
    require_output_token("dxg_scanout_bind_weak_evidence_matrix",
                         output, "native_present_credit=0");
    require_output_token("dxg_scanout_bind_weak_evidence_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("dxg_scanout_bind_weak_evidence_matrix",
                              output,
                              "dxg_scanout_bind_weak_evidence_matrix",
                              "status=PASS");
    require_output_token("wsl_standard_alloc_surface_abi_matrix",
                         output, "wsl_standard_alloc_surface_abi_matrix");
    require_output_token("wsl_standard_alloc_surface_abi_matrix",
                         output, "shared_primary_size=24");
    require_output_token("wsl_standard_alloc_surface_abi_matrix",
                         output, "shadow_size=16");
    require_output_token("wsl_standard_alloc_surface_abi_matrix",
                         output, "staging_size=12");
    require_output_token("wsl_standard_alloc_surface_abi_matrix",
                         output, "gdi_size=24");
    require_output_token("wsl_standard_alloc_surface_abi_matrix",
                         output, "command_union=sharedprimary,shadow,staging,gdi");
    require_output_token("wsl_standard_alloc_surface_abi_matrix",
                         output, "standard_alloc_role=private_driver_data");
    require_output_token("wsl_standard_alloc_surface_abi_matrix",
                         output, "display_bind_ioctl=0");
    require_output_token("wsl_standard_alloc_surface_abi_matrix",
                         output, "native_present_credit=0");
    require_output_token("wsl_standard_alloc_surface_abi_matrix",
                         output, "opengl_submit_credit=0");
    require_output_line_token("wsl_standard_alloc_surface_abi_matrix",
                              output,
                              "wsl_standard_alloc_surface_abi_matrix",
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
    printf("gpu_core_c_validator present_source_software_path_rejection_matrix "
           "framebuffer_blit=REJECTED cpu_map_readback=REJECTED "
           "dri_software_present=REJECTED copy_export_fallback=REJECTED "
           "callback_only=REJECTED release_only=REJECTED "
           "display_target_kind=0 native_present_claim=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator d3d12_shared_resource_fd_lifetime_matrix "
           "register_live_fd=PASS invalid_fd_rejected=PASS "
           "unverified_resource_fd=PASS stale_source_after_owner_close=PASS "
           "cleanup_balance=PASS native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator d3d12_present_source_admission_matrix "
           "same_adapter_luid=PASS resource_fd=PASS d3dkmt_handles=PASS "
           "dimensions=PASS format_modifier=PASS wait_sync_metadata=PASS "
           "unverified_resource_fd=PASS adapter_mismatch=PASS "
           "source_owner=PASS failclosed=PASS native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator d3d12_acquire_fence_lifetime_matrix "
           "monitored_fence=PASS wait_metadata=PASS "
           "wait_commit_failclosed=PASS query_sync_matches=PASS "
           "stale_source_cleanup=PASS native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator "
           "d3d12_present_bind_contract_failclosed_matrix "
           "foreign_source=PASS stale_source=PASS "
           "software_paths_rejected=PASS transport_present=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator "
           "d3d12_present_resource_fd_typed_admission_matrix "
           "typed_resource_fd=PASS sealed_before_admit=PASS "
           "shared_records_valid=PASS allocation_match=PASS "
           "generation_from_shared=PASS invalid_fd_rejected=PASS "
           "stale_source_cleanup=PASS native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator "
           "d3d12_native_completion_zero_credit_matrix "
           "display_bind=ABSENT transport_present=0 "
           "completion_source=required present_id=0 completed=0 "
           "callbacks_after_completion=0 releases_after_completion=0 "
           "per_client_generation=required native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator "
           "d3d12_native_completion_future_contract_matrix "
           "display_bind_gate=closed requires_present_id=1 "
           "requires_completed_ge_present=1 "
           "requires_same_resource_generation=1 "
           "requires_callback_release_after_completion=1 "
           "requires_close_before_signal_cancel=1 "
           "requires_cleanup_balance=1 native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS_FAILCLOSED\n");
    printf("gpu_core_c_validator "
           "dxg_resource_scanout_bind_host_abi_matrix "
           "selected_lane=gpup_dxg_scanout_bind custom_host_tool=0 "
           "wsl_dxg_display_bind_ioctl=0 "
           "wsl_ioctl_namespace_checked=1 "
           "wsl_display_bind_ioctl_absent=1 "
           "synthvid_vram_bridge=gpa_dirty_only "
           "standard_alloc_role=private_driver_data "
           "standard_alloc_display_bind_absent=1 missing_host_abi=1 "
           "transport_present=0 display_target_kind=0 present_id=0 "
           "completed=0 native_present_credit=0 opengl_submit_credit=0 "
           "status=PASS\n");
    printf("gpu_core_c_validator "
           "dxg_scanout_bind_skeleton_matrix "
           "attempts=0 rejects=0 successes=0 completion_queries=0 "
           "completion_successes=0 completion_pending=0 "
           "weak_evidence_rejects=0 transport=0 status_code=0 "
           "present_id=0 completed=0 source_generation=0 "
           "resource_generation=0 dirty_sequence=0 dirty_rects=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator "
           "dxg_scanout_bind_candidate_command_matrix "
           "presenthistory_cmd=34 redirected_flip_fence_cmd=35 blt_cmd=38 "
           "propagate_presenthistory_cmd=1 cmds_known=4 "
           "sender_contracts=0 completion_contracts=0 "
           "candidate_rejects=0 custom_host_tool=0 transport_present=0 "
           "vmbus_enum_known=1 linux_ioctl_contracts=0 "
           "resource_bind_contracts=0 display_completion_contracts=0 "
           "reject_reasons=0x7f "
           "present_id=0 completed=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=PASS\n");
    printf("gpu_core_c_validator "
           "dxg_native_present_lane_rejection_matrix "
           "wsl_presenthistory_enum_only=REJECTED "
           "wsl_presenthistory_sender_contract=0 "
           "wsl_presenthistory_completion_contract=0 "
           "synthvid_gpa_dirty_only=REJECTED "
           "linux_hyperv_drm_shadow_blit_only=REJECTED "
           "synthvid_gpa_dirty_present=0 "
           "synthvid_d3d12_resource_bind=0 "
           "dda_nouveau_separate_pci_path=ABSENT "
           "dda_pci_display_present=0 "
           "dda_d3d12_resource_import=0 dda_scanout_bind=0 "
           "dda_hw_flip_completion=0 "
           "vmbus_enum_known=1 linux_ioctl_contracts=0 "
           "resource_bind_contracts=0 display_completion_contracts=0 "
           "reject_reasons=0x7f "
           "custom_host_tool=0 transport_present=0 present_id=0 "
           "completed=0 native_present_credit=0 opengl_submit_credit=0 "
           "status=PASS\n");
    printf("gpu_core_c_validator "
           "dxg_scanout_bind_weak_evidence_matrix "
           "dxg_ready_only=0 d3dkmt_handles_only=0 "
           "same_adapter_resource_only=0 syncfile_only=0 "
           "synthvid_gpa_dirty_only=0 software_or_readback_path=0 "
           "weak_evidence_rejects=0 successes=0 present_id=0 "
           "completed=0 native_present_credit=0 opengl_submit_credit=0 "
           "status=PASS\n");
    printf("gpu_core_c_validator "
           "wsl_standard_alloc_surface_abi_matrix "
           "shared_primary_size=24 shadow_size=16 staging_size=12 "
           "gdi_size=24 command_union=sharedprimary,shadow,staging,gdi "
           "standard_alloc_role=private_driver_data display_bind_ioctl=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=PASS\n");
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
    unsigned long native_display_required_rejects;
    const char *nouveau_dma_map_state;
    const char *nouveau_dma_mask_state;
    const char *nouveau_coherent_dma_mask_state;
    const char *dxg_scanout_bind_state;
    int hyperv_gpup_failclosed;
    int native_display_failclosed_ok;
    int backend_opengl_submit;
    int nouveau_display_kms_ready;
    int nouveau_native_display_claimed;
    int nouveau_atomic_pageflip_backend_missing_ok;
    int nouveau_linux_display_readiness_ok;
    int nouveau_display_kms_registered;
    int nouveau_display_kms_registration_ok;
    int nouveau_kms_vblank_irq_source_ok;
    int nouveau_primary_plane_modifier_failclosed_ok;
    int kms_scanout_cpu_convert_separation_ok;
    int kms_gem_fb_plane_ref_ok;
    int kms_atomic_plane_state_ok;
    int kms_atomic_prepare_cleanup_fb_ok;
    int kms_page_flip_feature_gate_ok;
    int display_bind_id_shape_ok;
    int display_bind_success_shape_ok;
    int provider_credit_gate_ok;
    int display_bind_request_metadata_ok;
    int display_bind_pending_lifetime_ok;
    int display_bind_generation_revalidation_ok;
    int display_bind_provider_pending_publication_ok;
    int display_bind_provider_shared_parent_retention_ok;
    int display_bind_provider_sync_fence_alias_ok;
    int display_bind_provider_packet_lifetime_ok;
    int display_bind_provider_no_send_preflight_ok;
    int native_completion_lifetime_ok;
    int stale_source_zero_credit_ok;
    int stale_async_completion_contract_ok;
    int generic_completion_not_native_ok;
    int native_completion_consumer_escrow_ok;
    int standard_alloc_not_display_bind_ok;
    int dda_nouveau_separate_display_not_bind_ok;
    int foreign_prime_import_gap_ok;
    int wsl_uapi_negative_ok;
    int wsl_adapter_display_caps_negative_ok;
    int wsl_submit_present_fields_not_bind_ok;
    int wsl_stdalloc_and_alloc_flags_not_bind_ok;
    int wsl_trace_display_bind_negative_ok;
    int public_present_api_not_guest_bind_ok;
    int provider_credit_gate_negative_ok;
    int dda_nouveau_non_readback_display_proof_ok;
    int d3d12_display_bind_host_abi_discovery_ok;
    int d3d12_negative_abi_manifest_ok;
    int d3d12_display_bind_authority_chain_ok;
    int dda_nouveau_d3d12_bridge_disjoint_ok;
    int host_display_bind_source_catalog_ok;
    int d3d12_completion_source_authority_ok;
    int native_present_completion_source_namespace_ok;
    int gpu_remaining_holistic_skeleton_ok;

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

    nouveau_dma_map_state =
        stats.nouveau_pci_probe_accepts == 0 ? "GPU_P_FAIL_CLOSED" :
        (stats.nouveau_pci_dma_map_api_present != 0 &&
         stats.nouveau_pci_dma_map_attempts != 0 &&
         stats.nouveau_pci_dma_map_successes != 0 &&
         stats.nouveau_pci_dma_map_failures == 0 &&
         stats.nouveau_pci_dma_unmaps ==
             stats.nouveau_pci_dma_map_successes ? "PASS" : "FAIL");
    nouveau_dma_mask_state =
        stats.nouveau_pci_probe_accepts == 0 ? "NOT_CONFIGURED" :
        (stats.nouveau_pci_dma_mask_configured != 0 &&
         stats.nouveau_pci_dma_mask_requested_bits >= 32 &&
         stats.nouveau_pci_dma_mask_effective_bits >= 32 &&
         stats.nouveau_pci_dma_mask_bits ==
             stats.nouveau_pci_dma_mask_effective_bits ? "PASS" : "FAIL");
    nouveau_coherent_dma_mask_state =
        stats.nouveau_pci_probe_accepts == 0 ? "NOT_CONFIGURED" :
        (stats.nouveau_pci_coherent_dma_mask_configured != 0 &&
         stats.nouveau_pci_coherent_dma_mask_requested_bits >= 32 &&
         stats.nouveau_pci_coherent_dma_mask_effective_bits >= 32 &&
         stats.nouveau_pci_coherent_dma_mask_bits ==
             stats.nouveau_pci_coherent_dma_mask_effective_bits ? "PASS" :
                                                                  "FAIL");
    dxg_scanout_bind_state =
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_last_present_id == 0 &&
        stats.dxg_scanout_bind_last_completed == 0 &&
        (stats.dxg_scanout_bind_attempts == 0 ||
         (stats.dxg_scanout_bind_rejects >=
              stats.dxg_scanout_bind_attempts &&
          stats.dxg_scanout_bind_weak_evidence_rejects >=
              stats.dxg_scanout_bind_attempts)) &&
        (stats.dxg_scanout_bind_completion_queries == 0 ||
         stats.dxg_scanout_bind_completion_pending >=
             stats.dxg_scanout_bind_completion_queries) ? "PASS" : "FAIL";
    hyperv_gpup_failclosed =
        backend.backend == FB_GPU_BACKEND_HYPERV_DXG &&
        (backend.flags & FB_GPU_BACKEND_F_DXG_TRANSPORT) != 0 &&
        (backend.flags & FB_GPU_BACKEND_F_D3DKMT) != 0 &&
        (backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) == 0 &&
        stats.nouveau_pci_probe_accepts == 0;
    backend_opengl_submit =
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0;
    native_display_required_rejects =
        FB_GPU_KMS_PRESENT_REJECT_ALL &
        ~FB_GPU_KMS_PRESENT_REJECT_NO_ATOMIC_PAGEFLIP_BACKEND;
    native_display_failclosed_ok =
        hyperv_gpup_failclosed &&
        stats.nouveau_native_display_ready == 0 &&
        stats.nouveau_dda_native_display_present == 0 &&
        stats.nouveau_display_probe_attempts == 0 &&
        stats.nouveau_display_create_attempts == 0 &&
        stats.nouveau_display_create_successes == 0 &&
        stats.nouveau_display_create_fail_reason == 0 &&
        stats.nouveau_display_head_probe_attempts == 0 &&
        stats.nouveau_display_heads == 0 &&
        stats.nouveau_display_connector_probe_attempts == 0 &&
        stats.nouveau_display_connectors == 0 &&
        stats.nouveau_display_nonvirtual_connectors == 0 &&
        stats.nouveau_display_engine_object_created == 0 &&
        stats.nouveau_display_mode_config_ready == 0 &&
        stats.nouveau_display_crtc_count == 0 &&
        stats.nouveau_display_encoder_count == 0 &&
        stats.nouveau_display_primary_plane_count == 0 &&
        stats.nouveau_display_primary_plane_linear_required != 0 &&
        stats.nouveau_display_primary_plane_nonlinear_modifiers == 0 &&
        stats.nouveau_display_outp_mask_seen == 0 &&
        stats.nouveau_display_conn_mask_seen == 0 &&
        stats.nouveau_display_head_mask_seen == 0 &&
        stats.nouveau_display_nvif_head_ctor_successes == 0 &&
        stats.nouveau_display_hpd_event_registered == 0 &&
        stats.nouveau_display_dp_irq_event_registered == 0 &&
        stats.nouveau_display_vblank_supported == 0 &&
        stats.nouveau_display_vblank_irq_supported == 0 &&
        stats.nouveau_display_vblank_event_registered == 0 &&
        stats.nouveau_display_atomic_commit_tail_ready == 0 &&
        stats.nouveau_display_page_flip_event_source ==
            FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE &&
        stats.nouveau_display_vblank_source ==
            FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE &&
        stats.nouveau_display_vblank_irqs == 0 &&
        stats.nouveau_display_page_flip_completion_ready == 0 &&
        stats.nouveau_display_page_flip_completions == 0 &&
        stats.nouveau_display_atomic_pageflip_backend_missing == 0 &&
        (stats.nouveau_native_display_reject_reasons &
            native_display_required_rejects) ==
            native_display_required_rejects &&
        stats.kms_present_last_lane == FB_GPU_KMS_PRESENT_LANE_NONE &&
        stats.kms_present_dumb == 0 &&
        stats.kms_present_synthvid == 0 &&
        stats.kms_present_nouveau_hw == 0 &&
        (stats.kms_present_reject_reasons &
            native_display_required_rejects) ==
            native_display_required_rejects &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    nouveau_display_kms_ready =
        stats.nouveau_display_create_successes != 0 &&
        stats.nouveau_display_engine_object_created != 0 &&
        stats.nouveau_display_mode_config_ready != 0 &&
        stats.nouveau_display_crtc_count != 0 &&
        stats.nouveau_display_encoder_count != 0 &&
        stats.nouveau_display_primary_plane_count != 0 &&
        stats.nouveau_display_primary_plane_linear_required != 0 &&
        stats.nouveau_display_primary_plane_nonlinear_modifiers == 0 &&
        stats.nouveau_display_outp_mask_seen != 0 &&
        stats.nouveau_display_conn_mask_seen != 0 &&
        stats.nouveau_display_head_mask_seen != 0 &&
        stats.nouveau_display_nvif_head_ctor_successes != 0 &&
        stats.nouveau_display_heads != 0 &&
        stats.nouveau_display_connectors != 0 &&
        stats.nouveau_display_nonvirtual_connectors != 0 &&
        stats.nouveau_display_hpd_event_registered != 0 &&
        stats.nouveau_display_dp_irq_event_registered != 0 &&
        stats.nouveau_display_vblank_supported != 0 &&
        stats.nouveau_display_vblank_irq_supported != 0 &&
        stats.nouveau_display_vblank_event_registered != 0 &&
        stats.nouveau_display_atomic_commit_tail_ready != 0 &&
        stats.nouveau_display_page_flip_event_source ==
            FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ &&
        stats.nouveau_display_vblank_source ==
            FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ &&
        stats.nouveau_display_page_flip_completion_ready != 0 &&
        stats.nouveau_display_page_flip_completions != 0 &&
        stats.nouveau_display_atomic_pageflip_backend_missing == 0;
    nouveau_atomic_pageflip_backend_missing_ok =
        stats.nouveau_display_atomic_pageflip_backend_missing == 0 ||
        (stats.nouveau_pci_probe_accepts != 0 &&
         stats.nouveau_display_probe_attempts != 0 &&
         stats.nouveau_native_display_ready == 0 &&
         stats.nouveau_display_atomic_pageflip_backend_missing == 1);
    nouveau_native_display_claimed =
        stats.nouveau_native_display_ready != 0 ||
        stats.nouveau_dda_native_display_present != 0 ||
        stats.nouveau_pci_native_present_credit != 0 ||
        stats.kms_present_nouveau_hw != 0 ||
        stats.kms_present_last_lane == FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW;
    nouveau_display_kms_registered =
        nouveau_display_kms_ready &&
        stats.nouveau_native_display_ready != 0;
    nouveau_display_kms_registration_ok =
        !hyperv_gpup_failclosed ||
        (native_display_failclosed_ok && nouveau_display_kms_registered == 0);
    nouveau_kms_vblank_irq_source_ok =
        !hyperv_gpup_failclosed ||
        (stats.nouveau_display_vblank_supported == 0 &&
         stats.nouveau_display_vblank_irq_supported == 0 &&
         stats.nouveau_display_vblank_source ==
             FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE &&
         stats.nouveau_display_vblank_irqs == 0 &&
         stats.nouveau_display_page_flip_completion_ready == 0 &&
         stats.nouveau_display_page_flip_completions == 0 &&
         stats.nouveau_display_atomic_pageflip_backend_missing == 0 &&
         stats.nouveau_pci_irq_delivery_claimed == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         backend_opengl_submit == 0);
    nouveau_primary_plane_modifier_failclosed_ok =
        !hyperv_gpup_failclosed ||
        (stats.kms_present_nouveau_hw == 0 &&
         stats.nouveau_display_primary_plane_count == 0 &&
         stats.nouveau_display_primary_plane_linear_required != 0 &&
         stats.nouveau_display_primary_plane_nonlinear_modifiers == 0 &&
         stats.nouveau_native_display_ready == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         backend_opengl_submit == 0);
    nouveau_linux_display_readiness_ok =
        stats.nouveau_pci_probe_accepts == 0 ?
            (stats.nouveau_display_engine_object_created == 0 &&
             stats.nouveau_display_mode_config_ready == 0 &&
             stats.nouveau_display_crtc_count == 0 &&
             stats.nouveau_display_encoder_count == 0 &&
             stats.nouveau_display_primary_plane_count == 0 &&
             stats.nouveau_display_primary_plane_linear_required != 0 &&
             stats.nouveau_display_primary_plane_nonlinear_modifiers == 0 &&
             stats.nouveau_display_outp_mask_seen == 0 &&
             stats.nouveau_display_conn_mask_seen == 0 &&
             stats.nouveau_display_head_mask_seen == 0 &&
             stats.nouveau_display_nvif_head_ctor_successes == 0 &&
             stats.nouveau_display_vblank_event_registered == 0 &&
             stats.nouveau_display_hpd_event_registered == 0 &&
             stats.nouveau_display_dp_irq_event_registered == 0 &&
             stats.nouveau_display_atomic_commit_tail_ready == 0 &&
             stats.nouveau_display_page_flip_event_source ==
                 FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE &&
             stats.nouveau_native_display_ready == 0 &&
             stats.nouveau_pci_native_present_credit == 0 &&
             backend_opengl_submit == 0) :
            nouveau_display_kms_ready;
    kms_scanout_cpu_convert_separation_ok =
        stats.kms_present_nouveau_hw == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    kms_gem_fb_plane_ref_ok =
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    kms_atomic_plane_state_ok =
        stats.kms_atomic_out_fence_software_scanout_correlated == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    kms_atomic_prepare_cleanup_fb_ok =
        stats.kms_atomic_in_fence_fd_refs ==
            stats.kms_atomic_in_fence_fd_ref_puts &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    kms_page_flip_feature_gate_ok =
        stats.kms_present_nouveau_hw == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    display_bind_id_shape_ok =
        ((stats.dxg_display_bind_present_id == 0 &&
          stats.dxg_display_bind_completed_id == 0) ||
         (stats.dxg_display_bind_present_id != 0 &&
          stats.dxg_display_bind_completed_id >=
              stats.dxg_display_bind_present_id &&
          stats.dxg_display_bind_source_generation != 0 &&
          stats.dxg_display_bind_resource_generation != 0)) &&
        ((stats.dxg_scanout_bind_last_present_id == 0 &&
          stats.dxg_scanout_bind_last_completed == 0) ||
         (stats.dxg_scanout_bind_last_present_id != 0 &&
          stats.dxg_scanout_bind_last_completed >=
              stats.dxg_scanout_bind_last_present_id &&
          stats.dxg_scanout_bind_last_source_generation != 0 &&
          stats.dxg_scanout_bind_last_resource_generation != 0));
    provider_credit_gate_ok =
        (backend_opengl_submit == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         stats.dxg_scanout_bind_successes == 0 &&
         stats.dxg_scanout_bind_completion_successes == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         stats.dxg_present_helper_transport_present == 0 &&
         stats.dxg_present_display_target_kind ==
             FB_GPU_DXG_DISPLAY_TARGET_NONE) ||
        (stats.dxg_display_bind_provider_no_host_abi == 0 &&
         stats.dxg_display_bind_provider_no_sender == 0 &&
         stats.dxg_display_bind_provider_no_completion == 0);
    display_bind_request_metadata_ok =
        stats.dxg_display_bind_provider_submits == 0 ||
        (stats.dxg_display_bind_request_metadata_complete == 1 &&
         stats.dxg_display_bind_request_sync_metadata_complete == 1 &&
         stats.dxg_display_bind_request_missing_metadata == 0 &&
         stats.dxg_display_bind_source_generation != 0 &&
         stats.dxg_display_bind_resource_generation != 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0);
    display_bind_pending_lifetime_ok =
        stats.dxg_display_bind_pending_active == 0 &&
        stats.dxg_display_bind_pending_created >=
            stats.dxg_display_bind_pending_completed +
            stats.dxg_display_bind_pending_failclosed +
            stats.dxg_display_bind_pending_cancelled &&
        (stats.dxg_display_bind_pending_created == 0 ||
         (stats.dxg_display_bind_pending_sequence != 0 &&
          stats.dxg_display_bind_pending_peak != 0 &&
          stats.dxg_display_bind_pending_last_source_generation != 0 &&
          stats.dxg_display_bind_pending_last_resource_generation != 0)) &&
        (stats.dxg_display_bind_transport_present != 0 ||
         stats.dxg_display_bind_pending_completed == 0) &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        backend_opengl_submit == 0;
    display_bind_generation_revalidation_ok =
        stats.dxg_display_bind_provider_submits == 0 ||
        (stats.dxg_display_bind_lock_dropped_submits != 0 &&
         stats.dxg_display_bind_revalidate_attempts != 0 &&
         stats.dxg_display_bind_revalidate_successes != 0 &&
         stats.dxg_display_bind_revalidate_failures == 0 &&
         stats.dxg_display_bind_provider_pin_revalidated != 0 &&
         stats.dxg_display_bind_source_generation != 0 &&
         stats.dxg_display_bind_resource_generation != 0 &&
         stats.dxg_display_bind_pinned_resource_generation ==
             stats.dxg_display_bind_resource_generation &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         backend_opengl_submit == 0);
    display_bind_provider_pending_publication_ok =
        stats.dxg_display_bind_provider_submits == 0 ||
        (stats.dxg_display_bind_provider_publication_attempts != 0 &&
         stats.dxg_display_bind_provider_pending_owner_generation != 0 &&
         stats.dxg_display_bind_provider_pending_source_generation != 0 &&
         stats.dxg_display_bind_provider_pending_resource_generation != 0 &&
         stats.dxg_display_bind_provider_pending_dxgprocess_generation ==
             stats.dxg_display_bind_provider_pending_owner_generation &&
         stats.dxg_display_bind_provider_pending_process_adapter_generation != 0 &&
         stats.dxg_display_bind_provider_pending_hmgr_index_unique_valid != 0 &&
         stats.dxg_display_bind_provider_pending_process_namespace_valid != 0 &&
         stats.dxg_display_bind_provider_pending_device_hmgr_index_unique_valid != 0 &&
         stats.dxg_display_bind_provider_pending_resource_hmgr_index_unique_valid != 0 &&
         stats.dxg_display_bind_provider_pending_allocation_hmgr_index_unique_valid != 0 &&
         stats.dxg_display_bind_provider_pending_device_object_ref_active != 0 &&
         stats.dxg_display_bind_provider_pending_resource_object_ref_active != 0 &&
         stats.dxg_display_bind_provider_pending_allocation_object_ref_active != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_id != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_refs != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_children != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_fd_refs != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_global_share != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_host_nt_handle != 0 &&
         stats.dxg_display_bind_provider_pending_opened_child_parent_id_match != 0 &&
         stats.dxg_display_bind_provider_pending_opened_child_global_share_match != 0 &&
         stats.dxg_display_bind_provider_pending_opened_child_sealed_generation_match != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_snapshot_valid != 0 &&
         stats.dxg_display_bind_provider_pending_opened_child_snapshot_valid != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_global_share_match != 0 &&
         stats.dxg_display_bind_provider_pending_owner_close_cancelled == 0 &&
         (stats.dxg_display_bind_provider_pending_syncobject_object_ref_active == 0 ||
          (stats.dxg_display_bind_provider_pending_syncobject_shared_owner_present != 0 &&
           stats.dxg_display_bind_provider_pending_syncobject_monitored_fence != 0 &&
           stats.dxg_display_bind_provider_pending_syncobject_fence_value != 0 &&
           stats.dxg_display_bind_provider_pending_syncobject_fence_cpu_va_present != 0 &&
           stats.dxg_display_bind_provider_pending_syncobject_fence_gpu_va_present != 0 &&
           stats.dxg_display_bind_provider_pending_syncobject_fence_kva_present != 0 &&
           (stats.dxg_display_bind_provider_pending_syncobject_real_fence_gpu_va_present != 0 ||
            stats.dxg_display_bind_provider_pending_syncobject_fence_gpu_va_alias_gap != 0) &&
           stats.dxg_display_bind_provider_pending_syncobject_fence_map_size != 0)) &&
         stats.dxg_display_bind_provider_pending_source_generation ==
             stats.dxg_display_bind_source_generation &&
         stats.dxg_display_bind_provider_pending_resource_generation ==
             stats.dxg_display_bind_resource_generation &&
         stats.dxg_display_bind_pending_last_owner_generation ==
             stats.dxg_display_bind_provider_pending_owner_generation &&
         stats.dxg_display_bind_pending_last_source_generation ==
             stats.dxg_display_bind_provider_pending_source_generation &&
         stats.dxg_display_bind_pending_last_resource_generation ==
             stats.dxg_display_bind_provider_pending_resource_generation &&
         stats.dxg_display_bind_provider_publish_before_send == 0 &&
         stats.dxg_display_bind_provider_transport_pending_id == 0 &&
         stats.dxg_display_bind_provider_command_id == 0 &&
         stats.dxg_display_bind_provider_transaction_id == 0 &&
         stats.dxg_display_bind_provider_channel == 0 &&
         stats.dxg_display_bind_provider_completion_demux_registered == 0 &&
         stats.dxg_display_bind_transport_source ==
             FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE &&
         stats.dxg_display_bind_host_saw_packet == 0 &&
         stats.dxg_display_bind_wsl_presenthistory_completion_credit == 0 &&
         ((stats.dxg_display_bind_provider_resolved_or_cancelled != 0 &&
           stats.dxg_display_bind_provider_refs_released != 0 &&
           stats.dxg_display_bind_provider_no_host_abi_cancelled != 0 &&
           stats.dxg_display_bind_provider_no_host_abi_refs_released != 0) ||
          (stats.dxg_display_bind_provider_submits == 0 &&
           stats.dxg_display_bind_provider_resolved_or_cancelled == 0 &&
           stats.dxg_display_bind_provider_refs_released == 0 &&
           stats.dxg_display_bind_provider_no_host_abi_cancelled == 0 &&
           stats.dxg_display_bind_provider_no_host_abi_refs_released == 0)) &&
         stats.dxg_display_bind_provider_no_host_abi != 0 &&
         stats.dxg_display_bind_provider_no_sender != 0 &&
         stats.dxg_display_bind_provider_no_completion != 0 &&
         stats.dxg_display_bind_transport_present == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
         stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
         backend_opengl_submit == 0);
    display_bind_provider_shared_parent_retention_ok =
        stats.dxg_display_bind_provider_submits == 0 ||
        (stats.dxg_display_bind_provider_pending_shared_parent_id != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_refs != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_fd_refs != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_host_nt_refs != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_child_refs != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_children != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_global_share != 0 &&
         stats.dxg_display_bind_provider_pending_shared_parent_host_nt_handle != 0 &&
         stats.dxg_display_bind_provider_pending_opened_child_parent_id_match != 0 &&
         stats.dxg_display_bind_provider_pending_opened_child_global_share_match != 0 &&
         stats.dxg_display_bind_provider_pending_opened_child_sealed_generation_match != 0 &&
         stats.dxg_display_bind_host_saw_packet == 0 &&
         stats.dxg_display_bind_transport_source ==
             FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         backend_opengl_submit == 0);
    display_bind_provider_sync_fence_alias_ok =
        stats.dxg_display_bind_provider_pending_syncobject_object_ref_active == 0 ||
        (stats.dxg_display_bind_provider_pending_syncobject_shared_owner_present != 0 &&
         stats.dxg_display_bind_provider_pending_syncobject_monitored_fence != 0 &&
         stats.dxg_display_bind_provider_pending_syncobject_fence_value != 0 &&
         stats.dxg_display_bind_provider_pending_syncobject_fence_cpu_va_present != 0 &&
         stats.dxg_display_bind_provider_pending_syncobject_fence_kva_present != 0 &&
         stats.dxg_display_bind_provider_pending_syncobject_fence_gpu_va_present != 0 &&
         (stats.dxg_display_bind_provider_pending_syncobject_real_fence_gpu_va_present != 0 ||
          stats.dxg_display_bind_provider_pending_syncobject_fence_gpu_va_alias_gap != 0) &&
         stats.dxg_display_bind_provider_pending_syncobject_fence_map_size != 0 &&
         stats.dxg_display_bind_host_saw_packet == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         backend_opengl_submit == 0);
    display_bind_provider_packet_lifetime_ok =
        stats.dxg_display_bind_provider_submits == 0 ||
        (stats.dxg_display_bind_provider_publication_attempts != 0 &&
         stats.dxg_display_bind_provider_publish_before_send == 0 &&
         stats.dxg_display_bind_provider_transport_pending_id == 0 &&
         stats.dxg_display_bind_provider_command_id == 0 &&
         stats.dxg_display_bind_provider_transaction_id == 0 &&
         stats.dxg_display_bind_provider_channel == 0 &&
         stats.dxg_display_bind_provider_completion_demux_registered == 0 &&
         stats.dxg_display_bind_provider_resolved_or_cancelled != 0 &&
         stats.dxg_display_bind_provider_refs_released != 0 &&
         stats.dxg_display_bind_pending_active == 0 &&
         stats.dxg_display_bind_host_saw_packet == 0 &&
         stats.dxg_display_bind_transport_source ==
             FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE &&
         stats.dxg_display_bind_wsl_presenthistory_completion_credit == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         backend_opengl_submit == 0);
    display_bind_provider_no_send_preflight_ok =
        stats.dxg_display_bind_provider_submits == 0 ||
        (stats.dxg_display_bind_request_metadata_complete == 1 &&
         stats.dxg_display_bind_provider_pin_revalidated == 1 &&
         stats.dxg_display_bind_provider_preflight_ready == 1 &&
         stats.dxg_display_bind_provider_pending_source_generation != 0 &&
         stats.dxg_display_bind_provider_pending_source_generation ==
             stats.dxg_display_bind_source_generation &&
         stats.dxg_display_bind_provider_pending_resource_generation != 0 &&
         stats.dxg_display_bind_provider_pending_resource_generation ==
             stats.dxg_display_bind_resource_generation &&
         stats.dxg_display_bind_provider_send_attempts == 0 &&
         stats.dxg_display_bind_provider_send_blocked_no_host_abi != 0 &&
         stats.dxg_display_bind_provider_completion_demux_attempts == 0 &&
         stats.dxg_display_bind_provider_completion_demux_blocked_no_contract != 0 &&
         stats.dxg_display_bind_host_saw_packet == 0 &&
         stats.dxg_display_bind_transport_source ==
             FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE &&
         stats.dxg_display_bind_provider_no_host_abi == 1 &&
         stats.dxg_display_bind_provider_no_sender == 1 &&
         stats.dxg_display_bind_provider_no_completion == 1 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         backend_opengl_submit == 0);
    display_bind_success_shape_ok =
        (backend_opengl_submit == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         stats.dxg_scanout_bind_successes == 0 &&
         stats.dxg_scanout_bind_completion_successes == 0 &&
         stats.dxg_display_bind_transport_present == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         (stats.dxg_display_bind_provider_submits == 0 ||
          (stats.dxg_display_bind_provider_no_host_abi != 0 &&
           stats.dxg_display_bind_provider_no_sender != 0 &&
           stats.dxg_display_bind_provider_no_completion != 0))) ||
        (stats.dxg_display_bind_transport_present != 0 &&
         stats.dxg_display_bind_status == 0 &&
         stats.dxg_display_bind_block_reason == 0 &&
         stats.dxg_display_bind_completion_source ==
             FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
         stats.dxg_display_bind_present_id != 0 &&
         stats.dxg_display_bind_completed_id >=
             stats.dxg_display_bind_present_id &&
         stats.dxg_display_bind_source_generation != 0 &&
         stats.dxg_display_bind_resource_generation != 0 &&
         stats.dxg_scanout_bind_successes != 0 &&
         stats.dxg_scanout_bind_completion_successes != 0 &&
         stats.dxg_display_bind_provider_submits != 0 &&
         stats.dxg_display_bind_provider_pin_revalidated != 0 &&
         stats.dxg_display_bind_provider_publication_attempts != 0 &&
         stats.dxg_display_bind_provider_publish_before_send != 0 &&
         stats.dxg_display_bind_provider_transport_pending_id != 0 &&
         stats.dxg_display_bind_provider_command_id != 0 &&
         stats.dxg_display_bind_provider_transaction_id != 0 &&
         stats.dxg_display_bind_provider_channel != 0 &&
         stats.dxg_display_bind_provider_completion_demux_registered != 0 &&
         stats.dxg_display_bind_transport_source ==
             FB_GPU_DXG_DISPLAY_BIND_SOURCE_NON_WSL_DXGKRNL_EXTENSION &&
         stats.dxg_display_bind_host_saw_packet == 1 &&
         stats.dxg_display_bind_wsl_presenthistory_completion_credit == 0 &&
         stats.dxg_display_bind_provider_no_host_abi == 0 &&
         stats.dxg_display_bind_provider_no_sender == 0 &&
         stats.dxg_display_bind_provider_no_completion == 0);
    native_completion_lifetime_ok =
        (backend_opengl_submit == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         stats.dxg_scanout_bind_successes == 0 &&
         stats.dxg_scanout_bind_completion_successes == 0 &&
         stats.dxg_display_bind_transport_present == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         (stats.dxg_display_bind_provider_submits == 0 ||
          stats.dxg_display_bind_provider_no_completion != 0)) ||
        (stats.dxg_display_bind_transport_present != 0 &&
         stats.dxg_display_bind_status == 0 &&
         stats.dxg_display_bind_block_reason == 0 &&
         stats.dxg_display_bind_completion_source ==
             FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
         stats.dxg_display_bind_present_id != 0 &&
         stats.dxg_display_bind_completed_id >=
             stats.dxg_display_bind_present_id &&
         stats.dxg_display_bind_source_generation != 0 &&
         stats.dxg_display_bind_resource_generation != 0 &&
         stats.dxg_scanout_bind_completion_successes != 0 &&
         stats.dxg_display_bind_provider_no_host_abi == 0 &&
         stats.dxg_display_bind_provider_no_sender == 0 &&
         stats.dxg_display_bind_provider_no_completion == 0);
    stale_source_zero_credit_ok =
        stats.dxg_display_bind_late_completion_after_release == 0 &&
        stats.dxg_display_bind_after_close_nonzero_id_rejects == 0 &&
        (stats.dxg_display_bind_after_close_queries == 0 ||
         stats.dxg_display_bind_stale_source_rejects != 0) &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    stale_async_completion_contract_ok =
        stats.dxg_display_bind_provider_submits == 0 ||
        (stats.dxg_display_bind_provider_no_sender != 0 &&
         stats.dxg_display_bind_provider_no_completion != 0 &&
         stats.dxg_display_bind_provider_completion_demux_registered == 0 &&
         stats.dxg_display_bind_provider_transport_pending_id == 0 &&
         stats.dxg_display_bind_pending_active == 0 &&
         stats.dxg_display_bind_present_id == 0 &&
         stats.dxg_display_bind_completed_id == 0 &&
         stats.dxg_display_bind_late_completion_after_release == 0 &&
         stats.dxg_display_bind_after_close_nonzero_id_rejects == 0 &&
         (stats.dxg_display_bind_after_close_queries == 0 ||
          (stats.dxg_display_bind_stale_source_rejects != 0 &&
           stats.dxg_display_bind_stale_generation_rejects != 0 &&
           stats.dxg_display_bind_stale_completion_rejects != 0 &&
           stats.dxg_display_bind_stale_after_release_rejects != 0)) &&
         stats.dxg_display_bind_host_saw_packet == 0 &&
         stats.dxg_display_bind_transport_source ==
             FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE &&
         stats.nouveau_pci_native_present_credit == 0 &&
         backend_opengl_submit == 0);
    generic_completion_not_native_ok =
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.dxg_scanout_bind_last_present_id == 0 &&
        stats.dxg_scanout_bind_last_completed == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.kms_vblank_source_nouveau_hw == 0 &&
        stats.kms_page_flip_events_native_hw == 0;
    native_completion_consumer_escrow_ok =
        native_completion_lifetime_ok &&
        provider_credit_gate_ok &&
        generic_completion_not_native_ok &&
        stats.dxg_present_helper_transport_present == 0 &&
        stats.dxg_present_display_target_kind ==
            FB_GPU_DXG_DISPLAY_TARGET_NONE &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.dxg_display_bind_provider_completion_demux_registered == 0 &&
        stats.dxg_display_bind_provider_transport_pending_id == 0 &&
        stats.dxg_display_bind_host_saw_packet == 0 &&
        stats.dxg_display_bind_transport_source ==
            FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE &&
        (stats.dxg_display_bind_provider_submits == 0 ||
         stats.dxg_display_bind_provider_no_completion != 0) &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    standard_alloc_not_display_bind_ok =
        (stats.dxg_scanout_bind_standard_alloc_private_data == 0 ||
         stats.dxg_scanout_bind_standard_alloc_display_bind_absent != 0) &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0;
    dda_nouveau_separate_display_not_bind_ok =
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.dxg_scanout_bind_dda_resource_import_absent != 0 &&
        stats.dxg_scanout_bind_dda_scanout_bind_absent != 0 &&
        stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    foreign_prime_import_gap_ok =
        stats.dmabuf_local_imports == stats.dmabuf_imports &&
        stats.dmabuf_foreign_import_rejects >=
            stats.dmabuf_foreign_import_attempts &&
        stats.dmabuf_foreign_fd_rejects >=
            stats.dmabuf_foreign_import_rejects &&
        stats.dmabuf_d3d12_foreign_resource_imports == 0 &&
        stats.dmabuf_nouveau_scanout_bind_imports == 0 &&
        stats.dmabuf_native_present_credit == 0 &&
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    wsl_uapi_negative_ok =
        stats.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0;
    wsl_adapter_display_caps_negative_ok =
        stats.dxg_present_dxg_adapter_display_supported == 0 &&
        stats.dxg_present_dxg_adapter_sources == 0 &&
        stats.dxg_present_helper_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0;
    wsl_submit_present_fields_not_bind_ok =
        stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.dxg_display_bind_host_saw_packet == 0 &&
        stats.dxg_display_bind_transport_source ==
            FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE &&
        stats.dxg_display_bind_wsl_presenthistory_completion_credit == 0 &&
        backend_opengl_submit == 0;
    wsl_stdalloc_and_alloc_flags_not_bind_ok =
        standard_alloc_not_display_bind_ok &&
        stats.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        backend_opengl_submit == 0;
    wsl_trace_display_bind_negative_ok =
        stats.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        backend_opengl_submit == 0;
    public_present_api_not_guest_bind_ok =
        stats.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats.dxg_present_helper_transport_present == 0 &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    provider_credit_gate_negative_ok =
        provider_credit_gate_ok &&
        (stats.dxg_display_bind_provider_submits == 0 ||
         (stats.dxg_display_bind_provider_no_host_abi != 0 &&
          stats.dxg_display_bind_provider_no_sender != 0 &&
          stats.dxg_display_bind_provider_no_completion != 0)) &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        backend_opengl_submit == 0;
    d3d12_display_bind_host_abi_discovery_ok =
        wsl_uapi_negative_ok &&
        public_present_api_not_guest_bind_ok &&
        provider_credit_gate_negative_ok &&
        dda_nouveau_separate_display_not_bind_ok &&
        stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats.dxg_display_bind_provider_completion_demux_registered == 0 &&
        stats.dxg_display_bind_transport_source ==
            FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE &&
        stats.dxg_display_bind_host_saw_packet == 0 &&
        stats.dxg_display_bind_wsl_presenthistory_completion_credit == 0 &&
        stats.dxg_present_helper_transport_present == 0 &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    d3d12_negative_abi_manifest_ok =
        d3d12_display_bind_host_abi_discovery_ok &&
        stats.dxg_scanout_bind_candidate_presenthistory_cmd == 34 &&
        stats.dxg_scanout_bind_candidate_redirected_flip_fence_cmd == 35 &&
        stats.dxg_scanout_bind_candidate_blt_cmd == 38 &&
        stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd == 1;
    d3d12_display_bind_authority_chain_ok =
        d3d12_display_bind_host_abi_discovery_ok &&
        display_bind_provider_no_send_preflight_ok &&
        display_bind_provider_packet_lifetime_ok &&
        native_completion_lifetime_ok &&
        generic_completion_not_native_ok &&
        dda_nouveau_separate_display_not_bind_ok &&
        stats.dxg_display_bind_transport_source ==
            FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE &&
        stats.dxg_display_bind_host_saw_packet == 0 &&
        stats.dxg_display_bind_provider_completion_demux_registered == 0 &&
        stats.dxg_display_bind_wsl_presenthistory_completion_credit == 0 &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    dda_nouveau_d3d12_bridge_disjoint_ok =
        dda_nouveau_separate_display_not_bind_ok &&
        stats.dxg_scanout_bind_dda_resource_import_absent != 0 &&
        stats.dxg_scanout_bind_dda_scanout_bind_absent != 0 &&
        stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    dda_nouveau_non_readback_display_proof_ok =
        (stats.nouveau_pci_probe_accepts == 0 &&
         stats.nouveau_display_probe_attempts == 0 &&
         stats.nouveau_display_create_attempts == 0 &&
         stats.nouveau_display_create_successes == 0 &&
         stats.nouveau_display_head_probe_attempts == 0 &&
         stats.nouveau_display_heads == 0 &&
         stats.nouveau_display_connector_probe_attempts == 0 &&
         stats.nouveau_display_connectors == 0 &&
         stats.nouveau_display_nonvirtual_connectors == 0 &&
         stats.nouveau_display_engine_object_created == 0 &&
         stats.nouveau_display_mode_config_ready == 0 &&
         stats.nouveau_display_crtc_count == 0 &&
         stats.nouveau_display_encoder_count == 0 &&
         stats.nouveau_display_primary_plane_count == 0 &&
         stats.nouveau_display_primary_plane_linear_required != 0 &&
         stats.nouveau_display_primary_plane_nonlinear_modifiers == 0 &&
         stats.nouveau_display_outp_mask_seen == 0 &&
         stats.nouveau_display_conn_mask_seen == 0 &&
         stats.nouveau_display_head_mask_seen == 0 &&
         stats.nouveau_display_nvif_head_ctor_successes == 0 &&
         stats.nouveau_display_hpd_event_registered == 0 &&
         stats.nouveau_display_dp_irq_event_registered == 0 &&
         stats.nouveau_display_vblank_supported == 0 &&
         stats.nouveau_display_vblank_irq_supported == 0 &&
         stats.nouveau_display_vblank_event_registered == 0 &&
         stats.nouveau_display_atomic_commit_tail_ready == 0 &&
         stats.nouveau_display_page_flip_event_source ==
             FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE &&
         stats.nouveau_display_vblank_source ==
             FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE &&
         stats.nouveau_display_vblank_irqs == 0 &&
         stats.nouveau_display_page_flip_completion_ready == 0 &&
         stats.nouveau_display_page_flip_completions == 0 &&
         stats.nouveau_display_atomic_pageflip_backend_missing == 0 &&
         stats.kms_present_last_lane == FB_GPU_KMS_PRESENT_LANE_NONE &&
         stats.kms_present_dumb == 0 &&
         stats.kms_present_synthvid == 0 &&
         stats.kms_present_nouveau_hw == 0 &&
         stats.kms_vblank_source_nouveau_hw == 0 &&
         stats.kms_page_flip_events_native_hw == 0 &&
         stats.nouveau_pci_native_present_credit == 0 &&
         backend_opengl_submit == 0) ||
        (stats.nouveau_pci_probe_accepts != 0 &&
         !nouveau_native_display_claimed &&
         nouveau_atomic_pageflip_backend_missing_ok &&
         stats.nouveau_pci_native_present_credit == 0 &&
         backend_opengl_submit == 0) ||
        (stats.nouveau_pci_probe_accepts != 0 &&
         nouveau_display_kms_ready &&
         stats.kms_present_last_lane == FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW &&
         stats.kms_present_dumb == 0 &&
         stats.kms_present_synthvid == 0 &&
         stats.kms_present_nouveau_hw != 0 &&
         stats.kms_vblank_source_nouveau_hw != 0 &&
         stats.kms_vblank_source_software_display == 0 &&
         stats.kms_vblank_source_synthetic == 0 &&
         stats.kms_page_flip_events_native_hw != 0 &&
         stats.kms_page_flip_events_software_blit == 0);
    host_display_bind_source_catalog_ok =
        wsl_uapi_negative_ok &&
        wsl_adapter_display_caps_negative_ok &&
        wsl_submit_present_fields_not_bind_ok &&
        wsl_stdalloc_and_alloc_flags_not_bind_ok &&
        wsl_trace_display_bind_negative_ok &&
        public_present_api_not_guest_bind_ok &&
        provider_credit_gate_negative_ok &&
        d3d12_display_bind_host_abi_discovery_ok &&
        dda_nouveau_separate_display_not_bind_ok &&
        dda_nouveau_non_readback_display_proof_ok &&
        ((stats.dxg_display_bind_provider_submits == 0 &&
          stats.dxg_display_bind_transport_present == 0) ||
         (stats.dxg_display_bind_backend ==
              FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND &&
          stats.dxg_display_bind_transport ==
              FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE &&
          stats.dxg_display_bind_transport_present == 0 &&
          stats.dxg_display_bind_provider_no_host_abi != 0 &&
          stats.dxg_display_bind_provider_no_sender != 0 &&
          stats.dxg_display_bind_provider_no_completion != 0)) &&
        stats.dxg_present_helper_transport_present == 0 &&
        stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    d3d12_completion_source_authority_ok =
        host_display_bind_source_catalog_ok &&
        (stats.dxg_display_bind_provider_submits == 0 ||
         stats.dxg_display_bind_provider_no_completion != 0) &&
        stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.dxg_scanout_bind_last_present_id == 0 &&
        stats.dxg_scanout_bind_last_completed == 0 &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_present_helper_transport_present == 0 &&
        stats.kms_vblank_source_nouveau_hw == 0 &&
        stats.kms_page_flip_events_native_hw == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;
    native_present_completion_source_namespace_ok =
        d3d12_completion_source_authority_ok &&
        stats.nouveau_pci_irq_cause_valid == 0 &&
        stats.nouveau_pci_irq_cause_acks == 0 &&
        stats.nouveau_pci_irq_spurious == 0;
    gpu_remaining_holistic_skeleton_ok =
        host_display_bind_source_catalog_ok &&
        d3d12_completion_source_authority_ok &&
        native_present_completion_source_namespace_ok &&
        stats.dxg_display_bind_transport_present == 0 &&
        stats.dxg_display_bind_present_id == 0 &&
        stats.dxg_display_bind_completed_id == 0 &&
        stats.dxg_scanout_bind_successes == 0 &&
        stats.dxg_scanout_bind_completion_successes == 0 &&
        stats.nouveau_pci_native_present_credit == 0 &&
        backend_opengl_submit == 0;

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
           "dma=%lu/%lu/%lu/%lu fallback32=%lu "
           "coherent=%lu/%lu/%lu/%lu fallback32=%lu irq_mode=%lu "
           "dma_map=%lu/%lu/%lu unmaps=%lu dma_map_last=%lu/%lu/%lu "
           "irq_handler=%lu irq_delivery=%lu irq_claimed=%lu "
           "irq_alloc=%lu/%lu msi=%lu/%lu msix=%lu/%lu "
           "legacy_irq=%lu/%lu irq_cause=%lu/%lu/%lu/%lu "
           "pm=%lu/%lu balanced=%lu remove_suspended=%lu "
           "remove_calls=%lu remove_resume=%lu/%lu barriers=%lu "
           "remove_active=%lu hot_remove=%lu removed=%lu "
           "teardown=%lu/%lu/%lu/%lu/%lu/%lu "
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
           stats.nouveau_pci_dma_mask_requested_bits,
           stats.nouveau_pci_dma_mask_bits,
           stats.nouveau_pci_dma_mask_effective_bits,
           stats.nouveau_pci_dma_mask_fallback_32,
           stats.nouveau_pci_coherent_dma_mask_configured,
           stats.nouveau_pci_coherent_dma_mask_requested_bits,
           stats.nouveau_pci_coherent_dma_mask_bits,
           stats.nouveau_pci_coherent_dma_mask_effective_bits,
           stats.nouveau_pci_coherent_dma_mask_fallback_32,
           stats.nouveau_pci_irq_mode,
           stats.nouveau_pci_dma_map_attempts,
           stats.nouveau_pci_dma_map_successes,
           stats.nouveau_pci_dma_map_failures,
           stats.nouveau_pci_dma_unmaps,
           stats.nouveau_pci_dma_map_last_size,
           stats.nouveau_pci_dma_map_last_addr,
           stats.nouveau_pci_dma_map_last_ret,
           stats.nouveau_pci_irq_handler_registered,
           stats.nouveau_pci_irq_delivery_enabled,
           stats.nouveau_pci_irq_delivery_claimed,
           stats.nouveau_pci_irq_alloc_requests,
           stats.nouveau_pci_irq_alloc_failures,
           stats.nouveau_pci_msi_program_attempts,
           stats.nouveau_pci_msi_program_unsupported,
           stats.nouveau_pci_msix_program_attempts,
           stats.nouveau_pci_msix_program_unsupported,
           stats.nouveau_pci_legacy_irq_requests,
           stats.nouveau_pci_legacy_irq_grants,
           stats.nouveau_pci_irq_cause_reads,
           stats.nouveau_pci_irq_cause_valid,
           stats.nouveau_pci_irq_cause_acks,
           stats.nouveau_pci_irq_spurious,
           stats.nouveau_pci_suspend_count,
           stats.nouveau_pci_resume_count,
           stats.nouveau_pci_runtime_pm_balanced,
           stats.nouveau_pci_remove_runtime_suspended,
           stats.nouveau_pci_remove_calls,
           stats.nouveau_pci_remove_runtime_resume_attempts,
           stats.nouveau_pci_remove_runtime_resume_successes,
           stats.nouveau_pci_remove_runtime_barriers,
           stats.nouveau_pci_remove_active_before_callback,
           stats.nouveau_pci_hot_remove_events,
           stats.nouveau_pci_removed,
           stats.nouveau_pci_bar_iounmaps,
           stats.nouveau_pci_irq_unregisters,
           stats.nouveau_pci_irq_vectors_freed,
           stats.nouveau_pci_bus_master_clears,
           stats.nouveau_pci_device_disables,
           stats.nouveau_pci_drvdata_cleared,
           stats.nouveau_pci_native_present_credit,
           stats.dxg_present_helper_transport_present,
           stats.dxg_present_dxg_adapter_render_supported,
           stats.dxg_present_dxg_adapter_display_supported,
           stats.dxg_present_dxg_adapter_sources,
           stats.dxg_present_dxg_adapter_sources_known,
           stats.dxg_present_dda_nouveau_present,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present);
    printf("gpu_core_c_validator hyperv_opengl_submit_gate_matrix "
           "backend=%u backend_opengl_submit=%u "
           "requires_native_present=1 requires_finite_fps=1 "
           "requires_webkit_shared_surface=1 native_present_credit=0 "
           "display_target_kind=%lu present_id=0 completed=0 "
           "backend_gate=%s status=%s\n",
           backend.backend,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           stats.dxg_present_display_target_kind,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0 ?
               "open" : "closed",
           backend.backend == FB_GPU_BACKEND_HYPERV_DXG &&
                   (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0 &&
                   stats.dxg_present_display_target_kind ==
                       FB_GPU_DXG_DISPLAY_TARGET_NONE ?
               "PASS" : "DIAGNOSTIC");
    printf("gpu_core_c_validator opengl_submit_backend_separation_matrix "
           "backend=%u dxg_transport=%u d3dkmt=%u virgl_opengl=%u "
           "backend_opengl_submit=%u allowed_submit_backend=virgl "
           "hyperv_dxg_transport_is_submit=0 hyperv_d3dkmt_is_submit=0 "
           "kvm_virgl_submit_allowed=1 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           backend.backend,
           (backend.flags & FB_GPU_BACKEND_F_DXG_TRANSPORT) != 0,
           (backend.flags & FB_GPU_BACKEND_F_D3DKMT) != 0,
           (backend.flags & FB_GPU_BACKEND_F_VIRGL_OPENGL) != 0,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           backend.backend == FB_GPU_BACKEND_HYPERV_DXG &&
                   (backend.flags & FB_GPU_BACKEND_F_DXG_TRANSPORT) != 0 &&
                   (backend.flags & FB_GPU_BACKEND_F_D3DKMT) != 0 &&
                   (backend.flags & FB_GPU_BACKEND_F_VIRGL_OPENGL) == 0 &&
                   (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0 ?
               "PASS" : "DIAGNOSTIC");
    printf("gpu_core_c_validator wsl_dxg_uapi_namespace_negative_matrix "
           "uapi_namespace_checked=1 ioctl_namespace=linux_dxgkrnl "
           "last_known_ioctl_nr=0x49 checked_range=0x00-0x49 "
           "display_bind_ioctl_present=0 present_source_ioctl_present=0 "
           "present_completion_ioctl_present=0 "
           "out_of_namespace_native_present_ioctl=0 "
           "linux_ioctl_contracts=%lu "
           "resource_bind_contracts=%lu display_completion_contracts=%lu "
           "transport_present=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_linux_ioctl_contracts,
           stats.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats.dxg_scanout_bind_candidate_display_completion_contracts,
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           wsl_uapi_negative_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "wsl_dxg_adapter_display_caps_negative_matrix "
           "display_supported=%lu post_device=0 "
           "indirect_display_device=0 display_sources=%lu "
           "display_sources_known=%lu display_caps_cleared_by_wsl=1 "
           "display_bind_transport_present=%lu present_id=%lu "
           "completed=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_present_dxg_adapter_display_supported,
           stats.dxg_present_dxg_adapter_sources,
           stats.dxg_present_dxg_adapter_sources_known,
           stats.dxg_present_helper_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           wsl_adapter_display_caps_negative_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator wsl_submit_present_fields_not_bind_matrix "
           "submit_present_redirected_field_known=1 "
           "submit_present_history_token_field_known=1 "
           "written_primaries_field_known=1 resource_scanout_bind_sender=%lu "
           "display_completion_return=%lu sender_contracts=%lu "
           "completion_contracts=%lu transport_present=%lu present_id=%lu "
           "completed=%lu native_present_credit=0 opengl_submit_credit=0 "
           "status=%s\n",
           stats.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats.dxg_scanout_bind_candidate_display_completion_contracts,
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           wsl_submit_present_fields_not_bind_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "wsl_stdalloc_and_alloc_flags_not_bind_matrix "
           "stdalloc_private_data_sender_present=%lu "
           "stdalloc_display_bind_sender=%lu primary_alloc_flag_known=1 "
           "direct_flip_alloc_flag_known=1 "
           "alloc_flags_are_completion_contract=0 "
           "resource_bind_contracts=%lu display_completion_contracts=%lu "
           "transport_present=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_scanout_bind_standard_alloc_private_data,
           stats.dxg_scanout_bind_standard_alloc_display_bind_absent == 0 ?
               1UL : 0UL,
           stats.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats.dxg_scanout_bind_candidate_display_completion_contracts,
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           wsl_stdalloc_and_alloc_flags_not_bind_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator wsl_trace_display_bind_negative_matrix "
           "trace_paths_checked=2 trace_ioctl_namespace_subset_of_wsl_uapi=1 "
           "display_bind_opcode_seen=%lu "
           "nonzero_present_history_token_seen=0 "
           "open_resource_without_display_bind_seen=1 "
           "syncfile_without_display_completion_seen=1 "
           "resource_share_without_scanout_bind_seen=1 "
           "linux_ioctl_contracts=%lu resource_bind_contracts=%lu "
           "display_completion_contracts=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats.dxg_scanout_bind_candidate_linux_ioctl_contracts,
           stats.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats.dxg_scanout_bind_candidate_display_completion_contracts,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           wsl_trace_display_bind_negative_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "public_present_api_not_guest_bind_matrix "
           "reactos_d3dkmt_present_api=known "
           "reactos_present_redirected_api=known "
           "directx_shared_handle_api=known "
           "directx_sharing_contract_hwnd_only=1 "
           "wslg_local_source=absent freerdp_local_source=absent "
           "rdp_frame_transport=copy_or_dirty_frame "
           "guest_vmbus_display_bind_contract=0 "
           "guest_resource_bind_contracts=%lu "
           "guest_completion_contracts=%lu "
           "gpup_sender_contract=%lu gpup_completion_contract=%lu "
           "transport_present=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=%u "
           "webkit_accel_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats.dxg_scanout_bind_candidate_display_completion_contracts,
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           backend_opengl_submit,
           public_present_api_not_guest_bind_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "d3d12_display_bind_host_abi_discovery_matrix "
           "custom_host_tool=0 wsl_dxg_display_bind_ioctl=0 "
           "wsl_display_bind_ioctl_absent=1 "
           "wslg_frame_path=absent freerdp_frame_path=absent "
           "rdp_frame_path=copy_or_dirty_frame "
           "hvsock_display_bind_service=absent "
           "gpup_dxg_sender_contract=%lu "
           "gpup_dxg_completion_contract=%lu "
           "completion_demux_contract=%lu "
           "wsl_presenthistory_completion_credit=%lu "
           "host_saw_display_bind_packet=%lu "
           "display_bind_transport_source=%s "
           "dda_nouveau_d3d12_import=%lu "
           "dda_nouveau_scanout_bind=%lu "
           "dda_nouveau_hw_flip_completion=%s "
           "provider_state=failclosed provider_failclosed=1 "
           "host_abi_present=0 sender_present=0 completion_present=0 "
           "transport_present=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 "
           "webkit_accel_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_display_bind_provider_completion_demux_registered,
           stats.dxg_display_bind_wsl_presenthistory_completion_credit,
           stats.dxg_display_bind_host_saw_packet,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present,
           stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           d3d12_display_bind_host_abi_discovery_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator d3d12_negative_abi_manifest_matrix "
           "source_audited=1 wsl_uapi_header=d3dkmthk.h "
           "wsl_uapi_namespace_checked=1 wsl_last_ioctl_nr=0x49 "
           "wsl_display_bind_ioctl=0 wsl_vmbus_file=dxgvmbus.c "
           "presenthistory_cmd=%lu redirected_flip_fence_cmd=%lu "
           "blt_cmd=%lu submit_hwqueue_cmd=52 "
           "propagate_presenthistory_cmd=%lu "
           "presenthistory_is_telemetry=1 vm_pkt_comp_reply_only=1 "
           "hvsock_display_bind_service=absent "
           "resource_scanout_bind_sender=0 display_completion_demux=%lu "
           "synthvid_path=gpa_dirty_rect_only "
           "dda_nouveau_path=separate_pci_display "
           "dda_d3d12_import=%lu dda_scanout_bind=%lu "
           "dda_hw_flip_completion=%s host_saw_display_bind_packet=%lu "
           "display_bind_transport_source=%s "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_presenthistory_cmd,
           stats.dxg_scanout_bind_candidate_redirected_flip_fence_cmd,
           stats.dxg_scanout_bind_candidate_blt_cmd,
           stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd,
           stats.dxg_display_bind_provider_completion_demux_registered,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present,
           stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats.dxg_display_bind_host_saw_packet,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           d3d12_negative_abi_manifest_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator d3d12_display_bind_authority_chain_matrix "
           "chain_version=1 selected_lane=gpup_dxg_scanout_bind "
           "authority_order=host_abi,provider_send,host_packet,"
           "provider_demux,display_completion,resource_generation,"
           "consumer_credit "
           "host_abi_gate=closed provider_send_gate=closed "
           "host_packet_gate=closed completion_demux_gate=closed "
           "display_completion_gate=closed source_generation_gate=armed "
           "resource_generation_gate=armed consumer_credit_gate=closed "
           "transport_pending_id=%lu command_id=%lu transaction_id=%lu "
           "channel=%lu display_bind_transport_source=%s "
           "host_saw_display_bind_packet=%lu "
           "completion_demux_registered=%lu "
           "wsl_presenthistory_completion_credit=%lu "
           "dda_native_display_is_d3d12_bridge=0 "
           "kms_completion_is_d3d12=0 syncfile_is_display_completion=0 "
           "present_id=%lu completed=%lu native_present_credit=0 "
           "backend_opengl_submit=%u opengl_submit_credit=0 "
           "webkit_accel_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_transport_pending_id,
           stats.dxg_display_bind_provider_command_id,
           stats.dxg_display_bind_provider_transaction_id,
           stats.dxg_display_bind_provider_channel,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           stats.dxg_display_bind_host_saw_packet,
           stats.dxg_display_bind_provider_completion_demux_registered,
           stats.dxg_display_bind_wsl_presenthistory_completion_credit,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           backend_opengl_submit,
           d3d12_display_bind_authority_chain_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator provider_credit_gate_negative_matrix "
           "provider_submits=%lu "
           "host_abi_present=%u sender_present=%u completion_present=%u "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu transport_present=%lu "
           "present_id=%lu completed=%lu native_present_credit=0 "
           "backend_opengl_submit=%u status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_provider_submits != 0 &&
               stats.dxg_display_bind_provider_no_host_abi == 0,
           stats.dxg_display_bind_provider_submits != 0 &&
               stats.dxg_display_bind_provider_no_sender == 0,
           stats.dxg_display_bind_provider_submits != 0 &&
               stats.dxg_display_bind_provider_no_completion == 0,
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           backend_opengl_submit,
           provider_credit_gate_negative_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "dda_nouveau_non_readback_display_proof_matrix "
           "dda_pci_transport_present=%s "
           "dda_nouveau_display_present=%s "
           "dda_nouveau_non_readback_present=%s "
           "display_probe_attempts=%lu display_create_successes=%lu "
           "display_engine_object=%lu mode_config_ready=%lu "
           "head_probe_attempts=%lu heads=%lu "
           "connector_probe_attempts=%lu connectors=%lu "
           "crtcs=%lu encoders=%lu primary_planes=%lu "
           "nonvirtual_connectors=%lu outp_mask_seen=%lu "
           "conn_mask_seen=%lu head_mask_seen=%lu "
           "vblank_supported=%lu vblank_event=%lu "
           "vblank_irq_supported=%lu vblank_source=%s "
           "page_flip_ready=%lu page_flip_completions=%lu "
           "page_flip_event_source=%s atomic_commit_tail=%lu "
           "atomic_backend_missing=%lu "
           "kms_lane=%lu kms_present_dumb=%lu kms_present_synthvid=%lu "
           "kms_present_nouveau_hw=%lu "
           "kms_vblank_source_nouveau_hw=%lu "
           "kms_vblank_source_software_display=%lu "
           "kms_vblank_source_synthetic=%lu "
           "page_flip_events_software_blit=%lu "
           "page_flip_events_native_hw=%lu "
           "dda_native_display_credit=%lu d3d12_native_present_credit=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.nouveau_pci_probe_accepts != 0 ? "PASS" :
                                                   "GPU_P_FAIL_CLOSED",
           nouveau_display_kms_registered ? "PASS" : "ABSENT",
           stats.kms_present_last_lane == FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW &&
                   stats.kms_page_flip_events_native_hw != 0 &&
                   nouveau_display_kms_ready ?
               "PASS" : "ABSENT",
           stats.nouveau_display_probe_attempts,
           stats.nouveau_display_create_successes,
           stats.nouveau_display_engine_object_created,
           stats.nouveau_display_mode_config_ready,
           stats.nouveau_display_head_probe_attempts,
           stats.nouveau_display_heads,
           stats.nouveau_display_connector_probe_attempts,
           stats.nouveau_display_connectors,
           stats.nouveau_display_crtc_count,
           stats.nouveau_display_encoder_count,
           stats.nouveau_display_primary_plane_count,
           stats.nouveau_display_nonvirtual_connectors,
           stats.nouveau_display_outp_mask_seen,
           stats.nouveau_display_conn_mask_seen,
           stats.nouveau_display_head_mask_seen,
           stats.nouveau_display_vblank_supported,
           stats.nouveau_display_vblank_event_registered,
           stats.nouveau_display_vblank_irq_supported,
           stats.nouveau_display_vblank_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_page_flip_completion_ready,
           stats.nouveau_display_page_flip_completions,
           stats.nouveau_display_page_flip_event_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_atomic_commit_tail_ready,
           stats.nouveau_display_atomic_pageflip_backend_missing,
           stats.kms_present_last_lane,
           stats.kms_present_dumb,
           stats.kms_present_synthvid,
           stats.kms_present_nouveau_hw,
           stats.kms_vblank_source_nouveau_hw,
           stats.kms_vblank_source_software_display,
           stats.kms_vblank_source_synthetic,
           stats.kms_page_flip_events_software_blit,
           stats.kms_page_flip_events_native_hw,
           stats.nouveau_pci_native_present_credit,
           dda_nouveau_non_readback_display_proof_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator dda_nouveau_d3d12_bridge_disjoint_matrix "
           "dda_backend_flag=%u dda_pci_display_present=%lu "
           "dda_d3d12_import_path=%lu dda_d3d12_scanout_bind=%lu "
           "dda_hw_flip_completion_for_d3d12=0 "
           "dda_hw_flip_completion=%s kms_lane=%lu kms_lane_is_d3d12=0 "
           "display_bind_present_id=%lu display_bind_completed=%lu "
           "scanout_bind_successes=%lu completion_successes=%lu "
           "dda_native_display_credit=%lu native_present_credit=0 "
           "opengl_submit_credit=%u status=%s\n",
           (backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) != 0,
           stats.dxg_scanout_bind_dda_pci_display_present,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present,
           stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats.kms_present_last_lane,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_completion_successes,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           dda_nouveau_d3d12_bridge_disjoint_ok ?
               "PASS_FAILCLOSED" : "FAIL");
    printf("gpu_core_c_validator host_display_bind_source_catalog_matrix "
           "selected_source=missing selected_lane=gpup_dxg_scanout_bind "
           "provider_state=%s custom_host_tool=0 "
           "wsl_dxg_display_bind_ioctl=0 wslg_channel=absent "
           "gpup_dxg_sender=%lu gpup_dxg_completion=%lu "
           "host_saw_display_bind_packet=%lu "
           "display_bind_transport_source=%s "
           "wsl_presenthistory_completion_credit=%lu "
           "synthvid_d3d12_bind=0 dda_d3d12_resource_import=%lu "
           "dda_scanout_bind=%lu dda_hw_flip_completion=%s "
           "transport_present=%lu present_id=%lu completed=%lu "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu native_present_credit=0 "
           "opengl_submit_credit=0 webkit_accel_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_submits == 0 ? "not_sampled" :
                                                          "failclosed",
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_display_bind_host_saw_packet,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           stats.dxg_display_bind_wsl_presenthistory_completion_credit,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present,
           stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           host_display_bind_source_catalog_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator d3d12_completion_source_authority_matrix "
           "accepted_completion_source=display_bind_provider "
           "submit_ntstatus_as_completion=0 "
           "presenthistory_telemetry_as_completion=0 "
           "syncfile_fence_as_display_completion=0 "
           "callback_release_as_display_completion=0 "
           "kms_vblank_as_d3d12_completion=0 "
           "provider_submits=%lu provider_no_completion=%lu "
           "provider_completion_present=%u completion_contracts=%lu "
           "scanout_completion_successes=%lu present_id=%lu completed=%lu "
           "native_present_credit=%lu opengl_submit_credit=%u "
           "status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_provider_no_completion,
           stats.dxg_display_bind_provider_submits != 0 &&
               stats.dxg_display_bind_provider_no_completion == 0,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_scanout_bind_completion_successes,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           d3d12_completion_source_authority_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "native_present_completion_source_namespace_matrix "
           "completion_source=missing d3d12_display_bind_provider=%s "
           "d3d12_present_id=%lu d3d12_completed=%lu "
           "kms_vblank_sequence=%lu kms_page_flip_sequence=%lu "
           "nouveau_irq_cause_valid=%lu nouveau_irq_cause_acks=%lu "
           "namespace_mixed=0 native_present_credit=%lu "
           "opengl_submit_credit=%u status=%s\n",
           stats.dxg_display_bind_provider_submits == 0 ? "not_sampled" :
                                                          "failclosed",
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.kms_vblank_source_nouveau_hw,
           stats.kms_page_flip_events_native_hw,
           stats.nouveau_pci_irq_cause_valid,
           stats.nouveau_pci_irq_cause_acks,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           native_present_completion_source_namespace_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator d3d12_native_completion_zero_credit_matrix "
           "backend=%u display_bind=%s transport_present=%lu "
           "completion_source=required present_id=0 completed=0 "
           "callback_release_order=blocked per_client_generation=required "
           "id_shape=%s provider_credit_gate=%s "
           "native_present_credit=0 opengl_submit_credit=0 status=PENDING\n",
           backend.backend,
           stats.dxg_present_helper_transport_present ? "PRESENT" :
               "ABSENT",
           stats.dxg_present_helper_transport_present,
           display_bind_id_shape_ok ? "PASS" : "FAIL",
           provider_credit_gate_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator d3d12_display_bind_id_shape_matrix "
           "bind_present_id=%lu bind_completed_id=%lu "
           "bind_source_generation=%lu bind_resource_generation=%lu "
           "scanout_present_id=%lu scanout_completed_id=%lu "
           "scanout_source_generation=%lu scanout_resource_generation=%lu "
           "zero_ids_required_when_failclosed=1 "
           "completed_ge_present_if_nonzero=1 stale_id_rejected=1 "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_scanout_bind_last_present_id,
           stats.dxg_scanout_bind_last_completed,
           stats.dxg_scanout_bind_last_source_generation,
           stats.dxg_scanout_bind_last_resource_generation,
           display_bind_id_shape_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator d3d12_provider_credit_gate_matrix "
           "provider_submits=%lu provider_no_host_abi=%lu "
           "provider_no_sender=%lu provider_no_completion=%lu "
           "transport_present=%lu display_target_kind=%lu "
           "scanout_successes=%lu completion_successes=%lu "
           "native_present_credit=%lu backend_opengl_submit=%u "
           "credit_requires_provider_clear=1 status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           stats.dxg_present_helper_transport_present,
           stats.dxg_present_display_target_kind,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_completion_successes,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           provider_credit_gate_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator d3d12_display_bind_request_metadata_matrix "
           "provider_submits=%lu request_metadata_complete=%lu "
           "request_sync_metadata_complete=%lu missing_metadata=0x%lx "
           "required_metadata=0x%lx source_generation=%lu "
           "resource_generation=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_request_metadata_complete,
           stats.dxg_display_bind_request_sync_metadata_complete,
           stats.dxg_display_bind_request_missing_metadata,
           stats.dxg_display_bind_required_metadata,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           display_bind_request_metadata_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator d3d12_display_bind_pending_lifetime_matrix "
           "pending_sequence=%lu created=%lu active=%lu peak=%lu "
           "completed=%lu failclosed=%lu cancelled=%lu "
           "last_status=%lu last_block_reason=0x%lx "
           "source_generation=%lu resource_generation=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_pending_sequence,
           stats.dxg_display_bind_pending_created,
           stats.dxg_display_bind_pending_active,
           stats.dxg_display_bind_pending_peak,
           stats.dxg_display_bind_pending_completed,
           stats.dxg_display_bind_pending_failclosed,
           stats.dxg_display_bind_pending_cancelled,
           stats.dxg_display_bind_pending_last_status,
           stats.dxg_display_bind_pending_last_block_reason,
           stats.dxg_display_bind_pending_last_source_generation,
           stats.dxg_display_bind_pending_last_resource_generation,
           display_bind_pending_lifetime_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "d3d12_display_bind_generation_revalidation_matrix "
           "provider_submits=%lu lock_dropped_submits=%lu "
           "revalidate_attempts=%lu revalidate_successes=%lu "
           "revalidate_failures=%lu provider_pin_revalidated=%lu "
           "display_bind_source_generation=%lu "
           "display_bind_resource_generation=%lu "
           "pinned_resource_generation=%lu source_generation_present=%s "
           "resource_generation_present=%s pinned_generation_match=%s "
           "present_id=%lu completed=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_lock_dropped_submits,
           stats.dxg_display_bind_revalidate_attempts,
           stats.dxg_display_bind_revalidate_successes,
           stats.dxg_display_bind_revalidate_failures,
           stats.dxg_display_bind_provider_pin_revalidated,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_display_bind_pinned_resource_generation,
           stats.dxg_display_bind_source_generation != 0 ? "PASS" :
                                                           "NOT_SAMPLED",
           stats.dxg_display_bind_resource_generation != 0 ? "PASS" :
                                                             "NOT_SAMPLED",
           stats.dxg_display_bind_pinned_resource_generation ==
                       stats.dxg_display_bind_resource_generation &&
                   stats.dxg_display_bind_resource_generation != 0 ?
               "PASS" : "NOT_SAMPLED",
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           display_bind_generation_revalidation_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "d3d12_display_bind_provider_pending_publication_matrix "
           "provider_submits=%lu publication_attempts=%lu "
           "host_abi_present=0 sender_present=0 "
           "completion_present=0 owner_generation=%lu "
           "provider_source_generation=%lu provider_resource_generation=%lu "
           "pending_owner_generation=%lu pending_source_generation=%lu "
           "pending_resource_generation=%lu "
           "dxgprocess_generation=%lu process_adapter_generation=%lu "
           "process_namespace_valid=%lu hmgr_index_unique_valid=%lu "
           "device_hmgr_index_unique_valid=%lu "
           "resource_hmgr_index_unique_valid=%lu "
           "allocation_hmgr_index_unique_valid=%lu "
           "device_object_ref_active=%lu "
           "resource_object_ref_active=%lu allocation_object_ref_active=%lu "
           "shared_parent_id=%lu shared_parent_refs=%lu "
           "shared_parent_children=%lu shared_parent_fd_refs=%lu "
           "shared_parent_host_nt_refs=%lu shared_parent_child_refs=%lu "
           "shared_parent_global_share=%lu shared_parent_host_nt=%lu "
           "opened_child_parent_id_match=%lu "
           "opened_child_global_share_match=%lu "
           "opened_child_sealed_generation_match=%lu "
           "shared_parent_snapshot_valid=%lu "
           "opened_child_snapshot_valid=%lu "
           "shared_parent_global_share_match=%lu "
           "syncobject_object_ref_active=%lu "
           "syncobject_shared_owner_present=%lu "
           "syncobject_monitored_fence=%lu syncobject_fence_value=%lu "
           "syncobject_fence_cpu_va_present=%lu "
           "syncobject_fence_gpu_va_present=%lu "
           "syncobject_fence_kva_present=%lu "
           "syncobject_fence_gpu_va_alias_gap=%lu "
           "syncobject_real_fence_gpu_va_present=%lu "
           "syncobject_fence_gpu_va_source=%lu "
           "syncobject_fence_map_size=%lu "
           "owner_close_cancelled=%lu "
           "owner_generation_required=1 source_generation_required=1 "
           "resource_generation_required=1 pending_generation_match=%s "
           "publish_before_send=%lu "
           "transport_pending_id=%lu command_id=%lu transaction_id=%lu "
           "channel=%s completion_demux_registered=%lu "
           "resolved_or_cancelled=%lu refs_released=%lu "
           "host_saw_display_bind_packet=%lu "
           "display_bind_transport_source=%s "
           "wsl_presenthistory_completion_credit=%lu "
           "no_host_abi_cancelled=%lu no_host_abi_refs_released=%lu "
           "pending_cancelled=%lu publish_before_send_order=blocked "
           "cancellation_ref_release_credit=0 "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_provider_publication_attempts,
           stats.dxg_display_bind_provider_pending_owner_generation,
           stats.dxg_display_bind_provider_pending_source_generation,
           stats.dxg_display_bind_provider_pending_resource_generation,
           stats.dxg_display_bind_pending_last_owner_generation,
           stats.dxg_display_bind_pending_last_source_generation,
           stats.dxg_display_bind_pending_last_resource_generation,
           stats.dxg_display_bind_provider_pending_dxgprocess_generation,
           stats.dxg_display_bind_provider_pending_process_adapter_generation,
           stats.dxg_display_bind_provider_pending_process_namespace_valid,
           stats.dxg_display_bind_provider_pending_hmgr_index_unique_valid,
           stats.dxg_display_bind_provider_pending_device_hmgr_index_unique_valid,
           stats.dxg_display_bind_provider_pending_resource_hmgr_index_unique_valid,
           stats.dxg_display_bind_provider_pending_allocation_hmgr_index_unique_valid,
           stats.dxg_display_bind_provider_pending_device_object_ref_active,
           stats.dxg_display_bind_provider_pending_resource_object_ref_active,
           stats.dxg_display_bind_provider_pending_allocation_object_ref_active,
           stats.dxg_display_bind_provider_pending_shared_parent_id,
           stats.dxg_display_bind_provider_pending_shared_parent_refs,
           stats.dxg_display_bind_provider_pending_shared_parent_children,
           stats.dxg_display_bind_provider_pending_shared_parent_fd_refs,
           stats.dxg_display_bind_provider_pending_shared_parent_host_nt_refs,
           stats.dxg_display_bind_provider_pending_shared_parent_child_refs,
           stats.dxg_display_bind_provider_pending_shared_parent_global_share,
           stats.dxg_display_bind_provider_pending_shared_parent_host_nt_handle,
           stats.dxg_display_bind_provider_pending_opened_child_parent_id_match,
           stats.dxg_display_bind_provider_pending_opened_child_global_share_match,
           stats.dxg_display_bind_provider_pending_opened_child_sealed_generation_match,
           stats.dxg_display_bind_provider_pending_shared_parent_snapshot_valid,
           stats.dxg_display_bind_provider_pending_opened_child_snapshot_valid,
           stats.dxg_display_bind_provider_pending_shared_parent_global_share_match,
           stats.dxg_display_bind_provider_pending_syncobject_object_ref_active,
           stats.dxg_display_bind_provider_pending_syncobject_shared_owner_present,
           stats.dxg_display_bind_provider_pending_syncobject_monitored_fence,
           stats.dxg_display_bind_provider_pending_syncobject_fence_value,
           stats.dxg_display_bind_provider_pending_syncobject_fence_cpu_va_present,
           stats.dxg_display_bind_provider_pending_syncobject_fence_gpu_va_present,
           stats.dxg_display_bind_provider_pending_syncobject_fence_kva_present,
           stats.dxg_display_bind_provider_pending_syncobject_fence_gpu_va_alias_gap,
           stats.dxg_display_bind_provider_pending_syncobject_real_fence_gpu_va_present,
           stats.dxg_display_bind_provider_pending_syncobject_fence_gpu_va_source,
           stats.dxg_display_bind_provider_pending_syncobject_fence_map_size,
           stats.dxg_display_bind_provider_pending_owner_close_cancelled,
           stats.dxg_display_bind_provider_submits == 0 ?
               "NOT_SAMPLED" :
           (stats.dxg_display_bind_provider_pending_owner_generation != 0 &&
                   stats.dxg_display_bind_provider_pending_source_generation != 0 &&
                   stats.dxg_display_bind_provider_pending_resource_generation != 0 &&
                   stats.dxg_display_bind_provider_pending_dxgprocess_generation ==
                       stats.dxg_display_bind_provider_pending_owner_generation &&
                   stats.dxg_display_bind_provider_pending_process_adapter_generation != 0 &&
                   stats.dxg_display_bind_provider_pending_hmgr_index_unique_valid != 0 &&
                   stats.dxg_display_bind_provider_pending_device_object_ref_active != 0 &&
                   stats.dxg_display_bind_provider_pending_resource_object_ref_active != 0 &&
                   stats.dxg_display_bind_provider_pending_allocation_object_ref_active != 0 &&
                   stats.dxg_display_bind_provider_pending_shared_parent_snapshot_valid != 0 &&
                   stats.dxg_display_bind_provider_pending_opened_child_snapshot_valid != 0 &&
                   stats.dxg_display_bind_provider_pending_owner_close_cancelled == 0 &&
                   stats.dxg_display_bind_pending_last_owner_generation ==
                       stats.dxg_display_bind_provider_pending_owner_generation &&
                   stats.dxg_display_bind_pending_last_source_generation ==
                       stats.dxg_display_bind_provider_pending_source_generation &&
                   stats.dxg_display_bind_pending_last_resource_generation ==
                       stats.dxg_display_bind_provider_pending_resource_generation) ?
               "PASS" : "FAIL",
           stats.dxg_display_bind_provider_publish_before_send,
           stats.dxg_display_bind_provider_transport_pending_id,
           stats.dxg_display_bind_provider_command_id,
           stats.dxg_display_bind_provider_transaction_id,
           stats.dxg_display_bind_provider_channel == 0 ? "none" : "other",
           stats.dxg_display_bind_provider_completion_demux_registered,
           stats.dxg_display_bind_provider_resolved_or_cancelled,
           stats.dxg_display_bind_provider_refs_released,
           stats.dxg_display_bind_host_saw_packet,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           stats.dxg_display_bind_wsl_presenthistory_completion_credit,
           stats.dxg_display_bind_provider_no_host_abi_cancelled,
           stats.dxg_display_bind_provider_no_host_abi_refs_released,
           stats.dxg_display_bind_pending_cancelled,
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           display_bind_provider_pending_publication_ok ?
               "PASS_FAILCLOSED" : "FAIL");
    printf("gpu_core_c_validator "
           "d3d12_display_bind_provider_shared_parent_retention_matrix "
           "shared_parent_id=%lu shared_parent_refs=%lu "
           "shared_parent_fd_refs=%lu shared_parent_host_nt_refs=%lu "
           "shared_parent_child_refs=%lu shared_parent_children=%lu "
           "shared_parent_global_share=%lu shared_parent_host_nt=%lu "
           "opened_child_parent_id_match=%lu "
           "opened_child_global_share_match=%lu "
           "opened_child_sealed_generation_match=%lu "
           "host_saw_display_bind_packet=%lu "
           "display_bind_transport_source=%s present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 "
           "webkit_accel_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_pending_shared_parent_id,
           stats.dxg_display_bind_provider_pending_shared_parent_refs,
           stats.dxg_display_bind_provider_pending_shared_parent_fd_refs,
           stats.dxg_display_bind_provider_pending_shared_parent_host_nt_refs,
           stats.dxg_display_bind_provider_pending_shared_parent_child_refs,
           stats.dxg_display_bind_provider_pending_shared_parent_children,
           stats.dxg_display_bind_provider_pending_shared_parent_global_share,
           stats.dxg_display_bind_provider_pending_shared_parent_host_nt_handle,
           stats.dxg_display_bind_provider_pending_opened_child_parent_id_match,
           stats.dxg_display_bind_provider_pending_opened_child_global_share_match,
           stats.dxg_display_bind_provider_pending_opened_child_sealed_generation_match,
           stats.dxg_display_bind_host_saw_packet,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           display_bind_provider_shared_parent_retention_ok ?
               "PASS_FAILCLOSED" : "FAIL");
    printf("gpu_core_c_validator "
           "d3d12_display_bind_provider_sync_fence_alias_matrix "
           "syncobject_object_ref_active=%lu "
           "syncobject_shared_owner_present=%lu "
           "syncobject_monitored_fence=%lu syncobject_fence_value=%lu "
           "syncobject_fence_cpu_va_present=%lu "
           "syncobject_fence_kva_present=%lu "
           "syncobject_fence_gpu_va_present=%lu "
           "syncobject_fence_gpu_va_alias_gap=%lu "
           "syncobject_fence_map_size=%lu real_fence_gpu_va_present=%lu "
           "gpu_va_source=%lu kva_is_real_gpu_va=0 "
           "syncfile_dma_fence_display_completion_credit=0 "
           "host_saw_display_bind_packet=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 "
           "webkit_accel_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_pending_syncobject_object_ref_active,
           stats.dxg_display_bind_provider_pending_syncobject_shared_owner_present,
           stats.dxg_display_bind_provider_pending_syncobject_monitored_fence,
           stats.dxg_display_bind_provider_pending_syncobject_fence_value,
           stats.dxg_display_bind_provider_pending_syncobject_fence_cpu_va_present,
           stats.dxg_display_bind_provider_pending_syncobject_fence_kva_present,
           stats.dxg_display_bind_provider_pending_syncobject_fence_gpu_va_present,
           stats.dxg_display_bind_provider_pending_syncobject_fence_gpu_va_alias_gap,
           stats.dxg_display_bind_provider_pending_syncobject_fence_map_size,
           stats.dxg_display_bind_provider_pending_syncobject_real_fence_gpu_va_present,
           stats.dxg_display_bind_provider_pending_syncobject_fence_gpu_va_source,
           stats.dxg_display_bind_host_saw_packet,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           display_bind_provider_sync_fence_alias_ok ?
               "PASS_FAILCLOSED" : "FAIL");
    printf("gpu_core_c_validator "
           "d3d12_display_bind_provider_no_send_preflight_matrix "
           "provider_submits=%lu request_metadata_complete=%lu "
           "provider_pin_revalidated=%lu source_generation_match=%s "
           "resource_generation_match=%s preflight_ready=%lu "
           "send_attempts=%lu send_blocked_no_host_abi=%lu "
           "completion_demux_attempts=%lu "
           "completion_demux_blocked_no_contract=%lu "
           "host_saw_display_bind_packet=%lu "
           "display_bind_transport_source=%s "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_request_metadata_complete,
           stats.dxg_display_bind_provider_pin_revalidated,
           stats.dxg_display_bind_provider_pending_source_generation != 0 &&
                   stats.dxg_display_bind_provider_pending_source_generation ==
                       stats.dxg_display_bind_source_generation ?
               "PASS" : "FAIL",
           stats.dxg_display_bind_provider_pending_resource_generation != 0 &&
                   stats.dxg_display_bind_provider_pending_resource_generation ==
                       stats.dxg_display_bind_resource_generation ?
               "PASS" : "FAIL",
           stats.dxg_display_bind_provider_preflight_ready,
           stats.dxg_display_bind_provider_send_attempts,
           stats.dxg_display_bind_provider_send_blocked_no_host_abi,
           stats.dxg_display_bind_provider_completion_demux_attempts,
           stats.dxg_display_bind_provider_completion_demux_blocked_no_contract,
           stats.dxg_display_bind_host_saw_packet,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           display_bind_provider_no_send_preflight_ok ?
               "PASS_FAILCLOSED" : "FAIL");
    printf("gpu_core_c_validator "
           "d3d12_display_bind_provider_packet_lifetime_matrix "
           "provider_submits=%lu publication_attempts=%lu "
           "packet_listed=0 request_id=0 transport_pending_id=%lu "
           "command_id=%lu transaction_id=%lu channel=%s "
           "packet_completed=0 wait_cancelled=0 "
           "packet_removed_on_cancel=0 completion_demux_registered=%lu "
           "resolved_or_cancelled=%lu refs_released=%lu "
           "pending_active=%lu host_saw_display_bind_packet=%lu "
           "display_bind_transport_source=%s "
           "wsl_presenthistory_completion_credit=%lu "
           "present_id=%lu completed=%lu native_present_credit=%lu "
           "opengl_submit_credit=%u webkit_accel_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_provider_publication_attempts,
           stats.dxg_display_bind_provider_transport_pending_id,
           stats.dxg_display_bind_provider_command_id,
           stats.dxg_display_bind_provider_transaction_id,
           stats.dxg_display_bind_provider_channel == 0 ? "none" : "other",
           stats.dxg_display_bind_provider_completion_demux_registered,
           stats.dxg_display_bind_provider_resolved_or_cancelled,
           stats.dxg_display_bind_provider_refs_released,
           stats.dxg_display_bind_pending_active,
           stats.dxg_display_bind_host_saw_packet,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           stats.dxg_display_bind_wsl_presenthistory_completion_credit,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           display_bind_provider_packet_lifetime_ok ?
               "PASS_FAILCLOSED" : "FAIL");
    printf("gpu_core_c_validator d3d12_display_bind_success_shape_matrix "
           "transport_present=%lu status_code=%lu block_reason=0x%lx "
           "completion_source=%lu present_id=%lu completed=%lu "
           "source_generation=%lu resource_generation=%lu "
           "scanout_successes=%lu completion_successes=%lu "
           "provider_submits=%lu provider_pin_revalidated=%lu "
           "publication_attempted=%lu publish_before_send=%lu "
           "transport_pending_id=%lu command_id=%lu transaction_id=%lu "
           "channel=%lu completion_demux_registered=%lu "
           "transport_source=%s host_saw_display_bind_packet=%lu "
           "wsl_presenthistory_completion_credit=%lu "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu failclosed_allowed=1 "
           "success_requires_provider_clear=1 "
           "success_requires_display_completion=1 "
           "success_requires_source_authority=1 "
           "native_present_credit=%lu backend_opengl_submit=%u "
           "status=%s\n",
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_status,
           stats.dxg_display_bind_block_reason,
           stats.dxg_display_bind_completion_source,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_completion_successes,
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_provider_pin_revalidated,
           stats.dxg_display_bind_provider_publication_attempts,
           stats.dxg_display_bind_provider_publish_before_send,
           stats.dxg_display_bind_provider_transport_pending_id,
           stats.dxg_display_bind_provider_command_id,
           stats.dxg_display_bind_provider_transaction_id,
           stats.dxg_display_bind_provider_channel,
           stats.dxg_display_bind_provider_completion_demux_registered,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           stats.dxg_display_bind_host_saw_packet,
           stats.dxg_display_bind_wsl_presenthistory_completion_credit,
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           display_bind_success_shape_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator d3d12_native_completion_lifetime_matrix "
           "transport_present=%lu status_code=%lu block_reason=0x%lx "
           "completion_source=%lu present_id=%lu completed=%lu "
           "source_generation=%lu resource_generation=%lu "
           "completion_successes=%lu provider_no_completion=%lu "
           "callbacks_after_completion_required=1 "
           "releases_after_completion_required=1 "
           "close_before_signal_cancel_required=1 cleanup_balance_required=1 "
           "failclosed_callbacks_after_completion=0 "
           "failclosed_releases_after_completion=0 "
           "native_present_credit=%lu backend_opengl_submit=%u "
           "status=%s\n",
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_status,
           stats.dxg_display_bind_block_reason,
           stats.dxg_display_bind_completion_source,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_scanout_bind_completion_successes,
           stats.dxg_display_bind_provider_no_completion,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           native_completion_lifetime_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "d3d12_display_bind_stale_source_zero_credit_matrix "
           "after_close_queries=%lu stale_source_rejects=%lu "
           "release_clears=%lu stale_generation_rejects=%lu "
           "stale_completion_rejects=%lu late_completion_after_release=%lu "
           "after_close_nonzero_id_rejects=%lu "
           "global_present_id_after_close=%lu "
           "global_completed_after_close=%lu native_present_credit=%lu "
           "opengl_submit_credit=%u stale_completion_rejected=%s "
           "late_completion_rejected=%s webkit_accel_credit=0 status=%s\n",
           stats.dxg_display_bind_after_close_queries,
           stats.dxg_display_bind_stale_source_rejects,
           stats.dxg_display_bind_release_clears,
           stats.dxg_display_bind_stale_generation_rejects,
           stats.dxg_display_bind_stale_completion_rejects,
           stats.dxg_display_bind_late_completion_after_release,
           stats.dxg_display_bind_after_close_nonzero_id_rejects,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           stats.dxg_display_bind_stale_completion_rejects != 0 ?
               "PASS" : "PENDING",
           stats.dxg_display_bind_late_completion_after_release == 0 ?
               "PASS" : "FAIL",
           stale_source_zero_credit_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "d3d12_display_bind_stale_async_completion_contract_matrix "
           "real_sender=0 completion_demux_registered=%lu "
           "transport_pending_id=%lu pending_active=%lu "
           "owner_close_cancel_required_after_send=1 "
           "owner_close_cancelled=%lu owner_close_cancel_deferred=1 "
           "late_completion_reject_required=1 "
           "late_completion_after_release=%lu late_completion_rejected=%u "
           "stale_source_rejects=%lu stale_generation_rejects=%lu "
           "stale_after_release_rejects=%lu stale_completion_rejects=%lu "
           "after_close_nonzero_id_rejects=%lu host_saw_display_bind_packet=%lu "
           "display_bind_transport_source=%s present_id=%lu completed=%lu "
           "native_present_credit=%lu opengl_submit_credit=%u "
           "webkit_accel_credit=0 status=%s\n",
           stats.dxg_display_bind_provider_completion_demux_registered,
           stats.dxg_display_bind_provider_transport_pending_id,
           stats.dxg_display_bind_pending_active,
           stats.dxg_display_bind_provider_pending_owner_close_cancelled,
           stats.dxg_display_bind_late_completion_after_release,
           stale_async_completion_contract_ok ? 1 : 0,
           stats.dxg_display_bind_stale_source_rejects,
           stats.dxg_display_bind_stale_generation_rejects,
           stats.dxg_display_bind_stale_after_release_rejects,
           stats.dxg_display_bind_stale_completion_rejects,
           stats.dxg_display_bind_after_close_nonzero_id_rejects,
           stats.dxg_display_bind_host_saw_packet,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           stale_async_completion_contract_ok ?
               "PASS_FAILCLOSED" : "FAIL");
    printf("gpu_core_c_validator d3d12_native_completion_not_kms_matrix "
           "generic_display_last_complete=%lu "
           "kms_vblank_display_correlated=%lu "
           "kms_vblank_source_software_display=%lu "
           "kms_vblank_source_native_hw=%lu "
           "kms_atomic_out_fence_display_correlated=%lu "
           "kms_atomic_out_fence_software_scanout_correlated=%lu "
           "kms_page_flip_events=%lu page_flip_events_software_blit=%lu "
           "page_flip_events_native_hw=%lu display_wait_is_native=0 "
           "kms_generic_display_credit=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.display_last_complete,
           stats.kms_vblank_display_correlated,
           stats.kms_vblank_source_software_display,
           stats.kms_vblank_source_nouveau_hw,
           stats.kms_atomic_out_fence_display_correlated,
           stats.kms_atomic_out_fence_software_scanout_correlated,
           stats.kms_vblank_page_flip_events,
           stats.kms_page_flip_events_software_blit,
           stats.kms_page_flip_events_native_hw,
           generic_completion_not_native_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator "
           "d3d12_native_completion_consumer_escrow_matrix "
           "display_bind_gate=closed provider_completion_present=0 "
           "transport_present=%lu transport_pending_id=%lu "
           "completion_demux_registered=%lu host_saw_display_bind_packet=%lu "
           "display_bind_transport_source=%s present_id=%lu completed=%lu "
           "resource_generation=%lu provider_no_completion=%lu "
           "callback_release_credit=0 frame_callback_credit=0 "
           "final_handoff_credit=0 fps_visible_credit=0 "
           "content_progress_credit=0 webkit_accel_credit=0 "
           "callback_release_order=blocked release_order=blocked "
           "consumer_visible_credit=blocked "
           "close_before_signal_cancel=deferred "
           "display_target_kind=%lu native_present_credit=%lu "
           "opengl_submit_credit=%u status=%s\n",
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_provider_transport_pending_id,
           stats.dxg_display_bind_provider_completion_demux_registered,
           stats.dxg_display_bind_host_saw_packet,
           display_bind_transport_source_name(
               stats.dxg_display_bind_transport_source),
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_display_bind_provider_no_completion,
           stats.dxg_present_display_target_kind,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           native_completion_consumer_escrow_ok ?
               "PASS_FAILCLOSED" : "FAIL");
    printf("gpu_core_c_validator wsl_standard_alloc_not_display_bind_matrix "
           "standard_alloc_private_data=%lu "
           "standard_alloc_display_bind_absent=%lu "
           "standard_alloc_role=private_driver_data "
           "standard_alloc_native_present_credit=0 "
           "display_bind_transport_present=%lu present_id=%lu "
           "completed=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_scanout_bind_standard_alloc_private_data,
           stats.dxg_scanout_bind_standard_alloc_display_bind_absent,
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           standard_alloc_not_display_bind_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator gpu_remaining_plan_dependency_skeleton_matrix "
           "root_display_bind_gate=closed native_present_gate=closed "
           "real_display_bind_sender=%lu real_display_bind_completion=%lu "
           "gpup_sender_contract=%lu gpup_completion_contract=%lu "
           "native_completion_validators=armed "
           "native_completion_validator_gate=closed finite_480p_gate=closed "
           "demo_interaction_gate=closed backend_opengl_submit_gate=closed "
           "kvm_virgl_recheck_gate=deferred webkit_route_gate=closed "
           "webkit_content_gate=closed webkit_enabled_artifact_gate=closed "
           "dda_nouveau_blocker=separate-display-not-D3D12-bind "
           "dda_nouveau_reason=DDA/Nouveau-separate-display-not-D3D12-bind "
           "wsl_display_bind_ioctl=0 wsl_inband_presenthistory_handler=absent "
           "dda_d3d12_resource_import=%lu dda_scanout_bind=%lu "
           "dda_hw_flip_completion=%s display_bind_present_id=%lu "
           "display_bind_completed=%lu backend_opengl_submit=%u "
           "native_present_credit=0 opengl_submit_credit=0 "
           "webkit_accel_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present,
           stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           backend_opengl_submit,
           display_bind_success_shape_ok &&
                   display_bind_pending_lifetime_ok &&
                   display_bind_generation_revalidation_ok &&
                   display_bind_provider_pending_publication_ok &&
                   native_completion_lifetime_ok &&
                   native_completion_consumer_escrow_ok &&
                   provider_credit_gate_ok &&
                   generic_completion_not_native_ok &&
                   standard_alloc_not_display_bind_ok &&
                   dda_nouveau_separate_display_not_bind_ok &&
                   wsl_uapi_negative_ok &&
                   wsl_adapter_display_caps_negative_ok &&
                   wsl_submit_present_fields_not_bind_ok &&
                   wsl_stdalloc_and_alloc_flags_not_bind_ok &&
                   wsl_trace_display_bind_negative_ok &&
                   provider_credit_gate_negative_ok &&
                   d3d12_display_bind_host_abi_discovery_ok &&
                   dda_nouveau_non_readback_display_proof_ok &&
                   host_display_bind_source_catalog_ok &&
                   stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                   stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                   stats.dxg_present_dda_nouveau_import_path_present == 0 &&
                   stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
                   stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
                   stats.dxg_display_bind_present_id == 0 &&
                   stats.dxg_display_bind_completed_id == 0 &&
                   backend_opengl_submit == 0 ?
               "PASS" : "FAIL");
    printf("gpu_core_c_validator gpu_remaining_holistic_skeleton_matrix "
           "skeleton_version=2 active_open_items=12 "
           "plan_source=GPU_REMAINING_GAPS.md "
           "ordered_chunks=display_bind,native_completion,fps,backend,webkit "
           "display_bind_source_gate=closed "
           "bind_contract_gate=failclosed "
           "native_completion_gate=armed_without_lane "
           "finite_480p_gate=closed demo_interaction_gate=closed "
           "backend_opengl_submit_gate=closed "
           "kvm_virgl_recheck_gate=deferred "
           "webkit_route_gate=closed webkit_content_gate=closed "
           "webkit_enabled_artifact_gate=closed "
           "selected_lane=gpup_dxg_scanout_bind "
           "completion_authority=display_bind_provider "
           "wsl_display_bind_ioctl=0 "
           "gpup_sender_contract=%lu gpup_completion_contract=%lu "
           "dda_d3d12_resource_import=%lu dda_scanout_bind=%lu "
           "dda_hw_flip_completion=%s "
           "display_bind_transport_present=%lu "
           "display_bind_present_id=%lu display_bind_completed_id=%lu "
           "native_present_credit=%lu backend_opengl_submit=%u "
           "opengl_submit_credit=0 webkit_accel_credit=0 status=%s\n",
           stats.dxg_scanout_bind_candidate_sender_contracts,
           stats.dxg_scanout_bind_candidate_completion_contracts,
           stats.dxg_present_dda_nouveau_import_path_present,
           stats.dxg_present_dda_nouveau_scanout_bind_present,
           stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.nouveau_pci_native_present_credit,
           backend_opengl_submit,
           gpu_remaining_holistic_skeleton_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator d3d12_display_bind_backend_boundary_matrix "
           "backend=%lu contract_version=%lu transport=%lu "
           "transport_present=%lu operation=%lu completion_source=%lu "
           "required_metadata=0x%lx lifetime=0x%lx block_reason=0x%lx "
           "present_id=%lu completed=%lu source_generation=%lu "
           "resource_generation=%lu status_code=%lu provider_submits=%lu "
           "lock_dropped_submits=%lu revalidate_attempts=%lu "
           "revalidate_successes=%lu revalidate_failures=%lu "
           "provider_pin_revalidated=%lu provider_no_host_abi=%lu "
           "provider_no_sender=%lu provider_no_completion=%lu "
           "custom_host_tool=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_backend,
           stats.dxg_display_bind_contract_version,
           stats.dxg_display_bind_transport,
           stats.dxg_display_bind_transport_present,
           stats.dxg_display_bind_operation,
           stats.dxg_display_bind_completion_source,
           stats.dxg_display_bind_required_metadata,
           stats.dxg_display_bind_lifetime,
           stats.dxg_display_bind_block_reason,
           stats.dxg_display_bind_present_id,
           stats.dxg_display_bind_completed_id,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_display_bind_status,
           stats.dxg_display_bind_provider_submits,
           stats.dxg_display_bind_lock_dropped_submits,
           stats.dxg_display_bind_revalidate_attempts,
           stats.dxg_display_bind_revalidate_successes,
           stats.dxg_display_bind_revalidate_failures,
           stats.dxg_display_bind_provider_pin_revalidated,
           stats.dxg_display_bind_provider_no_host_abi,
           stats.dxg_display_bind_provider_no_sender,
           stats.dxg_display_bind_provider_no_completion,
           stats.dxg_display_bind_contract_version == 1 &&
                   stats.dxg_display_bind_backend ==
                       FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND &&
                   stats.dxg_display_bind_transport ==
                       FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE &&
                   stats.dxg_display_bind_transport_present == 0 &&
                   stats.dxg_display_bind_operation ==
                       FB_GPU_DXG_PRESENT_GPUP_DDA_OP_SCANOUT_BIND &&
                   stats.dxg_display_bind_completion_source ==
                       FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
                   stats.dxg_display_bind_present_id == 0 &&
                   stats.dxg_display_bind_completed_id == 0 &&
                   stats.dxg_display_bind_status == EOPNOTSUPP &&
                   stats.dxg_display_bind_revalidate_failures == 0 &&
                   stats.dxg_display_bind_provider_no_host_abi == 1 &&
                   stats.dxg_display_bind_provider_no_sender == 1 &&
                   stats.dxg_display_bind_provider_no_completion == 1 ?
               "PASS" : "DIAGNOSTIC");
    printf("gpu_core_c_validator d3d12_display_bind_pin_lifetime_matrix "
           "pin_attempts=%lu pin_successes=%lu pin_failures=%lu "
           "unpins=%lu pinned_dxg_file=%lu pinned_resource_file=%lu "
           "pinned_resource_generation=%lu pinned_process_generation=%lu "
           "pinned_process_refs=%lu pinned_shared_parent=%lu "
           "pinned_parent_refs=%lu pinned_parent_children=%lu "
           "source_generation=%lu "
           "resource_generation=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_display_bind_pin_attempts,
           stats.dxg_display_bind_pin_successes,
           stats.dxg_display_bind_pin_failures,
           stats.dxg_display_bind_unpins,
           stats.dxg_display_bind_pinned_dxg_file,
           stats.dxg_display_bind_pinned_resource_file,
           stats.dxg_display_bind_pinned_resource_generation,
           stats.dxg_display_bind_pinned_process_generation,
           stats.dxg_display_bind_pinned_process_refs,
           stats.dxg_display_bind_pinned_shared_parent,
           stats.dxg_display_bind_pinned_parent_refs,
           stats.dxg_display_bind_pinned_parent_children,
           stats.dxg_display_bind_source_generation,
           stats.dxg_display_bind_resource_generation,
           stats.dxg_display_bind_pin_attempts == 0 ||
                   (stats.dxg_display_bind_pin_successes != 0 &&
                    stats.dxg_display_bind_unpins ==
                        stats.dxg_display_bind_pin_successes &&
                    stats.dxg_display_bind_pinned_dxg_file == 1 &&
                    stats.dxg_display_bind_pinned_resource_file == 1 &&
                    stats.dxg_display_bind_pinned_resource_generation != 0 &&
                    stats.dxg_display_bind_pinned_process_generation != 0 &&
                    stats.dxg_display_bind_pinned_process_refs != 0 &&
                    stats.dxg_display_bind_pinned_shared_parent != 0 &&
                    stats.dxg_display_bind_pinned_parent_refs != 0 &&
                    stats.dxg_display_bind_pinned_parent_children != 0) ?
               "PASS" : "DIAGNOSTIC");
    printf("gpu_core_c_validator dxg_syncfile_not_kms_completion_matrix "
           "syncfile_only=%lu weak_evidence_rejects=%lu "
           "scanout_successes=%lu completion_successes=%lu "
           "completion_pending=%lu kms_no_hw_completion=%lu "
           "present_id=%lu completed=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.dxg_scanout_bind_weak_syncfile_only,
           stats.dxg_scanout_bind_weak_evidence_rejects,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_completion_successes,
           stats.dxg_scanout_bind_completion_pending,
           stats.kms_present_reject_no_hw_completion,
           stats.dxg_scanout_bind_last_present_id,
           stats.dxg_scanout_bind_last_completed,
           stats.dxg_scanout_bind_successes == 0 &&
                   stats.dxg_scanout_bind_completion_successes == 0 &&
                   stats.dxg_scanout_bind_last_present_id == 0 &&
                   stats.dxg_scanout_bind_last_completed == 0 ?
               "PASS" : "FAIL");
    printf("gpu_core_c_validator native_display_readiness_failclosed_matrix "
           "backend=%u hyperv_gpup=%s native_display_ready=%d "
           "dda_native_display_present=%d display_target_kind=%lu "
           "display_probe_attempts=%lu head_probe_attempts=%lu "
           "connector_probe_attempts=%lu engine_object=%lu "
           "mode_config_ready=%lu crtcs=%lu encoders=%lu "
           "primary_planes=%lu outp_mask_seen=%lu "
           "conn_mask_seen=%lu head_mask_seen=%lu nvif_heads=%lu "
           "nonvirtual_connectors=%lu hpd_event=%lu dp_irq_event=%lu "
           "vblank_event=%lu vblank_irq_supported=%lu "
           "vblank_source=%s page_flip_ready=%lu "
           "atomic_commit_tail=%lu page_flip_event_source=%s "
           "atomic_backend_missing=%lu "
           "dxg_scanout_bind_successes=%lu present_id=%lu completed=%lu "
           "reject_reasons=0x%lx reject_has_atomic_pageflip_backend=%u "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           backend.backend,
           hyperv_gpup_failclosed ? "PASS" : "FAIL",
           stats.nouveau_native_display_ready != 0,
           stats.nouveau_dda_native_display_present != 0,
           stats.dxg_present_display_target_kind,
           stats.nouveau_display_probe_attempts,
           stats.nouveau_display_head_probe_attempts,
           stats.nouveau_display_connector_probe_attempts,
           stats.nouveau_display_engine_object_created,
           stats.nouveau_display_mode_config_ready,
           stats.nouveau_display_crtc_count,
           stats.nouveau_display_encoder_count,
           stats.nouveau_display_primary_plane_count,
           stats.nouveau_display_outp_mask_seen,
           stats.nouveau_display_conn_mask_seen,
           stats.nouveau_display_head_mask_seen,
           stats.nouveau_display_nvif_head_ctor_successes,
           stats.nouveau_display_nonvirtual_connectors,
           stats.nouveau_display_hpd_event_registered,
           stats.nouveau_display_dp_irq_event_registered,
           stats.nouveau_display_vblank_event_registered,
           stats.nouveau_display_vblank_irq_supported,
           stats.nouveau_display_vblank_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_page_flip_completion_ready,
           stats.nouveau_display_atomic_commit_tail_ready,
           stats.nouveau_display_page_flip_event_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_atomic_pageflip_backend_missing,
           stats.dxg_scanout_bind_successes,
           stats.dxg_scanout_bind_last_present_id,
           stats.dxg_scanout_bind_last_completed,
           stats.nouveau_native_display_reject_reasons,
           (stats.nouveau_native_display_reject_reasons &
            FB_GPU_KMS_PRESENT_REJECT_NO_ATOMIC_PAGEFLIP_BACKEND) != 0,
           native_display_failclosed_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator nouveau_display_failclosed_matrix "
           "accepts=%lu probe_attempts=%lu create_attempts=%lu "
           "create_successes=%lu create_fail_closed=%lu "
           "create_fail_reason=0x%lx head_probe_attempts=%lu heads=%lu "
           "connector_probe_attempts=%lu connectors=%lu "
           "engine_object=%lu mode_config_ready=%lu "
           "crtcs=%lu encoders=%lu primary_planes=%lu "
           "linear_required=%lu nonlinear_modifiers=%lu "
           "outp_mask_seen=%lu conn_mask_seen=%lu "
           "head_mask_seen=%lu nvif_heads=%lu "
           "nonvirtual_connectors=%lu hpd_event=%lu dp_irq_event=%lu "
           "vblank_supported=%lu vblank_event=%lu "
           "vblank_irq_supported=%lu vblank_source=%s "
           "vblank_irqs=%lu page_flip_ready=%lu flip_completions=%lu "
           "atomic_commit_tail=%lu page_flip_event_source=%s "
           "atomic_backend_missing=%lu atomic_missing_policy=%s "
           "dda_native_display_present=%lu native_display_ready=%lu "
           "reject_reasons=0x%lx native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_display_probe_attempts,
           stats.nouveau_display_create_attempts,
           stats.nouveau_display_create_successes,
           stats.nouveau_display_create_fail_closed,
           stats.nouveau_display_create_fail_reason,
           stats.nouveau_display_head_probe_attempts,
           stats.nouveau_display_heads,
           stats.nouveau_display_connector_probe_attempts,
           stats.nouveau_display_connectors,
           stats.nouveau_display_engine_object_created,
           stats.nouveau_display_mode_config_ready,
           stats.nouveau_display_crtc_count,
           stats.nouveau_display_encoder_count,
           stats.nouveau_display_primary_plane_count,
           stats.nouveau_display_primary_plane_linear_required,
           stats.nouveau_display_primary_plane_nonlinear_modifiers,
           stats.nouveau_display_outp_mask_seen,
           stats.nouveau_display_conn_mask_seen,
           stats.nouveau_display_head_mask_seen,
           stats.nouveau_display_nvif_head_ctor_successes,
           stats.nouveau_display_nonvirtual_connectors,
           stats.nouveau_display_hpd_event_registered,
           stats.nouveau_display_dp_irq_event_registered,
           stats.nouveau_display_vblank_supported,
           stats.nouveau_display_vblank_event_registered,
           stats.nouveau_display_vblank_irq_supported,
           stats.nouveau_display_vblank_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_vblank_irqs,
           stats.nouveau_display_page_flip_completion_ready,
           stats.nouveau_display_page_flip_completions,
           stats.nouveau_display_atomic_commit_tail_ready,
           stats.nouveau_display_page_flip_event_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_atomic_pageflip_backend_missing,
           nouveau_atomic_pageflip_backend_missing_ok ? "PASS" : "FAIL",
           stats.nouveau_dda_native_display_present,
           stats.nouveau_native_display_ready,
           stats.nouveau_native_display_reject_reasons,
           native_display_failclosed_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator kms_present_discriminator_failclosed_matrix "
           "last_lane=%lu kms_present_dumb=%lu kms_present_synthvid=%lu "
           "kms_present_nouveau_hw=%lu rejects=%lu reject_reasons=0x%lx "
           "no_native_display=%lu no_nouveau_display=%lu "
           "no_display_create=%lu no_heads=%lu no_connectors=%lu "
           "no_vblank=%lu no_hw_completion=%lu "
           "no_atomic_pageflip_backend=%lu selected=none "
           "native_display_ready=%lu dda_native_display_present=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.kms_present_last_lane,
           stats.kms_present_dumb,
           stats.kms_present_synthvid,
           stats.kms_present_nouveau_hw,
           stats.kms_present_rejects,
           stats.kms_present_reject_reasons,
           stats.kms_present_reject_no_native_display,
           stats.kms_present_reject_no_nouveau_display,
           stats.kms_present_reject_no_display_create,
           stats.kms_present_reject_no_heads,
           stats.kms_present_reject_no_connectors,
           stats.kms_present_reject_no_vblank,
           stats.kms_present_reject_no_hw_completion,
           stats.kms_present_reject_no_atomic_pageflip_backend,
           stats.nouveau_native_display_ready,
           stats.nouveau_dda_native_display_present,
           native_display_failclosed_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator nouveau_display_kms_registration_matrix "
           "accepts=%lu display_probe_attempts=%lu "
           "display_create_attempts=%lu display_create_successes=%lu "
           "head_probe_attempts=%lu heads=%lu "
           "connector_probe_attempts=%lu connectors=%lu "
           "engine_object=%lu mode_config_ready=%lu "
           "crtcs=%lu encoders=%lu primary_planes=%lu "
           "outp_mask_seen=%lu conn_mask_seen=%lu "
           "head_mask_seen=%lu nvif_heads=%lu "
           "nonvirtual_connectors=%lu hpd_event=%lu dp_irq_event=%lu "
           "vblank_event=%lu vblank_irq_supported=%lu "
           "vblank_source=%s page_flip_ready=%lu "
           "atomic_commit_tail=%lu page_flip_event_source=%s "
           "atomic_backend_missing=%lu "
           "kms_registered=%u native_display_ready=%lu "
           "dda_native_display_present=%lu registration_source=%s "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_display_probe_attempts,
           stats.nouveau_display_create_attempts,
           stats.nouveau_display_create_successes,
           stats.nouveau_display_head_probe_attempts,
           stats.nouveau_display_heads,
           stats.nouveau_display_connector_probe_attempts,
           stats.nouveau_display_connectors,
           stats.nouveau_display_engine_object_created,
           stats.nouveau_display_mode_config_ready,
           stats.nouveau_display_crtc_count,
           stats.nouveau_display_encoder_count,
           stats.nouveau_display_primary_plane_count,
           stats.nouveau_display_outp_mask_seen,
           stats.nouveau_display_conn_mask_seen,
           stats.nouveau_display_head_mask_seen,
           stats.nouveau_display_nvif_head_ctor_successes,
           stats.nouveau_display_nonvirtual_connectors,
           stats.nouveau_display_hpd_event_registered,
           stats.nouveau_display_dp_irq_event_registered,
           stats.nouveau_display_vblank_event_registered,
           stats.nouveau_display_vblank_irq_supported,
           stats.nouveau_display_vblank_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_page_flip_completion_ready,
           stats.nouveau_display_atomic_commit_tail_ready,
           stats.nouveau_display_page_flip_event_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_atomic_pageflip_backend_missing,
           nouveau_display_kms_registered,
           stats.nouveau_native_display_ready,
           stats.nouveau_dda_native_display_present,
           stats.nouveau_pci_probe_accepts == 0 ? "GPU_P_FAIL_CLOSED" :
                                                   "DDA_DIAGNOSTIC",
           nouveau_display_kms_registration_ok ?
               (hyperv_gpup_failclosed ? "PASS" : "DIAGNOSTIC") : "FAIL");
    printf("gpu_core_c_validator nouveau_kms_vblank_irq_source_matrix "
           "kms_vblank_sequence=%lu kms_vblank_samples=%lu "
           "kms_display_correlated=%lu kms_synthetic=%lu "
           "kms_source_software_display=%lu kms_source_native_hw=%lu "
           "nouveau_vblank_supported=%lu "
           "nouveau_vblank_irq_supported=%lu nouveau_vblank_source=%s "
           "nouveau_vblank_event=%lu page_flip_event_source=%s "
           "atomic_commit_tail=%lu "
           "nouveau_vblank_irqs=%lu nouveau_irq_claimed=%lu "
           "page_flip_ready=%lu flip_completions=%lu irq_source=%s "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.kms_vblank_sequence,
           stats.kms_vblank_samples,
           stats.kms_vblank_display_correlated,
           stats.kms_vblank_synthetic,
           stats.kms_vblank_source_software_display,
           stats.kms_vblank_source_nouveau_hw,
           stats.nouveau_display_vblank_supported,
           stats.nouveau_display_vblank_irq_supported,
           stats.nouveau_display_vblank_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_vblank_event_registered,
           stats.nouveau_display_page_flip_event_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_atomic_commit_tail_ready,
           stats.nouveau_display_vblank_irqs,
           stats.nouveau_pci_irq_delivery_claimed,
           stats.nouveau_display_page_flip_completion_ready,
           stats.nouveau_display_page_flip_completions,
           stats.nouveau_display_vblank_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ?
               "nouveau_hw" : "not_nouveau",
           nouveau_kms_vblank_irq_source_ok ?
               (hyperv_gpup_failclosed ? "PASS" : "DIAGNOSTIC") : "FAIL");
    printf("gpu_core_c_validator "
           "nouveau_primary_plane_modifier_failclosed_matrix "
           "primary_plane=diagnostic required_modifier=LINEAR "
           "primary_planes=%lu linear_required=%lu "
           "nonlinear_modifiers=%lu nouveau_hw_scanout=%lu "
           "native_display_ready=%lu modifier_credit=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.nouveau_display_primary_plane_count,
           stats.nouveau_display_primary_plane_linear_required,
           stats.nouveau_display_primary_plane_nonlinear_modifiers,
           stats.kms_present_nouveau_hw,
           stats.nouveau_native_display_ready,
           nouveau_primary_plane_modifier_failclosed_ok ?
               (hyperv_gpup_failclosed ? "PASS" : "DIAGNOSTIC") : "FAIL");
    printf("gpu_core_c_validator nouveau_linux_display_readiness_matrix "
           "display_engine_object=%lu mode_config_ready=%lu "
           "outp_mask_seen=%lu conn_mask_seen=%lu head_mask_seen=%lu "
           "crtcs=%lu encoders=%lu primary_planes=%lu "
           "linear_required=%lu nonlinear_modifiers=%lu "
           "nvif_heads=%lu heads=%lu connectors=%lu "
           "nonvirtual_connectors=%lu hpd_event=%lu dp_irq_event=%lu "
           "vblank_event_registered=%lu vblank_source=%s "
           "page_flip_event_source=%s atomic_commit_tail=%lu "
           "native_display_ready=%lu dda_native_display_present=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.nouveau_display_engine_object_created,
           stats.nouveau_display_mode_config_ready,
           stats.nouveau_display_outp_mask_seen,
           stats.nouveau_display_conn_mask_seen,
           stats.nouveau_display_head_mask_seen,
           stats.nouveau_display_crtc_count,
           stats.nouveau_display_encoder_count,
           stats.nouveau_display_primary_plane_count,
           stats.nouveau_display_primary_plane_linear_required,
           stats.nouveau_display_primary_plane_nonlinear_modifiers,
           stats.nouveau_display_nvif_head_ctor_successes,
           stats.nouveau_display_heads,
           stats.nouveau_display_connectors,
           stats.nouveau_display_nonvirtual_connectors,
           stats.nouveau_display_hpd_event_registered,
           stats.nouveau_display_dp_irq_event_registered,
           stats.nouveau_display_vblank_event_registered,
           stats.nouveau_display_vblank_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_page_flip_event_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_atomic_commit_tail_ready,
           stats.nouveau_native_display_ready,
           stats.nouveau_dda_native_display_present,
           nouveau_linux_display_readiness_ok ?
               (hyperv_gpup_failclosed ? "PASS_FAILCLOSED" : "PASS") :
               "FAIL");
    printf("gpu_core_c_validator nouveau_kms_acceptance_shape_matrix "
           "linux_model=nouveau_display_create "
           "kernel_gate=full_linux_shape accepts=%lu "
           "display_create=%lu engine_object=%lu mode_config_ready=%lu "
           "crtcs=%lu encoders=%lu primary_planes=%lu "
           "outp_mask_seen=%lu conn_mask_seen=%lu head_mask_seen=%lu "
           "nvif_heads=%lu heads=%lu connectors=%lu "
           "nonvirtual_connectors=%lu hpd_event=%lu dp_irq_event=%lu "
           "vblank_supported=%lu vblank_irq_supported=%lu "
           "vblank_event=%lu vblank_source=%s "
           "page_flip_ready=%lu flip_completions=%lu "
           "page_flip_event_source=%s atomic_commit_tail=%lu "
           "atomic_backend_missing=%lu linear_required=%lu "
           "nonlinear_modifiers=%lu native_display_ready=%lu "
           "dda_native_display_present=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_display_create_successes,
           stats.nouveau_display_engine_object_created,
           stats.nouveau_display_mode_config_ready,
           stats.nouveau_display_crtc_count,
           stats.nouveau_display_encoder_count,
           stats.nouveau_display_primary_plane_count,
           stats.nouveau_display_outp_mask_seen,
           stats.nouveau_display_conn_mask_seen,
           stats.nouveau_display_head_mask_seen,
           stats.nouveau_display_nvif_head_ctor_successes,
           stats.nouveau_display_heads,
           stats.nouveau_display_connectors,
           stats.nouveau_display_nonvirtual_connectors,
           stats.nouveau_display_hpd_event_registered,
           stats.nouveau_display_dp_irq_event_registered,
           stats.nouveau_display_vblank_supported,
           stats.nouveau_display_vblank_irq_supported,
           stats.nouveau_display_vblank_event_registered,
           stats.nouveau_display_vblank_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_page_flip_completion_ready,
           stats.nouveau_display_page_flip_completions,
           stats.nouveau_display_page_flip_event_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_atomic_commit_tail_ready,
           stats.nouveau_display_atomic_pageflip_backend_missing,
           stats.nouveau_display_primary_plane_linear_required,
           stats.nouveau_display_primary_plane_nonlinear_modifiers,
           stats.nouveau_native_display_ready,
           stats.nouveau_dda_native_display_present,
           nouveau_linux_display_readiness_ok ?
               (hyperv_gpup_failclosed ? "PASS_FAILCLOSED" : "PASS") :
               "FAIL");
    printf("gpu_core_c_validator nouveau_dda_display_positive_shape_matrix "
           "linux_model=nouveau_display_create "
           "dda_positive=%u display_create=%lu engine_object=%lu "
           "mode_config_ready=%lu crtcs=%lu encoders=%lu "
           "primary_planes=%lu outp_mask_seen=%lu conn_mask_seen=%lu "
           "head_mask_seen=%lu nvif_heads=%lu heads=%lu "
           "connectors=%lu nonvirtual_connectors=%lu hpd_event=%lu "
           "dp_irq_event=%lu vblank_event=%lu vblank_source=%s "
           "page_flip_ready=%lu flip_completions=%lu "
           "page_flip_event_source=%s atomic_commit_tail=%lu "
           "linear_required=%lu nonlinear_modifiers=%lu "
           "kms_lane=%lu kms_present_nouveau_hw=%lu "
           "page_flip_events_native_hw=%lu dda_native_display_credit=%lu "
           "d3d12_native_present_credit=0 opengl_submit_credit=0 "
           "webkit_credit=0 status=%s\n",
           stats.nouveau_pci_probe_accepts != 0 &&
               nouveau_display_kms_ready &&
               stats.nouveau_dda_native_display_present != 0,
           stats.nouveau_display_create_successes,
           stats.nouveau_display_engine_object_created,
           stats.nouveau_display_mode_config_ready,
           stats.nouveau_display_crtc_count,
           stats.nouveau_display_encoder_count,
           stats.nouveau_display_primary_plane_count,
           stats.nouveau_display_outp_mask_seen,
           stats.nouveau_display_conn_mask_seen,
           stats.nouveau_display_head_mask_seen,
           stats.nouveau_display_nvif_head_ctor_successes,
           stats.nouveau_display_heads,
           stats.nouveau_display_connectors,
           stats.nouveau_display_nonvirtual_connectors,
           stats.nouveau_display_hpd_event_registered,
           stats.nouveau_display_dp_irq_event_registered,
           stats.nouveau_display_vblank_event_registered,
           stats.nouveau_display_vblank_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_page_flip_completion_ready,
           stats.nouveau_display_page_flip_completions,
           stats.nouveau_display_page_flip_event_source ==
                   FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
               "none",
           stats.nouveau_display_atomic_commit_tail_ready,
           stats.nouveau_display_primary_plane_linear_required,
           stats.nouveau_display_primary_plane_nonlinear_modifiers,
           stats.kms_present_last_lane,
           stats.kms_present_nouveau_hw,
           stats.kms_page_flip_events_native_hw,
           stats.nouveau_pci_native_present_credit,
           nouveau_linux_display_readiness_ok &&
                   backend_opengl_submit == 0 &&
                   stats.dxg_display_bind_present_id == 0 &&
                   stats.dxg_display_bind_completed_id == 0 ?
               (hyperv_gpup_failclosed ? "PASS_FAILCLOSED" : "PASS") :
               "FAIL");
    printf("gpu_core_c_validator kms_scanout_cpu_convert_separation_matrix "
           "kms_present_dumb=%lu kms_present_synthvid=%lu "
           "kms_present_nouveau_hw=%lu blit_bytes=%lu "
           "software_scanout_fence=%lu cpu_convert_native_present=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.kms_present_dumb,
           stats.kms_present_synthvid,
           stats.kms_present_nouveau_hw,
           stats.blit_bytes,
           stats.kms_atomic_out_fence_software_scanout_correlated,
           kms_scanout_cpu_convert_separation_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator kms_gem_fb_plane_ref_matrix "
           "kms_framebuffers=%lu stale_kms_fbs=%lu bo_handles=%lu "
           "plane_ref_fields=bounded existing_kernel_fields=1 "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats.kms_framebuffers,
           stats.drm_file_stale_kms_fbs,
           stats.bo_handles,
           kms_gem_fb_plane_ref_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator kms_atomic_plane_state_matrix "
           "atomic_commits=%lu framebuffers=%lu page_flips=%lu "
           "out_fence_display_correlated=%lu "
           "out_fence_software_scanout_correlated=%lu "
           "plane_state_native_present=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.kms_atomic_commits,
           stats.kms_framebuffers,
           stats.kms_page_flips,
           stats.kms_atomic_out_fence_display_correlated,
           stats.kms_atomic_out_fence_software_scanout_correlated,
           kms_atomic_plane_state_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator kms_atomic_prepare_cleanup_fb_matrix "
           "in_fence_fd_refs=%lu in_fence_fd_ref_puts=%lu "
           "out_fence_prepared=%lu out_fence_cleanup_closes=%lu "
           "test_only_placeholders=%lu stale_kms_fbs=%lu "
           "fb_prepare_cleanup_credit=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.kms_atomic_in_fence_fd_refs,
           stats.kms_atomic_in_fence_fd_ref_puts,
           stats.kms_atomic_out_fence_prepared,
           stats.kms_atomic_out_fence_cleanup_closes,
           stats.kms_atomic_out_fence_test_only_placeholders,
           stats.drm_file_stale_kms_fbs,
           kms_atomic_prepare_cleanup_fb_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator kms_page_flip_feature_gate_matrix "
           "page_flips=%lu target_rejects=%lu async_rejects=%lu "
           "invalid_noevent_rejects=%lu page_flip_events=%lu "
           "page_flip_ready=%lu atomic_backend_missing=%lu "
           "target_gate=%s async_gate=%s "
           "page_flip_native_present_credit=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats.kms_page_flips,
           stats.kms_page_flip_target_rejects,
           stats.kms_page_flip_async_rejects,
           stats.kms_page_flip_invalid_noevent_rejects,
           stats.kms_vblank_page_flip_events,
           stats.nouveau_display_page_flip_completion_ready,
           stats.nouveau_display_atomic_pageflip_backend_missing,
           stats.nouveau_native_display_ready == 0 ? "closed" :
                                                      "diagnostic",
           stats.nouveau_native_display_ready == 0 ? "closed" :
                                                      "diagnostic",
           kms_page_flip_feature_gate_ok ? "PASS" : "FAIL");
    printf("gpu_core_c_validator nouveau_pci_dma_resource_matrix "
           "registered=%lu accepts=%lu reject_dxg_present=%lu "
           "reject_no_bars=%lu dma_mask_configured=%lu "
           "dma_mask_requested_bits=%lu dma_mask_bits=%lu "
           "dma_mask_effective_bits=%lu dma_mask_fallback_32=%lu "
           "coherent_configured=%lu coherent_requested_bits=%lu "
           "coherent_bits=%lu coherent_effective_bits=%lu "
           "coherent_fallback_32=%lu "
           "bar0_len=%lu bar1_len=%lu bar0_claimed=%lu "
           "bar1_claimed=%lu claim_failures=%lu releases=%lu "
           "resource_claims=%lu resource_releases=%lu "
           "resource_iomaps=%lu owner_mismatches=%lu "
           "dma_map_api=%lu dma_map_attempts=%lu "
           "dma_map_successes=%lu dma_map_failures=%lu "
           "dma_unmaps=%lu dma_last_size=%lu dma_last_addr=%lu "
           "dma_last_ret=%lu dma_map=%s "
           "unclaimed_iomaps=%lu unclaimed_releases=%lu "
           "irq_mode=%lu irq_failures=%lu msi_requested=%lu "
           "msi_fail_closed=%lu irq_vector_valid=%lu "
           "irq_alloc_requests=%lu irq_alloc_failures=%lu "
           "msi_program_attempts=%lu msi_program_unsupported=%lu "
           "msix_program_attempts=%lu msix_program_unsupported=%lu "
           "legacy_irq_requests=%lu legacy_irq_grants=%lu "
           "irq_handler_registered=%lu irq_delivery_enabled=%lu "
           "irq_delivery_claimed=%lu legacy_irq_fallback=%lu "
           "irq_handler_invocations=%lu irq_cause_reads=%lu "
           "irq_cause_valid=%lu irq_cause_acks=%lu irq_spurious=%lu "
           "suspend_count=%lu resume_count=%lu "
           "pm_balanced=%lu remove_while_suspended=%lu "
           "remove_calls=%lu remove_resume_attempts=%lu "
           "remove_resume_successes=%lu remove_barriers=%lu "
           "remove_active_before_callback=%lu hot_remove_events=%lu "
           "removed=%lu bar_iounmaps=%lu irq_unregisters=%lu "
           "irq_vectors_freed=%lu bus_master_clears=%lu "
           "device_disables=%lu drvdata_cleared=%lu "
           "native_present_credit=%lu status=PENDING\n",
           stats.nouveau_pci_registered,
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_probe_reject_dxg_present,
           stats.nouveau_pci_probe_reject_no_bars,
           stats.nouveau_pci_dma_mask_configured,
           stats.nouveau_pci_dma_mask_requested_bits,
           stats.nouveau_pci_dma_mask_bits,
           stats.nouveau_pci_dma_mask_effective_bits,
           stats.nouveau_pci_dma_mask_fallback_32,
           stats.nouveau_pci_coherent_dma_mask_configured,
           stats.nouveau_pci_coherent_dma_mask_requested_bits,
           stats.nouveau_pci_coherent_dma_mask_bits,
           stats.nouveau_pci_coherent_dma_mask_effective_bits,
           stats.nouveau_pci_coherent_dma_mask_fallback_32,
           stats.nouveau_pci_bar0_len,
           stats.nouveau_pci_bar1_len,
           stats.nouveau_pci_bar0_claimed,
           stats.nouveau_pci_bar1_claimed,
           stats.nouveau_pci_bar_claim_failures,
           stats.nouveau_pci_bar_releases,
           stats.nouveau_pci_resource_claims,
           stats.nouveau_pci_resource_releases,
           stats.nouveau_pci_resource_iomaps,
           stats.nouveau_pci_resource_owner_mismatches,
           stats.nouveau_pci_dma_map_api_present,
           stats.nouveau_pci_dma_map_attempts,
           stats.nouveau_pci_dma_map_successes,
           stats.nouveau_pci_dma_map_failures,
           stats.nouveau_pci_dma_unmaps,
           stats.nouveau_pci_dma_map_last_size,
           stats.nouveau_pci_dma_map_last_addr,
           stats.nouveau_pci_dma_map_last_ret,
           nouveau_dma_map_state,
           stats.nouveau_pci_unclaimed_iomaps,
           stats.nouveau_pci_unclaimed_releases,
           stats.nouveau_pci_irq_mode,
           stats.nouveau_pci_irq_request_failures,
           stats.nouveau_pci_msi_requested,
           stats.nouveau_pci_msi_fail_closed,
           stats.nouveau_pci_irq_vector_valid,
           stats.nouveau_pci_irq_alloc_requests,
           stats.nouveau_pci_irq_alloc_failures,
           stats.nouveau_pci_msi_program_attempts,
           stats.nouveau_pci_msi_program_unsupported,
           stats.nouveau_pci_msix_program_attempts,
           stats.nouveau_pci_msix_program_unsupported,
           stats.nouveau_pci_legacy_irq_requests,
           stats.nouveau_pci_legacy_irq_grants,
           stats.nouveau_pci_irq_handler_registered,
           stats.nouveau_pci_irq_delivery_enabled,
           stats.nouveau_pci_irq_delivery_claimed,
           stats.nouveau_pci_legacy_irq_fallback,
           stats.nouveau_pci_irq_handler_invocations,
           stats.nouveau_pci_irq_cause_reads,
           stats.nouveau_pci_irq_cause_valid,
           stats.nouveau_pci_irq_cause_acks,
           stats.nouveau_pci_irq_spurious,
           stats.nouveau_pci_suspend_count,
           stats.nouveau_pci_resume_count,
           stats.nouveau_pci_runtime_pm_balanced,
           stats.nouveau_pci_remove_runtime_suspended,
           stats.nouveau_pci_remove_calls,
           stats.nouveau_pci_remove_runtime_resume_attempts,
           stats.nouveau_pci_remove_runtime_resume_successes,
           stats.nouveau_pci_remove_runtime_barriers,
           stats.nouveau_pci_remove_active_before_callback,
           stats.nouveau_pci_hot_remove_events,
           stats.nouveau_pci_removed,
           stats.nouveau_pci_bar_iounmaps,
           stats.nouveau_pci_irq_unregisters,
           stats.nouveau_pci_irq_vectors_freed,
           stats.nouveau_pci_bus_master_clears,
           stats.nouveau_pci_device_disables,
           stats.nouveau_pci_drvdata_cleared,
           stats.nouveau_pci_native_present_credit);
    printf("gpu_core_c_validator nouveau_pci_runtime_contract_matrix "
           "accepts=%lu gpup_only=%s dma_mask=%s coherent_dma_mask=%s "
           "dma_map=%s bar_claim=%s msi_msix_setup=%s legacy_irq_fallback=%s "
           "irq_handler=%s irq_delivery=%s runtime_pm_usage=%s "
           "remove_path=%s native_present_credit=%lu "
           "opengl_submit_credit=0 status=PENDING\n",
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_probe_accepts == 0 ? "PASS" : "NO",
           nouveau_dma_mask_state,
           nouveau_coherent_dma_mask_state,
           nouveau_dma_map_state,
           stats.nouveau_pci_probe_accepts == 0 ? "NOT_ATTEMPTED" :
               "DIAGNOSTIC",
           stats.nouveau_pci_msi_fail_closed ? "FAIL_CLOSED" :
               "NOT_ATTEMPTED",
           stats.nouveau_pci_probe_accepts == 0 ? "NOT_CLAIMED" :
               "DIAGNOSTIC",
           stats.nouveau_pci_irq_handler_registered ? "PRESENT" : "ABSENT",
           (stats.nouveau_pci_irq_delivery_enabled ||
            stats.nouveau_pci_irq_delivery_claimed) ? "PRESENT" : "ABSENT",
           stats.nouveau_pci_probe_accepts == 0 ? "DEFERRED" :
               "DIAGNOSTIC",
           stats.nouveau_pci_removes ? "DIAGNOSTIC" : "DEFERRED",
           stats.nouveau_pci_native_present_credit);
    printf("gpu_core_c_validator nouveau_pci_runtime_interface_matrix "
           "accepts=%lu resource_tree=%s dma_mapping_api=%s "
           "msi_msix_programming=%s legacy_irq_fallback=%s "
           "irq_delivery=%s runtime_pm=%s remove_path=%s "
           "resource_owner=%s claim_before_iomap=%s "
           "release_balance=%s owner_mismatch=%lu unclaimed_iomap=%lu "
           "unclaimed_release=%lu hot_remove=%s "
           "native_engine=%s native_present_credit=%lu "
           "opengl_submit_credit=0 status=PENDING\n",
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_probe_accepts == 0 ? "GPU_P_FAIL_CLOSED" :
               "DIAGNOSTIC",
           nouveau_dma_map_state,
           stats.nouveau_pci_msi_fail_closed ? "FAIL_CLOSED" :
               "NOT_ATTEMPTED",
           stats.nouveau_pci_probe_accepts == 0 ? "NOT_CLAIMED" :
               "DIAGNOSTIC",
           (stats.nouveau_pci_irq_delivery_enabled ||
            stats.nouveau_pci_irq_delivery_claimed) ? "PRESENT" : "ABSENT",
           stats.nouveau_pci_probe_accepts == 0 ? "DEFERRED" :
               "DIAGNOSTIC",
           stats.nouveau_pci_removes ? "DIAGNOSTIC" : "DEFERRED",
           stats.nouveau_pci_resource_owner_mismatches == 0 ?
               (stats.nouveau_pci_probe_accepts == 0 ?
                    "GPU_P_FAIL_CLOSED" : "PASS") : "FAIL",
           stats.nouveau_pci_unclaimed_iomaps == 0 ?
               (stats.nouveau_pci_probe_accepts == 0 ?
                    "GPU_P_FAIL_CLOSED" : "PASS") : "FAIL",
           stats.nouveau_pci_unclaimed_releases == 0 ?
               (stats.nouveau_pci_probe_accepts == 0 ?
                    "GPU_P_FAIL_CLOSED" : "PASS") : "FAIL",
           stats.nouveau_pci_resource_owner_mismatches,
           stats.nouveau_pci_unclaimed_iomaps,
           stats.nouveau_pci_unclaimed_releases,
           stats.nouveau_pci_probe_accepts == 0 ? "DEFERRED" :
               "DIAGNOSTIC",
           stats.nouveau_pci_probe_accepts == 0 ? "ABSENT" :
               "DIAGNOSTIC",
           stats.nouveau_pci_native_present_credit);
    printf("gpu_core_c_validator nouveau_pci_irq_provenance_matrix "
           "accepts=%lu msi_attempts=%lu msi_unsupported=%lu "
           "msix_attempts=%lu msix_unsupported=%lu "
           "legacy_requests=%lu legacy_grants=%lu "
           "handler_invocations=%lu cause_reads=%lu cause_valid=%lu "
           "cause_acks=%lu spurious=%lu device_cause=%s "
           "native_present_credit=%lu opengl_submit_credit=0 "
           "status=PENDING\n",
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_msi_program_attempts,
           stats.nouveau_pci_msi_program_unsupported,
           stats.nouveau_pci_msix_program_attempts,
           stats.nouveau_pci_msix_program_unsupported,
           stats.nouveau_pci_legacy_irq_requests,
           stats.nouveau_pci_legacy_irq_grants,
           stats.nouveau_pci_irq_handler_invocations,
           stats.nouveau_pci_irq_cause_reads,
           stats.nouveau_pci_irq_cause_valid,
           stats.nouveau_pci_irq_cause_acks,
           stats.nouveau_pci_irq_spurious,
           stats.nouveau_pci_probe_accepts == 0 ? "GPU_P_FAIL_CLOSED" :
               (stats.nouveau_pci_irq_cause_acks != 0 ? "PASS" :
                                                         "DIAGNOSTIC"),
           stats.nouveau_pci_native_present_credit);
    printf("gpu_core_c_validator nouveau_pci_remove_pm_matrix "
           "accepts=%lu remove_calls=%lu runtime_resume_attempts=%lu "
           "runtime_resume_successes=%lu runtime_barriers=%lu "
           "runtime_resume_before_remove=%s remove_while_suspended=%lu "
           "hot_remove_events=%lu removed=%lu bar_iounmaps=%lu "
           "irq_unregisters=%lu irq_vectors_freed=%lu "
           "bus_master_clears=%lu device_disables=%lu "
           "drvdata_cleared=%lu teardown=%s native_present_credit=%lu "
           "opengl_submit_credit=0 status=PENDING\n",
           stats.nouveau_pci_probe_accepts,
           stats.nouveau_pci_remove_calls,
           stats.nouveau_pci_remove_runtime_resume_attempts,
           stats.nouveau_pci_remove_runtime_resume_successes,
           stats.nouveau_pci_remove_runtime_barriers,
           stats.nouveau_pci_probe_accepts == 0 ? "NOT_APPLICABLE" :
               (stats.nouveau_pci_removes == 0 ? "DEFERRED" :
                   (stats.nouveau_pci_remove_runtime_suspended == 0 ?
                        "PASS" : "FAIL")),
           stats.nouveau_pci_remove_runtime_suspended,
           stats.nouveau_pci_hot_remove_events,
           stats.nouveau_pci_removed,
           stats.nouveau_pci_bar_iounmaps,
           stats.nouveau_pci_irq_unregisters,
           stats.nouveau_pci_irq_vectors_freed,
           stats.nouveau_pci_bus_master_clears,
           stats.nouveau_pci_device_disables,
           stats.nouveau_pci_drvdata_cleared,
           stats.nouveau_pci_probe_accepts == 0 ? "GPU_P_FAIL_CLOSED" :
               (stats.nouveau_pci_removes == 0 ? "DEFERRED" :
                   (stats.nouveau_pci_drvdata_cleared != 0 &&
                    stats.nouveau_pci_device_disables != 0 ? "PASS" :
                                                             "FAIL")),
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
    printf("gpu_core_c_validator nouveau_nvif_failclosed_matrix "
           "ioctls=%lu sclass_queries=%lu sclass_count=%lu "
           "new_rejects=%lu del_rejects=%lu unsupported=%lu "
           "status=PENDING\n",
           stats.nouveau_nvif_ioctls,
           stats.nouveau_nvif_sclass_queries,
           stats.nouveau_nvif_sclass_count,
           stats.nouveau_nvif_new_rejects,
           stats.nouveau_nvif_del_rejects,
           stats.nouveau_nvif_unsupported);
    printf("gpu_core_c_validator nouveau_submit_failclosed_matrix "
           "pushbuf_noops=%lu exec_noops=%lu vm_bind_noops=%lu "
           "nonempty_pushbuf_rejects=%lu nonempty_exec_rejects=%lu "
           "nonempty_vm_bind_rejects=%lu native_present_credit=%lu "
           "opengl_submit_credit=0 status=PENDING\n",
           stats.nouveau_pushbuf_noops,
           stats.nouveau_exec_noops,
           stats.nouveau_vm_bind_noops,
           stats.nouveau_nonempty_pushbuf_rejects,
           stats.nouveau_nonempty_exec_rejects,
           stats.nouveau_nonempty_vm_bind_rejects,
           stats.nouveau_pci_native_present_credit);
    printf("gpu_core_c_validator nouveau_gem_mmap_backing_matrix "
           "gem_news=%lu gem_infos=%lu cpu_preps=%lu cpu_finis=%lu "
           "mmap_backing=absent mmap_successes=0 "
           "backing_source=none linux_mmap_credit=0 "
           "native_present_credit=%lu opengl_submit_credit=0 "
           "status=%s\n",
           stats.nouveau_gem_news,
           stats.nouveau_gem_infos,
           stats.nouveau_cpu_preps,
           stats.nouveau_cpu_finis,
           stats.nouveau_pci_native_present_credit,
           stats.nouveau_pci_native_present_credit == 0 ? "PASS" : "FAIL");
    printf("gpu_core_c_validator nouveau_gpuvm_mapping_failclosed_matrix "
           "vm_inits=%lu vm_bind_noops=%lu "
           "nonempty_vm_bind_rejects=%lu mapping_successes=0 "
           "mapping_backend=fail_closed native_present_credit=%lu "
           "opengl_submit_credit=0 status=%s\n",
           stats.nouveau_vm_inits,
           stats.nouveau_vm_bind_noops,
           stats.nouveau_nonempty_vm_bind_rejects,
           stats.nouveau_pci_native_present_credit,
           stats.nouveau_pci_native_present_credit == 0 ? "PASS" : "FAIL");

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
    if (stats.dxg_present_helper_transport_present != 0 ||
        stats.dxg_present_display_target_kind !=
            FB_GPU_DXG_DISPLAY_TARGET_NONE) {
        note_fail("backend", "d3d12_native_completion_claim_without_bind");
        ok = 0;
    }
    if (!native_display_failclosed_ok) {
        note_fail("backend", "native_display_readiness_not_failclosed");
        ok = 0;
    }
    if (!nouveau_display_kms_registration_ok) {
        note_fail("backend", "nouveau_display_kms_registration_not_failclosed");
        ok = 0;
    }
    if (!nouveau_kms_vblank_irq_source_ok) {
        note_fail("backend", "nouveau_kms_vblank_irq_source_not_failclosed");
        ok = 0;
    }
    if (!nouveau_primary_plane_modifier_failclosed_ok) {
        note_fail("backend", "nouveau_primary_plane_modifier_not_failclosed");
        ok = 0;
    }
    if (!nouveau_linux_display_readiness_ok) {
        note_fail("backend", "nouveau_linux_display_readiness_not_proven");
        ok = 0;
    }
    if (!kms_scanout_cpu_convert_separation_ok) {
        note_fail("backend", "kms_scanout_cpu_convert_claimed_native_present");
        ok = 0;
    }
    if (!kms_gem_fb_plane_ref_ok) {
        note_fail("backend", "kms_gem_fb_plane_ref_claimed_native_present");
        ok = 0;
    }
    if (!kms_atomic_plane_state_ok) {
        note_fail("backend", "kms_atomic_plane_state_not_failclosed");
        ok = 0;
    }
    if (!kms_atomic_prepare_cleanup_fb_ok) {
        note_fail("backend", "kms_atomic_prepare_cleanup_fb_not_balanced");
        ok = 0;
    }
    if (!kms_page_flip_feature_gate_ok) {
        note_fail("backend", "kms_page_flip_feature_gate_not_failclosed");
        ok = 0;
    }
    if (!display_bind_id_shape_ok) {
        note_fail("backend", "d3d12_display_bind_id_shape_forged");
        ok = 0;
    }
    if (!provider_credit_gate_ok) {
        note_fail("backend", "d3d12_credit_without_provider_clear");
        ok = 0;
    }
    if (!display_bind_request_metadata_ok) {
        note_fail("backend", "d3d12_display_bind_request_metadata_invalid");
        ok = 0;
    }
    if (!display_bind_pending_lifetime_ok) {
        note_fail("backend", "d3d12_display_bind_pending_lifetime_invalid");
        ok = 0;
    }
    if (!display_bind_generation_revalidation_ok) {
        note_fail("backend",
                  "d3d12_display_bind_generation_revalidation_invalid");
        ok = 0;
    }
    if (!display_bind_provider_pending_publication_ok) {
        note_fail("backend",
                  "d3d12_display_bind_provider_pending_publication_invalid");
        ok = 0;
    }
    if (!display_bind_provider_shared_parent_retention_ok) {
        note_fail("backend",
                  "d3d12_display_bind_provider_shared_parent_invalid");
        ok = 0;
    }
    if (!display_bind_provider_sync_fence_alias_ok) {
        note_fail("backend",
                  "d3d12_display_bind_provider_sync_fence_alias_invalid");
        ok = 0;
    }
    if (!display_bind_provider_packet_lifetime_ok) {
        note_fail("backend",
                  "d3d12_display_bind_provider_packet_lifetime_invalid");
        ok = 0;
    }
    if (!display_bind_provider_no_send_preflight_ok) {
        note_fail("backend",
                  "d3d12_display_bind_provider_no_send_preflight_invalid");
        ok = 0;
    }
    if (!display_bind_success_shape_ok) {
        note_fail("backend", "d3d12_display_bind_success_shape_invalid");
        ok = 0;
    }
    if (!native_completion_lifetime_ok) {
        note_fail("backend", "d3d12_native_completion_lifetime_invalid");
        ok = 0;
    }
    if (!native_completion_consumer_escrow_ok) {
        note_fail("backend", "d3d12_native_completion_consumer_escrow_invalid");
        ok = 0;
    }
    if (!stale_source_zero_credit_ok) {
        note_fail("backend", "d3d12_stale_display_bind_credit");
        ok = 0;
    }
    if (!stale_async_completion_contract_ok) {
        note_fail("backend", "d3d12_stale_async_completion_contract_invalid");
        ok = 0;
    }
    if (!generic_completion_not_native_ok) {
        note_fail("backend", "generic_kms_completion_used_as_native");
        ok = 0;
    }
    if (!standard_alloc_not_display_bind_ok) {
        note_fail("backend", "standard_alloc_used_as_display_bind");
        ok = 0;
    }
    if (!dda_nouveau_separate_display_not_bind_ok) {
        note_fail("backend", "dda_nouveau_display_used_as_d3d12_bind");
        ok = 0;
    }
    if (!foreign_prime_import_gap_ok) {
        note_fail("backend", "foreign_prime_import_gap_claimed_credit");
        ok = 0;
    }
    if (!wsl_uapi_negative_ok) {
        note_fail("backend", "wsl_uapi_display_bind_contract_claimed");
        ok = 0;
    }
    if (!wsl_adapter_display_caps_negative_ok) {
        note_fail("backend", "wsl_adapter_display_caps_claimed");
        ok = 0;
    }
    if (!wsl_submit_present_fields_not_bind_ok) {
        note_fail("backend", "wsl_submit_present_fields_used_as_bind");
        ok = 0;
    }
    if (!wsl_stdalloc_and_alloc_flags_not_bind_ok) {
        note_fail("backend", "wsl_stdalloc_flags_used_as_bind");
        ok = 0;
    }
    if (!wsl_trace_display_bind_negative_ok) {
        note_fail("backend", "wsl_trace_display_bind_claimed");
        ok = 0;
    }
    if (!public_present_api_not_guest_bind_ok) {
        note_fail("backend", "public_present_api_claimed_guest_bind");
        ok = 0;
    }
    if (!provider_credit_gate_negative_ok) {
        note_fail("backend", "provider_credit_gate_not_failclosed");
        ok = 0;
    }
    if (!d3d12_display_bind_host_abi_discovery_ok) {
        note_fail("backend", "display_bind_host_abi_discovery_opened");
        ok = 0;
    }
    if (!d3d12_negative_abi_manifest_ok) {
        note_fail("backend", "d3d12_negative_abi_manifest_invalid");
        ok = 0;
    }
    if (!d3d12_display_bind_authority_chain_ok) {
        note_fail("backend", "d3d12_display_bind_authority_chain_invalid");
        ok = 0;
    }
    if (!dda_nouveau_non_readback_display_proof_ok) {
        note_fail("backend", "dda_nouveau_non_readback_display_not_proven");
        ok = 0;
    }
    if (!dda_nouveau_d3d12_bridge_disjoint_ok) {
        note_fail("backend", "dda_nouveau_d3d12_bridge_claimed");
        ok = 0;
    }
    if (!host_display_bind_source_catalog_ok) {
        note_fail("backend", "host_display_bind_source_catalog_inconsistent");
        ok = 0;
    }
    if (!d3d12_completion_source_authority_ok) {
        note_fail("backend", "d3d12_completion_source_authority_invalid");
        ok = 0;
    }
    if (!native_present_completion_source_namespace_ok) {
        note_fail("backend", "native_present_completion_namespace_mixed");
        ok = 0;
    }
    if (!gpu_remaining_holistic_skeleton_ok) {
        note_fail("backend", "gpu_remaining_holistic_skeleton_opened_early");
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
            stats.nouveau_pci_dma_mask_requested_bits < 32 ||
            stats.nouveau_pci_dma_mask_effective_bits < 32 ||
            stats.nouveau_pci_dma_mask_bits !=
                stats.nouveau_pci_dma_mask_effective_bits ||
            stats.nouveau_pci_coherent_dma_mask_configured == 0 ||
            stats.nouveau_pci_coherent_dma_mask_bits < 32 ||
            stats.nouveau_pci_coherent_dma_mask_requested_bits < 32 ||
            stats.nouveau_pci_coherent_dma_mask_effective_bits < 32 ||
            stats.nouveau_pci_coherent_dma_mask_bits !=
                stats.nouveau_pci_coherent_dma_mask_effective_bits) {
            note_fail("backend", "dda_nouveau_dma_mask_not_configured");
            ok = 0;
        }
        if (stats.nouveau_pci_dma_map_api_present == 0 ||
            stats.nouveau_pci_dma_map_attempts == 0 ||
            stats.nouveau_pci_dma_map_successes == 0 ||
            stats.nouveau_pci_dma_map_failures != 0 ||
            stats.nouveau_pci_dma_unmaps !=
                stats.nouveau_pci_dma_map_successes) {
            note_fail("backend", "dda_nouveau_dma_map_not_validated");
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
        if (stats.nouveau_pci_resource_owner_mismatches != 0 ||
            stats.nouveau_pci_unclaimed_iomaps != 0 ||
            stats.nouveau_pci_unclaimed_releases != 0) {
            note_fail("backend", "dda_nouveau_resource_ownership_broken");
            ok = 0;
        }
        if (stats.nouveau_pci_runtime_suspended != 0 ||
            stats.nouveau_pci_runtime_pm_balanced == 0 ||
            stats.nouveau_pci_suspend_count !=
                stats.nouveau_pci_resume_count) {
            note_fail("backend", "dda_nouveau_runtime_pm_unbalanced");
            ok = 0;
        }
        if (stats.nouveau_pci_irq_vector_valid == 0 ||
            stats.nouveau_pci_irq_handler_registered == 0 ||
            stats.nouveau_pci_irq_delivery_enabled == 0) {
            note_fail("backend", "dda_nouveau_irq_path_not_armed");
            ok = 0;
        }
        if (stats.nouveau_pci_irq_delivery_claimed != 0 &&
            (stats.nouveau_pci_irq_handler_registered == 0 ||
             stats.nouveau_pci_irq_delivery_enabled == 0 ||
             stats.nouveau_pci_irq_cause_valid == 0 ||
             stats.nouveau_pci_irq_cause_acks == 0)) {
            note_fail("backend", "dda_nouveau_irq_delivery_without_cause_ack");
            ok = 0;
        }
        if (stats.nouveau_pci_remove_runtime_resume_successes >
                stats.nouveau_pci_remove_runtime_resume_attempts ||
            stats.nouveau_pci_remove_runtime_resume_attempts >
                stats.nouveau_pci_remove_runtime_barriers) {
            note_fail("backend", "dda_nouveau_remove_pm_order_broken");
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
        if (nouveau_native_display_claimed && !nouveau_display_kms_ready) {
            note_fail("backend", "dda_nouveau_native_display_gates_missing");
            ok = 0;
        }
    } else {
        if ((backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) != 0) {
            note_fail("backend", "dda_backend_flag_without_accept");
            ok = 0;
        }
        if (stats.nouveau_pci_probe_reject_dxg_present == 0 &&
            stats.nouveau_pci_probe_reject_no_bars == 0 &&
            stats.nouveau_pci_probes != 0) {
            note_fail("backend", "gpu_p_fail_closed_reason_missing");
            ok = 0;
        }
        if (stats.dxg_present_dda_nouveau_present != 0) {
            note_fail("backend", "dda_present_without_accept");
            ok = 0;
        }
        if (stats.nouveau_pci_dma_mask_configured != 0 ||
            stats.nouveau_pci_dma_mask_requested_bits != 0 ||
            stats.nouveau_pci_dma_mask_effective_bits != 0 ||
            stats.nouveau_pci_dma_mask_fallback_32 != 0 ||
            stats.nouveau_pci_coherent_dma_mask_configured != 0 ||
            stats.nouveau_pci_coherent_dma_mask_requested_bits != 0 ||
            stats.nouveau_pci_coherent_dma_mask_effective_bits != 0 ||
            stats.nouveau_pci_coherent_dma_mask_fallback_32 != 0 ||
            stats.nouveau_pci_bar0_claimed != 0 ||
            stats.nouveau_pci_bar1_claimed != 0 ||
            stats.nouveau_pci_irq_vector_valid != 0 ||
            stats.nouveau_pci_irq_alloc_requests != 0 ||
            stats.nouveau_pci_irq_alloc_failures != 0 ||
            stats.nouveau_pci_msi_program_attempts != 0 ||
            stats.nouveau_pci_msi_program_unsupported != 0 ||
            stats.nouveau_pci_msix_program_attempts != 0 ||
            stats.nouveau_pci_msix_program_unsupported != 0 ||
            stats.nouveau_pci_legacy_irq_requests != 0 ||
            stats.nouveau_pci_legacy_irq_grants != 0 ||
            stats.nouveau_pci_irq_handler_registered != 0 ||
            stats.nouveau_pci_irq_delivery_enabled != 0 ||
            stats.nouveau_pci_irq_delivery_claimed != 0 ||
            stats.nouveau_pci_irq_handler_invocations != 0 ||
            stats.nouveau_pci_irq_cause_reads != 0 ||
            stats.nouveau_pci_irq_cause_valid != 0 ||
            stats.nouveau_pci_irq_cause_acks != 0 ||
            stats.nouveau_pci_irq_spurious != 0 ||
            stats.nouveau_pci_legacy_irq_fallback != 0 ||
            stats.nouveau_pci_resource_owner_mismatches != 0 ||
            stats.nouveau_pci_unclaimed_iomaps != 0 ||
            stats.nouveau_pci_unclaimed_releases != 0 ||
            stats.nouveau_pci_dma_map_attempts != 0 ||
            stats.nouveau_pci_dma_map_successes != 0 ||
            stats.nouveau_pci_dma_map_failures != 0 ||
            stats.nouveau_pci_dma_unmaps != 0 ||
            stats.nouveau_pci_dma_map_last_size != 0 ||
            stats.nouveau_pci_dma_map_last_addr != 0 ||
            stats.nouveau_pci_dma_map_last_ret != 0 ||
            stats.nouveau_pci_suspend_count != 0 ||
            stats.nouveau_pci_resume_count != 0 ||
            stats.nouveau_pci_runtime_suspended != 0 ||
            stats.nouveau_pci_remove_calls != 0 ||
            stats.nouveau_pci_remove_runtime_resume_attempts != 0 ||
            stats.nouveau_pci_remove_runtime_resume_successes != 0 ||
            stats.nouveau_pci_remove_runtime_barriers != 0 ||
            stats.nouveau_pci_remove_active_before_callback != 0 ||
            stats.nouveau_pci_hot_remove_events != 0 ||
            stats.nouveau_pci_removed != 0 ||
            stats.nouveau_pci_bar_iounmaps != 0 ||
            stats.nouveau_pci_irq_unregisters != 0 ||
            stats.nouveau_pci_irq_vectors_freed != 0 ||
            stats.nouveau_pci_bus_master_clears != 0 ||
            stats.nouveau_pci_device_disables != 0 ||
            stats.nouveau_pci_drvdata_cleared != 0) {
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
        if (stats.nouveau_display_probe_attempts != 0 ||
            stats.nouveau_display_create_attempts != 0 ||
            stats.nouveau_display_create_successes != 0 ||
            stats.nouveau_display_create_fail_closed != 0 ||
            stats.nouveau_display_create_fail_reason != 0 ||
            stats.nouveau_display_head_probe_attempts != 0 ||
            stats.nouveau_display_heads != 0 ||
            stats.nouveau_display_connector_probe_attempts != 0 ||
            stats.nouveau_display_connectors != 0 ||
            stats.nouveau_display_nonvirtual_connectors != 0 ||
            stats.nouveau_display_engine_object_created != 0 ||
            stats.nouveau_display_mode_config_ready != 0 ||
            stats.nouveau_display_crtc_count != 0 ||
            stats.nouveau_display_encoder_count != 0 ||
            stats.nouveau_display_primary_plane_count != 0 ||
            stats.nouveau_display_primary_plane_linear_required == 0 ||
            stats.nouveau_display_primary_plane_nonlinear_modifiers != 0 ||
            stats.nouveau_display_outp_mask_seen != 0 ||
            stats.nouveau_display_conn_mask_seen != 0 ||
            stats.nouveau_display_head_mask_seen != 0 ||
            stats.nouveau_display_nvif_head_ctor_successes != 0 ||
            stats.nouveau_display_hpd_event_registered != 0 ||
            stats.nouveau_display_dp_irq_event_registered != 0 ||
            stats.nouveau_display_vblank_supported != 0 ||
            stats.nouveau_display_vblank_irq_supported != 0 ||
            stats.nouveau_display_vblank_event_registered != 0 ||
            stats.nouveau_display_atomic_commit_tail_ready != 0 ||
            stats.nouveau_display_page_flip_event_source !=
                FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE ||
            stats.nouveau_display_vblank_source !=
                FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_NONE ||
            stats.nouveau_display_vblank_irqs != 0 ||
            stats.nouveau_display_page_flip_completion_ready != 0 ||
            stats.nouveau_display_page_flip_completions != 0 ||
            stats.nouveau_display_atomic_pageflip_backend_missing != 0) {
            note_fail("backend", "nouveau_display_probe_without_accept");
            ok = 0;
        }
    }
    if (!nouveau_atomic_pageflip_backend_missing_ok) {
        note_fail("backend", "nouveau_atomic_pageflip_backend_missing_policy");
        ok = 0;
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
    if (stats.nouveau_pci_irq_cause_valid >
            stats.nouveau_pci_irq_cause_reads ||
        stats.nouveau_pci_irq_cause_acks >
            stats.nouveau_pci_irq_cause_valid ||
        stats.nouveau_pci_irq_cause_reads >
            stats.nouveau_pci_irq_handler_invocations ||
        (stats.nouveau_pci_irq_delivery_claimed != 0 &&
         (stats.nouveau_pci_irq_cause_valid == 0 ||
          stats.nouveau_pci_irq_cause_acks == 0))) {
        note_fail("backend", "nouveau_pci_irq_provenance_inconsistent");
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
    if (stats.ttm_native_accel_credit != 0) {
        note_fail("backend", "ttm_real_move_fabricated_native_accel");
        ok = 0;
    }
    if (stats.nouveau_channel_active != 0) {
        note_fail("backend", "nouveau_channel_lifetime_leak");
        ok = 0;
    }
    if (stats.nouveau_nvif_sclass_count != 0) {
        note_fail("backend", "nouveau_nvif_fabricated_classes");
        ok = 0;
    }
    if (stats.dxg_present_dda_nouveau_import_path_present != 0 ||
        stats.dxg_present_dda_nouveau_scanout_bind_present != 0) {
        note_fail("backend", "dda_d3d12_present_path_fabricated");
        ok = 0;
    }
    if (stats.dxg_scanout_bind_successes != 0 ||
        stats.dxg_scanout_bind_completion_successes != 0 ||
        stats.dxg_scanout_bind_last_present_id != 0 ||
        stats.dxg_scanout_bind_last_completed != 0 ||
        strcmp(dxg_scanout_bind_state, "PASS") != 0) {
        note_fail("backend", "dxg_scanout_bind_fabricated_native_present");
        ok = 0;
    }
    if (stats.dxg_present_dxg_adapter_type_wsl != 0 &&
        stats.dxg_present_dxg_adapter_display_supported != 0) {
        note_fail("backend", "wsl_dxg_display_bit_not_suppressed");
        ok = 0;
    }
    if (ok) {
        printf("gpu_core_c_validator nouveau_pci_dma_resource_matrix "
               "bar_claim=PASS dma_mask=%s dma_map=%s "
               "irq_diagnostics=PASS "
               "resource_owner=PASS claim_before_iomap=PASS "
               "release_balance=PASS owner_mismatch=0 "
               "unclaimed_iomap=0 unclaimed_release=0 "
               "irq_handler_registered=%lu irq_delivery_enabled=%lu "
               "irq_delivery_claimed=%lu "
               "runtime_pm=PASS "
               "native_present_credit=0 status=PASS\n",
               nouveau_dma_mask_state,
               nouveau_dma_map_state,
               stats.nouveau_pci_irq_handler_registered,
               stats.nouveau_pci_irq_delivery_enabled,
               stats.nouveau_pci_irq_delivery_claimed);
        printf("gpu_core_c_validator "
               "d3d12_native_completion_zero_credit_matrix "
               "display_bind=ABSENT transport_present=0 "
               "completion_source=required present_id=0 completed=0 "
               "callback_release_order=blocked "
               "per_client_generation=required id_shape=PASS "
               "provider_credit_gate=PASS native_present_credit=0 "
               "opengl_submit_credit=0 status=PASS\n");
        printf("gpu_core_c_validator "
               "d3d12_display_bind_id_shape_matrix "
               "bind_present_id=%lu bind_completed_id=%lu "
               "bind_source_generation=%lu bind_resource_generation=%lu "
               "scanout_present_id=%lu scanout_completed_id=%lu "
               "scanout_source_generation=%lu "
               "scanout_resource_generation=%lu "
               "zero_ids_required_when_failclosed=1 "
               "completed_ge_present_if_nonzero=1 stale_id_rejected=1 "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=PASS\n",
               stats.dxg_display_bind_present_id,
               stats.dxg_display_bind_completed_id,
               stats.dxg_display_bind_source_generation,
               stats.dxg_display_bind_resource_generation,
               stats.dxg_scanout_bind_last_present_id,
               stats.dxg_scanout_bind_last_completed,
               stats.dxg_scanout_bind_last_source_generation,
               stats.dxg_scanout_bind_last_resource_generation);
        printf("gpu_core_c_validator "
               "d3d12_provider_credit_gate_matrix "
               "provider_submits=%lu provider_no_host_abi=%lu "
               "provider_no_sender=%lu provider_no_completion=%lu "
               "transport_present=%lu display_target_kind=%lu "
               "scanout_successes=%lu completion_successes=%lu "
               "native_present_credit=%lu backend_opengl_submit=%u "
               "credit_requires_provider_clear=1 status=PASS\n",
               stats.dxg_display_bind_provider_submits,
               stats.dxg_display_bind_provider_no_host_abi,
               stats.dxg_display_bind_provider_no_sender,
               stats.dxg_display_bind_provider_no_completion,
               stats.dxg_present_helper_transport_present,
               stats.dxg_present_display_target_kind,
               stats.dxg_scanout_bind_successes,
               stats.dxg_scanout_bind_completion_successes,
               stats.nouveau_pci_native_present_credit,
               backend_opengl_submit);
        printf("gpu_core_c_validator "
               "opengl_submit_backend_separation_matrix "
               "backend=%u dxg_transport=1 d3dkmt=1 virgl_opengl=0 "
               "backend_opengl_submit=0 allowed_submit_backend=virgl "
               "hyperv_dxg_transport_is_submit=0 "
               "hyperv_d3dkmt_is_submit=0 kvm_virgl_submit_allowed=1 "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=PASS\n",
               backend.backend);
        printf("gpu_core_c_validator "
               "wsl_dxg_uapi_namespace_negative_matrix "
               "uapi_namespace_checked=1 ioctl_namespace=linux_dxgkrnl "
               "last_known_ioctl_nr=0x49 checked_range=0x00-0x49 "
               "display_bind_ioctl_present=0 present_source_ioctl_present=0 "
               "present_completion_ioctl_present=0 "
               "out_of_namespace_native_present_ioctl=0 "
               "linux_ioctl_contracts=%lu "
               "resource_bind_contracts=%lu "
               "display_completion_contracts=%lu transport_present=%lu "
               "present_id=%lu completed=%lu native_present_credit=0 "
               "opengl_submit_credit=0 status=PASS\n",
               stats.dxg_scanout_bind_candidate_linux_ioctl_contracts,
               stats.dxg_scanout_bind_candidate_resource_bind_contracts,
               stats.dxg_scanout_bind_candidate_display_completion_contracts,
               stats.dxg_display_bind_transport_present,
               stats.dxg_display_bind_present_id,
               stats.dxg_display_bind_completed_id);
        printf("gpu_core_c_validator "
               "wsl_dxg_adapter_display_caps_negative_matrix "
               "display_supported=%lu post_device=0 "
               "indirect_display_device=0 display_sources=%lu "
               "display_sources_known=%lu display_caps_cleared_by_wsl=1 "
               "display_bind_transport_present=%lu present_id=%lu "
               "completed=%lu native_present_credit=0 "
               "opengl_submit_credit=0 status=PASS\n",
               stats.dxg_present_dxg_adapter_display_supported,
               stats.dxg_present_dxg_adapter_sources,
               stats.dxg_present_dxg_adapter_sources_known,
               stats.dxg_present_helper_transport_present,
               stats.dxg_display_bind_present_id,
               stats.dxg_display_bind_completed_id);
        printf("gpu_core_c_validator "
               "dda_nouveau_non_readback_display_proof_matrix "
               "dda_pci_transport_present=%s "
               "dda_nouveau_display_present=%s "
               "dda_nouveau_non_readback_present=%s "
               "display_probe_attempts=%lu display_create_successes=%lu "
               "display_engine_object=%lu mode_config_ready=%lu "
               "head_probe_attempts=%lu heads=%lu "
               "connector_probe_attempts=%lu connectors=%lu "
               "crtcs=%lu encoders=%lu primary_planes=%lu "
               "nonvirtual_connectors=%lu outp_mask_seen=%lu "
               "conn_mask_seen=%lu head_mask_seen=%lu "
               "vblank_supported=%lu vblank_event=%lu "
               "vblank_irq_supported=%lu vblank_source=%s "
               "page_flip_ready=%lu page_flip_completions=%lu "
               "page_flip_event_source=%s atomic_commit_tail=%lu "
               "atomic_backend_missing=%lu "
               "kms_lane=%lu kms_present_dumb=%lu "
               "kms_present_synthvid=%lu kms_present_nouveau_hw=%lu "
               "kms_vblank_source_nouveau_hw=%lu "
               "kms_vblank_source_software_display=%lu "
               "kms_vblank_source_synthetic=%lu "
               "page_flip_events_software_blit=%lu "
               "page_flip_events_native_hw=%lu "
               "dda_native_display_credit=%lu d3d12_native_present_credit=0 "
               "native_present_credit=0 opengl_submit_credit=0 status=PASS\n",
               stats.nouveau_pci_probe_accepts != 0 ? "PASS" :
                                                       "GPU_P_FAIL_CLOSED",
               nouveau_display_kms_registered ? "PASS" : "ABSENT",
               stats.kms_present_last_lane ==
                           FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW &&
                       stats.kms_page_flip_events_native_hw != 0 &&
                       nouveau_display_kms_ready ?
                   "PASS" : "ABSENT",
               stats.nouveau_display_probe_attempts,
               stats.nouveau_display_create_successes,
               stats.nouveau_display_engine_object_created,
               stats.nouveau_display_mode_config_ready,
               stats.nouveau_display_head_probe_attempts,
               stats.nouveau_display_heads,
               stats.nouveau_display_connector_probe_attempts,
               stats.nouveau_display_connectors,
               stats.nouveau_display_crtc_count,
               stats.nouveau_display_encoder_count,
               stats.nouveau_display_primary_plane_count,
               stats.nouveau_display_nonvirtual_connectors,
               stats.nouveau_display_outp_mask_seen,
               stats.nouveau_display_conn_mask_seen,
               stats.nouveau_display_head_mask_seen,
               stats.nouveau_display_vblank_supported,
               stats.nouveau_display_vblank_event_registered,
               stats.nouveau_display_vblank_irq_supported,
               stats.nouveau_display_vblank_source ==
                       FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
                   "none",
               stats.nouveau_display_page_flip_completion_ready,
               stats.nouveau_display_page_flip_completions,
               stats.nouveau_display_page_flip_event_source ==
                       FB_GPU_NOUVEAU_DISPLAY_VBLANK_SOURCE_IRQ ? "irq" :
                   "none",
               stats.nouveau_display_atomic_commit_tail_ready,
               stats.nouveau_display_atomic_pageflip_backend_missing,
               stats.kms_present_last_lane,
               stats.kms_present_dumb,
               stats.kms_present_synthvid,
               stats.kms_present_nouveau_hw,
               stats.kms_vblank_source_nouveau_hw,
               stats.kms_vblank_source_software_display,
               stats.kms_vblank_source_synthetic,
               stats.kms_page_flip_events_software_blit,
               stats.kms_page_flip_events_native_hw,
               stats.nouveau_pci_native_present_credit);
        printf("gpu_core_c_validator "
               "dxg_scanout_bind_skeleton_matrix "
               "attempts=%lu rejects=%lu successes=%lu "
               "completion_queries=%lu completion_successes=%lu "
               "completion_pending=%lu weak_evidence_rejects=%lu "
               "transport=%lu status_code=%lu present_id=%lu completed=%lu "
               "source_generation=%lu resource_generation=%lu "
               "dirty_sequence=%lu dirty_rects=%lu native_present_credit=0 "
               "opengl_submit_credit=0 status=%s\n",
               stats.dxg_scanout_bind_attempts,
               stats.dxg_scanout_bind_rejects,
               stats.dxg_scanout_bind_successes,
               stats.dxg_scanout_bind_completion_queries,
               stats.dxg_scanout_bind_completion_successes,
               stats.dxg_scanout_bind_completion_pending,
               stats.dxg_scanout_bind_weak_evidence_rejects,
               stats.dxg_scanout_bind_last_transport,
               stats.dxg_scanout_bind_last_status,
               stats.dxg_scanout_bind_last_present_id,
               stats.dxg_scanout_bind_last_completed,
               stats.dxg_scanout_bind_last_source_generation,
               stats.dxg_scanout_bind_last_resource_generation,
               stats.dxg_scanout_bind_last_dirty_sequence,
               stats.dxg_scanout_bind_last_dirty_rects,
               dxg_scanout_bind_state);
        printf("gpu_core_c_validator "
               "dxg_scanout_bind_candidate_command_matrix "
               "presenthistory_cmd=%lu redirected_flip_fence_cmd=%lu "
               "blt_cmd=%lu propagate_presenthistory_cmd=%lu "
               "cmds_known=%lu sender_contracts=%lu "
               "completion_contracts=%lu candidate_rejects=%lu "
               "custom_host_tool=0 transport_present=%lu present_id=0 "
               "vmbus_enum_known=%lu linux_ioctl_contracts=%lu "
               "resource_bind_contracts=%lu "
               "display_completion_contracts=%lu reject_reasons=0x%lx "
               "completed=0 native_present_credit=0 "
               "opengl_submit_credit=0 status=%s\n",
               stats.dxg_scanout_bind_candidate_presenthistory_cmd,
               stats.dxg_scanout_bind_candidate_redirected_flip_fence_cmd,
               stats.dxg_scanout_bind_candidate_blt_cmd,
               stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd,
               stats.dxg_scanout_bind_candidate_cmds_known,
               stats.dxg_scanout_bind_candidate_sender_contracts,
               stats.dxg_scanout_bind_candidate_completion_contracts,
               stats.dxg_scanout_bind_candidate_rejects,
               stats.dxg_present_helper_transport_present,
               stats.dxg_scanout_bind_candidate_vmbus_enum_known,
               stats.dxg_scanout_bind_candidate_linux_ioctl_contracts,
               stats.dxg_scanout_bind_candidate_resource_bind_contracts,
               stats.dxg_scanout_bind_candidate_display_completion_contracts,
               stats.dxg_scanout_bind_candidate_reject_reasons,
               stats.dxg_scanout_bind_candidate_cmds_known == 4 &&
                       stats.dxg_scanout_bind_candidate_presenthistory_cmd == 34 &&
                       stats.dxg_scanout_bind_candidate_redirected_flip_fence_cmd == 35 &&
                       stats.dxg_scanout_bind_candidate_blt_cmd == 38 &&
                       stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd == 1 &&
                       stats.dxg_scanout_bind_candidate_vmbus_enum_known == 1 &&
                       stats.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_reject_reasons ==
                           FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_ALL &&
                       stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                       stats.dxg_present_helper_transport_present == 0 &&
                       stats.dxg_scanout_bind_last_present_id == 0 &&
                       stats.dxg_scanout_bind_last_completed == 0 ?
                   "PASS" : "FAIL");
        printf("gpu_core_c_validator "
               "dxg_presenthistory_telemetry_not_completion_matrix "
               "presenthistory_cmd=%lu propagate_presenthistory_cmd=%lu "
               "linux_inband_handler=absent sender_contracts=%lu "
               "completion_contracts=%lu completion_successes=%lu "
               "display_bind_present_id=%lu display_bind_completed=%lu "
               "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
               stats.dxg_scanout_bind_candidate_presenthistory_cmd,
               stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd,
               stats.dxg_scanout_bind_candidate_sender_contracts,
               stats.dxg_scanout_bind_candidate_completion_contracts,
               stats.dxg_scanout_bind_completion_successes,
               stats.dxg_display_bind_present_id,
               stats.dxg_display_bind_completed_id,
               stats.dxg_scanout_bind_candidate_presenthistory_cmd == 34 &&
                       stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd == 1 &&
                       stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                       stats.dxg_scanout_bind_completion_successes == 0 &&
                       stats.dxg_display_bind_present_id == 0 &&
                       stats.dxg_display_bind_completed_id == 0 ?
                   "PASS" : "FAIL");
        printf("gpu_core_c_validator "
               "dxg_presenthistory_orphan_completion_rejection_matrix "
               "propagate_presenthistory_cmd=%lu presenthistory_packets=0 "
               "provider_pending_match=0 completion_demux_registered=%lu "
               "provider_resolve_delta=0 completion_successes=%lu "
               "display_bind_present_id=%lu display_bind_completed_id=%lu "
               "orphan_completion_rejected=1 native_present_credit=0 "
               "opengl_submit_credit=0 status=%s\n",
               stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd,
               stats.dxg_display_bind_provider_completion_demux_registered,
               stats.dxg_scanout_bind_completion_successes,
               stats.dxg_display_bind_present_id,
               stats.dxg_display_bind_completed_id,
               stats.dxg_scanout_bind_candidate_propagate_presenthistory_cmd == 1 &&
                       stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                       stats.dxg_display_bind_provider_completion_demux_registered == 0 &&
                       stats.dxg_scanout_bind_completion_successes == 0 &&
                       stats.dxg_display_bind_present_id == 0 &&
                       stats.dxg_display_bind_completed_id == 0 ?
                   "PASS" : "FAIL");
        printf("gpu_core_c_validator "
               "dxg_native_present_lane_rejection_matrix "
               "wsl_presenthistory_enum_only=REJECTED "
               "wsl_presenthistory_sender_contract=%lu "
               "wsl_presenthistory_completion_contract=%lu "
               "synthvid_gpa_dirty_only=REJECTED "
               "linux_hyperv_drm_shadow_blit_only=REJECTED "
               "synthvid_gpa_dirty_present=%lu "
               "synthvid_d3d12_resource_bind=0 "
               "dda_nouveau_separate_pci_path=%s "
               "dda_pci_display_present=%lu "
               "dda_d3d12_resource_import=%lu dda_scanout_bind=%lu "
               "dda_hw_flip_completion=0 "
               "vmbus_enum_known=%lu linux_ioctl_contracts=%lu "
               "resource_bind_contracts=%lu "
               "display_completion_contracts=%lu reject_reasons=0x%lx "
               "custom_host_tool=0 transport_present=%lu present_id=0 "
               "completed=0 native_present_credit=0 "
               "opengl_submit_credit=0 status=%s\n",
               stats.dxg_scanout_bind_candidate_sender_contracts,
               stats.dxg_scanout_bind_candidate_completion_contracts,
               stats.dxg_scanout_bind_synthvid_gpa_dirty_present,
               stats.dxg_present_dda_nouveau_present != 0 ?
                   "REJECTED_NO_IMPORT_PATH" : "ABSENT",
               stats.dxg_scanout_bind_dda_pci_display_present,
               stats.dxg_present_dda_nouveau_import_path_present,
               stats.dxg_present_dda_nouveau_scanout_bind_present,
               stats.dxg_scanout_bind_candidate_vmbus_enum_known,
               stats.dxg_scanout_bind_candidate_linux_ioctl_contracts,
               stats.dxg_scanout_bind_candidate_resource_bind_contracts,
               stats.dxg_scanout_bind_candidate_display_completion_contracts,
               stats.dxg_scanout_bind_candidate_reject_reasons,
               stats.dxg_present_helper_transport_present,
               stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_vmbus_enum_known == 1 &&
                       stats.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
                       stats.dxg_scanout_bind_synthvid_resource_bind_absent != 0 &&
                       stats.dxg_scanout_bind_dda_resource_import_absent != 0 &&
                       stats.dxg_scanout_bind_dda_scanout_bind_absent != 0 &&
                       stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
                       stats.dxg_scanout_bind_candidate_reject_reasons ==
                           FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_ALL &&
                       stats.dxg_present_helper_transport_present == 0 &&
                       stats.dxg_scanout_bind_last_present_id == 0 &&
                       stats.dxg_scanout_bind_last_completed == 0 &&
                       stats.dxg_present_dda_nouveau_import_path_present == 0 &&
                       stats.dxg_present_dda_nouveau_scanout_bind_present == 0 ?
                   "PASS" : "FAIL");
        printf("gpu_core_c_validator "
               "d3d12_dda_nouveau_separate_display_not_bind_matrix "
               "dda_backend_flag=%u dda_pci_display_present=%lu "
               "dda_d3d12_resource_import=%lu dda_scanout_bind=%lu "
               "dda_hw_flip_completion=%s separate_pci_display_path=%s "
               "display_bind_present_id=%lu display_bind_completed=%lu "
               "scanout_bind_successes=%lu completion_successes=%lu "
               "native_present_credit=%lu opengl_submit_credit=%d "
               "status=%s\n",
               (backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) != 0,
               stats.dxg_scanout_bind_dda_pci_display_present,
               stats.dxg_present_dda_nouveau_import_path_present,
               stats.dxg_present_dda_nouveau_scanout_bind_present,
               stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
                   "ABSENT" : "PRESENT",
               stats.dxg_scanout_bind_dda_pci_display_present != 0 ?
                   "REJECTED_D3D12_IMPORT_MISSING" : "ABSENT",
               stats.dxg_display_bind_present_id,
               stats.dxg_display_bind_completed_id,
               stats.dxg_scanout_bind_successes,
               stats.dxg_scanout_bind_completion_successes,
               stats.nouveau_pci_native_present_credit,
               backend_opengl_submit,
               dda_nouveau_separate_display_not_bind_ok ? "PASS" :
                   "FAIL");
        printf("gpu_core_c_validator foreign_prime_import_gap_matrix "
               "attempts=%lu local_imports=%lu accepted_imports=%lu "
               "foreign_attempts=%lu foreign_rejects=%lu "
               "legacy_foreign_fd_rejects=%lu local_only_import_path=%lu "
               "d3d12_foreign_resource_imports=%lu "
               "nouveau_scanout_bind_imports=%lu "
               "dmabuf_native_present_credit=%lu "
               "dxg_dda_import_path=%lu dxg_dda_scanout_bind=%lu "
               "scanout_bind_successes=%lu completion_successes=%lu "
               "native_present_credit=%lu opengl_submit_credit=%d "
               "status=%s\n",
               stats.dmabuf_import_attempts,
               stats.dmabuf_local_imports,
               stats.dmabuf_imports,
               stats.dmabuf_foreign_import_attempts,
               stats.dmabuf_foreign_import_rejects,
               stats.dmabuf_foreign_fd_rejects,
               stats.dmabuf_local_only_import_path != 0 ||
                   stats.dmabuf_import_attempts == 0 ? 1UL : 0UL,
               stats.dmabuf_d3d12_foreign_resource_imports,
               stats.dmabuf_nouveau_scanout_bind_imports,
               stats.dmabuf_native_present_credit,
               stats.dxg_present_dda_nouveau_import_path_present,
               stats.dxg_present_dda_nouveau_scanout_bind_present,
               stats.dxg_scanout_bind_successes,
               stats.dxg_scanout_bind_completion_successes,
               stats.nouveau_pci_native_present_credit,
               backend_opengl_submit,
               foreign_prime_import_gap_ok ? "PASS" : "FAIL");
        printf("gpu_core_c_validator "
               "dxg_scanout_bind_weak_evidence_matrix "
               "dxg_ready_only=%lu d3dkmt_handles_only=%lu "
               "same_adapter_resource_only=%lu syncfile_only=%lu "
               "synthvid_gpa_dirty_only=%lu software_or_readback_path=%lu "
               "weak_evidence_rejects=%lu successes=%lu present_id=0 "
               "completed=0 native_present_credit=0 "
               "opengl_submit_credit=0 status=%s\n",
               stats.dxg_scanout_bind_weak_dxg_ready_only,
               stats.dxg_scanout_bind_weak_d3dkmt_handles_only,
               stats.dxg_scanout_bind_weak_same_adapter_resource_only,
               stats.dxg_scanout_bind_weak_syncfile_only,
               stats.dxg_scanout_bind_weak_synthvid_gpa_dirty_only,
               stats.dxg_scanout_bind_weak_software_or_readback_path,
               stats.dxg_scanout_bind_weak_evidence_rejects,
               stats.dxg_scanout_bind_successes,
               (stats.dxg_scanout_bind_weak_evidence_rejects > 0 ||
                stats.dxg_scanout_bind_attempts == 0) &&
                       stats.dxg_scanout_bind_successes == 0 &&
                       stats.dxg_scanout_bind_last_present_id == 0 &&
                       stats.dxg_scanout_bind_last_completed == 0 ?
                   "PASS" : "FAIL");
        printf("gpu_core_c_validator "
               "dxg_syncfile_not_kms_completion_matrix "
               "syncfile_only=%lu weak_evidence_rejects=%lu "
               "scanout_successes=%lu completion_successes=%lu "
               "completion_pending=%lu kms_no_hw_completion=%lu "
               "present_id=%lu completed=%lu native_present_credit=0 "
               "opengl_submit_credit=0 status=%s\n",
               stats.dxg_scanout_bind_weak_syncfile_only,
               stats.dxg_scanout_bind_weak_evidence_rejects,
               stats.dxg_scanout_bind_successes,
               stats.dxg_scanout_bind_completion_successes,
               stats.dxg_scanout_bind_completion_pending,
               stats.kms_present_reject_no_hw_completion,
               stats.dxg_scanout_bind_last_present_id,
               stats.dxg_scanout_bind_last_completed,
               stats.dxg_scanout_bind_successes == 0 &&
                       stats.dxg_scanout_bind_completion_successes == 0 &&
                       stats.dxg_scanout_bind_last_present_id == 0 &&
                       stats.dxg_scanout_bind_last_completed == 0 ?
                   "PASS" : "FAIL");
        printf("gpu_core_c_validator "
               "d3d12_native_completion_not_kms_matrix "
               "generic_display_last_complete=%lu "
               "kms_vblank_display_correlated=%lu "
               "kms_vblank_source_software_display=%lu "
               "kms_vblank_source_native_hw=%lu "
               "kms_atomic_out_fence_display_correlated=%lu "
               "kms_atomic_out_fence_software_scanout_correlated=%lu "
               "kms_page_flip_events=%lu page_flip_events_software_blit=%lu "
               "page_flip_events_native_hw=%lu display_wait_is_native=0 "
               "kms_generic_display_credit=0 native_present_credit=0 "
               "opengl_submit_credit=0 status=%s\n",
               stats.display_last_complete,
               stats.kms_vblank_display_correlated,
               stats.kms_vblank_source_software_display,
               stats.kms_vblank_source_nouveau_hw,
               stats.kms_atomic_out_fence_display_correlated,
               stats.kms_atomic_out_fence_software_scanout_correlated,
               stats.kms_vblank_page_flip_events,
               stats.kms_page_flip_events_software_blit,
               stats.kms_page_flip_events_native_hw,
               stats.dxg_scanout_bind_successes == 0 &&
                       stats.dxg_scanout_bind_completion_successes == 0 &&
                       stats.kms_vblank_source_nouveau_hw == 0 &&
                       stats.kms_page_flip_events_native_hw == 0 ?
                   "PASS" : "FAIL");
        printf("gpu_core_c_validator "
               "wsl_standard_alloc_not_display_bind_matrix "
               "standard_alloc_private_data=%lu "
               "standard_alloc_display_bind_absent=%lu "
               "standard_alloc_role=private_driver_data "
               "standard_alloc_native_present_credit=0 "
               "display_bind_transport_present=%lu present_id=%lu "
               "completed=%lu native_present_credit=0 "
               "opengl_submit_credit=0 status=PASS\n",
               stats.dxg_scanout_bind_standard_alloc_private_data,
               stats.dxg_scanout_bind_standard_alloc_display_bind_absent,
               stats.dxg_display_bind_transport_present,
               stats.dxg_display_bind_present_id,
               stats.dxg_display_bind_completed_id);
        printf("gpu_core_c_validator "
               "gpu_remaining_plan_dependency_skeleton_matrix "
               "root_display_bind_gate=closed native_present_gate=closed "
               "real_display_bind_sender=%lu real_display_bind_completion=%lu "
               "gpup_sender_contract=%lu gpup_completion_contract=%lu "
               "native_completion_validators=armed "
               "native_completion_validator_gate=closed "
               "finite_480p_gate=closed demo_interaction_gate=closed "
               "backend_opengl_submit_gate=closed "
               "kvm_virgl_recheck_gate=deferred webkit_route_gate=closed "
               "webkit_content_gate=closed webkit_enabled_artifact_gate=closed "
               "dda_nouveau_blocker=separate-display-not-D3D12-bind "
               "dda_nouveau_reason=DDA/Nouveau-separate-display-not-D3D12-bind "
               "wsl_display_bind_ioctl=0 "
               "wsl_inband_presenthistory_handler=absent "
               "dda_d3d12_resource_import=%lu dda_scanout_bind=%lu "
               "dda_hw_flip_completion=%s display_bind_present_id=%lu "
               "display_bind_completed=%lu backend_opengl_submit=%u "
               "native_present_credit=0 opengl_submit_credit=0 "
               "webkit_accel_credit=0 status=%s\n",
               stats.dxg_scanout_bind_candidate_sender_contracts,
               stats.dxg_scanout_bind_candidate_completion_contracts,
               stats.dxg_scanout_bind_candidate_sender_contracts,
               stats.dxg_scanout_bind_candidate_completion_contracts,
               stats.dxg_present_dda_nouveau_import_path_present,
               stats.dxg_present_dda_nouveau_scanout_bind_present,
               stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
                   "ABSENT" : "PRESENT",
               stats.dxg_display_bind_present_id,
               stats.dxg_display_bind_completed_id,
               backend_opengl_submit,
               stats.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                       stats.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                       stats.dxg_present_dda_nouveau_import_path_present == 0 &&
                       stats.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
                       stats.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
                       stats.dxg_display_bind_present_id == 0 &&
                       stats.dxg_display_bind_completed_id == 0 &&
                       stats.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
                       stats.dxg_present_dxg_adapter_display_supported == 0 &&
                       stats.dxg_present_dxg_adapter_sources == 0 &&
                       stats.dxg_display_bind_provider_completion_demux_registered == 0 &&
                       stats.kms_present_last_lane ==
                           FB_GPU_KMS_PRESENT_LANE_NONE &&
                       stats.kms_page_flip_events_native_hw == 0 &&
                       backend_opengl_submit == 0 ?
                   "PASS" : "FAIL");
        if (stats.nouveau_pci_probe_accepts == 0) {
            printf("gpu_core_c_validator nouveau_pci_runtime_contract_matrix "
                   "accepts=0 gpup_only=PASS dma_mask=NOT_CONFIGURED "
                   "coherent_dma_mask=NOT_CONFIGURED "
                   "dma_map=GPU_P_FAIL_CLOSED "
                   "bar_claim=NOT_ATTEMPTED "
                   "msi_msix_setup=NOT_ATTEMPTED "
                   "legacy_irq_fallback=NOT_CLAIMED "
                   "irq_handler=ABSENT irq_delivery=ABSENT "
                   "runtime_pm_usage=DEFERRED remove_path=DEFERRED "
                   "native_present_credit=0 opengl_submit_credit=0 "
                   "status=PASS\n");
            printf("gpu_core_c_validator "
                   "nouveau_pci_runtime_interface_matrix "
                   "accepts=0 resource_tree=GPU_P_FAIL_CLOSED "
                   "dma_mapping_api=GPU_P_FAIL_CLOSED "
                   "msi_msix_programming=NOT_ATTEMPTED "
                   "legacy_irq_fallback=NOT_CLAIMED irq_delivery=ABSENT "
                   "runtime_pm=DEFERRED remove_path=DEFERRED "
                   "resource_owner=GPU_P_FAIL_CLOSED "
                   "claim_before_iomap=GPU_P_FAIL_CLOSED "
                   "release_balance=GPU_P_FAIL_CLOSED owner_mismatch=0 "
                   "unclaimed_iomap=0 unclaimed_release=0 "
                   "hot_remove=DEFERRED native_engine=ABSENT "
                   "native_present_credit=0 opengl_submit_credit=0 "
                   "status=PASS\n");
            printf("gpu_core_c_validator nouveau_pci_irq_provenance_matrix "
                   "accepts=0 msi_attempts=0 msi_unsupported=0 "
                   "msix_attempts=0 msix_unsupported=0 "
                   "legacy_requests=0 legacy_grants=0 "
                   "handler_invocations=0 cause_reads=0 cause_valid=0 "
                   "cause_acks=0 spurious=0 "
                   "device_cause=GPU_P_FAIL_CLOSED "
                   "native_present_credit=0 opengl_submit_credit=0 "
                   "status=PASS\n");
            printf("gpu_core_c_validator nouveau_pci_remove_pm_matrix "
                   "accepts=0 remove_calls=0 runtime_resume_attempts=0 "
                   "runtime_resume_successes=0 runtime_barriers=0 "
                   "runtime_resume_before_remove=NOT_APPLICABLE "
                   "remove_while_suspended=0 hot_remove_events=0 "
                   "removed=0 bar_iounmaps=0 irq_unregisters=0 "
                   "irq_vectors_freed=0 bus_master_clears=0 "
                   "device_disables=0 drvdata_cleared=0 "
                   "teardown=GPU_P_FAIL_CLOSED native_present_credit=0 "
                   "opengl_submit_credit=0 status=PASS\n");
        } else {
            printf("gpu_core_c_validator nouveau_pci_runtime_contract_matrix "
                   "accepts=%lu gpup_only=NO dma_mask=PASS "
                   "coherent_dma_mask=PASS dma_map=PASS bar_claim=PASS "
                   "msi_msix_setup=%s legacy_irq_fallback=%s "
                   "irq_handler=%s irq_delivery=%s "
                   "runtime_pm_usage=DIAGNOSTIC remove_path=%s "
                   "native_present_credit=0 opengl_submit_credit=0 "
                   "status=DIAGNOSTIC\n",
                   stats.nouveau_pci_probe_accepts,
                   stats.nouveau_pci_msi_fail_closed ? "FAIL_CLOSED" :
                       "NOT_ATTEMPTED",
                   stats.nouveau_pci_legacy_irq_fallback ? "PASS" :
                       "MISSING",
                   stats.nouveau_pci_irq_handler_registered ? "PRESENT" :
                       "ABSENT",
                   (stats.nouveau_pci_irq_delivery_enabled ||
                    stats.nouveau_pci_irq_delivery_claimed) ? "PRESENT" :
                       "ABSENT",
                   stats.nouveau_pci_removes ? "DIAGNOSTIC" :
                       "DEFERRED");
            printf("gpu_core_c_validator "
                   "nouveau_pci_runtime_interface_matrix "
                   "accepts=%lu resource_tree=PASS dma_mapping_api=PASS "
                   "msi_msix_programming=%s legacy_irq_fallback=%s "
                   "irq_delivery=%s runtime_pm=DIAGNOSTIC "
                   "remove_path=%s resource_owner=PASS "
                   "claim_before_iomap=PASS release_balance=PASS "
                   "owner_mismatch=0 unclaimed_iomap=0 "
                   "unclaimed_release=0 hot_remove=DIAGNOSTIC "
                   "native_engine=DIAGNOSTIC native_present_credit=0 "
                   "opengl_submit_credit=0 status=DIAGNOSTIC\n",
                   stats.nouveau_pci_probe_accepts,
                   stats.nouveau_pci_msi_fail_closed ? "FAIL_CLOSED" :
                       "NOT_ATTEMPTED",
                   stats.nouveau_pci_legacy_irq_fallback ? "PASS" :
                       "MISSING",
                   (stats.nouveau_pci_irq_delivery_enabled ||
                    stats.nouveau_pci_irq_delivery_claimed) ? "PRESENT" :
                       "ABSENT",
                   stats.nouveau_pci_removes ? "DIAGNOSTIC" :
                       "DEFERRED");
            printf("gpu_core_c_validator nouveau_pci_irq_provenance_matrix "
                   "accepts=%lu msi_attempts=%lu msi_unsupported=%lu "
                   "msix_attempts=%lu msix_unsupported=%lu "
                   "legacy_requests=%lu legacy_grants=%lu "
                   "handler_invocations=%lu cause_reads=%lu "
                   "cause_valid=%lu cause_acks=%lu spurious=%lu "
                   "device_cause=%s native_present_credit=0 "
                   "opengl_submit_credit=0 status=DIAGNOSTIC\n",
                   stats.nouveau_pci_probe_accepts,
                   stats.nouveau_pci_msi_program_attempts,
                   stats.nouveau_pci_msi_program_unsupported,
                   stats.nouveau_pci_msix_program_attempts,
                   stats.nouveau_pci_msix_program_unsupported,
                   stats.nouveau_pci_legacy_irq_requests,
                   stats.nouveau_pci_legacy_irq_grants,
                   stats.nouveau_pci_irq_handler_invocations,
                   stats.nouveau_pci_irq_cause_reads,
                   stats.nouveau_pci_irq_cause_valid,
                   stats.nouveau_pci_irq_cause_acks,
                   stats.nouveau_pci_irq_spurious,
                   stats.nouveau_pci_irq_cause_acks != 0 ? "PASS" :
                                                            "DIAGNOSTIC");
            printf("gpu_core_c_validator nouveau_pci_remove_pm_matrix "
                   "accepts=%lu remove_calls=%lu "
                   "runtime_resume_attempts=%lu "
                   "runtime_resume_successes=%lu runtime_barriers=%lu "
                   "runtime_resume_before_remove=%s "
                   "remove_while_suspended=%lu hot_remove_events=%lu "
                   "removed=%lu bar_iounmaps=%lu irq_unregisters=%lu "
                   "irq_vectors_freed=%lu bus_master_clears=%lu "
                   "device_disables=%lu drvdata_cleared=%lu "
                   "teardown=%s native_present_credit=0 "
                   "opengl_submit_credit=0 status=DIAGNOSTIC\n",
                   stats.nouveau_pci_probe_accepts,
                   stats.nouveau_pci_remove_calls,
                   stats.nouveau_pci_remove_runtime_resume_attempts,
                   stats.nouveau_pci_remove_runtime_resume_successes,
                   stats.nouveau_pci_remove_runtime_barriers,
                   stats.nouveau_pci_removes == 0 ? "DEFERRED" :
                       (stats.nouveau_pci_remove_runtime_suspended == 0 ?
                            "PASS" : "FAIL"),
                   stats.nouveau_pci_remove_runtime_suspended,
                   stats.nouveau_pci_hot_remove_events,
                   stats.nouveau_pci_removed,
                   stats.nouveau_pci_bar_iounmaps,
                   stats.nouveau_pci_irq_unregisters,
                   stats.nouveau_pci_irq_vectors_freed,
                   stats.nouveau_pci_bus_master_clears,
                   stats.nouveau_pci_device_disables,
                   stats.nouveau_pci_drvdata_cleared,
                   stats.nouveau_pci_removes == 0 ? "DEFERRED" :
                       (stats.nouveau_pci_drvdata_cleared != 0 &&
                        stats.nouveau_pci_device_disables != 0 ? "PASS" :
                                                                 "FAIL"));
        }
        if (stats.nouveau_pci_probe_accepts == 0) {
            printf("gpu_core_c_validator nouveau_gpup_failclosed_matrix "
                   "accepts=0 backend_dda_nouveau=0 reject_reason=PASS "
                   "no_fake_bar=PASS no_fake_dma=PASS no_fake_irq=PASS "
                   "no_fake_getparam=PASS no_fake_remove=PASS "
                   "no_fake_present=PASS "
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
        printf("gpu_core_c_validator nouveau_nvif_failclosed_matrix "
               "ioctls=%lu sclass_queries=%lu sclass_count=0 "
               "new_rejects=%lu del_rejects=%lu unsupported=%lu "
               "no_fabricated_classes=PASS status=PASS\n",
               stats.nouveau_nvif_ioctls,
               stats.nouveau_nvif_sclass_queries,
               stats.nouveau_nvif_new_rejects,
               stats.nouveau_nvif_del_rejects,
               stats.nouveau_nvif_unsupported);
        printf("gpu_core_c_validator nouveau_submit_failclosed_matrix "
               "pushbuf_noops=%lu exec_noops=%lu vm_bind_noops=%lu "
               "nonempty_pushbuf_rejects=%lu nonempty_exec_rejects=%lu "
               "nonempty_vm_bind_rejects=%lu native_present_credit=0 "
               "opengl_submit_credit=0 status=PASS\n",
               stats.nouveau_pushbuf_noops,
               stats.nouveau_exec_noops,
               stats.nouveau_vm_bind_noops,
               stats.nouveau_nonempty_pushbuf_rejects,
               stats.nouveau_nonempty_exec_rejects,
               stats.nouveau_nonempty_vm_bind_rejects);
        printf("gpu_core_c_validator nouveau_gem_mmap_backing_matrix "
               "gem_news=%lu gem_infos=%lu cpu_preps=%lu cpu_finis=%lu "
               "mmap_backing=absent mmap_successes=0 "
               "backing_source=none linux_mmap_credit=0 "
               "native_present_credit=0 opengl_submit_credit=0 "
               "status=PASS\n",
               stats.nouveau_gem_news,
               stats.nouveau_gem_infos,
               stats.nouveau_cpu_preps,
               stats.nouveau_cpu_finis);
        printf("gpu_core_c_validator "
               "nouveau_gpuvm_mapping_failclosed_matrix "
               "vm_inits=%lu vm_bind_noops=%lu "
               "nonempty_vm_bind_rejects=%lu mapping_successes=0 "
               "mapping_backend=fail_closed native_present_credit=0 "
               "opengl_submit_credit=0 status=PASS\n",
               stats.nouveau_vm_inits,
               stats.nouveau_vm_bind_noops,
               stats.nouveau_nonempty_vm_bind_rejects);
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

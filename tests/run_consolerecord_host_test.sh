#!/usr/bin/env bash
set -euo pipefail

user_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
top_root="$(cd -- "${user_root}/.." && pwd)"
tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/xv6-consolerecord-test.XXXXXX")"
trap 'rm -rf "${tmpdir}"' EXIT
cd "${top_root}"

cc -std=c11 -Wall -Wextra -Werror -DHOST_LIBC_PROGRAM -D_GNU_SOURCE \
    -DON_HOST_OS=1 -DCONFIG_ARCH_X86_64=1 \
    -I"${user_root}/lib" -I"${top_root}" -I"${top_root}/kernel" \
    -idirafter "${top_root}/kernel/kernel/inc" \
    "${user_root}/programs/consolerecord/consolerecord.c" \
    -o "${tmpdir}/consolerecord"
cc -std=c11 -Wall -Wextra -Werror -fPIC -shared \
    -iquote "${top_root}/kernel/kernel/inc" \
    "${user_root}/tests/consolerecord_ioctl_shim.c" \
    -o "${tmpdir}/consolerecord-ioctl-shim.so"

run_quiet() {
    local expected_rc=$1
    local name=$2
    shift 2
    local out="${tmpdir}/${name}.stdout"
    local err="${tmpdir}/${name}.stderr"
    local rc

    set +e
    "$@" >"${out}" 2>"${err}"
    rc=$?
    set -e
    if [[ ${rc} -ne ${expected_rc} || -s "${out}" || -s "${err}" ]]; then
        echo "consolerecord-host-test: ${name}: rc=${rc} expected=${expected_rc} stdout=$(wc -c <"${out}") stderr=$(wc -c <"${err}")" >&2
        return 1
    fi
}

run_ioctl_case() {
    local expected_rc=$1
    local name=$2
    local mode=$3
    local count=$4
    local expected_file=$5
    shift 5
    local trace="${tmpdir}/${name}.trace"

    run_quiet "${expected_rc}" "${name}" env \
        LD_PRELOAD="${tmpdir}/consolerecord-ioctl-shim.so" \
        CONSOLE_RECORD_TRACE_FILE="${trace}" \
        CONSOLE_RECORD_EXPECT_MODE="${mode}" \
        CONSOLE_RECORD_EXPECT_COUNT="${count}" \
        CONSOLE_RECORD_EXPECT_FILE="${expected_file}" \
        "$@"
    [[ -f "${trace}" && $(wc -c <"${trace}") -eq 1 ]] || {
        echo "consolerecord-host-test: ${name}: expected exactly one ioctl" >&2
        return 1
    }
}

run_reject_case() {
    local name=$1
    shift
    local trace="${tmpdir}/${name}.trace"

    run_quiet 2 "${name}" env \
        LD_PRELOAD="${tmpdir}/consolerecord-ioctl-shim.so" \
        CONSOLE_RECORD_TRACE_FILE="${trace}" \
        "$@"
    [[ ! -e "${trace}" ]] || {
        echo "consolerecord-host-test: ${name}: unexpected ioctl" >&2
        return 1
    }
}

run_terminal_case() {
    local expected_rc=$1
    local name=$2
    local marker=$3
    local source=$4
    local expected_file=$5
    local expected_trace=$6
    shift 6
    local trace="${tmpdir}/${name}.trace"

    run_quiet "${expected_rc}" "${name}" env \
        LD_PRELOAD="${tmpdir}/consolerecord-ioctl-shim.so" \
        CONSOLE_RECORD_TRACE_FILE="${trace}" \
        CONSOLE_RECORD_EXPECT_MODE=terminal \
        CONSOLE_RECORD_EXPECT_COUNT=2 \
        CONSOLE_RECORD_EXPECT_FILE="${expected_file}" \
        CONSOLE_RECORD_SOURCE_FILE="${source}" \
        "$@" \
        "${tmpdir}/consolerecord" --terminal-batch-file "${source}" \
        "${marker}"
    local actual_trace=""
    [[ ! -e "${trace}" ]] || actual_trace="$(<"${trace}")"
    [[ "${actual_trace}" == "${expected_trace}" ]] || {
        echo "consolerecord-host-test: ${name}: trace=${actual_trace} expected=${expected_trace}" >&2
        return 1
    }
}

make_terminal_source() {
    local path=$1
    local marker=$2
    printf '%s:RC:0\n%s:FENCE\n' "${marker}" "${marker}" >"${path}"
    chmod 0600 "${path}"
}

printf 'legacy\n' >"${tmpdir}/legacy.expected"
run_ioctl_case 0 legacy legacy 1 "${tmpdir}/legacy.expected" \
    "${tmpdir}/consolerecord" legacy

printf 'first\nsecond record\n' >"${tmpdir}/batch.valid"
run_ioctl_case 0 batch batch 2 "${tmpdir}/batch.valid" \
    "${tmpdir}/consolerecord" --batch-file "${tmpdir}/batch.valid"

dd if=/dev/zero bs=510 count=1 status=none | tr '\0' x >"${tmpdir}/max-record"
printf '\n' >>"${tmpdir}/max-record"
run_ioctl_case 0 max-record batch 1 "${tmpdir}/max-record" \
    "${tmpdir}/consolerecord" --batch-file "${tmpdir}/max-record"

for _ in $(seq 1 701); do printf 'x\n'; done >"${tmpdir}/max-record-count"
run_ioctl_case 0 max-record-count batch 701 "${tmpdir}/max-record-count" \
    "${tmpdir}/consolerecord" --batch-file "${tmpdir}/max-record-count"

{
    dd if=/dev/zero bs=510 count=697 status=none | tr '\0' x | fold -w 510
    printf '\n'
    printf '%205s\n' '' | tr ' ' x
    printf '%205s\n' '' | tr ' ' x
    printf '%296s\n' '' | tr ' ' x
    printf '%203s\n' '' | tr ' ' x
} >"${tmpdir}/max-logical"
[[ $(stat -c %s "${tmpdir}/max-logical") -eq 357080 ]]
run_ioctl_case 0 max-logical batch 701 "${tmpdir}/max-logical" \
    "${tmpdir}/consolerecord" --batch-file "${tmpdir}/max-logical"

trace="${tmpdir}/ioctl-failure.trace"
run_quiet 4 ioctl-failure env \
    LD_PRELOAD="${tmpdir}/consolerecord-ioctl-shim.so" \
    CONSOLE_RECORD_TRACE_FILE="${trace}" \
    CONSOLE_RECORD_EXPECT_MODE=batch \
    CONSOLE_RECORD_EXPECT_COUNT=2 \
    CONSOLE_RECORD_EXPECT_FILE="${tmpdir}/batch.valid" \
    CONSOLE_RECORD_IOCTL_FAIL=1 \
    "${tmpdir}/consolerecord" --batch-file "${tmpdir}/batch.valid"
[[ -f "${trace}" && $(wc -c <"${trace}") -eq 1 ]]

terminal_marker=T
make_terminal_source "${tmpdir}/terminal.expected" "${terminal_marker}"
cp "${tmpdir}/terminal.expected" "${tmpdir}/terminal.valid"
chmod 0600 "${tmpdir}/terminal.valid"
run_terminal_case 0 terminal-valid "${terminal_marker}" \
    "${tmpdir}/terminal.valid" "${tmpdir}/terminal.expected" OCUXIZ
[[ ! -e "${tmpdir}/terminal.valid" ]]

terminal_max_marker="$(printf '%064d' 0 | tr 0 M)"
make_terminal_source "${tmpdir}/terminal-max.expected" "${terminal_max_marker}"
cp "${tmpdir}/terminal-max.expected" "${tmpdir}/terminal-max"
chmod 0600 "${tmpdir}/terminal-max"
[[ $(stat -c %s "${tmpdir}/terminal-max") -eq 141 ]]
run_terminal_case 0 terminal-max "${terminal_max_marker}" \
    "${tmpdir}/terminal-max" "${tmpdir}/terminal-max.expected" OCUXIZ

for status_case in ioctl-fail console-close-fail; do
    source="${tmpdir}/terminal-${status_case}"
    make_terminal_source "${source}" "${terminal_marker}"
    if [[ ${status_case} == ioctl-fail ]]; then
        run_terminal_case 4 terminal-ioctl-fail "${terminal_marker}" \
            "${source}" "${tmpdir}/terminal.expected" OCUXIZ \
            CONSOLE_RECORD_IOCTL_FAIL=1
    else
        run_terminal_case 0 terminal-console-close-fail "${terminal_marker}" \
            "${source}" "${tmpdir}/terminal.expected" OCUXIZ \
            CONSOLE_RECORD_CONSOLE_CLOSE_FAIL=1
    fi
    [[ ! -e "${source}" ]]
done

source="${tmpdir}/terminal-console-open-fail"
make_terminal_source "${source}" "${terminal_marker}"
run_terminal_case 3 terminal-console-open-fail "${terminal_marker}" \
    "${source}" "${tmpdir}/terminal.expected" OCX \
    CONSOLE_RECORD_CONSOLE_OPEN_FAIL=1
[[ -f "${source}" ]]

source="${tmpdir}/terminal-unlink-fail"
make_terminal_source "${source}" "${terminal_marker}"
run_terminal_case 2 terminal-unlink-fail "${terminal_marker}" \
    "${source}" "${tmpdir}/terminal.expected" OCUXZ \
    CONSOLE_RECORD_UNLINK_FAIL=1
[[ -f "${source}" ]]

source="${tmpdir}/terminal-recreated"
make_terminal_source "${source}" "${terminal_marker}"
run_terminal_case 2 terminal-path-reappeared "${terminal_marker}" \
    "${source}" "${tmpdir}/terminal.expected" OCUXZ \
    CONSOLE_RECORD_RECREATE_AFTER_UNLINK=1
[[ -f "${source}" ]]

source="${tmpdir}/terminal-nlink"
make_terminal_source "${source}" "${terminal_marker}"
run_terminal_case 2 terminal-nlink-zero "${terminal_marker}" \
    "${source}" "${tmpdir}/terminal.expected" OCUXZ \
    CONSOLE_RECORD_NLINK_ZERO_FAIL=1
[[ ! -e "${source}" ]]

source="${tmpdir}/terminal-input-close"
make_terminal_source "${source}" "${terminal_marker}"
run_terminal_case 2 terminal-input-close "${terminal_marker}" \
    "${source}" "${tmpdir}/terminal.expected" OCUXXZ \
    CONSOLE_RECORD_INPUT_CLOSE_FAIL=1
[[ ! -e "${source}" ]]

for transfer_case in short grow replace; do
    source="${tmpdir}/terminal-${transfer_case}"
    make_terminal_source "${source}" "${terminal_marker}"
    case ${transfer_case} in
        short) injection=CONSOLE_RECORD_SHORT_READ=1 ;;
        grow) injection=CONSOLE_RECORD_GROW_AFTER_READ=1 ;;
        replace) injection=CONSOLE_RECORD_REPLACE_AFTER_READ=1 ;;
    esac
    run_terminal_case 2 "terminal-${transfer_case}" "${terminal_marker}" \
        "${source}" "${tmpdir}/terminal.expected" OCXZ "${injection}"
done

for grammar_case in nonzero duplicate reversed wrong-marker missing-fence; do
    source="${tmpdir}/terminal-grammar-${grammar_case}"
    case ${grammar_case} in
        nonzero) printf '%s:RC:7\n%s:FENCE\n' "${terminal_marker}" "${terminal_marker}" >"${source}" ;;
        duplicate) printf '%s:RC:0\n%s:RC:0\n%s:FENCE\n' "${terminal_marker}" "${terminal_marker}" "${terminal_marker}" >"${source}" ;;
        reversed) printf '%s:FENCE\n%s:RC:0\n' "${terminal_marker}" "${terminal_marker}" >"${source}" ;;
        wrong-marker) printf 'X:RC:0\nX:FENCE\n' >"${source}" ;;
        missing-fence) printf '%s:RC:0\n' "${terminal_marker}" >"${source}" ;;
    esac
    chmod 0600 "${source}"
    expected_grammar_trace=OCXZ
    if [[ ${grammar_case} == duplicate || ${grammar_case} == missing-fence ]]; then
        expected_grammar_trace=""
    fi
    run_terminal_case 2 "terminal-grammar-${grammar_case}" \
        "${terminal_marker}" "${source}" "${tmpdir}/terminal.expected" \
        "${expected_grammar_trace}"
done

source="${tmpdir}/terminal-nonprivate"
make_terminal_source "${source}" "${terminal_marker}"
chmod 0644 "${source}"
run_terminal_case 2 terminal-nonprivate "${terminal_marker}" \
    "${source}" "${tmpdir}/terminal.expected" ""

source="${tmpdir}/terminal-hardlink"
make_terminal_source "${source}" "${terminal_marker}"
ln "${source}" "${source}.link"
run_terminal_case 2 terminal-hardlink "${terminal_marker}" \
    "${source}" "${tmpdir}/terminal.expected" ""

ln -s terminal.expected "${tmpdir}/terminal-symlink"
run_terminal_case 2 terminal-symlink "${terminal_marker}" \
    "${tmpdir}/terminal-symlink" "${tmpdir}/terminal.expected" ""

source="${tmpdir}/terminal-marker-over"
make_terminal_source "${source}" "${terminal_marker}"
run_terminal_case 2 terminal-marker-over "$(printf '%065d' 0 | tr 0 M)" \
    "${source}" "${tmpdir}/terminal.expected" ""

: >"${tmpdir}/empty"
printf 'first\n\nthird\n' >"${tmpdir}/empty-record"
printf 'missing-final-lf' >"${tmpdir}/missing-lf"
printf 'bad\rrecord\n' >"${tmpdir}/cr"
printf 'bad\0record\n' >"${tmpdir}/nul"
printf 'bad\001record\n' >"${tmpdir}/binary"
dd if=/dev/zero bs=511 count=1 status=none | tr '\0' x >"${tmpdir}/long-record"
printf '\n' >>"${tmpdir}/long-record"
truncate -s 357081 "${tmpdir}/overcap"
ln -s batch.valid "${tmpdir}/symlink"
mkfifo "${tmpdir}/fifo"
mkdir "${tmpdir}/directory"
for _ in $(seq 1 702); do printf 'x\n'; done >"${tmpdir}/too-many"

run_reject_case empty "${tmpdir}/consolerecord" --batch-file "${tmpdir}/empty"
run_reject_case empty-record "${tmpdir}/consolerecord" --batch-file "${tmpdir}/empty-record"
run_reject_case missing-lf "${tmpdir}/consolerecord" --batch-file "${tmpdir}/missing-lf"
run_reject_case cr "${tmpdir}/consolerecord" --batch-file "${tmpdir}/cr"
run_reject_case nul "${tmpdir}/consolerecord" --batch-file "${tmpdir}/nul"
run_reject_case binary "${tmpdir}/consolerecord" --batch-file "${tmpdir}/binary"
run_reject_case long-record "${tmpdir}/consolerecord" --batch-file "${tmpdir}/long-record"
run_reject_case overcap "${tmpdir}/consolerecord" --batch-file "${tmpdir}/overcap"
run_reject_case symlink "${tmpdir}/consolerecord" --batch-file "${tmpdir}/symlink"
run_reject_case fifo timeout 2 "${tmpdir}/consolerecord" --batch-file "${tmpdir}/fifo"
run_reject_case directory "${tmpdir}/consolerecord" --batch-file "${tmpdir}/directory"
run_reject_case too-many "${tmpdir}/consolerecord" --batch-file "${tmpdir}/too-many"
run_reject_case wrong-option "${tmpdir}/consolerecord" --not-batch "${tmpdir}/batch.valid"

safe_rg="${top_root}/scripts/audit/safe-rg.sh"
if "${safe_rg}" -n '(^|[^[:alnum:]_])write[[:space:]]*\(' \
    user/programs/consolerecord/consolerecord.c >/dev/null; then
    echo "consolerecord-host-test: forbidden write fallback" >&2
    exit 1
fi
if "${safe_rg}" -n '(^|[^[:alnum:]_])(printf|fprintf|puts|fputs)[[:space:]]*\(' \
    user/programs/consolerecord/consolerecord.c >/dev/null; then
    echo "consolerecord-host-test: forbidden diagnostic output" >&2
    exit 1
fi

echo "consolerecord-host-test: PASS"

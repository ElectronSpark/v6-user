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

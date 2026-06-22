#!/usr/bin/env bash
#
# build-host.sh - compile d3d12probe natively on the WSL/Hyper-V host to sanity
# check the staged GPU-PV runtime independently of xv6. Produces
# build-x86_64/d3d12probe-host/d3d12probe-host by default.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

DXH="${REPO_ROOT}/ports/mesa/src/subprojects/DirectX-Headers-1.0/include"
RUNTIME="${GPUP_RUNTIME_DIR:-/usr/lib/wsl/lib}"

if [[ ! -d "${DXH}/directx" ]]; then
    echo "build-host: DirectX-Headers not found at ${DXH}" >&2
    exit 1
fi
if [[ ! -e "${RUNTIME}/libd3d12.so" || ! -e "${RUNTIME}/libdxcore.so" ]]; then
    echo "build-host: GPU-PV runtime (libd3d12.so/libdxcore.so) not in ${RUNTIME}" >&2
    echo "  set GPUP_RUNTIME_DIR or run on a host with the WSL GPU-PV runtime." >&2
    exit 1
fi

OUT_DIR="${D3D12PROBE_HOST_BUILD_DIR:-${REPO_ROOT}/build-x86_64/d3d12probe-host}"
OUT="${OUT_DIR}/d3d12probe-host"
DXGUIDS="${REPO_ROOT}/ports/mesa/src/subprojects/DirectX-Headers-1.0/src/dxguids.cpp"
mkdir -p "${OUT_DIR}"
set -x
g++ -std=c++17 -O2 -Wall \
    -I"${DXH}" -I"${DXH}/wsl/stubs" \
    "${SCRIPT_DIR}/d3d12probe.cpp" "${DXGUIDS}" \
    -L"${RUNTIME}" -ld3d12 -ldxcore -ldl -lpthread \
    -Wl,-rpath,"${RUNTIME}" \
    -o "${OUT}"
set +x
echo "build-host: built ${OUT}"
echo "build-host: run with: LD_LIBRARY_PATH=${RUNTIME} ${OUT}"

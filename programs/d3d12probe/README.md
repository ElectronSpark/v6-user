# d3d12probe — real-GPU D3D12 validation client for the GPU-P/DXG path

`d3d12probe` is a small, self-contained D3D12 client used to validate the
Hyper-V GPU-P + DXG/D3DKMT path end to end. It does **not** depend on a shader
compiler: it proves real GPU command execution using the GPU copy engine and a
GPU-signalled fence, which is exactly the keystone the xv6 DXG kernel stack
needs validated (CREATEDEVICE → CONTEXT → ALLOCATION → SUBMITCOMMAND →
monitored fence → readback).

## What it does

1. `DXCoreCreateAdapterFactory` → enumerate `D3D12_GRAPHICS` adapters and print
   driver description, driver version, instance LUID, dedicated VRAM, and the
   `IsHardware` flag for each (proves `libdxcore.so` sees the real adapter).
2. `D3D12CreateDevice` on the first hardware adapter (proves `libd3d12core.so`
   loads the NVIDIA UMD `libnvwgf2umx.so` and opens a D3DKMT device on
   `/dev/dxg`).
3. Build a copy-engine command list (UMD compiles a real GPU command buffer):
   UPLOAD buffer → DEFAULT buffer → READBACK buffer.
4. `ExecuteCommandLists` + `Signal` a fence, then wait. The fence is signalled
   by the **GPU**, exercising the D3DKMT SUBMITCOMMAND + monitored-fence path.
5. Map the readback buffer and verify the GPU copied the known pattern. Prints
   `D3D12PROBE: PASS` only if the GPU-written bytes match.

A non-zero exit and a `D3D12PROBE: FAIL` / `D3D12PROBE: ERROR` line indicate the
path did not complete — the probe is fail-closed and never fabricates success.

## Building and running on the WSL/Hyper-V host (sanity check)

This validates the staged runtime itself, independent of xv6:

```sh
scripts/stage-gpup-umd.sh            # stage libd3d12/libdxcore/UMD into overlay
user/programs/d3d12probe/build-host.sh
LD_LIBRARY_PATH=/usr/lib/wsl/lib user/programs/d3d12probe/d3d12probe-host
```

## Running inside the xv6 GPU-P guest

The rootfs build stages the same runtime (via `stage-gpup-umd.sh`) under
`/usr/lib/wsl/lib`, and `/etc/profile.d/gpup-d3d12.sh` puts it on the loader
path. Run `d3d12probe` from the guest shell on a GPU-P-attached VM. A `PASS`
there is the first real evidence the xv6 DXG kernel stack drives the physical
GPU.

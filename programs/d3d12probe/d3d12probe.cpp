// d3d12probe.cpp - real-GPU D3D12 validation client for the Hyper-V GPU-P / DXG
// path. Proves end-to-end GPU execution (adapter enumeration -> device ->
// GPU copy-engine command buffer -> SUBMITCOMMAND -> GPU-signalled fence ->
// readback) WITHOUT needing a shader compiler. Fail-closed: only prints
// "D3D12PROBE: PASS" when the GPU actually copied the known pattern.
//
// Links against the host-provided GPU-PV runtime:
//   libdxcore.so  (DXCoreCreateAdapterFactory)
//   libd3d12.so   (D3D12CreateDevice)
// which in turn load libd3d12core.so + the NVIDIA UMD libnvwgf2umx.so and talk
// D3DKMT to /dev/dxg.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

// On non-Windows the DirectX-Headers need the WSL adapter shim (Windows types,
// IID_PPV_ARGS, __uuidof) pulled in before the directx headers.
#include <wsl/winadapter.h>
#include <directx/dxcore.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>

namespace {

const char *g_stage = "startup";

template <typename T>
void safe_release(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool check(HRESULT hr, const char *what)
{
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D12PROBE: ERROR %s failed hr=0x%08lx (stage=%s)\n",
                     what, (unsigned long)hr, g_stage);
        return false;
    }
    return true;
}

// One pattern dword per element; large enough to force a real GPU copy.
constexpr uint32_t kElements = 4096;
constexpr uint32_t kBufBytes = kElements * sizeof(uint32_t);

} // namespace

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // ---- 1. Adapter enumeration via dxcore --------------------------------
    g_stage = "DXCoreCreateAdapterFactory";
    IDXCoreAdapterFactory *factory = nullptr;
    if (!check(DXCoreCreateAdapterFactory(IID_PPV_ARGS(&factory)),
               "DXCoreCreateAdapterFactory"))
        return 2;

    g_stage = "CreateAdapterList";
    IDXCoreAdapterList *list = nullptr;
    const GUID attrs[] = { DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS };
    if (!check(factory->CreateAdapterList(1, attrs, IID_PPV_ARGS(&list)),
               "CreateAdapterList")) {
        safe_release(factory);
        return 2;
    }

    const uint32_t count = list->GetAdapterCount();
    std::printf("D3D12PROBE: %u D3D12 graphics adapter(s)\n", count);

    IDXCoreAdapter *chosen = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        IDXCoreAdapter *ad = nullptr;
        if (FAILED(list->GetAdapter(i, IID_PPV_ARGS(&ad))) || !ad)
            continue;

        char desc[256] = {0};
        ad->GetProperty(DXCoreAdapterProperty::DriverDescription, sizeof(desc), desc);

        bool is_hw = false;
        ad->GetProperty(DXCoreAdapterProperty::IsHardware, sizeof(is_hw), &is_hw);

        uint64_t vram = 0;
        ad->GetProperty(DXCoreAdapterProperty::DedicatedAdapterMemory, sizeof(vram), &vram);

        LUID luid = {};
        ad->GetProperty(DXCoreAdapterProperty::InstanceLuid, sizeof(luid), &luid);

        uint64_t drvver = 0;
        ad->GetProperty(DXCoreAdapterProperty::DriverVersion, sizeof(drvver), &drvver);

        std::printf("D3D12PROBE:  adapter[%u] \"%s\" hw=%d vram=%lluMiB "
                    "luid=%08lx:%08lx drv=0x%016llx\n",
                    i, desc, (int)is_hw,
                    (unsigned long long)(vram >> 20),
                    (unsigned long)luid.HighPart, (unsigned long)luid.LowPart,
                    (unsigned long long)drvver);

        if (is_hw && !chosen) {
            chosen = ad;
            chosen->AddRef();
        }
        safe_release(ad);
    }
    safe_release(list);
    safe_release(factory);

    if (!chosen) {
        std::fprintf(stderr, "D3D12PROBE: ERROR no hardware D3D12 adapter found\n");
        return 3;
    }

    // ---- 2. Create the D3D12 device ---------------------------------------
    g_stage = "D3D12CreateDevice";
    ID3D12Device *device = nullptr;
    HRESULT hr = D3D12CreateDevice(chosen, D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&device));
    safe_release(chosen);
    if (!check(hr, "D3D12CreateDevice"))
        return 3;

    int rc = 4;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *alloc = nullptr;
    ID3D12GraphicsCommandList *cmd = nullptr;
    ID3D12Fence *fence = nullptr;
    ID3D12Resource *upload = nullptr;
    ID3D12Resource *dflt = nullptr;
    ID3D12Resource *readback = nullptr;

    // ---- 3. Command queue / allocator / list ------------------------------
    g_stage = "CreateCommandQueue";
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)),
               "CreateCommandQueue"))
        goto out;

    g_stage = "CreateCommandAllocator";
    if (!check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&alloc)),
               "CreateCommandAllocator"))
        goto out;

    g_stage = "CreateCommandList";
    if (!check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                         nullptr, IID_PPV_ARGS(&cmd)),
               "CreateCommandList"))
        goto out;

    // ---- 4. Buffers: UPLOAD -> DEFAULT -> READBACK ------------------------
    {
        D3D12_HEAP_PROPERTIES hp = {};
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kBufBytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        g_stage = "CreateUploadBuffer";
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (!check(device->CreateCommittedResource(
                       &hp, D3D12_HEAP_FLAG_NONE, &rd,
                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                       IID_PPV_ARGS(&upload)),
                   "CreateUploadBuffer"))
            goto out;

        g_stage = "CreateDefaultBuffer";
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (!check(device->CreateCommittedResource(
                       &hp, D3D12_HEAP_FLAG_NONE, &rd,
                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                       IID_PPV_ARGS(&dflt)),
                   "CreateDefaultBuffer"))
            goto out;

        g_stage = "CreateReadbackBuffer";
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        if (!check(device->CreateCommittedResource(
                       &hp, D3D12_HEAP_FLAG_NONE, &rd,
                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                       IID_PPV_ARGS(&readback)),
                   "CreateReadbackBuffer"))
            goto out;
    }

    // Fill the upload buffer with a known pattern (CPU side).
    {
        g_stage = "MapUpload";
        void *p = nullptr;
        D3D12_RANGE none = {0, 0};
        if (!check(upload->Map(0, &none, &p), "MapUpload"))
            goto out;
        uint32_t *u = static_cast<uint32_t *>(p);
        for (uint32_t i = 0; i < kElements; ++i)
            u[i] = 0xC0DE0000u ^ (i * 2654435761u);
        upload->Unmap(0, nullptr);
    }

    // ---- 5. Record GPU copies: upload->default, then default->readback ----
    g_stage = "RecordCopies";
    cmd->CopyResource(dflt, upload);
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dflt;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
    }
    cmd->CopyResource(readback, dflt);

    g_stage = "CloseCommandList";
    if (!check(cmd->Close(), "CloseCommandList"))
        goto out;

    // ---- 6. Submit (real GPU command buffer via D3DKMT SUBMITCOMMAND) -----
    g_stage = "CreateFence";
    if (!check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
               "CreateFence"))
        goto out;

    g_stage = "ExecuteCommandLists";
    {
        ID3D12CommandList *lists[] = { cmd };
        queue->ExecuteCommandLists(1, lists);
    }

    g_stage = "SignalFence";
    if (!check(queue->Signal(fence, 1), "SignalFence"))
        goto out;

    // ---- 7. Wait for the GPU to signal the fence --------------------------
    g_stage = "WaitFence";
    if (fence->GetCompletedValue() < 1) {
        // Busy-wait keeps the probe free of platform event-handle plumbing.
        for (int spins = 0; fence->GetCompletedValue() < 1 && spins < 100000000; ++spins)
            ;
        if (fence->GetCompletedValue() < 1) {
            std::fprintf(stderr, "D3D12PROBE: ERROR fence never signalled by GPU\n");
            goto out;
        }
    }

    // ---- 8. Verify the GPU-copied bytes -----------------------------------
    g_stage = "Verify";
    {
        void *p = nullptr;
        D3D12_RANGE full = {0, kBufBytes};
        if (!check(readback->Map(0, &full, &p), "MapReadback"))
            goto out;
        const uint32_t *r = static_cast<const uint32_t *>(p);
        uint32_t mismatches = 0;
        for (uint32_t i = 0; i < kElements; ++i) {
            uint32_t want = 0xC0DE0000u ^ (i * 2654435761u);
            if (r[i] != want)
                ++mismatches;
        }
        D3D12_RANGE none = {0, 0};
        readback->Unmap(0, &none);

        if (mismatches == 0) {
            std::printf("D3D12PROBE: PASS GPU copied %u bytes, fence signalled\n",
                        kBufBytes);
            rc = 0;
        } else {
            std::fprintf(stderr, "D3D12PROBE: FAIL %u/%u dwords mismatched\n",
                         mismatches, kElements);
            rc = 5;
        }
    }

out:
    safe_release(readback);
    safe_release(dflt);
    safe_release(upload);
    safe_release(fence);
    safe_release(cmd);
    safe_release(alloc);
    safe_release(queue);
    safe_release(device);
    return rc;
}

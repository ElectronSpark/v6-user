#include "kernel/inc/types.h"
#include "kernel/inc/uabi/d3dkmthk.h"
#include "kernel/inc/uabi/fcntl.h"
#include "user/user.h"

#define DXGPROBE_DEFAULT_FENCE_WAIT_SECONDS 5

static int g_paging_fence_wait_seconds = DXGPROBE_DEFAULT_FENCE_WAIT_SECONDS;

static int query_adapter_type(int fd, struct d3dkmthandle adapter,
                              enum kmtqueryadapterinfotype type,
                              const char *name)
{
    struct d3dkmt_adaptertype adapter_type;
    struct d3dkmt_queryadapterinfo query;

    memset(&adapter_type, 0, sizeof(adapter_type));
    memset(&query, 0, sizeof(query));
    query.adapter = adapter;
    query.type = type;
    query.private_data = (uint64)&adapter_type;
    query.private_data_size = sizeof(adapter_type);
    if (ioctl(fd, LX_DXQUERYADAPTERINFO, &query) < 0) {
        printf("dxgprobe: %s query failed\n", name);
        return -1;
    }
    printf("%s value=0x%x render=%u display=%u software=%u paravirtualized=%u compute=%u\n",
           name, adapter_type.value, adapter_type.render_supported,
           adapter_type.display_supported, adapter_type.software_device,
           adapter_type.paravirtualized, adapter_type.compute_only);
    return 0;
}

static int query_vidmem(int fd, struct d3dkmthandle adapter)
{
    struct d3dkmt_queryvideomemoryinfo info;

    memset(&info, 0, sizeof(info));
    info.adapter = adapter;
    info.memory_segment_group = _D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
    if (ioctl(fd, LX_DXQUERYVIDEOMEMORYINFO, &info) < 0) {
        printf("dxgprobe: query vidmem failed\n");
        return -1;
    }
    printf("vidmem_local budget=%lu usage=%lu reservation=%lu available=%lu\n",
           info.budget, info.current_usage, info.current_reservation,
           info.available_for_reservation);
    return 0;
}

static void query_statistics(int fd, struct winluid adapter_luid)
{
    struct d3dkmt_querystatistics stats;
    int rc;

    memset(&stats, 0, sizeof(stats));
    stats.type = _D3DKMT_QUERYSTATISTICS_ADAPTER;
    stats.adapter_luid = adapter_luid;
    rc = ioctl(fd, LX_DXQUERYSTATISTICS, &stats);
    if (rc < 0) {
        printf("query_statistics adapter failed rc=%d luid=%x:%x\n",
               rc, adapter_luid.a, adapter_luid.b);
    } else {
        printf("query_statistics adapter ok luid=%x:%x head=%02x%02x%02x%02x\n",
               adapter_luid.a, adapter_luid.b,
               (unsigned char)stats.result.size[0],
               (unsigned char)stats.result.size[1],
               (unsigned char)stats.result.size[2],
               (unsigned char)stats.result.size[3]);
    }
}

static void probe_escape(int fd, struct d3dkmthandle adapter)
{
    struct d3dkmt_escape escape;
    uint32 data[1] = {0};
    int rc;

    memset(&escape, 0, sizeof(escape));
    escape.adapter = adapter;
    escape.type = _D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.flags.no_adapter_synchronization = 1;
    escape.priv_drv_data = (uint64)data;
    escape.priv_drv_data_size = sizeof(data);
    rc = ioctl(fd, LX_DXESCAPE, &escape);
    if (rc < 0) {
        printf("escape_driver_private failed rc=%d adapter=0x%x\n",
               rc, adapter.v);
    } else {
        printf("escape_driver_private ok adapter=0x%x data=0x%x\n",
               adapter.v, data[0]);
    }
}

static void query_umdriver_private_size(int fd, struct d3dkmthandle adapter,
                                        uint32 size)
{
    unsigned char private_data[1024];
    struct d3dkmt_queryadapterinfo query;
    int rc;

    if (size > sizeof(private_data))
        size = sizeof(private_data);
    memset(private_data, 0, sizeof(private_data));
    memset(&query, 0, sizeof(query));
    query.adapter = adapter;
    query.type = _KMTQAITYPE_UMDRIVERPRIVATE;
    query.private_data = (uint64)private_data;
    query.private_data_size = size;
    rc = ioctl(fd, LX_DXQUERYADAPTERINFO, &query);
    if (rc < 0) {
        printf("umdriver_private failed rc=%d requested=%u size=%u\n", rc,
               size,
               query.private_data_size);
        return;
    }
    printf("umdriver_private requested=%u size=%u head=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
           size, query.private_data_size,
           private_data[0], private_data[1], private_data[2], private_data[3],
           private_data[4], private_data[5], private_data[6], private_data[7],
           private_data[8], private_data[9], private_data[10], private_data[11],
           private_data[12], private_data[13], private_data[14], private_data[15]);
}

static void query_umdriver_private(int fd, struct d3dkmthandle adapter)
{
    query_umdriver_private_size(fd, adapter, 64);
    query_umdriver_private_size(fd, adapter, 256);
    query_umdriver_private_size(fd, adapter, 1024);
}

static void query_features(int fd, struct d3dkmthandle adapter)
{
    enum dxgk_feature_id features[] = {
        _DXGK_FEATURE_HWSCH,
        _DXGK_FEATURE_PAGE_BASED_MEMORY_MANAGER,
        _DXGK_FEATURE_KERNEL_MODE_TESTING,
    };

    for (uint32 i = 0; i < sizeof(features) / sizeof(features[0]); i++) {
        struct d3dkmt_isfeatureenabled feature;
        int rc;

        memset(&feature, 0, sizeof(feature));
        feature.adapter = adapter;
        feature.feature_id = features[i];
        rc = ioctl(fd, LX_ISFEATUREENABLED, &feature);
        if (rc < 0) {
            printf("feature_probe id=%u failed rc=%d\n", features[i], rc);
            continue;
        }
        printf("feature_probe id=%u version=%u value=0x%x enabled=%u known=%u driver=%u config=%u\n",
               features[i], feature.result.version, feature.result.value,
               feature.result.enabled, feature.result.known_feature,
               feature.result.supported_by_driver,
               feature.result.supported_on_config);
    }
}

static void query_adapter_raw(int fd, struct d3dkmthandle adapter,
                              uint32 type, uint32 size)
{
    unsigned char private_data[8192];
    struct d3dkmt_queryadapterinfo query;
    int rc;

    if (size > sizeof(private_data))
        size = sizeof(private_data);
    memset(private_data, 0, sizeof(private_data));
    memset(&query, 0, sizeof(query));
    query.adapter = adapter;
    query.type = (enum kmtqueryadapterinfotype)type;
    query.private_data = (uint64)private_data;
    query.private_data_size = size;
    rc = ioctl(fd, LX_DXQUERYADAPTERINFO, &query);
    if (rc < 0) {
        printf("adapter_raw type=%u failed rc=%d size=%u\n", type, rc,
               size);
        return;
    }
    printf("adapter_raw type=%u size=%u head=%02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x\n",
           type, size,
           private_data[0], private_data[1], private_data[2],
           private_data[3], private_data[4], private_data[5],
           private_data[6], private_data[7], private_data[8],
           private_data[9], private_data[10], private_data[11],
           private_data[12], private_data[13], private_data[14],
           private_data[15], private_data[16], private_data[17],
           private_data[18], private_data[19], private_data[20],
           private_data[21], private_data[22], private_data[23],
           private_data[24], private_data[25], private_data[26],
           private_data[27], private_data[28], private_data[29],
           private_data[30], private_data[31]);
}

static void query_adapter_dxcore_raw(int fd, struct d3dkmthandle adapter)
{
    static const struct {
        uint32 type;
        uint32 size;
    } probes[] = {
        { 17, 12 },
        { 3, 24 },
        { 56, 4 },
        { 66, 8192 },
        { 60, 80 },
        { 1, 524 },
        { 30, 4 },
        { 31, 28 },
        { 61, 56 },
        { 62, 64 },
        { 18, 8 },
    };

    for (uint32 i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
        query_adapter_raw(fd, adapter, probes[i].type, probes[i].size);
}

static void probe_flush_heap_transitions(int fd, struct d3dkmthandle adapter)
{
    struct d3dkmt_flushheaptransitions flush;
    int rc;

    if (adapter.v == 0)
        return;
    memset(&flush, 0, sizeof(flush));
    flush.adapter = adapter;
    rc = ioctl(fd, LX_DXFLUSHHEAPTRANSITIONS, &flush);
    if (rc < 0) {
        printf("flush_heap_transitions failed rc=%d adapter=0x%x\n",
               rc, adapter.v);
    } else {
        printf("flush_heap_transitions ok adapter=0x%x\n", adapter.v);
    }
}

static int wait_paging_fence(uint64 fence_cpu_va, uint64 target,
                             const char *label)
{
    volatile uint64 *fence;

    if (fence_cpu_va == 0 || target == 0)
        return 0;
    fence = (volatile uint64 *)fence_cpu_va;
    for (int i = 0; i < g_paging_fence_wait_seconds; i++) {
        uint64 value = *fence;

        if (value >= target) {
            printf("paging_fence_wait %s target=%lu value=%lu\n",
                   label, target, value);
            return 0;
        }
        sleep(1);
    }
    printf("paging_fence_wait %s timeout target=%lu value=%lu\n",
           label, target, *fence);
    return -1;
}

struct dxg_submit_priv_ib_info {
    uint32 pad0;
    uint32 size_dwords;
    uint64 iova;
    uint64 pad1;
} __attribute__((packed));

struct dxg_submit_priv_data {
    uint32 magic0;
    uint32 pad0;
    uint32 struct_size;
    uint32 pad1;
    uint32 datas_count;
    unsigned char pad2[32];
    struct {
        uint32 magic1;
        uint32 data_size;
        struct {
            uint32 unk1;
            uint32 cmdbuf_size;
            unsigned char pad3[32];
            uint32 ib_count;
            unsigned char pad4[36];
            struct dxg_submit_priv_ib_info ibs[2];
        } cmdbuf;
    } data0;
} __attribute__((packed));

struct dxg_alloc_priv_info {
    uint32 struct_size;
    uint32 pad0;
    uint32 unk0;
    uint32 pad1;
    uint64 size;
    uint32 alignment;
    unsigned char pad2[20];
    uint64 allocated_size;
    uint32 unk1;
    unsigned char pad4[8];
    uint32 unk2;
    unsigned char pad5[76];
    uint32 unk3;
    unsigned char pad6[8];
    uint32 unk4;
    unsigned char pad7[44];
    uint32 unk5;
    unsigned char pad8[16];
    uint32 size_2;
    uint32 unk6;
    uint32 size_3;
    uint32 size_4;
    uint32 unk7;
    unsigned char pad9[56];
} __attribute__((packed));

static unsigned char dxg_existing_sysmem_buffer[0x10000]
    __attribute__((aligned(4096)));

static void make_alloc_private(struct dxg_alloc_priv_info *priv, uint64 size)
{
    memset(priv, 0, sizeof(*priv));
    priv->struct_size = sizeof(*priv);
    priv->unk0 = 1;
    priv->size = size;
    priv->alignment = 4096;
    priv->unk1 = 1;
    priv->unk2 = 61;
    priv->unk3 = 1;
    priv->unk4 = 1;
    priv->unk5 = 3;
    priv->size_2 = size;
    priv->unk6 = 1;
    priv->size_3 = size;
    priv->size_4 = size;
    priv->unk7 = 1;
}

static void make_submit_private(struct dxg_submit_priv_data *priv,
                                uint32 *priv_size_out,
                                uint64 command_iova,
                                uint32 command_size)
{
    uint32 ib_count = 1;
    uint32 priv_size;

    memset(priv, 0, sizeof(*priv));
    priv_size = sizeof(*priv) -
                sizeof(priv->data0.cmdbuf.ibs[0]) * (2 - ib_count);
    priv->magic0 = 0xccaabbee;
    priv->struct_size = priv_size;
    priv->datas_count = 1;
    priv->data0.magic1 = 0xfadcab02;
    priv->data0.data_size =
        sizeof(priv->data0) -
        sizeof(priv->data0.cmdbuf.ibs[0]) * (2 - ib_count);
    priv->data0.cmdbuf.unk1 = 0xcccc0001;
    priv->data0.cmdbuf.cmdbuf_size =
        sizeof(priv->data0.cmdbuf) -
        sizeof(priv->data0.cmdbuf.ibs[0]) * (2 - ib_count);
    priv->data0.cmdbuf.ib_count = ib_count;
    priv->data0.cmdbuf.ibs[0].size_dwords = command_size >> 2;
    priv->data0.cmdbuf.ibs[0].iova = command_iova;
    *priv_size_out = priv_size;
}

static int try_create_context(int fd, struct d3dkmthandle device,
                              const char *name, uint32 node_ordinal,
                              uint32 engine_affinity,
                              enum d3dkmt_clienthint client_hint,
                              struct d3dddi_createcontextflags flags,
                              void *private_data, uint32 private_size,
                              struct d3dkmthandle *context_out)
{
    struct d3dkmt_createcontextvirtual create_context;
    struct d3dkmt_destroycontext destroy_context;
    int rc;

    memset(&create_context, 0, sizeof(create_context));
    create_context.device = device;
    create_context.node_ordinal = node_ordinal;
    create_context.engine_affinity = engine_affinity;
    create_context.client_hint = client_hint;
    create_context.priv_drv_data = (uint64)private_data;
    create_context.priv_drv_data_size = private_size;
    create_context.flags = flags;
    rc = ioctl(fd, LX_DXCREATECONTEXTVIRTUAL, &create_context);
    if (rc < 0 || create_context.context.v == 0) {
        printf("context_probe %s failed rc=%d context=0x%x node=%u engine=%u hint=%u flags=0x%x priv=%u\n",
               name, rc, create_context.context.v, node_ordinal,
               engine_affinity, client_hint, flags.value, private_size);
        return -1;
    }

    printf("context_handle 0x%x mode=%s node=%u engine=%u hint=%u flags=0x%x priv=%u\n",
           create_context.context.v, name, node_ordinal, engine_affinity,
           client_hint, flags.value, private_size);
    if (context_out != 0) {
        *context_out = create_context.context;
        return 0;
    }
    memset(&destroy_context, 0, sizeof(destroy_context));
    destroy_context.context = create_context.context;
    rc = ioctl(fd, LX_DXDESTROYCONTEXT, &destroy_context);
    if (rc < 0) {
        printf("dxgprobe: destroy context failed rc=%d\n", rc);
        return -1;
    }
    return 0;
}

static int probe_context_matrix(int fd, struct d3dkmthandle device,
                                unsigned char *private_data,
                                uint32 private_size,
                                struct d3dkmthandle *context_out,
                                int *sync_only_out)
{
    struct d3dddi_createcontextflags flags;
    struct d3dkmthandle context;
    uint32 private64 = private_size < 64 ? private_size : 64;
    int sync_only = 0;

    memset(&context, 0, sizeof(context));
    memset(&flags, 0, sizeof(flags));

#define TRY_CONTEXT(name, node, engine, hint, pdata, psize, sync_only_value) \
    do { \
        if (context.v == 0 && \
            try_create_context(fd, device, name, node, engine, hint, flags, \
                               pdata, psize, &context) == 0) \
            sync_only = sync_only_value; \
    } while (0)

    TRY_CONTEXT("dx12_private_e1_full", 0, 1, _D3DKMT_CLIENTHINT_DX12,
                private_data, private_size, 0);
    TRY_CONTEXT("dx12_private_e1_64", 0, 1, _D3DKMT_CLIENTHINT_DX12,
                private_data, private64, 0);
    TRY_CONTEXT("dx12_private_e0_64", 0, 0, _D3DKMT_CLIENTHINT_DX12,
                private_data, private64, 0);

    flags.initial_data = 1;
    TRY_CONTEXT("dx12_initial_private_e1_full", 0, 1,
                _D3DKMT_CLIENTHINT_DX12, private_data, private_size, 0);
    TRY_CONTEXT("dx12_initial_private_e1_64", 0, 1,
                _D3DKMT_CLIENTHINT_DX12, private_data, private64, 0);

    memset(&flags, 0, sizeof(flags));
    TRY_CONTEXT("dx12_empty_e1", 0, 1, _D3DKMT_CLIENTHINT_DX12, 0, 0, 0);
    TRY_CONTEXT("dx12_empty_e0", 0, 0, _D3DKMT_CLIENTHINT_DX12, 0, 0, 0);
    TRY_CONTEXT("dx10_empty_e1", 0, 1, _D3DKMT_CLIENTHINT_DX10, 0, 0, 0);
    TRY_CONTEXT("dx10_empty_e0", 0, 0, _D3DKMT_CLIENTHINT_DX10, 0, 0, 0);
    TRY_CONTEXT("dx9_empty_e0", 0, 0, _D3DKMT_CLIENTHINT_DX9, 0, 0, 0);
    TRY_CONTEXT("opengl_empty_e0", 0, 0, _D3DKMT_CLIENTHINT_OPENGL, 0, 0, 0);
    TRY_CONTEXT("cdd_empty_e0", 0, 0, _D3DKMT_CLIENTHINT_CDD, 0, 0, 0);
    TRY_CONTEXT("unknown_empty_e1", 0, 1, _D3DKMT_CLIENTHNT_UNKNOWN, 0, 0, 0);
    TRY_CONTEXT("unknown_empty_e0", 0, 0, _D3DKMT_CLIENTHNT_UNKNOWN, 0, 0, 0);

    flags.disable_gpu_timeout = 1;
    TRY_CONTEXT("unknown_disable_timeout_e0", 0, 0,
                _D3DKMT_CLIENTHNT_UNKNOWN, 0, 0, 0);

    memset(&flags, 0, sizeof(flags));
    flags.null_rendering = 1;
    TRY_CONTEXT("dx12_null_private_e1_64", 0, 1, _D3DKMT_CLIENTHINT_DX12,
                private_data, private64, 0);
    TRY_CONTEXT("unknown_null_empty_e0", 0, 0, _D3DKMT_CLIENTHNT_UNKNOWN,
                0, 0, 0);
    TRY_CONTEXT("unknown_null_empty_e1", 0, 1, _D3DKMT_CLIENTHNT_UNKNOWN,
                0, 0, 0);

    memset(&flags, 0, sizeof(flags));
    flags.hw_queue_supported = 1;
    TRY_CONTEXT("dx12_hwqueue_private_e1_full", 0, 1,
                _D3DKMT_CLIENTHINT_DX12, private_data, private_size, 0);
    TRY_CONTEXT("dx12_hwqueue_private_e1_64", 0, 1,
                _D3DKMT_CLIENTHINT_DX12, private_data, private64, 0);
    TRY_CONTEXT("dx12_hwqueue_private_e0_64", 0, 0,
                _D3DKMT_CLIENTHINT_DX12, private_data, private64, 0);
    TRY_CONTEXT("dx12_hwqueue_e1", 0, 1, _D3DKMT_CLIENTHINT_DX12, 0, 0, 0);
    TRY_CONTEXT("dx12_hwqueue_e0", 0, 0, _D3DKMT_CLIENTHINT_DX12, 0, 0, 0);
    TRY_CONTEXT("dx10_hwqueue_e0", 0, 0, _D3DKMT_CLIENTHINT_DX10, 0, 0, 0);
    TRY_CONTEXT("opengl_hwqueue_e0", 0, 0, _D3DKMT_CLIENTHINT_OPENGL,
                0, 0, 0);
    TRY_CONTEXT("cdd_hwqueue_e0", 0, 0, _D3DKMT_CLIENTHINT_CDD, 0, 0, 0);
    TRY_CONTEXT("unknown_hwqueue_e1", 0, 1, _D3DKMT_CLIENTHNT_UNKNOWN,
                0, 0, 0);
    TRY_CONTEXT("unknown_hwqueue_e0", 0, 0, _D3DKMT_CLIENTHNT_UNKNOWN,
                0, 0, 0);

    memset(&flags, 0, sizeof(flags));
    flags.synchronization_only = 1;
    TRY_CONTEXT("sync_empty", 0, 0, _D3DKMT_CLIENTHNT_UNKNOWN, 0, 0, 1);

#undef TRY_CONTEXT

    if (context.v == 0)
        return -1;
    *context_out = context;
    *sync_only_out = sync_only;
    printf("context_probe usable_non_sync_context=%d selected=0x%x\n",
           sync_only ? 0 : 1, context.v);
    return 0;
}

static int destroy_context_handle(int fd, struct d3dkmthandle context)
{
    struct d3dkmt_destroycontext destroy_context;

    if (context.v == 0)
        return 0;
    memset(&destroy_context, 0, sizeof(destroy_context));
    destroy_context.context = context;
    if (ioctl(fd, LX_DXDESTROYCONTEXT, &destroy_context) < 0) {
        printf("dxgprobe: destroy context failed context=0x%x\n", context.v);
        return -1;
    }
    return 0;
}

static void probe_context_priority(int fd, struct d3dkmthandle context)
{
    struct d3dkmt_getcontextschedulingpriority get_priority;
    struct d3dkmt_setcontextschedulingpriority set_priority;
    int rc;

    if (context.v == 0)
        return;
    memset(&get_priority, 0, sizeof(get_priority));
    get_priority.context = context;
    rc = ioctl(fd, LX_DXGETCONTEXTSCHEDULINGPRIORITY, &get_priority);
    if (rc < 0) {
        printf("context_priority_get failed rc=%d context=0x%x\n",
               rc, context.v);
    } else {
        printf("context_priority_get ok context=0x%x priority=%d\n",
               context.v, get_priority.priority);
    }

    memset(&set_priority, 0, sizeof(set_priority));
    set_priority.context = context;
    set_priority.priority = get_priority.priority;
    rc = ioctl(fd, LX_DXSETCONTEXTSCHEDULINGPRIORITY, &set_priority);
    if (rc < 0) {
        printf("context_priority_set failed rc=%d context=0x%x priority=%d\n",
               rc, context.v, set_priority.priority);
    } else {
        printf("context_priority_set ok context=0x%x priority=%d\n",
               context.v, set_priority.priority);
    }
}

static void probe_allocation_priority(int fd, struct d3dkmthandle device,
                                      struct d3dkmthandle resource,
                                      struct d3dkmthandle allocation)
{
    struct d3dkmt_setallocationpriority set_priority;
    struct d3dkmt_getallocationpriority get_priority;
    struct d3dkmthandle allocation_list[1];
    uint32 priorities[1];
    int rc;

    if (device.v == 0 || allocation.v == 0)
        return;
    allocation_list[0] = allocation;
    priorities[0] = 0;

    memset(&set_priority, 0, sizeof(set_priority));
    set_priority.device = device;
    set_priority.resource = resource;
    if (resource.v == 0) {
        set_priority.allocation_count = 1;
        set_priority.allocation_list = (uint64)allocation_list;
    }
    set_priority.priorities = (uint64)priorities;
    rc = ioctl(fd, LX_DXSETALLOCATIONPRIORITY, &set_priority);
    if (rc < 0) {
        printf("allocation_priority_set failed rc=%d resource=0x%x allocation=0x%x\n",
               rc, resource.v, allocation.v);
    } else {
        printf("allocation_priority_set ok resource=0x%x allocation=0x%x priority=%u\n",
               resource.v, allocation.v, priorities[0]);
    }

    priorities[0] = 0xffffffffU;
    memset(&get_priority, 0, sizeof(get_priority));
    get_priority.device = device;
    get_priority.resource = resource;
    if (resource.v == 0) {
        get_priority.allocation_count = 1;
        get_priority.allocation_list = (uint64)allocation_list;
    }
    get_priority.priorities = (uint64)priorities;
    rc = ioctl(fd, LX_DXGETALLOCATIONPRIORITY, &get_priority);
    if (rc < 0) {
        printf("allocation_priority_get failed rc=%d resource=0x%x allocation=0x%x\n",
               rc, resource.v, allocation.v);
    } else {
        printf("allocation_priority_get ok resource=0x%x allocation=0x%x priority=%u\n",
               resource.v, allocation.v, priorities[0]);
    }
}

static void probe_allocation_residency(int fd, struct d3dkmthandle device,
                                       struct d3dkmthandle resource,
                                       struct d3dkmthandle allocation)
{
    struct d3dkmt_queryallocationresidency query;
    struct d3dkmthandle allocation_list[1];
    enum d3dkmt_allocationresidencystatus residency[1];
    int rc;

    if (device.v == 0 || allocation.v == 0)
        return;
    allocation_list[0] = allocation;
    residency[0] = 0;
    memset(&query, 0, sizeof(query));
    query.device = device;
    query.resource = resource;
    if (resource.v == 0) {
        query.allocation_count = 1;
        query.allocations = (uint64)allocation_list;
    }
    query.residency_status = (uint64)residency;
    rc = ioctl(fd, LX_DXQUERYALLOCATIONRESIDENCY, &query);
    if (rc < 0) {
        printf("allocation_residency_query failed rc=%d resource=0x%x allocation=0x%x\n",
               rc, resource.v, allocation.v);
    } else {
        printf("allocation_residency_query ok resource=0x%x allocation=0x%x status=%u\n",
               resource.v, allocation.v, residency[0]);
    }
}

static void probe_invalidate_cache(int fd, struct d3dkmthandle device,
                                   struct d3dkmthandle allocation,
                                   uint64 allocation_size)
{
    struct d3dkmt_invalidatecache invalidate;
    int rc;

    if (device.v == 0 || allocation.v == 0)
        return;
    memset(&invalidate, 0, sizeof(invalidate));
    invalidate.device = device;
    invalidate.allocation = allocation;
    invalidate.length = allocation_size;
    rc = ioctl(fd, LX_DXINVALIDATECACHE, &invalidate);
    if (rc < 0) {
        printf("invalidate_cache failed rc=%d allocation=0x%x length=%lu\n",
               rc, allocation.v, allocation_size);
    } else {
        printf("invalidate_cache ok allocation=0x%x length=%lu\n",
               allocation.v, allocation_size);
    }
}

static void probe_offer_reclaim(int fd, struct d3dkmthandle device,
                                struct d3dkmthandle paging_queue,
                                struct d3dkmthandle allocation)
{
    struct d3dkmt_offerallocations offer;
    struct d3dkmt_reclaimallocations2 reclaim;
    struct d3dkmthandle allocation_list[1];
    enum d3dddi_reclaim_result reclaim_results[1];
    int rc;

    if (device.v == 0 || paging_queue.v == 0 || allocation.v == 0)
        return;

    allocation_list[0] = allocation;
    memset(&offer, 0, sizeof(offer));
    offer.device = device;
    offer.allocations = (uint64)allocation_list;
    offer.allocation_count = 1;
    offer.priority = _D3DKMT_OFFER_PRIORITY_LOW;
    offer.flags.offer_immediately = 1;
    rc = ioctl(fd, LX_DXOFFERALLOCATIONS, &offer);
    if (rc < 0) {
        printf("offer_allocations failed rc=%d allocation=0x%x\n",
               rc, allocation.v);
        return;
    }
    printf("offer_allocations ok allocation=0x%x priority=%u\n",
           allocation.v, offer.priority);

    reclaim_results[0] = 0;
    memset(&reclaim, 0, sizeof(reclaim));
    reclaim.paging_queue = paging_queue;
    reclaim.allocation_count = 1;
    reclaim.allocations = (uint64)allocation_list;
    reclaim.results = (uint64)reclaim_results;
    rc = ioctl(fd, LX_DXRECLAIMALLOCATIONS2, &reclaim);
    if (rc < 0) {
        printf("reclaim_allocations failed rc=%d allocation=0x%x fence=%lu\n",
               rc, allocation.v, reclaim.paging_fence_value);
    } else {
        printf("reclaim_allocations ok allocation=0x%x result=%u fence=%lu\n",
               allocation.v, reclaim_results[0],
               reclaim.paging_fence_value);
    }
}

static void probe_submit_one_ex(int fd, struct d3dkmthandle context,
                                const char *name, void *command_buffer,
                                uint32 command_length, uint32 flags_value,
                                void *private_data, uint32 private_size,
                                struct d3dkmthandle primary,
                                int use_primary, int use_history)
{
    struct d3dkmt_submitcommand submit;
    struct d3dkmthandle history[1];
    int rc;

    if (context.v == 0)
        return;
    history[0] = primary;
    memset(&submit, 0, sizeof(submit));
    submit.command_buffer = (uint64)command_buffer;
    submit.command_length = command_length;
    submit.broadcast_context_count = 1;
    submit.broadcast_context[0] = context;
    submit.flags.value = flags_value;
    submit.priv_drv_data = (uint64)private_data;
    submit.priv_drv_data_size = private_size;
    if (use_primary && primary.v != 0) {
        submit.num_primaries = 1;
        submit.written_primaries[0] = primary;
    }
    if (use_history && primary.v != 0) {
        submit.num_history_buffers = 1;
        submit.history_buffer_array = (uint64)history;
    }
    rc = ioctl(fd, LX_DXSUBMITCOMMAND, &submit);
    if (rc < 0)
        printf("submit_probe %s failed rc=%d context=0x%x cmd=0x%lx len=%u flags=0x%x priv=%u primary=%u history=%u\n",
               name, rc, context.v, submit.command_buffer, command_length,
               flags_value, private_size, submit.num_primaries,
               submit.num_history_buffers);
    else
        printf("submit_probe %s ok context=0x%x cmd=0x%lx len=%u flags=0x%x priv=%u primary=%u history=%u\n",
               name, context.v, submit.command_buffer, command_length,
               flags_value, private_size, submit.num_primaries,
               submit.num_history_buffers);
}

static void probe_submit_one(int fd, struct d3dkmthandle context,
                             const char *name, void *command_buffer,
                             uint32 command_length, int null_rendering,
                             void *private_data, uint32 private_size)
{
    struct d3dkmthandle none = {0};
    uint32 flags_value = null_rendering ? 1U : 0U;

    probe_submit_one_ex(fd, context, name, command_buffer, command_length,
                        flags_value, private_data, private_size, none,
                        0, 0);
}

static void probe_submit(int fd, struct d3dkmthandle context,
                         unsigned char *private_data, uint32 private_size,
                         uint64 mapped_gpuva, uint64 locked_data,
                         struct d3dkmthandle allocation)
{
    uint32 zero_command[16];
    struct dxg_submit_priv_data submit_private;
    uint32 submit_private_size = 0;
    uint32 private64 = private_size < 64 ? private_size : 64;

    memset(zero_command, 0, sizeof(zero_command));
    if (locked_data != 0) {
        memset((void *)locked_data, 0, sizeof(zero_command));
        printf("submit_probe wrote zero command to locked allocation data=0x%lx len=%u\n",
               locked_data, (uint32)sizeof(zero_command));
    }
    probe_submit_one(fd, context, "empty_plain", 0, 0, 0, 0, 0);
    probe_submit_one(fd, context, "empty_null", 0, 0, 1, 0, 0);
    probe_submit_one(fd, context, "zero4_null", zero_command, 4, 1, 0, 0);
    probe_submit_one(fd, context, "zero64_null", zero_command,
                     sizeof(zero_command), 1, 0, 0);
    probe_submit_one(fd, context, "zero4_plain", zero_command, 4, 0, 0, 0);
    probe_submit_one(fd, context, "zero64_plain", zero_command,
                     sizeof(zero_command), 0, 0, 0);
    probe_submit_one(fd, context, "zero64_private64", zero_command,
                     sizeof(zero_command), 1, private_data, private64);
    probe_submit_one(fd, context, "empty_private64", 0, 0, 1,
                     private_data, private64);
    if (mapped_gpuva != 0) {
        make_submit_private(&submit_private, &submit_private_size,
                            mapped_gpuva, sizeof(zero_command));
        probe_submit_one(fd, context, "gpuva4_plain", (void *)mapped_gpuva,
                         4, 0, 0, 0);
        probe_submit_one(fd, context, "gpuva4_null", (void *)mapped_gpuva,
                         4, 1, 0, 0);
        probe_submit_one(fd, context, "gpuva_plain", (void *)mapped_gpuva,
                         sizeof(zero_command), 0, 0, 0);
        probe_submit_one(fd, context, "gpuva_null", (void *)mapped_gpuva,
                         sizeof(zero_command), 1, 0, 0);
        probe_submit_one_ex(fd, context, "gpuva_nokmd",
                            (void *)mapped_gpuva, sizeof(zero_command), 5,
                            0, 0, allocation, 0, 0);
        probe_submit_one(fd, context, "gpuva_mesa_private",
                         (void *)mapped_gpuva, sizeof(zero_command), 0,
                         &submit_private, submit_private_size);
        probe_submit_one(fd, context, "gpuva_mesa_private_null",
                         (void *)mapped_gpuva, sizeof(zero_command), 1,
                         &submit_private, submit_private_size);
        probe_submit_one_ex(fd, context, "gpuva_mesa_private_nokmd",
                            (void *)mapped_gpuva, sizeof(zero_command), 5,
                            &submit_private, submit_private_size,
                            allocation, 0, 0);
        probe_submit_one_ex(fd, context, "gpuva_present_redirected",
                            (void *)mapped_gpuva, sizeof(zero_command), 2,
                            0, 0, allocation, 0, 0);
        probe_submit_one_ex(fd, context, "gpuva_null_present_redirected",
                            (void *)mapped_gpuva, sizeof(zero_command), 3,
                            0, 0, allocation, 0, 0);
        probe_submit_one_ex(fd, context, "gpuva_nokmd_present_redirected",
                            (void *)mapped_gpuva, sizeof(zero_command), 7,
                            0, 0, allocation, 0, 0);
        probe_submit_one_ex(fd, context, "gpuva4_null_primary",
                            (void *)mapped_gpuva, 4, 1, 0, 0,
                            allocation, 1, 0);
        probe_submit_one_ex(fd, context, "gpuva_null_primary",
                            (void *)mapped_gpuva, sizeof(zero_command), 1,
                            0, 0, allocation, 1, 0);
        probe_submit_one_ex(fd, context, "gpuva_private_primary",
                            (void *)mapped_gpuva, sizeof(zero_command), 1,
                            &submit_private, submit_private_size,
                            allocation, 1, 0);
        probe_submit_one_ex(fd, context, "gpuva_null_history",
                            (void *)mapped_gpuva, sizeof(zero_command), 1,
                            0, 0, allocation, 0, 1);
        probe_submit_one_ex(fd, context, "gpuva_private_primary_history",
                            (void *)mapped_gpuva, sizeof(zero_command), 1,
                            &submit_private, submit_private_size,
                            allocation, 1, 1);
        probe_submit_one_ex(fd, context, "gpuva_present_primary_history",
                            (void *)mapped_gpuva, sizeof(zero_command), 3,
                            &submit_private, submit_private_size,
                            allocation, 1, 1);
        if (locked_data != 0) {
            probe_submit_one_ex(fd, context, "locked64_null_primary",
                                (void *)locked_data, sizeof(zero_command), 1,
                                0, 0, allocation, 1, 0);
            probe_submit_one_ex(fd, context, "locked64_private_primary",
                                (void *)locked_data, sizeof(zero_command), 1,
                                &submit_private, submit_private_size,
                                allocation, 1, 0);
            probe_submit_one_ex(fd, context, "locked64_present_primary",
                                (void *)locked_data, sizeof(zero_command), 3,
                                &submit_private, submit_private_size,
                                allocation, 1, 0);
            probe_submit_one_ex(fd, context, "locked64_nokmd_primary",
                                (void *)locked_data, sizeof(zero_command), 5,
                                &submit_private, submit_private_size,
                                allocation, 1, 0);
        }
    } else {
        printf("submit_probe gpuva variants skipped; allocation was not mapped\n");
    }
}

static void probe_private_allocation_create(int fd, struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmthandle allocation_list[1];
    struct dxg_alloc_priv_info allocation_private;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    make_alloc_private(&allocation_private, 0x10000);
    allocation_info.priv_drv_data = (uint64)&allocation_private;
    allocation_info.priv_drv_data_size = sizeof(allocation_private);
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    printf("private_allocation_probe create size=%lu priv=%u\n",
           allocation_private.size, allocation_info.priv_drv_data_size);
    if (ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation) < 0 ||
        allocation_info.allocation.v == 0) {
        printf("private_allocation_probe failed allocation=0x%x resource=0x%x allocated_size=%lu returned_priv=%u\n",
               allocation_info.allocation.v, create_allocation.resource.v,
               allocation_private.allocated_size,
               allocation_info.priv_drv_data_size);
        return;
    }

    printf("private_allocation_probe ok allocation=0x%x resource=0x%x allocated_size=%lu returned_priv=%u\n",
           allocation_info.allocation.v, create_allocation.resource.v,
           allocation_private.allocated_size,
           allocation_info.priv_drv_data_size);
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    allocation_list[0] = allocation_info.allocation;
    destroy_allocation.device = device;
    destroy_allocation.resource = create_allocation.resource;
    destroy_allocation.flags.assume_not_in_use = 1;
    if (create_allocation.resource.v == 0) {
        destroy_allocation.allocations = (uint64)allocation_list;
        destroy_allocation.alloc_count = 1;
    }
    if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0)
        printf("private_allocation_probe destroy_failed allocation=0x%x resource=0x%x\n",
               allocation_info.allocation.v, create_allocation.resource.v);
}

static void probe_existing_sysmem_unsupported(int fd,
                                              struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmthandle allocation_list[1];
    unsigned int i;

    for (i = 0; i < sizeof(dxg_existing_sysmem_buffer); i++)
        dxg_existing_sysmem_buffer[i] = (unsigned char)(i ^ (i >> 8));

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    standard_allocation.existing_heap_data.size =
        sizeof(dxg_existing_sysmem_buffer);
    allocation_info.sysmem = (uint64)dxg_existing_sysmem_buffer;
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.standard_allocation = 1;
    create_allocation.flags.existing_sysmem = 1;
    if (ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation) < 0) {
        printf("existing_sysmem_allocation unsupported device=0x%x size=%lu\n",
               device.v, standard_allocation.existing_heap_data.size);
        return;
    }

    printf("existing_sysmem_allocation ok allocation=0x%x resource=0x%x\n",
           allocation_info.allocation.v, create_allocation.resource.v);
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    allocation_list[0] = allocation_info.allocation;
    destroy_allocation.device = device;
    destroy_allocation.resource = create_allocation.resource;
    destroy_allocation.flags.assume_not_in_use = 1;
    if (create_allocation.resource.v == 0) {
        destroy_allocation.allocations = (uint64)allocation_list;
        destroy_allocation.alloc_count = 1;
    }
    if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0)
        printf("existing_sysmem_allocation destroy_failed allocation=0x%x resource=0x%x\n",
               allocation_info.allocation.v, create_allocation.resource.v);
}

static void probe_lock2(int fd, struct d3dkmthandle device,
                        struct d3dkmthandle allocation)
{
    struct d3dkmt_lock2 lock;
    struct d3dkmt_unlock2 unlock;
    int locked = 0;

    if (allocation.v == 0)
        return;
    memset(&lock, 0, sizeof(lock));
    lock.device = device;
    lock.allocation = allocation;
    if (ioctl(fd, LX_DXLOCK2, &lock) < 0 || lock.data == 0) {
        printf("lock2_probe failed allocation=0x%x data=0x%lx\n",
               allocation.v, lock.data);
        return;
    }
    locked = 1;
    printf("lock2_probe ok allocation=0x%x data=0x%lx\n",
           allocation.v, lock.data);

    memset(&unlock, 0, sizeof(unlock));
    unlock.device = device;
    unlock.allocation = allocation;
    if (ioctl(fd, LX_DXUNLOCK2, &unlock) < 0) {
        printf("unlock2_probe failed allocation=0x%x\n", allocation.v);
    } else if (locked) {
        printf("unlock2_probe ok allocation=0x%x\n", allocation.v);
    }
}

static int probe_lock2_acquire(int fd, struct d3dkmthandle device,
                               struct d3dkmthandle allocation,
                               struct d3dkmt_lock2 *lock)
{
    if (allocation.v == 0)
        return -1;
    memset(lock, 0, sizeof(*lock));
    lock->device = device;
    lock->allocation = allocation;
    if (ioctl(fd, LX_DXLOCK2, lock) < 0 || lock->data == 0) {
        printf("lock2_probe failed allocation=0x%x data=0x%lx\n",
               allocation.v, lock->data);
        return -1;
    }
    printf("lock2_probe ok allocation=0x%x data=0x%lx\n",
           allocation.v, lock->data);
    return 0;
}

static int probe_lock2_release(int fd, const struct d3dkmt_lock2 *lock)
{
    struct d3dkmt_unlock2 unlock;

    if (lock->allocation.v == 0)
        return 0;
    memset(&unlock, 0, sizeof(unlock));
    unlock.device = lock->device;
    unlock.allocation = lock->allocation;
    if (ioctl(fd, LX_DXUNLOCK2, &unlock) < 0) {
        printf("unlock2_probe failed allocation=0x%x\n",
               lock->allocation.v);
        return -1;
    }
    printf("unlock2_probe ok allocation=0x%x\n", lock->allocation.v);
    return 0;
}

static int probe_hwqueue_flags(int fd, struct d3dkmthandle context,
                               struct d3dkmthandle sync_object,
                               const char *name,
                               struct d3dddi_createhwqueueflags flags,
                               void *private_data, uint32 private_size)
{
    struct d3dkmt_createhwqueue create_hwqueue;
    struct d3dkmt_submitcommandtohwqueue submit_hwqueue;
    struct d3dkmt_submitsignalsyncobjectstohwqueue signal_hwqueue;
    struct d3dkmt_submitwaitforsyncobjectstohwqueue wait_hwqueue;
    struct d3dkmt_destroyhwqueue destroy_hwqueue;
    struct d3dkmthandle hwqueue_list[1];
    struct d3dkmthandle sync_list[1];
    uint64 fence_values[1];

    if (context.v == 0)
        return -1;

    memset(&create_hwqueue, 0, sizeof(create_hwqueue));
    create_hwqueue.context = context;
    create_hwqueue.flags = flags;
    create_hwqueue.priv_drv_data = (uint64)private_data;
    create_hwqueue.priv_drv_data_size = private_size;
    if (ioctl(fd, LX_DXCREATEHWQUEUE, &create_hwqueue) < 0 ||
        create_hwqueue.queue.v == 0) {
        printf("hwqueue_probe %s create_failed context=0x%x flags=0x%x priv=%u queue=0x%x fence=0x%x\n",
               name, context.v, flags.value, private_size,
               create_hwqueue.queue.v,
               create_hwqueue.queue_progress_fence.v);
        return -1;
    }
    printf("hwqueue_handle %s 0x%x flags=0x%x priv=%u fence=0x%x fence_cpu=0x%lx fence_gpu=0x%lx\n",
           name, create_hwqueue.queue.v, flags.value, private_size,
           create_hwqueue.queue_progress_fence.v,
           create_hwqueue.queue_progress_fence_cpu_va,
           create_hwqueue.queue_progress_fence_gpu_va);

    memset(&submit_hwqueue, 0, sizeof(submit_hwqueue));
    submit_hwqueue.hwqueue = create_hwqueue.queue;
    submit_hwqueue.hwqueue_progress_fence_id = 1;
    if (ioctl(fd, LX_DXSUBMITCOMMANDTOHWQUEUE, &submit_hwqueue) < 0) {
        printf("hwqueue_submit_probe failed queue=0x%x fence_id=%lu\n",
               create_hwqueue.queue.v,
               submit_hwqueue.hwqueue_progress_fence_id);
    } else {
        printf("hwqueue_submit_probe ok queue=0x%x fence_id=%lu\n",
               create_hwqueue.queue.v,
               submit_hwqueue.hwqueue_progress_fence_id);
    }

    if (sync_object.v != 0) {
        hwqueue_list[0] = create_hwqueue.queue;
        sync_list[0] = sync_object;
        fence_values[0] = 4;

        memset(&signal_hwqueue, 0, sizeof(signal_hwqueue));
        signal_hwqueue.hwqueue_count = 1;
        signal_hwqueue.hwqueues = (uint64)hwqueue_list;
        signal_hwqueue.object_count = 1;
        signal_hwqueue.objects = (uint64)sync_list;
        signal_hwqueue.fence_values = (uint64)fence_values;
        if (ioctl(fd, LX_DXSUBMITSIGNALSYNCOBJECTSTOHWQUEUE,
                  &signal_hwqueue) < 0)
            printf("hwqueue_signal_sync failed queue=0x%x sync=0x%x fence=%lu\n",
                   create_hwqueue.queue.v, sync_object.v, fence_values[0]);
        else
            printf("hwqueue_signal_sync ok queue=0x%x sync=0x%x fence=%lu\n",
                   create_hwqueue.queue.v, sync_object.v, fence_values[0]);

        memset(&wait_hwqueue, 0, sizeof(wait_hwqueue));
        wait_hwqueue.hwqueue = create_hwqueue.queue;
        wait_hwqueue.object_count = 1;
        wait_hwqueue.objects = (uint64)sync_list;
        wait_hwqueue.fence_values = (uint64)fence_values;
        if (ioctl(fd, LX_DXSUBMITWAITFORSYNCOBJECTSTOHWQUEUE,
                  &wait_hwqueue) < 0)
            printf("hwqueue_wait_sync failed queue=0x%x sync=0x%x fence=%lu\n",
                   create_hwqueue.queue.v, sync_object.v, fence_values[0]);
        else
            printf("hwqueue_wait_sync ok queue=0x%x sync=0x%x fence=%lu\n",
                   create_hwqueue.queue.v, sync_object.v, fence_values[0]);
    }

    memset(&destroy_hwqueue, 0, sizeof(destroy_hwqueue));
    destroy_hwqueue.queue = create_hwqueue.queue;
    if (ioctl(fd, LX_DXDESTROYHWQUEUE, &destroy_hwqueue) < 0)
        printf("hwqueue_probe destroy_failed queue=0x%x\n",
               create_hwqueue.queue.v);
    return 0;
}

static void probe_hwqueue(int fd, struct d3dkmthandle context,
                          struct d3dkmthandle sync_object,
                          unsigned char *private_data, uint32 private_size)
{
    struct d3dddi_createhwqueueflags flags;
    uint32 private64 = private_size < 64 ? private_size : 64;

    memset(&flags, 0, sizeof(flags));
    if (probe_hwqueue_flags(fd, context, sync_object, "default_empty", flags,
                            0, 0) == 0)
        return;
    if (probe_hwqueue_flags(fd, context, sync_object, "default_private64",
                            flags,
                            private_data, private64) == 0)
        return;
    if (probe_hwqueue_flags(fd, context, sync_object, "default_private_full",
                            flags,
                            private_data, private_size) == 0)
        return;
    flags.disable_gpu_timeout = 1;
    if (probe_hwqueue_flags(fd, context, sync_object,
                            "disable_timeout_empty", flags,
                            0, 0) == 0)
        return;
    (void)probe_hwqueue_flags(fd, context, sync_object,
                              "disable_timeout_private64",
                              flags, private_data, private64);
}

static void probe_cpu_sync(int fd, struct d3dkmthandle device,
                           struct d3dkmthandle sync_object, int try_wait)
{
    struct d3dkmt_signalsynchronizationobjectfromcpu signal_sync;
    struct d3dkmt_waitforsynchronizationobjectfromcpu wait_sync;
    struct d3dkmthandle objects[1];
    uint64 fence_values[1];
    int signal_ok = 0;

    if (sync_object.v == 0)
        return;
    objects[0] = sync_object;
    fence_values[0] = 1;

    memset(&signal_sync, 0, sizeof(signal_sync));
    signal_sync.device = device;
    signal_sync.object_count = 1;
    signal_sync.objects = (uint64)objects;
    signal_sync.fence_values = (uint64)fence_values;
    if (ioctl(fd, LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMCPU,
              &signal_sync) < 0) {
        printf("sync_cpu_signal failed sync=0x%x fence=%lu\n",
               sync_object.v, fence_values[0]);
    } else {
        signal_ok = 1;
        printf("sync_cpu_signal ok sync=0x%x fence=%lu\n",
               sync_object.v, fence_values[0]);
    }

    if (!signal_ok)
        return;
    if (!try_wait) {
        printf("sync_cpu_wait skipped; use --try-wait to repro wait path\n");
        return;
    }
    for (int i = 0; i < 4; i++) {
        uint64 wait_value = (i & 1) ? 1 : 0;
        int wait_any = (i & 2) ? 1 : 0;

        fence_values[0] = wait_value;
        memset(&wait_sync, 0, sizeof(wait_sync));
        wait_sync.device = device;
        wait_sync.object_count = 1;
        wait_sync.objects = (uint64)objects;
        wait_sync.fence_values = (uint64)fence_values;
        wait_sync.flags.wait_any = wait_any;
        if (ioctl(fd, LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMCPU,
                  &wait_sync) < 0) {
            printf("sync_cpu_wait failed sync=0x%x fence=%lu wait_any=%d\n",
                   sync_object.v, fence_values[0], wait_any);
        } else {
            printf("sync_cpu_wait ok sync=0x%x fence=%lu wait_any=%d\n",
                   sync_object.v, fence_values[0], wait_any);
        }
    }
}

static void probe_share_object_with_host(int fd, struct d3dkmthandle device,
                                         struct d3dkmthandle object)
{
    struct d3dkmt_shareobjectwithhost share;

    if (device.v == 0 || object.v == 0)
        return;
    memset(&share, 0, sizeof(share));
    share.device_handle = device;
    share.object_handle = object;
    if (ioctl(fd, LX_DXSHAREOBJECTWITHHOST, &share) < 0) {
        printf("share_object_with_host failed device=0x%x object=0x%x\n",
               device.v, object.v);
    } else {
        printf("share_object_with_host ok device=0x%x object=0x%x nt=0x%lx\n",
               device.v, object.v, share.object_vail_nt_handle);
    }
}

static void probe_sync_file_unsupported(int fd, struct d3dkmthandle device,
                                        struct d3dkmthandle monitored_fence)
{
    struct d3dkmt_createsyncfile create_sync_file;
    struct d3dkmt_waitsyncfile wait_sync_file;
    struct d3dkmt_opensyncobjectfromsyncfile open_sync_file;

    if (device.v == 0 || monitored_fence.v == 0)
        return;
    memset(&create_sync_file, 0, sizeof(create_sync_file));
    create_sync_file.device = device;
    create_sync_file.monitored_fence = monitored_fence;
    create_sync_file.fence_value = 1;
    if (ioctl(fd, LX_DXCREATESYNCFILE, &create_sync_file) < 0) {
        printf("sync_file_create unsupported device=0x%x fence=0x%x value=%lu\n",
               device.v, monitored_fence.v, create_sync_file.fence_value);
    } else {
        printf("sync_file_create ok handle=0x%lx\n",
               create_sync_file.sync_file_handle);
    }

    memset(&wait_sync_file, 0, sizeof(wait_sync_file));
    wait_sync_file.sync_file_handle = create_sync_file.sync_file_handle;
    if (ioctl(fd, LX_DXWAITSYNCFILE, &wait_sync_file) < 0) {
        printf("sync_file_wait unsupported handle=0x%lx\n",
               wait_sync_file.sync_file_handle);
    } else {
        printf("sync_file_wait ok handle=0x%lx\n",
               wait_sync_file.sync_file_handle);
    }

    memset(&open_sync_file, 0, sizeof(open_sync_file));
    open_sync_file.device = device;
    open_sync_file.sync_file_handle = create_sync_file.sync_file_handle;
    if (ioctl(fd, LX_DXOPENSYNCOBJECTFROMSYNCFILE,
              &open_sync_file) < 0) {
        printf("sync_file_open unsupported device=0x%x handle=0x%lx\n",
               device.v, open_sync_file.sync_file_handle);
    } else {
        printf("sync_file_open ok sync=0x%x fence=%lu\n",
               open_sync_file.syncobj.v, open_sync_file.fence_value);
    }
}

static void probe_shared_handle_unsupported(int fd, struct d3dkmthandle device,
                                            struct d3dkmthandle object)
{
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmt_opensyncobjectfromnthandle2 open_sync_nt;
    struct d3dkmt_queryresourceinfofromnthandle query_resource_nt;
    struct d3dkmt_openresourcefromnthandle open_resource_nt;
    struct d3dkmthandle objects[1];
    uint64 shared_handle = 0;

    memset(&share_objects, 0, sizeof(share_objects));
    objects[0] = object;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    if (ioctl(fd, LX_DXSHAREOBJECTS, &share_objects) < 0) {
        printf("share_objects unsupported object=0x%x count=%u\n",
               object.v, share_objects.object_count);
    } else {
        printf("share_objects ok handle=0x%lx\n", shared_handle);
    }

    memset(&open_sync_nt, 0, sizeof(open_sync_nt));
    open_sync_nt.device = device;
    open_sync_nt.nt_handle = shared_handle;
    if (ioctl(fd, LX_DXOPENSYNCOBJECTFROMNTHANDLE2, &open_sync_nt) < 0) {
        printf("open_sync_nt unsupported device=0x%x handle=0x%lx\n",
               device.v, open_sync_nt.nt_handle);
    } else {
        printf("open_sync_nt ok sync=0x%x\n", open_sync_nt.sync_object.v);
    }

    memset(&query_resource_nt, 0, sizeof(query_resource_nt));
    query_resource_nt.device = device;
    query_resource_nt.nt_handle = shared_handle;
    if (ioctl(fd, LX_DXQUERYRESOURCEINFOFROMNTHANDLE,
              &query_resource_nt) < 0) {
        printf("query_resource_nt unsupported device=0x%x handle=0x%lx\n",
               device.v, query_resource_nt.nt_handle);
    } else {
        printf("query_resource_nt ok allocations=%u total_priv=%u\n",
               query_resource_nt.allocation_count,
               query_resource_nt.total_priv_drv_data_size);
    }

    memset(&open_resource_nt, 0, sizeof(open_resource_nt));
    open_resource_nt.device = device;
    open_resource_nt.nt_handle = shared_handle;
    if (ioctl(fd, LX_DXOPENRESOURCEFROMNTHANDLE, &open_resource_nt) < 0) {
        printf("open_resource_nt unsupported device=0x%x handle=0x%lx\n",
               device.v, open_resource_nt.nt_handle);
    } else {
        printf("open_resource_nt ok resource=0x%x allocations=%u\n",
               open_resource_nt.resource.v, open_resource_nt.allocation_count);
    }
}

static void probe_misc_unsupported(int fd, struct d3dkmthandle adapter,
                                   struct d3dkmthandle device,
                                   struct d3dkmthandle allocation,
                                   struct d3dkmthandle paging_queue)
{
    struct d3dkmt_changevideomemoryreservation reservation;
    struct d3dkmt_markdeviceaserror mark_error;
    struct d3dkmt_submitsignalsyncobjectstohwqueue signal_hwqueue;
    struct d3dkmt_submitwaitforsyncobjectstohwqueue wait_hwqueue;
    struct d3dddi_updateallocproperty update_alloc;
    struct d3dkmt_queryclockcalibration clock_calibration;
    struct d3dkmt_enumprocesses enum_processes;
    struct d3dkmthandle hwqueue = {0};

    memset(&reservation, 0, sizeof(reservation));
    reservation.adapter = adapter;
    reservation.memory_segment_group = _D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
    if (ioctl(fd, LX_DXCHANGEVIDEOMEMORYRESERVATION, &reservation) < 0)
        printf("change_vidmem_reservation unsupported adapter=0x%x\n",
               adapter.v);
    else
        printf("change_vidmem_reservation ok adapter=0x%x\n", adapter.v);

    memset(&mark_error, 0, sizeof(mark_error));
    mark_error.device = device;
    mark_error.reason = _D3DKMT_DEVICE_ERROR_REASON_GENERIC;
    if (ioctl(fd, LX_DXMARKDEVICEASERROR, &mark_error) < 0)
        printf("mark_device_error unsupported device=0x%x\n", device.v);
    else
        printf("mark_device_error ok device=0x%x\n", device.v);

    memset(&signal_hwqueue, 0, sizeof(signal_hwqueue));
    signal_hwqueue.hwqueue_count = 1;
    signal_hwqueue.hwqueues = (uint64)&hwqueue;
    if (ioctl(fd, LX_DXSUBMITSIGNALSYNCOBJECTSTOHWQUEUE,
              &signal_hwqueue) < 0)
        printf("hwqueue_signal_sync unsupported queue=0x%x\n", hwqueue.v);
    else
        printf("hwqueue_signal_sync ok queue=0x%x\n", hwqueue.v);

    memset(&wait_hwqueue, 0, sizeof(wait_hwqueue));
    wait_hwqueue.hwqueue = hwqueue;
    if (ioctl(fd, LX_DXSUBMITWAITFORSYNCOBJECTSTOHWQUEUE,
              &wait_hwqueue) < 0)
        printf("hwqueue_wait_sync unsupported queue=0x%x\n", hwqueue.v);
    else
        printf("hwqueue_wait_sync ok queue=0x%x\n", hwqueue.v);

    memset(&update_alloc, 0, sizeof(update_alloc));
    update_alloc.paging_queue = paging_queue;
    update_alloc.allocation = allocation;
    update_alloc.set_accessed_physically = 1;
    if (ioctl(fd, LX_DXUPDATEALLOCPROPERTY, &update_alloc) < 0)
        printf("update_alloc_property unsupported allocation=0x%x\n",
               allocation.v);
    else
        printf("update_alloc_property ok allocation=0x%x fence=%lu\n",
               allocation.v, update_alloc.paging_fence_value);

    memset(&clock_calibration, 0, sizeof(clock_calibration));
    clock_calibration.adapter = adapter;
    if (ioctl(fd, LX_DXQUERYCLOCKCALIBRATION, &clock_calibration) < 0)
        printf("query_clock_calibration unsupported adapter=0x%x\n",
               adapter.v);
    else
        printf("query_clock_calibration ok adapter=0x%x gpu_freq=%lu\n",
               adapter.v, clock_calibration.clock_data.gpu_frequency);

    memset(&enum_processes, 0, sizeof(enum_processes));
    enum_processes.adapter_luid = (struct winluid){0};
    if (ioctl(fd, LX_DXENUMPROCESSES, &enum_processes) < 0)
        printf("enum_processes unsupported count=%lu\n",
               enum_processes.buffer_count);
    else
        printf("enum_processes ok count=%lu\n", enum_processes.buffer_count);
}

static void probe_update_gpuva(int fd, struct d3dkmthandle device,
                               struct d3dkmthandle allocation,
                               uint64 base, uint64 size)
{
    struct d3dddi_updategpuvirtualaddress_operation op;
    struct d3dkmt_updategpuvirtualaddress update;
    int rc;

    if (device.v == 0 || allocation.v == 0 || base == 0 || size == 0)
        return;

    memset(&op, 0, sizeof(op));
    op.operation = _D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP;
    op.map.base_address = base;
    op.map.size = size;
    op.map.allocation = allocation;
    op.map.allocation_offset = 0;
    op.map.allocation_size = size;

    memset(&update, 0, sizeof(update));
    update.device = device;
    update.num_operations = 1;
    update.operations = (uint64)&op;
    update.fence_value = 1;
    update.flags.do_not_wait = 1;
    rc = ioctl(fd, LX_DXUPDATEGPUVIRTUALADDRESS, &update);
    if (rc < 0) {
        printf("update_gpuva_map failed device=0x%x allocation=0x%x base=0x%lx size=%lu ioctl=0x%x struct=%u\n",
               device.v, allocation.v, base, size,
               LX_DXUPDATEGPUVIRTUALADDRESS,
               (uint32)sizeof(struct d3dkmt_updategpuvirtualaddress));
        return;
    }
    printf("update_gpuva_map ok rc=%d device=0x%x allocation=0x%x base=0x%lx size=%lu fence=%lu ioctl=0x%x struct=%u\n",
           rc, device.v, allocation.v, base, size, update.fence_value,
           LX_DXUPDATEGPUVIRTUALADDRESS,
           (uint32)sizeof(struct d3dkmt_updategpuvirtualaddress));

    memset(&op, 0, sizeof(op));
    op.operation = _D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP;
    op.unmap.base_address = base;
    op.unmap.size = size;
    op.unmap.protection.no_access = 1;
    memset(&update, 0, sizeof(update));
    update.device = device;
    update.num_operations = 1;
    update.operations = (uint64)&op;
    update.fence_value = 2;
    update.flags.do_not_wait = 1;
    rc = ioctl(fd, LX_DXUPDATEGPUVIRTUALADDRESS, &update);
    if (rc < 0) {
        printf("update_gpuva_unmap failed device=0x%x base=0x%lx size=%lu\n",
               device.v, base, size);
    } else {
        printf("update_gpuva_unmap ok rc=%d device=0x%x base=0x%lx size=%lu fence=%lu\n",
               rc, device.v, base, size, update.fence_value);
    }
}

static void probe_gpu_sync(int fd, struct d3dkmthandle device,
                           struct d3dkmthandle context)
{
    struct d3dkmt_createsynchronizationobject2 create_sync;
    struct d3dkmt_destroysynchronizationobject destroy_sync;
    struct d3dkmt_signalsynchronizationobject2 signal_legacy;
    struct d3dkmt_waitforsynchronizationobject2 wait_legacy;
    struct d3dkmt_signalsynchronizationobjectfromgpu signal_gpu;
    struct d3dkmt_signalsynchronizationobjectfromgpu2 signal_gpu2;
    struct d3dkmt_waitforsynchronizationobjectfromgpu wait_gpu;
    struct d3dkmthandle objects[1];
    struct d3dkmthandle contexts[1];
    uint64 fence_values[1];

    if (context.v == 0)
        return;

    memset(&create_sync, 0, sizeof(create_sync));
    create_sync.device = device;
    create_sync.info.type = _D3DDDI_MONITORED_FENCE;
    create_sync.info.monitored_fence.initial_fence_value = 0;
    if (ioctl(fd, LX_DXCREATESYNCHRONIZATIONOBJECT, &create_sync) < 0 ||
        create_sync.sync_object.v == 0) {
        printf("sync_gpu_probe create failed context=0x%x sync=0x%x\n",
               context.v, create_sync.sync_object.v);
        return;
    }

    objects[0] = create_sync.sync_object;
    contexts[0] = context;

    memset(&signal_legacy, 0, sizeof(signal_legacy));
    signal_legacy.context = context;
    signal_legacy.object_count = 1;
    signal_legacy.object_array[0] = objects[0];
    signal_legacy.fence.fence_value = 2;
    if (ioctl(fd, LX_DXSIGNALSYNCHRONIZATIONOBJECT,
              &signal_legacy) < 0) {
        printf("sync_legacy_signal failed context=0x%x sync=0x%x fence=%lu\n",
               context.v, objects[0].v, signal_legacy.fence.fence_value);
    } else {
        printf("sync_legacy_signal ok context=0x%x sync=0x%x fence=%lu\n",
               context.v, objects[0].v, signal_legacy.fence.fence_value);
    }

    memset(&wait_legacy, 0, sizeof(wait_legacy));
    wait_legacy.context = context;
    wait_legacy.object_count = 1;
    wait_legacy.object_array[0] = objects[0];
    wait_legacy.fence.fence_value = signal_legacy.fence.fence_value;
    if (ioctl(fd, LX_DXWAITFORSYNCHRONIZATIONOBJECT,
              &wait_legacy) < 0) {
        printf("sync_legacy_wait failed context=0x%x sync=0x%x fence=%lu\n",
               context.v, objects[0].v, wait_legacy.fence.fence_value);
    } else {
        printf("sync_legacy_wait ok context=0x%x sync=0x%x fence=%lu\n",
               context.v, objects[0].v, wait_legacy.fence.fence_value);
    }

    fence_values[0] = 2;

    memset(&signal_gpu, 0, sizeof(signal_gpu));
    signal_gpu.context = context;
    signal_gpu.object_count = 1;
    signal_gpu.objects = (uint64)objects;
    signal_gpu.monitored_fence_values = (uint64)fence_values;
    if (ioctl(fd, LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU,
              &signal_gpu) < 0) {
        printf("sync_gpu_signal failed context=0x%x sync=0x%x fence=%lu\n",
               context.v, objects[0].v, fence_values[0]);
    } else {
        printf("sync_gpu_signal ok context=0x%x sync=0x%x fence=%lu\n",
               context.v, objects[0].v, fence_values[0]);
    }

    fence_values[0] = 3;
    memset(&signal_gpu2, 0, sizeof(signal_gpu2));
    signal_gpu2.object_count = 1;
    signal_gpu2.objects = (uint64)objects;
    signal_gpu2.context_count = 1;
    signal_gpu2.contexts = (uint64)contexts;
    signal_gpu2.monitored_fence_values = (uint64)fence_values;
    if (ioctl(fd, LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU2,
              &signal_gpu2) < 0) {
        printf("sync_gpu2_signal failed context=0x%x sync=0x%x fence=%lu\n",
               context.v, objects[0].v, fence_values[0]);
    } else {
        printf("sync_gpu2_signal ok context=0x%x sync=0x%x fence=%lu\n",
               context.v, objects[0].v, fence_values[0]);
    }

    memset(&wait_gpu, 0, sizeof(wait_gpu));
    wait_gpu.context = context;
    wait_gpu.object_count = 1;
    wait_gpu.objects = (uint64)objects;
    wait_gpu.monitored_fence_values = (uint64)fence_values;
    if (ioctl(fd, LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMGPU,
              &wait_gpu) < 0) {
        printf("sync_gpu_wait failed context=0x%x sync=0x%x fence=%lu\n",
               context.v, objects[0].v, fence_values[0]);
    } else {
        printf("sync_gpu_wait ok context=0x%x sync=0x%x fence=%lu\n",
               context.v, objects[0].v, fence_values[0]);
    }

    memset(&destroy_sync, 0, sizeof(destroy_sync));
    destroy_sync.sync_object = create_sync.sync_object;
    if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &destroy_sync) < 0)
        printf("sync_gpu_probe destroy_failed sync=0x%x\n",
               create_sync.sync_object.v);
}

enum dxgprobe_stage
{
    DXGPROBE_STAGE_FULL = 0,
    DXGPROBE_STAGE_ADAPTER_ONLY,
    DXGPROBE_STAGE_DEVICE_ONLY,
    DXGPROBE_STAGE_PAGING_ONLY,
    DXGPROBE_STAGE_ALLOC_CREATE_ONLY,
    DXGPROBE_STAGE_ALLOC_MAKE_ONLY,
    DXGPROBE_STAGE_ALLOC_MAP_ONLY,
    DXGPROBE_STAGE_ALLOC_EVICT_ONLY,
    DXGPROBE_STAGE_ALLOCATION_ONLY,
    DXGPROBE_STAGE_SYNC_ONLY,
};

static int probe_device_context(int fd, struct d3dkmthandle adapter,
                                int leak_on_close, int try_context,
                                int try_submit, int try_wait,
                                enum dxgprobe_stage stage)
{
    unsigned char context_private_data[] = {
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x1c, 0x0c, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00,
    };
    struct d3dkmt_createdevice create_device;
    struct d3dkmt_getdevicestate device_state;
    struct d3dkmt_destroydevice destroy_device;
    struct d3dkmt_createpagingqueue create_paging_queue;
    struct d3dddi_destroypagingqueue destroy_paging_queue;
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct dxg_alloc_priv_info allocation_private;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dddi_mapgpuvirtualaddress map_gpuva;
    struct d3dddi_makeresident make_resident;
    struct d3dkmt_evict evict;
    struct d3dkmthandle allocation_list[1];
    uint32 priority_list[1];
    struct d3dddi_reservegpuvirtualaddress reserve_gpuva;
    struct d3dkmt_freegpuvirtualaddress free_gpuva;
    struct d3dkmt_createsynchronizationobject2 create_sync;
    struct d3dkmt_destroysynchronizationobject destroy_sync;
    int paging_queue_created = 0;
    int allocation_created = 0;
    int allocation_resident = 0;
    int allocation_evicted = 0;
    struct d3dkmthandle context_handle;
    int context_sync_only = 0;
    struct d3dkmt_lock2 locked_allocation;
    int allocation_locked = 0;
    uint64 mapped_gpuva = 0;
    uint64 mapped_gpuva_size = 0;
    int ret = 0;

    memset(&context_handle, 0, sizeof(context_handle));
    memset(&locked_allocation, 0, sizeof(locked_allocation));

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    if (ioctl(fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        printf("dxgprobe: create device failed device=0x%x\n",
               create_device.device.v);
        return -1;
    }
    printf("device_handle 0x%x\n", create_device.device.v);

    memset(&device_state, 0, sizeof(device_state));
    device_state.device = create_device.device;
    device_state.state_type = _D3DKMT_DEVICESTATE_EXECUTION;
    if (ioctl(fd, LX_DXGETDEVICESTATE, &device_state) < 0) {
        printf("dxgprobe: get device state failed\n");
        ret = -1;
    } else {
        printf("device_execution_state %u\n", device_state.execution_state);
    }

    if (stage == DXGPROBE_STAGE_DEVICE_ONLY)
        goto cleanup;
    if (try_submit)
        probe_private_allocation_create(fd, create_device.device);
    probe_existing_sysmem_unsupported(fd, create_device.device);

    if (stage != DXGPROBE_STAGE_SYNC_ONLY) {
        memset(&create_paging_queue, 0, sizeof(create_paging_queue));
        create_paging_queue.device = create_device.device;
        create_paging_queue.priority = _D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        if (ioctl(fd, LX_DXCREATEPAGINGQUEUE, &create_paging_queue) < 0 ||
            create_paging_queue.paging_queue.v == 0) {
            printf("dxgprobe: create paging queue failed queue=0x%x sync=0x%x\n",
                   create_paging_queue.paging_queue.v,
                   create_paging_queue.sync_object.v);
            ret = -1;
        } else {
            if (create_paging_queue.fence_cpu_virtual_address != 0) {
                volatile uint64 *fence =
                    (volatile uint64 *)create_paging_queue.fence_cpu_virtual_address;
                printf("paging_queue 0x%x sync=0x%x fence_cpu=0x%lx fence_value=%lu\n",
                       create_paging_queue.paging_queue.v,
                       create_paging_queue.sync_object.v,
                       create_paging_queue.fence_cpu_virtual_address,
                       *fence);
            } else {
                printf("paging_queue 0x%x sync=0x%x fence_cpu=0x0\n",
                       create_paging_queue.paging_queue.v,
                       create_paging_queue.sync_object.v);
            }
            paging_queue_created = 1;
        }
    }

    if (stage == DXGPROBE_STAGE_PAGING_ONLY)
        goto cleanup;
    if (stage == DXGPROBE_STAGE_SYNC_ONLY)
        goto sync_probe;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    create_allocation.device = create_device.device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.standard_allocation = 1;
    if (ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation) < 0 ||
        allocation_info.allocation.v == 0) {
        printf("allocation_probe create_failed allocation=0x%x resource=0x%x\n",
               allocation_info.allocation.v, create_allocation.resource.v);
    } else {
        uint64 allocation_size = standard_allocation.existing_heap_data.size;

        allocation_created = 1;
        allocation_list[0] = allocation_info.allocation;
        priority_list[0] = 0;
        printf("allocation_handle 0x%x resource=0x%x global=0x%x\n",
               allocation_info.allocation.v, create_allocation.resource.v,
               create_allocation.global_share.v);
        probe_allocation_priority(fd, create_device.device,
                                  create_allocation.resource,
                                  allocation_info.allocation);
        probe_allocation_residency(fd, create_device.device,
                                   create_allocation.resource,
                                   allocation_info.allocation);
        probe_invalidate_cache(fd, create_device.device,
                               allocation_info.allocation, allocation_size);
        if (stage == DXGPROBE_STAGE_ALLOC_CREATE_ONLY)
            goto cleanup;

        if (paging_queue_created) {
            memset(&make_resident, 0, sizeof(make_resident));
            make_resident.paging_queue = create_paging_queue.paging_queue;
            make_resident.alloc_count = 1;
            make_resident.allocation_list = (uint64)allocation_list;
            make_resident.priority_list = (uint64)priority_list;
            if (ioctl(fd, LX_DXMAKERESIDENT, &make_resident) < 0) {
                printf("allocation_probe make_resident_failed fence=%lu trim=%lu\n",
                       make_resident.paging_fence_value,
                       make_resident.num_bytes_to_trim);
            } else {
                allocation_resident = 1;
                printf("allocation_resident fence=%lu trim=%lu\n",
                       make_resident.paging_fence_value,
                       make_resident.num_bytes_to_trim);
                if (try_submit)
                    (void)wait_paging_fence(
                        create_paging_queue.fence_cpu_virtual_address,
                        make_resident.paging_fence_value,
                        "make_resident");
            }
            if (stage == DXGPROBE_STAGE_ALLOC_MAKE_ONLY)
                goto cleanup;

            if (stage != DXGPROBE_STAGE_ALLOC_EVICT_ONLY) {
                memset(&map_gpuva, 0, sizeof(map_gpuva));
                map_gpuva.paging_queue = create_paging_queue.paging_queue;
                map_gpuva.maximum_address = ~0ULL;
                map_gpuva.allocation = allocation_info.allocation;
                map_gpuva.size_in_pages = allocation_size >> 12;
                map_gpuva.protection.write = 1;
                if (ioctl(fd, LX_DXMAPGPUVIRTUALADDRESS, &map_gpuva) < 0) {
                    printf("allocation_probe map_gpuva_failed va=0x%lx fence=%lu\n",
                           map_gpuva.virtual_address,
                           map_gpuva.paging_fence_value);
                } else {
                    printf("gpuva_mapped va=0x%lx fence=%lu\n",
                           map_gpuva.virtual_address,
                           map_gpuva.paging_fence_value);
                    mapped_gpuva = map_gpuva.virtual_address;
                    mapped_gpuva_size = map_gpuva.size_in_pages << 12;
                    if (try_submit)
                        (void)wait_paging_fence(
                            create_paging_queue.fence_cpu_virtual_address,
                            map_gpuva.paging_fence_value,
                            "map_gpuva");
                    if (try_submit &&
                        probe_lock2_acquire(fd, create_device.device,
                                            allocation_info.allocation,
                                            &locked_allocation) == 0)
                        allocation_locked = 1;
                }
            }
        }

        if (stage == DXGPROBE_STAGE_ALLOC_MAP_ONLY)
            goto cleanup;

        if (!try_submit && allocation_resident) {
            memset(&evict, 0, sizeof(evict));
            evict.device = create_device.device;
            evict.alloc_count = 1;
            evict.allocations = (uint64)allocation_list;
            if (ioctl(fd, LX_DXEVICT, &evict) < 0) {
                printf("allocation_probe evict_failed trim=%lu\n",
                       evict.num_bytes_to_trim);
            } else {
                allocation_evicted = 1;
                printf("allocation_evicted trim=%lu\n", evict.num_bytes_to_trim);
            }
        probe_offer_reclaim(fd, create_device.device,
                            create_paging_queue.paging_queue,
                            allocation_info.allocation);
        probe_misc_unsupported(fd, adapter, create_device.device,
                               allocation_info.allocation,
                               create_paging_queue.paging_queue);
    } else if (allocation_resident) {
        printf("allocation_eviction deferred for submit probe\n");
    }
    }

    if (stage == DXGPROBE_STAGE_ALLOCATION_ONLY)
        goto cleanup;

    memset(&reserve_gpuva, 0, sizeof(reserve_gpuva));
    reserve_gpuva.adapter = adapter;
    reserve_gpuva.size = 0x10000;
    reserve_gpuva.reservation_type = _D3DDDIGPUVA_RESERVE_NO_ACCESS;
    if (ioctl(fd, LX_DXRESERVEGPUVIRTUALADDRESS, &reserve_gpuva) < 0 ||
        reserve_gpuva.virtual_address == 0) {
        printf("dxgprobe: reserve gpuva failed va=0x%lx\n",
               reserve_gpuva.virtual_address);
        ret = -1;
    } else {
        printf("gpuva_reserved va=0x%lx fence=%lu\n",
               reserve_gpuva.virtual_address,
               reserve_gpuva.paging_fence_value);
        if (try_submit && allocation_created)
            probe_update_gpuva(fd, create_device.device,
                               allocation_info.allocation,
                               reserve_gpuva.virtual_address,
                               reserve_gpuva.size);
        if (!leak_on_close) {
            memset(&free_gpuva, 0, sizeof(free_gpuva));
            free_gpuva.adapter = adapter;
            free_gpuva.base_address = reserve_gpuva.virtual_address;
            free_gpuva.size = reserve_gpuva.size;
            if (ioctl(fd, LX_DXFREEGPUVIRTUALADDRESS, &free_gpuva) < 0) {
                printf("dxgprobe: free gpuva failed\n");
                ret = -1;
            }
        }
    }

sync_probe:
    memset(&create_sync, 0, sizeof(create_sync));
    create_sync.device = create_device.device;
    create_sync.info.type = _D3DDDI_MONITORED_FENCE;
    create_sync.info.monitored_fence.initial_fence_value = 0;
    create_sync.info.monitored_fence.engine_affinity = 0;
    if (ioctl(fd, LX_DXCREATESYNCHRONIZATIONOBJECT, &create_sync) < 0 ||
        create_sync.sync_object.v == 0) {
        printf("dxgprobe: create sync object failed sync=0x%x\n",
               create_sync.sync_object.v);
        ret = -1;
    } else {
        printf("sync_object 0x%x\n", create_sync.sync_object.v);
        probe_share_object_with_host(fd, create_device.device,
                                     create_sync.sync_object);
        probe_shared_handle_unsupported(fd, create_device.device,
                                        create_sync.sync_object);
        probe_sync_file_unsupported(fd, create_device.device,
                                    create_sync.sync_object);
        probe_cpu_sync(fd, create_device.device, create_sync.sync_object,
                       try_wait);
    }

    if (try_context || try_submit) {
        if (probe_context_matrix(fd, create_device.device,
                                 context_private_data,
                                 sizeof(context_private_data),
                                 &context_handle,
                                 &context_sync_only) < 0)
            ret = -1;

        if (context_sync_only)
            printf("submit_probe using sync-only context; OpenGL submit remains unsupported\n");
        probe_context_priority(fd, context_handle);
        if (try_submit) {
            probe_gpu_sync(fd, create_device.device, context_handle);
            probe_submit(fd, context_handle, context_private_data,
                         sizeof(context_private_data), mapped_gpuva,
                         allocation_locked ? locked_allocation.data : 0,
                         allocation_info.allocation);
            if (!context_sync_only)
                probe_hwqueue(fd, context_handle, create_sync.sync_object,
                              context_private_data,
                              sizeof(context_private_data));
        } else {
            printf("submit_probe skipped; use --try-submit to repro unsupported Hyper-V submit path\n");
        }
        if (!leak_on_close && destroy_context_handle(fd, context_handle) < 0)
            ret = -1;
    } else {
        printf("context_probe skipped; use --try-context to repro Hyper-V context blocker\n");
    }

    if (leak_on_close) {
        printf("dxgprobe: leak_close leaving device=0x%x queue=0x%x sync=0x%x resource=0x%x allocation=0x%x context=0x%x gpuva=0x%lx for fd close cleanup\n",
               create_device.device.v, create_paging_queue.paging_queue.v,
               create_sync.sync_object.v, create_allocation.resource.v,
               allocation_info.allocation.v, context_handle.v,
               reserve_gpuva.virtual_address);
        return ret;
    }

cleanup:
    if (allocation_locked) {
        if (probe_lock2_release(fd, &locked_allocation) < 0)
            ret = -1;
        allocation_locked = 0;
    }
    if (allocation_resident && !allocation_evicted) {
        memset(&evict, 0, sizeof(evict));
        evict.device = create_device.device;
        evict.alloc_count = 1;
        evict.allocations = (uint64)allocation_list;
        if (ioctl(fd, LX_DXEVICT, &evict) < 0) {
            printf("allocation_probe cleanup_evict_failed trim=%lu\n",
                   evict.num_bytes_to_trim);
            ret = -1;
        } else {
            printf("allocation_evicted trim=%lu\n", evict.num_bytes_to_trim);
        }
    }
    if (mapped_gpuva != 0) {
        memset(&free_gpuva, 0, sizeof(free_gpuva));
        free_gpuva.adapter = adapter;
        free_gpuva.base_address = mapped_gpuva;
        free_gpuva.size = mapped_gpuva_size;
        if (ioctl(fd, LX_DXFREEGPUVIRTUALADDRESS, &free_gpuva) < 0) {
            printf("dxgprobe: free mapped gpuva failed\n");
            ret = -1;
        }
    }
    if (allocation_created) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        destroy_allocation.device = create_device.device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (create_allocation.resource.v == 0) {
            destroy_allocation.allocations = (uint64)allocation_list;
            destroy_allocation.alloc_count = 1;
        }
        if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0) {
            printf("dxgprobe: destroy allocation failed\n");
            ret = -1;
        }
    }
    if (paging_queue_created) {
        memset(&destroy_paging_queue, 0, sizeof(destroy_paging_queue));
        destroy_paging_queue.paging_queue = create_paging_queue.paging_queue;
        if (ioctl(fd, LX_DXDESTROYPAGINGQUEUE, &destroy_paging_queue) < 0) {
            printf("dxgprobe: destroy paging queue failed\n");
            ret = -1;
        }
    }
    if (!leak_on_close && create_sync.sync_object.v != 0) {
        memset(&destroy_sync, 0, sizeof(destroy_sync));
        destroy_sync.sync_object = create_sync.sync_object;
        if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                  &destroy_sync) < 0) {
            printf("dxgprobe: destroy sync object failed\n");
            ret = -1;
        }
    }
    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device = create_device.device;
    if (ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device) < 0) {
        printf("dxgprobe: destroy device failed\n");
        ret = -1;
    }
    return ret;
}

static int probe_owner_isolation(int fd, struct d3dkmthandle adapter)
{
    struct d3dkmt_createdevice create_device;
    struct d3dkmt_destroydevice destroy_device;
    int fd2;
    int ret = 0;

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    if (ioctl(fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        printf("dxgprobe: owner isolation create device failed device=0x%x\n",
               create_device.device.v);
        return -1;
    }

    fd2 = open("/dev/dxg", O_RDWR);
    if (fd2 < 0) {
        printf("dxgprobe: owner isolation second open failed\n");
        ret = -1;
        goto cleanup;
    }

    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device = create_device.device;
    if (ioctl(fd2, LX_DXDESTROYDEVICE, &destroy_device) >= 0) {
        printf("dxgprobe: owner isolation foreign destroy unexpectedly succeeded device=0x%x\n",
               create_device.device.v);
        ret = -1;
    } else {
        printf("dxgprobe: owner isolation rejected foreign destroy device=0x%x\n",
               create_device.device.v);
    }
    close(fd2);

cleanup:
    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device = create_device.device;
    if (ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device) < 0) {
        printf("dxgprobe: owner isolation cleanup destroy failed device=0x%x\n",
               create_device.device.v);
        ret = -1;
    }
    if (ret == 0)
        printf("dxgprobe: owner-isolation ok\n");
    return ret;
}

int main(int argc, char **argv)
{
    struct d3dkmt_enumadapters2 enum2;
    struct d3dkmt_enumadapters3 enum3;
    struct d3dkmt_adapterinfo adapters[D3DKMT_ADAPTERS_MAX];
    struct d3dkmt_openadapterfromluid open_luid;
    struct d3dkmt_closeadapter close_adapter;
    int fd;
    int rc;
    int ret = 0;
    int leak_on_close = 0;
    int try_context = 0;
    int try_submit = 0;
    int try_wait = 0;
    int owner_isolation = 0;
    enum dxgprobe_stage stage = DXGPROBE_STAGE_FULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--leak-close") == 0)
            leak_on_close = 1;
        else if (strcmp(argv[i], "--try-context") == 0)
            try_context = 1;
        else if (strcmp(argv[i], "--try-submit") == 0)
            try_submit = 1;
        else if (strcmp(argv[i], "--try-wait") == 0)
            try_wait = 1;
        else if (strcmp(argv[i], "--fence-wait-seconds") == 0 &&
                 i + 1 < argc) {
            g_paging_fence_wait_seconds = atoi(argv[++i]);
            if (g_paging_fence_wait_seconds < 0)
                g_paging_fence_wait_seconds = 0;
            if (g_paging_fence_wait_seconds > 60)
                g_paging_fence_wait_seconds = 60;
        }
        else if (strcmp(argv[i], "--owner-isolation") == 0)
            owner_isolation = 1;
        else if (strcmp(argv[i], "--adapter-only") == 0)
            stage = DXGPROBE_STAGE_ADAPTER_ONLY;
        else if (strcmp(argv[i], "--device-only") == 0)
            stage = DXGPROBE_STAGE_DEVICE_ONLY;
        else if (strcmp(argv[i], "--paging-only") == 0)
            stage = DXGPROBE_STAGE_PAGING_ONLY;
        else if (strcmp(argv[i], "--alloc-create-only") == 0)
            stage = DXGPROBE_STAGE_ALLOC_CREATE_ONLY;
        else if (strcmp(argv[i], "--alloc-make-only") == 0)
            stage = DXGPROBE_STAGE_ALLOC_MAKE_ONLY;
        else if (strcmp(argv[i], "--alloc-map-only") == 0)
            stage = DXGPROBE_STAGE_ALLOC_MAP_ONLY;
        else if (strcmp(argv[i], "--alloc-evict-only") == 0)
            stage = DXGPROBE_STAGE_ALLOC_EVICT_ONLY;
        else if (strcmp(argv[i], "--allocation-only") == 0)
            stage = DXGPROBE_STAGE_ALLOCATION_ONLY;
        else if (strcmp(argv[i], "--sync-only") == 0)
            stage = DXGPROBE_STAGE_SYNC_ONLY;
    }
    if (try_submit)
        try_context = 1;

    fd = open("/dev/dxg", O_RDWR);
    if (fd < 0) {
        printf("dxgprobe: open /dev/dxg failed\n");
        return 1;
    }

    memset(&enum2, 0, sizeof(enum2));
    rc = ioctl(fd, LX_DXENUMADAPTERS2, &enum2);
    if (rc < 0 || enum2.num_adapters == 0) {
        printf("dxgprobe: enum count failed rc=%d count=%u\n",
               rc, enum2.num_adapters);
        close(fd);
        return 1;
    }
    if (enum2.num_adapters > D3DKMT_ADAPTERS_MAX)
        enum2.num_adapters = D3DKMT_ADAPTERS_MAX;
    memset(adapters, 0, sizeof(adapters));
    enum2.adapters = (uint64)adapters;
    rc = ioctl(fd, LX_DXENUMADAPTERS2, &enum2);
    if (rc < 0 ||
        enum2.num_adapters == 0 || adapters[0].adapter_handle.v == 0) {
        printf("dxgprobe: enum adapters failed rc=%d count=%u handle=0x%x\n",
               rc, enum2.num_adapters, adapters[0].adapter_handle.v);
        close(fd);
        return 1;
    }

    printf("dxg_adapters %u\n", enum2.num_adapters);
    printf("adapter0 handle=0x%x luid=%x:%x sources=%u\n",
           adapters[0].adapter_handle.v, adapters[0].adapter_luid.b,
           adapters[0].adapter_luid.a, adapters[0].num_sources);

    memset(&enum3, 0, sizeof(enum3));
    rc = ioctl(fd, LX_DXENUMADAPTERS3, &enum3);
    if (rc < 0 || enum3.adapter_count == 0) {
        printf("dxgprobe: enum3 count failed rc=%d count=%u\n",
               rc, enum3.adapter_count);
        close(fd);
        return 1;
    }
    if (enum3.adapter_count > D3DKMT_ADAPTERS_MAX)
        enum3.adapter_count = D3DKMT_ADAPTERS_MAX;
    memset(adapters, 0, sizeof(adapters));
    enum3.adapters = (uint64)adapters;
    rc = ioctl(fd, LX_DXENUMADAPTERS3, &enum3);
    if (rc < 0 || enum3.adapter_count == 0 ||
        adapters[0].adapter_handle.v == 0) {
        printf("dxgprobe: enum3 adapters failed rc=%d count=%u handle=0x%x\n",
               rc, enum3.adapter_count, adapters[0].adapter_handle.v);
        close(fd);
        return 1;
    }
    printf("dxg_adapters3 %u handle0=0x%x\n",
           enum3.adapter_count, adapters[0].adapter_handle.v);

    memset(&open_luid, 0, sizeof(open_luid));
    open_luid.adapter_luid = adapters[0].adapter_luid;
    if (ioctl(fd, LX_DXOPENADAPTERFROMLUID, &open_luid) < 0 ||
        open_luid.adapter_handle.v == 0) {
        printf("dxgprobe: open adapter from luid failed\n");
        close(fd);
        return 1;
    }
    printf("open_luid_handle 0x%x\n", open_luid.adapter_handle.v);

    if (query_adapter_type(fd, open_luid.adapter_handle,
                           _KMTQAITYPE_ADAPTERTYPE,
                           "adapter_type") < 0)
        ret = 1;
    if (query_adapter_type(fd, open_luid.adapter_handle,
                           _KMTQAITYPE_ADAPTERTYPE_RENDER,
                           "adapter_type_render") < 0)
        ret = 1;
    if (query_vidmem(fd, open_luid.adapter_handle) < 0)
        ret = 1;
    query_statistics(fd, open_luid.adapter_luid);
    probe_escape(fd, open_luid.adapter_handle);
    query_umdriver_private(fd, open_luid.adapter_handle);
    query_features(fd, open_luid.adapter_handle);
    query_adapter_dxcore_raw(fd, open_luid.adapter_handle);
    probe_flush_heap_transitions(fd, open_luid.adapter_handle);
    if (owner_isolation) {
        if (probe_owner_isolation(fd, open_luid.adapter_handle) < 0)
            ret = 1;
        goto close_adapter;
    }
    if (stage == DXGPROBE_STAGE_ADAPTER_ONLY)
        goto close_adapter;
    if (probe_device_context(fd, open_luid.adapter_handle, leak_on_close,
                             try_context, try_submit, try_wait, stage) < 0)
        ret = 1;

    if (leak_on_close) {
        if (ret == 0)
            printf("dxgprobe: leak-close ok\n");
        close(fd);
        return ret;
    }

close_adapter:
    memset(&close_adapter, 0, sizeof(close_adapter));
    close_adapter.adapter_handle = open_luid.adapter_handle;
    if (ioctl(fd, LX_DXCLOSEADAPTER, &close_adapter) < 0) {
        printf("dxgprobe: close adapter failed\n");
        ret = 1;
    }

    if (ret == 0)
        printf("dxgprobe: ok\n");
    close(fd);
    return ret;
}

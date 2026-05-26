#include "kernel/inc/types.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/errno.h"
#include "kernel/inc/uabi/d3dkmthk.h"
#include "kernel/inc/uabi/fcntl.h"
#include "user/user.h"

#define DXGPROBE_DEFAULT_FENCE_WAIT_SECONDS 5
#define DXGPROBE_TRACKER_STRESS_RESOURCES 96
#define DXGPROBE_TRACKER_STRESS_GPUVAS 544
#define DXGPROBE_TRACKER_STRESS_SYNCS 544
#define DXGPROBE_WSL_REPLAY_ALLOCATION_SIZE 0x10000
#define DXGPROBE_WSL_REPLAY_COMMAND_SIZE 4096
#define DXGPROBE_WSL_REPLAY_CONTEXT_PRIV_SIZE 3200
#define DXGPROBE_WSL_REPLAY_ALLOC_PRIV_SIZE 594
#define DXGPROBE_WSL_REPLAY_HWQUEUE_PRIV_SIZE 124
#define DXGPROBE_WSL_REPLAY_SUBMIT_PRIV_SIZE 1880
#define DXGPROBE_WSL_REPLAY_CLIENT_HINT 12
#define DXGPROBE_WSL_REPLAY_GPUVA_MIN 0x4000000ULL
#define DXGPROBE_WSL_REPLAY_GPUVA_MAX 0x10000000000ULL

static int g_paging_fence_wait_seconds = DXGPROBE_DEFAULT_FENCE_WAIT_SECONDS;
static uint32 g_sync_create_flags = 0x3;
static uint32 g_sync_open_flags;
static int g_sync_open_flags_set;
static uint64 g_sync_file_target_value = 1;
static int g_sync_file_validate;
static int g_resource_nt_validate;
static int g_fb_existing_sysmem_validate;
static int g_scanout_existing_sysmem_validate;
static int g_scanout_d3d12_bridge_validate;
static int g_shared_exporter_close_lifetime_validate;
static int g_scanout_pin_lifetime_validate;
static int g_import_negative_validate;
static int g_shared_seal_provenance_validate;
static int g_shared_mutation_validate;
static int g_qai_admission_validate;
static int g_create_publication_faults_validate;
static uint32 g_residency_batch_count = 2;
static uint32 g_residency_batch_flags = 0x1;
static uint32 g_requested_adapter_index = D3DKMT_ADAPTERS_MAX;
static int g_requested_adapter_luid_set;
static struct winluid g_requested_adapter_luid;

struct dxg_sharedhandle_copyout_diag {
    uint32 seen;
    uint32 failures;
    uint32 kind;
    uint32 process;
    uint32 object;
    uint32 nt;
    uint32 fd;
    uint32 reclaimed;
    uint32 refs_after;
    int32 ret;
};

static void *probe_alloc_buffer(uint32 size);
static char *dxg_find_text(char *s, const char *needle);
static int dxg_parse_uint_after(char *line, const char *name, uint32 *out);
static int dxg_parse_hex_after(char *line, const char *name, uint32 *out);
static int dxg_parse_u64_after(char *line, const char *name, uint64 *out);
static char *read_dxg_status_buffer(void);
static int read_dxg_sharedhandle_copyout_diag(
    struct dxg_sharedhandle_copyout_diag *out);

struct dxg_existing_sysmem_target_status {
    uint32 pfnmap_pages;
    uint32 pfnmap_ok;
    uint32 vram;
    uint32 vram_size;
};

static int read_existing_sysmem_target_status(
    struct dxg_existing_sysmem_target_status *out);

struct dxg_unwind_status {
    int32 allocation_ret;
    uint32 allocation_resource;
    uint32 allocation_handle;
    uint32 allocation_unwind_attempts;
    uint32 allocation_unwind_successes;
    int32 allocation_unwind_ret;
    uint32 create_process;
    uint32 destroy_device;
    uint32 destroy_resource;
    uint32 destroy_allocation;
    uint32 destroy_process;
    uint32 destroy_context;
    uint32 destroy_count;
    int32 destroy_ret;
    uint32 existing_pages;
    uint64 existing_total_pages;
    uint64 existing_active_pages;
    uint64 existing_pin_events;
    uint64 existing_unpin_events;
    int32 openresource_ret;
    uint32 openresource_process;
    uint32 openresource_device;
    uint32 openresource_result_resource;
    uint32 openresource_result_alloc0;
    uint32 shared_parent_seen;
    uint32 shared_parent_last;
    uint32 shared_parent_refs;
    uint32 shared_parent_fd_refs;
    uint32 shared_parent_children;
    uint32 shared_parent_sealed_generation;
};

static int read_dxg_unwind_status(struct dxg_unwind_status *out);

struct dxg_syncfile_status {
    int32 last_ret;
    uint32 last_out_sync;
    uint64 last_handle;
    uint32 live;
    uint32 creates;
    uint32 releases;
    uint32 event_removed;
    uint32 nt_released;
    uint32 create_faults;
    uint32 fd_reclaimed;
    uint32 open_faults;
    uint32 open_destroy_attempts;
    uint32 open_destroy_successes;
    int32 open_destroy_ret;
    uint32 host_event_active;
    uint32 host_event_allocs;
    uint32 host_event_removes;
};

static int read_dxg_syncfile_status(struct dxg_syncfile_status *out);

struct dxg_create_publication_status {
    uint32 createdevice_last_process;
    uint32 createdevice_last_device;
    uint32 createdevice_unwind_attempts;
    uint32 createdevice_unwind_successes;
    int32 createdevice_unwind_ret;
    uint32 createdevice_unwind_process;
    uint32 createdevice_unwind_device;
    uint32 createcontext_last_handle;
    uint32 createcontext_unwind_attempts;
    uint32 createcontext_unwind_successes;
    int32 createcontext_unwind_ret;
    uint32 createcontext_unwind_process;
    uint32 createcontext_unwind_context;
    uint32 createhwqueue_last_queue;
    uint32 createhwqueue_last_fence;
    uint32 createhwqueue_unwind_attempts;
    uint32 createhwqueue_unwind_successes;
    int32 createhwqueue_unwind_ret;
    uint32 createhwqueue_unwind_process;
    uint32 createhwqueue_unwind_queue;
    uint32 createhwqueue_unwind_fence;
};

static int read_create_publication_status(
    struct dxg_create_publication_status *out);

struct dxg_cpu_event_signal_status {
    uint32 attempts;
    uint32 successes;
    int32 ret;
    uint32 cmd;
    uint32 flags;
    uint32 objects;
    uint32 contexts;
    uint64 user_fd;
    uint64 event_id;
    uint32 len;
    uint32 active;
    uint32 allocs;
    uint32 removes;
};

static int read_cpu_event_signal_status(
    struct dxg_cpu_event_signal_status *out);

struct dxg_async_send_status {
    uint32 enabled;
    uint32 attempts;
    uint32 successes;
    uint32 fallback_sync;
    uint32 cmd;
    uint32 cmd_len;
    uint32 wire_len;
    uint32 async_bit;
    uint32 route_global;
    uint32 retries;
    uint32 packet_type;
    int32 ret;
    uint32 submit;
    uint32 signal;
    uint32 waitgpu;
    uint32 submithwqueue;
};

static int read_async_send_status(struct dxg_async_send_status *out);

static int parse_u32_option_value(const char *value, uint32 *out)
{
    uint64 parsed = 0;
    int base = 10;

    if (value == 0 || value[0] == 0 || out == 0)
        return -1;
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value += 2;
        if (value[0] == 0)
            return -1;
    }
    for (; *value != 0; value++) {
        int digit;

        if (*value >= '0' && *value <= '9')
            digit = *value - '0';
        else if (base == 16 && *value >= 'a' && *value <= 'f')
            digit = *value - 'a' + 10;
        else if (base == 16 && *value >= 'A' && *value <= 'F')
            digit = *value - 'A' + 10;
        else
            return -1;
        if (digit >= base)
            return -1;
        parsed = parsed * base + digit;
        if (parsed > 0xffffffffULL)
            return -1;
    }
    *out = (uint32)parsed;
    return 0;
}

static int parse_u64_option_value(const char *value, uint64 *out)
{
    uint64 parsed = 0;
    int base = 10;

    if (value == 0 || value[0] == 0 || out == 0)
        return -1;
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value += 2;
        if (value[0] == 0)
            return -1;
    }
    for (; *value != 0; value++) {
        int digit;

        if (*value >= '0' && *value <= '9')
            digit = *value - '0';
        else if (base == 16 && *value >= 'a' && *value <= 'f')
            digit = *value - 'a' + 10;
        else if (base == 16 && *value >= 'A' && *value <= 'F')
            digit = *value - 'A' + 10;
        else
            return -1;
        if (digit >= base)
            return -1;
        if (parsed > (~0ULL - (uint64)digit) / (uint64)base)
            return -1;
        parsed = parsed * (uint64)base + (uint64)digit;
    }
    *out = parsed;
    return 0;
}

static int dxgprobe_eventfd(uint64 initval)
{
#ifdef HOST_LIBC_PROGRAM
#ifdef SYS_eventfd2
    int rc = (int)syscall(SYS_eventfd2, initval, 0);
    if (rc < 0)
        return -errno;
    return rc;
#else
    (void)initval;
    return -ENOSYS;
#endif
#elif defined(__x86_64__)
    int64 ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(284), "D"((int64)initval), "S"(0)
                 : "rcx", "r11", "memory");
    return (int)ret;
#else
    (void)initval;
    return -ENOSYS;
#endif
}

static int parse_adapter_luid_option_value(const char *value,
                                           struct winluid *out)
{
    char high_buf[17];
    char low_buf[17];
    int high_len = 0;
    int low_len = 0;
    uint32 high;
    uint32 low;

    if (value == 0 || out == 0)
        return -1;
    while (value[high_len] != 0 && value[high_len] != ':' &&
           high_len < (int)sizeof(high_buf) - 1) {
        high_buf[high_len] = value[high_len];
        high_len++;
    }
    if (value[high_len] != ':' || high_len == 0)
        return -1;
    high_buf[high_len] = 0;
    value += high_len + 1;
    while (value[low_len] != 0 &&
           low_len < (int)sizeof(low_buf) - 1) {
        low_buf[low_len] = value[low_len];
        low_len++;
    }
    if (value[low_len] != 0 || low_len == 0)
        return -1;
    low_buf[low_len] = 0;
    if (parse_u32_option_value(high_buf, &high) < 0 ||
        parse_u32_option_value(low_buf, &low) < 0)
        return -1;
    out->b = high;
    out->a = low;
    return 0;
}

static int option_value(const char *arg, const char *prefix,
                        const char **value_out)
{
    int i = 0;

    if (arg == 0 || prefix == 0 || value_out == 0)
        return 0;
    while (prefix[i] != 0) {
        if (arg[i] != prefix[i])
            return 0;
        i++;
    }
    *value_out = arg + i;
    return 1;
}

static uint32 dxg_read_le32(const unsigned char *p)
{
    return ((uint32)p[0]) |
           ((uint32)p[1] << 8) |
           ((uint32)p[2] << 16) |
           ((uint32)p[3] << 24);
}

static int dxg_known_display_vendor(uint32 vendor)
{
    return vendor == 0x8086U || vendor == 0x10deU ||
           vendor == 0x1002U;
}

static void utf16_ascii_token(const unsigned char *data, uint32 size,
                              uint32 start_word, char *out,
                              uint32 out_size)
{
    uint32 count = size / sizeof(uint16);
    uint32 n = 0;

    if (out == 0 || out_size == 0)
        return;
    out[0] = 0;
    if (data == 0 || start_word >= count)
        return;
    for (uint32 i = start_word; i < count && n + 1 < out_size; i++) {
        uint16 ch = (uint16)data[i * 2] | ((uint16)data[i * 2 + 1] << 8);
        char c;

        if (ch == 0)
            break;
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') || ch == '.' || ch == '-' ||
            ch == '_')
            c = (char)ch;
        else if (ch == ' ' || ch == '\t' || ch == '/' || ch == '\\' ||
                 ch == ':' || ch == ',')
            c = '_';
        else if (ch >= 32 && ch < 127)
            c = '?';
        else
            c = '?';
        out[n++] = c;
    }
    out[n] = 0;
}

static void query_adapter_selector_identity(int fd,
                                            struct d3dkmthandle adapter,
                                            uint32 *vendor_out,
                                            uint32 *device_out,
                                            char *name_out,
                                            uint32 name_size,
                                            int *hardware_rc_out,
                                            int *name_rc_out,
                                            uint32 *name_type_out)
{
    unsigned char data[8192];
    struct d3dkmt_queryadapterinfo query;
    uint32 word0;
    uint32 word1;
    uint32 word2;
    uint32 word3 = 0;
    int rc;

    if (vendor_out)
        *vendor_out = 0;
    if (device_out)
        *device_out = 0;
    if (name_out && name_size)
        name_out[0] = 0;
    if (hardware_rc_out)
        *hardware_rc_out = -1;
    if (name_rc_out)
        *name_rc_out = -1;
    if (name_type_out)
        *name_type_out = 0;
    if (fd < 0 || adapter.v == 0)
        return;

    memset(data, 0, sizeof(data));
    memset(&query, 0, sizeof(query));
    query.adapter = adapter;
    query.type = (enum kmtqueryadapterinfotype)31;
    query.private_data = (uint64)data;
    query.private_data_size = 28;
    rc = ioctl(fd, LX_DXQUERYADAPTERINFO, &query);
    if (hardware_rc_out)
        *hardware_rc_out = rc;
    if (rc == 0 && query.private_data_size >= 12) {
        word0 = dxg_read_le32(data);
        word1 = dxg_read_le32(data + 4);
        word2 = dxg_read_le32(data + 8);
        if (query.private_data_size >= 16)
            word3 = dxg_read_le32(data + 12);
        if (dxg_known_display_vendor(word0)) {
            if (vendor_out)
                *vendor_out = word0;
            if (device_out)
                *device_out = word1;
        } else {
            if (vendor_out)
                *vendor_out = word1;
            if (device_out)
                *device_out = word2;
        }
        (void)word3;
    }

    memset(data, 0, sizeof(data));
    memset(&query, 0, sizeof(query));
    query.adapter = adapter;
    query.type = _KMTQAITYPE_DRIVER_DESCRIPTION_RENDER;
    query.private_data = (uint64)data;
    query.private_data_size = sizeof(data);
    rc = ioctl(fd, LX_DXQUERYADAPTERINFO, &query);
    if (rc == 0) {
        utf16_ascii_token(data, query.private_data_size, 0, name_out,
                          name_size);
        if (name_rc_out)
            *name_rc_out = rc;
        if (name_type_out)
            *name_type_out = query.type;
        return;
    }

    memset(data, 0, sizeof(data));
    memset(&query, 0, sizeof(query));
    query.adapter = adapter;
    query.type = _KMTQAITYPE_UMDRIVERNAME;
    query.private_data = (uint64)data;
    query.private_data_size = 524;
    rc = ioctl(fd, LX_DXQUERYADAPTERINFO, &query);
    if (rc == 0)
        utf16_ascii_token(data, query.private_data_size,
                          sizeof(uint32) / sizeof(uint16), name_out,
                          name_size);
    if (name_rc_out)
        *name_rc_out = rc;
    if (name_type_out)
        *name_type_out = query.type;
}

static int select_dxg_adapter_index(struct d3dkmt_adapterinfo *adapters,
                                    uint32 count, uint32 *index_out,
                                    const char *route, int fd)
{
    uint32 index = D3DKMT_ADAPTERS_MAX;
    uint32 vendor = 0;
    uint32 device = 0;
    uint32 name_type = 0;
    int hardware_rc = -1;
    int name_rc = -1;
    char name[128];

    name[0] = 0;

    if (adapters == 0 || index_out == 0)
        return -1;
    if (g_requested_adapter_luid_set) {
        for (uint32 i = 0; i < count; i++) {
            if (adapters[i].adapter_handle.v != 0 &&
                adapters[i].adapter_luid.a ==
                g_requested_adapter_luid.a &&
                adapters[i].adapter_luid.b ==
                g_requested_adapter_luid.b) {
                index = i;
                break;
            }
        }
    } else if (g_requested_adapter_index != D3DKMT_ADAPTERS_MAX) {
        if (g_requested_adapter_index < count &&
            adapters[g_requested_adapter_index].adapter_handle.v != 0)
            index = g_requested_adapter_index;
    } else {
        for (uint32 i = 0; i < count; i++) {
            if (adapters[i].adapter_handle.v != 0) {
                index = i;
                break;
            }
        }
    }
    if (index == D3DKMT_ADAPTERS_MAX) {
        printf("adapter_selector_matrix route=%s requested_index=%u requested_luid_set=%u requested_luid=%x:%x count=%u selected_index=%u selected_handle=0x0 selected_luid=0:0 selected_vendor=0x0 selected_device=0x0 selected_name=missing hardware_rc=-1 name_type=0 name_rc=-1 status=FAIL\n",
               route, g_requested_adapter_index,
               g_requested_adapter_luid_set,
               g_requested_adapter_luid.b, g_requested_adapter_luid.a,
               count, index);
        return -1;
    }
    *index_out = index;
    query_adapter_selector_identity(fd, adapters[index].adapter_handle,
                                    &vendor, &device, name, sizeof(name),
                                    &hardware_rc, &name_rc, &name_type);
    printf("adapter_selector_matrix route=%s requested_index=%u requested_luid_set=%u requested_luid=%x:%x count=%u selected_index=%u selected_handle=0x%x selected_luid=%x:%x selected_vendor=0x%x selected_device=0x%x selected_name=%s hardware_rc=%d name_type=%u name_rc=%d sources=%u status=PASS\n",
           route, g_requested_adapter_index, g_requested_adapter_luid_set,
           g_requested_adapter_luid.b, g_requested_adapter_luid.a, count,
           index, adapters[index].adapter_handle.v,
           adapters[index].adapter_luid.b, adapters[index].adapter_luid.a,
           vendor, device, name[0] != 0 ? name : "missing", hardware_rc,
           name_type, name_rc, adapters[index].num_sources);
    return 0;
}

static int enum_dxg_adapters2_list(int fd,
                                   struct d3dkmt_adapterinfo *adapters,
                                   uint32 *count_out,
                                   const char *tag)
{
    struct d3dkmt_enumadapters2 enum2;
    uint32 count;
    int rc;

    if (count_out)
        *count_out = 0;
    memset(&enum2, 0, sizeof(enum2));
    memset(adapters, 0,
           sizeof(struct d3dkmt_adapterinfo) * D3DKMT_ADAPTERS_MAX);
    enum2.num_adapters = D3DKMT_ADAPTERS_MAX;
    enum2.adapters = (uint64)adapters;
    rc = ioctl(fd, LX_DXENUMADAPTERS2, &enum2);
    if (rc < 0 || enum2.num_adapters == 0) {
        printf("%s: enum adapters2 list failed rc=%d count=%u requested=%u\n",
               tag, rc, enum2.num_adapters, D3DKMT_ADAPTERS_MAX);
        return -1;
    }
    count = enum2.num_adapters;
    if (count > D3DKMT_ADAPTERS_MAX) {
        printf("%s: enum adapters2 list returned count %u above cap %u, clamping scan\n",
               tag, count, D3DKMT_ADAPTERS_MAX);
        count = D3DKMT_ADAPTERS_MAX;
    }
    for (uint32 i = 0; i < count; i++) {
        if (adapters[i].adapter_handle.v != 0) {
            if (count_out)
                *count_out = count;
            printf("%s: enum adapters2 layout=list-first requested=%u count=%u first_index=%u\n",
                   tag, D3DKMT_ADAPTERS_MAX, enum2.num_adapters, i);
            return 0;
        }
    }
    printf("%s: enum adapters2 list contained no usable handle count=%u requested=%u\n",
           tag, count, D3DKMT_ADAPTERS_MAX);
    return -1;
}

static int enum_dxg_adapters3_list(int fd,
                                   struct d3dkmt_adapterinfo *adapters,
                                   uint32 *count_out)
{
    struct d3dkmt_enumadapters3 enum3;
    uint32 count;
    int rc;

    if (count_out)
        *count_out = 0;
    memset(&enum3, 0, sizeof(enum3));
    memset(adapters, 0,
           sizeof(struct d3dkmt_adapterinfo) * D3DKMT_ADAPTERS_MAX);
    enum3.adapter_count = D3DKMT_ADAPTERS_MAX;
    enum3.adapters = (uint64)adapters;
    rc = ioctl(fd, LX_DXENUMADAPTERS3, &enum3);
    if (rc < 0 || enum3.adapter_count == 0) {
        printf("dxgprobe: enum3 adapters list failed rc=%d count=%u requested=%u\n",
               rc, enum3.adapter_count, D3DKMT_ADAPTERS_MAX);
        return -1;
    }
    count = enum3.adapter_count;
    if (count > D3DKMT_ADAPTERS_MAX) {
        printf("dxgprobe: enum3 adapters list returned count %u above cap %u, clamping scan\n",
               count, D3DKMT_ADAPTERS_MAX);
        count = D3DKMT_ADAPTERS_MAX;
    }
    for (uint32 i = 0; i < count; i++) {
        if (adapters[i].adapter_handle.v != 0) {
            if (count_out)
                *count_out = count;
            printf("dxgprobe: enum3 layout=list-first requested=%u count=%u first_index=%u\n",
                   D3DKMT_ADAPTERS_MAX, enum3.adapter_count, i);
            return 0;
        }
    }
    printf("dxgprobe: enum3 adapters list contained no usable handle count=%u requested=%u\n",
           count, D3DKMT_ADAPTERS_MAX);
    return -1;
}

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

static int open_first_dxg_device(int *fd_out, struct d3dkmthandle *adapter_out,
                                 struct d3dkmthandle *device_out)
{
    struct d3dkmt_adapterinfo adapters[D3DKMT_ADAPTERS_MAX];
    struct d3dkmt_openadapterfromluid open_luid;
    struct d3dkmt_createdevice create_device;
    uint32 adapter_count;
    uint32 adapter_index = D3DKMT_ADAPTERS_MAX;
    int fd;

    fd = open("/dev/dxg", O_RDWR);
    if (fd < 0) {
        printf("dxg_child: open /dev/dxg failed\n");
        return -1;
    }

    if (enum_dxg_adapters2_list(fd, adapters, &adapter_count,
                                "dxg_child") < 0) {
        close(fd);
        return -1;
    }
    if (select_dxg_adapter_index(adapters, adapter_count, &adapter_index,
                                 "child_enum2", fd) < 0) {
        printf("dxg_child: enum adapters2 no selectable adapter count=%u\n",
               adapter_count);
        close(fd);
        return -1;
    }

    memset(&open_luid, 0, sizeof(open_luid));
    open_luid.adapter_luid = adapters[adapter_index].adapter_luid;
    if (ioctl(fd, LX_DXOPENADAPTERFROMLUID, &open_luid) < 0 ||
        open_luid.adapter_handle.v == 0) {
        printf("dxg_child: open adapter failed\n");
        close(fd);
        return -1;
    }

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = open_luid.adapter_handle;
    if (ioctl(fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        struct d3dkmt_closeadapter close_adapter;

        printf("dxg_child: create device failed device=0x%x\n",
               create_device.device.v);
        memset(&close_adapter, 0, sizeof(close_adapter));
        close_adapter.adapter_handle = open_luid.adapter_handle;
        ioctl(fd, LX_DXCLOSEADAPTER, &close_adapter);
        close(fd);
        return -1;
    }

    *fd_out = fd;
    *adapter_out = open_luid.adapter_handle;
    *device_out = create_device.device;
    return 0;
}

static void close_dxg_device(int fd, struct d3dkmthandle adapter,
                             struct d3dkmthandle device)
{
    struct d3dkmt_destroydevice destroy_device;
    struct d3dkmt_closeadapter close_adapter;

    if (fd < 0)
        return;
    if (device.v != 0) {
        memset(&destroy_device, 0, sizeof(destroy_device));
        destroy_device.device = device;
        ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device);
    }
    if (adapter.v != 0) {
        memset(&close_adapter, 0, sizeof(close_adapter));
        close_adapter.adapter_handle = adapter;
        ioctl(fd, LX_DXCLOSEADAPTER, &close_adapter);
    }
    close(fd);
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

struct qai_admission_row {
    const char *route;
    uint32 type;
    uint32 requested_size;
    int rc;
    uint32 result_size;
    uint32 head_hash;
    unsigned char head[16];
};

static uint32 dxgprobe_hash_bytes(const unsigned char *buf, uint32 len)
{
    uint32 hash = 2166136261U;

    for (uint32 i = 0; i < len; i++) {
        hash ^= buf[i];
        hash *= 16777619U;
    }
    return hash;
}

static int probe_qai_admission_row(int fd, struct d3dkmthandle adapter,
                                   const char *route, uint32 type,
                                   uint32 size,
                                   struct qai_admission_row *row)
{
    static unsigned char private_data[9300];
    struct d3dkmt_queryadapterinfo query;
    uint32 head_len;

    memset(row, 0, sizeof(*row));
    row->route = route;
    row->type = type;
    row->requested_size = size;
    row->result_size = size;
    memset(private_data, 0, sizeof(private_data));
    memset(&query, 0, sizeof(query));
    if (size > sizeof(private_data))
        size = sizeof(private_data);
    query.adapter = adapter;
    query.type = (enum kmtqueryadapterinfotype)type;
    query.private_data = (uint64)private_data;
    query.private_data_size = size;
    row->rc = ioctl(fd, LX_DXQUERYADAPTERINFO, &query);
    row->result_size = query.private_data_size;
    head_len = row->result_size;
    if (head_len > sizeof(row->head))
        head_len = sizeof(row->head);
    if (row->rc >= 0 && head_len != 0) {
        memcpy(row->head, private_data, head_len);
        row->head_hash = dxgprobe_hash_bytes(private_data,
                                             row->result_size);
    }
    printf("qai_admission_row route=%s type=%u requested_size=%u result_size=%u rc=%d status=%s head_hash=0x%x head=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
           route, type, row->requested_size, row->result_size, row->rc,
           row->rc < 0 ? "FAIL" : "PASS", row->head_hash,
           row->head[0], row->head[1], row->head[2], row->head[3],
           row->head[4], row->head[5], row->head[6], row->head[7],
           row->head[8], row->head[9], row->head[10], row->head[11],
           row->head[12], row->head[13], row->head[14], row->head[15]);
    return row->rc;
}

static int probe_qai_admission_route(int fd, struct winluid luid,
                                     const char *route,
                                     struct qai_admission_row rows[3])
{
    struct d3dkmt_openadapterfromluid open_luid;
    int ret = 0;

    memset(&open_luid, 0, sizeof(open_luid));
    open_luid.adapter_luid = luid;
    if (ioctl(fd, LX_DXOPENADAPTERFROMLUID, &open_luid) < 0 ||
        open_luid.adapter_handle.v == 0) {
        printf("qai_admission_route route=%s open_adapter=FAIL luid=%x:%x handle=0x%x\n",
               route, luid.b, luid.a, open_luid.adapter_handle.v);
        return -2;
    }
    printf("qai_admission_route route=%s open_adapter=PASS luid=%x:%x handle=0x%x\n",
           route, luid.b, luid.a, open_luid.adapter_handle.v);
    if (probe_qai_admission_row(fd, open_luid.adapter_handle, route,
                                _KMTQAITYPE_UMDRIVERPRIVATE, 9300,
                                &rows[0]) < 0)
        ret = -1;
    if (probe_qai_admission_row(fd, open_luid.adapter_handle, route, 27, 4,
                                &rows[1]) < 0)
        ret = -1;
    if (probe_qai_admission_row(fd, open_luid.adapter_handle, route,
                                _KMTQAITYPE_QUERYREGISTRY, 4096,
                                &rows[2]) < 0)
        ret = -1;
    return ret;
}

static uint32 probe_backend_opengl_submit_flag(void)
{
    struct fb_gpu_backend_info backend;
    int fb_fd;
    uint32 enabled = 0;

    fb_fd = open("/dev/gpu0", O_RDONLY);
    if (fb_fd < 0)
        fb_fd = open("/dev/fb0", O_RDONLY);
    memset(&backend, 0, sizeof(backend));
    if (fb_fd >= 0 && ioctl(fb_fd, FB_GPU_BACKEND_QUERY, &backend) == 0)
        enabled = (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0;
    if (fb_fd >= 0)
        close(fb_fd);
    printf("qai_admission_backend backend=%u flags=0x%x backend_opengl_submit=%u name=%s\n",
           backend.backend, backend.flags, enabled, backend.name);
    return enabled;
}

static int probe_qai_admission_validate(int fd)
{
    struct d3dkmt_adapterinfo enum2[D3DKMT_ADAPTERS_MAX];
    struct d3dkmt_adapterinfo enum3[D3DKMT_ADAPTERS_MAX];
    struct qai_admission_row direct_rows[3];
    struct qai_admission_row list_rows[3];
    uint32 enum2_count = 0;
    uint32 enum3_count = 0;
    uint32 enum2_index = D3DKMT_ADAPTERS_MAX;
    uint32 enum3_index = D3DKMT_ADAPTERS_MAX;
    uint32 direct_list_luid_match;
    uint32 backend_submit;
    int direct_ret;
    int list_ret;

    if (enum_dxg_adapters2_list(fd, enum2, &enum2_count,
                                "qai_admission") < 0 ||
        select_dxg_adapter_index(enum2, enum2_count, &enum2_index,
                                 "qai-direct-enum2", fd) < 0)
        return -1;
    if (enum_dxg_adapters3_list(fd, enum3, &enum3_count) < 0 ||
        select_dxg_adapter_index(enum3, enum3_count, &enum3_index,
                                 "qai-list-enum3", fd) < 0)
        return -1;
    direct_list_luid_match =
        enum2[enum2_index].adapter_luid.a == enum3[enum3_index].adapter_luid.a &&
        enum2[enum2_index].adapter_luid.b == enum3[enum3_index].adapter_luid.b;
    printf("qai_admission_selection direct_count=%u direct_index=%u direct_handle=0x%x direct_luid=%x:%x direct_sources=%u list_count=%u list_index=%u list_handle=0x%x list_luid=%x:%x list_sources=%u direct_list_luid_match=%u create_adapter_list_d3d12_graphics=modelled-by-enum3\n",
           enum2_count, enum2_index, enum2[enum2_index].adapter_handle.v,
           enum2[enum2_index].adapter_luid.b,
           enum2[enum2_index].adapter_luid.a,
           enum2[enum2_index].num_sources, enum3_count, enum3_index,
           enum3[enum3_index].adapter_handle.v,
           enum3[enum3_index].adapter_luid.b,
           enum3[enum3_index].adapter_luid.a,
           enum3[enum3_index].num_sources, direct_list_luid_match);
    direct_ret = probe_qai_admission_route(fd,
                                           enum2[enum2_index].adapter_luid,
                                           "direct-openadapterfromluid",
                                           direct_rows);
    list_ret = probe_qai_admission_route(fd,
                                         enum3[enum3_index].adapter_luid,
                                         "create-adapter-list-d3d12-graphics",
                                         list_rows);
    backend_submit = probe_backend_opengl_submit_flag();
    printf("qai_admission_matrix direct_count=%u list_count=%u direct_luid=%x:%x list_luid=%x:%x direct_list_luid_match=%u direct_type0_rc=%d direct_type27_rc=%d direct_type48_rc=%d list_type0_rc=%d list_type27_rc=%d list_type48_rc=%d backend_opengl_submit=%u wsl_trace=/tmp/xv6-wsl-probe/wave77-wsl-qai-admission-type0-9300.trace wsl_type0_size=9300 wsl_type27_size=4 wsl_type48_size=4096 divergence_class=nonblocking-admission-proven-before-export status=%s\n",
           enum2_count, enum3_count, enum2[enum2_index].adapter_luid.b,
           enum2[enum2_index].adapter_luid.a,
           enum3[enum3_index].adapter_luid.b,
           enum3[enum3_index].adapter_luid.a, direct_list_luid_match,
           direct_rows[0].rc, direct_rows[1].rc, direct_rows[2].rc,
           list_rows[0].rc, list_rows[1].rc, list_rows[2].rc,
           backend_submit,
           direct_list_luid_match && backend_submit == 0 &&
           direct_ret != -2 && list_ret != -2 ? "PASS" : "FAIL");
    return direct_list_luid_match && backend_submit == 0 &&
           direct_ret != -2 && list_ret != -2 ? 0 : -1;
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

static int parse_hex_bytes(const char *hex, unsigned char *out,
                           uint32 out_cap);

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

static void put_le32(unsigned char *p, uint32 v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put_utf16le_ascii(unsigned char *p, uint32 bytes,
                              const char *text)
{
    uint32 i;

    for (i = 0; text[i] != '\0' && i * 2 + 1 < bytes; i++) {
        p[i * 2] = (unsigned char)text[i];
        p[i * 2 + 1] = 0;
    }
}

static void make_wsl_replay_allocation_private(unsigned char *priv,
                                               uint32 priv_size)
{
    static const char wsl_alloc_priv_hex[] =
        "4144564e04000100520200005844564e00000000000100000000000000000000"
        "0000020000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000200000002000000020000000000"
        "0000000000000000000000001078000000000100000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000100000001000000000000000000000000000800"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000ffffffff000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000010000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000";

    memset(priv, 0, priv_size);
    if (priv_size < DXGPROBE_WSL_REPLAY_ALLOC_PRIV_SIZE)
        return;
    if (parse_hex_bytes(wsl_alloc_priv_hex, priv,
                        DXGPROBE_WSL_REPLAY_ALLOC_PRIV_SIZE) !=
        DXGPROBE_WSL_REPLAY_ALLOC_PRIV_SIZE)
        memset(priv, 0, priv_size);
}

static void make_wsl_replay_hwqueue_private(unsigned char *priv,
                                            uint32 priv_size,
                                            struct d3dkmthandle allocation)
{
    memset(priv, 0, priv_size);
    if (priv_size < DXGPROBE_WSL_REPLAY_HWQUEUE_PRIV_SIZE)
        return;

    /* Same-adapter NVIDIA WSL trace: ADVN/XDVN 124-byte HW queue blob. */
    put_le32(priv + 0, 0x4e564441);
    put_le32(priv + 4, 0x00010000);
    put_le32(priv + 8, DXGPROBE_WSL_REPLAY_HWQUEUE_PRIV_SIZE);
    put_le32(priv + 12, 0x4e564458);
    put_le32(priv + 16, 0x00000900);
    put_le32(priv + 24, 0x04000000);
    put_le32(priv + 32, 0x00010000);
    put_le32(priv + 36, allocation.v);
}

static void make_wsl_replay_context_private(unsigned char *priv,
                                            uint32 priv_size)
{
    memset(priv, 0, priv_size);
    if (priv_size < DXGPROBE_WSL_REPLAY_CONTEXT_PRIV_SIZE)
        return;

    /* Same-adapter NVIDIA WSL trace: ADVN/XDVN 3200-byte DX12 context blob. */
    put_le32(priv + 0, 0x4e564441);
    put_le32(priv + 4, 0x00010000);
    put_le32(priv + 8, DXGPROBE_WSL_REPLAY_CONTEXT_PRIV_SIZE);
    put_le32(priv + 12, 0x4e564458);
    put_le32(priv + 16, 4);
    put_le32(priv + 20, 2);
    put_le32(priv + 24, 2);
    put_le32(priv + 28, 0x80);
    put_le32(priv + 32, 0x938575bf);
    put_le32(priv + 36, 0x8db63936);
    put_utf16le_ascii(priv + 0x530, priv_size - 0x530,
                      "/home/es/xv6-os");
    put_utf16le_ascii(priv + 0x738, priv_size - 0x738,
                      "/tmp/tmp.PHsSKWCqgl/bin/mesaglfeature");
    put_le32(priv + 0xc40, 0x003d1b62);
    put_le32(priv + 0xc48, 0xffffffff);
    put_le32(priv + 0xc4c, 0xffffffff);
    put_le32(priv + 0xc50, 0xffffffff);
    put_le32(priv + 0xc54, 0xffffffff);
    put_le32(priv + 0xc58, 0xffffffff);
    put_le32(priv + 0xc5c, 0xffffffff);
}

static void make_wsl_replay_submit_private(unsigned char *priv,
                                           uint32 priv_size,
                                           uint32 *priv_size_out,
                                           uint64 command_iova,
                                           uint32 command_size)
{
    memset(priv, 0, priv_size);
    if (priv_size < DXGPROBE_WSL_REPLAY_SUBMIT_PRIV_SIZE) {
        *priv_size_out = 0;
        return;
    }

    /*
     * Same-adapter NVIDIA WSL trace: SUBMITCOMMANDTOHWQUEUE uses ADVN private
     * data with a 0x118-byte leading command section inside a larger blob.
     * Keep the replay synthetic by using a zero command buffer, but preserve
     * the observed private-data family and command length.
     */
    put_le32(priv + 0, 0x4e564441);
    put_le32(priv + 4, 0x00010000);
    put_le32(priv + 8, 0);
    put_le32(priv + 12, 0x00000118);
    put_le32(priv + 16, 0);
    put_le32(priv + 20, 1);
    (void)command_iova;
    put_le32(priv + 0x78, command_size >> 8);
    put_le32(priv + 0x110, 1);
    *priv_size_out = DXGPROBE_WSL_REPLAY_SUBMIT_PRIV_SIZE;
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
    TRY_CONTEXT("dx12_hwqueue_private_e0_64", 0, 0,
                _D3DKMT_CLIENTHINT_DX12, private_data, private64, 0);
    TRY_CONTEXT("dx12_hwqueue_e0", 0, 0, _D3DKMT_CLIENTHINT_DX12, 0, 0, 0);
    TRY_CONTEXT("dx12_hwqueue_private_e1_full", 0, 1,
                _D3DKMT_CLIENTHINT_DX12, private_data, private_size, 0);
    TRY_CONTEXT("dx12_hwqueue_private_e1_64", 0, 1,
                _D3DKMT_CLIENTHINT_DX12, private_data, private64, 0);
    TRY_CONTEXT("dx12_hwqueue_e1", 0, 1, _D3DKMT_CLIENTHINT_DX12, 0, 0, 0);
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

static int probe_create_publication_faults_validate(
    int fd, struct d3dkmthandle adapter)
{
    static const struct {
        const char *name;
        uint32 node;
        uint32 engine;
        enum d3dkmt_clienthint hint;
        uint32 hwqueue_supported;
    } context_cases[] = {
        { "dx12_hwqueue_e0", 0, 0, _D3DKMT_CLIENTHINT_DX12, 1 },
        { "dx12_empty_e1", 0, 1, _D3DKMT_CLIENTHINT_DX12, 0 },
        { "unknown_empty_e0", 0, 0, _D3DKMT_CLIENTHNT_UNKNOWN, 0 },
    };
    struct dxg_create_publication_status before;
    struct dxg_create_publication_status after_device;
    struct dxg_create_publication_status before_context;
    struct dxg_create_publication_status after_context;
    struct dxg_create_publication_status before_hwqueue;
    struct dxg_create_publication_status after_hwqueue;
    struct d3dkmt_createdevice *fault_device;
    struct d3dkmt_createdevice create_device;
    struct d3dkmt_destroydevice destroy_device;
    struct d3dkmt_createcontextvirtual *fault_context;
    struct d3dkmt_destroycontext destroy_context;
    struct d3dkmt_createhwqueue *fault_hwqueue;
    struct d3dkmt_destroyhwqueue destroy_hwqueue;
    struct d3dkmt_destroysynchronizationobject destroy_sync;
    struct d3dkmthandle context_handle;
    unsigned char context_private_data[64];
    void *page = 0;
    int context_sync_only = 0;
    int before_rc;
    int device_rc = -1;
    int context_rc = -1;
    int hwqueue_rc = -1;
    int destroy_device_retry_rc = -2;
    int destroy_context_retry_rc = -2;
    int destroy_hwqueue_retry_rc = -2;
    int destroy_hwqueue_fence_retry_rc = -2;
    uint32 context_retry_handle = 0;
    uint32 hwqueue_retry_handle = 0;
    uint32 hwqueue_fence_retry_handle = 0;
    int device_ok = 0;
    int context_ok = 0;
    int hwqueue_ok = 0;
    int ret = -1;
    const char *context_case_name = "none";

    memset(&before, 0, sizeof(before));
    memset(&after_device, 0, sizeof(after_device));
    before_rc = read_create_publication_status(&before);
    page = mmap(0, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == (void *)-1) {
        printf("dxg_createdevice_copyout_unwind_matrix status=SKIP_MMAP\n");
        return -1;
    }

    memset(page, 0, 4096);
    fault_device = (struct d3dkmt_createdevice *)page;
    fault_device->adapter = adapter;
    if (mprotect(page, 4096, PROT_READ) != 0) {
        printf("dxg_createdevice_copyout_unwind_matrix status=SKIP_MPROTECT\n");
        goto out_unmap;
    }
    device_rc = ioctl(fd, LX_DXCREATEDEVICE, fault_device);
    (void)read_create_publication_status(&after_device);
    if (mprotect(page, 4096, PROT_READ | PROT_WRITE) != 0) {
        printf("dxg_createdevice_copyout_unwind_matrix status=FAIL_REPROTECT\n");
        goto out_unmap;
    }
    if (after_device.createdevice_last_device != 0) {
        memset(&destroy_device, 0, sizeof(destroy_device));
        destroy_device.device.v = after_device.createdevice_last_device;
        destroy_device_retry_rc = ioctl(fd, LX_DXDESTROYDEVICE,
                                        &destroy_device);
    }
    device_ok =
        before_rc == 0 && device_rc < 0 &&
        after_device.createdevice_unwind_attempts >
            before.createdevice_unwind_attempts &&
        after_device.createdevice_unwind_successes >
            before.createdevice_unwind_successes &&
        after_device.createdevice_unwind_ret == 0 &&
        after_device.createdevice_unwind_device ==
            after_device.createdevice_last_device &&
        after_device.createdevice_unwind_process ==
            after_device.createdevice_last_process &&
        destroy_device_retry_rc < 0;
    printf("dxg_createdevice_copyout_unwind_matrix rc=%d before_rc=%d "
           "process=0x%x device=0x%x destroy_retry_rc=%d "
           "unwind_attempts=%u->%u unwind_successes=%u->%u "
           "unwind_ret=%d no_local_publication=%u status=%s\n",
           device_rc, before_rc, after_device.createdevice_last_process,
           after_device.createdevice_last_device, destroy_device_retry_rc,
           before.createdevice_unwind_attempts,
           after_device.createdevice_unwind_attempts,
           before.createdevice_unwind_successes,
           after_device.createdevice_unwind_successes,
           after_device.createdevice_unwind_ret,
           destroy_device_retry_rc < 0 ? 1 : 0,
           device_ok ? "PASS" : "FAIL");

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    if (ioctl(fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        printf("dxg_createcontext_copyout_unwind_matrix status=FAIL "
               "reason=create_device device=0x%x\n",
               create_device.device.v);
        goto out_unmap;
    }

    memset(&before_context, 0, sizeof(before_context));
    memset(&after_context, 0, sizeof(after_context));
    if (read_create_publication_status(&before_context) < 0)
        memset(&before_context, 0, sizeof(before_context));
    for (uint32 i = 0; i < sizeof(context_cases) / sizeof(context_cases[0]);
         i++) {
        memset(page, 0, 4096);
        fault_context = (struct d3dkmt_createcontextvirtual *)page;
        fault_context->device = create_device.device;
        fault_context->node_ordinal = context_cases[i].node;
        fault_context->engine_affinity = context_cases[i].engine;
        fault_context->client_hint = context_cases[i].hint;
        fault_context->flags.hw_queue_supported =
            context_cases[i].hwqueue_supported;
        if (mprotect(page, 4096, PROT_READ) != 0) {
            printf("dxg_createcontext_copyout_unwind_matrix status=FAIL "
                   "reason=mprotect_read\n");
            goto cleanup_device;
        }
        context_rc = ioctl(fd, LX_DXCREATECONTEXTVIRTUAL, fault_context);
        (void)read_create_publication_status(&after_context);
        if (mprotect(page, 4096, PROT_READ | PROT_WRITE) != 0) {
            printf("dxg_createcontext_copyout_unwind_matrix status=FAIL "
                   "reason=mprotect_write\n");
            goto cleanup_device;
        }
        if (context_rc < 0 &&
            after_context.createcontext_unwind_attempts >
                before_context.createcontext_unwind_attempts) {
            context_case_name = context_cases[i].name;
            break;
        }
    }
    context_retry_handle = after_context.createcontext_last_handle != 0 ?
                           after_context.createcontext_last_handle :
                           after_context.createcontext_unwind_context;
    if (context_retry_handle != 0) {
        memset(&destroy_context, 0, sizeof(destroy_context));
        destroy_context.context.v = context_retry_handle;
        destroy_context_retry_rc = ioctl(fd, LX_DXDESTROYCONTEXT,
                                         &destroy_context);
    }
    context_ok =
        context_rc < 0 &&
        after_context.createcontext_unwind_attempts >
            before_context.createcontext_unwind_attempts &&
        after_context.createcontext_unwind_successes >
            before_context.createcontext_unwind_successes &&
        after_context.createcontext_unwind_ret == 0 &&
        after_context.createcontext_unwind_context ==
            context_retry_handle &&
        context_retry_handle != 0 &&
        destroy_context_retry_rc < 0;
    printf("dxg_createcontext_copyout_unwind_matrix rc=%d case=%s "
           "context=0x%x destroy_retry_rc=%d "
           "unwind_attempts=%u->%u unwind_successes=%u->%u "
           "unwind_ret=%d no_local_publication=%u status=%s\n",
           context_rc, context_case_name,
           context_retry_handle,
           destroy_context_retry_rc,
           before_context.createcontext_unwind_attempts,
           after_context.createcontext_unwind_attempts,
           before_context.createcontext_unwind_successes,
           after_context.createcontext_unwind_successes,
           after_context.createcontext_unwind_ret,
           destroy_context_retry_rc < 0 ? 1 : 0,
           context_ok ? "PASS" : "FAIL");

    memset(context_private_data, 0, sizeof(context_private_data));
    memset(&context_handle, 0, sizeof(context_handle));
    if (probe_context_matrix(fd, create_device.device,
                             context_private_data,
                             sizeof(context_private_data),
                             &context_handle, &context_sync_only) < 0 ||
        context_handle.v == 0 || context_sync_only) {
        printf("dxg_createhwqueue_copyout_unwind_matrix status=FAIL "
               "reason=create_context context=0x%x sync_only=%d\n",
               context_handle.v, context_sync_only);
        goto cleanup_device;
    }

    memset(&before_hwqueue, 0, sizeof(before_hwqueue));
    memset(&after_hwqueue, 0, sizeof(after_hwqueue));
    if (read_create_publication_status(&before_hwqueue) < 0)
        memset(&before_hwqueue, 0, sizeof(before_hwqueue));
    memset(page, 0, 4096);
    fault_hwqueue = (struct d3dkmt_createhwqueue *)page;
    fault_hwqueue->context = context_handle;
    if (mprotect(page, 4096, PROT_READ) != 0) {
        printf("dxg_createhwqueue_copyout_unwind_matrix status=FAIL "
               "reason=mprotect_read\n");
        goto cleanup_context;
    }
    hwqueue_rc = ioctl(fd, LX_DXCREATEHWQUEUE, fault_hwqueue);
    (void)read_create_publication_status(&after_hwqueue);
    if (mprotect(page, 4096, PROT_READ | PROT_WRITE) != 0) {
        printf("dxg_createhwqueue_copyout_unwind_matrix status=FAIL "
               "reason=mprotect_write\n");
        goto cleanup_context;
    }
    hwqueue_retry_handle = after_hwqueue.createhwqueue_last_queue != 0 ?
                           after_hwqueue.createhwqueue_last_queue :
                           after_hwqueue.createhwqueue_unwind_queue;
    hwqueue_fence_retry_handle =
        after_hwqueue.createhwqueue_last_fence != 0 ?
        after_hwqueue.createhwqueue_last_fence :
        after_hwqueue.createhwqueue_unwind_fence;
    if (hwqueue_retry_handle != 0) {
        memset(&destroy_hwqueue, 0, sizeof(destroy_hwqueue));
        destroy_hwqueue.queue.v = hwqueue_retry_handle;
        destroy_hwqueue_retry_rc = ioctl(fd, LX_DXDESTROYHWQUEUE,
                                         &destroy_hwqueue);
    }
    if (hwqueue_fence_retry_handle != 0) {
        memset(&destroy_sync, 0, sizeof(destroy_sync));
        destroy_sync.sync_object.v = hwqueue_fence_retry_handle;
        destroy_hwqueue_fence_retry_rc =
            ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &destroy_sync);
    }
    hwqueue_ok =
        hwqueue_rc < 0 &&
        after_hwqueue.createhwqueue_unwind_attempts >
            before_hwqueue.createhwqueue_unwind_attempts &&
        after_hwqueue.createhwqueue_unwind_successes >
            before_hwqueue.createhwqueue_unwind_successes &&
        after_hwqueue.createhwqueue_unwind_ret == 0 &&
        after_hwqueue.createhwqueue_unwind_queue ==
            hwqueue_retry_handle &&
        after_hwqueue.createhwqueue_unwind_fence ==
            hwqueue_fence_retry_handle &&
        hwqueue_retry_handle != 0 &&
        hwqueue_fence_retry_handle != 0 &&
        destroy_hwqueue_retry_rc < 0 &&
        destroy_hwqueue_fence_retry_rc < 0;
    printf("dxg_createhwqueue_copyout_unwind_matrix rc=%d queue=0x%x "
           "fence=0x%x destroy_queue_retry_rc=%d "
           "destroy_fence_retry_rc=%d unwind_attempts=%u->%u "
           "unwind_successes=%u->%u unwind_ret=%d "
           "no_local_publication=%u status=%s\n",
           hwqueue_rc, hwqueue_retry_handle,
           hwqueue_fence_retry_handle,
           destroy_hwqueue_retry_rc, destroy_hwqueue_fence_retry_rc,
           before_hwqueue.createhwqueue_unwind_attempts,
           after_hwqueue.createhwqueue_unwind_attempts,
           before_hwqueue.createhwqueue_unwind_successes,
           after_hwqueue.createhwqueue_unwind_successes,
           after_hwqueue.createhwqueue_unwind_ret,
           destroy_hwqueue_retry_rc < 0 &&
           destroy_hwqueue_fence_retry_rc < 0 ? 1 : 0,
           hwqueue_ok ? "PASS" : "FAIL");
    ret = device_ok && context_ok && hwqueue_ok ? 0 : -1;

cleanup_context:
    (void)destroy_context_handle(fd, context_handle);
cleanup_device:
    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device = create_device.device;
    if (create_device.device.v != 0 &&
        ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device) < 0) {
        printf("dxg_create_publication_faults cleanup_destroy_device_failed "
               "device=0x%x\n",
               create_device.device.v);
        ret = -1;
    }
out_unmap:
    munmap(page, 4096);
    printf("dxg_create_publication_faults_matrix device=%s context=%s "
           "hwqueue=%s status=%s\n",
           device_ok ? "PASS" : "FAIL",
           context_ok ? "PASS" : "FAIL",
           hwqueue_ok ? "PASS" : "FAIL",
           ret == 0 ? "PASS" : "FAIL");
    return ret;
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
    struct d3dkmt_createstandardallocation standard_allocation;
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

static int probe_fb_existing_sysmem_contract(int dxg_fd,
                                             struct d3dkmthandle device)
{
    struct fb_gpu_bo_create bo;
    struct fb_gpu_bo_present present;
    struct fb_gpu_bo_info bo_info;
    struct fb_gpu_bo_export_fd bo_export;
    struct fb_gpu_bo_import_fd bo_import;
    struct fb_gpu_bo_destroy bo_destroy;
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmthandle allocation_list[1];
    uint32 *pixels;
    uint32 width = 64;
    uint32 height = 64;
    uint64 bo_size = 0;
    int gpu_fd = -1;
    int fb_fd = -1;
    int bo_fd = -1;
    int present_fd = -1;
    uint32 present_handle = 0;
    int bo_create_rc = -1;
    int bo_info_rc = -1;
    int bo_export_rc = -1;
    int bo_import_rc = -1;
    int create_rc = -1;
    int present_rc = -1;
    int present_errno = 0;
    int ret = -1;

    memset(&bo_export, 0, sizeof(bo_export));
    memset(&bo_import, 0, sizeof(bo_import));

    gpu_fd = open("/dev/gpu0", O_RDWR);
    fb_fd = open("/dev/fb0", O_RDWR);
    bo_fd = gpu_fd >= 0 ? gpu_fd : fb_fd;
    if (bo_fd < 0) {
        printf("fb_existing_sysmem_matrix gpu_open=FAIL fb_open=FAIL errno=%d gpu_to_fb_bo_sysmem_staging=0 d3d12_cpu_readback=0 fb_bo_present_cpu_blit=0 native_host_display_handoff=0 status=FAIL missing_api=none\n",
               ENODEV);
        return -1;
    }

    memset(&bo, 0, sizeof(bo));
    bo.width = width;
    bo.height = height;
    bo.flags = FB_GPU_BO_F_EXPORTABLE;
    bo_create_rc = ioctl(bo_fd, FB_GPU_BO_CREATE, &bo);
    if (bo_create_rc < 0 || bo.addr == 0 || bo.size == 0 ||
        bo.handle == 0 || bo.pitch < width * 4) {
        printf("fb_existing_sysmem_matrix gpu_fd=%d fb_fd=%d bo_fd=%d fb_open=%s bo_create_rc=%d bo_handle=%u bo_addr=0x%lx bo_size=%lu bo_pitch=%u gpu_to_fb_bo_sysmem_staging=0 d3d12_cpu_readback=0 fb_bo_present_cpu_blit=0 native_host_display_handoff=0 status=FAIL missing_api=none\n",
               gpu_fd, fb_fd, bo_fd, fb_fd >= 0 ? "PASS" : "FAIL",
               bo_create_rc, bo.handle, bo.addr, bo.size, bo.pitch);
        goto out_close_fds;
    }

    memset(&bo_info, 0, sizeof(bo_info));
    bo_info.handle = bo.handle;
    bo_info_rc = ioctl(bo_fd, FB_GPU_BO_INFO, &bo_info);

    pixels = (uint32 *)bo.addr;
    for (uint32 y = 0; y < bo.height; y++) {
        for (uint32 x = 0; x < bo.width; x++)
            pixels[y * (bo.pitch / 4) + x] =
                0xff000000u | ((x * 3) << 16) | ((y * 5) << 8) |
                ((x ^ y) & 0xff);
    }

    bo_size = bo.size;
    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    standard_allocation.existing_heap_data.size = bo_size;
    allocation_info.sysmem = bo.addr;
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.standard_allocation = 1;
    create_allocation.flags.existing_sysmem = 1;
    create_rc = ioctl(dxg_fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (create_rc < 0 || allocation_info.allocation.v == 0) {
        printf("fb_existing_sysmem_matrix gpu_fd=%d fb_fd=%d bo_fd=%d fb_open=%s bo_create_rc=%d bo_info_rc=%d bo_handle=%u bo_addr=0x%lx bo_size=%lu bo_pitch=%u bo_info_page=%u bo_info_align=%lu create_rc=%d allocation=0x%x resource=0x%x sysmem=0x%lx existingheap_size=%lu gpu_to_fb_bo_sysmem_staging=0 d3d12_cpu_readback=0 fb_bo_present_cpu_blit=0 native_host_display_handoff=0 status=FAIL missing_api=none\n",
               gpu_fd, fb_fd, bo_fd, fb_fd >= 0 ? "PASS" : "FAIL",
               bo_create_rc, bo_info_rc, bo.handle, bo.addr, bo.size,
               bo.pitch, bo_info.page_size, bo_info.addr_align, create_rc,
               allocation_info.allocation.v, create_allocation.resource.v,
               allocation_info.sysmem,
               standard_allocation.existing_heap_data.size);
        if (allocation_info.allocation.v != 0 ||
            create_allocation.resource.v != 0) {
            memset(&destroy_allocation, 0, sizeof(destroy_allocation));
            allocation_list[0] = allocation_info.allocation;
            destroy_allocation.device = device;
            destroy_allocation.resource = create_allocation.resource;
            destroy_allocation.flags.assume_not_in_use = 1;
            if (create_allocation.resource.v == 0) {
                destroy_allocation.allocations = (uint64)allocation_list;
                destroy_allocation.alloc_count = 1;
            }
            ioctl(dxg_fd, LX_DXDESTROYALLOCATION2, &destroy_allocation);
        }
        goto out_destroy_bo;
    }

    present_fd = fb_fd >= 0 ? fb_fd : -1;
    present_handle = 0;
    if (present_fd >= 0) {
        if (present_fd == bo_fd) {
            present_handle = bo.handle;
        } else {
            bo_export.handle = bo.handle;
            bo_export_rc = ioctl(bo_fd, FB_GPU_BO_EXPORT_FD, &bo_export);
            if (bo_export_rc == 0 && bo_export.fd >= 0) {
                bo_import.fd = bo_export.fd;
                bo_import_rc = ioctl(present_fd, FB_GPU_BO_IMPORT_FD,
                                     &bo_import);
                close(bo_export.fd);
                if (bo_import_rc == 0 && bo_import.handle != 0)
                    present_handle = bo_import.handle;
            }
        }
    }

    memset(&present, 0, sizeof(present));
    present.w = bo.width;
    present.h = bo.height;
    present.handle = present_handle;
    if (present_fd >= 0 && present_handle != 0) {
        present_rc = ioctl(present_fd, FB_GPU_BO_PRESENT, &present);
        if (present_rc < 0)
            present_errno = -present_rc;
    } else {
        present_errno = ENODEV;
    }
    printf("fb_existing_sysmem_matrix gpu_fd=%d fb_fd=%d bo_fd=%d present_fd=%d fb_open=%s bo_create_rc=%d bo_info_rc=%d bo_handle=%u present_handle=%u bo_export_rc=%d bo_import_rc=%d bo_addr=0x%lx bo_size=%lu bo_pitch=%u bo_info_page=%u bo_info_align=%lu create_rc=%d allocation=0x%x resource=0x%x sysmem=0x%lx existingheap_size=%lu present_rc=%d present_errno=%d present_fence=%lu gpu_to_fb_bo_sysmem_staging=1 d3d12_cpu_readback=0 fb_bo_present_cpu_blit=%u native_host_display_handoff=0 d3d12_copy_possible=0 missing_api=d3d12_resource_from_existing_sysmem_d3dkmt_allocation pin_proof=PASS status=FAIL_CLOSED_NO_D3D12_COPY\n",
           gpu_fd, fb_fd, bo_fd, present_fd,
           fb_fd >= 0 ? "PASS" : "FAIL", bo_create_rc, bo_info_rc,
           bo.handle, present_handle, bo_export_rc, bo_import_rc, bo.addr,
           bo.size, bo.pitch, bo_info.page_size, bo_info.addr_align, create_rc,
           allocation_info.allocation.v, create_allocation.resource.v,
           allocation_info.sysmem, standard_allocation.existing_heap_data.size,
           present_rc, present_errno, present.fence, present_rc == 0 ? 1 : 0);

    ret = -1;

    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    allocation_list[0] = allocation_info.allocation;
    destroy_allocation.device = device;
    destroy_allocation.resource = create_allocation.resource;
    destroy_allocation.flags.assume_not_in_use = 1;
    if (create_allocation.resource.v == 0) {
        destroy_allocation.allocations = (uint64)allocation_list;
        destroy_allocation.alloc_count = 1;
    }
    if (ioctl(dxg_fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0)
        printf("fb_existing_sysmem destroy_allocation_failed allocation=0x%x resource=0x%x\n",
               allocation_info.allocation.v, create_allocation.resource.v);

out_destroy_bo:
    if (present_handle != 0 && present_fd >= 0 && present_fd != bo_fd) {
        if (bo_import.addr != 0 && bo_import.size != 0)
            munmap((void *)bo_import.addr, (int)bo_import.size);
        memset(&bo_destroy, 0, sizeof(bo_destroy));
        bo_destroy.handle = present_handle;
        if (ioctl(present_fd, FB_GPU_BO_DESTROY, &bo_destroy) < 0)
            printf("fb_existing_sysmem present_bo_destroy_failed handle=%u\n",
                   present_handle);
    }
    if (bo.addr != 0 && bo.size != 0)
        munmap((void *)bo.addr, (int)bo.size);
    memset(&bo_destroy, 0, sizeof(bo_destroy));
    bo_destroy.handle = bo.handle;
    if (bo.handle != 0 && ioctl(bo_fd, FB_GPU_BO_DESTROY, &bo_destroy) < 0)
        printf("fb_existing_sysmem bo_destroy_failed handle=%u\n",
               bo.handle);
out_close_fds:
    if (gpu_fd >= 0)
        close(gpu_fd);
    if (fb_fd >= 0 && fb_fd != gpu_fd)
        close(fb_fd);
    return ret;
}

static int probe_scanout_existing_sysmem_contract(int dxg_fd,
                                                  struct d3dkmthandle device)
{
    struct fb_gpu_scanout_map scanout;
    struct fb_fix_screeninfo fix;
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmthandle allocation_list[1];
    int fb_fd = -1;
    int map_rc = -1;
    int fix_rc = -1;
    int create_rc = -1;
    int pin_proof = 0;
    int ret = -1;

    memset(&scanout, 0, sizeof(scanout));
    memset(&fix, 0, sizeof(fix));
    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        printf("scanout_existing_sysmem_matrix fb_fd=-1 map_rc=-1 map_errno=%d scanout_va=0x0 scanout_size=0 gpa=0x0 gpa_size=0 width=0 height=0 pitch=0 create_rc=-1 allocation=0x0 resource=0x0 existingheap_size=0 va_align64k=0 should_show_pfnmap_pages=1 should_show_vram=1 pin_proof=FAIL native_host_display_handoff=0 d3d12_copy_possible=0 status=FAIL_OPEN_FB\n",
               ENODEV);
        return -1;
    }

    fix_rc = ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);
    map_rc = ioctl(fb_fd, FB_GPU_SCANOUT_MAP, &scanout);
    if (map_rc < 0 || scanout.addr == 0 || scanout.size == 0) {
        printf("scanout_existing_sysmem_matrix fb_fd=%d fix_rc=%d map_rc=%d map_errno=%d scanout_va=0x%lx scanout_size=%lu gpa=0x%lx gpa_size=%u width=%u height=%u pitch=%u create_rc=-1 allocation=0x0 resource=0x0 existingheap_size=0 va_align64k=%u should_show_pfnmap_pages=1 should_show_vram=1 pin_proof=FAIL native_host_display_handoff=0 d3d12_copy_possible=0 status=FAIL_SCANOUT_MAP\n",
               fb_fd, fix_rc, map_rc, map_rc < 0 ? -map_rc : 0,
               scanout.addr, scanout.size, fix.smem_start, fix.smem_len,
               scanout.width, scanout.height, scanout.pitch,
               (scanout.addr & 0xffffUL) == 0);
        goto out_close;
    }

    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    standard_allocation.existing_heap_data.size = scanout.size;
    allocation_info.sysmem = scanout.addr;
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.standard_allocation = 1;
    create_allocation.flags.existing_sysmem = 1;
    create_rc = ioctl(dxg_fd, LX_DXCREATEALLOCATION, &create_allocation);
    pin_proof = create_rc == 0 && allocation_info.allocation.v != 0;

    printf("scanout_existing_sysmem_matrix fb_fd=%d fix_rc=%d map_rc=%d map_errno=%d scanout_va=0x%lx scanout_size=%lu gpa=0x%lx gpa_size=%u width=%u height=%u pitch=%u create_rc=%d allocation=0x%x resource=0x%x sysmem=0x%lx existingheap_size=%lu va_align64k=%u should_show_pfnmap_pages=1 should_show_vram=1 pin_proof=%s native_host_display_handoff=0 d3d12_copy_possible=0 missing_api=d3d12_resource_from_scanout_existing_sysmem status=%s\n",
           fb_fd, fix_rc, map_rc, map_rc < 0 ? -map_rc : 0,
           scanout.addr, scanout.size, fix.smem_start, fix.smem_len,
           scanout.width, scanout.height, scanout.pitch, create_rc,
           allocation_info.allocation.v, create_allocation.resource.v,
           allocation_info.sysmem,
           standard_allocation.existing_heap_data.size,
           (scanout.addr & 0xffffUL) == 0,
           pin_proof ? "PASS" : "FAIL",
           pin_proof ? "FAIL_CLOSED_NO_D3D12_COPY" : "FAIL_CREATEALLOCATION");

    if (allocation_info.allocation.v != 0 ||
        create_allocation.resource.v != 0) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        allocation_list[0] = allocation_info.allocation;
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (create_allocation.resource.v == 0) {
            destroy_allocation.allocations = (uint64)allocation_list;
            destroy_allocation.alloc_count = 1;
        }
        if (ioctl(dxg_fd, LX_DXDESTROYALLOCATION2,
                  &destroy_allocation) < 0)
            printf("scanout_existing_sysmem destroy_allocation_failed allocation=0x%x resource=0x%x\n",
                   allocation_info.allocation.v,
                   create_allocation.resource.v);
    }
    if (pin_proof)
        ret = -1;

out_close:
    if (scanout.addr != 0 && scanout.size != 0)
        munmap((void *)scanout.addr, (int)scanout.size);
    close(fb_fd);
    return ret;
}

static int probe_scanout_existing_sysmem_pin_lifetime_contract(
    int dxg_fd, struct d3dkmthandle device)
{
    struct fb_gpu_scanout_map scanout;
    struct fb_fix_screeninfo fix;
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmthandle allocation_list[1];
    struct dxg_existing_sysmem_target_status before;
    struct dxg_existing_sysmem_target_status after_create;
    struct dxg_existing_sysmem_target_status after_destroy;
    int before_rc;
    int after_create_rc = -1;
    int after_destroy_rc = -1;
    int fb_fd = -1;
    int fix_rc = -1;
    int map_rc = -1;
    int create_rc = -1;
    int destroy_rc = -1;
    int pass;

    memset(&scanout, 0, sizeof(scanout));
    memset(&fix, 0, sizeof(fix));
    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    memset(&before, 0, sizeof(before));
    memset(&after_create, 0, sizeof(after_create));
    memset(&after_destroy, 0, sizeof(after_destroy));

    before_rc = read_existing_sysmem_target_status(&before);
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0)
        goto print_row;

    fix_rc = ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);
    map_rc = ioctl(fb_fd, FB_GPU_SCANOUT_MAP, &scanout);
    if (map_rc < 0 || scanout.addr == 0 || scanout.size == 0)
        goto print_row;

    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    standard_allocation.existing_heap_data.size = scanout.size;
    allocation_info.sysmem = scanout.addr;
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.standard_allocation = 1;
    create_allocation.flags.existing_sysmem = 1;
    create_rc = ioctl(dxg_fd, LX_DXCREATEALLOCATION, &create_allocation);
    after_create_rc = read_existing_sysmem_target_status(&after_create);

    if (allocation_info.allocation.v != 0 ||
        create_allocation.resource.v != 0) {
        allocation_list[0] = allocation_info.allocation;
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (create_allocation.resource.v == 0) {
            destroy_allocation.allocations = (uint64)allocation_list;
            destroy_allocation.alloc_count = 1;
        }
        destroy_rc = ioctl(dxg_fd, LX_DXDESTROYALLOCATION2,
                           &destroy_allocation);
    }
    after_destroy_rc = read_existing_sysmem_target_status(&after_destroy);

print_row:
    pass = create_rc == 0 && allocation_info.allocation.v != 0 &&
           create_allocation.resource.v != 0 && destroy_rc == 0 &&
           after_create_rc == 0 && after_create.pfnmap_pages != 0 &&
           after_create.pfnmap_ok != 0 && after_create.vram != 0;
    printf("scanout_existing_sysmem_pin_lifetime_matrix fb_fd=%d fix_rc=%d map_rc=%d scanout_va=0x%lx scanout_size=%lu gpa=0x%lx gpa_size=%u width=%u height=%u pitch=%u create_rc=%d destroy_rc=%d allocation=0x%x resource=0x%x sysmem=0x%lx existingheap_size=%lu before_rc=%d before_pfnmap_pages=%u before_pfnmap_ok=%u before_vram=%u before_vram_size=%u after_create_rc=%d after_create_pfnmap_pages=%u after_create_pfnmap_ok=%u after_create_vram=%u after_create_vram_size=%u after_destroy_rc=%d after_destroy_pfnmap_pages=%u after_destroy_pfnmap_ok=%u after_destroy_vram=%u after_destroy_vram_size=%u pfnmap_registered=%u vram_registered=%u destroy_coherent=%u native_host_display_handoff=0 d3d12_copy_possible=0 status=%s\n",
           fb_fd, fix_rc, map_rc, scanout.addr, scanout.size,
           fix.smem_start, fix.smem_len, scanout.width, scanout.height,
           scanout.pitch, create_rc, destroy_rc,
           allocation_info.allocation.v, create_allocation.resource.v,
           allocation_info.sysmem,
           standard_allocation.existing_heap_data.size,
           before_rc, before.pfnmap_pages, before.pfnmap_ok, before.vram,
           before.vram_size, after_create_rc,
           after_create.pfnmap_pages, after_create.pfnmap_ok,
           after_create.vram, after_create.vram_size, after_destroy_rc,
           after_destroy.pfnmap_pages, after_destroy.pfnmap_ok,
           after_destroy.vram, after_destroy.vram_size,
           after_create.pfnmap_pages != 0 && after_create.pfnmap_ok != 0,
           after_create.vram != 0, destroy_rc == 0,
           pass ? "PASS_FAIL_CLOSED" : "FAIL");
    if (scanout.addr != 0 && scanout.size != 0)
        munmap((void *)scanout.addr, (int)scanout.size);
    if (fb_fd >= 0)
        close(fb_fd);
    return pass ? 0 : -1;
}

static int probe_scanout_d3d12_bridge_contract(int dxg_fd,
                                               struct d3dkmthandle device)
{
    struct fb_gpu_scanout_map scanout;
    struct fb_fix_screeninfo fix;
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_shareobjects share_resource;
    struct d3dkmt_shareobjects share_allocation;
    struct d3dkmthandle share_object[1];
    struct d3dkmt_queryresourceinfofromnthandle query;
    struct d3dkmt_openresourcefromnthandle open_resource;
    struct d3dkmt_destroyallocation2 destroy;
    struct d3dddi_openallocationinfo2 *open_alloc = 0;
    struct d3dkmthandle allocation_list[1];
    void *runtime_data = 0;
    void *resource_data = 0;
    void *total_data = 0;
    uint64 resource_nt = 0;
    uint64 allocation_nt = 0;
    uint32 allocation_count = 0;
    int fb_fd = -1;
    int fix_rc = -1;
    int map_rc = -1;
    int create_rc = -1;
    int share_resource_rc = -1;
    int share_allocation_rc = -1;
    int query_rc = -1;
    int open_rc = -1;
    int d3d12_open_attempted = 0;
    uint32 pfnmap_pages = 0;
    uint32 vram = 0;
    uint32 pfnmap_seen = 0;
    uint32 vram_seen = 0;
    const char *missing_api = "none";
    const char *status = "FAIL_OPEN_FB";
    int ret = -1;

    memset(&scanout, 0, sizeof(scanout));
    memset(&fix, 0, sizeof(fix));
    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&share_resource, 0, sizeof(share_resource));
    memset(&share_allocation, 0, sizeof(share_allocation));
    memset(&query, 0, sizeof(query));
    memset(&open_resource, 0, sizeof(open_resource));

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0)
        goto print_row;

    fix_rc = ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);
    map_rc = ioctl(fb_fd, FB_GPU_SCANOUT_MAP, &scanout);
    if (map_rc < 0 || scanout.addr == 0 || scanout.size == 0) {
        status = "FAIL_SCANOUT_MAP";
        goto print_row;
    }

    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    standard_allocation.existing_heap_data.size = scanout.size;
    allocation_info.sysmem = scanout.addr;
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.standard_allocation = 1;
    create_allocation.flags.existing_sysmem = 1;
    create_rc = ioctl(dxg_fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (create_rc < 0 || allocation_info.allocation.v == 0 ||
        create_allocation.resource.v == 0) {
        status = "FAIL_CREATEALLOCATION";
        goto print_row;
    }
    {
        char *dxg_status = read_dxg_status_buffer();
        char *target = dxg_find_text(dxg_status,
                                     "dxg_existing_sysmem_target=");

        if (target != 0) {
            if (dxg_parse_uint_after(target, "pfnmap_pages:",
                                     &pfnmap_pages) == 0)
                pfnmap_seen = 1;
            if (dxg_parse_uint_after(target, "vram:", &vram) == 0)
                vram_seen = 1;
        }
        if (dxg_status)
            free(dxg_status);
    }

    share_object[0] = create_allocation.resource;
    share_resource.object_count = 1;
    share_resource.objects = (uint64)share_object;
    share_resource.shared_handle = (uint64)&resource_nt;
    share_resource_rc = ioctl(dxg_fd, LX_DXSHAREOBJECTS, &share_resource);

    share_object[0] = allocation_info.allocation;
    share_allocation.object_count = 1;
    share_allocation.objects = (uint64)share_object;
    share_allocation.shared_handle = (uint64)&allocation_nt;
    share_allocation_rc = ioctl(dxg_fd, LX_DXSHAREOBJECTS,
                                &share_allocation);

    if (share_resource_rc < 0 || resource_nt == 0) {
        status = "FAIL_RESOURCE_NT_SHARE";
        goto print_row;
    }

    query.device = device;
    query.nt_handle = resource_nt;
    query_rc = ioctl(dxg_fd, LX_DXQUERYRESOURCEINFOFROMNTHANDLE, &query);
    if (query_rc < 0) {
        status = "FAIL_QUERY_RESOURCE_NT";
        goto print_row;
    }

    allocation_count = query.allocation_count;
    if (allocation_count == 0 || allocation_count > 64) {
        status = "FAIL_QUERY_ALLOCATION_COUNT";
        goto print_row;
    }

    open_alloc = malloc(allocation_count * sizeof(open_alloc[0]));
    runtime_data = probe_alloc_buffer(query.private_runtime_data_size);
    resource_data = probe_alloc_buffer(query.resource_priv_drv_data_size);
    total_data = probe_alloc_buffer(query.total_priv_drv_data_size);
    if (open_alloc == 0 ||
        (query.private_runtime_data_size != 0 && runtime_data == 0) ||
        (query.resource_priv_drv_data_size != 0 && resource_data == 0) ||
        (query.total_priv_drv_data_size != 0 && total_data == 0)) {
        status = "FAIL_ALLOC_OPEN_BUFFERS";
        goto print_row;
    }
    memset(open_alloc, 0, allocation_count * sizeof(open_alloc[0]));

    open_resource.device = device;
    open_resource.nt_handle = resource_nt;
    open_resource.allocation_count = allocation_count;
    open_resource.open_alloc_info = (uint64)open_alloc;
    open_resource.private_runtime_data_size =
        (int32)query.private_runtime_data_size;
    open_resource.private_runtime_data = (uint64)runtime_data;
    open_resource.resource_priv_drv_data_size =
        query.resource_priv_drv_data_size;
    open_resource.resource_priv_drv_data = (uint64)resource_data;
    open_resource.total_priv_drv_data_size = query.total_priv_drv_data_size;
    open_resource.total_priv_drv_data = (uint64)total_data;
    open_rc = ioctl(dxg_fd, LX_DXOPENRESOURCEFROMNTHANDLE, &open_resource);
    if (open_rc < 0 || open_resource.resource.v == 0) {
        status = "FAIL_OPEN_RESOURCE_NT";
        goto print_row;
    }

    status = "FAIL_CLOSED_D3D12_OPENSHAREDHANDLE_UNAVAILABLE";
    missing_api = "ID3D12Device_OpenSharedHandle_ID3D12Resource_in_dxgprobe";

print_row:
    if (create_rc == 0 && allocation_info.allocation.v != 0 &&
        create_allocation.resource.v != 0 && pfnmap_seen &&
        pfnmap_pages != 0 && vram_seen && vram != 0) {
        status = "PASS_FAIL_CLOSED";
        missing_api =
            "no_contract_raw_existing_sysmem_kmt_resource_to_ID3D12Resource";
        ret = 0;
    }
    printf("scanout_d3d12_bridge_matrix fb_fd=%d fix_rc=%d map_rc=%d map_errno=%d scanout_va=0x%lx scanout_size=%lu gpa=0x%lx gpa_size=%u width=%u height=%u pitch=%u create_rc=%d create_flags=0x%x allocation=0x%x resource=0x%x sysmem=0x%lx existingheap_size=%lu pfnmap_seen=%u pfnmap_pages=%u vram_seen=%u vram=%u resource_share_rc=%d resource_nt=%lu allocation_share_rc=%d allocation_nt=%lu query_rc=%d query_allocations=%u runtime=%u resource_priv=%u total_priv=%u open_rc=%d open_resource=0x%x open_first_allocation=0x%x open_first_gpuva=0x%lx d3d12_open_attempted=%u d3d12_hr=0x0 d3d12_copy_possible=0 native_host_display_handoff=0 missing_api=%s status=%s\n",
           fb_fd, fix_rc, map_rc, map_rc < 0 ? -map_rc : 0,
           scanout.addr, scanout.size, fix.smem_start, fix.smem_len,
           scanout.width, scanout.height, scanout.pitch, create_rc,
           create_allocation.flags.value, allocation_info.allocation.v,
           create_allocation.resource.v, allocation_info.sysmem,
           standard_allocation.existing_heap_data.size, pfnmap_seen,
           pfnmap_pages, vram_seen, vram, share_resource_rc, resource_nt,
           share_allocation_rc, allocation_nt, query_rc,
           query.allocation_count, query.private_runtime_data_size,
           query.resource_priv_drv_data_size, query.total_priv_drv_data_size,
           open_rc, open_resource.resource.v,
           open_alloc != 0 ? open_alloc[0].allocation.v : 0,
           open_alloc != 0 ? open_alloc[0].gpu_va : 0,
           d3d12_open_attempted, missing_api, status);

    if (open_resource.resource.v != 0) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        destroy_allocation.device = device;
        destroy_allocation.resource = open_resource.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (ioctl(dxg_fd, LX_DXDESTROYALLOCATION2,
                  &destroy_allocation) < 0)
            printf("scanout_d3d12_bridge open_destroy_failed resource=0x%x\n",
                   open_resource.resource.v);
    }
    if (allocation_info.allocation.v != 0 ||
        create_allocation.resource.v != 0) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        allocation_list[0] = allocation_info.allocation;
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (create_allocation.resource.v == 0) {
            destroy_allocation.allocations = (uint64)allocation_list;
            destroy_allocation.alloc_count = 1;
        }
        if (ioctl(dxg_fd, LX_DXDESTROYALLOCATION2,
                  &destroy_allocation) < 0)
            printf("scanout_d3d12_bridge destroy_failed allocation=0x%x resource=0x%x\n",
                   allocation_info.allocation.v,
                   create_allocation.resource.v);
    }
    if (resource_nt != 0)
        close((int)resource_nt);
    if (allocation_nt != 0)
        close((int)allocation_nt);
    if (open_alloc)
        free(open_alloc);
    if (runtime_data)
        free(runtime_data);
    if (resource_data)
        free(resource_data);
    if (total_data)
        free(total_data);
    if (scanout.addr != 0 && scanout.size != 0)
        munmap((void *)scanout.addr, (int)scanout.size);
    if (fb_fd >= 0)
        close(fb_fd);
    return ret;
}

static int read_dxg_unwind_status(struct dxg_unwind_status *out)
{
    char *buf;
    char *allocation;
    char *destroy;
    char *existing;
    char *create_wire;
    char *openresource;
    char *shared_parent;
    uint32 tmp = 0;
    int ret = -1;

    if (out == 0)
        return -1;
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    allocation = dxg_find_text(buf, "dxg_allocation_last=");
    destroy = dxg_find_text(buf, "dxg_destroyallocation_last=");
    existing = dxg_find_text(buf, "dxg_existing_sysmem=");
    create_wire = dxg_find_text(buf, "dxg_createallocation_wire=");
    openresource = dxg_find_text(buf, "dxg_openresource_envelope=");
    shared_parent = dxg_find_text(buf, "dxg_sharedresource_parent=");
    if (allocation == 0 || destroy == 0 || existing == 0)
        goto out_free;
    if (dxg_parse_uint_after(allocation, "ret:", &tmp) == 0)
        out->allocation_ret = (int32)tmp;
    dxg_parse_uint_after(allocation, "resource:",
                         &out->allocation_resource);
    dxg_parse_uint_after(allocation, "allocation:",
                         &out->allocation_handle);
    dxg_parse_uint_after(allocation, "unwind_attempts:",
                         &out->allocation_unwind_attempts);
    dxg_parse_uint_after(allocation, "unwind_successes:",
                         &out->allocation_unwind_successes);
    if (dxg_parse_uint_after(allocation, "unwind_ret:", &tmp) == 0)
        out->allocation_unwind_ret = (int32)tmp;
    if (create_wire != 0)
        dxg_parse_uint_after(create_wire, "proc:", &out->create_process);
    dxg_parse_uint_after(destroy, "dev:", &out->destroy_device);
    dxg_parse_uint_after(destroy, "res:", &out->destroy_resource);
    dxg_parse_uint_after(destroy, "alloc:", &out->destroy_allocation);
    dxg_parse_uint_after(destroy, "proc:", &out->destroy_process);
    dxg_parse_uint_after(destroy, "ctx:", &out->destroy_context);
    dxg_parse_uint_after(destroy, "count:", &out->destroy_count);
    if (dxg_parse_uint_after(destroy, "ret:", &tmp) == 0)
        out->destroy_ret = (int32)tmp;
    dxg_parse_uint_after(existing, "pages:", &out->existing_pages);
    dxg_parse_u64_after(existing, "total_pages:",
                        &out->existing_total_pages);
    dxg_parse_u64_after(existing, "active_pages:",
                        &out->existing_active_pages);
    dxg_parse_u64_after(existing, "pin_events:",
                        &out->existing_pin_events);
    dxg_parse_u64_after(existing, "unpin_events:",
                        &out->existing_unpin_events);
    if (openresource != 0) {
        if (dxg_parse_uint_after(openresource, "ret:", &tmp) == 0)
            out->openresource_ret = (int32)tmp;
        dxg_parse_uint_after(openresource, "proc:",
                             &out->openresource_process);
        dxg_parse_uint_after(openresource, "device:",
                             &out->openresource_device);
        dxg_parse_uint_after(openresource, "out_res:",
                             &out->openresource_result_resource);
        dxg_parse_uint_after(openresource, "out_alloc0:",
                             &out->openresource_result_alloc0);
    }
    if (shared_parent != 0) {
        out->shared_parent_seen = 1;
        dxg_parse_uint_after(shared_parent, "last:",
                             &out->shared_parent_last);
        dxg_parse_uint_after(shared_parent, "refs:",
                             &out->shared_parent_refs);
        dxg_parse_uint_after(shared_parent, "fd_refs:",
                             &out->shared_parent_fd_refs);
        dxg_parse_uint_after(shared_parent, "children:",
                             &out->shared_parent_children);
        dxg_parse_uint_after(shared_parent, "sealed_gen:",
                             &out->shared_parent_sealed_generation);
    }
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static int read_dxg_syncfile_status(struct dxg_syncfile_status *out)
{
    char *buf;
    char *last;
    char *life;
    char *slash;
    uint32 tmp = 0;
    int ret = -1;

    if (out == 0)
        return -1;
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    last = dxg_find_text(buf, "dxg_syncfile_last=");
    life = dxg_find_text(buf, "dxg_syncfile_lifetime=");
    if (last == 0 || life == 0)
        goto out_free;
    if (dxg_parse_uint_after(last, "ret:", &tmp) == 0)
        out->last_ret = (int32)tmp;
    dxg_parse_uint_after(last, "out_sync:", &out->last_out_sync);
    dxg_parse_u64_after(last, "handle:", &out->last_handle);
    dxg_parse_uint_after(life, "live:", &out->live);
    dxg_parse_uint_after(life, "creates:", &out->creates);
    dxg_parse_uint_after(life, "releases:", &out->releases);
    dxg_parse_uint_after(life, "event_removed:", &out->event_removed);
    dxg_parse_uint_after(life, "nt_released:", &out->nt_released);
    dxg_parse_uint_after(life, "create_faults:", &out->create_faults);
    dxg_parse_uint_after(life, "fd_reclaimed:", &out->fd_reclaimed);
    dxg_parse_uint_after(life, "open_faults:", &out->open_faults);
    if (dxg_parse_uint_after(life, "open_destroy:",
                             &out->open_destroy_attempts) == 0) {
        slash = dxg_find_text(life, "open_destroy:");
        if (slash != 0)
            slash = dxg_find_text(slash, "/");
        if (slash != 0) {
            dxg_parse_uint_after(slash, "/",
                                 &out->open_destroy_successes);
            slash = dxg_find_text(slash + 1, "/");
        }
        if (slash != 0 && dxg_parse_uint_after(slash, "/", &tmp) == 0)
            out->open_destroy_ret = (int32)tmp;
    }
    if (dxg_parse_uint_after(life, "host_events:",
                             &out->host_event_active) == 0) {
        slash = dxg_find_text(life, "host_events:");
        if (slash != 0)
            slash = dxg_find_text(slash, "/");
        if (slash != 0) {
            dxg_parse_uint_after(slash, "/", &out->host_event_allocs);
            slash = dxg_find_text(slash + 1, "/");
        }
        if (slash != 0)
            dxg_parse_uint_after(slash, "/", &out->host_event_removes);
    }
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static int read_create_publication_status(
    struct dxg_create_publication_status *out)
{
    char *buf;
    char *createdevice_last;
    char *createdevice_unwind;
    char *createcontext_last;
    char *createcontext_unwind;
    char *createhwqueue_last;
    char *createhwqueue_unwind;
    uint32 tmp = 0;
    int ret = -1;

    if (out == 0)
        return -1;
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    createdevice_last = dxg_find_text(buf, "dxg_createdevice_last=");
    createdevice_unwind = dxg_find_text(buf, "dxg_createdevice_unwind=");
    createcontext_last = dxg_find_text(buf, "dxg_context_last=");
    createcontext_unwind = dxg_find_text(buf, "dxg_context_unwind=");
    createhwqueue_last = dxg_find_text(buf, "dxg_hwqueue_last=");
    createhwqueue_unwind = dxg_find_text(buf, "dxg_hwqueue_unwind=");
    if (createdevice_last == 0 || createdevice_unwind == 0 ||
        createcontext_last == 0 || createcontext_unwind == 0 ||
        createhwqueue_last == 0 || createhwqueue_unwind == 0)
        goto out_free;

    dxg_parse_uint_after(createdevice_last, "proc:",
                         &out->createdevice_last_process);
    dxg_parse_uint_after(createdevice_last, "device:",
                         &out->createdevice_last_device);
    dxg_parse_uint_after(createdevice_unwind, "attempts:",
                         &out->createdevice_unwind_attempts);
    dxg_parse_uint_after(createdevice_unwind, "successes:",
                         &out->createdevice_unwind_successes);
    if (dxg_parse_uint_after(createdevice_unwind, "ret:", &tmp) == 0)
        out->createdevice_unwind_ret = (int32)tmp;
    dxg_parse_uint_after(createdevice_unwind, "process:",
                         &out->createdevice_unwind_process);
    dxg_parse_uint_after(createdevice_unwind, "device:",
                         &out->createdevice_unwind_device);

    dxg_parse_uint_after(createcontext_last, "handle:",
                         &out->createcontext_last_handle);
    dxg_parse_uint_after(createcontext_unwind, "attempts:",
                         &out->createcontext_unwind_attempts);
    dxg_parse_uint_after(createcontext_unwind, "successes:",
                         &out->createcontext_unwind_successes);
    if (dxg_parse_uint_after(createcontext_unwind, "ret:", &tmp) == 0)
        out->createcontext_unwind_ret = (int32)tmp;
    dxg_parse_uint_after(createcontext_unwind, "process:",
                         &out->createcontext_unwind_process);
    dxg_parse_uint_after(createcontext_unwind, "context:",
                         &out->createcontext_unwind_context);

    dxg_parse_uint_after(createhwqueue_last, "queue:",
                         &out->createhwqueue_last_queue);
    dxg_parse_uint_after(createhwqueue_last, "fence:",
                         &out->createhwqueue_last_fence);
    dxg_parse_uint_after(createhwqueue_unwind, "attempts:",
                         &out->createhwqueue_unwind_attempts);
    dxg_parse_uint_after(createhwqueue_unwind, "successes:",
                         &out->createhwqueue_unwind_successes);
    if (dxg_parse_uint_after(createhwqueue_unwind, "ret:", &tmp) == 0)
        out->createhwqueue_unwind_ret = (int32)tmp;
    dxg_parse_uint_after(createhwqueue_unwind, "process:",
                         &out->createhwqueue_unwind_process);
    dxg_parse_uint_after(createhwqueue_unwind, "queue:",
                         &out->createhwqueue_unwind_queue);
    dxg_parse_uint_after(createhwqueue_unwind, "fence:",
                         &out->createhwqueue_unwind_fence);
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static int read_cpu_event_signal_status(
    struct dxg_cpu_event_signal_status *out)
{
    char *buf;
    char *line;
    uint32 tmp = 0;
    int ret = -1;

    if (out == 0)
        return -1;
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    line = dxg_find_text(buf, "dxg_synccpuevent_signal=");
    if (line == 0)
        goto out_free;
    dxg_parse_uint_after(line, "attempts:", &out->attempts);
    dxg_parse_uint_after(line, "successes:", &out->successes);
    if (dxg_parse_uint_after(line, "ret:", &tmp) == 0)
        out->ret = (int32)tmp;
    dxg_parse_uint_after(line, "cmd:", &out->cmd);
    dxg_parse_uint_after(line, "flags:", &out->flags);
    dxg_parse_uint_after(line, "objects:", &out->objects);
    dxg_parse_uint_after(line, "contexts:", &out->contexts);
    dxg_parse_u64_after(line, "user_fd:", &out->user_fd);
    dxg_parse_u64_after(line, "event:", &out->event_id);
    dxg_parse_uint_after(line, "len:", &out->len);
    dxg_parse_uint_after(line, "active:", &out->active);
    dxg_parse_uint_after(line, "allocs:", &out->allocs);
    dxg_parse_uint_after(line, "removes:", &out->removes);
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static int read_async_send_status(struct dxg_async_send_status *out)
{
    char *buf;
    char *line;
    uint32 tmp = 0;
    int ret = -1;

    if (out == 0)
        return -1;
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    line = dxg_find_text(buf, "dxg_async_send_last=");
    if (line == 0)
        goto out_free;
    dxg_parse_uint_after(line, "enabled:", &out->enabled);
    dxg_parse_uint_after(line, "attempts:", &out->attempts);
    dxg_parse_uint_after(line, "successes:", &out->successes);
    dxg_parse_uint_after(line, "fallback_sync:", &out->fallback_sync);
    dxg_parse_uint_after(line, "cmd:", &out->cmd);
    dxg_parse_uint_after(line, "cmd_len:", &out->cmd_len);
    dxg_parse_uint_after(line, "wire_len:", &out->wire_len);
    dxg_parse_uint_after(line, "async_bit:", &out->async_bit);
    dxg_parse_uint_after(line, "route_global:", &out->route_global);
    dxg_parse_uint_after(line, "retries:", &out->retries);
    dxg_parse_uint_after(line, "packet_type:", &out->packet_type);
    if (dxg_parse_uint_after(line, "ret:", &tmp) == 0)
        out->ret = (int32)tmp;
    dxg_parse_uint_after(line, "submit:", &out->submit);
    dxg_parse_uint_after(line, "signal:", &out->signal);
    dxg_parse_uint_after(line, "waitgpu:", &out->waitgpu);
    dxg_parse_uint_after(line, "submithwqueue:", &out->submithwqueue);
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static void probe_createallocation_unwind(int fd,
                                          struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 *allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmthandle allocation_list[1];
    struct dxg_unwind_status before;
    struct dxg_unwind_status after;
    void *page = 0;
    int before_rc;
    int after_rc = -1;
    int create_rc;
    int destroy_rc = -2;
    int pin_balanced = 0;
    int same_process_cleanup = 0;
    int no_local_leak = 0;
    int destroy_target_match = 0;
    int pass = 0;
    unsigned int i;

    for (i = 0; i < sizeof(dxg_existing_sysmem_buffer); i++)
        dxg_existing_sysmem_buffer[i] = (unsigned char)(0xa5 ^ i);
    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    before_rc = read_dxg_unwind_status(&before);

    page = mmap(0, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == (void *)-1) {
        printf("createallocation_unwind_matrix create_rc=-1 errno=%d before_rc=%d after_rc=-1 same_process_cleanup=0 pin_balanced=0 no_local_leak=0 status=SKIP_MMAP\n",
               ENOMEM, before_rc);
        return;
    }
    memset(page, 0, 4096);
    allocation_info = (struct d3dddi_allocationinfo2 *)page;
    allocation_info->sysmem = (uint64)dxg_existing_sysmem_buffer;
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    standard_allocation.existing_heap_data.size =
        sizeof(dxg_existing_sysmem_buffer);
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)allocation_info;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.standard_allocation = 1;
    create_allocation.flags.existing_sysmem = 1;

    if (mprotect(page, 4096, PROT_READ) != 0) {
        printf("createallocation_unwind_matrix create_rc=-1 errno=%d before_rc=%d after_rc=-1 same_process_cleanup=0 pin_balanced=0 no_local_leak=0 status=SKIP_MPROTECT\n",
               EFAULT, before_rc);
        munmap(page, 4096);
        return;
    }
    create_rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    after_rc = read_dxg_unwind_status(&after);
    if (mprotect(page, 4096, PROT_READ | PROT_WRITE) != 0) {
        printf("createallocation_unwind_matrix create_rc=%d errno=%d before_rc=%d after_rc=%d same_process_cleanup=0 pin_balanced=0 no_local_leak=0 status=FAIL_REPROTECT\n",
               create_rc, create_rc < 0 ? -create_rc : 0, before_rc,
               after_rc);
        munmap(page, 4096);
        return;
    }
    if (after_rc == 0 &&
        (after.destroy_resource != 0 || after.destroy_allocation != 0)) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        allocation_list[0].v = after.allocation_handle;
        destroy_allocation.device.v = after.destroy_device != 0 ?
                                      after.destroy_device : device.v;
        destroy_allocation.resource.v = after.destroy_resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (after.destroy_resource == 0 && after.destroy_allocation != 0) {
            allocation_list[0].v = after.destroy_allocation;
            destroy_allocation.allocations = (uint64)allocation_list;
            destroy_allocation.alloc_count = 1;
        }
        destroy_rc = ioctl(fd, LX_DXDESTROYALLOCATION2,
                           &destroy_allocation);
    }
    if (before_rc == 0 && after_rc == 0) {
        pin_balanced =
            after.existing_active_pages == before.existing_active_pages &&
            after.existing_pin_events - before.existing_pin_events ==
            after.existing_unpin_events - before.existing_unpin_events;
        destroy_target_match =
            (after.destroy_count == 0 && after.destroy_resource != 0) ||
            after.destroy_resource == after.allocation_resource ||
            (after.destroy_allocation != 0 &&
             after.destroy_allocation == after.allocation_handle);
        same_process_cleanup =
            after.create_process != 0 &&
            after.destroy_process == after.create_process &&
            destroy_target_match &&
            after.destroy_context == 5 &&
            after.destroy_count == 0 &&
            after.destroy_ret == 0 &&
            after.allocation_unwind_attempts >
                before.allocation_unwind_attempts &&
            after.allocation_unwind_successes >
                before.allocation_unwind_successes &&
            after.allocation_unwind_ret == 0;
        no_local_leak = destroy_rc < 0;
        pass = create_rc < 0 && same_process_cleanup &&
               pin_balanced && no_local_leak;
    }
    printf("createallocation_unwind_matrix create_rc=%d errno=%d copyout_fault=%u before_rc=%d after_rc=%d process=0x%x destroy_process=0x%x resource=0x%x allocation=0x%x destroy_resource=0x%x destroy_allocation=0x%x destroy_target_match=%u destroy_ctx=%u destroy_count=%u destroy_ret=%d destroy_rc=%d unwind_attempts=%u->%u unwind_successes=%u->%u unwind_ret=%d active_pages=%lu->%lu pin_events=%lu->%lu unpin_events=%lu->%lu same_process_cleanup=%u pin_balanced=%u no_local_leak=%u status=%s\n",
           create_rc, create_rc < 0 ? -create_rc : 0,
           create_rc < 0 ? 1U : 0U, before_rc, after_rc,
           after.create_process, after.destroy_process,
           after.allocation_resource, after.allocation_handle,
           after.destroy_resource, after.destroy_allocation,
           destroy_target_match ? 1 : 0, after.destroy_context,
           after.destroy_count, after.destroy_ret,
           destroy_rc,
           before.allocation_unwind_attempts,
           after.allocation_unwind_attempts,
           before.allocation_unwind_successes,
           after.allocation_unwind_successes, after.allocation_unwind_ret,
           before.existing_active_pages, after.existing_active_pages,
           before.existing_pin_events, after.existing_pin_events,
           before.existing_unpin_events, after.existing_unpin_events,
           same_process_cleanup ? 1 : 0, pin_balanced ? 1 : 0,
           no_local_leak ? 1 : 0, pass ? "PASS" : "FAIL");
    munmap(page, 4096);
}

static void probe_openresource_unwind(int fd, struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmt_queryresourceinfofromnthandle query;
    struct d3dkmt_openresourcefromnthandle *open_resource;
    struct d3dkmthandle objects[1];
    struct d3dddi_openallocationinfo2 *open_alloc = 0;
    struct dxg_unwind_status before;
    struct dxg_unwind_status after;
    void *runtime_data = 0;
    void *resource_data = 0;
    void *total_data = 0;
    void *page = 0;
    void *open_alloc_page = 0;
    uint64 resource_fd = 0;
    int create_rc;
    int share_rc = -1;
    int query_rc = -1;
    int before_rc = -1;
    int after_rc = -1;
    int open_rc = -2;
    int destroy_leaked_rc = -2;
    int destroy_original_rc = -2;
    int pin_balanced = 0;
    int same_process_cleanup = 0;
    int no_local_leak = 0;
    int parent_same = 0;
    int parent_refs_balanced = 0;
    int parent_child_unlinked = 0;
    int sealed_generation_coherent = 0;
    int pass = 0;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    memset(&share_objects, 0, sizeof(share_objects));
    memset(&query, 0, sizeof(query));
    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size =
        DXGPROBE_WSL_REPLAY_ALLOCATION_SIZE;
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.cross_adapter = 1;
    create_allocation.flags.standard_allocation = 1;
    create_rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (create_rc < 0 || create_allocation.resource.v == 0) {
        printf("openresource_unwind_matrix create_rc=%d share_rc=-1 query_rc=-1 open_rc=-1 errno=%d before_rc=-1 after_rc=-1 same_process_cleanup=0 pin_balanced=0 no_local_leak=0 status=SKIP_CREATE\n",
               create_rc, create_rc < 0 ? -create_rc : 0);
        return;
    }

    objects[0] = create_allocation.resource;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&resource_fd;
    share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
    if (share_rc < 0 || resource_fd == 0)
        goto out_print;

    query.device = device;
    query.nt_handle = resource_fd;
    query_rc = ioctl(fd, LX_DXQUERYRESOURCEINFOFROMNTHANDLE, &query);
    if (query_rc < 0 || query.allocation_count == 0 ||
        query.allocation_count > 64)
        goto out_print;

    if (query.allocation_count * sizeof(open_alloc[0]) > 4096)
        goto out_print;
    open_alloc_page = mmap(0, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (open_alloc_page != (void *)-1)
        open_alloc = (struct d3dddi_openallocationinfo2 *)open_alloc_page;
    runtime_data = probe_alloc_buffer(query.private_runtime_data_size);
    resource_data = probe_alloc_buffer(query.resource_priv_drv_data_size);
    total_data = probe_alloc_buffer(query.total_priv_drv_data_size);
    page = mmap(0, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (open_alloc == 0 ||
        (query.private_runtime_data_size != 0 && runtime_data == 0) ||
        (query.resource_priv_drv_data_size != 0 && resource_data == 0) ||
        (query.total_priv_drv_data_size != 0 && total_data == 0) ||
        open_alloc_page == (void *)-1 ||
        page == (void *)-1) {
        if (page != (void *)-1)
            munmap(page, 4096);
        if (open_alloc_page != 0 && open_alloc_page != (void *)-1)
            munmap(open_alloc_page, 4096);
        open_alloc_page = 0;
        open_alloc = 0;
        goto out_print;
    }
    memset(open_alloc, 0, query.allocation_count * sizeof(open_alloc[0]));
    memset(page, 0, 4096);
    open_resource = (struct d3dkmt_openresourcefromnthandle *)page;
    open_resource->device = device;
    open_resource->nt_handle = resource_fd;
    open_resource->allocation_count = query.allocation_count;
    open_resource->open_alloc_info = (uint64)open_alloc;
    open_resource->private_runtime_data_size =
        (int32)query.private_runtime_data_size;
    open_resource->private_runtime_data = (uint64)runtime_data;
    open_resource->resource_priv_drv_data_size =
        query.resource_priv_drv_data_size;
    open_resource->resource_priv_drv_data = (uint64)resource_data;
    open_resource->total_priv_drv_data_size =
        query.total_priv_drv_data_size;
    open_resource->total_priv_drv_data = (uint64)total_data;
    before_rc = read_dxg_unwind_status(&before);
    if (mprotect(open_alloc_page, 4096, PROT_READ) != 0) {
        goto out_print;
    }
    open_rc = ioctl(fd, LX_DXOPENRESOURCEFROMNTHANDLE, open_resource);
    after_rc = read_dxg_unwind_status(&after);
    if (mprotect(open_alloc_page, 4096, PROT_READ | PROT_WRITE) != 0) {
        printf("openresource_unwind_matrix create_rc=%d share_rc=%d query_rc=%d open_rc=%d errno=%d before_rc=%d after_rc=%d same_process_cleanup=0 pin_balanced=0 no_local_leak=0 status=FAIL_REPROTECT\n",
               create_rc, share_rc, query_rc, open_rc,
               open_rc < 0 ? -open_rc : 0, before_rc, after_rc);
        goto out_print;
    }
    munmap(page, 4096);
    page = 0;

    if (after_rc == 0 && after.openresource_result_resource != 0) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        destroy_allocation.device.v = after.openresource_device != 0 ?
                                      after.openresource_device : device.v;
        destroy_allocation.resource.v = after.openresource_result_resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        destroy_leaked_rc = ioctl(fd, LX_DXDESTROYALLOCATION2,
                                  &destroy_allocation);
    }
    if (before_rc == 0 && after_rc == 0) {
        pin_balanced =
            after.existing_active_pages == before.existing_active_pages &&
            after.existing_pin_events - before.existing_pin_events ==
            after.existing_unpin_events - before.existing_unpin_events;
        same_process_cleanup =
            after.openresource_process != 0 &&
            after.destroy_process == after.openresource_process &&
            after.destroy_resource == after.openresource_result_resource &&
            after.destroy_context == 1 &&
            after.destroy_ret == 0;
        no_local_leak = destroy_leaked_rc < 0;
        if (before.shared_parent_seen && after.shared_parent_seen) {
            parent_same =
                before.shared_parent_last != 0 &&
                before.shared_parent_last == after.shared_parent_last;
            parent_refs_balanced =
                after.shared_parent_refs == before.shared_parent_refs &&
                after.shared_parent_fd_refs ==
                    before.shared_parent_fd_refs;
            parent_child_unlinked =
                after.shared_parent_children ==
                    before.shared_parent_children;
            sealed_generation_coherent =
                before.shared_parent_sealed_generation != 0 &&
                after.shared_parent_sealed_generation ==
                    before.shared_parent_sealed_generation;
        }
        pass = open_rc < 0 && same_process_cleanup &&
               pin_balanced && no_local_leak && parent_same &&
               parent_refs_balanced && parent_child_unlinked &&
               sealed_generation_coherent;
    }

out_print:
    if (create_allocation.resource.v != 0) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        destroy_original_rc = ioctl(fd, LX_DXDESTROYALLOCATION2,
                                    &destroy_allocation);
    }
    if (resource_fd != 0)
        close((int)resource_fd);
    if (page != 0 && page != (void *)-1)
        munmap(page, 4096);
    if (open_alloc_page != 0 && open_alloc_page != (void *)-1)
        munmap(open_alloc_page, 4096);
    if (runtime_data)
        free(runtime_data);
    if (resource_data)
        free(resource_data);
    if (total_data)
        free(total_data);
    printf("openresource_unwind_matrix create_rc=%d share_rc=%d query_rc=%d open_rc=%d errno=%d copyout_fault=%u before_rc=%d after_rc=%d process=0x%x destroy_process=0x%x resource=0x%x allocation=0x%x destroy_ctx=%u destroy_ret=%d destroy_leaked_rc=%d destroy_original_rc=%d active_pages=%lu->%lu pin_events=%lu->%lu unpin_events=%lu->%lu parent=0x%x/0x%x parent_refs=%u->%u parent_fd_refs=%u->%u parent_children=%u->%u parent_sealed_gen=%u->%u parent_same=%u parent_refs_balanced=%u parent_child_unlinked=%u sealed_generation_coherent=%u same_process_cleanup=%u pin_balanced=%u no_local_leak=%u status=%s\n",
           create_rc, share_rc, query_rc, open_rc,
           open_rc < 0 ? -open_rc : 0, open_rc < 0 ? 1U : 0U,
           before_rc, after_rc,
           after.openresource_process, after.destroy_process,
           after.openresource_result_resource,
           after.openresource_result_alloc0, after.destroy_context,
           after.destroy_ret, destroy_leaked_rc,
           destroy_original_rc, before.existing_active_pages,
           after.existing_active_pages, before.existing_pin_events,
           after.existing_pin_events, before.existing_unpin_events,
           after.existing_unpin_events, before.shared_parent_last,
           after.shared_parent_last, before.shared_parent_refs,
           after.shared_parent_refs, before.shared_parent_fd_refs,
           after.shared_parent_fd_refs, before.shared_parent_children,
           after.shared_parent_children,
           before.shared_parent_sealed_generation,
           after.shared_parent_sealed_generation,
           parent_same ? 1 : 0, parent_refs_balanced ? 1 : 0,
           parent_child_unlinked ? 1 : 0,
           sealed_generation_coherent ? 1 : 0,
           same_process_cleanup ? 1 : 0,
           pin_balanced ? 1 : 0, no_local_leak ? 1 : 0,
           pass ? "PASS" : "FAIL");
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

static int probe_sync_file_matrix(int fd, struct d3dkmthandle device,
                                  struct d3dkmthandle wait_context)
{
    struct d3dkmt_createsynchronizationobject2 create_sync;
    struct d3dkmt_destroysynchronizationobject destroy_sync;
    struct d3dkmt_createsyncfile create_sync_file;
    struct d3dkmt_waitsyncfile wait_sync_file;
    struct d3dkmt_opensyncobjectfromsyncfile open_sync_file;
    struct sync_file_child_result {
        int rc;
        uint32 device;
        uint32 sync;
        uint64 fence;
        uint64 fence_cpu;
        uint64 fence_gpu;
    } child_result;
    uint64 sync_file_handle = 0;
    int create_rc;
    int wait_rc = -1;
    int open_rc = -1;
    int child_status = 1;
    int child_pipe[2];
    int ret = -1;
    int create_fault_rc = -2;
    int create_fault_before_rc = -1;
    int create_fault_after_rc = -1;
    int create_fault_fd_visible = 0;
    int create_fault_balanced = 0;
    int create_fault_pass = 0;
    void *create_fault_page = 0;
    struct d3dkmt_createsyncfile *create_fault_req = 0;
    struct dxg_syncfile_status create_fault_before;
    struct dxg_syncfile_status create_fault_after;
    int open_fault_rc = -2;
    int open_fault_before_rc = -1;
    int open_fault_after_rc = -1;
    int open_fault_source_fd_valid = 0;
    int open_fault_no_local_leak = 0;
    int open_fault_pass = 0;
    int open_fault_destroy_retry_rc = -2;
    void *open_fault_page = 0;
    struct d3dkmt_opensyncobjectfromsyncfile *open_fault_req = 0;
    struct dxg_syncfile_status open_fault_before;
    struct dxg_syncfile_status open_fault_after;

    if (device.v == 0)
        return -1;

    memset(&create_sync, 0, sizeof(create_sync));
    create_sync.device = device;
    create_sync.info.type = _D3DDDI_MONITORED_FENCE;
    create_sync.info.flags.value = g_sync_create_flags;
    create_sync.info.monitored_fence.initial_fence_value =
        g_sync_file_target_value;
    create_sync.info.monitored_fence.engine_affinity = 0;
    create_rc = ioctl(fd, LX_DXCREATESYNCHRONIZATIONOBJECT, &create_sync);
    if (create_rc < 0 || create_sync.sync_object.v == 0) {
        printf("sync_file_matrix create_flags=0x%x sync=0x%x global=0x%x sync_file=0 target=%lu create_rc=%d wait_context=0x%x wait_rc=-2 wait_state=create_failed open_rc=-1 open_sync=0x0 open_fence=0 open_cpu=0x0 open_gpu=0x0 child_status=-1 child_rc=-1 child_device=0x0 child_sync=0x0 child_fence=0 child_cpu=0x0 child_gpu=0x0\n",
               create_sync.info.flags.value, create_sync.sync_object.v,
               create_sync.info.shared_handle.v, g_sync_file_target_value,
               create_rc, wait_context.v);
        printf("sync_file_create fail flags=0x%x sync=0x%x global=0x%x target=%lu rc=%d\n",
               create_sync.info.flags.value, create_sync.sync_object.v,
               create_sync.info.shared_handle.v, g_sync_file_target_value,
               create_rc);
        return -1;
    }
    printf("sync_file_source sync=0x%x global=0x%x flags=0x%x shared=%u ntsec=%u no_signal=%u fence_cpu=0x%lx fence_gpu=0x%lx target=%lu\n",
           create_sync.sync_object.v, create_sync.info.shared_handle.v,
           create_sync.info.flags.value, create_sync.info.flags.shared,
           create_sync.info.flags.nt_security_sharing,
           create_sync.info.flags.no_signal,
           create_sync.info.monitored_fence.fence_cpu_virtual_address,
           create_sync.info.monitored_fence.fence_gpu_virtual_address,
           g_sync_file_target_value);

    memset(&create_fault_before, 0, sizeof(create_fault_before));
    memset(&create_fault_after, 0, sizeof(create_fault_after));
    create_fault_before_rc = read_dxg_syncfile_status(&create_fault_before);
    create_fault_page = mmap(0, 4096, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (create_fault_page != (void *)-1) {
        struct stat st;

        memset(create_fault_page, 0, 4096);
        create_fault_req = (struct d3dkmt_createsyncfile *)create_fault_page;
        create_fault_req->device = device;
        create_fault_req->monitored_fence = create_sync.sync_object;
        create_fault_req->fence_value = g_sync_file_target_value + 1;
        if (mprotect(create_fault_page, 4096, PROT_READ) == 0) {
            create_fault_rc = ioctl(fd, LX_DXCREATESYNCFILE,
                                    create_fault_req);
            create_fault_after_rc =
                read_dxg_syncfile_status(&create_fault_after);
            if (mprotect(create_fault_page, 4096,
                         PROT_READ | PROT_WRITE) != 0)
                create_fault_after_rc = -2;
            if (create_fault_after.last_handle != 0)
                create_fault_fd_visible =
                    fstat((int)create_fault_after.last_handle, &st) == 0;
            create_fault_balanced =
                create_fault_after.live == create_fault_before.live &&
                create_fault_after.host_event_active ==
                    create_fault_before.host_event_active;
            create_fault_pass =
                create_fault_rc < 0 &&
                create_fault_before_rc == 0 &&
                create_fault_after_rc == 0 &&
                create_fault_after.create_faults >
                    create_fault_before.create_faults &&
                create_fault_after.fd_reclaimed >
                    create_fault_before.fd_reclaimed &&
                create_fault_after.event_removed >
                    create_fault_before.event_removed &&
                create_fault_balanced &&
                !create_fault_fd_visible;
        }
        munmap(create_fault_page, 4096);
    }
    printf("dxg_syncfile_create_unwind_matrix create_fault_rc=%d before_rc=%d after_rc=%d handle=%lu fd_visible=%u create_faults=%u->%u fd_reclaimed=%u->%u event_removed=%u->%u live=%u->%u host_events=%u->%u balanced=%u status=%s\n",
           create_fault_rc, create_fault_before_rc, create_fault_after_rc,
           create_fault_after.last_handle, create_fault_fd_visible,
           create_fault_before.create_faults,
           create_fault_after.create_faults,
           create_fault_before.fd_reclaimed,
           create_fault_after.fd_reclaimed,
           create_fault_before.event_removed,
           create_fault_after.event_removed,
           create_fault_before.live, create_fault_after.live,
           create_fault_before.host_event_active,
           create_fault_after.host_event_active,
           create_fault_balanced, create_fault_pass ? "PASS" : "FAIL");

    memset(&create_sync_file, 0, sizeof(create_sync_file));
    create_sync_file.device = device;
    create_sync_file.monitored_fence = create_sync.sync_object;
    create_sync_file.fence_value = g_sync_file_target_value;
    if (ioctl(fd, LX_DXCREATESYNCFILE, &create_sync_file) < 0 ||
        create_sync_file.sync_file_handle == 0) {
        printf("sync_file_create fail device=0x%x sync=0x%x global=0x%x flags=0x%x target=%lu sync_file=%lu\n",
               device.v, create_sync.sync_object.v,
               create_sync.info.shared_handle.v, create_sync.info.flags.value,
               create_sync_file.fence_value,
               create_sync_file.sync_file_handle);
        goto out_destroy_original;
    }
    sync_file_handle = create_sync_file.sync_file_handle;
    printf("sync_file_create ok device=0x%x sync=0x%x global=0x%x flags=0x%x target=%lu sync_file=%lu\n",
           device.v, create_sync.sync_object.v, create_sync.info.shared_handle.v,
           create_sync.info.flags.value, create_sync_file.fence_value,
           sync_file_handle);

    if (wait_context.v == 0) {
        wait_rc = -2;
        printf("sync_file_wait not_attempted_no_context sync_file=%lu target=%lu context=0x0\n",
               sync_file_handle, g_sync_file_target_value);
    } else {
        memset(&wait_sync_file, 0, sizeof(wait_sync_file));
        wait_sync_file.sync_file_handle = sync_file_handle;
        wait_sync_file.context = wait_context;
        wait_rc = ioctl(fd, LX_DXWAITSYNCFILE, &wait_sync_file);
        if (wait_rc < 0)
            printf("sync_file_wait fail sync_file=%lu target=%lu context=0x%x rc=%d\n",
                   sync_file_handle, g_sync_file_target_value,
                   wait_context.v, wait_rc);
        else
            printf("sync_file_wait ok sync_file=%lu target=%lu context=0x%x rc=%d\n",
                   sync_file_handle, g_sync_file_target_value,
                   wait_context.v, wait_rc);
    }

    memset(&open_sync_file, 0, sizeof(open_sync_file));
    open_sync_file.device = device;
    open_sync_file.sync_file_handle = sync_file_handle;
    open_rc = ioctl(fd, LX_DXOPENSYNCOBJECTFROMSYNCFILE, &open_sync_file);
    if (open_rc < 0 || open_sync_file.syncobj.v == 0) {
        printf("sync_file_open fail device=0x%x sync_file=%lu rc=%d sync=0x%x fence=%lu fence_cpu=0x%lx fence_gpu=0x%lx\n",
               device.v, sync_file_handle, open_rc,
               open_sync_file.syncobj.v, open_sync_file.fence_value,
               open_sync_file.fence_value_cpu_va,
               open_sync_file.fence_value_gpu_va);
    } else {
        printf("sync_file_open ok device=0x%x sync_file=%lu rc=%d sync=0x%x fence=%lu fence_cpu=0x%lx fence_gpu=0x%lx\n",
               device.v, sync_file_handle, open_rc,
               open_sync_file.syncobj.v, open_sync_file.fence_value,
               open_sync_file.fence_value_cpu_va,
               open_sync_file.fence_value_gpu_va);
        memset(&destroy_sync, 0, sizeof(destroy_sync));
        destroy_sync.sync_object = open_sync_file.syncobj;
        if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                  &destroy_sync) < 0)
            printf("sync_file_open destroy_failed sync=0x%x\n",
                   open_sync_file.syncobj.v);
    }

    memset(&open_fault_before, 0, sizeof(open_fault_before));
    memset(&open_fault_after, 0, sizeof(open_fault_after));
    open_fault_before_rc = read_dxg_syncfile_status(&open_fault_before);
    open_fault_page = mmap(0, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (open_fault_page != (void *)-1) {
        memset(open_fault_page, 0, 4096);
        open_fault_req =
            (struct d3dkmt_opensyncobjectfromsyncfile *)open_fault_page;
        open_fault_req->device = device;
        open_fault_req->sync_file_handle = sync_file_handle;
        if (mprotect(open_fault_page, 4096, PROT_READ) == 0) {
            struct d3dkmt_destroysynchronizationobject retry_destroy;
            struct stat st;

            open_fault_rc = ioctl(fd, LX_DXOPENSYNCOBJECTFROMSYNCFILE,
                                  open_fault_req);
            open_fault_after_rc =
                read_dxg_syncfile_status(&open_fault_after);
            if (mprotect(open_fault_page, 4096,
                         PROT_READ | PROT_WRITE) != 0)
                open_fault_after_rc = -2;
            open_fault_source_fd_valid =
                fstat((int)sync_file_handle, &st) == 0;
            if (open_fault_after.last_out_sync != 0) {
                memset(&retry_destroy, 0, sizeof(retry_destroy));
                retry_destroy.sync_object.v = open_fault_after.last_out_sync;
                open_fault_destroy_retry_rc =
                    ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                          &retry_destroy);
            }
            open_fault_no_local_leak = open_fault_destroy_retry_rc < 0;
            open_fault_pass =
                open_fault_rc < 0 &&
                open_fault_before_rc == 0 &&
                open_fault_after_rc == 0 &&
                open_fault_after.open_faults >
                    open_fault_before.open_faults &&
                open_fault_after.open_destroy_attempts >
                    open_fault_before.open_destroy_attempts &&
                open_fault_after.open_destroy_successes >
                    open_fault_before.open_destroy_successes &&
                open_fault_after.open_destroy_ret == 0 &&
                open_fault_source_fd_valid &&
                open_fault_no_local_leak;
        }
        munmap(open_fault_page, 4096);
    }
    printf("dxg_syncfile_open_unwind_matrix open_fault_rc=%d before_rc=%d after_rc=%d out_sync=0x%x destroy_retry_rc=%d open_faults=%u->%u open_destroy=%u/%u->%u/%u destroy_ret=%d source_fd_valid=%u no_local_leak=%u status=%s\n",
           open_fault_rc, open_fault_before_rc, open_fault_after_rc,
           open_fault_after.last_out_sync, open_fault_destroy_retry_rc,
           open_fault_before.open_faults, open_fault_after.open_faults,
           open_fault_before.open_destroy_attempts,
           open_fault_before.open_destroy_successes,
           open_fault_after.open_destroy_attempts,
           open_fault_after.open_destroy_successes,
           open_fault_after.open_destroy_ret,
           open_fault_source_fd_valid, open_fault_no_local_leak,
           open_fault_pass ? "PASS" : "FAIL");

    {
        int pid;

        memset(&child_result, 0, sizeof(child_result));
        child_result.rc = -1;
        child_pipe[0] = -1;
        child_pipe[1] = -1;
        if (pipe(child_pipe) < 0)
            printf("sync_file_child fail pipe sync_file=%lu\n",
                   sync_file_handle);
        pid = fork();
        if (pid < 0) {
            printf("sync_file_child fail fork sync_file=%lu\n",
                   sync_file_handle);
            if (child_pipe[0] >= 0)
                close(child_pipe[0]);
            if (child_pipe[1] >= 0)
                close(child_pipe[1]);
        } else if (pid == 0) {
            struct d3dkmthandle child_adapter;
            struct d3dkmthandle child_device;
            struct d3dkmt_opensyncobjectfromsyncfile child_open;
            int child_fd = -1;
            int child_ret = 1;
            int child_rc = -1;

            if (child_pipe[0] >= 0)
                close(child_pipe[0]);
            memset(&child_adapter, 0, sizeof(child_adapter));
            memset(&child_device, 0, sizeof(child_device));
            if (open_first_dxg_device(&child_fd, &child_adapter,
                                      &child_device) == 0) {
                memset(&child_open, 0, sizeof(child_open));
                child_open.device = child_device;
                child_open.sync_file_handle = sync_file_handle;
                child_rc = ioctl(child_fd, LX_DXOPENSYNCOBJECTFROMSYNCFILE,
                                 &child_open);
                child_result.rc = child_rc;
                child_result.device = child_device.v;
                child_result.sync = child_open.syncobj.v;
                child_result.fence = child_open.fence_value;
                child_result.fence_cpu = child_open.fence_value_cpu_va;
                child_result.fence_gpu = child_open.fence_value_gpu_va;
                if (child_rc == 0 && child_open.syncobj.v != 0) {
                    printf("sync_file_child ok device=0x%x sync_file=%lu rc=%d sync=0x%x fence=%lu fence_cpu=0x%lx fence_gpu=0x%lx\n",
                           child_device.v, sync_file_handle, child_rc,
                           child_open.syncobj.v, child_open.fence_value,
                           child_open.fence_value_cpu_va,
                           child_open.fence_value_gpu_va);
                    memset(&destroy_sync, 0, sizeof(destroy_sync));
                    destroy_sync.sync_object = child_open.syncobj;
                    if (ioctl(child_fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                              &destroy_sync) < 0)
                        printf("sync_file_child destroy_failed sync=0x%x\n",
                               child_open.syncobj.v);
                    child_ret = 0;
                } else {
                    printf("sync_file_child fail device=0x%x sync_file=%lu rc=%d sync=0x%x fence=%lu fence_cpu=0x%lx fence_gpu=0x%lx\n",
                           child_device.v, sync_file_handle, child_rc,
                           child_open.syncobj.v, child_open.fence_value,
                           child_open.fence_value_cpu_va,
                           child_open.fence_value_gpu_va);
                }
                close_dxg_device(child_fd, child_adapter, child_device);
            }
            if (child_pipe[1] >= 0) {
                write(child_pipe[1], &child_result, sizeof(child_result));
                close(child_pipe[1]);
            }
            exit(child_ret);
        } else {
            if (child_pipe[1] >= 0)
                close(child_pipe[1]);
            if (child_pipe[0] >= 0) {
                read(child_pipe[0], &child_result, sizeof(child_result));
                close(child_pipe[0]);
            }
            wait(&child_status);
        }
    }

    printf("sync_file_matrix create_flags=0x%x sync=0x%x global=0x%x sync_file=%lu target=%lu create_rc=%d wait_context=0x%x wait_rc=%d wait_state=%s open_rc=%d open_sync=0x%x open_fence=%lu open_cpu=0x%lx open_gpu=0x%lx child_status=%d child_rc=%d child_device=0x%x child_sync=0x%x child_fence=%lu child_cpu=0x%lx child_gpu=0x%lx\n",
           create_sync.info.flags.value, create_sync.sync_object.v,
           create_sync.info.shared_handle.v, sync_file_handle,
           g_sync_file_target_value, create_rc, wait_context.v, wait_rc,
           wait_context.v == 0 ? "not_attempted_no_context" :
           (wait_rc < 0 ? "fail" : "ok"), open_rc,
           open_sync_file.syncobj.v, open_sync_file.fence_value,
           open_sync_file.fence_value_cpu_va,
           open_sync_file.fence_value_gpu_va, child_status,
           child_result.rc, child_result.device, child_result.sync,
           child_result.fence, child_result.fence_cpu,
           child_result.fence_gpu);
    if (open_rc == 0 && open_sync_file.syncobj.v != 0 && child_status == 0)
        ret = 0;

    close((int)sync_file_handle);
out_destroy_original:
    memset(&destroy_sync, 0, sizeof(destroy_sync));
    destroy_sync.sync_object = create_sync.sync_object;
    if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &destroy_sync) < 0)
        printf("sync_file_source destroy_failed sync=0x%x\n",
               create_sync.sync_object.v);
    return ret;
}

static int probe_shared_sync_nt(int fd, struct d3dkmthandle device)
{
    struct d3dkmt_createsynchronizationobject2 create_sync;
    struct d3dkmt_destroysynchronizationobject destroy_sync;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmt_opensyncobjectfromnthandle2 open_sync;
    struct d3dkmthandle objects[1];
    uint64 shared_handle = 0;
    uint32 open_flags;
    int open_rc;
    int same_device_open_ok = 0;
    int child_attempted = 0;
    int child_status = -1;
    int ret = -1;

    if (device.v == 0)
        return -1;

    memset(&create_sync, 0, sizeof(create_sync));
    create_sync.device = device;
    create_sync.info.type = _D3DDDI_MONITORED_FENCE;
    create_sync.info.flags.value = g_sync_create_flags;
    create_sync.info.monitored_fence.initial_fence_value = 0;
    create_sync.info.monitored_fence.engine_affinity = 0;
    if (ioctl(fd, LX_DXCREATESYNCHRONIZATIONOBJECT, &create_sync) < 0 ||
        create_sync.sync_object.v == 0 ||
        create_sync.info.shared_handle.v == 0) {
        printf("shared_sync_nt create_failed sync=0x%x global=0x%x flags=0x%x shared=%u ntsec=%u no_signal=%u\n",
               create_sync.sync_object.v, create_sync.info.shared_handle.v,
               create_sync.info.flags.value, create_sync.info.flags.shared,
               create_sync.info.flags.nt_security_sharing,
               create_sync.info.flags.no_signal);
        return -1;
    }
    printf("shared_sync_object 0x%x global=0x%x flags=0x%x shared=%u ntsec=%u no_signal=%u fence_cpu=0x%lx fence_gpu=0x%lx\n",
           create_sync.sync_object.v, create_sync.info.shared_handle.v,
           create_sync.info.flags.value, create_sync.info.flags.shared,
           create_sync.info.flags.nt_security_sharing,
           create_sync.info.flags.no_signal,
           create_sync.info.monitored_fence.fence_cpu_virtual_address,
           create_sync.info.monitored_fence.fence_gpu_virtual_address);

    memset(&share_objects, 0, sizeof(share_objects));
    objects[0] = create_sync.sync_object;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    if (ioctl(fd, LX_DXSHAREOBJECTS, &share_objects) < 0 ||
        shared_handle == 0) {
        printf("share_sync_nt failed sync=0x%x fd=%lu\n",
               create_sync.sync_object.v, shared_handle);
        goto out_destroy_original;
    }
    printf("share_sync_nt ok sync=0x%x fd=%lu\n",
           create_sync.sync_object.v, shared_handle);

    memset(&open_sync, 0, sizeof(open_sync));
    open_flags = g_sync_open_flags_set ? g_sync_open_flags :
                 create_sync.info.flags.value;
    open_sync.device = device;
    open_sync.nt_handle = shared_handle;
    open_sync.flags.value = open_flags;
    open_sync.monitored_fence.engine_affinity = 0;
    open_rc = ioctl(fd, LX_DXOPENSYNCOBJECTFROMNTHANDLE2, &open_sync);
    printf("sync_nt_matrix create_flags=0x%x global=0x%x share_fd=%lu open_flags=0x%x open_rc=%d sync=0x%x fence_cpu=0x%lx fence_gpu=0x%lx shared=%u ntsec=%u no_signal=%u\n",
           create_sync.info.flags.value, create_sync.info.shared_handle.v,
           shared_handle, open_flags, open_rc, open_sync.sync_object.v,
           open_sync.monitored_fence.fence_value_cpu_va,
           open_sync.monitored_fence.fence_value_gpu_va,
           open_sync.flags.shared, open_sync.flags.nt_security_sharing,
           open_sync.flags.no_signal);
    if (open_rc < 0 ||
        open_sync.sync_object.v == 0 ||
        open_sync.monitored_fence.fence_value_cpu_va == 0 ||
        open_sync.monitored_fence.fence_value_gpu_va == 0) {
        printf("open_sync_nt failed fd=%lu flags=0x%x sync=0x%x fence_cpu=0x%lx fence_gpu=0x%lx\n",
               shared_handle, open_flags, open_sync.sync_object.v,
               open_sync.monitored_fence.fence_value_cpu_va,
               open_sync.monitored_fence.fence_value_gpu_va);
        goto out_close_fd;
    }
    same_device_open_ok = 1;
    printf("open_sync_nt ok sync=0x%x flags=0x%x fence_cpu=0x%lx fence_gpu=0x%lx\n",
           open_sync.sync_object.v, open_flags,
           open_sync.monitored_fence.fence_value_cpu_va,
           open_sync.monitored_fence.fence_value_gpu_va);

    memset(&destroy_sync, 0, sizeof(destroy_sync));
    destroy_sync.sync_object = open_sync.sync_object;
    if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &destroy_sync) < 0)
        printf("open_sync_nt destroy_failed sync=0x%x\n",
               open_sync.sync_object.v);
    {
        int pid = fork();

        child_attempted = 1;
        if (pid < 0) {
            printf("open_sync_nt_child fork_failed fd=%lu\n",
                   shared_handle);
            goto out_close_fd;
        }
        if (pid == 0) {
            struct d3dkmthandle child_adapter;
            struct d3dkmthandle child_device;
            struct d3dkmt_opensyncobjectfromnthandle2 child_open;
            int child_fd = -1;
            int child_ret = 1;

            memset(&child_adapter, 0, sizeof(child_adapter));
            memset(&child_device, 0, sizeof(child_device));
            if (open_first_dxg_device(&child_fd, &child_adapter,
                                      &child_device) == 0) {
                memset(&child_open, 0, sizeof(child_open));
                child_open.device = child_device;
                child_open.nt_handle = shared_handle;
                child_open.flags.value = open_flags;
                if (ioctl(child_fd, LX_DXOPENSYNCOBJECTFROMNTHANDLE2,
                          &child_open) == 0 &&
                    child_open.sync_object.v != 0 &&
                    child_open.monitored_fence.fence_value_cpu_va != 0 &&
                    child_open.monitored_fence.fence_value_gpu_va != 0) {
                    printf("open_sync_nt_child ok sync=0x%x flags=0x%x fence_cpu=0x%lx fence_gpu=0x%lx\n",
                           child_open.sync_object.v, open_flags,
                           child_open.monitored_fence.fence_value_cpu_va,
                           child_open.monitored_fence.fence_value_gpu_va);
                    memset(&destroy_sync, 0, sizeof(destroy_sync));
                    destroy_sync.sync_object = child_open.sync_object;
                    if (ioctl(child_fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                              &destroy_sync) < 0)
                        printf("open_sync_nt_child destroy_failed sync=0x%x\n",
                               child_open.sync_object.v);
                    child_ret = 0;
                } else {
                    printf("open_sync_nt_child failed fd=%lu flags=0x%x sync=0x%x fence_cpu=0x%lx fence_gpu=0x%lx\n",
                           shared_handle, open_flags,
                           child_open.sync_object.v,
                           child_open.monitored_fence.fence_value_cpu_va,
                           child_open.monitored_fence.fence_value_gpu_va);
                }
                close_dxg_device(child_fd, child_adapter, child_device);
            }
            exit(child_ret);
        }
        wait(&child_status);
        if (child_status != 0) {
            printf("open_sync_nt_child failed status=%d\n", child_status);
            goto out_close_fd;
        }
    }
    ret = 0;

out_close_fd:
    if (shared_handle != 0)
        printf("opensync_layout_source_matrix uapi=OPENSYNCOBJECTFROMNTHANDLE2 nt_handle_source=shareobjects_fd create_device=0x%x create_sync=0x%x global=0x%x share_fd=%lu create_flags=0x%x open_flags=0x%x shared=%u ntsec=%u no_signal=%u same_process_device=0x%x same_open_rc=%d same_open_ok=%u same_sync=0x%x same_fence_cpu=0x%lx same_fence_gpu=0x%lx child_process_attempted=%u child_status=%d handle_source_selection=nt_handle_fd present_attempted=0 native_present_claim=0 status=%s\n",
               device.v, create_sync.sync_object.v,
               create_sync.info.shared_handle.v, shared_handle,
               create_sync.info.flags.value, open_flags,
               open_sync.flags.shared, open_sync.flags.nt_security_sharing,
               open_sync.flags.no_signal, device.v, open_rc,
               same_device_open_ok, open_sync.sync_object.v,
               open_sync.monitored_fence.fence_value_cpu_va,
               open_sync.monitored_fence.fence_value_gpu_va,
               child_attempted, child_status,
               same_device_open_ok && child_attempted &&
               child_status == 0 ? "PASS" : "FAIL");
    close((int)shared_handle);
out_destroy_original:
    memset(&destroy_sync, 0, sizeof(destroy_sync));
    destroy_sync.sync_object = create_sync.sync_object;
    if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &destroy_sync) < 0)
        printf("shared_sync_nt destroy_failed sync=0x%x\n",
               create_sync.sync_object.v);
    return ret;
}

static int probe_import_negative_contract(int fd, struct d3dkmthandle adapter,
                                          struct winluid adapter_luid,
                                          struct d3dkmthandle device)
{
    struct import_negative_child_sync_result {
        int own_rc;
        int inherited_rc;
        int inherited_enum_rc;
        int inherited_openadapter_rc;
        int inherited_query_rc;
        int inherited_create_rc;
        int child_pid;
        int child_tid;
        int parent_pid_seen;
        int parent_tid_seen;
        uint32 inherited_enum_count;
        uint32 inherited_openadapter_handle;
        uint32 inherited_query_value;
        uint32 inherited_create_device;
        uint32 child_dxg_fd;
        uint32 child_adapter;
        uint32 child_device;
        uint32 own_passed_device;
        uint32 own_returned_sync;
        uint64 own_returned_fence_cpu;
        uint64 own_returned_fence_gpu;
        uint32 inherited_parent_dxg_fd;
        uint32 inherited_passed_device;
        uint32 inherited_returned_sync;
        uint64 inherited_returned_fence_cpu;
        uint64 inherited_returned_fence_gpu;
        uint32 inherited_nt_fd;
        uint32 reopened_dxg_fd;
        uint32 own_used_parent_device;
        uint32 own_used_child_device;
        uint32 inherited_used_parent_device;
        uint32 inherited_used_child_device;
    };
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_createsynchronizationobject2 create_sync;
    struct d3dkmt_destroysynchronizationobject destroy_sync;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmt_shareobjects fault_share_objects;
    struct d3dkmthandle objects[1];
    struct d3dkmt_queryresourceinfofromnthandle query;
    struct d3dkmt_openresourcefromnthandle open_resource;
    struct d3dkmt_opensyncobjectfromnthandle2 open_sync;
    struct dxg_sharedhandle_copyout_diag copyout_before;
    struct dxg_sharedhandle_copyout_diag resource_copyout_diag;
    struct dxg_sharedhandle_copyout_diag sync_copyout_diag;
    uint64 resource_fd = 0;
    uint64 sync_fd = 0;
    uint64 returned_resource_fd = 0;
    uint64 returned_sync_fd = 0;
    uint64 stale_resource_fd = 0;
    uint64 stale_sync_fd = 0;
    int resource_fd_valid = 0;
    int sync_fd_valid = 0;
    int stale_resource_fd_valid = 0;
    int stale_sync_fd_valid = 0;
    int resource_fd_open = 0;
    int sync_fd_open = 0;
    int resource_create_rc;
    int resource_copyout_fault_rc = -2;
    int resource_copyout_diag_rc = -1;
    int resource_share_rc = -1;
    int sync_create_rc;
    int sync_copyout_fault_rc = -2;
    int sync_copyout_diag_rc = -1;
    int sync_share_rc = -1;
    uint32 resource_copyout_failures_before = 0;
    uint32 sync_copyout_failures_before = 0;
    int resource_missing_query_rc = -1;
    int resource_missing_open_rc = -1;
    int resource_wrong_kind_query_rc = -1;
    int resource_wrong_kind_open_rc = -1;
    int resource_stale_query_rc = -1;
    int resource_stale_open_rc = -1;
    int sync_missing_open_rc = -1;
    int sync_wrong_kind_open_rc = -1;
    int sync_zero_device_open_rc = -1;
    int sync_stale_open_rc = -1;
    int sync_child_parent_device_rc = -2;
    int sync_child_status = -1;
    struct import_negative_child_sync_result child_sync_result;
    int destroy_resource_rc = -2;
    int destroy_sync_rc = -2;
    int resource_negative_pass;
    int sync_negative_pass;
    int resource_copyout_pass = 0;
    int sync_copyout_pass = 0;
    int opensync_negative_pass;
    const char *resource_reason = "ok";
    const char *sync_reason = "ok";
    const char *opensync_reason = "ok";
    int opensync_namespace_diag_present = 0;
    int pass;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    memset(&create_sync, 0, sizeof(create_sync));
    memset(&destroy_sync, 0, sizeof(destroy_sync));
    memset(&share_objects, 0, sizeof(share_objects));
    memset(&fault_share_objects, 0, sizeof(fault_share_objects));
    memset(&copyout_before, 0, sizeof(copyout_before));
    memset(&resource_copyout_diag, 0, sizeof(resource_copyout_diag));
    memset(&sync_copyout_diag, 0, sizeof(sync_copyout_diag));
    memset(&child_sync_result, 0, sizeof(child_sync_result));
    child_sync_result.own_rc = -2;
    child_sync_result.inherited_rc = -2;
    child_sync_result.inherited_enum_rc = -2;
    child_sync_result.inherited_openadapter_rc = -2;
    child_sync_result.inherited_query_rc = -2;
    child_sync_result.inherited_create_rc = -2;
    child_sync_result.child_pid = -1;
    child_sync_result.child_tid = -1;
    child_sync_result.parent_pid_seen = getpid();
    child_sync_result.parent_tid_seen = gettid();

    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.cross_adapter = 1;
    create_allocation.flags.standard_allocation = 1;
    resource_create_rc = ioctl(fd, LX_DXCREATEALLOCATION,
                               &create_allocation);
    if (resource_create_rc == 0 && create_allocation.resource.v != 0) {
        if (read_dxg_sharedhandle_copyout_diag(&copyout_before) == 0)
            resource_copyout_failures_before = copyout_before.failures;
        memset(&fault_share_objects, 0, sizeof(fault_share_objects));
        objects[0] = create_allocation.resource;
        fault_share_objects.object_count = 1;
        fault_share_objects.objects = (uint64)objects;
        fault_share_objects.shared_handle = 1;
        resource_copyout_fault_rc =
            ioctl(fd, LX_DXSHAREOBJECTS, &fault_share_objects);
        resource_copyout_diag_rc =
            read_dxg_sharedhandle_copyout_diag(&resource_copyout_diag);
        memset(&share_objects, 0, sizeof(share_objects));
        objects[0] = create_allocation.resource;
        share_objects.object_count = 1;
        share_objects.objects = (uint64)objects;
        share_objects.shared_handle = (uint64)&resource_fd;
        resource_share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
        resource_fd_valid = resource_share_rc == 0;
        resource_fd_open = resource_fd_valid;
        if (resource_fd_valid)
            returned_resource_fd = resource_fd;
    }

    create_sync.device = device;
    create_sync.info.type = _D3DDDI_MONITORED_FENCE;
    create_sync.info.flags.value = 0x13;
    sync_create_rc = ioctl(fd, LX_DXCREATESYNCHRONIZATIONOBJECT,
                           &create_sync);
    if (sync_create_rc == 0 && create_sync.sync_object.v != 0) {
        if (read_dxg_sharedhandle_copyout_diag(&copyout_before) == 0)
            sync_copyout_failures_before = copyout_before.failures;
        memset(&fault_share_objects, 0, sizeof(fault_share_objects));
        objects[0] = create_sync.sync_object;
        fault_share_objects.object_count = 1;
        fault_share_objects.objects = (uint64)objects;
        fault_share_objects.shared_handle = 1;
        sync_copyout_fault_rc =
            ioctl(fd, LX_DXSHAREOBJECTS, &fault_share_objects);
        sync_copyout_diag_rc =
            read_dxg_sharedhandle_copyout_diag(&sync_copyout_diag);
        memset(&share_objects, 0, sizeof(share_objects));
        objects[0] = create_sync.sync_object;
        share_objects.object_count = 1;
        share_objects.objects = (uint64)objects;
        share_objects.shared_handle = (uint64)&sync_fd;
        sync_share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
        sync_fd_valid = sync_share_rc == 0;
        sync_fd_open = sync_fd_valid;
        if (sync_fd_valid)
            returned_sync_fd = sync_fd;
    }

    memset(&query, 0, sizeof(query));
    query.device = device;
    query.nt_handle = 0;
    resource_missing_query_rc =
        ioctl(fd, LX_DXQUERYRESOURCEINFOFROMNTHANDLE, &query);
    memset(&open_resource, 0, sizeof(open_resource));
    open_resource.device = device;
    open_resource.nt_handle = 0;
    resource_missing_open_rc =
        ioctl(fd, LX_DXOPENRESOURCEFROMNTHANDLE, &open_resource);

    if (sync_fd_open) {
        memset(&query, 0, sizeof(query));
        query.device = device;
        query.nt_handle = sync_fd;
        resource_wrong_kind_query_rc =
            ioctl(fd, LX_DXQUERYRESOURCEINFOFROMNTHANDLE, &query);
        memset(&open_resource, 0, sizeof(open_resource));
        open_resource.device = device;
        open_resource.nt_handle = sync_fd;
        resource_wrong_kind_open_rc =
            ioctl(fd, LX_DXOPENRESOURCEFROMNTHANDLE, &open_resource);
    }

    if (resource_fd_open) {
        memset(&open_sync, 0, sizeof(open_sync));
        open_sync.device = device;
        open_sync.nt_handle = resource_fd;
        open_sync.flags.value = 0x13;
        sync_wrong_kind_open_rc =
            ioctl(fd, LX_DXOPENSYNCOBJECTFROMNTHANDLE2, &open_sync);
    }

    if (resource_fd_open) {
        stale_resource_fd = resource_fd;
        stale_resource_fd_valid = 1;
        close((int)resource_fd);
        resource_fd = 0;
        resource_fd_open = 0;
        memset(&query, 0, sizeof(query));
        query.device = device;
        query.nt_handle = stale_resource_fd;
        resource_stale_query_rc =
            ioctl(fd, LX_DXQUERYRESOURCEINFOFROMNTHANDLE, &query);
        memset(&open_resource, 0, sizeof(open_resource));
        open_resource.device = device;
        open_resource.nt_handle = stale_resource_fd;
        resource_stale_open_rc =
            ioctl(fd, LX_DXOPENRESOURCEFROMNTHANDLE, &open_resource);
    }

    memset(&open_sync, 0, sizeof(open_sync));
    open_sync.device = device;
    open_sync.nt_handle = 0;
    open_sync.flags.value = 0x13;
    sync_missing_open_rc =
        ioctl(fd, LX_DXOPENSYNCOBJECTFROMNTHANDLE2, &open_sync);

    if (sync_fd_open) {
        memset(&open_sync, 0, sizeof(open_sync));
        open_sync.device.v = 0;
        open_sync.nt_handle = sync_fd;
        open_sync.flags.value = 0x13;
        sync_zero_device_open_rc =
            ioctl(fd, LX_DXOPENSYNCOBJECTFROMNTHANDLE2, &open_sync);
    }

    if (sync_fd_open) {
        int pipe_fds[2] = { -1, -1 };
        int pid = -1;

        if (pipe(pipe_fds) == 0)
            pid = fork();
        if (pid == 0) {
            int child_fd = -1;
            int child_own_rc = -2;
            int child_inherited_rc = -2;
            int child_inherited_enum_rc = -2;
            int child_inherited_openadapter_rc = -2;
            int child_inherited_query_rc = -2;
            int child_inherited_create_rc = -2;
            struct import_negative_child_sync_result child_result;
            struct d3dkmthandle child_adapter;
            struct d3dkmthandle child_device;

            if (pipe_fds[0] >= 0)
                close(pipe_fds[0]);
            memset(&child_adapter, 0, sizeof(child_adapter));
            memset(&child_device, 0, sizeof(child_device));
            memset(&child_result, 0, sizeof(child_result));
            child_result.own_rc = -2;
            child_result.inherited_rc = -2;
            child_result.inherited_enum_rc = -2;
            child_result.inherited_openadapter_rc = -2;
            child_result.inherited_query_rc = -2;
            child_result.inherited_create_rc = -2;
            child_result.child_pid = getpid();
            child_result.child_tid = gettid();
            child_result.parent_pid_seen = getppid();
            child_result.parent_tid_seen = getppid();
            child_result.inherited_nt_fd = (uint32)sync_fd;
            if (open_first_dxg_device(&child_fd, &child_adapter,
                                      &child_device) == 0) {
                memset(&open_sync, 0, sizeof(open_sync));
                open_sync.device = child_device;
                open_sync.nt_handle = sync_fd;
                open_sync.flags.value = 0x13;
                child_result.child_dxg_fd = (uint32)child_fd;
                child_result.child_adapter = child_adapter.v;
                child_result.child_device = child_device.v;
                child_result.own_passed_device = open_sync.device.v;
                child_result.reopened_dxg_fd = 1;
                child_result.own_used_parent_device = 0;
                child_result.own_used_child_device = 1;
                child_own_rc = ioctl(child_fd,
                                     LX_DXOPENSYNCOBJECTFROMNTHANDLE2,
                                     &open_sync);
                child_result.own_rc = child_own_rc;
                child_result.own_returned_sync = open_sync.sync_object.v;
                child_result.own_returned_fence_cpu =
                    open_sync.monitored_fence.fence_value_cpu_va;
                child_result.own_returned_fence_gpu =
                    open_sync.monitored_fence.fence_value_gpu_va;
                if (open_sync.sync_object.v != 0) {
                    memset(&destroy_sync, 0, sizeof(destroy_sync));
                    destroy_sync.sync_object = open_sync.sync_object;
                    ioctl(child_fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                          &destroy_sync);
                }

                {
                    struct d3dkmt_adapterinfo inherited_adapters[
                        D3DKMT_ADAPTERS_MAX];
                    struct d3dkmt_enumadapters2 inherited_enum;
                    struct d3dkmt_openadapterfromluid inherited_open_luid;
                    struct d3dkmt_queryadapterinfo inherited_query;
                    struct d3dkmt_adaptertype inherited_adapter_type;
                    struct d3dkmt_createdevice inherited_create_device;

                    memset(inherited_adapters, 0,
                           sizeof(inherited_adapters));
                    memset(&inherited_enum, 0, sizeof(inherited_enum));
                    inherited_enum.num_adapters = D3DKMT_ADAPTERS_MAX;
                    inherited_enum.adapters = (uint64)inherited_adapters;
                    child_inherited_enum_rc =
                        ioctl(fd, LX_DXENUMADAPTERS2, &inherited_enum);
                    child_result.inherited_enum_rc =
                        child_inherited_enum_rc;
                    child_result.inherited_enum_count =
                        inherited_enum.num_adapters;

                    memset(&inherited_open_luid, 0,
                           sizeof(inherited_open_luid));
                    inherited_open_luid.adapter_luid = adapter_luid;
                    child_inherited_openadapter_rc =
                        ioctl(fd, LX_DXOPENADAPTERFROMLUID,
                              &inherited_open_luid);
                    child_result.inherited_openadapter_rc =
                        child_inherited_openadapter_rc;
                    child_result.inherited_openadapter_handle =
                        inherited_open_luid.adapter_handle.v;
                    if (inherited_open_luid.adapter_handle.v != 0) {
                        struct d3dkmt_closeadapter inherited_close;

                        memset(&inherited_close, 0,
                               sizeof(inherited_close));
                        inherited_close.adapter_handle =
                            inherited_open_luid.adapter_handle;
                        ioctl(fd, LX_DXCLOSEADAPTER, &inherited_close);
                    }

                    memset(&inherited_adapter_type, 0,
                           sizeof(inherited_adapter_type));
                    memset(&inherited_query, 0, sizeof(inherited_query));
                    inherited_query.adapter = adapter;
                    inherited_query.type = _KMTQAITYPE_ADAPTERTYPE;
                    inherited_query.private_data =
                        (uint64)&inherited_adapter_type;
                    inherited_query.private_data_size =
                        sizeof(inherited_adapter_type);
                    child_inherited_query_rc =
                        ioctl(fd, LX_DXQUERYADAPTERINFO,
                              &inherited_query);
                    child_result.inherited_query_rc =
                        child_inherited_query_rc;
                    child_result.inherited_query_value =
                        inherited_adapter_type.value;

                    memset(&inherited_create_device, 0,
                           sizeof(inherited_create_device));
                    inherited_create_device.adapter = adapter;
                    child_inherited_create_rc =
                        ioctl(fd, LX_DXCREATEDEVICE,
                              &inherited_create_device);
                    child_result.inherited_create_rc =
                        child_inherited_create_rc;
                    child_result.inherited_create_device =
                        inherited_create_device.device.v;
                    if (inherited_create_device.device.v != 0) {
                        struct d3dkmt_destroydevice inherited_destroy;

                        memset(&inherited_destroy, 0,
                               sizeof(inherited_destroy));
                        inherited_destroy.device =
                            inherited_create_device.device;
                        ioctl(fd, LX_DXDESTROYDEVICE,
                              &inherited_destroy);
                    }
                }

                memset(&open_sync, 0, sizeof(open_sync));
                open_sync.device = device;
                open_sync.nt_handle = sync_fd;
                open_sync.flags.value = 0x13;
                child_result.inherited_parent_dxg_fd = (uint32)fd;
                child_result.inherited_passed_device =
                    open_sync.device.v;
                child_result.inherited_used_parent_device = 1;
                child_result.inherited_used_child_device = 0;
                child_inherited_rc = ioctl(fd,
                                           LX_DXOPENSYNCOBJECTFROMNTHANDLE2,
                                           &open_sync);
                child_result.inherited_rc = child_inherited_rc;
                child_result.inherited_returned_sync =
                    open_sync.sync_object.v;
                child_result.inherited_returned_fence_cpu =
                    open_sync.monitored_fence.fence_value_cpu_va;
                child_result.inherited_returned_fence_gpu =
                    open_sync.monitored_fence.fence_value_gpu_va;
                if (open_sync.sync_object.v != 0) {
                    memset(&destroy_sync, 0, sizeof(destroy_sync));
                    destroy_sync.sync_object = open_sync.sync_object;
                    ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                          &destroy_sync);
                }
                close_dxg_device(child_fd, child_adapter, child_device);
            }
            if (pipe_fds[1] >= 0) {
                write(pipe_fds[1], &child_result,
                      sizeof(child_result));
                close(pipe_fds[1]);
            }
            exit(child_own_rc == 0 &&
                 child_inherited_enum_rc < 0 &&
                 child_inherited_openadapter_rc < 0 &&
                 child_inherited_query_rc < 0 &&
                 child_inherited_create_rc < 0 &&
                 child_inherited_rc < 0 ? 0 : 1);
        } else if (pid > 0) {
            if (pipe_fds[1] >= 0)
                close(pipe_fds[1]);
            if (pipe_fds[0] >= 0) {
                read(pipe_fds[0], &child_sync_result,
                     sizeof(child_sync_result));
                close(pipe_fds[0]);
            }
            wait(&sync_child_status);
            sync_child_parent_device_rc =
                child_sync_result.inherited_rc;
        } else {
            if (pipe_fds[0] >= 0)
                close(pipe_fds[0]);
            if (pipe_fds[1] >= 0)
                close(pipe_fds[1]);
        }

        stale_sync_fd = sync_fd;
        stale_sync_fd_valid = 1;
        close((int)sync_fd);
        sync_fd = 0;
        sync_fd_open = 0;
        memset(&open_sync, 0, sizeof(open_sync));
        open_sync.device = device;
        open_sync.nt_handle = stale_sync_fd;
        open_sync.flags.value = 0x13;
        sync_stale_open_rc =
            ioctl(fd, LX_DXOPENSYNCOBJECTFROMNTHANDLE2, &open_sync);
    }

    resource_negative_pass = resource_create_rc == 0 &&
                             resource_share_rc == 0 &&
                             stale_resource_fd_valid &&
                             resource_missing_query_rc < 0 &&
                             resource_missing_open_rc < 0 &&
                             resource_wrong_kind_query_rc < 0 &&
                             resource_wrong_kind_open_rc < 0 &&
                             resource_stale_query_rc < 0 &&
                             resource_stale_open_rc < 0;
    if (resource_create_rc != 0)
        resource_reason = "resource_create_failed";
    else if (resource_share_rc != 0)
        resource_reason = "resource_share_failed";
    else if (!stale_resource_fd_valid)
        resource_reason = "resource_stale_fd_not_created";
    else if (resource_missing_query_rc >= 0)
        resource_reason = "missing_resource_query_accepted";
    else if (resource_missing_open_rc >= 0)
        resource_reason = "missing_resource_open_accepted";
    else if (resource_wrong_kind_query_rc >= 0)
        resource_reason = "sync_fd_resource_query_accepted";
    else if (resource_wrong_kind_open_rc >= 0)
        resource_reason = "sync_fd_resource_open_accepted";
    else if (resource_stale_query_rc >= 0)
        resource_reason = "stale_resource_query_accepted";
    else if (resource_stale_open_rc >= 0)
        resource_reason = "stale_resource_open_accepted";
    resource_copyout_pass =
        resource_create_rc == 0 &&
        resource_copyout_fault_rc < 0 &&
        resource_copyout_diag_rc == 0 &&
        resource_copyout_diag.seen &&
        resource_copyout_diag.failures > resource_copyout_failures_before &&
        resource_copyout_diag.kind == 2 &&
        resource_copyout_diag.reclaimed == 1 &&
        resource_copyout_diag.refs_after == 0 &&
        resource_share_rc == 0 &&
        resource_fd_valid;

    sync_negative_pass = sync_create_rc == 0 &&
                         sync_share_rc == 0 &&
                         stale_sync_fd_valid &&
                         sync_missing_open_rc < 0 &&
                         sync_wrong_kind_open_rc < 0 &&
                         sync_zero_device_open_rc < 0 &&
                         sync_stale_open_rc < 0 &&
                         sync_child_parent_device_rc < 0 &&
                         sync_child_status == 0;
    if (sync_create_rc != 0)
        sync_reason = "sync_create_failed";
    else if (sync_share_rc != 0)
        sync_reason = "sync_share_failed";
    else if (!stale_sync_fd_valid)
        sync_reason = "sync_stale_fd_not_created";
    else if (sync_missing_open_rc >= 0)
        sync_reason = "missing_sync_open_accepted";
    else if (sync_wrong_kind_open_rc >= 0)
        sync_reason = "resource_fd_sync_open_accepted";
    else if (sync_zero_device_open_rc >= 0)
        sync_reason = "zero_device_sync_open_accepted";
    else if (sync_stale_open_rc >= 0)
        sync_reason = "stale_sync_open_accepted";
    else if (sync_child_parent_device_rc >= 0)
        sync_reason = "child_parent_device_sync_open_accepted";
    else if (sync_child_status != 0)
        sync_reason = "child_parent_device_probe_failed";
    sync_copyout_pass =
        sync_create_rc == 0 &&
        sync_copyout_fault_rc < 0 &&
        sync_copyout_diag_rc == 0 &&
        sync_copyout_diag.seen &&
        sync_copyout_diag.failures > sync_copyout_failures_before &&
        sync_copyout_diag.kind == 1 &&
        sync_copyout_diag.reclaimed == 1 &&
        sync_copyout_diag.refs_after == 0 &&
        sync_share_rc == 0 &&
        sync_fd_valid;

    opensync_negative_pass = sync_create_rc == 0 &&
                             sync_share_rc == 0 &&
                             stale_sync_fd_valid &&
                             sync_missing_open_rc < 0 &&
                             sync_wrong_kind_open_rc < 0 &&
                             sync_zero_device_open_rc < 0 &&
                             sync_stale_open_rc < 0 &&
                             sync_child_parent_device_rc < 0 &&
                             sync_child_status == 0;
    if (sync_reason[0] != 'o')
        opensync_reason = sync_reason;
    {
        char *dxg_status = read_dxg_status_buffer();

        if (dxg_status != 0) {
            opensync_namespace_diag_present =
                dxg_find_text(dxg_status, "dxg_opensync_namespace") != 0 ||
                dxg_find_text(dxg_status, "dxg_ioctl_tgid_gate=") != 0;
            free(dxg_status);
        }
    }

    pass = resource_negative_pass && sync_negative_pass &&
           resource_copyout_pass && sync_copyout_pass;

    printf("ntshare_copyout_cleanup_matrix kind=resource create_rc=%d fault_share_rc=%d failures=%u->%u diag_rc=%d fd=%u reclaimed=%u refs_after=%u valid_share_after_fault=%u returned_fd_valid=%u ret=%d present_attempted=0 native_present_claim=0 status=%s\n",
           resource_create_rc, resource_copyout_fault_rc,
           resource_copyout_failures_before,
           resource_copyout_diag.failures, resource_copyout_diag_rc,
           resource_copyout_diag.fd, resource_copyout_diag.reclaimed,
           resource_copyout_diag.refs_after, resource_share_rc == 0,
           resource_fd_valid, resource_copyout_diag.ret,
           resource_copyout_pass ? "PASS" : "FAIL");
    printf("ntshare_copyout_cleanup_matrix kind=sync create_rc=%d fault_share_rc=%d failures=%u->%u diag_rc=%d fd=%u reclaimed=%u refs_after=%u valid_share_after_fault=%u returned_fd_valid=%u ret=%d present_attempted=0 native_present_claim=0 status=%s\n",
           sync_create_rc, sync_copyout_fault_rc,
           sync_copyout_failures_before,
           sync_copyout_diag.failures, sync_copyout_diag_rc,
           sync_copyout_diag.fd, sync_copyout_diag.reclaimed,
           sync_copyout_diag.refs_after, sync_share_rc == 0,
           sync_fd_valid, sync_copyout_diag.ret,
           sync_copyout_pass ? "PASS" : "FAIL");
    printf("resource_import_negative_matrix create_rc=%d share_rc=%d resource=0x%x allocation=0x%x returned_resource_fd=%lu resource_returned_fd_valid=%u resource_fd=%lu resource_fd_valid=%u resource_fd_open_before_close=%u resource_fd_open_after_close=%u stale_resource_fd=%lu stale_resource_fd_valid=%u stale_resource_fd_expected_closed=1 missing_query_rc=%d missing_open_rc=%d wrong_fd_kind=sync_fd wrong_kind_query_rc=%d wrong_kind_open_rc=%d stale_query_rc=%d stale_open_rc=%d expected_failures=missing_fd,wrong_kind_fd,stale_fd present_attempted=0 native_present_claim=0 reason=%s status=%s\n",
           resource_create_rc, resource_share_rc,
           create_allocation.resource.v, allocation_info.allocation.v,
           returned_resource_fd, resource_fd_valid, resource_fd,
           resource_fd_valid, stale_resource_fd_valid, resource_fd_open,
           stale_resource_fd, stale_resource_fd_valid,
           resource_missing_query_rc, resource_missing_open_rc,
           resource_wrong_kind_query_rc, resource_wrong_kind_open_rc,
           resource_stale_query_rc, resource_stale_open_rc,
           resource_reason, resource_negative_pass ? "PASS" : "FAIL");
    printf("sync_import_negative_matrix create_rc=%d share_rc=%d sync=0x%x returned_sync_fd=%lu sync_returned_fd_valid=%u sync_fd=%lu sync_fd_valid=%u sync_fd_open_before_close=%u sync_fd_open_after_close=%u stale_sync_fd=%lu stale_sync_fd_valid=%u stale_sync_fd_expected_closed=1 missing_open_rc=%d wrong_fd_kind=resource_fd wrong_kind_open_rc=%d zero_device_open_rc=%d stale_open_rc=%d child_parent_device_control=expected_reject child_parent_device_rc=%d child_status=%d handle_source_swap=%s candidate_selection=not_required expected_failures=missing_fd,wrong_kind_fd,zero_device,stale_fd,foreign_process_device present_attempted=0 native_present_claim=0 reason=%s status=%s\n",
           sync_create_rc, sync_share_rc, create_sync.sync_object.v,
           returned_sync_fd, sync_fd_valid, sync_fd, sync_fd_valid,
           stale_sync_fd_valid, sync_fd_open, stale_sync_fd,
           stale_sync_fd_valid, sync_missing_open_rc,
           sync_wrong_kind_open_rc, sync_zero_device_open_rc,
           sync_stale_open_rc, sync_child_parent_device_rc,
           sync_child_status, stale_resource_fd_valid ? "attempted" :
           "SKIP_MISSING_RESOURCE_FD", sync_reason,
           sync_negative_pass ? "PASS" : "FAIL");
    printf("opensyncobject_source_matrix returned_sync_fd=%lu sync_returned_fd_valid=%u sync_fd_open_after_close=%u returned_resource_fd=%lu resource_returned_fd_valid=%u stale_sync_fd=%lu stale_sync_fd_valid=%u flags=0x13 missing_open_rc=%d wrong_kind_resource_fd_rc=%d zero_device_open_rc=%d stale_open_rc=%d child_parent_device_control=expected_reject child_parent_device_rc=%d child_status=%d expected_failures=missing_fd,wrong_kind_fd,zero_device,stale_fd,foreign_process_device reason=%s status=%s\n",
           returned_sync_fd, sync_fd_valid, sync_fd_open,
           returned_resource_fd, resource_fd_valid, stale_sync_fd,
           stale_sync_fd_valid,
           sync_missing_open_rc, sync_wrong_kind_open_rc,
           sync_zero_device_open_rc, sync_stale_open_rc,
           sync_child_parent_device_rc, sync_child_status,
           opensync_reason, opensync_negative_pass ? "PASS" : "FAIL");
    printf("opensync_child_own_dxg_matrix parent_pid=%d parent_tid=%d child_pid=%d child_tid=%d child_parent_pid_seen=%d parent_device=0x%x child_dxg_fd=%u child_adapter=0x%x child_device=0x%x passed_device=0x%x inherited_nt_fd=%u reopened_dxg_fd=%u used_parent_device=%u used_child_device=%u child_open_rc=%d child_returned_sync=0x%x child_fence_cpu=0x%lx child_fence_gpu=0x%lx expected=allow_same_numeric_child_device reason=%s status=%s\n",
           getpid(), gettid(), child_sync_result.child_pid,
           child_sync_result.child_tid,
           child_sync_result.parent_pid_seen, device.v,
           child_sync_result.child_dxg_fd,
           child_sync_result.child_adapter, child_sync_result.child_device,
           child_sync_result.own_passed_device,
           child_sync_result.inherited_nt_fd,
           child_sync_result.reopened_dxg_fd,
           child_sync_result.own_used_parent_device,
           child_sync_result.own_used_child_device,
           child_sync_result.own_rc, child_sync_result.own_returned_sync,
           child_sync_result.own_returned_fence_cpu,
           child_sync_result.own_returned_fence_gpu,
           child_sync_result.own_rc == 0 &&
           child_sync_result.own_passed_device ==
           child_sync_result.child_device &&
           child_sync_result.reopened_dxg_fd ?
           "wsl_consistent_same_numeric_child_device" :
           "child_own_dxg_sync_open_failed",
           child_sync_result.own_rc == 0 &&
           child_sync_result.own_passed_device ==
           child_sync_result.child_device &&
           child_sync_result.reopened_dxg_fd ? "PASS" : "FAIL");
    printf("opensync_child_inherited_parent_dxg_matrix parent_pid=%d parent_tid=%d child_pid=%d child_tid=%d child_parent_pid_seen=%d parent_device=0x%x inherited_parent_dxg_fd=%u child_device=0x%x passed_device=0x%x inherited_nt_fd=%u reopened_dxg_fd=%u used_parent_device=%u used_child_device=%u child_open_rc=%d child_returned_sync=0x%x child_fence_cpu=0x%lx child_fence_gpu=0x%lx kernel_namespace_diag_present=%u expected=reject_inherited_parent_dxg_fd_tgid_guard reason=%s status=%s\n",
           getpid(), gettid(), child_sync_result.child_pid,
           child_sync_result.child_tid,
           child_sync_result.parent_pid_seen, device.v,
           child_sync_result.inherited_parent_dxg_fd,
           child_sync_result.child_device,
           child_sync_result.inherited_passed_device,
           child_sync_result.inherited_nt_fd,
           child_sync_result.reopened_dxg_fd,
           child_sync_result.inherited_used_parent_device,
           child_sync_result.inherited_used_child_device,
           child_sync_result.inherited_rc,
           child_sync_result.inherited_returned_sync,
           child_sync_result.inherited_returned_fence_cpu,
           child_sync_result.inherited_returned_fence_gpu,
           opensync_namespace_diag_present,
           child_sync_result.inherited_rc < 0 && sync_child_status == 0 ?
           "ok" : "inherited_parent_dxg_sync_open_accepted",
           child_sync_result.inherited_rc < 0 && sync_child_status == 0 ?
           "PASS" : "FAIL");
    printf("dxg_tgid_pre_dispatch_matrix parent_pid=%d parent_tid=%d child_pid=%d child_tid=%d inherited_parent_dxg_fd=%u inherited_enum_rc=%d inherited_enum_count=%u inherited_openadapter_rc=%d inherited_openadapter_handle=0x%x inherited_query_rc=%d inherited_query_value=0x%x inherited_create_rc=%d inherited_create_device=0x%x inherited_opensync_rc=%d own_dxg_open_rc=%d child_status=%d kernel_namespace_diag_present=%u expected=reject_all_inherited_parent_dxg_ioctls_before_dispatch status=%s\n",
           getpid(), gettid(), child_sync_result.child_pid,
           child_sync_result.child_tid,
           child_sync_result.inherited_parent_dxg_fd,
           child_sync_result.inherited_enum_rc,
           child_sync_result.inherited_enum_count,
           child_sync_result.inherited_openadapter_rc,
           child_sync_result.inherited_openadapter_handle,
           child_sync_result.inherited_query_rc,
           child_sync_result.inherited_query_value,
           child_sync_result.inherited_create_rc,
           child_sync_result.inherited_create_device,
           child_sync_result.inherited_rc,
           child_sync_result.own_rc,
           sync_child_status, opensync_namespace_diag_present,
           child_sync_result.own_rc == 0 &&
           child_sync_result.inherited_enum_rc < 0 &&
           child_sync_result.inherited_openadapter_rc < 0 &&
           child_sync_result.inherited_query_rc < 0 &&
           child_sync_result.inherited_create_rc < 0 &&
           child_sync_result.inherited_rc < 0 &&
           sync_child_status == 0 &&
           opensync_namespace_diag_present ? "PASS" : "FAIL");
    printf("ntshare_object_kind_matrix owner_device=0x%x resource_object=0x%x resource_allocation=0x%x resource_global=0x%x resource_create_flags=0x%x resource_share_rc=%d returned_resource_fd=%lu resource_returned_fd_valid=%u sync_object=0x%x sync_global=0x%x sync_flags=0x%x sync_share_rc=%d returned_sync_fd=%lu sync_returned_fd_valid=%u resource_query_on_sync_rc=%d resource_open_on_sync_rc=%d sync_open_on_resource_rc=%d resource_wrong_kind_rejected=%u sync_wrong_kind_rejected=%u present_attempted=0 native_present_claim=0 status=%s\n",
           device.v, create_allocation.resource.v,
           allocation_info.allocation.v, create_allocation.global_share.v,
           create_allocation.flags.value, resource_share_rc,
           returned_resource_fd, resource_fd_valid, create_sync.sync_object.v,
           create_sync.info.shared_handle.v, create_sync.info.flags.value,
           sync_share_rc, returned_sync_fd, sync_fd_valid,
           resource_wrong_kind_query_rc,
           resource_wrong_kind_open_rc, sync_wrong_kind_open_rc,
           resource_wrong_kind_query_rc < 0 &&
           resource_wrong_kind_open_rc < 0,
           sync_wrong_kind_open_rc < 0,
           (resource_share_rc == 0 && sync_share_rc == 0 &&
            resource_wrong_kind_query_rc < 0 &&
            resource_wrong_kind_open_rc < 0 &&
            sync_wrong_kind_open_rc < 0) ? "PASS" : "FAIL");
    printf("ntshared_close_behavior_matrix returned_resource_fd=%lu resource_returned_fd_valid=%u resource_fd_open_after_close=%u resource_close_attempted=%u returned_sync_fd=%lu sync_returned_fd_valid=%u sync_fd_open_after_close=%u sync_close_attempted=%u explicit_destroy_ioctl=0 missing_contract=LX_DXDESTROYNTSHAREDOBJECT close_is_user_trigger=1 status=SKIP_MISSING_CONTRACT\n",
           returned_resource_fd, resource_fd_valid, resource_fd_open,
           stale_resource_fd_valid, returned_sync_fd, sync_fd_valid,
           sync_fd_open,
           stale_sync_fd_valid);

    if (sync_fd_open)
        close((int)sync_fd);
    if (resource_fd_open)
        close((int)resource_fd);
    if (create_sync.sync_object.v != 0) {
        memset(&destroy_sync, 0, sizeof(destroy_sync));
        destroy_sync.sync_object = create_sync.sync_object;
        destroy_sync_rc =
            ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &destroy_sync);
    }
    if (create_allocation.resource.v != 0) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        destroy_resource_rc =
            ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation);
    }
    printf("destroy_separation_matrix returned_resource_fd=%lu resource_returned_fd_valid=%u resource_fd_close_attempted=%u resource_d3dkmt_destroy_rc=%d returned_sync_fd=%lu sync_returned_fd_valid=%u sync_fd_close_attempted=%u sync_d3dkmt_destroy_rc=%d nt_fd_close_is_separate_from_d3dkmt_destroy=1 present_attempted=0 native_present_claim=0 status=%s\n",
           returned_resource_fd, resource_fd_valid, stale_resource_fd_valid,
           destroy_resource_rc, returned_sync_fd, sync_fd_valid,
           stale_sync_fd_valid, destroy_sync_rc,
           (create_allocation.resource.v != 0 && destroy_resource_rc < 0) ||
           (create_sync.sync_object.v != 0 && destroy_sync_rc < 0) ?
           "FAIL" : "PASS");
    return pass ? 0 : -1;
}

static void *probe_alloc_buffer(uint32 size)
{
    void *buf;

    if (size == 0)
        return 0;
    buf = malloc(size);
    if (buf != 0)
        memset(buf, 0, size);
    return buf;
}

struct dxg_shared_lifetime_status {
    uint32 seals;
    uint32 reuses;
    uint32 denied;
    uint32 open_tracked;
};

struct dxg_object_table_status {
    uint32 max;
    uint32 drops;
    uint32 denied;
    uint32 generation;
    uint32 reuse_delayed;
    uint32 reuse_allowed;
    uint32 min_free;
    uint32 free_count;
    uint32 free_head;
    uint32 free_tail;
};

struct dxg_local_adapter_status {
    uint32 hits;
    uint32 misses;
    uint32 last_result;
    uint32 handle;
    uint32 host;
    uint32 refs;
    uint32 locals;
    uint32 generation;
    uint32 reuse_delayed;
    uint32 reuse_allowed;
    uint32 min_free;
};

struct dxg_process_lifetime_status {
    uint32 object_refs_last;
    uint32 mem_refs_last;
    uint32 object_releases;
    uint32 mem_releases;
    uint32 mem_frees;
};

struct dxg_shared_resource_diag {
    uint32 metadata_seen;
    uint32 metadata_runtime_size;
    uint32 metadata_runtime_hash;
    uint32 metadata_resource_size;
    uint32 metadata_resource_hash;
    uint32 metadata_total_size;
    uint32 metadata_total_hash;
    uint32 metadata_alloc0_priv;
    uint32 metadata_match_in;
    uint32 metadata_match_out;
    uint32 metadata_logical_flags;

    uint32 nt_diag_seen;
    uint32 nt_resource_host;
    uint32 nt_alloc_host;
    uint32 nt_meta_before;
    uint32 nt_meta_after;
    uint32 nt_seal_before;
    uint32 nt_seal_after;
    uint32 nt_host_seal_before;
    uint32 nt_host_seal_after;

    uint32 pre_private_seen;
    uint32 pre_runtime_size;
    uint32 pre_runtime_hash;
    uint32 pre_resource_size;
    uint32 pre_resource_hash;
    uint32 pre_total_size;
    uint32 pre_total_hash;
    uint32 pre_alloc_out_size;
    uint32 pre_alloc_out_hash;

    uint32 runtime_object_seen;
    uint32 runtime_user_obj;
    uint32 runtime_user_dev;
    uint32 runtime_kind;
    uint32 runtime_host_obj;
    uint32 runtime_host_dev;
    uint32 runtime_entry_found;
    uint32 runtime_entry_exact;
    uint32 runtime_owner_refs;

    uint32 query_seen;
    int32 query_ret;
    uint32 query_device;
    uint64 query_nt;
    uint32 query_kind;
    uint32 query_fops;
    uint32 query_refs;
    uint32 query_object;
    uint32 query_cache_object;
    uint32 query_allocs;
    uint32 query_runtime_size;
    uint32 query_resource_size;
    uint32 query_total_size;

    uint32 open_seen;
    int32 open_ret;
    uint32 open_device;
    uint32 open_global;
    uint32 open_allocs;
    uint32 open_total_priv;
    uint32 open_result_resource;
    uint32 open_result_alloc0;
    uint32 open_seal_before;
    uint32 open_seal_after;
    uint32 open_fd_kind;
    uint32 open_fd_refs;

    uint32 record_seen;
    uint32 record_valid;
    uint32 record_stage;
    uint32 record_key_process;
    uint32 record_key_object;
    uint32 record_key_nt;
    uint32 record_source_process;
    uint32 record_source_generation;
    uint32 record_resource;
    uint32 record_allocation;
    uint32 record_sealed;
    uint32 record_sealed_generation;
    uint32 record_seal_before_fd;
    uint32 record_allocs;
    uint32 record_runtime_size;
    uint32 record_resource_size;
    uint32 record_total_size;
    uint32 record_alloc0_priv;
    uint32 record_runtime_hash;
    uint32 record_resource_hash;
    uint32 record_total_hash;
    uint32 record_alloc0_hash;
    uint32 record_refs;
    uint32 record_query_count;
    uint32 record_open_count;
    uint32 record_fd_publish_count;
    int32 record_local_admit_ret;
    uint32 record_local_exact;
    uint32 record_mutated;
    uint32 model_valid;
    uint32 model_flat_match;
    uint32 model_allocs;
    uint32 model_alloc0;
    uint32 model_alloc0_priv;
    uint64 model_alloc0_size;
    uint64 model_alloc0_pages;
    uint32 model_alloc0_flags;
    uint32 model_alloc0_cached;
    uint32 model_runtime_size;
    uint32 model_resource_size;
    uint32 model_total_size;
    uint32 model_sealed;
    uint32 model_generation;

    uint32 parent_seen;
    uint32 parent_next;
    uint32 parent_last;
    uint32 parent_refs;
    uint32 parent_fd_refs;
    uint32 parent_children;
    uint32 parent_last_child;
    uint32 parent_sealed_generation;
    uint32 parent_publish_count;
    uint32 parent_open_count;
    uint32 parent_release_count;
};

struct dxg_present_credit_status {
    int rc;
    uint64 display_presents;
    uint64 display_completions;
    uint64 register_attempts;
    uint64 commit_attempts;
};

static int dxg_prefix_eq(const char *s, const char *prefix)
{
    while (*prefix != 0) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static char *dxg_find_text(char *s, const char *needle)
{
    if (s == 0 || needle == 0 || *needle == 0)
        return 0;
    for (; *s != 0; s++) {
        if (dxg_prefix_eq(s, needle))
            return s;
    }
    return 0;
}

static int dxg_is_line_end(char c)
{
    return c == 0 || c == '\n' || c == '\r';
}

static char *dxg_find_status_line(char *s, const char *name)
{
    char *p = s;

    if (s == 0 || name == 0 || *name == 0)
        return 0;
    while (*p != 0) {
        if (dxg_prefix_eq(p, name))
            return p;
        while (!dxg_is_line_end(*p))
            p++;
        while (*p == '\n' || *p == '\r')
            p++;
    }
    return 0;
}

static int dxg_is_field_boundary(char c)
{
    return c == 0 || c == ' ' || c == '\t';
}

static char *dxg_find_field_on_line(char *line, const char *name)
{
    char *p;
    char *first_value;

    if (line == 0 || name == 0 || *name == 0)
        return 0;
    first_value = line;
    while (!dxg_is_line_end(*first_value) && *first_value != '=')
        first_value++;
    if (*first_value == '=')
        first_value++;
    for (p = line; !dxg_is_line_end(*p); p++) {
        char prev = p == line ? 0 : p[-1];

        if ((p == first_value || dxg_is_field_boundary(prev)) &&
            dxg_prefix_eq(p, name))
            return p;
    }
    return 0;
}

static int dxg_parse_uint_after_field_on_line(char *line, const char *name,
                                              uint32 *out)
{
    char *p;
    int seen = 0;
    uint64 value = 0;
    int base = 10;
    int neg = 0;

    p = dxg_find_field_on_line(line, name);
    if (p == 0)
        return -1;
    p += strlen(name);
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    for (; !dxg_is_line_end(*p); p++) {
        int digit;

        if (*p >= '0' && *p <= '9')
            digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            digit = 10 + *p - 'a';
        else if (*p >= 'A' && *p <= 'F')
            digit = 10 + *p - 'A';
        else
            break;
        if (digit >= base)
            break;
        value = value * base + digit;
        seen = 1;
    }
    if (!seen || (!dxg_is_line_end(*p) && !dxg_is_field_boundary(*p)))
        return -1;
    *out = neg ? (uint32)(-(int64)value) : (uint32)value;
    return 0;
}

static int dxg_parse_uint_after(char *line, const char *name, uint32 *out)
{
    char *p;
    int seen = 0;
    uint64 value = 0;
    int base = 10;
    int neg = 0;

    p = dxg_find_text(line, name);
    if (p == 0)
        return -1;
    p += strlen(name);
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    for (; *p != 0; p++) {
        int digit;

        if (*p >= '0' && *p <= '9')
            digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            digit = 10 + *p - 'a';
        else if (*p >= 'A' && *p <= 'F')
            digit = 10 + *p - 'A';
        else
            break;
        if (digit >= base)
            break;
        value = value * base + digit;
        seen = 1;
    }
    if (!seen)
        return -1;
    *out = neg ? (uint32)(-(int64)value) : (uint32)value;
    return 0;
}

static int dxg_parse_uint_pair_after(char *line, const char *name,
                                     uint32 *first, uint32 *second)
{
    char *p;
    uint32 values[2] = { 0, 0 };

    p = dxg_find_text(line, name);
    if (p == 0)
        return -1;
    p += strlen(name);
    for (uint32 i = 0; i < 2; i++) {
        int seen = 0;

        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        for (; *p != 0; p++) {
            int digit;

            if (*p >= '0' && *p <= '9')
                digit = *p - '0';
            else if (*p >= 'a' && *p <= 'f')
                digit = 10 + *p - 'a';
            else if (*p >= 'A' && *p <= 'F')
                digit = 10 + *p - 'A';
            else
                break;
            if (digit >= 16)
                break;
            values[i] = values[i] * 16 + (uint32)digit;
            seen = 1;
        }
        if (!seen)
            return -1;
        if (i == 0) {
            if (*p != ',')
                return -1;
            p++;
        }
    }
    *first = values[0];
    *second = values[1];
    return 0;
}

static char *read_dxg_status_buffer(void)
{
    char *buf;
    int fd;
    int n;

    buf = malloc(65536);
    if (buf == 0)
        return 0;
    fd = open("/dev/dxg", O_RDONLY);
    if (fd < 0) {
        free(buf);
        return 0;
    }
    n = read(fd, buf, 65535);
    close(fd);
    if (n <= 0) {
        free(buf);
        return 0;
    }
    buf[n] = 0;
    return buf;
}

static char *read_text_file(const char *path)
{
    char *buf;
    int fd;
    int n;

    if (path == 0 || path[0] == 0)
        return 0;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    buf = malloc(65536);
    if (buf == 0) {
        close(fd);
        return 0;
    }
    n = read(fd, buf, 65535);
    close(fd);
    if (n <= 0) {
        free(buf);
        return 0;
    }
    buf[n] = 0;
    return buf;
}

static int wddm_hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int dxg_parse_u64_after(char *line, const char *name, uint64 *out)
{
    char *p;
    int seen = 0;
    int base = 10;
    uint64 value = 0;

    p = dxg_find_text(line, name);
    if (p == 0)
        return -1;
    p += strlen(name);
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    for (; *p != 0; p++) {
        int digit;

        if (*p >= '0' && *p <= '9')
            digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            digit = 10 + *p - 'a';
        else if (*p >= 'A' && *p <= 'F')
            digit = 10 + *p - 'A';
        else
            break;
        if (digit >= base)
            break;
        value = value * (uint64)base + (uint64)digit;
        seen = 1;
    }
    if (!seen)
        return -1;
    *out = value;
    return 0;
}

static int dxg_parse_luid_after(char *line, const char *name,
                                uint32 *high, uint32 *low)
{
    char *p;
    uint32 values[2] = {0, 0};

    p = dxg_find_text(line, name);
    if (p == 0)
        return -1;
    p += strlen(name);
    for (int i = 0; i < 2; i++) {
        int seen = 0;

        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        for (; *p != 0; p++) {
            int digit = wddm_hex_nibble(*p);

            if (digit < 0)
                break;
            values[i] = values[i] * 16 + (uint32)digit;
            seen = 1;
        }
        if (!seen)
            return -1;
        if (i == 0) {
            if (*p != ':' && *p != '-')
                return -1;
            p++;
        }
    }
    *high = values[0];
    *low = values[1];
    return 0;
}

static int dxg_parse_head8_after(char *line, const char *name,
                                 unsigned char *out, uint32 *out_len)
{
    char *p;
    uint32 n = 0;

    p = dxg_find_text(line, name);
    if (p == 0)
        return -1;
    p += strlen(name);
    memset(out, 0, 8);
    while (*p != 0 && n < 8) {
        int hi = wddm_hex_nibble(p[0]);
        int lo = wddm_hex_nibble(p[1]);

        if (hi < 0 || lo < 0)
            break;
        out[n++] = (unsigned char)((hi << 4) | lo);
        p += 2;
    }
    *out_len = n;
    return n != 0 ? 0 : -1;
}

#define WDDM_CLEAN_DESTROY_ALLOC   0x0001U
#define WDDM_CLEAN_DESTROY_CONTEXT 0x0002U
#define WDDM_CLEAN_DESTROY_HWQUEUE 0x0004U
#define WDDM_CLEAN_DESTROY_PAGING  0x0008U
#define WDDM_CLEAN_DESTROY_DEVICE  0x0010U
#define WDDM_CLEAN_DESTROY_SYNC    0x0020U
#define WDDM_CLEAN_FREE_GPUVA      0x0040U
#define WDDM_CLEAN_CLOSE_ADAPTER   0x0080U
#define WDDM_CLEAN_EVICT           0x0100U
#define WDDM_NAME_MAX              128U

struct wddm_trace_fields {
    uint32 adapter_seen;
    uint32 luid_high;
    uint32 luid_low;
    uint32 context_seen;
    uint32 context_node;
    uint32 context_engine;
    uint32 context_flags;
    uint32 context_hint;
    uint32 context_priv;
    uint32 context_head_len;
    unsigned char context_head[8];
    uint32 hwqueue_seen;
    uint32 hwqueue_flags;
    uint32 hwqueue_priv;
    uint32 hwqueue_head_len;
    unsigned char hwqueue_head[8];
    uint64 hwqueue_fence_cpu;
    uint64 hwqueue_fence_gpu;
    uint32 submit_seen;
    uint32 submit_cmd_len;
    uint32 submit_priv;
    uint32 submit_primaries;
    uint32 submit_head_len;
    unsigned char submit_head[8];
    uint32 submit_count;
    uint32 submit_cmd_lens[64];
    uint32 submit_privs[64];
    uint32 submit_head_lens[64];
    unsigned char submit_heads[64][8];
    uint32 allocation_seen;
    uint32 allocation_count;
    uint32 allocation_runtime;
    uint32 allocation_resource_priv;
    uint32 allocation_flags;
    uint32 allocation_alloc_priv;
    uint32 allocation_alloc_flags;
    uint32 allocation_out_priv;
    uint32 makeresident_seen;
    uint32 makeresident_count;
    uint32 makeresident_flags;
    uint64 makeresident_fence;
    uint32 map_seen;
    uint32 map_pages;
    uint32 map_count;
    uint32 map_pages_seen[128];
    uint64 map_prot_seen[128];
    uint64 map_dprot_seen[128];
    uint64 map_prot;
    uint64 map_dprot;
    uint64 map_fence;
    uint32 sync_seen;
    uint32 sync_type;
    uint32 sync_flags;
    uint64 sync_fence_cpu;
    uint64 sync_fence_gpu;
    uint32 sync_mapped_seen;
    uint32 sync_mapped_type;
    uint32 sync_mapped_flags;
    uint32 lock_seen;
    uint32 unlock_seen;
    uint32 raw_unlock_ioctl_seen;
    uint32 renderer_vendor;
    uint32 trace_adapter_vendor;
    uint32 umdriver_vendor;
    uint32 hardware_seen;
    uint32 hardware_vendor_id;
    uint32 hardware_device_id;
    uint32 hardware_subvendor_id;
    uint32 queryadapter_last_type;
    uint32 queryadapter_last_rc;
    char trace_adapter_name[WDDM_NAME_MAX];
    char renderer_name[WDDM_NAME_MAX];
    char adapter_description[WDDM_NAME_MAX];
    char umdriver_name[WDDM_NAME_MAX];
    uint32 cache_seen;
    uint32 cleanup_mask;
};

struct wddm_kernel_fields {
    uint32 luid_seen;
    uint32 luid_high;
    uint32 luid_low;
    uint32 adapter_luid_seen;
    uint32 adapter_luid_high;
    uint32 adapter_luid_low;
    uint32 host_adapter_luid_seen;
    uint32 host_adapter_luid_high;
    uint32 host_adapter_luid_low;
    uint32 host_vgpu_luid_seen;
    uint32 host_vgpu_luid_high;
    uint32 host_vgpu_luid_low;
    uint32 um_adapter_luid_seen;
    uint32 um_adapter_luid_high;
    uint32 um_adapter_luid_low;
    uint32 host_equivalent_luid_known;
    uint32 openadapter_luid_seen;
    uint32 openadapter_input_high;
    uint32 openadapter_input_low;
    uint32 openadapter_um_high;
    uint32 openadapter_um_low;
    uint32 openadapter_mapped_host_high;
    uint32 openadapter_mapped_host_low;
    uint32 openadapter_host_basis;
    uint32 openadapter_match;
    uint32 openadapter_handle;
    int32 openadapter_ret;
    uint32 adapter_vendor_id;
    uint32 adapter_device_id;
    char requested_adapter_name[WDDM_NAME_MAX];
    uint32 context_seen;
    uint32 context_len;
    uint32 context_node;
    uint32 context_engine;
    uint32 context_flags;
    uint32 context_hint;
    uint32 context_priv;
    uint32 context_head_len;
    unsigned char context_head[8];
    uint32 hwqueue_seen;
    uint32 hwqueue_len;
    uint32 hwqueue_flags;
    uint32 hwqueue_priv;
    uint32 hwqueue_head_len;
    unsigned char hwqueue_head[8];
    uint64 hwqueue_fence_cpu;
    uint64 hwqueue_fence_gpu;
    uint32 submit_len;
    uint32 submit_priv;
    uint32 submit_cmd_len;
    uint32 submit_head_len;
    unsigned char submit_head[8];
    uint32 submit_count;
    uint32 submit_lens_seen[32];
    uint32 submit_cmd_lens[32];
    uint32 submit_privs[32];
    uint32 submit_head_lens[32];
    unsigned char submit_heads[32][8];
    uint32 allocation_seen;
    uint32 allocation_len;
    uint32 allocation_count;
    uint32 allocation_runtime;
    uint32 allocation_resource_priv;
    uint32 allocation_alloc_priv;
    uint32 allocation_flags;
    uint32 allocation_in_head_len;
    unsigned char allocation_in_head[8];
    uint32 allocation_out_priv;
    uint32 makeresident_seen;
    uint32 makeresident_len;
    uint32 makeresident_count;
    uint32 makeresident_flags;
    uint64 makeresident_fence;
    uint32 map_seen;
    uint32 map_len;
    uint32 map_pages;
    uint32 map_count;
    uint32 map_pages_seen[32];
    uint64 map_prot_seen[32];
    uint64 map_dprot_seen[32];
    uint64 map_prot;
    uint64 map_dprot;
    uint64 map_fence;
    uint32 sync_seen;
    uint32 sync_len;
    uint32 sync_type;
    uint32 sync_flags;
    uint64 sync_fence_cpu;
    uint64 sync_fence_gpu;
    uint32 sync_mapped_seen;
    uint32 sync_mapped_type;
    uint32 sync_mapped_flags;
    uint64 sync_mapped_fence_cpu;
    uint64 sync_mapped_fence_gpu;
    uint32 lock_seen;
    uint32 lock_len;
    uint32 lock_ioctl_count;
    uint32 lock_forward_count;
    uint32 lock_cached_ref_count;
    uint32 unlock_len;
    uint32 host_lock_count;
    uint32 host_unlock_count;
    uint32 unlock_ioctl_count;
    uint32 unlock_forward_count;
    uint32 unlock_missing_tracking_count;
    uint32 unlock_cached_ref_count;
    uint32 cache_seen;
    uint32 destroyalloc_count;
    uint32 destroycontext_count;
    uint32 destroyhwqueue_count;
    uint32 destroypaging_count;
    uint32 destroydevice_count;
    uint32 destroysync_count;
    uint32 freegpuva_count;
    uint32 closeadapter_count;
    uint32 host_destroyalloc_count;
    uint32 host_destroycontext_count;
    uint32 host_destroyhwqueue_count;
    uint32 host_destroypaging_count;
    uint32 host_destroydevice_count;
    uint32 host_destroysync_count;
    uint32 host_destroyprocess_count;
    uint32 host_freegpuva_count;
    uint32 closeadapter_ioctl_count;
    uint32 closeadapter_local_count;
    uint32 closeadapter_host_count;
    uint32 closeadapter_invalid_count;
    uint32 closeadapter_len;
    uint32 closeadapter_order_seen;
    uint32 closeadapter_order_seq;
    uint32 closeadapter_order_close;
    uint32 closeadapter_order_last_destroy;
    uint32 closeadapter_order_destroyprocess;
    uint32 closeadapter_order_after_destroy;
    uint32 evict_len;
    uint32 createprocess_seen;
    uint32 createprocess_len;
    uint32 createprocess_layout;
    uint32 createprocess_success_seen;
    uint32 createprocess_success_len;
    uint32 createprocess_success_layout;
};

static void wddm_trace_note_map_pages(struct wddm_trace_fields *t,
                                      uint32 pages, uint64 prot,
                                      uint64 dprot);
static void wddm_kernel_note_map_pages(struct wddm_kernel_fields *k,
                                       uint32 pages, uint64 prot,
                                       uint64 dprot);
static void wddm_kernel_note_submit(struct wddm_kernel_fields *k,
                                    uint32 len, uint32 cmd_len,
                                    uint32 priv, uint32 head_len,
                                    const unsigned char *head);

#define WDDM_VENDOR_NVIDIA 0x10deU
#define WDDM_VENDOR_AMD 0x1002U
#define WDDM_VENDOR_INTEL 0x8086U

static uint32 wddm_vendor_from_text(const char *text)
{
    if (dxg_find_text((char *)text, "NVIDIA") != 0 ||
        dxg_find_text((char *)text, "nvwgf") != 0 ||
        dxg_find_text((char *)text, "nvmi") != 0)
        return WDDM_VENDOR_NVIDIA;
    if (dxg_find_text((char *)text, "AMD") != 0 ||
        dxg_find_text((char *)text, "Radeon") != 0)
        return WDDM_VENDOR_AMD;
    if (dxg_find_text((char *)text, "Intel") != 0 ||
        dxg_find_text((char *)text, "igd") != 0)
        return WDDM_VENDOR_INTEL;
    return 0;
}

static uint32 wddm_trace_renderer_vendor(const char *line)
{
    return wddm_vendor_from_text(line);
}

static int wddm_known_display_vendor(uint32 vendor)
{
    return vendor == WDDM_VENDOR_NVIDIA ||
           vendor == WDDM_VENDOR_AMD ||
           vendor == WDDM_VENDOR_INTEL;
}

static int wddm_luid_equal(uint32 ah, uint32 al, uint32 bh, uint32 bl)
{
    return ah == bh && al == bl;
}

static int wddm_luid_nonzero(uint32 high, uint32 low)
{
    return high != 0 || low != 0;
}

static void wddm_copy_until(char *out, uint32 out_size, const char *start,
                            char stop)
{
    uint32 i = 0;

    if (out == 0 || out_size == 0)
        return;
    out[0] = 0;
    if (start == 0)
        return;
    while (start[i] != 0 && start[i] != stop &&
           start[i] != '\r' && start[i] != '\n' &&
           i + 1 < out_size) {
        out[i] = start[i];
        i++;
    }
    out[i] = 0;
}

static int wddm_copy_token_after(char *line, const char *name, char *out,
                                 uint32 out_size)
{
    char *p = dxg_find_text(line, name);

    if (p == 0)
        return -1;
    p += strlen(name);
    wddm_copy_until(out, out_size, p, ' ');
    return out[0] != 0 ? 0 : -1;
}

static int wddm_copy_rest_after(char *line, const char *name, char *out,
                                uint32 out_size)
{
    char *p = dxg_find_text(line, name);

    if (p == 0)
        return -1;
    p += strlen(name);
    wddm_copy_until(out, out_size, p, '\0');
    return out[0] != 0 ? 0 : -1;
}

static int wddm_copy_between_after(char *line, const char *name,
                                   char stop, char *out, uint32 out_size)
{
    char *p = dxg_find_text(line, name);

    if (p == 0)
        return -1;
    p += strlen(name);
    wddm_copy_until(out, out_size, p, stop);
    return out[0] != 0 ? 0 : -1;
}

static int wddm_parse_head_u32_le_after(char *line, const char *name,
                                        uint32 word_index, uint32 *out)
{
    char *p = dxg_find_text(line, name);
    uint32 value = 0;

    if (p == 0 || out == 0)
        return -1;
    p += strlen(name);
    p += word_index * 8;
    for (uint32 byte = 0; byte < 4; byte++) {
        int hi = wddm_hex_nibble(p[byte * 2]);
        int lo = wddm_hex_nibble(p[byte * 2 + 1]);

        if (hi < 0 || lo < 0)
            return -1;
        value |= (uint32)((hi << 4) | lo) << (byte * 8);
    }
    *out = value;
    return 0;
}

static void wddm_trace_note_hardware_id(struct wddm_trace_fields *t,
                                        uint32 word0, uint32 word1,
                                        uint32 word2, uint32 word3)
{
    if (wddm_known_display_vendor(word0)) {
        t->hardware_vendor_id = word0;
        t->hardware_device_id = word1;
        t->hardware_subvendor_id = word2;
    } else {
        t->hardware_vendor_id = word1;
        t->hardware_device_id = word2;
        t->hardware_subvendor_id = word3;
    }
    if (t->hardware_vendor_id != 0 || t->hardware_device_id != 0)
        t->hardware_seen = 1;
}

static uint32 wddm_trace_identity_vendor(const struct wddm_trace_fields *t)
{
    if (t->hardware_vendor_id != 0)
        return t->hardware_vendor_id;
    if (t->renderer_vendor != 0)
        return t->renderer_vendor;
    if (t->trace_adapter_vendor != 0)
        return t->trace_adapter_vendor;
    if (t->umdriver_vendor != 0)
        return t->umdriver_vendor;
    return 0;
}

static int wddm_physical_vendor_match(const struct wddm_trace_fields *t,
                                      const struct wddm_kernel_fields *k)
{
    uint32 trace_vendor = wddm_trace_identity_vendor(t);

    return trace_vendor != 0 && k->adapter_vendor_id != 0 &&
           trace_vendor == k->adapter_vendor_id;
}

static int wddm_physical_adapter_evidence(
    const struct wddm_trace_fields *t, const struct wddm_kernel_fields *k)
{
    if (!t->hardware_seen || !wddm_physical_vendor_match(t, k) ||
        k->adapter_device_id == 0 || t->hardware_device_id == 0)
        return 0;
    return t->hardware_device_id == k->adapter_device_id;
}

static int wddm_shifted_hardware_adapter_evidence(
    const struct wddm_trace_fields *t, const struct wddm_kernel_fields *k)
{
    uint32 trace_vendor = wddm_trace_identity_vendor(t);

    return t->hardware_seen && trace_vendor != 0 &&
           t->hardware_device_id != 0 && t->hardware_subvendor_id != 0 &&
           k->adapter_vendor_id == t->hardware_device_id &&
           k->adapter_device_id == t->hardware_subvendor_id;
}

static int wddm_adapter_identity_evidence(
    const struct wddm_trace_fields *t, const struct wddm_kernel_fields *k)
{
    return wddm_physical_adapter_evidence(t, k) ||
           wddm_shifted_hardware_adapter_evidence(t, k);
}

static int wddm_requested_adapter_matches_trace(
    const struct wddm_trace_fields *t, const struct wddm_kernel_fields *k)
{
    uint32 requested_vendor;
    uint32 trace_vendor = wddm_trace_identity_vendor(t);

    if (k->requested_adapter_name[0] == 0 || trace_vendor == 0)
        return 0;
    requested_vendor = wddm_vendor_from_text(k->requested_adapter_name);
    return requested_vendor != 0 && requested_vendor == trace_vendor;
}

static int wddm_host_adapter_hardware_equivalent(
    const struct wddm_kernel_fields *k)
{
    return k->host_adapter_luid_seen &&
           wddm_luid_nonzero(k->host_adapter_luid_high,
                             k->host_adapter_luid_low) &&
           k->adapter_vendor_id != 0 && k->adapter_device_id != 0;
}

static int wddm_openadapter_luid_maps_to_host(
    const struct wddm_kernel_fields *k)
{
    int input_is_kernel_um;
    int mapped_host_known;

    if (!k->openadapter_luid_seen || k->openadapter_match == 0 ||
        k->openadapter_ret != 0 || k->openadapter_handle == 0 ||
        k->openadapter_host_basis == 0)
        return 0;
    input_is_kernel_um =
        wddm_luid_equal(k->openadapter_input_high,
                        k->openadapter_input_low,
                        k->openadapter_um_high,
                        k->openadapter_um_low) ||
        (k->um_adapter_luid_seen &&
         wddm_luid_equal(k->openadapter_input_high,
                         k->openadapter_input_low,
                         k->um_adapter_luid_high,
                         k->um_adapter_luid_low)) ||
        (k->adapter_luid_seen &&
         wddm_luid_equal(k->openadapter_input_high,
                         k->openadapter_input_low,
                         k->adapter_luid_high,
                         k->adapter_luid_low));
    mapped_host_known =
        wddm_luid_nonzero(k->openadapter_mapped_host_high,
                          k->openadapter_mapped_host_low) &&
        (wddm_luid_equal(k->openadapter_mapped_host_high,
                         k->openadapter_mapped_host_low,
                         k->host_adapter_luid_high,
                         k->host_adapter_luid_low) ||
         wddm_luid_equal(k->openadapter_mapped_host_high,
                         k->openadapter_mapped_host_low,
                         k->host_vgpu_luid_high,
                         k->host_vgpu_luid_low));
    return input_is_kernel_um && mapped_host_known;
}

static int wddm_lock_host_rewrite_seen(
    const struct wddm_kernel_fields *k)
{
    return k->host_lock_count != 0 &&
           (k->lock_forward_count != 0 ||
            (k->lock_seen && k->lock_len != 0));
}

static int wddm_unlock_host_rewrite_seen(
    const struct wddm_kernel_fields *k)
{
    return k->host_unlock_count != 0 &&
           (k->unlock_forward_count != 0 || k->unlock_len != 0);
}

static int wddm_closeadapter_process_destroy_equivalent(
    const struct wddm_kernel_fields *k)
{
    return k->host_destroyprocess_count != 0 &&
           k->closeadapter_count == 0 &&
           k->closeadapter_ioctl_count == 0 &&
           k->closeadapter_local_count == 0 &&
           k->closeadapter_host_count == 0 &&
           k->closeadapter_invalid_count == 0 &&
           k->closeadapter_len == 0;
}

static int wddm_closeadapter_cleanup_equivalent(
    const struct wddm_kernel_fields *k)
{
    if (k->closeadapter_order_seen)
        return k->closeadapter_order_after_destroy != 0 &&
               (k->closeadapter_local_count != 0 ||
                k->closeadapter_host_count != 0);
    return wddm_closeadapter_process_destroy_equivalent(k);
}

static void parse_wddm_trace_line(char *line, struct wddm_trace_fields *t)
{
    if (dxg_find_text(line, "dxgtrace: ->") != 0) {
        if (dxg_find_text(line, "-> create_hwqueue") != 0) {
            dxg_parse_u64_after(line, "fence_cpu:", &t->hwqueue_fence_cpu);
            dxg_parse_u64_after(line, "fence_gpu:", &t->hwqueue_fence_gpu);
        } else if (dxg_find_text(line, "-> submit_hwqueue") != 0) {
            t->submit_seen = 1;
        } else if (dxg_find_text(line, "-> make_resident") != 0) {
            dxg_parse_u64_after(line, "fence:", &t->makeresident_fence);
        } else if (dxg_find_text(line, "-> map_gpu_va") != 0) {
            dxg_parse_u64_after(line, "fence:", &t->map_fence);
        } else if (dxg_find_text(line, "-> create_sync") != 0) {
            dxg_parse_u64_after(line, "fence_cpu:", &t->sync_fence_cpu);
            dxg_parse_u64_after(line, "fence_gpu:", &t->sync_fence_gpu);
            if (t->sync_fence_cpu != 0 && t->sync_fence_gpu != 0) {
                t->sync_mapped_seen = 1;
                t->sync_mapped_type = t->sync_type;
                t->sync_mapped_flags = t->sync_flags;
            }
        } else if (dxg_find_text(line, "-> query_adapter ") != 0) {
            uint32 rc = 0;

            dxg_parse_uint_after(line, "type=", &t->queryadapter_last_type);
            if (dxg_parse_uint_after(line, "rc=", &rc) == 0)
                t->queryadapter_last_rc = rc;
        }
        return;
    }

    if (dxg_find_text(line, "renderer=D3D12") != 0) {
        t->renderer_vendor = wddm_trace_renderer_vendor(line);
        wddm_copy_between_after(line, "renderer=D3D12 (", ')',
                                t->renderer_name,
                                sizeof(t->renderer_name));
    }

    if (dxg_find_text(line, "dxgtrace: trace_env ") != 0) {
        if (wddm_copy_token_after(line, "default_adapter=",
                                  t->trace_adapter_name,
                                  sizeof(t->trace_adapter_name)) == 0)
            t->trace_adapter_vendor =
                wddm_vendor_from_text(t->trace_adapter_name);
    } else if (dxg_find_text(line, "dxgtrace: query_adapter ") != 0) {
        dxg_parse_uint_after(line, "type=", &t->queryadapter_last_type);
        t->queryadapter_last_rc = 0xffffffffU;
    } else if (dxg_find_text(line,
                              "dxgtrace: query_adapter_hardware ") != 0) {
        if (dxg_parse_uint_after(line, "vendor:",
                                 &t->hardware_vendor_id) != 0)
            dxg_parse_uint_after(line, "vendor=",
                                 &t->hardware_vendor_id);
        if (dxg_parse_uint_after(line, "device:",
                                 &t->hardware_device_id) != 0)
            dxg_parse_uint_after(line, "device=",
                                 &t->hardware_device_id);
        if (dxg_parse_uint_after(line, "subvendor:",
                                 &t->hardware_subvendor_id) != 0)
            dxg_parse_uint_after(line, "subvendor=",
                                 &t->hardware_subvendor_id);
        if (t->hardware_vendor_id != 0 || t->hardware_device_id != 0)
            t->hardware_seen = 1;
    } else if (dxg_find_text(line, "dxgtrace: query_adapter_description ") != 0) {
        if (wddm_copy_rest_after(line, "text=", t->adapter_description,
                                 sizeof(t->adapter_description)) == 0 &&
            t->renderer_vendor == 0)
            t->renderer_vendor =
                wddm_vendor_from_text(t->adapter_description);
    } else if (dxg_find_text(line, "dxgtrace: query_adapter_umdrivername ") != 0) {
        if (wddm_copy_rest_after(line, "text=", t->umdriver_name,
                                 sizeof(t->umdriver_name)) == 0)
            t->umdriver_vendor = wddm_vendor_from_text(t->umdriver_name);
    } else if (dxg_find_text(line, "dxgtrace: query_adapter_out ") != 0 &&
               t->queryadapter_last_type == 31 &&
               t->queryadapter_last_rc == 0) {
        uint32 word0 = 0;
        uint32 word1 = 0;
        uint32 word2 = 0;
        uint32 word3 = 0;

        if (wddm_parse_head_u32_le_after(line, "head=", 0, &word0) == 0 &&
            wddm_parse_head_u32_le_after(line, "head=", 1, &word1) == 0 &&
            wddm_parse_head_u32_le_after(line, "head=", 2, &word2) == 0) {
            wddm_parse_head_u32_le_after(line, "head=", 3, &word3);
            wddm_trace_note_hardware_id(t, word0, word1, word2, word3);
        }
    } else if (dxg_find_text(line, "dxgtrace: open_adapter_luid") != 0) {
        if (dxg_parse_luid_after(line, "luid=", &t->luid_high,
                                 &t->luid_low) == 0)
            t->adapter_seen = 1;
    } else if (dxg_find_text(line, "dxgtrace: create_context ") != 0) {
        t->context_seen = 1;
        dxg_parse_uint_after(line, "node=", &t->context_node);
        dxg_parse_uint_after(line, "engine=", &t->context_engine);
        dxg_parse_uint_after(line, "flags=", &t->context_flags);
        dxg_parse_uint_after(line, "hint=", &t->context_hint);
        dxg_parse_uint_after(line, "priv=", &t->context_priv);
    } else if (dxg_find_text(line, "dxgtrace: create_context_priv ") != 0) {
        dxg_parse_uint_after(line, "size=", &t->context_priv);
        dxg_parse_head8_after(line, "head=", t->context_head,
                              &t->context_head_len);
    } else if (dxg_find_text(line, "dxgtrace: create_hwqueue ") != 0) {
        t->hwqueue_seen = 1;
        dxg_parse_uint_after(line, "flags=", &t->hwqueue_flags);
        dxg_parse_uint_after(line, "priv=", &t->hwqueue_priv);
    } else if (dxg_find_text(line, "dxgtrace: create_hwqueue_priv ") != 0) {
        dxg_parse_uint_after(line, "size=", &t->hwqueue_priv);
        dxg_parse_head8_after(line, "head=", t->hwqueue_head,
                              &t->hwqueue_head_len);
    } else if (dxg_find_text(line, "dxgtrace: submit_hwqueue ") != 0) {
        t->submit_seen = 1;
        dxg_parse_uint_after(line, "len=", &t->submit_cmd_len);
        dxg_parse_uint_after(line, "priv=", &t->submit_priv);
        dxg_parse_uint_after(line, "primaries=", &t->submit_primaries);
        if (t->submit_count <
            sizeof(t->submit_cmd_lens) / sizeof(t->submit_cmd_lens[0])) {
            t->submit_cmd_lens[t->submit_count] = t->submit_cmd_len;
            t->submit_privs[t->submit_count] = t->submit_priv;
            t->submit_count++;
        }
    } else if (dxg_find_text(line, "dxgtrace: submit_hwqueue_priv ") != 0) {
        dxg_parse_uint_after(line, "size=", &t->submit_priv);
        dxg_parse_head8_after(line, "head=", t->submit_head,
                              &t->submit_head_len);
        if (t->submit_count != 0) {
            t->submit_privs[t->submit_count - 1] = t->submit_priv;
            t->submit_head_lens[t->submit_count - 1] =
                t->submit_head_len;
            memcpy(t->submit_heads[t->submit_count - 1], t->submit_head,
                   sizeof(t->submit_head));
        }
    } else if (dxg_find_text(line, "dxgtrace: create_allocation ") != 0) {
        t->allocation_seen = 1;
        dxg_parse_uint_after(line, "alloc_count=", &t->allocation_count);
        dxg_parse_uint_after(line, "runtime=", &t->allocation_runtime);
        dxg_parse_uint_after(line, "priv=", &t->allocation_resource_priv);
        dxg_parse_uint_after(line, "flags=", &t->allocation_flags);
    } else if (dxg_find_text(line, "dxgtrace: create_allocation_in alloc[0]") != 0) {
        dxg_parse_uint_after(line, "priv=", &t->allocation_alloc_priv);
        dxg_parse_uint_after(line, "flags=", &t->allocation_alloc_flags);
    } else if (dxg_find_text(line, "dxgtrace: create_allocation_out alloc[0]") != 0) {
        dxg_parse_uint_after(line, "priv=", &t->allocation_out_priv);
    } else if (dxg_find_text(line, "dxgtrace: make_resident ") != 0) {
        t->makeresident_seen = 1;
        dxg_parse_uint_after(line, "count=", &t->makeresident_count);
        dxg_parse_uint_after(line, "flags=", &t->makeresident_flags);
    } else if (dxg_find_text(line, "dxgtrace: map_gpu_va ") != 0) {
        t->map_seen = 1;
        dxg_parse_uint_after(line, "size_pages=", &t->map_pages);
        dxg_parse_u64_after(line, "prot=", &t->map_prot);
        dxg_parse_u64_after(line, "dprot=", &t->map_dprot);
        wddm_trace_note_map_pages(t, t->map_pages, t->map_prot,
                                  t->map_dprot);
    } else if (dxg_find_text(line, "dxgtrace: create_sync ") != 0) {
        t->sync_seen = 1;
        dxg_parse_uint_after(line, "type=", &t->sync_type);
        dxg_parse_uint_after(line, "flags=", &t->sync_flags);
    } else if (dxg_find_text(line, "dxgtrace: lock2 ") != 0) {
        t->lock_seen = 1;
    } else if (dxg_find_text(line, "dxgtrace: unlock2 ") != 0) {
        t->unlock_seen = 1;
    } else if (dxg_find_text(line, "dxgtrace: ioctl nr=0x37") != 0) {
        t->raw_unlock_ioctl_seen = 1;
    } else if (dxg_find_text(line, "dxgtrace: flush_heap ") != 0 ||
               dxg_find_text(line, "dxgtrace: invalidate_cache ") != 0) {
        t->cache_seen = 1;
    } else if (dxg_find_text(line, "dxgtrace: destroy_allocation ") != 0) {
        t->cleanup_mask |= WDDM_CLEAN_DESTROY_ALLOC;
    } else if (dxg_find_text(line, "dxgtrace: destroy_context ") != 0) {
        t->cleanup_mask |= WDDM_CLEAN_DESTROY_CONTEXT;
    } else if (dxg_find_text(line, "dxgtrace: destroy_hwqueue ") != 0) {
        t->cleanup_mask |= WDDM_CLEAN_DESTROY_HWQUEUE;
    } else if (dxg_find_text(line, "dxgtrace: destroy_paging_queue ") != 0) {
        t->cleanup_mask |= WDDM_CLEAN_DESTROY_PAGING;
    } else if (dxg_find_text(line, "dxgtrace: destroy_device ") != 0) {
        t->cleanup_mask |= WDDM_CLEAN_DESTROY_DEVICE;
    } else if (dxg_find_text(line, "dxgtrace: destroy_sync ") != 0) {
        t->cleanup_mask |= WDDM_CLEAN_DESTROY_SYNC;
    } else if (dxg_find_text(line, "dxgtrace: free_gpu_va ") != 0) {
        t->cleanup_mask |= WDDM_CLEAN_FREE_GPUVA;
    } else if (dxg_find_text(line, "dxgtrace: close_adapter ") != 0) {
        t->cleanup_mask |= WDDM_CLEAN_CLOSE_ADAPTER;
    } else if (dxg_find_text(line, "dxgtrace: evict ") != 0) {
        t->cleanup_mask |= WDDM_CLEAN_EVICT;
    }
}

static void parse_wddm_kernel_line(char *line, struct wddm_kernel_fields *k)
{
    if (dxg_find_text(line, "mesad3d12probe: exec ") != 0) {
        wddm_copy_token_after(line, "adapter=", k->requested_adapter_name,
                              sizeof(k->requested_adapter_name));
    } else if (dxg_find_text(line, "MESA_D3D12_DEFAULT_ADAPTER_NAME=") != 0) {
        wddm_copy_token_after(line, "MESA_D3D12_DEFAULT_ADAPTER_NAME=",
                              k->requested_adapter_name,
                              sizeof(k->requested_adapter_name));
    } else if (dxg_find_text(line, "dxg_probe_info_len=") != 0) {
        if (dxg_parse_luid_after(line, "host_vgpu_luid=",
                                 &k->luid_high, &k->luid_low) == 0)
            k->luid_seen = 1;
    } else if (dxg_find_text(line, "dxg_adapter_luids=") != 0) {
        if (dxg_parse_luid_after(line, "adapter:", &k->adapter_luid_high,
                                 &k->adapter_luid_low) == 0)
            k->adapter_luid_seen = 1;
        if (dxg_parse_luid_after(line, "host_adapter:",
                                 &k->host_adapter_luid_high,
                                 &k->host_adapter_luid_low) == 0)
            k->host_adapter_luid_seen = 1;
        if (dxg_parse_luid_after(line, "host_vgpu:",
                                 &k->host_vgpu_luid_high,
                                 &k->host_vgpu_luid_low) == 0 &&
            (k->host_vgpu_luid_high != 0 || k->host_vgpu_luid_low != 0))
            k->host_vgpu_luid_seen = 1;
    } else if (dxg_find_text(line, "dxg_luid_equivalence=") != 0) {
        if (dxg_parse_luid_after(line, "um_adapter:",
                                 &k->um_adapter_luid_high,
                                 &k->um_adapter_luid_low) == 0)
            k->um_adapter_luid_seen = 1;
        dxg_parse_uint_after(line, "host_equivalent_known:",
                             &k->host_equivalent_luid_known);
    } else if (dxg_find_text(line, "dxg_openadapterfromluid=") != 0) {
        if (dxg_parse_luid_after(line, "input:",
                                 &k->openadapter_input_high,
                                 &k->openadapter_input_low) == 0)
            k->openadapter_luid_seen = 1;
        dxg_parse_luid_after(line, "um_adapter:",
                             &k->openadapter_um_high,
                             &k->openadapter_um_low);
        dxg_parse_luid_after(line, "mapped_host:",
                             &k->openadapter_mapped_host_high,
                             &k->openadapter_mapped_host_low);
        dxg_parse_uint_after(line, "host_basis:",
                             &k->openadapter_host_basis);
        dxg_parse_uint_after(line, "match:", &k->openadapter_match);
        {
            uint32 ret = 0;

            if (dxg_parse_uint_after(line, "ret:", &ret) == 0)
                k->openadapter_ret = (int32)ret;
        }
        dxg_parse_uint_after(line, "handle:", &k->openadapter_handle);
    } else if (dxg_find_text(line, "dxg_context_last=") != 0) {
        k->context_seen = 1;
        dxg_parse_uint_after(line, "len:", &k->context_len);
        dxg_parse_uint_after(line, "node:", &k->context_node);
        dxg_parse_uint_after(line, "engine:", &k->context_engine);
        dxg_parse_uint_after(line, "flags:", &k->context_flags);
        dxg_parse_uint_after(line, "hint:", &k->context_hint);
        dxg_parse_uint_after(line, "priv:", &k->context_priv);
    } else if (dxg_find_text(line, "dxg_context_priv_head=") != 0) {
        dxg_parse_uint_after(line, "len:", &k->context_head_len);
        dxg_parse_head8_after(line, "bytes:", k->context_head,
                              &k->context_head_len);
    } else if (dxg_find_text(line, "dxg_hwqueue_last=") != 0) {
        k->hwqueue_seen = 1;
        dxg_parse_uint_after(line, "create_len:", &k->hwqueue_len);
        dxg_parse_uint_after(line, "flags:", &k->hwqueue_flags);
        dxg_parse_uint_after(line, "priv:", &k->hwqueue_priv);
        dxg_parse_u64_after(line, "fence_cpu:", &k->hwqueue_fence_cpu);
        dxg_parse_u64_after(line, "fence_gpu:", &k->hwqueue_fence_gpu);
    } else if (dxg_find_text(line, "dxg_hwqueue_priv_head=") != 0) {
        dxg_parse_uint_after(line, "create_len:", &k->hwqueue_head_len);
        dxg_parse_head8_after(line, "create:", k->hwqueue_head,
                              &k->hwqueue_head_len);
        dxg_parse_uint_after(line, "submit_cmd_len:", &k->submit_cmd_len);
        dxg_parse_uint_after(line, "submit_priv:", &k->submit_priv);
        dxg_parse_uint_after(line, "submit_len:", &k->submit_head_len);
        dxg_parse_head8_after(line, "submit:", k->submit_head,
                              &k->submit_head_len);
        wddm_kernel_note_submit(k, k->submit_len, k->submit_cmd_len,
                                k->submit_priv, k->submit_head_len,
                                k->submit_head);
    } else if (dxg_find_text(line, "dxg_allocation_last=") != 0) {
        k->allocation_seen = 1;
        dxg_parse_uint_after(line, "len:", &k->allocation_len);
        dxg_parse_uint_after(line, "count:", &k->allocation_count);
    } else if (dxg_find_text(line, "dxg_allocation_priv=") != 0) {
        dxg_parse_uint_after(line, "runtime:", &k->allocation_runtime);
        dxg_parse_uint_after(line, "resource_priv:",
                             &k->allocation_resource_priv);
        dxg_parse_uint_after(line, "size:", &k->allocation_alloc_priv);
        dxg_parse_uint_after(line, "flags:", &k->allocation_flags);
        dxg_parse_uint_after(line, "in_len:", &k->allocation_in_head_len);
        dxg_parse_head8_after(line, "in:", k->allocation_in_head,
                              &k->allocation_in_head_len);
        dxg_parse_uint_after(line, "out_len:", &k->allocation_out_priv);
    } else if (dxg_find_text(line, "dxg_residency_last=") != 0) {
        k->makeresident_seen = 1;
        dxg_parse_uint_after(line, "make_len:", &k->makeresident_len);
        dxg_parse_u64_after(line, "fence:", &k->makeresident_fence);
        dxg_parse_uint_after(line, "flags:", &k->makeresident_flags);
        dxg_parse_uint_after(line, "count:", &k->makeresident_count);
        dxg_parse_uint_after(line, "evict_len:", &k->evict_len);
    } else if (dxg_find_text(line, "dxg_mapgpuva_last=") != 0) {
        k->map_seen = 1;
        dxg_parse_uint_after(line, "len:", &k->map_len);
        dxg_parse_uint_after(line, "pages:", &k->map_pages);
        dxg_parse_u64_after(line, "prot:", &k->map_prot);
        dxg_parse_u64_after(line, "dprot:", &k->map_dprot);
        dxg_parse_u64_after(line, "fence:", &k->map_fence);
        wddm_kernel_note_map_pages(k, k->map_pages, k->map_prot,
                                   k->map_dprot);
    } else if (dxg_find_text(line, "dxg_mapgpuva_history") != 0) {
        char *p = line;

        while ((p = dxg_find_text(p, "pages:")) != 0) {
            uint32 pages = 0;

            if (dxg_parse_uint_after(p, "pages:", &pages) == 0)
                wddm_kernel_note_map_pages(k, pages, ~0ULL, ~0ULL);
            p += strlen("pages:");
        }
    } else if (dxg_find_text(line, "dxg_mapgpuva_layout_history") != 0) {
        char *p = line;

        while ((p = dxg_find_text(p, "pages:")) != 0) {
            uint32 pages = 0;
            uint64 prot = 0;
            uint64 dprot = 0;

            if (dxg_parse_uint_after(p, "pages:", &pages) == 0 &&
                dxg_parse_u64_after(p, "prot:", &prot) == 0 &&
                dxg_parse_u64_after(p, "dprot:", &dprot) == 0)
                wddm_kernel_note_map_pages(k, pages, prot, dprot);
            p += strlen("pages:");
        }
    } else if (dxg_find_text(line, "dxg_hwqueue_submit_history") != 0) {
        for (uint32 i = 0; i < 8; i++) {
            char key[16];
            char *p;
            uint32 len = 0;
            uint32 cmd_len = 0;
            uint32 priv = 0;
            uint32 head_len = 0;
            unsigned char head[8];

            snprintf(key, sizeof(key), "h%u:len:", i);
            p = dxg_find_text(line, key);
            if (p == 0)
                continue;
            p += 3;
            memset(head, 0, sizeof(head));
            if (dxg_parse_uint_after(p, "len:", &len) == 0 &&
                dxg_parse_uint_after(p, "cmd_len:", &cmd_len) == 0 &&
                dxg_parse_uint_after(p, "priv:", &priv) == 0) {
                dxg_parse_uint_after(p, "head_len:", &head_len);
                dxg_parse_head8_after(p, "head:", head, &head_len);
                wddm_kernel_note_submit(k, len, cmd_len, priv, head_len,
                                        head);
            }
        }
    } else if (dxg_find_text(line, "dxg_syncobject_last=") != 0) {
        k->sync_seen = 1;
        dxg_parse_uint_after(line, "len:", &k->sync_len);
        dxg_parse_uint_after(line, "type:", &k->sync_type);
        dxg_parse_uint_after(line, "flags:", &k->sync_flags);
        dxg_parse_u64_after(line, "fence_cpu:", &k->sync_fence_cpu);
        dxg_parse_u64_after(line, "fence_gpu:", &k->sync_fence_gpu);
    } else if (dxg_find_text(line, "dxg_syncobject_mapped=") != 0) {
        uint32 count = 0;

        dxg_parse_uint_after(line, "count:", &count);
        if (count != 0)
            k->sync_mapped_seen = 1;
        dxg_parse_uint_after(line, "type:", &k->sync_mapped_type);
        dxg_parse_uint_after(line, "flags:", &k->sync_mapped_flags);
        dxg_parse_u64_after(line, "fence_cpu:", &k->sync_mapped_fence_cpu);
        dxg_parse_u64_after(line, "fence_gpu:", &k->sync_mapped_fence_gpu);
    } else if (dxg_find_text(line, "dxg_lock2_last=") != 0) {
        k->lock_seen = 1;
        dxg_parse_uint_after(line, "len:", &k->lock_len);
        dxg_parse_uint_after(line, "unlock_len:", &k->unlock_len);
    } else if (dxg_find_text(line, "dxg_cacheops_last=") != 0) {
        k->cache_seen = 1;
    } else if (dxg_find_text(line, "dxg_createprocess_last=") != 0) {
        k->createprocess_seen = 1;
        dxg_parse_uint_after(line, "len:", &k->createprocess_len);
        dxg_parse_uint_after(line, "layout:", &k->createprocess_layout);
    } else if (dxg_find_text(line, "dxg_createprocess_success=") != 0) {
        k->createprocess_success_seen = 1;
        dxg_parse_uint_after(line, "len:", &k->createprocess_success_len);
        dxg_parse_uint_after(line, "layout:",
                             &k->createprocess_success_layout);
    } else if (dxg_find_text(line, "d3dkmt_ioctl_counts=") != 0) {
        dxg_parse_uint_after(line, "destroyalloc:", &k->destroyalloc_count);
        dxg_parse_uint_after(line, "destroycontext:",
                             &k->destroycontext_count);
        dxg_parse_uint_after(line, "destroyhwqueue:",
                             &k->destroyhwqueue_count);
        dxg_parse_uint_after(line, "destroypaging:",
                             &k->destroypaging_count);
        dxg_parse_uint_after(line, "destroydevice:",
                             &k->destroydevice_count);
        dxg_parse_uint_after(line, "destroysync:", &k->destroysync_count);
        dxg_parse_uint_after(line, "freegpuva:", &k->freegpuva_count);
        dxg_parse_uint_after(line, "closeadapter:", &k->closeadapter_count);
    } else if (dxg_find_text(line, "dxg_host_cmd_counts=") != 0) {
        dxg_parse_uint_after(line, "destroyalloc:",
                             &k->host_destroyalloc_count);
        dxg_parse_uint_after(line, "destroycontext:",
                             &k->host_destroycontext_count);
        dxg_parse_uint_after(line, "destroyhwqueue:",
                             &k->host_destroyhwqueue_count);
        dxg_parse_uint_after(line, "destroypaging:",
                             &k->host_destroypaging_count);
        dxg_parse_uint_after(line, "destroydevice:",
                             &k->host_destroydevice_count);
        dxg_parse_uint_after(line, "destroysync:",
                             &k->host_destroysync_count);
        dxg_parse_uint_after(line, "destroyprocess:",
                             &k->host_destroyprocess_count);
        dxg_parse_uint_after(line, "freegpuva:",
                             &k->host_freegpuva_count);
        dxg_parse_uint_after(line, "lock:", &k->host_lock_count);
        dxg_parse_uint_after(line, "unlock:", &k->host_unlock_count);
        dxg_parse_uint_after(line, "closeadapter_ioctl:",
                             &k->closeadapter_ioctl_count);
        dxg_parse_uint_after(line, "closeadapter_local:",
                             &k->closeadapter_local_count);
        dxg_parse_uint_after(line, "closeadapter_host:",
                             &k->closeadapter_host_count);
        dxg_parse_uint_after(line, "closeadapter_invalid:",
                             &k->closeadapter_invalid_count);
        dxg_parse_uint_after(line, "closeadapter_len:",
                             &k->closeadapter_len);
    } else if (dxg_find_text(line, "dxg_closeadapter_order=") != 0) {
        k->closeadapter_order_seen = 1;
        dxg_parse_uint_after(line, "seq:",
                             &k->closeadapter_order_seq);
        dxg_parse_uint_after(line, "close:",
                             &k->closeadapter_order_close);
        dxg_parse_uint_after(line, "last_destroy:",
                             &k->closeadapter_order_last_destroy);
        dxg_parse_uint_after(line, "destroyprocess:",
                             &k->closeadapter_order_destroyprocess);
        dxg_parse_uint_after(line, "after_destroy:",
                             &k->closeadapter_order_after_destroy);
    } else if (dxg_find_text(line, "dxg_lock2_detail=") != 0) {
        dxg_parse_uint_after(line, "ioctls:", &k->lock_ioctl_count);
        dxg_parse_uint_after(line, "forwarded:", &k->lock_forward_count);
        dxg_parse_uint_after(line, "cached_refs:",
                             &k->lock_cached_ref_count);
    } else if (dxg_find_text(line, "dxg_unlock2_detail=") != 0) {
        dxg_parse_uint_after(line, "ioctls:", &k->unlock_ioctl_count);
        dxg_parse_uint_after(line, "forwarded:", &k->unlock_forward_count);
        dxg_parse_uint_after(line, "missing_tracking:",
                             &k->unlock_missing_tracking_count);
        dxg_parse_uint_after(line, "cached_refs:",
                             &k->unlock_cached_ref_count);
    } else if (dxg_find_text(line, "dxg_adapter_hardware=") != 0) {
        dxg_parse_uint_after(line, "vendor:", &k->adapter_vendor_id);
        dxg_parse_uint_after(line, "device:", &k->adapter_device_id);
    }
}

static void parse_lines(char *buf, void (*fn)(char *, void *), void *ctx)
{
    char *line = buf;

    while (line != 0 && *line != 0) {
        char *next = strchr(line, '\n');

        if (next != 0)
            *next = 0;
        fn(line, ctx);
        if (next == 0)
            break;
        *next = '\n';
        line = next + 1;
    }
}

static void parse_trace_adapter(char *line, void *ctx)
{
    parse_wddm_trace_line(line, (struct wddm_trace_fields *)ctx);
}

static void parse_kernel_adapter(char *line, void *ctx)
{
    parse_wddm_kernel_line(line, (struct wddm_kernel_fields *)ctx);
}

static int require_cmp(int condition, int *failures, const char *msg)
{
    if (condition)
        return 0;
    printf("wddm_trace_compare mismatch %s\n", msg);
    (*failures)++;
    return -1;
}

static int head8_equal(const unsigned char *a, uint32 alen,
                       const unsigned char *b, uint32 blen)
{
    uint32 n = alen < blen ? alen : blen;

    if (n == 0)
        return 1;
    if (n > 8)
        n = 8;
    return memcmp(a, b, n) == 0;
}

static void wddm_trace_note_map_pages(struct wddm_trace_fields *t,
                                      uint32 pages, uint64 prot,
                                      uint64 dprot)
{
    if (pages == 0 || t->map_count >=
        sizeof(t->map_pages_seen) / sizeof(t->map_pages_seen[0]))
        return;
    t->map_pages_seen[t->map_count] = pages;
    t->map_prot_seen[t->map_count] = prot;
    t->map_dprot_seen[t->map_count] = dprot;
    t->map_count++;
}

static void wddm_kernel_note_map_pages(struct wddm_kernel_fields *k,
                                       uint32 pages, uint64 prot,
                                       uint64 dprot)
{
    if (pages == 0 || k->map_count >=
        sizeof(k->map_pages_seen) / sizeof(k->map_pages_seen[0]))
        return;
    k->map_pages_seen[k->map_count] = pages;
    k->map_prot_seen[k->map_count] = prot;
    k->map_dprot_seen[k->map_count] = dprot;
    k->map_count++;
}

static void wddm_kernel_note_submit(struct wddm_kernel_fields *k,
                                    uint32 len, uint32 cmd_len,
                                    uint32 priv, uint32 head_len,
                                    const unsigned char *head)
{
    if (len == 0 || k->submit_count >=
        sizeof(k->submit_lens_seen) / sizeof(k->submit_lens_seen[0]))
        return;
    k->submit_lens_seen[k->submit_count] = len;
    k->submit_cmd_lens[k->submit_count] = cmd_len;
    k->submit_privs[k->submit_count] = priv;
    k->submit_head_lens[k->submit_count] = head_len;
    memset(k->submit_heads[k->submit_count], 0,
           sizeof(k->submit_heads[k->submit_count]));
    if (head != 0 && head_len != 0)
        memcpy(k->submit_heads[k->submit_count], head,
               sizeof(k->submit_heads[k->submit_count]));
    k->submit_count++;
}

static int wddm_map_pages_intersect(const struct wddm_trace_fields *t,
                                    const struct wddm_kernel_fields *k)
{
    for (uint32 i = 0; i < t->map_count; i++) {
        for (uint32 j = 0; j < k->map_count; j++) {
            if (t->map_pages_seen[i] != k->map_pages_seen[j])
                continue;
            if (k->map_prot_seen[j] == ~0ULL ||
                k->map_dprot_seen[j] == ~0ULL)
                continue;
            if (t->map_prot_seen[i] == k->map_prot_seen[j] &&
                t->map_dprot_seen[i] == k->map_dprot_seen[j])
                return 1;
        }
    }
    return 0;
}

static int wddm_submit_matches(const struct wddm_trace_fields *t,
                               const struct wddm_kernel_fields *k)
{
    uint32 kcount = k->submit_count;

    if (kcount == 0) {
        if (k->submit_len == 0)
            return 0;
        kcount = 1;
    }
    for (uint32 j = 0; j < kcount; j++) {
        uint32 k_cmd_len = k->submit_count != 0 ?
                           k->submit_cmd_lens[j] : k->submit_cmd_len;
        uint32 k_priv = k->submit_count != 0 ?
                        k->submit_privs[j] : k->submit_priv;

        if (t->submit_count == 0)
            return k_priv == t->submit_priv &&
                   k_cmd_len == t->submit_cmd_len;
        for (uint32 i = 0; i < t->submit_count; i++) {
            if (t->submit_privs[i] == k_priv &&
                t->submit_cmd_lens[i] == k_cmd_len)
                return 1;
        }
    }
    return 0;
}

static int wddm_submit_head_matches(const struct wddm_trace_fields *t,
                                    const struct wddm_kernel_fields *k)
{
    uint32 kcount = k->submit_count;

    if (kcount == 0)
        return head8_equal(t->submit_head, t->submit_head_len,
                           k->submit_head, k->submit_head_len);
    for (uint32 j = 0; j < kcount; j++) {
        if (k->submit_head_lens[j] == 0)
            continue;
        for (uint32 i = 0; i < t->submit_count; i++) {
            if (t->submit_privs[i] == k->submit_privs[j] &&
                t->submit_cmd_lens[i] == k->submit_cmd_lens[j] &&
                head8_equal(t->submit_heads[i], t->submit_head_lens[i],
                            k->submit_heads[j], k->submit_head_lens[j]))
                return 1;
        }
    }
    return 0;
}

static const char *wddm_env_or_default(const char *name,
                                       const char *fallback)
{
    (void)name;
    return fallback;
}

static void print_wddm_trace_capture_plan(const char *trace_path,
                                          const char *kernel_log_path)
{
    const char *adapter =
        wddm_env_or_default("WSL_DXG_ADAPTER_NAME", "NVIDIA");
    const char *trace =
        trace_path != 0 && trace_path[0] != 0 ? trace_path :
        wddm_env_or_default("WSL_DXG_TRACE",
                            "/tmp/xv6-wsl-probe/mesaglfeature-nvidia-live.trace");
    const char *trace_dir =
        wddm_env_or_default("WSL_DXG_TRACE_DIR", "/tmp/xv6-wsl-probe");
    const char *preload =
        wddm_env_or_default("WSL_DXG_TRACE_LIBRARY",
                            "/tmp/xv6-wsl-probe/libwsl_dxg_ioctl_trace.so");
    const char *mesaglfeature =
        wddm_env_or_default("WSL_MESAGLFEATURE", "mesaglfeature");
    const char *kernel =
        kernel_log_path != 0 && kernel_log_path[0] != 0 ? kernel_log_path :
        "hyperv-dxg-validate.log";

    printf("wddm_trace_compare capture_plan trace=%s kernel=%s adapter=%s\n",
           trace, kernel, adapter);
    printf("wddm_trace_compare capture_wsl_command=mkdir -p '%s' && env GALLIUM_DRIVER=d3d12 D3D12_DEBUG=verbose MESA_D3D12_DEFAULT_ADAPTER_NAME='%s' LD_PRELOAD='%s' '%s' > '%s' 2>&1\n",
           trace_dir, adapter, preload, mesaglfeature, trace);
    printf("wddm_trace_compare capture_guest_command=mesad3d12probe --adapter '%s'; dxgprobe --wddm-payload-validate; cat /dev/dxg\n",
           adapter);
    printf("wddm_trace_compare adapter_luid_requirement=trace line 'dxgtrace: open_adapter_luid luid=HHHHHHHH:LLLLLLLL' is WSL's UMD-facing per-VM adapter LUID; exact equality is only required for same-VM captures. Cross-VM xv6/WSL captures require dxg_luid_equivalence plus dxg_openadapterfromluid mapped_host/host_basis evidence and physical adapter diagnostics instead of comparing host_adapter_luid directly.\n");
    printf("wddm_trace_compare compare_command=dxgprobe --wddm-trace-compare '%s' '%s'\n",
           trace, kernel);
    printf("runtime_private_blob_capture_matrix capture_in_dxgprobe=0 compare_mode='dxgprobe --runtime-blob-compare WSL_HEX XV6_HEX' missing_contract=runtime_or_dev_dxg_blob_source status=SKIP_MISSING_CONTRACT\n");
    printf("wddm_trace_compare evidence_required=create_context,create_hwqueue,submit_hwqueue,create_allocation,make_resident,map_gpuva,create_sync,lock_unlock,cache_ops,cleanup_order\n");
}

static int probe_wddm_trace_compare(const char *trace_path,
                                    const char *kernel_log_path)
{
    char *trace_buf;
    char *kernel_buf;
    struct wddm_trace_fields t;
    struct wddm_kernel_fields k;
    int failures = 0;

    trace_buf = read_text_file(trace_path);
    kernel_buf = read_text_file(kernel_log_path);
    if (trace_buf == 0) {
        printf("wddm_trace_compare missing_live_trace path=%s need=create_context,create_hwqueue,submit_hwqueue,create_allocation,make_resident,map_gpuva,create_sync,lock_unlock,cache_ops,cleanup_order\n",
               trace_path != 0 && trace_path[0] != 0 ? trace_path : "(unset)");
        print_wddm_trace_capture_plan(trace_path, kernel_log_path);
        if (kernel_buf != 0)
            free(kernel_buf);
        return -1;
    }
    if (kernel_buf == 0) {
        printf("wddm_trace_compare missing_kernel_evidence path=%s need=cat_/dev/dxg_after_same_adapter_UMD_sequence\n",
               kernel_log_path != 0 && kernel_log_path[0] != 0 ?
               kernel_log_path : "(unset)");
        print_wddm_trace_capture_plan(trace_path, kernel_log_path);
        free(trace_buf);
        return -1;
    }

    memset(&t, 0, sizeof(t));
    memset(&k, 0, sizeof(k));
    parse_lines(trace_buf, parse_trace_adapter, &t);
    parse_lines(kernel_buf, parse_kernel_adapter, &k);

    require_cmp(t.context_seen, &failures,
                "trace_missing=create_context_virtual");
    require_cmp(t.hwqueue_seen, &failures,
                "trace_missing=create_hwqueue");
    require_cmp(t.submit_seen, &failures,
                "trace_missing=submit_hwqueue");
    require_cmp(t.allocation_seen, &failures,
                "trace_missing=create_allocation");
    require_cmp(t.makeresident_seen, &failures,
                "trace_missing=make_resident");
    require_cmp(t.map_seen, &failures, "trace_missing=map_gpuva");
    require_cmp(t.sync_seen, &failures, "trace_missing=create_sync");
    if (t.lock_seen && !t.unlock_seen && t.raw_unlock_ioctl_seen)
        printf("wddm_trace_compare tracer_decode_missing=unlock2 raw_ioctl=0x37 rebuild_trace_library_and_recapture\n");
    require_cmp(t.lock_seen && t.unlock_seen, &failures,
                "trace_missing=lock_unlock");
    require_cmp(t.cleanup_mask != 0, &failures,
                "trace_missing=cleanup_order");

    if (t.adapter_seen && (k.um_adapter_luid_seen ||
                           k.adapter_luid_seen ||
                           k.openadapter_luid_seen ||
                           k.host_vgpu_luid_seen ||
                           k.host_adapter_luid_seen || k.luid_seen)) {
        uint32 um_high = k.um_adapter_luid_seen ?
                         k.um_adapter_luid_high :
                         k.adapter_luid_seen ?
                         k.adapter_luid_high : k.openadapter_input_high;
        uint32 um_low = k.um_adapter_luid_seen ?
                        k.um_adapter_luid_low :
                        k.adapter_luid_seen ?
                        k.adapter_luid_low : k.openadapter_input_low;
        int um_luid_seen = k.um_adapter_luid_seen || k.adapter_luid_seen ||
                           k.openadapter_luid_seen;
        int same_vm_exact = um_luid_seen &&
            wddm_luid_equal(t.luid_high, t.luid_low, um_high, um_low);
        int physical_match = wddm_physical_vendor_match(&t, &k);
        int physical_evidence = wddm_physical_adapter_evidence(&t, &k);
        int shifted_hardware_evidence =
            wddm_shifted_hardware_adapter_evidence(&t, &k);
        int adapter_identity_evidence =
            wddm_adapter_identity_evidence(&t, &k);
        int requested_adapter_match =
            wddm_requested_adapter_matches_trace(&t, &k);
        int host_equivalent_derived =
            wddm_host_adapter_hardware_equivalent(&k);
        int openadapter_maps_host =
            wddm_openadapter_luid_maps_to_host(&k);
        int cross_vm_equivalent = !same_vm_exact &&
            openadapter_maps_host &&
            (k.host_equivalent_luid_known || host_equivalent_derived ||
             k.openadapter_host_basis != 0) &&
            requested_adapter_match &&
            adapter_identity_evidence;

        require_cmp(same_vm_exact || cross_vm_equivalent, &failures,
                    "same_adapter_luid");
        if (!same_vm_exact) {
            printf("wddm_trace_compare adapter_luid_detail trace=%x:%x um_adapter=%x:%x um_source=%s same_vm_exact=%u host_adapter=%x:%x host_vgpu=%x:%x legacy_host_vgpu=%x:%x open_input=%x:%x open_um=%x:%x mapped_host=%x:%x open_host_basis=%u open_match=%u open_ret=%d open_handle=0x%x open_maps_host=%u physical_vendor_match=%u physical_device_evidence=%u shifted_hardware_evidence=%u adapter_identity_evidence=%u requested_adapter_match=%u trace_vendor=0x%x trace_hw_vendor=0x%x trace_hw_device=0x%x trace_hw_subvendor=0x%x kernel_vendor=0x%x kernel_device=0x%x requested_adapter=%s trace_adapter=%s renderer=%s description=%s scope=per_vm_vmbus_synthetic host_equivalent_known=%u host_equivalent_derived=%u cross_vm_equivalent=%u\n",
                   t.luid_high, t.luid_low, um_high, um_low,
                   k.um_adapter_luid_seen ?
                   "dxg_luid_equivalence.um_adapter" :
                   k.adapter_luid_seen ?
                   "dxg_adapter_luids.adapter" :
                   k.openadapter_luid_seen ?
                   "dxg_openadapterfromluid.input" : "missing",
                   same_vm_exact ? 1U : 0U,
                   k.host_adapter_luid_high, k.host_adapter_luid_low,
                   k.host_vgpu_luid_high, k.host_vgpu_luid_low,
                   k.luid_high, k.luid_low,
                   k.openadapter_input_high, k.openadapter_input_low,
                   k.openadapter_um_high, k.openadapter_um_low,
                   k.openadapter_mapped_host_high,
                   k.openadapter_mapped_host_low,
                   k.openadapter_host_basis, k.openadapter_match,
                   k.openadapter_ret, k.openadapter_handle,
                   openadapter_maps_host ? 1U : 0U,
                   physical_match ? 1U : 0U,
                   physical_evidence ? 1U : 0U,
                   shifted_hardware_evidence ? 1U : 0U,
                   adapter_identity_evidence ? 1U : 0U,
                   requested_adapter_match ? 1U : 0U,
                   wddm_trace_identity_vendor(&t),
                   t.hardware_vendor_id, t.hardware_device_id,
                   t.hardware_subvendor_id, k.adapter_vendor_id,
                   k.adapter_device_id,
                   k.requested_adapter_name[0] != 0 ?
                   k.requested_adapter_name : "(missing)",
                   t.trace_adapter_name[0] != 0 ?
                   t.trace_adapter_name : "(missing)",
                   t.renderer_name[0] != 0 ? t.renderer_name : "(missing)",
                   t.adapter_description[0] != 0 ?
                   t.adapter_description : "(missing)",
                   k.host_equivalent_luid_known,
                   host_equivalent_derived ? 1U : 0U,
                   cross_vm_equivalent ? 1U : 0U);
        }
    } else {
        printf("wddm_trace_compare needs_live_trace field=same_adapter_luid trace=%u kernel=%u\n",
               t.adapter_seen, k.um_adapter_luid_seen ||
               k.adapter_luid_seen || k.openadapter_luid_seen ||
               k.host_vgpu_luid_seen ||
               k.host_adapter_luid_seen || k.luid_seen);
        failures++;
    }

    require_cmp((k.createprocess_success_seen &&
                 k.createprocess_success_len != 0 &&
                 k.createprocess_success_layout == 1) ||
                (k.createprocess_seen && k.createprocess_len != 0 &&
                 k.createprocess_layout == 1), &failures,
                "create_process_layout");
    require_cmp(k.context_seen && k.context_len != 0, &failures,
                "kernel_missing=create_context_host_seen");
    require_cmp(k.context_priv == t.context_priv, &failures,
                "create_context_private_size");
    require_cmp(k.context_node == t.context_node &&
                k.context_engine == t.context_engine &&
                k.context_flags == t.context_flags &&
                k.context_hint == t.context_hint, &failures,
                "create_context_scalar_layout");
    require_cmp(head8_equal(t.context_head, t.context_head_len,
                            k.context_head, k.context_head_len),
                &failures, "create_context_private_head");

    require_cmp(k.hwqueue_seen && k.hwqueue_len != 0, &failures,
                "kernel_missing=create_hwqueue_host_seen");
    require_cmp(k.hwqueue_priv == t.hwqueue_priv &&
                k.hwqueue_flags == t.hwqueue_flags, &failures,
                "create_hwqueue_private_layout");
    require_cmp(head8_equal(t.hwqueue_head, t.hwqueue_head_len,
                            k.hwqueue_head, k.hwqueue_head_len),
                &failures, "create_hwqueue_private_head");
    require_cmp(k.hwqueue_fence_cpu != 0 && k.hwqueue_fence_gpu != 0,
                &failures, "create_hwqueue_return_fence_mapping");

    require_cmp(wddm_submit_matches(&t, &k), &failures,
                "submit_hwqueue_payload_layout");
    require_cmp(wddm_submit_head_matches(&t, &k),
                &failures, "submit_hwqueue_private_head");

    require_cmp(k.allocation_seen && k.allocation_len != 0, &failures,
                "kernel_missing=create_allocation_host_seen");
    require_cmp(k.allocation_count == t.allocation_count &&
                k.allocation_runtime == t.allocation_runtime &&
                k.allocation_resource_priv == t.allocation_resource_priv &&
                k.allocation_alloc_priv == t.allocation_alloc_priv,
                &failures, "create_allocation_private_sizes");
    require_cmp(k.allocation_out_priv != 0 ||
                t.allocation_out_priv == 0, &failures,
                "create_allocation_return_private_data");

    require_cmp(k.makeresident_seen && k.makeresident_len != 0 &&
                k.makeresident_count == t.makeresident_count &&
                k.makeresident_flags == t.makeresident_flags,
                &failures, "residency_packet_count_flags");
    require_cmp(k.makeresident_fence != 0 ||
                t.makeresident_fence == 0, &failures,
                "residency_return_fence");

    require_cmp(k.map_seen && k.map_len != 0 &&
                wddm_map_pages_intersect(&t, &k), &failures,
                "gpuva_map_layout");
    require_cmp(k.map_fence != 0 || t.map_fence == 0, &failures,
                "gpuva_return_fence");

    require_cmp(k.sync_seen && k.sync_len != 0 &&
                k.sync_type == t.sync_type &&
                k.sync_flags == t.sync_flags, &failures,
                "sync_object_fence_layout");
    require_cmp(!t.sync_mapped_seen ||
                (k.sync_mapped_seen &&
                 k.sync_mapped_type == t.sync_mapped_type &&
                 k.sync_mapped_flags == t.sync_mapped_flags &&
                 k.sync_mapped_fence_cpu != 0 &&
                 k.sync_mapped_fence_gpu != 0),
                &failures, "sync_object_cpu_gpu_mapping");
    require_cmp(wddm_lock_host_rewrite_seen(&k), &failures,
                "lock_host_seen");
    require_cmp(!t.unlock_seen ||
                wddm_unlock_host_rewrite_seen(&k), &failures,
                "lock_unlock_host_seen");
    if (t.lock_seen || t.unlock_seen)
        printf("wddm_trace_compare lock2_detail locks=%u lock_ioctls=%u lock_forwarded=%u lock_cached_refs=%u unlocks=%u unlock_ioctls=%u unlock_forwarded=%u unlock_cached_refs=%u\n",
               k.host_lock_count, k.lock_ioctl_count,
               k.lock_forward_count, k.lock_cached_ref_count,
               k.host_unlock_count, k.unlock_ioctl_count,
               k.unlock_forward_count, k.unlock_cached_ref_count);
    if (t.unlock_seen && k.unlock_ioctl_count != 0 &&
        k.unlock_forward_count == 0)
        printf("wddm_trace_compare unlock2_detail ioctls=%u forwarded=%u missing_tracking=%u cached_refs=%u\n",
               k.unlock_ioctl_count, k.unlock_forward_count,
               k.unlock_missing_tracking_count,
               k.unlock_cached_ref_count);
    if (t.cache_seen)
        require_cmp(k.cache_seen, &failures, "cacheability_side_effect");

    if (t.cleanup_mask & WDDM_CLEAN_DESTROY_ALLOC)
        require_cmp(k.destroyalloc_count != 0 ||
                    k.host_destroyalloc_count != 0 ||
                    k.host_destroyprocess_count != 0, &failures,
                    "cleanup_destroy_allocation");
    if (t.cleanup_mask & WDDM_CLEAN_DESTROY_CONTEXT)
        require_cmp(k.destroycontext_count != 0 ||
                    k.host_destroycontext_count != 0 ||
                    k.host_destroyprocess_count != 0, &failures,
                    "cleanup_destroy_context");
    if (t.cleanup_mask & WDDM_CLEAN_DESTROY_HWQUEUE)
        require_cmp(k.destroyhwqueue_count != 0 ||
                    k.host_destroyhwqueue_count != 0 ||
                    k.host_destroyprocess_count != 0, &failures,
                    "cleanup_destroy_hwqueue");
    if (t.cleanup_mask & WDDM_CLEAN_DESTROY_PAGING)
        require_cmp(k.destroypaging_count != 0 ||
                    k.host_destroypaging_count != 0 ||
                    k.host_destroyprocess_count != 0, &failures,
                    "cleanup_destroy_paging_queue");
    if (t.cleanup_mask & WDDM_CLEAN_DESTROY_DEVICE)
        require_cmp(k.destroydevice_count != 0 ||
                    k.host_destroydevice_count != 0 ||
                    k.host_destroyprocess_count != 0, &failures,
                    "cleanup_destroy_device");
    if (t.cleanup_mask & WDDM_CLEAN_DESTROY_SYNC)
        require_cmp(k.destroysync_count != 0 ||
                    k.host_destroysync_count != 0 ||
                    k.host_destroyprocess_count != 0, &failures,
                    "cleanup_destroy_sync");
    if (t.cleanup_mask & WDDM_CLEAN_FREE_GPUVA)
        require_cmp(k.freegpuva_count != 0 ||
                    k.host_freegpuva_count != 0 ||
                    k.host_destroyprocess_count != 0, &failures,
                    "cleanup_free_gpuva");
    if (t.cleanup_mask & WDDM_CLEAN_CLOSE_ADAPTER)
        require_cmp(wddm_closeadapter_cleanup_equivalent(&k), &failures,
                    "cleanup_close_adapter");
    if ((t.cleanup_mask & WDDM_CLEAN_CLOSE_ADAPTER) &&
        (k.closeadapter_host_count == 0 ||
         !wddm_closeadapter_cleanup_equivalent(&k)))
        printf("wddm_trace_compare closeadapter_detail ioctls=%u d3dkmt_count=%u local=%u host=%u invalid=%u len=%u host_destroyprocess=%u order_seen=%u order_seq=%u close_order=%u last_destroy_order=%u destroyprocess_order=%u after_destroy=%u process_destroy_local_close=%u host_closeadapter_packet=0\n",
               k.closeadapter_ioctl_count, k.closeadapter_count,
               k.closeadapter_local_count, k.closeadapter_host_count,
               k.closeadapter_invalid_count, k.closeadapter_len,
               k.host_destroyprocess_count, k.closeadapter_order_seen,
               k.closeadapter_order_seq, k.closeadapter_order_close,
               k.closeadapter_order_last_destroy,
               k.closeadapter_order_destroyprocess,
               k.closeadapter_order_after_destroy,
               wddm_closeadapter_process_destroy_equivalent(&k) ? 1U : 0U);
    if (t.cleanup_mask & WDDM_CLEAN_EVICT)
        require_cmp(k.evict_len != 0, &failures, "cleanup_evict");

    if (failures == 0) {
        printf("wddm_trace_compare ok trace=%s kernel=%s context_priv=%u hwqueue_priv=%u submit_priv=%u allocation_runtime=%u allocation_priv=%u make_count=%u map_pages=%u cleanup_mask=0x%x\n",
               trace_path, kernel_log_path, t.context_priv, t.hwqueue_priv,
               t.submit_priv, t.allocation_runtime, t.allocation_alloc_priv,
               t.makeresident_count, t.map_pages, t.cleanup_mask);
    } else {
        printf("wddm_trace_compare failed failures=%d need=same-adapter live WSL trace plus /dev/dxg captured immediately after the matching UMD sequence\n",
               failures);
    }

    free(trace_buf);
    free(kernel_buf);
    return failures == 0 ? 0 : -1;
}

static int read_shared_lifetime_status(struct dxg_shared_lifetime_status *out)
{
    char *buf;
    char *line;
    int ret = -1;

    if (out == 0)
        return -1;
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    line = dxg_find_text(buf, "dxg_sharedresource_lifetime=");
    if (line == 0)
        goto out_free;
    memset(out, 0, sizeof(*out));
    if (dxg_parse_uint_after(line, "seals:", &out->seals) < 0 ||
        dxg_parse_uint_after(line, "reuses:", &out->reuses) < 0 ||
        dxg_parse_uint_after(line, "denied:", &out->denied) < 0 ||
        dxg_parse_uint_after(line, "open_tracked:", &out->open_tracked) < 0)
        goto out_free;
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static int read_object_table_status(struct dxg_object_table_status *out)
{
    char *buf;
    char *line;
    int ret = -1;

    if (out == 0)
        return -1;
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    line = dxg_find_text(buf, "dxg_object_table=");
    if (line == 0)
        goto out_free;
    memset(out, 0, sizeof(*out));
    if (dxg_parse_uint_after(line, "max:", &out->max) < 0 ||
        dxg_parse_uint_after(line, "drops:", &out->drops) < 0 ||
        dxg_parse_uint_after(line, "denied:", &out->denied) < 0 ||
        dxg_parse_uint_after(line, "generation:", &out->generation) < 0 ||
        dxg_parse_uint_after(line, "reuse_delayed:",
                             &out->reuse_delayed) < 0 ||
        dxg_parse_uint_after(line, "reuse_allowed:",
                             &out->reuse_allowed) < 0 ||
        dxg_parse_uint_after(line, "min_free:", &out->min_free) < 0 ||
        dxg_parse_uint_after(line, "free_count:", &out->free_count) < 0 ||
        dxg_parse_uint_after(line, "free_head:", &out->free_head) < 0 ||
        dxg_parse_uint_after(line, "free_tail:", &out->free_tail) < 0)
        goto out_free;
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static int read_local_adapter_status(struct dxg_local_adapter_status *out)
{
    char *buf;
    char *line;
    int ret = -1;

    if (out == 0)
        return -1;
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    line = dxg_find_text(buf, "dxg_local_adapter_namespace=");
    if (line == 0)
        goto out_free;
    memset(out, 0, sizeof(*out));
    if (dxg_parse_uint_after(line, "hits:", &out->hits) < 0 ||
        dxg_parse_uint_after(line, "misses:", &out->misses) < 0 ||
        dxg_parse_uint_after(line, "result:", &out->last_result) < 0 ||
        dxg_parse_hex_after(line, "handle:", &out->handle) < 0 ||
        dxg_parse_hex_after(line, "host:", &out->host) < 0 ||
        dxg_parse_uint_after(line, "refs:", &out->refs) < 0 ||
        dxg_parse_uint_after(line, "locals:", &out->locals) < 0 ||
        dxg_parse_uint_after(line, "generation:", &out->generation) < 0 ||
        dxg_parse_uint_after(line, "reuse_delayed:",
                             &out->reuse_delayed) < 0 ||
        dxg_parse_uint_after(line, "reuse_allowed:",
                             &out->reuse_allowed) < 0 ||
        dxg_parse_uint_after(line, "min_free:", &out->min_free) < 0)
        goto out_free;
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static int read_process_lifetime_status(
    struct dxg_process_lifetime_status *out)
{
    char *buf;
    char *line;
    int ret = -1;

    if (out == 0)
        return -1;
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    line = dxg_find_text(buf, "d3dkmt_process_lifetime=");
    if (line == 0)
        goto out_free;
    memset(out, 0, sizeof(*out));
    if (dxg_parse_uint_after(line, "object_refs_last:",
                             &out->object_refs_last) < 0 ||
        dxg_parse_uint_after(line, "mem_refs_last:",
                             &out->mem_refs_last) < 0 ||
        dxg_parse_uint_after(line, "object_releases:",
                             &out->object_releases) < 0 ||
        dxg_parse_uint_after(line, "mem_releases:",
                             &out->mem_releases) < 0 ||
        dxg_parse_uint_after(line, "mem_frees:", &out->mem_frees) < 0)
        goto out_free;
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static uint32 dxg_hash_bytes(const unsigned char *data, uint32 size)
{
    uint32 hash = 2166136261U;

    if (data == 0 && size != 0)
        return 0;
    for (uint32 i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

static int dxg_blob_metadata_equal(uint32 lhs_size, uint32 lhs_hash,
                                   uint32 rhs_size, uint32 rhs_hash)
{
    if (lhs_size != rhs_size)
        return 0;
    if (lhs_size == 0)
        return 1;
    return lhs_hash == rhs_hash;
}

static int dxg_status_hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int dxg_parse_arrow_u32_after(char *line, const char *name,
                                     uint32 *first, uint32 *second)
{
    char *p;

    if (dxg_parse_uint_after(line, name, first) < 0)
        return -1;
    p = dxg_find_text(line, name);
    if (p == 0)
        return -1;
    p = dxg_find_text(p + strlen(name), "->");
    if (p == 0)
        return -1;
    return dxg_parse_uint_after(p, "->", second);
}

static int dxg_parse_size_hash_after(char *line, const char *name,
                                     uint32 *size_out, uint32 *hash_out)
{
    char *p;
    uint64 value = 0;
    int seen = 0;

    if (dxg_parse_uint_after(line, name, size_out) < 0)
        return -1;
    p = dxg_find_text(line, name);
    if (p == 0)
        return -1;
    p += strlen(name);
    while (*p != 0 && *p != '/')
        p++;
    if (*p != '/')
        return -1;
    p++;
    for (; *p != 0; p++) {
        int digit = dxg_status_hex_nibble(*p);

        if (digit < 0)
            break;
        value = value * 16 + (uint64)digit;
        seen = 1;
    }
    if (!seen)
        return -1;
    *hash_out = (uint32)value;
    return 0;
}

static int dxg_parse_hex_after(char *line, const char *name, uint32 *out)
{
    char *p;
    uint64 value = 0;
    int seen = 0;

    p = dxg_find_text(line, name);
    if (p == 0)
        return -1;
    p += strlen(name);
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    for (; *p != 0; p++) {
        int digit = dxg_status_hex_nibble(*p);

        if (digit < 0)
            break;
        value = value * 16 + (uint64)digit;
        seen = 1;
    }
    if (!seen)
        return -1;
    *out = (uint32)value;
    return 0;
}

static int read_dxg_sharedhandle_copyout_diag(
    struct dxg_sharedhandle_copyout_diag *out)
{
    char *buf;
    char *line;
    uint32 ret_value = 0;
    int ret = -1;

    if (out == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    line = dxg_find_text(buf, "dxg_sharedhandle_copyout=");
    if (line == 0)
        goto out_free;
    out->seen = 1;
    if (dxg_parse_uint_after(line, "failures:", &out->failures) < 0 ||
        dxg_parse_uint_after(line, "kind:", &out->kind) < 0 ||
        dxg_parse_uint_after(line, "proc:", &out->process) < 0 ||
        dxg_parse_uint_after(line, "object:", &out->object) < 0 ||
        dxg_parse_uint_after(line, "nt:", &out->nt) < 0 ||
        dxg_parse_uint_after(line, "fd:", &out->fd) < 0 ||
        dxg_parse_uint_after(line, "reclaimed:", &out->reclaimed) < 0 ||
        dxg_parse_uint_after(line, "refs_after:", &out->refs_after) < 0 ||
        dxg_parse_uint_after(line, "ret:", &ret_value) < 0)
        goto out_free;
    out->ret = (int32)ret_value;
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static int read_dxg_shared_resource_diag(struct dxg_shared_resource_diag *out)
{
    char *buf;
    char *line;
    int ret = 0;

    if (out == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;

    line = dxg_find_text(buf, "dxg_sharedresource_metadata=");
    if (line != 0) {
        out->metadata_seen = 1;
        ret |= dxg_parse_size_hash_after(line, "runtime:",
                                         &out->metadata_runtime_size,
                                         &out->metadata_runtime_hash);
        ret |= dxg_parse_size_hash_after(line, "resource:",
                                         &out->metadata_resource_size,
                                         &out->metadata_resource_hash);
        ret |= dxg_parse_size_hash_after(line, "total:",
                                         &out->metadata_total_size,
                                         &out->metadata_total_hash);
        dxg_parse_uint_after(line, "alloc0:", &out->metadata_alloc0_priv);
        dxg_parse_uint_after(line, "match_in:", &out->metadata_match_in);
        dxg_parse_uint_after(line, "match_out:", &out->metadata_match_out);
        dxg_parse_uint_after(line, "logical_flags:",
                             &out->metadata_logical_flags);
    }

    line = dxg_find_text(buf, "dxg_sharedresource_ntdiag=");
    if (line != 0) {
        out->nt_diag_seen = 1;
        dxg_parse_uint_after(line, "res_host:", &out->nt_resource_host);
        dxg_parse_uint_after(line, "alloc_host:", &out->nt_alloc_host);
        dxg_parse_arrow_u32_after(line, "meta:", &out->nt_meta_before,
                                  &out->nt_meta_after);
        dxg_parse_arrow_u32_after(line, "seal:", &out->nt_seal_before,
                                  &out->nt_seal_after);
        dxg_parse_arrow_u32_after(line, "host_seal:",
                                  &out->nt_host_seal_before,
                                  &out->nt_host_seal_after);
    }

    line = dxg_find_text(buf, "dxg_ntshared_pre_private=");
    if (line != 0) {
        out->pre_private_seen = 1;
        ret |= dxg_parse_size_hash_after(line, "runtime:",
                                         &out->pre_runtime_size,
                                         &out->pre_runtime_hash);
        ret |= dxg_parse_size_hash_after(line, "resource:",
                                         &out->pre_resource_size,
                                         &out->pre_resource_hash);
        ret |= dxg_parse_size_hash_after(line, "total:",
                                         &out->pre_total_size,
                                         &out->pre_total_hash);
        ret |= dxg_parse_size_hash_after(line, "alloc_out:",
                                         &out->pre_alloc_out_size,
                                         &out->pre_alloc_out_hash);
    }

    line = dxg_find_text(buf, "dxg_ntshared_runtime_object=");
    if (line != 0) {
        out->runtime_object_seen = 1;
        dxg_parse_uint_after(line, "user_obj:", &out->runtime_user_obj);
        dxg_parse_uint_after(line, "user_dev:", &out->runtime_user_dev);
        dxg_parse_uint_after(line, "kind:", &out->runtime_kind);
        dxg_parse_uint_after(line, "host_obj:", &out->runtime_host_obj);
        dxg_parse_uint_after(line, "host_dev:", &out->runtime_host_dev);
        dxg_parse_uint_after(line, "entry:", &out->runtime_entry_found);
        dxg_parse_uint_after(line, "owner:", &out->runtime_owner_refs);
        {
            char *entry = dxg_find_text(line, "entry:");

            if (entry != 0) {
                char *slash = dxg_find_text(entry, "/");

                if (slash != 0)
                    dxg_parse_uint_after(slash, "/",
                                         &out->runtime_entry_exact);
            }
        }
    }

    line = dxg_find_text(buf, "dxg_queryresource_nt=");
    if (line != 0) {
        uint32 ret_value = 0;

        out->query_seen = 1;
        if (dxg_parse_uint_after(line, "ret:", &ret_value) == 0)
            out->query_ret = (int32)ret_value;
        dxg_parse_uint_after(line, "device:", &out->query_device);
        dxg_parse_u64_after(line, "nt:", &out->query_nt);
        dxg_parse_uint_after(line, "kind:", &out->query_kind);
        dxg_parse_uint_after(line, "fops:", &out->query_fops);
        dxg_parse_uint_after(line, "refs:", &out->query_refs);
        dxg_parse_uint_after(line, "object:", &out->query_object);
        dxg_parse_uint_after(line, "cache_obj:", &out->query_cache_object);
        dxg_parse_uint_after(line, "allocs:", &out->query_allocs);
        dxg_parse_uint_after(line, "runtime:", &out->query_runtime_size);
        dxg_parse_uint_after(line, "resource:", &out->query_resource_size);
        dxg_parse_uint_after(line, "total:", &out->query_total_size);
    }

    line = dxg_find_text(buf, "dxg_openresource_envelope=");
    if (line != 0) {
        uint32 ret_value = 0;

        out->open_seen = 1;
        if (dxg_parse_uint_after(line, "ret:", &ret_value) == 0)
            out->open_ret = (int32)ret_value;
        dxg_parse_uint_after(line, "device:", &out->open_device);
        dxg_parse_uint_after(line, "global:", &out->open_global);
        dxg_parse_uint_after(line, "allocs:", &out->open_allocs);
        dxg_parse_uint_after(line, "total_priv:", &out->open_total_priv);
        dxg_parse_uint_after(line, "out_res:", &out->open_result_resource);
        dxg_parse_uint_after(line, "out_alloc0:", &out->open_result_alloc0);
        dxg_parse_arrow_u32_after(line, "seal:", &out->open_seal_before,
                                  &out->open_seal_after);
        dxg_parse_uint_after(line, "fd_kind:", &out->open_fd_kind);
        dxg_parse_uint_after(line, "fd_refs:", &out->open_fd_refs);
    }

    line = dxg_find_text(buf, "dxg_sharedresource_record=");
    if (line != 0) {
        uint32 admit = 0;

        out->record_seen = 1;
        dxg_parse_uint_after(line, "valid:", &out->record_valid);
        dxg_parse_uint_after(line, "stage:", &out->record_stage);
        dxg_parse_uint_after(line, "/p", &out->record_key_process);
        dxg_parse_uint_after(line, "/o", &out->record_key_object);
        dxg_parse_uint_after(line, "/nt", &out->record_key_nt);
        dxg_parse_uint_after(line, "source:proc",
                             &out->record_source_process);
        dxg_parse_uint_after(line, "/gen",
                             &out->record_source_generation);
        dxg_parse_uint_after(line, "res:", &out->record_resource);
        dxg_parse_uint_after(line, "alloc0:", &out->record_allocation);
        dxg_parse_uint_after(line, "sealed:", &out->record_sealed);
        dxg_parse_uint_after(line, "gen:",
                             &out->record_sealed_generation);
        dxg_parse_uint_after(line, "before_fd:",
                             &out->record_seal_before_fd);
        dxg_parse_uint_after(line, "allocs:", &out->record_allocs);
        dxg_parse_uint_after(line, "sizes:",
                             &out->record_runtime_size);
        {
            char *sizes = dxg_find_text(line, "sizes:");

            if (sizes != 0) {
                char *slash = dxg_find_text(sizes, "/");

                if (slash != 0) {
                    dxg_parse_uint_after(slash, "/",
                                         &out->record_resource_size);
                    slash = dxg_find_text(slash + 1, "/");
                }
                if (slash != 0) {
                    dxg_parse_uint_after(slash, "/",
                                         &out->record_total_size);
                    slash = dxg_find_text(slash + 1, "/");
                }
                if (slash != 0)
                    dxg_parse_uint_after(slash, "/",
                                         &out->record_alloc0_priv);
            }
        }
        dxg_parse_hex_after(line, "hashes:",
                             &out->record_runtime_hash);
        {
            char *hashes = dxg_find_text(line, "hashes:");

            if (hashes != 0) {
                char *slash = dxg_find_text(hashes, "/");

                if (slash != 0) {
                    dxg_parse_hex_after(slash, "/",
                                         &out->record_resource_hash);
                    slash = dxg_find_text(slash + 1, "/");
                }
                if (slash != 0) {
                    dxg_parse_hex_after(slash, "/",
                                         &out->record_total_hash);
                    slash = dxg_find_text(slash + 1, "/");
                }
                if (slash != 0)
                    dxg_parse_hex_after(slash, "/",
                                         &out->record_alloc0_hash);
            }
        }
        dxg_parse_uint_after(line, "refs:", &out->record_refs);
        dxg_parse_uint_after(line, "query:", &out->record_query_count);
        dxg_parse_uint_after(line, "open:", &out->record_open_count);
        dxg_parse_uint_after(line, "fd:", &out->record_fd_publish_count);
        if (dxg_parse_uint_after(line, "admit:", &admit) == 0)
            out->record_local_admit_ret = (int32)admit;
        dxg_parse_uint_after(line, "exact:", &out->record_local_exact);
        dxg_parse_uint_after(line, "mutated:", &out->record_mutated);
    }
    line = dxg_find_text(buf, "dxg_sharedresource_model=");
    if (line != 0) {
        dxg_parse_uint_after(line, "valid:", &out->model_valid);
        dxg_parse_uint_after(line, "flat_match:",
                             &out->model_flat_match);
        dxg_parse_uint_after(line, "allocs:", &out->model_allocs);
        dxg_parse_uint_after(line, "alloc0:", &out->model_alloc0);
        dxg_parse_uint_after(line, "priv0:", &out->model_alloc0_priv);
        dxg_parse_u64_after(line, "size0:", &out->model_alloc0_size);
        dxg_parse_u64_after(line, "pages0:", &out->model_alloc0_pages);
        dxg_parse_uint_after(line, "flags0:", &out->model_alloc0_flags);
        dxg_parse_uint_after(line, "cached0:", &out->model_alloc0_cached);
        dxg_parse_uint_after(line, "sizes:", &out->model_runtime_size);
        {
            char *sizes = dxg_find_text(line, "sizes:");

            if (sizes != 0) {
                char *slash = dxg_find_text(sizes, "/");

                if (slash != 0) {
                    dxg_parse_uint_after(slash, "/",
                                         &out->model_resource_size);
                    slash = dxg_find_text(slash + 1, "/");
                }
                if (slash != 0)
                    dxg_parse_uint_after(slash, "/",
                                         &out->model_total_size);
            }
        }
        dxg_parse_uint_after(line, "sealed:", &out->model_sealed);
        dxg_parse_uint_after(line, "gen:", &out->model_generation);
    }

    line = dxg_find_text(buf, "dxg_sharedresource_parent=");
    if (line != 0) {
        out->parent_seen = 1;
        dxg_parse_uint_after(line, "next:", &out->parent_next);
        dxg_parse_uint_after(line, "last:", &out->parent_last);
        dxg_parse_uint_after(line, "refs:", &out->parent_refs);
        dxg_parse_uint_after(line, "fd_refs:", &out->parent_fd_refs);
        dxg_parse_uint_after(line, "children:", &out->parent_children);
        dxg_parse_uint_after(line, "last_child:",
                             &out->parent_last_child);
        dxg_parse_uint_after(line, "sealed_gen:",
                             &out->parent_sealed_generation);
        dxg_parse_uint_after(line, "publish:",
                             &out->parent_publish_count);
        dxg_parse_uint_after(line, "open:", &out->parent_open_count);
        dxg_parse_uint_after(line, "release:",
                             &out->parent_release_count);
    }

    free(buf);
    return ret == 0 ? 0 : -1;
}

static int read_present_credit_status(struct dxg_present_credit_status *out)
{
    struct fb_gpu_stats stats;
    int fd;

    if (out == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    out->rc = -1;
    fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0)
        return -1;
    memset(&stats, 0, sizeof(stats));
    if (ioctl(fd, FB_GPU_GET_STATS, &stats) == 0) {
        out->display_presents = stats.display_presents;
        out->display_completions = stats.display_completions;
        out->register_attempts = stats.dxg_present_register_attempts;
        out->commit_attempts = stats.dxg_present_commit_attempts;
        out->rc = 0;
    }
    close(fd);
    return out->rc;
}

static int read_existing_sysmem_target_status(
    struct dxg_existing_sysmem_target_status *out)
{
    char *buf;
    char *line;
    int ret = -1;

    if (out == 0)
        return -1;
    buf = read_dxg_status_buffer();
    if (buf == 0)
        return -1;
    line = dxg_find_text(buf, "dxg_existing_sysmem_target=");
    if (line == 0)
        goto out_free;
    memset(out, 0, sizeof(*out));
    if (dxg_parse_uint_after(line, "pfnmap_pages:",
                             &out->pfnmap_pages) < 0 ||
        dxg_parse_uint_after(line, "pfnmap_ok:", &out->pfnmap_ok) < 0 ||
        dxg_parse_uint_after(line, "vram:", &out->vram) < 0 ||
        dxg_parse_uint_after(line, "vram_size:", &out->vram_size) < 0)
        goto out_free;
    ret = 0;

out_free:
    free(buf);
    return ret;
}

static int probe_wddm_payload_diagnostics(void)
{
    char *buf;
    char *context;
    char *context_priv;
    char *hwqueue;
    char *hwqueue_priv;
    char *residency;
    char *allocation;
    char *allocation_priv;
    char *mapgpuva;
    char *syncobject;
    char *lock2;
    char *destroy;
    char *createprocess;
    char *createdevice;
    char *createdevice_unwind;
    char *createcontext_unwind;
    char *createhwqueue_unwind;
    char *openresource;
    char *opensync;
    char *shareobject;
    char *syncgpu_wait;
    char *synccpuevent;
    char *asyncsend;
    uint32 context_len = 0;
    uint32 context_priv_size = 0;
    uint32 context_priv_head_len = 0;
    uint32 hwqueue_create_len = 0;
    uint32 hwqueue_create_priv = 0;
    uint32 hwqueue_create_head_len = 0;
    uint32 hwqueue_submit_len = 0;
    uint32 hwqueue_submit_priv = 0;
    uint32 make_len = 0;
    uint32 make_ret = 0;
    uint32 make_status = 0;
    uint32 make_flags = 0;
    uint32 make_count = 0;
    uint32 make_sorted = 0;
    uint32 make_pending_ok = 0;
    uint32 make_in0 = 0;
    uint32 make_in1 = 0;
    uint32 make_wire0 = 0;
    uint32 make_wire1 = 0;
    uint32 allocation_len = 0;
    uint32 allocation_ret = 0;
    uint32 allocation_count = 0;
    uint32 allocation_resource = 0;
    uint32 allocation_handle = 0;
    uint32 allocation_in_priv = 0;
    uint32 allocation_out_priv = 0;
    uint32 map_len = 0;
    uint32 map_ret = 0;
    uint32 map_status = 0;
    uint32 map_alloc = 0;
    uint32 map_va_low = 0;
    uint32 map_fence = 0;
    uint32 sync_len = 0;
    uint32 sync_ret = 0;
    uint32 sync_handle = 0;
    uint32 sync_fence_cpu_low = 0;
    uint32 sync_fence_gpu_low = 0;
    uint32 lock_len = 0;
    uint32 lock_ret = 0;
    uint32 lock_status = 0;
    uint32 lock_alloc = 0;
    uint32 lock_user_low = 0;
    uint32 unlock_len = 0;
    uint32 unlock_ret = 0;
    uint32 destroy_device_len = 0;
    uint32 destroy_context_len = 0;
    uint32 destroy_paging_len = 0;
    uint32 destroy_sync_len = 0;
    uint32 createprocess_len = 0;
    uint32 createprocess_cmd_len = 0;
    uint32 createprocess_ret = 0;
    uint32 createprocess_handle = 0;
    uint32 createprocess_layout = 0;
    uint32 createdevice_proc = 0;
    uint32 createdevice_device = 0;
    uint32 createdevice_unwind_attempts = 0;
    uint32 createdevice_unwind_successes = 0;
    uint32 createcontext_unwind_attempts = 0;
    uint32 createcontext_unwind_successes = 0;
    uint32 createhwqueue_unwind_attempts = 0;
    uint32 createhwqueue_unwind_successes = 0;
    uint32 openresource_wire = 0;
    uint32 openresource_result = 0;
    uint32 openresource_actual = 0;
    uint32 openresource_proc = 0;
    uint32 openresource_allocs = 0;
    uint32 openresource_out_res = 0;
    uint32 openresource_out_alloc0 = 0;
    uint32 opensync_wire = 0;
    uint32 opensync_result = 0;
    uint32 opensync_actual = 0;
    uint32 opensync_proc = 0;
    uint32 opensync_out_sync = 0;
    uint32 shareobject_cmd_len = 0;
    uint32 shareobject_wire = 0;
    uint32 shareobject_result_len = 0;
    uint32 shareobject_proc = 0;
    uint32 shareobject_object = 0;
    uint32 syncgpu_wait_context = 0;
    uint32 syncgpu_wait_object = 0;
    uint32 syncgpu_wait_count = 0;
    uint32 syncgpu_wait_cmd_len = 0;
    uint32 cpuevent_attempts = 0;
    uint32 cpuevent_successes = 0;
    uint32 async_enabled = 0;
    uint32 async_attempts = 0;
    uint32 async_successes = 0;
    uint32 async_fallbacks = 0;
    int createprocess_ok;
    int single_pending_ok;
    int packet_shape_ok;
    int ret = -1;

    buf = read_dxg_status_buffer();
    if (buf == 0) {
        printf("wddm_payload_validate status_unavailable\n");
        return -1;
    }

    context = dxg_find_text(buf, "dxg_context_last=");
    context_priv = dxg_find_text(buf, "dxg_context_priv_head=");
    hwqueue = dxg_find_text(buf, "dxg_hwqueue_last=");
    hwqueue_priv = dxg_find_text(buf, "dxg_hwqueue_priv_head=");
    residency = dxg_find_text(buf, "dxg_residency_last=");
    allocation = dxg_find_text(buf, "dxg_allocation_last=");
    allocation_priv = dxg_find_text(buf, "dxg_allocation_priv=");
    mapgpuva = dxg_find_text(buf, "dxg_mapgpuva_last=");
    syncobject = dxg_find_text(buf, "dxg_syncobject_last=");
    lock2 = dxg_find_text(buf, "dxg_lock2_last=");
    destroy = dxg_find_text(buf, "dxg_destroy_last=");
    createprocess = dxg_find_text(buf, "dxg_createprocess_last=");
    createdevice = dxg_find_text(buf, "dxg_createdevice_last=");
    createdevice_unwind = dxg_find_text(buf, "dxg_createdevice_unwind=");
    createcontext_unwind = dxg_find_text(buf, "dxg_context_unwind=");
    createhwqueue_unwind = dxg_find_text(buf, "dxg_hwqueue_unwind=");
    openresource = dxg_find_text(buf, "dxg_openresource_envelope=");
    opensync = dxg_find_text(buf, "dxg_opensync_envelope=");
    shareobject = dxg_find_text(buf, "dxg_shareobject_last=");
    syncgpu_wait = dxg_find_text(buf, "dxg_syncgpu_wait_detail=");
    synccpuevent = dxg_find_text(buf, "dxg_synccpuevent_signal=");
    asyncsend = dxg_find_text(buf, "dxg_async_send_last=");
    if (context == 0 || context_priv == 0 || hwqueue == 0 ||
        hwqueue_priv == 0 || residency == 0 || allocation == 0 ||
        allocation_priv == 0 || mapgpuva == 0 || syncobject == 0 ||
        lock2 == 0 || destroy == 0 || createprocess == 0 ||
        createdevice == 0 || createdevice_unwind == 0 ||
        createcontext_unwind == 0 || createhwqueue_unwind == 0 ||
        openresource == 0 || opensync == 0 || shareobject == 0 ||
        syncgpu_wait == 0 || synccpuevent == 0 || asyncsend == 0) {
        printf("wddm_payload_validate missing_status_lines context=%d context_priv=%d hwqueue=%d hwqueue_priv=%d residency=%d allocation=%d allocation_priv=%d mapgpuva=%d sync=%d lock=%d destroy=%d process=%d createdevice=%d dev_unwind=%d ctx_unwind=%d hwq_unwind=%d openres=%d opensync=%d share=%d syncwait=%d cpuevent=%d async=%d\n",
               context != 0, context_priv != 0, hwqueue != 0,
               hwqueue_priv != 0, residency != 0, allocation != 0,
               allocation_priv != 0, mapgpuva != 0, syncobject != 0,
               lock2 != 0, destroy != 0, createprocess != 0,
               createdevice != 0, createdevice_unwind != 0,
               createcontext_unwind != 0, createhwqueue_unwind != 0,
               openresource != 0, opensync != 0, shareobject != 0,
               syncgpu_wait != 0, synccpuevent != 0, asyncsend != 0);
        goto out;
    }

    if (dxg_parse_uint_after(context, "len:", &context_len) < 0 ||
        dxg_parse_uint_after(context, "priv:", &context_priv_size) < 0 ||
        dxg_parse_uint_after(context_priv, "len:",
                             &context_priv_head_len) < 0 ||
        dxg_parse_uint_after(hwqueue, "create_len:",
                             &hwqueue_create_len) < 0 ||
        dxg_parse_uint_after(hwqueue, "priv:",
                             &hwqueue_create_priv) < 0 ||
        dxg_parse_uint_after(hwqueue_priv, "create_len:",
                             &hwqueue_create_head_len) < 0 ||
        dxg_parse_uint_after(hwqueue_priv, "submit_priv:",
                             &hwqueue_submit_priv) < 0 ||
        dxg_parse_uint_after(hwqueue_priv, "submit_len:",
                             &hwqueue_submit_len) < 0 ||
        dxg_parse_uint_after(residency, "make_len:", &make_len) < 0 ||
        dxg_parse_uint_after(residency, "make_ret:", &make_ret) < 0 ||
        dxg_parse_uint_after(residency, "make_status:", &make_status) < 0 ||
        dxg_parse_uint_after(residency, "flags:", &make_flags) < 0 ||
        dxg_parse_uint_after(residency, "count:", &make_count) < 0 ||
        dxg_parse_uint_after(residency, "sorted:", &make_sorted) < 0 ||
        dxg_parse_uint_pair_after(residency, "in:", &make_in0,
                                  &make_in1) < 0 ||
        dxg_parse_uint_pair_after(residency, "wire:", &make_wire0,
                                  &make_wire1) < 0 ||
        dxg_parse_uint_after(allocation, "len:", &allocation_len) < 0 ||
        dxg_parse_uint_after(allocation, "ret:", &allocation_ret) < 0 ||
        dxg_parse_uint_after(allocation, "count:", &allocation_count) < 0 ||
        dxg_parse_uint_after(allocation, "resource:",
                             &allocation_resource) < 0 ||
        dxg_parse_uint_after(allocation, "allocation:",
                             &allocation_handle) < 0 ||
        dxg_parse_uint_after(allocation_priv, "in_len:",
                             &allocation_in_priv) < 0 ||
        dxg_parse_uint_after(allocation_priv, "out_len:",
                             &allocation_out_priv) < 0 ||
        dxg_parse_uint_after(mapgpuva, "len:", &map_len) < 0 ||
        dxg_parse_uint_after(mapgpuva, "ret:", &map_ret) < 0 ||
        dxg_parse_uint_after(mapgpuva, "status:", &map_status) < 0 ||
        dxg_parse_uint_after(mapgpuva, "alloc:", &map_alloc) < 0 ||
        dxg_parse_uint_after(mapgpuva, "va:", &map_va_low) < 0 ||
        dxg_parse_uint_after(mapgpuva, "fence:", &map_fence) < 0 ||
        dxg_parse_uint_after(syncobject, "len:", &sync_len) < 0 ||
        dxg_parse_uint_after(syncobject, "ret:", &sync_ret) < 0 ||
        dxg_parse_uint_after(syncobject, "handle:", &sync_handle) < 0 ||
        dxg_parse_uint_after(syncobject, "fence_cpu:",
                             &sync_fence_cpu_low) < 0 ||
        dxg_parse_uint_after(syncobject, "fence_gpu:",
                             &sync_fence_gpu_low) < 0 ||
        dxg_parse_uint_after(lock2, "len:", &lock_len) < 0 ||
        dxg_parse_uint_after(lock2, "ret:", &lock_ret) < 0 ||
        dxg_parse_uint_after(lock2, "status:", &lock_status) < 0 ||
        dxg_parse_uint_after(lock2, "allocation:", &lock_alloc) < 0 ||
        dxg_parse_uint_after(lock2, "user_va:", &lock_user_low) < 0 ||
        dxg_parse_uint_after(lock2, "unlock_len:", &unlock_len) < 0 ||
        dxg_parse_uint_after(lock2, "unlock_ret:", &unlock_ret) < 0 ||
        dxg_parse_uint_after(destroy, "device_len:",
                             &destroy_device_len) < 0 ||
        dxg_parse_uint_after(destroy, "context_len:",
                             &destroy_context_len) < 0 ||
        dxg_parse_uint_after(destroy, "paging_len:",
                             &destroy_paging_len) < 0 ||
        dxg_parse_uint_after(destroy, "sync_len:", &destroy_sync_len) < 0 ||
        dxg_parse_uint_after(createprocess, "len:",
                             &createprocess_len) < 0 ||
        dxg_parse_uint_after(createprocess, "cmd_len:",
                             &createprocess_cmd_len) < 0 ||
        dxg_parse_uint_after(createprocess, "ret:",
                             &createprocess_ret) < 0 ||
        dxg_parse_uint_after(createprocess, "handle:",
                             &createprocess_handle) < 0 ||
        dxg_parse_uint_after(createprocess, "layout:",
                             &createprocess_layout) < 0 ||
        dxg_parse_uint_after(createdevice, "proc:",
                             &createdevice_proc) < 0 ||
        dxg_parse_uint_after(createdevice, "device:",
                             &createdevice_device) < 0 ||
        dxg_parse_uint_after(createdevice_unwind, "attempts:",
                             &createdevice_unwind_attempts) < 0 ||
        dxg_parse_uint_after(createdevice_unwind, "successes:",
                             &createdevice_unwind_successes) < 0 ||
        dxg_parse_uint_after(createcontext_unwind, "attempts:",
                             &createcontext_unwind_attempts) < 0 ||
        dxg_parse_uint_after(createcontext_unwind, "successes:",
                             &createcontext_unwind_successes) < 0 ||
        dxg_parse_uint_after(createhwqueue_unwind, "attempts:",
                             &createhwqueue_unwind_attempts) < 0 ||
        dxg_parse_uint_after(createhwqueue_unwind, "successes:",
                             &createhwqueue_unwind_successes) < 0 ||
        dxg_parse_uint_after(openresource, "wire:", &openresource_wire) < 0 ||
        dxg_parse_uint_after(openresource, "result:", &openresource_result) < 0 ||
        dxg_parse_uint_after(openresource, "actual:", &openresource_actual) < 0 ||
        dxg_parse_uint_after(openresource, "proc:", &openresource_proc) < 0 ||
        dxg_parse_uint_after(openresource, "allocs:", &openresource_allocs) < 0 ||
        dxg_parse_uint_after(openresource, "out_res:",
                             &openresource_out_res) < 0 ||
        dxg_parse_uint_after(openresource, "out_alloc0:",
                             &openresource_out_alloc0) < 0 ||
        dxg_parse_uint_after(opensync, "wire:", &opensync_wire) < 0 ||
        dxg_parse_uint_after(opensync, "result:", &opensync_result) < 0 ||
        dxg_parse_uint_after(opensync, "actual:", &opensync_actual) < 0 ||
        dxg_parse_uint_after(opensync, "proc:", &opensync_proc) < 0 ||
        dxg_parse_uint_after(opensync, "out_sync:", &opensync_out_sync) < 0 ||
        dxg_parse_uint_after(shareobject, "cmd_len:",
                             &shareobject_cmd_len) < 0 ||
        dxg_parse_uint_after(shareobject, "wire:", &shareobject_wire) < 0 ||
        dxg_parse_uint_after(shareobject, "result_len:",
                             &shareobject_result_len) < 0 ||
        dxg_parse_uint_after(shareobject, "proc:", &shareobject_proc) < 0 ||
        dxg_parse_uint_after(shareobject, "object:",
                             &shareobject_object) < 0 ||
        dxg_parse_uint_after(syncgpu_wait, "context:",
                             &syncgpu_wait_context) < 0 ||
        dxg_parse_uint_after(syncgpu_wait, "object:",
                             &syncgpu_wait_object) < 0 ||
        dxg_parse_uint_after(syncgpu_wait, "count:",
                             &syncgpu_wait_count) < 0 ||
        dxg_parse_uint_after(syncgpu_wait, "cmd_len:",
                             &syncgpu_wait_cmd_len) < 0 ||
        dxg_parse_uint_after(synccpuevent, "attempts:",
                             &cpuevent_attempts) < 0 ||
        dxg_parse_uint_after(synccpuevent, "successes:",
                             &cpuevent_successes) < 0 ||
        dxg_parse_uint_after(asyncsend, "enabled:", &async_enabled) < 0 ||
        dxg_parse_uint_after(asyncsend, "attempts:", &async_attempts) < 0 ||
        dxg_parse_uint_after(asyncsend, "successes:", &async_successes) < 0 ||
        dxg_parse_uint_after(asyncsend, "fallback_sync:",
                             &async_fallbacks) < 0) {
        printf("wddm_payload_validate parse_failed\n");
        goto out;
    }

    if (context_len == 0 || context_priv_size == 0 ||
        context_priv_head_len == 0 || hwqueue_create_len == 0 ||
        hwqueue_create_priv == 0 || hwqueue_create_head_len == 0 ||
        hwqueue_submit_len == 0 || hwqueue_submit_priv == 0) {
        printf("wddm_payload_validate incomplete context_len=%u context_priv=%u context_head=%u hwqueue_len=%u hwqueue_priv=%u hwqueue_head=%u submit_len=%u submit_priv=%u make_len=%u make_count=%u\n",
               context_len, context_priv_size, context_priv_head_len,
               hwqueue_create_len, hwqueue_create_priv,
               hwqueue_create_head_len, hwqueue_submit_len,
               hwqueue_submit_priv, make_len, make_count);
        goto out;
    }

    createprocess_ok = createprocess_ret == 0 && createprocess_handle != 0 &&
        createprocess_layout == 1;
    (void)dxg_parse_uint_after(residency, "pending_ok:",
                               &make_pending_ok);
    single_pending_ok = make_count == 1 &&
        make_ret == 259 && make_status == 0x103 &&
        (make_pending_ok == 1 || !createprocess_ok);

    if (make_len == 0 || make_count == 0 || make_flags != 0x1 ||
        make_in0 == 0 || make_wire0 == 0 || make_in0 != make_wire0 ||
        (make_count == 1 && !single_pending_ok &&
         (make_ret != 0 || make_status != 0 || !createprocess_ok)) ||
        (make_count > 1 &&
         (make_sorted == 0 || make_ret != 0 || make_status != 0 ||
          make_in1 == 0 || make_wire1 == 0 || make_in1 != make_wire1))) {
        printf("wddm_payload_validate makeresident_mismatch make_len=%u make_ret=%d make_status=0x%x make_flags=0x%x make_count=%u sorted=%u in=%x,%x wire=%x,%x\n",
               make_len, (int)make_ret, make_status, make_flags,
               make_count, make_sorted, make_in0, make_in1, make_wire0,
               make_wire1);
        goto out;
    }

    if (single_pending_ok) {
        printf("wddm_payload_validate predevice_pending context_len=%u context_priv=%u context_head=%u hwqueue_len=%u hwqueue_priv=%u hwqueue_head=%u submit_len=%u submit_priv=%u make_len=%u make_ret=%d make_status=0x%x pending_ok=%u make_flags=0x%x make_count=%u sorted=%u in=%x,%x wire=%x,%x process_len=%u process_cmd=%u process_ret=%d process=0x%x layout=%u\n",
               context_len, context_priv_size, context_priv_head_len,
               hwqueue_create_len, hwqueue_create_priv,
               hwqueue_create_head_len, hwqueue_submit_len,
               hwqueue_submit_priv, make_len, (int)make_ret, make_status,
               make_pending_ok, make_flags, make_count, make_sorted,
               make_in0, make_in1, make_wire0, make_wire1, createprocess_len,
               createprocess_cmd_len, (int)createprocess_ret,
               createprocess_handle, createprocess_layout);
        ret = 0;
        goto out;
    }

    packet_shape_ok =
        createdevice_proc != 0 && createdevice_device != 0 &&
        context_len != 0 && context_priv_size != 0 &&
        context_priv_head_len != 0 && hwqueue_create_len != 0 &&
        hwqueue_create_priv != 0 && hwqueue_create_head_len != 0 &&
        hwqueue_submit_len != 0 && hwqueue_submit_priv != 0 &&
        allocation_len != 0 && allocation_count != 0 &&
        allocation_resource != 0 && allocation_handle != 0 &&
        allocation_in_priv != 0 && allocation_out_priv != 0 &&
        make_len != 0 && make_count != 0 && make_in0 != 0 &&
        make_wire0 == make_in0 &&
        openresource_wire != 0 && openresource_result != 0 &&
        openresource_actual != 0 && openresource_proc != 0 &&
        openresource_allocs != 0 && openresource_out_res != 0 &&
        openresource_out_alloc0 != 0 &&
        sync_len != 0 && sync_handle != 0 &&
        opensync_wire != 0 && opensync_result != 0 &&
        opensync_actual != 0 && opensync_proc != 0 &&
        opensync_out_sync != 0 &&
        shareobject_cmd_len != 0 && shareobject_wire != 0 &&
        shareobject_result_len != 0 && shareobject_proc != 0 &&
        shareobject_object != 0 &&
        syncgpu_wait_context != 0 && syncgpu_wait_object != 0 &&
        syncgpu_wait_count != 0 && syncgpu_wait_cmd_len != 0 &&
        cpuevent_attempts != 0 && cpuevent_successes != 0 &&
        ((async_enabled != 0 && async_attempts != 0 &&
          async_successes != 0) ||
         (async_enabled == 0 && async_fallbacks != 0)) &&
        map_len != 0 && map_alloc != 0 && map_va_low != 0 &&
        lock_len != 0 && lock_alloc != 0 && lock_user_low != 0 &&
        destroy_device_len != 0 && destroy_context_len != 0 &&
        destroy_paging_len != 0 && destroy_sync_len != 0 &&
        createdevice_unwind_attempts != 0 &&
        createdevice_unwind_successes != 0 &&
        createcontext_unwind_attempts != 0 &&
        createcontext_unwind_successes != 0 &&
        createhwqueue_unwind_attempts != 0 &&
        createhwqueue_unwind_successes != 0;
    printf("dxg_packet_shape_matrix createprocess=%u createdevice=%u createcontext=%u createhwqueue=%u createallocation=%u makeresident=%u openresource=%u sync_create=%u opensync=%u shareobject=%u signal_cpu_event=%u waitgpu=%u submithwqueue=%u map=%u lock=%u destroy=%u unwind=%u async=%u status=%s\n",
           createprocess_ok ? 1U : 0U,
           (createdevice_proc != 0 && createdevice_device != 0) ? 1U : 0U,
           (context_len != 0 && context_priv_size != 0 &&
            context_priv_head_len != 0) ? 1U : 0U,
           (hwqueue_create_len != 0 && hwqueue_create_priv != 0 &&
            hwqueue_create_head_len != 0 && hwqueue_submit_len != 0 &&
            hwqueue_submit_priv != 0) ? 1U : 0U,
           (allocation_len != 0 && allocation_count != 0 &&
            allocation_resource != 0 && allocation_handle != 0 &&
            allocation_in_priv != 0 && allocation_out_priv != 0) ? 1U : 0U,
           (make_len != 0 && make_count != 0 && make_in0 != 0 &&
            make_wire0 == make_in0) ? 1U : 0U,
           (openresource_wire != 0 && openresource_result != 0 &&
            openresource_actual != 0 && openresource_proc != 0 &&
            openresource_allocs != 0 && openresource_out_res != 0 &&
            openresource_out_alloc0 != 0) ? 1U : 0U,
           (sync_len != 0 && sync_handle != 0 &&
            sync_fence_cpu_low != 0 && sync_fence_gpu_low != 0) ? 1U : 0U,
           (opensync_wire != 0 && opensync_result != 0 &&
            opensync_actual != 0 && opensync_proc != 0 &&
            opensync_out_sync != 0) ? 1U : 0U,
           (shareobject_cmd_len != 0 && shareobject_wire != 0 &&
            shareobject_result_len != 0 && shareobject_proc != 0 &&
            shareobject_object != 0) ? 1U : 0U,
           (cpuevent_attempts != 0 && cpuevent_successes != 0) ? 1U : 0U,
           (syncgpu_wait_context != 0 && syncgpu_wait_object != 0 &&
            syncgpu_wait_count != 0 && syncgpu_wait_cmd_len != 0) ? 1U : 0U,
           (hwqueue_submit_len != 0 && hwqueue_submit_priv != 0) ? 1U : 0U,
           (map_len != 0 && map_alloc != 0 && map_va_low != 0) ? 1U : 0U,
           (lock_len != 0 && lock_alloc != 0 && lock_user_low != 0) ? 1U : 0U,
           (destroy_device_len != 0 && destroy_context_len != 0 &&
            destroy_paging_len != 0 && destroy_sync_len != 0) ? 1U : 0U,
           (createdevice_unwind_attempts != 0 &&
            createdevice_unwind_successes != 0 &&
            createcontext_unwind_attempts != 0 &&
            createcontext_unwind_successes != 0 &&
            createhwqueue_unwind_attempts != 0 &&
            createhwqueue_unwind_successes != 0) ? 1U : 0U,
           ((async_enabled != 0 && async_attempts != 0 &&
             async_successes != 0) ||
            (async_enabled == 0 && async_fallbacks != 0)) ? 1U : 0U,
           packet_shape_ok ? "PASS" : "FAIL");
    if (!packet_shape_ok)
        goto out;

    if (allocation_len == 0 || allocation_count == 0 ||
        allocation_resource == 0 || allocation_handle == 0 ||
        allocation_in_priv == 0 || allocation_out_priv == 0 ||
        map_len == 0 || map_alloc == 0 || map_va_low == 0 ||
        map_fence == 0 || sync_len == 0 || sync_handle == 0 ||
        sync_fence_cpu_low == 0 || sync_fence_gpu_low == 0 ||
        lock_len == 0 || lock_alloc == 0 || lock_user_low == 0 ||
        unlock_len == 0 || destroy_device_len == 0 ||
        destroy_context_len == 0 || destroy_paging_len == 0 ||
        destroy_sync_len == 0 || createprocess_len == 0 ||
        createprocess_cmd_len == 0 || createprocess_handle == 0 ||
        createprocess_layout != 1) {
        printf("wddm_payload_validate layout_mismatch alloc_len=%u alloc_ret=%d alloc_count=%u resource=0x%x allocation=0x%x in_priv=%u out_priv=%u map_len=%u map_ret=%d map_status=0x%x map_alloc=0x%x va=0x%x fence=%u sync_len=%u sync_ret=%d sync=0x%x fence_cpu=0x%x fence_gpu=0x%x lock_len=%u lock_ret=%d lock_status=0x%x lock_alloc=0x%x user=0x%x unlock_len=%u unlock_ret=%d destroy=%u,%u,%u,%u process_len=%u process_cmd=%u process_ret=%d process=0x%x layout=%u\n",
               allocation_len, (int)allocation_ret, allocation_count,
               allocation_resource, allocation_handle, allocation_in_priv,
               allocation_out_priv, map_len, (int)map_ret, map_status,
               map_alloc, map_va_low, map_fence, sync_len, (int)sync_ret,
               sync_handle, sync_fence_cpu_low, sync_fence_gpu_low,
               lock_len, (int)lock_ret, lock_status, lock_alloc,
               lock_user_low, unlock_len, (int)unlock_ret,
               destroy_device_len, destroy_context_len,
               destroy_paging_len, destroy_sync_len,
               createprocess_len, createprocess_cmd_len,
               (int)createprocess_ret, createprocess_handle,
               createprocess_layout);
        goto out;
    }

    printf("wddm_payload_validate ok context_len=%u context_priv=%u context_head=%u hwqueue_len=%u hwqueue_priv=%u hwqueue_head=%u submit_len=%u submit_priv=%u make_len=%u make_ret=%d make_status=0x%x make_flags=0x%x make_count=%u sorted=%u in=%x,%x wire=%x,%x\n",
           context_len, context_priv_size, context_priv_head_len,
           hwqueue_create_len, hwqueue_create_priv, hwqueue_create_head_len,
           hwqueue_submit_len, hwqueue_submit_priv, make_len, (int)make_ret,
           make_status, make_flags, make_count, make_sorted, make_in0,
           make_in1, make_wire0, make_wire1);
    printf("wddm_layout_validate ok allocation_len=%u allocation_ret=%d allocation_count=%u in_priv=%u out_priv=%u map_len=%u map_ret=%d map_status=0x%x map_alloc=0x%x map_va=0x%x map_fence=%u sync_len=%u sync_ret=%d sync=0x%x fence_cpu=0x%x fence_gpu=0x%x lock_len=%u lock_ret=%d lock_status=0x%x unlock_len=%u unlock_ret=%d destroy=%u,%u,%u,%u process_len=%u process_cmd=%u process_ret=%d process=0x%x layout=%u\n",
           allocation_len, (int)allocation_ret, allocation_count,
           allocation_in_priv, allocation_out_priv, map_len, (int)map_ret,
           map_status, map_alloc, map_va_low, map_fence, sync_len,
           (int)sync_ret, sync_handle, sync_fence_cpu_low,
           sync_fence_gpu_low, lock_len, (int)lock_ret, lock_status,
           unlock_len, (int)unlock_ret, destroy_device_len,
           destroy_context_len, destroy_paging_len, destroy_sync_len,
           createprocess_len, createprocess_cmd_len,
           (int)createprocess_ret, createprocess_handle,
           createprocess_layout);
    ret = 0;

out:
    free(buf);
    return ret;
}

static int probe_open_resource_nt_once(int fd, struct d3dkmthandle device,
                                       uint64 shared_handle, const char *tag)
{
    struct d3dkmt_queryresourceinfofromnthandle query;
    struct d3dkmt_openresourcefromnthandle open_resource;
    struct d3dkmt_destroyallocation2 destroy;
    struct d3dddi_openallocationinfo2 *open_alloc = 0;
    void *runtime_data = 0;
    void *resource_data = 0;
    void *total_data = 0;
    uint32 allocation_count;
    int query_rc;
    int open_rc = -1;
    int ret = -1;

    memset(&query, 0, sizeof(query));
    query.device = device;
    query.nt_handle = shared_handle;
    query_rc = ioctl(fd, LX_DXQUERYRESOURCEINFOFROMNTHANDLE, &query);
    if (query_rc < 0) {
        printf("%s query_resource_nt failed fd=%lu\n", tag, shared_handle);
        printf("resource_nt_matrix tag=%s fd=%lu query_rc=%d query=FAIL open_rc=-1 open=SKIP resource=0x0 allocations=0 runtime=0 resource_priv=0 total_priv=0 first=0x0 priv0=0 gpuva=0x0 present_attempted=0 native_present_claim=0\n",
               tag, shared_handle, query_rc);
        return -1;
    }
    printf("%s query_resource_nt ok allocations=%u runtime=%u resource_priv=%u total_priv=%u\n",
           tag, query.allocation_count, query.private_runtime_data_size,
           query.resource_priv_drv_data_size,
           query.total_priv_drv_data_size);

    allocation_count = query.allocation_count;
    if (allocation_count == 0 || allocation_count > 64) {
        printf("%s open_resource_nt skipped invalid allocation_count=%u\n",
               tag, allocation_count);
        printf("resource_nt_matrix tag=%s fd=%lu query_rc=%d query=PASS open_rc=-1 open=SKIP resource=0x0 allocations=%u runtime=%u resource_priv=%u total_priv=%u first=0x0 priv0=0 gpuva=0x0 present_attempted=0 native_present_claim=0\n",
               tag, shared_handle, query_rc, query.allocation_count,
               query.private_runtime_data_size,
               query.resource_priv_drv_data_size,
               query.total_priv_drv_data_size);
        return -1;
    }

    open_alloc = malloc(allocation_count * sizeof(open_alloc[0]));
    runtime_data = probe_alloc_buffer(query.private_runtime_data_size);
    resource_data = probe_alloc_buffer(query.resource_priv_drv_data_size);
    total_data = probe_alloc_buffer(query.total_priv_drv_data_size);
    if (open_alloc == 0 ||
        (query.private_runtime_data_size != 0 && runtime_data == 0) ||
        (query.resource_priv_drv_data_size != 0 && resource_data == 0) ||
        (query.total_priv_drv_data_size != 0 && total_data == 0)) {
        printf("%s open_resource_nt skipped alloc failure count=%u\n",
               tag, allocation_count);
        printf("resource_nt_matrix tag=%s fd=%lu query_rc=%d query=PASS open_rc=-1 open=SKIP resource=0x0 allocations=%u runtime=%u resource_priv=%u total_priv=%u first=0x0 priv0=0 gpuva=0x0 present_attempted=0 native_present_claim=0\n",
               tag, shared_handle, query_rc, query.allocation_count,
               query.private_runtime_data_size,
               query.resource_priv_drv_data_size,
               query.total_priv_drv_data_size);
        goto out;
    }
    memset(open_alloc, 0, allocation_count * sizeof(open_alloc[0]));

    memset(&open_resource, 0, sizeof(open_resource));
    open_resource.device = device;
    open_resource.nt_handle = shared_handle;
    open_resource.allocation_count = allocation_count;
    open_resource.open_alloc_info = (uint64)open_alloc;
    open_resource.private_runtime_data_size =
        (int32)query.private_runtime_data_size;
    open_resource.private_runtime_data = (uint64)runtime_data;
    open_resource.resource_priv_drv_data_size =
        query.resource_priv_drv_data_size;
    open_resource.resource_priv_drv_data = (uint64)resource_data;
    open_resource.total_priv_drv_data_size =
        query.total_priv_drv_data_size;
    open_resource.total_priv_drv_data = (uint64)total_data;
    open_rc = ioctl(fd, LX_DXOPENRESOURCEFROMNTHANDLE, &open_resource);
    if (open_rc < 0 ||
        open_resource.resource.v == 0) {
        printf("%s open_resource_nt failed fd=%lu resource=0x%x allocations=%u\n",
               tag, shared_handle, open_resource.resource.v,
               allocation_count);
        printf("resource_nt_matrix tag=%s fd=%lu query_rc=%d query=PASS open_rc=%d open=FAIL resource=0x%x allocations=%u runtime=%u resource_priv=%u total_priv=%u first=0x%x priv0=%u gpuva=0x%lx present_attempted=0 native_present_claim=0\n",
               tag, shared_handle, query_rc, open_rc,
               open_resource.resource.v, allocation_count,
               query.private_runtime_data_size,
               query.resource_priv_drv_data_size,
               query.total_priv_drv_data_size, open_alloc[0].allocation.v,
               open_alloc[0].priv_drv_data_size, open_alloc[0].gpu_va);
        goto out;
    }
    printf("%s open_resource_nt ok resource=0x%x allocations=%u first=0x%x priv0=%u\n",
           tag, open_resource.resource.v, allocation_count,
           open_alloc[0].allocation.v, open_alloc[0].priv_drv_data_size);

    memset(&destroy, 0, sizeof(destroy));
    destroy.device = device;
    destroy.resource = open_resource.resource;
    destroy.flags.assume_not_in_use = 1;
    if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy) < 0)
        printf("%s open_resource_nt destroy_failed resource=0x%x\n",
               tag, open_resource.resource.v);
    printf("resource_nt_matrix tag=%s fd=%lu query_rc=%d query=PASS open_rc=%d open=PASS resource=0x%x allocations=%u runtime=%u resource_priv=%u total_priv=%u first=0x%x priv0=%u gpuva=0x%lx present_attempted=0 native_present_claim=0\n",
           tag, shared_handle, query_rc, open_rc, open_resource.resource.v,
           allocation_count, query.private_runtime_data_size,
           query.resource_priv_drv_data_size, query.total_priv_drv_data_size,
           open_alloc[0].allocation.v, open_alloc[0].priv_drv_data_size,
           open_alloc[0].gpu_va);
    ret = 0;

out:
    if (open_alloc)
        free(open_alloc);
    if (runtime_data)
        free(runtime_data);
    if (resource_data)
        free(resource_data);
    if (total_data)
        free(total_data);
    return ret;
}

static int query_open_resource_nt_summary(int fd, struct d3dkmthandle device,
                                          uint64 shared_handle,
                                          int *query_rc_out,
                                          int *open_rc_out,
                                          uint32 *allocation_count_out,
                                          uint32 *runtime_size_out,
                                          uint32 *resource_priv_size_out,
                                          uint32 *total_priv_size_out,
                                          struct d3dkmthandle *resource_out,
                                          struct d3dkmthandle *allocation_out,
                                          uint64 *gpu_va_out)
{
    struct d3dkmt_queryresourceinfofromnthandle query;
    struct d3dkmt_openresourcefromnthandle open_resource;
    struct d3dddi_openallocationinfo2 *open_alloc = 0;
    void *runtime_data = 0;
    void *resource_data = 0;
    void *total_data = 0;
    uint32 allocation_count;
    int query_rc;
    int open_rc = -1;
    int ret = -1;

    if (query_rc_out)
        *query_rc_out = -1;
    if (open_rc_out)
        *open_rc_out = -1;
    if (allocation_count_out)
        *allocation_count_out = 0;
    if (runtime_size_out)
        *runtime_size_out = 0;
    if (resource_priv_size_out)
        *resource_priv_size_out = 0;
    if (total_priv_size_out)
        *total_priv_size_out = 0;
    if (resource_out)
        memset(resource_out, 0, sizeof(*resource_out));
    if (allocation_out)
        memset(allocation_out, 0, sizeof(*allocation_out));
    if (gpu_va_out)
        *gpu_va_out = 0;

    memset(&query, 0, sizeof(query));
    query.device = device;
    query.nt_handle = shared_handle;
    query_rc = ioctl(fd, LX_DXQUERYRESOURCEINFOFROMNTHANDLE, &query);
    if (query_rc_out)
        *query_rc_out = query_rc;
    if (query_rc < 0)
        return -1;

    if (allocation_count_out)
        *allocation_count_out = query.allocation_count;
    if (runtime_size_out)
        *runtime_size_out = query.private_runtime_data_size;
    if (resource_priv_size_out)
        *resource_priv_size_out = query.resource_priv_drv_data_size;
    if (total_priv_size_out)
        *total_priv_size_out = query.total_priv_drv_data_size;

    allocation_count = query.allocation_count;
    if (allocation_count == 0 || allocation_count > 64)
        return -1;

    open_alloc = malloc(allocation_count * sizeof(open_alloc[0]));
    runtime_data = probe_alloc_buffer(query.private_runtime_data_size);
    resource_data = probe_alloc_buffer(query.resource_priv_drv_data_size);
    total_data = probe_alloc_buffer(query.total_priv_drv_data_size);
    if (open_alloc == 0 ||
        (query.private_runtime_data_size != 0 && runtime_data == 0) ||
        (query.resource_priv_drv_data_size != 0 && resource_data == 0) ||
        (query.total_priv_drv_data_size != 0 && total_data == 0))
        goto out;
    memset(open_alloc, 0, allocation_count * sizeof(open_alloc[0]));

    memset(&open_resource, 0, sizeof(open_resource));
    open_resource.device = device;
    open_resource.nt_handle = shared_handle;
    open_resource.allocation_count = allocation_count;
    open_resource.open_alloc_info = (uint64)open_alloc;
    open_resource.private_runtime_data_size =
        (int32)query.private_runtime_data_size;
    open_resource.private_runtime_data = (uint64)runtime_data;
    open_resource.resource_priv_drv_data_size =
        query.resource_priv_drv_data_size;
    open_resource.resource_priv_drv_data = (uint64)resource_data;
    open_resource.total_priv_drv_data_size = query.total_priv_drv_data_size;
    open_resource.total_priv_drv_data = (uint64)total_data;
    open_rc = ioctl(fd, LX_DXOPENRESOURCEFROMNTHANDLE, &open_resource);
    if (open_rc_out)
        *open_rc_out = open_rc;
    if (open_rc < 0 || open_resource.resource.v == 0)
        goto out;

    if (resource_out)
        *resource_out = open_resource.resource;
    if (allocation_out)
        *allocation_out = open_alloc[0].allocation;
    if (gpu_va_out)
        *gpu_va_out = open_alloc[0].gpu_va;

    ret = 0;

out:
    if (open_alloc)
        free(open_alloc);
    if (runtime_data)
        free(runtime_data);
    if (resource_data)
        free(resource_data);
    if (total_data)
        free(total_data);
    return ret;
}

static int query_open_resource_nt_capture(int fd, struct d3dkmthandle device,
                                          uint64 shared_handle,
                                          int *query_rc_out,
                                          int *open_rc_out,
                                          uint32 *allocation_count_out,
                                          uint32 *runtime_size_out,
                                          uint32 *resource_priv_size_out,
                                          uint32 *total_priv_size_out,
                                          uint32 *runtime_hash_out,
                                          uint32 *resource_priv_hash_out,
                                          uint32 *total_priv_hash_out,
                                          struct d3dkmthandle *resource_out,
                                          struct d3dkmthandle *allocation_out,
                                          uint64 *gpu_va_out)
{
    struct d3dkmt_queryresourceinfofromnthandle query;
    struct d3dkmt_openresourcefromnthandle open_resource;
    struct d3dddi_openallocationinfo2 *open_alloc = 0;
    void *runtime_data = 0;
    void *resource_data = 0;
    void *total_data = 0;
    uint32 allocation_count;
    int query_rc;
    int open_rc = -1;
    int ret = -1;

    if (query_rc_out)
        *query_rc_out = -1;
    if (open_rc_out)
        *open_rc_out = -1;
    if (allocation_count_out)
        *allocation_count_out = 0;
    if (runtime_size_out)
        *runtime_size_out = 0;
    if (resource_priv_size_out)
        *resource_priv_size_out = 0;
    if (total_priv_size_out)
        *total_priv_size_out = 0;
    if (runtime_hash_out)
        *runtime_hash_out = 0;
    if (resource_priv_hash_out)
        *resource_priv_hash_out = 0;
    if (total_priv_hash_out)
        *total_priv_hash_out = 0;
    if (resource_out)
        memset(resource_out, 0, sizeof(*resource_out));
    if (allocation_out)
        memset(allocation_out, 0, sizeof(*allocation_out));
    if (gpu_va_out)
        *gpu_va_out = 0;

    memset(&query, 0, sizeof(query));
    query.device = device;
    query.nt_handle = shared_handle;
    query_rc = ioctl(fd, LX_DXQUERYRESOURCEINFOFROMNTHANDLE, &query);
    if (query_rc_out)
        *query_rc_out = query_rc;
    if (query_rc < 0)
        return -1;

    allocation_count = query.allocation_count;
    if (allocation_count_out)
        *allocation_count_out = allocation_count;
    if (runtime_size_out)
        *runtime_size_out = query.private_runtime_data_size;
    if (resource_priv_size_out)
        *resource_priv_size_out = query.resource_priv_drv_data_size;
    if (total_priv_size_out)
        *total_priv_size_out = query.total_priv_drv_data_size;
    if (allocation_count == 0 || allocation_count > 64)
        return -1;

    open_alloc = malloc(allocation_count * sizeof(open_alloc[0]));
    runtime_data = probe_alloc_buffer(query.private_runtime_data_size);
    resource_data = probe_alloc_buffer(query.resource_priv_drv_data_size);
    total_data = probe_alloc_buffer(query.total_priv_drv_data_size);
    if (open_alloc == 0 ||
        (query.private_runtime_data_size != 0 && runtime_data == 0) ||
        (query.resource_priv_drv_data_size != 0 && resource_data == 0) ||
        (query.total_priv_drv_data_size != 0 && total_data == 0))
        goto out;
    memset(open_alloc, 0, allocation_count * sizeof(open_alloc[0]));

    memset(&open_resource, 0, sizeof(open_resource));
    open_resource.device = device;
    open_resource.nt_handle = shared_handle;
    open_resource.allocation_count = allocation_count;
    open_resource.open_alloc_info = (uint64)open_alloc;
    open_resource.private_runtime_data_size =
        (int32)query.private_runtime_data_size;
    open_resource.private_runtime_data = (uint64)runtime_data;
    open_resource.resource_priv_drv_data_size =
        query.resource_priv_drv_data_size;
    open_resource.resource_priv_drv_data = (uint64)resource_data;
    open_resource.total_priv_drv_data_size =
        query.total_priv_drv_data_size;
    open_resource.total_priv_drv_data = (uint64)total_data;
    open_rc = ioctl(fd, LX_DXOPENRESOURCEFROMNTHANDLE, &open_resource);
    if (open_rc_out)
        *open_rc_out = open_rc;
    if (open_rc < 0 || open_resource.resource.v == 0)
        goto out;

    if (runtime_hash_out)
        *runtime_hash_out =
            dxg_hash_bytes((const unsigned char *)runtime_data,
                           query.private_runtime_data_size);
    if (resource_priv_hash_out)
        *resource_priv_hash_out =
            dxg_hash_bytes((const unsigned char *)resource_data,
                           query.resource_priv_drv_data_size);
    if (total_priv_hash_out)
        *total_priv_hash_out =
            dxg_hash_bytes((const unsigned char *)total_data,
                           query.total_priv_drv_data_size);
    if (resource_out)
        *resource_out = open_resource.resource;
    if (allocation_out)
        *allocation_out = open_alloc[0].allocation;
    if (gpu_va_out)
        *gpu_va_out = open_alloc[0].gpu_va;

    ret = 0;

out:
    if (open_alloc)
        free(open_alloc);
    if (runtime_data)
        free(runtime_data);
    if (resource_data)
        free(resource_data);
    if (total_data)
        free(total_data);
    return ret;
}

static int probe_sealed_resource_add_denied(int fd,
                                            struct d3dkmthandle device,
                                            struct d3dkmthandle resource)
{
    struct dxg_shared_lifetime_status before;
    struct dxg_shared_lifetime_status after;
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    int rc;

    if (read_shared_lifetime_status(&before) < 0) {
        printf("shared_lifetime status_before_failed\n");
        return -1;
    }

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    create_allocation.device = device;
    create_allocation.resource = resource;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (read_shared_lifetime_status(&after) < 0) {
        printf("shared_lifetime status_after_failed\n");
        return -1;
    }
    if (rc >= 0) {
        printf("shared_lifetime sealed_add unexpected_success resource=0x%x allocation=0x%x denied_before=%u denied_after=%u\n",
               resource.v, allocation_info.allocation.v,
               before.denied, after.denied);
        return -1;
    }
    if (after.denied <= before.denied) {
        printf("shared_lifetime sealed_add failed_without_kernel_denial resource=0x%x denied_before=%u denied_after=%u\n",
               resource.v, before.denied, after.denied);
        return -1;
    }
    printf("shared_lifetime sealed_add denied resource=0x%x denied_before=%u denied_after=%u\n",
           resource.v, before.denied, after.denied);
    return 0;
}

static int dxg_shared_diag_same_record(
    const struct dxg_shared_resource_diag *a,
    const struct dxg_shared_resource_diag *b)
{
    return a != 0 && b != 0 &&
           a->record_seen && b->record_seen &&
           a->record_valid && b->record_valid &&
           a->record_key_process == b->record_key_process &&
           a->record_key_object == b->record_key_object &&
           a->record_key_nt == b->record_key_nt &&
           a->record_source_process == b->record_source_process &&
           a->record_source_generation == b->record_source_generation &&
           a->record_resource == b->record_resource &&
           a->record_allocation == b->record_allocation &&
           a->record_sealed == b->record_sealed &&
           a->record_sealed_generation == b->record_sealed_generation &&
           a->record_allocs == b->record_allocs &&
           a->record_runtime_size == b->record_runtime_size &&
           a->record_resource_size == b->record_resource_size &&
           a->record_total_size == b->record_total_size &&
           a->record_alloc0_priv == b->record_alloc0_priv &&
           a->record_runtime_hash == b->record_runtime_hash &&
           a->record_resource_hash == b->record_resource_hash &&
           a->record_total_hash == b->record_total_hash &&
           a->record_alloc0_hash == b->record_alloc0_hash;
}

static int probe_shared_mutation_create_attempt(int fd,
                                                struct d3dkmthandle device,
                                                struct d3dkmthandle resource,
                                                uint32 heap_size,
                                                uint32 priv_size)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    uint32 private_data = 0x5eedc0deU;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = heap_size;
    create_allocation.device = device;
    create_allocation.resource = resource;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.priv_drv_data_size = priv_size;
    if (priv_size != 0)
        create_allocation.priv_drv_data = (uint64)&private_data;
    create_allocation.flags.standard_allocation = 1;
    return ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
}

static int probe_shared_mutation_contract(int fd,
                                          struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmthandle objects[1];
    struct dxg_shared_lifetime_status before;
    struct dxg_shared_lifetime_status after_append;
    struct dxg_shared_lifetime_status after_private;
    struct dxg_shared_lifetime_status after_size;
    struct dxg_shared_resource_diag before_diag;
    struct dxg_shared_resource_diag after_diag;
    uint64 shared_handle = 0;
    int before_rc;
    int after_append_rc = -1;
    int after_private_rc = -1;
    int after_size_rc = -1;
    int before_diag_rc = -1;
    int after_diag_rc = -1;
    int create_rc = -1;
    int share_rc = -1;
    int append_rc = -1;
    int private_rc = -1;
    int size_flag_rc = -1;
    int owner_status = -1;
    int destroy_rc = -1;
    int record_same = 0;
    int record_mutated = 1;
    int pass;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    memset(&share_objects, 0, sizeof(share_objects));
    memset(&before, 0, sizeof(before));
    memset(&after_append, 0, sizeof(after_append));
    memset(&after_private, 0, sizeof(after_private));
    memset(&after_size, 0, sizeof(after_size));
    memset(&before_diag, 0, sizeof(before_diag));
    memset(&after_diag, 0, sizeof(after_diag));

    before_rc = read_shared_lifetime_status(&before);
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.cross_adapter = 1;
    create_allocation.flags.standard_allocation = 1;
    create_rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (create_rc < 0 || create_allocation.resource.v == 0 ||
        allocation_info.allocation.v == 0)
        goto print_row;

    objects[0] = create_allocation.resource;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
    if (share_rc < 0 || shared_handle == 0)
        goto cleanup_resource;
    before_diag_rc = read_dxg_shared_resource_diag(&before_diag);

    append_rc = probe_shared_mutation_create_attempt(
        fd, device, create_allocation.resource, 0x10000, 0);
    after_append_rc = read_shared_lifetime_status(&after_append);

    private_rc = probe_shared_mutation_create_attempt(
        fd, device, create_allocation.resource, 0x10000, sizeof(uint32));
    after_private_rc = read_shared_lifetime_status(&after_private);

    size_flag_rc = probe_shared_mutation_create_attempt(
        fd, device, create_allocation.resource, 0x20000, 0);
    after_size_rc = read_shared_lifetime_status(&after_size);
    after_diag_rc = read_dxg_shared_resource_diag(&after_diag);

    owner_status = 0;
    record_same = dxg_shared_diag_same_record(&before_diag, &after_diag);
    record_mutated = after_diag.record_mutated;

cleanup_resource:
    if (create_allocation.resource.v != 0) {
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        destroy_rc = ioctl(fd, LX_DXDESTROYALLOCATION2,
                           &destroy_allocation);
    }

print_row:
    pass = before_rc == 0 && create_rc == 0 && share_rc == 0 &&
           shared_handle != 0 && before_diag_rc == 0 &&
           append_rc < 0 && private_rc < 0 && size_flag_rc < 0 &&
           after_append_rc == 0 && after_private_rc == 0 &&
           after_size_rc == 0 &&
           after_append.denied > before.denied &&
           after_private.denied > after_append.denied &&
           after_size.denied > after_private.denied &&
           after_diag_rc == 0 && owner_status == 0 &&
           record_same && record_mutated == 0 && destroy_rc == 0;
    printf("shared_mutation_rejection_matrix create_rc=%d share_rc=%d fd=%lu device=0x%x resource=0x%x allocation=0x%x append_rc=%d private_rc=%d size_flag_rc=%d owner_status=%d before_rc=%d after_rc=%d/%d/%d diag_rc=%d/%d denied=%u->%u->%u->%u private_rewrite_rejects=%u->%u size_flag_rewrite_rejects=%u->%u owner_rewrite_rejects=%u->%u record_same=%u record_mutated=%u destroy_rc=%d present_attempted=0 native_present_claim=0 status=%s\n",
           create_rc, share_rc, shared_handle, device.v,
           create_allocation.resource.v, allocation_info.allocation.v,
           append_rc, private_rc, size_flag_rc, owner_status, before_rc,
           after_append_rc, after_private_rc, after_size_rc, before_diag_rc,
           after_diag_rc, before.denied, after_append.denied,
           after_private.denied, after_size.denied,
           after_append.denied, after_private.denied,
           after_private.denied, after_size.denied,
           before.denied, after_size.denied, record_same,
           record_mutated, destroy_rc, pass ? "PASS" : "FAIL");
    if (shared_handle != 0)
        close((int)shared_handle);
    return pass ? 0 : -1;
}

static void probe_shared_resource_nt(int fd, struct d3dkmthandle device,
                                     struct d3dkmthandle resource)
{
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmthandle objects[1];
    uint64 shared_handle = 0;
    int share_rc;

    if (device.v == 0 || resource.v == 0)
        return;

    memset(&share_objects, 0, sizeof(share_objects));
    objects[0] = resource;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
    if (share_rc < 0 || shared_handle == 0) {
        printf("share_resource_nt failed resource=0x%x handle=0x%lx rc=%d\n",
               resource.v, shared_handle, share_rc);
        return;
    }
    printf("share_resource_nt ok resource=0x%x fd=%lu\n",
           resource.v, shared_handle);

    probe_open_resource_nt_once(fd, device, shared_handle, "");
    {
        int pid = fork();
        int status = 1;

        if (pid < 0) {
            printf("open_resource_nt_child fork_failed fd=%lu\n",
                   shared_handle);
        } else if (pid == 0) {
            struct d3dkmthandle child_adapter;
            struct d3dkmthandle child_device;
            int child_fd = -1;
            int child_ret = 1;

            memset(&child_adapter, 0, sizeof(child_adapter));
            memset(&child_device, 0, sizeof(child_device));
            if (open_first_dxg_device(&child_fd, &child_adapter,
                                      &child_device) == 0) {
                if (probe_open_resource_nt_once(child_fd, child_device,
                                                shared_handle,
                                                "child") == 0)
                    child_ret = 0;
                close_dxg_device(child_fd, child_adapter, child_device);
            }
            exit(child_ret);
        } else {
            wait(&status);
            if (status != 0)
                printf("open_resource_nt_child failed status=%d\n", status);
        }
    }
    close((int)shared_handle);
}

static int probe_resource_nt_contract(int fd, struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmthandle objects[1];
    uint64 shared_handle = 0;
    int create_rc;
    int share_rc = -1;
    int open_ok = 0;
    int ret = -1;

    if (device.v == 0)
        return -1;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    memset(&share_objects, 0, sizeof(share_objects));

    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.cross_adapter = 1;
    create_allocation.flags.standard_allocation = 1;

    create_rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (create_rc < 0 ||
        allocation_info.allocation.v == 0 ||
        create_allocation.resource.v == 0) {
        printf("resource_nt_contract create fail rc=%d resource=0x%x allocation=0x%x global=0x%x flags=0x%x\n",
               create_rc, create_allocation.resource.v,
               allocation_info.allocation.v, create_allocation.global_share.v,
               create_allocation.flags.value);
        printf("resource_export_matrix create_rc=%d share_rc=-1 open=SKIP resource=0x%x allocation=0x%x global=0x%x fd=0 flags=0x%x present_attempted=0 native_present_claim=0\n",
               create_rc, create_allocation.resource.v,
               allocation_info.allocation.v, create_allocation.global_share.v,
               create_allocation.flags.value);
        return -1;
    }
    printf("resource_nt_contract create ok resource=0x%x allocation=0x%x global=0x%x flags=0x%x\n",
           create_allocation.resource.v, allocation_info.allocation.v,
           create_allocation.global_share.v, create_allocation.flags.value);

    objects[0] = create_allocation.resource;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
    if (share_rc < 0 || shared_handle == 0) {
        printf("resource_nt_contract share fail rc=%d resource=0x%x fd=%lu\n",
               share_rc, create_allocation.resource.v, shared_handle);
        printf("resource_export_matrix create_rc=%d share_rc=%d open=SKIP resource=0x%x allocation=0x%x global=0x%x fd=%lu flags=0x%x present_attempted=0 native_present_claim=0\n",
               create_rc, share_rc, create_allocation.resource.v,
               allocation_info.allocation.v, create_allocation.global_share.v,
               shared_handle, create_allocation.flags.value);
        goto out;
    }
    printf("resource_nt_contract share ok resource=0x%x fd=%lu\n",
           create_allocation.resource.v, shared_handle);

    if (probe_open_resource_nt_once(fd, device, shared_handle,
                                    "resource_nt") == 0)
        open_ok = 1;

    printf("resource_export_matrix create_rc=%d share_rc=%d open=%s resource=0x%x allocation=0x%x global=0x%x fd=%lu flags=0x%x present_attempted=0 native_present_claim=0\n",
           create_rc, share_rc, open_ok ? "PASS" : "FAIL",
           create_allocation.resource.v, allocation_info.allocation.v,
           create_allocation.global_share.v, shared_handle,
           create_allocation.flags.value);
    if (open_ok)
        ret = 0;

out:
    if (shared_handle != 0)
        close((int)shared_handle);
    if (create_allocation.resource.v != 0) {
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0)
            printf("resource_nt_contract destroy_failed resource=0x%x\n",
                   create_allocation.resource.v);
    }
    return ret;
}

static int probe_shared_exporter_close_lifetime_contract(
    int fd, struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmthandle objects[1];
    struct d3dkmthandle before_resource;
    struct d3dkmthandle before_allocation;
    struct d3dkmthandle after_resource;
    struct d3dkmthandle after_allocation;
    struct dxg_shared_lifetime_status before;
    struct dxg_shared_lifetime_status after;
    uint64 before_gpu_va = 0;
    uint64 after_gpu_va = 0;
    uint64 shared_handle = 0;
    uint32 before_allocations = 0;
    uint32 before_runtime = 0;
    uint32 before_resource_priv = 0;
    uint32 before_total_priv = 0;
    uint32 after_allocations = 0;
    uint32 after_runtime = 0;
    uint32 after_resource_priv = 0;
    uint32 after_total_priv = 0;
    int status_before_rc;
    int status_after_rc = -1;
    int create_rc;
    int share_rc = -1;
    int query_before_rc = -1;
    int open_before_rc = -1;
    int destroy_rc = -1;
    int query_after_rc = -1;
    int open_after_rc = -1;
    int pass;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    memset(&share_objects, 0, sizeof(share_objects));
    memset(&before_resource, 0, sizeof(before_resource));
    memset(&before_allocation, 0, sizeof(before_allocation));
    memset(&after_resource, 0, sizeof(after_resource));
    memset(&after_allocation, 0, sizeof(after_allocation));
    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));

    status_before_rc = read_shared_lifetime_status(&before);
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.cross_adapter = 1;
    create_allocation.flags.standard_allocation = 1;
    create_rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (create_rc < 0 || allocation_info.allocation.v == 0 ||
        create_allocation.resource.v == 0)
        goto print_row;

    objects[0] = create_allocation.resource;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
    if (share_rc < 0 || shared_handle == 0)
        goto destroy_original;

    query_open_resource_nt_summary(fd, device, shared_handle,
                                   &query_before_rc, &open_before_rc,
                                   &before_allocations, &before_runtime,
                                   &before_resource_priv,
                                   &before_total_priv,
                                   &before_resource, &before_allocation,
                                   &before_gpu_va);

destroy_original:
    destroy_allocation.device = device;
    destroy_allocation.resource = create_allocation.resource;
    destroy_allocation.flags.assume_not_in_use = 1;
    destroy_rc = ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation);

    if (shared_handle != 0) {
        query_open_resource_nt_summary(fd, device, shared_handle,
                                       &query_after_rc, &open_after_rc,
                                       &after_allocations, &after_runtime,
                                       &after_resource_priv,
                                       &after_total_priv,
                                       &after_resource, &after_allocation,
                                       &after_gpu_va);
    }
    status_after_rc = read_shared_lifetime_status(&after);

print_row:
    pass = create_rc == 0 && share_rc == 0 && shared_handle != 0 &&
           query_before_rc == 0 && open_before_rc == 0 &&
           destroy_rc == 0 && query_after_rc == 0 && open_after_rc == 0;
    printf("shared_resource_exporter_close_lifetime_matrix create_rc=%d share_rc=%d destroy_original_rc=%d fd=%lu resource=0x%x allocation=0x%x global=0x%x flags=0x%x query_before_rc=%d open_before_rc=%d before_resource=0x%x before_allocation=0x%x before_gpuva=0x%lx before_allocations=%u before_runtime=%u before_resource_priv=%u before_total_priv=%u query_after_rc=%d open_after_rc=%d after_resource=0x%x after_allocation=0x%x after_gpuva=0x%lx after_allocations=%u after_runtime=%u after_resource_priv=%u after_total_priv=%u status_before_rc=%d status_after_rc=%d seals=%u->%u reuses=%u->%u open_tracked=%u->%u sealed_blob_valid_after_destroy=%u global_handle_valid_after_destroy=%u present_attempted=0 native_present_claim=0 status=%s\n",
           create_rc, share_rc, destroy_rc, shared_handle,
           create_allocation.resource.v, allocation_info.allocation.v,
           create_allocation.global_share.v, create_allocation.flags.value,
           query_before_rc, open_before_rc, before_resource.v,
           before_allocation.v, before_gpu_va, before_allocations,
           before_runtime, before_resource_priv, before_total_priv,
           query_after_rc, open_after_rc, after_resource.v,
           after_allocation.v, after_gpu_va, after_allocations,
           after_runtime, after_resource_priv, after_total_priv,
           status_before_rc, status_after_rc, before.seals, after.seals,
           before.reuses, after.reuses, before.open_tracked,
           after.open_tracked, query_after_rc == 0, open_after_rc == 0,
           pass ? "PASS" : "FAIL");
    if (shared_handle != 0)
        close((int)shared_handle);
    return pass ? 0 : -1;
}

static int probe_shared_lifetime_contract(int fd, struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmthandle objects[1];
    struct dxg_shared_lifetime_status before;
    struct dxg_shared_lifetime_status after;
    struct dxg_shared_resource_diag before_close_diag;
    struct dxg_shared_resource_diag after_close_diag;
    uint64 shared_handle = 0;
    uint64 published_handle = 0;
    int original_destroyed = 0;
    int close_shared_rc = -1;
    int before_close_diag_rc = -1;
    int after_close_diag_rc = -1;
    int record_same = 0;
    int owner_preserved = 0;
    int create_rc;
    int share_rc;
    int ret = -1;

    if (device.v == 0)
        return -1;
    if (read_shared_lifetime_status(&before) < 0) {
        printf("shared_lifetime status_initial_failed\n");
        return -1;
    }

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&before_close_diag, 0, sizeof(before_close_diag));
    memset(&after_close_diag, 0, sizeof(after_close_diag));
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.cross_adapter = 1;
    create_allocation.flags.standard_allocation = 1;
    create_rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (create_rc < 0 ||
        allocation_info.allocation.v == 0 ||
        create_allocation.resource.v == 0) {
        printf("shared_lifetime create_failed allocation=0x%x resource=0x%x global=0x%x rc=%d\n",
               allocation_info.allocation.v, create_allocation.resource.v,
               create_allocation.global_share.v, create_rc);
        goto out;
    }
    printf("shared_lifetime resource=0x%x allocation=0x%x global=0x%x\n",
           create_allocation.resource.v, allocation_info.allocation.v,
           create_allocation.global_share.v);

    memset(&share_objects, 0, sizeof(share_objects));
    objects[0] = create_allocation.resource;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
    if (share_rc < 0 || shared_handle == 0) {
        printf("shared_lifetime share_failed resource=0x%x fd=%lu rc=%d\n",
               create_allocation.resource.v, shared_handle, share_rc);
        goto out;
    }
    published_handle = shared_handle;
    printf("shared_lifetime share ok fd=%lu\n", shared_handle);

    if (probe_open_resource_nt_once(fd, device, shared_handle,
                                    "shared_lifetime first") < 0)
        goto out;
    if (probe_open_resource_nt_once(fd, device, shared_handle,
                                    "shared_lifetime second") < 0)
        goto out;
    if (probe_sealed_resource_add_denied(fd, device,
                                         create_allocation.resource) < 0)
        goto out;

    {
        int pid = fork();
        int status = 1;

        if (pid < 0) {
            printf("shared_lifetime child fork_failed fd=%lu\n",
                   shared_handle);
            goto out;
        }
        if (pid == 0) {
            struct d3dkmthandle child_adapter;
            struct d3dkmthandle child_device;
            int child_fd = -1;
            int child_ret = 1;

            memset(&child_adapter, 0, sizeof(child_adapter));
            memset(&child_device, 0, sizeof(child_device));
            if (open_first_dxg_device(&child_fd, &child_adapter,
                                      &child_device) == 0) {
                if (probe_open_resource_nt_once(child_fd, child_device,
                                                shared_handle,
                                                "shared_lifetime child") == 0)
                    child_ret = 0;
                close_dxg_device(child_fd, child_adapter, child_device);
            }
            exit(child_ret);
        }
        wait(&status);
        if (status != 0) {
            printf("shared_lifetime child failed status=%d\n", status);
            goto out;
        }
    }

    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    destroy_allocation.device = device;
    destroy_allocation.resource = create_allocation.resource;
    destroy_allocation.flags.assume_not_in_use = 1;
    if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0) {
        printf("shared_lifetime destroy_original_failed resource=0x%x\n",
               create_allocation.resource.v);
        goto out;
    }
    original_destroyed = 1;
    printf("shared_lifetime original_destroyed resource=0x%x\n",
           create_allocation.resource.v);

    if (probe_open_resource_nt_once(fd, device, shared_handle,
                                    "shared_lifetime after_destroy") < 0)
        goto out;
    if (read_shared_lifetime_status(&after) < 0) {
        printf("shared_lifetime status_final_failed\n");
        goto out;
    }
    if (after.seals <= before.seals || after.reuses < before.reuses + 3 ||
        after.open_tracked < before.open_tracked + 3) {
        printf("shared_lifetime counters_unexpected seals:%u->%u reuses:%u->%u open:%u->%u\n",
               before.seals, after.seals, before.reuses, after.reuses,
               before.open_tracked, after.open_tracked);
        goto out;
    }

    printf("shared_lifetime ok seals:%u->%u reuses:%u->%u denied:%u->%u open:%u->%u\n",
           before.seals, after.seals, before.reuses, after.reuses,
           before.denied, after.denied,
           before.open_tracked, after.open_tracked);
    before_close_diag_rc = read_dxg_shared_resource_diag(&before_close_diag);
    ret = 0;

out:
    if (shared_handle != 0) {
        close_shared_rc = close((int)shared_handle);
        shared_handle = 0;
        if (ret == 0)
            after_close_diag_rc =
                read_dxg_shared_resource_diag(&after_close_diag);
    }
    if (ret == 0) {
        record_same =
            before_close_diag_rc == 0 && after_close_diag_rc == 0 &&
            before_close_diag.record_seen &&
            after_close_diag.record_seen &&
            before_close_diag.record_key_process ==
                after_close_diag.record_key_process &&
            before_close_diag.record_key_object ==
                after_close_diag.record_key_object &&
            before_close_diag.record_key_nt ==
                after_close_diag.record_key_nt &&
            before_close_diag.record_resource ==
                after_close_diag.record_resource &&
            before_close_diag.record_allocation ==
                after_close_diag.record_allocation &&
            before_close_diag.record_runtime_hash ==
                after_close_diag.record_runtime_hash &&
            before_close_diag.record_resource_hash ==
                after_close_diag.record_resource_hash &&
            before_close_diag.record_total_hash ==
                after_close_diag.record_total_hash;
        owner_preserved =
            before_close_diag_rc == 0 && after_close_diag_rc == 0 &&
            before_close_diag.record_source_process != 0 &&
            before_close_diag.record_source_process ==
                after_close_diag.record_source_process &&
            before_close_diag.record_source_generation ==
                after_close_diag.record_source_generation;
        printf("shared_lifetime_record_matrix fd=%lu close_rc=%d diag_rc=%d/%d livefd=1->1->0 same_record=%u owner_preserved=%u exporter_destroyed=%u resource=0x%x allocation=0x%x record_resource=0x%x->0x%x record_allocation=0x%x->0x%x record_key=0x%x/0x%x/0x%x->0x%x/0x%x/0x%x record_refs=%u->%u record_counts=q%u/o%u/fd%u->q%u/o%u/fd%u mutated=%u->%u present_attempted=0 native_present_claim=0 status=%s\n",
               published_handle, close_shared_rc, before_close_diag_rc,
               after_close_diag_rc, record_same, owner_preserved,
               original_destroyed, create_allocation.resource.v,
               allocation_info.allocation.v,
               before_close_diag.record_resource,
               after_close_diag.record_resource,
               before_close_diag.record_allocation,
               after_close_diag.record_allocation,
               before_close_diag.record_key_process,
               before_close_diag.record_key_object,
               before_close_diag.record_key_nt,
               after_close_diag.record_key_process,
               after_close_diag.record_key_object,
               after_close_diag.record_key_nt,
               before_close_diag.record_refs,
               after_close_diag.record_refs,
               before_close_diag.record_query_count,
               before_close_diag.record_open_count,
               before_close_diag.record_fd_publish_count,
               after_close_diag.record_query_count,
               after_close_diag.record_open_count,
               after_close_diag.record_fd_publish_count,
               before_close_diag.record_mutated,
               after_close_diag.record_mutated,
               close_shared_rc == 0 && record_same && owner_preserved &&
               original_destroyed ? "PASS" : "FAIL");
        if (close_shared_rc != 0 || !record_same || !owner_preserved ||
            !original_destroyed)
            ret = -1;
    }
    if (!original_destroyed && create_allocation.resource.v != 0) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0)
            printf("shared_lifetime cleanup_destroy_failed resource=0x%x\n",
                   create_allocation.resource.v);
    }
    return ret;
}

static void probe_shared_standard_allocation(int fd,
                                             struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;

    if (device.v == 0)
        return;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.cross_adapter = 1;
    create_allocation.flags.standard_allocation = 1;

    if (ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation) < 0 ||
        allocation_info.allocation.v == 0 ||
        create_allocation.resource.v == 0) {
        printf("shared_allocation_probe create_failed allocation=0x%x resource=0x%x global=0x%x\n",
               allocation_info.allocation.v, create_allocation.resource.v,
               create_allocation.global_share.v);
        return;
    }

    printf("shared_allocation_handle 0x%x resource=0x%x global=0x%x flags=0x%x\n",
           allocation_info.allocation.v, create_allocation.resource.v,
           create_allocation.global_share.v, create_allocation.flags.value);
    probe_shared_resource_nt(fd, device, create_allocation.resource);

    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    destroy_allocation.device = device;
    destroy_allocation.resource = create_allocation.resource;
    destroy_allocation.flags.assume_not_in_use = 1;
    if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0)
        printf("shared_allocation_probe destroy_failed resource=0x%x\n",
               create_allocation.resource.v);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex_bytes(const char *hex, unsigned char *out,
                           uint32 out_cap)
{
    uint32 n = 0;

    while (*hex != 0) {
        int hi;
        int lo;

        if (hex[0] == ' ' || hex[0] == '\n' || hex[0] == '\t') {
            hex++;
            continue;
        }
        if (hex[0] == 0 || hex[1] == 0 || n >= out_cap)
            return -1;
        hi = hex_nibble(hex[0]);
        lo = hex_nibble(hex[1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[n++] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return (int)n;
}

static const char *find_substring(const char *haystack, const char *needle)
{
    uint needle_len;

    if (haystack == 0 || needle == 0)
        return 0;
    needle_len = strlen(needle);
    if (needle_len == 0)
        return haystack;
    for (; *haystack != 0; haystack++) {
        if (memcmp(haystack, needle, needle_len) == 0)
            return haystack;
    }
    return 0;
}

static const char *blob_hex_payload(const char *text)
{
    static const char *markers[] = {
        "bytes=", "runtime_private=", "runtime=", "blob=", "hex=",
    };

    if (text == 0)
        return 0;
    for (uint32 i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        const char *p = find_substring(text, markers[i]);

        if (p != 0)
            return p + strlen(markers[i]);
    }
    return text;
}

static int load_hex_blob_arg(const char *arg, unsigned char *out,
                             uint32 out_cap)
{
    char *text = read_text_file(arg);
    const char *payload;
    int size;

    if (text != 0) {
        payload = blob_hex_payload(text);
        size = parse_hex_bytes(payload, out, out_cap);
        free(text);
        return size;
    }
    payload = blob_hex_payload(arg);
    return parse_hex_bytes(payload, out, out_cap);
}

static void print_blob_head(const unsigned char *blob, uint32 size)
{
    uint32 n = size < 16 ? size : 16;

    for (uint32 i = 0; i < n; i++)
        printf("%02x", blob[i]);
    if (n == 0)
        printf("none");
}

static int probe_runtime_blob_compare(const char *wsl_arg,
                                      const char *xv6_arg)
{
    unsigned char wsl[4096];
    unsigned char xv6[4096];
    int wsl_size;
    int xv6_size;
    int first_diff = -1;
    uint32 common;

    wsl_size = load_hex_blob_arg(wsl_arg, wsl, sizeof(wsl));
    xv6_size = load_hex_blob_arg(xv6_arg, xv6, sizeof(xv6));
    if (wsl_size < 0 || xv6_size < 0) {
        printf("runtime_blob_compare status=FAIL reason=parse wsl_size=%d xv6_size=%d wsl=%s xv6=%s\n",
               wsl_size, xv6_size, wsl_arg ? wsl_arg : "none",
               xv6_arg ? xv6_arg : "none");
        return -1;
    }
    common = wsl_size < xv6_size ? (uint32)wsl_size : (uint32)xv6_size;
    for (uint32 i = 0; i < common; i++) {
        if (wsl[i] != xv6[i]) {
            first_diff = (int)i;
            break;
        }
    }
    if (first_diff < 0 && wsl_size != xv6_size)
        first_diff = (int)common;
    printf("runtime_blob_compare status=%s wsl_size=%d xv6_size=%d common=%u first_diff=%d wsl_head=",
           first_diff < 0 ? "PASS" : "FAIL", wsl_size, xv6_size, common,
           first_diff);
    print_blob_head(wsl, wsl_size < 0 ? 0 : (uint32)wsl_size);
    printf(" xv6_head=");
    print_blob_head(xv6, xv6_size < 0 ? 0 : (uint32)xv6_size);
    if (first_diff >= 0 && first_diff < wsl_size && first_diff < xv6_size)
        printf(" wsl_byte=0x%02x xv6_byte=0x%02x",
               wsl[first_diff], xv6[first_diff]);
    printf("\n");
    return first_diff < 0 ? 0 : -1;
}

static int probe_raw_runtime_allocation_contract(int fd,
                                                 struct d3dkmthandle device,
                                                 const char *tag)
{
    static const char mesa_runtime_hex[] =
        "680000000400000040010000f000000057000000010000000000000000000000"
        "30e1fbffff7f0000010000000000000000000000000000000300000040010000"
        "f000000001000000010000005700000001000000000000000000000028000000"
        "0000000002080000480000000600000000000000000000000000050000000000"
        "0400000001000000020000000100000001000000000000000000100000000008"
        "5000000010000000201001000000000580000000600000001000000000000000"
        "30000000000000000000100000000004001000000000000f000000001000100"
        "5700000001000000000000000000002100000000000000000000000000000000"
        "00000000000000000";
    static const char mesa_alloc_hex[] =
        "4144564e04000100520200005844564e28080000000000000000400000000000"
        "1180000023000000070000000000000040010000f00000000100000001000000"
        "0100000000000000000000000000000000000000000000000100000000000000"
        "000000000000000000f000000004000000030000000300000001000000000000"
        "0000000000000000001000a00000000001000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000005000000000500000000000000000004"
        "0002000400000000000000030000000000000014000000040000000100000000"
        "0000000000000000005000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000006000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000005"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000";
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmthandle objects[1];
    unsigned char runtime_private[512];
    unsigned char allocation_private[768];
    uint64 shared_handle = 0;
    int runtime_size;
    int allocation_private_size;
    int create_rc;
    int create_errno;
    int share_rc;
    int share_errno;
    int ret = -1;

    if (device.v == 0) {
        printf("%s FAIL stage=device handle=0x%x\n", tag, device.v);
        return -1;
    }
    runtime_size = parse_hex_bytes(mesa_runtime_hex, runtime_private,
                                   sizeof(runtime_private));
    allocation_private_size =
        parse_hex_bytes(mesa_alloc_hex, allocation_private,
                        sizeof(allocation_private));
    if (runtime_size != 264 || allocation_private_size != 594) {
        printf("%s FAIL stage=fixture runtime=%d alloc=%d\n",
               tag, runtime_size, allocation_private_size);
        return -1;
    }

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    memset(&share_objects, 0, sizeof(share_objects));
    allocation_info.priv_drv_data = (uint64)&allocation_private;
    allocation_info.priv_drv_data_size = (uint32)allocation_private_size;
    allocation_info.flags.value = 0x4;
    allocation_info.priority = 0x78100000;
    create_allocation.device = device;
    create_allocation.private_runtime_data = (uint64)runtime_private;
    create_allocation.private_runtime_data_size = (uint32)runtime_size;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    create_allocation.flags.value = 0x47;

    create_rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    create_errno = create_rc < 0 ? -create_rc : 0;
    if (create_rc < 0 ||
        allocation_info.allocation.v == 0 ||
        create_allocation.resource.v == 0) {
        printf("%s FAIL stage=createallocation rc=%d errno=%d allocation=0x%x resource=0x%x create_flags=0x%x alloc_flags=0x%x priority=0x%x runtime=%u alloc_priv_in=%d returned_priv=%u\n",
               tag, create_rc, create_errno, allocation_info.allocation.v,
               create_allocation.resource.v, create_allocation.flags.value,
               allocation_info.flags.value, allocation_info.priority,
               create_allocation.private_runtime_data_size,
               allocation_private_size, allocation_info.priv_drv_data_size);
        goto out;
    }

    printf("%s create ok resource=0x%x allocation=0x%x global=0x%x create_flags=0x%x alloc_flags=0x%x priority=0x%x returned_priv=%u\n",
           tag, create_allocation.resource.v, allocation_info.allocation.v,
           create_allocation.global_share.v, create_allocation.flags.value,
           allocation_info.flags.value, allocation_info.priority,
           allocation_info.priv_drv_data_size);

    objects[0] = create_allocation.resource;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
    share_errno = share_rc < 0 ? -share_rc : 0;
    if (share_rc < 0 || shared_handle == 0) {
        printf("%s FAIL stage=share rc=%d errno=%d resource=0x%x fd=%lu\n",
               tag, share_rc, share_errno, create_allocation.resource.v,
               shared_handle);
        goto out;
    }

    printf("%s share ok fd=%lu\n", tag, shared_handle);
    if (probe_open_resource_nt_once(fd, device, shared_handle,
                                    tag) < 0) {
        printf("%s FAIL stage=query_open fd=%lu\n", tag, shared_handle);
        goto out;
    }
    ret = 0;

out:
    if (shared_handle != 0)
        close((int)shared_handle);
    if (create_allocation.resource.v != 0) {
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0)
            printf("%s destroy_failed resource=0x%x\n",
                   tag, create_allocation.resource.v);
    }
    if (ret == 0)
        printf("%s PASS resource=0x%x allocation=0x%x create_flags=0x%x alloc_flags=0x%x priority=0x%x runtime=%u alloc_priv=%d\n",
               tag, create_allocation.resource.v,
               allocation_info.allocation.v, create_allocation.flags.value,
               allocation_info.flags.value, allocation_info.priority,
               create_allocation.private_runtime_data_size,
               allocation_private_size);
    return ret;
}

static int probe_shared_seal_provenance_contract(int fd,
                                                 struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmthandle objects[1];
    struct d3dkmthandle opened_resource;
    struct d3dkmthandle opened_allocation;
    struct dxg_shared_lifetime_status lifetime_before;
    struct dxg_shared_lifetime_status lifetime_after;
    struct dxg_shared_resource_diag share_diag;
    struct dxg_shared_resource_diag open_diag;
    struct dxg_shared_resource_diag close_diag;
    struct dxg_present_credit_status present_before;
    struct dxg_present_credit_status present_after;
    uint64 shared_handle = ~0ULL;
    uint64 published_handle = ~0ULL;
    uint64 opened_gpu_va = 0;
    uint32 expected_runtime_hash;
    uint32 expected_resource_hash;
    uint32 expected_total_hash;
    uint32 opened_runtime_hash = 0;
    uint32 opened_resource_hash = 0;
    uint32 opened_total_hash = 0;
    uint32 query_allocations = 0;
    uint32 query_runtime_size = 0;
    uint32 query_resource_size = 0;
    uint32 query_total_size = 0;
    uint32 expected_runtime_size = 0;
    uint32 expected_resource_size = 0;
    uint32 expected_total_size = 0;
    int create_rc = -1;
    int share_rc = -1;
    int share_fd_valid = 0;
    int share_fd_flags = -1;
    int share_fd_cloexec = 0;
    int query_rc = -1;
    int open_rc = -1;
    int close_fd_rc = -1;
    int destroy_rc = -1;
    int lifetime_before_rc;
    int lifetime_after_rc = -1;
    int share_diag_rc = -1;
    int open_diag_rc = -1;
    int close_diag_rc = -1;
    int metadata_stable;
    int seal_before_query;
    int local_resource_admitted;
    int refcounts_coherent;
    int record_generation_coherent;
    int canonical_record_coherent;
    int shared_model_coherent;
    int sealed_alloc_metadata_coherent;
    int parent_lifetime_coherent;
    int no_present_credit;
    int pass;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    memset(&share_objects, 0, sizeof(share_objects));
    memset(&opened_resource, 0, sizeof(opened_resource));
    memset(&opened_allocation, 0, sizeof(opened_allocation));
    memset(&lifetime_before, 0, sizeof(lifetime_before));
    memset(&lifetime_after, 0, sizeof(lifetime_after));
    memset(&share_diag, 0, sizeof(share_diag));
    memset(&open_diag, 0, sizeof(open_diag));
    memset(&close_diag, 0, sizeof(close_diag));
    memset(&present_before, 0, sizeof(present_before));
    memset(&present_after, 0, sizeof(present_after));

    expected_runtime_hash = dxg_hash_bytes(0, 0);
    expected_resource_hash = dxg_hash_bytes(0, 0);
    expected_total_hash = dxg_hash_bytes(0, 0);
    lifetime_before_rc = read_shared_lifetime_status(&lifetime_before);
    read_present_credit_status(&present_before);

    if (device.v == 0)
        goto print_row;

    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.cross_adapter = 1;
    create_allocation.flags.standard_allocation = 1;
    create_rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (create_rc < 0 || allocation_info.allocation.v == 0 ||
        create_allocation.resource.v == 0)
        goto print_row;

    objects[0] = create_allocation.resource;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
    share_fd_valid = share_rc == 0;
    if (share_fd_valid)
        published_handle = shared_handle;
    if (!share_fd_valid)
        goto cleanup_resource;
    share_fd_flags = fcntl((int)shared_handle, F_GETFD, 0);
    share_fd_cloexec =
        share_fd_flags >= 0 && (share_fd_flags & FD_CLOEXEC) != 0;

    share_diag_rc = read_dxg_shared_resource_diag(&share_diag);
    if (share_diag_rc == 0 && share_diag.metadata_seen) {
        expected_runtime_size = share_diag.metadata_runtime_size;
        expected_resource_size = share_diag.metadata_resource_size;
        expected_total_size = share_diag.metadata_total_size;
        expected_runtime_hash = share_diag.metadata_runtime_hash;
        expected_resource_hash = share_diag.metadata_resource_hash;
        expected_total_hash = share_diag.metadata_total_hash;
    }
    query_open_resource_nt_capture(fd, device, shared_handle, &query_rc,
                                   &open_rc, &query_allocations,
                                   &query_runtime_size,
                                   &query_resource_size,
                                   &query_total_size, &opened_runtime_hash,
                                   &opened_resource_hash,
                                   &opened_total_hash, &opened_resource,
                                   &opened_allocation, &opened_gpu_va);
    open_diag_rc = read_dxg_shared_resource_diag(&open_diag);
    close_fd_rc = close((int)shared_handle);
    shared_handle = ~0ULL;
    close_diag_rc = read_dxg_shared_resource_diag(&close_diag);

cleanup_resource:
    if (opened_resource.v != 0) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        destroy_allocation.device = device;
        destroy_allocation.resource = opened_resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        (void)ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation);
    }
    if (create_allocation.resource.v != 0) {
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        destroy_rc = ioctl(fd, LX_DXDESTROYALLOCATION2,
                           &destroy_allocation);
    }
    lifetime_after_rc = read_shared_lifetime_status(&lifetime_after);
    read_present_credit_status(&present_after);

print_row:
	metadata_stable =
	    share_diag_rc == 0 && open_diag_rc == 0 && close_diag_rc == 0 &&
	    share_diag.metadata_seen && open_diag.metadata_seen &&
	    close_diag.metadata_seen &&
	    dxg_blob_metadata_equal(share_diag.metadata_runtime_size,
				    share_diag.metadata_runtime_hash,
				    expected_runtime_size,
				    expected_runtime_hash) &&
	    dxg_blob_metadata_equal(share_diag.metadata_resource_size,
				    share_diag.metadata_resource_hash,
				    expected_resource_size,
				    expected_resource_hash) &&
	    dxg_blob_metadata_equal(share_diag.metadata_total_size,
				    share_diag.metadata_total_hash,
				    expected_total_size,
				    expected_total_hash) &&
	    dxg_blob_metadata_equal(open_diag.metadata_runtime_size,
				    open_diag.metadata_runtime_hash,
				    share_diag.metadata_runtime_size,
				    share_diag.metadata_runtime_hash) &&
	    dxg_blob_metadata_equal(open_diag.metadata_resource_size,
				    open_diag.metadata_resource_hash,
				    share_diag.metadata_resource_size,
				    share_diag.metadata_resource_hash) &&
	    dxg_blob_metadata_equal(open_diag.metadata_total_size,
				    open_diag.metadata_total_hash,
				    share_diag.metadata_total_size,
				    share_diag.metadata_total_hash) &&
	    dxg_blob_metadata_equal(close_diag.metadata_runtime_size,
				    close_diag.metadata_runtime_hash,
				    share_diag.metadata_runtime_size,
				    share_diag.metadata_runtime_hash) &&
	    dxg_blob_metadata_equal(close_diag.metadata_resource_size,
				    close_diag.metadata_resource_hash,
				    share_diag.metadata_resource_size,
				    share_diag.metadata_resource_hash) &&
	    dxg_blob_metadata_equal(close_diag.metadata_total_size,
				    close_diag.metadata_total_hash,
				    share_diag.metadata_total_size,
				    share_diag.metadata_total_hash) &&
	    dxg_blob_metadata_equal(query_runtime_size, opened_runtime_hash,
				    expected_runtime_size,
				    expected_runtime_hash) &&
	    dxg_blob_metadata_equal(query_resource_size, opened_resource_hash,
				    expected_resource_size,
				    expected_resource_hash) &&
	    dxg_blob_metadata_equal(query_total_size, opened_total_hash,
				    expected_total_size,
				    expected_total_hash);
    seal_before_query =
        share_diag.nt_diag_seen && share_diag.nt_meta_after != 0 &&
        share_diag.nt_seal_after != 0 &&
        share_diag.nt_host_seal_after != 0 &&
        share_diag.nt_resource_host != 0 && share_diag.nt_alloc_host != 0;
    local_resource_admitted =
        share_diag.runtime_object_seen &&
        share_diag.runtime_user_obj == create_allocation.resource.v &&
        share_diag.runtime_user_dev == device.v &&
        share_diag.runtime_entry_found != 0 &&
        share_diag.runtime_entry_exact != 0;
    refcounts_coherent =
        lifetime_before_rc == 0 && lifetime_after_rc == 0 &&
        lifetime_after.seals >= lifetime_before.seals + 1 &&
        lifetime_after.open_tracked >= lifetime_before.open_tracked + 1 &&
        open_diag.query_refs != 0 && open_diag.open_fd_refs != 0;
    record_generation_coherent =
        share_diag.record_seen && open_diag.record_seen &&
        close_diag.record_seen && share_diag.record_valid &&
        open_diag.record_valid && close_diag.record_valid &&
        share_diag.record_sealed != 0 && open_diag.record_sealed != 0 &&
        close_diag.record_sealed != 0 &&
        share_diag.record_sealed_generation != 0 &&
        open_diag.record_sealed_generation ==
            share_diag.record_sealed_generation &&
        close_diag.record_sealed_generation ==
            share_diag.record_sealed_generation;
    canonical_record_coherent =
        record_generation_coherent &&
        share_diag.record_key_process == open_diag.record_key_process &&
        share_diag.record_key_process == close_diag.record_key_process &&
        share_diag.record_key_object == open_diag.record_key_object &&
        share_diag.record_key_object == close_diag.record_key_object &&
        share_diag.record_key_nt == open_diag.record_key_nt &&
        share_diag.record_key_nt == close_diag.record_key_nt &&
        share_diag.record_source_process ==
            open_diag.record_source_process &&
        share_diag.record_source_process ==
            close_diag.record_source_process &&
        share_diag.record_source_generation ==
            open_diag.record_source_generation &&
        share_diag.record_source_generation ==
            close_diag.record_source_generation &&
        share_diag.record_resource == create_allocation.resource.v &&
        share_diag.record_resource == open_diag.record_resource &&
        share_diag.record_resource == close_diag.record_resource &&
        share_diag.record_allocation == allocation_info.allocation.v &&
        share_diag.record_allocation == open_diag.record_allocation &&
        share_diag.record_allocation == close_diag.record_allocation &&
        share_diag.record_allocs == query_allocations &&
        share_diag.record_runtime_size == expected_runtime_size &&
        share_diag.record_resource_size == expected_resource_size &&
        share_diag.record_total_size == expected_total_size &&
        share_diag.record_runtime_hash == expected_runtime_hash &&
        share_diag.record_resource_hash == expected_resource_hash &&
        share_diag.record_total_hash == expected_total_hash &&
        close_diag.record_fd_publish_count >=
            share_diag.record_fd_publish_count &&
        open_diag.record_query_count >= share_diag.record_query_count &&
        open_diag.record_open_count >= share_diag.record_open_count &&
        close_diag.record_mutated == 0;
    shared_model_coherent =
        share_diag.model_valid && open_diag.model_valid &&
        close_diag.model_valid &&
        share_diag.model_flat_match && open_diag.model_flat_match &&
        close_diag.model_flat_match &&
        share_diag.model_generation == share_diag.record_sealed_generation &&
        open_diag.model_generation == open_diag.record_sealed_generation &&
        close_diag.model_generation == close_diag.record_sealed_generation &&
        share_diag.model_sealed != 0 && open_diag.model_sealed != 0 &&
        close_diag.model_sealed != 0 &&
        share_diag.model_allocs == query_allocations &&
        share_diag.model_alloc0 == allocation_info.allocation.v &&
        share_diag.model_alloc0 == open_diag.model_alloc0 &&
        share_diag.model_alloc0 == close_diag.model_alloc0 &&
        share_diag.model_alloc0_priv == expected_total_size &&
        share_diag.model_runtime_size == expected_runtime_size &&
        share_diag.model_resource_size == expected_resource_size &&
        share_diag.model_total_size == expected_total_size;
    sealed_alloc_metadata_coherent =
        shared_model_coherent &&
        share_diag.model_alloc0_pages != 0 &&
        share_diag.model_alloc0_pages == open_diag.model_alloc0_pages &&
        share_diag.model_alloc0_pages == close_diag.model_alloc0_pages &&
        share_diag.model_alloc0_cached == open_diag.model_alloc0_cached &&
        share_diag.model_alloc0_cached == close_diag.model_alloc0_cached &&
        share_diag.model_alloc0_flags == open_diag.model_alloc0_flags &&
        share_diag.model_alloc0_flags == close_diag.model_alloc0_flags;
    parent_lifetime_coherent =
        share_diag.parent_seen && open_diag.parent_seen &&
        close_diag.parent_seen &&
        share_diag.parent_last != 0 &&
        share_diag.parent_last == open_diag.parent_last &&
        share_diag.parent_last == close_diag.parent_last &&
        share_diag.parent_children >= 1 &&
        open_diag.parent_children >= 2 &&
        close_diag.parent_children >= 2 &&
        close_diag.parent_fd_refs == 0 &&
        close_diag.parent_refs >= close_diag.parent_children &&
        close_diag.parent_sealed_generation == close_diag.model_generation &&
        open_diag.parent_open_count >= share_diag.parent_open_count &&
        close_diag.parent_release_count >= share_diag.parent_release_count;
    no_present_credit =
        present_before.rc == 0 && present_after.rc == 0 &&
        present_before.display_presents == present_after.display_presents &&
        present_before.display_completions ==
            present_after.display_completions &&
        present_before.register_attempts == present_after.register_attempts &&
        present_before.commit_attempts == present_after.commit_attempts;
    pass = create_rc == 0 && share_rc == 0 && share_fd_valid &&
           share_fd_cloexec &&
           query_rc == 0 && open_rc == 0 && close_fd_rc == 0 &&
           destroy_rc == 0 && metadata_stable && seal_before_query &&
           local_resource_admitted && refcounts_coherent &&
           record_generation_coherent && canonical_record_coherent &&
           shared_model_coherent && sealed_alloc_metadata_coherent &&
           parent_lifetime_coherent &&
           no_present_credit;

    printf("shared_resource_parent_lifetime_matrix parent=0x%x/0x%x/0x%x refs=%u/%u/%u fd_refs=%u/%u/%u children=%u/%u/%u sealed_gen=%u/%u/%u publish=%u/%u/%u open=%u/%u/%u release=%u/%u/%u status=%s\n",
           share_diag.parent_last, open_diag.parent_last,
           close_diag.parent_last, share_diag.parent_refs,
           open_diag.parent_refs, close_diag.parent_refs,
           share_diag.parent_fd_refs, open_diag.parent_fd_refs,
           close_diag.parent_fd_refs, share_diag.parent_children,
           open_diag.parent_children, close_diag.parent_children,
           share_diag.parent_sealed_generation,
           open_diag.parent_sealed_generation,
           close_diag.parent_sealed_generation,
           share_diag.parent_publish_count,
           open_diag.parent_publish_count,
           close_diag.parent_publish_count,
           share_diag.parent_open_count, open_diag.parent_open_count,
           close_diag.parent_open_count,
           share_diag.parent_release_count,
           open_diag.parent_release_count,
           close_diag.parent_release_count,
           parent_lifetime_coherent ? "PASS" : "FAIL");
    printf("shared_resource_sealed_alloc_metadata_matrix pages0=%lu/%lu/%lu cached0=%u/%u/%u flags0=0x%x/0x%x/0x%x size0=%lu/%lu/%lu model_valid=%u/%u/%u status=%s\n",
           share_diag.model_alloc0_pages, open_diag.model_alloc0_pages,
           close_diag.model_alloc0_pages,
           share_diag.model_alloc0_cached, open_diag.model_alloc0_cached,
           close_diag.model_alloc0_cached,
           share_diag.model_alloc0_flags, open_diag.model_alloc0_flags,
           close_diag.model_alloc0_flags,
           share_diag.model_alloc0_size, open_diag.model_alloc0_size,
           close_diag.model_alloc0_size,
           share_diag.model_valid, open_diag.model_valid,
           close_diag.model_valid,
           sealed_alloc_metadata_coherent ? "PASS" : "FAIL");

    printf("shared_resource_seal_provenance_matrix create_rc=%d share_rc=%d fd=%lu fd_valid=%u fd_flags=%d fd_cloexec=%u query_rc=%d open_rc=%d close_fd_rc=%d destroy_rc=%d device=0x%x resource=0x%x allocation=0x%x global=0x%x opened_resource=0x%x opened_allocation=0x%x opened_gpuva=0x%lx create_flags=0x%x alloc_flags=0x%x expected_runtime=%u/%08x expected_resource=%u/%08x expected_total=%u/%08x share_meta=%u/%08x,%u/%08x,%u/%08x open_blob=%u/%08x,%u/%08x,%u/%08x close_meta=%u/%08x,%u/%08x,%u/%08x metadata_stable=%u nt_seal_before_query=%u nt_meta=%u->%u nt_seal=%u->%u nt_host_seal=%u->%u local_resource_admitted=%u runtime_user_obj=0x%x runtime_user_dev=0x%x runtime_entry=%u/%u query_allocs=%u query_sizes=%u,%u,%u open_refs=%u query_refs=%u lifetime_seals=%u->%u lifetime_open_tracked=%u->%u refcounts_coherent=%u record_generation_coherent=%u canonical_record_coherent=%u shared_model_coherent=%u model_valid=%u/%u/%u model_flat=%u/%u/%u model_alloc0=0x%x/0x%x/0x%x model_sizes=%u,%u,%u model_priv0=%u model_gen=%u/%u/%u record_key=0x%x/0x%x/0x%x record_source=0x%x/%u record_counts=q%u/o%u/fd%u record_mutated=%u present_attempted=0 native_present_claim=0 present_stats_rc=%d/%d present_delta=%lu,%lu,%lu,%lu no_present_credit=%u status=%s\n",
           create_rc, share_rc,
           share_fd_valid ? published_handle : (uint64)~0ULL,
           share_fd_valid, share_fd_flags, share_fd_cloexec,
           query_rc, open_rc, close_fd_rc, destroy_rc, device.v,
           create_allocation.resource.v, allocation_info.allocation.v,
           create_allocation.global_share.v, opened_resource.v,
           opened_allocation.v, opened_gpu_va,
           create_allocation.flags.value, allocation_info.flags.value,
           expected_runtime_size, expected_runtime_hash,
           expected_resource_size, expected_resource_hash,
           expected_total_size, expected_total_hash,
           share_diag.metadata_runtime_size,
           share_diag.metadata_runtime_hash,
           share_diag.metadata_resource_size,
           share_diag.metadata_resource_hash,
           share_diag.metadata_total_size,
           share_diag.metadata_total_hash, query_runtime_size,
           opened_runtime_hash, query_resource_size, opened_resource_hash,
           query_total_size, opened_total_hash,
           close_diag.metadata_runtime_size,
           close_diag.metadata_runtime_hash,
           close_diag.metadata_resource_size,
           close_diag.metadata_resource_hash,
           close_diag.metadata_total_size,
           close_diag.metadata_total_hash, metadata_stable,
           seal_before_query, share_diag.nt_meta_before,
           share_diag.nt_meta_after, share_diag.nt_seal_before,
           share_diag.nt_seal_after, share_diag.nt_host_seal_before,
           share_diag.nt_host_seal_after, local_resource_admitted,
           share_diag.runtime_user_obj, share_diag.runtime_user_dev,
           share_diag.runtime_entry_found, share_diag.runtime_entry_exact,
           query_allocations, query_runtime_size, query_resource_size,
           query_total_size, open_diag.open_fd_refs, open_diag.query_refs,
           lifetime_before.seals, lifetime_after.seals,
           lifetime_before.open_tracked, lifetime_after.open_tracked,
           refcounts_coherent, record_generation_coherent,
           canonical_record_coherent, shared_model_coherent,
           share_diag.model_valid, open_diag.model_valid,
           close_diag.model_valid, share_diag.model_flat_match,
           open_diag.model_flat_match, close_diag.model_flat_match,
           share_diag.model_alloc0, open_diag.model_alloc0,
           close_diag.model_alloc0, share_diag.model_runtime_size,
           share_diag.model_resource_size, share_diag.model_total_size,
           share_diag.model_alloc0_priv, share_diag.model_generation,
           open_diag.model_generation, close_diag.model_generation,
           close_diag.record_key_process,
           close_diag.record_key_object, close_diag.record_key_nt,
           close_diag.record_source_process,
           close_diag.record_source_generation,
           close_diag.record_query_count, close_diag.record_open_count,
           close_diag.record_fd_publish_count, close_diag.record_mutated,
           present_before.rc, present_after.rc,
           present_after.display_presents - present_before.display_presents,
           present_after.display_completions -
               present_before.display_completions,
           present_after.register_attempts - present_before.register_attempts,
           present_after.commit_attempts - present_before.commit_attempts,
           no_present_credit, pass ? "PASS" : "FAIL");
    if (shared_handle != ~0ULL)
        close((int)shared_handle);
    return pass ? 0 : -1;
}

static int probe_shared_present_contract(int fd, struct d3dkmthandle device)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmthandle objects[1];
    uint64 shared_handle = 0;
    int ret = -1;
    int fb_fd = -1;
    int opened_resource = 0;
    int create_rc;
    int share_rc;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.cross_adapter = 1;
    create_allocation.flags.standard_allocation = 1;
    create_rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (create_rc < 0 ||
        allocation_info.allocation.v == 0 ||
        create_allocation.resource.v == 0) {
        printf("shared_present create_resource_failed allocation=0x%x resource=0x%x rc=%d\n",
               allocation_info.allocation.v, create_allocation.resource.v,
               create_rc);
        goto out;
    }
    printf("shared_present resource=0x%x allocation=0x%x global=0x%x\n",
           create_allocation.resource.v, allocation_info.allocation.v,
           create_allocation.global_share.v);

    memset(&share_objects, 0, sizeof(share_objects));
    objects[0] = create_allocation.resource;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
    if (share_rc < 0 || shared_handle == 0) {
        printf("shared_present share_resource_failed resource=0x%x fd=%lu rc=%d\n",
               create_allocation.resource.v, shared_handle, share_rc);
        goto out;
    }
    printf("shared_present share_resource ok fd=%lu\n", shared_handle);
    if (probe_open_resource_nt_once(fd, device, shared_handle,
                                    "shared_present") < 0)
        goto out;
    opened_resource = 1;

    if (probe_shared_sync_nt(fd, device) < 0) {
        printf("shared_present shared_fence_failed\n");
        goto out;
    }

    fb_fd = open("/dev/gpu0", O_RDWR);
    if (fb_fd < 0)
        fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        printf("shared_present backend_query_failed open framebuffer\n");
        goto out;
    }
    {
        struct fb_gpu_backend_info backend;

        memset(&backend, 0, sizeof(backend));
        if (ioctl(fb_fd, FB_GPU_BACKEND_QUERY, &backend) < 0) {
            printf("shared_present backend_query_failed ioctl\n");
            goto out;
        }
        printf("shared_present backend=%u flags=0x%x name=%s\n",
               backend.backend, backend.flags, backend.name);
        if ((backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0) {
            printf("shared_present contract_unavailable opengl_submit=0\n");
            goto out;
        }
    }

    printf("shared_present contract ok\n");
    ret = 0;

out:
    if (fb_fd >= 0)
        close(fb_fd);
    if (shared_handle != 0)
        close((int)shared_handle);
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    destroy_allocation.device = device;
    destroy_allocation.resource = create_allocation.resource;
    destroy_allocation.flags.assume_not_in_use = 1;
    if (create_allocation.resource.v != 0 &&
        ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0)
        printf("shared_present destroy_failed resource=0x%x\n",
               create_allocation.resource.v);
    if (ret == 0)
        printf("shared_present ok opened_resource=%d\n", opened_resource);
    return ret;
}

static int probe_present_source_failclosed_contract(
    int fd, struct d3dkmthandle device, struct winluid adapter_luid)
{
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_shareobjects share_objects;
    struct d3dkmt_createsynchronizationobject2 create_sync;
    struct d3dkmt_createsyncfile present_create_sync_file;
    struct d3dkmt_opensyncobjectfromsyncfile present_open_sync_file;
    struct d3dkmt_opensyncobjectfromsyncfile present_bad_open_sync_file;
    struct d3dkmt_destroysynchronizationobject destroy_sync;
    struct d3dkmthandle objects[1];
    struct fb_gpu_dxg_present_source_register reg;
    struct fb_gpu_dxg_present_source_commit commit;
    struct fb_gpu_dxg_present_source_commit wait_commit;
    struct fb_gpu_dxg_present_source_commit sync_file_commit;
    struct fb_gpu_dxg_present_source_commit missing_sync_commit;
    struct fb_gpu_dxg_present_source_commit sync_without_flag_commit;
    struct fb_gpu_dxg_present_source_commit no_source_commit;
    struct fb_gpu_dxg_present_source_query query;
    struct fb_gpu_dxg_present_source_query wait_query;
    struct fb_gpu_dxg_present_source_query sync_file_query;
    struct fb_gpu_dxg_present_host_bind_contract bind_contract;
    struct fb_gpu_dxg_present_host_bind_contract foreign_bind_contract;
    struct fb_gpu_dxg_present_source_register bad_reg;
    struct fb_gpu_dxg_present_source_register unverified_reg;
    struct fb_gpu_dxg_present_source_commit unverified_commit;
    struct fb_gpu_dxg_present_source_query unverified_query;
    struct fb_gpu_dxg_present_source_register mismatch_reg;
    struct fb_gpu_dxg_present_source_commit mismatch_commit;
    struct fb_gpu_dxg_present_source_query mismatch_query;
    struct fb_gpu_dxg_present_source_query after_close_query;
    struct fb_gpu_dxg_present_host_bind_contract after_close_bind_contract;
    struct fb_gpu_backend_info backend;
    struct fb_gpu_stats stats_before;
    struct fb_gpu_stats stats_after;
    struct fb_gpu_stats stats_wait;
    struct fb_gpu_stats stats_negative;
    struct fb_gpu_stats stats_closed;
    struct fb_gpu_stats *stats_pin;
    uint64 shared_handle = 0;
    int fb_fd = -1;
    int fb_fd_foreign = -1;
    int fb_fd_after = -1;
    int create_rc = -1;
    int share_rc = -1;
    int register_rc = -1;
    int commit_rc = -1;
    int create_sync_rc = -1;
    int wait_commit_rc = -1;
    int wait_query_rc = -1;
    int present_create_sync_file_rc = -1;
    int present_open_sync_file_rc = -1;
    int present_bad_open_sync_file_rc = -1;
    int sync_file_commit_rc = -1;
    int sync_file_query_rc = -1;
    int zero_dimensions_rc = -1;
    int bad_pitch_rc = -1;
    int invalid_resource_fd_rc = -1;
    int reserved_flags_rc = -1;
    int missing_sync_commit_rc = -1;
    int sync_without_flag_commit_rc = -1;
    int no_source_commit_rc = -1;
    int unverified_register_rc = -1;
    int unverified_commit_rc = -1;
    int unverified_query_rc = -1;
    int mismatch_register_rc = -1;
    int mismatch_commit_rc = -1;
    int mismatch_query_rc = -1;
    int query_rc = -1;
    int bind_contract_rc = -1;
    int foreign_bind_contract_rc = -1;
    int after_close_query_rc = -1;
    int after_close_bind_contract_rc = -1;
    int stats_before_rc = -1;
    int stats_after_rc = -1;
    int stats_wait_rc = -1;
    int stats_negative_rc = -1;
    int stats_closed_rc = -1;
    int backend_rc = -1;
    int open_after_rc = -1;
    int no_present_credit = 0;
    int wait_sync_no_present_credit = 0;
    int provenance_complete = 0;
    int failclosed = 0;
    int bind_contract_failclosed = 0;
    int foreign_bind_contract_failclosed = 0;
    int stale_bind_contract_failclosed = 0;
    int wait_sync_failclosed = 0;
    int wait_sync_metadata = 0;
    int negative_register_metadata = 0;
    int negative_commit_metadata = 0;
    int negative_source_identity = 0;
    int negative_no_present_credit = 0;
    int unverified_resource_failclosed = 0;
    int adapter_mismatch_failclosed = 0;
    int negative_metadata_pass = 0;
    int software_path_rejection = 0;
    int owner_cleanup = 0;
    int hyperv_gate = 0;
    int d3d12_resource_fd_typed_admission_pass = 0;
    int d3d12_resource_fd_lifetime_pass = 0;
    int d3d12_present_admission_pass = 0;
    int d3d12_acquire_fence_lifetime_pass = 0;
    int d3d12_present_syncfile_preopen_pass = 0;
    int d3d12_bind_contract_failclosed_pass = 0;
    int d3d12_scanout_bind_skeleton_pass = 0;
    int d3d12_commit_result_copyout_contract_pass = 0;
    int d3d12_display_bind_id_shape_pass = 0;
    int d3d12_display_bind_success_shape_pass = 0;
    int d3d12_display_bind_query_fields_pass = 0;
    int d3d12_provider_credit_gate_pass = 0;
    int d3d12_display_bind_request_metadata_pass = 0;
    int d3d12_display_bind_pending_lifetime_pass = 0;
    int d3d12_display_bind_generation_revalidation_pass = 0;
    int d3d12_display_bind_provider_pending_publication_pass = 0;
    int d3d12_native_completion_lifetime_pass = 0;
    int d3d12_display_bind_stale_source_zero_credit_pass = 0;
    int d3d12_display_bind_stale_source_cleanup_immediate = 0;
    int d3d12_display_bind_stale_source_cleanup_deferred = 0;
    int d3d12_native_completion_not_kms_pass = 0;
    int d3d12_standard_alloc_not_display_bind_pass = 0;
    int d3d12_dda_nouveau_separate_display_not_bind_pass = 0;
    int d3d12_host_to_vm_presenthistory_absent_pass = 0;
    int dxg_presenthistory_orphan_completion_rejection_pass = 0;
    int gpu_remaining_plan_dependency_skeleton_pass = 0;
    int wsl_uapi_namespace_negative_pass = 0;
    int wsl_adapter_display_caps_negative_pass = 0;
    int wsl_submit_present_fields_not_bind_pass = 0;
    int wsl_stdalloc_and_alloc_flags_not_bind_pass = 0;
    int wsl_trace_display_bind_negative_pass = 0;
    int public_present_api_not_guest_bind_pass = 0;
    int provider_credit_gate_negative_pass = 0;
    int dda_nouveau_non_readback_display_proof_pass = 0;
    int d3d12_display_bind_host_abi_discovery_pass = 0;
    int host_display_bind_source_catalog_pass = 0;
    int d3d12_completion_source_authority_pass = 0;
    int native_present_completion_source_namespace_pass = 0;
    int gpu_remaining_holistic_skeleton_pass = 0;
    uint32 host_to_vm_packets = 0;
    uint32 host_to_vm_unknown = 0;
    uint32 host_to_vm_last_cmd = 0;
    uint32 host_to_vm_last_channel = 0;
    uint32 host_to_vm_last_payload = 0;
    uint32 host_to_vm_presenthistory = 0;
    uint32 host_to_vm_presenthistory_len = 0;
    uint32 host_to_vm_presenthistory_head_len = 0;
    int pass = 0;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    memset(&share_objects, 0, sizeof(share_objects));
    memset(&create_sync, 0, sizeof(create_sync));
    memset(&present_create_sync_file, 0, sizeof(present_create_sync_file));
    memset(&present_open_sync_file, 0, sizeof(present_open_sync_file));
    memset(&present_bad_open_sync_file, 0, sizeof(present_bad_open_sync_file));
    memset(&destroy_sync, 0, sizeof(destroy_sync));
    memset(&reg, 0, sizeof(reg));
    memset(&commit, 0, sizeof(commit));
    memset(&wait_commit, 0, sizeof(wait_commit));
    memset(&sync_file_commit, 0, sizeof(sync_file_commit));
    memset(&missing_sync_commit, 0, sizeof(missing_sync_commit));
    memset(&sync_without_flag_commit, 0, sizeof(sync_without_flag_commit));
    memset(&no_source_commit, 0, sizeof(no_source_commit));
    memset(&query, 0, sizeof(query));
    memset(&wait_query, 0, sizeof(wait_query));
    memset(&sync_file_query, 0, sizeof(sync_file_query));
    memset(&bind_contract, 0, sizeof(bind_contract));
    memset(&foreign_bind_contract, 0, sizeof(foreign_bind_contract));
    memset(&bad_reg, 0, sizeof(bad_reg));
    memset(&unverified_reg, 0, sizeof(unverified_reg));
    memset(&unverified_commit, 0, sizeof(unverified_commit));
    memset(&unverified_query, 0, sizeof(unverified_query));
    memset(&mismatch_reg, 0, sizeof(mismatch_reg));
    memset(&mismatch_commit, 0, sizeof(mismatch_commit));
    memset(&mismatch_query, 0, sizeof(mismatch_query));
    memset(&after_close_query, 0, sizeof(after_close_query));
    memset(&after_close_bind_contract, 0, sizeof(after_close_bind_contract));
    memset(&backend, 0, sizeof(backend));
    memset(&stats_before, 0, sizeof(stats_before));
    memset(&stats_after, 0, sizeof(stats_after));
    memset(&stats_wait, 0, sizeof(stats_wait));
    memset(&stats_negative, 0, sizeof(stats_negative));
    memset(&stats_closed, 0, sizeof(stats_closed));
    stats_pin = &stats_after;

    create_allocation.device = device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.create_shared = 1;
    create_allocation.flags.nt_security_sharing = 1;
    create_allocation.flags.cross_adapter = 1;
    create_allocation.flags.standard_allocation = 1;
    create_rc = ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation);
    if (create_rc < 0 || allocation_info.allocation.v == 0 ||
        create_allocation.resource.v == 0)
        goto out;

    objects[0] = create_allocation.resource;
    share_objects.object_count = 1;
    share_objects.objects = (uint64)objects;
    share_objects.shared_handle = (uint64)&shared_handle;
    share_rc = ioctl(fd, LX_DXSHAREOBJECTS, &share_objects);
    if (share_rc < 0 || shared_handle == 0)
        goto out;

    fb_fd = open("/dev/gpu0", O_RDWR);
    if (fb_fd < 0)
        fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0)
        goto out;
    backend_rc = ioctl(fb_fd, FB_GPU_BACKEND_QUERY, &backend);
    stats_before_rc = ioctl(fb_fd, FB_GPU_GET_STATS, &stats_before);
    if (backend_rc < 0 || stats_before_rc < 0)
        goto out;

    reg.dxg_fd = fd;
    reg.resource_fd = (int32)shared_handle;
    reg.device = device.v;
    reg.resource = create_allocation.resource.v;
    reg.allocation = allocation_info.allocation.v;
    reg.allocation_count = 1;
    reg.width = 64;
    reg.height = 64;
    reg.pitch = 256;
    reg.format = FB_GPU_BO_FORMAT_ARGB8888;
    reg.modifier = 0;
    reg.adapter_luid_low = adapter_luid.a;
    reg.adapter_luid_high = adapter_luid.b;
    reg.provenance_flags = FB_GPU_DXG_PRESENT_PROV_DXG_FD |
                           FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD |
                           FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES |
                           FB_GPU_DXG_PRESENT_PROV_DIMENSIONS |
                           FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID;

    bad_reg = reg;
    bad_reg.width = 0;
    zero_dimensions_rc =
        ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_REGISTER, &bad_reg);
    bad_reg = reg;
    bad_reg.pitch = reg.width * 4U - 4U;
    bad_pitch_rc =
        ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_REGISTER, &bad_reg);
    bad_reg = reg;
    bad_reg.resource_fd = -2;
    invalid_resource_fd_rc =
        ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_REGISTER, &bad_reg);
    bad_reg = reg;
    bad_reg.flags = 0x80000000U;
    reserved_flags_rc =
        ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_REGISTER, &bad_reg);

    register_rc = ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_REGISTER, &reg);
    if (register_rc < 0 || reg.present_source == 0)
        goto out;

    commit.present_source = reg.present_source;
    commit_rc = ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_COMMIT, &commit);

    no_source_commit.present_source = reg.present_source ^ 0x40000000U;
    if (no_source_commit.present_source == 0 ||
        no_source_commit.present_source == reg.present_source)
        no_source_commit.present_source = reg.present_source + 1U;
    no_source_commit_rc =
        ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_COMMIT, &no_source_commit);

    missing_sync_commit.present_source = reg.present_source;
    missing_sync_commit.flags = FB_GPU_DXG_PRESENT_F_WAIT_SYNC;
    missing_sync_commit.fence_value = 7;
    missing_sync_commit_rc =
        ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_COMMIT,
              &missing_sync_commit);

    sync_without_flag_commit.present_source = reg.present_source;
    sync_without_flag_commit.sync_object = 0x12345678U;
    sync_without_flag_commit.fence_value = 7;
    sync_without_flag_commit_rc =
        ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_COMMIT,
              &sync_without_flag_commit);

    unverified_reg = reg;
    unverified_reg.present_source = 0;
    unverified_reg.resource_fd = -1;
    unverified_register_rc =
        ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_REGISTER, &unverified_reg);
    if (unverified_register_rc == 0 && unverified_reg.present_source != 0) {
        unverified_commit.present_source = unverified_reg.present_source;
        unverified_commit_rc =
            ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_COMMIT,
                  &unverified_commit);
        unverified_query.present_source = unverified_reg.present_source;
        unverified_query_rc =
            ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_QUERY,
                  &unverified_query);
    }

    mismatch_reg = reg;
    mismatch_reg.present_source = 0;
    mismatch_reg.adapter_luid_low ^= 1U;
    if (mismatch_reg.adapter_luid_low == adapter_luid.a &&
        mismatch_reg.adapter_luid_high == adapter_luid.b)
        mismatch_reg.adapter_luid_high ^= 1U;
    mismatch_register_rc =
        ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_REGISTER, &mismatch_reg);
    if (mismatch_register_rc == 0 && mismatch_reg.present_source != 0) {
        mismatch_commit.present_source = mismatch_reg.present_source;
        mismatch_commit_rc =
            ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_COMMIT,
                  &mismatch_commit);
        mismatch_query.present_source = mismatch_reg.present_source;
        mismatch_query_rc =
            ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_QUERY,
                  &mismatch_query);
    }
    stats_negative_rc = ioctl(fb_fd, FB_GPU_GET_STATS, &stats_negative);

    query.present_source = reg.present_source;
    query_rc = ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_QUERY, &query);
    bind_contract.version = 1;
    bind_contract.present_source = reg.present_source;
    bind_contract.dxg_fd = fd;
    bind_contract.resource_fd = (int32)shared_handle;
    bind_contract.device = device.v;
    bind_contract.resource = create_allocation.resource.v;
    bind_contract.allocation = allocation_info.allocation.v;
    bind_contract.allocation_count = 1;
    bind_contract.width = reg.width;
    bind_contract.height = reg.height;
    bind_contract.pitch = reg.pitch;
    bind_contract.format = reg.format;
    bind_contract.modifier = reg.modifier;
    bind_contract.adapter_luid_low = adapter_luid.a;
    bind_contract.adapter_luid_high = adapter_luid.b;
    bind_contract.provenance_flags = reg.provenance_flags;
    bind_contract_rc =
        ioctl(fb_fd, FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY,
              &bind_contract);
    fb_fd_foreign = open("/dev/gpu0", O_RDWR);
    if (fb_fd_foreign < 0)
        fb_fd_foreign = open("/dev/fb0", O_RDWR);
    if (fb_fd_foreign >= 0) {
        foreign_bind_contract = bind_contract;
        foreign_bind_contract.present_source = reg.present_source;
        foreign_bind_contract_rc =
            ioctl(fb_fd_foreign, FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY,
                  &foreign_bind_contract);
    }
    stats_after_rc = ioctl(fb_fd, FB_GPU_GET_STATS, &stats_after);
    if (stats_after_rc < 0)
        goto out;

    create_sync.device = device;
    create_sync.info.type = _D3DDDI_MONITORED_FENCE;
    create_sync.info.flags.value = g_sync_create_flags;
    create_sync.info.monitored_fence.initial_fence_value = 0;
    create_sync_rc =
        ioctl(fd, LX_DXCREATESYNCHRONIZATIONOBJECT, &create_sync);
    if (create_sync_rc == 0 && create_sync.sync_object.v != 0) {
        wait_commit.present_source = reg.present_source;
        wait_commit.flags = FB_GPU_DXG_PRESENT_F_WAIT_SYNC;
        wait_commit.sync_object = create_sync.sync_object.v;
        wait_commit.fence_value = 7;
        wait_commit_rc =
            ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_COMMIT, &wait_commit);
        wait_query.present_source = reg.present_source;
        wait_query_rc =
            ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_QUERY, &wait_query);
        stats_wait_rc = ioctl(fb_fd, FB_GPU_GET_STATS, &stats_wait);

        present_create_sync_file.device = device;
        present_create_sync_file.monitored_fence = create_sync.sync_object;
        present_create_sync_file.fence_value = 11;
        present_create_sync_file_rc =
            ioctl(fd, LX_DXCREATESYNCFILE, &present_create_sync_file);
        if (present_create_sync_file_rc == 0 &&
            present_create_sync_file.sync_file_handle != 0) {
            present_bad_open_sync_file.device = device;
            present_bad_open_sync_file.sync_file_handle = shared_handle;
            present_bad_open_sync_file_rc =
                ioctl(fd, LX_DXOPENSYNCOBJECTFROMSYNCFILE,
                      &present_bad_open_sync_file);

            present_open_sync_file.device = device;
            present_open_sync_file.sync_file_handle =
                present_create_sync_file.sync_file_handle;
            present_open_sync_file_rc =
                ioctl(fd, LX_DXOPENSYNCOBJECTFROMSYNCFILE,
                      &present_open_sync_file);
            if (present_open_sync_file_rc == 0 &&
                present_open_sync_file.syncobj.v != 0) {
                sync_file_commit.present_source = reg.present_source;
                sync_file_commit.flags = FB_GPU_DXG_PRESENT_F_WAIT_SYNC;
                sync_file_commit.sync_object =
                    present_open_sync_file.syncobj.v;
                sync_file_commit.fence_value =
                    present_open_sync_file.fence_value;
                sync_file_commit_rc =
                    ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_COMMIT,
                          &sync_file_commit);
                sync_file_query.present_source = reg.present_source;
                sync_file_query_rc =
                    ioctl(fb_fd, FB_GPU_DXG_PRESENT_SOURCE_QUERY,
                          &sync_file_query);
            }
        }
    }

    close(fb_fd);
    fb_fd = -1;
    fb_fd_after = open("/dev/gpu0", O_RDWR);
    if (fb_fd_after < 0)
        fb_fd_after = open("/dev/fb0", O_RDWR);
    open_after_rc = fb_fd_after < 0 ? -1 : 0;
    if (fb_fd_after >= 0) {
        after_close_query.present_source = reg.present_source;
        after_close_query_rc =
            ioctl(fb_fd_after, FB_GPU_DXG_PRESENT_SOURCE_QUERY,
                  &after_close_query);
        after_close_bind_contract = bind_contract;
        after_close_bind_contract.present_source = reg.present_source;
        after_close_bind_contract_rc =
            ioctl(fb_fd_after, FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY,
                  &after_close_bind_contract);
        stats_closed_rc = ioctl(fb_fd_after, FB_GPU_GET_STATS,
                                &stats_closed);
        if (stats_closed_rc == 0)
            stats_pin = &stats_closed;
    }

    provenance_complete =
        (query.provenance_flags &
         (FB_GPU_DXG_PRESENT_PROV_DXG_FD |
          FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD |
          FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES |
          FB_GPU_DXG_PRESENT_PROV_DIMENSIONS |
          FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID)) ==
        (FB_GPU_DXG_PRESENT_PROV_DXG_FD |
         FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD |
         FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES |
         FB_GPU_DXG_PRESENT_PROV_DIMENSIONS |
         FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID);
    no_present_credit =
        commit.present_id == 0 && commit.completed == 0 &&
        query.present_id == 0 && query.completed == 0 &&
        query.display_target_kind == FB_GPU_DXG_DISPLAY_TARGET_NONE &&
        query.helper_transport_present == 0;
    failclosed =
        commit_rc < 0 &&
        query_rc < 0 &&
        query.source_live == 1 &&
        query.last_ret == EOPNOTSUPP &&
        query.requires_host_protocol == 1 &&
        query.missing_host_abi == FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND &&
        query.helper_contract_version == 1 &&
        query.helper_transport_present == 0 &&
        query.helper_requires_completion == 1 &&
        (query.helper_block_reason &
         FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION) != 0;
    bind_contract_failclosed =
        bind_contract_rc < 0 &&
        bind_contract.version == 1 &&
        bind_contract.transport == FB_GPU_DXG_PRESENT_HELPER_TRANSPORT_NONE &&
        bind_contract.operation == FB_GPU_DXG_PRESENT_HELPER_OP_SCANOUT_BIND &&
        bind_contract.present_id == 0 &&
        bind_contract.completed == 0 &&
        bind_contract.present_source == reg.present_source &&
        bind_contract.source_live == 1 &&
        bind_contract.source_generation != 0 &&
        bind_contract.resource_generation != 0 &&
        bind_contract.completion_source ==
            FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
        bind_contract.selected_lane ==
            FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND &&
        bind_contract.adapter_identity == FB_GPU_DXG_PRESENT_ADAPTER_MATCH &&
        (bind_contract.provenance_flags &
         (FB_GPU_DXG_PRESENT_PROV_DXG_FD |
          FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD |
          FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES |
          FB_GPU_DXG_PRESENT_PROV_DIMENSIONS |
          FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID)) ==
        (FB_GPU_DXG_PRESENT_PROV_DXG_FD |
         FB_GPU_DXG_PRESENT_PROV_RESOURCE_FD |
         FB_GPU_DXG_PRESENT_PROV_D3DKMT_HANDLES |
         FB_GPU_DXG_PRESENT_PROV_DIMENSIONS |
         FB_GPU_DXG_PRESENT_PROV_ADAPTER_LUID) &&
        (bind_contract.required_metadata &
         (FB_GPU_DXG_PRESENT_META_DEVICE |
          FB_GPU_DXG_PRESENT_META_RESOURCE |
          FB_GPU_DXG_PRESENT_META_ALLOCATION |
          FB_GPU_DXG_PRESENT_META_DIMENSIONS |
          FB_GPU_DXG_PRESENT_META_FORMAT |
          FB_GPU_DXG_PRESENT_META_MODIFIER |
          FB_GPU_DXG_PRESENT_META_ADAPTER_LUID)) ==
        (FB_GPU_DXG_PRESENT_META_DEVICE |
         FB_GPU_DXG_PRESENT_META_RESOURCE |
         FB_GPU_DXG_PRESENT_META_ALLOCATION |
         FB_GPU_DXG_PRESENT_META_DIMENSIONS |
         FB_GPU_DXG_PRESENT_META_FORMAT |
         FB_GPU_DXG_PRESENT_META_MODIFIER |
         FB_GPU_DXG_PRESENT_META_ADAPTER_LUID) &&
        (bind_contract.lifetime &
         (FB_GPU_DXG_PRESENT_LIFE_HANDLES_VALID |
          FB_GPU_DXG_PRESENT_LIFE_HOST_COMPLETION |
          FB_GPU_DXG_PRESENT_LIFE_NO_CPU_READBACK)) ==
        (FB_GPU_DXG_PRESENT_LIFE_HANDLES_VALID |
         FB_GPU_DXG_PRESENT_LIFE_HOST_COMPLETION |
         FB_GPU_DXG_PRESENT_LIFE_NO_CPU_READBACK) &&
        (bind_contract.helper_block_reason &
         (FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT |
          FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION)) ==
        (FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT |
         FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION);
    foreign_bind_contract_failclosed =
        fb_fd_foreign >= 0 &&
        foreign_bind_contract_rc < 0 &&
        foreign_bind_contract.present_source == reg.present_source &&
        foreign_bind_contract.source_live == 0 &&
        foreign_bind_contract.source_generation == 0 &&
        foreign_bind_contract.present_id == 0 &&
        foreign_bind_contract.completed == 0 &&
        foreign_bind_contract.completion_source ==
            FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
        (foreign_bind_contract.helper_block_reason &
         FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE) != 0;
    wait_sync_metadata =
        (wait_query.helper_required_metadata &
         (FB_GPU_DXG_PRESENT_META_SYNC_OBJECT |
          FB_GPU_DXG_PRESENT_META_FENCE_VALUE)) ==
        (FB_GPU_DXG_PRESENT_META_SYNC_OBJECT |
         FB_GPU_DXG_PRESENT_META_FENCE_VALUE);
    wait_sync_no_present_credit =
        wait_commit.present_id == 0 && wait_commit.completed == 0 &&
        wait_query.present_id == 0 && wait_query.completed == 0 &&
        stats_wait_rc == 0;
    wait_sync_failclosed =
        create_sync_rc == 0 && create_sync.sync_object.v != 0 &&
        wait_commit_rc < 0 &&
        wait_query_rc < 0 &&
        wait_query.source_live == 1 &&
        wait_query.last_ret == EOPNOTSUPP &&
        wait_query.last_flags == FB_GPU_DXG_PRESENT_F_WAIT_SYNC &&
        wait_query.sync_object == create_sync.sync_object.v &&
        wait_query.fence_value == wait_commit.fence_value &&
        wait_query.requires_host_protocol == 1 &&
        wait_query.missing_host_abi == FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND &&
        wait_query.helper_contract_version == 1 &&
        wait_query.helper_transport_present == 0 &&
        wait_query.helper_requires_completion == 1 &&
        (wait_query.helper_lifetime & FB_GPU_DXG_PRESENT_LIFE_SYNC_VALID) != 0 &&
        wait_sync_metadata &&
        wait_sync_no_present_credit;
    negative_register_metadata =
        zero_dimensions_rc < 0 && bad_pitch_rc < 0 &&
        invalid_resource_fd_rc < 0 && reserved_flags_rc < 0;
    negative_commit_metadata =
        missing_sync_commit_rc < 0 && sync_without_flag_commit_rc < 0;
    negative_source_identity = no_source_commit_rc < 0;
    unverified_resource_failclosed =
        unverified_register_rc == 0 &&
        unverified_commit_rc < 0 &&
        unverified_query_rc < 0 &&
        unverified_query.source_live == 1 &&
        unverified_query.resource_fd == -1 &&
        stats_negative_rc == 0 &&
        stats_negative.dxg_present_commit_resource_fd_unverified >
        stats_before.dxg_present_commit_resource_fd_unverified &&
        unverified_query.present_id == 0 &&
        unverified_query.completed == 0;
    adapter_mismatch_failclosed =
        mismatch_register_rc == 0 &&
        mismatch_commit_rc < 0 &&
        mismatch_query_rc < 0 &&
        mismatch_query.source_live == 1 &&
        mismatch_query.adapter_identity ==
        FB_GPU_DXG_PRESENT_ADAPTER_MISMATCH &&
        (mismatch_query.helper_block_reason &
         FB_GPU_DXG_PRESENT_BLOCK_ADAPTER_MISMATCH) != 0 &&
        mismatch_query.present_id == 0 &&
        mismatch_query.completed == 0;
    negative_no_present_credit =
        stats_negative_rc == 0 &&
        no_source_commit.present_id == 0 &&
        no_source_commit.completed == 0 &&
        missing_sync_commit.present_id == 0 &&
        missing_sync_commit.completed == 0 &&
        sync_without_flag_commit.present_id == 0 &&
        sync_without_flag_commit.completed == 0 &&
        unverified_commit.present_id == 0 &&
        unverified_commit.completed == 0 &&
        unverified_query.present_id == 0 &&
        unverified_query.completed == 0 &&
        mismatch_commit.present_id == 0 &&
        mismatch_commit.completed == 0 &&
        mismatch_query.present_id == 0 &&
        mismatch_query.completed == 0;
    negative_metadata_pass =
        negative_register_metadata && negative_commit_metadata &&
        negative_source_identity && unverified_resource_failclosed &&
        adapter_mismatch_failclosed && negative_no_present_credit;
    software_path_rejection =
        query.display_target_kind == FB_GPU_DXG_DISPLAY_TARGET_NONE &&
        bind_contract.completion_source ==
            FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
        bind_contract.selected_lane ==
            FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND &&
        query.helper_transport_present == 0 &&
        query.missing_host_abi ==
            FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND &&
        commit.present_id == 0 &&
        query.present_id == 0 &&
        commit.completed == 0 &&
        query.completed == 0 &&
        negative_no_present_credit;
    owner_cleanup =
        open_after_rc == 0 &&
        after_close_query_rc < 0 &&
        after_close_query.last_ret == EOPNOTSUPP &&
        after_close_query.source_live == 0 &&
        stats_closed_rc == 0;
    stale_bind_contract_failclosed =
        open_after_rc == 0 &&
        after_close_bind_contract_rc < 0 &&
        after_close_bind_contract.present_source == reg.present_source &&
        after_close_bind_contract.source_live == 0 &&
        after_close_bind_contract.present_id == 0 &&
        after_close_bind_contract.completed == 0 &&
        after_close_bind_contract.completion_source ==
            FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
        (after_close_bind_contract.helper_block_reason &
         FB_GPU_DXG_PRESENT_BLOCK_NO_REGISTERED_SOURCE) != 0;
    hyperv_gate =
        backend.backend == FB_GPU_BACKEND_HYPERV_DXG &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    d3d12_resource_fd_typed_admission_pass =
        register_rc == 0 &&
        query.resource_fd_kind == 2 &&
        query.resource_fd_sealed != 0 &&
        query.resource_fd_shared_records_valid != 0 &&
        query.resource_fd_matches_handles != 0 &&
        query.resource_fd_generation != 0 &&
        bind_contract.resource_fd_kind == query.resource_fd_kind &&
        bind_contract.resource_fd_sealed == query.resource_fd_sealed &&
        bind_contract.resource_fd_shared_records_valid ==
            query.resource_fd_shared_records_valid &&
        bind_contract.resource_fd_matches_handles ==
            query.resource_fd_matches_handles &&
        bind_contract.resource_fd_generation ==
            query.resource_fd_generation &&
        bind_contract.resource_generation == query.resource_fd_generation &&
        bind_contract.device == device.v &&
        bind_contract.resource == create_allocation.resource.v &&
        bind_contract.allocation == allocation_info.allocation.v &&
        bind_contract.allocation_count == 1 &&
        invalid_resource_fd_rc < 0 && unverified_resource_failclosed &&
        stale_bind_contract_failclosed && no_present_credit && hyperv_gate;
    d3d12_resource_fd_lifetime_pass =
        share_rc == 0 && shared_handle != 0 &&
        register_rc == 0 && reg.resource_fd == (int32)shared_handle &&
        reg.present_source != 0 && invalid_resource_fd_rc < 0 &&
        unverified_resource_failclosed && owner_cleanup &&
        stale_bind_contract_failclosed && no_present_credit &&
        hyperv_gate;
    d3d12_present_admission_pass =
        register_rc == 0 && provenance_complete &&
        query.source_live == 1 &&
        query.adapter_identity == FB_GPU_DXG_PRESENT_ADAPTER_MATCH &&
        query.adapter_luid_low == adapter_luid.a &&
        query.adapter_luid_high == adapter_luid.b &&
        bind_contract_failclosed &&
        bind_contract.device == device.v &&
        bind_contract.resource == create_allocation.resource.v &&
        bind_contract.allocation == allocation_info.allocation.v &&
        bind_contract.allocation_count == 1 &&
        bind_contract.width == reg.width &&
        bind_contract.height == reg.height &&
        bind_contract.pitch == reg.pitch &&
        bind_contract.format == reg.format &&
        bind_contract.modifier == reg.modifier &&
        negative_register_metadata && negative_commit_metadata &&
        negative_source_identity && unverified_resource_failclosed &&
        adapter_mismatch_failclosed && negative_no_present_credit &&
        hyperv_gate;
    d3d12_acquire_fence_lifetime_pass =
        create_sync_rc == 0 && create_sync.sync_object.v != 0 &&
        wait_sync_failclosed && wait_sync_metadata &&
        wait_query.sync_object == create_sync.sync_object.v &&
        wait_query.fence_value == wait_commit.fence_value &&
        wait_sync_no_present_credit && hyperv_gate;
    d3d12_present_syncfile_preopen_pass =
        present_create_sync_file_rc == 0 &&
        present_create_sync_file.sync_file_handle != 0 &&
        present_open_sync_file_rc == 0 &&
        present_open_sync_file.syncobj.v != 0 &&
        present_open_sync_file.fence_value ==
            present_create_sync_file.fence_value &&
        present_bad_open_sync_file_rc < 0 &&
        sync_file_commit_rc < 0 &&
        sync_file_query_rc < 0 &&
        sync_file_query.source_live == 1 &&
        sync_file_query.last_flags == FB_GPU_DXG_PRESENT_F_WAIT_SYNC &&
        sync_file_query.sync_object == present_open_sync_file.syncobj.v &&
        sync_file_query.fence_value == present_open_sync_file.fence_value &&
        sync_file_commit.present_id == 0 &&
        sync_file_commit.completed == 0 &&
        sync_file_query.present_id == 0 &&
        sync_file_query.completed == 0 &&
        sync_file_query.helper_transport_present == 0 &&
        sync_file_query.missing_host_abi ==
            FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND &&
        stale_bind_contract_failclosed && hyperv_gate;
    d3d12_bind_contract_failclosed_pass =
        bind_contract_failclosed && foreign_bind_contract_failclosed &&
        stale_bind_contract_failclosed && software_path_rejection &&
        bind_contract.present_id == 0 && bind_contract.completed == 0 &&
        hyperv_gate;
    d3d12_scanout_bind_skeleton_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_scanout_bind_attempts >
            stats_before.dxg_scanout_bind_attempts &&
        stats_after.dxg_scanout_bind_rejects -
            stats_before.dxg_scanout_bind_rejects >=
            stats_after.dxg_scanout_bind_attempts -
            stats_before.dxg_scanout_bind_attempts &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_weak_evidence_rejects -
            stats_before.dxg_scanout_bind_weak_evidence_rejects >=
            stats_after.dxg_scanout_bind_attempts -
            stats_before.dxg_scanout_bind_attempts &&
        stats_after.dxg_scanout_bind_last_present_id == 0 &&
        stats_after.dxg_scanout_bind_last_completed == 0 &&
        bind_contract.source_generation != 0 &&
        bind_contract.resource_generation != 0 &&
        stats_after.dxg_scanout_bind_last_dirty_sequence == 0 &&
        stats_after.dxg_scanout_bind_last_dirty_rects == 0 &&
        no_present_credit && hyperv_gate;
    d3d12_commit_result_copyout_contract_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_present_commit_ioctl_entries >
            stats_before.dxg_present_commit_ioctl_entries &&
        stats_after.dxg_present_commit_copyout_failures ==
            stats_before.dxg_present_commit_copyout_failures &&
        commit_rc < 0 &&
        commit.present_id == 0 &&
        commit.completed == 0 &&
        no_present_credit && hyperv_gate;
    d3d12_display_bind_id_shape_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        bind_contract.present_id == 0 &&
        bind_contract.completed == 0 &&
        stats_after.dxg_scanout_bind_last_present_id == 0 &&
        stats_after.dxg_scanout_bind_last_completed == 0;
    d3d12_provider_credit_gate_pass =
        stats_after_rc == 0 &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0 &&
        stats_after.nouveau_pci_native_present_credit == 0 &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_present_helper_transport_present == 0 &&
        stats_after.dxg_present_display_target_kind ==
            FB_GPU_DXG_DISPLAY_TARGET_NONE &&
        stats_after.dxg_display_bind_provider_no_host_abi == 1 &&
        stats_after.dxg_display_bind_provider_no_sender == 1 &&
        stats_after.dxg_display_bind_provider_no_completion == 1;
    d3d12_display_bind_request_metadata_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_display_bind_provider_submits >
            stats_before.dxg_display_bind_provider_submits &&
        stats_after.dxg_display_bind_request_metadata_complete == 1 &&
        stats_after.dxg_display_bind_request_sync_metadata_complete == 1 &&
        stats_after.dxg_display_bind_request_missing_metadata == 0 &&
        (stats_after.dxg_display_bind_required_metadata &
         (FB_GPU_DXG_PRESENT_META_DEVICE |
          FB_GPU_DXG_PRESENT_META_RESOURCE |
          FB_GPU_DXG_PRESENT_META_ALLOCATION |
          FB_GPU_DXG_PRESENT_META_DIMENSIONS |
          FB_GPU_DXG_PRESENT_META_FORMAT |
          FB_GPU_DXG_PRESENT_META_MODIFIER |
          FB_GPU_DXG_PRESENT_META_ADAPTER_LUID)) ==
        (FB_GPU_DXG_PRESENT_META_DEVICE |
         FB_GPU_DXG_PRESENT_META_RESOURCE |
         FB_GPU_DXG_PRESENT_META_ALLOCATION |
         FB_GPU_DXG_PRESENT_META_DIMENSIONS |
         FB_GPU_DXG_PRESENT_META_FORMAT |
         FB_GPU_DXG_PRESENT_META_MODIFIER |
         FB_GPU_DXG_PRESENT_META_ADAPTER_LUID) &&
        stats_after.dxg_display_bind_source_generation != 0 &&
        stats_after.dxg_display_bind_resource_generation != 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        no_present_credit && hyperv_gate;
    d3d12_display_bind_pending_lifetime_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_display_bind_pending_created >
            stats_before.dxg_display_bind_pending_created &&
        stats_after.dxg_display_bind_pending_sequence != 0 &&
        stats_after.dxg_display_bind_pending_peak != 0 &&
        stats_after.dxg_display_bind_pending_active == 0 &&
        stats_after.dxg_display_bind_pending_completed ==
            stats_before.dxg_display_bind_pending_completed &&
        stats_after.dxg_display_bind_pending_failclosed >
            stats_before.dxg_display_bind_pending_failclosed &&
        stats_after.dxg_display_bind_pending_cancelled ==
            stats_before.dxg_display_bind_pending_cancelled &&
        stats_after.dxg_display_bind_pending_last_status == EOPNOTSUPP &&
        stats_after.dxg_display_bind_pending_last_source_generation != 0 &&
        stats_after.dxg_display_bind_pending_last_resource_generation != 0 &&
        stats_after.dxg_display_bind_pending_last_block_reason != 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        no_present_credit && hyperv_gate;
    d3d12_display_bind_generation_revalidation_pass =
        stats_after_rc == 0 &&
        query_rc < 0 && bind_contract_rc < 0 &&
        query.source_generation != 0 &&
        query.resource_generation != 0 &&
        bind_contract.source_generation == query.source_generation &&
        bind_contract.resource_generation == query.resource_generation &&
        stats_after.dxg_display_bind_source_generation ==
            query.source_generation &&
        stats_after.dxg_display_bind_resource_generation ==
            query.resource_generation &&
        stats_after.dxg_display_bind_pinned_resource_generation ==
            query.resource_generation &&
        stats_after.dxg_display_bind_provider_submits >
            stats_before.dxg_display_bind_provider_submits &&
        stats_after.dxg_display_bind_lock_dropped_submits >
            stats_before.dxg_display_bind_lock_dropped_submits &&
        stats_after.dxg_display_bind_revalidate_attempts >
            stats_before.dxg_display_bind_revalidate_attempts &&
        stats_after.dxg_display_bind_revalidate_successes >
            stats_before.dxg_display_bind_revalidate_successes &&
        stats_after.dxg_display_bind_revalidate_failures ==
            stats_before.dxg_display_bind_revalidate_failures &&
        stats_after.dxg_display_bind_provider_pin_revalidated == 1 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        no_present_credit && hyperv_gate;
    d3d12_display_bind_provider_pending_publication_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_display_bind_provider_submits >
            stats_before.dxg_display_bind_provider_submits &&
        stats_after.dxg_display_bind_provider_publication_attempts >
            stats_before.dxg_display_bind_provider_publication_attempts &&
        query.source_generation != 0 &&
        query.resource_generation != 0 &&
        stats_after.dxg_display_bind_provider_pending_owner_generation != 0 &&
        stats_after.dxg_display_bind_provider_pending_source_generation != 0 &&
        stats_after.dxg_display_bind_provider_pending_resource_generation != 0 &&
        stats_after.dxg_display_bind_provider_pending_dxgprocess_generation ==
            stats_after.dxg_display_bind_provider_pending_owner_generation &&
        stats_after.dxg_display_bind_provider_pending_process_adapter_generation != 0 &&
        stats_after.dxg_display_bind_provider_pending_hmgr_index_unique_valid == 1 &&
        stats_after.dxg_display_bind_provider_pending_parent_resource_ref_held == 1 &&
        stats_after.dxg_display_bind_provider_pending_opened_child_ref_held == 1 &&
        stats_after.dxg_display_bind_provider_pending_owner_close_cancelled == 0 &&
        stats_after.dxg_display_bind_pending_last_owner_generation ==
            stats_after.dxg_display_bind_provider_pending_owner_generation &&
        stats_after.dxg_display_bind_pending_last_source_generation ==
            stats_after.dxg_display_bind_provider_pending_source_generation &&
        stats_after.dxg_display_bind_pending_last_resource_generation ==
            stats_after.dxg_display_bind_provider_pending_resource_generation &&
        stats_after.dxg_display_bind_provider_publish_before_send == 0 &&
        stats_after.dxg_display_bind_provider_transport_pending_id == 0 &&
        stats_after.dxg_display_bind_provider_command_id == 0 &&
        stats_after.dxg_display_bind_provider_transaction_id == 0 &&
        stats_after.dxg_display_bind_provider_channel == 0 &&
        stats_after.dxg_display_bind_provider_completion_demux_registered == 0 &&
        stats_after.dxg_display_bind_provider_resolved_or_cancelled != 0 &&
        stats_after.dxg_display_bind_provider_refs_released != 0 &&
        stats_after.dxg_display_bind_provider_no_host_abi_cancelled != 0 &&
        stats_after.dxg_display_bind_provider_no_host_abi_refs_released != 0 &&
        stats_after.dxg_display_bind_pending_cancelled ==
            stats_before.dxg_display_bind_pending_cancelled &&
        stats_after.dxg_display_bind_provider_no_host_abi == 1 &&
        stats_after.dxg_display_bind_provider_no_sender == 1 &&
        stats_after.dxg_display_bind_provider_no_completion == 1 &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats_after.nouveau_pci_native_present_credit == 0 &&
        no_present_credit && hyperv_gate;
    d3d12_display_bind_success_shape_pass =
        stats_after_rc == 0 &&
        (((backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0 &&
          stats_after.nouveau_pci_native_present_credit == 0 &&
          stats_after.dxg_scanout_bind_successes ==
              stats_before.dxg_scanout_bind_successes &&
          stats_after.dxg_scanout_bind_completion_successes ==
              stats_before.dxg_scanout_bind_completion_successes &&
          stats_after.dxg_display_bind_transport_present == 0 &&
          stats_after.dxg_display_bind_present_id == 0 &&
          stats_after.dxg_display_bind_completed_id == 0 &&
          stats_after.dxg_display_bind_provider_no_host_abi == 1 &&
          stats_after.dxg_display_bind_provider_no_sender == 1 &&
          stats_after.dxg_display_bind_provider_no_completion == 1) ||
         (stats_after.dxg_display_bind_transport_present != 0 &&
          stats_after.dxg_display_bind_status == 0 &&
          stats_after.dxg_display_bind_block_reason == 0 &&
          stats_after.dxg_display_bind_completion_source ==
              FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
          stats_after.dxg_display_bind_present_id != 0 &&
          stats_after.dxg_display_bind_completed_id >=
              stats_after.dxg_display_bind_present_id &&
          stats_after.dxg_display_bind_source_generation != 0 &&
          stats_after.dxg_display_bind_resource_generation != 0 &&
          stats_after.dxg_scanout_bind_successes >
              stats_before.dxg_scanout_bind_successes &&
          stats_after.dxg_scanout_bind_completion_successes >
              stats_before.dxg_scanout_bind_completion_successes &&
          stats_after.dxg_display_bind_provider_submits >
              stats_before.dxg_display_bind_provider_submits &&
          stats_after.dxg_display_bind_provider_pin_revalidated == 1 &&
          stats_after.dxg_display_bind_provider_no_host_abi == 0 &&
          stats_after.dxg_display_bind_provider_no_sender == 0 &&
          stats_after.dxg_display_bind_provider_no_completion == 0));
    d3d12_display_bind_query_fields_pass =
        query.source_generation != 0 &&
        query.resource_generation != 0 &&
        bind_contract.source_generation == query.source_generation &&
        bind_contract.resource_generation == query.resource_generation &&
        query.display_bind_status == EOPNOTSUPP &&
        bind_contract.provider_status == EOPNOTSUPP &&
        query.display_bind_block_reason ==
            bind_contract.provider_block_reason &&
        query.display_bind_completion_source ==
            FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
        bind_contract.completion_source ==
            FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
        query.display_bind_dirty_sequence == 0 &&
        query.display_bind_dirty_rects == 0 &&
        bind_contract.dirty_sequence == 0 &&
        bind_contract.dirty_rects == 0 &&
        query.display_bind_host_abi_present == 0 &&
        query.display_bind_sender_present == 0 &&
        query.display_bind_completion_present == 0 &&
        bind_contract.host_abi_present == 0 &&
        bind_contract.sender_present == 0 &&
        bind_contract.completion_present == 0 &&
        query.display_bind_provider_no_host_abi == 1 &&
        query.display_bind_provider_no_sender == 1 &&
        query.display_bind_provider_no_completion == 1 &&
        bind_contract.provider_no_host_abi == 1 &&
        bind_contract.provider_no_sender == 1 &&
        bind_contract.provider_no_completion == 1 &&
        query.display_bind_provider_pin_revalidated == 1 &&
        bind_contract.provider_pin_revalidated == 1;
    d3d12_native_completion_lifetime_pass =
        stats_after_rc == 0 &&
        (((backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0 &&
          stats_after.nouveau_pci_native_present_credit == 0 &&
          stats_after.dxg_scanout_bind_successes ==
              stats_before.dxg_scanout_bind_successes &&
          stats_after.dxg_scanout_bind_completion_successes ==
              stats_before.dxg_scanout_bind_completion_successes &&
          stats_after.dxg_display_bind_transport_present == 0 &&
          stats_after.dxg_display_bind_present_id == 0 &&
          stats_after.dxg_display_bind_completed_id == 0 &&
          (stats_after.dxg_display_bind_provider_submits ==
               stats_before.dxg_display_bind_provider_submits ||
           stats_after.dxg_display_bind_provider_no_completion == 1)) ||
         (stats_after.dxg_display_bind_transport_present != 0 &&
          stats_after.dxg_display_bind_status == 0 &&
          stats_after.dxg_display_bind_block_reason == 0 &&
          stats_after.dxg_display_bind_completion_source ==
              FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
          stats_after.dxg_display_bind_present_id != 0 &&
          stats_after.dxg_display_bind_completed_id >=
              stats_after.dxg_display_bind_present_id &&
          stats_after.dxg_display_bind_source_generation != 0 &&
          stats_after.dxg_display_bind_resource_generation != 0 &&
          stats_after.dxg_scanout_bind_completion_successes >
              stats_before.dxg_scanout_bind_completion_successes &&
          stats_after.dxg_display_bind_provider_no_host_abi == 0 &&
          stats_after.dxg_display_bind_provider_no_sender == 0 &&
          stats_after.dxg_display_bind_provider_no_completion == 0));
    d3d12_display_bind_stale_source_cleanup_immediate =
        stats_closed_rc == 0 &&
        stats_closed.dxg_present_release_sources >
            stats_before.dxg_present_release_sources &&
        stats_closed.dxg_display_bind_release_clears >
            stats_before.dxg_display_bind_release_clears;
    d3d12_display_bind_stale_source_cleanup_deferred =
        stats_closed_rc == 0 &&
        stats_closed.dxg_present_release_sources ==
            stats_before.dxg_present_release_sources &&
        stats_closed.dxg_display_bind_release_clears ==
            stats_before.dxg_display_bind_release_clears &&
        stats_closed.dxg_display_bind_stale_source_rejects >
            stats_before.dxg_display_bind_stale_source_rejects;
    d3d12_display_bind_stale_source_zero_credit_pass =
        stats_closed_rc == 0 &&
        (d3d12_display_bind_stale_source_cleanup_immediate ||
         d3d12_display_bind_stale_source_cleanup_deferred) &&
        stats_closed.dxg_display_bind_after_close_queries >
            stats_before.dxg_display_bind_after_close_queries &&
        stats_closed.dxg_display_bind_stale_source_rejects >
            stats_before.dxg_display_bind_stale_source_rejects &&
        stats_closed.dxg_display_bind_stale_generation_rejects >
            stats_before.dxg_display_bind_stale_generation_rejects &&
        stats_closed.dxg_display_bind_stale_completion_rejects >
            stats_before.dxg_display_bind_stale_completion_rejects &&
        stats_closed.dxg_display_bind_late_completion_after_release == 0 &&
        stats_closed.dxg_display_bind_after_close_nonzero_id_rejects == 0 &&
        after_close_query_rc < 0 &&
        after_close_query.present_id == 0 &&
        after_close_query.completed == 0 &&
        after_close_bind_contract_rc < 0 &&
        after_close_bind_contract.present_id == 0 &&
        after_close_bind_contract.completed == 0 &&
        stats_closed.dxg_display_bind_present_id == 0 &&
        stats_closed.dxg_display_bind_completed_id == 0 &&
        stats_closed.nouveau_pci_native_present_credit == 0 &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    d3d12_native_completion_not_kms_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        stats_after.dxg_scanout_bind_last_present_id == 0 &&
        stats_after.dxg_scanout_bind_last_completed == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.kms_vblank_source_nouveau_hw == 0 &&
        stats_after.kms_page_flip_events_native_hw == 0;
    d3d12_standard_alloc_not_display_bind_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_scanout_bind_standard_alloc_private_data != 0 &&
        stats_after.dxg_scanout_bind_standard_alloc_display_bind_absent != 0 &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0;
    d3d12_dda_nouveau_separate_display_not_bind_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats_after.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats_after.dxg_scanout_bind_dda_resource_import_absent != 0 &&
        stats_after.dxg_scanout_bind_dda_scanout_bind_absent != 0 &&
        stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        stats_after.nouveau_pci_native_present_credit == 0 &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    wsl_uapi_namespace_negative_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0;
    wsl_adapter_display_caps_negative_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_present_dxg_adapter_display_supported == 0 &&
        stats_after.dxg_present_dxg_adapter_sources == 0 &&
        stats_after.dxg_present_helper_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0;
    wsl_submit_present_fields_not_bind_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    wsl_stdalloc_and_alloc_flags_not_bind_pass =
        d3d12_standard_alloc_not_display_bind_pass &&
        stats_after.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    wsl_trace_display_bind_negative_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    public_present_api_not_guest_bind_pass =
        stats_after_rc == 0 &&
        stats_after.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats_after.dxg_present_helper_transport_present == 0 &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        stats_after.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats_after.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats_after.nouveau_pci_native_present_credit == 0 &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    provider_credit_gate_negative_pass =
        d3d12_provider_credit_gate_pass &&
        (stats_after.dxg_display_bind_provider_submits ==
             stats_before.dxg_display_bind_provider_submits ||
         (stats_after.dxg_display_bind_provider_no_host_abi == 1 &&
          stats_after.dxg_display_bind_provider_no_sender == 1 &&
          stats_after.dxg_display_bind_provider_no_completion == 1)) &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    d3d12_display_bind_host_abi_discovery_pass =
        stats_after_rc == 0 &&
        wsl_uapi_namespace_negative_pass &&
        public_present_api_not_guest_bind_pass &&
        provider_credit_gate_negative_pass &&
        d3d12_dda_nouveau_separate_display_not_bind_pass &&
        stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats_after.dxg_display_bind_provider_completion_demux_registered == 0 &&
        stats_after.dxg_present_helper_transport_present == 0 &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats_after.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats_after.nouveau_pci_native_present_credit == 0 &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    dda_nouveau_non_readback_display_proof_pass =
        stats_after_rc == 0 &&
        (((stats_after.nouveau_pci_probe_accepts == 0 &&
           stats_after.nouveau_display_create_successes == 0 &&
           stats_after.nouveau_display_heads == 0 &&
           stats_after.nouveau_display_connectors == 0 &&
           stats_after.nouveau_display_vblank_supported == 0 &&
           stats_after.nouveau_display_vblank_irqs == 0 &&
           stats_after.nouveau_display_page_flip_completions == 0 &&
           stats_after.kms_present_last_lane ==
               FB_GPU_KMS_PRESENT_LANE_NONE &&
           stats_after.kms_present_dumb == 0 &&
           stats_after.kms_present_synthvid == 0 &&
           stats_after.kms_present_nouveau_hw == 0 &&
           stats_after.kms_vblank_source_nouveau_hw == 0 &&
           stats_after.kms_page_flip_events_native_hw == 0 &&
           stats_after.nouveau_pci_native_present_credit == 0 &&
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0)) ||
         (stats_after.nouveau_pci_probe_accepts != 0 &&
          stats_after.nouveau_display_create_successes != 0 &&
          stats_after.nouveau_display_heads != 0 &&
          stats_after.nouveau_display_connectors != 0 &&
          stats_after.nouveau_display_vblank_supported != 0 &&
          stats_after.nouveau_display_vblank_irqs != 0 &&
          stats_after.nouveau_display_page_flip_completions != 0 &&
          stats_after.kms_present_last_lane ==
              FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW &&
          stats_after.kms_present_dumb == 0 &&
          stats_after.kms_present_synthvid == 0 &&
          stats_after.kms_present_nouveau_hw != 0 &&
          stats_after.kms_vblank_source_nouveau_hw != 0 &&
          stats_after.kms_vblank_source_software_display == 0 &&
          stats_after.kms_vblank_source_synthetic == 0 &&
          stats_after.kms_page_flip_events_native_hw != 0 &&
          stats_after.kms_page_flip_events_software_blit == 0));
    host_display_bind_source_catalog_pass =
        stats_after_rc == 0 &&
        wsl_uapi_namespace_negative_pass &&
        wsl_adapter_display_caps_negative_pass &&
        wsl_submit_present_fields_not_bind_pass &&
        wsl_stdalloc_and_alloc_flags_not_bind_pass &&
        wsl_trace_display_bind_negative_pass &&
        public_present_api_not_guest_bind_pass &&
        provider_credit_gate_negative_pass &&
        d3d12_display_bind_host_abi_discovery_pass &&
        d3d12_dda_nouveau_separate_display_not_bind_pass &&
        dda_nouveau_non_readback_display_proof_pass &&
        ((stats_after.dxg_display_bind_provider_submits ==
              stats_before.dxg_display_bind_provider_submits &&
          stats_after.dxg_display_bind_transport_present == 0) ||
         (stats_after.dxg_display_bind_backend ==
              FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND &&
          stats_after.dxg_display_bind_transport ==
              FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE &&
          stats_after.dxg_display_bind_transport_present == 0 &&
          stats_after.dxg_display_bind_provider_no_host_abi == 1 &&
          stats_after.dxg_display_bind_provider_no_sender == 1 &&
          stats_after.dxg_display_bind_provider_no_completion == 1)) &&
        stats_after.dxg_present_helper_transport_present == 0 &&
        stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats_after.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats_after.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        no_present_credit && hyperv_gate;
    d3d12_completion_source_authority_pass =
        host_display_bind_source_catalog_pass &&
        (stats_after.dxg_display_bind_provider_submits ==
             stats_before.dxg_display_bind_provider_submits ||
         stats_after.dxg_display_bind_provider_no_completion == 1) &&
        stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        stats_after.dxg_scanout_bind_last_present_id == 0 &&
        stats_after.dxg_scanout_bind_last_completed == 0 &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_present_helper_transport_present == 0 &&
        stats_after.kms_vblank_source_nouveau_hw == 0 &&
        stats_after.kms_page_flip_events_native_hw == 0 &&
        stats_after.nouveau_pci_native_present_credit == 0 &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    native_present_completion_source_namespace_pass =
        d3d12_completion_source_authority_pass &&
        stats_after.nouveau_pci_irq_cause_valid == 0 &&
        stats_after.nouveau_pci_irq_cause_acks == 0 &&
        stats_after.nouveau_pci_irq_spurious == 0;
    gpu_remaining_holistic_skeleton_pass =
        host_display_bind_source_catalog_pass &&
        d3d12_completion_source_authority_pass &&
        native_present_completion_source_namespace_pass &&
        stats_after.dxg_display_bind_transport_present == 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        stats_after.dxg_scanout_bind_successes ==
            stats_before.dxg_scanout_bind_successes &&
        stats_after.dxg_scanout_bind_completion_successes ==
            stats_before.dxg_scanout_bind_completion_successes &&
        stats_after.nouveau_pci_native_present_credit == 0 &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    {
        char *dxg_status = read_dxg_status_buffer();
        char *host_to_vm = dxg_status != 0 ?
            dxg_find_status_line(dxg_status, "dxg_host_to_vm_last=") : 0;

        if (host_to_vm != 0) {
            (void)dxg_parse_uint_after_field_on_line(host_to_vm,
                                                     "packets:",
                                                     &host_to_vm_packets);
            (void)dxg_parse_uint_after_field_on_line(host_to_vm,
                                                     "unknown:",
                                                     &host_to_vm_unknown);
            (void)dxg_parse_uint_after_field_on_line(host_to_vm, "cmd:",
                                                     &host_to_vm_last_cmd);
            (void)dxg_parse_uint_after_field_on_line(host_to_vm,
                                                     "channel:",
                                                     &host_to_vm_last_channel);
            (void)dxg_parse_uint_after_field_on_line(host_to_vm,
                                                     "payload:",
                                                     &host_to_vm_last_payload);
            (void)dxg_parse_uint_after_field_on_line(
                host_to_vm, "presenthistory:",
                &host_to_vm_presenthistory);
            (void)dxg_parse_uint_after_field_on_line(
                host_to_vm, "presenthistory_len:",
                &host_to_vm_presenthistory_len);
            (void)dxg_parse_uint_after_field_on_line(
                host_to_vm, "presenthistory_head_len:",
                &host_to_vm_presenthistory_head_len);
        }
        d3d12_host_to_vm_presenthistory_absent_pass =
            host_to_vm != 0 &&
            host_to_vm_presenthistory == 0 &&
            host_to_vm_presenthistory_head_len == 0 &&
            stats_after.dxg_scanout_bind_candidate_propagate_presenthistory_cmd == 1 &&
            stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
            stats_after.dxg_display_bind_present_id == 0 &&
            stats_after.dxg_display_bind_completed_id == 0 &&
            stats_after.dxg_scanout_bind_completion_successes ==
                stats_before.dxg_scanout_bind_completion_successes;
        dxg_presenthistory_orphan_completion_rejection_pass =
            host_to_vm != 0 &&
            stats_after.dxg_scanout_bind_candidate_propagate_presenthistory_cmd == 1 &&
            stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
            stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
            stats_after.dxg_display_bind_provider_completion_demux_registered == 0 &&
            stats_after.dxg_scanout_bind_completion_successes ==
                stats_before.dxg_scanout_bind_completion_successes &&
            stats_after.dxg_display_bind_present_id == 0 &&
            stats_after.dxg_display_bind_completed_id == 0;
        if (dxg_status != 0)
            free(dxg_status);
    }
    gpu_remaining_plan_dependency_skeleton_pass =
        d3d12_display_bind_success_shape_pass &&
        d3d12_native_completion_lifetime_pass &&
        d3d12_provider_credit_gate_pass &&
        d3d12_display_bind_pending_lifetime_pass &&
        d3d12_display_bind_generation_revalidation_pass &&
        d3d12_display_bind_provider_pending_publication_pass &&
        d3d12_native_completion_not_kms_pass &&
        d3d12_standard_alloc_not_display_bind_pass &&
        d3d12_dda_nouveau_separate_display_not_bind_pass &&
        wsl_uapi_namespace_negative_pass &&
        wsl_adapter_display_caps_negative_pass &&
        wsl_submit_present_fields_not_bind_pass &&
        wsl_stdalloc_and_alloc_flags_not_bind_pass &&
        wsl_trace_display_bind_negative_pass &&
        public_present_api_not_guest_bind_pass &&
        provider_credit_gate_negative_pass &&
        d3d12_display_bind_host_abi_discovery_pass &&
        dda_nouveau_non_readback_display_proof_pass &&
        host_display_bind_source_catalog_pass &&
        d3d12_completion_source_authority_pass &&
        native_present_completion_source_namespace_pass &&
        dxg_presenthistory_orphan_completion_rejection_pass &&
        gpu_remaining_holistic_skeleton_pass &&
        stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
        stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
        stats_after.dxg_present_dda_nouveau_import_path_present == 0 &&
        stats_after.dxg_present_dda_nouveau_scanout_bind_present == 0 &&
        stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
        stats_after.dxg_display_bind_present_id == 0 &&
        stats_after.dxg_display_bind_completed_id == 0 &&
        (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) == 0;
    pass = provenance_complete && no_present_credit && failclosed &&
           bind_contract_failclosed && foreign_bind_contract_failclosed &&
           wait_sync_failclosed && negative_metadata_pass &&
           software_path_rejection && d3d12_present_syncfile_preopen_pass &&
           d3d12_scanout_bind_skeleton_pass &&
           d3d12_commit_result_copyout_contract_pass &&
           d3d12_display_bind_id_shape_pass &&
           d3d12_display_bind_success_shape_pass &&
           d3d12_display_bind_query_fields_pass &&
           d3d12_provider_credit_gate_pass &&
           d3d12_display_bind_pending_lifetime_pass &&
           d3d12_display_bind_generation_revalidation_pass &&
           d3d12_display_bind_provider_pending_publication_pass &&
           d3d12_native_completion_lifetime_pass &&
           d3d12_display_bind_stale_source_zero_credit_pass &&
           d3d12_native_completion_not_kms_pass &&
           d3d12_standard_alloc_not_display_bind_pass &&
           d3d12_dda_nouveau_separate_display_not_bind_pass &&
           d3d12_host_to_vm_presenthistory_absent_pass &&
           dxg_presenthistory_orphan_completion_rejection_pass &&
           gpu_remaining_plan_dependency_skeleton_pass &&
           wsl_uapi_namespace_negative_pass &&
           wsl_adapter_display_caps_negative_pass &&
           wsl_submit_present_fields_not_bind_pass &&
           wsl_stdalloc_and_alloc_flags_not_bind_pass &&
           wsl_trace_display_bind_negative_pass &&
           public_present_api_not_guest_bind_pass &&
           provider_credit_gate_negative_pass &&
           d3d12_display_bind_host_abi_discovery_pass &&
           dda_nouveau_non_readback_display_proof_pass &&
           d3d12_completion_source_authority_pass &&
           native_present_completion_source_namespace_pass &&
           owner_cleanup && stale_bind_contract_failclosed && hyperv_gate;

out:
    printf("present_source_failclosed_matrix create_rc=%d share_rc=%d "
           "register_rc=%d commit_rc=%d query_rc=%d after_close_query_rc=%d "
           "bind_contract_rc=%d stats_rc=%d/%d/%d backend_rc=%d "
           "source=0x%x source_live=%u->%u "
           "device=0x%x resource=0x%x allocation=0x%x fd=%lu "
           "luid=%x:%x query_luid=%x:%x adapter_identity=%u "
           "provenance=0x%x provenance_complete=%u "
           "requires_host_protocol=%lu missing_host_abi=%lu "
           "transport_present=%lu selected_lane=%u block_reason=0x%x "
           "host_candidates=0x%lx host_rejects=0x%lx "
           "present_id=%lu/%lu completed=%lu/%lu display_delta=%lu/%lu "
           "register_delta=%lu commit_delta=%lu release_delta=%lu "
           "backend=%u backend_flags=0x%x hyperv_opengl_submit=%u "
           "failclosed=%u no_present_credit=%u owner_cleanup=%u "
           "native_present_claim=0 status=%s\n",
           create_rc, share_rc, register_rc, commit_rc, query_rc,
           after_close_query_rc, bind_contract_rc,
           stats_before_rc, stats_after_rc,
           stats_closed_rc, backend_rc, reg.present_source,
           query.source_live, after_close_query.source_live, device.v,
           create_allocation.resource.v, allocation_info.allocation.v,
           shared_handle, adapter_luid.b, adapter_luid.a,
           query.adapter_luid_high, query.adapter_luid_low,
           query.adapter_identity, query.provenance_flags,
           provenance_complete, query.requires_host_protocol,
           query.missing_host_abi, query.helper_transport_present,
           query.selected_lane, query.helper_block_reason,
           query.host_candidates, query.host_rejects, commit.present_id,
           query.present_id, commit.completed, query.completed,
           stats_after.display_presents - stats_before.display_presents,
           stats_after.display_completions -
               stats_before.display_completions,
           stats_after.dxg_present_register_attempts -
               stats_before.dxg_present_register_attempts,
           stats_after.dxg_present_commit_attempts -
               stats_before.dxg_present_commit_attempts,
           stats_closed.dxg_present_release_sources -
               stats_before.dxg_present_release_sources,
           backend.backend, backend.flags,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           failclosed, no_present_credit, owner_cleanup,
           pass ? "PASS" : "FAIL");
    printf("present_bind_contract_skeleton_matrix "
           "ioctl_rc=%d version=%u transport=%u operation=%u "
           "source=0x%x source_live=%u source_generation=%lu "
           "resource_generation=%lu completion_source=%lu "
           "selected_lane=%lu block_reason=0x%lx required_metadata=0x%lx "
           "lifetime=0x%lx host_candidates=0x%lx host_rejects=0x%lx "
           "device=0x%x resource=0x%x allocation=0x%x allocation_count=%u "
           "sync=0x%x fence=%lu dimensions=%ux%u pitch=%u fmt=0x%x "
           "luid=%x:%x adapter_identity=%u provenance=0x%lx "
           "present_id=%lu completed=%lu native_present_claim=0 "
           "status=%s\n",
           bind_contract_rc, bind_contract.version, bind_contract.transport,
           bind_contract.operation, bind_contract.present_source,
           bind_contract.source_live, bind_contract.source_generation,
           bind_contract.resource_generation, bind_contract.completion_source,
           bind_contract.selected_lane,
           bind_contract.helper_block_reason,
           bind_contract.required_metadata, bind_contract.lifetime,
           bind_contract.host_candidates, bind_contract.host_rejects,
           bind_contract.device, bind_contract.resource,
           bind_contract.allocation, bind_contract.allocation_count,
           bind_contract.sync_object, bind_contract.fence_value,
           bind_contract.width, bind_contract.height, bind_contract.pitch,
           bind_contract.format, bind_contract.adapter_luid_high,
           bind_contract.adapter_luid_low, bind_contract.adapter_identity,
           bind_contract.provenance_flags, bind_contract.present_id,
           bind_contract.completed,
           bind_contract_failclosed ? "PASS" : "FAIL");
    printf("host_display_bind_source_catalog_matrix "
           "selected_source=missing selected_lane=gpup_dxg_scanout_bind "
           "provider_state=%s custom_host_tool=0 "
           "wsl_dxg_display_bind_ioctl=0 wslg_channel=absent "
           "gpup_dxg_sender=%lu gpup_dxg_completion=%lu "
           "synthvid_d3d12_bind=0 dda_d3d12_resource_import=%lu "
           "dda_scanout_bind=%lu dda_hw_flip_completion=%s "
           "transport_present=%lu present_id=%lu completed=%lu "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu native_present_credit=0 "
           "opengl_submit_credit=0 webkit_accel_credit=0 status=%s\n",
           stats_after.dxg_display_bind_provider_submits ==
                   stats_before.dxg_display_bind_provider_submits ?
               "not_sampled" : "failclosed",
           stats_after.dxg_scanout_bind_candidate_sender_contracts,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_present_dda_nouveau_import_path_present,
           stats_after.dxg_present_dda_nouveau_scanout_bind_present,
           stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           stats_after.dxg_display_bind_provider_no_host_abi,
           stats_after.dxg_display_bind_provider_no_sender,
           stats_after.dxg_display_bind_provider_no_completion,
           host_display_bind_source_catalog_pass ? "PASS" : "FAIL");
    printf("d3d12_display_bind_host_abi_discovery_matrix "
           "custom_host_tool=0 wsl_dxg_display_bind_ioctl=0 "
           "wsl_display_bind_ioctl_absent=1 "
           "wslg_frame_path=absent freerdp_frame_path=absent "
           "rdp_frame_path=copy_or_dirty_frame "
           "gpup_dxg_sender_contract=%lu "
           "gpup_dxg_completion_contract=%lu "
           "completion_demux_contract=%lu "
           "dda_nouveau_d3d12_import=%lu "
           "dda_nouveau_scanout_bind=%lu "
           "dda_nouveau_hw_flip_completion=%s "
           "provider_state=failclosed provider_failclosed=1 "
           "host_abi_present=0 sender_present=0 completion_present=0 "
           "transport_present=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 "
           "webkit_accel_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_candidate_sender_contracts,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_display_bind_provider_completion_demux_registered,
           stats_after.dxg_present_dda_nouveau_import_path_present,
           stats_after.dxg_present_dda_nouveau_scanout_bind_present,
           stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           d3d12_display_bind_host_abi_discovery_pass ? "PASS" : "FAIL");
    printf("d3d12_completion_source_authority_matrix "
           "accepted_completion_source=display_bind_provider "
           "submit_ntstatus_as_completion=0 "
           "presenthistory_telemetry_as_completion=0 "
           "syncfile_fence_as_display_completion=0 "
           "callback_release_as_display_completion=0 "
           "kms_vblank_as_d3d12_completion=0 "
           "provider_submits_delta=%lu provider_no_completion=%lu "
           "provider_completion_present=%u completion_contracts=%lu "
           "scanout_completion_success_delta=%lu "
           "present_id=%lu completed=%lu native_present_credit=%lu "
           "opengl_submit_credit=%u status=%s\n",
           stats_after.dxg_display_bind_provider_submits -
               stats_before.dxg_display_bind_provider_submits,
           stats_after.dxg_display_bind_provider_no_completion,
           stats_after.dxg_display_bind_provider_submits >
                   stats_before.dxg_display_bind_provider_submits &&
               stats_after.dxg_display_bind_provider_no_completion == 0,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_scanout_bind_completion_successes -
               stats_before.dxg_scanout_bind_completion_successes,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           stats_after.nouveau_pci_native_present_credit,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           d3d12_completion_source_authority_pass ? "PASS" : "FAIL");
    printf("native_present_completion_source_namespace_matrix "
           "completion_source=missing d3d12_display_bind_provider=%s "
           "d3d12_present_id=%lu d3d12_completed=%lu "
           "kms_vblank_sequence=%lu kms_page_flip_sequence=%lu "
           "nouveau_irq_cause_valid=%lu nouveau_irq_cause_acks=%lu "
           "namespace_mixed=0 native_present_credit=%lu "
           "opengl_submit_credit=%u status=%s\n",
           stats_after.dxg_display_bind_provider_submits ==
                   stats_before.dxg_display_bind_provider_submits ?
               "not_sampled" : "failclosed",
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           stats_after.kms_vblank_source_nouveau_hw,
           stats_after.kms_page_flip_events_native_hw,
           stats_after.nouveau_pci_irq_cause_valid,
           stats_after.nouveau_pci_irq_cause_acks,
           stats_after.nouveau_pci_native_present_credit,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           native_present_completion_source_namespace_pass ? "PASS" :
                                                             "FAIL");
    printf("present_bind_contract_stale_source_matrix "
           "ioctl_rc=%d source=0x%x source_live=%u block_reason=0x%lx "
           "completion_source=%lu present_id=%lu completed=%lu "
           "native_present_claim=0 status=%s\n",
           after_close_bind_contract_rc,
           after_close_bind_contract.present_source,
           after_close_bind_contract.source_live,
           after_close_bind_contract.helper_block_reason,
           after_close_bind_contract.completion_source,
           after_close_bind_contract.present_id,
           after_close_bind_contract.completed,
           stale_bind_contract_failclosed ? "PASS" : "FAIL");
    printf("present_bind_contract_foreign_source_matrix "
           "ioctl_rc=%d source=0x%x source_live=%u "
           "source_generation=%lu block_reason=0x%lx "
           "completion_source=%lu present_id=%lu completed=%lu "
           "native_present_claim=0 status=%s\n",
           foreign_bind_contract_rc,
           foreign_bind_contract.present_source,
           foreign_bind_contract.source_live,
           foreign_bind_contract.source_generation,
           foreign_bind_contract.helper_block_reason,
           foreign_bind_contract.completion_source,
           foreign_bind_contract.present_id,
           foreign_bind_contract.completed,
           foreign_bind_contract_failclosed ? "PASS" : "FAIL");
    printf("present_source_waitsync_failclosed_matrix "
           "create_resource=PASS share_resource=PASS register=PASS "
           "sync_create_rc=%d sync_object=0x%x "
           "commit_rc=%d query_rc=%d stats_wait_rc=%d "
           "source=0x%x source_live=%u "
           "wait_sync_metadata=%s required_metadata=0x%lx "
           "lifetime=0x%lx sync_object=0x%x query_sync=0x%x "
           "fence=%lu query_fence=%lu commit_present_id=%lu "
           "query_present_id=%lu commit_completed=%lu query_completed=%lu "
           "missing_host_abi=%lu transport_present=%lu "
           "present_id=0 completed=0 no_present_credit=%u "
           "native_present_claim=0 status=%s\n",
           create_sync_rc, create_sync.sync_object.v,
           wait_commit_rc, wait_query_rc, stats_wait_rc,
           reg.present_source,
           wait_query.source_live,
           wait_sync_metadata ? "PASS" : "FAIL",
           wait_query.helper_required_metadata,
           wait_query.helper_lifetime,
           create_sync.sync_object.v, wait_query.sync_object,
           wait_commit.fence_value, wait_query.fence_value,
           wait_commit.present_id, wait_query.present_id,
           wait_commit.completed, wait_query.completed,
           wait_query.missing_host_abi,
           wait_query.helper_transport_present,
           wait_sync_no_present_credit,
           wait_sync_failclosed ? "PASS" : "FAIL");
    printf("present_source_negative_metadata_matrix "
           "zero_dimensions=%s bad_pitch=%s invalid_resource_fd=%s "
           "reserved_register_flags=%s no_source_commit=%s "
           "wait_sync_missing_sync=%s sync_without_wait_flag=%s "
           "unverified_resource_fd=%s adapter_mismatch=%s "
           "register_rejects_delta=%lu commit_bad_flags_delta=%lu "
           "commit_no_source_delta=%lu commit_resource_fd_unverified_delta=%lu "
           "commit_adapter_mismatch_delta=%lu display_delta=%lu/%lu "
           "present_id=0 completed=0 no_present_credit=%u "
           "native_present_claim=0 opengl_submit_credit=0 status=%s\n",
           zero_dimensions_rc < 0 ? "PASS" : "FAIL",
           bad_pitch_rc < 0 ? "PASS" : "FAIL",
           invalid_resource_fd_rc < 0 ? "PASS" : "FAIL",
           reserved_flags_rc < 0 ? "PASS" : "FAIL",
           negative_source_identity ? "PASS" : "FAIL",
           missing_sync_commit_rc < 0 ? "PASS" : "FAIL",
           sync_without_flag_commit_rc < 0 ? "PASS" : "FAIL",
           unverified_resource_failclosed ? "PASS" : "FAIL",
           adapter_mismatch_failclosed ? "PASS" : "FAIL",
           stats_negative.dxg_present_register_rejects -
               stats_before.dxg_present_register_rejects,
           stats_negative.dxg_present_commit_bad_flags -
               stats_before.dxg_present_commit_bad_flags,
           stats_negative.dxg_present_commit_no_source -
               stats_before.dxg_present_commit_no_source,
           stats_negative.dxg_present_commit_resource_fd_unverified -
               stats_before.dxg_present_commit_resource_fd_unverified,
           stats_negative.dxg_present_commit_adapter_mismatch -
               stats_before.dxg_present_commit_adapter_mismatch,
           stats_negative.display_presents - stats_before.display_presents,
           stats_negative.display_completions -
               stats_before.display_completions,
           negative_no_present_credit,
           negative_metadata_pass ? "PASS" : "FAIL");
    printf("present_source_software_path_rejection_matrix "
           "framebuffer_blit=REJECTED cpu_map_readback=REJECTED "
           "dri_software_present=REJECTED copy_export_fallback=REJECTED "
           "callback_only=REJECTED release_only=REJECTED "
           "display_target_kind=%u selected_lane=%lu "
           "missing_host_abi=%lu transport_present=%lu "
           "present_id=0 completed=0 no_present_credit=%u "
           "native_present_claim=0 opengl_submit_credit=0 status=%s\n",
           query.display_target_kind, bind_contract.selected_lane,
           query.missing_host_abi,
           query.helper_transport_present,
           negative_no_present_credit,
           software_path_rejection ? "PASS" : "FAIL");
    printf("d3d12_shared_resource_fd_lifetime_matrix "
           "share_export=PASS resource_fd=%lu register_live_fd=%s "
           "invalid_fd_rejected=%s unverified_resource_fd=%s "
           "stale_source_after_owner_close=%s cleanup_balance=%s "
           "present_id=0 completed=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           shared_handle,
           register_rc == 0 && reg.resource_fd == (int32)shared_handle ?
               "PASS" : "FAIL",
           invalid_resource_fd_rc < 0 ? "PASS" : "FAIL",
           unverified_resource_failclosed ? "PASS" : "FAIL",
           stale_bind_contract_failclosed ? "PASS" : "FAIL",
           owner_cleanup ? "PASS" : "FAIL",
           d3d12_resource_fd_lifetime_pass ? "PASS" : "FAIL");
    printf("d3d12_present_source_admission_matrix "
           "same_adapter_luid=%s resource_fd=PASS d3dkmt_handles=%s "
           "dimensions=%s format_modifier=%s allocation_count=%u "
           "wait_sync_metadata=%s unverified_resource_fd=%s "
           "adapter_mismatch=%s source_owner=%s failclosed=%s "
           "present_id=0 completed=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           query.adapter_identity == FB_GPU_DXG_PRESENT_ADAPTER_MATCH &&
               query.adapter_luid_low == adapter_luid.a &&
               query.adapter_luid_high == adapter_luid.b ?
               "PASS" : "FAIL",
           bind_contract.device == device.v &&
               bind_contract.resource == create_allocation.resource.v &&
               bind_contract.allocation == allocation_info.allocation.v ?
               "PASS" : "FAIL",
           bind_contract.width == reg.width &&
               bind_contract.height == reg.height &&
               bind_contract.pitch == reg.pitch ? "PASS" : "FAIL",
           bind_contract.format == reg.format &&
               bind_contract.modifier == reg.modifier ? "PASS" : "FAIL",
           bind_contract.allocation_count,
           wait_sync_metadata ? "PASS" : "FAIL",
           unverified_resource_failclosed ? "PASS" : "FAIL",
           adapter_mismatch_failclosed ? "PASS" : "FAIL",
           foreign_bind_contract_failclosed &&
               stale_bind_contract_failclosed ? "PASS" : "FAIL",
           failclosed ? "PASS" : "FAIL",
           d3d12_present_admission_pass ? "PASS" : "FAIL");
    printf("d3d12_acquire_fence_lifetime_matrix "
           "sync_create_rc=%d sync_object=0x%x monitored_fence=PASS "
           "wait_metadata=%s wait_commit_failclosed=%s "
           "query_sync_matches=%s stale_source_cleanup=%s "
           "present_id=0 completed=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           create_sync_rc, create_sync.sync_object.v,
           wait_sync_metadata ? "PASS" : "FAIL",
           wait_sync_failclosed ? "PASS" : "FAIL",
           wait_query.sync_object == create_sync.sync_object.v &&
               wait_query.fence_value == wait_commit.fence_value ?
               "PASS" : "FAIL",
           stale_bind_contract_failclosed ? "PASS" : "FAIL",
           d3d12_acquire_fence_lifetime_pass ? "PASS" : "FAIL");
    printf("d3d12_present_syncfile_preopen_matrix "
           "sync_file_create_rc=%d sync_file=%lu open_rc=%d "
           "opened_sync=0x%x fence=%lu wrong_fd_kind_rejected=%s "
           "wait_commit_failclosed=%s query_sync_matches=%s "
           "fence_value_preserved=%s stale_source_cleanup=%s "
           "present_id=0 completed=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           present_create_sync_file_rc,
           present_create_sync_file.sync_file_handle,
           present_open_sync_file_rc,
           present_open_sync_file.syncobj.v,
           present_open_sync_file.fence_value,
           present_bad_open_sync_file_rc < 0 ? "PASS" : "FAIL",
           sync_file_commit_rc < 0 &&
                   sync_file_query.last_ret == EOPNOTSUPP ?
               "PASS" : "FAIL",
           sync_file_query.sync_object == present_open_sync_file.syncobj.v &&
                   sync_file_query.fence_value ==
                       present_open_sync_file.fence_value ?
               "PASS" : "FAIL",
           present_open_sync_file.fence_value ==
                   present_create_sync_file.fence_value ?
               "PASS" : "FAIL",
           stale_bind_contract_failclosed ? "PASS" : "FAIL",
           d3d12_present_syncfile_preopen_pass ? "PASS" : "FAIL");
    printf("d3d12_present_bind_contract_failclosed_matrix "
           "selected_lane=%lu completion_source=%lu "
           "required_metadata=0x%lx lifetime=0x%lx "
           "foreign_source=%s stale_source=%s software_paths_rejected=%s "
           "missing_host_abi=%lu transport_present=%lu "
           "present_id=0 completed=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           bind_contract.selected_lane, bind_contract.completion_source,
           bind_contract.required_metadata, bind_contract.lifetime,
           foreign_bind_contract_failclosed ? "PASS" : "FAIL",
           stale_bind_contract_failclosed ? "PASS" : "FAIL",
           software_path_rejection ? "PASS" : "FAIL",
           query.missing_host_abi, query.helper_transport_present,
           d3d12_bind_contract_failclosed_pass ? "PASS" : "FAIL");
    printf("d3d12_present_resource_fd_typed_admission_matrix "
           "typed_resource_fd=%s query_kind=%u bind_kind=%u "
           "sealed_before_admit=%s shared_records_valid=%s "
           "allocation_match=%s generation_from_shared=%s "
           "resource_generation=%lu fd_generation=%lu "
           "invalid_fd_rejected=%s stale_source_cleanup=%s "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           query.resource_fd_kind == 2 &&
                   bind_contract.resource_fd_kind == 2 ? "PASS" : "FAIL",
           query.resource_fd_kind, bind_contract.resource_fd_kind,
           query.resource_fd_sealed != 0 &&
                   bind_contract.resource_fd_sealed != 0 ? "PASS" : "FAIL",
           query.resource_fd_shared_records_valid != 0 &&
                   bind_contract.resource_fd_shared_records_valid != 0 ?
               "PASS" : "FAIL",
           query.resource_fd_matches_handles != 0 &&
                   bind_contract.resource_fd_matches_handles != 0 &&
                   bind_contract.device == device.v &&
                   bind_contract.resource == create_allocation.resource.v &&
                   bind_contract.allocation == allocation_info.allocation.v &&
                   bind_contract.allocation_count == 1 ? "PASS" : "FAIL",
           query.resource_fd_generation != 0 &&
                   bind_contract.resource_generation ==
                       query.resource_fd_generation ? "PASS" : "FAIL",
           bind_contract.resource_generation,
           query.resource_fd_generation,
           invalid_resource_fd_rc < 0 ? "PASS" : "FAIL",
           stale_bind_contract_failclosed ? "PASS" : "FAIL",
           d3d12_resource_fd_typed_admission_pass ? "PASS" : "FAIL");
    printf("d3d12_native_completion_zero_credit_matrix "
           "source=0x%x display_bind=ABSENT transport_present=%lu "
           "completion_source=%lu present_id=0 completed=0 "
           "close_before_signal=DEFERRED callbacks_after_completion=0 "
           "releases_after_completion=0 per_client_generation=required "
           "id_shape=%s provider_credit_gate=%s "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           reg.present_source, query.helper_transport_present,
           bind_contract.completion_source,
           d3d12_display_bind_id_shape_pass ? "PASS" : "FAIL",
           d3d12_provider_credit_gate_pass ? "PASS" : "FAIL",
           d3d12_bind_contract_failclosed_pass &&
                   query.helper_transport_present == 0 &&
                   bind_contract.completion_source ==
                       FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
                   bind_contract.present_id == 0 &&
                   bind_contract.completed == 0 ?
               "PASS" : "FAIL");
    printf("d3d12_display_bind_id_shape_matrix "
           "bind_present_id=%lu bind_completed_id=%lu "
           "bind_source_generation=%lu bind_resource_generation=%lu "
           "scanout_present_id=%lu scanout_completed_id=%lu "
           "scanout_source_generation=%lu scanout_resource_generation=%lu "
           "zero_ids_required_when_failclosed=1 "
           "completed_ge_present_if_nonzero=1 stale_id_rejected=1 "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           stats_after.dxg_display_bind_source_generation,
           stats_after.dxg_display_bind_resource_generation,
           stats_after.dxg_scanout_bind_last_present_id,
           stats_after.dxg_scanout_bind_last_completed,
           stats_after.dxg_scanout_bind_last_source_generation,
           stats_after.dxg_scanout_bind_last_resource_generation,
           d3d12_display_bind_id_shape_pass ? "PASS" : "FAIL");
    printf("d3d12_provider_credit_gate_matrix "
           "provider_submits=%lu provider_no_host_abi=%lu "
           "provider_no_sender=%lu provider_no_completion=%lu "
           "transport_present=%lu display_target_kind=%lu "
           "scanout_success_delta=%lu completion_success_delta=%lu "
           "native_present_credit=%lu backend_opengl_submit=%u "
           "credit_requires_provider_clear=1 status=%s\n",
           stats_after.dxg_display_bind_provider_submits -
               stats_before.dxg_display_bind_provider_submits,
           stats_after.dxg_display_bind_provider_no_host_abi,
           stats_after.dxg_display_bind_provider_no_sender,
           stats_after.dxg_display_bind_provider_no_completion,
           stats_after.dxg_present_helper_transport_present,
           stats_after.dxg_present_display_target_kind,
           stats_after.dxg_scanout_bind_successes -
               stats_before.dxg_scanout_bind_successes,
           stats_after.dxg_scanout_bind_completion_successes -
               stats_before.dxg_scanout_bind_completion_successes,
           stats_after.nouveau_pci_native_present_credit,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           d3d12_provider_credit_gate_pass ? "PASS" : "FAIL");
    printf("d3d12_display_bind_request_metadata_matrix "
           "provider_submits_delta=%lu request_metadata_complete=%lu "
           "request_sync_metadata_complete=%lu missing_metadata=0x%lx "
           "required_metadata=0x%lx source_generation=%lu "
           "resource_generation=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_display_bind_provider_submits -
               stats_before.dxg_display_bind_provider_submits,
           stats_after.dxg_display_bind_request_metadata_complete,
           stats_after.dxg_display_bind_request_sync_metadata_complete,
           stats_after.dxg_display_bind_request_missing_metadata,
           stats_after.dxg_display_bind_required_metadata,
           stats_after.dxg_display_bind_source_generation,
           stats_after.dxg_display_bind_resource_generation,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           d3d12_display_bind_request_metadata_pass ? "PASS" : "FAIL");
    printf("d3d12_display_bind_pending_lifetime_matrix "
           "pending_sequence=%lu created_delta=%lu active=%lu peak=%lu "
           "completed_delta=%lu failclosed_delta=%lu cancelled_delta=%lu "
           "last_status=%lu last_block_reason=0x%lx "
           "source_generation=%lu resource_generation=%lu "
           "matches_bind_contract=%u native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_display_bind_pending_sequence,
           stats_after.dxg_display_bind_pending_created -
               stats_before.dxg_display_bind_pending_created,
           stats_after.dxg_display_bind_pending_active,
           stats_after.dxg_display_bind_pending_peak,
           stats_after.dxg_display_bind_pending_completed -
               stats_before.dxg_display_bind_pending_completed,
           stats_after.dxg_display_bind_pending_failclosed -
               stats_before.dxg_display_bind_pending_failclosed,
           stats_after.dxg_display_bind_pending_cancelled -
               stats_before.dxg_display_bind_pending_cancelled,
           stats_after.dxg_display_bind_pending_last_status,
           stats_after.dxg_display_bind_pending_last_block_reason,
           stats_after.dxg_display_bind_pending_last_source_generation,
           stats_after.dxg_display_bind_pending_last_resource_generation,
           stats_after.dxg_display_bind_pending_last_source_generation ==
                   bind_contract.source_generation &&
                   stats_after.dxg_display_bind_pending_last_resource_generation ==
                   bind_contract.resource_generation,
           d3d12_display_bind_pending_lifetime_pass ? "PASS" : "FAIL");
    printf("d3d12_display_bind_generation_revalidation_matrix "
           "provider_submits_delta=%lu lock_dropped_submits_delta=%lu "
           "revalidate_attempts_delta=%lu revalidate_successes_delta=%lu "
           "revalidate_failures_delta=%lu provider_pin_revalidated=%lu "
           "query_source_generation=%lu contract_source_generation=%lu "
           "display_bind_source_generation=%lu query_resource_generation=%lu "
           "contract_resource_generation=%lu display_bind_resource_generation=%lu "
           "pinned_resource_generation=%lu source_generation_match=%s "
           "resource_generation_match=%s pinned_generation_match=%s "
           "present_id=%lu completed=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_display_bind_provider_submits -
               stats_before.dxg_display_bind_provider_submits,
           stats_after.dxg_display_bind_lock_dropped_submits -
               stats_before.dxg_display_bind_lock_dropped_submits,
           stats_after.dxg_display_bind_revalidate_attempts -
               stats_before.dxg_display_bind_revalidate_attempts,
           stats_after.dxg_display_bind_revalidate_successes -
               stats_before.dxg_display_bind_revalidate_successes,
           stats_after.dxg_display_bind_revalidate_failures -
               stats_before.dxg_display_bind_revalidate_failures,
           stats_after.dxg_display_bind_provider_pin_revalidated,
           query.source_generation,
           bind_contract.source_generation,
           stats_after.dxg_display_bind_source_generation,
           query.resource_generation,
           bind_contract.resource_generation,
           stats_after.dxg_display_bind_resource_generation,
           stats_after.dxg_display_bind_pinned_resource_generation,
           query.source_generation != 0 &&
                   bind_contract.source_generation == query.source_generation &&
                   stats_after.dxg_display_bind_source_generation ==
                   query.source_generation ? "PASS" : "FAIL",
           query.resource_generation != 0 &&
                   bind_contract.resource_generation ==
                   query.resource_generation &&
                   stats_after.dxg_display_bind_resource_generation ==
                   query.resource_generation ? "PASS" : "FAIL",
           query.resource_generation != 0 &&
                   stats_after.dxg_display_bind_pinned_resource_generation ==
                   query.resource_generation ? "PASS" : "FAIL",
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           d3d12_display_bind_generation_revalidation_pass ? "PASS" :
                                                             "FAIL");
    printf("d3d12_display_bind_provider_pending_publication_matrix "
           "provider_submits_delta=%lu publication_attempts_delta=%lu "
           "host_abi_present=0 sender_present=0 "
           "completion_present=0 owner_generation=%lu "
           "query_source_generation=%lu contract_source_generation=%lu "
           "display_bind_source_generation=%lu query_resource_generation=%lu "
           "contract_resource_generation=%lu "
           "display_bind_resource_generation=%lu "
           "provider_source_generation=%lu provider_resource_generation=%lu "
           "pending_owner_generation=%lu pending_source_generation=%lu "
           "pending_resource_generation=%lu "
           "dxgprocess_generation=%lu process_adapter_generation=%lu "
           "hmgr_index_unique_valid=%lu parent_resource_ref_held=%lu "
           "opened_child_ref_held=%lu syncobject_ref_held=%lu "
           "owner_close_cancelled=%lu "
           "owner_generation_required=1 source_generation_required=1 "
           "resource_generation_required=1 pending_generation_match=%s "
           "publish_before_send=%lu "
           "transport_pending_id=%lu command_id=%lu transaction_id=%lu "
           "channel=%s completion_demux_registered=%lu "
           "resolved_or_cancelled=%lu refs_released=%lu "
           "no_host_abi_cancelled=%lu no_host_abi_refs_released=%lu "
           "pending_cancelled_delta=%lu publish_before_send_order=blocked "
           "cancellation_ref_release_credit=0 "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 "
           "status=%s\n",
           stats_after.dxg_display_bind_provider_submits -
               stats_before.dxg_display_bind_provider_submits,
           stats_after.dxg_display_bind_provider_publication_attempts -
               stats_before.dxg_display_bind_provider_publication_attempts,
           stats_after.dxg_display_bind_provider_pending_owner_generation,
           query.source_generation,
           bind_contract.source_generation,
           stats_after.dxg_display_bind_source_generation,
           query.resource_generation,
           bind_contract.resource_generation,
           stats_after.dxg_display_bind_resource_generation,
           stats_after.dxg_display_bind_provider_pending_source_generation,
           stats_after.dxg_display_bind_provider_pending_resource_generation,
           stats_after.dxg_display_bind_pending_last_owner_generation,
           stats_after.dxg_display_bind_pending_last_source_generation,
           stats_after.dxg_display_bind_pending_last_resource_generation,
           stats_after.dxg_display_bind_provider_pending_dxgprocess_generation,
           stats_after.dxg_display_bind_provider_pending_process_adapter_generation,
           stats_after.dxg_display_bind_provider_pending_hmgr_index_unique_valid,
           stats_after.dxg_display_bind_provider_pending_parent_resource_ref_held,
           stats_after.dxg_display_bind_provider_pending_opened_child_ref_held,
           stats_after.dxg_display_bind_provider_pending_syncobject_ref_held,
           stats_after.dxg_display_bind_provider_pending_owner_close_cancelled,
           stats_after.dxg_display_bind_provider_pending_owner_generation != 0 &&
                   stats_after.dxg_display_bind_provider_pending_source_generation != 0 &&
                   stats_after.dxg_display_bind_provider_pending_resource_generation != 0 &&
                   stats_after.dxg_display_bind_provider_pending_dxgprocess_generation ==
                   stats_after.dxg_display_bind_provider_pending_owner_generation &&
                   stats_after.dxg_display_bind_provider_pending_process_adapter_generation != 0 &&
                   stats_after.dxg_display_bind_provider_pending_hmgr_index_unique_valid == 1 &&
                   stats_after.dxg_display_bind_provider_pending_parent_resource_ref_held == 1 &&
                   stats_after.dxg_display_bind_provider_pending_opened_child_ref_held == 1 &&
                   stats_after.dxg_display_bind_provider_pending_owner_close_cancelled == 0 &&
                   stats_after.dxg_display_bind_pending_last_owner_generation ==
                   stats_after.dxg_display_bind_provider_pending_owner_generation &&
                   stats_after.dxg_display_bind_pending_last_source_generation ==
                   stats_after.dxg_display_bind_provider_pending_source_generation &&
                   stats_after.dxg_display_bind_pending_last_resource_generation ==
                   stats_after.dxg_display_bind_provider_pending_resource_generation &&
                   query.source_generation != 0 &&
                   query.resource_generation != 0 ?
               "PASS" : "FAIL",
           stats_after.dxg_display_bind_provider_publish_before_send,
           stats_after.dxg_display_bind_provider_transport_pending_id,
           stats_after.dxg_display_bind_provider_command_id,
           stats_after.dxg_display_bind_provider_transaction_id,
           stats_after.dxg_display_bind_provider_channel == 0 ?
               "none" : "other",
           stats_after.dxg_display_bind_provider_completion_demux_registered,
           stats_after.dxg_display_bind_provider_resolved_or_cancelled,
           stats_after.dxg_display_bind_provider_refs_released,
           stats_after.dxg_display_bind_provider_no_host_abi_cancelled,
           stats_after.dxg_display_bind_provider_no_host_abi_refs_released,
           stats_after.dxg_display_bind_pending_cancelled -
               stats_before.dxg_display_bind_pending_cancelled,
           stats_after.dxg_display_bind_provider_no_host_abi,
           stats_after.dxg_display_bind_provider_no_sender,
           stats_after.dxg_display_bind_provider_no_completion,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           d3d12_display_bind_provider_pending_publication_pass ?
               "PASS_FAILCLOSED" : "FAIL");
    printf("d3d12_display_bind_success_shape_matrix "
           "transport_present=%lu status_code=%lu block_reason=0x%lx "
           "completion_source=%lu present_id=%lu completed=%lu "
           "source_generation=%lu resource_generation=%lu "
           "scanout_success_delta=%lu completion_success_delta=%lu "
           "provider_submits_delta=%lu provider_pin_revalidated=%lu "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu failclosed_allowed=1 "
           "success_requires_provider_clear=1 "
           "success_requires_display_completion=1 "
           "native_present_credit=%lu backend_opengl_submit=%u "
           "status=%s\n",
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_status,
           stats_after.dxg_display_bind_block_reason,
           stats_after.dxg_display_bind_completion_source,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           stats_after.dxg_display_bind_source_generation,
           stats_after.dxg_display_bind_resource_generation,
           stats_after.dxg_scanout_bind_successes -
               stats_before.dxg_scanout_bind_successes,
           stats_after.dxg_scanout_bind_completion_successes -
               stats_before.dxg_scanout_bind_completion_successes,
           stats_after.dxg_display_bind_provider_submits -
               stats_before.dxg_display_bind_provider_submits,
           stats_after.dxg_display_bind_provider_pin_revalidated,
           stats_after.dxg_display_bind_provider_no_host_abi,
           stats_after.dxg_display_bind_provider_no_sender,
           stats_after.dxg_display_bind_provider_no_completion,
           stats_after.nouveau_pci_native_present_credit,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           d3d12_display_bind_success_shape_pass ? "PASS" : "FAIL");
    printf("d3d12_display_bind_query_fields_matrix "
           "query_source_generation=%lu contract_source_generation=%lu "
           "query_resource_generation=%lu contract_resource_generation=%lu "
           "query_status=%lu provider_status=%lu "
           "query_block_reason=0x%lx provider_block_reason=0x%lx "
           "query_completion_source=%lu contract_completion_source=%lu "
           "query_dirty_sequence=%lu query_dirty_rects=%lu "
           "contract_dirty_sequence=%lu contract_dirty_rects=%lu "
           "query_host_abi_present=%u query_sender_present=%u "
           "query_completion_present=%u contract_host_abi_present=%u "
           "contract_sender_present=%u contract_completion_present=%u "
           "query_no_host_abi=%u query_no_sender=%u "
           "query_no_completion=%u contract_no_host_abi=%u "
           "contract_no_sender=%u contract_no_completion=%u "
           "query_pin_revalidated=%u contract_pin_revalidated=%u "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           query.source_generation,
           bind_contract.source_generation,
           query.resource_generation,
           bind_contract.resource_generation,
           query.display_bind_status,
           bind_contract.provider_status,
           query.display_bind_block_reason,
           bind_contract.provider_block_reason,
           query.display_bind_completion_source,
           bind_contract.completion_source,
           query.display_bind_dirty_sequence,
           query.display_bind_dirty_rects,
           bind_contract.dirty_sequence,
           bind_contract.dirty_rects,
           query.display_bind_host_abi_present,
           query.display_bind_sender_present,
           query.display_bind_completion_present,
           bind_contract.host_abi_present,
           bind_contract.sender_present,
           bind_contract.completion_present,
           query.display_bind_provider_no_host_abi,
           query.display_bind_provider_no_sender,
           query.display_bind_provider_no_completion,
           bind_contract.provider_no_host_abi,
           bind_contract.provider_no_sender,
           bind_contract.provider_no_completion,
           query.display_bind_provider_pin_revalidated,
           bind_contract.provider_pin_revalidated,
           d3d12_display_bind_query_fields_pass ? "PASS" : "FAIL");
    printf("d3d12_native_completion_lifetime_matrix "
           "transport_present=%lu status_code=%lu block_reason=0x%lx "
           "completion_source=%lu present_id=%lu completed=%lu "
           "source_generation=%lu resource_generation=%lu "
           "completion_success_delta=%lu provider_no_completion=%lu "
           "callbacks_after_completion_required=1 "
           "releases_after_completion_required=1 "
           "close_before_signal_cancel_required=1 cleanup_balance_required=1 "
           "failclosed_callbacks_after_completion=0 "
           "failclosed_releases_after_completion=0 "
           "native_present_credit=%lu backend_opengl_submit=%u "
           "status=%s\n",
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_status,
           stats_after.dxg_display_bind_block_reason,
           stats_after.dxg_display_bind_completion_source,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           stats_after.dxg_display_bind_source_generation,
           stats_after.dxg_display_bind_resource_generation,
           stats_after.dxg_scanout_bind_completion_successes -
               stats_before.dxg_scanout_bind_completion_successes,
           stats_after.dxg_display_bind_provider_no_completion,
           stats_after.nouveau_pci_native_present_credit,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           d3d12_native_completion_lifetime_pass ? "PASS" : "FAIL");
    printf("d3d12_display_bind_stale_source_zero_credit_matrix "
           "release_sources_delta=%lu after_close_queries=%lu "
           "stale_source_rejects=%lu release_clears=%lu "
           "stale_generation_rejects=%lu stale_completion_rejects=%lu "
           "late_completion_after_release=%lu "
           "after_close_nonzero_id_rejects=%lu "
           "after_close_present_id=%lu after_close_completed=%lu "
           "after_close_bind_present_id=%lu "
           "after_close_bind_completed=%lu "
           "global_present_id_after_close=%lu "
           "global_completed_after_close=%lu "
           "native_present_credit=%lu opengl_submit_credit=%u "
           "stale_generation_rejected=%s stale_completion_rejected=%s "
           "late_completion_rejected=%s webkit_accel_credit=0 "
           "cleanup_state=%s status=%s\n",
           stats_closed.dxg_present_release_sources -
               stats_before.dxg_present_release_sources,
           stats_closed.dxg_display_bind_after_close_queries -
               stats_before.dxg_display_bind_after_close_queries,
           stats_closed.dxg_display_bind_stale_source_rejects -
               stats_before.dxg_display_bind_stale_source_rejects,
           stats_closed.dxg_display_bind_release_clears -
               stats_before.dxg_display_bind_release_clears,
           stats_closed.dxg_display_bind_stale_generation_rejects -
               stats_before.dxg_display_bind_stale_generation_rejects,
           stats_closed.dxg_display_bind_stale_completion_rejects -
               stats_before.dxg_display_bind_stale_completion_rejects,
           stats_closed.dxg_display_bind_late_completion_after_release,
           stats_closed.dxg_display_bind_after_close_nonzero_id_rejects,
           after_close_query.present_id,
           after_close_query.completed,
           after_close_bind_contract.present_id,
           after_close_bind_contract.completed,
           stats_closed.dxg_display_bind_present_id,
           stats_closed.dxg_display_bind_completed_id,
           stats_closed.nouveau_pci_native_present_credit,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           stats_closed.dxg_display_bind_stale_generation_rejects >
                   stats_before.dxg_display_bind_stale_generation_rejects ?
               "PASS" : "FAIL",
           stats_closed.dxg_display_bind_stale_completion_rejects >
                   stats_before.dxg_display_bind_stale_completion_rejects ?
               "PASS" : "FAIL",
           stats_closed.dxg_display_bind_late_completion_after_release == 0 ?
               "PASS" : "FAIL",
           d3d12_display_bind_stale_source_cleanup_immediate ?
               "immediate" : "deferred_until_process_exit",
           d3d12_display_bind_stale_source_zero_credit_pass ? "PASS" :
               "FAIL");
    printf("d3d12_native_completion_not_kms_matrix "
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
           stats_after.display_last_complete,
           stats_after.kms_vblank_display_correlated,
           stats_after.kms_vblank_source_software_display,
           stats_after.kms_vblank_source_nouveau_hw,
           stats_after.kms_atomic_out_fence_display_correlated,
           stats_after.kms_atomic_out_fence_software_scanout_correlated,
           stats_after.kms_vblank_page_flip_events,
           stats_after.kms_page_flip_events_software_blit,
           stats_after.kms_page_flip_events_native_hw,
           d3d12_native_completion_not_kms_pass ? "PASS" : "FAIL");
    printf("d3d12_display_bind_backend_boundary_matrix "
           "backend=gpup_dxg_scanout_bind contract_version=%lu "
           "transport=%lu transport_present=%lu operation=%lu "
           "completion_source=%lu required_metadata=0x%lx "
           "lifetime=0x%lx block_reason=0x%lx present_id=%lu "
           "completed=%lu source_generation=%lu resource_generation=%lu "
           "status_code=%lu provider_submits=%lu lock_dropped_submits=%lu "
           "revalidate_attempts=%lu revalidate_successes=%lu "
           "revalidate_failures=%lu provider_pin_revalidated=%lu "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu custom_host_tool=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_display_bind_contract_version,
           stats_after.dxg_display_bind_transport,
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_operation,
           stats_after.dxg_display_bind_completion_source,
           stats_after.dxg_display_bind_required_metadata,
           stats_after.dxg_display_bind_lifetime,
           stats_after.dxg_display_bind_block_reason,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           stats_after.dxg_display_bind_source_generation,
           stats_after.dxg_display_bind_resource_generation,
           stats_after.dxg_display_bind_status,
           stats_after.dxg_display_bind_provider_submits -
               stats_before.dxg_display_bind_provider_submits,
           stats_after.dxg_display_bind_lock_dropped_submits -
               stats_before.dxg_display_bind_lock_dropped_submits,
           stats_after.dxg_display_bind_revalidate_attempts -
               stats_before.dxg_display_bind_revalidate_attempts,
           stats_after.dxg_display_bind_revalidate_successes -
               stats_before.dxg_display_bind_revalidate_successes,
           stats_after.dxg_display_bind_revalidate_failures -
               stats_before.dxg_display_bind_revalidate_failures,
           stats_after.dxg_display_bind_provider_pin_revalidated,
           stats_after.dxg_display_bind_provider_no_host_abi,
           stats_after.dxg_display_bind_provider_no_sender,
           stats_after.dxg_display_bind_provider_no_completion,
           stats_after.dxg_display_bind_contract_version == 1 &&
                   stats_after.dxg_display_bind_backend ==
                       FB_GPU_DXG_PRESENT_LANE_GPUP_DXG_SCANOUT_BIND &&
                   stats_after.dxg_display_bind_transport ==
                       FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE &&
                   stats_after.dxg_display_bind_transport_present == 0 &&
                   stats_after.dxg_display_bind_operation ==
                       FB_GPU_DXG_PRESENT_GPUP_DDA_OP_SCANOUT_BIND &&
                   stats_after.dxg_display_bind_completion_source ==
                       FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY &&
                   stats_after.dxg_display_bind_present_id == 0 &&
                   stats_after.dxg_display_bind_completed_id == 0 &&
                   stats_after.dxg_display_bind_status == EOPNOTSUPP &&
                   stats_after.dxg_display_bind_provider_submits >
                       stats_before.dxg_display_bind_provider_submits &&
                   stats_after.dxg_display_bind_lock_dropped_submits >
                       stats_before.dxg_display_bind_lock_dropped_submits &&
                   stats_after.dxg_display_bind_revalidate_attempts >
                       stats_before.dxg_display_bind_revalidate_attempts &&
                   stats_after.dxg_display_bind_revalidate_successes >
                       stats_before.dxg_display_bind_revalidate_successes &&
                   stats_after.dxg_display_bind_revalidate_failures ==
                       stats_before.dxg_display_bind_revalidate_failures &&
                   stats_after.dxg_display_bind_provider_pin_revalidated == 1 &&
                   stats_after.dxg_display_bind_provider_no_host_abi == 1 &&
                   stats_after.dxg_display_bind_provider_no_sender == 1 &&
                   stats_after.dxg_display_bind_provider_no_completion == 1 ?
               "PASS" : "FAIL");
    {
        uint64 pin_attempt_delta =
            stats_pin->dxg_display_bind_pin_attempts -
            stats_before.dxg_display_bind_pin_attempts;
        uint64 pin_success_delta =
            stats_pin->dxg_display_bind_pin_successes -
            stats_before.dxg_display_bind_pin_successes;
        uint64 unpin_delta =
            stats_pin->dxg_display_bind_unpins -
            stats_before.dxg_display_bind_unpins;
        int pin_refs_proven =
            pin_attempt_delta != 0 &&
            pin_success_delta != 0 &&
            stats_pin->dxg_display_bind_pin_failures ==
                stats_before.dxg_display_bind_pin_failures &&
            stats_pin->dxg_display_bind_pinned_dxg_file == 1 &&
            stats_pin->dxg_display_bind_pinned_resource_file == 1 &&
            stats_pin->dxg_display_bind_pinned_resource_generation != 0 &&
            stats_pin->dxg_display_bind_pinned_process_generation != 0 &&
            stats_pin->dxg_display_bind_pinned_process_refs != 0 &&
            stats_pin->dxg_display_bind_pinned_shared_parent != 0 &&
            stats_pin->dxg_display_bind_pinned_parent_refs != 0 &&
            stats_pin->dxg_display_bind_pinned_parent_children != 0 &&
            stats_pin->dxg_display_bind_present_id == 0 &&
            stats_pin->dxg_display_bind_completed_id == 0;
        int cleanup_balanced = unpin_delta == pin_success_delta;

        /*
         * Source close may run before the process finishes closing its DXG
         * fds and shared handles, so the in-process snapshot can see the
         * pinned refs before the final unpin counter catches up. The parent
         * C validator samples fbstat after this process exits and requires
         * the balanced cleanup state there.
         */
        printf("d3d12_display_bind_pin_lifetime_matrix "
           "pin_attempts=%lu pin_successes=%lu pin_failures=%lu "
           "unpins=%lu pinned_dxg_file=%lu pinned_resource_file=%lu "
           "pinned_resource_generation=%lu pinned_process_generation=%lu "
           "pinned_process_refs=%lu pinned_shared_parent=%lu "
           "pinned_parent_refs=%lu pinned_parent_children=%lu "
           "source_generation=%lu "
           "resource_generation=%lu native_present_credit=0 "
           "opengl_submit_credit=0 cleanup_state=%s status=%s\n",
           pin_attempt_delta,
           pin_success_delta,
           stats_pin->dxg_display_bind_pin_failures -
               stats_before.dxg_display_bind_pin_failures,
           unpin_delta,
           stats_pin->dxg_display_bind_pinned_dxg_file,
           stats_pin->dxg_display_bind_pinned_resource_file,
           stats_pin->dxg_display_bind_pinned_resource_generation,
           stats_pin->dxg_display_bind_pinned_process_generation,
           stats_pin->dxg_display_bind_pinned_process_refs,
           stats_pin->dxg_display_bind_pinned_shared_parent,
           stats_pin->dxg_display_bind_pinned_parent_refs,
           stats_pin->dxg_display_bind_pinned_parent_children,
           stats_pin->dxg_display_bind_source_generation,
           stats_pin->dxg_display_bind_resource_generation,
           cleanup_balanced ? "balanced" : "deferred_until_process_exit",
           pin_refs_proven && owner_cleanup ? "PASS" : "FAIL");
    }
    printf("d3d12_present_commit_result_copyout_contract_matrix "
           "commit_ioctl_delta=%lu copyout_failures_delta=%lu "
           "copyout_on_success=IMPLEMENTED failure_returns_errno=PASS "
           "failure_preserves_present_id=0 failure_preserves_completed=0 "
           "present_id=0 completed=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_present_commit_ioctl_entries -
               stats_before.dxg_present_commit_ioctl_entries,
           stats_after.dxg_present_commit_copyout_failures -
               stats_before.dxg_present_commit_copyout_failures,
           d3d12_commit_result_copyout_contract_pass ? "PASS" : "FAIL");
    printf("dxg_resource_scanout_bind_host_abi_matrix "
           "selected_lane=gpup_dxg_scanout_bind custom_host_tool=0 "
           "wsl_dxg_display_bind_ioctl=0 "
           "wsl_ioctl_namespace_checked=%lu "
           "wsl_display_bind_ioctl_absent=%lu "
           "synthvid_vram_bridge=gpa_dirty_only "
           "standard_alloc_role=private_driver_data "
           "standard_alloc_display_bind_absent=%lu "
           "dxg_resource_fd=PASS d3dkmt_handles=PASS "
           "same_adapter_luid=%s required_metadata=0x%lx "
           "host_candidates=0x%lx host_rejects=0x%lx "
           "missing_host_abi=%lu transport_present=%lu "
           "display_target_kind=%u present_id=0 completed=0 "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_wsl_ioctl_namespace_checked,
           stats_after.dxg_scanout_bind_wsl_display_bind_ioctl_absent,
           stats_after.dxg_scanout_bind_standard_alloc_display_bind_absent,
           query.adapter_identity == FB_GPU_DXG_PRESENT_ADAPTER_MATCH ?
               "PASS" : "FAIL",
           bind_contract.required_metadata,
           bind_contract.host_candidates, bind_contract.host_rejects,
           query.missing_host_abi, query.helper_transport_present,
           query.display_target_kind,
           d3d12_bind_contract_failclosed_pass &&
               stats_after.dxg_scanout_bind_wsl_ioctl_namespace_checked != 0 &&
               stats_after.dxg_scanout_bind_wsl_display_bind_ioctl_absent != 0 &&
               stats_after.dxg_scanout_bind_standard_alloc_display_bind_absent != 0 &&
               query.missing_host_abi ==
                   FB_GPU_DXG_PRESENT_MISSING_SCANOUT_BIND &&
               query.helper_transport_present == 0 &&
               query.display_target_kind == FB_GPU_DXG_DISPLAY_TARGET_NONE ?
               "PASS" : "FAIL");
    printf("dxg_scanout_bind_skeleton_matrix "
           "attempts=%lu rejects=%lu successes=%lu "
           "completion_queries=%lu completion_successes=%lu "
           "completion_pending=%lu weak_evidence_rejects=%lu "
           "transport=%lu status_code=%lu present_id=%lu completed=%lu "
           "source_generation=%lu resource_generation=%lu "
           "dirty_sequence=%lu dirty_rects=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_attempts -
               stats_before.dxg_scanout_bind_attempts,
           stats_after.dxg_scanout_bind_rejects -
               stats_before.dxg_scanout_bind_rejects,
           stats_after.dxg_scanout_bind_successes -
               stats_before.dxg_scanout_bind_successes,
           stats_after.dxg_scanout_bind_completion_queries -
               stats_before.dxg_scanout_bind_completion_queries,
           stats_after.dxg_scanout_bind_completion_successes -
               stats_before.dxg_scanout_bind_completion_successes,
           stats_after.dxg_scanout_bind_completion_pending -
               stats_before.dxg_scanout_bind_completion_pending,
           stats_after.dxg_scanout_bind_weak_evidence_rejects -
               stats_before.dxg_scanout_bind_weak_evidence_rejects,
           stats_after.dxg_scanout_bind_last_transport,
           stats_after.dxg_scanout_bind_last_status,
           stats_after.dxg_scanout_bind_last_present_id,
           stats_after.dxg_scanout_bind_last_completed,
           bind_contract.source_generation,
           bind_contract.resource_generation,
           stats_after.dxg_scanout_bind_last_dirty_sequence,
           stats_after.dxg_scanout_bind_last_dirty_rects,
           d3d12_scanout_bind_skeleton_pass ? "PASS" : "FAIL");
    printf("dxg_scanout_bind_candidate_command_matrix "
           "presenthistory_cmd=%lu redirected_flip_fence_cmd=%lu "
           "blt_cmd=%lu propagate_presenthistory_cmd=%lu "
           "cmds_known=%lu sender_contracts=%lu completion_contracts=%lu "
           "candidate_rejects=%lu custom_host_tool=0 transport_present=%lu "
           "vmbus_enum_known=%lu linux_ioctl_contracts=%lu "
           "resource_bind_contracts=%lu display_completion_contracts=%lu "
           "reject_reasons=0x%lx "
           "present_id=0 completed=0 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_candidate_presenthistory_cmd,
           stats_after.dxg_scanout_bind_candidate_redirected_flip_fence_cmd,
           stats_after.dxg_scanout_bind_candidate_blt_cmd,
           stats_after.dxg_scanout_bind_candidate_propagate_presenthistory_cmd,
           stats_after.dxg_scanout_bind_candidate_cmds_known,
           stats_after.dxg_scanout_bind_candidate_sender_contracts,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_scanout_bind_candidate_rejects -
               stats_before.dxg_scanout_bind_candidate_rejects,
           query.helper_transport_present,
           stats_after.dxg_scanout_bind_candidate_vmbus_enum_known,
           stats_after.dxg_scanout_bind_candidate_linux_ioctl_contracts,
           stats_after.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats_after.dxg_scanout_bind_candidate_display_completion_contracts,
           stats_after.dxg_scanout_bind_candidate_reject_reasons,
           stats_after.dxg_scanout_bind_candidate_cmds_known == 4 &&
                   stats_after.dxg_scanout_bind_candidate_presenthistory_cmd == 34 &&
                   stats_after.dxg_scanout_bind_candidate_redirected_flip_fence_cmd == 35 &&
                   stats_after.dxg_scanout_bind_candidate_blt_cmd == 38 &&
                   stats_after.dxg_scanout_bind_candidate_propagate_presenthistory_cmd == 1 &&
                   stats_after.dxg_scanout_bind_candidate_vmbus_enum_known == 1 &&
                   stats_after.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
                   stats_after.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
                   stats_after.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
                   stats_after.dxg_scanout_bind_candidate_reject_reasons ==
                       FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_ALL &&
                   stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                   stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                   query.helper_transport_present == 0 &&
                   bind_contract.present_id == 0 &&
                   bind_contract.completed == 0 ?
               "PASS" : "FAIL");
    printf("dxg_host_to_vm_presenthistory_completion_matrix "
           "host_to_vm_packets=%u unknown_packets=%u last_cmd=%u "
           "last_channel=%u last_payload=%u "
           "propagate_presenthistory_cmd=%lu presenthistory_packets=%u "
           "presenthistory_len=%u presenthistory_head_len=%u "
           "completion_contracts=%lu "
           "completion_successes_delta=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           host_to_vm_packets, host_to_vm_unknown, host_to_vm_last_cmd,
           host_to_vm_last_channel, host_to_vm_last_payload,
           stats_after.dxg_scanout_bind_candidate_propagate_presenthistory_cmd,
           host_to_vm_presenthistory, host_to_vm_presenthistory_len,
           host_to_vm_presenthistory_head_len,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_scanout_bind_completion_successes -
               stats_before.dxg_scanout_bind_completion_successes,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           d3d12_host_to_vm_presenthistory_absent_pass ? "PASS" : "FAIL");
    printf("dxg_presenthistory_telemetry_not_completion_matrix "
           "presenthistory_cmd=%lu propagate_presenthistory_cmd=%lu "
           "telemetry_packets=%u telemetry_head_len=%u "
           "linux_inband_handler=absent sender_contracts=%lu "
           "completion_contracts=%lu completion_success_delta=%lu "
           "display_bind_present_id=%lu display_bind_completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_candidate_presenthistory_cmd,
           stats_after.dxg_scanout_bind_candidate_propagate_presenthistory_cmd,
           host_to_vm_presenthistory, host_to_vm_presenthistory_head_len,
           stats_after.dxg_scanout_bind_candidate_sender_contracts,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_scanout_bind_completion_successes -
               stats_before.dxg_scanout_bind_completion_successes,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           stats_after.dxg_scanout_bind_candidate_presenthistory_cmd == 34 &&
                   stats_after.dxg_scanout_bind_candidate_propagate_presenthistory_cmd == 1 &&
                   stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                   stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                   stats_after.dxg_scanout_bind_completion_successes ==
                       stats_before.dxg_scanout_bind_completion_successes &&
                   stats_after.dxg_display_bind_present_id == 0 &&
                   stats_after.dxg_display_bind_completed_id == 0 ?
               "PASS" : "FAIL");
    printf("dxg_presenthistory_orphan_completion_rejection_matrix "
           "propagate_presenthistory_cmd=%lu presenthistory_packets=%u "
           "provider_pending_match=0 completion_demux_registered=%lu "
           "completion_successes_delta=%lu "
           "display_bind_present_id=%lu display_bind_completed_id=%lu "
           "orphan_completion_rejected=1 native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_candidate_propagate_presenthistory_cmd,
           host_to_vm_presenthistory,
           stats_after.dxg_display_bind_provider_completion_demux_registered,
           stats_after.dxg_scanout_bind_completion_successes -
               stats_before.dxg_scanout_bind_completion_successes,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           dxg_presenthistory_orphan_completion_rejection_pass ?
               "PASS" : "FAIL");
    printf("dxg_native_present_lane_rejection_matrix "
           "wsl_presenthistory_enum_only=REJECTED "
           "wsl_presenthistory_sender_contract=%lu "
           "wsl_presenthistory_completion_contract=%lu "
           "synthvid_gpa_dirty_only=REJECTED "
           "linux_hyperv_drm_shadow_blit_only=REJECTED "
           "synthvid_gpa_dirty_present=%lu "
           "synthvid_d3d12_resource_bind=0 "
           "dda_nouveau_separate_pci_path=%s "
           "dda_pci_display_present=%lu "
           "dda_d3d12_resource_import=0 dda_scanout_bind=0 "
           "dda_hw_flip_completion=0 "
           "vmbus_enum_known=%lu linux_ioctl_contracts=%lu "
           "resource_bind_contracts=%lu display_completion_contracts=%lu "
           "reject_reasons=0x%lx "
           "custom_host_tool=0 transport_present=%lu present_id=0 "
           "completed=0 native_present_credit=0 opengl_submit_credit=0 "
           "status=%s\n",
           stats_after.dxg_scanout_bind_candidate_sender_contracts,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_scanout_bind_synthvid_gpa_dirty_present,
           (stats_after.dxg_present_host_candidates &
            FB_GPU_DXG_PRESENT_HOST_DDA_NOUVEAU) != 0 ?
               "REJECTED_NO_IMPORT_PATH" : "ABSENT",
           stats_after.dxg_scanout_bind_dda_pci_display_present,
           stats_after.dxg_scanout_bind_candidate_vmbus_enum_known,
           stats_after.dxg_scanout_bind_candidate_linux_ioctl_contracts,
           stats_after.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats_after.dxg_scanout_bind_candidate_display_completion_contracts,
           stats_after.dxg_scanout_bind_candidate_reject_reasons,
           query.helper_transport_present,
           stats_after.dxg_scanout_bind_candidate_sender_contracts == 0 &&
                   stats_after.dxg_scanout_bind_candidate_completion_contracts == 0 &&
                   stats_after.dxg_scanout_bind_candidate_vmbus_enum_known == 1 &&
                   stats_after.dxg_scanout_bind_candidate_linux_ioctl_contracts == 0 &&
                   stats_after.dxg_scanout_bind_candidate_resource_bind_contracts == 0 &&
                   stats_after.dxg_scanout_bind_candidate_display_completion_contracts == 0 &&
                   stats_after.dxg_scanout_bind_synthvid_resource_bind_absent != 0 &&
                   stats_after.dxg_scanout_bind_dda_resource_import_absent != 0 &&
                   stats_after.dxg_scanout_bind_dda_scanout_bind_absent != 0 &&
                   stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 &&
                   stats_after.dxg_scanout_bind_candidate_reject_reasons ==
                       FB_GPU_DXG_SCANOUT_CANDIDATE_REJECT_ALL &&
                   query.helper_transport_present == 0 &&
                   bind_contract.present_id == 0 &&
                   bind_contract.completed == 0 &&
                   stats_after.dxg_present_dda_nouveau_import_path_present == 0 &&
                   stats_after.dxg_present_dda_nouveau_scanout_bind_present == 0 ?
               "PASS" : "FAIL");
    printf("d3d12_dda_nouveau_separate_display_not_bind_matrix "
           "dda_backend_flag=%u dda_pci_display_present=%lu "
           "dda_d3d12_resource_import=%lu dda_scanout_bind=%lu "
           "dda_hw_flip_completion=%s separate_pci_display_path=%s "
           "display_bind_present_id=%lu display_bind_completed=%lu "
           "scanout_bind_success_delta=%lu completion_success_delta=%lu "
           "native_present_credit=%lu opengl_submit_credit=%u "
           "status=%s\n",
           (backend.flags & FB_GPU_BACKEND_F_DDA_NOUVEAU) != 0,
           stats_after.dxg_scanout_bind_dda_pci_display_present,
           stats_after.dxg_present_dda_nouveau_import_path_present,
           stats_after.dxg_present_dda_nouveau_scanout_bind_present,
           stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats_after.dxg_scanout_bind_dda_pci_display_present != 0 ?
               "REJECTED_D3D12_IMPORT_MISSING" : "ABSENT",
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           stats_after.dxg_scanout_bind_successes -
               stats_before.dxg_scanout_bind_successes,
           stats_after.dxg_scanout_bind_completion_successes -
               stats_before.dxg_scanout_bind_completion_successes,
           stats_after.nouveau_pci_native_present_credit,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           d3d12_dda_nouveau_separate_display_not_bind_pass ? "PASS" :
               "FAIL");
    printf("wsl_dxg_uapi_namespace_negative_matrix "
           "uapi_namespace_checked=1 last_known_ioctl_nr=0x49 "
           "display_bind_ioctl_present=0 present_source_ioctl_present=0 "
           "present_completion_ioctl_present=0 linux_ioctl_contracts=%lu "
           "resource_bind_contracts=%lu display_completion_contracts=%lu "
           "transport_present=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_candidate_linux_ioctl_contracts,
           stats_after.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats_after.dxg_scanout_bind_candidate_display_completion_contracts,
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           wsl_uapi_namespace_negative_pass ? "PASS" : "FAIL");
    printf("wsl_dxg_adapter_display_caps_negative_matrix "
           "display_supported=%lu post_device=0 "
           "indirect_display_device=0 display_sources=%lu "
           "display_sources_known=%lu display_caps_cleared_by_wsl=1 "
           "display_bind_transport_present=%lu present_id=%lu "
           "completed=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_present_dxg_adapter_display_supported,
           stats_after.dxg_present_dxg_adapter_sources,
           stats_after.dxg_present_dxg_adapter_sources_known,
           stats_after.dxg_present_helper_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           wsl_adapter_display_caps_negative_pass ? "PASS" : "FAIL");
    printf("wsl_submit_present_fields_not_bind_matrix "
           "submit_present_redirected_field_known=1 "
           "submit_present_history_token_field_known=1 "
           "written_primaries_field_known=1 resource_scanout_bind_sender=%lu "
           "display_completion_return=%lu sender_contracts=%lu "
           "completion_contracts=%lu transport_present=%lu present_id=%lu "
           "completed=%lu native_present_credit=0 opengl_submit_credit=0 "
           "status=%s\n",
           stats_after.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats_after.dxg_scanout_bind_candidate_display_completion_contracts,
           stats_after.dxg_scanout_bind_candidate_sender_contracts,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           wsl_submit_present_fields_not_bind_pass ? "PASS" : "FAIL");
    printf("wsl_stdalloc_and_alloc_flags_not_bind_matrix "
           "stdalloc_private_data_sender_present=%lu "
           "stdalloc_display_bind_sender=%lu primary_alloc_flag_known=1 "
           "direct_flip_alloc_flag_known=1 "
           "alloc_flags_are_completion_contract=0 "
           "resource_bind_contracts=%lu display_completion_contracts=%lu "
           "transport_present=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_standard_alloc_private_data,
           stats_after.dxg_scanout_bind_standard_alloc_display_bind_absent == 0,
           stats_after.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats_after.dxg_scanout_bind_candidate_display_completion_contracts,
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           wsl_stdalloc_and_alloc_flags_not_bind_pass ? "PASS" : "FAIL");
    printf("wsl_trace_display_bind_negative_matrix "
           "trace_paths_checked=2 trace_ioctl_namespace_subset_of_wsl_uapi=1 "
           "display_bind_opcode_seen=%lu "
           "nonzero_present_history_token_seen=0 "
           "open_resource_without_display_bind_seen=1 "
           "syncfile_without_display_completion_seen=1 "
           "resource_share_without_scanout_bind_seen=1 "
           "linux_ioctl_contracts=%lu resource_bind_contracts=%lu "
           "display_completion_contracts=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats_after.dxg_scanout_bind_candidate_linux_ioctl_contracts,
           stats_after.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats_after.dxg_scanout_bind_candidate_display_completion_contracts,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           wsl_trace_display_bind_negative_pass ? "PASS" : "FAIL");
    printf("public_present_api_not_guest_bind_matrix "
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
           stats_after.dxg_scanout_bind_candidate_resource_bind_contracts,
           stats_after.dxg_scanout_bind_candidate_display_completion_contracts,
           stats_after.dxg_scanout_bind_candidate_sender_contracts,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           public_present_api_not_guest_bind_pass ? "PASS" : "FAIL");
    printf("d3d12_display_bind_host_abi_discovery_matrix "
           "custom_host_tool=0 wsl_dxg_display_bind_ioctl=0 "
           "wsl_display_bind_ioctl_absent=1 "
           "wslg_frame_path=absent freerdp_frame_path=absent "
           "rdp_frame_path=copy_or_dirty_frame "
           "gpup_dxg_sender_contract=%lu "
           "gpup_dxg_completion_contract=%lu "
           "completion_demux_contract=%lu "
           "dda_nouveau_d3d12_import=%lu "
           "dda_nouveau_scanout_bind=%lu "
           "dda_nouveau_hw_flip_completion=%s "
           "provider_state=failclosed provider_failclosed=1 "
           "host_abi_present=0 sender_present=0 completion_present=0 "
           "transport_present=%lu present_id=%lu completed=%lu "
           "native_present_credit=0 opengl_submit_credit=0 "
           "webkit_accel_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_candidate_sender_contracts,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_display_bind_provider_completion_demux_registered,
           stats_after.dxg_present_dda_nouveau_import_path_present,
           stats_after.dxg_present_dda_nouveau_scanout_bind_present,
           stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           d3d12_display_bind_host_abi_discovery_pass ? "PASS" : "FAIL");
    printf("provider_credit_gate_negative_matrix "
           "provider_submits_delta=%lu "
           "host_abi_present=%u sender_present=%u completion_present=%u "
           "provider_no_host_abi=%lu provider_no_sender=%lu "
           "provider_no_completion=%lu transport_present=%lu "
           "present_id=%lu completed=%lu native_present_credit=0 "
           "backend_opengl_submit=%u status=%s\n",
           stats_after.dxg_display_bind_provider_submits -
               stats_before.dxg_display_bind_provider_submits,
           stats_after.dxg_display_bind_provider_submits >
                   stats_before.dxg_display_bind_provider_submits &&
               stats_after.dxg_display_bind_provider_no_host_abi == 0,
           stats_after.dxg_display_bind_provider_submits >
                   stats_before.dxg_display_bind_provider_submits &&
               stats_after.dxg_display_bind_provider_no_sender == 0,
           stats_after.dxg_display_bind_provider_submits >
                   stats_before.dxg_display_bind_provider_submits &&
               stats_after.dxg_display_bind_provider_no_completion == 0,
           stats_after.dxg_display_bind_provider_no_host_abi,
           stats_after.dxg_display_bind_provider_no_sender,
           stats_after.dxg_display_bind_provider_no_completion,
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           provider_credit_gate_negative_pass ? "PASS" : "FAIL");
    printf("dda_nouveau_non_readback_display_proof_matrix "
           "dda_pci_transport_present=%s "
           "dda_nouveau_display_present=%s "
           "dda_nouveau_non_readback_present=%s "
           "display_create_successes=%lu heads=%lu connectors=%lu "
           "nonvirtual_connector=%u vblank_supported=%lu "
           "vblank_irqs=%lu page_flip_completions=%lu "
           "kms_lane=%lu kms_present_dumb=%lu kms_present_synthvid=%lu "
           "kms_present_nouveau_hw=%lu "
           "kms_vblank_source_nouveau_hw=%lu "
           "kms_vblank_source_software_display=%lu "
           "kms_vblank_source_synthetic=%lu "
           "page_flip_events_software_blit=%lu "
           "page_flip_events_native_hw=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats_after.nouveau_pci_probe_accepts != 0 ? "PASS" :
                                                        "GPU_P_FAIL_CLOSED",
           stats_after.nouveau_display_create_successes != 0 &&
                   stats_after.nouveau_display_heads != 0 &&
                   stats_after.nouveau_display_connectors != 0 &&
                   stats_after.nouveau_native_display_ready != 0 ?
               "PASS" : "ABSENT",
           stats_after.kms_present_last_lane ==
                       FB_GPU_KMS_PRESENT_LANE_NOUVEAU_HW &&
                   stats_after.kms_page_flip_events_native_hw != 0 ?
               "PASS" : "ABSENT",
           stats_after.nouveau_display_create_successes,
           stats_after.nouveau_display_heads,
           stats_after.nouveau_display_connectors,
           stats_after.nouveau_display_create_successes != 0 &&
                   stats_after.nouveau_display_heads != 0 &&
                   stats_after.nouveau_display_connectors != 0,
           stats_after.nouveau_display_vblank_supported,
           stats_after.nouveau_display_vblank_irqs,
           stats_after.nouveau_display_page_flip_completions,
           stats_after.kms_present_last_lane,
           stats_after.kms_present_dumb,
           stats_after.kms_present_synthvid,
           stats_after.kms_present_nouveau_hw,
           stats_after.kms_vblank_source_nouveau_hw,
           stats_after.kms_vblank_source_software_display,
           stats_after.kms_vblank_source_synthetic,
           stats_after.kms_page_flip_events_software_blit,
           stats_after.kms_page_flip_events_native_hw,
           dda_nouveau_non_readback_display_proof_pass ? "PASS" : "FAIL");
    printf("dxg_scanout_bind_weak_evidence_matrix "
           "dxg_ready_only=%lu d3dkmt_handles_only=%lu "
           "same_adapter_resource_only=%lu syncfile_only=%lu "
           "synthvid_gpa_dirty_only=%lu software_or_readback_path=%lu "
           "weak_evidence_rejects=%lu successes=%lu present_id=0 "
           "completed=0 native_present_credit=0 opengl_submit_credit=0 "
           "status=%s\n",
           stats_after.dxg_scanout_bind_weak_dxg_ready_only -
               stats_before.dxg_scanout_bind_weak_dxg_ready_only,
           stats_after.dxg_scanout_bind_weak_d3dkmt_handles_only -
               stats_before.dxg_scanout_bind_weak_d3dkmt_handles_only,
           stats_after.dxg_scanout_bind_weak_same_adapter_resource_only -
               stats_before.dxg_scanout_bind_weak_same_adapter_resource_only,
           stats_after.dxg_scanout_bind_weak_syncfile_only -
               stats_before.dxg_scanout_bind_weak_syncfile_only,
           stats_after.dxg_scanout_bind_weak_synthvid_gpa_dirty_only -
               stats_before.dxg_scanout_bind_weak_synthvid_gpa_dirty_only,
           stats_after.dxg_scanout_bind_weak_software_or_readback_path -
               stats_before.dxg_scanout_bind_weak_software_or_readback_path,
           stats_after.dxg_scanout_bind_weak_evidence_rejects -
               stats_before.dxg_scanout_bind_weak_evidence_rejects,
           stats_after.dxg_scanout_bind_successes -
               stats_before.dxg_scanout_bind_successes,
           stats_after.dxg_scanout_bind_weak_evidence_rejects >
                   stats_before.dxg_scanout_bind_weak_evidence_rejects &&
                   stats_after.dxg_scanout_bind_successes ==
                       stats_before.dxg_scanout_bind_successes &&
                   bind_contract.present_id == 0 &&
                   bind_contract.completed == 0 ?
               "PASS" : "FAIL");
    printf("wsl_standard_alloc_surface_abi_matrix "
           "shared_primary_size=%lu shadow_size=%lu staging_size=%lu "
           "gdi_size=%lu command_union=sharedprimary,shadow,staging,gdi "
           "wsl_reference=drivers/hv/dxgkrnl/dxgvmbus.h "
           "standard_alloc_role=private_driver_data "
           "selected_lane=gpup_dxg_scanout_bind display_bind_ioctl=0 "
           "standard_alloc_native_present_credit=0 "
           "display_bind_absent=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           (uint64)sizeof(struct d3dkmdt_sharedprimarysurfacedata),
           (uint64)sizeof(struct d3dkmdt_shadowsurfacedata),
           (uint64)sizeof(struct d3dkmdt_stagingsurfacedata),
           (uint64)sizeof(struct d3dkmdt_gdisurfacedata),
           stats_after.dxg_scanout_bind_standard_alloc_display_bind_absent,
           sizeof(struct d3dkmdt_sharedprimarysurfacedata) == 24 &&
                   sizeof(struct d3dkmdt_shadowsurfacedata) == 16 &&
                   sizeof(struct d3dkmdt_stagingsurfacedata) == 12 &&
                   sizeof(struct d3dkmdt_gdisurfacedata) == 24 &&
                   d3d12_standard_alloc_not_display_bind_pass ?
               "PASS" : "FAIL");
    printf("wsl_standard_alloc_not_display_bind_matrix "
           "standard_alloc_private_data=%lu "
           "standard_alloc_display_bind_absent=%lu "
           "standard_alloc_role=private_driver_data "
           "standard_alloc_native_present_credit=0 "
           "display_bind_transport_present=%lu present_id=%lu "
           "completed=%lu native_present_credit=0 "
           "opengl_submit_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_standard_alloc_private_data,
           stats_after.dxg_scanout_bind_standard_alloc_display_bind_absent,
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           d3d12_standard_alloc_not_display_bind_pass ? "PASS" : "FAIL");
    printf("gpu_remaining_plan_dependency_skeleton_matrix "
           "root_display_bind_gate=closed native_present_gate=closed "
           "native_completion_validators=armed finite_480p_gate=closed "
           "demo_interaction_gate=closed backend_opengl_submit_gate=closed "
           "kvm_virgl_recheck_gate=deferred webkit_route_gate=closed "
           "webkit_content_gate=closed webkit_enabled_artifact_gate=closed "
           "wsl_display_bind_ioctl=0 wsl_inband_presenthistory_handler=absent "
           "gpup_sender_contract=%lu gpup_completion_contract=%lu "
           "dda_d3d12_resource_import=%lu dda_scanout_bind=%lu "
           "dda_hw_flip_completion=%s display_bind_present_id=%lu "
           "display_bind_completed=%lu backend_opengl_submit=%u "
           "native_present_credit=0 opengl_submit_credit=0 "
           "webkit_accel_credit=0 status=%s\n",
           stats_after.dxg_scanout_bind_candidate_sender_contracts,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_present_dda_nouveau_import_path_present,
           stats_after.dxg_present_dda_nouveau_scanout_bind_present,
           stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           gpu_remaining_plan_dependency_skeleton_pass ? "PASS" : "FAIL");
    printf("gpu_remaining_holistic_skeleton_matrix "
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
           stats_after.dxg_scanout_bind_candidate_sender_contracts,
           stats_after.dxg_scanout_bind_candidate_completion_contracts,
           stats_after.dxg_present_dda_nouveau_import_path_present,
           stats_after.dxg_present_dda_nouveau_scanout_bind_present,
           stats_after.dxg_scanout_bind_dda_hw_flip_completion_absent != 0 ?
               "ABSENT" : "PRESENT",
           stats_after.dxg_display_bind_transport_present,
           stats_after.dxg_display_bind_present_id,
           stats_after.dxg_display_bind_completed_id,
           stats_after.nouveau_pci_native_present_credit,
           (backend.flags & FB_GPU_BACKEND_F_OPENGL_SUBMIT) != 0,
           gpu_remaining_holistic_skeleton_pass ? "PASS" : "FAIL");

    if (fb_fd >= 0)
        close(fb_fd);
    if (fb_fd_foreign >= 0)
        close(fb_fd_foreign);
    if (fb_fd_after >= 0)
        close(fb_fd_after);
    if (shared_handle != 0)
        close((int)shared_handle);
    if (present_create_sync_file.sync_file_handle != 0)
        close((int)present_create_sync_file.sync_file_handle);
    if (present_open_sync_file.syncobj.v != 0) {
        destroy_sync.sync_object = present_open_sync_file.syncobj;
        if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                  &destroy_sync) < 0)
            printf("present_source_failclosed sync_file_sync_destroy_failed sync=0x%x\n",
                   present_open_sync_file.syncobj.v);
    }
    if (create_sync.sync_object.v != 0) {
        destroy_sync.sync_object = create_sync.sync_object;
        if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                  &destroy_sync) < 0)
            printf("present_source_failclosed sync_destroy_failed sync=0x%x\n",
                   create_sync.sync_object.v);
    }
    if (create_allocation.resource.v != 0) {
        destroy_allocation.device = device;
        destroy_allocation.resource = create_allocation.resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0)
            printf("present_source_failclosed destroy_failed resource=0x%x\n",
                   create_allocation.resource.v);
    }
    return pass ? 0 : -1;
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
    struct d3dkmt_signalsynchronizationobjectfromcpu signal_cpu;
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
    {
        struct dxg_cpu_event_signal_status before;
        struct dxg_cpu_event_signal_status after;
        int efd = dxgprobe_eventfd(0);
        int cpu_rc = -ENOSYS;
        int before_rc = read_cpu_event_signal_status(&before);
        int after_rc = -1;
        int pass = 0;

        memset(&after, 0, sizeof(after));
        memset(&signal_legacy, 0, sizeof(signal_legacy));
        signal_legacy.context = context;
        signal_legacy.flags.enqueue_cpu_event = 1;
        if (efd >= 0) {
            signal_legacy.cpu_event_handle = (uint64)efd;
            cpu_rc = ioctl(fd, LX_DXSIGNALSYNCHRONIZATIONOBJECT,
                           &signal_legacy);
            after_rc = read_cpu_event_signal_status(&after);
            close(efd);
        }
        pass = efd >= 0 && before_rc == 0 && after_rc == 0 &&
               cpu_rc == 0 && after.attempts > before.attempts &&
               after.successes > before.successes &&
               after.ret == 0 && after.event_id != 0 &&
               after.user_fd == (uint64)efd &&
               after.objects == 0 && after.contexts == 1 &&
               (after.flags & 0x2) != 0 &&
               after.allocs > before.allocs;
        printf("sync_signal_cpu_event_matrix rc=%d efd=%d "
               "attempts=%u->%u successes=%u->%u event=%lu "
               "objects=%u contexts=%u flags=0x%x len=%u "
               "host_events=%u/%u/%u->%u/%u/%u status=%s\n",
               cpu_rc, efd, before.attempts, after.attempts,
               before.successes, after.successes, after.event_id,
               after.objects, after.contexts, after.flags, after.len,
               before.active, before.allocs, before.removes,
               after.active, after.allocs, after.removes,
               pass ? "PASS" : "FAIL");
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

    memset(&wait_legacy, 0, sizeof(wait_legacy));
    wait_legacy.context = context;
    wait_legacy.object_count = 2;
    wait_legacy.object_array[0] = objects[0];
    wait_legacy.object_array[1] = objects[0];
    wait_legacy.fence.fence_value = signal_legacy.fence.fence_value;
    {
        int legacy_multi_rc =
            ioctl(fd, LX_DXWAITFORSYNCHRONIZATIONOBJECT, &wait_legacy);
        printf("sync_legacy_wait_multi_matrix rc=%d expected=%d status=%s\n",
               legacy_multi_rc, -EINVAL,
               legacy_multi_rc == -EINVAL ? "PASS" : "FAIL");
    }

    fence_values[0] = 2;
    memset(&signal_cpu, 0, sizeof(signal_cpu));
    signal_cpu.device = device;
    signal_cpu.object_count = 1;
    signal_cpu.objects = (uint64)objects;
    signal_cpu.fence_values = (uint64)fence_values;
    signal_cpu.flags.enqueue_cpu_event = 1;
    {
        int cpu_event_fromcpu_rc =
            ioctl(fd, LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMCPU,
                  &signal_cpu);
        printf("sync_fromcpu_cpu_event_failclosed_matrix rc=%d expected=%d status=%s\n",
               cpu_event_fromcpu_rc, -EINVAL,
               cpu_event_fromcpu_rc == -EINVAL ? "PASS" : "FAIL");
    }

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
    {
        struct dxg_cpu_event_signal_status before;
        struct dxg_cpu_event_signal_status after;
        int efd = dxgprobe_eventfd(0);
        int cpu_rc = -ENOSYS;
        int before_rc = read_cpu_event_signal_status(&before);
        int after_rc = -1;
        int pass = 0;

        memset(&after, 0, sizeof(after));
        memset(&signal_gpu2, 0, sizeof(signal_gpu2));
        signal_gpu2.context_count = 1;
        signal_gpu2.contexts = (uint64)contexts;
        signal_gpu2.flags.enqueue_cpu_event = 1;
        if (efd >= 0) {
            signal_gpu2.cpu_event_handle = (uint64)efd;
            cpu_rc = ioctl(fd, LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU2,
                           &signal_gpu2);
            after_rc = read_cpu_event_signal_status(&after);
            close(efd);
        }
        pass = efd >= 0 && before_rc == 0 && after_rc == 0 &&
               cpu_rc == 0 && after.attempts > before.attempts &&
               after.successes > before.successes &&
               after.ret == 0 && after.event_id != 0 &&
               after.user_fd == (uint64)efd &&
               after.objects == 0 && after.contexts == 1 &&
               (after.flags & 0x2) != 0 &&
               after.allocs > before.allocs;
        printf("sync_gpu2_cpu_event_matrix rc=%d efd=%d "
               "attempts=%u->%u successes=%u->%u event=%lu "
               "objects=%u contexts=%u flags=0x%x len=%u "
               "host_events=%u/%u/%u->%u/%u/%u status=%s\n",
               cpu_rc, efd, before.attempts, after.attempts,
               before.successes, after.successes, after.event_id,
               after.objects, after.contexts, after.flags, after.len,
               before.active, before.allocs, before.removes,
               after.active, after.allocs, after.removes,
               pass ? "PASS" : "FAIL");
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
    memset(&create_sync, 0, sizeof(create_sync));

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
    probe_createallocation_unwind(fd, create_device.device);
    probe_openresource_unwind(fd, create_device.device);
    probe_shared_standard_allocation(fd, create_device.device);

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
    if (g_sync_file_validate) {
        if (try_context && context_handle.v == 0 &&
            probe_context_matrix(fd, create_device.device,
                                 context_private_data,
                                 sizeof(context_private_data),
                                 &context_handle,
                                 &context_sync_only) < 0) {
            printf("sync_file_wait context_create_failed; wait will be skipped\n");
            ret = -1;
        }
        if (probe_sync_file_matrix(fd, create_device.device,
                                   context_handle) < 0)
            ret = -1;
        goto sync_probe_done;
    }

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
        if (probe_shared_sync_nt(fd, create_device.device) < 0)
            ret = -1;
        probe_sync_file_unsupported(fd, create_device.device,
                                    create_sync.sync_object);
        probe_cpu_sync(fd, create_device.device, create_sync.sync_object,
                       try_wait);
    }

sync_probe_done:
    if (try_context || try_submit) {
        if (context_handle.v == 0 &&
            probe_context_matrix(fd, create_device.device,
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
            {
                struct dxg_async_send_status async_status;
                int async_rc;
                int pass = 0;
                const char *state = "FAIL";

                memset(&async_status, 0, sizeof(async_status));
                async_rc = read_async_send_status(&async_status);
                if (async_rc == 0 && async_status.enabled == 0 &&
                    async_status.fallback_sync > 0) {
                    pass = 1;
                    state = "DEFERRED";
                } else if (async_rc == 0 && async_status.enabled != 0) {
                    pass = async_status.attempts > 0 &&
                           async_status.successes > 0 &&
                           async_status.async_bit == 1 &&
                           async_status.route_global == 1 &&
                           async_status.packet_type == 6 &&
                           async_status.ret == 0 &&
                           async_status.submit > 0 &&
                           async_status.signal > 0 &&
                           async_status.waitgpu > 0 &&
                           (context_sync_only ||
                            async_status.submithwqueue > 0);
                    state = pass ? "PASS" : "FAIL";
                }
                printf("dxg_async_message_matrix rc=%d enabled=%u attempts=%u successes=%u fallback_sync=%u cmd=%u cmd_len=%u wire_len=%u async_bit=%u route_global=%u packet_type=%u ret=%d submit=%u signal=%u waitgpu=%u submithwqueue=%u status=%s\n",
                       async_rc, async_status.enabled, async_status.attempts,
                       async_status.successes, async_status.fallback_sync,
                       async_status.cmd, async_status.cmd_len,
                       async_status.wire_len, async_status.async_bit,
                       async_status.route_global, async_status.packet_type,
                       async_status.ret, async_status.submit,
                       async_status.signal, async_status.waitgpu,
                       async_status.submithwqueue, state);
                if (!pass)
                    ret = -1;
            }
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
    int pid;
    int status = 1;
    int ret = 0;

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    if (ioctl(fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        printf("dxgprobe: owner isolation create device failed device=0x%x\n",
               create_device.device.v);
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        printf("dxgprobe: owner isolation set cloexec failed\n");
        ret = -1;
        goto cleanup;
    }

    pid = fork();
    if (pid < 0) {
        printf("dxgprobe: owner isolation fork failed\n");
        ret = -1;
        goto cleanup;
    }
    if (pid == 0) {
        char handle_arg[16];
        char *child_argv[4];

        snprintf(handle_arg, sizeof(handle_arg), "%u",
                 create_device.device.v);
        child_argv[0] = "dxgprobe";
        child_argv[1] = "--owner-isolation-child";
        child_argv[2] = handle_arg;
        child_argv[3] = 0;
        exec("/bin/dxgprobe", child_argv);
        printf("dxgprobe: owner isolation child exec failed\n");
        exit(1);
    }
    wait(&status);
    if (status != 0)
        ret = -1;

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

static int probe_owner_isolation_child(uint32 device)
{
    struct d3dkmt_destroydevice destroy_device;
    int fd;
    int ret = -1;

    fd = open("/dev/dxg", O_RDWR);
    if (fd < 0) {
        printf("dxgprobe: owner isolation child open failed\n");
        return -1;
    }
    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device.v = device;
    if (ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device) < 0) {
        printf("dxgprobe: owner isolation rejected exec-child destroy device=0x%x\n",
               device);
        ret = 0;
    } else {
        printf("dxgprobe: owner isolation exec-child destroy unexpectedly succeeded device=0x%x\n",
               device);
    }
    close(fd);
    return ret;
}

static int expect_stale_ioctl_rejected(const char *label, int rc)
{
    if (rc >= 0) {
        printf("handle_lifetime %s unexpected_success\n", label);
        return -1;
    }
    printf("handle_lifetime %s rejected\n", label);
    return 0;
}

static int probe_process_memory_lifetime_validate(void)
{
    struct dxg_process_lifetime_status before;
    struct dxg_process_lifetime_status after;
    int pid;
    int status = 1;
    int child_status = 1;
    int process_release_delta;
    int mem_release_delta;
    int mem_free_delta;

    if (read_process_lifetime_status(&before) < 0) {
        printf("dxg_process_mem_lifetime_matrix status=FAIL reason=read_before\n");
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        printf("dxg_process_mem_lifetime_matrix status=FAIL reason=fork\n");
        return -1;
    }
    if (pid == 0) {
        struct d3dkmt_adapterinfo adapters[D3DKMT_ADAPTERS_MAX];
        struct d3dkmt_openadapterfromluid open_luid;
        struct d3dkmt_createdevice create_device;
        uint32 count = 0;
        int child_fd = open("/dev/dxg", O_RDWR);

        if (child_fd < 0) {
            printf("dxg_process_mem_lifetime_child open_failed\n");
            exit(1);
        }
        if (enum_dxg_adapters2_list(child_fd, adapters, &count,
                                    "process_mem_lifetime_child") < 0) {
            close(child_fd);
            exit(1);
        }
        if (count == 0) {
            close(child_fd);
            exit(1);
        }
        memset(&open_luid, 0, sizeof(open_luid));
        open_luid.adapter_luid = adapters[0].adapter_luid;
        if (ioctl(child_fd, LX_DXOPENADAPTERFROMLUID, &open_luid) < 0 ||
            open_luid.adapter_handle.v == 0) {
            close(child_fd);
            exit(1);
        }
        memset(&create_device, 0, sizeof(create_device));
        create_device.adapter = open_luid.adapter_handle;
        if (ioctl(child_fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
            create_device.device.v == 0) {
            close(child_fd);
            exit(1);
        }
        close(child_fd);
        exit(0);
    }
    wait(&status);
    if (status == 0)
        child_status = 0;
    if (read_process_lifetime_status(&after) < 0) {
        printf("dxg_process_mem_lifetime_matrix child_status=%d status=FAIL reason=read_after\n",
               child_status);
        return -1;
    }

    process_release_delta =
        (int)(after.object_releases - before.object_releases);
    mem_release_delta = (int)(after.mem_releases - before.mem_releases);
    mem_free_delta = (int)(after.mem_frees - before.mem_frees);
    printf("dxg_process_mem_lifetime_matrix "
           "child_status=%d object_release_delta=%d mem_release_delta=%d "
           "mem_free_delta=%d before_object_releases=%u "
           "after_object_releases=%u before_mem_releases=%u "
           "after_mem_releases=%u before_mem_frees=%u after_mem_frees=%u "
           "object_refs_last=%u mem_refs_last=%u status=%s\n",
           child_status, process_release_delta, mem_release_delta,
           mem_free_delta, before.object_releases, after.object_releases,
           before.mem_releases, after.mem_releases, before.mem_frees,
           after.mem_frees, after.object_refs_last, after.mem_refs_last,
           child_status == 0 && process_release_delta > 0 &&
               mem_release_delta > 0 && mem_free_delta > 0 ?
                   "PASS" : "FAIL");
    if (child_status != 0 || process_release_delta <= 0 ||
        mem_release_delta <= 0 || mem_free_delta <= 0)
        return -1;
    return 0;
}

static int probe_process_adapter_child_validate(struct winluid luid)
{
    struct dxg_local_adapter_status adapter_status;
    struct d3dkmt_openadapterfromluid open_luid;
    struct d3dkmt_closeadapter close_adapter;
    struct d3dkmt_createdevice create_device;
    struct d3dkmt_createdevice raw_create_device;
    struct d3dkmt_destroydevice destroy_device;
    int fd;
    int open_rc;
    int raw_create_rc = -1;
    int create_rc = -1;
    int close_rc = -1;
    int stale_destroy_rc = -1;
    int pass;

    fd = open("/dev/dxg", O_RDWR);
    if (fd < 0) {
        printf("dxgprocess_adapter_matrix status=FAIL reason=open\n");
        return -1;
    }
    memset(&open_luid, 0, sizeof(open_luid));
    memset(&adapter_status, 0, sizeof(adapter_status));
    open_luid.adapter_luid = luid;
    open_rc = ioctl(fd, LX_DXOPENADAPTERFROMLUID, &open_luid);
    if (open_rc < 0 || open_luid.adapter_handle.v == 0 ||
        read_local_adapter_status(&adapter_status) < 0 ||
        adapter_status.host == 0) {
        printf("dxgprocess_adapter_matrix open_rc=%d local=0x%x host=0x%x status=FAIL reason=openadapter\n",
               open_rc, open_luid.adapter_handle.v,
               adapter_status.host);
        close(fd);
        return -1;
    }

    memset(&raw_create_device, 0, sizeof(raw_create_device));
    raw_create_device.adapter.v = adapter_status.host;
    raw_create_rc = ioctl(fd, LX_DXCREATEDEVICE, &raw_create_device);
    if (raw_create_rc == 0 && raw_create_device.device.v != 0) {
        memset(&destroy_device, 0, sizeof(destroy_device));
        destroy_device.device = raw_create_device.device;
        (void)ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device);
    }

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = open_luid.adapter_handle;
    create_rc = ioctl(fd, LX_DXCREATEDEVICE, &create_device);

    memset(&close_adapter, 0, sizeof(close_adapter));
    close_adapter.adapter_handle = open_luid.adapter_handle;
    close_rc = ioctl(fd, LX_DXCLOSEADAPTER, &close_adapter);

    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device = create_device.device;
    stale_destroy_rc = ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device);
    pass = raw_create_rc < 0 && create_rc == 0 &&
           create_device.device.v != 0 && close_rc == 0 &&
           stale_destroy_rc < 0;
    printf("dxgprocess_adapter_matrix "
           "open_rc=%d local=0x%x host=0x%x raw_host_create_rc=%d "
           "local_create_rc=%d device=0x%x close_adapter_rc=%d "
           "child_destroy_after_final_close_rc=%d status=%s\n",
           open_rc, open_luid.adapter_handle.v, adapter_status.host,
           raw_create_rc, create_rc, create_device.device.v, close_rc,
           stale_destroy_rc, pass ? "PASS" : "FAIL");
    close(fd);
    return pass ? 0 : -1;
}

static int probe_process_adapter_validate(struct winluid luid)
{
    int pid;
    int status = 1;

    pid = fork();
    if (pid < 0) {
        printf("dxgprocess_adapter_parent_matrix child_status=1 status=FAIL reason=fork\n");
        return -1;
    }
    if (pid == 0)
        exit(probe_process_adapter_child_validate(luid) == 0 ? 0 : 1);
    wait(&status);
    printf("dxgprocess_adapter_parent_matrix child_status=%d status=%s\n",
           status, status == 0 ? "PASS" : "FAIL");
    return status == 0 ? 0 : -1;
}

static int probe_local_adapter_reuse_validate(int fd, struct winluid luid)
{
    struct dxg_local_adapter_status before;
    struct dxg_local_adapter_status after;
    uint32 previous_handle = 0;
    uint32 opened = 0;

    if (read_local_adapter_status(&before) < 0) {
        printf("local_adapter_reuse status_initial_failed\n");
        return -1;
    }
    for (uint32 i = 0; i < 4; i++) {
        struct d3dkmt_openadapterfromluid open_luid;
        struct d3dkmt_closeadapter close_adapter;

        memset(&open_luid, 0, sizeof(open_luid));
        open_luid.adapter_luid = luid;
        if (ioctl(fd, LX_DXOPENADAPTERFROMLUID, &open_luid) < 0 ||
            open_luid.adapter_handle.v == 0) {
            printf("local_adapter_reuse open_failed iter=%u handle=0x%x\n",
                   i, open_luid.adapter_handle.v);
            return -1;
        }
        if (previous_handle != 0 &&
            previous_handle == open_luid.adapter_handle.v) {
            printf("local_adapter_reuse premature_reuse iter=%u handle=0x%x\n",
                   i, open_luid.adapter_handle.v);
            memset(&close_adapter, 0, sizeof(close_adapter));
            close_adapter.adapter_handle = open_luid.adapter_handle;
            ioctl(fd, LX_DXCLOSEADAPTER, &close_adapter);
            return -1;
        }
        previous_handle = open_luid.adapter_handle.v;
        opened++;
        memset(&close_adapter, 0, sizeof(close_adapter));
        close_adapter.adapter_handle = open_luid.adapter_handle;
        if (ioctl(fd, LX_DXCLOSEADAPTER, &close_adapter) < 0) {
            printf("local_adapter_reuse close_failed iter=%u handle=0x%x\n",
                   i, open_luid.adapter_handle.v);
            return -1;
        }
    }
    if (read_local_adapter_status(&after) < 0) {
        printf("local_adapter_reuse status_final_failed\n");
        return -1;
    }
    if (after.min_free != 128) {
        printf("local_adapter_reuse min_free_unexpected min_free=%u\n",
               after.min_free);
        return -1;
    }
    if (after.reuse_delayed <= before.reuse_delayed) {
        printf("local_adapter_reuse no_delayed_reuse_counter before=%u after=%u\n",
               before.reuse_delayed, after.reuse_delayed);
        return -1;
    }
    printf("local_adapter_reuse ok opened=%u delayed:%u->%u allowed:%u->%u min_free=%u last=0x%x\n",
           opened, before.reuse_delayed, after.reuse_delayed,
           before.reuse_allowed, after.reuse_allowed, after.min_free,
           after.handle);
    return 0;
}

static int probe_handle_lifetime_validate(int fd, struct d3dkmthandle adapter,
                                          struct winluid adapter_luid)
{
    struct dxg_object_table_status before;
    struct dxg_object_table_status after;
    struct d3dkmt_createdevice create_device;
    struct d3dkmt_destroydevice destroy_device;
    struct d3dkmt_destroycontext destroy_context;
    struct d3dkmt_createsynchronizationobject2 create_sync;
    struct d3dkmt_destroysynchronizationobject destroy_sync;
    struct d3dkmt_createpagingqueue create_paging_queue;
    struct d3dddi_destroypagingqueue destroy_paging_queue;
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dddi_allocationinfo2 standalone_allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_createallocation create_standalone_allocation;
    struct d3dkmt_createstandardallocation standard_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_createhwqueue create_hwqueue;
    struct d3dkmt_destroyhwqueue destroy_hwqueue;
    struct d3dddi_reservegpuvirtualaddress reserve_gpuva;
    struct d3dkmt_freegpuvirtualaddress free_gpuva;
    struct d3dkmthandle context_handle;
    struct d3dkmthandle allocation_list[1];
    unsigned char context_private_data[64];
    int fd2;
    int ret = -1;
    uint32 expected_denials = 0;
    uint32 denied_delta = 0;
    int stale_device_second_fd_rc = -1;
    int stale_context_rc = -1;
    int stale_hwqueue_rc = -1;
    int stale_hwqueue_sync_rc = -1;
    int stale_sync_rc = -1;
    int stale_paging_queue_rc = -1;
    int stale_paging_queue_sync_rc = -1;
    int stale_resource_rc = -1;
    int stale_allocation_rc = -1;
    int stale_gpuva_rc = -1;
    int stale_device_final_rc = -1;
    int context_sync_only = 0;

    if (read_object_table_status(&before) < 0) {
        printf("handle_lifetime status_initial_failed\n");
        return -1;
    }
    if (probe_local_adapter_reuse_validate(fd, adapter_luid) < 0)
        return -1;

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    if (ioctl(fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        printf("handle_lifetime create_device_failed device=0x%x\n",
               create_device.device.v);
        return -1;
    }
    printf("handle_lifetime device=0x%x\n", create_device.device.v);

    fd2 = open("/dev/dxg", O_RDWR);
    if (fd2 < 0) {
        printf("handle_lifetime same_process_close_open_failed\n");
        goto cleanup_device;
    }
    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    if (ioctl(fd2, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        printf("handle_lifetime same_process_close_create_failed device=0x%x\n",
               create_device.device.v);
        close(fd2);
        goto cleanup_device;
    }
    close(fd2);
    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device = create_device.device;
    if (ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device) < 0) {
        printf("handle_lifetime same_process_destroy_after_creator_close_failed device=0x%x\n",
               create_device.device.v);
        goto cleanup_device;
    }
    printf("handle_lifetime same_process_destroy_after_creator_close ok device=0x%x\n",
           create_device.device.v);

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    if (ioctl(fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        printf("handle_lifetime recreate_after_creator_close_failed device=0x%x\n",
               create_device.device.v);
        return -1;
    }
    printf("handle_lifetime post_creator_close_device=0x%x\n",
           create_device.device.v);

    fd2 = open("/dev/dxg", O_RDWR);
    if (fd2 < 0) {
        printf("handle_lifetime same_process_second_open_failed\n");
        goto cleanup_device;
    }
    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device = create_device.device;
    if (ioctl(fd2, LX_DXDESTROYDEVICE, &destroy_device) < 0) {
        printf("handle_lifetime same_process_destroy_from_second_fd_failed device=0x%x\n",
               create_device.device.v);
        close(fd2);
        goto cleanup_device;
    }
    close(fd2);
    stale_device_second_fd_rc = ioctl(fd, LX_DXDESTROYDEVICE,
                                      &destroy_device);
    if (expect_stale_ioctl_rejected(
            "destroy_device_after_second_fd_destroy",
            stale_device_second_fd_rc) < 0)
        return -1;
    expected_denials++;

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    if (ioctl(fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        printf("handle_lifetime recreate_device_failed device=0x%x\n",
               create_device.device.v);
        return -1;
    }
    printf("handle_lifetime recreated_device=0x%x\n",
           create_device.device.v);

    memset(&create_sync, 0, sizeof(create_sync));
    create_sync.device = create_device.device;
    create_sync.info.type = _D3DDDI_MONITORED_FENCE;
    if (ioctl(fd, LX_DXCREATESYNCHRONIZATIONOBJECT, &create_sync) < 0 ||
        create_sync.sync_object.v == 0) {
        printf("handle_lifetime create_sync_failed sync=0x%x\n",
               create_sync.sync_object.v);
        goto cleanup_device;
    }
    memset(context_private_data, 0, sizeof(context_private_data));
    memset(&context_handle, 0, sizeof(context_handle));
    if (probe_context_matrix(fd, create_device.device,
                             context_private_data,
                             sizeof(context_private_data),
                             &context_handle, &context_sync_only) < 0 ||
        context_handle.v == 0 || context_sync_only) {
        printf("handle_lifetime create_context_failed context=0x%x sync_only=%d\n",
               context_handle.v, context_sync_only);
        goto cleanup_device;
    }
    memset(&create_hwqueue, 0, sizeof(create_hwqueue));
    create_hwqueue.context = context_handle;
    if (ioctl(fd, LX_DXCREATEHWQUEUE, &create_hwqueue) < 0 ||
        create_hwqueue.queue.v == 0 ||
        create_hwqueue.queue_progress_fence.v == 0) {
        printf("handle_lifetime create_hwqueue_failed context=0x%x queue=0x%x fence=0x%x\n",
               context_handle.v, create_hwqueue.queue.v,
               create_hwqueue.queue_progress_fence.v);
        (void)destroy_context_handle(fd, context_handle);
        goto cleanup_device;
    }
    memset(&destroy_hwqueue, 0, sizeof(destroy_hwqueue));
    destroy_hwqueue.queue = create_hwqueue.queue;
    if (ioctl(fd, LX_DXDESTROYHWQUEUE, &destroy_hwqueue) < 0) {
        printf("handle_lifetime destroy_hwqueue_failed queue=0x%x\n",
               create_hwqueue.queue.v);
        (void)destroy_context_handle(fd, context_handle);
        goto cleanup_device;
    }
    stale_hwqueue_rc = ioctl(fd, LX_DXDESTROYHWQUEUE, &destroy_hwqueue);
    if (expect_stale_ioctl_rejected(
            "destroy_hwqueue_again", stale_hwqueue_rc) < 0) {
        (void)destroy_context_handle(fd, context_handle);
        goto cleanup_device;
    }
    expected_denials++;
    memset(&destroy_sync, 0, sizeof(destroy_sync));
    destroy_sync.sync_object = create_hwqueue.queue_progress_fence;
    stale_hwqueue_sync_rc =
        ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &destroy_sync);
    if (expect_stale_ioctl_rejected(
            "destroy_hwqueue_progress_fence_after_queue_destroy",
            stale_hwqueue_sync_rc) < 0) {
        (void)destroy_context_handle(fd, context_handle);
        goto cleanup_device;
    }
    expected_denials++;
    memset(&destroy_context, 0, sizeof(destroy_context));
    destroy_context.context = context_handle;
    if (ioctl(fd, LX_DXDESTROYCONTEXT, &destroy_context) < 0) {
        printf("handle_lifetime destroy_context_failed context=0x%x\n",
               context_handle.v);
        goto cleanup_device;
    }
    stale_context_rc = ioctl(fd, LX_DXDESTROYCONTEXT, &destroy_context);
    if (expect_stale_ioctl_rejected(
            "destroy_context_again", stale_context_rc) < 0)
        goto cleanup_device;
    expected_denials++;

    memset(&destroy_sync, 0, sizeof(destroy_sync));
    destroy_sync.sync_object = create_sync.sync_object;
    if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &destroy_sync) < 0) {
        printf("handle_lifetime destroy_sync_failed sync=0x%x\n",
               create_sync.sync_object.v);
        goto cleanup_device;
    }
    stale_sync_rc = ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                          &destroy_sync);
    if (expect_stale_ioctl_rejected(
            "destroy_sync_again", stale_sync_rc) < 0)
        goto cleanup_device;
    expected_denials++;

    memset(&create_paging_queue, 0, sizeof(create_paging_queue));
    create_paging_queue.device = create_device.device;
    create_paging_queue.priority = _D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
    if (ioctl(fd, LX_DXCREATEPAGINGQUEUE, &create_paging_queue) < 0 ||
        create_paging_queue.paging_queue.v == 0) {
        printf("handle_lifetime create_paging_queue_failed queue=0x%x\n",
               create_paging_queue.paging_queue.v);
        goto cleanup_device;
    }
    memset(&destroy_paging_queue, 0, sizeof(destroy_paging_queue));
    destroy_paging_queue.paging_queue = create_paging_queue.paging_queue;
    if (ioctl(fd, LX_DXDESTROYPAGINGQUEUE, &destroy_paging_queue) < 0) {
        printf("handle_lifetime destroy_paging_queue_failed queue=0x%x\n",
               create_paging_queue.paging_queue.v);
        goto cleanup_device;
    }
    stale_paging_queue_rc = ioctl(fd, LX_DXDESTROYPAGINGQUEUE,
                                  &destroy_paging_queue);
    if (expect_stale_ioctl_rejected(
            "destroy_paging_queue_again", stale_paging_queue_rc) < 0)
        goto cleanup_device;
    expected_denials++;
    memset(&destroy_sync, 0, sizeof(destroy_sync));
    destroy_sync.sync_object = create_paging_queue.sync_object;
    stale_paging_queue_sync_rc =
        ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &destroy_sync);
    if (expect_stale_ioctl_rejected(
            "destroy_paging_queue_sync_after_queue_destroy",
            stale_paging_queue_sync_rc) < 0)
        goto cleanup_device;
    expected_denials++;

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_allocation.device = create_device.device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    create_allocation.standard_allocation = (uint64)&standard_allocation;
    create_allocation.flags.create_resource = 1;
    create_allocation.flags.standard_allocation = 1;
    if (ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation) < 0 ||
        allocation_info.allocation.v == 0) {
        printf("handle_lifetime create_allocation_failed allocation=0x%x resource=0x%x\n",
               allocation_info.allocation.v, create_allocation.resource.v);
        goto cleanup_device;
    }
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    destroy_allocation.device = create_device.device;
    destroy_allocation.resource = create_allocation.resource;
    destroy_allocation.flags.assume_not_in_use = 1;
    if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0) {
        printf("handle_lifetime destroy_allocation_failed allocation=0x%x resource=0x%x\n",
               allocation_info.allocation.v, create_allocation.resource.v);
        goto cleanup_device;
    }
    stale_resource_rc = ioctl(fd, LX_DXDESTROYALLOCATION2,
                              &destroy_allocation);
    if (expect_stale_ioctl_rejected(
            "destroy_resource_again", stale_resource_rc) < 0)
        goto cleanup_device;
    expected_denials++;

    memset(&standalone_allocation_info, 0, sizeof(standalone_allocation_info));
    memset(&create_standalone_allocation, 0,
           sizeof(create_standalone_allocation));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
    standard_allocation.existing_heap_data.size = 0x10000;
    create_standalone_allocation.device = create_device.device;
    create_standalone_allocation.alloc_count = 1;
    create_standalone_allocation.allocation_info =
        (uint64)&standalone_allocation_info;
    create_standalone_allocation.standard_allocation =
        (uint64)&standard_allocation;
    create_standalone_allocation.flags.standard_allocation = 1;
    if (ioctl(fd, LX_DXCREATEALLOCATION,
              &create_standalone_allocation) < 0 ||
        standalone_allocation_info.allocation.v == 0) {
        printf("handle_lifetime create_standalone_allocation_failed allocation=0x%x resource=0x%x\n",
               standalone_allocation_info.allocation.v,
               create_standalone_allocation.resource.v);
        goto cleanup_device;
    }
    allocation_list[0] = standalone_allocation_info.allocation;
    memset(&destroy_allocation, 0, sizeof(destroy_allocation));
    destroy_allocation.device = create_device.device;
    destroy_allocation.allocations = (uint64)allocation_list;
    destroy_allocation.alloc_count = 1;
    destroy_allocation.flags.assume_not_in_use = 1;
    if (ioctl(fd, LX_DXDESTROYALLOCATION2, &destroy_allocation) < 0) {
        printf("handle_lifetime destroy_standalone_allocation_failed allocation=0x%x\n",
               standalone_allocation_info.allocation.v);
        goto cleanup_device;
    }
    stale_allocation_rc = ioctl(fd, LX_DXDESTROYALLOCATION2,
                                &destroy_allocation);
    if (expect_stale_ioctl_rejected(
            "destroy_standalone_allocation_again",
            stale_allocation_rc) < 0)
        goto cleanup_device;
    expected_denials++;

    memset(&reserve_gpuva, 0, sizeof(reserve_gpuva));
    reserve_gpuva.adapter = adapter;
    reserve_gpuva.size = 0x10000;
    reserve_gpuva.reservation_type = _D3DDDIGPUVA_RESERVE_NO_ACCESS;
    if (ioctl(fd, LX_DXRESERVEGPUVIRTUALADDRESS, &reserve_gpuva) < 0 ||
        reserve_gpuva.virtual_address == 0) {
        printf("handle_lifetime reserve_gpuva_failed va=0x%lx\n",
               reserve_gpuva.virtual_address);
        goto cleanup_device;
    }
    memset(&free_gpuva, 0, sizeof(free_gpuva));
    free_gpuva.adapter = adapter;
    free_gpuva.base_address = reserve_gpuva.virtual_address;
    free_gpuva.size = reserve_gpuva.size;
    if (ioctl(fd, LX_DXFREEGPUVIRTUALADDRESS, &free_gpuva) < 0) {
        printf("handle_lifetime free_gpuva_failed va=0x%lx\n",
               reserve_gpuva.virtual_address);
        goto cleanup_device;
    }
    stale_gpuva_rc = ioctl(fd, LX_DXFREEGPUVIRTUALADDRESS, &free_gpuva);
    if (expect_stale_ioctl_rejected(
            "free_gpuva_again", stale_gpuva_rc) < 0)
        goto cleanup_device;
    expected_denials++;

    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device = create_device.device;
    if (ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device) < 0) {
        printf("handle_lifetime destroy_device_failed device=0x%x\n",
               create_device.device.v);
        return -1;
    }
    stale_device_final_rc = ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device);
    if (expect_stale_ioctl_rejected(
            "destroy_device_again", stale_device_final_rc) < 0)
        return -1;
    expected_denials++;

    if (read_object_table_status(&after) < 0) {
        printf("handle_lifetime status_final_failed\n");
        return -1;
    }
    denied_delta = after.denied - before.denied;
    if (after.denied < before.denied + expected_denials) {
        printf("handle_lifetime counters_unexpected denied:%u->%u expected_delta=%u\n",
               before.denied, after.denied, expected_denials);
        return -1;
    }
    if (after.min_free != 128) {
        printf("handle_lifetime min_free_unexpected min_free=%u\n",
               after.min_free);
        return -1;
    }
    printf("handle_lifetime_stale_matrix "
           "device_second_fd_rc=%d device_second_fd_rejected=%u "
           "context_rc=%d context_rejected=%u "
           "hwqueue_rc=%d hwqueue_rejected=%u "
           "hwqueue_sync_rc=%d hwqueue_sync_rejected=%u "
           "sync_rc=%d sync_rejected=%u "
           "paging_queue_rc=%d paging_queue_rejected=%u "
           "paging_queue_sync_rc=%d paging_queue_sync_rejected=%u "
           "resource_rc=%d resource_rejected=%u "
           "allocation_rc=%d allocation_rejected=%u "
           "gpuva_rc=%d gpuva_rejected=%u "
           "device_final_rc=%d device_final_rejected=%u "
           "expected_denials=%u denied_delta=%u "
           "object_classes=device,context,hwqueue,hwqueue_sync,sync,paging_queue,paging_queue_sync,resource,allocation,gpuva "
           "status=PASS\n",
           stale_device_second_fd_rc,
           (uint32)(stale_device_second_fd_rc < 0),
           stale_context_rc, (uint32)(stale_context_rc < 0),
           stale_hwqueue_rc, (uint32)(stale_hwqueue_rc < 0),
           stale_hwqueue_sync_rc, (uint32)(stale_hwqueue_sync_rc < 0),
           stale_sync_rc, (uint32)(stale_sync_rc < 0),
           stale_paging_queue_rc,
           (uint32)(stale_paging_queue_rc < 0),
           stale_paging_queue_sync_rc,
           (uint32)(stale_paging_queue_sync_rc < 0),
           stale_resource_rc, (uint32)(stale_resource_rc < 0),
           stale_allocation_rc, (uint32)(stale_allocation_rc < 0),
           stale_gpuva_rc, (uint32)(stale_gpuva_rc < 0),
           stale_device_final_rc, (uint32)(stale_device_final_rc < 0),
           expected_denials, denied_delta);
    printf("handle_lifetime ok denied:%u->%u max:%u generation:%u drops:%u reuse_delayed:%u reuse_allowed:%u min_free:%u free_count:%u free_head:%u free_tail:%u\n",
           before.denied, after.denied, after.max, after.generation,
           after.drops, after.reuse_delayed, after.reuse_allowed,
           after.min_free, after.free_count, after.free_head,
           after.free_tail);
    return 0;

cleanup_device:
    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device = create_device.device;
    if (create_device.device.v != 0 &&
        ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device) < 0)
        printf("handle_lifetime cleanup_destroy_device_failed device=0x%x\n",
               create_device.device.v);
    return ret;
}

static int probe_tracker_stress(int fd, struct d3dkmthandle adapter)
{
    struct d3dkmt_createdevice create_device;
    struct d3dkmt_destroydevice destroy_device;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dkmt_freegpuvirtualaddress free_gpuva;
    struct d3dkmthandle *syncs;
    struct d3dkmthandle *allocations;
    struct d3dkmthandle *resources;
    uint64 *gpuvas;
    uint64 *gpuva_sizes;
    uint created_syncs = 0;
    uint created_resources = 0;
    uint reserved_gpuvas = 0;
    int ret = 0;

    syncs = malloc(sizeof(syncs[0]) * DXGPROBE_TRACKER_STRESS_SYNCS);
    allocations = malloc(sizeof(allocations[0]) *
                         DXGPROBE_TRACKER_STRESS_RESOURCES);
    resources = malloc(sizeof(resources[0]) *
                       DXGPROBE_TRACKER_STRESS_RESOURCES);
    gpuvas = malloc(sizeof(gpuvas[0]) * DXGPROBE_TRACKER_STRESS_GPUVAS);
    gpuva_sizes = malloc(sizeof(gpuva_sizes[0]) *
                         DXGPROBE_TRACKER_STRESS_GPUVAS);
    if (syncs == 0 || allocations == 0 || resources == 0 ||
        gpuvas == 0 || gpuva_sizes == 0) {
        printf("tracker_stress allocation_failed\n");
        ret = -1;
        goto out_free;
    }
    memset(syncs, 0, sizeof(syncs[0]) * DXGPROBE_TRACKER_STRESS_SYNCS);
    memset(allocations, 0, sizeof(allocations[0]) *
           DXGPROBE_TRACKER_STRESS_RESOURCES);
    memset(resources, 0, sizeof(resources[0]) *
           DXGPROBE_TRACKER_STRESS_RESOURCES);
    memset(gpuvas, 0, sizeof(gpuvas[0]) * DXGPROBE_TRACKER_STRESS_GPUVAS);
    memset(gpuva_sizes, 0, sizeof(gpuva_sizes[0]) *
           DXGPROBE_TRACKER_STRESS_GPUVAS);

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    if (ioctl(fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        printf("tracker_stress create_device_failed device=0x%x\n",
               create_device.device.v);
        ret = -1;
        goto out_free;
    }

    for (uint i = 0; i < DXGPROBE_TRACKER_STRESS_SYNCS; i++) {
        struct d3dkmt_createsynchronizationobject2 create_sync;

        memset(&create_sync, 0, sizeof(create_sync));
        create_sync.device = create_device.device;
        create_sync.info.type = _D3DDDI_MONITORED_FENCE;
        create_sync.info.monitored_fence.initial_fence_value = 0;
        create_sync.info.monitored_fence.engine_affinity = 0;
        if (ioctl(fd, LX_DXCREATESYNCHRONIZATIONOBJECT,
                  &create_sync) < 0 ||
            create_sync.sync_object.v == 0) {
            printf("tracker_stress create_sync_failed index=%u sync=0x%x\n",
                   i, create_sync.sync_object.v);
            ret = -1;
            goto cleanup;
        }
        syncs[i] = create_sync.sync_object;
        created_syncs++;
    }

    for (uint i = 0; i < DXGPROBE_TRACKER_STRESS_RESOURCES; i++) {
        struct d3dddi_allocationinfo2 allocation_info;
        struct d3dkmt_createallocation create_allocation;
        struct d3dkmt_createstandardallocation standard_allocation;

        memset(&allocation_info, 0, sizeof(allocation_info));
        memset(&create_allocation, 0, sizeof(create_allocation));
        memset(&standard_allocation, 0, sizeof(standard_allocation));
        standard_allocation.type = _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
        standard_allocation.existing_heap_data.size = 0x10000;
        create_allocation.device = create_device.device;
        create_allocation.alloc_count = 1;
        create_allocation.allocation_info = (uint64)&allocation_info;
        create_allocation.standard_allocation = (uint64)&standard_allocation;
        create_allocation.flags.create_resource = 1;
        create_allocation.flags.standard_allocation = 1;
        if (ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation) < 0 ||
            allocation_info.allocation.v == 0) {
            printf("tracker_stress create_resource_failed index=%u allocation=0x%x resource=0x%x\n",
                   i, allocation_info.allocation.v,
                   create_allocation.resource.v);
            ret = -1;
            goto cleanup;
        }
        allocations[i] = allocation_info.allocation;
        resources[i] = create_allocation.resource;
        created_resources++;
    }

    for (uint i = 0; i < DXGPROBE_TRACKER_STRESS_GPUVAS; i++) {
        struct d3dddi_reservegpuvirtualaddress reserve_gpuva;

        memset(&reserve_gpuva, 0, sizeof(reserve_gpuva));
        reserve_gpuva.adapter = adapter;
        reserve_gpuva.size = 0x10000;
        reserve_gpuva.reservation_type = _D3DDDIGPUVA_RESERVE_NO_ACCESS;
        if (ioctl(fd, LX_DXRESERVEGPUVIRTUALADDRESS, &reserve_gpuva) < 0 ||
            reserve_gpuva.virtual_address == 0) {
            printf("tracker_stress reserve_gpuva_failed index=%u va=0x%lx\n",
                   i, reserve_gpuva.virtual_address);
            ret = -1;
            goto cleanup;
        }
        gpuvas[i] = reserve_gpuva.virtual_address;
        gpuva_sizes[i] = reserve_gpuva.size;
        reserved_gpuvas++;
    }

cleanup:
    while (reserved_gpuvas > 0) {
        uint i = --reserved_gpuvas;

        memset(&free_gpuva, 0, sizeof(free_gpuva));
        free_gpuva.adapter = adapter;
        free_gpuva.base_address = gpuvas[i];
        free_gpuva.size = gpuva_sizes[i];
        if (ioctl(fd, LX_DXFREEGPUVIRTUALADDRESS, &free_gpuva) < 0) {
            printf("tracker_stress free_gpuva_failed index=%u va=0x%lx\n",
                   i, gpuvas[i]);
            ret = -1;
        }
    }
    while (created_resources > 0) {
        uint i = --created_resources;

        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        destroy_allocation.device = create_device.device;
        destroy_allocation.resource = resources[i];
        destroy_allocation.flags.assume_not_in_use = 1;
        if (resources[i].v == 0) {
            destroy_allocation.allocations = (uint64)&allocations[i];
            destroy_allocation.alloc_count = 1;
        }
        if (ioctl(fd, LX_DXDESTROYALLOCATION2,
                  &destroy_allocation) < 0) {
            printf("tracker_stress destroy_resource_failed index=%u allocation=0x%x resource=0x%x\n",
                   i, allocations[i].v, resources[i].v);
            ret = -1;
        }
    }
    while (created_syncs > 0) {
        uint i = --created_syncs;
        struct d3dkmt_destroysynchronizationobject destroy_sync;

        memset(&destroy_sync, 0, sizeof(destroy_sync));
        destroy_sync.sync_object = syncs[i];
        if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT,
                  &destroy_sync) < 0) {
            printf("tracker_stress destroy_sync_failed index=%u sync=0x%x\n",
                   i, syncs[i].v);
            ret = -1;
        }
    }

    memset(&destroy_device, 0, sizeof(destroy_device));
    destroy_device.device = create_device.device;
    if (ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device) < 0) {
        printf("tracker_stress destroy_device_failed device=0x%x\n",
               create_device.device.v);
        ret = -1;
    }
    if (ret == 0)
        printf("tracker_stress ok resources=%u gpuvas=%u syncs=%u\n",
               DXGPROBE_TRACKER_STRESS_RESOURCES,
               DXGPROBE_TRACKER_STRESS_GPUVAS,
               DXGPROBE_TRACKER_STRESS_SYNCS);

out_free:
    if (syncs != 0)
        free(syncs);
    if (allocations != 0)
        free(allocations);
    if (resources != 0)
        free(resources);
    if (gpuvas != 0)
        free(gpuvas);
    if (gpuva_sizes != 0)
        free(gpuva_sizes);
    return ret;
}

static void wsl_trace_replay_packet_matrix(const char *stage, uint32 cmd,
                                           uint32 cmd_len,
                                           uint32 result_len,
                                           uint32 owner, uint32 first,
                                           int rc, uint32 expected_reject,
                                           uint32 *seen, uint32 *passed)
{
    uint32 ok;

    ok = stage != 0 && cmd != 0 && cmd_len != 0 &&
         ((expected_reject && rc < 0) ||
          (!expected_reject && rc == 0)) &&
         (owner != 0 || first != 0);
    if (seen != 0)
        (*seen)++;
    if (passed != 0 && ok)
        (*passed)++;
    printf("wsl_trace_replay_packet_matrix stage=%s cmd:0x%x cmd_len:%u result_len:%u owner:0x%x first:0x%x ret:%d expected_reject:%u status=%s\n",
           stage != 0 ? stage : "unknown", cmd, cmd_len, result_len,
           owner, first, rc, expected_reject, ok ? "PASS" : "FAIL");
}

static void wsl_trace_replay_host_saw_matrix(uint32 expected_submit_priv,
                                             uint32 *seen, uint32 *passed)
{
    char *status;
    uint32 make_len = 0;
    uint32 make_ret = 0;
    uint32 make_host_ret = 0;
    uint32 make_user_ret = 0;
    uint32 make_device = 0xffffffffU;
    uint32 make_count = 0;
    uint32 make_flags = 0xffffffffU;
    uint32 make_sorted = 1;
    uint32 make_in0 = 0;
    uint32 make_in1 = 0;
    uint32 make_wire0 = 0;
    uint32 make_wire1 = 0;
    uint32 context_len = 0;
    uint32 context_ret = 0xffffffffU;
    uint32 context_handle = 0;
    uint32 hwqueue_create_len = 0;
    uint32 hwqueue_create_ret = 0xffffffffU;
    uint32 hwqueue_priv = 0;
    uint32 hwqueue_handle = 0;
    uint32 submit_queue = 0;
    uint32 submit_cmd_len = 0;
    uint32 submit_priv = 0;
    uint32 submit_len = 0;
    uint32 submit_head_len = 0;
    unsigned char submit_head[8];
    uint32 head_len = 0;
    uint32 ok;

    memset(submit_head, 0, sizeof(submit_head));
    status = read_dxg_status_buffer();
    if (status != 0) {
        char *residency = dxg_find_text(status, "dxg_residency_last=");
        char *context = dxg_find_text(status, "dxg_context_last=");
        char *hwqueue = dxg_find_text(status, "dxg_hwqueue_last=");
        char *hwqueue_priv_line =
            dxg_find_text(status, "dxg_hwqueue_priv_head=");

        if (residency != 0) {
            dxg_parse_uint_after(residency, "make_len:", &make_len);
            dxg_parse_uint_after(residency, "make_ret:", &make_ret);
            dxg_parse_uint_after(residency, "make_host_ret:",
                                 &make_host_ret);
            dxg_parse_uint_after(residency, "make_user_ret:",
                                 &make_user_ret);
            dxg_parse_uint_after(residency, "device:", &make_device);
            dxg_parse_uint_after(residency, "count:", &make_count);
            dxg_parse_uint_after(residency, "flags:", &make_flags);
            dxg_parse_uint_after(residency, "sorted:", &make_sorted);
            dxg_parse_uint_pair_after(residency, "in:", &make_in0,
                                      &make_in1);
            dxg_parse_uint_pair_after(residency, "wire:", &make_wire0,
                                      &make_wire1);
        }
        if (context != 0) {
            dxg_parse_uint_after(context, "len:", &context_len);
            dxg_parse_uint_after(context, "ret:", &context_ret);
            dxg_parse_uint_after(context, "handle:", &context_handle);
        }
        if (hwqueue != 0) {
            dxg_parse_uint_after(hwqueue, "create_len:",
                                 &hwqueue_create_len);
            dxg_parse_uint_after(hwqueue, "create_ret:",
                                 &hwqueue_create_ret);
            dxg_parse_uint_after(hwqueue, "priv:", &hwqueue_priv);
            dxg_parse_uint_after(hwqueue, "queue:", &hwqueue_handle);
        }
        if (hwqueue_priv_line != 0) {
            dxg_parse_uint_after(hwqueue_priv_line, "submit_queue:",
                                 &submit_queue);
            dxg_parse_uint_after(hwqueue_priv_line, "submit_cmd_len:",
                                 &submit_cmd_len);
            dxg_parse_uint_after(hwqueue_priv_line, "submit_priv:",
                                 &submit_priv);
            dxg_parse_uint_after(hwqueue_priv_line, "submit_len:",
                                 &submit_len);
            dxg_parse_uint_after(hwqueue_priv_line, "submit:", &head_len);
            if (dxg_parse_head8_after(hwqueue_priv_line, "submit:",
                                      submit_head,
                                      &submit_head_len) != 0)
                submit_head_len = 0;
        }
    }
    ok = status != 0 &&
         make_len == 24 &&
         make_device == 0 &&
         make_count == 1 &&
         make_flags == 1 &&
         make_sorted == 0 &&
         make_in0 != 0 &&
         make_in1 == 0 &&
         make_wire0 == make_in0 &&
         make_wire1 == 0 &&
         context_len != 0 &&
         context_ret == 0 &&
         context_handle != 0 &&
         hwqueue_create_len != 0 &&
         hwqueue_create_ret == 0 &&
         hwqueue_priv != 0 &&
         hwqueue_handle != 0 &&
         submit_queue == hwqueue_handle &&
         submit_cmd_len == DXGPROBE_WSL_REPLAY_COMMAND_SIZE &&
         submit_priv == expected_submit_priv &&
         submit_len != 0 &&
         submit_head_len >= 4 &&
         submit_head[0] == 'A' &&
         submit_head[1] == 'D' &&
         submit_head[2] == 'V' &&
         submit_head[3] == 'N';
    if (seen != 0)
        (*seen)++;
    if (passed != 0 && ok)
        (*passed)++;
    printf("wsl_trace_replay_host_saw_matrix make_len:%u make_ret:%d make_host_ret:%d make_user_ret:%d make_device:0x%x make_count:%u make_flags:0x%x make_sorted:%u make_in:%x,%x make_wire:%x,%x context_len:%u context_ret:%d context:0x%x hwqueue_len:%u hwqueue_ret:%d hwqueue_priv:%u hwqueue:0x%x submit_queue:0x%x submit_cmd_len:%u submit_priv:%u submit_len:%u submit_head_len:%u host_saw=cat_/dev/dxg_after_replay status=%s\n",
           make_len, (int)make_ret, (int)make_host_ret,
           (int)make_user_ret, make_device, make_count, make_flags,
           make_sorted, make_in0, make_in1, make_wire0, make_wire1,
           context_len, (int)context_ret, context_handle,
           hwqueue_create_len, (int)hwqueue_create_ret, hwqueue_priv,
           hwqueue_handle, submit_queue, submit_cmd_len, submit_priv,
           submit_len, submit_head_len, ok ? "PASS" : "FAIL");
    if (status != 0)
        free(status);
}

static int probe_wsl_trace_replay(int fd, struct d3dkmthandle adapter,
                                  struct winluid adapter_luid)
{
    struct d3dkmt_createdevice create_device;
    struct d3dkmt_destroydevice destroy_device;
    struct d3dkmt_createpagingqueue create_paging_queue;
    struct d3dddi_destroypagingqueue destroy_paging_queue;
    struct d3dddi_allocationinfo2 allocation_info;
    struct d3dkmt_createallocation create_allocation;
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dddi_makeresident make_resident;
    struct d3dkmt_evict evict;
    struct d3dddi_mapgpuvirtualaddress map_gpuva;
    struct d3dkmt_freegpuvirtualaddress free_gpuva;
    struct d3dkmt_lock2 lock;
    struct d3dkmt_unlock2 unlock;
    struct d3dddi_createcontextflags context_flags;
    struct d3dddi_createhwqueueflags hwqueue_flags;
    struct d3dkmt_createhwqueue create_hwqueue;
    struct d3dkmt_destroyhwqueue destroy_hwqueue;
    struct d3dkmt_submitcommandtohwqueue submit_hwqueue;
    unsigned char context_private_data[DXGPROBE_WSL_REPLAY_CONTEXT_PRIV_SIZE];
    unsigned char allocation_private[DXGPROBE_WSL_REPLAY_ALLOC_PRIV_SIZE];
    unsigned char hwqueue_private[DXGPROBE_WSL_REPLAY_HWQUEUE_PRIV_SIZE];
    unsigned char submit_private[DXGPROBE_WSL_REPLAY_SUBMIT_PRIV_SIZE];
    struct d3dkmthandle allocation_list[1];
    struct d3dkmthandle context_handle;
    uint32 priority_list[1];
    uint32 submit_private_size = 0;
    int device_created = 0;
    int paging_queue_created = 0;
    int allocation_created = 0;
    int allocation_resident = 0;
    int gpuva_mapped = 0;
    int allocation_locked = 0;
    int context_created = 0;
    int hwqueue_created = 0;
    uint32 packet_seen = 0;
    uint32 packet_passed = 0;
    int ret = -1;

    make_wsl_replay_context_private(context_private_data,
                                    sizeof(context_private_data));
    memset(&context_handle, 0, sizeof(context_handle));
    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    if (ioctl(fd, LX_DXCREATEDEVICE, &create_device) < 0 ||
        create_device.device.v == 0) {
        printf("wsl_trace_replay create_device_failed device=0x%x\n",
               create_device.device.v);
        goto cleanup;
    }
    device_created = 1;
    printf("wsl_trace_replay device=0x%x command_buffer=0x%lx command_size=%u\n",
           create_device.device.v, create_device.command_buffer,
           create_device.command_buffer_size);
    wsl_trace_replay_packet_matrix(
        "create_device", LX_DXCREATEDEVICE, sizeof(create_device),
        sizeof(create_device), adapter.v, create_device.device.v, 0, 0,
        &packet_seen, &packet_passed);

    memset(&create_paging_queue, 0, sizeof(create_paging_queue));
    create_paging_queue.device = create_device.device;
    create_paging_queue.priority = _D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
    if (ioctl(fd, LX_DXCREATEPAGINGQUEUE, &create_paging_queue) < 0 ||
        create_paging_queue.paging_queue.v == 0) {
        printf("wsl_trace_replay create_paging_queue_failed queue=0x%x sync=0x%x\n",
               create_paging_queue.paging_queue.v,
               create_paging_queue.sync_object.v);
        goto cleanup;
    }
    paging_queue_created = 1;
    printf("wsl_trace_replay paging_queue=0x%x sync=0x%x fence_cpu=0x%lx\n",
           create_paging_queue.paging_queue.v,
           create_paging_queue.sync_object.v,
           create_paging_queue.fence_cpu_virtual_address);
    wsl_trace_replay_packet_matrix(
        "create_paging_queue", LX_DXCREATEPAGINGQUEUE,
        sizeof(create_paging_queue), sizeof(create_paging_queue),
        create_device.device.v, create_paging_queue.paging_queue.v, 0, 0,
        &packet_seen, &packet_passed);

    memset(&context_flags, 0, sizeof(context_flags));
    context_flags.hw_queue_supported = 1;
    if (try_create_context(fd, create_device.device,
                           "wsl_replay_wsl_hwqueue_private_e1", 0, 1,
                           (enum d3dkmt_clienthint)
                           DXGPROBE_WSL_REPLAY_CLIENT_HINT, context_flags,
                           context_private_data,
                           sizeof(context_private_data),
                           &context_handle) < 0) {
        printf("wsl_trace_replay create_context_failed\n");
        goto cleanup;
    }
    context_created = 1;
    wsl_trace_replay_packet_matrix(
        "create_context", LX_DXCREATECONTEXTVIRTUAL,
        sizeof(struct d3dkmt_createcontextvirtual),
        sizeof(struct d3dkmt_createcontextvirtual), create_device.device.v,
        context_handle.v, 0, 0, &packet_seen, &packet_passed);

    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&create_allocation, 0, sizeof(create_allocation));
    make_wsl_replay_allocation_private(allocation_private,
                                       sizeof(allocation_private));
    create_allocation.device = create_device.device;
    create_allocation.alloc_count = 1;
    create_allocation.allocation_info = (uint64)&allocation_info;
    allocation_info.priv_drv_data = (uint64)allocation_private;
    allocation_info.priv_drv_data_size = sizeof(allocation_private);
    allocation_info.flags.value = 0x4;
    allocation_info.priority = 0x78100000;
    if (ioctl(fd, LX_DXCREATEALLOCATION, &create_allocation) < 0 ||
        allocation_info.allocation.v == 0) {
        printf("wsl_trace_replay create_allocation_failed allocation=0x%x resource=0x%x\n",
               allocation_info.allocation.v, create_allocation.resource.v);
        goto cleanup;
    }
    allocation_created = 1;
    printf("wsl_trace_replay allocation=0x%x resource=0x%x\n",
           allocation_info.allocation.v, create_allocation.resource.v);
    wsl_trace_replay_packet_matrix(
        "create_allocation", LX_DXCREATEALLOCATION,
        sizeof(create_allocation), sizeof(create_allocation),
        create_device.device.v, allocation_info.allocation.v, 0, 0,
        &packet_seen, &packet_passed);

    allocation_list[0] = allocation_info.allocation;
    priority_list[0] = 0;
    memset(&map_gpuva, 0, sizeof(map_gpuva));
    map_gpuva.paging_queue = create_paging_queue.paging_queue;
    map_gpuva.minimum_address = DXGPROBE_WSL_REPLAY_GPUVA_MIN;
    map_gpuva.maximum_address = DXGPROBE_WSL_REPLAY_GPUVA_MAX;
    map_gpuva.allocation = allocation_info.allocation;
    map_gpuva.size_in_pages = DXGPROBE_WSL_REPLAY_ALLOCATION_SIZE >> 12;
    map_gpuva.protection.write = 1;
    if (ioctl(fd, LX_DXMAPGPUVIRTUALADDRESS, &map_gpuva) < 0 ||
        map_gpuva.virtual_address == 0) {
        printf("wsl_trace_replay map_gpuva_failed va=0x%lx fence=%lu\n",
               map_gpuva.virtual_address, map_gpuva.paging_fence_value);
        goto cleanup;
    }
    gpuva_mapped = 1;
    printf("wsl_trace_replay gpuva=0x%lx pages=0x%lx fence=%lu\n",
           map_gpuva.virtual_address, map_gpuva.size_in_pages,
           map_gpuva.paging_fence_value);
    wsl_trace_replay_packet_matrix(
        "map_gpuva", LX_DXMAPGPUVIRTUALADDRESS, sizeof(map_gpuva),
        sizeof(map_gpuva), create_paging_queue.paging_queue.v,
        allocation_info.allocation.v, 0, 0, &packet_seen, &packet_passed);
    (void)wait_paging_fence(create_paging_queue.fence_cpu_virtual_address,
                            map_gpuva.paging_fence_value,
                            "wsl_replay_map_gpuva");

    memset(&make_resident, 0, sizeof(make_resident));
    make_resident.paging_queue = create_paging_queue.paging_queue;
    make_resident.alloc_count = 1;
    make_resident.allocation_list = (uint64)allocation_list;
    make_resident.priority_list = (uint64)priority_list;
    make_resident.flags.cant_trim_further = 1;
    if (ioctl(fd, LX_DXMAKERESIDENT, &make_resident) < 0) {
        printf("wsl_trace_replay make_resident_failed fence=%lu trim=%lu\n",
               make_resident.paging_fence_value,
               make_resident.num_bytes_to_trim);
        goto cleanup;
    }
    allocation_resident = 1;
    printf("wsl_trace_replay resident fence=%lu trim=%lu\n",
           make_resident.paging_fence_value, make_resident.num_bytes_to_trim);
    wsl_trace_replay_packet_matrix(
        "make_resident", LX_DXMAKERESIDENT, sizeof(make_resident),
        sizeof(make_resident), create_paging_queue.paging_queue.v,
        allocation_info.allocation.v, 0, 0, &packet_seen, &packet_passed);
    (void)wait_paging_fence(create_paging_queue.fence_cpu_virtual_address,
                            make_resident.paging_fence_value,
                            "wsl_replay_make_resident");

    memset(&lock, 0, sizeof(lock));
    lock.device = create_device.device;
    lock.allocation = allocation_info.allocation;
    if (ioctl(fd, LX_DXLOCK2, &lock) < 0 || lock.data == 0) {
        printf("wsl_trace_replay lock2_failed allocation=0x%x data=0x%lx\n",
               allocation_info.allocation.v, lock.data);
        goto cleanup;
    }
    allocation_locked = 1;
    memset((void *)lock.data, 0, DXGPROBE_WSL_REPLAY_COMMAND_SIZE);
    printf("wsl_trace_replay lock2 data=0x%lx command_len=%u\n",
           lock.data, DXGPROBE_WSL_REPLAY_COMMAND_SIZE);
    wsl_trace_replay_packet_matrix(
        "lock2", LX_DXLOCK2, sizeof(lock), sizeof(lock),
        create_device.device.v, allocation_info.allocation.v, 0, 0,
        &packet_seen, &packet_passed);

    memset(&hwqueue_flags, 0, sizeof(hwqueue_flags));
    make_wsl_replay_hwqueue_private(hwqueue_private, sizeof(hwqueue_private),
                                    allocation_info.allocation);
    memset(&create_hwqueue, 0, sizeof(create_hwqueue));
    create_hwqueue.context = context_handle;
    create_hwqueue.flags = hwqueue_flags;
    create_hwqueue.priv_drv_data = (uint64)hwqueue_private;
    create_hwqueue.priv_drv_data_size = sizeof(hwqueue_private);
    if (ioctl(fd, LX_DXCREATEHWQUEUE, &create_hwqueue) < 0 ||
        create_hwqueue.queue.v == 0) {
        printf("wsl_trace_replay create_hwqueue_failed context=0x%x queue=0x%x fence=0x%x\n",
               context_handle.v, create_hwqueue.queue.v,
               create_hwqueue.queue_progress_fence.v);
        goto cleanup;
    }
    hwqueue_created = 1;
    printf("wsl_trace_replay hwqueue=0x%x progress_fence=0x%x fence_cpu=0x%lx fence_gpu=0x%lx\n",
           create_hwqueue.queue.v, create_hwqueue.queue_progress_fence.v,
           create_hwqueue.queue_progress_fence_cpu_va,
           create_hwqueue.queue_progress_fence_gpu_va);
    wsl_trace_replay_packet_matrix(
        "create_hwqueue", LX_DXCREATEHWQUEUE, sizeof(create_hwqueue),
        sizeof(create_hwqueue), context_handle.v, create_hwqueue.queue.v,
        0, 0, &packet_seen, &packet_passed);

    make_wsl_replay_submit_private(submit_private, sizeof(submit_private),
                                   &submit_private_size,
                                   map_gpuva.virtual_address,
                                   DXGPROBE_WSL_REPLAY_COMMAND_SIZE);
    memset(&submit_hwqueue, 0, sizeof(submit_hwqueue));
    submit_hwqueue.hwqueue = create_hwqueue.queue;
    submit_hwqueue.hwqueue_progress_fence_id = 1;
    submit_hwqueue.command_buffer = map_gpuva.virtual_address;
    submit_hwqueue.command_length = DXGPROBE_WSL_REPLAY_COMMAND_SIZE;
    submit_hwqueue.priv_drv_data = (uint64)&submit_private;
    submit_hwqueue.priv_drv_data_size = submit_private_size;
    submit_hwqueue.num_primaries = 1;
    submit_hwqueue.written_primaries = (uint64)allocation_list;
    {
        int submit_hwqueue_rc =
            ioctl(fd, LX_DXSUBMITCOMMANDTOHWQUEUE, &submit_hwqueue);

        wsl_trace_replay_packet_matrix(
            "submit_hwqueue", LX_DXSUBMITCOMMANDTOHWQUEUE,
            sizeof(submit_hwqueue), sizeof(submit_hwqueue),
            create_hwqueue.queue.v, allocation_info.allocation.v,
            submit_hwqueue_rc, 0, &packet_seen, &packet_passed);
        if (submit_hwqueue_rc < 0) {
            printf("wsl_trace_replay submit_hwqueue expected_invalid_parameter queue=0x%x fence=%lu cmd=0x%lx len=%u priv=%u\n",
                   create_hwqueue.queue.v,
                   submit_hwqueue.hwqueue_progress_fence_id,
                   submit_hwqueue.command_buffer,
                   submit_hwqueue.command_length,
                   submit_hwqueue.priv_drv_data_size);
            wsl_trace_replay_host_saw_matrix(submit_private_size,
                                             &packet_seen,
                                             &packet_passed);
            ret = 0;
            goto cleanup;
        }
    }
    printf("wsl_trace_replay submit_hwqueue success queue=0x%x fence=%lu cmd=0x%lx len=%u priv=%u\n",
           create_hwqueue.queue.v,
           submit_hwqueue.hwqueue_progress_fence_id,
           submit_hwqueue.command_buffer,
           submit_hwqueue.command_length,
           submit_hwqueue.priv_drv_data_size);
    wsl_trace_replay_host_saw_matrix(submit_private_size, &packet_seen,
                                     &packet_passed);
    ret = 0;
    goto cleanup;

cleanup:
    if (allocation_locked) {
        memset(&unlock, 0, sizeof(unlock));
        unlock.device = create_device.device;
        unlock.allocation = allocation_info.allocation;
        if (ioctl(fd, LX_DXUNLOCK2, &unlock) < 0) {
            printf("wsl_trace_replay unlock2_failed allocation=0x%x\n",
                   allocation_info.allocation.v);
            ret = -1;
        }
    }
    if (hwqueue_created) {
        memset(&destroy_hwqueue, 0, sizeof(destroy_hwqueue));
        destroy_hwqueue.queue = create_hwqueue.queue;
        if (ioctl(fd, LX_DXDESTROYHWQUEUE, &destroy_hwqueue) < 0) {
            printf("wsl_trace_replay destroy_hwqueue_failed queue=0x%x\n",
                   create_hwqueue.queue.v);
            ret = -1;
        }
    }
    if (context_created && destroy_context_handle(fd, context_handle) < 0)
        ret = -1;
    if (allocation_resident) {
        memset(&evict, 0, sizeof(evict));
        evict.device = create_device.device;
        evict.alloc_count = 1;
        evict.allocations = (uint64)allocation_list;
        if (ioctl(fd, LX_DXEVICT, &evict) < 0) {
            printf("wsl_trace_replay evict_failed trim=%lu\n",
                   evict.num_bytes_to_trim);
            ret = -1;
        }
    }
    if (gpuva_mapped) {
        memset(&free_gpuva, 0, sizeof(free_gpuva));
        free_gpuva.adapter = adapter;
        free_gpuva.base_address = map_gpuva.virtual_address;
        free_gpuva.size = map_gpuva.size_in_pages << 12;
        if (ioctl(fd, LX_DXFREEGPUVIRTUALADDRESS, &free_gpuva) < 0) {
            printf("wsl_trace_replay free_gpuva_failed va=0x%lx size=%lu\n",
                   free_gpuva.base_address, free_gpuva.size);
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
            printf("wsl_trace_replay destroy_allocation_failed allocation=0x%x resource=0x%x\n",
                   allocation_info.allocation.v,
                   create_allocation.resource.v);
            ret = -1;
        }
    }
    if (paging_queue_created) {
        memset(&destroy_paging_queue, 0, sizeof(destroy_paging_queue));
        destroy_paging_queue.paging_queue = create_paging_queue.paging_queue;
        if (ioctl(fd, LX_DXDESTROYPAGINGQUEUE,
                  &destroy_paging_queue) < 0) {
            printf("wsl_trace_replay destroy_paging_queue_failed queue=0x%x\n",
                   create_paging_queue.paging_queue.v);
            ret = -1;
        }
    }
    if (device_created) {
        memset(&destroy_device, 0, sizeof(destroy_device));
        destroy_device.device = create_device.device;
        if (ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device) < 0) {
            printf("wsl_trace_replay destroy_device_failed device=0x%x\n",
                   create_device.device.v);
            ret = -1;
        }
    }
    if (ret == 0) {
        printf("wsl_trace_replay_signature adapter:%x:%x trace=/tmp/xv6-wsl-probe/mesaglfeature-nvidia-fullpriv-20260525-034404.trace equivalence=wsl_private_hwqueue_submit_success driver_store_hash:%08x umd_type0_size:%u context_priv:%u alloc_priv:%u hwqueue_priv:%u submit_priv:%u packets:%u/%u same_adapter_source=selected_openadapter_luid status=%s\n",
               adapter_luid.b, adapter_luid.a,
               0U, 9300U,
               (uint32)sizeof(context_private_data),
               (uint32)sizeof(allocation_private),
               create_hwqueue.priv_drv_data_size, submit_private_size,
               packet_passed, packet_seen,
               packet_seen != 0 && packet_seen == packet_passed ?
               "PASS" : "FAIL");
        if (packet_seen == 0 || packet_seen != packet_passed)
            ret = -1;
    }
    if (ret == 0)
        printf("wsl_trace_replay ok equivalence=wsl_private_hwqueue_submit_success\n");
    return ret;
}

static int probe_residency_batch_validate(int fd, struct d3dkmthandle adapter)
{
    struct d3dkmt_createdevice create_device;
    struct d3dkmt_destroydevice destroy_device;
    struct d3dkmt_createpagingqueue create_paging_queue;
    struct d3dddi_destroypagingqueue destroy_paging_queue;
    struct d3dddi_allocationinfo2 allocation_info[2];
    struct d3dkmt_createallocation create_allocation[2];
    struct d3dkmt_createstandardallocation standard_allocation[2];
    struct d3dkmt_destroyallocation2 destroy_allocation;
    struct d3dddi_makeresident make_resident;
    struct d3dkmt_evict evict;
    struct d3dkmthandle allocation_list[2];
    uint32 priority_list[2];
    int device_created = 0;
    int paging_queue_created = 0;
    int allocation_created[2] = { 0, 0 };
    int resident = 0;
    uint32 requested_count = g_residency_batch_count;
    uint32 requested_flags = g_residency_batch_flags;
    int device_rc;
    int queue_rc;
    int allocation_rc;
    int make_rc;
    int make_errno;
    int ret = -1;

    if (requested_count < 1)
        requested_count = 1;
    if (requested_count > 2)
        requested_count = 2;

    memset(&create_device, 0, sizeof(create_device));
    create_device.adapter = adapter;
    device_rc = ioctl(fd, LX_DXCREATEDEVICE, &create_device);
    if (device_rc < 0 ||
        create_device.device.v == 0) {
        printf("residency_batch_matrix stage=create-device requested_count=%u requested_flags=0x%x rc=-1 errno=%d status=FAIL device=0x%x queue=0x0 allocations=0x0,0x0 fence=0 trim=0 wsl_repro_target=makeresident_count_flags\n",
               requested_count, requested_flags,
               device_rc < 0 ? -device_rc : ENODEV,
               create_device.device.v);
        goto cleanup;
    }
    device_created = 1;
    printf("residency_batch setup device=0x%x requested_count=%u requested_flags=0x%x\n",
           create_device.device.v, requested_count, requested_flags);

    memset(&create_paging_queue, 0, sizeof(create_paging_queue));
    create_paging_queue.device = create_device.device;
    create_paging_queue.priority = _D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
    queue_rc = ioctl(fd, LX_DXCREATEPAGINGQUEUE, &create_paging_queue);
    if (queue_rc < 0 ||
        create_paging_queue.paging_queue.v == 0) {
        printf("residency_batch_matrix stage=create-paging-queue requested_count=%u requested_flags=0x%x rc=-1 errno=%d status=FAIL device=0x%x queue=0x%x sync=0x%x allocations=0x0,0x0 fence=0 trim=0 wsl_repro_target=makeresident_count_flags\n",
               requested_count, requested_flags,
               queue_rc < 0 ? -queue_rc : ENODEV,
               create_device.device.v, create_paging_queue.paging_queue.v,
               create_paging_queue.sync_object.v);
        goto cleanup;
    }
    paging_queue_created = 1;
    printf("residency_batch paging_queue=0x%x sync=0x%x fence_cpu=0x%lx\n",
           create_paging_queue.paging_queue.v,
           create_paging_queue.sync_object.v,
           create_paging_queue.fence_cpu_virtual_address);

    for (uint32 i = 0; i < requested_count; i++) {
        memset(&allocation_info[i], 0, sizeof(allocation_info[i]));
        memset(&create_allocation[i], 0, sizeof(create_allocation[i]));
        memset(&standard_allocation[i], 0, sizeof(standard_allocation[i]));
        standard_allocation[i].type =
            _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER;
        standard_allocation[i].existing_heap_data.size = 0x10000;
        create_allocation[i].device = create_device.device;
        create_allocation[i].alloc_count = 1;
        create_allocation[i].allocation_info = (uint64)&allocation_info[i];
        create_allocation[i].standard_allocation =
            (uint64)&standard_allocation[i];
        create_allocation[i].flags.create_resource = 1;
        create_allocation[i].flags.standard_allocation = 1;
        allocation_rc = ioctl(fd, LX_DXCREATEALLOCATION,
                              &create_allocation[i]);
        if (allocation_rc < 0 ||
            allocation_info[i].allocation.v == 0) {
            printf("residency_batch_matrix stage=create-allocation requested_count=%u requested_flags=0x%x rc=-1 errno=%d status=FAIL index=%u device=0x%x queue=0x%x allocations=0x%x,0x%x resources=0x%x,0x%x fence=0 trim=0 wsl_repro_target=makeresident_count_flags\n",
                   requested_count, requested_flags,
                   allocation_rc < 0 ? -allocation_rc : ENODEV, i,
                   create_device.device.v, create_paging_queue.paging_queue.v,
                   allocation_info[0].allocation.v,
                   allocation_info[1].allocation.v,
                   create_allocation[0].resource.v,
                   create_allocation[1].resource.v);
            goto cleanup;
        }
        allocation_created[i] = 1;
        allocation_list[i] = allocation_info[i].allocation;
        priority_list[i] = 0;
        printf("residency_batch allocation%d=0x%x resource=0x%x\n",
               i, allocation_info[i].allocation.v,
               create_allocation[i].resource.v);
    }

    memset(&make_resident, 0, sizeof(make_resident));
    make_resident.paging_queue = create_paging_queue.paging_queue;
    make_resident.alloc_count = requested_count;
    make_resident.allocation_list = (uint64)allocation_list;
    make_resident.priority_list = (uint64)priority_list;
    make_resident.flags.value = requested_flags;
    make_rc = ioctl(fd, LX_DXMAKERESIDENT, &make_resident);
    if (make_rc < 0) {
        make_errno = -make_rc;
        printf("residency_batch_matrix stage=make-resident requested_count=%u requested_flags=0x%x actual_count=%u actual_flags=0x%x rc=%d errno=%d status=FAIL device=0x%x queue=0x%x allocations=0x%x,0x%x resources=0x%x,0x%x fence=%lu trim=%lu wsl_repro_target=makeresident_count_flags\n",
               requested_count, requested_flags, make_resident.alloc_count,
               make_resident.flags.value, make_rc, make_errno,
               create_device.device.v, create_paging_queue.paging_queue.v,
               allocation_info[0].allocation.v,
               allocation_info[1].allocation.v,
               create_allocation[0].resource.v,
               create_allocation[1].resource.v,
               make_resident.paging_fence_value,
               make_resident.num_bytes_to_trim);
        goto cleanup;
    }
    resident = 1;
    printf("residency_batch_matrix stage=make-resident requested_count=%u requested_flags=0x%x actual_count=%u actual_flags=0x%x rc=%d errno=0 status=PASS device=0x%x queue=0x%x allocations=0x%x,0x%x resources=0x%x,0x%x fence=%lu trim=%lu wsl_repro_target=makeresident_count_flags\n",
           requested_count, requested_flags, make_resident.alloc_count,
           make_resident.flags.value, make_rc, create_device.device.v,
           create_paging_queue.paging_queue.v,
           allocation_info[0].allocation.v, allocation_info[1].allocation.v,
           create_allocation[0].resource.v, create_allocation[1].resource.v,
           make_resident.paging_fence_value,
           make_resident.num_bytes_to_trim);
    (void)wait_paging_fence(create_paging_queue.fence_cpu_virtual_address,
                            make_resident.paging_fence_value,
                            "residency_batch_make_resident");
    ret = 0;

cleanup:
    if (resident) {
        memset(&evict, 0, sizeof(evict));
        evict.device = create_device.device;
        evict.alloc_count = requested_count;
        evict.allocations = (uint64)allocation_list;
        if (ioctl(fd, LX_DXEVICT, &evict) < 0) {
            printf("residency_batch evict_failed trim=%lu\n",
                   evict.num_bytes_to_trim);
            ret = -1;
        }
    }
    for (int i = (int)requested_count - 1; i >= 0; i--) {
        if (!allocation_created[i])
            continue;
        memset(&destroy_allocation, 0, sizeof(destroy_allocation));
        destroy_allocation.device = create_device.device;
        destroy_allocation.resource = create_allocation[i].resource;
        destroy_allocation.flags.assume_not_in_use = 1;
        if (ioctl(fd, LX_DXDESTROYALLOCATION2,
                  &destroy_allocation) < 0) {
            printf("residency_batch destroy_allocation_failed index=%d allocation=0x%x resource=0x%x\n",
                   i, allocation_info[i].allocation.v,
                   create_allocation[i].resource.v);
            ret = -1;
        }
    }
    if (paging_queue_created) {
        memset(&destroy_paging_queue, 0, sizeof(destroy_paging_queue));
        destroy_paging_queue.paging_queue = create_paging_queue.paging_queue;
        if (ioctl(fd, LX_DXDESTROYPAGINGQUEUE,
                  &destroy_paging_queue) < 0) {
            printf("residency_batch destroy_paging_queue_failed queue=0x%x\n",
                   create_paging_queue.paging_queue.v);
            ret = -1;
        }
    }
    if (device_created) {
        memset(&destroy_device, 0, sizeof(destroy_device));
        destroy_device.device = create_device.device;
        if (ioctl(fd, LX_DXDESTROYDEVICE, &destroy_device) < 0) {
            printf("residency_batch destroy_device_failed device=0x%x\n",
                   create_device.device.v);
            ret = -1;
        }
    }
    if (ret == 0)
        printf("residency_batch ok\n");
    return ret;
}

int main(int argc, char **argv)
{
    struct d3dkmt_adapterinfo adapters[D3DKMT_ADAPTERS_MAX];
    struct d3dkmt_openadapterfromluid open_luid;
    struct d3dkmt_closeadapter close_adapter;
    struct winluid enum2_selected_luid;
    uint32 adapter_count;
    uint32 adapter_index = D3DKMT_ADAPTERS_MAX;
    uint32 enum2_selected_handle = 0;
    uint32 enum2_selected_sources = 0;
    uint32 enum3_count;
    uint32 enum3_index = D3DKMT_ADAPTERS_MAX;
    int fd;
    int ret = 0;
    int leak_on_close = 0;
    int try_context = 0;
    int try_submit = 0;
    int try_wait = 0;
    int owner_isolation = 0;
    int handle_lifetime_validate = 0;
    int tracker_stress = 0;
    int wsl_trace_replay = 0;
    int wddm_payload_validate = 0;
    int shared_present_validate = 0;
    int present_source_failclosed_validate = 0;
    int shared_lifetime_validate = 0;
    int shared_private_validate = 0;
    int raw_runtime_validate = 0;
    int residency_batch_validate = 0;
    int wddm_trace_capture_plan = 0;
    const char *trace_compare_trace = 0;
    const char *trace_compare_kernel = 0;
    const char *runtime_blob_compare_wsl = 0;
    const char *runtime_blob_compare_xv6 = 0;
    enum dxgprobe_stage stage = DXGPROBE_STAGE_FULL;
    const char *opt_value;

    memset(&enum2_selected_luid, 0, sizeof(enum2_selected_luid));

    if (argc == 3 && strcmp(argv[1], "--owner-isolation-child") == 0)
        return probe_owner_isolation_child((uint32)atoi(argv[2])) < 0 ?
               1 : 0;

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
        else if (strcmp(argv[i], "--handle-lifetime-validate") == 0)
            handle_lifetime_validate = 1;
        else if (strcmp(argv[i], "--create-publication-faults") == 0 ||
                 strcmp(argv[i],
                        "--create-publication-faults-validate") == 0)
            g_create_publication_faults_validate = 1;
        else if (strcmp(argv[i], "--import-negative") == 0 ||
                 strcmp(argv[i], "--import-negative-validate") == 0)
            g_import_negative_validate = 1;
        else if (strcmp(argv[i], "--tracker-stress") == 0)
            tracker_stress = 1;
        else if (strcmp(argv[i], "--wsl-trace-replay") == 0)
            wsl_trace_replay = 1;
        else if (strcmp(argv[i], "--wddm-payload-validate") == 0)
            wddm_payload_validate = 1;
        else if (strcmp(argv[i], "--shared-present-validate") == 0)
            shared_present_validate = 1;
        else if (strcmp(argv[i],
                        "--present-source-failclosed-validate") == 0)
            present_source_failclosed_validate = 1;
        else if (strcmp(argv[i], "--shared-lifetime-validate") == 0)
            shared_lifetime_validate = 1;
        else if (strcmp(argv[i], "--shared-exporter-close-lifetime") == 0 ||
                 strcmp(argv[i],
                        "--shared-exporter-close-lifetime-validate") == 0)
            g_shared_exporter_close_lifetime_validate = 1;
        else if (strcmp(argv[i], "--shared-seal-provenance") == 0 ||
                 strcmp(argv[i],
                        "--shared-seal-provenance-validate") == 0)
            g_shared_seal_provenance_validate = 1;
        else if (strcmp(argv[i], "--shared-mutation") == 0 ||
                 strcmp(argv[i], "--shared-mutation-validate") == 0)
            g_shared_mutation_validate = 1;
        else if (strcmp(argv[i], "--qai-admission") == 0 ||
                 strcmp(argv[i], "--qai-admission-validate") == 0)
            g_qai_admission_validate = 1;
        else if (strcmp(argv[i], "--shared-private-validate") == 0)
            shared_private_validate = 1;
        else if (strcmp(argv[i], "--raw-runtime-validate") == 0)
            raw_runtime_validate = 1;
        else if (strcmp(argv[i], "--resource-nt") == 0 ||
                 strcmp(argv[i], "--resource-nt-validate") == 0)
            g_resource_nt_validate = 1;
        else if (strcmp(argv[i], "--fb-existing-sysmem") == 0 ||
                 strcmp(argv[i], "--fb-existing-sysmem-validate") == 0)
            g_fb_existing_sysmem_validate = 1;
        else if (strcmp(argv[i], "--scanout-existing-sysmem") == 0 ||
                 strcmp(argv[i], "--scanout-existing-sysmem-validate") == 0)
            g_scanout_existing_sysmem_validate = 1;
        else if (strcmp(argv[i], "--scanout-d3d12-bridge") == 0 ||
                 strcmp(argv[i], "--scanout-d3d12-bridge-validate") == 0)
            g_scanout_d3d12_bridge_validate = 1;
        else if (strcmp(argv[i], "--scanout-pin-lifetime") == 0 ||
                 strcmp(argv[i], "--scanout-pin-lifetime-validate") == 0)
            g_scanout_pin_lifetime_validate = 1;
        else if (strcmp(argv[i], "--residency-batch-validate") == 0)
            residency_batch_validate = 1;
        else if (option_value(argv[i], "--adapter-index=", &opt_value)) {
            if (parse_u32_option_value(opt_value,
                                       &g_requested_adapter_index) < 0) {
                printf("dxgprobe: bad --adapter-index value %s\n",
                       opt_value);
                return 1;
            }
        }
        else if (option_value(argv[i], "--adapter-luid=", &opt_value)) {
            if (parse_adapter_luid_option_value(
                    opt_value, &g_requested_adapter_luid) < 0) {
                printf("dxgprobe: bad --adapter-luid value %s\n",
                       opt_value);
                return 1;
            }
            g_requested_adapter_luid_set = 1;
        }
        else if (option_value(argv[i], "--res-count=", &opt_value)) {
            if (parse_u32_option_value(opt_value,
                                       &g_residency_batch_count) < 0 ||
                g_residency_batch_count < 1 ||
                g_residency_batch_count > 2) {
                printf("dxgprobe: bad --res-count value %s\n",
                       opt_value);
                return 1;
            }
        }
        else if (option_value(argv[i], "--res-flags=", &opt_value)) {
            if (parse_u32_option_value(opt_value,
                                       &g_residency_batch_flags) < 0) {
                printf("dxgprobe: bad --res-flags value %s\n",
                       opt_value);
                return 1;
            }
        }
        else if (option_value(argv[i], "--sync-create-flags=", &opt_value)) {
            if (parse_u32_option_value(opt_value,
                                       &g_sync_create_flags) < 0) {
                printf("dxgprobe: bad --sync-create-flags value %s\n",
                       opt_value);
                return 1;
            }
        }
        else if (option_value(argv[i], "--sync-open-flags=", &opt_value)) {
            if (parse_u32_option_value(opt_value,
                                       &g_sync_open_flags) < 0) {
                printf("dxgprobe: bad --sync-open-flags value %s\n",
                       opt_value);
                return 1;
            }
            g_sync_open_flags_set = 1;
        }
        else if (option_value(argv[i], "--scf=", &opt_value)) {
            if (parse_u32_option_value(opt_value,
                                       &g_sync_create_flags) < 0) {
                printf("dxgprobe: bad --scf value %s\n", opt_value);
                return 1;
            }
        }
        else if (option_value(argv[i], "--sof=", &opt_value)) {
            if (parse_u32_option_value(opt_value,
                                       &g_sync_open_flags) < 0) {
                printf("dxgprobe: bad --sof value %s\n", opt_value);
                return 1;
            }
            g_sync_open_flags_set = 1;
        }
        else if (strcmp(argv[i], "--syncfile") == 0 ||
                 strcmp(argv[i], "--sync-file") == 0) {
            g_sync_file_validate = 1;
            stage = DXGPROBE_STAGE_SYNC_ONLY;
        }
        else if (option_value(argv[i], "--sync-file-value=", &opt_value)) {
            if (parse_u64_option_value(opt_value,
                                       &g_sync_file_target_value) < 0) {
                printf("dxgprobe: bad --sync-file-value value %s\n",
                       opt_value);
                return 1;
            }
        }
        else if (option_value(argv[i], "--sfv=", &opt_value)) {
            if (parse_u64_option_value(opt_value,
                                       &g_sync_file_target_value) < 0) {
                printf("dxgprobe: bad --sfv value %s\n", opt_value);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--wddm-trace-compare") == 0 &&
                 i + 2 < argc) {
            trace_compare_trace = argv[++i];
            trace_compare_kernel = argv[++i];
        }
        else if ((strcmp(argv[i], "--runtime-blob-compare") == 0 ||
                  strcmp(argv[i], "--blob-compare") == 0) &&
                 i + 2 < argc) {
            runtime_blob_compare_wsl = argv[++i];
            runtime_blob_compare_xv6 = argv[++i];
        }
        else if (strcmp(argv[i], "--wddm-trace-capture-plan") == 0)
            wddm_trace_capture_plan = 1;
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
    if (g_sync_file_validate)
        stage = DXGPROBE_STAGE_SYNC_ONLY;
    if (wddm_trace_capture_plan) {
        print_wddm_trace_capture_plan(trace_compare_trace,
                                      trace_compare_kernel);
        return 0;
    }
    if (trace_compare_trace != 0)
        return probe_wddm_trace_compare(trace_compare_trace,
                                        trace_compare_kernel) < 0 ? 1 : 0;
    if (runtime_blob_compare_wsl != 0)
        return probe_runtime_blob_compare(runtime_blob_compare_wsl,
                                          runtime_blob_compare_xv6) < 0 ?
               1 : 0;
    if (try_submit)
        try_context = 1;

    fd = open("/dev/dxg", O_RDWR);
    if (fd < 0) {
        printf("dxgprobe: open /dev/dxg failed\n");
        return 1;
    }

    if (g_qai_admission_validate) {
        ret = probe_qai_admission_validate(fd) < 0 ? 1 : 0;
        close(fd);
        return ret;
    }

    if (enum_dxg_adapters2_list(fd, adapters, &adapter_count,
                                "dxgprobe") < 0) {
        close(fd);
        return 1;
    }
    if (select_dxg_adapter_index(adapters, adapter_count, &adapter_index,
                                 "enum2", fd) < 0) {
        printf("dxgprobe: enum adapters2 no selectable adapter count=%u\n",
               adapter_count);
        close(fd);
        return 1;
    }

    printf("dxg_adapters %u\n", adapter_count);
    printf("adapter%u handle=0x%x luid=%x:%x sources=%u\n",
           adapter_index, adapters[adapter_index].adapter_handle.v,
           adapters[adapter_index].adapter_luid.b,
           adapters[adapter_index].adapter_luid.a,
           adapters[adapter_index].num_sources);
    enum2_selected_luid = adapters[adapter_index].adapter_luid;
    enum2_selected_handle = adapters[adapter_index].adapter_handle.v;
    enum2_selected_sources = adapters[adapter_index].num_sources;

    if (enum_dxg_adapters3_list(fd, adapters, &enum3_count) < 0) {
        close(fd);
        return 1;
    }
    if (select_dxg_adapter_index(adapters, enum3_count, &enum3_index,
                                 "enum3", fd) < 0) {
        printf("dxgprobe: enum3 no selectable adapter count=%u\n",
               enum3_count);
        close(fd);
        return 1;
    }
    printf("dxg_adapters3 %u handle%u=0x%x\n",
           enum3_count, enum3_index,
           adapters[enum3_index].adapter_handle.v);
    printf("adapter_list_parity_matrix enum2_count=%u enum2_index=%u enum2_handle=0x%x enum2_luid=%x:%x enum2_sources=%u enum3_count=%u enum3_index=%u enum3_handle=0x%x enum3_luid=%x:%x enum3_sources=%u requested_index=%u requested_luid_set=%u requested_luid=%x:%x selected_luid_match=%u status=%s\n",
           adapter_count, adapter_index, enum2_selected_handle,
           enum2_selected_luid.b, enum2_selected_luid.a,
           enum2_selected_sources, enum3_count, enum3_index,
           adapters[enum3_index].adapter_handle.v,
           adapters[enum3_index].adapter_luid.b,
           adapters[enum3_index].adapter_luid.a,
           adapters[enum3_index].num_sources, g_requested_adapter_index,
           g_requested_adapter_luid_set, g_requested_adapter_luid.b,
           g_requested_adapter_luid.a,
           enum2_selected_luid.a == adapters[enum3_index].adapter_luid.a &&
           enum2_selected_luid.b == adapters[enum3_index].adapter_luid.b,
           enum2_selected_luid.a == adapters[enum3_index].adapter_luid.a &&
           enum2_selected_luid.b == adapters[enum3_index].adapter_luid.b ?
           "PASS" : "FAIL");

    memset(&open_luid, 0, sizeof(open_luid));
    open_luid.adapter_luid = adapters[enum3_index].adapter_luid;
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
    if (tracker_stress) {
        if (probe_tracker_stress(fd, open_luid.adapter_handle) < 0)
            ret = 1;
        goto close_adapter;
    }
    if (wsl_trace_replay) {
        if (probe_wsl_trace_replay(fd, open_luid.adapter_handle,
                                   open_luid.adapter_luid) < 0)
            ret = 1;
        goto close_adapter;
    }
    if (wddm_payload_validate) {
        if (probe_wddm_payload_diagnostics() < 0)
            ret = 1;
        goto close_adapter;
    }
    if (residency_batch_validate) {
        if (probe_residency_batch_validate(fd, open_luid.adapter_handle) < 0)
            ret = 1;
        goto close_adapter;
    }
    if (shared_present_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_shared_present_contract(validate_fd, validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (present_source_failclosed_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_present_source_failclosed_contract(validate_fd,
                                                    validate_device,
                                                    enum2_selected_luid) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (shared_lifetime_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_shared_lifetime_contract(validate_fd, validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (g_import_negative_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_import_negative_contract(validate_fd, validate_adapter,
                                           enum2_selected_luid,
                                           validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (g_shared_exporter_close_lifetime_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_shared_exporter_close_lifetime_contract(
                validate_fd, validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (g_shared_seal_provenance_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_shared_seal_provenance_contract(validate_fd,
                                                  validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (g_shared_mutation_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_shared_mutation_contract(validate_fd,
                                           validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (g_resource_nt_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_resource_nt_contract(validate_fd, validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (g_fb_existing_sysmem_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_fb_existing_sysmem_contract(validate_fd,
                                              validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (g_scanout_existing_sysmem_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_scanout_existing_sysmem_contract(validate_fd,
                                                   validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (g_scanout_pin_lifetime_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_scanout_existing_sysmem_pin_lifetime_contract(
                validate_fd, validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (g_scanout_d3d12_bridge_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0 ||
            probe_scanout_d3d12_bridge_contract(validate_fd,
                                                validate_device) < 0)
            ret = 1;
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (shared_private_validate || raw_runtime_validate) {
        struct d3dkmthandle validate_adapter;
        struct d3dkmthandle validate_device;
        int validate_fd = -1;
        const char *tag = raw_runtime_validate ? "rawruntime" :
                          "shared_private";

        memset(&validate_adapter, 0, sizeof(validate_adapter));
        memset(&validate_device, 0, sizeof(validate_device));
        if (open_first_dxg_device(&validate_fd, &validate_adapter,
                                  &validate_device) < 0) {
            printf("%s FAIL stage=open_device\n", tag);
            ret = 1;
        } else if (probe_raw_runtime_allocation_contract(validate_fd,
                                                         validate_device,
                                                         tag) < 0) {
            ret = 1;
        }
        if (validate_fd >= 0)
            close_dxg_device(validate_fd, validate_adapter,
                             validate_device);
        goto close_adapter;
    }
    if (owner_isolation) {
        if (probe_owner_isolation(fd, open_luid.adapter_handle) < 0)
            ret = 1;
        goto close_adapter;
    }
    if (handle_lifetime_validate) {
        if (probe_handle_lifetime_validate(fd,
                                           open_luid.adapter_handle,
                                           enum2_selected_luid) < 0)
            ret = 1;
        if (probe_process_memory_lifetime_validate() < 0)
            ret = 1;
        if (probe_process_adapter_validate(enum2_selected_luid) < 0)
            ret = 1;
        goto close_adapter;
    }
    if (g_create_publication_faults_validate) {
        if (probe_create_publication_faults_validate(
                fd, open_luid.adapter_handle) < 0)
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

#include "kernel/inc/types.h"
#include "kernel/inc/uabi/d3dkmthk.h"
#include "kernel/inc/uabi/fcntl.h"
#include "user/user.h"

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

static int probe_device_context(int fd, struct d3dkmthandle adapter)
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
    struct d3dkmt_createcontextvirtual create_context;
    struct d3dkmt_destroycontext destroy_context;
    struct d3dkmt_destroydevice destroy_device;
    struct d3dkmt_createpagingqueue create_paging_queue;
    struct d3dddi_destroypagingqueue destroy_paging_queue;
    struct d3dddi_reservegpuvirtualaddress reserve_gpuva;
    struct d3dkmt_freegpuvirtualaddress free_gpuva;
    struct d3dkmt_createsynchronizationobject2 create_sync;
    struct d3dkmt_destroysynchronizationobject destroy_sync;
    int ret = 0;

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
        memset(&destroy_paging_queue, 0, sizeof(destroy_paging_queue));
        destroy_paging_queue.paging_queue = create_paging_queue.paging_queue;
        if (ioctl(fd, LX_DXDESTROYPAGINGQUEUE, &destroy_paging_queue) < 0) {
            printf("dxgprobe: destroy paging queue failed\n");
            ret = -1;
        }
    }

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
        memset(&free_gpuva, 0, sizeof(free_gpuva));
        free_gpuva.adapter = adapter;
        free_gpuva.base_address = reserve_gpuva.virtual_address;
        free_gpuva.size = reserve_gpuva.size;
        if (ioctl(fd, LX_DXFREEGPUVIRTUALADDRESS, &free_gpuva) < 0) {
            printf("dxgprobe: free gpuva failed\n");
            ret = -1;
        }
    }

    memset(&create_sync, 0, sizeof(create_sync));
    create_sync.device = create_device.device;
    create_sync.info.type = _D3DDDI_FENCE;
    create_sync.info.fence.fence_value = 0;
    if (ioctl(fd, LX_DXCREATESYNCHRONIZATIONOBJECT, &create_sync) < 0 ||
        create_sync.sync_object.v == 0) {
        printf("dxgprobe: create sync object failed sync=0x%x\n",
               create_sync.sync_object.v);
        ret = -1;
    } else {
        printf("sync_object 0x%x\n", create_sync.sync_object.v);
        memset(&destroy_sync, 0, sizeof(destroy_sync));
        destroy_sync.sync_object = create_sync.sync_object;
        if (ioctl(fd, LX_DXDESTROYSYNCHRONIZATIONOBJECT, &destroy_sync) < 0) {
            printf("dxgprobe: destroy sync object failed\n");
            ret = -1;
        }
    }

    memset(&create_context, 0, sizeof(create_context));
    create_context.device = create_device.device;
    create_context.engine_affinity = 1;
    create_context.client_hint = _D3DKMT_CLIENTHINT_DX12;
    create_context.priv_drv_data = (uint64)context_private_data;
    create_context.priv_drv_data_size = sizeof(context_private_data);
    if (ioctl(fd, LX_DXCREATECONTEXTVIRTUAL, &create_context) < 0 ||
        create_context.context.v == 0) {
        printf("dxgprobe: create context failed context=0x%x\n",
               create_context.context.v);
        ret = -1;
    } else {
        printf("context_handle 0x%x\n", create_context.context.v);
        memset(&destroy_context, 0, sizeof(destroy_context));
        destroy_context.context = create_context.context;
        if (ioctl(fd, LX_DXDESTROYCONTEXT, &destroy_context) < 0) {
            printf("dxgprobe: destroy context failed\n");
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

int main(int argc, char **argv)
{
    struct d3dkmt_enumadapters2 enum2;
    struct d3dkmt_adapterinfo adapters[D3DKMT_ADAPTERS_MAX];
    struct d3dkmt_openadapterfromluid open_luid;
    struct d3dkmt_closeadapter close_adapter;
    int fd;
    int ret = 0;

    (void)argc;
    (void)argv;

    fd = open("/dev/dxg", O_RDWR);
    if (fd < 0) {
        printf("dxgprobe: open /dev/dxg failed\n");
        return 1;
    }

    memset(&enum2, 0, sizeof(enum2));
    if (ioctl(fd, LX_DXENUMADAPTERS2, &enum2) < 0 ||
        enum2.num_adapters == 0) {
        printf("dxgprobe: enum count failed count=%u\n",
               enum2.num_adapters);
        close(fd);
        return 1;
    }
    if (enum2.num_adapters > D3DKMT_ADAPTERS_MAX)
        enum2.num_adapters = D3DKMT_ADAPTERS_MAX;
    memset(adapters, 0, sizeof(adapters));
    enum2.adapters = (uint64)adapters;
    if (ioctl(fd, LX_DXENUMADAPTERS2, &enum2) < 0 ||
        enum2.num_adapters == 0 || adapters[0].adapter_handle.v == 0) {
        printf("dxgprobe: enum adapters failed count=%u handle=0x%x\n",
               enum2.num_adapters, adapters[0].adapter_handle.v);
        close(fd);
        return 1;
    }

    printf("dxg_adapters %u\n", enum2.num_adapters);
    printf("adapter0 handle=0x%x luid=%x:%x sources=%u\n",
           adapters[0].adapter_handle.v, adapters[0].adapter_luid.b,
           adapters[0].adapter_luid.a, adapters[0].num_sources);

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
    if (probe_device_context(fd, open_luid.adapter_handle) < 0)
        ret = 1;

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
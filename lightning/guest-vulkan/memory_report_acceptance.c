/* Module: lightning.guest_vulkan.memory_report_acceptance
 * Purpose: Exercise the pinned Venus allocation-report callback on Windows.
 * Created: 2026-10-11
 * Architecture: The build extracts the pinned Mesa callback verbatim with its
 * actual object headers into memory_report_under_test.h for this C ABI fixture.
 */
#include "memory_report_under_test.h"
#include <stdio.h>

static VkDeviceMemoryReportCallbackDataEXT observed;
static unsigned reports;

static void VKAPI_CALL record_report(const VkDeviceMemoryReportCallbackDataEXT *data, void *user)
{
    (void)user;
    observed = *data;
    reports++;
}

int main(void)
{
    struct vn_physical_device physical = {0};
    struct vn_device device = {0};
    struct vn_device_memory memory = {0};
    struct vn_renderer_bo buffer = {0};
    struct vk_device_memory_report callback = {record_report, NULL};

    device.physical_device = &physical;
    memory.base.vk.export_handle_types = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    memory.base.vk.size = 4096;
    memory.base.id = 37;
    physical.memory_properties.memoryTypes[0].heapIndex = 2;
    vn_device_memory_emit_report(&device, &memory, true, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    device.base.vk.memory_reports = &callback;
    device.base.vk.memory_report_count = 1;
    vn_device_memory_emit_report(&device, &memory, true, VK_ERROR_OUT_OF_DEVICE_MEMORY);

    if (reports != 1 || observed.type != VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATION_FAILED_EXT ||
        observed.memoryObjectId != 0 || observed.size != 4096 || observed.heapIndex != 2)
        return 1;
    memory.base.vk.export_handle_types = 0;
    memory.base.vk.import_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    vn_device_memory_emit_report(&device, &memory, true, VK_ERROR_INVALID_EXTERNAL_HANDLE);

    if (reports != 2 || observed.type != VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATION_FAILED_EXT ||
        observed.memoryObjectId != 0)
        return 2;
    buffer.res_id = 59;
    memory.base_bo = &buffer;
    vn_device_memory_emit_report(&device, &memory, true, VK_SUCCESS);

    if (reports != 3 || observed.type != VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT ||
        observed.memoryObjectId != 59)
        return 3;
    memory.base.vk.import_handle_type = 0;
    memory.base_bo = NULL;
    vn_device_memory_emit_report(&device, &memory, true, VK_SUCCESS);

    if (reports != 4 || observed.type != VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT ||
        observed.memoryObjectId != 37)
        return 4;
    puts("PASS: failed export/import reports and successful allocation identities");
    return 0;
}

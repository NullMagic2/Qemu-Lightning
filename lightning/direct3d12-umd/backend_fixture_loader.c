/*
 * Module: lightning.direct3d12_umd.backend_fixture_loader
 * Purpose: Model only the exclusive direct-driver loader calls used by the fixture.
 * Created: 2026-10-24
 * Architecture: Test dependency, not the Khronos loader or evidence of real Venus.
 */
#include "backend_fixture_model.h"
static PFN_vkGetInstanceProcAddr selected_driver;

static VkResult VKAPI_CALL model_create(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    const VkDirectDriverLoadingListLUNARG *list = info->pNext;
    PFN_vkCreateInstance create;

    if (info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
        !info->pApplicationInfo || info->pApplicationInfo->apiVersion != QLDX12_REQUIRED_VULKAN_API ||
        info->enabledLayerCount != 0 || info->enabledExtensionCount != 1 ||
        lstrcmpA(info->ppEnabledExtensionNames[0], VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME) != 0 ||
        list == NULL || list->sType != VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG ||
        list->mode != VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG || list->driverCount != 1 ||
        !list->pDrivers || list->pDrivers[0].sType != VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG ||
        list->pDrivers[0].flags != 0 || !list->pDrivers[0].pfnGetInstanceProcAddr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    selected_driver = list->pDrivers[0].pfnGetInstanceProcAddr;
    create = (PFN_vkCreateInstance)(uintptr_t)selected_driver(VK_NULL_HANDLE, "vkCreateInstance");
    return create ? create(info, allocator, instance) : VK_ERROR_INITIALIZATION_FAILED;
}

__declspec(dllexport) void VKAPI_CALL vkDestroyInstance(VkInstance instance,
    const VkAllocationCallbacks *allocator)
{
    PFN_vkDestroyInstance destroy = selected_driver
        ? (PFN_vkDestroyInstance)(uintptr_t)selected_driver(instance, "vkDestroyInstance") : NULL;

    if (destroy) {
        destroy(instance, allocator);
    }
}

__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (lstrcmpA(name, "vkCreateInstance") == 0) {
        return (PFN_vkVoidFunction)(uintptr_t)model_create;
    }
    if (lstrcmpA(name, "vkDestroyInstance") == 0) {
        return (PFN_vkVoidFunction)(uintptr_t)vkDestroyInstance;
    }
    return selected_driver ? selected_driver(instance, name) : NULL;
}

/*
 * Module: lightning.direct3d12_umd.native_present_abi
 * Purpose: Expose the experimental Win32 native-image presentation entry points.
 * Created: 2026-11-17
 * Architecture: Resolve official DDI storage cookies and pass scalar identities
 * to Rust. The Win32 WSI owns the swapchain and the final compositor handoff.
 */
#include "native_present.h"

HRESULT ql_dx12_native_present(uintptr_t device, uint64_t device_cookie,
    uintptr_t resource, uint64_t resource_cookie, uintptr_t window, uint32_t interval);
HRESULT ql_dx12_native_present_retire(uintptr_t device, uint64_t device_cookie);

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalPresentNativeImage(
    D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE resource, HWND window, UINT interval)
{
    uint64_t device_cookie, resource_cookie;
    HRESULT result = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(result))
        result = ql_dx12_native_cookie_for(resource.pDrvPrivate,
            QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);
    if (FAILED(result))
        return result;

    return ql_dx12_native_present((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)resource.pDrvPrivate, resource_cookie, (uintptr_t)window, interval);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalRetireNativePresentation(D3D12DDI_HDEVICE device)
{
    uint64_t cookie;
    HRESULT result = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (FAILED(result))
        return result;

    return ql_dx12_native_present_retire((uintptr_t)device.pDrvPrivate, cookie);
}

HRESULT ql_dx12_native_present_on_queue(uintptr_t device, uint64_t device_cookie,
    uintptr_t queue, uint64_t queue_cookie, uintptr_t resource, uint64_t resource_cookie,
    uintptr_t window, uint32_t interval);
HRESULT ql_dx12_native_present_retire_window(uintptr_t device, uint64_t device_cookie, uintptr_t window);

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalPresentNativeImageOnQueue(
    D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDQUEUE queue, D3D12DDI_HRESOURCE resource, HWND window, UINT interval)
{
    uint64_t device_cookie, queue_cookie, resource_cookie;
    HRESULT result = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(result))
        result = ql_dx12_native_cookie_for(queue.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_QUEUE, &queue_cookie);
    if (SUCCEEDED(result))
        result = ql_dx12_native_cookie_for(resource.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);
    if (FAILED(result))
        return result;

    return ql_dx12_native_present_on_queue((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)queue.pDrvPrivate, queue_cookie, (uintptr_t)resource.pDrvPrivate,
        resource_cookie, (uintptr_t)window, interval);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalRetireNativePresentationWindow(
    D3D12DDI_HDEVICE device, HWND window)
{
    uint64_t cookie;
    HRESULT result = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (FAILED(result))
        return result;
    return ql_dx12_native_present_retire_window((uintptr_t)device.pDrvPrivate, cookie, (uintptr_t)window);
}

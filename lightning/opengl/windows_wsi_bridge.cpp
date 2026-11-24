/* Module: lightning.opengl.windows_wsi_bridge
 * Purpose: Present shared Vulkan images through native D3D11 and DXGI.
 * Created: 2026-11-08
 * Architecture: SDK-only C++ ABI adapter; no mapped pixels or GDI presentation.
 */
#include <windows.h>
#include <winternl.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dkmthk.h>
#include "policy.h"
#include "windows_wsi_bridge.h"

namespace {
constexpr ULONGLONG gpu_timeout_ms = 10000;
constexpr UINT maximum_private_bytes = 1024 * 1024;
template <typename T> void release(T *&object) {
    if (object) { object->Release(); object = nullptr; }
}
HMODULE system_library(const wchar_t *name) {
    return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}
HRESULT native_failure(NTSTATUS status) {
    return status < 0 ? HRESULT_FROM_NT(status) : S_OK;
}
}

struct qlg_wsi_bridge {
    HMODULE d3d11_module;
    HMODULE dxgi_module;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain1 *swapchain;
    ID3D11Query *completion;
    D3DKMT_HANDLE adapter;
    D3DKMT_HANDLE kernel_device;
    UINT width, height;
    DXGI_FORMAT format;
};
struct qlg_wsi_image {
    ID3D11Texture2D *texture;
    D3DKMT_HANDLE resource;
};

extern "C" void qlg_wsi_destroy(qlg_wsi_bridge *bridge) {
    if (!bridge) return;
    release(bridge->completion);
    release(bridge->swapchain);
    release(bridge->context);
    release(bridge->device);
    if (bridge->kernel_device) {
        D3DKMT_DESTROYDEVICE request = {};
        request.hDevice = bridge->kernel_device;
        D3DKMTDestroyDevice(&request);
    }
    if (bridge->adapter) {
        D3DKMT_CLOSEADAPTER request = {};
        request.hAdapter = bridge->adapter;
        D3DKMTCloseAdapter(&request);
    }
    if (bridge->d3d11_module) FreeLibrary(bridge->d3d11_module);
    if (bridge->dxgi_module) FreeLibrary(bridge->dxgi_module);
    HeapFree(GetProcessHeap(), 0, bridge);
}

extern "C" int32_t qlg_wsi_create(const void *luid_bytes, uint32_t width,
    uint32_t height, uint32_t format, qlg_wsi_bridge **output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!luid_bytes || !width || !height ||
        (format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM))
        return E_INVALIDARG;
    if (uint64_t(width) * height > QLG_MAX_ALLOCATION_BYTES / QLG_COLOR_BYTES)
        return E_OUTOFMEMORY;
    auto *bridge = static_cast<qlg_wsi_bridge *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(qlg_wsi_bridge)));
    if (!bridge) return E_OUTOFMEMORY;
    bridge->width = width; bridge->height = height;
    bridge->format = static_cast<DXGI_FORMAT>(format);
    bridge->dxgi_module = system_library(L"dxgi.dll");
    bridge->d3d11_module = system_library(L"d3d11.dll");
    using Factory = HRESULT (WINAPI *)(REFIID, void **);
    using Device = decltype(&D3D11CreateDevice);
    Factory factory_fn = nullptr; Device device_fn = nullptr;
    FARPROC factory_address = bridge->dxgi_module ? GetProcAddress(bridge->dxgi_module, "CreateDXGIFactory1") : nullptr;
    FARPROC device_address = bridge->d3d11_module ? GetProcAddress(bridge->d3d11_module, "D3D11CreateDevice") : nullptr;
    CopyMemory(&factory_fn, &factory_address, sizeof(factory_fn));
    CopyMemory(&device_fn, &device_address, sizeof(device_fn));
    HRESULT hr = factory_fn && device_fn ? S_OK : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    IDXGIFactory2 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    if (SUCCEEDED(hr)) hr = factory_fn(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));
    LUID luid = {}; CopyMemory(&luid, luid_bytes, sizeof(luid));
    if (SUCCEEDED(hr)) {
        for (UINT index = 0;; ++index) {
            hr = factory->EnumAdapters1(index, &adapter);
            if (FAILED(hr)) break;
            DXGI_ADAPTER_DESC1 desc = {};
            hr = adapter->GetDesc1(&desc);
            if (SUCCEEDED(hr) && desc.AdapterLuid.LowPart == luid.LowPart &&
                desc.AdapterLuid.HighPart == luid.HighPart && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
            release(adapter);
        }
    }
    if (SUCCEEDED(hr)) hr = device_fn(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &bridge->device, nullptr, &bridge->context);
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = width; desc.Height = height; desc.Format = bridge->format;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = factory->CreateSwapChainForComposition(bridge->device, &desc, nullptr, &bridge->swapchain);
    }
    if (SUCCEEDED(hr)) {
        D3D11_QUERY_DESC desc = {}; desc.Query = D3D11_QUERY_EVENT;
        hr = bridge->device->CreateQuery(&desc, &bridge->completion);
    }
    if (SUCCEEDED(hr)) {
        D3DKMT_OPENADAPTERFROMLUID request = {}; request.AdapterLuid = luid;
        hr = native_failure(D3DKMTOpenAdapterFromLuid(&request));
        if (SUCCEEDED(hr)) bridge->adapter = request.hAdapter;
    }
    if (SUCCEEDED(hr)) {
        D3DKMT_CREATEDEVICE request = {}; request.hAdapter = bridge->adapter;
        hr = native_failure(D3DKMTCreateDevice(&request));
        if (SUCCEEDED(hr)) bridge->kernel_device = request.hDevice;
    }
    release(adapter); release(factory);
    if (FAILED(hr)) { qlg_wsi_destroy(bridge); return hr; }
    *output = bridge;
    return S_OK;
}

extern "C" void *qlg_wsi_content(qlg_wsi_bridge *bridge) {
    return bridge ? bridge->swapchain : nullptr;
}
extern "C" void qlg_wsi_image_destroy(qlg_wsi_bridge *bridge, qlg_wsi_image *image) {
    if (!image) return;
    if (image->resource) {
        D3DKMT_DESTROYALLOCATION request = {};
        request.hDevice = bridge->kernel_device; request.hResource = image->resource;
        D3DKMTDestroyAllocation(&request);
    }
    release(image->texture);
    HeapFree(GetProcessHeap(), 0, image);
}

extern "C" int32_t qlg_wsi_image_create(qlg_wsi_bridge *bridge,
    qlg_wsi_image **output, uint32_t *blob) {
    if (!bridge || !output || !blob) return E_INVALIDARG;
    *output = nullptr; *blob = 0;
    auto *image = static_cast<qlg_wsi_image *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(qlg_wsi_image)));
    if (!image) return E_OUTOFMEMORY;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = bridge->width; desc.Height = bridge->height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = bridge->format; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = bridge->device->CreateTexture2D(&desc, nullptr, &image->texture);
    IDXGIResource *shared = nullptr;
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr)) hr = image->texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void **>(&shared));
    if (SUCCEEDED(hr)) hr = shared->GetSharedHandle(&handle);
    release(shared);
    /* Legacy shared handles are kernel names, not CloseHandle-owned NT handles.
     * Keep the D3D texture and the opened kernel allocation until Vulkan releases
     * its import. Never treat the host blob ID as a Windows object handle. */
    D3DKMT_QUERYRESOURCEINFO query = {};
    query.hDevice = bridge->kernel_device;
    query.hGlobalShare = static_cast<D3DKMT_HANDLE>(reinterpret_cast<uintptr_t>(handle));
    if (SUCCEEDED(hr)) hr = native_failure(D3DKMTQueryResourceInfo(&query));
    if (SUCCEEDED(hr) && (query.NumAllocations != 1 ||
        query.PrivateRuntimeDataSize > maximum_private_bytes ||
        query.ResourcePrivateDriverDataSize > maximum_private_bytes ||
        query.TotalPrivateDriverDataSize > maximum_private_bytes)) hr = E_INVALIDARG;
    const SIZE_T storage_bytes = SIZE_T(query.PrivateRuntimeDataSize) +
        query.ResourcePrivateDriverDataSize + query.TotalPrivateDriverDataSize;
    auto *storage = SUCCEEDED(hr) ? static_cast<BYTE *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, storage_bytes ? storage_bytes : 1)) : nullptr;
    if (SUCCEEDED(hr) && !storage) hr = E_OUTOFMEMORY;
    D3DDDI_OPENALLOCATIONINFO allocation = {};
    if (SUCCEEDED(hr)) {
        D3DKMT_OPENRESOURCE request = {};
        request.hDevice = bridge->kernel_device; request.hGlobalShare = query.hGlobalShare;
        request.NumAllocations = 1; request.pOpenAllocationInfo = &allocation;
        request.pPrivateRuntimeData = storage; request.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
        request.pResourcePrivateDriverData = storage + query.PrivateRuntimeDataSize;
        request.ResourcePrivateDriverDataSize = query.ResourcePrivateDriverDataSize;
        request.pTotalPrivateDriverDataBuffer = storage + query.PrivateRuntimeDataSize + query.ResourcePrivateDriverDataSize;
        request.TotalPrivateDriverDataBufferSize = query.TotalPrivateDriverDataSize;
        hr = native_failure(D3DKMTOpenResource(&request));
        if (SUCCEEDED(hr)) image->resource = request.hResource;
    }
    QLG_ALLOCATION_DESC actual = {};
    if (SUCCEEDED(hr)) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(storage);
        const uintptr_t address = reinterpret_cast<uintptr_t>(allocation.pPrivateDriverData);
        if (allocation.PrivateDriverDataSize != sizeof(actual) || address < begin ||
            storage_bytes < sizeof(actual) || address - begin > storage_bytes - sizeof(actual)) hr = E_INVALIDARG;
        else CopyMemory(&actual, allocation.pPrivateDriverData, sizeof(actual));
    }
    if (SUCCEEDED(hr) && (actual.magic != QLG_PRIVATE_MAGIC || actual.version != QLG_PRIVATE_VERSION ||
        !actual.shared_id || actual.kind != QLG_RESOURCE_RGBA8 || actual.width != bridge->width ||
        actual.height != bridge->height || actual.image_format != uint32_t(bridge->format) ||
        actual.levels != 1 || actual.layers != 1)) hr = E_INVALIDARG;
    if (storage) HeapFree(GetProcessHeap(), 0, storage);
    if (FAILED(hr)) { qlg_wsi_image_destroy(bridge, image); return hr; }
    bridge->context->Flush(); /* Publish the producer's GENERAL/EXTERNAL handoff. */
    *blob = actual.shared_id; *output = image;
    return S_OK;
}

extern "C" int32_t qlg_wsi_present(qlg_wsi_bridge *bridge, qlg_wsi_image *image, uint32_t interval) {
    if (!bridge || !image || interval > 1) return E_INVALIDARG;
    ID3D11Texture2D *destination = nullptr;
    HRESULT hr = bridge->swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&destination));
    if (FAILED(hr)) return hr;
    bridge->context->CopyResource(destination, image->texture);
    release(destination);
    bridge->context->End(bridge->completion);
    bridge->context->Flush();
    const ULONGLONG deadline = GetTickCount64() + gpu_timeout_ms;
    do {
        hr = bridge->context->GetData(bridge->completion, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr != S_FALSE) break;
        hr = bridge->device->GetDeviceRemovedReason();
        if (FAILED(hr)) return hr;
        if (GetTickCount64() >= deadline) return DXGI_ERROR_DEVICE_HUNG;
        Sleep(1);
    } while (true);
    if (FAILED(hr)) return hr;
    hr = bridge->device->GetDeviceRemovedReason();
    return FAILED(hr) ? hr : bridge->swapchain->Present(interval, 0);
}

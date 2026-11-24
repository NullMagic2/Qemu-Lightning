// Module: lightning.direct3d_bridge.buffer_map_acceptance
// Purpose: Compare mapped buffer extents with the Windows software device.
// Created: 2026-10-15
// Architecture: Reports public D3D11 map fields without copying app memory.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>

int main()
{
    const bool warp = strstr(GetCommandLineA(), "--warp") != nullptr;
    freopen(warp ? "C:\\Users\\Public\\QemuLightning-pitch-warp.txt" :
                   "C:\\Users\\Public\\QemuLightning-pitch-hardware.txt", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr UINT vendor = 0x1234, graphics_device = 0x11f1;
    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return 1;

    if (!warp) {
        for (UINT index = 0; factory->EnumAdapters1(index, &adapter) == S_OK; ++index) {
            DXGI_ADAPTER_DESC1 desc = {};
            adapter->GetDesc1(&desc);

            if (desc.VendorId == vendor && desc.DeviceId == graphics_device)
                break;
            adapter->Release();
            adapter = nullptr;
        }

        if (!adapter)
            return 2;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_10_1};
    HRESULT hr = D3D11CreateDevice(adapter, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
        nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context);

    if (FAILED(hr)) {
        printf("FAIL device=%08lx\n", static_cast<unsigned long>(hr));
        return 3;
    }
    const struct Scenario { const char *name; UINT bind, bytes; } scenarios[] = {
        {"vertex", D3D11_BIND_VERTEX_BUFFER, 128},
        {"index", D3D11_BIND_INDEX_BUFFER, 128},
        {"constant", D3D11_BIND_CONSTANT_BUFFER, 64},
        {"staging", 0, 128},
    };
    UINT failures = 0;

    for (const auto &scenario : scenarios) {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = scenario.bytes;
        desc.BindFlags = scenario.bind;
        desc.Usage = scenario.bind ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Buffer *buffer = nullptr;
        hr = device->CreateBuffer(&desc, nullptr, &buffer);
        D3D11_MAPPED_SUBRESOURCE mapped = {};

        if (SUCCEEDED(hr))
            hr = context->Map(buffer, 0, scenario.bind ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE, 0, &mapped);
        const bool pass = SUCCEEDED(hr) && mapped.pData &&
            mapped.RowPitch >= scenario.bytes && mapped.DepthPitch >= scenario.bytes;
        printf("%s %s bytes=%u row=%u depth=%u hr=%08lx warp=%u\n", pass ? "PASS" : "FAIL",
            scenario.name, scenario.bytes, mapped.RowPitch, mapped.DepthPitch,
            static_cast<unsigned long>(hr), warp);
        failures += !pass;

        if (SUCCEEDED(hr)) {
            memset(mapped.pData, 0, scenario.bytes);
            context->Unmap(buffer, 0);
        }

        if (buffer)
            buffer->Release();
    }
    printf("DONE failures=%u\n", failures);
    context->Release();
    device->Release();

    if (adapter)
        adapter->Release();
    factory->Release();
    return failures ? 4 : 0;
}

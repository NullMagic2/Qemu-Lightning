// Module: lightning.direct3d_bridge.composition_frame_acceptance
// Purpose: Exercise continuous composition frames with a uniform color marker.
// Created: 2026-10-15
// Architecture: Presentation remains GPU-backed. Sparse screen-pixel samples
// from the owned marker distinguish guest composition from a stale host console.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void check(HRESULT result, const char *phase)
{
    if (FAILED(result)) {
        printf("FAIL %s=%08lx\n", phase, static_cast<unsigned long>(result));
        exit(1);
    }
}

int main()
{
    freopen("C:\\Users\\Public\\QemuLightning-frame-integrity.txt", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr UINT width = 320, height = 180, duration_ms = 18000;
    constexpr ULONGLONG color_hold_ms = 1000;
    constexpr UINT lightning_vendor = 0x1234, lightning_device = 0x11f1;
    constexpr int left = 120, top = 120;
    IDXGIFactory2 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    check(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory)), "factory");

    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) == S_OK; ++index) {
        DXGI_ADAPTER_DESC1 info = {};
        adapter->GetDesc1(&info);

        if (info.VendorId == lightning_vendor && info.DeviceId == lightning_device)
            break;
        adapter->Release();
        adapter = nullptr;
    }

    if (!adapter)
        return 2;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_10_1};
    check(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
        &device, nullptr, &context), "device");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LightningFrameIntegrity";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Lightning frame integrity marker", WS_POPUP | WS_VISIBLE,
        left, top, width, height, nullptr, nullptr, wc.hInstance, nullptr);

    if (!window)
        return 3;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    IDXGISwapChain1 *swap = nullptr;
    check(factory->CreateSwapChainForComposition(device, &desc, nullptr, &swap), "swap_chain");
    IDXGIDevice *dxgi = nullptr;
    IDCompositionDevice *composition = nullptr;
    IDCompositionTarget *composition_target = nullptr;
    IDCompositionVisual *visual = nullptr;
    check(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi)), "dxgi_device");
    check(DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice), reinterpret_cast<void **>(&composition)), "composition");
    check(composition->CreateTargetForHwnd(window, TRUE, &composition_target), "composition_target");
    check(composition->CreateVisual(&visual), "visual");
    check(visual->SetContent(swap), "content");
    check(composition_target->SetRoot(visual), "root");
    check(composition->Commit(), "commit");
    ID3D11Texture2D *image = nullptr;
    ID3D11RenderTargetView *view = nullptr;
    check(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&image)), "back_buffer");
    check(device->CreateRenderTargetView(image, nullptr, &view), "render_view");
    printf("START screen=%dx%d marker=%d,%d,%u,%u duration_ms=%u\n",
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), left, top, width, height, duration_ms);
    const bool sample_screen = strstr(GetCommandLineA(), "--sample-screen") != nullptr;
    const ULONGLONG start = GetTickCount64();
    UINT frames = 0;
    ULONGLONG sampled_bucket = ~ULONGLONG(0);

    while (GetTickCount64() - start < duration_ms) {
        FLOAT color[4] = {0, 0, 0, 1};
        color[((GetTickCount64() - start) / color_hold_ms) % 3] = 1;
        context->ClearRenderTargetView(view, color);
        DXGI_PRESENT_PARAMETERS parameters = {};
        check(swap->Present1(1, 0, &parameters), "present");
        ++frames;
        const ULONGLONG elapsed = GetTickCount64() - start;
        const ULONGLONG bucket = elapsed / color_hold_ms;
        constexpr ULONGLONG sample_settle_ms = 500;

        if (sample_screen && bucket != sampled_bucket && elapsed % color_hold_ms >= sample_settle_ms) {
            HDC screen = GetDC(nullptr);
            const COLORREF pixel = GetPixel(screen, left + width / 2, top + height / 2);
            ReleaseDC(nullptr, screen);
            printf("SAMPLE elapsed_ms=%llu expected_channel=%llu rgb=%u,%u,%u valid=%u\n",
                elapsed, bucket % 3, GetRValue(pixel), GetGValue(pixel), GetBValue(pixel),
                pixel != CLR_INVALID);
            sampled_bucket = bucket;
        }
        MSG message = {};

        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    printf("DONE frames=%u elapsed_ms=%llu reason=%08lx\n", frames,
        GetTickCount64() - start, static_cast<unsigned long>(device->GetDeviceRemovedReason()));
    DestroyWindow(window);
    visual->Release();
    composition_target->Release();
    composition->Release();
    dxgi->Release();
    view->Release();
    image->Release();
    swap->Release();
    context->Release();
    device->Release();
    adapter->Release();
    factory->Release();
    return 0;
}

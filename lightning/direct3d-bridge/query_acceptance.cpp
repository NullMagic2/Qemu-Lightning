// Module: lightning.direct3d_bridge.query_acceptance
// Purpose: Compare asynchronous event polling and completion with WARP.
// Created: 2026-10-15
// Architecture: Exercises public D3D11 queries on the selected real adapter.
// No-flush readiness is recorded separately from latency and eventual completion.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>

static UINT failures;
static constexpr DWORD completion_ms = 3000;
static constexpr double maximum_poll_ms = 500;
static constexpr UINT initial_polls = 4;

static bool check(HRESULT hr, const char *phase)
{
    printf("%s %s=%08lx\n", FAILED(hr) ? "FAIL" : "PASS", phase,
        static_cast<unsigned long>(hr));
    failures += FAILED(hr);
    return SUCCEEDED(hr);
}

static void event_case(ID3D11Device *device, ID3D11DeviceContext *context,
                       ID3D11RenderTargetView *view, const char *name,
                       bool clear, bool allow_flush, bool reuse)
{
    D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *query = nullptr;

    if (!check(device->CreateQuery(&desc, &query), "create_event"))
        return;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const UINT generations = reuse ? 2 : 1;

    for (UINT generation = 0; generation < generations; ++generation) {
        const FLOAT color[] = {.25f, .5f, .75f, 1};

        if (clear)
            context->ClearRenderTargetView(view, color);
        context->End(query);
        UINT ready_polls = 0;
        double longest_ms = 0;
        printf("ISSUED %s generation=%u flags=%u\n", name, generation,
            allow_flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);

        for (UINT poll = 0; poll < initial_polls; ++poll) {
            BOOL done = FALSE;
            LARGE_INTEGER begin, end;
            QueryPerformanceCounter(&begin);
            const HRESULT hr = context->GetData(query, &done, sizeof(done),
                allow_flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
            QueryPerformanceCounter(&end);
            const double elapsed = double(end.QuadPart - begin.QuadPart) * 1000 / frequency.QuadPart;
            longest_ms = elapsed > longest_ms ? elapsed : longest_ms;
            printf("POLL %s generation=%u index=%u hr=%08lx done=%u elapsed_ms=%.3f\n",
                name, generation, poll, static_cast<unsigned long>(hr), done, elapsed);
            failures += (hr != S_OK && hr != S_FALSE) || (hr == S_OK && !done);
            ready_polls += hr == S_OK;
        }
        printf("%s NONBLOCKING %s generation=%u max_ms=%.3f ready_before_explicit_flush=%u\n",
            longest_ms <= maximum_poll_ms ? "PASS" : "FAIL", name, generation, longest_ms, ready_polls);
        failures += longest_ms > maximum_poll_ms;
        context->Flush();
        const ULONGLONG deadline = GetTickCount64() + completion_ms;
        BOOL done = FALSE;
        HRESULT hr = S_FALSE;

        while (GetTickCount64() < deadline) {
            hr = context->GetData(query, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);

            if (hr != S_FALSE)
                break;
            Sleep(1);
        }
        const bool completed = hr == S_OK && done;
        printf("%s COMPLETION %s generation=%u hr=%08lx done=%u\n",
            completed ? "PASS" : "FAIL", name, generation, static_cast<unsigned long>(hr), done);
        failures += !completed;
    }
    query->Release();
}

int main()
{
    const bool warp = strstr(GetCommandLineA(), "--warp") != nullptr;
    freopen(warp ? "C:\\Users\\Public\\QemuLightning-query-warp.txt" :
                   "C:\\Users\\Public\\QemuLightning-query-hardware.txt", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr UINT vendor = 0x1234, graphics_device = 0x11f1;
    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;

    if (!check(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
            reinterpret_cast<void **>(&factory)), "factory"))
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

    if (!check(D3D11CreateDevice(adapter, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context), "device"))
        return 3;
    wchar_t module_path[MAX_PATH] = {};
    HMODULE module = GetModuleHandleW(L"QemuLightningD3D10.dll");

    if (module && GetModuleFileNameW(module, module_path, MAX_PATH))
        printf("UMD path=%ls\n", module_path);
    D3D11_TEXTURE2D_DESC desc = {};
    constexpr UINT side = 32;
    desc.Width = desc.Height = side;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *texture = nullptr;
    ID3D11RenderTargetView *view = nullptr;

    if (!check(device->CreateTexture2D(&desc, nullptr, &texture), "texture") ||
        !check(device->CreateRenderTargetView(texture, nullptr, &view), "view"))
        return 4;
    event_case(device, context, view, "empty_no_flush", false, false, false);
    event_case(device, context, view, "clear_no_flush", true, false, false);
    event_case(device, context, view, "clear_allow_flush", true, true, false);
    event_case(device, context, view, "reuse_no_flush", true, false, true);
    context->OMSetRenderTargets(1, &view, nullptr);
    event_case(device, context, view, "bound_clear_no_flush", true, false, true);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    printf("DONE failures=%u reason=%08lx warp=%u\n", failures,
        static_cast<unsigned long>(device->GetDeviceRemovedReason()), warp);
    view->Release();
    texture->Release();
    context->Release();
    device->Release();

    if (adapter)
        adapter->Release();
    factory->Release();
    return failures ? 5 : 0;
}

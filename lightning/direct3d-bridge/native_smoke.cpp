/* Module: lightning.direct3d_bridge.native_smoke
 * Purpose: Validate a selected adapter through ordinary DXGI and Direct3D 11 APIs.
 * Created: 2026-10-09
 * Architecture: This application never calls private driver callbacks or falls
 * back to WARP. Pinned sibling DXVK DLLs translate ordinary application calls
 * through the genuine guest Venus ICD; system-runtime UMD acceptance stays separate.
 */
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include "native_shader_fixtures.h"
#include "graphics_adapter.h"

static const UINT IMAGE_WIDTH = 64;
static const UINT IMAGE_HEIGHT = 64;
static const UINT PIXEL_BYTES = 4;
static const UINT TRIANGLE_VERTICES = 3;
static const UINT LUID_HEX_DIGITS = sizeof(LUID) * 2;
static const UINT HEX_RADIX = 16;
static const UINT BITS_PER_HEX_DIGIT = 4;
static const UINT DWORD_BITS = sizeof(DWORD) * 8;
static const DWORD EXIT_INVALID_SELECTION = 2;
static const DWORD EXIT_FAILED_OPERATION = 3;
static const FLOAT CLEAR_COLOR[] = {0.0f, 0.0f, 1.0f, 1.0f};
static const BYTE DRAW_COLOR[] = {64, 128, 191, 255};
static const BYTE BACKGROUND_COLOR[] = {0, 0, 255, 255};

static void message(const char *text)
{
    DWORD length = 0;

    while (text[length]) {
        ++length;
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, nullptr);
}

#include "vulkan_probe.h"

static HMODULE bridge_dll(const WCHAR *name)
{
    WCHAR path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);

    if (!length || length >= MAX_PATH)
        ExitProcess(EXIT_INVALID_SELECTION);
    while (length && path[length - 1] != L'\\')
        --length;
    if (!length || lstrlenW(name) >= static_cast<int>(MAX_PATH - length))
        ExitProcess(EXIT_INVALID_SELECTION);
    lstrcpyW(path + length, name);
    HMODULE value = load_exact(path);

    if (!value) {
        message("FAIL: exact sibling DXVK DLL loading.\n");
        ExitProcess(EXIT_INVALID_SELECTION);
    }
    return value;
}

static void check(HRESULT result, const char *phase)
{
    if (SUCCEEDED(result)) {
        return;
    }
    const char digits[] = "0123456789abcdef";
    char code[DWORD_BITS / BITS_PER_HEX_DIGIT + 1] = {};

    for (UINT index = 0; index < DWORD_BITS / BITS_PER_HEX_DIGIT; ++index) {
        UINT shift = DWORD_BITS - BITS_PER_HEX_DIGIT * (index + 1);
        code[index] = digits[(static_cast<UINT>(result) >> shift) & (HEX_RADIX - 1)];
    }
    message("FAIL: ");
    message(phase);
    message(" HRESULT=0x");
    message(code);
    message("\n");
    ExitProcess(EXIT_FAILED_OPERATION);
}

/* An explicit LUID prevents a software renderer or unrelated physical adapter
 * from making an incomplete Lightning driver appear to pass this test. */
static LUID selected_luid()
{
    WCHAR text[LUID_HEX_DIGITS + 1] = {};
    DWORD length = GetEnvironmentVariableW(L"QLG_TEST_ADAPTER_LUID", text,
                                           static_cast<DWORD>(sizeof(text) / sizeof(text[0])));

    if (length != LUID_HEX_DIGITS) {
        message("Set QLG_TEST_ADAPTER_LUID to the selected DXGI adapter LUID as 16 hexadecimal "
                "digits.\n");
        ExitProcess(EXIT_INVALID_SELECTION);
    }
    UINT64 value = 0;

    for (UINT index = 0; index < LUID_HEX_DIGITS; ++index) {
        UINT digit = HEX_RADIX;

        if (text[index] >= L'0' && text[index] <= L'9') {
            digit = text[index] - L'0';
        } else if (text[index] >= L'a' && text[index] <= L'f') {
            digit = text[index] - L'a' + 10;
        } else if (text[index] >= L'A' && text[index] <= L'F') {
            digit = text[index] - L'A' + 10;
        }
        if (digit >= HEX_RADIX) {
            ExitProcess(EXIT_INVALID_SELECTION);
        }
        value = (value << BITS_PER_HEX_DIGIT) | digit;
    }
    LUID luid = {};
    luid.LowPart = static_cast<DWORD>(value);
    luid.HighPart = static_cast<LONG>(value >> DWORD_BITS);
    return luid;
}

extern "C" int _fltused = 0;
extern "C" void mainCRTStartup(void)
{
    LUID luid = selected_luid();
    UINT physical_vendor = 0, physical_device = 0;

    if (!bridge_vulkan_gate(luid, &physical_vendor, &physical_device))
        ExitProcess(EXIT_INVALID_SELECTION);
    WCHAR probe_only[2] = {};

    if (GetEnvironmentVariableW(L"QLG_TEST_PROBE_ONLY", probe_only, 2) == 1 &&
        probe_only[0] == L'1') {
        message("PASS: native physical compatibility probe completed.\n");
        ExitProcess(0);
    }
    HMODULE dxgi = bridge_dll(L"dxgi.dll"), d3d11 = bridge_dll(L"d3d11.dll");
    typedef HRESULT(WINAPI * FactoryCreate)(REFIID, void **);
    auto CreateDXGIFactory1 =
        reinterpret_cast<FactoryCreate>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    auto D3D11CreateDevice =
        reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"));

    if (!CreateDXGIFactory1 || !D3D11CreateDevice)
        ExitProcess(EXIT_INVALID_SELECTION);
    IDXGIFactory1 *factory = nullptr;
    check(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)),
          "CreateDXGIFactory1");
    IDXGIAdapter *adapter = nullptr;

    for (UINT index = 0;; ++index) {
        IDXGIAdapter1 *candidate = nullptr;
        HRESULT result = factory->EnumAdapters1(index, &candidate);

        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        check(result, "EnumAdapters");
        DXGI_ADAPTER_DESC1 description = {};
        check(candidate->GetDesc1(&description), "GetDesc1");
        if (description.AdapterLuid.HighPart == luid.HighPart &&
            description.AdapterLuid.LowPart == luid.LowPart) {
            if (description.VendorId != physical_vendor ||
                description.DeviceId != physical_device ||
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                message(
                    "Selected LUID does not identify the paired Lightning PCI hardware adapter.\n");
                ExitProcess(EXIT_INVALID_SELECTION);
            }
            adapter = candidate;
            break;
        }
        candidate->Release();
    }
    if (!adapter) {
        message("Selected adapter LUID is not present; no fallback was attempted.\n");
        ExitProcess(EXIT_INVALID_SELECTION);
    }
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_10_0;
    D3D_FEATURE_LEVEL actual = {};
    check(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            D3D11_CREATE_DEVICE_SINGLETHREADED, &requested, 1, D3D11_SDK_VERSION,
                            &device, &actual, &context),
          "D3D11CreateDevice(selected adapter, FL10_0)");
    if (actual != requested) {
        message("FAIL: unexpected Direct3D feature level.\n");
        ExitProcess(EXIT_FAILED_OPERATION);
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = IMAGE_WIDTH;
    desc.Height = IMAGE_HEIGHT;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *target = nullptr, *staging = nullptr;
    check(device->CreateTexture2D(&desc, nullptr, &target), "CreateTexture2D(default)");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc, nullptr, &staging), "CreateTexture2D(staging)");
    ID3D11RenderTargetView *view = nullptr;
    check(device->CreateRenderTargetView(target, nullptr, &view), "CreateRenderTargetView");
    ID3D11VertexShader *vertex = nullptr;
    ID3D11PixelShader *pixel = nullptr;
    check(device->CreateVertexShader(QLG_VERTEX_DXBC, sizeof(QLG_VERTEX_DXBC), nullptr, &vertex),
          "CreateVertexShader");
    check(device->CreatePixelShader(QLG_PIXEL_DXBC, sizeof(QLG_PIXEL_DXBC), nullptr, &pixel),
          "CreatePixelShader");
    D3D11_RASTERIZER_DESC raster_desc = {};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    ID3D11RasterizerState *raster = nullptr;
    check(device->CreateRasterizerState(&raster_desc, &raster), "CreateRasterizerState");
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<FLOAT>(IMAGE_WIDTH);
    viewport.Height = static_cast<FLOAT>(IMAGE_HEIGHT);
    viewport.MaxDepth = 1.0f;
    context->OMSetRenderTargets(1, &view, nullptr);
    context->RSSetState(raster);
    context->RSSetViewports(1, &viewport);
    context->VSSetShader(vertex, nullptr, 0);
    context->PSSetShader(pixel, nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->ClearRenderTargetView(view, CLEAR_COLOR);
    context->Draw(TRIANGLE_VERTICES, 0);
    context->CopyResource(staging, target);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    check(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped), "Map(staging)");
    const BYTE *bytes = static_cast<const BYTE *>(mapped.pData);

    if (!bytes || mapped.RowPitch < IMAGE_WIDTH * PIXEL_BYTES) {
        ExitProcess(EXIT_FAILED_OPERATION);
    }
    UINT verified = 0;

    for (UINT row = 0; row < IMAGE_HEIGHT; ++row) {
        FLOAT y = 1.0f - 2.0f * (row + 0.5f) / IMAGE_HEIGHT;

        for (UINT column = 0; column < IMAGE_WIDTH; ++column) {
            FLOAT x = 2.0f * (column + 0.5f) / IMAGE_WIDTH - 1.0f;
            FLOAT horizontal = x < 0 ? -x : x;
            FLOAT bottom = y + 0.75f, side = (0.75f - y) / 2.0f - horizontal;
            const FLOAT edge_margin = 3.0f / IMAGE_WIDTH;

            if ((bottom > -edge_margin && bottom < edge_margin) ||
                (side > -edge_margin && side < edge_margin))
                continue;
            const BYTE *expected = bottom > 0 && side > 0 ? DRAW_COLOR : BACKGROUND_COLOR;
            const BYTE *pixel = bytes + row * mapped.RowPitch + column * PIXEL_BYTES;

            for (UINT channel = 0; channel < PIXEL_BYTES; ++channel)
                if (pixel[channel] != expected[channel]) {
                    message(
                        "FAIL: selected bridge GPU returned incorrect shader/background pixels.\n");
                    ExitProcess(EXIT_FAILED_OPERATION);
                }
            ++verified;
        }
    }
    if (!verified)
        ExitProcess(EXIT_FAILED_OPERATION);
    context->Unmap(staging, 0);
    context->ClearState();
    context->Flush();
    raster->Release();
    pixel->Release();
    vertex->Release();
    view->Release();
    staging->Release();
    target->Release();
    context->Release();
    device->Release();
    adapter->Release();
    factory->Release();
    FreeLibrary(d3d11);
    FreeLibrary(dxgi);
    FreeLibrary(bridge_loader);
    message("PASS: native guest Vulkan-backed public D3D11CreateDevice, texture/RTV/shader/state "
            "creation, Draw, CopyResource and Map on the explicitly selected adapter.\n");
    ExitProcess(0);
}

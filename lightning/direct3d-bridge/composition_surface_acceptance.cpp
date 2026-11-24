// Module: lightning.direct3d_bridge.composition_surface_acceptance
// Purpose: Compare DirectComposition surface allocation and drawing with WARP.
// Created: 2026-10-15
// Architecture: The Windows runtime owns composition surfaces. D3D11 and
// Direct2D draws verify every pixel in the returned tile against fixed colors.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

static UINT failures;

static bool result(HRESULT hr, const char *phase)
{
    printf("%s %s=%08lx\n", FAILED(hr) ? "FAIL" : "PASS", phase, static_cast<unsigned long>(hr));
    failures += FAILED(hr);
    return SUCCEEDED(hr);
}

// Read only the test-owned surface tile, including any runtime atlas offset.
static void verify_pixels(ID3D11Device *device, ID3D11DeviceContext *context,
                          ID3D11Texture2D *texture, POINT offset,
                          UINT width, UINT height, DXGI_FORMAT format)
{
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);

    if (offset.x < 0 || offset.y < 0 || UINT(offset.x) > desc.Width ||
        UINT(offset.y) > desc.Height || width > desc.Width - UINT(offset.x) ||
        height > desc.Height - UINT(offset.y)) {
        result(E_INVALIDARG, "pixel_tile_bounds");
        return;
    }
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;

    if (result(device->CreateTexture2D(&desc, nullptr, &staging), "pixel_staging")) {
        const D3D11_BOX box = {UINT(offset.x), UINT(offset.y), 0,
            UINT(offset.x) + width, UINT(offset.y) + height, 1};
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped = {};

        if (result(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped), "pixel_readback")) {
            const bool floating = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            constexpr UINT channels = 4;
            const UINT pixel_bytes = channels * (floating ? sizeof(uint16_t) : sizeof(BYTE));

            if (!mapped.pData || mapped.RowPitch < width * pixel_bytes) {
                result(E_UNEXPECTED, "pixel_map_extent");
                context->Unmap(staging, 0);
                staging->Release();
                return;
            }
            UINT incorrect = 0;
            const UINT float_expected[] = {0x3400, 0x3800, 0x3a00, 0x3c00};
            const UINT rgba_expected[] = {64, 128, 191, 255};
            const UINT bgra_expected[] = {191, 128, 64, 255};
            const UINT *expected = floating ? float_expected :
                format == DXGI_FORMAT_B8G8R8A8_UNORM ? bgra_expected : rgba_expected;
            constexpr UINT unorm_tolerance = 1;

            for (UINT y = 0; y < height; ++y) {
                const BYTE *row = static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch;

                for (UINT x = 0; x < width; ++x) {
                    bool correct = true;

                    for (UINT channel = 0; channel < channels; ++channel) {
                        const UINT actual = floating ?
                            reinterpret_cast<const uint16_t *>(row)[x * channels + channel] :
                            row[x * channels + channel];
                        correct &= abs(int(actual) - int(expected[channel])) <=
                            int(floating ? 0 : unorm_tolerance);
                    }
                    incorrect += !correct;
                }
            }
            printf("PIXELS format=%u checked=%u incorrect=%u offset=%ld,%ld\n",
                format, width * height, incorrect, offset.x, offset.y);
            failures += incorrect != 0;
            context->Unmap(staging, 0);
        }
        staging->Release();
    }
}

static void draw_surface(ID3D11Device *device, ID3D11DeviceContext *context,
                         IDCompositionSurface *surface, DXGI_FORMAT format,
                         UINT width, UINT height, const char *phase)
{
    IDXGISurface *update = nullptr;
    POINT offset = {};

    if (!result(surface->BeginDraw(nullptr, __uuidof(IDXGISurface),
            reinterpret_cast<void **>(&update), &offset), phase))
        return;
    ID3D11Texture2D *texture = nullptr;
    ID3D11RenderTargetView *view = nullptr;

    if (result(update->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void **>(&texture)), "update_texture") &&
        result(device->CreateRenderTargetView(texture, nullptr, &view), "update_view")) {
        const char shader[] =
            "float4 VS(uint id:SV_VertexID):SV_POSITION{"
            "float2 p=float2((id<<1)&2,id&2);return float4(p*2-1,0,1);}"
            "float4 PS():SV_TARGET{return float4(.25,.5,.75,1);}";
        ID3DBlob *vs_code = nullptr, *ps_code = nullptr;
        ID3D11VertexShader *vs = nullptr;
        ID3D11PixelShader *ps = nullptr;
        ID3D11RasterizerState *raster = nullptr;
        D3D11_RASTERIZER_DESC raster_desc = {};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_NONE;
        raster_desc.DepthClipEnable = TRUE;
        const bool ready = result(D3DCompile(shader, sizeof(shader), nullptr, nullptr,
                nullptr, "VS", "vs_4_0", 0, 0, &vs_code, nullptr), "compile_vertex") &&
            result(D3DCompile(shader, sizeof(shader), nullptr, nullptr,
                nullptr, "PS", "ps_4_0", 0, 0, &ps_code, nullptr), "compile_pixel") &&
            result(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(),
                nullptr, &vs), "vertex_shader") &&
            result(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(),
                nullptr, &ps), "pixel_shader") &&
            result(device->CreateRasterizerState(&raster_desc, &raster), "rasterizer");

        if (ready) {
            // The offset belongs to the current atlas tile. Draw only inside
            // that tile rather than clearing unrelated pixels in the atlas.
            D3D11_VIEWPORT viewport = {FLOAT(offset.x), FLOAT(offset.y),
                FLOAT(width), FLOAT(height), 0, 1};
            context->OMSetRenderTargets(1, &view, nullptr);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vs, nullptr, 0);
            context->PSSetShader(ps, nullptr, 0);
            context->RSSetState(raster);
            context->RSSetViewports(1, &viewport);
            context->Draw(3, 0);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            verify_pixels(device, context, texture, offset, width, height, format);
            context->ClearState();
        }

        if (raster)
            raster->Release();

        if (ps)
            ps->Release();

        if (vs)
            vs->Release();

        if (ps_code)
            ps_code->Release();

        if (vs_code)
            vs_code->Release();
    }
    result(surface->EndDraw(), "end_draw");

    if (view)
        view->Release();

    if (texture)
        texture->Release();
    update->Release();
}

// DirectComposition owns BeginDraw/EndDraw for a returned Direct2D context.
// Flush before the explicit D3D readback; never begin a second D2D transaction.
static void draw_direct2d(ID3D11Device *device, ID3D11DeviceContext *context,
                          IDCompositionSurface *surface, DXGI_FORMAT format,
                          UINT width, UINT height)
{
    ID2D1DeviceContext *drawing = nullptr;
    POINT offset = {};

    if (!result(surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void **>(&drawing), &offset), "d2d_begin"))
        return;
    ID2D1SolidColorBrush *brush = nullptr;
    const D2D1_COLOR_F color = {.25f, .5f, .75f, 1.f};
    const D2D1_RECT_F rectangle = {FLOAT(offset.x), FLOAT(offset.y),
        FLOAT(offset.x) + width, FLOAT(offset.y) + height};
    const D2D1_MATRIX_3X2_F identity = {1, 0, 0, 1, 0, 0};
    drawing->SetDpi(96, 96);
    drawing->SetTransform(&identity);
    drawing->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    if (result(drawing->CreateSolidColorBrush(&color, nullptr, &brush), "d2d_brush")) {
        drawing->FillRectangle(&rectangle, brush);
        result(drawing->Flush(), "d2d_flush");
        ID2D1Image *image = nullptr;
        ID2D1Bitmap1 *bitmap = nullptr;
        IDXGISurface *dxgi_surface = nullptr;
        ID3D11Texture2D *texture = nullptr;
        drawing->GetTarget(&image);

        if (image && result(image->QueryInterface(__uuidof(ID2D1Bitmap1),
                reinterpret_cast<void **>(&bitmap)), "d2d_target_bitmap") &&
            result(bitmap->GetSurface(&dxgi_surface), "d2d_target_surface") &&
            result(dxgi_surface->QueryInterface(__uuidof(ID3D11Texture2D),
                reinterpret_cast<void **>(&texture)), "d2d_target_texture"))
            verify_pixels(device, context, texture, offset, width, height, format);

        if (!image)
            result(E_POINTER, "d2d_target_image");

        if (texture)
            texture->Release();

        if (dxgi_surface)
            dxgi_surface->Release();

        if (bitmap)
            bitmap->Release();

        if (image)
            image->Release();
        brush->Release();
    }
    drawing->Release();
    result(surface->EndDraw(), "d2d_end");
}

int main()
{
    const bool warp = strstr(GetCommandLineA(), "--warp") != nullptr;
    freopen(warp ? "C:\\Users\\Public\\QemuLightning-dcomp-warp.txt" :
                   "C:\\Users\\Public\\QemuLightning-dcomp-hardware.txt", "w", stdout);
    freopen(warp ? "C:\\Users\\Public\\QemuLightning-dcomp-warp-stderr.txt" :
                   "C:\\Users\\Public\\QemuLightning-dcomp-hardware-stderr.txt", "w", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    constexpr UINT lightning_vendor = 0x1234, lightning_device = 0x11f1;
    constexpr UINT width = 256, height = 192;
    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;

    if (!result(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
            reinterpret_cast<void **>(&factory)), "factory"))
        return 1;

    if (!warp) {
        for (UINT index = 0; factory->EnumAdapters1(index, &adapter) == S_OK; ++index) {
            DXGI_ADAPTER_DESC1 desc = {};
            adapter->GetDesc1(&desc);

            if (desc.VendorId == lightning_vendor && desc.DeviceId == lightning_device)
                break;
            adapter->Release();
            adapter = nullptr;
        }

        if (!adapter)
            return 2;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_10_1};

    if (!result(D3D11CreateDevice(adapter, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
            &device, nullptr, &context), "device"))
        return 3;
    IDXGIDevice *dxgi = nullptr;
    IDCompositionDevice *composition = nullptr;

    if (!result(device->QueryInterface(__uuidof(IDXGIDevice),
            reinterpret_cast<void **>(&dxgi)), "dxgi_device") ||
        !result(DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice),
            reinterpret_cast<void **>(&composition)), "composition_v1"))
        return 4;
    IDCompositionDesktopDevice *composition2 = nullptr;

    if (result(DCompositionCreateDevice2(dxgi, __uuidof(IDCompositionDesktopDevice),
            reinterpret_cast<void **>(&composition2)), "composition_v2")) {
        IDCompositionSurfaceFactory *surface_factory = nullptr;

        if (result(composition2->CreateSurfaceFactory(dxgi, &surface_factory), "surface_factory")) {
            IDCompositionSurface *surface = nullptr;

            if (result(surface_factory->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                    DXGI_ALPHA_MODE_PREMULTIPLIED, &surface), "factory_surface")) {
                draw_surface(device, context, surface, DXGI_FORMAT_B8G8R8A8_UNORM, width, height, "factory_begin_draw");
                surface->Release();
            }
            surface_factory->Release();
        }
        result(composition2->Commit(), "commit_v2");
        composition2->Release();
    }
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};

    for (DXGI_FORMAT format : formats) {
        IDCompositionSurface *surface = nullptr;
        printf("FORMAT %u warp=%u\n", format, warp);

        if (result(composition->CreateSurface(width, height, format,
                DXGI_ALPHA_MODE_PREMULTIPLIED, &surface), "create_surface_v1")) {
            draw_surface(device, context, surface, format, width, height, "begin_draw_v1");
            surface->Release();
        }
        IDCompositionVirtualSurface *virtual_surface = nullptr;

        if (result(composition->CreateVirtualSurface(width, height, format,
                DXGI_ALPHA_MODE_PREMULTIPLIED, &virtual_surface), "create_virtual_v1")) {
            draw_surface(device, context, virtual_surface, format, width, height, "begin_virtual_v1");
            result(virtual_surface->Resize(width * 2, height * 2), "resize_virtual");
            const RECT retained = {0,0,LONG(width),LONG(height)};
            result(virtual_surface->Trim(&retained, 1), "trim_virtual");
            virtual_surface->Release();
        }
    }
    ID2D1Factory1 *d2d_factory = nullptr;
    ID2D1Device *d2d_device = nullptr;
    IDCompositionDesktopDevice *d2d_composition = nullptr;
    const D2D1_FACTORY_OPTIONS options = {};

    if (result(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), &options,
            reinterpret_cast<void **>(&d2d_factory)), "d2d_factory") &&
        result(d2d_factory->CreateDevice(dxgi, &d2d_device), "d2d_device") &&
        result(DCompositionCreateDevice2(d2d_device, __uuidof(IDCompositionDesktopDevice),
            reinterpret_cast<void **>(&d2d_composition)), "d2d_composition")) {
        for (DXGI_FORMAT format : formats) {
            IDCompositionSurface *surface = nullptr;
            printf("D2D_FORMAT %u warp=%u\n", format, warp);

            if (result(d2d_composition->CreateSurface(width, height, format,
                    DXGI_ALPHA_MODE_PREMULTIPLIED, &surface), "d2d_surface")) {
                draw_direct2d(device, context, surface, format, width, height);
                surface->Release();
            }
        }
        result(d2d_composition->Commit(), "d2d_commit");
    }

    if (d2d_composition)
        d2d_composition->Release();

    if (d2d_device)
        d2d_device->Release();

    if (d2d_factory)
        d2d_factory->Release();
    result(composition->Commit(), "commit_v1");
    printf("DONE failures=%u reason=%08lx\n", failures,
        static_cast<unsigned long>(device->GetDeviceRemovedReason()));
    composition->Release();
    dxgi->Release();
    context->Release();
    device->Release();

    if (adapter)
        adapter->Release();
    factory->Release();
    return failures ? 5 : 0;
}

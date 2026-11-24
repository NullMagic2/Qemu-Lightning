// Module: lightning.direct3d_bridge.composition_visual_acceptance
// Purpose: Verify composed visual trees against explicit pixel fixtures.
// Created: 2026-10-15
// Architecture: Windows owns the visual tree and compositor. The fixture draws
// Direct2D surfaces and captures only its own window region after each commit.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static UINT failures;
static constexpr UINT canvas_width = 256, canvas_height = 192;
static constexpr UINT child_width = 64, child_height = 48;
static constexpr UINT lightning_vendor = 0x1234, lightning_device = 0x11f1;
static constexpr DWORD frame_settle_ms = 120;
static constexpr int color_tolerance = 2;

struct Layer {
    RECT rectangle;
    BYTE red, green, blue;
};

static bool result(HRESULT hr, const char *phase)
{
    printf("%s %s=%08lx\n", FAILED(hr) ? "FAIL" : "PASS", phase,
        static_cast<unsigned long>(hr));
    failures += FAILED(hr);
    return SUCCEEDED(hr);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// BeginDraw returns an atlas tile; only the requested rectangle is modified.
static IDCompositionSurface *make_surface(IDCompositionDevice *composition,
    UINT width, UINT height, const D2D1_COLOR_F &color)
{
    IDCompositionSurface *surface = nullptr;
    ID2D1DeviceContext *drawing = nullptr;
    ID2D1SolidColorBrush *brush = nullptr;
    POINT offset = {};

    if (!result(composition->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface), "create_surface"))
        return nullptr;

    if (result(surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void **>(&drawing), &offset), "surface_begin")) {
        const D2D1_MATRIX_3X2_F identity = {1, 0, 0, 1, 0, 0};
        const D2D1_RECT_F tile = {FLOAT(offset.x), FLOAT(offset.y),
            FLOAT(offset.x) + width, FLOAT(offset.y) + height};
        drawing->SetTransform(&identity);
        drawing->SetDpi(96, 96);
        drawing->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

        if (result(drawing->CreateSolidColorBrush(&color, nullptr, &brush), "surface_brush")) {
            drawing->FillRectangle(&tile, brush);
            brush->Release();
        }
        drawing->Release();
        result(surface->EndDraw(), "surface_end");
    }
    return surface;
}

// GDI reads only this fixture's client rectangle. No desktop image is exported.
static void check_scene(IDCompositionDevice *composition, HWND window,
                        const char *phase, const Layer *layers, UINT count)
{
    if (!result(composition->Commit(), "commit") ||
        !result(composition->WaitForCommitCompletion(), "commit_completion") ||
        !result(DwmFlush(), "dwm_flush"))
        return;
    MSG message;

    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Sleep(frame_settle_ms);
    POINT origin = {};
    ClientToScreen(window, &origin);
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = canvas_width;
    info.bmiHeader.biHeight = -LONG(canvas_height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    BYTE *pixels = nullptr;
    HBITMAP bitmap = screen ? CreateDIBSection(screen, &info, DIB_RGB_COLORS,
        reinterpret_cast<void **>(&pixels), nullptr, 0) : nullptr;
    HGDIOBJ previous = memory && bitmap ? SelectObject(memory, bitmap) : nullptr;
    const bool captured = previous && previous != HGDI_ERROR && pixels &&
        BitBlt(memory, 0, 0, canvas_width, canvas_height, screen,
            origin.x, origin.y, SRCCOPY | CAPTUREBLT);
    UINT incorrect = 0;

    if (captured) {
        GdiFlush();

        for (UINT y = 0; y < canvas_height; ++y) {
            for (UINT x = 0; x < canvas_width; ++x) {
                BYTE red = 0, green = 0, blue = 255;

                for (UINT index = 0; index < count; ++index) {
                    const Layer &layer = layers[index];

                    if (LONG(x) >= layer.rectangle.left && LONG(x) < layer.rectangle.right &&
                        LONG(y) >= layer.rectangle.top && LONG(y) < layer.rectangle.bottom) {
                        red = layer.red;
                        green = layer.green;
                        blue = layer.blue;
                    }
                }
                const BYTE *pixel = pixels + (y * canvas_width + x) * 4;
                const bool correct = abs(int(pixel[2]) - red) <= color_tolerance &&
                    abs(int(pixel[1]) - green) <= color_tolerance &&
                    abs(int(pixel[0]) - blue) <= color_tolerance;

                if (!correct && incorrect == 0)
                    printf("FIRST_PIXEL %s xy=%u,%u actual=%u,%u,%u expected=%u,%u,%u\n",
                        phase, x, y, pixel[2], pixel[1], pixel[0], red, green, blue);
                incorrect += !correct;
            }
        }
    }
    printf("%s SCENE %s checked=%u incorrect=%u captured=%u\n",
        captured && incorrect == 0 ? "PASS" : "FAIL", phase,
        captured ? canvas_width * canvas_height : 0, incorrect, captured);
    failures += !captured || incorrect != 0;

    if (previous && previous != HGDI_ERROR)
        SelectObject(memory, previous);

    if (bitmap)
        DeleteObject(bitmap);

    if (memory)
        DeleteDC(memory);

    if (screen)
        ReleaseDC(nullptr, screen);
}

int main()
{
    const bool warp = strstr(GetCommandLineA(), "--warp") != nullptr;
    freopen(warp ? "C:\\Users\\Public\\QemuLightning-visual-warp.txt" :
                   "C:\\Users\\Public\\QemuLightning-visual-hardware.txt", "w", stdout);
    freopen(warp ? "C:\\Users\\Public\\QemuLightning-visual-warp-stderr.txt" :
                   "C:\\Users\\Public\\QemuLightning-visual-hardware-stderr.txt", "w", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    SetProcessDPIAware();
    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;

    if (!result(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
            reinterpret_cast<void **>(&factory)), "dxgi_factory"))
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
    IDXGIDevice *dxgi = nullptr;
    ID2D1Factory1 *d2d_factory = nullptr;
    ID2D1Device *d2d = nullptr;
    IDCompositionDevice *composition = nullptr;
    const D2D1_FACTORY_OPTIONS options = {};

    if (!result(D3D11CreateDevice(adapter, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
            &device, nullptr, &context), "d3d_device") ||
        !result(device->QueryInterface(__uuidof(IDXGIDevice),
            reinterpret_cast<void **>(&dxgi)), "dxgi_device") ||
        !result(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
            &options, reinterpret_cast<void **>(&d2d_factory)), "d2d_factory") ||
        !result(d2d_factory->CreateDevice(dxgi, &d2d), "d2d_device") ||
        !result(DCompositionCreateDevice2(d2d, __uuidof(IDCompositionDevice),
            reinterpret_cast<void **>(&composition)), "composition_device"))
        return 3;
    const wchar_t window_class[] = L"QemuLightningCompositionVisualAcceptance";
    WNDCLASSW registration = {};
    registration.lpfnWndProc = window_proc;
    registration.hInstance = GetModuleHandleW(nullptr);
    registration.lpszClassName = window_class;

    if (!RegisterClassW(&registration))
        return 4;
    constexpr int window_left = 80, window_top = 80;
    HWND window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE, window_class, L"Qemu Lightning composition pixel test",
        WS_POPUP, window_left, window_top, canvas_width, canvas_height,
        nullptr, nullptr, registration.hInstance, nullptr);

    if (!window)
        return 5;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    IDCompositionTarget *target = nullptr;
    IDCompositionVisual *root = nullptr, *child = nullptr, *overlay = nullptr;
    const D2D1_COLOR_F blue = {0, 0, 1, 1}, red = {1, 0, 0, 1}, green = {0, 1, 0, 1};
    IDCompositionSurface *background = make_surface(composition, canvas_width, canvas_height, blue);
    IDCompositionSurface *foreground = make_surface(composition, child_width, child_height, red);
    constexpr UINT overlay_size = 32;
    IDCompositionSurface *cover = make_surface(composition, overlay_size, overlay_size, green);

    if (!background || !foreground || !cover ||
        !result(composition->CreateTargetForHwnd(window, TRUE, &target), "target") ||
        !result(composition->CreateVisual(&root), "root") ||
        !result(composition->CreateVisual(&child), "child") ||
        !result(composition->CreateVisual(&overlay), "overlay"))
        return 6;
    const Layer baseline[] = {{{32, 32, 96, 80}, 255, 0, 0}};
    const Layer moved[] = {{{64, 32, 128, 80}, 255, 0, 0}};
    const Layer scaled[] = {{{0, 0, 128, 96}, 255, 0, 0}};
    const Layer rotated[] = {{{8, 0, 56, 56}, 255, 0, 0}};
    const D2D_MATRIX_3X2_F matrix = {1, 0, 0, 1, 32, 32};
    const D2D_RECT_F clip = {16, 8, 48, 40};
    const Layer clipped[] = {{{48, 40, 80, 72}, 255, 0, 0}};
    const Layer blended[] = {{{32, 32, 96, 80}, 128, 0, 128}};
    const Layer above[] = {{{32, 32, 96, 80}, 255, 0, 0}, {{48, 48, 80, 80}, 0, 255, 0}};
    constexpr FLOAT scale_factor = 2.f, rotation_degrees = 90.f, opacity_half = .5f;
    result(root->SetContent(background), "background_content");
    result(child->SetContent(foreground), "child_content");
    result(child->SetOffsetX(FLOAT(baseline[0].rectangle.left)), "child_x");
    result(child->SetOffsetY(FLOAT(baseline[0].rectangle.top)), "child_y");
    result(root->AddVisual(child, TRUE, nullptr), "add_child");
    result(target->SetRoot(root), "set_root");
    check_scene(composition, window, "baseline", baseline, 1);
    result(child->SetOffsetX(FLOAT(moved[0].rectangle.left)), "move_x");
    check_scene(composition, window, "offset", moved, 1);
    result(child->SetOffsetX(0.f), "reset_x");
    result(child->SetOffsetY(0.f), "reset_y");
    IDCompositionTranslateTransform *translation = nullptr;

    if (result(composition->CreateTranslateTransform(&translation), "translation")) {
        result(translation->SetOffsetX(FLOAT(baseline[0].rectangle.left)), "translate_x");
        result(translation->SetOffsetY(FLOAT(baseline[0].rectangle.top)), "translate_y");
        result(child->SetTransform(translation), "set_translation");
        check_scene(composition, window, "translation", baseline, 1);
        translation->Release();
    }
    IDCompositionScaleTransform *scale = nullptr;

    if (result(composition->CreateScaleTransform(&scale), "scale")) {
        result(scale->SetScaleX(scale_factor), "scale_x");
        result(scale->SetScaleY(scale_factor), "scale_y");
        result(child->SetTransform(scale), "set_scale");
        check_scene(composition, window, "scale", scaled, 1);
        scale->Release();
    }
    IDCompositionRotateTransform *rotation = nullptr;

    if (result(composition->CreateRotateTransform(&rotation), "rotation")) {
        result(rotation->SetCenterX(FLOAT(child_width) / 2), "rotate_center_x");
        result(rotation->SetCenterY(FLOAT(child_height) / 2), "rotate_center_y");
        result(rotation->SetAngle(rotation_degrees), "rotate_angle");
        result(child->SetTransform(rotation), "set_rotation");
        check_scene(composition, window, "rotation", rotated, 1);
        rotation->Release();
    }
    result(child->SetTransform(matrix), "set_matrix");
    check_scene(composition, window, "matrix", baseline, 1);
    result(child->SetTransform(static_cast<IDCompositionTransform *>(nullptr)), "clear_transform");
    result(child->SetOffsetX(FLOAT(baseline[0].rectangle.left)), "restore_x");
    result(child->SetOffsetY(FLOAT(baseline[0].rectangle.top)), "restore_y");
    result(child->SetClip(clip), "set_clip");
    check_scene(composition, window, "rectangle_clip", clipped, 1);
    result(child->SetClip(static_cast<IDCompositionClip *>(nullptr)), "clear_clip");
    IDCompositionEffectGroup *opacity = nullptr;

    if (result(composition->CreateEffectGroup(&opacity), "opacity_group")) {
        result(opacity->SetOpacity(opacity_half), "opacity_half");
        result(child->SetEffect(opacity), "set_opacity");
        check_scene(composition, window, "opacity", blended, 1);
        result(child->SetEffect(nullptr), "clear_opacity");
        opacity->Release();
    }
    result(overlay->SetContent(cover), "overlay_content");
    result(overlay->SetOffsetX(FLOAT(above[1].rectangle.left)), "overlay_x");
    result(overlay->SetOffsetY(FLOAT(above[1].rectangle.top)), "overlay_y");
    result(root->AddVisual(overlay, TRUE, child), "overlay_above");
    check_scene(composition, window, "child_order_above", above, 2);
    result(root->RemoveVisual(overlay), "remove_overlay");
    result(root->AddVisual(overlay, FALSE, child), "overlay_below");
    check_scene(composition, window, "child_order_below", baseline, 1);
    result(root->RemoveAllVisuals(), "remove_all");
    check_scene(composition, window, "remove_children", nullptr, 0);
    result(root->AddVisual(child, TRUE, nullptr), "readd_child");
    result(child->SetContent(nullptr), "clear_content");
    check_scene(composition, window, "clear_content", nullptr, 0);
    result(child->SetContent(foreground), "restore_content");
    check_scene(composition, window, "restore_content", baseline, 1);
    DCOMPOSITION_FRAME_STATISTICS statistics = {};
    result(composition->GetFrameStatistics(&statistics), "frame_statistics");
    printf("DONE failures=%u reason=%08lx warp=%u\n", failures,
        static_cast<unsigned long>(device->GetDeviceRemovedReason()), warp);
    target->SetRoot(nullptr);
    composition->Commit();
    overlay->Release();
    child->Release();
    root->Release();
    target->Release();
    cover->Release();
    foreground->Release();
    background->Release();
    DestroyWindow(window);
    composition->Release();
    d2d->Release();
    d2d_factory->Release();
    dxgi->Release();
    context->Release();
    device->Release();

    if (adapter)
        adapter->Release();
    factory->Release();
    return failures ? 7 : 0;
}

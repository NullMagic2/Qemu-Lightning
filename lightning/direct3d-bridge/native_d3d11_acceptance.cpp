/* Module: lightning.direct3d_bridge.native_d3d11_acceptance
 * Purpose: Accept the system Direct3D 11 runtime path on the Lightning adapter.
 * Created: 2026-10-10
 * Architecture: Uses only the system dxgi.dll and d3d11.dll, so Windows loads
 * the UserModeDriverName registered by the installed Lightning INF. Selects the
 * adapter by PCI identity and never falls back to WARP, DXVK or another GPU.
 * Draws a triangle with precompiled DXBC from the Mesa d3d10umd tests, checks
 * exact pixels through a staging copy, then times windowed draw and Present.
 * --dynamic-buffer checks discard/NO_OVERWRITE appends before any readback wait.
 * --initial-buffer checks NO_OVERWRITE appends after a buffer's initial upload.
 * --index-discard checks index-buffer replacement between queued draws.
 * Prints one JSON object; any failure prints its phase and exits nonzero.
 * The --warp argument is a harness control only: it runs the same checks on
 * the Microsoft WARP software device to prove the program and its capture.
 */
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include "tri_vs_4_0.h"
#include "tri_ps_4_0.h"

extern "C" int _fltused = 0;
extern "C" int __cdecl _snprintf(char *buffer, size_t count, const char *format, ...);

static const UINT LIGHTNING_VENDOR_ID = 0x1234;
static const UINT LIGHTNING_DEVICE_ID = 0x11F1;
static const UINT IMAGE_WIDTH = 64;
static const UINT IMAGE_HEIGHT = 64;
static const UINT WINDOW_WIDTH = 256;
static const UINT WINDOW_HEIGHT = 256;
static const UINT PIXEL_BYTES = 4;
static const UINT TRIANGLE_VERTICES = 3;
static const UINT FRAME_COUNT = 300;
static const UINT DYNAMIC_BUFFER_BYTES = 1024 * 1024;
static const UINT DYNAMIC_APPENDS = 32;
static const UINT DYNAMIC_CYCLES = 4;
static const UINT INDEX_TILE_COLUMNS = 8;
static const UINT INDEX_TILE_ROWS = 8;
static const UINT INDEX_TILE_VERTICES = TRIANGLE_VERTICES * 2;
static const UINT SWAP_CHAIN_BUFFERS = 1;
static const UINT JSON_BYTES = 2048;
static const UINT TEXT_BYTES = 256;
static const DWORD EXIT_FAILED_OPERATION = 3;
static const DWORD EXIT_NO_ADAPTER = 2;
static const DWORD COMPLETION_TIMEOUT_MILLISECONDS = 10000;
static const FLOAT CLEAR_COLOR[] = {0.0f, 0.0f, 1.0f, 1.0f};
static const BYTE BACKGROUND_COLOR[] = {0, 0, 255, 255};
static const BYTE DRAW_COLOR[] = {64, 128, 192, 255};
static const FLOAT UNORM8_MAXIMUM = 255.0f;
/* Triangle extent in normalized device coordinates; the pixel model below is
 * derived from these vertices. */
static const FLOAT TRIANGLE_HALF_EXTENT = 0.5f;
/* Pixels this close to an edge (in NDC units, a few pixels wide) are skipped
 * because rasterization rules may legally assign them to either side. */
static const FLOAT EDGE_MARGIN = 3.0f / IMAGE_WIDTH;
static const FLOAT DEPTH = 0.5f;
static const FLOAT POSITION_W = 1.0f;
static const WCHAR WARP_ARGUMENT[] = L"--warp";
static const WCHAR DYNAMIC_ARGUMENT[] = L"--dynamic-buffer";
static const WCHAR INITIAL_BUFFER_ARGUMENT[] = L"--initial-buffer";
static const WCHAR INDEX_DISCARD_ARGUMENT[] = L"--index-discard";
/* --vsync requests a sync interval of one; status counters reveal occluded calls. */
static const WCHAR VSYNC_ARGUMENT[] = L"--vsync";
static const UINT VSYNC_INTERVAL = 1;
static const D3D_FEATURE_LEVEL REQUESTED_LEVELS[] = {D3D_FEATURE_LEVEL_10_1,
                                                     D3D_FEATURE_LEVEL_10_0};

/* Matches the POSITION and COLOR float4 inputs declared by tri_vs_4_0.h. */
struct Vertex {
    FLOAT position[4];
    FLOAT color[4];
};

static char json[JSON_BYTES];
static size_t json_length = 0;

static void append(const char *text)
{
    while (*text && json_length + 1 < JSON_BYTES)
        json[json_length++] = *text++;
    json[json_length] = 0;
}

static void emit_and_exit(DWORD code)
{
    append("}\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), json, static_cast<DWORD>(json_length), &written,
              nullptr);
    ExitProcess(code);
}

static void fail(const char *phase, HRESULT result, DWORD code = EXIT_FAILED_OPERATION)
{
    char text[TEXT_BYTES] = {};
    _snprintf(text, sizeof(text) - 1,
              "\"result\":\"fail\",\"phase\":\"%s\",\"hresult\":\"0x%08lx\"", phase,
              static_cast<unsigned long>(result));
    append(text);
    emit_and_exit(code);
}

/* Reports each completed phase on stderr so a hang inside the driver still
 * shows the last call that returned. */
static void check(HRESULT result, const char *phase)
{
    if (FAILED(result))
        fail(phase, result);
    char text[TEXT_BYTES] = {};
    int length = _snprintf(text, sizeof(text) - 1, "ok %s\n", phase);
    DWORD written = 0;

    if (length > 0)
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(length), &written,
                  nullptr);
}

/* Converts the adapter description to UTF-8 and drops JSON-significant characters. */
static void append_description(const WCHAR *description)
{
    char utf8[TEXT_BYTES] = {};
    WideCharToMultiByte(CP_UTF8, 0, description, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    for (char *cursor = utf8; *cursor; ++cursor) {
        if (*cursor == '"' || *cursor == '\\' || static_cast<unsigned char>(*cursor) < ' ')
            *cursor = ' ';
    }
    append(utf8);
}

static IDXGIAdapter1 *select_adapter(IDXGIFactory1 *factory)
{
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1 *candidate = nullptr;
        HRESULT result = factory->EnumAdapters1(index, &candidate);

        if (result == DXGI_ERROR_NOT_FOUND)
            return nullptr;
        check(result, "EnumAdapters1");
        DXGI_ADAPTER_DESC1 description = {};
        check(candidate->GetDesc1(&description), "GetDesc1");
        if (description.VendorId == LIGHTNING_VENDOR_ID &&
            description.DeviceId == LIGHTNING_DEVICE_ID &&
            !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            char text[TEXT_BYTES] = {};
            append("\"adapter\":\"");
            append_description(description.Description);
            _snprintf(text, sizeof(text) - 1, "\",\"adapter_index\":%u,\"luid\":\"%08lx%08lx\",",
                      index, static_cast<unsigned long>(description.AdapterLuid.HighPart),
                      static_cast<unsigned long>(description.AdapterLuid.LowPart));
            append(text);
            LARGE_INTEGER umd_version = {};

            if (SUCCEEDED(candidate->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd_version))) {
                _snprintf(text, sizeof(text) - 1, "\"umd_version\":\"%u.%u.%u.%u\",",
                          HIWORD(umd_version.HighPart), LOWORD(umd_version.HighPart),
                          HIWORD(umd_version.LowPart), LOWORD(umd_version.LowPart));
                append(text);
            }
            return candidate;
        }
        candidate->Release();
    }
}

static ID3D11Texture2D *create_texture(ID3D11Device *device, D3D11_USAGE usage, UINT bind,
                                       UINT access)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = IMAGE_WIDTH;
    desc.Height = IMAGE_HEIGHT;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = usage;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = access;
    ID3D11Texture2D *texture = nullptr;
    check(device->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D");
    return texture;
}

/* Binds the triangle pipeline: vertex buffer, input layout, shaders and state. */
static void triangle_vertices(Vertex (&output)[TRIANGLE_VERTICES])
{
    const FLOAT red = DRAW_COLOR[0] / UNORM8_MAXIMUM;
    const FLOAT green = DRAW_COLOR[1] / UNORM8_MAXIMUM;
    const FLOAT blue = DRAW_COLOR[2] / UNORM8_MAXIMUM;
    const FLOAT alpha = DRAW_COLOR[3] / UNORM8_MAXIMUM;
    const Vertex vertices[TRIANGLE_VERTICES] = {
        {{-TRIANGLE_HALF_EXTENT, -TRIANGLE_HALF_EXTENT, DEPTH, POSITION_W},
         {red, green, blue, alpha}},
        {{0.0f, TRIANGLE_HALF_EXTENT, DEPTH, POSITION_W}, {red, green, blue, alpha}},
        {{TRIANGLE_HALF_EXTENT, -TRIANGLE_HALF_EXTENT, DEPTH, POSITION_W},
         {red, green, blue, alpha}},
    };
    CopyMemory(output, vertices, sizeof(vertices));
}

static void bind_pipeline(ID3D11Device *device, ID3D11DeviceContext *context)
{
    Vertex vertices[TRIANGLE_VERTICES] = {};
    triangle_vertices(vertices);
    D3D11_BUFFER_DESC buffer_desc = {};
    buffer_desc.ByteWidth = sizeof(vertices);
    buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = vertices;
    ID3D11Buffer *buffer = nullptr;
    check(device->CreateBuffer(&buffer_desc, &initial, &buffer), "CreateBuffer(vertex)");
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, position),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, color),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout *layout = nullptr;
    check(device->CreateInputLayout(elements, ARRAYSIZE(elements), g_VS, sizeof(g_VS), &layout),
          "CreateInputLayout");
    ID3D11VertexShader *vertex = nullptr;
    ID3D11PixelShader *pixel = nullptr;
    check(device->CreateVertexShader(g_VS, sizeof(g_VS), nullptr, &vertex), "CreateVertexShader");
    check(device->CreatePixelShader(g_PS, sizeof(g_PS), nullptr, &pixel), "CreatePixelShader");
    D3D11_RASTERIZER_DESC raster_desc = {};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    ID3D11RasterizerState *raster = nullptr;
    check(device->CreateRasterizerState(&raster_desc, &raster), "CreateRasterizerState");
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
    context->IASetInputLayout(layout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex, nullptr, 0);
    context->PSSetShader(pixel, nullptr, 0);
    context->RSSetState(raster);
    /* The context holds its own references for the rest of the process. */
    buffer->Release();
    layout->Release();
    vertex->Release();
    pixel->Release();
    raster->Release();
}

static void set_viewport(ID3D11DeviceContext *context, UINT width, UINT height)
{
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<FLOAT>(width);
    viewport.Height = static_cast<FLOAT>(height);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
}

/* Compares every pixel away from the triangle edges with the analytic model. */
static UINT verify_pixels(const BYTE *bytes, UINT row_pitch)
{
    UINT verified = 0;

    for (UINT row = 0; row < IMAGE_HEIGHT; ++row) {
        FLOAT y = 1.0f - 2.0f * (row + 0.5f) / IMAGE_HEIGHT;

        for (UINT column = 0; column < IMAGE_WIDTH; ++column) {
            FLOAT x = 2.0f * (column + 0.5f) / IMAGE_WIDTH - 1.0f;
            FLOAT horizontal = x < 0 ? -x : x;
            FLOAT bottom = y + TRIANGLE_HALF_EXTENT;
            FLOAT side = (TRIANGLE_HALF_EXTENT - y) / 2.0f - horizontal;

            if ((bottom > -EDGE_MARGIN && bottom < EDGE_MARGIN) ||
                (side > -EDGE_MARGIN && side < EDGE_MARGIN))
                continue;
            const BYTE *expected = bottom > 0 && side > 0 ? DRAW_COLOR : BACKGROUND_COLOR;
            const BYTE *pixel = bytes + row * row_pitch + column * PIXEL_BYTES;

            for (UINT channel = 0; channel < PIXEL_BYTES; ++channel) {
                if (pixel[channel] != expected[channel]) {
                    char text[TEXT_BYTES] = {};
                    _snprintf(text, sizeof(text) - 1,
                              "\"pixel_mismatch\":{\"x\":%u,\"y\":%u,\"actual\":[%u,%u,%u,%u],"
                              "\"expected\":[%u,%u,%u,%u]},",
                              column, row, pixel[0], pixel[1], pixel[2], pixel[3], expected[0],
                              expected[1], expected[2], expected[3]);
                    append(text);
                    fail("verify pixels", E_FAIL);
                }
            }
            ++verified;
        }
    }
    return verified;
}

static void wait_for_gpu(ID3D11Device *device, ID3D11DeviceContext *context)
{
    D3D11_QUERY_DESC desc = {};
    desc.Query = D3D11_QUERY_EVENT;
    ID3D11Query *query = nullptr;
    check(device->CreateQuery(&desc, &query), "CreateQuery(event)");
    context->End(query);
    context->Flush();
    BOOL done = FALSE;
    ULONGLONG deadline = GetTickCount64() + COMPLETION_TIMEOUT_MILLISECONDS;

    while (context->GetData(query, &done, sizeof(done), 0) != S_OK || !done) {
        if (GetTickCount64() > deadline)
            fail("GPU completion timeout", E_FAIL);
        SwitchToThread();
    }
    query->Release();
}

static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static HWND create_window()
{
    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"QemuLightningD3d11Acceptance";
    if (!RegisterClassW(&window_class))
        fail("RegisterClass", HRESULT_FROM_WIN32(GetLastError()));
    RECT rect = {0, 0, static_cast<LONG>(WINDOW_WIDTH), static_cast<LONG>(WINDOW_HEIGHT)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Qemu Lightning D3D11 acceptance",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                  window_class.hInstance, nullptr);

    if (!window)
        fail("CreateWindow", HRESULT_FROM_WIN32(GetLastError()));
    return window;
}

static void pump_messages()
{
    MSG message = {};

    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

/* Times Present calls and records visibility status; call rate is not displayed FPS. */
static void present_loop(IDXGIFactory1 *factory, ID3D11Device *device, ID3D11DeviceContext *context)
{
    const UINT sync_interval = wcsstr(GetCommandLineW(), VSYNC_ARGUMENT) ? VSYNC_INTERVAL : 0;
    HWND window = create_window();
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = WINDOW_WIDTH;
    desc.BufferDesc.Height = WINDOW_HEIGHT;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = SWAP_CHAIN_BUFFERS;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *swap_chain = nullptr;
    check(factory->CreateSwapChain(device, &desc, &swap_chain), "CreateSwapChain");
    ID3D11Texture2D *back_buffer = nullptr;
    check(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                reinterpret_cast<void **>(&back_buffer)),
          "GetBuffer");
    ID3D11RenderTargetView *view = nullptr;
    check(device->CreateRenderTargetView(back_buffer, nullptr, &view),
          "CreateRenderTargetView(back buffer)");
    context->OMSetRenderTargets(1, &view, nullptr);
    set_viewport(context, WINDOW_WIDTH, WINDOW_HEIGHT);
    LARGE_INTEGER frequency = {}, start = {}, end = {};
    QueryPerformanceFrequency(&frequency);
    UINT presented_frames = 0, occluded_frames = 0, other_statuses = 0;
    QueryPerformanceCounter(&start);

    for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
        pump_messages();
        context->ClearRenderTargetView(view, CLEAR_COLOR);
        context->Draw(TRIANGLE_VERTICES, 0);
        HRESULT presented = swap_chain->Present(sync_interval, 0);

        if (presented == S_OK)
            ++presented_frames;
        else if (presented == DXGI_STATUS_OCCLUDED)
            ++occluded_frames;
        else
            ++other_statuses;

        /* Only the first frame reports progress so stderr stays readable. */
        if (frame == 0)
            check(presented, "Present(first frame)");
        else if (FAILED(presented))
            fail("Present", presented);
    }
    wait_for_gpu(device, context);
    QueryPerformanceCounter(&end);
    double seconds = static_cast<double>(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    char text[TEXT_BYTES] = {};
    _snprintf(text, sizeof(text) - 1,
              "\"sync_interval\":%u,\"frames\":%u,\"seconds\":%.3f,\"present_calls_per_second\":%.1f,", sync_interval,
              FRAME_COUNT, seconds, FRAME_COUNT / seconds);
    append(text);
    _snprintf(text, sizeof(text) - 1,
              "\"present_ok\":%u,\"present_occluded\":%u,\"present_other_status\":%u,",
              presented_frames, occluded_frames, other_statuses);
    append(text);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    view->Release();
    back_buffer->Release();
    swap_chain->Release();
    DestroyWindow(window);
}

/* Queue every append before reading pixels so a deferred buffer upload
 * cannot hide its overwrite of later NO_OVERWRITE data behind a readback wait. */
static void verify_dynamic_buffers(ID3D11Device *device, ID3D11DeviceContext *context,
                                   ID3D11Texture2D *target, ID3D11RenderTargetView *view,
                                   bool initialized)
{
    Vertex vertices[TRIANGLE_VERTICES] = {};
    triangle_vertices(vertices);
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = DYNAMIC_BUFFER_BYTES;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *buffer = nullptr;
    D3D11_SUBRESOURCE_DATA initial = {};

    if (initialized) {
        initial.pSysMem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DYNAMIC_BUFFER_BYTES);

        if (!initial.pSysMem)
            fail("allocate initial buffer data", E_OUTOFMEMORY);
    }
    check(device->CreateBuffer(&desc, initialized ? &initial : nullptr, &buffer),
          "CreateBuffer(dynamic)");

    if (initial.pSysMem)
        HeapFree(GetProcessHeap(), 0, const_cast<void *>(initial.pSysMem));
    UINT stride = sizeof(Vertex), offset = 0;
    context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
    ID3D11Texture2D *snapshots[DYNAMIC_APPENDS] = {};

    for (UINT index = 0; index < DYNAMIC_APPENDS; ++index)
        snapshots[index] = create_texture(device, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ);

    UINT verified = 0;

    for (UINT cycle = 0; cycle < DYNAMIC_CYCLES; ++cycle) {
        for (UINT index = 0; index < DYNAMIC_APPENDS; ++index) {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            const D3D11_MAP mode = initialized || index
                ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
            check(context->Map(buffer, 0, mode, 0, &mapped), "Map(dynamic)");
            CopyMemory(static_cast<BYTE *>(mapped.pData) + index * sizeof(vertices),
                       vertices, sizeof(vertices));
            context->Unmap(buffer, 0);
            context->ClearRenderTargetView(view, CLEAR_COLOR);
            context->Draw(TRIANGLE_VERTICES, index * TRIANGLE_VERTICES);
            context->CopyResource(snapshots[index], target);
        }

        for (UINT index = 0; index < DYNAMIC_APPENDS; ++index) {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            check(context->Map(snapshots[index], 0, D3D11_MAP_READ, 0, &mapped), "Map(dynamic snapshot)");
            verified += verify_pixels(static_cast<const BYTE *>(mapped.pData), mapped.RowPitch);
            context->Unmap(snapshots[index], 0);
        }
    }
    char text[TEXT_BYTES] = {};
    _snprintf(text, sizeof(text) - 1,
              "\"dynamic_initial_data\":%s,\"dynamic_buffer_bytes\":%u,"
              "\"dynamic_draws\":%u,\"verified_pixels\":%u,",
              initialized ? "true" : "false", DYNAMIC_BUFFER_BYTES,
              DYNAMIC_CYCLES * DYNAMIC_APPENDS, verified);
    append(text);

    for (UINT index = 0; index < DYNAMIC_APPENDS; ++index)
        snapshots[index]->Release();
    buffer->Release();
}

/* Queue distinct tiles while discarding one bound index buffer between draws.
 * No readback or explicit flush may hide a stale index-allocation binding. */
static void verify_index_discards(ID3D11Device *device, ID3D11DeviceContext *context,
                                  ID3D11Texture2D *target, ID3D11Texture2D *staging,
                                  ID3D11RenderTargetView *view)
{
    static const FLOAT corners[INDEX_TILE_VERTICES][2] = {
        {-1, -1}, {-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1},
    };
    static Vertex vertices[INDEX_TILE_COLUMNS * INDEX_TILE_ROWS * INDEX_TILE_VERTICES];

    for (UINT row = 0; row < INDEX_TILE_ROWS; ++row) {
        for (UINT column = 0; column < INDEX_TILE_COLUMNS; ++column) {
            for (UINT index = 0; index < INDEX_TILE_VERTICES; ++index) {
                Vertex &vertex = vertices[(row * INDEX_TILE_COLUMNS + column) * INDEX_TILE_VERTICES + index];
                vertex.position[0] = (corners[index][0] + 1 + static_cast<FLOAT>(2 * column)) / INDEX_TILE_COLUMNS - 1;
                vertex.position[1] = (corners[index][1] + 1 + static_cast<FLOAT>(2 * row)) / INDEX_TILE_ROWS - 1;
                vertex.position[2] = DEPTH;
                vertex.position[3] = POSITION_W;

                for (UINT channel = 0; channel < PIXEL_BYTES; ++channel)
                    vertex.color[channel] = DRAW_COLOR[channel] / UNORM8_MAXIMUM;
            }
        }
    }
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(vertices);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial = {vertices, 0, 0};
    ID3D11Buffer *vertex_buffer = nullptr, *index_buffer = nullptr;
    check(device->CreateBuffer(&desc, &initial, &vertex_buffer), "CreateBuffer(index tiles)");
    desc.ByteWidth = INDEX_TILE_VERTICES * sizeof(USHORT);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device->CreateBuffer(&desc, nullptr, &index_buffer), "CreateBuffer(discard indices)");
    const UINT stride = sizeof(Vertex), offset = 0;
    context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    context->IASetIndexBuffer(index_buffer, DXGI_FORMAT_R16_UINT, 0);
    context->ClearRenderTargetView(view, CLEAR_COLOR);

    for (UINT tile = 0; tile < INDEX_TILE_COLUMNS * INDEX_TILE_ROWS; ++tile) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        check(context->Map(index_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map(discard indices)");
        USHORT *indices = static_cast<USHORT *>(mapped.pData);

        for (UINT index = 0; index < INDEX_TILE_VERTICES; ++index)
            indices[index] = static_cast<USHORT>(tile * INDEX_TILE_VERTICES + index);
        context->Unmap(index_buffer, 0);
        context->DrawIndexed(INDEX_TILE_VERTICES, 0, 0);
    }
    context->CopyResource(staging, target);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    check(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped), "Map(index tiles)");

    if (!mapped.pData || mapped.RowPitch < IMAGE_WIDTH * PIXEL_BYTES)
        fail("Map(index tiles) layout", E_FAIL);

    UINT incorrect = 0;

    for (UINT y = 0; y < IMAGE_HEIGHT; ++y) {
        const BYTE *row = static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch;

        for (UINT x = 0; x < IMAGE_WIDTH; ++x) {
            for (UINT channel = 0; channel < PIXEL_BYTES; ++channel) {
                if (row[x * PIXEL_BYTES + channel] != DRAW_COLOR[channel]) {
                    ++incorrect;
                    break;
                }
            }
        }
    }
    context->Unmap(staging, 0);
    char text[TEXT_BYTES] = {};
    _snprintf(text, sizeof(text) - 1,
              "\"index_discard_draws\":%u,\"verified_pixels\":%u,\"incorrect_pixels\":%u,",
              INDEX_TILE_COLUMNS * INDEX_TILE_ROWS, IMAGE_WIDTH * IMAGE_HEIGHT, incorrect);
    append(text);

    if (incorrect)
        fail("index discard pixels", E_FAIL);
    vertex_buffer->Release();
    index_buffer->Release();
}

extern "C" void mainCRTStartup(void)
{
    append("{");
    IDXGIFactory1 *factory = nullptr;
    check(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)),
          "CreateDXGIFactory1");
    const bool warp = wcsstr(GetCommandLineW(), WARP_ARGUMENT) != nullptr;
    IDXGIAdapter1 *adapter = nullptr;

    if (warp) {
        append("\"adapter\":\"WARP control\",");
    } else {
        adapter = select_adapter(factory);

        if (!adapter)
            fail("no Lightning 1234:11F1 hardware adapter", DXGI_ERROR_NOT_FOUND, EXIT_NO_ADAPTER);
    }
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    D3D_FEATURE_LEVEL achieved = {};
    check(D3D11CreateDevice(adapter, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            0, REQUESTED_LEVELS, ARRAYSIZE(REQUESTED_LEVELS), D3D11_SDK_VERSION,
                            &device, &achieved, &context),
          "D3D11CreateDevice");
    char text[TEXT_BYTES] = {};
    _snprintf(text, sizeof(text) - 1, "\"feature_level\":\"%u_%u\",", (achieved >> 12) & 0xF,
              (achieved >> 8) & 0xF);
    append(text);
    ID3D11Texture2D *target =
        create_texture(device, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0);
    ID3D11Texture2D *staging =
        create_texture(device, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ);
    ID3D11RenderTargetView *view = nullptr;
    check(device->CreateRenderTargetView(target, nullptr, &view), "CreateRenderTargetView");
    bind_pipeline(device, context);
    set_viewport(context, IMAGE_WIDTH, IMAGE_HEIGHT);
    context->OMSetRenderTargets(1, &view, nullptr);
    const bool initialized = wcsstr(GetCommandLineW(), INITIAL_BUFFER_ARGUMENT) != nullptr;
    const bool dynamic = initialized || wcsstr(GetCommandLineW(), DYNAMIC_ARGUMENT) != nullptr;

    const bool index_discard = wcsstr(GetCommandLineW(), INDEX_DISCARD_ARGUMENT) != nullptr;

    if (index_discard) {
        verify_index_discards(device, context, target, staging, view);
    } else if (dynamic) {
        verify_dynamic_buffers(device, context, target, view, initialized);
    } else {
        context->ClearRenderTargetView(view, CLEAR_COLOR);
        context->Draw(TRIANGLE_VERTICES, 0);
        context->CopyResource(staging, target);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        check(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped), "Map(staging)");
        if (!mapped.pData || mapped.RowPitch < IMAGE_WIDTH * PIXEL_BYTES)
            fail("Map(staging) layout", E_FAIL);
        UINT verified = verify_pixels(static_cast<const BYTE *>(mapped.pData), mapped.RowPitch);
        context->Unmap(staging, 0);
        if (!verified)
            fail("verify pixels", E_FAIL);
        _snprintf(text, sizeof(text) - 1, "\"pixels\":\"pass\",\"verified_pixels\":%u,", verified);
        append(text);
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    view->Release();
    staging->Release();
    target->Release();
    if (!dynamic && !index_discard)
        present_loop(factory, device, context);
    context->ClearState();
    context->Flush();
    context->Release();
    device->Release();
    if (adapter)
        adapter->Release();
    factory->Release();
    append("\"result\":\"pass\"");
    emit_and_exit(0);
}

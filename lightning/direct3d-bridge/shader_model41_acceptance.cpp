// Module: lightning.direct3d_bridge.shader_model41_acceptance
// Purpose: Verify SM4.1 texture query and gather results against WARP.
// Created: 2026-10-16
// Architecture: Public D3D11 calls exercise the FL10.1 Windows driver ABI.
// Shader disassembly verifies each test actually emits its intended opcode.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>

using Microsoft::WRL::ComPtr;

static constexpr UINT image_side = 4;
static constexpr UINT multisample_count = 4;
static constexpr UINT color_channels = 4;
static constexpr UINT lightning_vendor = 0x1234, lightning_device = 0x11f1;
static constexpr DWORD readback_timeout_ms = 3000;
static constexpr float byte_tolerance = 1.1f;
using Pixel = std::array<float, color_channels>;

static void require(HRESULT result, const char *phase)
{
    if (FAILED(result)) {
        printf("FAIL phase=%s hr=%08lx\n", phase, static_cast<unsigned long>(result));
        throw result;
    }
}

static ComPtr<ID3DBlob> compile(const char *source, const char *profile,
                               const char *expected_opcode)
{
    ComPtr<ID3DBlob> code, errors, assembly;
    const HRESULT result = D3DCompile(source, strlen(source), nullptr, nullptr,
        nullptr, "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0,
        code.GetAddressOf(), errors.GetAddressOf());

    if (FAILED(result) && errors)
        printf("COMPILER %s\n", static_cast<const char *>(errors->GetBufferPointer()));
    require(result, "compile");

    if (expected_opcode) {
        require(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(),
            0, nullptr, assembly.GetAddressOf()), "disassemble");
        const bool found = strstr(static_cast<const char *>(assembly->GetBufferPointer()),
            expected_opcode) != nullptr;
        printf("BYTECODE profile=%s opcode=%s found=%u bytes=%zu\n", profile,
            expected_opcode, found, code->GetBufferSize());

        if (!found)
            throw E_FAIL;
    }
    return code;
}

enum class Input { Lod, Gather, Multisample, None };

struct Scenario {
    const char *name, *opcode, *source;
    Input input;
    bool multisample_target;
    std::array<Pixel, image_side> expected;
};

static constexpr Pixel lod_result = {0, .25f, 0, 1};
static constexpr Pixel gather_result = {.5f, .75f, .25f, 0};
static constexpr Pixel gather_offset_result = {.75f, .5f, 0, .25f};
static constexpr Pixel sample_count_result = {.5f, 0, 0, 1};
static constexpr std::array<Pixel, image_side> sample_positions = {{
    {.375f, .125f, 0, 1}, {.875f, .375f, 0, 1},
    {.125f, .625f, 0, 1}, {.625f, .875f, 0, 1},
}};

static const Scenario scenarios[] = {
    {"lod-clamped-unclamped", "lod ", R"(
        Texture2D<float> tex : register(t0);
        SamplerState state : register(s0);
        float4 main(float4 p : SV_Position) : SV_Target {
            float2 uv = p.xy * 0.5;
            return float4(tex.CalculateLevelOfDetail(state, uv) / 4.0,
                tex.CalculateLevelOfDetailUnclamped(state, uv) / 4.0, 0, 1);
        })", Input::Lod, false, {lod_result, lod_result, lod_result, lod_result}},
    {"gather-red-order", "gather4", R"(
        Texture2D<float> tex : register(t0);
        SamplerState state : register(s0);
        float4 main(float4 p : SV_Position) : SV_Target {
            return tex.Gather(state, float2(0.5, 0.5));
        })", Input::Gather, false,
        {gather_result, gather_result, gather_result, gather_result}},
    {"gather-texel-offset", "gather4", R"(
        Texture2D<float> tex : register(t0);
        SamplerState state : register(s0);
        float4 main(float4 p : SV_Position) : SV_Target {
            return tex.Gather(state, float2(0.5, 0.5), int2(1, 0));
        })", Input::Gather, false,
        {gather_offset_result, gather_offset_result, gather_offset_result, gather_offset_result}},
    {"resource-sample-info", "sample_info", R"(
        Texture2DMS<float4> tex : register(t0);
        float4 main(float4 p : SV_Position) : SV_Target {
            uint width, height, samples;
            tex.GetDimensions(width, height, samples);
            return float4(float(samples) / 8.0, 0, 0, 1);
        })", Input::Multisample, false,
        {sample_count_result, sample_count_result, sample_count_result, sample_count_result}},
    {"resource-sample-position", "sample_pos", R"(
        Texture2DMS<float4> tex : register(t0);
        float4 main(float4 p : SV_Position) : SV_Target {
            return float4(tex.GetSamplePosition(uint(p.x) & 3) + 0.5, 0, 1);
        })", Input::Multisample, false, sample_positions},
    {"rasterizer-sample-info", "sample_info", R"(
        float4 main(float4 p : SV_Position) : SV_Target {
            return float4(float(GetRenderTargetSampleCount()) / 8.0, 0, 0, 1);
        })", Input::None, true,
        {sample_count_result, sample_count_result, sample_count_result, sample_count_result}},
    {"rasterizer-sample-position", "sample_pos", R"(
        float4 main(float4 p : SV_Position) : SV_Target {
            return float4(GetRenderTargetSamplePosition(uint(p.x) & 3) + 0.5, 0, 1);
        })", Input::None, true, sample_positions},
};

static ComPtr<ID3D11ShaderResourceView> make_input(ID3D11Device *device, Input input)
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = image_side;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::array<float, image_side * image_side> lod_pixels = {};
    const float gather_pixels[] = {0, .25f, .5f, .75f};
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = lod_pixels.data();
    data.SysMemPitch = image_side * sizeof(float);
    const D3D11_SUBRESOURCE_DATA *initial = &data;

    if (input == Input::None)
        return view;

    if (input == Input::Gather) {
        constexpr UINT gather_side = 2;
        desc.Width = desc.Height = gather_side;
        data.pSysMem = gather_pixels;
        data.SysMemPitch = gather_side * sizeof(float);
    } else if (input == Input::Multisample) {
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = multisample_count;
        desc.SampleDesc.Quality = D3D11_STANDARD_MULTISAMPLE_PATTERN;
        initial = nullptr;
    }
    require(device->CreateTexture2D(&desc, initial, texture.GetAddressOf()), "input_texture");
    require(device->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf()), "input_view");
    return view;
}

static void render(ID3D11Device *device, ID3D11DeviceContext *context,
                   const Scenario &scenario, ID3DBlob *code)
{
    ComPtr<ID3D11PixelShader> shader;
    require(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
        nullptr, shader.GetAddressOf()), "pixel_shader");
    auto input = make_input(device, scenario.input);
    ID3D11ShaderResourceView *resource = input.Get();
    context->PSSetShaderResources(0, 1, &resource);
    context->PSSetShader(shader.Get(), nullptr, 0);
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = image_side;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = scenario.multisample_target ? multisample_count : 1;
    desc.SampleDesc.Quality = scenario.multisample_target ? D3D11_STANDARD_MULTISAMPLE_PATTERN : 0;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, resolved, staging;
    ComPtr<ID3D11RenderTargetView> view;
    require(device->CreateTexture2D(&desc, nullptr, target.GetAddressOf()), "target");
    require(device->CreateRenderTargetView(target.Get(), nullptr, view.GetAddressOf()), "target_view");
    ID3D11RenderTargetView *output = view.Get();
    context->OMSetRenderTargets(1, &output, nullptr);
    const float clear[] = {1, 0, 1, 1};
    context->ClearRenderTargetView(output, clear);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11Texture2D *read_source = target.Get();
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BindFlags = 0;

    if (scenario.multisample_target) {
        require(device->CreateTexture2D(&desc, nullptr, resolved.GetAddressOf()), "resolve_texture");
        context->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, desc.Format);
        read_source = resolved.Get();
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()), "staging");
    context->CopyResource(staging.Get(), read_source);
    context->Flush();
    const ULONGLONG deadline = GetTickCount64() + readback_timeout_ms;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT result;

    do {
        result = context->Map(staging.Get(), 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);

        if (result != DXGI_ERROR_WAS_STILL_DRAWING)
            break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    require(result, "map_readback");
    UINT mismatches = 0;

    if (!mapped.pData || mapped.RowPitch < image_side * color_channels) {
        context->Unmap(staging.Get(), 0);
        throw E_FAIL;
    }

    for (UINT y = 0; y < image_side; ++y) {
        const auto *row = static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch;

        for (UINT x = 0; x < image_side; ++x) {
            for (UINT channel = 0; channel < color_channels; ++channel) {
                const float expected = scenario.expected[x][channel] * 255;
                mismatches += fabsf(row[x * color_channels + channel] - expected) > byte_tolerance;
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    printf("PIXELS scenario=%s channels=%u mismatches=%u\n", scenario.name,
        image_side * image_side * color_channels, mismatches);

    if (mismatches)
        throw E_FAIL;
}

int main()
{
    const bool warp = strstr(GetCommandLineA(), "--warp") != nullptr;
    freopen(warp ? "C:\\Users\\Public\\QemuLightning-sm41-warp.txt" :
        "C:\\Users\\Public\\QemuLightning-sm41-hardware.txt", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);
    UINT failures = 0;

    try {
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter;
        require(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
            reinterpret_cast<void **>(factory.GetAddressOf())), "factory");

        if (!warp) {
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> candidate;

                if (factory->EnumAdapters1(index, candidate.GetAddressOf()) != S_OK)
                    break;
                DXGI_ADAPTER_DESC1 desc = {};
                require(candidate->GetDesc1(&desc), "adapter_desc");

                if (desc.VendorId == lightning_vendor && desc.DeviceId == lightning_device) {
                    adapter = candidate;
                    break;
                }
            }

            if (!adapter)
                throw DXGI_ERROR_NOT_FOUND;
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_10_1};
        require(D3D11CreateDevice(adapter.Get(), warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, 0, levels, 1, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
            context.GetAddressOf()), "device");
        wchar_t module_path[MAX_PATH] = {};
        HMODULE module = GetModuleHandleW(L"QemuLightningD3D10.dll");

        if (module && GetModuleFileNameW(module, module_path, MAX_PATH))
            printf("UMD path=%ls\n", module_path);
        const char *vertex_source = R"(
            float4 main(uint id : SV_VertexID) : SV_Position {
                float2 uv = float2((id << 1) & 2, id & 2);
                return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
            })";
        auto vertex_code = compile(vertex_source, "vs_4_0", nullptr);
        ComPtr<ID3D11VertexShader> vertex;
        require(device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(),
            nullptr, vertex.GetAddressOf()), "vertex_shader");
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D11_VIEWPORT viewport = {0, 0, float(image_side), float(image_side), 0, 1};
        context->RSSetViewports(1, &viewport);
        D3D11_RASTERIZER_DESC raster_desc = {};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_NONE;
        raster_desc.DepthClipEnable = TRUE;
        raster_desc.MultisampleEnable = TRUE;
        ComPtr<ID3D11RasterizerState> raster;
        require(device->CreateRasterizerState(&raster_desc, raster.GetAddressOf()), "rasterizer");
        context->RSSetState(raster.Get());
        D3D11_SAMPLER_DESC sampler_desc = {};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler_desc.MaxAnisotropy = 1;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        ComPtr<ID3D11SamplerState> sampler;
        require(device->CreateSamplerState(&sampler_desc, sampler.GetAddressOf()), "sampler");
        ID3D11SamplerState *sampler_pointer = sampler.Get();
        context->PSSetSamplers(0, 1, &sampler_pointer);

        for (const auto &scenario : scenarios) {
            printf("START scenario=%s warp=%u\n", scenario.name, warp);

            try {
                auto code = compile(scenario.source, "ps_4_1", scenario.opcode);
                render(device.Get(), context.Get(), scenario, code.Get());
                printf("PASS scenario=%s\n", scenario.name);
            } catch (HRESULT) {
                ++failures;
                printf("FAIL scenario=%s\n", scenario.name);
            }
        }
        context->ClearState();
        context->Flush();
    } catch (HRESULT) {
        ++failures;
    }
    printf("DONE failures=%u\n", failures);
    return failures ? 1 : 0;
}

/*
 * Module: lightning.direct3d12_umd.native_pixel_shaders
 * Purpose: Declare additive genuine R0 pixel shader object callbacks.
 * Created: 2026-10-24
 * Architecture: The original VS/CS bundle layout and admission remain unchanged.
 * Actual module/root ownership and private stage query are shared. This subset
 * does not advertise graphics pipeline, draw or public native D3D12 readiness.
 */
#pragma once
#include "native_shaders.h"

typedef struct QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALC_PRIVATE_SHADER_SIZE CalcPrivateShaderSize;
    PFND3D12DDI_CREATE_SHADER_0003 CreatePixelShader;
    PFND3D12DDI_DESTROYSHADER DestroyShader;
    HRESULT (APIENTRY *RetireShader)(D3D12DDI_HDEVICE, D3D12DDI_HSHADER);
    HRESULT (APIENTRY *QueryShaderInfo)(D3D12DDI_HDEVICE, D3D12DDI_HSHADER, UINT, QLDX12_NATIVE_SHADER_INFO *);
} QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS;

/* Original R0 signatures use genuine UNDEFINED/NONE system metadata for pixel
 * color output, never an invented SV_Target enum. Bounded supported code has no
 * inputs/resources and one full float4 output register0. Caller keeps tokens,
 * signatures and aligned disjoint runtime private storage valid through all
 * overlapping calls and successful retirement. Same-device empty root and
 * native runtime callback ownership remain retained through module lifetime.
 */
HRESULT ql_dx12_native_pixel_shader_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS *output);

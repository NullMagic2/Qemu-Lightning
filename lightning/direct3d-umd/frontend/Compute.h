/*
 * Module: d3d10umd.Compute (Qemu Lightning)
 * Purpose: D3D11 compute dispatch, direct and indirect.
 * Created: 2026-10-16
 * Architecture: The compute shader, its resources, samplers and constant
 *          buffers are bound by Shader.cpp (Cs* entry points) and its UAVs
 *          by UnorderedAccess.cpp. A dispatch binds the samplers paired with
 *          the shader's textures (BindStageResources in Draw.cpp) and
 *          launches a grid with the shader's thread group as the block.
 */

#ifndef COMPUTE_H
#define COMPUTE_H

#include "DriverIncludes.h"

void APIENTRY Dispatch(D3D10DDI_HDEVICE hDevice, UINT ThreadGroupCountX,
                       UINT ThreadGroupCountY, UINT ThreadGroupCountZ);

void APIENTRY DispatchIndirect(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hBuffer,
                               UINT AlignedByteOffset);

#endif /* COMPUTE_H */

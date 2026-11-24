/*
 * Module: d3d10umd.UnorderedAccess (Qemu Lightning)
 * Purpose: D3D11 unordered access views: creation, binding for the compute
 *          stage and the output merger, clears, append/consume counters and
 *          CopyStructureCount.
 * Created: 2026-10-16
 * Architecture: A UAV is a gallium image view. Raw and structured buffer
 *          views are R32_UINT buffer images, matching what ShaderNIR.c
 *          emits for ld_raw/store_raw and the structured forms. A view
 *          created with APPEND or COUNTER owns a persistent word in the shared
 *          device counter arena; index tables cover all 64 slots in every
 *          graphics stage and the separate compute namespace.
 *          Bindings are kept per bind point in the Device and pushed to the
 *          pipe whole whenever one changes. 3D views keep a full native mip;
 *          private CB15 W ranges drive NIR bounds/address/query translation,
 *          while clears use exact depth boxes.
 */

#ifndef UNORDERED_ACCESS_H
#define UNORDERED_ACCESS_H

#include "DriverIncludes.h"

struct Device;

/* Bracket commands using UAVs with GPU dependencies, including their counter
 * buffers. No bound UAV means no additional barrier. Failure is explicit. */
HRESULT OrderUnorderedAccess(Device *device, mesa_shader_stage stage);

SIZE_T APIENTRY CalcPrivateUnorderedAccessViewSize(
    D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *pCreate);

void APIENTRY CreateUnorderedAccessView(D3D10DDI_HDEVICE hDevice,
                                        const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *pCreate,
                                        D3D11DDI_HUNORDEREDACCESSVIEW hView,
                                        D3D11DDI_HRTUNORDEREDACCESSVIEW hRTView);

void APIENTRY DestroyUnorderedAccessView(D3D10DDI_HDEVICE hDevice,
                                         D3D11DDI_HUNORDEREDACCESSVIEW hView);

void APIENTRY ClearUnorderedAccessViewUint(D3D10DDI_HDEVICE hDevice,
                                           D3D11DDI_HUNORDEREDACCESSVIEW hView,
                                           const UINT Values[4]);

/* D3D11.1 ClearView on a UAV, optionally limited to rectangles. */
void ClearUnorderedAccessViewRects(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hView,
                                   const FLOAT Values[4], const D3D10_DDI_RECT *pRects, UINT NumRects);

void APIENTRY ClearUnorderedAccessViewFloat(D3D10DDI_HDEVICE hDevice,
                                            D3D11DDI_HUNORDEREDACCESSVIEW hView,
                                            const FLOAT Values[4]);

void APIENTRY CsSetUnorderedAccessViews(D3D10DDI_HDEVICE hDevice, UINT StartSlot,
                                        UINT NumViews,
                                        const D3D11DDI_HUNORDEREDACCESSVIEW *phViews,
                                        const UINT *pInitialCounts);

void APIENTRY CopyStructureCount(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDstBuffer,
                                 UINT DstAlignedByteOffset,
                                 D3D11DDI_HUNORDEREDACCESSVIEW hSrcView);

/* The UAV part of the D3D11 SetRenderTargets: slots in the updated range
 * that the call does not set are unbound. */
void SetOutputMergerUnorderedAccessViews(D3D10DDI_HDEVICE hDevice,
                                         const D3D11DDI_HUNORDEREDACCESSVIEW *phViews,
                                         const UINT *pInitialCounts, UINT StartSlot,
                                         UINT NumViews, UINT RangeStart, UINT RangeSize);

/* Unbind every UAV and counter (device destruction). */
void UnbindUnorderedAccessViews(struct Device *pDevice);

#endif /* UNORDERED_ACCESS_H */

/*
 * Module: d3d10umd.Wddm (Qemu Lightning)
 * Purpose: WDDM allocation and present entry points used by the frontend.
 * Created: 2026-10-10
 * Architecture: See Wddm.cpp.
 */

#pragma once

#include "DriverIncludes.h"

#include "pipe/p_defines.h"
#include "util/format/u_formats.h"

struct Device;
struct Resource;

/* Reject unsupported shared-image requests before creating native ownership. */
HRESULT WddmValidateSharedResource(struct pipe_screen *screen,
                                   const D3D10DDIARG_CREATERESOURCE *create);
unsigned WddmSharedBind(struct pipe_screen *screen, const D3D10DDIARG_CREATERESOURCE *pCreateResource,
                        enum pipe_format format, enum pipe_texture_target target, unsigned bind);
HRESULT WddmAllocate(Device *pDevice, Resource *pResource,
                     const D3D10DDIARG_CREATERESOURCE *pCreateResource,
                     D3D10DDI_HRTRESOURCE hRTResource);
void WddmDeallocate(Device *pDevice, Resource *pResource);
HRESULT WddmPresent(Device *pDevice, DXGI_DDI_ARG_PRESENT *pPresentData);
void WddmDestroyContext(Device *pDevice);
HRESULT WddmFlush(Device *pDevice);
HRESULT APIENTRY WddmResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *pResolve);
HRESULT WddmOpen(Device *pDevice, Resource *pResource,
                 const D3D10DDIARG_OPENRESOURCE *pOpenResource,
                 D3D10DDI_HRTRESOURCE hRTResource);

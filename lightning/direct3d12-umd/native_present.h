/*
 * Module: lightning.direct3d12_umd.native_present
 * Purpose: Declare the private native-image presentation entry points.
 * Created: 2026-11-17
 * Architecture: Owned native resource cookies feed the selected Vulkan queue
 * and the guest ICD's Win32 WSI. Public D3D12/DXGI tables do not use this ABI.
 */
#pragma once
#include "native_objects.h"

/* The caller must complete rendering and keep the same-process HWND live for
 * the duration of its presentation use. Sources are single-layer Texture1D or Texture2D. Each window owns its swapchain until
 * retired. Interval 0 requests immediate mode and 1 requests FIFO. A busy
 * pending frame must be retried with the same source generation, HWND and
 * interval, or retired. S_OK means the copied frame was accepted by WSI. */
typedef HRESULT (APIENTRY *QLDX12_PRESENT_NATIVE_IMAGE)(
    D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE, HWND, UINT);
typedef HRESULT (APIENTRY *QLDX12_RETIRE_NATIVE_PRESENTATION)(D3D12DDI_HDEVICE);

/* Present through one exact DIRECT queue. The target retains this queue's
 * generation until retirement. The call returns busy while earlier queue
 * operations are outstanding. A pending frame keeps its queue reservation. */
typedef HRESULT (APIENTRY *QLDX12_PRESENT_NATIVE_IMAGE_ON_QUEUE)(
    D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE, D3D12DDI_HRESOURCE, HWND, UINT);
typedef HRESULT (APIENTRY *QLDX12_RETIRE_NATIVE_PRESENTATION_WINDOW)(D3D12DDI_HDEVICE, HWND);

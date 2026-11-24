/*
 * Module: lightning.direct3d12_umd.native_image_readback_commands
 * Purpose: Expose a bounded genuine original R0 full-image readback transaction.
 * Created: 2026-10-25
 * Architecture: Genuine DDI fields stay in C; Rust owns local sequence and image
 * leases. Actual Vulkan barriers execute only a closed net-render-target pair.
 */
#pragma once
#include "native_commands.h"

/* One explicit RT->COPY_SOURCE barrier, one full-image CopyTextureRegion and
 * the explicit reverse barrier on one initialized same-device image. Source is
 * SELECT_SUBRESOURCE chooses an array layer or the full 3D mip; destination is physical RGBA8, BGRA8, RGB10A2, RGBA16_FLOAT or RGBA16_UNORM matching the image format and extent, zero XYZ, with
 * 512-byte placement and 256-byte row alignment, exact SlicePitch=Pitch*64.
 * Logical buffer bounds include the final row texels, excluding its unused
 * trailing pitch padding. No source box, conversion, arbitrary state stream or public readiness.
 * READBACK Map/retirement remain BUSY until every recording/pending owner has
 * released the exact resource, including completed unreset list references. */
#define QLDX12_NC_IMAGE_READBACK_BUNDLE_VERSION 1u
typedef struct QLDX12_NATIVE_IMAGE_READBACK_COMMAND_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALCPRIVATECOMMANDQUEUESIZE_0001 CalcPrivateCommandQueueSize;
    PFND3D12DDI_CREATECOMMANDQUEUE_0001 CreateCommandQueue;
    PFND3D12DDI_DESTROYCOMMANDQUEUE DestroyCommandQueue;
    PFND3D12DDI_CALC_PRIVATE_COMMAND_LIST_SIZE_0001 CalcPrivateCommandListSize;
    PFND3D12DDI_CREATE_COMMAND_LIST_0001 CreateCommandList;
    PFND3D12DDI_DESTROYCOMMANDLIST DestroyCommandList;
    PFND3D12DDI_RESOURCEBARRIER_0003 ResourceBarrier;
    PFND3D12DDI_COPYTEXTUREREGION_0003 CopyTextureRegion;
    PFND3D12DDI_CLOSECOMMANDLIST CloseCommandList;
    PFND3D12DDI_EXECUTECOMMANDLISTS ExecuteCommandLists;
    PFND3D12DDI_RESETCOMMANDLIST ResetCommandList;
    HRESULT (APIENTRY *PollCompletion)(D3D12DDI_HCOMMANDQUEUE, UINT, QLDX12_NC_COMPLETION *);
    HRESULT (APIENTRY *Drain)(D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandQueue)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandList)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST);
    HRESULT (APIENTRY *QueryRecording)(D3D12DDI_HCOMMANDLIST, UINT, QLDX12_NC_RECORDING_INFO *);
} QLDX12_NATIVE_IMAGE_READBACK_COMMAND_FUNCTIONS;
HRESULT ql_dx12_native_image_readback_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_IMAGE_READBACK_COMMAND_FUNCTIONS *output);
HRESULT ql_dx12_native_image_readback_list_create(uintptr_t device, uint64_t cookie,
    uintptr_t storage, uintptr_t allocator, uint64_t allocator_cookie, uint64_t *output);
HRESULT ql_dx12_native_image_readback_record(uintptr_t storage, uint64_t cookie,
    uintptr_t source, uint64_t source_cookie, uintptr_t destination, uint64_t destination_cookie,
    const QLDX12_NC_IMAGE_READBACK_FOOTPRINT *footprint);

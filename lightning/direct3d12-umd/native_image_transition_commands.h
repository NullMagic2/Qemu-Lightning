/*
 * Module: lightning.direct3d12_umd.native_image_transition_commands
 * Purpose: Expose a bounded genuine original R0 image transition transaction.
 * Created: 2026-10-25
 * Architecture: Genuine DDI fields stay in C; Rust owns local sequence and image
 * leases. Actual Vulkan barriers execute only a closed net-render-target pair.
 */
#pragma once
#include "native_commands.h"

/* One initialized RGBA8, BGRA8, RGB10A2, RGBA16_FLOAT or RGBA16_UNORM image, exactly RT->COPY_SOURCE then
 * COPY_SOURCE->RT on the same layer or all layers. Split, UAV and aliasing barriers are outside this path.
 * This transition-only bundle supplies no copy; the separate readback bundle
 * inserts its validated copy between the same explicit transitions. Close
 * refuses an unmatched transition. Every actual
 * attempt retains image/allocator ownership until observed completion or safe
 * drain. No unknown submission is described as a successful state rollback.
 * This private bundle does not advertise public DDI device readiness. */
#define QLDX12_NC_IMAGE_TRANSITION_BUNDLE_VERSION 1u
typedef struct QLDX12_NATIVE_IMAGE_TRANSITION_COMMAND_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALCPRIVATECOMMANDQUEUESIZE_0001 CalcPrivateCommandQueueSize;
    PFND3D12DDI_CREATECOMMANDQUEUE_0001 CreateCommandQueue;
    PFND3D12DDI_DESTROYCOMMANDQUEUE DestroyCommandQueue;
    PFND3D12DDI_CALC_PRIVATE_COMMAND_LIST_SIZE_0001 CalcPrivateCommandListSize;
    PFND3D12DDI_CREATE_COMMAND_LIST_0001 CreateCommandList;
    PFND3D12DDI_DESTROYCOMMANDLIST DestroyCommandList;
    PFND3D12DDI_RESOURCEBARRIER_0003 ResourceBarrier;
    PFND3D12DDI_CLOSECOMMANDLIST CloseCommandList;
    PFND3D12DDI_EXECUTECOMMANDLISTS ExecuteCommandLists;
    PFND3D12DDI_RESETCOMMANDLIST ResetCommandList;
    HRESULT (APIENTRY *PollCompletion)(D3D12DDI_HCOMMANDQUEUE, UINT, QLDX12_NC_COMPLETION *);
    HRESULT (APIENTRY *Drain)(D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandQueue)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandList)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST);
    HRESULT (APIENTRY *QueryRecording)(D3D12DDI_HCOMMANDLIST, UINT, QLDX12_NC_RECORDING_INFO *);
} QLDX12_NATIVE_IMAGE_TRANSITION_COMMAND_FUNCTIONS;
HRESULT ql_dx12_native_image_transition_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_IMAGE_TRANSITION_COMMAND_FUNCTIONS *output);
HRESULT ql_dx12_native_image_transition_list_create(uintptr_t device, uint64_t cookie,
    uintptr_t storage, uintptr_t allocator, uint64_t allocator_cookie, uint64_t *output);
HRESULT ql_dx12_native_image_transition_record(uintptr_t storage, uint64_t cookie,
    uintptr_t resource, uint64_t resource_cookie, uint32_t subresource, uint32_t before, uint32_t after);

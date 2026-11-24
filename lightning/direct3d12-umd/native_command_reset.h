/*
 * Module: lightning.direct3d12_umd.native_command_reset
 * Purpose: Declare an additive experimental genuine R0 command-list reset bundle.
 * Created: 2026-10-24
 * Architecture: Existing COPY and COMPUTE bundles keep their exact byte layouts.
 * Reset snapshots the original R0 allocator/Slot/ID/flags descriptor; the newer
 * recorder-based reset ABI is not substituted. Rust owns recording lifetimes.
 */
#pragma once
#include "native_commands.h"

#define QLDX12_NATIVE_COMMAND_RESET_BUNDLE_VERSION 1u

typedef struct QLDX12_NATIVE_COMMAND_RESET_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_RESETCOMMANDLIST ResetCommandList;
    HRESULT (APIENTRY *QueryRecording)(D3D12DDI_HCOMMANDLIST, UINT, QLDX12_NC_RECORDING_INFO *);
} QLDX12_NATIVE_COMMAND_RESET_FUNCTIONS;

/* Caller owns valid immutable reset descriptors and live aligned runtime storage
 * through all overlapping calls. Slot0/NONE is the admitted experimental subset;
 * ID is opaque runtime metadata, never an object generation or submission target.
 * Rejected resets preserve the current recording and all live/GPU-held owners.
 */
HRESULT ql_dx12_native_command_reset_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_COMMAND_RESET_FUNCTIONS *output);
HRESULT ql_dx12_native_command_list_reset(uintptr_t list, uint64_t list_cookie,
    uintptr_t allocator, uint64_t allocator_cookie, UINT slot, UINT64 id);
HRESULT ql_dx12_native_command_recording(uintptr_t list, uint64_t list_cookie,
    UINT bytes, QLDX12_NC_RECORDING_INFO *output);

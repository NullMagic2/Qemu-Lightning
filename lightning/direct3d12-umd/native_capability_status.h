/* Module: lightning.direct3d12_umd.native_capability_status
 * Purpose: Expose explicit selected-backend and native implementation boundaries.
 * Created: 2026-11-19
 * Architecture: This versioned private snapshot never replaces official GetCaps.
 * Pipeline/tier zero means unavailable here; it is not a successful official enum.
 */
#pragma once
#include "native_objects.h"
#include "native_caps_contract.h"

typedef HRESULT (APIENTRY *QLDX12_QUERY_NATIVE_CAPABILITY_STATUS)(D3D12DDI_HADAPTER,
    UINT version, UINT bytes, QLDX12_NATIVE_CAPABILITY_STATUS *);
HRESULT ql_dx12_native_capability_status(uintptr_t adapter, UINT bytes,
    QLDX12_NATIVE_CAPABILITY_STATUS *output);

/*
 * Module: lightning.direct3d12_umd.native_wddm
 * Purpose: Keep adapter-owned WDDM callbacks and returned queue/allocation handles.
 * Created: 2026-11-18
 */
#pragma once
#include <windows.h>
#include <d3d9types.h>
#include <d3dumddi.h>
#include <stdint.h>
#include <d3d12umddi.h>

typedef struct QLDX12_WDDM_QUEUE_CALLBACKS {
    uintptr_t create_context;
    uintptr_t destroy_context;
    uintptr_t create_paging_queue;
    uintptr_t destroy_paging_queue;
} QLDX12_WDDM_QUEUE_CALLBACKS;

typedef struct QLDX12_WDDM_QUEUE_INFO {
    HANDLE context;
    D3DKMT_HANDLE paging_queue;
    D3DKMT_HANDLE paging_fence;
} QLDX12_WDDM_QUEUE_INFO;

/* Returned handles are borrowed until explicit retirement. A failed destroy
 * retains runtime queue ownership and must be retried before releasing it. */
typedef HRESULT (APIENTRY *QLDX12_PREPARE_NATIVE_PAGING_QUEUE)(D3D12DDI_HCOMMANDQUEUE,
    UINT bytes, QLDX12_WDDM_QUEUE_INFO *output);

HRESULT ql_dx12_wddm_context_create(const QLDX12_WDDM_QUEUE_CALLBACKS *callbacks,
    uintptr_t runtime_queue, uintptr_t *output);
HRESULT ql_dx12_wddm_paging_prepare(uintptr_t queue, QLDX12_WDDM_QUEUE_INFO *output);
HRESULT ql_dx12_wddm_queue_close(uintptr_t queue);

HRESULT APIENTRY ql_dx12_native_ddi_prepare_paging(D3D12DDI_HCOMMANDQUEUE queue,
    UINT bytes, QLDX12_WDDM_QUEUE_INFO *output);


typedef struct QLDX12_WDDM_RESOURCE_CALLBACKS {
    uintptr_t allocate;
    uintptr_t deallocate;
    uintptr_t register_buffers;
    uintptr_t create_paging;
    uintptr_t destroy_paging;
    uintptr_t map_gpuva;
    uintptr_t free_gpuva;
    uintptr_t make_resident;
    uintptr_t evict;
    uintptr_t offer;
    uintptr_t reclaim;
    uintptr_t deallocate2;
} QLDX12_WDDM_RESOURCE_CALLBACKS;

HRESULT ql_dx12_wddm_bind_image(const QLDX12_WDDM_RESOURCE_CALLBACKS *callbacks,
    uintptr_t runtime_device, uint32_t shared_id, uint64_t bytes, uintptr_t *output);
HRESULT ql_dx12_wddm_bind_buffer(const QLDX12_WDDM_RESOURCE_CALLBACKS *callbacks,
    uintptr_t runtime_device, uint32_t shared_id, uint64_t bytes, uint64_t logical_bytes, uint32_t writable, uintptr_t *output);
HRESULT ql_dx12_wddm_unbind_image(uintptr_t owner);
D3DKMT_HANDLE ql_dx12_wddm_image_allocation(uintptr_t owner);
uint64_t ql_dx12_wddm_resource_gpuva(uintptr_t owner);

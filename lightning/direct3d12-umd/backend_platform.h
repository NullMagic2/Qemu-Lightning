/*
 * Module: lightning.direct3d12_umd.backend_platform
 * Purpose: Separate Win32/WDK/Vulkan ABI operations from Rust ownership policy.
 * Created: 2026-10-24
 * Architecture: Opaque runtime/backend pointers are owned by the Rust registry.
 * The generated scalar snapshot is private; it does not replace any official ABI.
 */
#pragma once
#include <windows.h>
#include <d3d9types.h>
#include <d3dumddi.h>
#include <stdint.h>
#include "backend_contract.h"
#include "requirements_contract.h"
#include "device_contract.h"
#include "queue_contract.h"
#include "resource_contract.h"
#include "transfer_contract.h"
#include "native_caps_contract.h"
#include "graphics_adapter.h"

void ql_dx12_policy_lock(void);
void ql_dx12_policy_unlock(void);
uintptr_t ql_dx12_runtime_create(HANDLE adapter, PFND3DDDI_QUERYADAPTERINFOCB callback);
void ql_dx12_runtime_destroy(uintptr_t runtime);
HRESULT ql_dx12_backend_create(uintptr_t runtime, QLDX12_BACKEND_SNAPSHOT *snapshot,
    uintptr_t *backend, uint32_t *guest_maximum_dimension, QLDX12_EVIDENCE *evidence);
void ql_dx12_backend_destroy(uintptr_t backend);

uintptr_t ql_dx12_adapter_open(uintptr_t runtime, UINT native_r0);
UINT ql_dx12_adapter_native_r0_version(uintptr_t adapter);
HRESULT ql_dx12_adapter_bind_command_table(uintptr_t adapter, uintptr_t runtime_table,
    void *output, const void *functions, SIZE_T bytes);
unsigned char ql_dx12_adapter_live(uintptr_t token);
HRESULT ql_dx12_adapter_close(uintptr_t token);
HRESULT ql_dx12_native_memory_architecture(uintptr_t token);
HRESULT ql_dx12_native_shader_support(uintptr_t token, uint32_t *features, uint32_t *models);
HRESULT ql_dx12_backend_query(uintptr_t token, uint32_t bytes, QLDX12_BACKEND_SNAPSHOT *snapshot);
HRESULT ql_dx12_memory_budget_query(uintptr_t token, uint32_t bytes, QLDX12_MEMORY_BUDGET *budget);
unsigned char ql_dx12_validate_guest(const QLGA_ADAPTER_INFO *information,
    const QLGA_ADAPTER_IDENTITY *identity);
unsigned char ql_dx12_candidate_matches(uint32_t expected_low, uint32_t expected_high,
    uint32_t expected_mask, uint32_t candidate_low, uint32_t candidate_high,
    uint32_t candidate_mask, uint32_t luid_valid, uint32_t hardware, uint32_t prefix_valid);

unsigned char ql_dx12_backend_extension_read(uintptr_t backend, uint32_t index,
    uint8_t name[QLDX12_EXTENSION_NAME_BYTES], uint32_t *spec_version);
HRESULT ql_dx12_requirement_query(uintptr_t token, uint32_t bytes, uint32_t index,
    QLDX12_REQUIREMENT_RESULT *result);
HRESULT ql_dx12_extension_query(uintptr_t token, uint32_t bytes, uint32_t start,
    uint32_t *entries, QLDX12_BACKEND_EXTENSION *extensions);

unsigned char ql_dx12_backend_queue_candidate(uintptr_t backend, uint32_t index,
    QLDX12_QUEUE_CANDIDATE *candidate);
HRESULT ql_dx12_device_create(uintptr_t backend, uint32_t family, uint32_t shared_images, uintptr_t *device,
    uint32_t *enabled_extensions);
HRESULT ql_dx12_device_drain(uintptr_t device);
void ql_dx12_device_destroy(uintptr_t device);
HRESULT ql_dx12_device_initialize(uintptr_t token, uint32_t bytes, QLDX12_DEVICE_RESULT *result);

HRESULT ql_dx12_device_execute(uintptr_t device, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion);
HRESULT ql_dx12_queue_operation(uintptr_t token, const QLDX12_QUEUE_COMMAND *command,
    uint32_t bytes, QLDX12_QUEUE_REPORT *report);

HRESULT ql_dx12_device_buffer_prepare(uintptr_t device,uint64_t bytes,uint32_t kind,
    uintptr_t *owner,QLDX12_BUFFER_REQUIREMENTS *requirements);
unsigned char ql_dx12_device_memory_type(uintptr_t device,uint32_t index,QLDX12_MEMORY_CANDIDATE *candidate);
HRESULT ql_dx12_resource_create(uintptr_t adapter,const QLDX12_BUFFER_COMMAND *command,uint32_t bytes,QLDX12_BUFFER_RESULT *result);
HRESULT ql_dx12_resource_destroy(uintptr_t adapter,uint64_t token);
HRESULT ql_dx12_resource_copy(uintptr_t adapter,uint64_t token,uint64_t offset,uint32_t bytes,unsigned char write,void *data);

HRESULT ql_dx12_transfer_operation(uintptr_t adapter,const QLDX12_TRANSFER_COMMAND *command,
    uint32_t bytes,QLDX12_QUEUE_REPORT *report);
HRESULT ql_dx12_native_parent_retain(uintptr_t adapter);
HRESULT ql_dx12_native_parent_release(uintptr_t adapter);
HRESULT ql_dx12_native_pool_create(uintptr_t adapter,uintptr_t *owner);
HRESULT ql_dx12_native_parent_fault(uintptr_t adapter,HRESULT status);
HRESULT ql_dx12_native_parent_status(uintptr_t adapter);

/* Native-owned committed buffers share device/quotas but never private tokens.
 * Cleanup/unmap remains legal after shared terminal loss; new maps do not. */
HRESULT ql_dx12_native_buffer_create(uintptr_t adapter,uint32_t kind,uint64_t bytes,
    uint32_t required_properties,uint32_t logical_pool,QLDX12_BUFFER_RESULT *result);
HRESULT ql_dx12_native_buffer_destroy(uintptr_t adapter,uint64_t token);
HRESULT ql_dx12_native_buffer_map(uintptr_t adapter,uint64_t token,void **pointer);
HRESULT ql_dx12_native_buffer_unmap(uintptr_t adapter,uint64_t token);

HRESULT ql_dx12_device_empty_layout_create(uintptr_t device, uintptr_t *owner);
HRESULT ql_dx12_device_shader_create(uintptr_t device, const uint32_t *words,
    SIZE_T bytes, uintptr_t *owner);
HRESULT ql_dx12_native_layout_create(uintptr_t adapter, uintptr_t *owner);
HRESULT ql_dx12_native_shader_module_create(uintptr_t adapter, const uint32_t *words,
    SIZE_T bytes, uintptr_t *owner);

#include "native_transfer_contract.h"
HRESULT ql_dx12_device_native_transfer(uintptr_t device, uintptr_t allocator,
    uintptr_t source, uintptr_t destination, const QLDX12_TRANSFER_PLAN *transfer,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);
HRESULT ql_dx12_device_drain_pending(uintptr_t device, uint32_t *complete);
HRESULT ql_dx12_native_queue_prepare(uintptr_t adapter);
HRESULT ql_dx12_native_transfer_submit(uintptr_t adapter, uintptr_t allocator,
    const QLDX12_TRANSFER_COMMAND *command, QLDX12_NATIVE_TRANSFER_RESULT *result);
HRESULT ql_dx12_native_transfer_poll(uintptr_t adapter, QLDX12_NATIVE_TRANSFER_RESULT *result);
HRESULT ql_dx12_native_transfer_drain(uintptr_t adapter, QLDX12_NATIVE_TRANSFER_RESULT *result);

HRESULT ql_dx12_device_compute_pipeline_create(uintptr_t device, uintptr_t layout,
    uintptr_t module, uintptr_t *output);
HRESULT ql_dx12_native_compute_pipeline_create(uintptr_t adapter, uintptr_t layout,
    uintptr_t module, uintptr_t *output);

#include "native_command_contract.h"
HRESULT ql_dx12_device_native_compute(uintptr_t device,uintptr_t adapter, uintptr_t allocator,
    uint32_t count, const uintptr_t *pipelines, const QLDX12_NC_DISPATCH_ARGUMENTS *arguments, const QLDX12_NC_ROOT_CONSTANTS *constants,const QLDX12_NC_INDIRECT_WORK *indirect, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);
HRESULT ql_dx12_native_compute_submit(uintptr_t adapter, uintptr_t allocator,
    uint32_t count, const uintptr_t *pipelines, const QLDX12_NC_DISPATCH_ARGUMENTS *arguments, const QLDX12_NC_ROOT_CONSTANTS *constants,const QLDX12_NC_INDIRECT_WORK *indirect, QLDX12_NATIVE_TRANSFER_RESULT *result);

HRESULT ql_dx12_device_buffer_initialize(uintptr_t device, uintptr_t buffer,
    uint64_t bytes, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);
HRESULT ql_dx12_native_buffer_initialize(uintptr_t adapter, uint64_t token,
    QLDX12_NATIVE_TRANSFER_RESULT *result);
HRESULT ql_dx12_native_buffer_initialize_drain(uintptr_t adapter, uint64_t token,
    QLDX12_NATIVE_TRANSFER_RESULT *result);

#include "native_fixed_state_contract.h"
HRESULT ql_dx12_device_graphics_pipeline_create(uintptr_t device, uintptr_t layout,
    uintptr_t vertex, uintptr_t pixel, uint32_t render_target_format, uint32_t depth_format, uint32_t sample_count,
    const QLDX12_NATIVE_FIXED_PLAN *plan, const QLDX12_NATIVE_BLEND_PLAN *blend_plan, const QLDX12_NATIVE_INPUT_LAYOUT *input, uintptr_t *output);
HRESULT ql_dx12_native_graphics_pipeline_create(uintptr_t adapter, uintptr_t layout,
    uintptr_t vertex, uintptr_t pixel, uint32_t render_target_format, uint32_t depth_format, uint32_t sample_count,
    const QLDX12_NATIVE_FIXED_PLAN *plan, const QLDX12_NATIVE_BLEND_PLAN *blend_plan, const QLDX12_NATIVE_INPUT_LAYOUT *input, uintptr_t *output);


HRESULT ql_dx12_device_native_empty(uintptr_t device, uintptr_t allocator,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);
HRESULT ql_dx12_native_empty_submit(uintptr_t adapter, uintptr_t allocator, QLDX12_NATIVE_TRANSFER_RESULT *result);

HRESULT ql_dx12_device_native_direct(uintptr_t device, uintptr_t allocator,
    uintptr_t pipeline, const QLDX12_NC_DIRECT_STATE *state,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);
HRESULT ql_dx12_native_direct_submit(uintptr_t adapter, uintptr_t allocator,
    uintptr_t pipeline, const QLDX12_NC_DIRECT_STATE *state, QLDX12_NATIVE_TRANSFER_RESULT *result);

HRESULT ql_dx12_device_native_draw(uintptr_t device, uintptr_t allocator,
    uintptr_t pipeline, uintptr_t target, uintptr_t depth_target, const QLDX12_NC_DIRECT_STATE *state, const QLDX12_NC_DRAW_ARGUMENTS *arguments, const QLDX12_NC_BLEND_CONSTANTS *constants,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);
HRESULT ql_dx12_native_draw_submit(uintptr_t adapter, uintptr_t allocator,
    uintptr_t pipeline, uintptr_t target, uintptr_t depth_target, const QLDX12_NC_DIRECT_STATE *state, const QLDX12_NC_DRAW_ARGUMENTS *arguments, const QLDX12_NC_BLEND_CONSTANTS *constants, QLDX12_NATIVE_TRANSFER_RESULT *result);

#include "native_image_contract.h"
HRESULT ql_dx12_device_image_prepare(uintptr_t device, const QLDX12_NATIVE_IMAGE_REQUEST *request,
    uintptr_t *owner, QLDX12_BUFFER_REQUIREMENTS *requirements, uint64_t linear[3]);
HRESULT ql_dx12_device_image_initialize(uintptr_t device, uintptr_t owner,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

HRESULT ql_dx12_device_image_view_create(uintptr_t device, uintptr_t image_owner,
    uint32_t layer, uint32_t layer_count, uint32_t read_only_flags, uintptr_t *output);

HRESULT ql_dx12_device_native_render_target(uintptr_t device, uintptr_t allocator,
    uintptr_t binding_owner, uintptr_t clear_owner, const QLDX12_NC_RENDER_TARGET_WORK *work,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

HRESULT ql_dx12_device_native_image_transition(uintptr_t device, uintptr_t allocator,
    uintptr_t image_owner, uint32_t layer, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

HRESULT ql_dx12_device_native_image_readback(uintptr_t device, uintptr_t allocator,
    uintptr_t image_owner, uint32_t layer, uintptr_t buffer_owner,
    const QLDX12_NC_IMAGE_READBACK_FOOTPRINT *footprint, uint64_t active_bytes,
    const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

HRESULT ql_dx12_device_native_image_upload(uintptr_t device, uintptr_t allocator,
    uintptr_t image_owner, uint32_t layer, uintptr_t buffer_owner,
    const QLDX12_NC_IMAGE_UPLOAD_FOOTPRINT *footprint, uint64_t active_bytes,
    const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

uint32_t ql_dx12_backend_layer_output(uintptr_t backend);
uint32_t ql_dx12_device_layer_output(uintptr_t device);

HRESULT ql_dx12_device_native_image_copy(uintptr_t device,uintptr_t allocator,uintptr_t source,uintptr_t destination,
    const QLDX12_QUEUE_PLAN *plan,QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted);

HRESULT ql_dx12_device_native_buffer_prepare(uintptr_t device,uint64_t bytes,uint32_t kind,uintptr_t *owner,QLDX12_BUFFER_REQUIREMENTS *requirements);
HRESULT ql_dx12_device_native_buffer_prepare_shared(uintptr_t device,uint64_t bytes,uint32_t kind,uintptr_t *owner,QLDX12_BUFFER_REQUIREMENTS *requirements);

HRESULT ql_dx12_device_native_draw_indexed(uintptr_t device,uintptr_t allocator,uintptr_t pipeline,uintptr_t target,uintptr_t depth_target,uintptr_t buffer,
    const QLDX12_NC_DIRECT_STATE *state,const QLDX12_NC_INDEXED_WORK *indexed,const QLDX12_NC_BLEND_CONSTANTS *constants,
    const QLDX12_QUEUE_PLAN *plan,QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted);

HRESULT ql_dx12_device_native_graphics_stream(uintptr_t device,uintptr_t allocator,uint32_t count,const QLDX12_NC_GRAPHICS_WORK *work,uintptr_t adapter,
    const QLDX12_QUEUE_PLAN *plan,QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted);

HRESULT ql_dx12_device_vertex_format_query(uintptr_t device,uint32_t format,uint32_t *output);

#include "native_descriptor_contract.h"
HRESULT ql_dx12_device_sampler_create(uintptr_t device,const QLDX12_ND_SAMPLER_PLAN *plan,uintptr_t *output);
void ql_dx12_sampler_destroy(uintptr_t owner);

HRESULT ql_dx12_device_mrt_pipeline_create(uintptr_t,uintptr_t,uintptr_t,uintptr_t,UINT,UINT,const QLDX12_NATIVE_FIXED_PLAN *,const QLDX12_NATIVE_MRT_PLAN *,const QLDX12_NATIVE_INPUT_LAYOUT *,uintptr_t *);
HRESULT ql_dx12_native_mrt_pipeline_create(uintptr_t,uintptr_t,uintptr_t,uintptr_t,UINT,UINT,const QLDX12_NATIVE_FIXED_PLAN *,const QLDX12_NATIVE_MRT_PLAN *,const QLDX12_NATIVE_INPUT_LAYOUT *,uintptr_t *);

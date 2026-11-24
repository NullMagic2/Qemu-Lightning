/* Module: lightning.direct3d12_umd.backend_queue
 * Purpose: Own one private Vulkan queue timeline behind generated Rust policy.
 * Created: 2026-10-24
 * Architecture: Genuine Vulkan/Win32 types stay here. Rust serializes commands
 * and pins the device. No Vulkan handle, native D3D12 queue or OS fence escapes.
 */
#pragma once
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "queue_contract.h"
#include "transfer_contract.h"
#include "resource_contract.h"
#include "native_command_contract.h"
#include "native_transfer_contract.h"
#include "backend_image.h"
#include "native_image_contract.h"

/* Inputs must remain live until successful destroy. Resolve official loader
 * destruction before creating the VkDevice and pass that fallback here. Create
 * publishes only an initialized timeline with counter0; failures preserve output. */
HRESULT ql_dx12_queue_create(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get,
    PFN_vkDestroySemaphore destroy_fallback, uintptr_t *owner);
/* Failed drain retains every queue/semaphore reference for a close retry. Device
 * loss permits Vulkan cleanup; successful teardown does not imply work succeeded.
 * The caller must exclude operations and retain the parent VkDevice throughout. */
HRESULT ql_dx12_queue_destroy(uintptr_t owner);
/* The private plan and completion storage are valid/aligned/nonoverlapping.
 * The parent owns one exclusive operation lease. Completion is internal staged
 * output, including accepted signals whose subsequent counter probe failed.
 * Only the public Rust boundary decides whether to publish an outer report. */
HRESULT ql_dx12_queue_execute(uintptr_t owner, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion);

/* Real recorded copy, serialized by the parent device lease. attempted is set
 * immediately before vkQueueSubmit2; every such attempt retains resource refs. */
HRESULT ql_dx12_queue_transfer(uintptr_t owner,uint32_t family,VkBuffer source,VkBuffer destination,
    const QLDX12_TRANSFER_PLAN *transfer,const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted);

/* Successful idle or confirmed loss sets complete1; ordinary failure retains
 * pending command/resource references. This does not destroy the queue owner. */
HRESULT ql_dx12_queue_drain_pending(uintptr_t owner, uint32_t *complete);
/* The caller retains the actual native allocator pool until observed completion
 * or complete1 drain; no reset/destroy may race this borrowed command storage. */
HRESULT ql_dx12_queue_transfer_native(uintptr_t owner, uint32_t family,
    VkCommandPool pool, VkBuffer source, VkBuffer destination,
    const QLDX12_TRANSFER_PLAN *transfer, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

/* Exactly one resource-free workgroup; typed native policy retains compute PSO
 * and actual allocator pool. Completion/attempt facts share the COPY contract. */
typedef struct QLDX12_ROOT_CBV_STEP {VkBuffer buffer;VkDeviceSize offset,bytes;uint32_t kind;} QLDX12_ROOT_CBV_STEP;
typedef struct QLDX12_UNIFORM_STEP {VkBuffer buffer;VkDeviceSize offset,bytes;uint32_t binding;} QLDX12_UNIFORM_STEP;
typedef struct QLDX12_ROOT_CBV_BATCH {uint32_t count;QLDX12_ROOT_CBV_STEP entries[32];uint32_t uniform_count,uniform_bindings;VkDescriptorSetLayout uniform_layout;QLDX12_UNIFORM_STEP uniforms[64];} QLDX12_ROOT_CBV_BATCH;
HRESULT ql_dx12_queue_compute_native(uintptr_t owner, uint32_t family,
    VkCommandPool pool, uint32_t count, const VkPipeline *pipelines, const QLDX12_NC_DISPATCH_ARGUMENTS *arguments,const VkPipelineLayout *layouts,const QLDX12_NC_ROOT_CONSTANTS *constants,const QLDX12_ROOT_CBV_BATCH *cbvs,const VkBuffer *indirect_buffers,const QLDX12_NC_INDIRECT_WORK *indirect,const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

/* Fill the entire checked buffer span with zero using the shared construction
 * pool. Rust registry ownership and pins survive every actual submit attempt. */
HRESULT ql_dx12_queue_fill(uintptr_t owner, uint32_t family, VkBuffer buffer,
    VkDeviceSize bytes, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

/* Rust admits one finite viewport/scissor within the named 4096 Vulkan core floor
 * and retains this actual graphics pipeline/allocator until target observation
 * or explicit safe drain. No render-pass, attachment or draw command is issued. */
HRESULT ql_dx12_queue_empty_native(uintptr_t owner, uint32_t family, VkCommandPool pool,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

HRESULT ql_dx12_queue_direct_native(uintptr_t owner, uint32_t family,
    VkCommandPool pool, VkPipeline pipeline, const QLDX12_NC_DIRECT_STATE *state,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

/* Hidden registry-owned image only: no retry after any actual submission attempt.
 * Clear covers observable color texels, not opaque allocation padding/metadata. */
HRESULT ql_dx12_queue_image_initialize(uintptr_t owner, uint32_t family, VkImage image,
    uint32_t array_layers, VkImageAspectFlags aspect, const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

/* Independent OM and explicit clear targets; Rust admits one optional operation
 * of each kind and owns same-device initialized RTV references through retirement. */
HRESULT ql_dx12_queue_render_target_native(uintptr_t owner, uint32_t family,
    VkCommandPool pool, VkImage bound_image, VkImageView bound_view, VkExtent2D bound_extent, QLDX12_ATTACHMENT_RANGE bound_range,
    VkImage clear_image, VkImageView clear_view, VkExtent2D clear_extent, QLDX12_ATTACHMENT_RANGE clear_range, const QLDX12_NC_RENDER_TARGET_WORK *work,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

HRESULT ql_dx12_queue_image_transition_native(uintptr_t owner, uint32_t family,
    VkCommandPool pool, VkImage image, uint32_t layer, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

/* Caller supplies the immutable Rust-validated logical footprint and retains
 * both actual selected-device owners. The active_bytes range is computed by
 * Rust from the last accessed texel, excluding final-row pitch padding.
 * No caller pointer or image CPU layout
 * survives recording; the buffer pitch is independent of image allocation. */
HRESULT ql_dx12_queue_image_readback_native(uintptr_t owner, uint32_t family,
    VkCommandPool pool, VkImage image, uint32_t layer, VkBuffer buffer,
    const QLDX12_NC_IMAGE_READBACK_FOOTPRINT *footprint, uint64_t active_bytes,
    const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

HRESULT ql_dx12_queue_draw_native(uintptr_t owner, uint32_t family, VkCommandPool pool,
    VkPipeline pipeline, VkImage image, VkImageView view, VkExtent2D extent, QLDX12_ATTACHMENT_RANGE range, VkImage depth_image, VkImageView depth_view, QLDX12_ATTACHMENT_RANGE depth_range, const QLDX12_NC_DIRECT_STATE *state, const QLDX12_NC_DRAW_ARGUMENTS *arguments, const QLDX12_NC_BLEND_CONSTANTS *constants,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

HRESULT ql_dx12_queue_image_upload_native(uintptr_t owner, uint32_t family,
    VkCommandPool pool, VkImage image, uint32_t layer, VkBuffer buffer,
    const QLDX12_NC_IMAGE_UPLOAD_FOOTPRINT *footprint, uint64_t active_bytes,
    const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

HRESULT ql_dx12_queue_color_resolve(uintptr_t owner, uint32_t family, VkCommandPool pool,
    VkImage source, VkImage destination, VkExtent2D extent, uint32_t source_layer, uint32_t destination_layer,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

/* A native list records every admitted copy in one actual command buffer and
 * submits one real timeline target. All buffer/pool owners stay retained. */
HRESULT ql_dx12_queue_transfer_native_many(uintptr_t owner, uint32_t family,
    VkCommandPool pool, UINT count, const VkBuffer *sources, const VkBuffer *destinations,
    const QLDX12_TRANSFER_PLAN *transfers, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted);

HRESULT ql_dx12_queue_image_copy(uintptr_t owner,uint32_t family,VkCommandPool pool,VkImage source,VkImage destination,VkExtent3D extent,uint32_t layers,
    const QLDX12_QUEUE_PLAN *plan,QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted);

HRESULT ql_dx12_queue_draw_indexed_native(uintptr_t owner,uint32_t family,VkCommandPool pool,VkPipeline pipeline,
    VkImage image,VkImageView view,VkExtent2D extent,QLDX12_ATTACHMENT_RANGE range,VkImage depth_image,VkImageView depth_view,QLDX12_ATTACHMENT_RANGE depth_range,
    const QLDX12_NC_DIRECT_STATE *state,VkBuffer buffer,const QLDX12_NC_INDEXED_WORK *indexed,const QLDX12_NC_BLEND_CONSTANTS *constants,
    const QLDX12_QUEUE_PLAN *plan,QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted);

/* All steps are validated against one selected VkDevice before any recording.
 * The Rust node chain, not this borrowed scalar array, owns every operand. */
typedef struct QLDX12_GRAPHICS_STEP {
    VkQueryPool query_pool;
    VkBuffer indirect_buffer;
    uint32_t indirect_limit;
    VkBuffer query_destination;
    VkBuffer fill_destination;
    VkPipeline pipeline;
    VkPrimitiveTopology topology;
    VkPipelineLayout root_layout;
    QLDX12_ROOT_CBV_BATCH cbvs;
    VkImage image, depth_image;
    VkImageView view, depth_view;
    VkBuffer buffer;
    VkExtent2D extent;
    QLDX12_ATTACHMENT_RANGE range, depth_range;
    VkImage extra_images[7];
    VkImageView extra_views[7];
    QLDX12_ATTACHMENT_RANGE extra_ranges[7];
    QLDX12_NC_GRAPHICS_WORK work;
    uint32_t vertex_slots[32];
    VkBuffer vertex_buffers[32];
    VkDeviceSize vertex_offsets[32],vertex_sizes[32],vertex_strides[32];
} QLDX12_GRAPHICS_STEP;
HRESULT ql_dx12_queue_graphics_stream(uintptr_t owner,uint32_t family,VkCommandPool pool,uint32_t count,
    const QLDX12_GRAPHICS_STEP *steps,const QLDX12_QUEUE_PLAN *plan,QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted);

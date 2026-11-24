/* Module: lightning.direct3d12_umd.backend_queue
 * Purpose: Execute genuine copy/compute/direct state commands, fills and signals.
 * Created: 2026-10-24
 * Architecture: Thin official Vulkan calls surround Rust command policy. The
 * device owner serializes operations and close; this module never invokes the
 * adapter registry or exports handles. Destruction drains before releasing refs.
 */
#include "backend_queue.h"

extern uint32_t ql_dx12_native_color_texel_bytes(uint32_t format);

_Static_assert(VK_HEADER_VERSION == 335, "pinned Vulkan-Headers v1.4.335");

struct queue_owner {
    VkDevice device;
    VkQueue queue;
    VkSemaphore timeline;
    PFN_vkDestroySemaphore destroy;
    PFN_vkGetSemaphoreCounterValue counter;
    PFN_vkQueueSubmit2 submit;
    PFN_vkWaitSemaphores wait;
    PFN_vkQueueWaitIdle drain;
    uint64_t submitted;
    uint64_t completed;
    BOOL submission_attempted;
    PFN_vkGetDeviceProcAddr get;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkCommandPool native_pool;
    VkCommandBuffer native_command;
    VkDescriptorPool uniform_pool;
    PFN_vkDestroyDescriptorPool destroy_uniform_pool;
    PFN_vkCmdBindDescriptorSets bind_uniform_sets;
    uint32_t family;
    BOOL command_inflight,command_uncertain;
    uint64_t command_target;
    PFN_vkDestroyCommandPool destroy_pool;
    PFN_vkResetCommandPool reset_pool;
    PFN_vkAllocateCommandBuffers allocate_commands;
    PFN_vkFreeCommandBuffers free_commands;
    PFN_vkBeginCommandBuffer begin;
    PFN_vkEndCommandBuffer end;
    PFN_vkCmdCopyBuffer copy;
    PFN_vkCmdPipelineBarrier2 barrier;
    PFN_vkCmdBindPipeline bind_pipeline;
    PFN_vkCmdDispatch dispatch;
    PFN_vkCmdFillBuffer fill;
    PFN_vkCmdBindPipeline bind_graphics;
    PFN_vkCmdSetViewport set_viewport;
    PFN_vkCmdSetPrimitiveTopology set_topology;
    PFN_vkCmdSetScissor set_scissor;
    PFN_vkCmdSetStencilReference set_stencil_reference;
    PFN_vkCmdDraw draw;
    PFN_vkCmdSetBlendConstants set_blend_constants;
    PFN_vkCmdClearColorImage clear_image;
    PFN_vkCmdPipelineBarrier2 image_barrier;
    PFN_vkCmdBeginRendering begin_rendering;
    PFN_vkCmdEndRendering end_rendering;
    PFN_vkCmdClearAttachments clear_attachments;
    PFN_vkCmdPipelineBarrier2 rendering_barrier;
};

static void release_native_command(struct queue_owner *owner)
{
    if (owner->native_command != VK_NULL_HANDLE) {
        owner->free_commands(owner->device, owner->native_pool, 1, &owner->native_command);
        owner->native_command = VK_NULL_HANDLE;
        owner->native_pool = VK_NULL_HANDLE;
    }
    if(owner->uniform_pool){owner->destroy_uniform_pool(owner->device,owner->uniform_pool,NULL);owner->uniform_pool=VK_NULL_HANDLE;}
}

static HRESULT result(VkResult status)
{
    switch (status) {
    case VK_SUCCESS:
        return S_OK;
    case VK_TIMEOUT:
        return DXGI_ERROR_WAIT_TIMEOUT;
    case VK_ERROR_DEVICE_LOST:
        return DXGI_ERROR_DEVICE_REMOVED;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return E_OUTOFMEMORY;
    default:
        return E_FAIL;
    }
}

HRESULT ql_dx12_queue_create(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get,
    PFN_vkDestroySemaphore destroy_fallback, uintptr_t *out_owner)
{
    struct queue_owner *owner;
    PFN_vkCreateSemaphore create;
    VkSemaphoreTypeCreateInfo type = {0};
    VkSemaphoreCreateInfo info = {0};
    VkResult status;
    uint64_t initial = UINT64_MAX;

    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || !get || !out_owner) {
        return E_INVALIDARG;
    }
    owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));

    if (!owner) {
        return E_OUTOFMEMORY;
    }
    owner->device = device;
    owner->get = get;
    owner->queue = queue;
    create = (PFN_vkCreateSemaphore)(uintptr_t)get(device, "vkCreateSemaphore");
    owner->destroy = (PFN_vkDestroySemaphore)(uintptr_t)get(device, "vkDestroySemaphore");
    owner->counter = (PFN_vkGetSemaphoreCounterValue)(uintptr_t)get(device, "vkGetSemaphoreCounterValue");
    owner->submit = (PFN_vkQueueSubmit2)(uintptr_t)get(device, "vkQueueSubmit2");
    owner->wait = (PFN_vkWaitSemaphores)(uintptr_t)get(device, "vkWaitSemaphores");
    owner->drain = (PFN_vkQueueWaitIdle)(uintptr_t)get(device, "vkQueueWaitIdle");

    if (!owner->destroy) {
        owner->destroy = destroy_fallback;
    }
    if (!create || !owner->destroy || !owner->counter || !owner->submit || !owner->wait || !owner->drain) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_NOINTERFACE;
    }
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;
    status = create(device, &info, NULL, &owner->timeline);

    if (status != VK_SUCCESS || owner->timeline == VK_NULL_HANDLE) {
        HeapFree(GetProcessHeap(), 0, owner);
        return status == VK_SUCCESS ? E_FAIL : result(status);
    }
    status = owner->counter(device, owner->timeline, &initial);

    if (status != VK_SUCCESS || initial != 0) {
        owner->destroy(device, owner->timeline, NULL);
        HeapFree(GetProcessHeap(), 0, owner);
        return status == VK_SUCCESS ? E_FAIL : result(status);
    }
    *out_owner = (uintptr_t)owner;
    return S_OK;
}

/* This drain keeps the shared queue/timeline owner alive. A completed drain
 * permits reference cleanup even on device loss; it never reports lost work as
 * completed successfully. Failed ordinary drains retain every borrowed object. */
HRESULT ql_dx12_queue_drain_pending(uintptr_t handle, uint32_t *complete)
{
    struct queue_owner *owner = (void *)handle;
    VkResult status = VK_SUCCESS;

    if (!owner || !complete) {
        return E_INVALIDARG;
    }
    *complete = 0;
    if (owner->submission_attempted) {
        status = owner->drain(owner->queue);
        if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
            return result(status);
        }
    }
    release_native_command(owner);
    owner->command_inflight = FALSE;
    owner->command_uncertain = FALSE;
    owner->submission_attempted = FALSE;
    if (status == VK_SUCCESS) {
        owner->completed = owner->submitted;
    }
    *complete = 1;
    return result(status);
}

HRESULT ql_dx12_queue_destroy(uintptr_t handle)
{
    struct queue_owner *owner = (void *)handle;
    uint32_t complete = 0;
    HRESULT status;

    if (!owner) {
        return S_OK;
    }
    status = ql_dx12_queue_drain_pending(handle, &complete);
    if (!complete) {
        return status;
    }
    if (owner->pool != VK_NULL_HANDLE) {
        owner->destroy_pool(owner->device, owner->pool, NULL);
    }
    owner->destroy(owner->device, owner->timeline, NULL);
    HeapFree(GetProcessHeap(), 0, owner);
    return S_OK;
}

static HRESULT observe(struct queue_owner *owner, QLDX12_QUEUE_COMPLETION *completion)
{
    uint64_t value = 0;
    VkResult status = owner->counter(owner->device, owner->timeline, &value);

    if (status != VK_SUCCESS) {
        return result(status);
    }
    if (value < owner->completed || value > owner->submitted) {
        return E_FAIL;
    }
    owner->completed = value;
    if (owner->command_inflight && !owner->command_uncertain && value >= owner->command_target) {
        owner->command_inflight = FALSE;
        release_native_command(owner);
    }
    completion->completed = value;
    return S_OK;
}

HRESULT ql_dx12_queue_execute(uintptr_t handle, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion)
{
    struct queue_owner *owner = (void *)handle;
    VkResult status;
    HRESULT observed;

    if (!owner || !plan || !completion) {
        return E_INVALIDARG;
    }
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->accepted = 0;
    completion->completed = owner->completed;

    if (plan->reserved) {
        return E_INVALIDARG;
    }
    switch (plan->operation) {
    case QLDX12_QUEUE_SIGNAL: {
        VkSemaphoreSubmitInfo signal = {0};
        VkSubmitInfo2 submit = {0};

        if (owner->submitted == UINT64_MAX || plan->value != owner->submitted + 1 || plan->timeout_ns) {
            return E_INVALIDARG;
        }
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = owner->timeline;
        signal.value = plan->value;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        /* Even a failed submission attempt is drained on close: do not infer
         * that an error return proves no queue-side reference can remain. */
        owner->submission_attempted = TRUE;
        status = owner->submit(owner->queue, 1, &submit, VK_NULL_HANDLE);

        if (status != VK_SUCCESS) {
            return result(status);
        }
        owner->submitted = plan->value;
        completion->accepted = 1;
        break;
    }
    case QLDX12_QUEUE_QUERY:
        if (plan->value || plan->timeout_ns) {
            return E_INVALIDARG;
        }
        break;
    case QLDX12_QUEUE_WAIT: {
        VkSemaphoreWaitInfo wait = {0};

        if (plan->value > owner->submitted || plan->timeout_ns > QLDX12_QUEUE_MAX_WAIT_NS) {
            return E_INVALIDARG;
        }
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &owner->timeline;
        wait.pValues = &plan->value;
        status = owner->wait(owner->device, &wait, plan->timeout_ns);

        if (status == VK_TIMEOUT) {
            completion->outcome = QLDX12_QUEUE_TIMEOUT;
            return DXGI_ERROR_WAIT_TIMEOUT;
        }
        if (status != VK_SUCCESS) {
            return result(status);
        }
        break;
    }
    default:
        return E_INVALIDARG;
    }
    observed = observe(owner, completion);

    if (FAILED(observed)) {
        return observed;
    }
    if (plan->operation == QLDX12_QUEUE_WAIT && completion->completed < plan->value) {
        return E_FAIL;
    }
    completion->outcome = QLDX12_QUEUE_SUCCESS;
    return S_OK;
}

static HRESULT command_functions(struct queue_owner *owner)
{
    PFN_vkAllocateCommandBuffers allocate;
    PFN_vkFreeCommandBuffers free;
    PFN_vkBeginCommandBuffer begin;
    PFN_vkEndCommandBuffer end;

    if (owner->begin) {
        return S_OK;
    }
    allocate = (PFN_vkAllocateCommandBuffers)(uintptr_t)owner->get(owner->device, "vkAllocateCommandBuffers");
    free = (PFN_vkFreeCommandBuffers)(uintptr_t)owner->get(owner->device, "vkFreeCommandBuffers");
    begin = (PFN_vkBeginCommandBuffer)(uintptr_t)owner->get(owner->device, "vkBeginCommandBuffer");
    end = (PFN_vkEndCommandBuffer)(uintptr_t)owner->get(owner->device, "vkEndCommandBuffer");

    if (!allocate || !free || !begin || !end) {
        return E_NOINTERFACE;
    }
    owner->allocate_commands = allocate;
    owner->free_commands = free;
    owner->begin = begin;
    owner->end = end;
    return S_OK;
}

static HRESULT transfer_functions(struct queue_owner *owner)
{
    PFN_vkCmdCopyBuffer copy;
    PFN_vkCmdPipelineBarrier2 barrier;

    if (owner->copy) {
        return S_OK;
    }
    copy = (PFN_vkCmdCopyBuffer)(uintptr_t)owner->get(owner->device, "vkCmdCopyBuffer");
    barrier = (PFN_vkCmdPipelineBarrier2)(uintptr_t)owner->get(owner->device, "vkCmdPipelineBarrier2");

    if (!copy || !barrier) {
        return E_NOINTERFACE;
    }
    owner->copy = copy;
    owner->barrier = barrier;
    return S_OK;
}

static HRESULT compute_functions(struct queue_owner *owner,BOOL dispatch_required)
{
    if(!owner->bind_pipeline)owner->bind_pipeline=(PFN_vkCmdBindPipeline)(uintptr_t)owner->get(owner->device,"vkCmdBindPipeline");
    if(dispatch_required && !owner->dispatch)owner->dispatch=(PFN_vkCmdDispatch)(uintptr_t)owner->get(owner->device,"vkCmdDispatch");
    return !owner->bind_pipeline || (dispatch_required && !owner->dispatch)?E_NOINTERFACE:S_OK;
}

/* Independent lazy command groups keep DIRECT state recording separate from
 * COMPUTE dispatch prerequisites even though vkCmdBindPipeline is shared. */
static HRESULT direct_functions(struct queue_owner *owner)
{
    PFN_vkCmdBindPipeline bind;
    PFN_vkCmdSetViewport viewport;
    PFN_vkCmdSetScissor scissor;
    PFN_vkCmdSetStencilReference stencil;

    if (owner->bind_graphics) {
        return S_OK;
    }
    bind = (PFN_vkCmdBindPipeline)(uintptr_t)owner->get(owner->device, "vkCmdBindPipeline");
    viewport = (PFN_vkCmdSetViewport)(uintptr_t)owner->get(owner->device, "vkCmdSetViewport");
    scissor = (PFN_vkCmdSetScissor)(uintptr_t)owner->get(owner->device, "vkCmdSetScissor");

    stencil=(PFN_vkCmdSetStencilReference)(uintptr_t)owner->get(owner->device,"vkCmdSetStencilReference");
    PFN_vkCmdSetPrimitiveTopology topology=(PFN_vkCmdSetPrimitiveTopology)(uintptr_t)owner->get(owner->device,"vkCmdSetPrimitiveTopology");
    if (!bind || !viewport || !scissor || !stencil || !topology) {
        return E_NOINTERFACE;
    }
    owner->bind_graphics = bind;
    owner->set_topology=topology;
    owner->set_viewport = viewport;
    owner->set_scissor = scissor;
    owner->set_stencil_reference = stencil;
    return S_OK;
}

static HRESULT fill_functions(struct queue_owner *owner)
{
    PFN_vkCmdFillBuffer fill;
    PFN_vkCmdPipelineBarrier2 barrier;

    if (owner->fill) {
        return S_OK;
    }
    fill = (PFN_vkCmdFillBuffer)(uintptr_t)owner->get(owner->device, "vkCmdFillBuffer");
    barrier = (PFN_vkCmdPipelineBarrier2)(uintptr_t)owner->get(owner->device, "vkCmdPipelineBarrier2");

    if (!fill || !barrier) {
        return E_NOINTERFACE;
    }
    owner->fill = fill;
    owner->barrier = barrier;
    return S_OK;
}

static HRESULT recording_commands(struct queue_owner *owner, uint32_t family,
    VkCommandPool borrowed, VkCommandBuffer *output)
{
    VkCommandPool pool = borrowed;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info = {0};
    VkCommandBufferAllocateInfo allocation = {0};
    VkResult status;
    HRESULT functions = command_functions(owner);

    if (FAILED(functions)) {
        return functions;
    }
    if (borrowed == VK_NULL_HANDLE) {
        if (owner->pool == VK_NULL_HANDLE) {
            PFN_vkCreateCommandPool create = (PFN_vkCreateCommandPool)(uintptr_t)owner->get(owner->device, "vkCreateCommandPool");
            PFN_vkDestroyCommandPool destroy = (PFN_vkDestroyCommandPool)(uintptr_t)owner->get(owner->device, "vkDestroyCommandPool");
            PFN_vkResetCommandPool reset = (PFN_vkResetCommandPool)(uintptr_t)owner->get(owner->device, "vkResetCommandPool");
            if (!create || !destroy || !reset) {
                return E_NOINTERFACE;
            }
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.queueFamilyIndex = family;
            status = create(owner->device, &pool_info, NULL, &pool);
            if (status != VK_SUCCESS) {
                return result(status);
            }
            if (pool == VK_NULL_HANDLE) {
                return E_FAIL;
            }
            owner->pool = pool;
            owner->family = family;
            owner->destroy_pool = destroy;
            owner->reset_pool = reset;
        }
        if (owner->family != family) {
            return E_INVALIDARG;
        }
        pool = owner->pool;
        if (owner->command != VK_NULL_HANDLE) {
            *output = owner->command;
            return S_OK;
        }
    }
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    status = owner->allocate_commands(owner->device, &allocation, &command);
    if (status != VK_SUCCESS) {
        return result(status);
    }
    if (command == VK_NULL_HANDLE) {
        return E_FAIL;
    }
    if (borrowed == VK_NULL_HANDLE) {
        owner->command = command;
    } else {
        owner->native_pool = borrowed;
        owner->native_command = command;
    }
    *output = command;
    return S_OK;
}

/* COPY, COMPUTE and DIRECT share storage, attempt bookkeeping and teardown.
 * Borrowed pools stay pinned by native list/allocator claims until completion. */
static HRESULT recorded_begin(struct queue_owner *owner, uint32_t family,
    VkCommandPool borrowed, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted, VkCommandBuffer *recorded)
{
    VkCommandBufferBeginInfo begin = {0};
    HRESULT initialized;
    VkResult status;

    if (!owner || !plan || !completion || !attempted || !recorded) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->accepted = 0;
    completion->completed = owner->completed;

    if (owner->command_inflight) {
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    if (plan->operation != QLDX12_QUEUE_SIGNAL || plan->reserved || plan->timeout_ns || owner->submitted == UINT64_MAX || plan->value != owner->submitted + 1) {
        return E_INVALIDARG;
    }
    initialized = recording_commands(owner, family, borrowed, recorded);

    if (FAILED(initialized)) {
        return initialized;
    }
    if (borrowed == VK_NULL_HANDLE) {
        status = owner->reset_pool(owner->device, owner->pool, 0);
        if (status != VK_SUCCESS) {
            return result(status);
        }
    }
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    status = owner->begin(*recorded, &begin);

    if (status != VK_SUCCESS) {
        release_native_command(owner);
        return result(status);
    }
    return S_OK;
}

static HRESULT recorded_submit(struct queue_owner *owner, VkCommandBuffer recorded,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    VkCommandBufferSubmitInfo command = {0};
    VkSemaphoreSubmitInfo signal = {0};
    VkSubmitInfo2 submit = {0};
    VkResult status;
    HRESULT observed;

    status = owner->end(recorded);

    if (status != VK_SUCCESS) {
        release_native_command(owner);
        return result(status);
    }
    command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command.commandBuffer = recorded;
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = owner->timeline;
    signal.value = plan->value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    owner->submission_attempted = TRUE;
    owner->command_inflight = TRUE;
    owner->command_uncertain = TRUE;
    *attempted = 1;
    status = owner->submit(owner->queue, 1, &submit, VK_NULL_HANDLE);

    if (status != VK_SUCCESS) {
        return result(status);
    }
    owner->submitted = plan->value;
    owner->command_target = plan->value;
    owner->command_uncertain = FALSE;
    completion->accepted = 1;
    observed = observe(owner, completion);

    if (FAILED(observed)) {
        return observed;
    }
    completion->outcome = QLDX12_QUEUE_SUCCESS;
    return S_OK;
}

static HRESULT queue_transfer_many(uintptr_t handle, uint32_t family, VkCommandPool borrowed,
    UINT count, const VkBuffer *sources, const VkBuffer *destinations,
    const QLDX12_TRANSFER_PLAN *transfers, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkBufferMemoryBarrier2 barriers[2] = {{0}, {0}};
    VkDependencyInfo dependency = {0};
    VkBufferCopy region = {0};
    VkCommandBuffer recorded = VK_NULL_HANDLE;
    HRESULT status;

    if (!owner || !count || count > QLDX12_NATIVE_COPY_BATCH_LIMIT ||
        !sources || !destinations || !transfers || !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    status = transfer_functions(owner);

    if (FAILED(status)) {
        *attempted = 0;
        completion->outcome = QLDX12_QUEUE_FAILED;
        completion->accepted = 0;
        completion->completed = owner->completed;
        return status;
    }
    status = recorded_begin(owner, family, borrowed, plan, completion, attempted, &recorded);

    if (FAILED(status)) {
        return status;
    }
    for (UINT operation = 0; operation < count; ++operation) {
        const VkBuffer source = sources[operation], destination = destinations[operation];
        const QLDX12_TRANSFER_PLAN *transfer = &transfers[operation];
        for (UINT index = 0; index < 2; ++index) {
            barriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barriers[index].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
            barriers[index].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
            barriers[index].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barriers[index].srcQueueFamilyIndex = barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[index].buffer = index == 0 ? source : destination;
            barriers[index].offset = index == 0 ? transfer->source_offset : transfer->destination_offset;
            barriers[index].size = transfer->byte_size;
        }
        barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[1].srcAccessMask |= VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.bufferMemoryBarrierCount = 2;
        dependency.pBufferMemoryBarriers = barriers;
        owner->barrier(recorded, &dependency);
        region.srcOffset = transfer->source_offset;
        region.dstOffset = transfer->destination_offset;
        region.size = transfer->byte_size;
        owner->copy(recorded, source, destination, 1, &region);

        if (transfer->destination_kind == QLDX12_BUFFER_READBACK) {
            barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            barriers[1].dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &barriers[1];
            owner->barrier(recorded, &dependency);
        }
    }
    return recorded_submit(owner, recorded, plan, completion, attempted);
}

static HRESULT queue_transfer(uintptr_t handle, uint32_t family, VkCommandPool borrowed,
    VkBuffer source, VkBuffer destination, const QLDX12_TRANSFER_PLAN *transfer,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    return queue_transfer_many(handle, family, borrowed, 1, &source, &destination,
        transfer, plan, completion, attempted);
}

HRESULT ql_dx12_queue_transfer(uintptr_t owner, uint32_t family, VkBuffer source,
    VkBuffer destination, const QLDX12_TRANSFER_PLAN *transfer, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    return queue_transfer(owner, family, VK_NULL_HANDLE, source, destination, transfer,
        plan, completion, attempted);
}

HRESULT ql_dx12_queue_transfer_native(uintptr_t owner, uint32_t family,
    VkCommandPool pool, VkBuffer source, VkBuffer destination,
    const QLDX12_TRANSFER_PLAN *transfer, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    if (pool == VK_NULL_HANDLE) {
        return E_INVALIDARG;
    }
    return queue_transfer(owner, family, pool, source, destination, transfer,
        plan, completion, attempted);
}

/* Each attempted native command owns a descriptor pool until the same receipt
 * releases its command buffer. Every draw/dispatch gets independent set data. */
static const QLDX12_ROOT_CBV_BATCH *uniform_batch_at(const QLDX12_ROOT_CBV_BATCH *first,SIZE_T stride,UINT i){return (const void*)((const BYTE*)first+(SIZE_T)i*stride);}
struct uniform_write_scratch {VkWriteDescriptorSet writes[64];VkDescriptorBufferInfo buffers[64];};
static HRESULT prepare_uniform_sets(struct queue_owner *owner,UINT count,const QLDX12_ROOT_CBV_BATCH *first,SIZE_T stride,VkDescriptorPool *pool_out,VkDescriptorSet **sets_out)
{
    *pool_out=VK_NULL_HANDLE;*sets_out=NULL;UINT used=0,total=0;
    for(UINT i=0;i<count;++i){const QLDX12_ROOT_CBV_BATCH *batch=uniform_batch_at(first,stride,i);if(!batch->uniform_count)continue;
        if(!batch->uniform_layout || !batch->uniform_bindings || batch->uniform_bindings>64 || batch->uniform_count>batch->uniform_bindings || total>UINT32_MAX-batch->uniform_bindings)return E_INVALIDARG;
        total+=batch->uniform_bindings;used++;
    }
    if(!used)return S_OK;
    if(owner->command_inflight || owner->uniform_pool)return DXGI_ERROR_WAS_STILL_DRAWING;
    PFN_vkCreateDescriptorPool create=(PFN_vkCreateDescriptorPool)(uintptr_t)owner->get(owner->device,"vkCreateDescriptorPool");
    PFN_vkAllocateDescriptorSets allocate=(PFN_vkAllocateDescriptorSets)(uintptr_t)owner->get(owner->device,"vkAllocateDescriptorSets");
    PFN_vkUpdateDescriptorSets update=(PFN_vkUpdateDescriptorSets)(uintptr_t)owner->get(owner->device,"vkUpdateDescriptorSets");
    owner->destroy_uniform_pool=(PFN_vkDestroyDescriptorPool)(uintptr_t)owner->get(owner->device,"vkDestroyDescriptorPool");
    owner->bind_uniform_sets=(PFN_vkCmdBindDescriptorSets)(uintptr_t)owner->get(owner->device,"vkCmdBindDescriptorSets");
    if(!create || !allocate || !update || !owner->destroy_uniform_pool || !owner->bind_uniform_sets)return E_NOINTERFACE;
    SIZE_T set_bytes=(SIZE_T)count*sizeof(VkDescriptorSet);if(set_bytes>SIZE_MAX-sizeof(struct uniform_write_scratch))return E_OUTOFMEMORY;
    VkDescriptorSet *sets=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,set_bytes+sizeof(struct uniform_write_scratch));if(!sets)return E_OUTOFMEMORY;
    struct uniform_write_scratch *scratch=(void*)((BYTE*)sets+set_bytes);
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,total};VkDescriptorPoolCreateInfo info={0};info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;info.maxSets=used;info.poolSizeCount=1;info.pPoolSizes=&size;
    VkDescriptorPool pool=VK_NULL_HANDLE;VkResult vk=create(owner->device,&info,NULL,&pool);HRESULT status;
    if(vk!=VK_SUCCESS){HeapFree(GetProcessHeap(),0,sets);return result(vk);}if(!pool){HeapFree(GetProcessHeap(),0,sets);return E_FAIL;}
    for(UINT i=0;i<count;++i){const QLDX12_ROOT_CBV_BATCH *batch=uniform_batch_at(first,stride,i);if(!batch->uniform_count)continue;
        VkDescriptorSetAllocateInfo allocation={0};allocation.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;allocation.descriptorPool=pool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&batch->uniform_layout;
        VkDescriptorSet set=VK_NULL_HANDLE;vk=allocate(owner->device,&allocation,&set);if(vk!=VK_SUCCESS){status=result(vk);goto failed;}if(!set){status=E_FAIL;goto failed;}sets[i]=set;
        ZeroMemory(scratch,sizeof(*scratch));VkWriteDescriptorSet *writes=scratch->writes;VkDescriptorBufferInfo *buffers=scratch->buffers;
        for(UINT j=0;j<batch->uniform_count;++j){const QLDX12_UNIFORM_STEP *step=&batch->uniforms[j];
            buffers[j].buffer=step->buffer;buffers[j].offset=step->offset;buffers[j].range=step->bytes;
            writes[j].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[j].dstSet=set;writes[j].dstBinding=step->binding;writes[j].descriptorCount=1;writes[j].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;writes[j].pBufferInfo=&buffers[j];
        }
        update(owner->device,batch->uniform_count,writes,0,NULL);
    }
    *pool_out=pool;*sets_out=sets;return S_OK;
failed:
    owner->destroy_uniform_pool(owner->device,pool,NULL);HeapFree(GetProcessHeap(),0,sets);return status;
}

/* The typed native owner has admitted a resource-free compute PSO and retained
 * it plus the actual allocator pool. The shared submit engine retains command
 * storage after any actual queue attempt, including an uncertain failure. */
static void record_cbv_reads(struct queue_owner *owner,VkCommandBuffer command,const QLDX12_ROOT_CBV_BATCH *batch,BOOL before)
{
    for(UINT i=0;i<batch->uniform_count;++i){const QLDX12_UNIFORM_STEP *step=&batch->uniforms[i];if(!step->buffer)continue;
        const VkPipelineStageFlags2 shaders=VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkBufferMemoryBarrier2 barrier={0};barrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask=before?(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT):shaders;barrier.dstStageMask=before?shaders:(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT);
        barrier.srcAccessMask=before?(VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT):VK_ACCESS_2_UNIFORM_READ_BIT;barrier.dstAccessMask=before?VK_ACCESS_2_UNIFORM_READ_BIT:(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT);
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=step->buffer;barrier.offset=step->offset;barrier.size=step->bytes;
        VkDependencyInfo dependency={0};dependency.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO;dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&barrier;owner->rendering_barrier(command,&dependency);
    }

    for(uint32_t i=0;i<batch->count;++i){const QLDX12_ROOT_CBV_STEP *step=&batch->entries[i];
        VkBufferMemoryBarrier2 barrier={0};VkDependencyInfo dependency={0};barrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        const VkPipelineStageFlags2 shaders=VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcStageMask=before?(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT):shaders;
        barrier.dstStageMask=before?shaders:(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT);
        const VkAccessFlags2 access=VK_ACCESS_2_SHADER_READ_BIT|(step->kind==4?VK_ACCESS_2_SHADER_WRITE_BIT:0);
        barrier.srcAccessMask=before?(VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT):access;
        barrier.dstAccessMask=before?access:(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT|(step->kind==4?VK_ACCESS_2_HOST_READ_BIT:0));
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=step->buffer;barrier.offset=step->offset;barrier.size=step->bytes;
        dependency.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO;dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&barrier;owner->rendering_barrier(command,&dependency);
    }
}

/* Shared argument visibility for graphics and compute indirect execution. */
static void record_indirect_barrier(struct queue_owner *owner, VkCommandBuffer command,
    VkBuffer buffer, const QLDX12_NC_INDIRECT_WORK *work, BOOL before)
{
    VkBufferMemoryBarrier2 barrier = {0};
    VkDependencyInfo dependency = {0};
    const VkPipelineStageFlags2 general = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = before ? general : VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    barrier.dstStageMask = before ? VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT : general;
    barrier.srcAccessMask = before ? VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT : VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    barrier.dstAccessMask = before ? VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer; barrier.offset = work->offset; barrier.size = work->bytes;
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1; dependency.pBufferMemoryBarriers = &barrier;
    owner->rendering_barrier(command, &dependency);
}

HRESULT ql_dx12_queue_compute_native(uintptr_t handle, uint32_t family,
    VkCommandPool pool, uint32_t count, const VkPipeline *pipelines, const QLDX12_NC_DISPATCH_ARGUMENTS *arguments,const VkPipelineLayout *layouts,const QLDX12_NC_ROOT_CONSTANTS *constants,const QLDX12_ROOT_CBV_BATCH *cbvs,const VkBuffer *indirect_buffers,const QLDX12_NC_INDIRECT_WORK *indirect,const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer recorded = VK_NULL_HANDLE;
    HRESULT status;

    if (!owner || pool == VK_NULL_HANDLE || !count || count > QLDX12_NC_COMPUTE_STREAM_LIMIT || !pipelines ||
        !indirect_buffers || !indirect || !cbvs || !constants || !layouts || !arguments || !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->accepted = 0;
    completion->completed = owner->completed;
    for (uint32_t i = 0; i < count; ++i) if (pipelines[i] == VK_NULL_HANDLE) return E_INVALIDARG;
    BOOL direct_required=FALSE,indirect_required=FALSE;
    for(uint32_t i=0;i<count;++i){if(indirect[i].buffer_token){if(!indirect_buffers[i])return E_INVALIDARG;if(indirect[i].count)indirect_required=TRUE;}else direct_required=TRUE;}
    PFN_vkCmdDispatchIndirect dispatch_indirect=indirect_required?(PFN_vkCmdDispatchIndirect)(uintptr_t)owner->get(owner->device,"vkCmdDispatchIndirect"):NULL;
    if(indirect_required && !dispatch_indirect)return E_NOINTERFACE;
    BOOL push_required=FALSE;
    for(uint32_t i=0;i<count;++i){if(!layouts[i] || constants[i].words>64)return E_INVALIDARG;if(constants[i].words && (!indirect[i].buffer_token || indirect[i].count))push_required=TRUE;}
    PFN_vkCmdPushConstants push=(PFN_vkCmdPushConstants)(uintptr_t)owner->get(owner->device,"vkCmdPushConstants");
    if(push_required && !push)return E_NOINTERFACE;
    status = compute_functions(owner,direct_required);

    if (FAILED(status)) {
        return status;
    }
    BOOL reads=indirect_required;for(uint32_t i=0;i<count;++i){if(cbvs[i].count>32 || cbvs[i].uniform_count>64)return E_INVALIDARG;if(cbvs[i].count || cbvs[i].uniform_count)reads=TRUE;}
    if(reads && !owner->rendering_barrier)owner->rendering_barrier=(PFN_vkCmdPipelineBarrier2)(uintptr_t)owner->get(owner->device,"vkCmdPipelineBarrier2");
    if(reads && !owner->rendering_barrier)return E_NOINTERFACE;
    VkDescriptorPool uniform_pool=VK_NULL_HANDLE;VkDescriptorSet *sets=NULL;
    status=prepare_uniform_sets(owner,count,cbvs,sizeof(*cbvs),&uniform_pool,&sets);if(FAILED(status))return status;
    status = recorded_begin(owner, family, pool, plan, completion, attempted, &recorded);
    if (FAILED(status)){if(uniform_pool)owner->destroy_uniform_pool(owner->device,uniform_pool,NULL);if(sets)HeapFree(GetProcessHeap(),0,sets);return status;}
    owner->uniform_pool=uniform_pool;
    for(uint32_t i=0;i<count;++i) {
        if(indirect[i].buffer_token && !indirect[i].count){owner->bind_pipeline(recorded,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[i]);continue;}
        if (indirect[i].buffer_token) record_indirect_barrier(owner, recorded, indirect_buffers[i], &indirect[i], TRUE);
        record_cbv_reads(owner,recorded,&cbvs[i],TRUE);
        owner->bind_pipeline(recorded, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[i]);
        if(sets && sets[i])owner->bind_uniform_sets(recorded,VK_PIPELINE_BIND_POINT_COMPUTE,layouts[i],0,1,&sets[i],0,NULL);
        if(constants[i].words)push(recorded,layouts[i],VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,constants[i].words*4,constants[i].values);
        if(indirect[i].buffer_token){
            VkDeviceSize offset=indirect[i].offset;
            for(uint32_t draw=0;draw<indirect[i].count;++draw){dispatch_indirect(recorded,indirect_buffers[i],offset);offset+=indirect[i].stride;}
        }else owner->dispatch(recorded, arguments[i].x, arguments[i].y, arguments[i].z);
        record_cbv_reads(owner,recorded,&cbvs[i],FALSE);
        if (indirect[i].buffer_token) record_indirect_barrier(owner, recorded, indirect_buffers[i], &indirect[i], FALSE);
    }
    status=recorded_submit(owner, recorded, plan, completion, attempted);if(sets)HeapFree(GetProcessHeap(),0,sets);return status;
}

/* Resource construction owns the buffer in the Rust registry before this call.
 * The shared internal command pool is retained by the selected device; unlike
 * native list recording, no borrowed allocator pool is required here. */
HRESULT ql_dx12_queue_fill(uintptr_t handle, uint32_t family, VkBuffer buffer,
    VkDeviceSize bytes, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer recorded = VK_NULL_HANDLE;
    VkBufferMemoryBarrier2 barrier = {0};
    VkDependencyInfo dependency = {0};
    HRESULT status;
    const VkDeviceSize fill_alignment = sizeof(uint32_t);

    if (!owner || buffer == VK_NULL_HANDLE || !bytes || bytes % fill_alignment ||
        !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->accepted = 0;
    completion->completed = owner->completed;
    status = fill_functions(owner);

    if (FAILED(status)) {
        return status;
    }
    status = recorded_begin(owner, family, VK_NULL_HANDLE, plan, completion, attempted, &recorded);

    if (FAILED(status)) {
        return status;
    }
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.size = bytes;
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    owner->barrier(recorded, &dependency);
    owner->fill(recorded, buffer, 0, bytes, 0);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    owner->barrier(recorded, &dependency);
    return recorded_submit(owner, recorded, plan, completion, attempted);
}

/* The selected backend requires Vulkan 1.3. Its core viewport minimum is 4096 and
 * bounds include [-8192, 8191]; Rust admits only nonnegative endpoints <=4096.
 * https://docs.vulkan.org/spec/latest/chapters/limits.html
 * Negative viewport height is core 1.1+: y=top+height and height=-height preserve
 * the native D3D orientation. This records state only, without raster output.
 * https://docs.vulkan.org/refpages/latest/refpages/source/VkViewport.html */
static void record_direct_state(struct queue_owner *owner, VkCommandBuffer recorded,
    VkPipeline pipeline, const QLDX12_NC_DIRECT_STATE *state,VkPrimitiveTopology topology)
{
    VkViewport viewport = {0};
    VkRect2D scissor = {0};

    viewport.x = state->viewport_x;
    viewport.y = state->viewport_y + state->viewport_height;
    viewport.width = state->viewport_width;
    viewport.height = -state->viewport_height;
    viewport.minDepth = state->min_depth;
    viewport.maxDepth = state->max_depth;
    scissor.offset.x = state->scissor_left;
    scissor.offset.y = state->scissor_top;
    scissor.extent.width = (uint32_t)(state->scissor_right - state->scissor_left);
    scissor.extent.height = (uint32_t)(state->scissor_bottom - state->scissor_top);
    owner->bind_graphics(recorded, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    owner->set_topology(recorded,topology);
    owner->set_viewport(recorded, 0, 1, &viewport);
    owner->set_scissor(recorded, 0, 1, &scissor);
    owner->set_stencil_reference(recorded,VK_STENCIL_FACE_FRONT_AND_BACK,state->stencil_reference);
}

HRESULT ql_dx12_queue_empty_native(uintptr_t handle, uint32_t family, VkCommandPool pool,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer recorded = VK_NULL_HANDLE;
    if (!owner || pool == VK_NULL_HANDLE || !plan || !completion || !attempted) return E_INVALIDARG;
    HRESULT status = recorded_begin(owner, family, pool, plan, completion, attempted, &recorded);
    if (FAILED(status)) return status;
    return recorded_submit(owner, recorded, plan, completion, attempted);
}

HRESULT ql_dx12_queue_direct_native(uintptr_t handle, uint32_t family,
    VkCommandPool pool, VkPipeline pipeline, const QLDX12_NC_DIRECT_STATE *state,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer recorded = VK_NULL_HANDLE;
    HRESULT status;

    if (!owner || pool == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE || !state ||
        !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->accepted = 0;
    completion->completed = owner->completed;
    status = direct_functions(owner);

    if (FAILED(status)) {
        return status;
    }
    status = recorded_begin(owner, family, pool, plan, completion, attempted, &recorded);

    if (FAILED(status)) {
        return status;
    }
    record_direct_state(owner, recorded, pipeline, state,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    return recorded_submit(owner, recorded, plan, completion, attempted);
}

/* Real optimal-image initialization shares the retained construction pool and
 * exact submit receipt with buffer fill. Only Rust's observed own target may
 * publish a native resource; all attempted failures remain drain-only cleanup. */
HRESULT ql_dx12_queue_image_initialize(uintptr_t handle, uint32_t family, VkImage image,
    uint32_t array_layers, VkImageAspectFlags aspect, const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer recorded = VK_NULL_HANDLE;
    VkImageMemoryBarrier2 barrier = {0};
    VkDependencyInfo dependency = {0};
    VkClearColorValue zero = {0};
    VkImageSubresourceRange range = {0};
    PFN_vkCmdClearDepthStencilImage depth_clear = NULL;
    const BOOL depth = !!(aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
    HRESULT status;

    if (!owner || image == VK_NULL_HANDLE || !array_layers || (aspect != VK_IMAGE_ASPECT_COLOR_BIT && aspect != VK_IMAGE_ASPECT_DEPTH_BIT && aspect != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) || !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->accepted = 0;
    completion->completed = owner->completed;

    PFN_vkCmdPipelineBarrier2 image_barrier = (PFN_vkCmdPipelineBarrier2)(uintptr_t)
        owner->get(owner->device, "vkCmdPipelineBarrier2");
    if (depth) depth_clear = (PFN_vkCmdClearDepthStencilImage)(uintptr_t)owner->get(owner->device, "vkCmdClearDepthStencilImage");
    else if (!owner->clear_image) owner->clear_image = (PFN_vkCmdClearColorImage)(uintptr_t)owner->get(owner->device, "vkCmdClearColorImage");
    if (!image_barrier || (depth ? !depth_clear : !owner->clear_image)) return E_NOINTERFACE;
    owner->image_barrier = image_barrier;
    status = recorded_begin(owner, family, VK_NULL_HANDLE, plan, completion, attempted, &recorded);

    if (FAILED(status)) {
        return status;
    }
    range.aspectMask = aspect;
    range.levelCount = 1;
    range.layerCount = array_layers;
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    owner->image_barrier(recorded, &dependency);
    if (depth) {
        const VkClearDepthStencilValue initial = {0.0f, 0};
        depth_clear(recorded, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &initial, 1, &range);
    } else owner->clear_image(recorded, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = depth ? VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = depth ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    owner->image_barrier(recorded, &dependency);
    return recorded_submit(owner, recorded, plan, completion, attempted);
}

/* Independent dynamic-rendering group: a valid clear requires neither pipeline
 * nor viewport state. Missing functions reject before recording/submission. */
static HRESULT rendering_functions(struct queue_owner *owner, BOOL rectangles)
{
    PFN_vkCmdBeginRendering begin;
    PFN_vkCmdEndRendering end;
    PFN_vkCmdPipelineBarrier2 barrier;

    if (rectangles && !owner->clear_attachments) {
        owner->clear_attachments = (PFN_vkCmdClearAttachments)(uintptr_t)
            owner->get(owner->device, "vkCmdClearAttachments");
        if (!owner->clear_attachments) {
            return E_NOINTERFACE;
        }
    }
    if (owner->begin_rendering) {
        return S_OK;
    }
    begin = (PFN_vkCmdBeginRendering)(uintptr_t)owner->get(owner->device, "vkCmdBeginRendering");
    end = (PFN_vkCmdEndRendering)(uintptr_t)owner->get(owner->device, "vkCmdEndRendering");
    barrier = (PFN_vkCmdPipelineBarrier2)(uintptr_t)owner->get(owner->device, "vkCmdPipelineBarrier2");

    if (!begin || !end || !barrier) {
        return E_NOINTERFACE;
    }
    owner->begin_rendering = begin;
    owner->end_rendering = end;
    owner->rendering_barrier = barrier;
    return S_OK;
}

/* Images enter and leave in COLOR_ATTACHMENT_OPTIMAL. Full-subresource barriers
 * serialize prior initialization/rendering and later attachment/transfer use.
 * LOAD/STORE binds an OM snapshot without clearing or inventing draw work.
 * CLEAR/STORE implements an explicit full clear. Rectangle clears use LOAD to
 * preserve untouched texels, then vkCmdClearAttachments with clipped positive
 * rectangles. Explicit lists clipped to empty issue no attachment clear.
 * https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderingAttachmentInfo.html */
static VkImageLayout attachment_layout(QLDX12_ATTACHMENT_RANGE range)
{
    if (!(range.barrier.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const BOOL stencil=!!(range.barrier.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (range.read_only_flags & 1) return !stencil || (range.read_only_flags & 2) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    if (stencil && (range.read_only_flags & 2)) return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

static void record_blend_constants(struct queue_owner *owner, VkCommandBuffer recorded,
    const QLDX12_NC_BLEND_CONSTANTS *constants)
{
    float values[4];
    _Static_assert(sizeof(values) == sizeof(*constants), "four exact blend float scalars");
    CopyMemory(values, constants, sizeof(values));
    owner->set_blend_constants(recorded, values);
}

static void record_render_target(struct queue_owner *owner, VkCommandBuffer recorded,
    VkImage image, VkImageView view, VkExtent2D extent, QLDX12_ATTACHMENT_RANGE range, BOOL clear, const QLDX12_NC_RENDER_TARGET_WORK *work,
    VkPipeline pipeline, const QLDX12_NC_DIRECT_STATE *state, const QLDX12_NC_DRAW_ARGUMENTS *arguments, const QLDX12_NC_BLEND_CONSTANTS *constants)
{
    VkImageMemoryBarrier2 barrier = {0};
    VkDependencyInfo dependency = {0};
    VkRenderingAttachmentInfo attachment = {0};
    VkRenderingInfo rendering = {0};
    VkRenderingAttachmentInfo stencil_attachment = {0};
    VkClearAttachment clear_attachment = {0};
    VkClearRect rectangles[QLDX12_NC_CLEAR_RECT_LIMIT] = {0};
    const BOOL depth = !!(range.barrier.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT);
    const BOOL stencil = !!(range.barrier.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT);
    const VkImageLayout layout = attachment_layout(range);
    const VkPipelineStageFlags2 stages = depth ? VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkAccessFlags2 access = depth ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    BOOL partial = clear && (work->flags & QLDX12_NC_RENDER_TARGET_CLEAR_RECTS);

    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = stages;
    barrier.dstAccessMask = access;
    barrier.oldLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range.barrier;
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    owner->rendering_barrier(recorded, &dependency);
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = view;
    attachment.imageLayout = layout;
    attachment.loadOp = clear && !partial ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    if (clear) {
        attachment.clearValue.color.float32[0] = work->clear_red;
        attachment.clearValue.color.float32[1] = work->clear_green;
        attachment.clearValue.color.float32[2] = work->clear_blue;
        attachment.clearValue.color.float32[3] = work->clear_alpha;
    }
    if (depth) {
        if (clear) attachment.clearValue.depthStencil = (VkClearDepthStencilValue){work->clear_red, (uint32_t)work->clear_green};
        stencil_attachment = attachment;
        attachment.loadOp = clear && !partial && (work->flags & QLDX12_NC_RENDER_TARGET_DEPTH_CLEAR) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        stencil_attachment.loadOp = clear && !partial && (work->flags & QLDX12_NC_RENDER_TARGET_STENCIL_CLEAR) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = extent;
    rendering.layerCount = range.framebuffer_layers;
    rendering.colorAttachmentCount = depth ? 0 : 1;
    rendering.pColorAttachments = depth ? NULL : &attachment;
    rendering.pDepthAttachment = depth ? &attachment : NULL;
    rendering.pStencilAttachment = stencil ? &stencil_attachment : NULL;
    owner->begin_rendering(recorded, &rendering);
    if (partial && work->rect_count) {
        clear_attachment.aspectMask = depth ? ((work->flags & QLDX12_NC_RENDER_TARGET_DEPTH_CLEAR ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (work->flags & QLDX12_NC_RENDER_TARGET_STENCIL_CLEAR ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
        clear_attachment.colorAttachment = 0;
        clear_attachment.clearValue = attachment.clearValue;
        for (uint32_t index = 0; index < work->rect_count; ++index) {
            const QLDX12_NC_CLEAR_RECT *source = &work->rectangles[index];
            rectangles[index].rect.offset.x = source->left;
            rectangles[index].rect.offset.y = source->top;
            rectangles[index].rect.extent.width = (uint32_t)(source->right - source->left);
            rectangles[index].rect.extent.height = (uint32_t)(source->bottom - source->top);
            rectangles[index].baseArrayLayer = 0;
            rectangles[index].layerCount = range.framebuffer_layers;
        }
        owner->clear_attachments(recorded, 1, &clear_attachment, work->rect_count, rectangles);
    }
    if (state) {
        record_direct_state(owner, recorded, pipeline, state,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        record_blend_constants(owner, recorded, constants);
        owner->draw(recorded, arguments->vertices, arguments->instances, arguments->first_vertex, arguments->first_instance);
    }
    owner->end_rendering(recorded);
    barrier.oldLayout = layout;
    barrier.newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcStageMask = stages;
    barrier.srcAccessMask = access;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    owner->rendering_barrier(recorded, &dependency);
}

/* Typed callers retain both optional RTV snapshots and their images plus the
 * actual DIRECT allocator pool. Each attempted submit follows the shared
 * uncertain/accepted receipt and command retirement machinery. */
HRESULT ql_dx12_queue_render_target_native(uintptr_t handle, uint32_t family,
    VkCommandPool pool, VkImage bound_image, VkImageView bound_view, VkExtent2D bound_extent, QLDX12_ATTACHMENT_RANGE bound_range,
    VkImage clear_image, VkImageView clear_view, VkExtent2D clear_extent, QLDX12_ATTACHMENT_RANGE clear_range, const QLDX12_NC_RENDER_TARGET_WORK *work,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer recorded = VK_NULL_HANDLE;
    HRESULT status;

    if (!owner || pool == VK_NULL_HANDLE || !work || !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->accepted = 0;
    completion->completed = owner->completed;

    if (!work->flags || (work->flags & ~(QLDX12_NC_RENDER_TARGET_BIND | QLDX12_NC_RENDER_TARGET_CLEAR | QLDX12_NC_RENDER_TARGET_CLEAR_RECTS | QLDX12_NC_RENDER_TARGET_DEPTH_CLEAR | QLDX12_NC_RENDER_TARGET_STENCIL_CLEAR)) ||
        work->reserved ||
        ((work->flags & QLDX12_NC_RENDER_TARGET_BIND) ?
            (bound_image == VK_NULL_HANDLE || bound_view == VK_NULL_HANDLE) :
            (bound_image != VK_NULL_HANDLE || bound_view != VK_NULL_HANDLE)) ||
        ((work->flags & QLDX12_NC_RENDER_TARGET_CLEAR) ?
            (clear_image == VK_NULL_HANDLE || clear_view == VK_NULL_HANDLE) :
            (clear_image != VK_NULL_HANDLE || clear_view != VK_NULL_HANDLE))) {
        return E_INVALIDARG;
    }
    if (work->rect_count > QLDX12_NC_CLEAR_RECT_LIMIT ||
        ((work->flags & QLDX12_NC_RENDER_TARGET_CLEAR_RECTS) &&
            !(work->flags & QLDX12_NC_RENDER_TARGET_CLEAR)) ||
        (!(work->flags & QLDX12_NC_RENDER_TARGET_CLEAR_RECTS) && work->rect_count)) {
        return E_INVALIDARG;
    }
    if (((work->flags & QLDX12_NC_RENDER_TARGET_DEPTH_CLEAR) && (clear_range.read_only_flags & 1)) ||
        ((work->flags & QLDX12_NC_RENDER_TARGET_STENCIL_CLEAR) && (clear_range.read_only_flags & 2))) return E_INVALIDARG;
    for (uint32_t index = 0; index < QLDX12_NC_CLEAR_RECT_LIMIT; ++index) {
        const QLDX12_NC_CLEAR_RECT *rect = &work->rectangles[index];
        if (index < work->rect_count ?
            (rect->left < 0 || rect->top < 0 || rect->right <= rect->left || rect->bottom <= rect->top ||
                rect->right > (int32_t)clear_extent.width || rect->bottom > (int32_t)clear_extent.height) :
            (rect->left || rect->top || rect->right || rect->bottom)) {
            return E_INVALIDARG;
        }
    }
    if ((work->flags & QLDX12_NC_RENDER_TARGET_BIND) && bound_range.barrier.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) return E_INVALIDARG;
    if ((work->flags & QLDX12_NC_RENDER_TARGET_CLEAR) &&
        (!!(clear_range.barrier.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != !!(work->flags & (QLDX12_NC_RENDER_TARGET_DEPTH_CLEAR | QLDX12_NC_RENDER_TARGET_STENCIL_CLEAR)))) return E_INVALIDARG;
    status = rendering_functions(owner, work->rect_count != 0);

    if (FAILED(status)) {
        return status;
    }
    status = recorded_begin(owner, family, pool, plan, completion, attempted, &recorded);

    if (FAILED(status)) {
        return status;
    }
    if (work->flags & QLDX12_NC_RENDER_TARGET_BIND) {
        record_render_target(owner, recorded, bound_image, bound_view, bound_extent, bound_range, FALSE, work, VK_NULL_HANDLE, NULL, NULL, NULL);
    }
    if (work->flags & QLDX12_NC_RENDER_TARGET_CLEAR) {
        record_render_target(owner, recorded, clear_image, clear_view, clear_extent, clear_range, TRUE, work, VK_NULL_HANDLE, NULL, NULL, NULL);
    }
    return recorded_submit(owner, recorded, plan, completion, attempted);
}

/* Exact genuine original R0 transition callbacks have recorded a closed
 * RENDER_TARGET -> COPY_SOURCE -> RENDER_TARGET transaction. Emit both ordered
 * Vulkan barriers. An optional validated READBACK copy executes strictly between
 * them. There is no draw, implicit transition or persistent inter-list state.
 * An attempted failure keeps the original unknown/terminal work state and all
 * owners until actual completion/drain; no CPU state pretends to roll it back. */
static HRESULT image_transaction_native(uintptr_t handle, uint32_t family,
    VkCommandPool pool, VkImage image, uint32_t layer, VkBuffer buffer,
    const QLDX12_NC_IMAGE_READBACK_FOOTPRINT *footprint, uint64_t active_bytes,
    const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer recorded = VK_NULL_HANDLE;
    PFN_vkCmdPipelineBarrier2 transition;
    VkImageMemoryBarrier2 barrier = {0};
    VkDependencyInfo dependency = {0};
    PFN_vkCmdCopyImageToBuffer copy = NULL;
    uint32_t texel_bytes = 0;
    VkBufferMemoryBarrier2 buffer_barrier = {0};
    VkDependencyInfo buffer_dependency = {0};
    VkBufferImageCopy region = {0};
    HRESULT status;

    if (!owner || !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->accepted = 0;
    completion->completed = owner->completed;

    if (pool == VK_NULL_HANDLE || image == VK_NULL_HANDLE) {
        return E_INVALIDARG;
    }
    transition = (PFN_vkCmdPipelineBarrier2)(uintptr_t)owner->get(owner->device, "vkCmdPipelineBarrier2");

    if (!transition) {
        return E_NOINTERFACE;
    }
    if (footprint) {
        texel_bytes = ql_dx12_native_color_texel_bytes(footprint->format);
        if (buffer == VK_NULL_HANDLE || !texel_bytes) {
            return E_INVALIDARG;
        }
        copy = (PFN_vkCmdCopyImageToBuffer)(uintptr_t)owner->get(owner->device, "vkCmdCopyImageToBuffer");

        if (!copy) {
            return E_NOINTERFACE;
        }
    }
    status = recorded_begin(owner, family, pool, plan, completion, attempted, &recorded);

    if (FAILED(status)) {
        return status;
    }
    if (footprint) {
        buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        buffer_barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        buffer_barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
        buffer_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        buffer_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buffer_barrier.buffer = buffer;
        buffer_barrier.offset = footprint->offset;
        buffer_barrier.size = active_bytes;
        buffer_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        buffer_dependency.bufferMemoryBarrierCount = 1;
        buffer_dependency.pBufferMemoryBarriers = &buffer_barrier;
        transition(recorded, &buffer_dependency);
    }
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = layer == UINT32_MAX ? 0 : layer;
    barrier.subresourceRange.layerCount = layer == UINT32_MAX ? VK_REMAINING_ARRAY_LAYERS : 1;
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    transition(recorded, &dependency);

    if (copy) {
        region.bufferOffset = footprint->offset;
        region.bufferRowLength = footprint->row_pitch / texel_bytes;
        region.bufferImageHeight = footprint->height;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.baseArrayLayer = (uint32_t)footprint->source_subresource;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = footprint->width;
        region.imageExtent.height = footprint->height;
        region.imageExtent.depth = footprint->depth;
        copy(recorded, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    }
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    transition(recorded, &dependency);

    if (copy) {
        buffer_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        buffer_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        buffer_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        buffer_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        transition(recorded, &buffer_dependency);
    }
    return recorded_submit(owner, recorded, plan, completion, attempted);
}

HRESULT ql_dx12_queue_image_transition_native(uintptr_t handle, uint32_t family,
    VkCommandPool pool, VkImage image, uint32_t layer, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    return image_transaction_native(handle, family, pool, image, layer, VK_NULL_HANDLE,
        NULL, 0, plan, completion, attempted);
}

HRESULT ql_dx12_queue_image_readback_native(uintptr_t handle, uint32_t family,
    VkCommandPool pool, VkImage image, uint32_t layer, VkBuffer buffer,
    const QLDX12_NC_IMAGE_READBACK_FOOTPRINT *footprint, uint64_t active_bytes,
    const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    if (!footprint) {
        return E_INVALIDARG;
    }
    return image_transaction_native(handle, family, pool, image, layer, buffer,
        footprint, active_bytes, plan, completion, attempted);
}

/* Admission and the fixed draw shape are Rust policy. The Vulkan boundary emits
 * that exact procedural triangle within the retained single color attachment LOAD/STORE
 * rendering scope, preserving the explicit COLOR_ATTACHMENT_OPTIMAL state. */
static void record_depth_draw(struct queue_owner *owner, VkCommandBuffer recorded, VkPipeline pipeline,
    VkImage color_image, VkImageView color_view, VkExtent2D extent, QLDX12_ATTACHMENT_RANGE color_range,
    VkImage depth_image, VkImageView depth_view, QLDX12_ATTACHMENT_RANGE depth_range, const QLDX12_NC_DIRECT_STATE *state, const QLDX12_NC_DRAW_ARGUMENTS *arguments, const QLDX12_NC_BLEND_CONSTANTS *constants)
{
    VkImageMemoryBarrier2 barriers[2]={{0}};
    const VkPipelineStageFlags2 stages[]={VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT};
    const VkAccessFlags2 access[]={VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    const VkImage images[]={color_image,depth_image};
    const VkImageLayout layouts[]={VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,attachment_layout(depth_range)};
    const VkImageSubresourceRange ranges[]={color_range.barrier,depth_range.barrier};
    for(uint32_t index=0;index<2;++index) {
        VkImageMemoryBarrier2 *b=&barriers[index];b->sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b->srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b->srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        b->dstStageMask=stages[index];b->dstAccessMask=access[index];b->oldLayout=index?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b->newLayout=layouts[index];
        b->srcQueueFamilyIndex=b->dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b->image=images[index];b->subresourceRange=ranges[index];
    }
    VkDependencyInfo dependency={0};dependency.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO;dependency.imageMemoryBarrierCount=2;dependency.pImageMemoryBarriers=barriers;
    owner->rendering_barrier(recorded,&dependency);
    const VkRenderingAttachmentInfo color={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,NULL,color_view,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_RESOLVE_MODE_NONE,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE,{{0}}};
    VkRenderingAttachmentInfo depth=color;depth.imageView=depth_view;depth.imageLayout=layouts[1];
    VkRenderingInfo rendering={0};rendering.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;rendering.renderArea.extent=extent;rendering.layerCount=color_range.framebuffer_layers;
    rendering.colorAttachmentCount=1;rendering.pColorAttachments=&color;rendering.pDepthAttachment=&depth;
    rendering.pStencilAttachment=depth_range.barrier.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT ? &depth : NULL;
    owner->begin_rendering(recorded,&rendering);
    record_direct_state(owner,recorded,pipeline,state,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    record_blend_constants(owner, recorded, constants);
    owner->draw(recorded,arguments->vertices,arguments->instances,arguments->first_vertex,arguments->first_instance);
    owner->end_rendering(recorded);
    for(uint32_t index=0;index<2;++index) {
        barriers[index].oldLayout=layouts[index];barriers[index].newLayout=index?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[index].srcStageMask=stages[index];barriers[index].srcAccessMask=access[index];
        barriers[index].dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;barriers[index].dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    owner->rendering_barrier(recorded,&dependency);
}

HRESULT ql_dx12_queue_draw_native(uintptr_t handle, uint32_t family, VkCommandPool pool,
    VkPipeline pipeline, VkImage image, VkImageView view, VkExtent2D extent, QLDX12_ATTACHMENT_RANGE range, VkImage depth_image, VkImageView depth_view, QLDX12_ATTACHMENT_RANGE depth_range, const QLDX12_NC_DIRECT_STATE *state, const QLDX12_NC_DRAW_ARGUMENTS *arguments, const QLDX12_NC_BLEND_CONSTANTS *constants,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer recorded = VK_NULL_HANDLE;
    QLDX12_NC_RENDER_TARGET_WORK work = {0};
    HRESULT status;

    if (!owner || pool == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE ||
        image == VK_NULL_HANDLE || view == VK_NULL_HANDLE || !state || !arguments || !constants ||
        !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->accepted = 0;
    completion->completed = owner->completed;
    status = direct_functions(owner);

    if (FAILED(status)) {
        return status;
    }
    if (!owner->set_blend_constants) {
        owner->set_blend_constants = (PFN_vkCmdSetBlendConstants)(uintptr_t)owner->get(owner->device, "vkCmdSetBlendConstants");
        if (!owner->set_blend_constants) return E_NOINTERFACE;
    }
    status = rendering_functions(owner, FALSE);

    if (FAILED(status)) {
        return status;
    }
    if (!owner->draw) {
        owner->draw = (PFN_vkCmdDraw)(uintptr_t)owner->get(owner->device, "vkCmdDraw");

        if (!owner->draw) {
            return E_NOINTERFACE;
        }
    }
    status = recorded_begin(owner, family, pool, plan, completion, attempted, &recorded);

    if (FAILED(status)) {
        return status;
    }
    if (depth_view != VK_NULL_HANDLE) record_depth_draw(owner,recorded,pipeline,image,view,extent,range,depth_image,depth_view,depth_range,state,arguments,constants);
    else record_render_target(owner, recorded, image, view, extent, range, FALSE, &work, pipeline, state, arguments, constants);
    return recorded_submit(owner, recorded, plan, completion, attempted);
}

/* A genuine closed RT/COPY_DEST/RT transaction uploads from an unmapped retained
 * native source. Rust owns placement, source extent and destination admission.
 * Host writes become transfer reads; the explicit reverse barrier makes actual
 * copied texels available for later attachment use without persistent state. */
HRESULT ql_dx12_queue_image_upload_native(uintptr_t handle, uint32_t family,
    VkCommandPool pool, VkImage image, uint32_t layer, VkBuffer buffer,
    const QLDX12_NC_IMAGE_UPLOAD_FOOTPRINT *footprint, uint64_t active_bytes,
    const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer recorded = VK_NULL_HANDLE;
    PFN_vkCmdPipelineBarrier2 transition;
    VkImageMemoryBarrier2 barrier = {0};
    VkDependencyInfo dependency = {0};
    PFN_vkCmdCopyBufferToImage copy = NULL;
    VkBufferMemoryBarrier2 buffer_barrier = {0};
    VkDependencyInfo buffer_dependency = {0};
    VkBufferImageCopy region = {0};
    HRESULT status;

    if (!owner || !footprint || !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->accepted = 0;
    completion->completed = owner->completed;

    if (pool == VK_NULL_HANDLE || image == VK_NULL_HANDLE) {
        return E_INVALIDARG;
    }
    transition = (PFN_vkCmdPipelineBarrier2)(uintptr_t)owner->get(owner->device, "vkCmdPipelineBarrier2");

    if (!transition) {
        return E_NOINTERFACE;
    }
    const uint32_t texel_bytes = ql_dx12_native_color_texel_bytes(footprint->format);

    if (buffer == VK_NULL_HANDLE || !texel_bytes) {
        return E_INVALIDARG;
    }
    copy = (PFN_vkCmdCopyBufferToImage)(uintptr_t)owner->get(owner->device, "vkCmdCopyBufferToImage");

    if (!copy) {
        return E_NOINTERFACE;
    }

    status = recorded_begin(owner, family, pool, plan, completion, attempted, &recorded);

    if (FAILED(status)) {
        return status;
    }
    buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    buffer_barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    buffer_barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    buffer_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.buffer = buffer;
    buffer_barrier.offset = footprint->offset;
    buffer_barrier.size = active_bytes;
    buffer_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    buffer_dependency.bufferMemoryBarrierCount = 1;
    buffer_dependency.pBufferMemoryBarriers = &buffer_barrier;
    transition(recorded, &buffer_dependency);

    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = layer == UINT32_MAX ? 0 : layer;
    barrier.subresourceRange.layerCount = layer == UINT32_MAX ? VK_REMAINING_ARRAY_LAYERS : 1;
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    transition(recorded, &dependency);

    region.bufferOffset = footprint->offset;
    region.bufferRowLength = footprint->row_pitch / texel_bytes;
    region.bufferImageHeight = footprint->height;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.baseArrayLayer = (uint32_t)footprint->destination_subresource;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = footprint->width;
    region.imageExtent.height = footprint->height;
    region.imageExtent.depth = footprint->depth;
    copy(recorded, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    transition(recorded, &dependency);

    buffer_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    buffer_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    buffer_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
    transition(recorded, &buffer_dependency);

    return recorded_submit(owner, recorded, plan, completion, attempted);
}


HRESULT ql_dx12_queue_color_resolve(uintptr_t handle, uint32_t family, VkCommandPool pool,
    VkImage source, VkImage destination, VkExtent2D extent, uint32_t source_layer, uint32_t destination_layer,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkImageMemoryBarrier2 barriers[2] = {{0}};
    VkDependencyInfo dependency = {0};
    VkImageResolve region = {0};
    if (!owner || !plan || !completion || !attempted) return E_INVALIDARG;
    *attempted = 0;
    completion->accepted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->completed = owner->completed;
    PFN_vkCmdResolveImage resolve = (PFN_vkCmdResolveImage)(uintptr_t)owner->get(owner->device, "vkCmdResolveImage");
    PFN_vkCmdPipelineBarrier2 transition = (PFN_vkCmdPipelineBarrier2)(uintptr_t)owner->get(owner->device, "vkCmdPipelineBarrier2");
    if (!resolve || !transition) return E_NOINTERFACE;
    if (!source || !destination || source == destination || !extent.width || !extent.height || !pool)
        return E_INVALIDARG;
    HRESULT status = recorded_begin(owner, family, pool, plan, completion, attempted, &command);
    if (FAILED(status)) return status;
    for (uint32_t i = 0; i < 2; ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[i].srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_RESOLVE_BIT;
        barriers[i].dstAccessMask = i ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[i].newLayout = i ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = i ? destination : source;
        barriers[i].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
            i ? destination_layer : source_layer, 1};
    }
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 2;
    dependency.pImageMemoryBarriers = barriers;
    transition(command, &dependency);
    region.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, source_layer, 1};
    region.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, destination_layer, 1};
    region.extent = (VkExtent3D){extent.width, extent.height, 1};
    resolve(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    for (uint32_t i = 0; i < 2; ++i) {
        barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_RESOLVE_BIT;
        barriers[i].srcAccessMask = barriers[i].dstAccessMask;
        barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barriers[i].oldLayout = barriers[i].newLayout;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    transition(command, &dependency);
    return recorded_submit(owner, command, plan, completion, attempted);
}


HRESULT ql_dx12_queue_transfer_native_many(uintptr_t owner, uint32_t family,
    VkCommandPool pool, UINT count, const VkBuffer *sources, const VkBuffer *destinations,
    const QLDX12_TRANSFER_PLAN *transfers, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    if (pool == VK_NULL_HANDLE) return E_INVALIDARG;
    return queue_transfer_many(owner, family, pool, count, sources, destinations,
        transfers, plan, completion, attempted);
}


HRESULT ql_dx12_queue_image_copy(uintptr_t handle, uint32_t family, VkCommandPool pool,
    VkImage source, VkImage destination, VkExtent3D extent, uint32_t layers,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct queue_owner *owner = (void *)handle;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkImageMemoryBarrier2 barriers[2] = {{0}};
    VkDependencyInfo dependency = {0};
    VkImageCopy region = {0};
    if (!owner || !plan || !completion || !attempted) return E_INVALIDARG;
    *attempted = 0;
    completion->accepted = 0;
    completion->outcome = QLDX12_QUEUE_FAILED;
    completion->completed = owner->completed;
    PFN_vkCmdCopyImage copy = (PFN_vkCmdCopyImage)(uintptr_t)owner->get(owner->device, "vkCmdCopyImage");
    PFN_vkCmdPipelineBarrier2 transition = (PFN_vkCmdPipelineBarrier2)(uintptr_t)owner->get(owner->device, "vkCmdPipelineBarrier2");
    if (!copy || !transition) return E_NOINTERFACE;
    if (!source || !destination || source == destination || !extent.width || !extent.height || !extent.depth || !layers || !pool)
        return E_INVALIDARG;
    HRESULT status = recorded_begin(owner, family, pool, plan, completion, attempted, &command);
    if (FAILED(status)) return status;
    for (uint32_t i = 0; i < 2; ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[i].srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barriers[i].dstAccessMask = i ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[i].newLayout = i ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = i ? destination : source;
        barriers[i].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
            0, layers};
    }
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 2;
    dependency.pImageMemoryBarriers = barriers;
    transition(command, &dependency);
    region.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    region.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    region.extent = extent;
    copy(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    for (uint32_t i = 0; i < 2; ++i) {
        barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barriers[i].srcAccessMask = barriers[i].dstAccessMask;
        barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barriers[i].oldLayout = barriers[i].newLayout;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    transition(command, &dependency);
    return recorded_submit(owner, command, plan, completion, attempted);
}

/* One retained indexed draw. Every procedure and selected object is admitted
 * before recorded_begin, preserving whole-list no-attempt retry semantics. */
HRESULT ql_dx12_queue_draw_indexed_native(uintptr_t handle,uint32_t family,VkCommandPool pool,VkPipeline pipeline,
    VkImage image,VkImageView view,VkExtent2D extent,QLDX12_ATTACHMENT_RANGE range,VkImage depth_image,VkImageView depth_view,QLDX12_ATTACHMENT_RANGE depth_range,
    const QLDX12_NC_DIRECT_STATE *state,VkBuffer buffer,const QLDX12_NC_INDEXED_WORK *indexed,const QLDX12_NC_BLEND_CONSTANTS *constants,
    const QLDX12_QUEUE_PLAN *plan,QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted)
{
    struct queue_owner *owner=(void*)handle;VkCommandBuffer command=VK_NULL_HANDLE;HRESULT status;
    if(!owner || !pool || !pipeline || !image || !view || !buffer || !state || !indexed || !constants || !plan || !completion || !attempted)return E_INVALIDARG;
    *attempted=0;completion->accepted=0;completion->outcome=QLDX12_QUEUE_FAILED;completion->completed=owner->completed;
    if(indexed->format!=57 && indexed->format!=42)return E_INVALIDARG;
    PFN_vkCmdBindIndexBuffer bind=(PFN_vkCmdBindIndexBuffer)(uintptr_t)owner->get(owner->device,"vkCmdBindIndexBuffer");
    PFN_vkCmdDrawIndexed draw=(PFN_vkCmdDrawIndexed)(uintptr_t)owner->get(owner->device,"vkCmdDrawIndexed");
    if(!bind || !draw)return E_NOINTERFACE;
    status=direct_functions(owner);if(FAILED(status))return status;
    status=rendering_functions(owner,FALSE);if(FAILED(status))return status;
    if(!owner->set_blend_constants)owner->set_blend_constants=(PFN_vkCmdSetBlendConstants)(uintptr_t)owner->get(owner->device,"vkCmdSetBlendConstants");
    if(!owner->set_blend_constants)return E_NOINTERFACE;
    status=recorded_begin(owner,family,pool,plan,completion,attempted,&command);if(FAILED(status))return status;
    VkBufferMemoryBarrier2 index_barrier={0};VkDependencyInfo index_dependency={0};
    index_barrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    index_barrier.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT;
    index_barrier.srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT;
    index_barrier.dstStageMask=VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;index_barrier.dstAccessMask=VK_ACCESS_2_INDEX_READ_BIT;
    index_barrier.srcQueueFamilyIndex=index_barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    index_barrier.buffer=buffer;index_barrier.offset=indexed->offset;index_barrier.size=indexed->bytes;
    index_dependency.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO;index_dependency.bufferMemoryBarrierCount=1;index_dependency.pBufferMemoryBarriers=&index_barrier;
    owner->rendering_barrier(command,&index_dependency);
    VkImageMemoryBarrier2 barriers[2]={{0}};VkDependencyInfo dependency={0};
    const VkImage images[]={image,depth_image};
    const VkImageLayout original[]={VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkImageLayout layouts[]={VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,attachment_layout(depth_range)};
    const VkPipelineStageFlags2 stages[]={VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT};
    const VkAccessFlags2 access[]={VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    const VkImageSubresourceRange ranges[]={range.barrier,depth_range.barrier};const uint32_t count=depth_view?2:1;
    for(uint32_t i=0;i<count;++i){
        barriers[i].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;barriers[i].srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[i].srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;barriers[i].dstStageMask=stages[i];barriers[i].dstAccessMask=access[i];
        barriers[i].srcQueueFamilyIndex=barriers[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image=images[i];barriers[i].oldLayout=original[i];barriers[i].newLayout=layouts[i];barriers[i].subresourceRange=ranges[i];
    }
    dependency.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO;dependency.imageMemoryBarrierCount=count;dependency.pImageMemoryBarriers=barriers;
    owner->rendering_barrier(command,&dependency);
    const VkRenderingAttachmentInfo color={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,NULL,view,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_RESOLVE_MODE_NONE,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE,{{0}}};
    VkRenderingAttachmentInfo depth=color;depth.imageView=depth_view;depth.imageLayout=layouts[1];
    VkRenderingInfo rendering={0};rendering.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;rendering.renderArea.extent=extent;rendering.layerCount=range.framebuffer_layers;
    rendering.colorAttachmentCount=1;rendering.pColorAttachments=&color;rendering.pDepthAttachment=depth_view?&depth:NULL;
    rendering.pStencilAttachment=depth_view && (depth_range.barrier.aspectMask&VK_IMAGE_ASPECT_STENCIL_BIT)?&depth:NULL;
    owner->begin_rendering(command,&rendering);record_direct_state(owner,command,pipeline,state,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);record_blend_constants(owner,command,constants);
    bind(command,buffer,indexed->offset,indexed->format==57?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32);
    draw(command,indexed->count,indexed->instances,indexed->first_index,indexed->base_vertex,indexed->first_instance);
    owner->end_rendering(command);
    for(uint32_t i=0;i<count;++i){barriers[i].srcStageMask=stages[i];barriers[i].srcAccessMask=access[i];barriers[i].dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[i].dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;barriers[i].oldLayout=layouts[i];barriers[i].newLayout=original[i];}
    owner->rendering_barrier(command,&dependency);
    index_barrier.srcStageMask=VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;index_barrier.srcAccessMask=VK_ACCESS_2_INDEX_READ_BIT;
    index_barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT;
    index_barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT;
    owner->rendering_barrier(command,&index_dependency);
    return recorded_submit(owner,command,plan,completion,attempted);
}


static void record_graphics_step(struct queue_owner *owner,VkCommandBuffer command,const QLDX12_GRAPHICS_STEP *step,
    PFN_vkCmdBindIndexBuffer bind,PFN_vkCmdDrawIndexed draw,PFN_vkCmdDraw plain,PFN_vkCmdDrawIndirect indirect_draw,PFN_vkCmdDrawIndexedIndirect indirect_indexed,PFN_vkCmdBindIndexBuffer2KHR bind_index_range,PFN_vkCmdBindVertexBuffers2 bind_vertices,PFN_vkCmdPushConstants push_constants,VkDescriptorSet uniform_set)
{
    const VkPipeline pipeline=step->pipeline;const VkImage image=step->image,depth_image=step->depth_image;
    const VkImageView view=step->view,depth_view=step->depth_view;const VkExtent2D extent=step->extent;
    const QLDX12_ATTACHMENT_RANGE range=step->range,depth_range=step->depth_range;const VkBuffer buffer=step->buffer;
    const QLDX12_NC_DIRECT_STATE *state=&step->work.state;const QLDX12_NC_BLEND_CONSTANTS *constants=&step->work.constants;
    const BOOL indirect_index=step->work.kind==QLDX12_NC_INDIRECT_INDEXED_DRAW;
    const QLDX12_NC_INDEXED_WORK *indexed=(step->work.kind==1 || (indirect_index && step->work.indirect.count))?&step->work.indexed:NULL;
    const QLDX12_NC_DRAW_ARGUMENTS *arguments=&step->work.arguments;
    record_cbv_reads(owner,command,&step->cbvs,TRUE);
    for(uint32_t v=0;v<step->work.vertex_count;++v) {
        if(step->vertex_buffers[v]==VK_NULL_HANDLE)continue;
        VkBufferMemoryBarrier2 barrier={0};VkDependencyInfo dependency={0};barrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT;barrier.srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT;
        barrier.dstStageMask=VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;barrier.dstAccessMask=VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=step->vertex_buffers[v];barrier.offset=step->vertex_offsets[v];barrier.size=step->vertex_sizes[v];
        dependency.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO;dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&barrier;owner->rendering_barrier(command,&dependency);
    }
    const BOOL indirect=step->work.kind==QLDX12_NC_INDIRECT_DRAW || indirect_index;
    if (indirect && step->work.indirect.count) record_indirect_barrier(owner, command, step->indirect_buffer, &step->work.indirect, TRUE);
    VkBufferMemoryBarrier2 index_barrier={0};VkDependencyInfo index_dependency={0};
    if(indexed) {
    index_barrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    index_barrier.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT;
    index_barrier.srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT;
    index_barrier.dstStageMask=VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;index_barrier.dstAccessMask=VK_ACCESS_2_INDEX_READ_BIT;
    index_barrier.srcQueueFamilyIndex=index_barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    index_barrier.buffer=buffer;index_barrier.offset=indexed->offset;index_barrier.size=indexed->bytes;
    index_dependency.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO;index_dependency.bufferMemoryBarrierCount=1;index_dependency.pBufferMemoryBarriers=&index_barrier;
    owner->rendering_barrier(command,&index_dependency);
    }
    VkImageMemoryBarrier2 barriers[9]={{0}};VkDependencyInfo dependency={0};
    VkImage images[9]={image};VkImageSubresourceRange ranges[9]={range.barrier};
    VkImageLayout original[9],layouts[9];VkPipelineStageFlags2 stages[9];VkAccessFlags2 access[9];
    VkRenderingAttachmentInfo colors[8]={{0}},depth={0};
    const uint32_t colors_count=step->work.target_count,count=colors_count+(depth_view!=VK_NULL_HANDLE);
    for(uint32_t i=0;i<colors_count;++i) {
        if(i){images[i]=step->extra_images[i-1];ranges[i]=step->extra_ranges[i-1].barrier;}
        original[i]=layouts[i]=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        stages[i]=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;access[i]=VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        colors[i].sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;colors[i].imageView=i?step->extra_views[i-1]:view;
        colors[i].imageLayout=layouts[i];colors[i].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;colors[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    }
    if(depth_view) {
        const uint32_t i=colors_count;images[i]=depth_image;ranges[i]=depth_range.barrier;
        original[i]=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;layouts[i]=attachment_layout(depth_range);
        stages[i]=VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        access[i]=VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depth.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;depth.imageView=depth_view;depth.imageLayout=layouts[i];
        depth.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    }
    for(uint32_t i=0;i<count;++i){
        barriers[i].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;barriers[i].srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[i].srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;barriers[i].dstStageMask=stages[i];barriers[i].dstAccessMask=access[i];
        barriers[i].srcQueueFamilyIndex=barriers[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image=images[i];barriers[i].oldLayout=original[i];barriers[i].newLayout=layouts[i];barriers[i].subresourceRange=ranges[i];
    }
    dependency.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO;dependency.imageMemoryBarrierCount=count;dependency.pImageMemoryBarriers=barriers;
    owner->rendering_barrier(command,&dependency);
    VkRenderingInfo rendering={0};rendering.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;rendering.renderArea.extent=extent;rendering.layerCount=range.framebuffer_layers;
    rendering.colorAttachmentCount=colors_count;rendering.pColorAttachments=colors;rendering.pDepthAttachment=depth_view?&depth:NULL;
    rendering.pStencilAttachment=depth_view && (depth_range.barrier.aspectMask&VK_IMAGE_ASPECT_STENCIL_BIT)?&depth:NULL;
    owner->begin_rendering(command,&rendering);record_direct_state(owner,command,pipeline,state,step->topology);record_blend_constants(owner,command,constants);
    if(uniform_set)owner->bind_uniform_sets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,step->root_layout,0,1,&uniform_set,0,NULL);
    if(step->work.root_constants.words)push_constants(command,step->root_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,step->work.root_constants.words*4,step->work.root_constants.values);
    for(uint32_t v=0;v<step->work.vertex_count;++v)bind_vertices(command,step->vertex_slots[v],1,&step->vertex_buffers[v],&step->vertex_offsets[v],&step->vertex_sizes[v],&step->vertex_strides[v]);
    if(step->work.kind==1) {
        bind(command,buffer,indexed->offset,indexed->format==57?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32);
        draw(command,indexed->count,indexed->instances,indexed->first_index,indexed->base_vertex,indexed->first_instance);
    } else if(indirect) {
        if(indexed)bind_index_range(command,buffer,indexed->offset,indexed->bytes,indexed->format==57?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32);
        /* The GPU consumes each argument record; no CPU decode/readback. A
         * selected device without multiDrawIndirect executes single-record
         * calls; larger supported histories split at the actual reported limit. */
        uint32_t remaining=step->work.indirect.count;VkDeviceSize offset=step->work.indirect.offset;
        while(remaining){const uint32_t batch=remaining<step->indirect_limit?remaining:step->indirect_limit;
            if(indirect_index)indirect_indexed(command,step->indirect_buffer,offset,batch,step->work.indirect.stride);
            else indirect_draw(command,step->indirect_buffer,offset,batch,step->work.indirect.stride);
            remaining-=batch;offset+=(uint64_t)batch*step->work.indirect.stride;}
    } else {plain(command,arguments->vertices,arguments->instances,arguments->first_vertex,arguments->first_instance);}
    owner->end_rendering(command);
    if (indirect && step->work.indirect.count) record_indirect_barrier(owner, command, step->indirect_buffer, &step->work.indirect, FALSE);
    record_cbv_reads(owner,command,&step->cbvs,FALSE);
    for(uint32_t i=0;i<count;++i){barriers[i].srcStageMask=stages[i];barriers[i].srcAccessMask=access[i];barriers[i].dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[i].dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;barriers[i].oldLayout=layouts[i];barriers[i].newLayout=original[i];}
    owner->rendering_barrier(command,&dependency);
    if(indexed) {
    index_barrier.srcStageMask=VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;index_barrier.srcAccessMask=VK_ACCESS_2_INDEX_READ_BIT;
    index_barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT;
    index_barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT;
    owner->rendering_barrier(command,&index_dependency);
    }
    for(uint32_t v=0;v<step->work.vertex_count;++v) {
        if(step->vertex_buffers[v]==VK_NULL_HANDLE)continue;
        VkBufferMemoryBarrier2 barrier={0};VkDependencyInfo dependency={0};barrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask=VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;barrier.srcAccessMask=VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT;barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=step->vertex_buffers[v];barrier.offset=step->vertex_offsets[v];barrier.size=step->vertex_sizes[v];
        dependency.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO;dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&barrier;owner->rendering_barrier(command,&dependency);
    }

}
/* Query commands are outside dynamic rendering and may encompass complete
 * render-pass instances (Vulkan queries chapter). The exact node chain retains
 * every pool and writer. Initialization proofs precede any WAIT result copy. */
static void query_memory_barrier(PFN_vkCmdPipelineBarrier2 barrier,VkCommandBuffer command,
    VkPipelineStageFlags2 source,VkAccessFlags2 source_access,VkPipelineStageFlags2 destination,VkAccessFlags2 destination_access)
{
    VkMemoryBarrier2 memory={0};VkDependencyInfo dependency={0};
    memory.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;memory.srcStageMask=source;memory.srcAccessMask=source_access;
    memory.dstStageMask=destination;memory.dstAccessMask=destination_access;
    dependency.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO;dependency.memoryBarrierCount=1;dependency.pMemoryBarriers=&memory;
    barrier(command,&dependency);
}
HRESULT ql_dx12_queue_graphics_stream(uintptr_t handle,uint32_t family,VkCommandPool pool,uint32_t count,const QLDX12_GRAPHICS_STEP *steps,
    const QLDX12_QUEUE_PLAN *plan,QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted)
{
    struct queue_owner *owner=(void*)handle;VkCommandBuffer command=VK_NULL_HANDLE;HRESULT status;
    BOOL fills=FALSE,indexed=FALSE,plain=FALSE,indirect_work=FALSE,indirect_call=FALSE,indirect_indexed_call=FALSE,vertices=FALSE,root_constants=FALSE,queries=FALSE,reset=FALSE,begin_query=FALSE,end_query=FALSE,timestamp=FALSE,resolve=FALSE;
    if(!owner || !pool || !count || !steps || !plan || !completion || !attempted)return E_INVALIDARG;
    *attempted=0;completion->accepted=0;completion->outcome=QLDX12_QUEUE_FAILED;completion->completed=owner->completed;
    for(uint32_t i=0;i<count;++i) {
        const UINT kind=steps[i].work.kind;
        if(kind==QLDX12_NC_BUFFER_UAV_CLEAR) {
            if(!steps[i].fill_destination || !steps[i].work.fill.bytes || (steps[i].work.fill.offset&3) || (steps[i].work.fill.bytes&3))return E_INVALIDARG;
            fills=TRUE;continue;
        }
        if(kind>=QLDX12_NC_QUERY_TIMESTAMP && kind<=QLDX12_NC_QUERY_RESOLVE) {
            if(!steps[i].query_pool || (kind==QLDX12_NC_QUERY_RESOLVE && !steps[i].query_destination))return E_INVALIDARG;
            queries=TRUE;
            if(kind==QLDX12_NC_QUERY_BEGIN){reset=TRUE;begin_query=TRUE;}
            if(kind==QLDX12_NC_QUERY_TIMESTAMP){reset=TRUE;timestamp=TRUE;}
            if(kind==QLDX12_NC_QUERY_END)end_query=TRUE;
            if(kind==QLDX12_NC_QUERY_RESOLVE)resolve=TRUE;
            continue;
        }
        if((kind>1 && kind!=QLDX12_NC_INDIRECT_DRAW && kind!=QLDX12_NC_INDIRECT_INDEXED_DRAW) || !steps[i].pipeline || !steps[i].image || !steps[i].view)return E_INVALIDARG;
        if(kind==QLDX12_NC_INDIRECT_DRAW || kind==QLDX12_NC_INDIRECT_INDEXED_DRAW){indirect_work=TRUE;if(!steps[i].indirect_buffer || (steps[i].work.indirect.count && !steps[i].indirect_limit))return E_INVALIDARG;if(steps[i].work.indirect.count){if(kind==QLDX12_NC_INDIRECT_INDEXED_DRAW)indirect_indexed_call=TRUE;else indirect_call=TRUE;}}
        else if(kind)indexed=TRUE;else plain=TRUE;
        if(steps[i].work.vertex_count)vertices=TRUE;if(steps[i].work.root_constants.words)root_constants=TRUE;
    }
#define GET_QUERY(type,name,needed) type name=(needed)?(type)(uintptr_t)owner->get(owner->device,#name):NULL
    GET_QUERY(PFN_vkCmdResetQueryPool,vkCmdResetQueryPool,reset);
    GET_QUERY(PFN_vkCmdBeginQuery,vkCmdBeginQuery,begin_query);
    GET_QUERY(PFN_vkCmdEndQuery,vkCmdEndQuery,end_query);
    GET_QUERY(PFN_vkCmdWriteTimestamp2,vkCmdWriteTimestamp2,timestamp);
    GET_QUERY(PFN_vkCmdCopyQueryPoolResults,vkCmdCopyQueryPoolResults,resolve);
    GET_QUERY(PFN_vkCmdPipelineBarrier2,vkCmdPipelineBarrier2,queries||fills);
    GET_QUERY(PFN_vkCmdFillBuffer,vkCmdFillBuffer,fills);
#undef GET_QUERY
    if((reset&&!vkCmdResetQueryPool)||(begin_query&&!vkCmdBeginQuery)||(end_query&&!vkCmdEndQuery)||
        (timestamp&&!vkCmdWriteTimestamp2)||(resolve&&!vkCmdCopyQueryPoolResults)||((queries||fills)&&!vkCmdPipelineBarrier2)||(fills&&!vkCmdFillBuffer))return E_NOINTERFACE;
    PFN_vkCmdDrawIndirect draw_indirect=indirect_call?(PFN_vkCmdDrawIndirect)(uintptr_t)owner->get(owner->device,"vkCmdDrawIndirect"):NULL;
    if(indirect_call && !draw_indirect)return E_NOINTERFACE;
    PFN_vkCmdDrawIndexedIndirect draw_indexed_indirect=indirect_indexed_call?(PFN_vkCmdDrawIndexedIndirect)(uintptr_t)owner->get(owner->device,"vkCmdDrawIndexedIndirect"):NULL;
    PFN_vkCmdBindIndexBuffer2KHR bind_index_range=indirect_indexed_call?(PFN_vkCmdBindIndexBuffer2KHR)(uintptr_t)owner->get(owner->device,"vkCmdBindIndexBuffer2KHR"):NULL;
    if(indirect_indexed_call && (!draw_indexed_indirect || !bind_index_range))return E_NOINTERFACE;
    PFN_vkCmdBindIndexBuffer bind=indexed?(PFN_vkCmdBindIndexBuffer)(uintptr_t)owner->get(owner->device,"vkCmdBindIndexBuffer"):NULL;
    PFN_vkCmdDrawIndexed draw=indexed?(PFN_vkCmdDrawIndexed)(uintptr_t)owner->get(owner->device,"vkCmdDrawIndexed"):NULL;
    PFN_vkCmdDraw nonindexed=plain?(PFN_vkCmdDraw)(uintptr_t)owner->get(owner->device,"vkCmdDraw"):NULL;
    PFN_vkCmdBindVertexBuffers2 bind_vertices=vertices?(PFN_vkCmdBindVertexBuffers2)(uintptr_t)owner->get(owner->device,"vkCmdBindVertexBuffers2"):NULL;
    PFN_vkCmdPushConstants push_constants=root_constants?(PFN_vkCmdPushConstants)(uintptr_t)owner->get(owner->device,"vkCmdPushConstants"):NULL;
    if((root_constants && !push_constants) || (vertices && !bind_vertices) || (indexed && (!bind || !draw)) || (plain && !nonindexed))return E_NOINTERFACE;
    const BOOL graphics=indexed||plain||indirect_work;
    if(graphics) {
        status=direct_functions(owner);if(FAILED(status))return status;
        status=rendering_functions(owner,FALSE);if(FAILED(status))return status;
        if(!owner->set_blend_constants)owner->set_blend_constants=(PFN_vkCmdSetBlendConstants)(uintptr_t)owner->get(owner->device,"vkCmdSetBlendConstants");
        if(!owner->set_blend_constants)return E_NOINTERFACE;
    }
    VkDescriptorPool uniform_pool=VK_NULL_HANDLE;VkDescriptorSet *sets=NULL;
    if(graphics){status=prepare_uniform_sets(owner,count,&steps[0].cbvs,sizeof(*steps),&uniform_pool,&sets);if(FAILED(status))return status;}
    status=recorded_begin(owner,family,pool,plan,completion,attempted,&command);
    if(FAILED(status)){if(uniform_pool)owner->destroy_uniform_pool(owner->device,uniform_pool,NULL);if(sets)HeapFree(GetProcessHeap(),0,sets);return status;}
    owner->uniform_pool=uniform_pool;
    for(uint32_t i=0;i<count;++i) {
        const QLDX12_GRAPHICS_STEP *step=&steps[i];const QLDX12_NC_QUERY_WORK *q=&step->work.query;
        const UINT kind=step->work.kind;
        if(kind<=1 || kind==QLDX12_NC_INDIRECT_DRAW || kind==QLDX12_NC_INDIRECT_INDEXED_DRAW){record_graphics_step(owner,command,step,bind,draw,nonindexed,draw_indirect,draw_indexed_indirect,bind_index_range,bind_vertices,push_constants,sets?sets[i]:VK_NULL_HANDLE);continue;}
        if(kind==QLDX12_NC_BUFFER_UAV_CLEAR) {
            query_memory_barrier(vkCmdPipelineBarrier2,command,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT|VK_ACCESS_2_HOST_WRITE_BIT,
                VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdFillBuffer(command,step->fill_destination,step->work.fill.offset,step->work.fill.bytes,step->work.fill.value);
            query_memory_barrier(vkCmdPipelineBarrier2,command,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT);
            continue;
        }
        if(kind==QLDX12_NC_QUERY_BEGIN || kind==QLDX12_NC_QUERY_TIMESTAMP) {
            /* Reset/reuse cannot race earlier query reads or writers. This is
             * outside all rendering, including replay of a closed list. */
            query_memory_barrier(vkCmdPipelineBarrier2,command,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
            vkCmdResetQueryPool(command,step->query_pool,q->index,1);
            if(kind==QLDX12_NC_QUERY_BEGIN)vkCmdBeginQuery(command,step->query_pool,q->index,q->heap_kind==0?VK_QUERY_CONTROL_PRECISE_BIT:0);
            else vkCmdWriteTimestamp2(command,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,step->query_pool,q->index);
        } else if(kind==QLDX12_NC_QUERY_END)vkCmdEndQuery(command,step->query_pool,q->index);
        else {
            query_memory_barrier(vkCmdPipelineBarrier2,command,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT|VK_ACCESS_2_HOST_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyQueryPoolResults(command,step->query_pool,q->index,q->count,step->query_destination,q->offset,q->heap_kind==2?88:8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);
            query_memory_barrier(vkCmdPipelineBarrier2,command,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT);
        }
    }
    status=recorded_submit(owner,command,plan,completion,attempted);if(sets)HeapFree(GetProcessHeap(),0,sets);return status;
}

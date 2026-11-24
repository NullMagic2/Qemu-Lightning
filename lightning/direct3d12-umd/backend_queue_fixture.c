/* Module: lightning.direct3d12_umd.backend_queue_fixture
 * Purpose: Check production queue ownership through official Vulkan function types.
 * Created: 2026-10-24
 * Architecture: A modeled queue delays timeline completion and injects failures.
 * The actual backend_queue.c builds Vulkan structures and owns/drains resources.
 * No real ICD, GPU, Windows D3D12 runtime or OS fence is exercised by this model.
 */
#include "backend_queue.h"
#define CHECK(condition) do { if (!(condition)) ExitProcess(__LINE__); } while (0)

enum { DEVICE_TAG = 17, QUEUE_TAG = 19, SEMAPHORE_TAG = 23, OWNER_SENTINEL = 31,
       MISSING_NONE = 0, MISSING_SUBMIT = 1, MISSING_DESTROY = 2,
       FAIL_INITIAL_COUNTER = 7 };
static struct {
    uint64_t completed, submitted;
    unsigned created, destroyed, submitted_calls, drained, waited;
    unsigned missing;
    VkResult counter_status, submit_status, drain_status;
    BOOL fail_probe_after_submit;
    BOOL incorrect_wait_success;
} model;

static VkDevice device(void) { return (VkDevice)(uintptr_t)DEVICE_TAG; }
static VkQueue queue(void) { return (VkQueue)(uintptr_t)QUEUE_TAG; }
static VkSemaphore semaphore(void) { return (VkSemaphore)(uintptr_t)SEMAPHORE_TAG; }

static VKAPI_ATTR VkResult VKAPI_CALL create_semaphore(VkDevice value,
    const VkSemaphoreCreateInfo *info, const VkAllocationCallbacks *allocator, VkSemaphore *output)
{
    const VkSemaphoreTypeCreateInfo *type = info->pNext;

    CHECK(value == device() && !allocator && output && info);
    CHECK(info->sType == VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO && info->flags == 0);
    CHECK(type && type->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO && !type->pNext);
    CHECK(type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE && type->initialValue == 0);
    ++model.created;
    *output = semaphore();
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL destroy_semaphore(VkDevice value,
    VkSemaphore object, const VkAllocationCallbacks *allocator)
{
    CHECK(value == device() && object == semaphore() && !allocator);
    CHECK(model.submitted_calls == 0 || model.drained != 0);
    ++model.destroyed;
}

static VKAPI_ATTR VkResult VKAPI_CALL counter(VkDevice value, VkSemaphore object, uint64_t *output)
{
    CHECK(value == device() && object == semaphore() && output);
    if (model.counter_status != VK_SUCCESS)
        return model.counter_status;
    if (model.fail_probe_after_submit && model.submitted_calls)
        return VK_ERROR_DEVICE_LOST;
    *output = model.completed;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue value, uint32_t count,
    const VkSubmitInfo2 *info, VkFence fence)
{
    const VkSemaphoreSubmitInfo *signal = info->pSignalSemaphoreInfos;

    CHECK(value == queue() && count == 1 && info && fence == VK_NULL_HANDLE);
    CHECK(info->sType == VK_STRUCTURE_TYPE_SUBMIT_INFO_2 && !info->pNext && !info->flags);
    CHECK(!info->waitSemaphoreInfoCount && !info->pWaitSemaphoreInfos);
    CHECK(!info->commandBufferInfoCount && !info->pCommandBufferInfos);
    CHECK(info->signalSemaphoreInfoCount == 1 && signal);
    CHECK(signal->sType == VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO && !signal->pNext);
    CHECK(signal->semaphore == semaphore() && signal->value == model.submitted + 1);
    CHECK(signal->stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT && !signal->deviceIndex);
    ++model.submitted_calls;
    if (model.submit_status != VK_SUCCESS)
        return model.submit_status;
    model.submitted = signal->value;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice value, const VkSemaphoreWaitInfo *info,
    uint64_t timeout)
{
    CHECK(value == device() && info && info->sType == VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
    CHECK(!info->pNext && !info->flags && info->semaphoreCount == 1);
    CHECK(info->pSemaphores && info->pSemaphores[0] == semaphore());
    CHECK(info->pValues && timeout <= QLDX12_QUEUE_MAX_WAIT_NS);
    ++model.waited;
    return model.completed >= info->pValues[0] || model.incorrect_wait_success ? VK_SUCCESS : VK_TIMEOUT;
}

static VKAPI_ATTR VkResult VKAPI_CALL drain(VkQueue value)
{
    CHECK(value == queue());
    if (model.drain_status == VK_SUCCESS || model.drain_status == VK_ERROR_DEVICE_LOST) {
        ++model.drained;
        model.completed = model.submitted;
    }
    return model.drain_status;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get(VkDevice value, const char *name)
{
    CHECK(value == device());
#define PROC(label, function) if (lstrcmpA(name, label) == 0) return (PFN_vkVoidFunction)(uintptr_t)&function
    PROC("vkCreateSemaphore", create_semaphore);
    if (model.missing != MISSING_DESTROY) { PROC("vkDestroySemaphore", destroy_semaphore); }
    PROC("vkGetSemaphoreCounterValue", counter);
    if (model.missing != MISSING_SUBMIT) { PROC("vkQueueSubmit2", submit); }
    PROC("vkWaitSemaphores", wait);
    PROC("vkQueueWaitIdle", drain);
#undef PROC
    return NULL;
}

static uintptr_t create(void)
{
    uintptr_t owner = OWNER_SENTINEL;

    CHECK(ql_dx12_queue_create(device(), queue(), get, destroy_semaphore, &owner) == S_OK);
    CHECK(owner != 0 && owner != OWNER_SENTINEL && model.created == 1);
    return owner;
}

static QLDX12_QUEUE_PLAN plan(uint32_t operation, uint64_t value)
{
    QLDX12_QUEUE_PLAN result = {0};

    result.operation = operation;
    result.value = value;
    return result;
}

static void clear_model(void) { ZeroMemory(&model, sizeof(model)); }

static void create_failure_preserves_outputs_and_cleans_partial_resources(void)
{
    uintptr_t owner = OWNER_SENTINEL;

    clear_model();
    model.missing = MISSING_SUBMIT;
    CHECK(ql_dx12_queue_create(device(), queue(), get, destroy_semaphore, &owner) == E_NOINTERFACE);
    CHECK(owner == OWNER_SENTINEL && !model.created && !model.destroyed);
    model.missing = MISSING_NONE;
    model.completed = FAIL_INITIAL_COUNTER;
    CHECK(ql_dx12_queue_create(device(), queue(), get, destroy_semaphore, &owner) == E_FAIL);
    CHECK(owner == OWNER_SENTINEL && model.created == 1 && model.destroyed == 1);
    clear_model();
    model.missing = MISSING_DESTROY;
    CHECK(ql_dx12_queue_create(device(), queue(), get, NULL, &owner) == E_NOINTERFACE);
    CHECK(owner == OWNER_SENTINEL && model.created == 0);
    owner = create();
    CHECK(ql_dx12_queue_destroy(owner) == S_OK && model.destroyed == 1);
}

static void submit_acceptance_is_not_timeline_completion(void)
{
    uintptr_t owner;
    QLDX12_QUEUE_COMPLETION completion;
    QLDX12_QUEUE_PLAN command;

    clear_model();
    owner = create();
    command = plan(QLDX12_QUEUE_SIGNAL, 1);
    CHECK(ql_dx12_queue_execute(owner, &command, &completion) == S_OK);
    CHECK(completion.outcome == QLDX12_QUEUE_SUCCESS && completion.accepted == 1 && !completion.completed);
    command = plan(QLDX12_QUEUE_WAIT, 1);
    command.timeout_ns = QLDX12_QUEUE_MAX_WAIT_NS;
    CHECK(ql_dx12_queue_execute(owner, &command, &completion) == DXGI_ERROR_WAIT_TIMEOUT);
    CHECK(completion.outcome == QLDX12_QUEUE_TIMEOUT && !completion.accepted && !completion.completed);
    model.completed = 1;
    CHECK(ql_dx12_queue_execute(owner, &command, &completion) == S_OK);
    CHECK(completion.completed == 1 && model.waited == 2);
    command = plan(QLDX12_QUEUE_SIGNAL, 1);
    CHECK(ql_dx12_queue_execute(owner, &command, &completion) == E_INVALIDARG);
    CHECK(ql_dx12_queue_destroy(owner) == S_OK);
    CHECK(model.drained == 1 && model.destroyed == 1);
}

static void accepted_submit_then_probe_failure_retains_queue_for_drain(void)
{
    uintptr_t owner;
    QLDX12_QUEUE_COMPLETION completion;
    QLDX12_QUEUE_PLAN command = plan(QLDX12_QUEUE_SIGNAL, 1);

    clear_model();
    owner = create();
    model.fail_probe_after_submit = TRUE;
    CHECK(ql_dx12_queue_execute(owner, &command, &completion) == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(completion.accepted == 1 && completion.outcome == QLDX12_QUEUE_FAILED);
    model.drain_status = VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(ql_dx12_queue_destroy(owner) == E_OUTOFMEMORY);
    CHECK(model.destroyed == 0 && model.drained == 0);
    model.drain_status = VK_SUCCESS;
    CHECK(ql_dx12_queue_destroy(owner) == S_OK && model.destroyed == 1 && model.drained == 1);
}

static void unsuccessful_wait_or_impossible_counter_never_reports_success(void)
{
    uintptr_t owner;
    QLDX12_QUEUE_COMPLETION completion;
    QLDX12_QUEUE_PLAN command = plan(QLDX12_QUEUE_SIGNAL, 1);

    clear_model();
    owner = create();
    CHECK(ql_dx12_queue_execute(owner, &command, &completion) == S_OK);
    command = plan(QLDX12_QUEUE_WAIT, 1);
    model.incorrect_wait_success = TRUE;
    CHECK(ql_dx12_queue_execute(owner, &command, &completion) == E_FAIL);
    CHECK(completion.outcome == QLDX12_QUEUE_FAILED);
    command = plan(QLDX12_QUEUE_QUERY, 0);
    model.completed = 2;
    CHECK(ql_dx12_queue_execute(owner, &command, &completion) == E_FAIL);
    CHECK(ql_dx12_queue_destroy(owner) == S_OK && model.destroyed == 1);
}

static void failed_submit_and_device_loss_still_clean_up_in_order(void)
{
    uintptr_t owner;
    QLDX12_QUEUE_COMPLETION completion;
    QLDX12_QUEUE_PLAN command = plan(QLDX12_QUEUE_SIGNAL, 1);

    clear_model();
    owner = create();
    model.submit_status = VK_ERROR_DEVICE_LOST;
    CHECK(ql_dx12_queue_execute(owner, &command, &completion) == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(completion.accepted == 0 && completion.outcome == QLDX12_QUEUE_FAILED);
    model.drain_status = VK_ERROR_DEVICE_LOST;
    CHECK(ql_dx12_queue_destroy(owner) == S_OK && model.destroyed == 1 && model.drained == 1);
}

void fixture_entry(void)
{
    create_failure_preserves_outputs_and_cleans_partial_resources();
    submit_acceptance_is_not_timeline_completion();
    accepted_submit_then_probe_failure_retains_queue_for_drain();
    unsuccessful_wait_or_impossible_counter_never_reports_success();
    failed_submit_and_device_loss_still_clean_up_in_order();
    ExitProcess(0);
}

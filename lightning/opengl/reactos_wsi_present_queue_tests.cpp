/*
Module: qemu_lightning.opengl.reactos_wsi_present_queue_tests
Purpose: Verify fence-ordered multi-image presentation, its bounded queue and per-image receipts.
Created: 2026-10-23
Architecture: Drives the production run_present_worker with Win32 primitives in
place of Mesa's image-state mutex and condition, a model producer fence per
image and a model native present that registers one lease per image and
records its source-read receipt. The async-present cases drive the production
wait_receipt/settle_receipt/prefer_idle_image policy and revision-2 encoder
against a model host copy per image. The frame-waits cases drive the production
frame pipeline policy (reactos_frame_pipeline.hpp): its depth, surface minimum,
end-of-frame fence ring and the worker-retired present fence protocol against a
model Vulkan fence state per image. Built with GCC13 MinGW and run under Wine.
*/
#include "reactos_wsi_present_queue.hpp"
#include "reactos_wsi_gpu.hpp"
#include "reactos_frame_pipeline.hpp"
#include <windows.h>
#include <cstdio>
#include <initializer_list>

namespace {
using namespace qemu_lightning::opengl::nt5;
unsigned checks = 0;
unsigned failures = 0;
constexpr DWORD settle_ms = 50;
constexpr DWORD bounded_wait_ms = 5000;

void check(bool condition, const char* description) {
    ++checks;

    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

enum class ImageState { idle, drawing, queued };

// Leases are a process-wide model of the native per-image registrations.
struct LeaseLedger {
    std::uint64_t next = 1;
    unsigned live = 0;
};

struct ModelChain {
    CRITICAL_SECTION mutex{};
    CONDITION_VARIABLE work = CONDITION_VARIABLE_INIT;
    CONDITION_VARIABLE idle = CONDITION_VARIABLE_INIT;
    PresentQueue queue{};
    std::uint32_t image_count = 0;
    ImageState state[maximum_swapchain_images]{};
    HANDLE fence[maximum_swapchain_images]{};
    std::uint64_t lease[maximum_swapchain_images]{};
    std::uint32_t order[256]{};
    unsigned presented = 0;
    unsigned receipts = 0;
    unsigned registrations = 0;
    LeaseLedger* ledger = nullptr;
    HANDLE thread = nullptr;
};

struct ModelOps {
    ModelChain* chain;

    void lock() { EnterCriticalSection(&chain->mutex); }
    void unlock() { LeaveCriticalSection(&chain->mutex); }
    void wait() { SleepConditionVariableCS(&chain->work, &chain->mutex, INFINITE); }

    // Mirrors wsi_win32_present_image: own fence, one registration, receipt,
    // then the idle transition under the image-state mutex.
    void present(std::uint32_t index) {
        WaitForSingleObject(chain->fence[index], INFINITE);
        EnterCriticalSection(&chain->mutex);

        if (chain->lease[index] == 0) {
            chain->lease[index] = chain->ledger->next++;
            ++chain->ledger->live;
            ++chain->registrations;
        }
        if (chain->presented < 256) {
            chain->order[chain->presented] = index;
        }
        ++chain->presented;
        ++chain->receipts;
        chain->state[index] = ImageState::idle;
        WakeAllConditionVariable(&chain->idle);
        LeaveCriticalSection(&chain->mutex);
    }
};

DWORD WINAPI worker_entry(void* data) {
    ModelOps ops{static_cast<ModelChain*>(data)};
    run_present_worker(ops.chain->queue, ops);
    return 0;
}

void open_chain(ModelChain& chain, std::uint32_t images, LeaseLedger& ledger) {
    InitializeCriticalSection(&chain.mutex);
    chain.image_count = images;
    chain.ledger = &ledger;

    for (std::uint32_t index = 0; index < images; ++index) {
        chain.fence[index] = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    }
    chain.thread = CreateThread(nullptr, 0, worker_entry, &chain, 0, nullptr);
    check(chain.thread != nullptr, "presentation worker starts");
}

// Mirrors wsi_win32_enqueue_present.
bool enqueue(ModelChain& chain, std::uint32_t index) {
    EnterCriticalSection(&chain.mutex);
    const bool queued = present_queue_push(chain.queue, index, chain.image_count);

    if (queued) {
        chain.state[index] = ImageState::queued;
        WakeConditionVariable(&chain.work);
    }
    LeaveCriticalSection(&chain.mutex);
    return queued;
}

// Mirrors wsi_win32_acquire_idle_cpu_image: block until any image is idle.
bool acquire(ModelChain& chain, std::uint32_t& index, DWORD timeout_ms) {
    EnterCriticalSection(&chain.mutex);

    for (;;) {
        for (std::uint32_t candidate = 0; candidate < chain.image_count; ++candidate) {
            if (chain.state[candidate] == ImageState::idle) {
                chain.state[candidate] = ImageState::drawing;
                index = candidate;
                LeaveCriticalSection(&chain.mutex);
                return true;
            }
        }
        if (!SleepConditionVariableCS(&chain.idle, &chain.mutex, timeout_ms)) {
            LeaveCriticalSection(&chain.mutex);
            return false;
        }
    }
}

// Mirrors the swapchain destroy hook, then retires the per-image leases.
bool close_chain(ModelChain& chain, DWORD timeout_ms) {
    EnterCriticalSection(&chain.mutex);
    chain.queue.stopping = true;
    WakeAllConditionVariable(&chain.work);
    LeaveCriticalSection(&chain.mutex);

    if (WaitForSingleObject(chain.thread, timeout_ms) != WAIT_OBJECT_0) {
        return false;
    }
    CloseHandle(chain.thread);

    for (std::uint32_t index = 0; index < chain.image_count; ++index) {
        if (chain.lease[index] != 0) {
            --chain.ledger->live;
            chain.lease[index] = 0;
        }
        CloseHandle(chain.fence[index]);
    }
    DeleteCriticalSection(&chain.mutex);
    return true;
}

void check_queue() {
    PresentQueue queue{};
    std::uint32_t index = 0;
    check(!present_queue_pop(queue, index), "zeroed queue is empty");
    check(present_queue_push(queue, 0, 2) && present_queue_push(queue, 1, 2),
        "every image of a chain can be queued once");
    check(!present_queue_push(queue, 1, 2), "an image cannot be queued twice");
    check(!present_queue_push(queue, 2, 2), "an out-of-range image index is refused");
    check(present_queue_push(queue, 2, 3) && queue.count == 3, "a larger chain queues each image once");
    check(!present_queue_push(queue, 0, 2), "a queue already holding the whole chain refuses more");
    check(present_queue_pop(queue, index) && index == 0, "queue preserves presentation order");
    check(!present_queue_push(queue, 0, maximum_swapchain_images + 1), "oversized chain is refused");
    queue = PresentQueue{};
    check(present_queue_push(queue, 1, 3) && !present_queue_push(queue, 1, 3) && queue.count == 1,
        "an already queued image is refused even while the queue has room");
    queue = PresentQueue{};
    unsigned in_order = 0;

    for (unsigned round = 0; round < 1000; ++round) {
        const std::uint32_t pushed = round % maximum_swapchain_images;
        std::uint32_t popped = UINT32_MAX;

        if (present_queue_push(queue, pushed, maximum_swapchain_images) &&
            (round == 0 || (present_queue_pop(queue, popped) &&
                            popped == (round - 1) % maximum_swapchain_images))) {
            ++in_order;
        }
    }
    check(in_order == 1000 && queue.count == 1, "ring indices wrap in order without loss over many frames");
    queue.stopping = true;
    check(!present_queue_push(queue, 3, maximum_swapchain_images), "a stopping queue accepts no present");
    check(!use_present_worker(1) && use_present_worker(2) && use_present_worker(maximum_swapchain_images) &&
        !use_present_worker(maximum_swapchain_images + 1), "worker serves exactly the bounded multi-image chains");
    check(minimum_swapchain_images == 2 && minimum_swapchain_images <= maximum_swapchain_images,
        "surface reports a two-image minimum within the bounded maximum");
}

void check_aperture_cost() {
    std::uint64_t bytes = 0;
    check(present_image_aperture_bytes(640, 480, exported_linear_pitch_alignment, 4096, bytes) &&
        bytes == 1228800, "640x480 image maps 1.17 MiB");
    check(present_image_aperture_bytes(800, 600, exported_linear_pitch_alignment, 4096, bytes) &&
        bytes == 1998848, "800x600 pitch rounds to 3328 bytes, then pages");
    check(present_image_aperture_bytes(1024, 768, exported_linear_pitch_alignment, 4096, bytes) &&
        bytes == 3145728, "1024x768 image maps 3 MiB");
    check(present_image_aperture_bytes(1920, 1080, exported_linear_pitch_alignment, 4096, bytes) &&
        bytes == 8294400, "1920x1080 image maps 7.9 MiB");
    check(!present_image_aperture_bytes(0, 480, exported_linear_pitch_alignment, 4096, bytes) && bytes == 0,
        "empty extent is rejected");
}

void check_fence_ordering() {
    LeaseLedger ledger;
    ModelChain chain;
    open_chain(chain, 2, ledger);
    std::uint32_t a = 0, b = 0;
    check(acquire(chain, a, 0) && acquire(chain, b, 0) && a != b, "both images are acquirable at once");
    ResetEvent(chain.fence[b]);
    check(enqueue(chain, a) && enqueue(chain, b), "both frames are accepted without waiting on the GPU");
    std::uint32_t reused = UINT32_MAX;
    check(acquire(chain, reused, bounded_wait_ms) && reused == a,
        "reusing the first image waits only for its own fence and receipt, not the later frame's");
    Sleep(settle_ms);
    EnterCriticalSection(&chain.mutex);
    const bool held = chain.state[b] == ImageState::queued && chain.presented == 1 && chain.order[0] == a;
    LeaveCriticalSection(&chain.mutex);
    check(held, "the later image still waits for its own producer fence");
    std::uint32_t none = UINT32_MAX;
    check(!acquire(chain, none, settle_ms), "reusing the pending image blocks until its present completes");
    SetEvent(chain.fence[b]);
    check(acquire(chain, none, bounded_wait_ms) && none == b, "its fence and receipt release exactly that image");
    check(chain.order[1] == b && chain.receipts == 2, "frames present in order with one receipt each");
    check(close_chain(chain, bounded_wait_ms) && ledger.live == 0, "closing retires every image lease");
}

void check_stop_drains() {
    LeaseLedger ledger;
    ModelChain chain;
    open_chain(chain, 2, ledger);
    std::uint32_t index = 0;
    check(acquire(chain, index, 0), "drain fixture acquires an image");
    ResetEvent(chain.fence[index]);
    check(enqueue(chain, index), "drain fixture queues a frame whose GPU work is pending");
    EnterCriticalSection(&chain.mutex);
    chain.queue.stopping = true;
    WakeAllConditionVariable(&chain.work);
    LeaveCriticalSection(&chain.mutex);
    check(WaitForSingleObject(chain.thread, settle_ms) == WAIT_TIMEOUT,
        "teardown waits for an accepted present instead of abandoning it");
    SetEvent(chain.fence[index]);
    check(close_chain(chain, bounded_wait_ms) && chain.presented == 1 && chain.receipts == 1,
        "the accepted present completes exactly once before the worker exits");
    check(ledger.live == 0, "drained teardown leaves no lease behind");
}

void check_mode_change_and_reopen() {
    LeaseLedger ledger;
    ModelChain old_chain;
    open_chain(old_chain, 2, ledger);

    for (unsigned frame = 0; frame < 100; ++frame) {
        std::uint32_t index = 0;
        check(acquire(old_chain, index, bounded_wait_ms) && enqueue(old_chain, index),
            "steady frames cycle through both images");
    }
    Sleep(settle_ms);
    check(old_chain.registrations == 2 && ledger.live == 2, "each image registers its lease once");
    ModelChain new_chain;
    open_chain(new_chain, 3, ledger);
    std::uint32_t index = 0;
    check(acquire(new_chain, index, 0) && enqueue(new_chain, index), "replacement chain presents");
    check(close_chain(old_chain, bounded_wait_ms), "old chain retires after a mode change");
    Sleep(settle_ms);
    check(ledger.live == 1 && old_chain.receipts == 100, "mode change retires only the old leases");
    check(close_chain(new_chain, bounded_wait_ms) && ledger.live == 0, "close leaves no lease behind");
    ModelChain reopened;
    open_chain(reopened, 2, ledger);
    check(acquire(reopened, index, 0) && enqueue(reopened, index), "reopened chain presents");
    Sleep(settle_ms);
    check(reopened.lease[index] > 3, "reopen registers fresh lease identities");
    check(close_chain(reopened, bounded_wait_ms) && ledger.live == 0, "reopen and close balance leases");
}

// async-present: scripted receipt answers on a fake clock exercise the exact
// production wait_receipt/settle_receipt pacing without real time.
struct ScriptedReceipt {
    ReceiptState answers[8]{};
    unsigned count = 0;
    unsigned queries = 0;
    unsigned yields = 0;
    unsigned sleeps = 0;
    std::uint64_t clock = 0;

    ReceiptState query() {
        const ReceiptState state = answers[queries < count ? queries : count - 1];
        ++queries;
        return state;
    }
    void yield() { ++yields; clock += 1000; }
    void sleep_ms(std::uint32_t milliseconds) { ++sleeps; clock += std::uint64_t{milliseconds} * 1000000; }
    std::uint64_t now_ns() const { return clock; }
};

ScriptedReceipt script(std::initializer_list<ReceiptState> states) {
    ScriptedReceipt result{};

    for (const auto state : states) {
        result.answers[result.count++] = state;
    }
    return result;
}

void check_receipt_policy() {
    auto immediate = script({ReceiptState::retired});
    check(wait_receipt(immediate, UINT64_MAX) == ReceiptWait::retired && immediate.queries == 1 &&
        immediate.yields == 0 && immediate.sleeps == 0, "a retired receipt costs one query and no pause");
    auto later = script({ReceiptState::pending, ReceiptState::pending, ReceiptState::retired});
    check(wait_receipt(later, UINT64_MAX) == ReceiptWait::retired && later.queries == 3 && later.yields == 2 &&
        later.sleeps == 0, "a briefly pending receipt is polled with yields first");
    std::uint64_t until = 0;
    {
        // Retire after the yield budget so sleeps begin.
        struct Late : ScriptedReceipt {
            ReceiptState query() { return ++queries > receipt_yield_polls + 3 ? ReceiptState::retired : ReceiptState::pending; }
        } late{};
        check(wait_receipt(late, UINT64_MAX) == ReceiptWait::retired && late.yields == receipt_yield_polls &&
            late.sleeps == 3, "after the yield budget the wait sleeps one millisecond per poll");
        until = late.clock;
    }
    check(until < receipt_stall_limit_ns, "a slow but progressing read finishes well inside the stall bound");
    auto failed = script({ReceiptState::pending, ReceiptState::failed});
    check(wait_receipt(failed, UINT64_MAX) == ReceiptWait::failed && failed.queries == 2,
        "a failed receipt ends the wait as device loss");
    auto poll_once = script({ReceiptState::pending});
    check(wait_receipt(poll_once, 0) == ReceiptWait::timeout && poll_once.queries == 1,
        "a zero acquire timeout polls exactly once");
    auto bounded = script({ReceiptState::pending});
    check(wait_receipt(bounded, UINT64_C(5000000)) == ReceiptWait::timeout && bounded.clock >= UINT64_C(5000000) &&
        bounded.clock < receipt_stall_limit_ns, "a caller deadline is honored before the stall bound");
    auto stuck = script({ReceiptState::pending});
    check(wait_receipt(stuck, UINT64_MAX) == ReceiptWait::unavailable && stuck.clock >= receipt_stall_limit_ns,
        "a read that never retires ends at the stall bound instead of hanging acquire");
    auto silent = script({ReceiptState::unavailable});
    check(wait_receipt(silent, UINT64_MAX) == ReceiptWait::unavailable && silent.clock >= receipt_stall_limit_ns,
        "a display that cannot answer is retried within the same bound");
    auto recovered = script({ReceiptState::unavailable, ReceiptState::unavailable, ReceiptState::retired});
    check(wait_receipt(recovered, UINT64_MAX) == ReceiptWait::retired, "an answer after a transition still retires");

    bool pending = false;
    auto untouched = script({ReceiptState::failed});
    check(settle_receipt(untouched, pending) && untouched.queries == 0, "an image without a receipt settles at once");
    pending = true;
    auto drained = script({ReceiptState::pending, ReceiptState::retired});
    check(settle_receipt(drained, pending) && !pending && drained.queries == 2,
        "destruction waits for a pending receipt and clears its mark");
    pending = true;
    auto abandoned = script({ReceiptState::pending});
    check(!settle_receipt(abandoned, pending) && !pending && abandoned.clock >= receipt_destroy_timeout_ns,
        "destruction gives up only after its bound; the native filter still pins the memory");

    check(prefer_idle_image(false, 9, true, 1) && !prefer_idle_image(true, 1, false, 9),
        "an image without an outstanding copy is preferred");
    check(prefer_idle_image(true, 1, true, 2) && !prefer_idle_image(true, 2, true, 1),
        "among pending images the earliest queued copy is preferred");
    check(!prefer_idle_image(false, 1, false, 2) && !prefer_idle_image(false, 2, false, 1),
        "retired images keep the first-found order");
}

// frame-waits: the retired steady state must not read NT5's PIT-backed clock.
struct ClockedReceipt : ScriptedReceipt {
    mutable unsigned clock_reads = 0;
    std::uint64_t now_ns() const { ++clock_reads; return clock; }
};

void check_receipt_clock() {
    ClockedReceipt retired{};
    retired.answers[retired.count++] = ReceiptState::retired;
    check(wait_receipt(retired, UINT64_MAX) == ReceiptWait::retired && retired.queries == 1 &&
        retired.clock_reads == 0, "an already retired receipt costs one query and no clock read");
    ClockedReceipt failed{};
    failed.answers[failed.count++] = ReceiptState::failed;
    check(wait_receipt(failed, UINT64_MAX) == ReceiptWait::failed && failed.clock_reads == 0,
        "an immediately failed receipt reads no clock either");
    ClockedReceipt later{};
    for (const auto state : {ReceiptState::pending, ReceiptState::pending, ReceiptState::retired}) {
        later.answers[later.count++] = state;
    }
    check(wait_receipt(later, UINT64_MAX) == ReceiptWait::retired && later.queries == 3 && later.yields == 2 &&
        later.clock_reads == 2, "a pending receipt starts the clock at its first non-retired answer");
    ClockedReceipt bounded{};
    bounded.answers[bounded.count++] = ReceiptState::pending;
    bounded.clock = UINT64_C(1000000000);  // A monotonic clock rarely starts at zero.
    check(wait_receipt(bounded, UINT64_C(5000)) == ReceiptWait::timeout &&
        bounded.clock >= UINT64_C(1000005000) && bounded.queries > 1,
        "the deadline is measured from the first pending answer");
}

void check_frame_pipeline_policy() {
    check(ql_nt5_frame_pipeline_parse(nullptr, 0) == QL_NT5_FRAME_PIPELINE_DEFAULT &&
        ql_nt5_frame_pipeline_parse("", 0) == QL_NT5_FRAME_PIPELINE_DEFAULT,
        "an unset pipeline variable selects the default depth");
    check(ql_nt5_frame_pipeline_parse("0", 1) == QL_NT5_FRAME_PIPELINE_OFF,
        "QEMU_LIGHTNING_FRAME_PIPELINE=0 restores the per-frame wait");
    check(ql_nt5_frame_pipeline_parse("1", 1) == 1 && ql_nt5_frame_pipeline_parse("4", 1) == 4,
        "a single digit selects the depth up to the bound");
    check(ql_nt5_frame_pipeline_parse("5", 1) == QL_NT5_FRAME_PIPELINE_DEFAULT &&
        ql_nt5_frame_pipeline_parse("10", 2) == QL_NT5_FRAME_PIPELINE_DEFAULT &&
        ql_nt5_frame_pipeline_parse("x", 1) == QL_NT5_FRAME_PIPELINE_DEFAULT,
        "oversized or malformed values keep the default instead of disabling");
    check(QL_NT5_FRAME_PIPELINE_DEFAULT == 2 && QL_NT5_FRAME_PIPELINE_IMAGES == 3 &&
        QL_NT5_FRAME_PIPELINE_IMAGES <= maximum_swapchain_images, "default depth two, three images, bounded");

    check(ql_nt5_frame_pipeline_min_images(minimum_swapchain_images, maximum_swapchain_images,
        maximum_swapchain_images, 2) == QL_NT5_FRAME_PIPELINE_IMAGES,
        "a pipelined CPU-image surface advertises three images");
    check(ql_nt5_frame_pipeline_min_images(minimum_swapchain_images, maximum_swapchain_images,
        maximum_swapchain_images, QL_NT5_FRAME_PIPELINE_OFF) == minimum_swapchain_images,
        "pipeline off keeps the two-image minimum");
    check(ql_nt5_frame_pipeline_min_images(2, 2, maximum_swapchain_images, 2) == 2 &&
        ql_nt5_frame_pipeline_min_images(2, 8, maximum_swapchain_images, 2) == 2,
        "only the bounded CPU-image surface changes: DXGI and other maxima are unchanged");
    check(ql_nt5_frame_pipeline_min_images(4, maximum_swapchain_images, maximum_swapchain_images, 2) == 4,
        "a larger platform minimum is never lowered");

    check(ql_nt5_frame_pipeline_retire_fence(true, 2) && ql_nt5_frame_pipeline_retire_fence(true, 1) &&
        !ql_nt5_frame_pipeline_retire_fence(true, QL_NT5_FRAME_PIPELINE_OFF) &&
        !ql_nt5_frame_pipeline_retire_fence(false, 2),
        "only the pipelined present worker retires image fences");
    bool retired = true;
    check(ql_nt5_frame_pipeline_take_retired(&retired) && !retired && !ql_nt5_frame_pipeline_take_retired(&retired),
        "a retired mark is consumed by exactly one later present");

    // The end-of-frame ring: frame N waits frame N - depth, never itself.
    int frames[16]{};
    for (unsigned depth = 1; depth <= QL_NT5_FRAME_PIPELINE_MAXIMUM; ++depth) {
        ql_nt5_frame_ring ring{};
        bool exact = true;

        for (int frame = 0; frame < 16; ++frame) {
            void* waited = ql_nt5_frame_ring_push(&ring, &frames[frame], depth);
            void* expected = frame >= static_cast<int>(depth) ? &frames[frame - depth] : nullptr;
            exact = exact && waited == expected && ring.count == (frame + 1u < depth ? frame + 1u : depth);
        }
        unsigned released = 0;
        while (ring.count) {
            exact = exact && ql_nt5_frame_ring_pop(&ring) == &frames[16 - depth + released];
            ++released;
        }
        exact = exact && released == depth && ql_nt5_frame_ring_pop(&ring) == nullptr;
        char description[96];
        std::snprintf(description, sizeof(description),
            "depth %u: frame N waits exactly frame N-%u and teardown releases the rest in order", depth, depth);
        check(exact, description);
    }
    ql_nt5_frame_ring clamped{};
    for (int frame = 0; frame < 8; ++frame) {
        (void)ql_nt5_frame_ring_push(&clamped, &frames[frame], 99);
    }
    check(clamped.count == QL_NT5_FRAME_PIPELINE_MAXIMUM, "an oversized depth is clamped to the ring bound");
    ql_nt5_frame_ring nulls{};
    check(ql_nt5_frame_ring_push(&nulls, nullptr, 1) == nullptr && nulls.count == 1 &&
        ql_nt5_frame_ring_push(&nulls, &frames[0], 1) == nullptr && nulls.count == 1 &&
        ql_nt5_frame_ring_pop(&nulls) == &frames[0], "a frame without a fence still counts as one frame");
    check(ql_nt5_frame_ring_push(&nulls, &frames[1], 0) == nullptr && nulls.count == 1 &&
        ql_nt5_frame_ring_push(&nulls, &frames[2], 0) == &frames[1], "depth zero behaves as depth one");
}

// frame-waits: Vulkan state of one per-image present fence. wsi_common_queue_present
// may only submit an unsignaled fence with no pending use, and only waits a fence
// some submission will signal; the worker waits it before reading the image.
enum class FenceState { unsignaled, pending, signaled };

struct FenceModel {
    CRITICAL_SECTION mutex{};
    CONDITION_VARIABLE changed = CONDITION_VARIABLE_INIT;
    FenceState state[maximum_swapchain_images]{};
    bool created[maximum_swapchain_images]{};
    bool retired[maximum_swapchain_images]{};
    std::uint32_t gpu[64]{};
    unsigned gpu_head = 0;
    unsigned gpu_count = 0;
    bool gpu_stop = false;
    unsigned creations = 0;
    unsigned app_waits = 0;
    unsigned app_resets = 0;
    unsigned worker_resets = 0;
    unsigned violations = 0;
    unsigned depth = 0;
};

DWORD WINAPI gpu_entry(void* data) {
    auto* fences = static_cast<FenceModel*>(data);
    EnterCriticalSection(&fences->mutex);

    for (;;) {
        while (fences->gpu_count == 0 && !fences->gpu_stop) {
            SleepConditionVariableCS(&fences->changed, &fences->mutex, INFINITE);
        }
        if (fences->gpu_count == 0) {
            break;
        }
        const std::uint32_t index = fences->gpu[fences->gpu_head];
        fences->gpu_head = (fences->gpu_head + 1) % 64;
        --fences->gpu_count;
        LeaveCriticalSection(&fences->mutex);
        Sleep(0);  // The blit executes after the submission returned.
        EnterCriticalSection(&fences->mutex);
        fences->state[index] = FenceState::signaled;
        WakeAllConditionVariable(&fences->changed);
    }
    LeaveCriticalSection(&fences->mutex);
    return 0;
}

// Waits a fence the caller holds the mutex for; a fence no submission will
// signal is a hang in Vulkan and a violation here.
bool fence_wait_locked(FenceModel& fences, std::uint32_t index) {
    if (fences.state[index] == FenceState::unsignaled) {
        ++fences.violations;
        return false;
    }
    while (fences.state[index] != FenceState::signaled) {
        SleepConditionVariableCS(&fences.changed, &fences.mutex, INFINITE);
    }
    return true;
}

struct RetiringChain {
    ModelChain base;
    FenceModel* fences;
};

struct RetiringOps {
    RetiringChain* chain;

    void lock() { EnterCriticalSection(&chain->base.mutex); }
    void unlock() { LeaveCriticalSection(&chain->base.mutex); }
    void wait() { SleepConditionVariableCS(&chain->base.work, &chain->base.mutex, INFINITE); }

    // Mirrors present body v3: own fence wait, then the production retire
    // decision and reset, then the idle transition under the image mutex.
    void present(std::uint32_t index) {
        FenceModel& fences = *chain->fences;
        EnterCriticalSection(&fences.mutex);
        const bool waited = fence_wait_locked(fences, index);

        if (waited && ql_nt5_frame_pipeline_retire_fence(true, fences.depth)) {
            fences.state[index] = FenceState::unsignaled;
            fences.retired[index] = true;
            ++fences.worker_resets;
        }
        LeaveCriticalSection(&fences.mutex);
        EnterCriticalSection(&chain->base.mutex);
        ++chain->base.presented;
        chain->base.state[index] = ImageState::idle;
        WakeAllConditionVariable(&chain->base.idle);
        LeaveCriticalSection(&chain->base.mutex);
    }
};

DWORD WINAPI retiring_entry(void* data) {
    RetiringOps ops{static_cast<RetiringChain*>(data)};
    run_present_worker(ops.chain->base.queue, ops);
    return 0;
}

// Mirrors wsi_common_queue_present's throttle block and blit submission on the
// application thread, using the production take-retired decision.
void present_on_app(FenceModel& fences, std::uint32_t index) {
    EnterCriticalSection(&fences.mutex);

    if (!fences.created[index]) {
        fences.created[index] = true;
        ++fences.creations;
    } else if (ql_nt5_frame_pipeline_take_retired(&fences.retired[index])) {
        // Retired by the worker: no wait, no reset here.
    } else {
        ++fences.app_waits;
        if (fence_wait_locked(fences, index)) {
            fences.state[index] = FenceState::unsignaled;
            ++fences.app_resets;
        }
    }
    if (fences.state[index] != FenceState::unsignaled) {
        ++fences.violations;  // Submitting a signaled or still pending fence.
    }
    fences.state[index] = FenceState::pending;
    fences.gpu[(fences.gpu_head + fences.gpu_count) % 64] = index;
    ++fences.gpu_count;
    WakeAllConditionVariable(&fences.changed);
    LeaveCriticalSection(&fences.mutex);
}

void run_retiring_chain(std::uint32_t images, unsigned depth, unsigned frames, FenceModel& fences) {
    LeaseLedger ledger;
    RetiringChain chain{};
    chain.fences = &fences;
    InitializeCriticalSection(&fences.mutex);
    fences.depth = depth;
    HANDLE gpu = CreateThread(nullptr, 0, gpu_entry, &fences, 0, nullptr);
    InitializeCriticalSection(&chain.base.mutex);
    chain.base.image_count = images;
    chain.base.ledger = &ledger;
    chain.base.thread = CreateThread(nullptr, 0, retiring_entry, &chain, 0, nullptr);
    unsigned accepted = 0;

    for (unsigned frame = 0; frame < frames; ++frame) {
        std::uint32_t index = 0;

        if (acquire(chain.base, index, bounded_wait_ms)) {
            present_on_app(fences, index);
            accepted += enqueue(chain.base, index) ? 1 : 0;
        }
    }
    EnterCriticalSection(&chain.base.mutex);
    chain.base.queue.stopping = true;
    WakeAllConditionVariable(&chain.base.work);
    LeaveCriticalSection(&chain.base.mutex);
    check(WaitForSingleObject(chain.base.thread, bounded_wait_ms) == WAIT_OBJECT_0 && accepted == frames &&
        chain.base.presented == frames, "every pipelined frame is accepted and presented once");
    CloseHandle(chain.base.thread);
    EnterCriticalSection(&fences.mutex);
    fences.gpu_stop = true;
    WakeAllConditionVariable(&fences.changed);
    LeaveCriticalSection(&fences.mutex);
    WaitForSingleObject(gpu, INFINITE);
    CloseHandle(gpu);
    DeleteCriticalSection(&chain.base.mutex);
    DeleteCriticalSection(&fences.mutex);
}

void check_retired_fences() {
    constexpr unsigned frames = 600;

    for (const std::uint32_t images : {2u, 3u}) {
        FenceModel pipelined{};
        run_retiring_chain(images, QL_NT5_FRAME_PIPELINE_DEFAULT, frames, pipelined);
        char description[112];
        std::snprintf(description, sizeof(description),
            "%u images: the app thread never waits or resets a present fence; the worker retires each once", images);
        check(pipelined.violations == 0 && pipelined.app_waits == 0 && pipelined.app_resets == 0 &&
            pipelined.worker_resets == frames, description);

        FenceModel upstream{};
        run_retiring_chain(images, QL_NT5_FRAME_PIPELINE_OFF, frames, upstream);
        std::snprintf(description, sizeof(description),
            "%u images, pipeline off: the upstream throttle waits and resets on every reuse", images);
        check(upstream.violations == 0 && upstream.worker_resets == 0 &&
            upstream.creations <= images && upstream.app_waits == frames - upstream.creations &&
            upstream.app_resets == frames - upstream.creations, description);
    }
}

void check_async_request() {
    const auto named = async_present_request(UINT64_C(0x1234567890), 640, 480, 2560, 64, 77, 1);
    check(named.present.size == LIGHTNING_OPENGL_PRESENT_ASYNC_BYTES &&
        named.present.version == LIGHTNING_OPENGL_PRESENT_ASYNC_VERSION &&
        named.present.lease == UINT64_C(0x1234567890) && named.present.image_width == 640 &&
        named.present.image_height == 480 && named.present.image_stride == 2560 && named.present.image_offset == 64 &&
        named.present.x == 0 && named.present.y == 0 && named.present.width == 640 && named.present.height == 480,
        "revision 2 carries the version-1 geometry unchanged");
    check(named.flags == (LIGHTNING_OPENGL_PRESENT_FLAG_ASYNC | LIGHTNING_OPENGL_PRESENT_FLAG_PRODUCER) &&
        named.producer_sync == 77 && named.producer_value == 1 && named.reserved == 0,
        "a known producer point is named and the copy is asynchronous");
    const auto anonymous = async_present_request(1, 2, 2, 256, 0, 77, 0);
    const auto unsignaled = async_present_request(1, 2, 2, 256, 0, 0, 1);
    check(anonymous.flags == LIGHTNING_OPENGL_PRESENT_FLAG_ASYNC && anonymous.producer_sync == 0 &&
        anonymous.producer_value == 0 && unsignaled.flags == LIGHTNING_OPENGL_PRESENT_FLAG_ASYNC &&
        unsignaled.producer_sync == 0, "an incomplete producer point is never sent: the full wait remains");
    check(present_gpu_async(nullptr, 1, 2, 2, 256, 0, 0, 0) == GpuPresentResult::unsupported &&
        present_gpu_async(reinterpret_cast<HDC>(1), 0, 2, 2, 256, 0, 0, 0) == GpuPresentResult::unsupported &&
        present_gpu_async(reinterpret_cast<HDC>(1), 1, 2, 2, UINT64_C(1) << 32, 0, 0, 0) ==
            GpuPresentResult::unsupported, "invalid requests never reach DrawEscape");
    check(query_gpu_receipt(nullptr, 1) == ReceiptState::unavailable &&
        query_gpu_receipt(reinterpret_cast<HDC>(1), 0) == ReceiptState::unavailable,
        "a receipt query without a DC or lease has no answer");
}

// async-present model chain: the worker queues each copy with its receipt
// pending; acquire waits for exactly the handed-out image's receipt.
struct AsyncChain {
    ModelChain base;
    HANDLE copied[maximum_swapchain_images]{};
    bool pending[maximum_swapchain_images]{};
    std::uint64_t order[maximum_swapchain_images]{};
    std::uint64_t clock = 0;
    unsigned receipt_queries = 0;
    unsigned receipts_seen_before_retire = 0;
};

struct AsyncReceiptOps : Win32ReceiptTiming {
    AsyncChain* chain;
    std::uint32_t index;

    ReceiptState query() const {
        ++chain->receipt_queries;
        return WaitForSingleObject(chain->copied[index], 0) == WAIT_OBJECT_0 ? ReceiptState::retired :
            ReceiptState::pending;
    }
};

struct AsyncWorkerOps {
    AsyncChain* chain;

    void lock() { EnterCriticalSection(&chain->base.mutex); }
    void unlock() { LeaveCriticalSection(&chain->base.mutex); }
    void wait() { SleepConditionVariableCS(&chain->base.work, &chain->base.mutex, INFINITE); }
    // Mirrors the adapter's revision-2 branch: own fence, queued copy, then
    // idle at once with the receipt pending and its queue order stamped.
    void present(std::uint32_t index) {
        WaitForSingleObject(chain->base.fence[index], INFINITE);
        EnterCriticalSection(&chain->base.mutex);
        if (chain->base.lease[index] == 0) {
            chain->base.lease[index] = chain->base.ledger->next++;
            ++chain->base.ledger->live;
        }
        chain->pending[index] = WaitForSingleObject(chain->copied[index], 0) != WAIT_OBJECT_0;
        chain->order[index] = ++chain->clock;
        ++chain->base.presented;
        chain->base.state[index] = ImageState::idle;
        WakeAllConditionVariable(&chain->base.idle);
        LeaveCriticalSection(&chain->base.mutex);
    }
};

DWORD WINAPI async_worker_entry(void* data) {
    AsyncWorkerOps ops{static_cast<AsyncChain*>(data)};
    run_present_worker(ops.chain->base.queue, ops);
    return 0;
}

void open_async(AsyncChain& chain, std::uint32_t images, LeaseLedger& ledger) {
    InitializeCriticalSection(&chain.base.mutex);
    chain.base.image_count = images;
    chain.base.ledger = &ledger;

    for (std::uint32_t index = 0; index < images; ++index) {
        chain.base.fence[index] = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        chain.copied[index] = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    }
    chain.base.thread = CreateThread(nullptr, 0, async_worker_entry, &chain, 0, nullptr);
    check(chain.base.thread != nullptr, "async presentation worker starts");
}

// A host copy that has not executed yet: its receipt will be pending.
void queue_copy(AsyncChain& chain, std::uint32_t index) { ResetEvent(chain.copied[index]); }

// Mirrors the adapter's find_idle_image preference plus acquire receipt wait.
bool acquire_async(AsyncChain& chain, std::uint32_t& index, std::uint64_t timeout_ns) {
    EnterCriticalSection(&chain.base.mutex);
    std::uint32_t chosen = UINT32_MAX;

    for (;;) {
        for (std::uint32_t candidate = 0; candidate < chain.base.image_count; ++candidate) {
            if (chain.base.state[candidate] == ImageState::idle &&
                (chosen == UINT32_MAX || prefer_idle_image(chain.pending[candidate], chain.order[candidate],
                    chain.pending[chosen], chain.order[chosen]))) {
                chosen = candidate;
            }
        }
        if (chosen != UINT32_MAX) break;
        if (!SleepConditionVariableCS(&chain.base.idle, &chain.base.mutex, bounded_wait_ms)) {
            LeaveCriticalSection(&chain.base.mutex);
            return false;
        }
    }
    chain.base.state[chosen] = ImageState::drawing;
    const bool receipt = chain.pending[chosen];
    LeaveCriticalSection(&chain.base.mutex);
    index = chosen;

    if (!receipt) {
        return true;
    }
    AsyncReceiptOps ops{};
    ops.chain = &chain;
    ops.index = chosen;

    if (wait_receipt(ops, timeout_ns) == ReceiptWait::retired) {
        chain.pending[chosen] = false;
        return true;
    }
    EnterCriticalSection(&chain.base.mutex);
    chain.base.state[chosen] = ImageState::idle;
    WakeAllConditionVariable(&chain.base.idle);
    LeaveCriticalSection(&chain.base.mutex);
    return false;
}

bool enqueue_async(AsyncChain& chain, std::uint32_t index) { return enqueue(chain.base, index); }

// Mirrors the destroy hook: drain the worker, then settle every pending
// receipt before any lease, image memory or DC is released.
bool close_async(AsyncChain& chain, DWORD timeout_ms, unsigned& unretired) {
    EnterCriticalSection(&chain.base.mutex);
    chain.base.queue.stopping = true;
    WakeAllConditionVariable(&chain.base.work);
    LeaveCriticalSection(&chain.base.mutex);

    if (WaitForSingleObject(chain.base.thread, timeout_ms) != WAIT_OBJECT_0) {
        return false;
    }
    CloseHandle(chain.base.thread);
    unretired = 0;

    for (std::uint32_t index = 0; index < chain.base.image_count; ++index) {
        AsyncReceiptOps ops{};
        ops.chain = &chain;
        ops.index = index;
        const bool was_pending = chain.pending[index];
        if (!settle_receipt(ops, chain.pending[index])) ++unretired;
        if (was_pending && WaitForSingleObject(chain.copied[index], 0) == WAIT_OBJECT_0) {
            ++chain.receipts_seen_before_retire;
        }
        if (chain.base.lease[index] != 0) {
            --chain.base.ledger->live;
            chain.base.lease[index] = 0;
        }
        CloseHandle(chain.base.fence[index]);
        CloseHandle(chain.copied[index]);
    }
    DeleteCriticalSection(&chain.base.mutex);
    return true;
}

struct DelayedSignal {
    HANDLE event;
    DWORD delay_ms;
};

DWORD WINAPI signal_later(void* data) {
    const auto* signal = static_cast<DelayedSignal*>(data);
    Sleep(signal->delay_ms);
    SetEvent(signal->event);
    return 0;
}

void check_async_present() {
    LeaseLedger ledger;
    AsyncChain chain;
    open_async(chain, 2, ledger);
    std::uint32_t a = 0, b = 0;
    check(acquire_async(chain, a, 0) && acquire_async(chain, b, 0) && a != b, "both images start idle");
    queue_copy(chain, a);
    queue_copy(chain, b);
    check(enqueue_async(chain, a) && enqueue_async(chain, b), "both frames are queued without any wait");
    Sleep(settle_ms);
    EnterCriticalSection(&chain.base.mutex);
    const bool both_idle = chain.base.state[a] == ImageState::idle && chain.base.state[b] == ImageState::idle &&
        chain.pending[a] && chain.pending[b] && chain.order[a] < chain.order[b];
    LeaveCriticalSection(&chain.base.mutex);
    check(both_idle, "the worker returns as soon as each copy is queued: images are idle with receipts pending");
    std::uint32_t reused = UINT32_MAX;
    check(!acquire_async(chain, reused, 0) && reused == a,
        "acquire picks the earliest copy and a zero timeout does not hand out an image still being read");
    EnterCriticalSection(&chain.base.mutex);
    const bool returned = chain.base.state[a] == ImageState::idle && chain.pending[a];
    LeaveCriticalSection(&chain.base.mutex);
    check(returned, "an image whose read is outstanding goes back to idle with its receipt pending");
    SetEvent(chain.copied[a]);
    check(acquire_async(chain, reused, UINT64_MAX) && reused == a && !chain.pending[a],
        "once its own copy retired the image is reused");
    DelayedSignal later{chain.copied[b], settle_ms};
    HANDLE helper = CreateThread(nullptr, 0, signal_later, &later, 0, nullptr);
    const DWORD started = GetTickCount();
    std::uint32_t next = UINT32_MAX;
    check(acquire_async(chain, next, UINT64_MAX) && next == b && GetTickCount() - started + 5 >= settle_ms &&
        !chain.pending[b], "acquire waits for exactly that image's receipt, then hands it out");
    WaitForSingleObject(helper, INFINITE);
    CloseHandle(helper);
    check(enqueue_async(chain, a), "the retired image presents again");
    Sleep(settle_ms);
    std::uint32_t free_image = UINT32_MAX;
    const unsigned queries = chain.receipt_queries;
    check(acquire_async(chain, free_image, 0) && free_image == a && chain.receipt_queries == queries,
        "a copy that already retired at queue time needs no receipt query");
    queue_copy(chain, a);
    queue_copy(chain, b);
    check(enqueue_async(chain, a) && enqueue_async(chain, b), "two more copies are queued");
    Sleep(settle_ms);
    DelayedSignal finish_a{chain.copied[a], settle_ms};
    DelayedSignal finish_b{chain.copied[b], 2 * settle_ms};
    HANDLE first = CreateThread(nullptr, 0, signal_later, &finish_a, 0, nullptr);
    HANDLE second = CreateThread(nullptr, 0, signal_later, &finish_b, 0, nullptr);
    unsigned unretired = 1;
    check(close_async(chain, bounded_wait_ms, unretired) && unretired == 0 &&
        chain.receipts_seen_before_retire == 2 && ledger.live == 0,
        "destruction waits for each pending receipt before retiring its lease");
    WaitForSingleObject(first, INFINITE);
    WaitForSingleObject(second, INFINITE);
    CloseHandle(first);
    CloseHandle(second);
}
}

int main() {
    check_queue();
    check_aperture_cost();
    check_fence_ordering();
    check_stop_drains();
    check_mode_change_and_reopen();
    check_receipt_policy();
    check_async_request();
    check_async_present();
    check_receipt_clock();
    check_frame_pipeline_policy();
    check_retired_fences();
    std::printf("%u WSI present queue checks; %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

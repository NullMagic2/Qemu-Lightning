/* Module: lightning.graphics_driver.display_abi
 * Purpose: Check display callbacks, residency references and empty paging with WDK types.
 * Created: 2026-10-10
 * Architecture: OS mode allocation and paging locks are mocked. This executable never loads
 * a driver and does not establish VidPN, presentation or desktop acceleration.
 */
#include "miniport.h"
#include "internal.h"
#include <stdlib.h>
__declspec(dllimport) __declspec(noreturn) void WINAPI ExitProcess(UINT);
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            ExitProcess(__LINE__);                                                                 \
    } while (0)
enum {
    FIXTURE_MODE_ID = 73,
    FIXTURE_POISON = 0xa5,
    FIXTURE_HANDLE_GENERATION_SHIFT = 32,
    /* WIDTH, HEIGHT, STRIDE, OFFSET, RESOURCE_LO, RESOURCE_HI and COMMAND. */
    FIXTURE_FRAME_REGISTER_WRITES = 7,
    /* A new GPU buffer of the same mode and context: OFFSET, RESOURCE_LO, COMMAND. */
    FIXTURE_FLIP_REGISTER_WRITES = 3,
    /* Both resource halves and COMMAND when only the GPU image is dropped. */
    FIXTURE_RESOURCE_REGISTER_WRITES = 3,
};
static QLG_MINIPORT adapter;
static ULONG display_registers[LIGHTNING_SCANOUT_END / LIGHTNING_SCANOUT_REGISTER_BYTES];
static UINT display_writes, display_reads, display_presents;
/* QEMU's side of the scanout block: STATUS and the GPU_COMPLETED latch. */
static ULONG host_status;
static BOOLEAN host_completed;
#define REGISTER(offset) display_registers[(offset) / LIGHTNING_SCANOUT_REGISTER_BYTES]

void QlvKernelWriteRegister(void *transport, ULONG offset, ULONG value)
{
    CHECK(transport == &adapter && offset < LIGHTNING_SCANOUT_END &&
          offset % LIGHTNING_SCANOUT_REGISTER_BYTES == 0);
    REGISTER(offset) = value;
    display_writes++;

    if (offset == LIGHTNING_SCANOUT_REG_IRQ && (value & LIGHTNING_SCANOUT_GPU_COMPLETED))
        host_completed = FALSE;

    if (offset != LIGHTNING_SCANOUT_REG_COMMAND)
        return;
    /* As hw/misc/lightning.c: CPU frames copy during the write, GPU frames
     * queue a read, DISABLE cancels it without a completion. */
    if (value == LIGHTNING_SCANOUT_PRESENT) {
        CHECK(host_status != LIGHTNING_SCANOUT_PENDING);
        display_presents++;
        host_status = REGISTER(LIGHTNING_SCANOUT_REG_RESOURCE_LO) ||
                              REGISTER(LIGHTNING_SCANOUT_REG_RESOURCE_HI)
                          ? LIGHTNING_SCANOUT_PENDING
                          : LIGHTNING_SCANOUT_ACTIVE;
    } else {
        CHECK(value == LIGHTNING_SCANOUT_DISABLE);
        host_status = LIGHTNING_SCANOUT_IDLE;
    }
}

ULONG QlvKernelReadRegister(void *transport, ULONG offset)
{
    CHECK(transport == &adapter);
    display_reads++;

    if (offset == LIGHTNING_SCANOUT_REG_STATUS)
        return host_status;
    CHECK(offset == LIGHTNING_SCANOUT_REG_IRQ);
    return host_completed ? LIGHTNING_SCANOUT_GPU_COMPLETED : 0;
}

/* QEMU finishes the queued GPU read. Hosts before 2026-10-16 raised no
 * completion for a rejected (INVALID) frame. */
static void host_finish(ULONG status, BOOLEAN completion)
{
    CHECK(host_status == LIGHTNING_SCANOUT_PENDING);
    host_status = status;
    host_completed = completion;
}

static void check_display_programming(void)
{
    const UINT64 gpu_image = ((UINT64)1 << FIXTURE_HANDLE_GENERATION_SHIFT) | FIXTURE_MODE_ID;
    QLG_SCANOUT_FRAME frame = {LIGHTNING_SCANOUT_MODE_0_WIDTH, LIGHTNING_SCANOUT_MODE_0_HEIGHT,
        LIGHTNING_SCANOUT_MODE_0_WIDTH * LIGHTNING_SCANOUT_PIXEL_BYTES, QLM_PAGE_BYTES, gpu_image};
    adapter.venus_transport = &adapter;
    QlgDisplayProgramFrame(&adapter, &frame);
    CHECK(display_writes == FIXTURE_FRAME_REGISTER_WRITES && display_presents == 1);
    CHECK(REGISTER(LIGHTNING_SCANOUT_REG_RESOURCE_LO) == FIXTURE_MODE_ID);
    CHECK(REGISTER(LIGHTNING_SCANOUT_REG_RESOURCE_HI) == 1);
    CHECK(REGISTER(LIGHTNING_SCANOUT_REG_OFFSET) == QLM_PAGE_BYTES);
    host_finish(LIGHTNING_SCANOUT_ACTIVE, FALSE);
    /* Dropping the GPU image rewrites both halves, as candidate 67 requires,
     * but unchanged geometry is not sent again. */
    frame.gpu_resource = 0;
    display_writes = 0;
    QlgDisplayProgramFrame(&adapter, &frame);
    CHECK(display_writes == FIXTURE_RESOURCE_REGISTER_WRITES);
    CHECK(REGISTER(LIGHTNING_SCANOUT_REG_RESOURCE_LO) == 0);
    CHECK(REGISTER(LIGHTNING_SCANOUT_REG_RESOURCE_HI) == 0);
    CHECK(REGISTER(LIGHTNING_SCANOUT_REG_COMMAND) == LIGHTNING_SCANOUT_PRESENT);
    CHECK(adapter.display.counters[QLG_DISPLAY_SCANOUT_WRITES] ==
          FIXTURE_FRAME_REGISTER_WRITES + FIXTURE_RESOURCE_REGISTER_WRITES);
    RtlZeroMemory(&adapter.display, sizeof(adapter.display));
    host_status = LIGHTNING_SCANOUT_IDLE;
}
#define EDID_TEST_BYTES 128u
static D3DKMDT_MONITOR_SOURCE_MODE mode;
static UINT creates, adds, releases;
static NTSTATUS create_result, add_result, release_result;

static NTSTATUS APIENTRY create_mode(D3DKMDT_HMONITORSOURCEMODESET set,
                                     D3DKMDT_MONITOR_SOURCE_MODE **output)
{
    CHECK(set == (D3DKMDT_HMONITORSOURCEMODESET)&mode);
    ++creates;
    if (!NT_SUCCESS(create_result))
        return create_result;
    RtlFillMemory(&mode, sizeof(mode), FIXTURE_POISON);
    mode.Id = FIXTURE_MODE_ID;
    *output = &mode;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY add_mode(D3DKMDT_HMONITORSOURCEMODESET set,
                                  D3DKMDT_MONITOR_SOURCE_MODE *value)
{
    CHECK(set == (D3DKMDT_HMONITORSOURCEMODESET)&mode && value == &mode);
    CHECK(value->Id == FIXTURE_MODE_ID);
    CHECK(QlgDisplayModeSupported((UINT)value->VideoSignalInfo.ActiveSize.cx,
                                  (UINT)value->VideoSignalInfo.ActiveSize.cy));
    /* Without EDID-derived timing, frequencies are unspecified (any match). */
    CHECK(value->VideoSignalInfo.TotalSize.cx == value->VideoSignalInfo.ActiveSize.cx);
    CHECK(value->VideoSignalInfo.VSyncFreq.Numerator == D3DKMDT_FREQUENCY_NOTSPECIFIED);
    CHECK(value->VideoSignalInfo.AdditionalSignalInfo.Reserved == 0);
    CHECK(value->ColorBasis == D3DKMDT_CB_SRGB &&
          value->ColorCoeffDynamicRanges.FourthChannel == 0);
    CHECK(value->Origin == D3DKMDT_MCO_DRIVER);
    CHECK((value->Preference == D3DKMDT_MP_PREFERRED) ==
          (value->VideoSignalInfo.ActiveSize.cx == LIGHTNING_SCANOUT_MODE_0_WIDTH));
    ++adds;
    return add_result;
}

static NTSTATUS APIENTRY release_mode(D3DKMDT_HMONITORSOURCEMODESET set,
                                      const D3DKMDT_MONITOR_SOURCE_MODE *value)
{
    CHECK(set == (D3DKMDT_HMONITORSOURCEMODESET)&mode && value == &mode);
    ++releases;
    return release_result;
}

__declspec(noreturn) void QlgPanic(void)
{
    ExitProcess(__LINE__);
}

static KIRQL paging_lock(void *context)
{
    UNREFERENCED_PARAMETER(context);
    return 0;
}

static void paging_unlock(void *context, KIRQL level)
{
    UNREFERENCED_PARAMETER(context);
    CHECK(level == 0);
}

static UCHAR aperture_page[QLM_PAGE_BYTES];
static UINT aperture_unmaps;

static NTSTATUS map_aperture_fixture(void *context, PMDL mdl, BOOLEAN write, void **mapping,
                                     SIZE_T *bytes, void **cookie)
{
    CHECK(context == aperture_page && mdl == (PMDL)aperture_page && write);
    *mapping = *cookie = aperture_page;
    *bytes = sizeof(aperture_page);
    return STATUS_SUCCESS;
}

static void unmap_aperture_fixture(void *context, void *cookie)
{
    CHECK(context == aperture_page && cookie == aperture_page);
    ++aperture_unmaps;
}

/* A shared texture must retain its real system pages through Present, with
 * no access outside the allocation or after its aperture range is removed. */
static void check_texture_aperture(void)
{
    static QLG_ADAPTER core;
    static QLG_OBJECT allocation;
    DXGKARG_BUILDPAGINGBUFFER paging = {0};
    QLG_PRESENT_COMMAND command = {0};
    enum { COPY_WIDTH = 16, COPY_HEIGHT = 16 };
    const UINT copy_color = 0xff19ab73u;
    const UINT copy_bytes = COPY_WIDTH * COPY_HEIGHT * LIGHTNING_SCANOUT_PIXEL_BYTES;
    UINT index;

    core.state = calloc(1, qlg_state_size(QLG_KIND_ADAPTER));
    core.memory_arena = calloc(1, qlm_arena_size());
    allocation.state = calloc(1, qlg_state_size(QLG_KIND_ALLOCATION));
    allocation.memory_state = calloc(1, qlm_allocation_size());
    CHECK(core.state && core.memory_arena && allocation.state && allocation.memory_state);
    core.services.lock = paging_lock;
    core.services.unlock = paging_unlock;
    core.mdl.context = aperture_page;
    core.mdl.map = map_aperture_fixture;
    core.mdl.unmap = unmap_aperture_fixture;
    InitializeListHead(&core.allocations);
    InsertTailList(&core.allocations, &allocation.link);
    allocation.adapter = &core;
    allocation.kind = QLG_KIND_ALLOCATION;
    allocation.resource_kind = QLG_RESOURCE_RGBA8;
    allocation.allocation_bytes = sizeof(aperture_page);
    qlg_adapter_init(core.state);
    qlm_arena_init(core.memory_arena);
    CHECK(qlg_allocation_init(core.state, allocation.state, sizeof(aperture_page)) == STATUS_SUCCESS);
    CHECK(qlm_allocation_init(allocation.memory_state, 1, sizeof(aperture_page)) == STATUS_SUCCESS);
    paging.Operation = DXGK_OPERATION_MAP_APERTURE_SEGMENT;
    paging.MapApertureSegment.hAllocation = &allocation;
    paging.MapApertureSegment.SegmentId = QLM_APERTURE_SEGMENT;
    paging.MapApertureSegment.NumberOfPages = 1;
    paging.MapApertureSegment.OffsetInPages = 1;
    paging.MapApertureSegment.pMdl = (PMDL)aperture_page;
    CHECK(QlgAperturePaging(&core, &paging) == STATUS_SUCCESS);
    /* VidMm must receive its documented allocation-busy retry status, with
     * the existing mapping and reference count intact after refusal. */
    CHECK(QlgAperturePaging(&core, &paging) == STATUS_GRAPHICS_ALLOCATION_BUSY);
    CHECK(qlg_paging_acquire(core.state, allocation.state) == STATUS_SUCCESS);
    CHECK(QlgAperturePaging(&core, &paging) == STATUS_GRAPHICS_ALLOCATION_BUSY);
    CHECK(qlg_paging_release(core.state, allocation.state) == STATUS_SUCCESS);
    CHECK(QlgPresentMapping(&core, QLM_APERTURE_SEGMENT, QLM_PAGE_BYTES,
                            sizeof(aperture_page)) == aperture_page);
    CHECK(!QlgPresentMapping(&core, QLM_APERTURE_SEGMENT, QLM_PAGE_BYTES - 1, 1));
    CHECK(!QlgPresentMapping(&core, QLM_APERTURE_SEGMENT, QLM_PAGE_BYTES + 1,
                             sizeof(aperture_page)));
    CHECK(!QlgPresentMapping(&core, QLM_APERTURE_SEGMENT, MAXUINT64, 1));
    /* The native window path copies between two aperture ranges. Equal
     * numeric offsets in different segments must not publish the primary. */
    adapter.lifecycle.adapter = &core;
    adapter.display.address = QLM_PAGE_BYTES + copy_bytes;
    adapter.display.scanout_pending = FALSE;
    command.operation = QLG_PRESENT_BLT;
    command.source.segment = command.destination.segment = QLM_APERTURE_SEGMENT;
    command.source.width = command.destination.width = COPY_WIDTH;
    command.source.height = command.destination.height = COPY_HEIGHT;
    command.source.pitch = command.destination.pitch = COPY_WIDTH * LIGHTNING_SCANOUT_PIXEL_BYTES;
    command.source.offset = QLM_PAGE_BYTES;
    command.destination.offset = QLM_PAGE_BYTES + copy_bytes;
    command.src_rect.right = command.dst_rect.right = command.rects[0].right = COPY_WIDTH;
    command.src_rect.bottom = command.dst_rect.bottom = command.rects[0].bottom = COPY_HEIGHT;
    command.rect_count = 1;

    for (index = 0; index < copy_bytes / sizeof(UINT); ++index)
        ((UINT *)aperture_page)[index] = copy_color;
    CHECK(QlgDisplayExecute(&adapter, &command) == STATUS_SUCCESS);

    for (index = 0; index < copy_bytes / sizeof(UINT); ++index)
        CHECK(((UINT *)(aperture_page + copy_bytes))[index] == copy_color);
    CHECK(!adapter.display.scanout_pending);
    command.destination.offset = QLM_PAGE_BYTES + sizeof(aperture_page) - copy_bytes + 1;
    CHECK(QlgDisplayExecute(&adapter, &command) == STATUS_NOT_SUPPORTED);
    adapter.lifecycle.adapter = NULL;

    RtlZeroMemory(&paging, sizeof(paging));
    paging.Operation = DXGK_OPERATION_UNMAP_APERTURE_SEGMENT;
    paging.UnmapApertureSegment.hAllocation = &allocation;
    paging.UnmapApertureSegment.SegmentId = QLM_APERTURE_SEGMENT;
    paging.UnmapApertureSegment.NumberOfPages = 1;
    paging.UnmapApertureSegment.OffsetInPages = 1;
    CHECK(qlg_paging_acquire(core.state, allocation.state) == STATUS_SUCCESS);
    CHECK(QlgAperturePaging(&core, &paging) == STATUS_GRAPHICS_ALLOCATION_BUSY);
    CHECK(aperture_unmaps == 0);
    CHECK(QlgPresentMapping(&core, QLM_APERTURE_SEGMENT, QLM_PAGE_BYTES, 1) == aperture_page);
    CHECK(qlg_paging_release(core.state, allocation.state) == STATUS_SUCCESS);
    CHECK(QlgAperturePaging(&core, &paging) == STATUS_SUCCESS && aperture_unmaps == 1);
    CHECK(!QlgPresentMapping(&core, QLM_APERTURE_SEGMENT, QLM_PAGE_BYTES, 1));
    free(allocation.memory_state);
    free(allocation.state);
    free(core.memory_arena);
    free(core.state);
}

static void check_discard_during_reset(void)
{
    static QLG_ADAPTER core;
    static QLG_OBJECT allocation;
    QLM_REQUEST fill = {0};
    QLM_PLAN plan = {0};
    DXGKARG_BUILDPAGINGBUFFER discard = {0};

    core.state = calloc(1, qlg_state_size(QLG_KIND_ADAPTER));
    core.memory_arena = calloc(1, qlm_arena_size());
    allocation.state = calloc(1, qlg_state_size(QLG_KIND_ALLOCATION));
    allocation.memory_state = calloc(1, qlm_allocation_size());
    CHECK(core.state && core.memory_arena && allocation.state && allocation.memory_state);
    core.services.lock = paging_lock;
    core.services.unlock = paging_unlock;
    allocation.adapter = &core;
    allocation.kind = QLG_KIND_ALLOCATION;
    allocation.allocation_bytes = QLM_PAGE_BYTES;
    qlg_adapter_init(core.state);
    qlm_arena_init(core.memory_arena);
    CHECK(qlg_allocation_init(core.state, allocation.state, QLM_PAGE_BYTES) == STATUS_SUCCESS);
    CHECK(qlm_allocation_init(allocation.memory_state, 1, QLM_PAGE_BYTES) == STATUS_SUCCESS);
    fill.operation = QLM_FILL;
    fill.destination_segment = QLM_MEMORY_SEGMENT;
    fill.bytes = QLM_PAGE_BYTES;
    fill.flags = QLM_BEGIN | QLM_END;
    CHECK(qlm_prepare(core.memory_arena, allocation.memory_state, &fill, &plan) == STATUS_SUCCESS);
    CHECK(qlm_start(core.memory_arena, allocation.memory_state, &plan) == STATUS_SUCCESS);
    CHECK(qlm_finish(core.memory_arena, allocation.memory_state, &plan) == STATUS_SUCCESS);
    discard.Operation = DXGK_OPERATION_DISCARD_CONTENT;
    discard.DiscardContent.hAllocation = &allocation;
    discard.DiscardContent.SegmentId = QLM_MEMORY_SEGMENT;
    discard.DiscardContent.Flags.AllocationIsIdle = 1;
    CHECK(qlg_paging_acquire(core.state, allocation.state) == STATUS_SUCCESS);
    CHECK(QlgDiscardPaging(&core, &discard) == STATUS_GRAPHICS_ALLOCATION_BUSY);
    CHECK(qlg_paging_release(core.state, allocation.state) == STATUS_SUCCESS);
    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS);
    CHECK(qlg_reset_quiesced(core.state) == STATUS_SUCCESS);
    CHECK(qlg_paging_acquire(core.state, allocation.state) == STATUS_DEVICE_NOT_READY);
    discard.Operation = DXGK_OPERATION_DISCARD_CONTENT;
    discard.DiscardContent.hAllocation = &allocation;
    discard.DiscardContent.SegmentId = QLM_MEMORY_SEGMENT;
    discard.DiscardContent.Flags.AllocationIsIdle = 1;
    discard.DiscardContent.SegmentAddress.QuadPart = QLM_PAGE_BYTES;
    CHECK(QlgDiscardPaging(&core, &discard) == STATUS_INVALID_PARAMETER);
    discard.DiscardContent.SegmentAddress.QuadPart = 0;
    CHECK(QlgDiscardPaging(&core, &discard) == STATUS_SUCCESS);
    CHECK(!discard.pDmaBuffer && !discard.pDmaBufferPrivateData);
    CHECK(qlm_prepare(core.memory_arena, allocation.memory_state, &fill, &plan) == STATUS_SUCCESS);
    CHECK(qlm_abort(core.memory_arena, allocation.memory_state, &plan, 0) == STATUS_SUCCESS);
    free(allocation.memory_state);
    free(allocation.state);
    free(core.memory_arena);
    free(core.state);
}

static void check_empty_paging(void)
{
    static QLG_PAGING_CONTEXT paging;
    UCHAR dma[32] = {0}, private_data[32] = {0};
    DXGKARG_PATCH patch = {0};
    paging.services.lock = paging_lock;
    paging.services.unlock = paging_unlock;
    patch.Flags.Paging = 1;
    patch.pDmaBuffer = dma;
    patch.DmaBufferSize = sizeof(dma);
    patch.DmaBufferSubmissionStartOffset = patch.DmaBufferSubmissionEndOffset = 16;
    patch.pDmaBufferPrivateData = private_data;
    patch.DmaBufferPrivateDataSize = sizeof(private_data);
    patch.DmaBufferPrivateDataSubmissionStartOffset =
        patch.DmaBufferPrivateDataSubmissionEndOffset = 16;
    CHECK(QlgPagingPatch(&paging, &patch) == STATUS_SUCCESS);
    patch.DmaBufferPrivateDataSubmissionEndOffset = 32;
    CHECK(QlgPagingPatch(&paging, &patch) == STATUS_INVALID_PARAMETER);
    patch.DmaBufferPrivateDataSubmissionEndOffset = 16;
    patch.DmaBufferSubmissionEndOffset = patch.DmaBufferSubmissionStartOffset = 48;
    CHECK(QlgPagingPatch(&paging, &patch) == STATUS_INVALID_PARAMETER);
}

static NTSTATUS paging_copy_admission(void *context, HANDLE allocation)
{
    CHECK(context == allocation);
    return STATUS_SUCCESS;
}

/* Exercise actual fill, eviction and page-in bytes around the Rust residency
 * transitions. Guard words detect writes beyond the allocation. */
static void check_paging_copy_bytes(void)
{
    enum { COPY_GUARD_WORDS = 2, COPY_PATTERN = 0x19ab73d5, COPY_GUARD = 0x67ca3491 };
    static QLG_PAGING_CONTEXT paging;
    QLG_PAGING_JOB *job = &paging.jobs[0];
    void *state = calloc(1, qlm_allocation_size());
    void *arena = calloc(1, qlm_arena_size());
    const SIZE_T words = QLM_PAGE_BYTES / sizeof(UINT);
    UINT *video = malloc(QLM_PAGE_BYTES + COPY_GUARD_WORDS * sizeof(UINT));
    UINT *system = malloc(QLM_PAGE_BYTES + COPY_GUARD_WORDS * sizeof(UINT));
    QLG_PAGING_TICKET ticket;
    QLM_REQUEST request = {0};
    SIZE_T index;

    CHECK(state && arena && video && system);
    qlm_arena_init(arena);
    CHECK(qlm_allocation_init(state, 1, QLM_PAGE_BYTES) == STATUS_SUCCESS);
    paging.services.context = state;
    paging.services.lock = paging_lock;
    paging.services.unlock = paging_unlock;
    paging.services.validate_admission = paging_copy_admission;
    paging.arena = arena;
    paging.job_count = QLM_MAX_JOBS;
    paging.memory = (volatile UCHAR *)(video + 1);
    job->occupied = TRUE;
    job->allocation = job->state = state;
    job->mapping = system + 1;
    job->submission = 1;
    video[0] = video[words + 1] = COPY_GUARD;
    system[0] = system[words + 1] = COPY_GUARD;
    request.operation = QLM_FILL;
    request.destination_segment = QLM_MEMORY_SEGMENT;
    request.bytes = QLM_PAGE_BYTES;
    request.flags = QLM_BEGIN | QLM_END;
    request.pattern = COPY_PATTERN;
    CHECK(qlm_prepare(arena, state, &request, &job->plan) == STATUS_SUCCESS);
    ticket.epoch = job->plan.epoch;
    ticket.ticket = job->plan.ticket;
    CHECK(QlgExecutePagingSubmitted(&paging, &ticket, 1) == STATUS_SUCCESS);

    for (index = 1; index <= words; ++index)
        CHECK(video[index] == COPY_PATTERN);
    request.operation = QLM_TRANSFER;
    request.source_segment = QLM_MEMORY_SEGMENT;
    request.destination_segment = 0;
    CHECK(qlm_prepare(arena, state, &request, &job->plan) == STATUS_SUCCESS);
    ticket.ticket = job->plan.ticket;
    CHECK(QlgExecutePagingSubmitted(&paging, &ticket, 1) == STATUS_SUCCESS);

    for (index = 1; index <= words; ++index) {
        CHECK(system[index] == COPY_PATTERN);
        system[index] ^= (UINT)index;
    }
    request.source_segment = 0;
    request.destination_segment = QLM_MEMORY_SEGMENT;
    CHECK(qlm_prepare(arena, state, &request, &job->plan) == STATUS_SUCCESS);
    ticket.ticket = job->plan.ticket;
    CHECK(QlgExecutePagingSubmitted(&paging, &ticket, 1) == STATUS_SUCCESS);

    for (index = 1; index <= words; ++index)
        CHECK(video[index] == (COPY_PATTERN ^ (UINT)index));
    CHECK(video[0] == COPY_GUARD && video[words + 1] == COPY_GUARD);
    CHECK(system[0] == COPY_GUARD && system[words + 1] == COPY_GUARD);
    free(system);
    free(video);
    free(arena);
    free(state);
}


/* Fence notification can reenter BuildPagingBuffer after bytes finish but before
 * the earlier job retires. Exercise actual callback ownership, not a fake busy
 * status: pending memory stays exclusive while finished lifetime pins can share. */
static void check_finished_paging_handoff(void)
{
    enum { HANDOFF_FIRST_PATTERN = 0x19ab73d5, HANDOFF_SECOND_PATTERN = 0x71ac3905 };
    static QLG_ADAPTER core;
    static QLG_OBJECT allocation;
    QLG_MDL_SERVICES mdl = {0};
    DXGKARG_BUILDPAGINGBUFFER args = {0};
    QLG_PAGING_TICKET dma, private_dma, first, second;
    QLG_PAGING_JOB *job;
    UINT *memory = calloc(1, QLM_ARENA_BYTES);
    SIZE_T index;

    core.state = calloc(1, qlg_state_size(QLG_KIND_ADAPTER));
    core.memory_arena = calloc(1, qlm_arena_size());
    allocation.state = calloc(1, qlg_state_size(QLG_KIND_ALLOCATION));
    allocation.memory_state = calloc(1, qlm_allocation_size());
    CHECK(memory && core.state && core.memory_arena && allocation.state && allocation.memory_state);
    core.services.lock = paging_lock;
    core.services.unlock = paging_unlock;
    qlg_adapter_init(core.state);
    mdl.context = aperture_page;
    mdl.map = map_aperture_fixture;
    mdl.unmap = unmap_aperture_fixture;
    CHECK(QlgAttachMemory(&core, memory, QLM_ARENA_BYTES, &mdl) == STATUS_SUCCESS);
    allocation.adapter = &core;
    allocation.kind = QLG_KIND_ALLOCATION;
    allocation.allocation_bytes = QLM_PAGE_BYTES;
    CHECK(qlg_allocation_init(core.state, allocation.state, QLM_PAGE_BYTES) == STATUS_SUCCESS);
    CHECK(qlm_allocation_init(allocation.memory_state, 1, QLM_PAGE_BYTES) == STATUS_SUCCESS);
    args.Operation = DXGK_OPERATION_FILL;
    args.Fill.hAllocation = &allocation;
    args.Fill.FillSize = QLM_PAGE_BYTES;
    args.Fill.FillPattern = HANDOFF_FIRST_PATTERN;
    args.Fill.Destination.SegmentId = QLM_MEMORY_SEGMENT;
    args.pDmaBuffer = &dma;
    args.DmaSize = sizeof(dma);
    args.pDmaBufferPrivateData = &private_dma;
    args.DmaBufferPrivateDataSize = sizeof(private_dma);
    CHECK(QlgBuildPagingBuffer(&core.paging, &args) == STATUS_SUCCESS);
    first = dma;
    job = QlgPagingFindLocked(&core.paging, &first);
    CHECK(job != NULL);
    job->submission = 1;
    args.pDmaBuffer = &dma;
    args.DmaSize = sizeof(dma);
    args.pDmaBufferPrivateData = &private_dma;
    args.DmaBufferPrivateDataSize = sizeof(private_dma);
    CHECK(QlgBuildPagingBuffer(&core.paging, &args) == STATUS_GRAPHICS_ALLOCATION_BUSY);
    CHECK(QlgExecutePagingSubmitted(&core.paging, &first, 1) == STATUS_SUCCESS);
    for (index = 0; index < QLM_PAGE_BYTES / sizeof(UINT); ++index)
        CHECK(memory[index] == HANDOFF_FIRST_PATTERN);
    CHECK(qlg_allocation_drop(core.state, allocation.state) == STATUS_DEVICE_BUSY);
    args.Fill.FillPattern = HANDOFF_SECOND_PATTERN;
    CHECK(QlgBuildPagingBuffer(&core.paging, &args) == STATUS_SUCCESS);
    second = dma;
    CHECK(second.ticket != first.ticket);
    CHECK(qlg_allocation_drop(core.state, allocation.state) == STATUS_DEVICE_BUSY);
    CHECK(QlgRetirePagingSubmitted(&core.paging, &first, 1, TRUE, FALSE, NULL, NULL) == STATUS_SUCCESS);
    CHECK(qlg_allocation_drop(core.state, allocation.state) == STATUS_DEVICE_BUSY);
    job = QlgPagingFindLocked(&core.paging, &second);
    CHECK(job != NULL);
    job->submission = 2;
    CHECK(QlgExecutePagingSubmitted(&core.paging, &second, 2) == STATUS_SUCCESS);
    for (index = 0; index < QLM_PAGE_BYTES / sizeof(UINT); ++index)
        CHECK(memory[index] == HANDOFF_SECOND_PATTERN);
    CHECK(QlgRetirePagingSubmitted(&core.paging, &second, 2, TRUE, FALSE, NULL, NULL) == STATUS_SUCCESS);
    CHECK(qlm_allocation_drop(core.memory_arena, allocation.memory_state) == STATUS_SUCCESS);
    CHECK(qlg_allocation_drop(core.state, allocation.state) == STATUS_SUCCESS);
    CHECK(qlg_adapter_empty(core.state));
    free(allocation.memory_state);
    free(allocation.state);
    free(core.memory_arena);
    free(core.state);
    free(memory);
}

static void check_present_residency(void)
{
    static QLG_ADAPTER core = {0};
    enum { FIXTURE_SURFACE_WIDTH = 64, FIXTURE_SURFACE_HEIGHT = 32 };
    static QLG_OBJECT context = {0}, source = {0}, destination = {0};
    static QLG_OBJECT source_open = {0}, destination_open = {0};
    static DXGK_ALLOCATIONLIST allocations[3] = {0};
    static D3DDDI_PATCHLOCATIONLIST patches[2] = {0};
    static QLG_PRESENT_DMA dma = {0};
    static QLG_PRESENT_COMMAND command = {0};
    DXGKARG_PRESENT present = {0};
    DXGKARG_PATCH patch = {0};
    RECT rects[QLG_PRESENT_MAX_RECTS + 1] = {0};
    UINT index;

    core.services.context = &adapter;
    context.adapter = &core;
    source.kind = destination.kind = QLG_KIND_ALLOCATION;
    source.width = destination.width = FIXTURE_SURFACE_WIDTH;
    source.height = destination.height = FIXTURE_SURFACE_HEIGHT;
    source.pitch = destination.pitch = FIXTURE_SURFACE_WIDTH * LIGHTNING_SCANOUT_PIXEL_BYTES;
    source_open.kind = destination_open.kind = QLG_KIND_OPEN;
    source_open.resource = &source;
    destination_open.resource = &destination;
    allocations[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation = &source_open;
    allocations[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation = &destination_open;
    allocations[DXGK_PRESENT_SOURCE_INDEX].PhysicalAddress.QuadPart = 4096;
    allocations[DXGK_PRESENT_DESTINATION_INDEX].PhysicalAddress.QuadPart = 8192;
    present.pDmaBuffer = &dma;
    present.DmaSize = sizeof(dma);
    present.pDmaBufferPrivateData = &command;
    present.DmaBufferPrivateDataSize = sizeof(command);
    present.pAllocationList = allocations;
    present.pPatchLocationListOut = patches;
    present.PatchLocationListOutSize = 1;
    present.Flags.Blt = 1;
    CHECK(QlgMiniportPresent(&context, &present) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    CHECK(present.pDmaBuffer == &dma && present.pPatchLocationListOut == patches);
    present.PatchLocationListOutSize = 2;
    CHECK(QlgMiniportPresent(&context, &present) == STATUS_SUCCESS);
    CHECK(present.pPatchLocationListOut == patches + 2 && present.pDmaBuffer == &dma + 1);
    CHECK(patches[0].AllocationIndex == DXGK_PRESENT_SOURCE_INDEX &&
          patches[1].AllocationIndex == DXGK_PRESENT_DESTINATION_INDEX);
    CHECK(patches[0].PatchOffset == FIELD_OFFSET(QLG_PRESENT_DMA, source) &&
          patches[1].PatchOffset == FIELD_OFFSET(QLG_PRESENT_DMA, destination));
    CHECK(dma.source == 4096 && dma.destination == 8192);
    CHECK(command.source.width == source.width && command.source.height == source.height &&
          command.source.pitch == source.pitch && command.destination.width == destination.width &&
          command.destination.height == destination.height &&
          command.destination.pitch == destination.pitch);
    patch.pDmaBuffer = &dma;
    patch.DmaBufferSize = patch.DmaBufferSubmissionEndOffset = sizeof(dma);
    patch.pDmaBufferPrivateData = &command;
    patch.DmaBufferPrivateDataSize = patch.DmaBufferPrivateDataSubmissionEndOffset = sizeof(command);
    patch.pAllocationList = allocations;
    patch.AllocationListSize = 3;
    allocations[DXGK_PRESENT_DESTINATION_INDEX].PhysicalAddress.QuadPart = 12288;
    CHECK(QlgDisplayPatch(&patch) == STATUS_SUCCESS);
    CHECK(dma.destination == 12288 && command.destination.offset == 12288);
    present.pDmaBuffer = &dma;
    present.pDmaBufferPrivateData = &command;
    present.pPatchLocationListOut = patches;
    allocations[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation = &source;
    CHECK(QlgMiniportPresent(&context, &present) == STATUS_INVALID_PARAMETER);
    CHECK(present.pDmaBuffer == &dma && present.pPatchLocationListOut == patches);
    allocations[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation = &source_open;
    present.Flags.Blt = 0;
    present.Flags.ColorFill = 1;
    present.PatchLocationListOutSize = 1;
    allocations[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation = NULL;
    CHECK(QlgMiniportPresent(&context, &present) == STATUS_SUCCESS);
    CHECK(present.pPatchLocationListOut == patches + 1 &&
          patches[0].AllocationIndex == DXGK_PRESENT_DESTINATION_INDEX);
    present.pDmaBuffer = &dma;
    present.pDmaBufferPrivateData = &command;
    present.pPatchLocationListOut = patches;

    for (index = 0; index < ARRAYSIZE(rects); ++index)
        rects[index].right = (LONG)index + 1;
    present.SubRectCnt = ARRAYSIZE(rects);
    present.pDstSubRects = rects;
    present.pDmaBuffer = &dma;
    present.pDmaBufferPrivateData = &command;
    present.pPatchLocationListOut = patches;
    CHECK(QlgMiniportPresent(&context, &present) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    CHECK(command.more_rects && command.rect_count == QLG_PRESENT_MAX_RECTS &&
          present.MultipassOffset == QLG_PRESENT_MAX_RECTS);
    present.pDmaBuffer = &dma;
    present.pDmaBufferPrivateData = &command;
    present.pPatchLocationListOut = patches;
    CHECK(QlgMiniportPresent(&context, &present) == STATUS_SUCCESS);
    CHECK(!command.more_rects && command.rect_count == 1 &&
          command.rects[0].right == (LONG)ARRAYSIZE(rects));
    present.SubRectCnt = present.MultipassOffset = 0;
    present.pDstSubRects = NULL;
    present.pDmaBuffer = &dma;
    present.pDmaBufferPrivateData = &command;
    present.pPatchLocationListOut = patches;
    present.Flags.ColorFill = 0;
    present.Flags.Flip = 1;
    CHECK(QlgMiniportPresent(&context, &present) == STATUS_SUCCESS);
    CHECK(present.pPatchLocationListOut == patches + 1);

    /* The MMIO flip path supplies no DMA or patch buffers. */
    RtlZeroMemory(&present, sizeof(present));
    present.pAllocationList = allocations;
    present.Flags.Flip = 1;
    present.Flags.FlipWithNoWait = 1;
    CHECK(QlgMiniportPresent(&context, &present) == STATUS_SUCCESS);
    CHECK(!present.pDmaBuffer && !present.pDmaBufferPrivateData &&
          !present.pPatchLocationListOut && !present.MultipassOffset);
    present.Flags.Flip = 0;
    present.Flags.FlipWithNoWait = 0;
    present.Flags.Blt = 1;
    CHECK(QlgMiniportPresent(&context, &present) == STATUS_INVALID_PARAMETER);
    present.Flags.Blt = 0;
    present.Flags.ColorFill = 1;
    CHECK(QlgMiniportPresent(&context, &present) == STATUS_INVALID_PARAMETER);
}

static UINT address_synchronizations;

static NTSTATUS APIENTRY synchronize_address(HANDLE device, PKSYNCHRONIZE_ROUTINE routine,
                                              void *context, ULONG message, BOOLEAN *result)
{
    CHECK(device == &adapter && message == QLG_MINIPORT_INTERRUPT_MESSAGE);
    ++address_synchronizations;
    *result = routine(context);
    return STATUS_SUCCESS;
}

/* DIRQL flips must never reacquire the interrupt lock or take a DPC lock. */
static void check_source_address(void)
{
    DXGKARG_SETVIDPNSOURCEADDRESS source = {0};
    const UINT64 primary_offset = QLM_PAGE_BYTES;
    const LONG64 gpu_image = ((LONG64)1 << FIXTURE_HANDLE_GENERATION_SHIFT) | FIXTURE_MODE_ID;
    QLG_OBJECT allocation = {0};
    UINT64 latched = 0;
    adapter.kernel.DeviceHandle = &adapter;
    adapter.kernel.DxgkCbSynchronizeExecution = synchronize_address;
    CHECK(!QlgDisplayLatchAddress(&adapter.display, &latched));
    source.PrimaryAddress.QuadPart = primary_offset;
    source.PrimarySegment = QLM_MEMORY_SEGMENT;
    CHECK(QlgMiniportSourceAddress(&adapter, &source) == STATUS_SUCCESS);
    CHECK(address_synchronizations == 1 && adapter.display.address == primary_offset);
    CHECK(QlgDisplayLatchAddress(&adapter.display, &latched) && latched == primary_offset);
    CHECK(!QlgDisplayLatchAddress(&adapter.display, &latched));
    source.ContextCount = 1;
    allocation.kind = QLG_KIND_ALLOCATION;
    allocation.gpu_scanout_resource = gpu_image;
    source.hAllocation = &allocation;
    source.Flags.FlipOnNextVSync = 1;
    source.PrimaryAddress.QuadPart += QLM_PAGE_BYTES;
    CHECK(QlgMiniportSourceAddress(&adapter, &source) == STATUS_SUCCESS);
    CHECK(address_synchronizations == 1 && adapter.display.flip_pending &&
          adapter.display.pending == primary_offset + QLM_PAGE_BYTES &&
          adapter.display.address == primary_offset && adapter.display.gpu_resource == 0 &&
          adapter.display.pending_gpu_resource == (UINT64)gpu_image);
    CHECK(QlgDisplayLatchAddress(&adapter.display, &latched) &&
          latched == primary_offset + QLM_PAGE_BYTES && !adapter.display.flip_pending &&
          adapter.display.gpu_resource == (UINT64)gpu_image);
    CHECK(!QlgDisplayLatchAddress(&adapter.display, &latched));
    /* A fresh flip to the same allocation still contains a new completed frame. */
    CHECK(QlgMiniportSourceAddress(&adapter, &source) == STATUS_SUCCESS);
    CHECK(QlgDisplayLatchAddress(&adapter.display, &latched));
    CHECK(!QlgDisplayLatchAddress(&adapter.display, &latched));
    source.Flags.FlipOnNextVSync = 0;
    CHECK(QlgMiniportSourceAddress(&adapter, &source) == STATUS_SUCCESS);
    CHECK(address_synchronizations == 1 && !adapter.display.flip_pending &&
          adapter.display.address == primary_offset + QLM_PAGE_BYTES);
    CHECK(QlgDisplayLatchAddress(&adapter.display, &latched));
    CHECK(!QlgDisplayLatchAddress(&adapter.display, &latched));

    /* A mode change cannot publish the previous mode's image or queued flip.
     * An outstanding host read still owns its completion independently. */
    QlgDisplaySetMode(&adapter.display, LIGHTNING_SCANOUT_MODE_0_WIDTH,
                      LIGHTNING_SCANOUT_MODE_0_HEIGHT,
                      LIGHTNING_SCANOUT_MODE_0_WIDTH * LIGHTNING_SCANOUT_PIXEL_BYTES);
    source.Flags.FlipOnNextVSync = 1;
    CHECK(QlgMiniportSourceAddress(&adapter, &source) == STATUS_SUCCESS);
    adapter.display.gpu_inflight = TRUE;
    QlgDisplaySetMode(&adapter.display, LIGHTNING_SCANOUT_MODE_1_WIDTH,
                      LIGHTNING_SCANOUT_MODE_1_HEIGHT,
                      LIGHTNING_SCANOUT_MODE_1_WIDTH * LIGHTNING_SCANOUT_PIXEL_BYTES);
    CHECK(!adapter.display.flip_pending && adapter.display.gpu_inflight);
    CHECK(!QlgDisplayLatchAddress(&adapter.display, &latched));
    CHECK(adapter.display.width == LIGHTNING_SCANOUT_MODE_1_WIDTH &&
          adapter.display.height == LIGHTNING_SCANOUT_MODE_1_HEIGHT);
    CHECK(QlgMiniportSourceAddress(&adapter, &source) == STATUS_SUCCESS);
    CHECK(QlgDisplayLatchAddress(&adapter.display, &latched));
    CHECK(adapter.display.gpu_resource == (UINT64)gpu_image);
    QlgDisplaySetMode(&adapter.display, LIGHTNING_SCANOUT_MODE_1_WIDTH,
                      LIGHTNING_SCANOUT_MODE_1_HEIGHT,
                      LIGHTNING_SCANOUT_MODE_1_WIDTH * LIGHTNING_SCANOUT_PIXEL_BYTES);
    CHECK(QlgDisplayLatchAddress(&adapter.display, &latched));
    /* Cancellation retires an outstanding host read and acknowledges any
     * completion left over from the canceled generation. */
    display_writes = 0;
    QlgDisplayCancelFrame(&adapter);
    CHECK(!adapter.display.gpu_inflight && display_writes == 2);
    CHECK(adapter.display.shown_address == adapter.display.gpu_inflight_address);
    CHECK(adapter.display.counters[QLG_DISPLAY_CANCELED_GPU_READS] == 1);
    CHECK(display_registers[LIGHTNING_SCANOUT_REG_COMMAND / LIGHTNING_SCANOUT_REGISTER_BYTES] ==
          LIGHTNING_SCANOUT_DISABLE);
    CHECK(display_registers[LIGHTNING_SCANOUT_REG_IRQ / LIGHTNING_SCANOUT_REGISTER_BYTES] ==
          LIGHTNING_SCANOUT_GPU_COMPLETED);
}

static UINT vsyncs, dpcs;
static UINT64 vsync_address;

static void APIENTRY record_vsync(IN_CONST_HANDLE device,
                                  IN_CONST_PDXGKARGCB_NOTIFY_INTERRUPT_DATA data)
{
    CHECK(device == &adapter && data->InterruptType == DXGK_INTERRUPT_CRTC_VSYNC &&
          data->CrtcVsync.VidPnTargetId == LIGHTNING_SCANOUT_TARGET_ID);
    vsync_address = (UINT64)data->CrtcVsync.PhysicalAddress.QuadPart;
    vsyncs++;
}

static BOOLEAN APIENTRY record_dpc(HANDLE device)
{
    CHECK(device == &adapter);
    dpcs++;
    return TRUE;
}

/* One vertical blank exactly as miniport_vidpn's timer runs it. Every tick
 * must notify; the reported address tells Dxgkrnl which flip is visible. */
static UINT64 vertical_blank(void)
{
    QLG_VSYNC_REPORT report = {.miniport = &adapter};
    UINT before = vsyncs;
    CHECK(QlgDisplayTick(&report) && report.notify);
    CHECK(QlgDisplayNotifyVsync(&report) && vsyncs == before + 1 && dpcs == vsyncs);
    CHECK(vsync_address == report.address);
    return vsync_address;
}

/* An MMIO flip at DIRQL, as Dxgkrnl issues it after the previous flip retired. */
static void mmio_flip(QLG_OBJECT *allocation, UINT64 address, UINT64 resource)
{
    DXGKARG_SETVIDPNSOURCEADDRESS source = {0};
    allocation->kind = QLG_KIND_ALLOCATION;
    allocation->gpu_scanout_resource = resource;
    source.hAllocation = allocation;
    source.PrimarySegment = QLM_MEMORY_SEGMENT;
    source.PrimaryAddress.QuadPart = (LONGLONG)address;
    source.ContextCount = 1;
    source.Flags.FlipOnNextVSync = 1;
    CHECK(QlgMiniportSourceAddress(&adapter, &source) == STATUS_SUCCESS);
}

static void start_display(BOOLEAN interrupts)
{
    RtlZeroMemory(&adapter.display, sizeof(adapter.display));
    adapter.venus_transport = &adapter;
    adapter.kernel.DeviceHandle = &adapter;
    adapter.kernel.DxgkCbSynchronizeExecution = synchronize_address;
    adapter.kernel.DxgkCbNotifyInterrupt = record_vsync;
    adapter.kernel.DxgkCbQueueDpc = record_dpc;
    adapter.display.committed = adapter.display.visible = adapter.display.vsync_enabled = TRUE;
    adapter.display.gpu_irq_enabled = interrupts;
    QlgDisplaySetMode(&adapter.display, LIGHTNING_SCANOUT_MODE_0_WIDTH,
                      LIGHTNING_SCANOUT_MODE_0_HEIGHT,
                      LIGHTNING_SCANOUT_MODE_0_WIDTH * LIGHTNING_SCANOUT_PIXEL_BYTES);
    host_status = LIGHTNING_SCANOUT_IDLE;
    host_completed = FALSE;
    vsyncs = dpcs = display_presents = 0;
}

static void check_adapter_suspend_resume(void)
{
    const UINT64 gpu_image = ((UINT64)1 << FIXTURE_HANDLE_GENERATION_SHIFT) | FIXTURE_MODE_ID;
    QLG_OBJECT allocation = {0};
    QLG_VSYNC_REPORT report = {.miniport = &adapter};
    UINT writes;

    start_display(TRUE);
    mmio_flip(&allocation, QLM_PAGE_BYTES, gpu_image);
    CHECK(adapter.display.gpu_inflight && display_presents == 1);

    CHECK(QlgDisplaySuspendAdapter(&adapter, TRUE) == STATUS_SUCCESS);
    writes = display_writes;
    CHECK(QlgDisplayTick(&report) && !report.notify);
    CHECK(adapter.display.gpu_inflight && adapter.display.flip_pending &&
          display_writes == writes && adapter.display.visible);

    CHECK(QlgDisplaySuspendAdapter(&adapter, FALSE) == STATUS_SUCCESS);
    CHECK(!adapter.display.programmed_known && adapter.display.gpu_inflight);
    CHECK(vertical_blank() == 0 && display_writes == writes);
    host_finish(LIGHTNING_SCANOUT_ACTIVE, TRUE);
    CHECK(QlgDisplayInterrupt(&adapter, TRUE));
    CHECK(vertical_blank() == QLM_PAGE_BYTES && display_presents == 1);

    /* A new flip published during suspend waits until resume. */
    CHECK(QlgDisplaySuspendAdapter(&adapter, TRUE) == STATUS_SUCCESS);
    writes = display_writes;
    mmio_flip(&allocation, 2 * QLM_PAGE_BYTES, gpu_image);
    CHECK(adapter.display.flip_pending && !adapter.display.gpu_inflight);
    CHECK(QlgDisplayTick(&report) && !report.notify && display_writes == writes);
    CHECK(QlgDisplaySuspendAdapter(&adapter, FALSE) == STATUS_SUCCESS);
    CHECK(vertical_blank() == QLM_PAGE_BYTES && adapter.display.gpu_inflight &&
          display_writes == writes + FIXTURE_FRAME_REGISTER_WRITES);
    CHECK(REGISTER(LIGHTNING_SCANOUT_REG_RESOURCE_LO) == FIXTURE_MODE_ID);
    host_finish(LIGHTNING_SCANOUT_ACTIVE, TRUE);
    CHECK(QlgDisplayInterrupt(&adapter, TRUE));
    CHECK(vertical_blank() == 2 * QLM_PAGE_BYTES);
}

/* Timer and acknowledgement ordering for GPU flips (2026-10-16). Candidate 39
 * accepted DMA-free MMIO flips; candidate 40 moved address and latch state
 * into the interrupt domain; candidate 70 synchronized submission, hiding and
 * cancellation there. All of that holds; what changes is that a host read no
 * longer suppresses vertical blanks or reports a second one from the ISR. */
/* The vertical-blank deadline stays on the 60 Hz grid however late the timer is. */
static void check_vblank_deadline(void)
{
    const LONGLONG period = LIGHTNING_SCANOUT_VBLANK_PERIOD_100NS;

    CHECK(QlgDisplayNextVblank(1000, 999) == 1000 + period);
    CHECK(QlgDisplayNextVblank(1000, 1000) == 1000 + period);
    CHECK(QlgDisplayNextVblank(1000, 1000 + 15000) == 1000 + period);
    CHECK(QlgDisplayNextVblank(1000, 1000 + period) == 1000 + 2 * period);
    CHECK(QlgDisplayNextVblank(1000, 1000 + 3 * period + 7) == 1000 + 4 * period);
    CHECK(QlgDisplayNextVblank(1000, 1000 + period - 1000) == 1000 + 2 * period);
    {
        LONGLONG due = 0, now;
        UINT ticks = 0;

        /* Every expiry 1.5 ms late: 600 ticks still span 10 s of the grid. */
        for (now = 0; ticks < 600; ++ticks) {
            now = due + 15000;
            due = QlgDisplayNextVblank(due, now);
        }
        CHECK(due == 600 * period);
    }
}

static void check_flip_pacing(void)
{
    enum { BUFFER_A = 1, BUFFER_B = 2, BUFFER_C = 3, CPU_BUFFER = 4 };
    const UINT64 context = (UINT64)1 << FIXTURE_HANDLE_GENERATION_SHIFT;
    QLG_OBJECT first = {0}, second = {0}, third = {0}, cpu = {0};
    UINT presents, reads, writes, tick;

    start_display(TRUE);
    /* A GPU flip reaches QEMU when it arrives, not at the next tick, and
     * the first frame after start writes every register. */
    mmio_flip(&first, BUFFER_A * QLM_PAGE_BYTES, context | BUFFER_A);
    CHECK(display_presents == 1 && host_status == LIGHTNING_SCANOUT_PENDING);
    CHECK(adapter.display.gpu_inflight && adapter.display.flip_started);
    CHECK(adapter.display.counters[QLG_DISPLAY_IMMEDIATE_FLIPS] == 1);
    CHECK(adapter.display.counters[QLG_DISPLAY_SCANOUT_WRITES] == FIXTURE_FRAME_REGISTER_WRITES);
    /* The vertical blank during the host read still notifies, reporting the
     * previous image: the flip stays pending without stalling the clock. */
    CHECK(vertical_blank() == 0 && adapter.display.counters[QLG_DISPLAY_HELD_VSYNCS] == 1);
    CHECK(display_presents == 1 && adapter.display.address == 0);
    /* The completion interrupt only records the finished read. */
    host_finish(LIGHTNING_SCANOUT_ACTIVE, TRUE);
    CHECK(QlgDisplayInterrupt(&adapter, FALSE) && !host_completed && vsyncs == 1);
    CHECK(!adapter.display.gpu_inflight);
    /* The next vertical blank retires it and adopts the primary without
     * sending the frame twice. */
    CHECK(vertical_blank() == BUFFER_A * QLM_PAGE_BYTES && display_presents == 1);
    CHECK(adapter.display.address == BUFFER_A * QLM_PAGE_BYTES &&
          adapter.display.gpu_resource == (context | BUFFER_A) && !adapter.display.flip_pending);
    /* Idle refresh periods keep notifying the same image without MMIO. */
    reads = display_reads;
    writes = display_writes;
    CHECK(vertical_blank() == BUFFER_A * QLM_PAGE_BYTES);
    CHECK(display_reads == reads && display_writes == writes && display_presents == 1);
    /* An ISR for another source (Venus) does not read the display latch
     * while nothing is in flight. */
    CHECK(!QlgDisplayInterrupt(&adapter, FALSE) && display_reads == reads);

    /* Steady flipping between two buffers of one context sends only the
     * changed registers: OFFSET, RESOURCE_LO and COMMAND. */
    writes = display_writes;
    mmio_flip(&second, BUFFER_B * QLM_PAGE_BYTES, context | BUFFER_B);
    CHECK(display_writes - writes == FIXTURE_FLIP_REGISTER_WRITES && display_presents == 2);
    CHECK(REGISTER(LIGHTNING_SCANOUT_REG_RESOURCE_HI) == 1 &&
          REGISTER(LIGHTNING_SCANOUT_REG_OFFSET) == BUFFER_B * QLM_PAGE_BYTES);

    /* A slow host read: every tick still notifies with the previous image
     * and no STATUS read happens before QLG_GPU_READ_POLL_TICKS. */
    reads = display_reads;

    for (tick = 1; tick < QLG_GPU_READ_POLL_TICKS; ++tick)
        CHECK(vertical_blank() == BUFFER_A * QLM_PAGE_BYTES);
    CHECK(display_reads == reads && display_presents == 2);
    host_finish(LIGHTNING_SCANOUT_ACTIVE, TRUE);
    CHECK(QlgDisplayInterrupt(&adapter, FALSE));
    CHECK(vertical_blank() == BUFFER_B * QLM_PAGE_BYTES);

    /* Hosts before 2026-10-16 reject a frame without GPU_COMPLETED. The
     * tick's STATUS poll retires it instead of holding VSYNC forever. */
    mmio_flip(&third, BUFFER_C * QLM_PAGE_BYTES, context | BUFFER_C);
    host_finish(LIGHTNING_SCANOUT_INVALID, FALSE);
    CHECK(!QlgDisplayInterrupt(&adapter, TRUE));

    for (tick = 1; tick < QLG_GPU_READ_POLL_TICKS; ++tick)
        CHECK(vertical_blank() == BUFFER_B * QLM_PAGE_BYTES);
    CHECK(vertical_blank() == BUFFER_C * QLM_PAGE_BYTES && !adapter.display.gpu_inflight);
    CHECK(adapter.display.counters[QLG_DISPLAY_REJECTED_GPU_READS] == 1 &&
          adapter.display.counters[QLG_DISPLAY_POLLED_GPU_READS] == 1);
    /* The next flip starts at once: nothing is left in flight. */
    presents = display_presents;
    mmio_flip(&first, BUFFER_A * QLM_PAGE_BYTES, context | BUFFER_A);
    CHECK(display_presents == presents + 1);
    /* Current hosts complete a rejected frame through the interrupt. */
    host_finish(LIGHTNING_SCANOUT_INVALID, TRUE);
    CHECK(QlgDisplayInterrupt(&adapter, FALSE) && !host_completed);
    CHECK(vertical_blank() == BUFFER_A * QLM_PAGE_BYTES);

    /* A stale completion with nothing in flight is still cleared when no
     * other source claims the interrupt, so the shared line cannot storm. */
    host_completed = TRUE;
    CHECK(QlgDisplayInterrupt(&adapter, TRUE) && !host_completed);

    /* Hiding cancels the started read and retires its flip; showing again
     * rewrites every register and sends the adopted frame once more. */
    mmio_flip(&second, BUFFER_B * QLM_PAGE_BYTES, context | BUFFER_B);
    adapter.display.visible = FALSE;
    adapter.display.programmed_known = FALSE;
    QlgDisplayCancelFrame(&adapter);
    CHECK(!adapter.display.flip_started && host_status == LIGHTNING_SCANOUT_IDLE);
    presents = display_presents;
    CHECK(vertical_blank() == BUFFER_B * QLM_PAGE_BYTES && display_presents == presents);
    adapter.display.visible = TRUE;
    InterlockedExchange(&adapter.display.scanout_pending, TRUE);
    writes = display_writes;
    CHECK(vertical_blank() == BUFFER_B * QLM_PAGE_BYTES && display_presents == presents + 1);
    CHECK(display_writes - writes == FIXTURE_FRAME_REGISTER_WRITES && adapter.display.gpu_inflight);
    host_finish(LIGHTNING_SCANOUT_ACTIVE, TRUE);
    CHECK(QlgDisplayInterrupt(&adapter, FALSE));
    CHECK(vertical_blank() == BUFFER_B * QLM_PAGE_BYTES);

    /* CPU frames still latch at the tick, under the scanout lock, and are
     * shown by the PRESENT write itself. */
    mmio_flip(&cpu, CPU_BUFFER * QLM_PAGE_BYTES, 0);
    CHECK(display_presents == presents + 1 && adapter.display.flip_pending);
    CHECK(vertical_blank() == CPU_BUFFER * QLM_PAGE_BYTES && display_presents == presents + 2);
    CHECK(!adapter.display.gpu_inflight && host_status == LIGHTNING_SCANOUT_ACTIVE);

    /* Hosts without GPU interrupts are polled every tick, as before. */
    start_display(FALSE);
    mmio_flip(&first, BUFFER_A * QLM_PAGE_BYTES, context | BUFFER_A);
    CHECK(vertical_blank() == 0);
    host_finish(LIGHTNING_SCANOUT_ACTIVE, FALSE);
    writes = display_writes;
    CHECK(vertical_blank() == BUFFER_A * QLM_PAGE_BYTES && display_writes == writes);
    CHECK(adapter.display.counters[QLG_DISPLAY_POLLED_GPU_READS] == 0);
    RtlZeroMemory(&adapter.display, sizeof(adapter.display));
}

/* Native user-mode fixture: these locks replace kernel IRQL operations only. */
KIRQL QlgKernelLock(void *context)
{
    CHECK(context == &adapter.display.scanout_lock);
    return 0;
}

void QlgKernelUnlock(void *context, KIRQL level)
{
    CHECK(context == &adapter.display.scanout_lock && level == 0);
}

/* A timer between rectangle packets must retain the previous complete frame. */
static void check_multipass_visibility(void)
{
    enum { FIXTURE_WIDTH = 16, FIXTURE_HEIGHT = 16, FIXTURE_COLOR = 0x0012ab34 };
    static UINT pixels[FIXTURE_WIDTH * FIXTURE_HEIGHT];
    static QLG_ADAPTER core;
    QLG_PRESENT_COMMAND command = {0};
    UINT64 address;
    UINT index;

    adapter.lifecycle.adapter = &core;
    adapter.display.address = 0;
    adapter.display.scanout_pending = TRUE;
    core.paging.memory = (volatile UCHAR *)pixels;
    core.paging.memory_bytes = sizeof(pixels);
    command.operation = QLG_PRESENT_FILL;
    command.color = FIXTURE_COLOR;
    command.destination.segment = QLM_MEMORY_SEGMENT;
    command.destination.width = FIXTURE_WIDTH;
    command.destination.height = FIXTURE_HEIGHT;
    command.destination.pitch = FIXTURE_WIDTH * sizeof(pixels[0]);
    command.rect_count = 1;
    command.rects[0].right = FIXTURE_WIDTH / 2;
    command.rects[0].bottom = FIXTURE_HEIGHT;
    command.more_rects = TRUE;
    CHECK(QlgDisplayExecute(&adapter, &command) == STATUS_SUCCESS);
    CHECK(pixels[0] == FIXTURE_COLOR && pixels[FIXTURE_WIDTH / 2] == 0);
    CHECK(!QlgDisplayLatchAddress(&adapter.display, &address));
    command.rects[0].left = FIXTURE_WIDTH / 2;
    command.rects[0].right = FIXTURE_WIDTH;
    command.more_rects = FALSE;
    CHECK(QlgDisplayExecute(&adapter, &command) == STATUS_SUCCESS);
    CHECK(QlgDisplayLatchAddress(&adapter.display, &address) && address == 0);

    for (index = 0; index < ARRAYSIZE(pixels); ++index)
        CHECK(pixels[index] == FIXTURE_COLOR);
    adapter.lifecycle.adapter = NULL;
}

void main(void)
{
    check_display_programming();
    DXGK_MONITORSOURCEMODESET_INTERFACE interface = {0};
    DXGKARG_RECOMMENDMONITORMODES args = {0};
    DXGK_CHILD_DESCRIPTOR children[2];
    DXGK_CHILD_STATUS child = {0};
    DXGK_DEVICE_DESCRIPTOR descriptor = {0};
    check_empty_paging();
    check_paging_copy_bytes();
    check_finished_paging_handoff();
    check_discard_during_reset();
    check_texture_aperture();
    check_present_residency();
    check_source_address();
    check_vblank_deadline();
    check_adapter_suspend_resume();
    check_flip_pacing();
    check_multipass_visibility();
    interface.pfnCreateNewModeInfo = create_mode;
    interface.pfnAddMode = add_mode;
    interface.pfnReleaseModeInfo = release_mode;
    args.hMonitorSourceModeSet = (D3DKMDT_HMONITORSOURCEMODESET)&mode;
    args.pMonitorSourceModeSetInterface = &interface;
    CHECK(QlgMiniportMonitorModes(&adapter, &args) == STATUS_DEVICE_NOT_READY);
    CHECK(QlgMiniportChildren(&adapter, children, sizeof(children[0])) == STATUS_SUCCESS);
    CHECK(children[0].ChildDeviceType == TypeUninitialized && creates == 0);
    /* Only this fixture activates the staged callbacks; production does not. */
    adapter.started = adapter.display_ready = TRUE;
    CHECK(QlgMiniportChildren(&adapter, children, sizeof(children[0])) == STATUS_BUFFER_TOO_SMALL);
    CHECK(QlgMiniportChildren(&adapter, children, sizeof(children)) == STATUS_SUCCESS);
    CHECK(children[0].ChildDeviceType == TypeVideoOutput &&
          children[1].ChildDeviceType == TypeUninitialized);
    child.Type = StatusConnection;
    CHECK(QlgMiniportChildStatus(&adapter, &child, TRUE) == STATUS_SUCCESS &&
          child.HotPlug.Connected);
    child.ChildUid = LIGHTNING_SCANOUT_TARGET_ID + 1;
    CHECK(QlgMiniportChildStatus(&adapter, &child, TRUE) == STATUS_INVALID_PARAMETER);
    {
        UCHAR edid[EDID_TEST_BYTES];
        descriptor.DescriptorBuffer = edid;
        descriptor.DescriptorLength = sizeof(edid);
        CHECK(QlgMiniportDeviceDescriptor(&adapter, LIGHTNING_SCANOUT_TARGET_ID, &descriptor) ==
              STATUS_SUCCESS);
        descriptor.DescriptorOffset = EDID_TEST_BYTES;
        CHECK(QlgMiniportDeviceDescriptor(&adapter, LIGHTNING_SCANOUT_TARGET_ID, &descriptor) ==
              STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA);
    }
    CHECK(QlgMiniportMonitorModes(&adapter, &args) == STATUS_SUCCESS &&
          adds == LIGHTNING_SCANOUT_MODE_COUNT && releases == 0);
    create_result = STATUS_NO_MEMORY;
    CHECK(QlgMiniportMonitorModes(&adapter, &args) == STATUS_NO_MEMORY &&
          adds == LIGHTNING_SCANOUT_MODE_COUNT && releases == 0);
    create_result = STATUS_SUCCESS;
    add_result = STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET;
    CHECK(QlgMiniportMonitorModes(&adapter, &args) == STATUS_SUCCESS &&
          releases == LIGHTNING_SCANOUT_MODE_COUNT);
    add_result = STATUS_NO_MEMORY;
    CHECK(QlgMiniportMonitorModes(&adapter, &args) == STATUS_NO_MEMORY &&
          releases == LIGHTNING_SCANOUT_MODE_COUNT + 1);
    release_result = STATUS_UNSUCCESSFUL;
    CHECK(QlgMiniportMonitorModes(&adapter, &args) == STATUS_UNSUCCESSFUL &&
          releases == LIGHTNING_SCANOUT_MODE_COUNT + 2);
    args.VideoPresentTargetId = LIGHTNING_SCANOUT_TARGET_ID + 1;
    CHECK(QlgMiniportMonitorModes(&adapter, &args) == STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET);
    args.VideoPresentTargetId = LIGHTNING_SCANOUT_TARGET_ID;
    interface.pfnAddMode = NULL;
    CHECK(QlgMiniportMonitorModes(&adapter, &args) == STATUS_INVALID_PARAMETER);
    adapter.stopping = TRUE;
    CHECK(QlgMiniportMonitorModes(&adapter, &args) == STATUS_DEVICE_NOT_READY);
    ExitProcess(0);
}

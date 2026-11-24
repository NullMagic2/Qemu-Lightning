/*
 * Module: lightning.direct3d12_umd.native_image_upload_commands_fixture
 * Purpose: Verify genuine R0 uploads, source footprints and retained ownership.
 * Created: 2026-10-25
 * Architecture: Genuine callbacks and actual native Map/readback expose uploaded
 * bytes through isolated modeled Vulkan ABI; CPU Vulkan is checked separately.
 */
#define fixture_entry readback_only_fixture_entry
#include "native_image_readback_commands_fixture.c"
#undef fixture_entry
#include "native_image_upload_commands.h"
typedef HRESULT (APIENTRY *GET_UPLOAD)(UINT, UINT, QLDX12_NATIVE_IMAGE_UPLOAD_COMMAND_FUNCTIONS *);
static QLDX12_NATIVE_IMAGE_UPLOAD_COMMAND_FUNCTIONS uploads;
static SECOND_IMAGE upload_source;
static MODEL_IMAGE_UPLOAD_COUNTERS upload_counters;
enum { UPLOAD_PREFIX = 512, UPLOAD_PITCH = 512, UPLOAD_POISON = 0x5A,
    UPLOAD_ROW_FACTOR = 13, UPLOAD_COLUMN_FACTOR = 7, UPLOAD_CHANNEL_FACTOR = 61 };

enum invalid_upload_scenario {
    UPLOAD_ZERO_PITCH, UPLOAD_MISALIGNED_PITCH, UPLOAD_SHORT_SLICE, UPLOAD_BAD_FORMAT,
    UPLOAD_BAD_WIDTH, UPLOAD_BAD_HEIGHT, UPLOAD_BAD_DEPTH, UPLOAD_NONZERO_SUBRESOURCE,
    UPLOAD_NONZERO_X, UPLOAD_SOURCE_BOX, UPLOAD_BAD_ALIGNMENT, UPLOAD_LOGICAL_OVERRUN,
    UPLOAD_WRONG_SOURCE_KIND, UPLOAD_MAPPED_SOURCE, UPLOAD_WRONG_SOURCE_LAYOUT,
    UPLOAD_OVERFLOW_PITCH, UPLOAD_OVERFLOW_OFFSET,
    UPLOAD_MISSING_OPEN, UPLOAD_MISSING_COPY, UPLOAD_WRONG_OPEN_STATE,
    UPLOAD_INVALID_SCENARIO_COUNT
};

static BYTE upload_pattern(UINT row, UINT x, UINT channel)
{
    return (BYTE)(row * UPLOAD_ROW_FACTOR + x * UPLOAD_COLUMN_FACTOR + channel * UPLOAD_CHANNEL_FACTOR);
}

static UINT upload_logical_bytes = READBACK_LOGICAL_BYTES;
static UINT upload_pitch = UPLOAD_PITCH;
static UINT target_texel_bytes = READBACK_CHANNELS;

static void create_upload_source(void)
{
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 heap = {0};
    D3D12DDIARG_CREATERESOURCE_0003 description = {0};
    void *pointer = NULL;

    upload_source.heap_storage = storage();
    upload_source.resource_storage = storage();
    upload_source.heap.pDrvPrivate = &upload_source.heap_storage;
    upload_source.resource.pDrvPrivate = &upload_source.resource_storage;
    heap.ByteSize = READBACK_HEAP_BYTES;
    heap.Alignment = READBACK_HEAP_BYTES;
    heap.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    description.ResourceType = D3D12DDI_RT_BUFFER;
    description.Width = upload_logical_bytes;
    description.Height = description.DepthOrArraySize = description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12DDI_TL_ROW_MAJOR;
    description.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(resources.CreateHeapAndResource(owner.device, &heap, upload_source.heap, runtime,
        &description, NULL, upload_source.resource) == S_OK, "actual admitted same-device L0 WRITE_BACK/COPY_SOURCE native upload source");
    check(resources.MapHeap(owner.device, upload_source.heap, &pointer) == S_OK && pointer,
        "native Map supplies actual upload backing");
    FillMemory(pointer, READBACK_HEAP_BYTES, UPLOAD_POISON);

    for (UINT y = 0; y < target_height * target_depth; ++y) {
        for (UINT x = 0; x < target_width; ++x) {
            for (UINT channel = 0; channel < target_texel_bytes; ++channel) {
                ((BYTE *)pointer)[UPLOAD_PREFIX + y * upload_pitch + x * target_texel_bytes + channel] =
                    upload_pattern(y, x, channel);
            }
        }
    }
    resources.UnmapHeap(owner.device, upload_source.heap);
}

static D3D12DDI_HCOMMANDLIST upload_list(FIXTURE_STORAGE *memory, UINT allocator_index)
{
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};

    if (unified_r0) {
        uploads.CalcPrivateCommandListSize = unified_core.pfnCalcPrivateCommandListSize;
        uploads.CreateCommandList = unified_core.pfnCreateCommandList;
        uploads.ResourceBarrier = unified_list.pfnResourceBarrier;
        uploads.CopyTextureRegion = unified_list.pfnCopyTextureRegion;
        uploads.CloseCommandList = unified_list.pfnCloseCommandList;
        uploads.ExecuteCommandLists = unified_queue.pfnExecuteCommandLists;
        uploads.ResetCommandList = unified_list.pfnResetCommandList;
    }
    args.hDrvCommandList.pDrvPrivate = memory;
    args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    args.hDrvCommandAllocator = allocators[allocator_index];
    args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(uploads.CalcPrivateCommandListSize(owner.device, &args) == sizeof(uint64_t) &&
        uploads.CreateCommandList(owner.device, &args) == S_OK, "additive genuine original R0 native upload list");
    return args.hDrvCommandList;
}

static void upload_barrier(D3D12DDI_HCOMMANDLIST list, BOOL closing)
{
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = transition(closing);

    barrier.Transition.StateBefore = closing ? D3D12DDI_RESOURCE_STATE_COPY_DEST : D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = closing ? D3D12DDI_RESOURCE_STATE_RENDER_TARGET : D3D12DDI_RESOURCE_STATE_COPY_DEST;
    uploads.ResourceBarrier(list, 1, &barrier);
    SecureZeroMemory(&barrier, sizeof(barrier));
}

static void upload_copy(D3D12DDI_HCOMMANDLIST list)
{
    D3D12DDIARG_BUFFER_PLACEMENT source = {0}, destination = {0};
    D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT physical = physical_layout();
    D3D12DDIARG_PLACED_RESOURCE source_layout = {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &physical};
    D3D12DDIARG_PLACED_RESOURCE destination_layout = {D3D12DDI_RL_SELECT_SUBRESOURCE, (void *)(uintptr_t)1};

    physical.Pitch = upload_pitch;
    physical.SlicePitch = upload_pitch * target_height;
    source.BaseAddress.UMD.hResource = upload_source.resource;
    source.BaseAddress.UMD.Offset = UPLOAD_PREFIX;
    destination.BaseAddress.UMD.hResource = owner.resource;
    destination.BaseAddress.UMD.Offset = copy_layer;
    uploads.CopyTextureRegion(list, &destination, destination_layout, 0, 0, 0, &source, source_layout, NULL);
    SecureZeroMemory(&source, sizeof(source));
    SecureZeroMemory(&destination, sizeof(destination));
    SecureZeroMemory(&physical, sizeof(physical));
}

static void upload_transaction(D3D12DDI_HCOMMANDLIST list)
{
    upload_barrier(list, FALSE);
    upload_copy(list);
    upload_barrier(list, TRUE);
}

static void upload_busy(void)
{
    void *pointer = (void *)STORAGE_SENTINEL;

    check(resources.MapHeap(owner.device, upload_source.heap, &pointer) == DXGI_ERROR_WAS_STILL_DRAWING &&
        pointer == (void *)STORAGE_SENTINEL &&
        resources.RetireHeapAndResource(owner.device, upload_source.heap, upload_source.resource) == DXGI_ERROR_WAS_STILL_DRAWING &&
        images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING,
        "exact source read lease and initialized image remain pinned; Map output is preserved");
}

static void invalid_uploads(void)
{
    for (UINT scenario = 0; scenario < UPLOAD_INVALID_SCENARIO_COUNT; ++scenario) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = upload_list(&memory, 0);
        D3D12DDIARG_BUFFER_PLACEMENT source = {0}, destination = {0};
        D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT physical = physical_layout();
        D3D12DDIARG_PLACED_RESOURCE source_layout = {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &physical};
        D3D12DDIARG_PLACED_RESOURCE destination_layout = {D3D12DDI_RL_SELECT_SUBRESOURCE, (void *)(uintptr_t)1};
        const D3D12DDI_BOX *box = NULL;
        void *mapped = NULL;
        UINT destination_x = 0;
        LONG errors;

        source.BaseAddress.UMD.hResource = upload_source.resource;
        source.BaseAddress.UMD.Offset = UPLOAD_PREFIX;
        destination.BaseAddress.UMD.hResource = owner.resource;
        physical.Pitch = UPLOAD_PITCH;
        physical.SlicePitch = UPLOAD_PITCH * target_height;

        if (scenario != UPLOAD_MISSING_OPEN && scenario != UPLOAD_WRONG_OPEN_STATE) {
            upload_barrier(list, FALSE);
        }
        switch (scenario) {
        case UPLOAD_ZERO_PITCH: physical.Pitch = 0; break;
        case UPLOAD_MISALIGNED_PITCH: physical.Pitch += 1; break;
        case UPLOAD_SHORT_SLICE: physical.SlicePitch -= QLDX12_NC_IMAGE_COPY_ROW_ALIGNMENT; break;
        case UPLOAD_BAD_FORMAT: physical.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case UPLOAD_BAD_WIDTH: physical.PhysicalWidth -= 1; break;
        case UPLOAD_BAD_HEIGHT: physical.PhysicalHeight += 1; break;
        case UPLOAD_BAD_DEPTH: physical.PhysicalDepth += 1; break;
        case UPLOAD_NONZERO_SUBRESOURCE: destination.BaseAddress.UMD.Offset = 1; break;
        case UPLOAD_NONZERO_X: destination_x = 1; break;
        case UPLOAD_SOURCE_BOX: box = (const D3D12DDI_BOX *)(uintptr_t)1; break;
        case UPLOAD_BAD_ALIGNMENT: source.BaseAddress.UMD.Offset = 1; break;
        case UPLOAD_LOGICAL_OVERRUN: source.BaseAddress.UMD.Offset = 8192; break;
        case UPLOAD_WRONG_SOURCE_KIND: source.BaseAddress.UMD.hResource = readback.resource; break;
        case UPLOAD_MAPPED_SOURCE:
            check(resources.MapHeap(owner.device, upload_source.heap, &mapped) == S_OK && mapped,
                "source maps before candidate upload read ownership");
            break;
        case UPLOAD_WRONG_SOURCE_LAYOUT: source_layout.Layout = D3D12DDI_RL_SELECT_SUBRESOURCE; break;
        case UPLOAD_OVERFLOW_PITCH: physical.Pitch = UINT32_MAX & ~(QLDX12_NC_IMAGE_COPY_ROW_ALIGNMENT - 1u); break;
        case UPLOAD_OVERFLOW_OFFSET: source.BaseAddress.UMD.Offset = UINT64_MAX - (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u); break;
        default: break;
        }
        errors = error_count;

        if (scenario == UPLOAD_MISSING_COPY) {
            upload_barrier(list, TRUE);
        } else if (scenario == UPLOAD_WRONG_OPEN_STATE) {
            D3D12DDIARG_RESOURCE_BARRIER_0003 wrong = transition(FALSE);

            uploads.ResourceBarrier(list, 1, &wrong);
        } else {
            uploads.CopyTextureRegion(list, &destination, destination_layout, destination_x, 0, 0,
                &source, source_layout, box);
        }
        check(error_count == errors + 1 &&
            last_error == (scenario == UPLOAD_MAPPED_SOURCE ? DXGI_ERROR_WAS_STILL_DRAWING : E_INVALIDARG),
            "genuine upload rejects unsupported active layout, footprint, extent, source kind, map or barrier sequence");

        if (mapped) {
            resources.UnmapHeap(owner.device, upload_source.heap);
        }
        check(uploads.RetireCommandList(owner.device, list) == S_OK,
            "rejected upload candidates release their exact temporary owners");
    }
}

static void ordinary_upload_read_sharing(void)
{
    FIXTURE_STORAGE upload_memory = storage(), copy_memory = storage(), allocator_memory = storage();
    D3D12DDI_HCOMMANDALLOCATOR allocator = copy_allocator(&allocator_memory);
    D3D12DDI_HCOMMANDLIST first, second;
    LONG errors;

    first = upload_list(&upload_memory, 0);
    upload_transaction(first);
    second = ordinary_copy_list(&copy_memory, allocator);
    errors = error_count;
    ordinary_copy_record(second, upload_source.resource);
    check(error_count == errors, "ordinary COPY may share a prior read-only image-upload source");
    upload_busy();
    check(copy_commands.RetireCommandList(owner.device, second) == S_OK &&
        uploads.RetireCommandList(owner.device, first) == S_OK, "each source reader releases only its own reference");
    copy_memory = storage();
    upload_memory = storage();
    first = ordinary_copy_list(&copy_memory, allocator);
    ordinary_copy_record(first, upload_source.resource);
    second = upload_list(&upload_memory, 0);
    errors = error_count;
    upload_transaction(second);
    check(error_count == errors, "image upload may share a prior ordinary COPY source in reverse acquisition order");
    upload_busy();
    check(uploads.RetireCommandList(owner.device, second) == S_OK &&
        copy_commands.RetireCommandList(owner.device, first) == S_OK &&
        native.RetireCommandAllocator(owner.device, allocator) == S_OK,
        "reverse-order shared source cleanup preserves existing COPY semantics");
}

static void verify_fp16_clear(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
    const uint16_t background[] = {0xbc00, 0x3800, 0x4000, 0x3c00};
    const uint16_t red[] = {0x3c00, 0, 0, 0x3c00};
    void *mapping = NULL;

    transaction(list);
    execute_complete(list);
    check(commands.RetireCommandList(owner.device, list) == S_OK &&
        resources.MapHeap(owner.device, readback.heap, &mapping) == S_OK && mapping,
        "half-float render clear can be read through the native copy scope");
    BYTE *pixels = (BYTE *)mapping + READBACK_OFFSET;
    for (UINT y = 0; y < target_height; ++y) {
        for (UINT x = 0; x < target_width; ++x) {
            const BOOL inside = x >= RECT_LEFT && x < RECT_RIGHT && y >= RECT_TOP && y < RECT_BOTTOM;
            check(!memcmp(pixels + y * readback_row_pitch + x * sizeof(red), inside ? red : background, sizeof(red)),
                "half-float attachment clear preserves signed, fractional and above-one values");
        }
    }
    resources.UnmapHeap(owner.device, readback.heap);
}

static void verify_uploaded_bytes(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
    void *pointer = NULL;

    transaction(list);
    execute_complete(list);
    check(commands.RetireCommandList(owner.device, list) == S_OK &&
        resources.MapHeap(owner.device, readback.heap, &pointer) == S_OK && pointer,
        "existing genuine native readback exposes actually uploaded texels");

    for (UINT index = 0; index < READBACK_HEAP_BYTES; ++index) {
        BYTE expected = READBACK_POISON;

        if (index >= READBACK_OFFSET && index < READBACK_OFFSET + readback_row_pitch * target_height * target_depth) {
            const UINT row = (index - READBACK_OFFSET) / readback_row_pitch;
            const UINT row_byte = (index - READBACK_OFFSET) % readback_row_pitch;

            if (row_byte < target_width * target_texel_bytes) {
                expected = upload_pattern(row, row_byte / target_texel_bytes, row_byte % target_texel_bytes);
            }
        }
        check(((BYTE *)pointer)[index] == expected,
            "every native readback texel and preserved prefix/row-padding/tail byte after actual upload");
    }
    resources.UnmapHeap(owner.device, readback.heap);
    check(resources.MapHeap(owner.device, upload_source.heap, &pointer) == S_OK && pointer,
        "all source owners released before remapping upload backing");

    for (UINT index = 0; index < READBACK_HEAP_BYTES; ++index) {
        BYTE expected = UPLOAD_POISON;

        if (index >= UPLOAD_PREFIX && index < UPLOAD_PREFIX + upload_pitch * target_height * target_depth) {
            const UINT row = (index - UPLOAD_PREFIX) / upload_pitch;
            const UINT row_byte = (index - UPLOAD_PREFIX) % upload_pitch;

            if (row_byte < target_width * target_texel_bytes) {
                expected = upload_pattern(row, row_byte / target_texel_bytes, row_byte % target_texel_bytes);
            }
        }
        check(((BYTE *)pointer)[index] == expected, "upload is read-only including source prefix/padding/tail");
    }
    resources.UnmapHeap(owner.device, upload_source.heap);
}

static void verify_layer_clear(UINT layer, BOOL selected)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
    void *mapping = NULL;

    copy_layer = layer;
    transaction(list);
    execute_complete(list);
    check(commands.RetireCommandList(owner.device, list) == S_OK &&
        resources.MapHeap(owner.device, readback.heap, &mapping) == S_OK && mapping,
        "read an independent image layer through the same owned resource");
    const BYTE *pixels = (const BYTE *)mapping + READBACK_OFFSET;
    for (UINT y = 0; y < target_height; ++y) {
        for (UINT x = 0; x < target_width; ++x) {
            const BOOL inside = x >= RECT_LEFT && x < RECT_RIGHT && y >= RECT_TOP && y < RECT_BOTTOM;
            const BYTE pixel[] = {inside ? 255 : 0, inside ? 0 : 255, 0, 255};
            const BYTE zero[] = {0, 0, 0, 0};

            check(!memcmp(pixels + y * readback_row_pitch + x * sizeof(pixel),
                selected ? pixel : zero, sizeof(pixel)), "clear affects only the selected array view layers");
        }
    }
    resources.UnmapHeap(owner.device, readback.heap);
    copy_layer = target_view_layer;
}

static void verify_volume_clear(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
    void *mapping = NULL;

    transaction(list);
    execute_complete(list);
    check(commands.RetireCommandList(owner.device, list) == S_OK &&
        resources.MapHeap(owner.device, readback.heap, &mapping) == S_OK && mapping, "3D mip readback is observable");
    const BYTE *pixels = (const BYTE *)mapping + READBACK_OFFSET;
    for (UINT z = 0; z < target_depth; ++z) {
        for (UINT y = 0; y < target_height; ++y) {
            for (UINT x = 0; x < target_width; ++x) {
                const BOOL inside = x >= RECT_LEFT && x < RECT_RIGHT && y >= RECT_TOP && y < RECT_BOTTOM;
                const BYTE pixel[] = {inside ? 255 : 0, inside ? 0 : 255, 0, 255};
                const BYTE zero[] = {0, 0, 0, 0};

                check(!memcmp(pixels + (z * target_height + y) * readback_row_pitch + x * sizeof(pixel),
                    z >= target_view_layer ? pixel : zero, sizeof(pixel)), "3D RTV leaves unselected W slices zero");
            }
        }
    }
    resources.UnmapHeap(owner.device, readback.heap);
}

static void invalid_layer_transfers(void)
{
    for (UINT scenario = 0; scenario < 3; ++scenario) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = upload_list(&memory, 0);
        const LONG errors = error_count;

        if (scenario == 0) {
            copy_layer = target_layers;
            upload_barrier(list, FALSE);
        } else {
            upload_barrier(list, FALSE);
            if (scenario == 1) {
                copy_layer = 0;
                upload_copy(list);
            } else {
                upload_copy(list);
                copy_layer = 0;
                upload_barrier(list, TRUE);
            }
        }
        copy_layer = target_view_layer;
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            uploads.RetireCommandList(owner.device, list) == S_OK,
            "out-of-bounds or mismatched layer fails before submission");
    }
}

static void upload_cleanup(void)
{
    check(resources.RetireHeapAndResource(owner.device, upload_source.heap, upload_source.resource) == S_OK,
        "upload source retires only after all source leases release");
    finish_readback();
}

static void final_row_upload_bounds(void)
{
    const UINT row_bytes = QLDX12_NI_IMAGE_WIDTH * READBACK_CHANNELS;
    const UINT active_bytes = (QLDX12_NI_IMAGE_HEIGHT - 1) * UPLOAD_PITCH + row_bytes;

    for (UINT short_by = 0; short_by <= 1; ++short_by) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list;
        LONG errors;
        MODEL_IMAGE_UPLOAD_COUNTS counts;

        upload_logical_bytes = UPLOAD_PREFIX + active_bytes - short_by;
        start_readback();
        create_upload_source();
        list = upload_list(&memory, 0);
        upload_barrier(list, FALSE);
        errors = error_count;
        upload_copy(list);

        if (short_by) {
            check(error_count == errors + 1 && last_error == E_INVALIDARG,
                "one byte missing from final upload texel rejects despite larger backing allocation");
            upload_counters(&counts);
            check(counts.records == 0 && counts.submits == 0,
                "short logical upload extent reaches no Vulkan copy or submission");
        } else {
            check(error_count == errors, "exact final-row source extent admits genuine upload callback");
            upload_barrier(list, TRUE);
            uploads.CloseCommandList(list);
            uploads.ExecuteCommandLists(queue, 1, &list);
            {
                QLDX12_NC_COMPLETION completion;

                check(error_count == errors && uploads.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
                    completion.state == QLDX12_NC_COMPLETE, "exact final-row source actually completes upload");
            }
        }
        check(uploads.RetireCommandList(owner.device, list) == S_OK,
            "final-row upload recording releases exact source and image references");

        if (!short_by) {
            verify_uploaded_bytes();
        }
        upload_cleanup();
    }
    upload_logical_bytes = READBACK_LOGICAL_BYTES;
}

static void genuine_resolve_barrier(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HRESOURCE resource,
    BOOL source, BOOL closing)
{
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = {0};
    const D3D12DDI_RESOURCE_STATES state = source ? D3D12DDI_RESOURCE_STATE_RESOLVE_SOURCE : D3D12DDI_RESOURCE_STATE_RESOLVE_DEST;
    barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.hResource = resource;
    barrier.Transition.Subresource = closing ? UINT32_MAX : 0;
    barrier.Transition.StateBefore = closing ? state : D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = closing ? D3D12DDI_RESOURCE_STATE_RENDER_TARGET : state;
    unified_list.pfnResourceBarrier(list, 1, &barrier);
}

static void genuine_unified_resolve(void)
{
    target_volume = target_1d = FALSE;
    target_layers = target_depth = 1; target_view_layer = copy_layer = 0; target_view_count = 1;
    target_width = target_height = 64; target_samples = 4;
    target_format = DXGI_FORMAT_R8G8B8A8_UNORM; target_texel_bytes = 4;
    readback_row_pitch = READBACK_ROW_PITCH; readback_logical_bytes = READBACK_LOGICAL_BYTES;
    unified_r0 = TRUE;
    start_readback();
    check(unified_list.pfnResourceResolveSubresource != NULL,
        "genuine unified R0 list publishes typed resolve-subresource with explicit states");
    for (UINT invalid = 0; invalid < 15; ++invalid) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
        MODEL_IMAGE_COUNTS before, after;
        LONG errors = error_count;
        image_counters(&before);
        if (invalid == 10) {
            genuine_resolve_barrier(list, other_image.resource, TRUE, FALSE);
        } else if (invalid == 11) {
            D3D12DDIARG_RESOURCE_BARRIER_0003 bad = transition(FALSE);
            bad.Transition.StateAfter = D3D12DDI_RESOURCE_STATE_RESOLVE_SOURCE | D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
            unified_list.pfnResourceBarrier(list, 1, &bad);
        } else {
            if (invalid != 0) genuine_resolve_barrier(list, owner.resource, TRUE, FALSE);
            if (invalid > 1 && invalid != 8 && invalid != 9)
                genuine_resolve_barrier(list, other_image.resource, FALSE, FALSE);
            if (invalid == 7) genuine_resolve_barrier(list, owner.resource, TRUE, TRUE);
            else if (invalid == 8) genuine_resolve_barrier(list, owner.resource, TRUE, FALSE);
            else if (invalid == 9) genuine_resolve_barrier(list, owner.resource, FALSE, FALSE);
            else {
                unified_list.pfnResourceResolveSubresource(list,
                    invalid == 4 ? owner.resource : other_image.resource, 0,
                    invalid == 4 ? other_image.resource : owner.resource, invalid == 5 ? 1 : 0,
                    invalid == 2 ? DXGI_FORMAT_B8G8R8A8_UNORM : invalid == 3 ? DXGI_FORMAT_UNKNOWN : target_format);
                if (invalid == 6) {
                    genuine_resolve_barrier(list, other_image.resource, FALSE, TRUE);
                    // Duplicate close must refuse without consuming the first acknowledged state.
                    genuine_resolve_barrier(list, other_image.resource, FALSE, TRUE);
                } else if (invalid == 12) {
                    unified_list.pfnCloseCommandList(list);
                } else if (invalid == 13) {
                    genuine_resolve_barrier(list, owner.resource, TRUE, TRUE);
                    unified_list.pfnCloseCommandList(list);
                } else if (invalid == 14) {
                    unified_list.pfnResourceResolveSubresource(list, other_image.resource, 0,
                        owner.resource, 0, target_format);
                }
            }
        }
        image_counters(&after);
        check(error_count == errors + 1 && last_error == D3DDDIERR_APPLICATIONERROR &&
            before.submits == after.submits && before.executes == after.executes &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "invalid resolve states, format, ordering, layers, samples and incomplete/duplicate transactions preserve GPU owners without submitting");
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
        D3D12DDIARG_RESETCOMMANDLIST reset = {0};
        QLDX12_NC_COMPLETION completion;
        LONG errors = error_count;
        genuine_resolve_barrier(list, owner.resource, TRUE, FALSE);
        genuine_resolve_barrier(list, other_image.resource, FALSE, FALSE);
        check(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING &&
            images.RetireHeapAndResource(owner.device, other_image.heap, other_image.resource) == DXGI_ERROR_WAS_STILL_DRAWING,
            "explicit resolve opening states own both exact initialized allocations before the resolve callback");
        unified_list.pfnResourceResolveSubresource(list, other_image.resource, 0, owner.resource, 0, target_format);
        genuine_resolve_barrier(list, other_image.resource, FALSE, TRUE);
        genuine_resolve_barrier(list, owner.resource, TRUE, TRUE);
        unified_list.pfnCloseCommandList(list);
        check(error_count == errors, "both explicit resolve state pairs close after a typed destination-first resolve");
        set_mode(MODEL_RESOLVE_MISSING_PROC);
        unified_queue.pfnExecuteCommandLists(queue, 1, &list);
        check(error_count == errors + 1 && last_error == E_NOINTERFACE,
            "missing real Vulkan resolve procedure preserves the closed unified transaction for retry");
        set_mode(MODEL_RENDER_TARGET_DELAY);
        unified_queue.pfnExecuteCommandLists(queue, 1, &list);
        check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_PENDING,
            "real unified resolve retry submits its own still-pending target");
        reset.hDrvCommandAllocator = allocators[1]; reset.ID = UINT64_C(0xAE5017E0);
        unified_list.pfnResetCommandList(list, &reset);
        check(commands.RetireCommandList(owner.device, list) == S_OK &&
            images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING &&
            images.RetireHeapAndResource(owner.device, other_image.heap, other_image.resource) == DXGI_ERROR_WAS_STILL_DRAWING,
            "reset/retired recording cannot release detached pending resolve image and barrier owners");
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_COMPLETE, "actual resolve timeline completion permits detached owner cleanup");
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HRESOURCE source = owner.resource;
        owner.resource = other_image.resource;
        D3D12DDI_HCOMMANDLIST read = new_list(&memory, 0);
        transaction(read); execute_complete(read);
        check(commands.RetireCommandList(owner.device, read) == S_OK,
            "resolved single-sample image reaches an actual unified readback");
        verify_readback();
        owner.resource = source;
    }
    finish_readback(); target_samples = 1;
    message("PASS: genuine unified R0 typed multisample resolve, explicit source/destination state pairs, exact pixels, no-attempt retry and detached reset lifetimes\n");
}

static void whole_image_barrier(D3D12DDI_HCOMMANDLIST list,D3D12DDI_HRESOURCE resource,BOOL source,BOOL closing)
{
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier={0};barrier.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.hResource=resource;barrier.Transition.Subresource=UINT_MAX;
    UINT copy=source?D3D12DDI_RESOURCE_STATE_COPY_SOURCE:D3D12DDI_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateBefore=closing?copy:D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter=closing?D3D12DDI_RESOURCE_STATE_RENDER_TARGET:copy;
    unified_list.pfnResourceBarrier(list,1,&barrier);
}
static void whole_image_record(D3D12DDI_HCOMMANDLIST list,BOOL destination_first)
{
    if(destination_first)whole_image_barrier(list,other_image.resource,FALSE,FALSE);
    whole_image_barrier(list,owner.resource,TRUE,FALSE);
    if(!destination_first)whole_image_barrier(list,other_image.resource,FALSE,FALSE);
    unified_list.pfnResourceCopy(list,other_image.resource,owner.resource);
    whole_image_barrier(list,owner.resource,TRUE,TRUE);whole_image_barrier(list,other_image.resource,FALSE,TRUE);
    unified_list.pfnCloseCommandList(list);
}
static void genuine_whole_image_copy(HMODULE icd)
{
    MODEL_TRANSFER_COUNTERS counters=(MODEL_TRANSFER_COUNTERS)(uintptr_t)GetProcAddress(icd,"ModelImageCopyCounters");
    check(counters!=NULL,"actual modeled image-to-image copy observations");
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_UNORM};
    for(UINT trial=0;trial<11;++trial) {
        target_1d=trial==1;target_volume=trial==3;target_layers=(trial==1 || trial==2)?3:1;target_depth=target_volume?3:1;
        target_view_layer=0;target_view_count=target_layers>1?2:1;copy_layer=0;transition_all=FALSE;target_samples=trial==10?4:1;
        target_width=32;target_height=target_1d?1:24;target_format=trial<ARRAYSIZE(formats)?formats[trial]:DXGI_FORMAT_R8G8B8A8_UNORM;
        target_texel_bytes=target_format==DXGI_FORMAT_R16G16B16A16_FLOAT || target_format==DXGI_FORMAT_R16G16B16A16_UNORM?8:4;
        readback_row_pitch=READBACK_ROW_PITCH;readback_logical_bytes=READBACK_LOGICAL_BYTES;upload_pitch=UPLOAD_PITCH;upload_logical_bytes=READBACK_LOGICAL_BYTES;
        unified_r0=TRUE;start_readback();
        if(target_samples>1) {
            check(images.RetireHeapAndResource(owner.device,other_image.heap,other_image.resource)==S_OK,"replace single-sample readback helper target with matching multisample copy target");
            other_image.heap_storage=storage();other_image.resource_storage=storage();D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
            check(images.CreateHeapAndResource(owner.device,&owner.heap_args,other_image.heap,runtime,&owner.resource_args,NULL,other_image.resource)==S_OK,"whole-image matching multisample target owns real selected backing");
        } else {
            create_upload_source();
            for(copy_layer=0;copy_layer<target_layers;++copy_layer) {
                FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=upload_list(&memory,0);upload_transaction(list);execute_complete(list);
                check(commands.RetireCommandList(owner.device,list)==S_OK,"distinct exact source bytes uploaded into every array layer or volume slice");
            }
            copy_layer=0;
        }
        MODEL_TRANSFER_COUNTS before,after;counters(&before);
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=new_list(&memory,0);LONG errors=error_count;
        whole_image_record(list,trial&1);
        check(error_count==errors,"genuine whole-image copy consumes paired source/destination states and closes both transactions");
        if(trial==0) {
            set_mode(MODEL_RESOLVE_MISSING_PROC);unified_queue.pfnExecuteCommandLists(queue,1,&list);counters(&after);
            check(error_count==errors+1 && last_error==E_NOINTERFACE && before.records==after.records && before.submits==after.submits,
                "missing actual vkCmdCopyImage is a whole-transaction no-attempt refusal");errors=error_count;
        }
        set_mode(trial==8?MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE:trial==9?MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE:MODEL_RENDER_TARGET_DELAY);
        unified_queue.pfnExecuteCommandLists(queue,1,&list);counters(&after);
        check(after.records==before.records+1 && after.submits==before.submits+1 && before.executes==after.executes,
            "all whole-image layers/slices record in one actual command and receipt without fabricated completion");
        check(images.RetireHeapAndResource(owner.device,owner.heap,owner.resource)==DXGI_ERROR_WAS_STILL_DRAWING &&
            images.RetireHeapAndResource(owner.device,other_image.heap,other_image.resource)==DXGI_ERROR_WAS_STILL_DRAWING,
            "whole-image source and destination retain exact original image/allocation owners through the actual attempt");
        QLDX12_NC_COMPLETION completion;
        if(trial==8 || trial==9) {
            check(error_count==errors+1 && FAILED(last_error),"whole-image uncertain or accepted failed execution preserves actual failure");
            set_mode(MODEL_DEVICE_DRAIN_FAILURE);
            check(FAILED(commands.Drain(queue)) && commands.RetireCommandList(owner.device,list)==DXGI_ERROR_WAS_STILL_DRAWING &&
                images.RetireHeapAndResource(owner.device,owner.heap,owner.resource)==DXGI_ERROR_WAS_STILL_DRAWING &&
                images.RetireHeapAndResource(owner.device,other_image.heap,other_image.resource)==DXGI_ERROR_WAS_STILL_DRAWING,
                "failed whole-image drain preserves both image and barrier generations for retry");
            set_mode(MODEL_RENDER_TARGET_GOOD);
            check(FAILED(commands.Drain(queue)) && commands.RetireCommandList(owner.device,list)==S_OK,"safe whole-image quiescence permits cleanup while retaining the failed result");
        } else {
            check(error_count==errors && commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,
                "whole-image copy remains pending until its own timeline receipt is observed");
            D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocators[1],.ID=0x1A6EC0F1};unified_list.pfnResetCommandList(list,&reset);
            check(commands.RetireCommandList(owner.device,list)==S_OK && !memory.cookie &&
                images.RetireHeapAndResource(owner.device,other_image.heap,other_image.resource)==DXGI_ERROR_WAS_STILL_DRAWING,
                "reset detaches exact pending whole-image states before replacement caller storage retires");
            set_mode(MODEL_RENDER_TARGET_GOOD);
            check(commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && !memory.cookie && memory.guard==STORAGE_GUARD,
                "whole-image completion retires both detached states without changing reused caller storage");
            counters(&after);check(after.executes==before.executes+1 && after.bytes==before.bytes+(UINT64)target_width*target_height*target_depth*target_layers*target_texel_bytes,
                "actual modeled copy executes the complete admitted color image byte extent, all array layers and volume slices");
            if(target_samples==1) {
                D3D12DDI_HRESOURCE saved=owner.resource;owner.resource=other_image.resource;
                for(copy_layer=0;copy_layer<target_layers;++copy_layer)verify_uploaded_bytes();
                owner.resource=saved;copy_layer=0;
            }
            if(trial==0) {
                counters(&before);
                for(UINT invalid=0;invalid<7;++invalid) {
                    memory=storage();list=new_list(&memory,0);errors=error_count;
                    if(invalid)whole_image_barrier(list,owner.resource,TRUE,FALSE);
                    if(invalid>1)whole_image_barrier(list,other_image.resource,FALSE,FALSE);
                    if(invalid==2)whole_image_barrier(list,owner.resource,TRUE,TRUE);
                    else if(invalid==3)unified_list.pfnResourceCopy(list,other_image.resource,(D3D12DDI_HRESOURCE){0});
                    else if(invalid==4)unified_list.pfnResourceCopy(list,owner.resource,other_image.resource);
                    else {
                        unified_list.pfnResourceCopy(list,other_image.resource,owner.resource);
                        if(invalid==5)unified_list.pfnResourceCopy(list,other_image.resource,owner.resource);
                        else if(invalid==6) {whole_image_barrier(list,owner.resource,TRUE,TRUE);unified_list.pfnCloseCommandList(list);}
                    }
                    check(error_count>errors && commands.RetireCommandList(owner.device,list)==S_OK,
                        "whole-image copy rejects missing/early/duplicate/wrong-role states and incomplete closing transactions without leaking owners");
                }
                for(UINT mismatch=0;mismatch<4;++mismatch) {
                    SECOND_IMAGE other={0};other.heap_storage=storage();other.resource_storage=storage();
                    other.heap.pDrvPrivate=&other.heap_storage;other.resource.pDrvPrivate=&other.resource_storage;
                    D3D12DDIARG_CREATERESOURCE_0003 shape=owner.resource_args;D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
                    if(mismatch==0)--shape.Width;else if(mismatch==1)shape.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
                    else if(mismatch==2)shape.SampleDesc.Count=4;else {shape.ResourceType=D3D12DDI_RT_TEXTURE1D;shape.Height=1;}
                    check(images.CreateHeapAndResource(owner.device,&owner.heap_args,other.heap,runtime,&shape,NULL,other.resource)==S_OK,"independent incompatible whole-image copy target");
                    memory=storage();list=new_list(&memory,0);errors=error_count;
                    whole_image_barrier(list,owner.resource,TRUE,FALSE);whole_image_barrier(list,other.resource,FALSE,FALSE);
                    unified_list.pfnResourceCopy(list,other.resource,owner.resource);
                    check(error_count==errors+1 && commands.RetireCommandList(owner.device,list)==S_OK && images.RetireHeapAndResource(owner.device,other.heap,other.resource)==S_OK,
                        "whole-image copy refuses unequal dimensions, format, samples or resource type with exact candidate cleanup");
                }
                counters(&after);check(before.records==after.records && before.submits==after.submits,"invalid whole-image recording consumes no physical copy or receipt");
            }
        }
        if(target_samples>1)finish_readback();else upload_cleanup();
    }
    message("PASS: genuine whole-image ResourceCopy, typed paired states, arrays/1D/3D and color formats, exact uploaded readback bytes, matching multisample Vulkan ABI, no-attempt retry and pending/uncertain cleanup\n");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE get_native;
    GET_IMAGES get_images;
    GET_DESCRIPTORS get_descriptors;
    GET_COMMANDS get_commands;
    GET_RESOURCES get_resources;
    GET_RENDER_COMMANDS get_render_commands;
    GET_COPY_COMMANDS get_copy_commands;

    check(driver && icd, "actual production driver and isolated modeled ICD");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    open_r0_adapter = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    get_native = (GET_NATIVE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_images = (GET_IMAGES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeImageFunctions");
    get_descriptors = (GET_DESCRIPTORS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeDescriptorFunctions");
    get_commands = (GET_COMMANDS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeImageReadbackCommandFunctions");
    get_copy_commands = (GET_COPY_COMMANDS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeCommandFunctions");
    get_resources = (GET_RESOURCES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeResourceFunctions");
    get_render_commands = (GET_RENDER_COMMANDS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeRenderTargetCommandFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    image_counters = (MODEL_IMAGE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelImageCounters");
    view_counters = (MODEL_VIEW_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelViewCounters");
    readback_counters = (MODEL_IMAGE_READBACK_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelImageReadbackCounters");
    check(open_adapter && get_native && get_images && get_descriptors && get_commands && get_resources && get_render_commands && get_copy_commands &&
        reset_model && set_mode && image_counters && view_counters && readback_counters,
        "exact genuine and modeled image-readback exports");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_images(QLDX12_NI_BUNDLE_VERSION, sizeof(images), &images) == S_OK &&
        get_descriptors(QLDX12_ND_BUNDLE_VERSION, sizeof(descriptors), &descriptors) == S_OK &&
        get_commands(QLDX12_NC_IMAGE_READBACK_BUNDLE_VERSION, sizeof(commands), &commands) == S_OK &&
        get_copy_commands(QLDX12_NC_BUNDLE_VERSION, sizeof(copy_commands), &copy_commands) == S_OK &&
        get_resources(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(resources), &resources) == S_OK &&
        get_render_commands(QLDX12_NC_RENDER_TARGET_BUNDLE_VERSION, sizeof(render_commands), &render_commands) == S_OK,
        "additive original R0 copy bundle preserves existing transition/resource/render-target bundles");

    {
        GET_UPLOAD get_upload = (GET_UPLOAD)(uintptr_t)GetProcAddress(driver,
            "QemuLightningInternalGetNativeImageUploadCommandFunctions");
        QLDX12_NATIVE_IMAGE_UPLOAD_COMMAND_FUNCTIONS rejected;

        upload_counters = (MODEL_IMAGE_UPLOAD_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelImageUploadCounters");
        check(get_upload && upload_counters &&
            get_upload(QLDX12_NC_IMAGE_UPLOAD_BUNDLE_VERSION, sizeof(uploads), &uploads) == S_OK,
            "separate genuine original R0 upload callbacks and isolated model counters");
        FillMemory(&rejected, sizeof(rejected), READBACK_POISON);
        check(get_upload(0, sizeof(rejected), &rejected) == E_INVALIDARG &&
            get_upload(QLDX12_NC_IMAGE_UPLOAD_BUNDLE_VERSION, sizeof(rejected) - 1, &rejected) == E_INVALIDARG,
            "wrong upload bundle version/size rejects");
    }
    final_row_upload_bounds();
    start_readback();
    create_upload_source();
    invalid_uploads();
    ordinary_upload_read_sharing();
    {
        FIXTURE_STORAGE memory = storage(), shared_memory = storage();
        D3D12DDI_HCOMMANDLIST list = upload_list(&memory, 0);
        D3D12DDI_HCOMMANDLIST shared = upload_list(&shared_memory, 1);
        D3D12DDIARG_RESETCOMMANDLIST reset = {0};
        QLDX12_NC_COMPLETION completion;
        MODEL_IMAGE_UPLOAD_COUNTS counts;
        LONG errors = error_count;

        upload_transaction(list);
        upload_transaction(shared);
        check(error_count == errors, "two real upload recordings share one read-only source safely");
        upload_busy();
        check(uploads.RetireCommandList(owner.device, shared) == S_OK,
            "one source reader retirement leaves the other exact reference held");
        upload_busy();
        set_mode(MODEL_RENDER_TARGET_DELAY);
        uploads.CloseCommandList(list);
        uploads.ExecuteCommandLists(queue, 1, &list);
        check(error_count == errors && uploads.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_PENDING, "upload actual attempt holds pending resources");
        upload_busy();
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(uploads.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_COMPLETE, "own uploaded image target completion is observed");
        upload_busy();
        reset.hDrvCommandAllocator = allocators[0];
        uploads.ResetCommandList(list, &reset);
        check(error_count == errors && uploads.RetireCommandList(owner.device, list) == S_OK,
            "reset of completed upload releases exact source and image references");
        upload_counters(&counts);
        check(counts.records == 1 && counts.executes == 1 && counts.bytes == QLDX12_NI_IMAGE_WIDTH * QLDX12_NI_IMAGE_HEIGHT * READBACK_CHANNELS,
            "actual retained Vulkan buffer-to-image ABI and execution counters");
        verify_uploaded_bytes();
    }
    upload_cleanup();
    start_readback();
    create_upload_source();
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = upload_list(&memory, 0);
        QLDX12_NC_COMPLETION completion;
        LONG errors;

        upload_transaction(list);
        uploads.CloseCommandList(list);
        set_mode(MODEL_IMAGE_UPLOAD_MISSING_COPY);
        errors = error_count;
        uploads.ExecuteCommandLists(queue, 1, &list);
        check(error_count == errors + 1 && last_error == E_NOINTERFACE,
            "missing vkCmdCopyBufferToImage fails before attempt without discarding closed owners");
        upload_busy();
        set_mode(MODEL_RENDER_TARGET_GOOD);
        errors = error_count;
        uploads.ExecuteCommandLists(queue, 1, &list);
        check(error_count == errors && uploads.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_COMPLETE && uploads.RetireCommandList(owner.device, list) == S_OK,
            "same closed upload list retries after missing procedure recovers");
        verify_uploaded_bytes();
    }
    upload_cleanup();

    start_readback();
    create_upload_source();
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = upload_list(&memory, 0);
        D3D12DDIARG_RESETCOMMANDLIST reset = {0};
        QLDX12_NC_RECORDING_INFO before, after;
        QLDX12_NC_COMPLETION completion;
        LONG errors;

        upload_transaction(list);
        uploads.CloseCommandList(list);
        set_mode(MODEL_RENDER_TARGET_DELAY);
        errors = error_count;
        uploads.ExecuteCommandLists(queue, 1, &list);
        check(error_count == errors && uploads.QueryRecording(list, sizeof(before), &before) == S_OK,
            "pending upload has a retained exact recording generation");
        reset.hDrvCommandAllocator = allocators[1];
        reset.ID = UINT64_C(0xA7100001);
        uploads.ResetCommandList(list, &reset);
        check(error_count == errors && uploads.QueryRecording(list, sizeof(after), &after) == S_OK &&
            after.generation != before.generation && after.id == reset.ID && after.recording,
            "pending upload reset detaches old image/source owners from the replacement generation");
        upload_busy();
        check(uploads.RetireCommandList(owner.device, list) == S_OK,
            "replacement empty upload recording may retire while old attempt is pending");
        upload_busy();
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(uploads.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_COMPLETE, "old upload target completion releases only detached old owners");
        verify_uploaded_bytes();
    }
    upload_cleanup();

    {
        const UINT failure_modes[] = {MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE, MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE};

        for (UINT failure = 0; failure < ARRAYSIZE(failure_modes); ++failure) {
            FIXTURE_STORAGE memory = storage();
            D3D12DDI_HCOMMANDLIST list;
            LONG errors;

            start_readback();
            create_upload_source();
            list = upload_list(&memory, 0);
            upload_transaction(list);
            uploads.CloseCommandList(list);
            set_mode(failure_modes[failure]);
            errors = error_count;
            uploads.ExecuteCommandLists(queue, 1, &list);
            check(error_count == errors + 1 && FAILED(last_error), "unknown submit or failed accepted-target observation reports terminal upload failure");
            upload_busy();
            set_mode(MODEL_DEVICE_DRAIN_FAILURE);
            check(FAILED(uploads.Drain(queue)) &&
                uploads.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING,
                "failed upload drain preserves every actual source/image/allocator owner");
            upload_busy();
            set_mode(MODEL_RENDER_TARGET_GOOD);
            check(FAILED(uploads.Drain(queue)) && uploads.RetireCommandList(owner.device, list) == S_OK,
                "safe upload cleanup retains original terminal work failure without fabricating success");
            upload_cleanup();
        }
    }
    const DXGI_FORMAT additional_formats[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM
    };
    for (UINT format_index = 0; format_index < ARRAYSIZE(additional_formats); ++format_index) {
        FIXTURE_STORAGE memory = storage();
        QLDX12_NC_COMPLETION completion;
        D3D12DDI_HCOMMANDLIST list;
        LONG errors = error_count;

        target_1d = format_index + 1 == ARRAYSIZE(additional_formats);
        target_width = 72;
        target_height = target_1d ? 1 : 48;
        target_format = additional_formats[format_index];
        const BOOL wide_texel = target_format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
            target_format == DXGI_FORMAT_R16G16B16A16_UNORM;
        target_texel_bytes = wide_texel ? 8 : 4;
        upload_pitch = wide_texel ? 768 : UPLOAD_PITCH;
        readback_row_pitch = wide_texel ? 768 : READBACK_ROW_PITCH;
        start_readback();
        create_upload_source();
        if (target_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            verify_fp16_clear();
        }
        if (wide_texel) {
            list = upload_list(&memory, 0);
            upload_barrier(list, FALSE);
            upload_pitch = UPLOAD_PITCH; /* 512 bytes is aligned but smaller than 72 * 8. */
            upload_copy(list);
            check(error_count == errors + 1 && last_error == E_INVALIDARG &&
                uploads.RetireCommandList(owner.device, list) == S_OK,
                "undersized aligned eight-byte texel footprint refuses recording");
            upload_pitch = 768;
            memory = storage();
            errors = error_count;
        }
        list = upload_list(&memory, 0);
        upload_transaction(list);
        uploads.CloseCommandList(list);
        uploads.ExecuteCommandLists(queue, 1, &list);
        check(error_count == errors &&
            uploads.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_COMPLETE &&
            uploads.RetireCommandList(owner.device, list) == S_OK,
            "non-square native Vulkan upload completes with aligned row padding");
        verify_uploaded_bytes();
        upload_cleanup();
    }
    message("PASS: 1D and non-square 2D uploads and readback match every texel and padding byte\n");
    target_layers = 4;
    target_view_layer = 1;
    target_view_count = 2;
    copy_layer = target_view_layer;
    target_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (UINT mode = 0; mode < 2; ++mode) {
        FIXTURE_STORAGE memory = storage();
        QLDX12_NC_COMPLETION completion;
        target_1d = mode != 0;
        target_height = target_1d ? 1 : 16;
        start_readback();
        create_upload_source();
        verify_layer_clear(0, FALSE);
        verify_layer_clear(2, TRUE);
        verify_layer_clear(3, FALSE);
        invalid_layer_transfers();

        /* Also exercise an explicit ALL -> copy one layer -> ALL sequence. */
        transition_all = mode != 0;
        const LONG errors = error_count;
        D3D12DDI_HCOMMANDLIST list = upload_list(&memory, 0);
        upload_transaction(list);
        uploads.CloseCommandList(list);
        uploads.ExecuteCommandLists(queue, 1, &list);
        check(error_count == errors &&
            uploads.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_COMPLETE &&
            uploads.RetireCommandList(owner.device, list) == S_OK,
            "native 1D or 2D array layer upload completes");
        verify_uploaded_bytes();
        verify_layer_clear(0, FALSE);
        verify_layer_clear(2, TRUE);
        verify_layer_clear(3, FALSE);
        transition_all = FALSE;
        upload_cleanup();
    }
    message("PASS: 1D/2D array clear ranges and individual layer transfers preserve neighboring layers\n");
    {
        FIXTURE_STORAGE memory = storage();
        QLDX12_NC_COMPLETION completion;
        target_1d = FALSE;
        target_volume = TRUE;
        target_layers = 1;
        target_depth = 4;
        target_height = 16;
        target_view_layer = 2;
        target_view_count = UINT32_MAX;
        copy_layer = 0;
        readback_logical_bytes = READBACK_OFFSET + readback_row_pitch * (target_depth * target_height - 1) +
            target_width * target_texel_bytes;
        upload_logical_bytes = UPLOAD_PREFIX + upload_pitch * (target_depth * target_height - 1) +
            target_width * target_texel_bytes;
        start_readback();
        verify_volume_clear();
        create_upload_source();
        D3D12DDI_HCOMMANDLIST list = upload_list(&memory, 0);
        LONG errors = error_count;
        upload_barrier(list, FALSE);
        target_depth = 3;
        upload_copy(list);
        target_depth = 4;
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            uploads.RetireCommandList(owner.device, list) == S_OK,
            "short physical volume extent is rejected before submission");

        memory = storage();
        list = upload_list(&memory, 0);
        errors = error_count;
        upload_transaction(list);
        uploads.CloseCommandList(list);
        uploads.ExecuteCommandLists(queue, 1, &list);
        check(error_count == errors &&
            uploads.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_COMPLETE &&
            uploads.RetireCommandList(owner.device, list) == S_OK, "full mip upload fits exact active multi-slice span");
        verify_uploaded_bytes();
        upload_cleanup();
    }
    message("PASS: volume clears isolate W slices and full 3D transfers preserve pitched padding\n");
    {
        target_volume = target_1d = FALSE;
        target_layers = target_depth = 1;
        target_view_layer = copy_layer = 0;
        target_view_count = 1;
        target_width = target_height = 64;
        target_samples = 4;
        readback_row_pitch = READBACK_ROW_PITCH;
        readback_logical_bytes = READBACK_LOGICAL_BYTES;
        start_readback();
        HMODULE driver = GetModuleHandleW(L"QemuLightningD3D12Bootstrap.dll");
        QLDX12_RECORD_NATIVE_COLOR_RESOLVE resolve = (QLDX12_RECORD_NATIVE_COLOR_RESOLVE)(uintptr_t)
            GetProcAddress(driver, "QemuLightningInternalRecordNativeColorResolve");
        check(resolve != NULL, "private complete color resolve entry exists");
        for (UINT invalid = 0; invalid < 3; ++invalid) {
            FIXTURE_STORAGE memory = storage();
            D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};
            args.hDrvCommandList.pDrvPrivate = &memory;
            args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
            args.hDrvCommandAllocator = allocators[0];
            args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
            args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
            check(render_commands.CreateCommandList(owner.device, &args) == S_OK, "resolve records on retained DIRECT pool");
            check(resolve(args.hDrvCommandList, invalid == 0 ? other_image.resource : owner.resource,
                invalid == 1 ? 1 : 0, invalid == 2 ? owner.resource : other_image.resource, 0) == E_INVALIDARG &&
                render_commands.RetireCommandList(owner.device, args.hDrvCommandList) == S_OK,
                "sample-one source, invalid layer and aliasing destination reject without leaking owners");
        }
        FIXTURE_STORAGE memory = storage();
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};
        QLDX12_NC_COMPLETION completion;
        args.hDrvCommandList.pDrvPrivate = &memory;
        args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
        args.hDrvCommandAllocator = allocators[0];
        args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(render_commands.CreateCommandList(owner.device, &args) == S_OK &&
            resolve(args.hDrvCommandList, owner.resource, 0, other_image.resource, 0) == S_OK,
            "resolve retains exact initialized multisample and single-sample images");
        set_mode(MODEL_RESOLVE_MISSING_PROC);
        LONG errors = error_count;
        render_commands.CloseCommandList(args.hDrvCommandList);
        render_commands.ExecuteCommandLists(queue, 1, &args.hDrvCommandList);
        check(error_count == errors + 1 && last_error == E_NOINTERFACE,
            "missing resolve procedure refuses a GPU attempt and preserves a retryable recording");
        set_mode(MODEL_RENDER_TARGET_DELAY);
        render_commands.ExecuteCommandLists(queue, 1, &args.hDrvCommandList);
        check(render_commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_PENDING &&
            images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING &&
            images.RetireHeapAndResource(owner.device, other_image.heap, other_image.resource) == DXGI_ERROR_WAS_STILL_DRAWING,
            "pending resolve owns both complete resource generations");
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(render_commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_COMPLETE &&
            render_commands.RetireCommandList(owner.device, args.hDrvCommandList) == S_OK,
            "resolve completes its own target before releasing operands");
        D3D12DDI_HRESOURCE source = owner.resource;
        owner.resource = other_image.resource;
        memory = storage();
        D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
        transaction(list);
        execute_complete(list);
        check(commands.RetireCommandList(owner.device, list) == S_OK, "resolved pixels can be read back");
        verify_readback();
        owner.resource = source;
        finish_readback();
        target_samples = 1;
    }
    message("PASS: native multisample clear resolves through retained images to pixel-checked readback\n");

    message("PASS: genuine R0 uploads reject invalid footprints/kinds/maps/sequences, snapshot active ABI arms, preserve shared read-only source and exact image ownership, execute native upload/readback bytes, retain detached reset/uncertain/failed-counter work through safe drain and retry missing procedures; modeled ABI only; tests0\n");
    unified_r0 = TRUE;
    start_readback();
    create_upload_source();
    void *persistent_upload=NULL;check(unified_core.pfnMapHeap(owner.device,upload_source.heap,&persistent_upload)==S_OK && persistent_upload,"genuine UPLOAD pointer stays open across actual buffer-to-image execution and readback reset");
    {
        FIXTURE_STORAGE failed_memory = storage();
        D3D12DDI_HCOMMANDLIST failed = upload_list(&failed_memory, 0);
        MODEL_IMAGE_UPLOAD_COUNTS before, after;
        LONG errors = error_count;
        upload_counters(&before);
        upload_barrier(failed, FALSE);
        upload_barrier(failed, TRUE);
        upload_counters(&after);
        check(error_count == errors + 1 && last_error == D3DDDIERR_APPLICATIONERROR &&
            before.records == after.records && before.executes == after.executes &&
            uploads.RetireCommandList(owner.device, failed) == S_OK,
            "missing unified upload copy reports recording error and retires without a GPU attempt");
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = upload_list(&memory, 0);
        D3D12DDIARG_RESETCOMMANDLIST reset = {0};
        QLDX12_NC_COMPLETION completion;
        LONG errors = error_count;
        upload_transaction(list);
        upload_busy();
        uploads.CloseCommandList(list);
        uploads.ExecuteCommandLists(queue, 1, &list);
        check(error_count == errors && uploads.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_COMPLETE, "unified buffer-to-image upload completes its own genuine target");
        reset.hDrvCommandAllocator = allocators[1]; reset.ID = UINT64_C(0x1A6E5500);
        unified_list.pfnResetCommandList(list, &reset);
        transaction(list);
        execute_complete(list);
        check(error_count == errors && commands.RetireCommandList(owner.device, list) == S_OK,
            "same genuine unified list resets from upload into an independently owned readback transaction");
        check(((BYTE*)persistent_upload)[UPLOAD_PREFIX]==upload_pattern(0,0,0),"mapped upload source remains valid and unmodified after actual image copy");
        unified_core.pfnUnmapHeap(owner.device,upload_source.heap);
        verify_uploaded_bytes();
    }
    upload_cleanup();
    message("PASS: genuine unified R0 buffer/image transfer selection, upload-to-readback reset and exact byte/padding preservation\n");
    genuine_unified_resolve();
    genuine_whole_image_copy(icd);
    ExitProcess(0);
}

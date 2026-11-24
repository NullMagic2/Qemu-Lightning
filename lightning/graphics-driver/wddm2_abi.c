/* Module: lightning.graphics_driver.wddm2_abi
 * Purpose: Check the WDDM 2.0 software MMU and its DDIs with real WDK types.
 * Created: 2026-10-17
 * Architecture: Links the genuine gpuva.c, miniport_wddm2.c, miniport_contracts.c,
 * paging.c, objects.c and objects_memory.c of the QLG_WDDM=2_0 build. A 1 GiB reservation
 * stands in for the BAR, fixture pages for system memory (their addresses are
 * the "physical" PFNs) and a bounce-buffer MDL mapping for the kernel's. It
 * never loads a driver: dxgkrnl's acceptance of the MMU is the W0 VM check.
 */
#include "miniport.h"
#include "internal.h"
#include "gpuva.h"
#include "paging_submit.h"
#include "platform_memory.h"
#include <stdlib.h>
#include <string.h>
__declspec(dllimport) __declspec(noreturn) void WINAPI ExitProcess(UINT);
__declspec(dllimport) void *WINAPI VirtualAlloc(void *address, SIZE_T bytes, ULONG type,
                                                ULONG protect);
__declspec(dllimport) int __cdecl printf(const char *format, ...);
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            printf("WDDM ABI failed at line %u\n", (UINT)__LINE__); \
            ExitProcess(1); \
        } \
    } while (0)

enum {
    FIXTURE_POISON = 0xa5,
    FIXTURE_FILL = 0x5a17c3e9,
    FIXTURE_SYSTEM_PAGES = 4,
    FIXTURE_TICKETS = 4,
    FIXTURE_PARTIAL_FENCE = 78,
    FIXTURE_IMPLICIT_FENCE = 79,
};
/* The genuine WDK defines only local/system, system/local and local/local. */
#define FIXTURE_UNSUPPORTED_TRANSFER_DIRECTION \
    ((DXGK_MEMORY_TRANSFER_DIRECTION)(DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL + 1))
/* Paging-process layout in segment 1 and the virtual addresses it maps. */
#define ROOT_OFFSET 0x100000ull
#define MIDDLE_OFFSET 0x102000ull
#define LEAF_OFFSET 0x103000ull
#define LOCAL_A 0x200000ull /* two pages: allocation A's first placement */
#define LOCAL_B 0x300000ull /* two pages: A's second placement */
#define LOCAL_C 0x400000ull /* two pages: B, placed by VidMm without a copy */
#define LOCAL_FENCE 0x500000ull /* a fence page in segment 1 */
#define TABLE_LOCAL_A 0x600000ull
#define TABLE_LOCAL_B 0x700000ull
#define BASE_VA 0x40000000ull /* root index 1, middle index 0 */
#define VA_LOCAL_A (BASE_VA + 0x0000ull)    /* leaf 0..1 */
#define VA_SYSTEM (BASE_VA + 0x2000ull)     /* leaf 2..3, scattered pages */
#define VA_LOCAL_B (BASE_VA + 0x4000ull)    /* leaf 4..5 */
#define VA_LOCAL_C (BASE_VA + 0x6000ull)    /* leaf 6..7 */
#define VA_COPY (BASE_VA + 0x8000ull)       /* leaf 8..9, written by COPY_PAGE_TABLE_ENTRIES */
#define VA_LEAF_TABLE (BASE_VA + 0x10000ull) /* leaf 16: the leaf table itself */
#define VA_SPLIT (BASE_VA + 0x12000ull)     /* leaf 18..19: local pages out of order */
#define VA_FENCE (BASE_VA + 0x14000ull)     /* leaf 20: system page 2, a fence page */
#define VA_FENCE_LOCAL (BASE_VA + 0x15000ull) /* leaf 21: LOCAL_FENCE */
#define VA_FENCE_READ (BASE_VA + 0x16000ull) /* leaf 22: system page 2, read-only */
#define FENCE_OFFSET 0x40ull               /* a fence slot inside the page */
#define VA_TABLE_A (BASE_VA + 0x18000ull)
#define VA_TABLE_B (BASE_VA + 0x1a000ull)
#define TABLE_TEST_INDEX 7u
#define TWO_PAGES (2 * QLG_GPUVA_PAGE_BYTES)

static QLG_MINIPORT miniport;
static QLG_ADAPTER core;
static QLG_OBJECT system_context, allocation_a, allocation_b;
static volatile UCHAR *bar;
static UCHAR *system_pages;
static UINT mdl_maps, mdl_unmaps, locks, data_blocks;
static BOOLEAN refuse_paging_growth;
static BOOLEAN reset_during_map;
static UINT system_query_fault, system_query_calls;
enum { LARGE_UPLOAD_TEST_PAGES = 3 };
static UINT upload_page_locks[LARGE_UPLOAD_TEST_PAGES], upload_page_reads;
#if QLG_PAGING_SIGNALS
static UINT fence_reports, completions;
static UINT64 fence_seen_at_report, local_fence_at_report;
#endif

/* ---- Mocked kernel services ------------------------------------------------ */

static PMDL mock_allocate_mdl(PVOID address, ULONG bytes, BOOLEAN secondary, BOOLEAN charge,
                              PIRP irp)
{
    SIZE_T pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(address, bytes);
    PMDL mdl = malloc(sizeof(MDL) + pages * sizeof(PFN_NUMBER));

    CHECK(!secondary && !charge && !irp && address == NULL);
    if (mdl)
        MmInitializeMdl(mdl, address, bytes);
    return mdl;
}

static void mock_free_mdl(PMDL mdl)
{
    free(mdl);
}

/* The driver imports these; the fixture supplies the import slots. */
PMDL (*__imp_IoAllocateMdl)(PVOID, ULONG, BOOLEAN, BOOLEAN, PIRP) = mock_allocate_mdl;
void (*__imp_IoFreeMdl)(PMDL) = mock_free_mdl;

typedef struct FIXTURE_MAPPING {
    PFN_NUMBER pfns[FIXTURE_SYSTEM_PAGES];
    SIZE_T pages;
    BOOLEAN write;
    UCHAR *view;
    PMDL partial;
    QLG_PLATFORM_MEMORY owner;
    QLG_PLATFORM_MDL retained;
    QLM_PFN_INDEX_ENTRY index[FIXTURE_SYSTEM_PAGES];
} FIXTURE_MAPPING;

/* As QlgPlatformRetainMdl: copy the PFNs before returning (the source MDL is
 * freed right after the build), then map them contiguously. A bounce view
 * stands in for the kernel's aliasing map; writes land at unmap, which the
 * paging path calls only after the copy executed. */
static NTSTATUS mock_map_retained(void *context, PMDL mdl, BOOLEAN write, BOOLEAN indexed,
                                  void **mapping, SIZE_T *bytes, void **cookie)
{
    FIXTURE_MAPPING *map = calloc(1, sizeof(*map));
    SIZE_T page;

    CHECK(context == &core && map && (mdl->MdlFlags & MDL_PAGES_LOCKED));
    map->pages = MmGetMdlByteCount(mdl) / QLG_GPUVA_PAGE_BYTES;
    CHECK(map->pages && map->pages <= FIXTURE_SYSTEM_PAGES && !MmGetMdlByteOffset(mdl));
    map->write = write;
    map->partial = mock_allocate_mdl(NULL, (ULONG)(map->pages * QLG_GPUVA_PAGE_BYTES),
                                     FALSE, FALSE, NULL);
    CHECK(map->partial);
    map->view = malloc(map->pages * QLG_GPUVA_PAGE_BYTES);
    CHECK(map->view);
    for (page = 0; page < map->pages; ++page) {
        map->pfns[page] = MmGetMdlPfnArray(mdl)[page];
        MmGetMdlPfnArray(map->partial)[page] = map->pfns[page];
        memcpy(map->view + page * QLG_GPUVA_PAGE_BYTES,
               (void *)(map->pfns[page] << QLG_GPUVA_PAGE_SHIFT), QLG_GPUVA_PAGE_BYTES);
    }
    map->retained.owner = &map->owner;
    map->retained.partial = map->partial;
    map->retained.first_page = map->view;
    map->retained.bytes = map->pages * QLG_GPUVA_PAGE_BYTES;
    if (indexed && map->pages > 1)
        QlgPlatformMdlIndex(&map->retained, map->index);
    *mapping = map->view;
    *bytes = map->pages * QLG_GPUVA_PAGE_BYTES;
    *cookie = map;
    ++mdl_maps;
    if (reset_during_map) {
        reset_during_map = FALSE;
        CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS mock_map(void *context, PMDL mdl, BOOLEAN write, void **mapping,
                         SIZE_T *bytes, void **cookie)
{
    return mock_map_retained(context, mdl, write, FALSE, mapping, bytes, cookie);
}

static NTSTATUS mock_map_indexed_write(void *context, PMDL mdl, void **mapping,
                                       SIZE_T *bytes, void **cookie)
{
    return mock_map_retained(context, mdl, TRUE, TRUE, mapping, bytes, cookie);
}

static void mock_unmap(void *context, void *cookie)
{
    FIXTURE_MAPPING *map = cookie;
    SIZE_T page;

    CHECK(context == &core);
    for (page = 0; map->write && page < map->pages; ++page)
        memcpy((void *)(map->pfns[page] << QLG_GPUVA_PAGE_SHIFT),
               map->view + page * QLG_GPUVA_PAGE_BYTES, QLG_GPUVA_PAGE_BYTES);
    mock_free_mdl(map->partial);
    free(map->view);
    free(map);
    ++mdl_unmaps;
}

/* Same production accessor as the platform service, over this cookie's owned
 * partial descriptor. Its first-page prefix and page ordinal remain explicit. */
static NTSTATUS mock_address(void *context, const void *cookie, SIZE_T offset, UINT64 *address)
{
    const FIXTURE_MAPPING *map = cookie;

    CHECK(context == &core && map);
    ++system_query_calls;
    if (system_query_fault && --system_query_fault == 0)
        return STATUS_INVALID_PARAMETER;
    return QlgPlatformMdlAddress(&map->retained, offset, address);
}

static NTSTATUS observe_upload_address(void *context, const void *cookie, SIZE_T offset, UINT64 *address)
{
    CHECK(upload_page_reads < RTL_NUMBER_OF(upload_page_locks));
    upload_page_locks[upload_page_reads++] = locks;
    return mock_address(context, cookie, offset, address);
}

static NTSTATUS mock_last_alias(void *context, const void *cookie, UINT64 start,
                                 UINT64 bytes, UINT64 physical, UINT64 *offset)
{
    const FIXTURE_MAPPING *map = cookie;

    CHECK(context == &core && map);
    ++system_query_calls;
    if (system_query_fault && --system_query_fault == 0)
        return STATUS_INVALID_PARAMETER;
    return QlgPlatformMdlLastAlias(&map->retained, start, bytes, physical, offset);
}

/* Lifetime faults stop the fixture with a distinct code. */
__declspec(noreturn) void QlgPanic(void)
{
    ExitProcess(1);
}

static KIRQL fixture_lock(void *context)
{
    CHECK(context == &core);
    ++locks;
    return 0;
}

static void fixture_unlock(void *context, KIRQL level)
{
    CHECK(context == &core && level == 0);
}

static void *fixture_allocate(void *context, SIZE_T bytes)
{
    void *data;

    CHECK(context == &core);
    if (refuse_paging_growth)
        return NULL;
    data = malloc(bytes);
    if (data)
        ++data_blocks;
    return data;
}

static void fixture_release(void *context, void *memory)
{
    CHECK(context == &core && data_blocks != 0);
    --data_blocks;
    free(memory);
}

/* miniport_contracts.c links against these; only the segment matters here. */
NTSTATUS QlgLifecycleQuerySegment(const QLG_ADAPTER_LIFECYCLE *lifecycle,
                                  DXGK_SEGMENTDESCRIPTOR *segment)
{
    CHECK(lifecycle == &miniport.lifecycle);
    RtlZeroMemory(segment, sizeof(*segment));
    segment->Size = segment->CommitLimit = (SIZE_T)QLM_SEGMENT_BYTES;
    segment->Flags.CpuVisible = 1;
    return STATUS_SUCCESS;
}

/* This fixture exercises allocation callbacks without a scheduling engine. */
BOOLEAN QlgEngineDestroyReadyLocked(QLG_ENGINE *engine)
{
    UNREFERENCED_PARAMETER(engine);
    QlgPanic();
}

void QlgEngineFree(QLG_ENGINE *engine)
{
    UNREFERENCED_PARAMETER(engine);
    QlgPanic();
}

void QlgCursorCaps(const QLG_MINIPORT *owner, DXGK_DRIVERCAPS *caps)
{
    CHECK(owner == &miniport && caps);
}

NTSTATUS QlgMiniportVenusEscape(QLG_MINIPORT *owner, const DXGKARG_ESCAPE *args)
{
    UNREFERENCED_PARAMETER(owner);
    UNREFERENCED_PARAMETER(args);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS QlgDisplayPatch(const DXGKARG_PATCH *args)
{
    UNREFERENCED_PARAMETER(args);
    return STATUS_NOT_SUPPORTED;
}

/* ---- Helpers --------------------------------------------------------------- */

static DXGK_PTE pte(UINT segment, UINT64 address)
{
    DXGK_PTE entry = {0};
    entry.Valid = 1;
    entry.Segment = segment;
    entry.PageAddress = address >> QLG_GPUVA_PAGE_SHIFT;
    return entry;
}

static UCHAR *system_page(UINT index)
{
    return system_pages + index * QLG_GPUVA_PAGE_BYTES;
}

static UINT64 system_address(UINT index)
{
    return (UINT64)(ULONG_PTR)system_page(index);
}

static void init_allocation(QLG_OBJECT *allocation, UINT64 bytes)
{
    allocation->adapter = &core;
    allocation->kind = QLG_KIND_ALLOCATION;
    allocation->allocation_bytes = (SIZE_T)bytes;
    allocation->state = calloc(1, qlg_state_size(QLG_KIND_ALLOCATION));
    allocation->memory_state = calloc(1, qlm_allocation_size());
    CHECK(allocation->state && allocation->memory_state);
    CHECK(qlg_allocation_init(core.state, allocation->state, bytes) == STATUS_SUCCESS);
    CHECK(qlm_allocation_init(allocation->memory_state, (UINT64)(ULONG_PTR)allocation, bytes) ==
          STATUS_SUCCESS);
}

/* One paging operation through the 2.0 build-paging DDI path, with the ticket
 * (if any) it wrote. */
typedef struct FIXTURE_DMA {
    QLG_PAGING_TICKET dma[FIXTURE_TICKETS], data[FIXTURE_TICKETS];
} FIXTURE_DMA;

__declspec(noinline) static NTSTATUS build(DXGKARG_BUILDPAGINGBUFFER *args, FIXTURE_DMA *buffer, UINT *tickets)
{
    NTSTATUS status;

    RtlZeroMemory(buffer, sizeof(*buffer));
    args->pDmaBuffer = buffer->dma;
    args->DmaSize = sizeof(buffer->dma);
    args->pDmaBufferPrivateData = buffer->data;
    args->DmaBufferPrivateDataSize = sizeof(buffer->data);
    args->hSystemContext = &system_context;
    status = QlgWddm2BuildPaging(&core, args);
    *tickets =
        (UINT)(((UCHAR *)args->pDmaBuffer - (UCHAR *)buffer->dma) / sizeof(QLG_PAGING_TICKET));
    CHECK(args->DmaSize == sizeof(buffer->dma) - *tickets * sizeof(QLG_PAGING_TICKET));
    CHECK((UCHAR *)args->pDmaBufferPrivateData - (UCHAR *)buffer->data ==
          (LONG_PTR)(*tickets * sizeof(QLG_PAGING_TICKET)));
    return status;
}

/* Build, then run the one ticket as the paging worker would. */
__declspec(noinline) static NTSTATUS build_and_execute(DXGKARG_BUILDPAGINGBUFFER *args)
{
    FIXTURE_DMA buffer;
    UINT tickets;
    NTSTATUS status = build(args, &buffer, &tickets);

    if (!NT_SUCCESS(status))
        return status;
    CHECK(tickets == 1 && buffer.dma[0].ticket == buffer.data[0].ticket &&
          buffer.dma[0].epoch == buffer.data[0].epoch);
    return QlgExecutePaging(&core.paging, &buffer.dma[0]);
}

/* Paging-process bootstrap is the official immediate CPU_VIRTUAL exception:
 * pDmaBuffer is NULL and no ticket may be emitted. */
__declspec(noinline) static NTSTATUS update_table(UINT64 offset, UINT level, UINT start,
                                                  DXGK_PTE *entries, UINT count, BOOLEAN repeat)
{
    DXGKARG_BUILDPAGINGBUFFER args = {0};

    args.Operation = DXGK_OPERATION_UPDATE_PAGE_TABLE;
    args.UpdatePageTable.PageTableLevel = level;
    args.UpdatePageTable.PageTableAddress.CpuVirtual = (void *)(bar + offset);
    args.UpdatePageTable.pPageTableEntries = entries;
    args.UpdatePageTable.StartIndex = start;
    args.UpdatePageTable.NumPageTableEntries = count;
    args.UpdatePageTable.Flags.Repeat = repeat;
    return QlgWddm2BuildPaging(&core, &args);
}

__declspec(noinline) static DXGKARG_BUILDPAGINGBUFFER transfer(QLG_OBJECT *allocation, UINT64 offset, UINT64 bytes,
                                          UINT64 source, UINT64 destination,
                                          DXGK_MEMORY_TRANSFER_DIRECTION direction)
{
    DXGKARG_BUILDPAGINGBUFFER args = {0};
    args.Operation = DXGK_OPERATION_VIRTUAL_TRANSFER;
    args.TransferVirtual.hAllocation = allocation;
    args.TransferVirtual.AllocationOffsetInBytes = offset;
    args.TransferVirtual.TransferSizeInBytes = bytes;
    args.TransferVirtual.SourceVirtualAddress = source;
    args.TransferVirtual.DestinationVirtualAddress = destination;
    args.TransferVirtual.TransferDirection = direction;
    return args;
}

__declspec(noinline) static void notify(QLG_OBJECT *allocation, BOOLEAN resident, UINT segment, UINT64 offset)
{
    DXGKARG_BUILDPAGINGBUFFER args = {0};
    FIXTURE_DMA buffer;
    UINT tickets;

    args.Operation = DXGK_OPERATION_NOTIFY_RESIDENCY;
    args.NotifyResidency.hAllocation = allocation;
    args.NotifyResidency.Resident = resident;
    args.NotifyResidency.PhysicalAddress.SegmentId = segment;
    args.NotifyResidency.PhysicalAddress.SegmentOffset = offset;
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS && tickets == 0);
}

/* ---- Checks ---------------------------------------------------------------- */

static void check_geometry_and_queries(void)
{
    DXGK_PAGE_TABLE_LEVEL_DESC desc;
    DXGKARG_GETROOTPAGETABLESIZE root = {0};
    DXGK_GPUMMUCAPS caps;
    UINT level;

    CHECK(QLG_GPUVA_ADDRESS_BITS == 40);
    CHECK(QlgGpuVaLevelEntries(0) == 512 && QlgGpuVaLevelEntries(1) == 512 &&
          QlgGpuVaLevelEntries(2) == 1024 && QlgGpuVaLevelEntries(3) == 0);
    CHECK(QlgGpuVaLevelShift(0) == 12 && QlgGpuVaLevelShift(1) == 21 &&
          QlgGpuVaLevelShift(2) == 30);
    CHECK(QlgGpuVaIndex(2, BASE_VA) == 1 && QlgGpuVaIndex(1, BASE_VA) == 0 &&
          QlgGpuVaIndex(0, VA_SYSTEM) == 2 && QlgGpuVaIndex(2, (1ull << 40) - 1) == 1023);
    QlgGpuVaMmuCaps(&caps);
    CHECK(caps.PageTableUpdateMode == DXGK_PAGETABLEUPDATE_GPU_PHYSICAL &&
          caps.VirtualAddressBitCount == 40 && caps.PageTableLevelCount == 3 &&
          caps.CacheCoherentMemorySupported && !caps.LargePageSupported &&
          !caps.DualPteSupported && !caps.ZeroInPteSupported &&
          caps.LeafPageTableSizeFor64KPagesInBytes == QLG_GPUVA_PAGE_BYTES &&
          !caps.LegacyBehaviors.SourcePageTableVaInTransfer);
    for (level = 0; level < QLG_GPUVA_LEVELS; ++level) {
        CHECK(QlgGpuVaLevelDesc(level, &desc) == STATUS_SUCCESS);
        CHECK(desc.PageTableSegmentId == QLM_MEMORY_SEGMENT &&
              desc.PagingProcessPageTableSegmentId == QLM_MEMORY_SEGMENT &&
              desc.PageTableAlignmentInBytes == QLG_GPUVA_PAGE_BYTES &&
              desc.PageTableSizeInBytes == QlgGpuVaLevelEntries(level) * sizeof(QLG_GPUVA_PTE) &&
              desc.PageTableIndexBitCount == (level == 2 ? 10u : 9u));
    }
    CHECK(QlgGpuVaLevelDesc(3, &desc) == STATUS_INVALID_PARAMETER);
    root.NumberOfPte = 7;
    CHECK(QlgMiniportRootSize(&miniport, &root) == 8192 && root.NumberOfPte == 1024);
    root.PhysicalAdapterIndex = 1;
    CHECK(QlgMiniportRootSize(&miniport, &root) == 0);
}

static void check_encoding(void)
{
    DXGK_PTE entry = {0};
    QLG_GPUVA_PTE encoded = ~0ull;
    UINT segment;
    UINT64 address;

    CHECK(QlgGpuVaEncode(&entry, TRUE, &encoded) == STATUS_SUCCESS && encoded == 0);
    CHECK(!QlgGpuVaDecode(encoded, &segment, &address));
    entry = pte(QLM_MEMORY_SEGMENT, LOCAL_A);
    entry.ReadOnly = 1;
    CHECK(QlgGpuVaEncode(&entry, FALSE, &encoded) == STATUS_SUCCESS);
    CHECK(QlgGpuVaDecode(encoded, &segment, &address) && segment == QLM_MEMORY_SEGMENT &&
          address == LOCAL_A && (encoded & QLG_GPUVA_PTE_READ_ONLY));
    entry = pte(QLM_SYSTEM_SEGMENT, 0x123456789000ull);
    CHECK(QlgGpuVaEncode(&entry, TRUE, &encoded) == STATUS_SUCCESS);
    CHECK(QlgGpuVaDecode(encoded, &segment, &address) && segment == QLM_SYSTEM_SEGMENT &&
          address == 0x123456789000ull);
    /* Page tables only live in segment 1; nothing else is honoured. */
    CHECK(QlgGpuVaEncode(&entry, FALSE, &encoded) == STATUS_NOT_SUPPORTED);
    entry = pte(QLM_MEMORY_SEGMENT, QLM_SEGMENT_BYTES - QLM_PAGE_BYTES);
    CHECK(QlgGpuVaEncode(&entry, TRUE, &encoded) == STATUS_SUCCESS);
    entry = pte(QLM_MEMORY_SEGMENT, QLM_SEGMENT_BYTES);
    CHECK(QlgGpuVaEncode(&entry, TRUE, &encoded) == STATUS_INVALID_PARAMETER);
    entry = pte(QLM_APERTURE_SEGMENT, 0);
    CHECK(QlgGpuVaEncode(&entry, TRUE, &encoded) == STATUS_NOT_SUPPORTED);
    entry = pte(QLM_MEMORY_SEGMENT, 0);
    entry.LargePage = 1;
    CHECK(QlgGpuVaEncode(&entry, TRUE, &encoded) == STATUS_NOT_SUPPORTED);
    entry = pte(QLM_MEMORY_SEGMENT, 0);
    entry.PhysicalAdapterIndex = 1;
    CHECK(QlgGpuVaEncode(&entry, TRUE, &encoded) == STATUS_NOT_SUPPORTED);
    entry = pte(QLM_MEMORY_SEGMENT, 0);
    entry.Zero = 1;
    CHECK(QlgGpuVaEncode(&entry, TRUE, &encoded) == STATUS_NOT_SUPPORTED);
}

/* Build the paging process's tables through UPDATE_PAGE_TABLE, as VidMm does. */
static void check_page_table_updates(void)
{
    DXGK_PTE entries[8];
    DXGKARG_SETROOTPAGETABLE root = {0};
    volatile QLG_GPUVA_PTE *leaf = (volatile QLG_GPUVA_PTE *)(bar + LEAF_OFFSET);
    UINT index;

    /* Every entry invalid first, with Repeat. */
    RtlZeroMemory(entries, sizeof(entries));
    memset((void *)(bar + ROOT_OFFSET), FIXTURE_POISON, 4 * QLG_GPUVA_PAGE_BYTES);
    CHECK(update_table(ROOT_OFFSET, 2, 0, entries, 1024, TRUE) == STATUS_SUCCESS);
    CHECK(update_table(MIDDLE_OFFSET, 1, 0, entries, 512, TRUE) == STATUS_SUCCESS);
    CHECK(update_table(LEAF_OFFSET, 0, 0, entries, 512, TRUE) == STATUS_SUCCESS);
    for (index = 0; index < 512; ++index)
        CHECK(leaf[index] == 0);
    entries[0] = pte(QLM_MEMORY_SEGMENT, MIDDLE_OFFSET);
    CHECK(update_table(ROOT_OFFSET, 2, 1, entries, 1, FALSE) == STATUS_SUCCESS);
    entries[0] = pte(QLM_MEMORY_SEGMENT, LEAF_OFFSET);
    CHECK(update_table(MIDDLE_OFFSET, 1, 0, entries, 1, FALSE) == STATUS_SUCCESS);
    /* Leaves: A, scattered system pages 3 and 1, B, C. */
    entries[0] = pte(QLM_MEMORY_SEGMENT, LOCAL_A);
    entries[1] = pte(QLM_MEMORY_SEGMENT, LOCAL_A + QLG_GPUVA_PAGE_BYTES);
    entries[2] = pte(QLM_SYSTEM_SEGMENT, system_address(3));
    entries[3] = pte(QLM_SYSTEM_SEGMENT, system_address(1));
    entries[4] = pte(QLM_MEMORY_SEGMENT, LOCAL_B);
    entries[5] = pte(QLM_MEMORY_SEGMENT, LOCAL_B + QLG_GPUVA_PAGE_BYTES);
    entries[6] = pte(QLM_MEMORY_SEGMENT, LOCAL_C);
    entries[7] = pte(QLM_MEMORY_SEGMENT, LOCAL_C + QLG_GPUVA_PAGE_BYTES);
    CHECK(update_table(LEAF_OFFSET, 0, 0, entries, 8, FALSE) == STATUS_SUCCESS);
    entries[0] = pte(QLM_MEMORY_SEGMENT, LEAF_OFFSET);
    CHECK(update_table(LEAF_OFFSET, 0, 16, entries, 1, FALSE) == STATUS_SUCCESS);
    entries[0] = pte(QLM_MEMORY_SEGMENT, LOCAL_A + QLG_GPUVA_PAGE_BYTES);
    entries[1] = pte(QLM_MEMORY_SEGMENT, LOCAL_A);
    CHECK(update_table(LEAF_OFFSET, 0, 18, entries, 2, FALSE) == STATUS_SUCCESS);
    CHECK(leaf[2] == (QLG_GPUVA_PTE_VALID | system_address(3)));

    /* A batch with one bad entry stores nothing; bounds and modes are checked. */
    entries[0] = pte(QLM_MEMORY_SEGMENT, LOCAL_B);
    entries[1] = pte(QLM_APERTURE_SEGMENT, 0);
    CHECK(update_table(LEAF_OFFSET, 0, 0, entries, 2, FALSE) == STATUS_NOT_SUPPORTED);
    CHECK(leaf[0] == (QLG_GPUVA_PTE_VALID | (QLM_MEMORY_SEGMENT << 2) | LOCAL_A));
    CHECK(update_table(LEAF_OFFSET, 0, 511, entries, 2, FALSE) == STATUS_INVALID_PARAMETER);
    CHECK(update_table(ROOT_OFFSET, 2, 1023, entries, 1, FALSE) == STATUS_SUCCESS);
    CHECK(update_table(ROOT_OFFSET, 3, 0, entries, 1, FALSE) == STATUS_INVALID_PARAMETER);
    RtlZeroMemory(entries, sizeof(entries));
    CHECK(update_table(ROOT_OFFSET, 2, 1023, entries, 1, FALSE) == STATUS_SUCCESS);

    /* Before SetRootPageTable no virtual operation can resolve. */
    {
        DXGKARG_BUILDPAGINGBUFFER args =
            transfer(&allocation_a, 0, TWO_PAGES, VA_LOCAL_A, VA_SYSTEM,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
        FIXTURE_DMA buffer;
        UINT tickets;
        CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_DEVICE_STATE && tickets == 0);
    }
    root.hContext = &system_context;
    root.Address.SegmentId = QLM_SYSTEM_SEGMENT;
    root.Address.SegmentOffset = ROOT_OFFSET;
    root.NumEntries = 1024;
    QlgMiniportSetRoot(&miniport, &root);
    CHECK(system_context.root_entries == 0);
    root.Address.SegmentId = QLM_MEMORY_SEGMENT;
    QlgMiniportSetRoot(&miniport, &root);
    CHECK(system_context.root_entries == 1024 && system_context.root_address == ROOT_OFFSET);
}

static void check_walks(void)
{
    QLG_GPUVA_SPACE space = {0};
    QLG_GPUVA_RUN run;
    PFN_NUMBER pfns[2];
    DXGK_BUILDPAGINGBUFFER_COPY_RANGE range = {0};
    UINT segment;
    UINT64 page;

    space.context = &core;
    space.root_segment = system_context.root_segment;
    space.root_address = system_context.root_address;
    space.root_entries = system_context.root_entries;
    /* The real resolver of miniport_wddm2.c is static; walks go through the
     * DDIs below. Here a null resolver is refused. */
    CHECK(QlgGpuVaTranslate(&space, VA_LOCAL_A, &segment, &page) == STATUS_INVALID_PARAMETER);
    CHECK(QlgGpuVaResolveRange(&space, VA_LOCAL_A, 0, &run, pfns) == STATUS_INVALID_PARAMETER);

    /* COPY_PAGE_TABLE_ENTRIES: the leaf table is mapped at VA_LEAF_TABLE. */
    range.NumPageTableEntries = 2;
    range.SrcPageTableAddress = range.DstPageTableAddress = VA_LEAF_TABLE;
    range.SrcStartPteIndex = 0;
    range.DstStartPteIndex = 8;
    {
        DXGKARG_BUILDPAGINGBUFFER args = {0};
        FIXTURE_DMA buffer;
        UINT tickets;
        volatile QLG_GPUVA_PTE *leaf = (volatile QLG_GPUVA_PTE *)(bar + LEAF_OFFSET);

        args.Operation = DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES;
        args.CopyPageTableEntries.NumRanges = 1;
        args.CopyPageTableEntries.pRanges = &range;
        CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS && tickets == 1);
        CHECK(leaf[8] == 0 && leaf[9] == 0);
        CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_SUCCESS);
        CHECK(leaf[8] == leaf[0] && leaf[9] == leaf[1]);
        /* A table address that is not mapped fails without copying. */
        range.SrcPageTableAddress = BASE_VA + 0x100000ull;
        range.DstStartPteIndex = 10;
        CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && leaf[10] == 0);
        /* Entries mapping system pages are not page tables. */
        range.SrcPageTableAddress = VA_SYSTEM;
        CHECK(build(&args, &buffer, &tickets) == STATUS_NOT_SUPPORTED && leaf[10] == 0);
    }
}

static void check_virtual_paging(void)
{
    /* Static: many operations in one frame would need a stack probe. */
    static DXGKARG_BUILDPAGINGBUFFER args;
    static FIXTURE_DMA buffer;
    UINT tickets, index, words = (UINT)(TWO_PAGES / sizeof(UINT));
    const UINT *local_a = (const UINT *)(bar + LOCAL_A);
    const UINT *local_b = (const UINT *)(bar + LOCAL_B);

    /* FILL_VIRTUAL: whole allocation A at LOCAL_A, through the 1.3 fill. */
    args.Operation = DXGK_OPERATION_VIRTUAL_FILL;
    args.FillVirtual.hAllocation = &allocation_a;
    args.FillVirtual.FillSizeInBytes = TWO_PAGES;
    args.FillVirtual.FillPattern = FIXTURE_FILL;
    args.FillVirtual.DestinationVirtualAddress = VA_LOCAL_A;
    CHECK(build_and_execute(&args) == STATUS_SUCCESS);
    for (index = 0; index < words; ++index)
        CHECK(local_a[index] == FIXTURE_FILL);
    args.FillVirtual.AllocationOffsetInBytes = QLG_GPUVA_PAGE_BYTES;
    args.FillVirtual.FillSizeInBytes = QLG_GPUVA_PAGE_BYTES;
    args.FillVirtual.DestinationVirtualAddress += QLG_GPUVA_PAGE_BYTES;
    CHECK(build_and_execute(&args) == STATUS_SUCCESS);
    args.FillVirtual.AllocationOffsetInBytes = 0;
    args.FillVirtual.FillSizeInBytes = TWO_PAGES;
    args.FillVirtual.DestinationVirtualAddress = VA_SYSTEM;
    CHECK(build(&args, &buffer, &tickets) == STATUS_NOT_SUPPORTED && tickets == 0);

    /* Evict A to the scattered system pages: page 0 lands in system page 3,
     * page 1 in system page 1, through an MDL built from the walked PFNs. */
    ((UINT *)(bar + LOCAL_A))[0] = 0x11111111;
    ((UINT *)(bar + LOCAL_A + QLG_GPUVA_PAGE_BYTES))[0] = 0x22222222;
    args = transfer(&allocation_a, 0, TWO_PAGES, VA_LOCAL_A, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build_and_execute(&args) == STATUS_SUCCESS && mdl_maps == mdl_unmaps);
    CHECK(((UINT *)system_page(3))[0] == 0x11111111 && ((UINT *)system_page(1))[0] == 0x22222222);
    CHECK(((UINT *)system_page(3))[1] == FIXTURE_FILL && ((UINT *)system_page(0))[0] == 0);

    /* Page A back in at LOCAL_B in two ordered chunks. */
    ((UINT *)system_page(3))[1] = 0x33333333;
    args = transfer(&allocation_a, 0, QLG_GPUVA_PAGE_BYTES, VA_SYSTEM, VA_LOCAL_B,
                    DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build_and_execute(&args) == STATUS_SUCCESS);
    args = transfer(&allocation_a, QLG_GPUVA_PAGE_BYTES, QLG_GPUVA_PAGE_BYTES,
                    VA_SYSTEM + QLG_GPUVA_PAGE_BYTES, VA_LOCAL_B + QLG_GPUVA_PAGE_BYTES,
                    DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build_and_execute(&args) == STATUS_SUCCESS);
    CHECK(local_b[0] == 0x11111111 && local_b[1] == 0x33333333 &&
          local_b[words / 2] == 0x22222222 && local_b[words / 2 + 1] == FIXTURE_FILL);

    /* Local to local: B to A's first place, then the policy's own checks. */
    args = transfer(&allocation_a, 0, TWO_PAGES, VA_LOCAL_B, VA_LOCAL_A,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build_and_execute(&args) == STATUS_SUCCESS && local_a[1] == 0x33333333);
    /* A is no longer at LOCAL_B: copying out from there is refused. */
    args = transfer(&allocation_a, 0, TWO_PAGES, VA_LOCAL_B, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);

    /* Translation failures leave no ticket and no MDL behind. */
    args = transfer(&allocation_a, 0, TWO_PAGES, VA_SPLIT, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &buffer, &tickets) == STATUS_NOT_SUPPORTED && tickets == 0);
    args = transfer(&allocation_a, 0, TWO_PAGES, VA_LOCAL_A, VA_LOCAL_B,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args = transfer(&allocation_a, 0, TWO_PAGES, VA_SYSTEM, VA_SYSTEM,
                    FIXTURE_UNSUPPORTED_TRANSFER_DIRECTION);
    CHECK(build(&args, &buffer, &tickets) == STATUS_NOT_SUPPORTED && tickets == 0);
    args = transfer(&allocation_a, 0, TWO_PAGES, BASE_VA + 0x100000ull, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args = transfer(&allocation_a, 0, TWO_PAGES * 2, VA_LOCAL_A, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args = transfer(NULL, 0, TWO_PAGES, VA_LOCAL_A, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &buffer, &tickets) != STATUS_SUCCESS && tickets == 0);
    args = transfer(&allocation_a, 0, TWO_PAGES, VA_LOCAL_A, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    args.TransferVirtual.Flags.Src64KBPages = 1;
    CHECK(build(&args, &buffer, &tickets) == STATUS_NOT_SUPPORTED && tickets == 0);
    CHECK(mdl_maps == mdl_unmaps);
}

typedef struct FIXTURE_PARTIAL_COMPLETION {
    FIXTURE_DMA *buffer;
    UINT reports;
} FIXTURE_PARTIAL_COMPLETION;

static void partial_enqueue(void *context, UINT64 identity)
{
    CHECK(context && identity == 1);
}

/* A failed, undelivered notification keeps the finished job pinned. Retrying
 * reports completion without executing its range again or releasing it early. */
static NTSTATUS partial_complete(void *context, UINT fence, BOOLEAN *delivered)
{
    FIXTURE_PARTIAL_COMPLETION *completion = context;
    const QLG_PAGING_TICKET *ticket = &completion->buffer->dma[0];
    UINT *local = (UINT *)(bar + LOCAL_A);

    CHECK(fence == FIXTURE_PARTIAL_FENCE);
    CHECK(qlg_allocation_can_drop(core.state, allocation_a.state) != STATUS_SUCCESS);
    CHECK(QlgCancelPaging(&core.paging, ticket, TRUE) == STATUS_DEVICE_BUSY);
    if (completion->reports++ == 0) {
        CHECK(local[0] == (UINT)~FIXTURE_FILL);
        local[0] = FIXTURE_FILL;
        *delivered = FALSE;
        return STATUS_UNSUCCESSFUL;
    }
    CHECK(local[0] == FIXTURE_FILL);
    *delivered = TRUE;
    return STATUS_SUCCESS;
}

/* Partial fills change exactly their resolved subrange, retain the original
 * residency and use the production paging ticket lifetime through cancellation. */
static void check_partial_virtual_fills(void)
{
    static DXGKARG_BUILDPAGINGBUFFER args;
    static FIXTURE_DMA buffer;
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    FIXTURE_PARTIAL_COMPLETION completion = {&buffer, 0};
    QLG_PAGING_SUBMIT_SERVICES services = {0};
    DXGKARG_SUBMITCOMMAND command = {0};
    UINT tickets, index, page_words = (UINT)(QLG_GPUVA_PAGE_BYTES / sizeof(UINT));
    UINT *local = (UINT *)(bar + LOCAL_A);
    const UINT alternate = ~FIXTURE_FILL;

    args.Operation = DXGK_OPERATION_VIRTUAL_FILL;
    args.FillVirtual.hAllocation = &allocation_a;
    args.FillVirtual.FillSizeInBytes = QLG_GPUVA_PAGE_BYTES;
    args.FillVirtual.FillPattern = alternate;
    args.FillVirtual.DestinationVirtualAddress = VA_LOCAL_A;
    CHECK(build_and_execute(&args) == STATUS_SUCCESS);
    for (index = 0; index < page_words; ++index)
        CHECK(local[index] == alternate);

    /* Cancel a prepared suffix: it neither writes nor relinquishes residency. */
    args.FillVirtual.AllocationOffsetInBytes = QLG_GPUVA_PAGE_BYTES;
    args.FillVirtual.DestinationVirtualAddress = VA_LOCAL_A + QLG_GPUVA_PAGE_BYTES;
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS && tickets == 1);
    CHECK(qlm_can_drop(allocation_a.memory_state) != STATUS_SUCCESS);
    CHECK(qlm_adopt(core.memory_arena, allocation_a.memory_state, LOCAL_B, TRUE) != STATUS_SUCCESS);
    CHECK(QlgCancelPaging(&core.paging, &buffer.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_INVALID_PARAMETER);
    CHECK(local[page_words] == 0x22222222);
    CHECK(qlm_adopt(core.memory_arena, allocation_a.memory_state, LOCAL_A, FALSE) == STATUS_SUCCESS);
    CHECK(qlm_adopt(core.memory_arena, allocation_a.memory_state, LOCAL_B, FALSE) != STATUS_SUCCESS);
    CHECK(build_and_execute(&args) == STATUS_SUCCESS);
    for (index = 0; index < 2 * page_words; ++index)
        CHECK(local[index] == alternate);

    /* A valid translated range at the wrong allocation base cannot relocate A. */
    args.FillVirtual.DestinationVirtualAddress = VA_LOCAL_B + QLG_GPUVA_PAGE_BYTES;
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args.FillVirtual.DestinationVirtualAddress = VA_LOCAL_A;
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args.FillVirtual.AllocationOffsetInBytes = 0;
    args.FillVirtual.DestinationVirtualAddress = VA_LOCAL_C;
    args.FillVirtual.hAllocation = &allocation_b;
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);

    /* Malformed bounds never produce tickets, including overflow and alignment. */
    args.FillVirtual.hAllocation = &allocation_a;
    args.FillVirtual.DestinationVirtualAddress = VA_LOCAL_A;
    args.FillVirtual.FillSizeInBytes = 0;
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args.FillVirtual.FillSizeInBytes = MAXUINT64;
    args.FillVirtual.AllocationOffsetInBytes = QLG_GPUVA_PAGE_BYTES;
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args.FillVirtual.FillSizeInBytes = QLG_GPUVA_PAGE_BYTES;
    args.FillVirtual.AllocationOffsetInBytes = MAXUINT64;
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args.FillVirtual.AllocationOffsetInBytes = 1;
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args.FillVirtual.AllocationOffsetInBytes = 0;
    args.FillVirtual.FillSizeInBytes = QLG_GPUVA_PAGE_BYTES - 1;
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);

    /* A ticket admitted before reset cannot execute after admission is closed. */
    args.FillVirtual.FillSizeInBytes = QLG_GPUVA_PAGE_BYTES;
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS);
    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_DEVICE_NOT_READY);
    CHECK(QlgCancelPaging(&core.paging, &buffer.dma[0], TRUE) == STATUS_SUCCESS);
    CHECK(build(&args, &buffer, &tickets) == STATUS_DEVICE_NOT_READY && tickets == 0);
    CHECK(qlg_reset_quiesced(core.state) == STATUS_SUCCESS &&
          qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);
    CHECK(qlm_adopt(core.memory_arena, allocation_a.memory_state, LOCAL_A, FALSE) == STATUS_SUCCESS);

    /* Exercise the production Submit path and its failed-report retry lifetime. */
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS && tickets == 1);
    services.context = &completion;
    services.enqueue = partial_enqueue;
    services.complete = partial_complete;
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    command.Flags.Paging = 1;
    command.NodeOrdinal = QLG_RENDER_NODE;
    command.EngineOrdinal = QLG_RENDER_ENGINE;
    command.SubmissionFenceId = FIXTURE_PARTIAL_FENCE;
    command.DmaBufferSize = sizeof(buffer.dma);
    command.DmaBufferSubmissionEndOffset = sizeof(QLG_PAGING_TICKET);
    command.pDmaBufferPrivateData = buffer.data;
    command.DmaBufferPrivateDataSize = sizeof(buffer.data);
    command.DmaBufferPrivateDataSubmissionEndOffset = sizeof(QLG_PAGING_TICKET);
    CHECK(QlgPagingSubmit(&submit, &command) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_UNSUCCESSFUL);
    CHECK(qlg_allocation_can_drop(core.state, allocation_a.state) != STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_SUCCESS && completion.reports == 2);
    CHECK(qlg_allocation_can_drop(core.state, allocation_a.state) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_INVALID_PARAMETER);
}

typedef struct FIXTURE_IMPLICIT_COMPLETION {
    FIXTURE_DMA *buffer;
    UINT reports, maps;
} FIXTURE_IMPLICIT_COMPLETION;

static void implicit_enqueue(void *context, UINT64 identity)
{
    CHECK(context && identity == 1);
}

static NTSTATUS implicit_complete(void *context, UINT fence, BOOLEAN *delivered)
{
    FIXTURE_IMPLICIT_COMPLETION *completion = context;
    UINT *destination = (UINT *)(bar + TABLE_LOCAL_B);

    CHECK(fence == FIXTURE_IMPLICIT_FENCE);
    CHECK(mdl_maps == completion->maps + 1 && mdl_unmaps == completion->maps);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) != STATUS_SUCCESS);
    CHECK(QlgCancelPaging(&core.paging, &completion->buffer->dma[0], TRUE) == STATUS_DEVICE_BUSY);
    if (completion->reports++ == 0) {
        CHECK(destination[0] == FIXTURE_FILL);
        destination[0] = ~FIXTURE_FILL;
        *delivered = FALSE;
        return STATUS_UNSUCCESSFUL;
    }
    CHECK(destination[0] == (UINT)~FIXTURE_FILL);
    *delivered = TRUE;
    return STATUS_SUCCESS;
}

/* OS-owned page tables remain distinct from driver allocations. The production
 * DDI resolves their actual ranges, owns a real adapter reservation and retains
 * mappings through copy and DMA-fence reporting, including a retry. */
static void check_implicit_paging(void)
{
    static DXGKARG_BUILDPAGINGBUFFER args;
    static FIXTURE_DMA buffer;
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    QLG_PAGING_SUBMIT_SERVICES services = {0};
    FIXTURE_IMPLICIT_COMPLETION completion = {&buffer, 0, mdl_maps};
    DXGKARG_SUBMITCOMMAND command = {0};
    DXGK_PTE entries[4];
    UINT tickets, index;
    UINT *source = (UINT *)(bar + TABLE_LOCAL_A);
    UINT *destination = (UINT *)(bar + TABLE_LOCAL_B);
    volatile QLG_GPUVA_PTE *table = (volatile QLG_GPUVA_PTE *)(bar + TABLE_LOCAL_A);
    QLG_GPUVA_PTE before;

    /* An unmatched implicit release must never consume a driver allocation's
     * own paging reservation from the common adapter quota. */
    CHECK(qlg_paging_acquire(core.state, allocation_a.state) == STATUS_SUCCESS);
    CHECK(qlg_implicit_paging_release(core.state) == STATUS_INVALID_PARAMETER);
    CHECK(qlg_paging_release(core.state, allocation_a.state) == STATUS_SUCCESS);

    entries[0] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_A);
    entries[1] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_A + QLG_GPUVA_PAGE_BYTES);
    entries[2] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_B);
    entries[3] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_B + QLG_GPUVA_PAGE_BYTES);
    CHECK(update_table(LEAF_OFFSET, 0, 24, entries, 4, FALSE) == STATUS_SUCCESS);
    for (index = 0; index < TWO_PAGES / sizeof(UINT); ++index)
        source[index] = FIXTURE_FILL;
    args = transfer(NULL, QLG_GPUVA_PAGE_BYTES, QLG_GPUVA_PAGE_BYTES,
                    VA_TABLE_A + QLG_GPUVA_PAGE_BYTES, VA_TABLE_B + QLG_GPUVA_PAGE_BYTES,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS && tickets == 1);
    CHECK(destination[QLG_GPUVA_PAGE_BYTES / sizeof(UINT)] == 0);
    CHECK(qlm_adopt(core.memory_arena, allocation_b.memory_state, TABLE_LOCAL_B, TRUE) != STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_SUCCESS);
    CHECK(destination[0] == 0 && destination[QLG_GPUVA_PAGE_BYTES / sizeof(UINT)] == FIXTURE_FILL);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_INVALID_PARAMETER);

    /* GPU_PHYSICAL configurations also receive physical paging operations.
     * TransferOffset is added once to each local segment base. */
    RtlZeroMemory(&args, sizeof(args));
    args.Operation = DXGK_OPERATION_TRANSFER;
    args.Transfer.TransferOffset = (UINT)QLG_GPUVA_PAGE_BYTES;
    args.Transfer.TransferSize = (SIZE_T)QLG_GPUVA_PAGE_BYTES;
    args.Transfer.Source.SegmentId = args.Transfer.Destination.SegmentId = QLM_MEMORY_SEGMENT;
    args.Transfer.Source.SegmentAddress.QuadPart = TABLE_LOCAL_A;
    args.Transfer.Destination.SegmentAddress.QuadPart = TABLE_LOCAL_B;
    CHECK(build_and_execute(&args) == STATUS_SUCCESS && destination[0] == 0);
    CHECK(destination[QLG_GPUVA_PAGE_BYTES / sizeof(UINT)] == FIXTURE_FILL);
    RtlZeroMemory(&args, sizeof(args));
    args.Operation = DXGK_OPERATION_FILL;
    args.Fill.FillSize = (SIZE_T)QLG_GPUVA_PAGE_BYTES;
    args.Fill.FillPattern = FIXTURE_FILL;
    args.Fill.Destination.SegmentId = QLM_MEMORY_SEGMENT;
    args.Fill.Destination.SegmentAddress.QuadPart = TABLE_LOCAL_B;
    CHECK(build_and_execute(&args) == STATUS_SUCCESS && destination[0] == FIXTURE_FILL);

    /* Both MDL directions copy the resolved scattered pages, without a fake handle. */
    args = transfer(NULL, 0, TWO_PAGES, VA_TABLE_A, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build_and_execute(&args) == STATUS_SUCCESS && mdl_maps == mdl_unmaps);
    CHECK(((UINT *)system_page(3))[0] == FIXTURE_FILL && ((UINT *)system_page(1))[0] == FIXTURE_FILL);
    args = transfer(NULL, 0, TWO_PAGES, VA_SYSTEM, VA_TABLE_B,
                    DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build_and_execute(&args) == STATUS_SUCCESS && destination[0] == FIXTURE_FILL);
    args = transfer(NULL, 0, TWO_PAGES, VA_TABLE_A, VA_TABLE_A,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args = transfer(NULL, MAXUINT64, TWO_PAGES, VA_TABLE_A, VA_TABLE_B,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    args = transfer(NULL, 0, TWO_PAGES, VA_TABLE_A, VA_SYSTEM,
                    FIXTURE_UNSUPPORTED_TRANSFER_DIRECTION);
    CHECK(build(&args, &buffer, &tickets) == STATUS_NOT_SUPPORTED && tickets == 0);

    /* A NULL-handle fill affects only the requested range and adopts no placement. */
    RtlZeroMemory(&args, sizeof(args));
    args.Operation = DXGK_OPERATION_VIRTUAL_FILL;
    args.FillVirtual.DestinationVirtualAddress = VA_TABLE_A;
    args.FillVirtual.FillSizeInBytes = QLG_GPUVA_PAGE_BYTES;
    args.FillVirtual.FillPattern = ~FIXTURE_FILL;
    CHECK(build_and_execute(&args) == STATUS_SUCCESS && source[0] == (UINT)~FIXTURE_FILL);
    CHECK(source[QLG_GPUVA_PAGE_BYTES / sizeof(UINT)] == FIXTURE_FILL);

    /* Ordinary GPU_PHYSICAL updates snapshot PTE descriptors and emit a ticket. */
    RtlZeroMemory(&args, sizeof(args));
    args.Operation = DXGK_OPERATION_UPDATE_PAGE_TABLE;
    args.UpdatePageTable.UpdateMode = DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    args.UpdatePageTable.PageTableAddress.GpuPhysical.SegmentId = QLM_MEMORY_SEGMENT;
    args.UpdatePageTable.PageTableAddress.GpuPhysical.SegmentOffset = TABLE_LOCAL_A;
    args.UpdatePageTable.StartIndex = TABLE_TEST_INDEX;
    args.UpdatePageTable.NumPageTableEntries = 2;
    entries[0] = pte(QLM_MEMORY_SEGMENT, LOCAL_B);
    entries[1] = pte(QLM_SYSTEM_SEGMENT, system_address(3));
    args.UpdatePageTable.pPageTableEntries = entries;
    before = table[TABLE_TEST_INDEX];
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS && tickets == 1);
    CHECK(table[TABLE_TEST_INDEX] == before);
    RtlZeroMemory(entries, sizeof(entries));
    {
        DXGKARG_BUILDPAGINGBUFFER dependent = transfer(NULL, 0, TWO_PAGES, VA_TABLE_A, VA_TABLE_B,
                                                       DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
        FIXTURE_DMA other;
        UINT count;

        /* A local source can now follow its exact queued writer, with a real
         * retained proof. It cannot execute before that producer completes. */
        CHECK(build(&dependent, &other, &count) == STATUS_SUCCESS && count == 1);
        CHECK(QlgExecutePaging(&core.paging, &other.dma[0]) == STATUS_DEVICE_BUSY);
        CHECK(QlgCancelPaging(&core.paging, &other.dma[0], FALSE) == STATUS_SUCCESS);
        CHECK(update_table(LEAF_OFFSET, 0, 31, entries, 1, FALSE) != STATUS_SUCCESS);
    }
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_SUCCESS);
    CHECK(table[TABLE_TEST_INDEX] == (QLG_GPUVA_PTE_VALID |
          (QLM_MEMORY_SEGMENT << QLG_GPUVA_PTE_SEGMENT_SHIFT) | LOCAL_B));
    CHECK(table[TABLE_TEST_INDEX + 1] == (QLG_GPUVA_PTE_VALID | system_address(3)));
    args.UpdatePageTable.UpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    CHECK(build(&args, &buffer, &tickets) == STATUS_NOT_SUPPORTED && tickets == 0);

    /* Overlapping PTE copies preserve memmove ordering on the actual worker. */
    {
        DXGK_BUILDPAGINGBUFFER_COPY_RANGE range = {0};
        const QLG_GPUVA_PTE first = table[TABLE_TEST_INDEX];
        const QLG_GPUVA_PTE second = table[TABLE_TEST_INDEX + 1];

        RtlZeroMemory(&args, sizeof(args));
        range.SrcPageTableAddress = range.DstPageTableAddress = VA_TABLE_A;
        range.SrcStartPteIndex = TABLE_TEST_INDEX;
        range.DstStartPteIndex = TABLE_TEST_INDEX + 1;
        range.NumPageTableEntries = 2;
        args.Operation = DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES;
        args.CopyPageTableEntries.NumRanges = 1;
        args.CopyPageTableEntries.pRanges = &range;
        CHECK(build_and_execute(&args) == STATUS_SUCCESS);
        CHECK(table[TABLE_TEST_INDEX + 1] == first && table[TABLE_TEST_INDEX + 2] == second);
        args.CopyPageTableEntries.NumRanges = 2;
        CHECK(build(&args, &buffer, &tickets) == STATUS_NOT_SUPPORTED && tickets == 0);
    }

    /* An earlier relocation owns its pages, so a later update cannot overtake it. */
    args = transfer(NULL, 0, TWO_PAGES, VA_TABLE_A, VA_TABLE_B,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS);
    {
        DXGKARG_BUILDPAGINGBUFFER mutation = {0};
        FIXTURE_DMA other;
        UINT count;

        mutation.Operation = DXGK_OPERATION_UPDATE_PAGE_TABLE;
        mutation.UpdatePageTable.UpdateMode = DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
        mutation.UpdatePageTable.PageTableAddress.GpuPhysical.SegmentId = QLM_MEMORY_SEGMENT;
        mutation.UpdatePageTable.PageTableAddress.GpuPhysical.SegmentOffset = TABLE_LOCAL_A;
        mutation.UpdatePageTable.NumPageTableEntries = 1;
        mutation.UpdatePageTable.pPageTableEntries = entries;
        CHECK(build(&mutation, &other, &count) == STATUS_SUCCESS && count == 1);
        CHECK(QlgExecutePaging(&core.paging, &other.dma[0]) == STATUS_DEVICE_BUSY);
        CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_SUCCESS);
        CHECK(memcmp(source, destination, TWO_PAGES) == 0);
        CHECK(QlgExecutePaging(&core.paging, &other.dma[0]) == STATUS_SUCCESS);
    }

    /* A reset during a blocking MDL map must reject publication and release
     * both the newly mapped pages and the adapter reservation. */
    args = transfer(NULL, 0, TWO_PAGES, VA_SYSTEM, VA_TABLE_B,
                    DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    reset_during_map = TRUE;
    CHECK(build(&args, &buffer, &tickets) == STATUS_DEVICE_NOT_READY && tickets == 0);
    CHECK(mdl_maps == mdl_unmaps && !reset_during_map);
    CHECK(qlg_reset_quiesced(core.state) == STATUS_SUCCESS &&
          qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);

    /* Reset rejects execution/admission while the real reservation blocks restart. */
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS);
    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS && qlg_reset_quiesced(core.state) == STATUS_SUCCESS);
    CHECK(qlg_restart_ready(core.state) != STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_DEVICE_NOT_READY);
    CHECK(QlgCancelPaging(&core.paging, &buffer.dma[0], TRUE) == STATUS_SUCCESS);
    CHECK(mdl_maps == mdl_unmaps && qlg_restart_ready(core.state) == STATUS_SUCCESS);
    CHECK(build(&args, &buffer, &tickets) == STATUS_DEVICE_NOT_READY && tickets == 0);
    CHECK(qlg_restart(core.state) == STATUS_SUCCESS);

    /* Failed fence notification retains mappings/ranges; retry does not copy again. */
    completion.maps = mdl_maps;
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS);
    services.context = &completion;
    services.enqueue = implicit_enqueue;
    services.complete = implicit_complete;
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    command.Flags.Paging = 1;
    command.NodeOrdinal = QLG_RENDER_NODE;
    command.EngineOrdinal = QLG_RENDER_ENGINE;
    command.SubmissionFenceId = FIXTURE_IMPLICIT_FENCE;
    command.DmaBufferSize = sizeof(buffer.dma);
    command.DmaBufferSubmissionEndOffset = sizeof(QLG_PAGING_TICKET);
    command.pDmaBufferPrivateData = buffer.data;
    command.DmaBufferPrivateDataSize = sizeof(buffer.data);
    command.DmaBufferPrivateDataSubmissionEndOffset = sizeof(QLG_PAGING_TICKET);
    CHECK(QlgPagingSubmit(&submit, &command) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_UNSUCCESSFUL);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_SUCCESS && completion.reports == 2);
    CHECK(mdl_maps == mdl_unmaps);
}

/* WDDMv2 places and drops allocations with no copy the policy can see. */
static void check_residency_notifications(void)
{
    static DXGKARG_BUILDPAGINGBUFFER args;
    static FIXTURE_DMA buffer;
    UINT tickets;

    /* B was placed at LOCAL_C by VidMm: its eviction adopts the placement. */
    ((UINT *)(bar + LOCAL_C))[0] = 0x44444444;
    args = transfer(&allocation_b, 0, TWO_PAGES, VA_LOCAL_C, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build_and_execute(&args) == STATUS_SUCCESS && ((UINT *)system_page(3))[0] == 0x44444444);
    /* A placement on pages A owns is never adopted. */
    args = transfer(&allocation_b, 0, TWO_PAGES, VA_LOCAL_A, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    /* A is evicted without contents (notified), so its pages are free for B. */
    notify(&allocation_a, FALSE, 0, 0);
    notify(&allocation_b, TRUE, QLM_MEMORY_SEGMENT, LOCAL_A);
    args = transfer(&allocation_b, 0, TWO_PAGES, VA_LOCAL_A, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build_and_execute(&args) == STATUS_SUCCESS);
    /* A's old placement was forgotten: copying A from there now needs a fresh
     * notification (the transfer fallback cannot take B's pages). */
    notify(&allocation_b, TRUE, QLM_MEMORY_SEGMENT, LOCAL_A);
    args = transfer(&allocation_a, 0, TWO_PAGES, VA_LOCAL_A, VA_SYSTEM,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    notify(&allocation_b, TRUE, QLM_SYSTEM_SEGMENT, 0);
    CHECK(build_and_execute(&args) == STATUS_SUCCESS);
    /* Notifications for allocations without a driver handle are accepted. */
    notify(NULL, TRUE, QLM_MEMORY_SEGMENT, LOCAL_C);
}

/* W2: dxgkrnl maps each monitored fence's page into the process GPU VA (read
 * and write for signals from the paging process, read-only for waiters).
 * Those are system-memory leaves with cache and protection bits. */
static void check_fence_pages(void)
{
    DXGK_PTE entries[3];
    volatile QLG_GPUVA_PTE *leaf = (volatile QLG_GPUVA_PTE *)(bar + LEAF_OFFSET);

    entries[0] = pte(QLM_SYSTEM_SEGMENT, system_address(2));
    entries[0].CacheCoherent = 1;
    entries[1] = pte(QLM_MEMORY_SEGMENT, LOCAL_FENCE);
    entries[2] = entries[0];
    entries[2].ReadOnly = 1;
    entries[2].NoExecute = 1;
    CHECK(update_table(LEAF_OFFSET, 0, 20, entries, 3, FALSE) == STATUS_SUCCESS);
    CHECK(leaf[20] == (QLG_GPUVA_PTE_VALID | system_address(2)));
    CHECK(leaf[21] == (QLG_GPUVA_PTE_VALID | (QLM_MEMORY_SEGMENT << 2) | LOCAL_FENCE));
    CHECK(leaf[22] == (QLG_GPUVA_PTE_VALID | QLG_GPUVA_PTE_READ_ONLY | system_address(2)));
}

#if QLG_PAGING_SIGNALS
static void fence_reported(void *context)
{
    CHECK(context == &miniport);
    /* The value is stored before dxgkrnl is told. */
    local_fence_at_report = *(volatile UINT64 *)(bar + LOCAL_FENCE + FENCE_OFFSET);
    ++fence_reports;
}

static NTSTATUS build_signal(UINT64 address, UINT64 value, FIXTURE_DMA *buffer, UINT *tickets)
{
    DXGKARG_BUILDPAGINGBUFFER args = {0};
    args.Operation = DXGK_OPERATION_SIGNAL_MONITORED_FENCE;
    args.SignalMonitoredFence.MonitoredFenceGpuVa = address;
    args.SignalMonitoredFence.MonitoredFenceValue = value;
    return build(&args, buffer, tickets);
}

static void submit_enqueue(void *context, UINT64 identity)
{
    CHECK(context == &core && identity);
}

/* The batch's DMA fence follows the paging-fence report. */
static NTSTATUS submit_complete(void *context, UINT fence, BOOLEAN *delivered)
{
    CHECK(context == &core && fence == 77 && fence_reports == 1);
    ++completions;
    *delivered = TRUE;
    return STATUS_SUCCESS;
}

static void ordered_report(void *context)
{
    CHECK(context == &miniport && completions == 0);
    /* The copy before the signal has already landed. */
    fence_seen_at_report = ((volatile UINT *)(bar + LOCAL_B))[0];
    ++fence_reports;
}

static void check_paging_signal_admission(void)
{
    static FIXTURE_DMA buffer;
    UINT tickets, index;
    UINT maps = mdl_maps, blocks = data_blocks;

    for (index = 0; index < QLM_MAX_JOBS; ++index)
        CHECK(qlg_implicit_paging_acquire(core.state) == STATUS_SUCCESS);
    CHECK(QlgGrowPagingStorage(&core, 1));
    for (; index < QLM_MAX_PAGING_CAPACITY; ++index)
        CHECK(qlg_implicit_paging_acquire(core.state) == STATUS_SUCCESS);
    refuse_paging_growth = TRUE;
    CHECK(build_signal(VA_FENCE + FENCE_OFFSET, 3, &buffer, &tickets) == STATUS_NO_MEMORY &&
          tickets == 0 && mdl_maps == maps);
    refuse_paging_growth = FALSE;
    for (index = 0; index < QLM_MAX_PAGING_CAPACITY; ++index)
        CHECK(qlg_implicit_paging_release(core.state) == STATUS_SUCCESS);
    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS && qlg_reset_quiesced(core.state) == STATUS_SUCCESS);
    CHECK(QlgResetMemory(&core) == STATUS_SUCCESS && data_blocks == blocks);
    CHECK(qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);

    CHECK(build_signal(VA_FENCE + FENCE_OFFSET, 3, &buffer, &tickets) == STATUS_SUCCESS &&
          tickets == 1);
    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS &&
          qlg_reset_quiesced(core.state) == STATUS_SUCCESS);
    CHECK(qlg_restart_ready(core.state) == STATUS_DEVICE_BUSY);
    CHECK(QlgCancelPaging(&core.paging, &buffer.dma[0], TRUE) == STATUS_SUCCESS);
    CHECK(mdl_unmaps == mdl_maps && qlg_restart_ready(core.state) == STATUS_SUCCESS &&
          qlg_restart(core.state) == STATUS_SUCCESS);

    reset_during_map = TRUE;
    CHECK(build_signal(VA_FENCE + FENCE_OFFSET, 3, &buffer, &tickets) == STATUS_DEVICE_NOT_READY &&
          tickets == 0 && mdl_maps == maps + 2 && mdl_unmaps == mdl_maps);
    CHECK(qlg_reset_quiesced(core.state) == STATUS_SUCCESS &&
          qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);
}

/* W3 (2.2): paging-queue fence signals are tickets executed in stream order. */
static void check_paging_fence_signals(void)
{
    static FIXTURE_DMA buffer;
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    static DXGKARG_BUILDPAGINGBUFFER args;
    QLG_PAGING_SUBMIT_SERVICES services = {0};
    DXGKARG_SUBMITCOMMAND command = {0};
    volatile UINT64 *system_fence = (volatile UINT64 *)(system_page(2) + FENCE_OFFSET);
    volatile UINT64 *local_fence = (volatile UINT64 *)(bar + LOCAL_FENCE + FENCE_OFFSET);
    UINT tickets, maps;

    check_paging_signal_admission();
    maps = mdl_maps;
    core.fence_context = &miniport;
    core.fence_signaled = fence_reported;
    /* Built, not yet executed: nothing is written. */
    CHECK(build_signal(VA_FENCE + FENCE_OFFSET, 5, &buffer, &tickets) == STATUS_SUCCESS && tickets == 1);
    CHECK(*system_fence == 0 && fence_reports == 0);
    CHECK(QlgPagingFindLocked(&core.paging, &buffer.dma[0])->plan.request.operation ==
          QLM_FENCE_WRITE);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_SUCCESS);
    CHECK(*system_fence == 5 && fence_reports == 1 && mdl_maps == maps + 1 &&
          mdl_unmaps == mdl_maps);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_INVALID_PARAMETER);
    CHECK(build_signal(VA_FENCE_LOCAL + FENCE_OFFSET, 6, &buffer, &tickets) == STATUS_SUCCESS);
    CHECK(*local_fence == 0 && QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_SUCCESS);
    CHECK(*local_fence == 6 && fence_reports == 2 && local_fence_at_report == 6);
    /* A cancelled signal never writes. */
    CHECK(build_signal(VA_FENCE_LOCAL + FENCE_OFFSET, 9, &buffer, &tickets) == STATUS_SUCCESS);
    CHECK(QlgCancelPaging(&core.paging, &buffer.dma[0], FALSE) == STATUS_SUCCESS && *local_fence == 6);
    /* Malformed or unmapped addresses leave no ticket. */
    CHECK(build_signal(VA_FENCE + 4, 7, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    CHECK(build_signal(BASE_VA + 0x100000ull, 7, &buffer, &tickets) == STATUS_INVALID_PARAMETER &&
          tickets == 0);
    /* Stopped adapters admit no signal. */
    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS);
    CHECK(build_signal(VA_FENCE + FENCE_OFFSET, 7, &buffer, &tickets) == STATUS_DEVICE_NOT_READY &&
          tickets == 0 && mdl_maps == mdl_unmaps);
    CHECK(qlg_reset_quiesced(core.state) == STATUS_SUCCESS &&
          qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);

    /* One stream: page A in to LOCAL_B, then signal 7. Through the real
     * submission bridge, the signal is reported after the copy and before the
     * stream's DMA fence. */
    fence_reports = 0;
    core.fence_signaled = ordered_report;
    ((UINT *)system_page(3))[0] = 0x55555555;
    services.context = &core;
    services.enqueue = submit_enqueue;
    services.complete = submit_complete;
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    RtlZeroMemory(&buffer, sizeof(buffer));
    args = transfer(&allocation_a, 0, TWO_PAGES, VA_SYSTEM, VA_LOCAL_B,
                    DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    args.pDmaBuffer = buffer.dma;
    args.DmaSize = sizeof(buffer.dma);
    args.pDmaBufferPrivateData = buffer.data;
    args.DmaBufferPrivateDataSize = sizeof(buffer.data);
    args.hSystemContext = &system_context;
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_SUCCESS);
    args.Operation = DXGK_OPERATION_SIGNAL_MONITORED_FENCE;
    RtlZeroMemory(&args.SignalMonitoredFence, sizeof(args.SignalMonitoredFence));
    args.SignalMonitoredFence.MonitoredFenceGpuVa = VA_FENCE + FENCE_OFFSET;
    args.SignalMonitoredFence.MonitoredFenceValue = 7;
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_SUCCESS);
    CHECK((UCHAR *)args.pDmaBuffer == (UCHAR *)&buffer.dma[2]);
    CHECK(buffer.dma[0].epoch == buffer.dma[1].epoch &&
          buffer.dma[0].ticket < buffer.dma[1].ticket);
    command.Flags.Paging = 1;
    command.NodeOrdinal = QLG_RENDER_NODE;
    command.EngineOrdinal = QLG_RENDER_ENGINE;
    command.SubmissionFenceId = 77;
    command.DmaBufferSize = sizeof(buffer.dma);
    command.DmaBufferSubmissionEndOffset = 2 * sizeof(QLG_PAGING_TICKET);
    command.pDmaBufferPrivateData = buffer.data;
    command.DmaBufferPrivateDataSize = sizeof(buffer.data);
    command.DmaBufferPrivateDataSubmissionEndOffset = 2 * sizeof(QLG_PAGING_TICKET);
    CHECK(QlgPagingSubmit(&submit, &command) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_SUCCESS);
    CHECK(fence_seen_at_report == 0x55555555 && fence_reports == 1 && completions == 1);
    CHECK(*system_fence == 7 && mdl_maps == mdl_unmaps);
    core.fence_signaled = NULL;
}
#endif

/* Planned uploads are immutable, entry-granular views. The real serial worker
 * commits producers first; consumers retain epoch/ticket proofs, not job slots. */
typedef struct PLANNED_COMPLETION {
    UINT reports, expected_fence;
    BOOLEAN fail_once;
} PLANNED_COMPLETION;

static void planned_enqueue(void *context, UINT64 identity)
{
    CHECK(context && identity);
}

static NTSTATUS planned_complete(void *context, UINT fence, BOOLEAN *delivered)
{
    PLANNED_COMPLETION *completion = context;
    CHECK(fence == completion->expected_fence);
    ++completion->reports;
    if (completion->fail_once) {
        completion->fail_once = FALSE;
        *delivered = FALSE;
        return STATUS_UNSUCCESSFUL;
    }
    *delivered = TRUE;
    return STATUS_SUCCESS;
}

__declspec(noinline) static NTSTATUS planned_submit(QLG_PAGING_SUBMIT_CONTEXT *submit,
                                                     FIXTURE_DMA *buffer, UINT count, UINT fence)
{
    DXGKARG_SUBMITCOMMAND command = {0};
    command.Flags.Paging = 1;
    command.NodeOrdinal = QLG_RENDER_NODE;
    command.EngineOrdinal = QLG_RENDER_ENGINE;
    command.SubmissionFenceId = fence;
    command.DmaBufferSize = sizeof(buffer->dma);
    command.DmaBufferSubmissionEndOffset = count * sizeof(QLG_PAGING_TICKET);
    command.pDmaBufferPrivateData = buffer->data;
    command.DmaBufferPrivateDataSize = sizeof(buffer->data);
    command.DmaBufferPrivateDataSubmissionEndOffset = count * sizeof(QLG_PAGING_TICKET);
    return QlgPagingSubmit(submit, &command);
}

static void planned_upload(DXGKARG_BUILDPAGINGBUFFER *args, UINT index, DXGK_PTE *entry)
{
    RtlZeroMemory(args, sizeof(*args));
    args->Operation = DXGK_OPERATION_UPDATE_PAGE_TABLE;
    args->UpdatePageTable.UpdateMode = DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    args->UpdatePageTable.PageTableAddress.GpuPhysical.SegmentId = QLM_MEMORY_SEGMENT;
    args->UpdatePageTable.PageTableAddress.GpuPhysical.SegmentOffset = LEAF_OFFSET;
    args->UpdatePageTable.StartIndex = index;
    args->UpdatePageTable.NumPageTableEntries = 1;
    args->UpdatePageTable.pPageTableEntries = entry;
}

static void planned_transfer(DXGKARG_BUILDPAGINGBUFFER *args, UINT64 bytes, UINT64 source,
                               UINT64 destination, DXGK_MEMORY_TRANSFER_DIRECTION direction)
{
    RtlZeroMemory(args, sizeof(*args));
    args->Operation = DXGK_OPERATION_VIRTUAL_TRANSFER;
    args->TransferVirtual.TransferSizeInBytes = bytes;
    args->TransferVirtual.SourceVirtualAddress = source;
    args->TransferVirtual.DestinationVirtualAddress = destination;
    args->TransferVirtual.TransferDirection = direction;
}

static void check_planned_paging(void)
{
    static DXGKARG_BUILDPAGINGBUFFER args, next;
    static FIXTURE_DMA producer, consumer, scratch, reversed;
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    QLG_PAGING_SUBMIT_SERVICES services = {0};
    PLANNED_COMPLETION completion = {0};
    DXGK_PTE entries[3];
    volatile QLG_GPUVA_PTE *leaf = (volatile QLG_GPUVA_PTE *)(bar + LEAF_OFFSET);
    QLG_PAGING_JOB *job;
    QLG_PAGING_TICKET saved;
    UINT count, index;
    KIRQL level;
    const UINT64 planned_va = BASE_VA + 32 * QLG_GPUVA_PAGE_BYTES;
    const UINT64 three_pages = 3 * QLG_GPUVA_PAGE_BYTES;

    services.context = &completion;
    services.enqueue = planned_enqueue;
    services.complete = planned_complete;
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    /* A committed first leaf plus two separate partial uploads form a contiguous
     * source. Parent tables and destination leaves remain committed bytes. */
    entries[0] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_A);
    CHECK(update_table(LEAF_OFFSET, 0, 32, entries, 1, FALSE) == STATUS_SUCCESS);
    entries[0] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_B + TWO_PAGES);
    CHECK(update_table(LEAF_OFFSET, 0, 28, entries, 1, FALSE) == STATUS_SUCCESS);
    for (index = 0; index < three_pages / sizeof(UINT); ++index)
        ((UINT *)(bar + TABLE_LOCAL_A))[index] = 0x93000000u + index;
    memset((void *)(bar + TABLE_LOCAL_B), 0, (SIZE_T)three_pages);
    entries[0] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_A + QLG_GPUVA_PAGE_BYTES);
    planned_upload(&args, 33, &entries[0]);
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS && count == 1);
    entries[1] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_A + TWO_PAGES);
    planned_upload(&next, 34, &entries[1]);
    next.pDmaBuffer = args.pDmaBuffer;
    next.DmaSize = args.DmaSize;
    next.pDmaBufferPrivateData = args.pDmaBufferPrivateData;
    next.DmaBufferPrivateDataSize = args.DmaBufferPrivateDataSize;
    next.hSystemContext = &system_context;
    CHECK(QlgWddm2BuildPaging(&core, &next) == STATUS_SUCCESS);
    planned_transfer(&args, three_pages, planned_va, VA_TABLE_B,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    args.pDmaBuffer = next.pDmaBuffer;
    args.DmaSize = next.DmaSize;
    args.pDmaBufferPrivateData = next.pDmaBufferPrivateData;
    args.DmaBufferPrivateDataSize = next.DmaBufferPrivateDataSize;
    args.hSystemContext = &system_context;
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_SUCCESS);
    job = QlgPagingFindLocked(&core.paging, &producer.dma[2]);
    CHECK(job && job->dependencies.count == 2 && leaf[33] == 0 && leaf[34] == 0);
    CHECK(QlgExecutePaging(&core.paging, &producer.dma[2]) == STATUS_DEVICE_BUSY);
    CHECK(((UINT *)(bar + TABLE_LOCAL_B))[0] == 0);
    reversed = producer;
    reversed.data[0] = producer.data[2];
    reversed.data[1] = producer.data[0];
    reversed.data[2] = producer.data[1];
    CHECK(planned_submit(&submit, &reversed, 3, 91) == STATUS_INVALID_PARAMETER);
    /* Descriptor edits after Build cannot alter the captured view or upload. */
    RtlZeroMemory(entries, sizeof(entries));
    completion.expected_fence = 91;
    CHECK(planned_submit(&submit, &producer, 3, 91) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_SUCCESS && completion.reports == 1);
    CHECK(memcmp((void *)(bar + TABLE_LOCAL_A), (void *)(bar + TABLE_LOCAL_B),
                 (SIZE_T)three_pages) == 0);

    /* A finished producer awaiting notification still supplies a complete proof.
     * The consumer remains valid after retirement and immediate slot reuse. */
    entries[0] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_A);
    planned_upload(&args, 35, &entries[0]);
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    completion.expected_fence = 92;
    completion.fail_once = TRUE;
    CHECK(planned_submit(&submit, &producer, 1, 92) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 2) == STATUS_UNSUCCESSFUL);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, BASE_VA + 35 * QLG_GPUVA_PAGE_BYTES,
                     VA_TABLE_B, DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&args, &consumer, &count) == STATUS_SUCCESS);
    CHECK(QlgPagingFindLocked(&core.paging, &consumer.dma[0])->dependencies.count == 1);
    CHECK(QlgPagingExecuteBatch(&submit, 2) == STATUS_SUCCESS);
    saved = producer.dma[0];
    entries[0] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_A + QLG_GPUVA_PAGE_BYTES);
    planned_upload(&next, 36, &entries[0]);
    CHECK(build(&next, &scratch, &count) == STATUS_SUCCESS && scratch.dma[0].ticket != saved.ticket);
    CHECK(QlgCancelPaging(&core.paging, &scratch.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* Cancellation is remembered after producer slot reuse, at Submit AND at
     * execution; releasing the failed consumer allows the next reset epoch. */
    planned_upload(&args, 35, &entries[0]);
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    planned_transfer(&next, QLG_GPUVA_PAGE_BYTES, BASE_VA + 35 * QLG_GPUVA_PAGE_BYTES,
                     VA_TABLE_B, DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(QlgCancelPaging(&core.paging, &producer.dma[0], FALSE) == STATUS_SUCCESS);
    planned_upload(&args, 36, &entries[0]);
    CHECK(build(&args, &scratch, &count) == STATUS_SUCCESS);
    CHECK(QlgCancelPaging(&core.paging, &scratch.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &consumer, 1, 93) == STATUS_INVALID_PARAMETER);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_INVALID_PARAMETER);
    CHECK(QlgCancelPaging(&core.paging, &consumer.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* Requeue cannot move a producer behind an already submitted dependent
     * batch or change observable DMA fence order. No ticket is mutated. */
    planned_upload(&args, 35, &entries[0]);
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &consumer, 1, 95) == STATUS_INVALID_PARAMETER);
    CHECK(planned_submit(&submit, &producer, 1, 94) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &consumer, 1, 95) == STATUS_SUCCESS);
    level = core.paging.services.lock(core.paging.services.context);
    CHECK(QlgPagingRequeueLocked(&submit, 3) == STATUS_DEVICE_BUSY);
    CHECK(QlgPagingFindLocked(&core.paging, &producer.dma[0])->submission == 3);
    core.paging.services.unlock(core.paging.services.context, level);
    CHECK(QlgPagingExecuteBatch(&submit, 4) == STATUS_DEVICE_BUSY);
    completion.expected_fence = 94;
    CHECK(QlgPagingExecuteBatch(&submit, 3) == STATUS_SUCCESS);
    completion.expected_fence = 95;
    CHECK(QlgPagingExecuteBatch(&submit, 4) == STATUS_SUCCESS);

    /* An unsubmitted consumer can follow a resubmitted producer. It never
     * bypasses an older native batch or reports a fence before its producer. */
    planned_upload(&args, 35, &entries[0]);
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &producer, 1, 96) == STATUS_SUCCESS);
    level = core.paging.services.lock(core.paging.services.context);
    CHECK(QlgPagingRequeueLocked(&submit, 5) == STATUS_SUCCESS);
    CHECK(QlgPagingFindLocked(&core.paging, &producer.dma[0])->submission == 0);
    core.paging.services.unlock(core.paging.services.context, level);
    CHECK(planned_submit(&submit, &producer, 1, 96) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &consumer, 1, 97) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 7) == STATUS_DEVICE_BUSY);
    completion.expected_fence = 96;
    CHECK(QlgPagingExecuteBatch(&submit, 6) == STATUS_SUCCESS);
    completion.expected_fence = 97;
    CHECK(QlgPagingExecuteBatch(&submit, 7) == STATUS_SUCCESS);

    /* Build failure after a captured reference releases it. Reset cancellation
     * also releases all captured proofs, adapter reservations and mappings. */
    planned_upload(&args, 35, &entries[0]);
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    planned_transfer(&next, QLG_GPUVA_PAGE_BYTES, BASE_VA + 35 * QLG_GPUVA_PAGE_BYTES,
                     VA_TABLE_B, DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&next, &consumer, &count) != STATUS_SUCCESS && count == 0);
    planned_transfer(&next, QLG_GPUVA_PAGE_BYTES, BASE_VA + 35 * QLG_GPUVA_PAGE_BYTES,
                     VA_TABLE_B, DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    next.pDmaBuffer = consumer.dma;
    next.pDmaBufferPrivateData = consumer.data;
    next.DmaBufferPrivateDataSize = sizeof(consumer.data);
    next.hSystemContext = &system_context;
    CHECK(QlgWddm2BuildPaging(&core, &next) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    planned_transfer(&next, QLG_GPUVA_PAGE_BYTES, BASE_VA + 35 * QLG_GPUVA_PAGE_BYTES,
                     VA_SYSTEM, DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    reset_during_map = TRUE;
    CHECK(build(&next, &consumer, &count) == STATUS_DEVICE_NOT_READY && count == 0);
    CHECK(mdl_maps == mdl_unmaps && !reset_during_map);
    CHECK(QlgCancelPaging(&core.paging, &producer.dma[0], TRUE) == STATUS_SUCCESS);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    CHECK(qlg_reset_quiesced(core.state) == STATUS_SUCCESS &&
          qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);

    /* A queued zero fill projects invalid entries. It must not silently use
     * still-valid committed mappings, and failed walks release every capture. */
    RtlZeroMemory(&args, sizeof(args));
    args.Operation = DXGK_OPERATION_FILL;
    args.Fill.Destination.SegmentId = QLM_MEMORY_SEGMENT;
    args.Fill.Destination.SegmentAddress.QuadPart = LEAF_OFFSET;
    args.Fill.FillSize = QLG_GPUVA_PAGE_BYTES;
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    planned_transfer(&next, QLG_GPUVA_PAGE_BYTES, BASE_VA + 35 * QLG_GPUVA_PAGE_BYTES,
                     VA_TABLE_B, DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&next, &consumer, &count) == STATUS_INVALID_PARAMETER && count == 0);
    CHECK(QlgCancelPaging(&core.paging, &producer.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
}

/* Projected mutations use the exact bytes the real executor will write. */
#define MUTATION_FIRST_INDEX 40u
#define MUTATION_SOURCE_VA (BASE_VA + (MUTATION_FIRST_INDEX + 1) * QLG_GPUVA_PAGE_BYTES)
#define RELOCATION_SOURCE_INDEX 44u
#define RELOCATION_DESTINATION_INDEX 45u
#define RELOCATION_DATA 0x800000ull
#define MUTATION_FENCE 101u
#define FINISHED_MUTATION_FENCE 102u

static void physical_copy(DXGKARG_BUILDPAGINGBUFFER *args, UINT64 source, UINT64 destination,
                           SIZE_T bytes)
{
    RtlZeroMemory(args, sizeof(*args));
    args->Operation = DXGK_OPERATION_TRANSFER;
    args->Transfer.Source.SegmentId = args->Transfer.Destination.SegmentId = QLM_MEMORY_SEGMENT;
    args->Transfer.Source.SegmentAddress.QuadPart = source;
    args->Transfer.Destination.SegmentAddress.QuadPart = destination;
    args->Transfer.TransferSize = bytes;
}

static void check_planned_mutations(void)
{
    static DXGKARG_BUILDPAGINGBUFFER args, next;
    static FIXTURE_DMA producer, consumer, packet;
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    QLG_PAGING_SUBMIT_SERVICES services = {0};
    PLANNED_COMPLETION completion = {0};
    DXGK_BUILDPAGINGBUFFER_COPY_RANGE range = {0};
    DXGK_PTE entries[4];
    QLG_PAGING_JOB *job;
    UINT count, index;
    volatile QLG_GPUVA_PTE *leaf = (volatile QLG_GPUVA_PTE *)(bar + LEAF_OFFSET);
    const UINT64 three_pages = TWO_PAGES + QLG_GPUVA_PAGE_BYTES;

    services.context = &completion;
    services.enqueue = planned_enqueue;
    services.complete = planned_complete;
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    entries[0] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_A);
    entries[1] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_A + QLG_GPUVA_PAGE_BYTES);
    entries[2] = entries[0];
    entries[3] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_A + TWO_PAGES);
    CHECK(update_table(LEAF_OFFSET, 0, MUTATION_FIRST_INDEX, entries, 4, FALSE) == STATUS_SUCCESS);
    for (index = 0; index < three_pages / sizeof(UINT); ++index)
        ((UINT *)(bar + TABLE_LOCAL_A))[index] = FIXTURE_FILL ^ index;
    memset((void *)(bar + TABLE_LOCAL_B), 0, (SIZE_T)three_pages);

    /* Copy two overlapping entries. Its partial overwrite composes with the
     * third committed leaf. Later source edits cannot change the snapshot. */
    range.SrcPageTableAddress = range.DstPageTableAddress = VA_LEAF_TABLE;
    range.SrcStartPteIndex = MUTATION_FIRST_INDEX;
    range.DstStartPteIndex = MUTATION_FIRST_INDEX + 1;
    range.NumPageTableEntries = 2;
    args.Operation = DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES;
    args.CopyPageTableEntries.NumRanges = 1;
    args.CopyPageTableEntries.pRanges = &range;
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    job = QlgPagingFindLocked(&core.paging, &producer.dma[0]);
    CHECK(job && job->upload && ((const QLG_GPUVA_PTE *)job->upload)[0] == leaf[MUTATION_FIRST_INDEX]);
    leaf[MUTATION_FIRST_INDEX] = leaf[MUTATION_FIRST_INDEX + 1];
    planned_transfer(&next, three_pages, MUTATION_SOURCE_VA, VA_TABLE_B,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(QlgPagingFindLocked(&core.paging, &consumer.dma[0])->dependencies.count == 1);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_DEVICE_BUSY);
    packet = producer;
    packet.dma[1] = consumer.dma[0];
    packet.data[1] = consumer.data[0];
    completion.expected_fence = MUTATION_FENCE;
    CHECK(planned_submit(&submit, &packet, 2, MUTATION_FENCE) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_SUCCESS);
    CHECK(leaf[MUTATION_FIRST_INDEX + 1] != leaf[MUTATION_FIRST_INDEX] &&
          leaf[MUTATION_FIRST_INDEX + 2] == leaf[MUTATION_FIRST_INDEX]);
    CHECK(memcmp((const void *)(bar + TABLE_LOCAL_A), (const void *)(bar + TABLE_LOCAL_B),
                 (SIZE_T)three_pages) == 0);

    /* Exercise the production reader at EXECUTING deterministically: Rust starts
     * the actual retained job; a partial destination write must not leak through
     * the immutable planned view. Quiesced cancellation restores all ownership. */
    CHECK(update_table(LEAF_OFFSET, 0, MUTATION_FIRST_INDEX, entries, 4, FALSE) == STATUS_SUCCESS);
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    job = QlgPagingFindLocked(&core.paging, &producer.dma[0]);
    CHECK(qlm_range_start(core.memory_arena, &job->plan) == STATUS_SUCCESS);
    leaf[MUTATION_FIRST_INDEX + 1] = leaf[MUTATION_FIRST_INDEX + 2] = 0;
    planned_transfer(&next, TWO_PAGES, MUTATION_SOURCE_VA, VA_TABLE_B,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(QlgPagingFindLocked(&core.paging, &consumer.dma[0])->plan.request.source == TABLE_LOCAL_A);
    CHECK(QlgCancelPaging(&core.paging, &consumer.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(QlgCancelPaging(&core.paging, &producer.dma[0], TRUE) == STATUS_SUCCESS);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    CHECK(update_table(LEAF_OFFSET, 0, MUTATION_FIRST_INDEX, entries, 4, FALSE) == STATUS_SUCCESS);

    /* Finished-but-not-notified TABLE_COPY uses committed destination bytes and
     * a complete proof. Retirement then slot reuse cannot invalidate a consumer. */
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    completion.expected_fence = FINISHED_MUTATION_FENCE;
    completion.fail_once = TRUE;
    CHECK(planned_submit(&submit, &producer, 1, FINISHED_MUTATION_FENCE) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 2) == STATUS_UNSUCCESSFUL);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(QlgPagingFindLocked(&core.paging, &consumer.dma[0])->dependencies.count == 1);
    CHECK(QlgPagingExecuteBatch(&submit, 2) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);

    /* A local table relocation projects its earlier source state. A queued
     * source writer must wait for the reader and can cancel independently. */
    entries[0] = pte(QLM_MEMORY_SEGMENT, TABLE_LOCAL_B);
    entries[1] = pte(QLM_MEMORY_SEGMENT, RELOCATION_DATA);
    CHECK(update_table(LEAF_OFFSET, 0, RELOCATION_SOURCE_INDEX, entries, 2, FALSE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + TABLE_LOCAL_A), (const void *)(bar + LEAF_OFFSET),
                   (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    memset((void *)(bar + RELOCATION_DATA), 0, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    physical_copy(&args, TABLE_LOCAL_A, LEAF_OFFSET, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    RtlZeroMemory(&next, sizeof(next));
    next.Operation = DXGK_OPERATION_FILL;
    next.Fill.Destination.SegmentId = QLM_MEMORY_SEGMENT;
    next.Fill.Destination.SegmentAddress.QuadPart = TABLE_LOCAL_A;
    next.Fill.FillSize = (SIZE_T)QLG_GPUVA_PAGE_BYTES;
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgCancelPaging(&core.paging, &consumer.dma[0], FALSE) == STATUS_SUCCESS);
    planned_transfer(&next, QLG_GPUVA_PAGE_BYTES,
                     BASE_VA + RELOCATION_SOURCE_INDEX * QLG_GPUVA_PAGE_BYTES,
                     BASE_VA + RELOCATION_DESTINATION_INDEX * QLG_GPUVA_PAGE_BYTES,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(QlgPagingFindLocked(&core.paging, &consumer.dma[0])->dependencies.count == 1);
    CHECK(QlgExecutePaging(&core.paging, &producer.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);
    CHECK(memcmp((const void *)(bar + TABLE_LOCAL_B), (const void *)(bar + RELOCATION_DATA),
                 (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);

    /* A legacy mapping provider without physical identity keeps the exact
     * unfrozen-source barrier; it cannot opt into planned snapshots. */
    core.mdl.physical_address = NULL;
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, VA_SYSTEM, VA_LEAF_TABLE,
                     DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    CHECK(build(&next, &consumer, &count) == STATUS_NOT_SUPPORTED && count == 0);
    CHECK(QlgCancelPaging(&core.paging, &producer.dma[0], FALSE) == STATUS_SUCCESS);
    core.mdl.physical_address = mock_address;
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
}

#define CHAIN_SOURCE 0x900000ull
#define CHAIN_MIDDLE 0xa00000ull
#define CHAIN_DESTINATION 0xb00000ull
#define CHAIN_DATA_SOURCE 0xc00000ull
#define CHAIN_DATA_DESTINATION 0xd00000ull
#define CHAIN_SCRATCH 0xe00000ull
#define CHAIN_INDEX 48u
#define CHAIN_FENCE 103u
#define CHAIN_RETRY_FENCE 104u
#define CHAIN_CONSUMER_FENCE 105u

static void chain_fill(DXGKARG_BUILDPAGINGBUFFER *args, UINT64 destination)
{
    RtlZeroMemory(args, sizeof(*args));
    args->Operation = DXGK_OPERATION_FILL;
    args->Fill.Destination.SegmentId = QLM_MEMORY_SEGMENT;
    args->Fill.Destination.SegmentAddress.QuadPart = destination;
    args->Fill.FillSize = (SIZE_T)QLG_GPUVA_PAGE_BYTES;
    args->Fill.FillPattern = FIXTURE_FILL;
}

static void check_local_copy_chains(void)
{
    static DXGKARG_BUILDPAGINGBUFFER args, next;
    static FIXTURE_DMA first, second, consumer, packet, scratch;
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    QLG_PAGING_SUBMIT_SERVICES services = {0};
    PLANNED_COMPLETION completion = {0};
    DXGK_PTE entry;
    DXGK_BUILDPAGINGBUFFER_COPY_RANGE table_range = {0};
    QLG_GPUVA_PTE encoded;
    volatile QLG_GPUVA_PTE *leaf = (volatile QLG_GPUVA_PTE *)(bar + LEAF_OFFSET);
    QLG_GPUVA_PTE *source_table = (QLG_GPUVA_PTE *)(bar + CHAIN_SOURCE);
    QLG_PAGING_JOB *job;
    UINT count, index;
    KIRQL level;

    services.context = &completion;
    services.enqueue = planned_enqueue;
    services.complete = planned_complete;
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    RtlCopyMemory(source_table, (const void *)(bar + LEAF_OFFSET), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    entry = pte(QLM_MEMORY_SEGMENT, CHAIN_DATA_SOURCE);
    CHECK(QlgGpuVaEncode(&entry, TRUE, &source_table[CHAIN_INDEX]) == STATUS_SUCCESS);
    entry = pte(QLM_MEMORY_SEGMENT, CHAIN_DATA_DESTINATION);
    CHECK(QlgGpuVaEncode(&entry, TRUE, &source_table[CHAIN_INDEX + 1]) == STATUS_SUCCESS);
    entry = pte(QLM_MEMORY_SEGMENT, CHAIN_MIDDLE);
    CHECK(QlgGpuVaEncode(&entry, TRUE, &source_table[CHAIN_INDEX + 2]) == STATUS_SUCCESS);
    CHECK(leaf[CHAIN_INDEX] == 0 && leaf[CHAIN_INDEX + 1] == 0);
    memset((void *)(bar + CHAIN_MIDDLE), 0, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    memset((void *)(bar + CHAIN_DATA_DESTINATION), 0, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    for (index = 0; index < QLG_GPUVA_PAGE_BYTES / sizeof(UINT); ++index)
        ((UINT *)(bar + CHAIN_DATA_SOURCE))[index] = FIXTURE_FILL ^ index;

    /* Two queued local relocations are followed through the planned table view.
     * The second source intersects the first destination and owns its proof. */
    physical_copy(&args, CHAIN_SOURCE, CHAIN_MIDDLE, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    physical_copy(&args, CHAIN_MIDDLE, LEAF_OFFSET, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&args, &second, &count) == STATUS_SUCCESS);
    job = QlgPagingFindLocked(&core.paging, &second.dma[0]);
    CHECK(job && job->dependencies.count == 1 && job->dependencies.tickets[0] == first.dma[0].ticket);
    planned_transfer(&next, QLG_GPUVA_PAGE_BYTES, BASE_VA + CHAIN_INDEX * QLG_GPUVA_PAGE_BYTES,
                     BASE_VA + (CHAIN_INDEX + 1) * QLG_GPUVA_PAGE_BYTES,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(QlgPagingFindLocked(&core.paging, &consumer.dma[0])->dependencies.count == 2);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_DEVICE_BUSY);
    packet = consumer;
    packet.data[1] = first.data[0];
    packet.data[2] = second.data[0];
    CHECK(planned_submit(&submit, &packet, 3, CHAIN_FENCE) == STATUS_INVALID_PARAMETER);
    packet = first;
    packet.dma[1] = second.dma[0];
    packet.data[1] = second.data[0];
    packet.dma[2] = consumer.dma[0];
    packet.data[2] = consumer.data[0];
    completion.expected_fence = CHAIN_FENCE;
    CHECK(planned_submit(&submit, &packet, 3, CHAIN_FENCE) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_SUCCESS);
    CHECK(memcmp((const void *)(bar + CHAIN_DATA_SOURCE),
                 (const void *)(bar + CHAIN_DATA_DESTINATION), (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);

    /* A table snapshot composes queued writers before any source DMA executes.
     * The physical copy independently retains every partial source writer. */
    RtlCopyMemory((void *)(bar + CHAIN_MIDDLE), (const void *)(bar + LEAF_OFFSET),
                   (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    entry = pte(QLM_MEMORY_SEGMENT, CHAIN_DATA_SOURCE);
    planned_upload(&args, CHAIN_INDEX, &entry);
    args.UpdatePageTable.PageTableAddress.GpuPhysical.SegmentOffset = CHAIN_MIDDLE;
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    entry = pte(QLM_MEMORY_SEGMENT, CHAIN_DATA_DESTINATION);
    planned_upload(&args, CHAIN_INDEX + 1, &entry);
    args.UpdatePageTable.PageTableAddress.GpuPhysical.SegmentOffset = CHAIN_MIDDLE;
    CHECK(build(&args, &second, &count) == STATUS_SUCCESS);
    table_range.SrcPageTableAddress = BASE_VA + (CHAIN_INDEX + 2) * QLG_GPUVA_PAGE_BYTES;
    table_range.DstPageTableAddress = VA_LEAF_TABLE;
    table_range.SrcStartPteIndex = table_range.DstStartPteIndex = CHAIN_INDEX;
    table_range.NumPageTableEntries = 1;
    RtlZeroMemory(&next, sizeof(next));
    next.Operation = DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES;
    next.CopyPageTableEntries.NumRanges = 1;
    next.CopyPageTableEntries.pRanges = &table_range;
    CHECK(build(&next, &scratch, &count) == STATUS_SUCCESS && count == 1);
    job = QlgPagingFindLocked(&core.paging, &scratch.dma[0]);
    CHECK(job && job->dependencies.count == 1 &&
          job->dependencies.tickets[0] == first.dma[0].ticket);
    CHECK(QlgExecutePaging(&core.paging, &scratch.dma[0]) == STATUS_DEVICE_BUSY);
    /* A second destination snapshot queues behind the first. Cancelling it
     * releases its own proofs without invalidating the earlier source writer. */
    CHECK(build(&next, &packet, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &packet.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgCancelPaging(&core.paging, &packet.dma[0], FALSE) == STATUS_SUCCESS);
    physical_copy(&args, CHAIN_MIDDLE, CHAIN_DESTINATION, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&args, &consumer, &count) == STATUS_SUCCESS);
    CHECK(QlgPagingFindLocked(&core.paging, &consumer.dma[0])->dependencies.count == 2);
    /* A later destination writer also waits for the existing destination copy. */
    CHECK(build(&args, &packet, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &packet.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgCancelPaging(&core.paging, &packet.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &second.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &first.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &scratch.dma[0]) == STATUS_SUCCESS);
    CHECK(leaf[CHAIN_INDEX] ==
          ((QLG_GPUVA_PTE *)(bar + CHAIN_DESTINATION))[CHAIN_INDEX]);
    CHECK(QlgGpuVaEncode(&entry, TRUE, &encoded) == STATUS_SUCCESS);
    CHECK(((QLG_GPUVA_PTE *)(bar + CHAIN_DESTINATION))[CHAIN_INDEX + 1] == encoded);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* A later source writer is admitted with both reader dependencies. It
     * cannot execute after only one reader completes or change either copy. */
    chain_fill(&args, CHAIN_MIDDLE);
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    physical_copy(&next, CHAIN_MIDDLE, CHAIN_DESTINATION, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&next, &second, &count) == STATUS_SUCCESS);
    physical_copy(&next, CHAIN_MIDDLE, CHAIN_SCRATCH, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &first.dma[0]) == STATUS_SUCCESS);
    args.Fill.FillPattern = FIXTURE_FILL ^ FIXTURE_POISON;
    CHECK(build(&args, &scratch, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &scratch.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &second.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &scratch.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &scratch.dma[0]) == STATUS_SUCCESS);
    CHECK(((UINT *)(bar + CHAIN_DESTINATION))[0] == FIXTURE_FILL);
    CHECK(((UINT *)(bar + CHAIN_SCRATCH))[0] == FIXTURE_FILL);
    CHECK(((UINT *)(bar + CHAIN_MIDDLE))[0] == (FIXTURE_FILL ^ FIXTURE_POISON));
    args.Fill.FillPattern = FIXTURE_FILL;

    /* A queued dependent prevents producer requeue. Cancelling that producer
     * poisons its exact proof even if the freed job slot is immediately reused. */
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &first, 1, CHAIN_RETRY_FENCE) == STATUS_SUCCESS);
    level = core.paging.services.lock(core.paging.services.context);
    CHECK(QlgPagingRequeueLocked(&submit, 2) == STATUS_SUCCESS);
    core.paging.services.unlock(core.paging.services.context, level);
    CHECK(planned_submit(&submit, &first, 1, CHAIN_RETRY_FENCE) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &consumer, 1, CHAIN_CONSUMER_FENCE) == STATUS_SUCCESS);
    level = core.paging.services.lock(core.paging.services.context);
    CHECK(QlgPagingRequeueLocked(&submit, 3) == STATUS_DEVICE_BUSY);
    core.paging.services.unlock(core.paging.services.context, level);
    CHECK(QlgPagingCancelBatch(&submit, 3, TRUE) == STATUS_SUCCESS);
    chain_fill(&args, CHAIN_DESTINATION);
    CHECK(build(&args, &scratch, &count) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 4) == STATUS_INVALID_PARAMETER);
    CHECK(QlgCancelPaging(&core.paging, &scratch.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(QlgPagingCancelBatch(&submit, 4, TRUE) == STATUS_SUCCESS);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* A completed source writer awaiting DMA notification admits a reader, but
     * strict native batch order still prevents its fence from overtaking. */
    chain_fill(&args, CHAIN_MIDDLE);
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    completion.expected_fence = CHAIN_RETRY_FENCE;
    completion.fail_once = TRUE;
    CHECK(planned_submit(&submit, &first, 1, CHAIN_RETRY_FENCE) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 5) == STATUS_UNSUCCESSFUL);
    physical_copy(&next, CHAIN_MIDDLE, CHAIN_DESTINATION, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &consumer, 1, CHAIN_CONSUMER_FENCE) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 6) == STATUS_DEVICE_BUSY);
    CHECK(QlgPagingExecuteBatch(&submit, 5) == STATUS_SUCCESS);
    completion.expected_fence = CHAIN_CONSUMER_FENCE;
    CHECK(QlgPagingExecuteBatch(&submit, 6) == STATUS_SUCCESS);
    CHECK(((UINT *)(bar + CHAIN_DESTINATION))[0] == FIXTURE_FILL);

    /* The service-free path still does not invent immutable system contents. */
    core.mdl.physical_address = NULL;
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, VA_SYSTEM,
                     BASE_VA + (CHAIN_INDEX + 2) * QLG_GPUVA_PAGE_BYTES,
                     DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    physical_copy(&args, CHAIN_MIDDLE, LEAF_OFFSET, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&args, &second, &count) == STATUS_SUCCESS);
    planned_transfer(&next, QLG_GPUVA_PAGE_BYTES, BASE_VA + CHAIN_INDEX * QLG_GPUVA_PAGE_BYTES,
                     BASE_VA + (CHAIN_INDEX + 1) * QLG_GPUVA_PAGE_BYTES,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&next, &consumer, &count) == STATUS_NOT_SUPPORTED && count == 0);
    CHECK(QlgCancelPaging(&core.paging, &second.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(QlgCancelPaging(&core.paging, &first.dma[0], FALSE) == STATUS_SUCCESS);
    core.mdl.physical_address = mock_address;
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
}

/* The alias source maps system page 1 before page 3, the reverse of VA_SYSTEM.
 * A two-page older MDL's page ordinal therefore differs from the newer source. */
#define SYSTEM_ALIAS_INDEX 64u
#define SYSTEM_SOURCE_INDEX 66u
#define SYSTEM_DATA_INDEX 80u
#define SYSTEM_ALIAS_VA (BASE_VA + SYSTEM_ALIAS_INDEX * QLG_GPUVA_PAGE_BYTES)
#define SYSTEM_SOURCE_VA (BASE_VA + SYSTEM_SOURCE_INDEX * QLG_GPUVA_PAGE_BYTES)
#define SYSTEM_DATA_VA (BASE_VA + SYSTEM_DATA_INDEX * QLG_GPUVA_PAGE_BYTES)
#define SYSTEM_SOURCE_LOCAL 0x1100000ull
#define SYSTEM_TEMPLATE_LOCAL 0x1200000ull
#define SYSTEM_DATA_LOCAL 0x1300000ull
#define SYSTEM_DEST_LOCAL 0x1400000ull
#define SYSTEM_SOURCE_FENCE 109u
#define SYSTEM_QUERY_FAULT_COUNT 3u

static void system_layout(QLG_GPUVA_PTE *saved, QLG_GPUVA_PTE *encoded)
{
    DXGK_PTE entries[4];
    DXGK_PTE data_entry;
    UINT index;

    RtlCopyMemory(saved, (const void *)(bar + LEAF_OFFSET), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    entries[0] = pte(QLM_SYSTEM_SEGMENT, system_address(1));
    entries[1] = pte(QLM_SYSTEM_SEGMENT, system_address(3));
    entries[2] = pte(QLM_MEMORY_SEGMENT, SYSTEM_SOURCE_LOCAL);
    entries[3] = pte(QLM_MEMORY_SEGMENT, SYSTEM_SOURCE_LOCAL + QLG_GPUVA_PAGE_BYTES);
    CHECK(update_table(LEAF_OFFSET, 0, SYSTEM_ALIAS_INDEX, entries, 4, FALSE) == STATUS_SUCCESS);
    RtlCopyMemory(encoded, (const void *)(bar + LEAF_OFFSET), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    data_entry = pte(QLM_MEMORY_SEGMENT, SYSTEM_DATA_LOCAL);
    CHECK(QlgGpuVaEncode(&data_entry, TRUE, &encoded[SYSTEM_DATA_INDEX]) == STATUS_SUCCESS);
    data_entry = pte(QLM_MEMORY_SEGMENT, SYSTEM_DEST_LOCAL);
    CHECK(QlgGpuVaEncode(&data_entry, TRUE, &encoded[SYSTEM_DATA_INDEX + 1]) == STATUS_SUCCESS);
    for (index = 0; index < QLG_GPUVA_PAGE_BYTES / sizeof(UINT); ++index)
        ((UINT *)(bar + SYSTEM_DATA_LOCAL))[index] = FIXTURE_FILL ^ index;
    RtlZeroMemory((void *)(bar + SYSTEM_DEST_LOCAL), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
}

static void check_retained_pfn_prefix(void)
{
    typedef struct FIXTURE_PARTIAL_PREFIX {
        MDL descriptor;
        PFN_NUMBER pages[2];
    } FIXTURE_PARTIAL_PREFIX;
    FIXTURE_PARTIAL_PREFIX partial = {0};
    QLG_PLATFORM_MEMORY owner = {0};
    QLG_PLATFORM_MDL retained = {0};
    UINT64 address;

    CHECK(FIELD_OFFSET(FIXTURE_PARTIAL_PREFIX, pages) == sizeof(MDL));
    /* The retained view starts at PFN zero even if the original MDL begins
     * inside that page. Query offsets must not add the ByteOffset again. */
    partial.descriptor.ByteOffset = sizeof(QLG_GPUVA_PTE);
    partial.descriptor.ByteCount = (ULONG)QLG_GPUVA_PAGE_BYTES;
    partial.pages[0] = (PFN_NUMBER)(system_address(1) >> QLG_GPUVA_PAGE_SHIFT);
    partial.pages[1] = (PFN_NUMBER)(system_address(3) >> QLG_GPUVA_PAGE_SHIFT);
    retained.owner = &owner;
    retained.partial = &partial.descriptor;
    retained.bytes = TWO_PAGES;
    CHECK(QlgPlatformMdlAddress(&retained, 0, &address) == STATUS_SUCCESS &&
          address == system_address(1));
    CHECK(QlgPlatformMdlAddress(&retained, (SIZE_T)(QLG_GPUVA_PAGE_BYTES + sizeof(QLG_GPUVA_PTE)),
                                &address) == STATUS_SUCCESS &&
          address == system_address(3) + sizeof(QLG_GPUVA_PTE));
    CHECK(QlgPlatformMdlAddress(&retained, (SIZE_T)TWO_PAGES, &address) == STATUS_INVALID_PARAMETER);
}

static void check_system_snapshots(void)
{
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES], encoded[QLG_GPUVA_TABLE_ENTRIES];
    static FIXTURE_DMA first, second, upload, consumer, scratch;
    static DXGKARG_BUILDPAGINGBUFFER args, next;
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    QLG_PAGING_SUBMIT_SERVICES services = {0};
    PLANNED_COMPLETION completion = {0};
    QLG_PAGING_JOB *job;
    PMDL physical_mdl;
    UINT count, blocks;

    system_layout(saved, encoded);
    RtlCopyMemory(system_page(1), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_ALIAS_VA, VA_LEAF_TABLE,
                     DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &upload, &count) == STATUS_SUCCESS && count == 1);
    job = QlgPagingFindLocked(&core.paging, &upload.dma[0]);
    CHECK(job && job->upload && job->plan.request.operation == QLM_TABLE_UPLOAD);
    /* Locked PFNs do not freeze source bytes. This deliberate source edit must
     * change neither the projected PTEs nor the later real executor's payload. */
    RtlZeroMemory(system_page(1), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    planned_transfer(&next, QLG_GPUVA_PAGE_BYTES, SYSTEM_DATA_VA,
                     SYSTEM_DATA_VA + QLG_GPUVA_PAGE_BYTES, DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_SUCCESS);
    CHECK(memcmp((const void *)(bar + LEAF_OFFSET), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);
    CHECK(memcmp((const void *)(bar + SYSTEM_DATA_LOCAL),
                 (const void *)(bar + SYSTEM_DEST_LOCAL), (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* The SYSTEM source's first PFN aliases the older destination's second PFN.
     * Its bytes must include the older queued local writer, before either copy
     * executes. The immutable upload also breaks any mutable source cycle. */
    RtlCopyMemory((void *)(bar + SYSTEM_TEMPLATE_LOCAL), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    RtlZeroMemory((void *)(bar + SYSTEM_SOURCE_LOCAL), (SIZE_T)TWO_PAGES);
    RtlZeroMemory(system_page(1), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    physical_copy(&args, SYSTEM_TEMPLATE_LOCAL, SYSTEM_SOURCE_LOCAL + QLG_GPUVA_PAGE_BYTES,
                   (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    planned_transfer(&args, TWO_PAGES, SYSTEM_SOURCE_VA, VA_SYSTEM,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &second, &count) == STATUS_SUCCESS);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_ALIAS_VA, VA_LEAF_TABLE,
                     DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &upload, &count) == STATUS_SUCCESS);
    job = QlgPagingFindLocked(&core.paging, &upload.dma[0]);
    CHECK(job && job->dependencies.count == 2 &&
          memcmp(job->upload, encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(build(&next, &consumer, &count) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &second.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &first.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &second.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);
    CHECK(memcmp((const void *)(bar + LEAF_OFFSET), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(memcmp(system_page(1), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* Distinct MDLs write the same physical page with different source bytes.
     * System WAW prerequisites prohibit reversing the actual writes; projection
     * chooses the latest writer rather than whichever slot happens to be first. */
    RtlZeroMemory((void *)(bar + SYSTEM_SOURCE_LOCAL), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    RtlCopyMemory((void *)(bar + SYSTEM_SOURCE_LOCAL + QLG_GPUVA_PAGE_BYTES), encoded,
                   (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_SOURCE_VA, VA_SYSTEM,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_SOURCE_VA + QLG_GPUVA_PAGE_BYTES,
                     SYSTEM_ALIAS_VA + QLG_GPUVA_PAGE_BYTES, DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &second, &count) == STATUS_SUCCESS);
    job = QlgPagingFindLocked(&core.paging, &second.dma[0]);
    CHECK(job && job->dependencies.count == 1 &&
          job->dependencies.tickets[0] == first.dma[0].ticket);
    CHECK(QlgExecutePaging(&core.paging, &second.dma[0]) == STATUS_DEVICE_BUSY);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_ALIAS_VA + QLG_GPUVA_PAGE_BYTES,
                     VA_LEAF_TABLE, DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &upload, &count) == STATUS_SUCCESS);
    job = QlgPagingFindLocked(&core.paging, &upload.dma[0]);
    CHECK(job && job->dependencies.count == 1 &&
          job->dependencies.tickets[0] == second.dma[0].ticket &&
          memcmp(job->upload, encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(QlgExecutePaging(&core.paging, &first.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &second.dma[0]) == STATUS_SUCCESS);
    CHECK(memcmp(system_page(3), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_SUCCESS);
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* Finished-but-not-retired uses the retained destination mapping. The
     * fixture publishes bounce pages only at unmap, and a new source fill has
     * already destroyed the original local bytes when the snapshot is built. */
    services.context = &completion;
    services.enqueue = planned_enqueue;
    services.complete = planned_complete;
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + SYSTEM_SOURCE_LOCAL), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    RtlZeroMemory(system_page(3), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_SOURCE_VA, VA_SYSTEM,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    completion.expected_fence = SYSTEM_SOURCE_FENCE;
    completion.fail_once = TRUE;
    CHECK(planned_submit(&submit, &first, 1, SYSTEM_SOURCE_FENCE) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_UNSUCCESSFUL);
    CHECK(((const QLG_GPUVA_PTE *)system_page(3))[SYSTEM_DATA_INDEX] == 0);
    chain_fill(&args, SYSTEM_SOURCE_LOCAL);
    CHECK(build(&args, &scratch, &count) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &scratch.dma[0]) == STATUS_SUCCESS);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_ALIAS_VA + QLG_GPUVA_PAGE_BYTES,
                     VA_LEAF_TABLE, DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &upload, &count) == STATUS_SUCCESS);
    job = QlgPagingFindLocked(&core.paging, &upload.dma[0]);
    CHECK(job && job->dependencies.count == 1 &&
          memcmp(job->upload, encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_SUCCESS);
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* A failed later PTE read must release newly acquired producer references
     * and the mapped/source snapshot without changing a caller's DMA cursor. */
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_SOURCE_VA, VA_SYSTEM,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    blocks = data_blocks;
    system_query_fault = SYSTEM_QUERY_FAULT_COUNT;
    planned_transfer(&args, TWO_PAGES, VA_SYSTEM,
                     VA_TABLE_B, DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &upload, &count) == STATUS_INVALID_PARAMETER && count == 0);
    CHECK(system_query_fault == 0 && data_blocks == blocks);
    CHECK(QlgCancelPaging(&core.paging, &first.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* An earlier writer can exceed the snapshot bound. Its index is queried
     * once for the source page, even when the matching PFN is the last one. */
    {
        DXGK_PTE entries[3];
        const UINT oversized_index = SYSTEM_DATA_INDEX + 4;
        const UINT64 oversized_va = BASE_VA + oversized_index * QLG_GPUVA_PAGE_BYTES;
        const UINT64 oversized_bytes = TWO_PAGES + QLG_GPUVA_PAGE_BYTES;

        entries[0] = pte(QLM_MEMORY_SEGMENT, SYSTEM_SOURCE_LOCAL + TWO_PAGES);
        CHECK(update_table(LEAF_OFFSET, 0, SYSTEM_SOURCE_INDEX + 2, entries, 1, FALSE) == STATUS_SUCCESS);
        entries[0] = pte(QLM_SYSTEM_SEGMENT, system_address(3));
        entries[1] = pte(QLM_SYSTEM_SEGMENT, system_address(1));
        entries[2] = pte(QLM_SYSTEM_SEGMENT, system_address(0));
        CHECK(update_table(LEAF_OFFSET, 0, oversized_index, entries, 3, FALSE) == STATUS_SUCCESS);
        RtlCopyMemory((void *)(bar + SYSTEM_SOURCE_LOCAL + TWO_PAGES), encoded,
                      (SIZE_T)QLG_GPUVA_PAGE_BYTES);
        RtlZeroMemory(system_page(0), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
        planned_transfer(&args, oversized_bytes, SYSTEM_SOURCE_VA, oversized_va,
                         DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
        CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
        system_query_calls = 0;
        planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, oversized_va + TWO_PAGES, VA_LEAF_TABLE,
                         DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
        CHECK(build(&args, &upload, &count) == STATUS_SUCCESS && count == 1);
        job = QlgPagingFindLocked(&core.paging, &upload.dma[0]);
        CHECK(system_query_calls == 2 && job && job->dependencies.count == 1 &&
              memcmp(job->upload, encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
        CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_DEVICE_BUSY);
        CHECK(QlgExecutePaging(&core.paging, &first.dma[0]) == STATUS_SUCCESS);
        CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_SUCCESS);
        CHECK(memcmp(system_page(0), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
        CHECK(memcmp((const void *)(bar + LEAF_OFFSET), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
        CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    }

    /* The second write to one repeated PFN is authoritative for both the
     * frozen snapshot and the bytes read back after physical execution. */
    {
        DXGK_PTE duplicate[2];

        duplicate[0] = duplicate[1] = pte(QLM_SYSTEM_SEGMENT, system_address(3));
        CHECK(update_table(LEAF_OFFSET, 0, SYSTEM_ALIAS_INDEX, duplicate, 2, FALSE) == STATUS_SUCCESS);
    }
    RtlZeroMemory((void *)(bar + SYSTEM_SOURCE_LOCAL), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    RtlCopyMemory((void *)(bar + SYSTEM_SOURCE_LOCAL + QLG_GPUVA_PAGE_BYTES), encoded,
                  (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    RtlZeroMemory(system_page(3), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    planned_transfer(&args, TWO_PAGES, SYSTEM_SOURCE_VA, SYSTEM_ALIAS_VA,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &first, &count) == STATUS_SUCCESS);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_ALIAS_VA, VA_LEAF_TABLE,
                     DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &upload, &count) == STATUS_SUCCESS && count == 1);
    job = QlgPagingFindLocked(&core.paging, &upload.dma[0]);
    CHECK(job && job->dependencies.count == 1 &&
          memcmp(job->upload, encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(QlgExecutePaging(&core.paging, &first.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_SUCCESS);
    CHECK(memcmp(system_page(3), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(memcmp((const void *)(bar + LEAF_OFFSET), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* A physical writer without SYSTEM ordering metadata is still a barrier
     * when a newer tracked writer targets the same physical page. */
    physical_mdl = IoAllocateMdl(NULL, (ULONG)QLG_GPUVA_PAGE_BYTES, FALSE, FALSE, NULL);
    CHECK(physical_mdl);
    physical_mdl->MdlFlags |= MDL_PAGES_LOCKED;
    MmGetMdlPfnArray(physical_mdl)[0] = (PFN_NUMBER)(system_address(3) >> QLG_GPUVA_PAGE_SHIFT);
    physical_copy(&args, SYSTEM_SOURCE_LOCAL, 0, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    args.Transfer.Destination.SegmentId = QLM_SYSTEM_SEGMENT;
    args.Transfer.Destination.pMdl = physical_mdl;
    args.pDmaBuffer = first.dma;
    args.DmaSize = sizeof(first.dma);
    args.pDmaBufferPrivateData = first.data;
    args.DmaBufferPrivateDataSize = sizeof(first.data);
    CHECK(QlgBuildPagingBuffer(&core.paging, &args) == STATUS_SUCCESS &&
          args.pDmaBuffer == &first.dma[1]);
    IoFreeMdl(physical_mdl);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_SOURCE_VA + QLG_GPUVA_PAGE_BYTES,
                     SYSTEM_ALIAS_VA + QLG_GPUVA_PAGE_BYTES, DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &second, &count) == STATUS_SUCCESS);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_ALIAS_VA + QLG_GPUVA_PAGE_BYTES,
                     VA_LEAF_TABLE, DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &upload, &count) == STATUS_NOT_SUPPORTED && count == 0);
    CHECK(QlgCancelPaging(&core.paging, &second.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(QlgCancelPaging(&core.paging, &first.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
}

#if QLG_PAGING_SIGNALS
static void check_paging_fence_aliases(void)
{
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES], encoded[QLG_GPUVA_TABLE_ENTRIES];
    static FIXTURE_DMA local, copy, first, last, upload;
    const UINT64 local_value = 0xfedcba9876543211ull;
    const UINT64 first_value = 0x23456789abcdef01ull;
    const UINT64 last_value = 0xaabbccddeeff1122ull;
    const SIZE_T system_slot = FENCE_OFFSET + sizeof(UINT64);
    const SIZE_T untouched = system_slot + sizeof(UINT64);
    const UINT64 untouched_value = 0xa5a5a5a5a5a5a5a5ull;
    DXGKARG_BUILDPAGINGBUFFER args;
    UINT count;

    system_layout(saved, encoded);
    RtlZeroMemory((void *)(bar + SYSTEM_SOURCE_LOCAL), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    *(UINT64 *)(bar + SYSTEM_SOURCE_LOCAL + untouched) = untouched_value;
    RtlZeroMemory(system_page(1), (SIZE_T)QLG_GPUVA_PAGE_BYTES);

    CHECK(build_signal(SYSTEM_SOURCE_VA + FENCE_OFFSET, local_value, &local, &count) ==
          STATUS_SUCCESS && count == 1);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_SOURCE_VA, SYSTEM_ALIAS_VA,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &copy, &count) == STATUS_SUCCESS && count == 1);
    /* The two virtual addresses resolve to the same owned PFN. */
    CHECK(build_signal(VA_SYSTEM + QLG_GPUVA_PAGE_BYTES + system_slot, first_value, &first, &count) ==
          STATUS_SUCCESS && count == 1);
    CHECK(build_signal(SYSTEM_ALIAS_VA + system_slot, last_value, &last, &count) ==
          STATUS_SUCCESS && count == 1);
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_ALIAS_VA, SYSTEM_SOURCE_VA + QLG_GPUVA_PAGE_BYTES,
                     DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &upload, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &copy.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &first.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &last.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &local.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &copy.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &first.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &last.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_SUCCESS);
    CHECK(*(UINT64 *)(bar + SYSTEM_SOURCE_LOCAL + QLG_GPUVA_PAGE_BYTES + FENCE_OFFSET) == local_value);
    CHECK(*(UINT64 *)(bar + SYSTEM_SOURCE_LOCAL + QLG_GPUVA_PAGE_BYTES + system_slot) == last_value);
    CHECK(*(UINT64 *)(bar + SYSTEM_SOURCE_LOCAL + QLG_GPUVA_PAGE_BYTES + untouched) == untouched_value);

    /* A copied literal cannot satisfy a dependency on a cancelled store. */
    CHECK(build_signal(SYSTEM_ALIAS_VA + system_slot, first_value, &first, &count) ==
          STATUS_SUCCESS && count == 1);
    CHECK(build(&args, &upload, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgCancelPaging(&core.paging, &first.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_INVALID_PARAMETER);
    CHECK(QlgCancelPaging(&core.paging, &upload.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(*(UINT64 *)(bar + SYSTEM_SOURCE_LOCAL + QLG_GPUVA_PAGE_BYTES + system_slot) == last_value);
    CHECK(mdl_unmaps == mdl_maps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
}
#endif

static void check_allocated_system_writer(void)
{
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES], encoded[QLG_GPUVA_TABLE_ENTRIES];
    static FIXTURE_DMA download, upload;
    QLG_OBJECT allocation = {0};
    DXGKARG_BUILDPAGINGBUFFER write, read;
    const UCHAR pattern = 0xa3;
    UINT count;

    system_layout(saved, encoded);
    init_allocation(&allocation, QLG_GPUVA_PAGE_BYTES);
    notify(&allocation, TRUE, QLM_MEMORY_SEGMENT, SYSTEM_SOURCE_LOCAL);
    RtlFillMemory((void *)(bar + SYSTEM_SOURCE_LOCAL), (SIZE_T)QLG_GPUVA_PAGE_BYTES, pattern);
    RtlZeroMemory(system_page(1), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    write = transfer(&allocation, 0, QLG_GPUVA_PAGE_BYTES, SYSTEM_SOURCE_VA,
                     VA_SYSTEM + QLG_GPUVA_PAGE_BYTES, DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    planned_transfer(&read, QLG_GPUVA_PAGE_BYTES, SYSTEM_ALIAS_VA,
                     SYSTEM_SOURCE_VA + QLG_GPUVA_PAGE_BYTES, DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&write, &download, &count) == STATUS_SUCCESS && count == 1);
    CHECK(build(&read, &upload, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgCancelPaging(&core.paging, &download.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_INVALID_PARAMETER);
    CHECK(QlgCancelPaging(&core.paging, &upload.dma[0], FALSE) == STATUS_SUCCESS);

    CHECK(build(&write, &download, &count) == STATUS_SUCCESS && count == 1);
    CHECK(build(&read, &upload, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &download.dma[0]) == STATUS_SUCCESS);
    RtlZeroMemory(system_page(1), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_SUCCESS);
    for (SIZE_T byte = 0; byte < QLG_GPUVA_PAGE_BYTES; ++byte)
        CHECK(bar[SYSTEM_SOURCE_LOCAL + QLG_GPUVA_PAGE_BYTES + byte] == pattern);

    CHECK(qlm_allocation_drop(core.memory_arena, allocation.memory_state) == STATUS_SUCCESS);
    CHECK(qlg_allocation_drop(core.state, allocation.state) == STATUS_SUCCESS);
    free(allocation.memory_state);
    free(allocation.state);
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
}

static void check_allocated_local_writer(void)
{
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES], encoded[QLG_GPUVA_TABLE_ENTRIES];
    static FIXTURE_DMA writer, consumer;
    QLG_OBJECT allocation = {0};
    DXGKARG_BUILDPAGINGBUFFER write, read;
    UINT count;

    system_layout(saved, encoded);
    init_allocation(&allocation, QLG_GPUVA_PAGE_BYTES);
    notify(&allocation, TRUE, QLM_MEMORY_SEGMENT, SYSTEM_TEMPLATE_LOCAL);
    RtlCopyMemory((void *)(bar + SYSTEM_TEMPLATE_LOCAL), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    physical_copy(&write, SYSTEM_TEMPLATE_LOCAL, LEAF_OFFSET, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    write.Transfer.hAllocation = &allocation;
    write.Transfer.Flags.TransferStart = write.Transfer.Flags.TransferEnd = 1;
    planned_transfer(&read, QLG_GPUVA_PAGE_BYTES, SYSTEM_DATA_VA,
                     SYSTEM_DATA_VA + QLG_GPUVA_PAGE_BYTES, DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);

    /* The virtual addresses only resolve through the pending allocation copy. */
    CHECK(build(&write, &writer, &count) == STATUS_SUCCESS && count == 1);
    CHECK(build(&read, &consumer, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgCancelPaging(&core.paging, &writer.dma[0], FALSE) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_INVALID_PARAMETER);
    CHECK(QlgCancelPaging(&core.paging, &consumer.dma[0], FALSE) == STATUS_SUCCESS);

    CHECK(build(&write, &writer, &count) == STATUS_SUCCESS && count == 1);
    CHECK(build(&read, &consumer, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &writer.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);
    CHECK(memcmp((const void *)(bar + SYSTEM_DATA_LOCAL),
                 (const void *)(bar + SYSTEM_DEST_LOCAL), (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(qlm_allocation_drop(core.memory_arena, allocation.memory_state) == STATUS_SUCCESS);
    CHECK(qlg_allocation_drop(core.state, allocation.state) == STATUS_SUCCESS);
    free(allocation.memory_state);
    free(allocation.state);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
}

static void check_allocated_upload_snapshot(void)
{
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES], encoded[QLG_GPUVA_TABLE_ENTRIES];
    static FIXTURE_DMA writer, consumer;
    QLG_OBJECT allocation = {0};
    DXGKARG_BUILDPAGINGBUFFER write, read;
    UINT count;

    system_layout(saved, encoded);
    init_allocation(&allocation, QLG_GPUVA_PAGE_BYTES);
    RtlCopyMemory(system_page(1), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    write = transfer(&allocation, 0, QLG_GPUVA_PAGE_BYTES, SYSTEM_ALIAS_VA, VA_LEAF_TABLE,
                     DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    planned_transfer(&read, QLG_GPUVA_PAGE_BYTES, SYSTEM_DATA_VA,
                     SYSTEM_DATA_VA + QLG_GPUVA_PAGE_BYTES, DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&write, &writer, &count) == STATUS_SUCCESS && count == 1);
    /* Mutate the source before either the page walk or the worker sees it. */
    RtlZeroMemory(system_page(1), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&read, &consumer, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &writer.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);
    CHECK(memcmp((const void *)(bar + LEAF_OFFSET), encoded, (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(memcmp((const void *)(bar + SYSTEM_DATA_LOCAL),
                 (const void *)(bar + SYSTEM_DEST_LOCAL), (SIZE_T)QLG_GPUVA_PAGE_BYTES) == 0);
    CHECK(qlm_allocation_drop(core.memory_arena, allocation.memory_state) == STATUS_SUCCESS);
    CHECK(qlg_allocation_drop(core.state, allocation.state) == STATUS_SUCCESS);
    free(allocation.memory_state);
    free(allocation.state);
    CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
}

static void check_large_allocated_upload(void)
{
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES], encoded[QLG_GPUVA_TABLE_ENTRIES];
    static FIXTURE_DMA writer, consumer;
    const SIZE_T page_bytes = QLG_GPUVA_PAGE_BYTES;
    const SIZE_T upload_bytes = LARGE_UPLOAD_TEST_PAGES * page_bytes;
    const UCHAR second_value = 0x8c, third_value = 0xd9;
    const UINT initial_blocks = data_blocks;
    QLG_OBJECT allocation = {0};
    DXGKARG_BUILDPAGINGBUFFER write, read;
    PMDL mdl;
    UINT count;

    system_layout(saved, encoded);
    init_allocation(&allocation, upload_bytes);
    RtlCopyMemory(system_page(1), encoded, page_bytes);
    RtlFillMemory(system_page(3), page_bytes, second_value);
    RtlFillMemory(system_page(0), page_bytes, third_value);
    mdl = IoAllocateMdl(NULL, (ULONG)upload_bytes, FALSE, FALSE, NULL);
    CHECK(mdl);
    mdl->MdlFlags |= MDL_PAGES_LOCKED;
    MmGetMdlPfnArray(mdl)[0] = (PFN_NUMBER)(system_address(1) >> QLG_GPUVA_PAGE_SHIFT);
    MmGetMdlPfnArray(mdl)[1] = (PFN_NUMBER)(system_address(3) >> QLG_GPUVA_PAGE_SHIFT);
    MmGetMdlPfnArray(mdl)[2] = (PFN_NUMBER)(system_address(0) >> QLG_GPUVA_PAGE_SHIFT);
    physical_copy(&write, 0, LEAF_OFFSET, upload_bytes);
    write.Transfer.hAllocation = &allocation;
    write.Transfer.Flags.TransferStart = write.Transfer.Flags.TransferEnd = 1;
    write.Transfer.Source.SegmentId = QLM_SYSTEM_SEGMENT;
    write.Transfer.Source.pMdl = mdl;

    /* Failure in the second lock slice must unwind the whole attempt. */
    system_query_fault = LARGE_UPLOAD_TEST_PAGES;
    CHECK(build(&write, &writer, &count) == STATUS_INVALID_PARAMETER && count == 0);
    CHECK(!core.paging.large_upload_bytes && data_blocks == initial_blocks && mdl_maps == mdl_unmaps);
    upload_page_reads = 0;
    core.mdl.physical_address = observe_upload_address;
    CHECK(build(&write, &writer, &count) == STATUS_SUCCESS && count == 1);
    core.mdl.physical_address = mock_address;
    CHECK(upload_page_reads == LARGE_UPLOAD_TEST_PAGES &&
          upload_page_locks[0] == upload_page_locks[1] && upload_page_locks[2] > upload_page_locks[1]);
    CHECK(core.paging.large_upload_bytes == upload_bytes && data_blocks == initial_blocks + 1);
    IoFreeMdl(mdl);
    RtlZeroMemory(system_page(1), page_bytes);
    RtlZeroMemory(system_page(3), page_bytes);
    RtlZeroMemory(system_page(0), page_bytes);
    planned_transfer(&read, page_bytes, SYSTEM_DATA_VA, SYSTEM_DATA_VA + page_bytes,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&read, &consumer, &count) == STATUS_SUCCESS && count == 1);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &writer.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);
    CHECK(memcmp((const void *)(bar + LEAF_OFFSET), encoded, page_bytes) == 0);
    for (SIZE_T byte = 0; byte < page_bytes; ++byte) {
        CHECK(bar[LEAF_OFFSET + page_bytes + byte] == second_value);
        CHECK(bar[LEAF_OFFSET + TWO_PAGES + byte] == third_value);
    }
    CHECK(memcmp((const void *)(bar + SYSTEM_DATA_LOCAL),
                 (const void *)(bar + SYSTEM_DEST_LOCAL), page_bytes) == 0);
    CHECK(!core.paging.large_upload_bytes && data_blocks == initial_blocks && mdl_maps == mdl_unmaps);
    CHECK(qlm_allocation_drop(core.memory_arena, allocation.memory_state) == STATUS_SUCCESS);
    CHECK(qlg_allocation_drop(core.state, allocation.state) == STATUS_SUCCESS);
    free(allocation.memory_state);
    free(allocation.state);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, page_bytes);
}

static void check_physical_system_writers(void)
{
    enum { MDL_PREFIX_PAGES = 1, MDL_WRITE_PAGES = 2, MDL_PAGES = MDL_PREFIX_PAGES + MDL_WRITE_PAGES };
    const SIZE_T page_bytes = QLG_GPUVA_PAGE_BYTES;
    const UCHAR prefix_value = 0x37, first_value = 0x79, last_value = 0xe5;
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES], encoded[QLG_GPUVA_TABLE_ENTRIES];
    static FIXTURE_DMA physical, upload;
    DXGK_PTE destination = pte(QLM_MEMORY_SEGMENT, SYSTEM_DEST_LOCAL);
    DXGKARG_BUILDPAGINGBUFFER write, read;
    QLG_OBJECT allocation = {0};
    UINT count;
    PMDL mdl;

    system_layout(saved, encoded);
    CHECK(update_table(LEAF_OFFSET, 0, SYSTEM_DATA_INDEX, &destination, 1, FALSE) == STATUS_SUCCESS);
    init_allocation(&allocation, TWO_PAGES);
    for (UINT allocated = 0; allocated <= 1; ++allocated) {
        if (allocated)
            notify(&allocation, TRUE, QLM_MEMORY_SEGMENT, SYSTEM_SOURCE_LOCAL);
        RtlFillMemory((UCHAR *)(bar + SYSTEM_SOURCE_LOCAL), page_bytes, first_value);
        RtlFillMemory((UCHAR *)(bar + SYSTEM_SOURCE_LOCAL + page_bytes), page_bytes, last_value);
        RtlFillMemory(system_page(3), page_bytes, prefix_value);
        RtlZeroMemory(system_page(1), page_bytes);
        mdl = IoAllocateMdl(NULL, (ULONG)(MDL_PAGES * page_bytes), FALSE, FALSE, NULL);
        CHECK(mdl);
        mdl->MdlFlags |= MDL_PAGES_LOCKED;
        MmGetMdlPfnArray(mdl)[0] = (PFN_NUMBER)(system_address(3) >> QLG_GPUVA_PAGE_SHIFT);
        MmGetMdlPfnArray(mdl)[1] = (PFN_NUMBER)(system_address(1) >> QLG_GPUVA_PAGE_SHIFT);
        MmGetMdlPfnArray(mdl)[2] = MmGetMdlPfnArray(mdl)[1];
        physical_copy(&write, SYSTEM_SOURCE_LOCAL, 0, TWO_PAGES);
        write.Transfer.hAllocation = allocated ? &allocation : NULL;
        write.Transfer.Flags.TransferStart = write.Transfer.Flags.TransferEnd = 1;
        write.Transfer.Destination.SegmentId = QLM_SYSTEM_SEGMENT;
        write.Transfer.Destination.pMdl = mdl;
        write.Transfer.MdlOffset = MDL_PREFIX_PAGES;
        CHECK(build(&write, &physical, &count) == STATUS_SUCCESS && count == 1);
        IoFreeMdl(mdl);
        planned_transfer(&read, page_bytes, SYSTEM_ALIAS_VA, SYSTEM_DATA_VA,
                         DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
        CHECK(build(&read, &upload, &count) == STATUS_SUCCESS && count == 1);
        CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_DEVICE_BUSY);
        CHECK(QlgExecutePaging(&core.paging, &physical.dma[0]) == STATUS_SUCCESS);
        CHECK(QlgExecutePaging(&core.paging, &upload.dma[0]) == STATUS_SUCCESS);
        for (SIZE_T byte = 0; byte < page_bytes; ++byte) {
            CHECK(system_page(3)[byte] == prefix_value);
            CHECK(system_page(1)[byte] == last_value);
            CHECK(bar[SYSTEM_DEST_LOCAL + byte] == last_value);
        }
        CHECK(mdl_maps == mdl_unmaps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    }
    CHECK(qlm_allocation_drop(core.memory_arena, allocation.memory_state) == STATUS_SUCCESS);
    CHECK(qlg_allocation_drop(core.state, allocation.state) == STATUS_SUCCESS);
    free(allocation.memory_state);
    free(allocation.state);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, page_bytes);
}

static void check_multipass_system_transfer(void)
{
    enum {
        UPLOAD_SOURCE_INDEX = 96, UPLOAD_DESTINATION_INDEX = 100, UPLOAD_PAGES = 3,
        UPLOAD_FIRST_FENCE = 131, UPLOAD_SECOND_FENCE = 132
    };
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES];
    static FIXTURE_DMA buffer, resumed;
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    const UINT64 page_bytes = QLG_GPUVA_PAGE_BYTES;
    const UINT64 source_va = BASE_VA + UPLOAD_SOURCE_INDEX * page_bytes;
    const UINT64 destination_va = BASE_VA + UPLOAD_DESTINATION_INDEX * page_bytes;
    const UINT source_pages[UPLOAD_PAGES] = {1, 3, 0};
    const UINT64 destination_pages[UPLOAD_PAGES] = {
        SYSTEM_TEMPLATE_LOCAL, SYSTEM_TEMPLATE_LOCAL + QLG_GPUVA_PAGE_BYTES, SYSTEM_DATA_LOCAL,
    };
    const UCHAR pattern[UPLOAD_PAGES] = {0x74, 0xa9, 0xe2};
    DXGK_PTE source_ptes[UPLOAD_PAGES], destination_ptes[UPLOAD_PAGES] = {0};
    DXGKARG_BUILDPAGINGBUFFER args;
    PLANNED_COMPLETION completion = {0};
    QLG_PAGING_SUBMIT_SERVICES services = {
        .context = &completion,
        .enqueue = planned_enqueue,
        .complete = planned_complete,
    };
    UINT page, tickets;
    UINT before = mdl_maps;

    RtlCopyMemory(saved, (const void *)(bar + LEAF_OFFSET), (SIZE_T)page_bytes);
    for (page = 0; page < UPLOAD_PAGES; ++page) {
        source_ptes[page] = pte(QLM_SYSTEM_SEGMENT, system_address(source_pages[page]));
        RtlFillMemory(system_page(source_pages[page]), (SIZE_T)page_bytes, pattern[page]);
        RtlZeroMemory((void *)(bar + destination_pages[page]), (SIZE_T)page_bytes);
    }
    destination_ptes[0] = pte(QLM_MEMORY_SEGMENT, destination_pages[0]);
    destination_ptes[1] = pte(QLM_MEMORY_SEGMENT, destination_pages[1]);
    CHECK(update_table(LEAF_OFFSET, 0, UPLOAD_SOURCE_INDEX, source_ptes, UPLOAD_PAGES, FALSE) ==
          STATUS_SUCCESS);
    CHECK(update_table(LEAF_OFFSET, 0, UPLOAD_DESTINATION_INDEX, destination_ptes,
                       UPLOAD_PAGES, FALSE) == STATUS_SUCCESS);
    planned_transfer(&args, UPLOAD_PAGES * page_bytes, source_va, destination_va,
                     DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &buffer, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0 &&
          args.MultipassOffset == 0);
    CHECK(mdl_maps == before + 1 && mdl_unmaps == mdl_maps &&
          bar[destination_pages[0]] == 0 && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    destination_ptes[2] = pte(QLM_MEMORY_SEGMENT, destination_pages[2]);
    CHECK(update_table(LEAF_OFFSET, 0, UPLOAD_DESTINATION_INDEX, destination_ptes,
                       UPLOAD_PAGES, FALSE) == STATUS_SUCCESS);
    args.pDmaBuffer = buffer.dma;
    args.DmaSize = sizeof(buffer.dma[0]);
    args.pDmaBufferPrivateData = buffer.data;
    args.DmaBufferPrivateDataSize = sizeof(buffer.data[0]);
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    CHECK(args.MultipassOffset == TWO_PAGES && !args.DmaSize && !args.DmaBufferPrivateDataSize &&
          args.TransferVirtual.TransferSizeInBytes == UPLOAD_PAGES * page_bytes &&
          args.TransferVirtual.SourceVirtualAddress == source_va &&
          args.TransferVirtual.DestinationVirtualAddress == destination_va);
    CHECK(memcmp(&buffer.dma[0], &buffer.data[0], sizeof(buffer.dma[0])) == 0);
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &buffer, 1, UPLOAD_FIRST_FENCE) == STATUS_SUCCESS);
    /* Windows can build the next packet while the first is still queued. */
    CHECK(build(&args, &resumed, &tickets) == STATUS_SUCCESS && tickets == 1 &&
          args.MultipassOffset == 0);
    CHECK(planned_submit(&submit, &resumed, 1, UPLOAD_SECOND_FENCE) == STATUS_SUCCESS);
    for (page = 0; page < UPLOAD_PAGES; ++page)
        RtlZeroMemory(system_page(source_pages[page]), (SIZE_T)page_bytes);
    completion.expected_fence = UPLOAD_FIRST_FENCE;
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_SUCCESS);
    completion.expected_fence = UPLOAD_SECOND_FENCE;
    CHECK(QlgPagingExecuteBatch(&submit, 2) == STATUS_SUCCESS && completion.reports == 2);
    for (page = 0; page < UPLOAD_PAGES; ++page) {
        UINT64 byte;

        for (byte = 0; byte < page_bytes; ++byte)
            CHECK(bar[destination_pages[page] + byte] == pattern[page]);
    }
    CHECK(mdl_unmaps == mdl_maps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* This time the discontinuity is inside a single nominal snapshot. */
    destination_ptes[1] = destination_ptes[2];
    CHECK(update_table(LEAF_OFFSET, 0, UPLOAD_DESTINATION_INDEX, destination_ptes,
                       UPLOAD_PAGES, FALSE) == STATUS_SUCCESS);
    RtlFillMemory(system_page(source_pages[0]), (SIZE_T)page_bytes, pattern[1]);
    RtlFillMemory(system_page(source_pages[1]), (SIZE_T)page_bytes, pattern[0]);
    planned_transfer(&args, TWO_PAGES, source_va, destination_va,
                     DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS && tickets == 2 &&
          args.MultipassOffset == 0);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_SUCCESS &&
          QlgExecutePaging(&core.paging, &buffer.dma[1]) == STATUS_SUCCESS);
    CHECK(bar[destination_pages[0]] == pattern[1] && bar[destination_pages[2]] == pattern[0]);
    CHECK(mdl_unmaps == mdl_maps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* The same fragmented local layout also splits a download back to SYSTEM. */
    RtlZeroMemory(system_page(source_pages[0]), (SIZE_T)page_bytes);
    RtlZeroMemory(system_page(source_pages[1]), (SIZE_T)page_bytes);
    planned_transfer(&args, TWO_PAGES, destination_va, source_va,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    args.hSystemContext = &system_context;
    args.pDmaBuffer = buffer.dma;
    args.DmaSize = sizeof(buffer.dma[0]);
    args.pDmaBufferPrivateData = buffer.data;
    args.DmaBufferPrivateDataSize = sizeof(buffer.data[0]);
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
          args.MultipassOffset == page_bytes && !args.DmaSize && !args.DmaBufferPrivateDataSize);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_SUCCESS);
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS && tickets == 1 &&
          args.MultipassOffset == 0);
    CHECK(QlgExecutePaging(&core.paging, &buffer.dma[0]) == STATUS_SUCCESS);
    CHECK(system_page(source_pages[0])[0] == pattern[1] &&
          system_page(source_pages[1])[0] == pattern[0]);
    CHECK(mdl_unmaps == mdl_maps && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)page_bytes);
}

/* Return queued consumers before producers while retaining all exact tickets.
 * Resubmission restores forward native fence order without dropping source pins. */
static void check_fragmented_local_transfer(void)
{
    enum { SOURCE_INDEX = 112, DESTINATION_INDEX = 120, FRAGMENT_PAGES = 3 };
    const UINT64 page_bytes = QLG_GPUVA_PAGE_BYTES;
    const UINT64 source_va = BASE_VA + SOURCE_INDEX * page_bytes;
    const UINT64 destination_va = BASE_VA + DESTINATION_INDEX * page_bytes;
    const UINT64 source[] = {SYSTEM_SOURCE_LOCAL, SYSTEM_TEMPLATE_LOCAL, SYSTEM_DATA_LOCAL};
    const UINT64 destination[] = {SYSTEM_DEST_LOCAL, TABLE_LOCAL_A, TABLE_LOCAL_B};
    const UCHAR contents[] = {0x25, 0x72, 0xb1};
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES];
    static FIXTURE_DMA packet;
    DXGK_PTE source_ptes[FRAGMENT_PAGES], destination_ptes[FRAGMENT_PAGES];
    DXGKARG_BUILDPAGINGBUFFER args;
    QLG_OBJECT blocker = {0};
    UINT tickets;

    RtlCopyMemory(saved, (const void *)(bar + LEAF_OFFSET), (SIZE_T)page_bytes);
    for (UINT page = 0; page < FRAGMENT_PAGES; ++page) {
        source_ptes[page] = pte(QLM_MEMORY_SEGMENT, source[page]);
        destination_ptes[page] = pte(QLM_MEMORY_SEGMENT, destination[page]);
        RtlFillMemory((void *)(bar + source[page]), (SIZE_T)page_bytes, contents[page]);
        RtlZeroMemory((void *)(bar + destination[page]), (SIZE_T)page_bytes);
    }
    CHECK(update_table(LEAF_OFFSET, 0, SOURCE_INDEX, source_ptes, FRAGMENT_PAGES, FALSE) == STATUS_SUCCESS);
    CHECK(update_table(LEAF_OFFSET, 0, DESTINATION_INDEX, destination_ptes, FRAGMENT_PAGES, FALSE) ==
          STATUS_SUCCESS);
    planned_transfer(&args, FRAGMENT_PAGES * page_bytes, source_va, destination_va,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);

    /* A later write would clobber an earlier run's source. Reject the whole plan. */
    DXGK_PTE overlap = source_ptes[0];
    CHECK(update_table(LEAF_OFFSET, 0, DESTINATION_INDEX + 1, &overlap, 1, FALSE) == STATUS_SUCCESS);
    CHECK(build(&args, &packet, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    CHECK(update_table(LEAF_OFFSET, 0, DESTINATION_INDEX + 1, &destination_ptes[1], 1, FALSE) ==
          STATUS_SUCCESS);

    args.pDmaBuffer = packet.dma;
    args.DmaSize = sizeof(packet.dma[0]);
    args.pDmaBufferPrivateData = packet.data;
    args.DmaBufferPrivateDataSize = sizeof(packet.data[0]);
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
          args.MultipassOffset == 0 && args.pDmaBuffer == packet.dma &&
          args.pDmaBufferPrivateData == packet.data);

    /* The second run fails admission after the first has taken a lease.
     * The unsubmitted packet must release that lease and its proof. */
    init_allocation(&blocker, page_bytes);
    notify(&blocker, TRUE, QLM_MEMORY_SEGMENT, destination[1]);
    CHECK(build(&args, &packet, &tickets) == STATUS_GRAPHICS_ALLOCATION_BUSY && tickets == 0);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    CHECK(qlm_allocation_drop(core.memory_arena, blocker.memory_state) == STATUS_SUCCESS);
    CHECK(qlg_allocation_drop(core.state, blocker.state) == STATUS_SUCCESS);
    free(blocker.memory_state);
    free(blocker.state);

    CHECK(build(&args, &packet, &tickets) == STATUS_SUCCESS && tickets == FRAGMENT_PAGES);
    CHECK(QlgExecutePaging(&core.paging, &packet.dma[1]) == STATUS_DEVICE_BUSY);
    for (UINT page = 0; page < FRAGMENT_PAGES; ++page) {
        CHECK(QlgExecutePaging(&core.paging, &packet.dma[page]) == STATUS_SUCCESS);
        for (SIZE_T byte = 0; byte < page_bytes; ++byte)
            CHECK(bar[destination[page] + byte] == contents[page]);
    }
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)page_bytes);
}

static void check_fragmented_multipass(void)
{
    enum { SOURCE_INDEX = 160, DESTINATION_INDEX = 224,
           RUN_COUNT = QLG_MAX_PAGING_PACKET_JOBS * 2 + 1, PACKETS = 3 };
    static struct {
        QLG_PAGING_TICKET dma[QLG_MAX_PAGING_PACKET_JOBS];
        QLG_PAGING_TICKET data[QLG_MAX_PAGING_PACKET_JOBS];
    } packets[PACKETS];
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES];
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    const UINT64 source_base = 0x1700000ull, destination_base = 0x1800000ull;
    const UINT64 page_bytes = QLG_GPUVA_PAGE_BYTES;
    const UINT64 source_va = BASE_VA + SOURCE_INDEX * page_bytes;
    const UINT64 destination_va = BASE_VA + DESTINATION_INDEX * page_bytes;
    const UINT first_fence = 151;
    const UINT blocks = data_blocks;
    DXGK_PTE source[RUN_COUNT], destination[RUN_COUNT];
    DXGKARG_BUILDPAGINGBUFFER args, bad;
    PLANNED_COMPLETION completion = {0};
    QLG_PAGING_SUBMIT_SERVICES services = {
        .context = &completion, .enqueue = planned_enqueue, .complete = planned_complete,
    };
    UINT prior_progress = 0;

    RtlCopyMemory(saved, (const void *)(bar + LEAF_OFFSET), (SIZE_T)page_bytes);
    for (UINT run = 0; run < RUN_COUNT; ++run) {
        const UINT64 source_page = source_base + run * TWO_PAGES;
        const UINT64 destination_page = destination_base + run * TWO_PAGES;

        source[run] = pte(QLM_MEMORY_SEGMENT, source_page);
        destination[run] = pte(QLM_MEMORY_SEGMENT, destination_page);
        RtlFillMemory((void *)(bar + source_page), (SIZE_T)page_bytes, (UCHAR)(run + 1));
        RtlZeroMemory((void *)(bar + destination_page), (SIZE_T)page_bytes);
    }
    CHECK(update_table(LEAF_OFFSET, 0, SOURCE_INDEX, source, RUN_COUNT, FALSE) == STATUS_SUCCESS);
    CHECK(update_table(LEAF_OFFSET, 0, DESTINATION_INDEX, destination, RUN_COUNT, FALSE) == STATUS_SUCCESS);
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services,
                                    QLG_RENDER_NODE, QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    planned_transfer(&args, RUN_COUNT * page_bytes, source_va, destination_va,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    args.hSystemContext = &system_context;
    for (UINT packet = 0; packet < PACKETS; ++packet) {
        const BOOLEAN last = packet == PACKETS - 1;
        DXGKARG_SUBMITCOMMANDVIRTUAL virtual_args = {0};
        DXGKARG_SUBMITCOMMAND command;

        args.pDmaBuffer = packets[packet].dma;
        args.DmaSize = sizeof(packets[packet].dma);
        args.pDmaBufferPrivateData = packets[packet].data;
        args.DmaBufferPrivateDataSize = sizeof(packets[packet].data);
        if (packet) {
            bad = args;
            if (packet == 1)
                bad.TransferVirtual.DestinationVirtualAddress += page_bytes;
            else
                bad.MultipassOffset = prior_progress;
            CHECK(QlgWddm2BuildPaging(&core, &bad) == STATUS_INVALID_PARAMETER &&
                  bad.pDmaBuffer == packets[packet].dma);
        }
        const UINT before = args.MultipassOffset;
        CHECK(QlgWddm2BuildPaging(&core, &args) ==
              (last ? STATUS_SUCCESS : STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER));
        CHECK(last ? !args.MultipassOffset : args.MultipassOffset && args.MultipassOffset != before);
        if (packet == 1)
            prior_progress = before;
        CHECK(core.paging.fragmented_copies == NULL ? last : !last);
        CHECK(data_blocks == (last ? blocks : blocks + 1));
        virtual_args.Flags.Paging = 1;
        virtual_args.NodeOrdinal = QLG_RENDER_NODE;
        virtual_args.EngineOrdinal = QLG_RENDER_ENGINE;
        virtual_args.SubmissionFenceId = first_fence + packet;
        virtual_args.DmaBufferSize = sizeof(packets[packet].dma) - args.DmaSize;
        virtual_args.pDmaBufferPrivateData = packets[packet].data;
        virtual_args.DmaBufferPrivateDataSize = sizeof(packets[packet].data) - args.DmaBufferPrivateDataSize;
        CHECK(QlgWddm2TranslateSubmit(&virtual_args, &command) == STATUS_SUCCESS);
        CHECK(QlgPagingSubmit(&submit, &command) == STATUS_SUCCESS);
        if (!packet) {
            /* A resolver on the next call would walk these invalid endpoints. */
            RtlZeroMemory((void *)(bar + LEAF_OFFSET + SOURCE_INDEX * sizeof(QLG_GPUVA_PTE)),
                          sizeof(QLG_GPUVA_PTE) * RUN_COUNT);
        }
    }
    CHECK(QlgPagingExecuteBatch(&submit, 2) == STATUS_DEVICE_BUSY);
    for (UINT packet = 0; packet < PACKETS; ++packet) {
        completion.expected_fence = first_fence + packet;
        CHECK(QlgPagingExecuteBatch(&submit, packet + 1) == STATUS_SUCCESS);
    }
    CHECK(completion.reports == PACKETS);
    for (UINT run = 0; run < RUN_COUNT; ++run)
        for (SIZE_T byte = 0; byte < page_bytes; ++byte)
            CHECK(bar[destination_base + run * TWO_PAGES + byte] == (UCHAR)(run + 1));
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    CHECK(update_table(LEAF_OFFSET, 0, SOURCE_INDEX, source, RUN_COUNT, FALSE) == STATUS_SUCCESS);
    args.pDmaBuffer = packets[0].dma;
    args.DmaSize = sizeof(packets[0].dma);
    args.pDmaBufferPrivateData = packets[0].data;
    args.DmaBufferPrivateDataSize = sizeof(packets[0].data);
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
          args.MultipassOffset && data_blocks == blocks + 1);
    /* A valid resume with a cancelled first-packet producer is terminal.
     * The still-private tail must release every lease immediately. */
    CHECK(QlgCancelPaging(&core.paging, &packets[0].dma[QLG_MAX_PAGING_PACKET_JOBS - 1], FALSE) ==
          STATUS_SUCCESS);
    bad = args;
    bad.pDmaBuffer = packets[1].dma;
    bad.DmaSize = sizeof(packets[1].dma);
    bad.pDmaBufferPrivateData = packets[1].data;
    bad.DmaBufferPrivateDataSize = sizeof(packets[1].data);
    CHECK(QlgWddm2BuildPaging(&core, &bad) == STATUS_INVALID_PARAMETER &&
          bad.pDmaBuffer == packets[1].dma && bad.pDmaBufferPrivateData == packets[1].data);
    CHECK(!core.paging.fragmented_copies && data_blocks == blocks);
    for (UINT run = QLG_MAX_PAGING_PACKET_JOBS - 1; run > 0; --run)
        CHECK(QlgCancelPaging(&core.paging, &packets[0].dma[run - 1], FALSE) == STATUS_SUCCESS);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    args.MultipassOffset = 0;
    args.pDmaBuffer = packets[0].dma;
    args.DmaSize = sizeof(packets[0].dma);
    args.pDmaBufferPrivateData = packets[0].data;
    args.DmaBufferPrivateDataSize = sizeof(packets[0].data);
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
          args.MultipassOffset && data_blocks == blocks + 1);
    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS && qlg_reset_quiesced(core.state) == STATUS_SUCCESS);
    CHECK(QlgResetMemory(&core) == STATUS_SUCCESS && !core.paging.fragmented_copies && data_blocks == blocks);
    CHECK(qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);
    args.DmaSize = sizeof(packets[0].dma);
    args.DmaBufferPrivateDataSize = sizeof(packets[0].data);
    args.pDmaBuffer = packets[0].dma;
    args.pDmaBufferPrivateData = packets[0].data;
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_INVALID_PARAMETER);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)page_bytes);
}

static void check_sparse_fragmented_local_copy(void)
{
    enum { SOURCE_INDEX = 0, DESTINATION_INDEX = 256, OBSERVER_INDEX = 430,
           RUN_COUNT = QLM_MAX_JOBS * 2 + 2, ALIAS_FIRST = QLM_MAX_JOBS + 1,
           ALIAS_LAST = ALIAS_FIRST + 8, JOBS = 3 };
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES];
    static DXGK_PTE source[RUN_COUNT], destination[RUN_COUNT];
    const UINT64 source_base = 0x1900000ull, destination_base = 0x1b00000ull;
    const UINT64 observer_offset = 0x1d00000ull;
    const UINT64 page_bytes = QLG_GPUVA_PAGE_BYTES;
    const UINT64 source_va = BASE_VA + SOURCE_INDEX * page_bytes;
    const UINT64 destination_va = BASE_VA + DESTINATION_INDEX * page_bytes;
    const UINT64 source_tail = source_base + (RUN_COUNT - 1) * TWO_PAGES;
    const UINT64 alias_page = destination_base + ALIAS_FIRST * TWO_PAGES;
    DXGK_PTE observer = pte(QLM_MEMORY_SEGMENT, observer_offset);
    const UINT blocks = data_blocks;
    DXGKARG_BUILDPAGINGBUFFER args;
    FIXTURE_DMA producer, packet, consumer;
    UINT tickets;

    RtlCopyMemory(saved, (const void *)(bar + LEAF_OFFSET), (SIZE_T)page_bytes);
    for (UINT run = 0; run < RUN_COUNT; ++run) {
        const UINT64 source_page = source_base + run * TWO_PAGES;
        const UINT64 destination_page = destination_base + run * TWO_PAGES;

        source[run] = pte(QLM_MEMORY_SEGMENT, source_page);
        destination[run] = pte(QLM_MEMORY_SEGMENT, destination_page);
        RtlFillMemory((void *)(bar + source_page), (SIZE_T)page_bytes, (UCHAR)(run + 1));
        RtlZeroMemory((void *)(bar + destination_page), (SIZE_T)page_bytes);
    }
    destination[ALIAS_LAST] = destination[ALIAS_FIRST];
    CHECK(update_table(LEAF_OFFSET, 0, SOURCE_INDEX, source, RUN_COUNT, FALSE) == STATUS_SUCCESS);
    CHECK(update_table(LEAF_OFFSET, 0, DESTINATION_INDEX, destination, RUN_COUNT, FALSE) == STATUS_SUCCESS);
    CHECK(update_table(LEAF_OFFSET, 0, OBSERVER_INDEX, &observer, 1, FALSE) == STATUS_SUCCESS);

    RtlZeroMemory(&args, sizeof(args));
    args.Operation = DXGK_OPERATION_VIRTUAL_FILL;
    args.FillVirtual.FillSizeInBytes = page_bytes;
    args.FillVirtual.FillPattern = FIXTURE_FILL;
    args.FillVirtual.DestinationVirtualAddress = source_va + (RUN_COUNT - 1) * page_bytes;
    CHECK(build(&args, &producer, &tickets) == STATUS_SUCCESS && tickets == 1);

    planned_transfer(&args, RUN_COUNT * page_bytes, source_va, destination_va,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&args, &packet, &tickets) == STATUS_SUCCESS && tickets == JOBS);
    CHECK(data_blocks == blocks + JOBS);

    KIRQL level = core.paging.services.lock(core.paging.services.context);
    QLM_PROJECTION projection;
    const QLG_PAGING_JOB *job = QlgPagingFindLocked(&core.paging, &packet.dma[1]);

    CHECK(job && qlm_project_entry(core.memory_arena, &job->plan, alias_page, &projection) ==
          STATUS_SUCCESS && projection.kind == QLM_PROJECT_LOCAL &&
          projection.offset == source_base + ALIAS_LAST * TWO_PAGES);
    core.paging.services.unlock(core.paging.services.context, level);

    planned_transfer(&args, page_bytes, destination_va + ALIAS_FIRST * page_bytes,
                     BASE_VA + OBSERVER_INDEX * page_bytes, DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&args, &consumer, &tickets) == STATUS_SUCCESS && tickets == 1);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &packet.dma[2]) == STATUS_DEVICE_BUSY);
    for (UINT index = 0; index < JOBS - 1; ++index)
        CHECK(QlgExecutePaging(&core.paging, &packet.dma[index]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &packet.dma[2]) == STATUS_DEVICE_BUSY);
    CHECK(QlgExecutePaging(&core.paging, &producer.dma[0]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &packet.dma[2]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_SUCCESS);
    CHECK(data_blocks == blocks);
    for (UINT run = 0; run < RUN_COUNT; ++run) {
        const UINT index = run == ALIAS_FIRST ? ALIAS_LAST : run;
        const UINT64 actual = destination_base + run * TWO_PAGES;
        const UINT64 expected = source_base + index * TWO_PAGES;

        if (run == ALIAS_LAST)
            continue;
        for (SIZE_T byte = 0; byte < page_bytes; ++byte)
            CHECK(bar[actual + byte] == bar[expected + byte]);
    }
    for (SIZE_T byte = 0; byte < page_bytes; ++byte)
        CHECK(bar[observer_offset + byte] == bar[alias_page + byte]);
    CHECK(*(const volatile ULONG *)(bar + source_tail) == FIXTURE_FILL);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    DXGK_PTE collision = source[RUN_COUNT - 1];
    CHECK(update_table(LEAF_OFFSET, 0, DESTINATION_INDEX, &collision, 1, FALSE) == STATUS_SUCCESS);
    planned_transfer(&args, RUN_COUNT * page_bytes, source_va, destination_va,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&args, &packet, &tickets) == STATUS_INVALID_PARAMETER && tickets == 0);
    CHECK(data_blocks == blocks);
    CHECK(update_table(LEAF_OFFSET, 0, DESTINATION_INDEX, destination, 1, FALSE) == STATUS_SUCCESS);

    /* A two-run copy can still use the last slot when unrelated work holds
     * every other ticket. All of those older tickets retain their ownership. */
    QLG_PAGING_TICKET pending[QLM_MAX_JOBS - 1];
    const UINT64 pressure_base = observer_offset + page_bytes;

    for (UINT index = 0; index < QLM_MAX_JOBS - 1; ++index) {
        chain_fill(&args, pressure_base + index * page_bytes);
        CHECK(build(&args, &producer, &tickets) == STATUS_SUCCESS && tickets == 1);
        pending[index] = producer.dma[0];
    }
    planned_transfer(&args, 2 * page_bytes, source_va, destination_va,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(build(&args, &packet, &tickets) == STATUS_SUCCESS && tickets == 1);
    CHECK(QlgExecutePaging(&core.paging, &packet.dma[0]) == STATUS_SUCCESS);
    for (UINT index = QLM_MAX_JOBS - 1; index > 0; --index)
        CHECK(QlgCancelPaging(&core.paging, &pending[index - 1], FALSE) == STATUS_SUCCESS);
    CHECK(data_blocks == blocks && qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)page_bytes);
}

static void check_virtual_buffer_paging(void)
{
    enum { SOURCE_INDEX = 64, DESTINATION_INDEX = 128, SYSTEM_INDEX = 160,
           RUN_COUNT = QLG_MAX_PAGING_PACKET_JOBS + 1 };
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES];
    static DXGK_PTE source[RUN_COUNT], destination[RUN_COUNT];
    static struct {
        QLG_PAGING_TICKET dma[QLG_MAX_PAGING_PACKET_JOBS];
        QLG_PAGING_TICKET data[QLG_MAX_PAGING_PACKET_JOBS];
    } first;
    const UINT64 source_base = 0x2000000ull, destination_base = 0x2100000ull;
    const UINT64 page_bytes = QLG_GPUVA_PAGE_BYTES;
    const UINT64 source_va = BASE_VA + SOURCE_INDEX * page_bytes;
    const UINT64 destination_va = BASE_VA + DESTINATION_INDEX * page_bytes;
    const UINT64 system_va = BASE_VA + SYSTEM_INDEX * page_bytes;
    const UINT64 allocation_bytes = RUN_COUNT * page_bytes;
    QLG_ALLOCATION_DESC descriptions[] = {
        { .magic = QLG_PRIVATE_MAGIC, .version = QLG_PRIVATE_VERSION,
          .kind = QLG_RESOURCE_BUFFER, .bytes = allocation_bytes, .logical_bytes = allocation_bytes },
        { .magic = QLG_PRIVATE_MAGIC, .version = QLG_PRIVATE_VERSION,
          .kind = QLG_RESOURCE_RGBA8, .bytes = page_bytes, .logical_bytes = page_bytes,
          .width = 32, .height = 32, .pitch = 128 },
    };
    DXGK_ALLOCATIONINFO information[RTL_NUMBER_OF(descriptions)] = {0};
    DXGKARG_CREATEALLOCATION create = {0};
    DXGKARG_DESTROYALLOCATION destroy = {0};
    HANDLE handles[RTL_NUMBER_OF(descriptions)];
    DXGKARG_BUILDPAGINGBUFFER args;
    FIXTURE_DMA next;
    DXGK_PTE system[] = {
        pte(QLM_SYSTEM_SEGMENT, system_address(3)), pte(QLM_SYSTEM_SEGMENT, system_address(1)),
    };
    UINT tickets;

    create.NumAllocations = RTL_NUMBER_OF(information);
    create.pAllocationInfo = information;
    for (UINT index = 0; index < RTL_NUMBER_OF(information); ++index) {
        information[index].pPrivateDriverData = &descriptions[index];
        information[index].PrivateDriverDataSize = sizeof(descriptions[index]);
        information[index].FlagsWddm2.DXGK_ALLOC_RESERVED16 = 1;
        information[index].FlagsWddm2.DXGK_ALLOC_RESERVED17 = 1;
    }
    CHECK(QlgCreateAllocation(&core, &create) == STATUS_SUCCESS);
    for (UINT index = 0; index < RTL_NUMBER_OF(information); ++index)
        CHECK(information[index].FlagsWddm2.DXGK_ALLOC_RESERVED16 &&
              information[index].FlagsWddm2.DXGK_ALLOC_RESERVED17);
    CHECK(information[0].SupportedWriteSegmentSet ==
              (QLM_MEMORY_SEGMENT_MASK | QLM_APERTURE_SEGMENT_MASK) &&
          information[1].SupportedWriteSegmentSet == QLM_APERTURE_SEGMENT_MASK);
    CHECK(information[1].FlagsWddm2.AccessedPhysically &&
          information[1].FlagsWddm2.ExplicitResidencyNotification);
    CHECK(!information[0].FlagsWddm2.AccessedPhysically &&
          !information[0].FlagsWddm2.ExplicitResidencyNotification);
    for (UINT index = 0; index < RTL_NUMBER_OF(handles); ++index)
        handles[index] = information[index].hAllocation;
    destroy.NumAllocations = RTL_NUMBER_OF(handles);
    destroy.pAllocationList = handles;

    RtlCopyMemory(saved, (const void *)(bar + LEAF_OFFSET), (SIZE_T)page_bytes);
    for (UINT run = 0; run < RUN_COUNT; ++run) {
        const UINT64 src = source_base + run * TWO_PAGES;
        const UINT64 dst = destination_base + run * TWO_PAGES;

        source[run] = pte(QLM_MEMORY_SEGMENT, src);
        destination[run] = pte(QLM_MEMORY_SEGMENT, dst);
        RtlFillMemory((void *)(bar + src), (SIZE_T)page_bytes, (UCHAR)(run + 1));
        RtlZeroMemory((void *)(bar + dst), (SIZE_T)page_bytes);
    }
    CHECK(update_table(LEAF_OFFSET, 0, SOURCE_INDEX, source, RUN_COUNT, FALSE) == STATUS_SUCCESS);
    CHECK(update_table(LEAF_OFFSET, 0, DESTINATION_INDEX, destination, RUN_COUNT, FALSE) == STATUS_SUCCESS);
    CHECK(update_table(LEAF_OFFSET, 0, SYSTEM_INDEX, system, RTL_NUMBER_OF(system), FALSE) ==
          STATUS_SUCCESS);

    args = transfer(handles[0], 0, allocation_bytes, source_va, destination_va,
                    DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    args.hSystemContext = &system_context;
    args.pDmaBuffer = first.dma;
    args.DmaSize = sizeof(first.dma);
    args.pDmaBufferPrivateData = first.data;
    args.DmaBufferPrivateDataSize = sizeof(first.data);
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
          args.MultipassOffset && args.DmaSize == 0);
    CHECK(QlgDestroyAllocation(&core, &destroy) == STATUS_DEVICE_BUSY);
    CHECK(build(&args, &next, &tickets) == STATUS_SUCCESS && tickets == 1);
    CHECK(QlgExecutePaging(&core.paging, &next.dma[0]) == STATUS_DEVICE_BUSY);
    for (UINT run = 0; run < QLG_MAX_PAGING_PACKET_JOBS; ++run)
        CHECK(QlgExecutePaging(&core.paging, &first.dma[run]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &next.dma[0]) == STATUS_SUCCESS);
    for (UINT run = 0; run < RUN_COUNT; ++run)
        CHECK(bar[destination_base + run * TWO_PAGES + page_bytes - 1] == (UCHAR)(run + 1));

    RtlZeroMemory(&args, sizeof(args));
    args.Operation = DXGK_OPERATION_VIRTUAL_FILL;
    args.FillVirtual.hAllocation = handles[0];
    args.FillVirtual.AllocationOffsetInBytes = page_bytes;
    args.FillVirtual.FillSizeInBytes = TWO_PAGES;
    args.FillVirtual.FillPattern = FIXTURE_FILL;
    args.FillVirtual.DestinationVirtualAddress = destination_va + page_bytes;
    CHECK(build(&args, &next, &tickets) == STATUS_SUCCESS && tickets == 2);
    for (UINT run = 0; run < tickets; ++run)
        CHECK(QlgExecutePaging(&core.paging, &next.dma[run]) == STATUS_SUCCESS);
    CHECK(*(const volatile ULONG *)(bar + destination_base + TWO_PAGES) == FIXTURE_FILL &&
          *(const volatile ULONG *)(bar + destination_base + 2 * TWO_PAGES) == FIXTURE_FILL);
    CHECK(bar[destination_base] == 1);

    RtlFillMemory(system_page(3), (SIZE_T)page_bytes, 0x91);
    RtlFillMemory(system_page(1), (SIZE_T)page_bytes, 0x92);
    args = transfer(handles[0], 2 * page_bytes, TWO_PAGES, system_va,
                    destination_va + 2 * page_bytes, DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &next, &tickets) == STATUS_SUCCESS && tickets == 2);
    for (UINT run = 0; run < tickets; ++run)
        CHECK(QlgExecutePaging(&core.paging, &next.dma[run]) == STATUS_SUCCESS);
    RtlZeroMemory(system_page(3), (SIZE_T)page_bytes);
    RtlZeroMemory(system_page(1), (SIZE_T)page_bytes);
    args = transfer(handles[0], 2 * page_bytes, TWO_PAGES, destination_va + 2 * page_bytes,
                    system_va, DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM);
    CHECK(build(&args, &next, &tickets) == STATUS_SUCCESS && tickets == 2);
    for (UINT run = 0; run < tickets; ++run)
        CHECK(QlgExecutePaging(&core.paging, &next.dma[run]) == STATUS_SUCCESS);
    CHECK(system_page(3)[page_bytes - 1] == 0x91 && system_page(1)[page_bytes - 1] == 0x92);
    RtlZeroMemory(&args, sizeof(args));
    args.Operation = DXGK_OPERATION_VIRTUAL_FILL;
    args.FillVirtual.hAllocation = handles[0];
    args.FillVirtual.FillSizeInBytes = allocation_bytes;
    args.FillVirtual.FillPattern = FIXTURE_FILL;
    args.FillVirtual.DestinationVirtualAddress = destination_va;
    args.hSystemContext = &system_context;
    args.pDmaBuffer = first.dma;
    args.DmaSize = sizeof(first.dma);
    args.pDmaBufferPrivateData = first.data;
    args.DmaBufferPrivateDataSize = sizeof(first.data);
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
          args.MultipassOffset && args.DmaSize == 0);
    CHECK(build(&args, &next, &tickets) == STATUS_SUCCESS && tickets == 1);
    for (UINT run = 0; run < QLG_MAX_PAGING_PACKET_JOBS; ++run)
        CHECK(QlgExecutePaging(&core.paging, &first.dma[run]) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &next.dma[0]) == STATUS_SUCCESS);
    CHECK(*(const volatile ULONG *)(bar + destination_base + (RUN_COUNT - 1) * TWO_PAGES) ==
          FIXTURE_FILL);
    CHECK(QlgDestroyAllocation(&core, &destroy) == STATUS_SUCCESS);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)page_bytes);
}

static QLG_PAGING_SUBMIT_CONTEXT *retiring_paging;
static UINT retirement_prepares, retirement_signals, retirement_waits;

static void prepare_paging_retirement(void *context)
{
    CHECK(context == &core && !core.paging_wait_armed);
    ++retirement_prepares;
}

static void signal_paging_retirement(void *context)
{
    CHECK(context == &core && core.paging_wait_armed);
    ++retirement_signals;
}

static void wait_paging_retirement(void *context)
{
    CHECK(context == &core && core.paging_wait_armed);
    ++retirement_waits;
    if (retiring_paging)
        CHECK(QlgPagingExecuteBatch(retiring_paging, 1) == STATUS_SUCCESS);
}

static void check_paging_packet_headroom(void)
{
    typedef struct PACKET {
        QLG_PAGING_TICKET dma[QLG_MAX_PAGING_PACKET_JOBS];
        QLG_PAGING_TICKET private_data[QLG_MAX_PAGING_PACKET_JOBS];
    } PACKET;
    enum { EXPANDED_PACKETS = QLM_MAX_PAGING_CAPACITY / QLG_MAX_PAGING_PACKET_JOBS };
    static PACKET packets[EXPANDED_PACKETS + 1];
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    const UINT first_fence = 140;
    const UINT blocks = data_blocks;
    const UINT64 first_offset = 0x1500000ull;
    PLANNED_COMPLETION completion = {0};
    QLG_PAGING_SUBMIT_SERVICES services = {
        .context = &completion,
        .enqueue = planned_enqueue,
        .complete = planned_complete,
    };
    DXGKARG_BUILDPAGINGBUFFER build_args;
    DXGKARG_SUBMITCOMMANDVIRTUAL virtual_args = {0};
    DXGKARG_SUBMITCOMMAND command;

    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    virtual_args.Flags.Paging = 1;
    virtual_args.NodeOrdinal = QLG_RENDER_NODE;
    virtual_args.EngineOrdinal = QLG_RENDER_ENGINE;
    virtual_args.DmaBufferPrivateDataSize = QLG_PAGING_PACKET_BYTES;
    for (UINT packet = 0; packet < EXPANDED_PACKETS; ++packet) {
        chain_fill(&build_args, first_offset + packet * QLG_MAX_PAGING_PACKET_JOBS * QLG_GPUVA_PAGE_BYTES);
        build_args.pDmaBuffer = packets[packet].dma;
        build_args.DmaSize = sizeof(packets[packet].dma);
        build_args.pDmaBufferPrivateData = packets[packet].private_data;
        build_args.DmaBufferPrivateDataSize = sizeof(packets[packet].private_data);
        for (UINT job = 0; job < QLG_MAX_PAGING_PACKET_JOBS; ++job) {
            CHECK(QlgWddm2BuildPaging(&core, &build_args) == STATUS_SUCCESS);
            build_args.Fill.Destination.SegmentAddress.QuadPart += QLG_GPUVA_PAGE_BYTES;
        }
        const BOOLEAN expanded = packet >= QLG_PAGING_PACKET_BUDGET;

        CHECK(core.paging.job_count == (expanded ? QLM_MAX_PAGING_CAPACITY : QLM_MAX_JOBS));
        CHECK(data_blocks == blocks + (expanded ? 1u : 0u));
        CHECK(!build_args.DmaSize && !build_args.DmaBufferPrivateDataSize &&
              QlgWddm2BuildPaging(&core, &build_args) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
        virtual_args.SubmissionFenceId = first_fence + packet;
        virtual_args.DmaBufferSize = QLG_PAGING_PACKET_BYTES;
        virtual_args.pDmaBufferPrivateData = packets[packet].private_data;
        CHECK(QlgWddm2TranslateSubmit(&virtual_args, &command) == STATUS_SUCCESS);
        CHECK(QlgPagingSubmit(&submit, &command) == STATUS_SUCCESS);
    }
    const UINT overflow = EXPANDED_PACKETS;
    const UINT64 copy_bytes = QLM_MAX_PAGING_CAPACITY * QLG_GPUVA_PAGE_BYTES;
    const UINT64 retry_offset = first_offset + copy_bytes;
    physical_copy(&build_args, first_offset, retry_offset, (SIZE_T)copy_bytes);
    build_args.pDmaBuffer = packets[overflow].dma;
    build_args.DmaSize = sizeof(packets[overflow].dma);
    build_args.pDmaBufferPrivateData = packets[overflow].private_data;
    build_args.DmaBufferPrivateDataSize = sizeof(packets[overflow].private_data);
    refuse_paging_growth = TRUE;
    CHECK(QlgWddm2BuildPaging(&core, &build_args) == STATUS_NO_MEMORY &&
          build_args.pDmaBuffer == packets[overflow].dma &&
          build_args.pDmaBufferPrivateData == packets[overflow].private_data);
    CHECK(QlgPagingExecuteBatch(&submit, 2) == STATUS_DEVICE_BUSY);
    core.services.prepare_paging_wait = prepare_paging_retirement;
    core.services.signal_paging_retired = signal_paging_retirement;
    core.services.wait_paging_retirement = wait_paging_retirement;
    CHECK(QlgWddm2BuildPaging(&core, &build_args) == STATUS_NO_MEMORY &&
          build_args.pDmaBuffer == packets[overflow].dma && retirement_signals == 0);
    refuse_paging_growth = FALSE;
    completion.expected_fence = first_fence;
    retiring_paging = &submit;
    CHECK(QlgWddm2BuildPaging(&core, &build_args) == STATUS_SUCCESS &&
          retirement_prepares == 2 && retirement_waits == 2 && retirement_signals != 0);
    retiring_paging = NULL;
    core.services.prepare_paging_wait = NULL;
    core.services.signal_paging_retired = NULL;
    core.services.wait_paging_retirement = NULL;
    virtual_args.SubmissionFenceId = first_fence + overflow;
    virtual_args.DmaBufferSize = sizeof(QLG_PAGING_TICKET);
    virtual_args.pDmaBufferPrivateData = packets[overflow].private_data;
    CHECK(QlgWddm2TranslateSubmit(&virtual_args, &command) == STATUS_SUCCESS);
    CHECK(QlgPagingSubmit(&submit, &command) == STATUS_SUCCESS);
    for (UINT packet = 1; packet <= overflow; ++packet) {
        completion.expected_fence = first_fence + packet;
        CHECK(QlgPagingExecuteBatch(&submit, packet + 1) == STATUS_SUCCESS);
    }
    CHECK(completion.reports == overflow + 1 &&
          *(volatile UINT *)(bar + retry_offset) == FIXTURE_FILL &&
          *(volatile UINT *)(bar + retry_offset + copy_bytes - sizeof(UINT)) == FIXTURE_FILL &&
          qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);

    /* Reset owns the overflow slab just like the initial job storage. */
    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS && qlg_reset_quiesced(core.state) == STATUS_SUCCESS);
    CHECK(QlgResetMemory(&core) == STATUS_SUCCESS);
    CHECK(data_blocks == blocks && !core.paging.overflow && core.paging.job_count == QLM_MAX_JOBS);
    CHECK(qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);
}

static void check_full_pool_flush(void)
{
    static QLG_PAGING_TICKET staged[QLM_MAX_PAGING_CAPACITY - 1];
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    FIXTURE_DMA scratch, packet, fresh;
    DXGKARG_BUILDPAGINGBUFFER args;
    const UINT64 base = 0x1e00000ull;
    const UINT64 page = QLG_GPUVA_PAGE_BYTES;
    const UINT64 target = base + QLM_MAX_PAGING_CAPACITY * page;
    const UINT fence = 191, blocks = data_blocks;
    PLANNED_COMPLETION completion = {0};
    QLG_PAGING_SUBMIT_SERVICES services = {
        .context = &completion, .enqueue = planned_enqueue, .complete = planned_complete,
    };
    UINT tickets;

    for (UINT index = 0; index < RTL_NUMBER_OF(staged); ++index) {
        chain_fill(&args, base + index * page);
        CHECK(build(&args, &scratch, &tickets) == STATUS_SUCCESS && tickets == 1);
        staged[index] = scratch.dma[0];
    }
    chain_fill(&args, target);
    CHECK(build(&args, &packet, &tickets) == STATUS_SUCCESS && tickets == 1);
    args.Fill.Destination.SegmentAddress.QuadPart += page;
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
          args.pDmaBuffer == &packet.dma[1] && args.pDmaBufferPrivateData == &packet.data[1] &&
          args.DmaSize == sizeof(packet.dma) - sizeof(packet.dma[0]));
    /* One flush requires one real staged tail. It cannot enter a retry loop. */
    refuse_paging_growth = TRUE;
    CHECK(QlgWddm2BuildPaging(&core, &args) == STATUS_NO_MEMORY);
    CHECK(build(&args, &fresh, &tickets) == STATUS_NO_MEMORY && tickets == 0);
    refuse_paging_growth = FALSE;
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    completion.expected_fence = fence;
    CHECK(planned_submit(&submit, &packet, 1, fence) == STATUS_SUCCESS);
    CHECK(QlgPagingExecuteBatch(&submit, 1) == STATUS_SUCCESS);
    CHECK(build(&args, &fresh, &tickets) == STATUS_SUCCESS && tickets == 1);
    CHECK(QlgExecutePaging(&core.paging, &fresh.dma[0]) == STATUS_SUCCESS &&
          *(volatile UINT *)(bar + target + page) == FIXTURE_FILL);
    for (UINT index = RTL_NUMBER_OF(staged); index > 0; --index)
        CHECK(QlgCancelPaging(&core.paging, &staged[index - 1], FALSE) == STATUS_SUCCESS);
    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS && qlg_reset_quiesced(core.state) == STATUS_SUCCESS);
    CHECK(QlgResetMemory(&core) == STATUS_SUCCESS && data_blocks == blocks && !core.paging.overflow);
    CHECK(qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);
}

static void check_fresh_packet_grows_past_reserved_jobs(void)
{
    enum { WRITERS = 2 * QLM_MAX_PAGING_CAPACITY };
    static QLG_PAGING_TICKET staged[WRITERS + 1];
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    const SIZE_T bytes = WRITERS * QLG_GPUVA_PAGE_BYTES;
    const UINT64 source = 0x3100000ull, destination = source + bytes;
    const UINT first_fence = 300, blocks = data_blocks;
    FIXTURE_DMA packet;
    DXGKARG_BUILDPAGINGBUFFER args;
    PLANNED_COMPLETION completion = {0};
    QLG_PAGING_SUBMIT_SERVICES services = {
        .context = &completion, .enqueue = planned_enqueue, .complete = planned_complete,
    };
    UINT tickets;

    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services,
                                    QLG_RENDER_NODE, QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    for (UINT index = 0; index < WRITERS; ++index) {
        chain_fill(&args, source + index * QLG_GPUVA_PAGE_BYTES);
        CHECK(build(&args, &packet, &tickets) == STATUS_SUCCESS && tickets == 1);
        staged[index] = packet.dma[0];
    }
    CHECK(core.paging.job_count == WRITERS);
    physical_copy(&args, source, destination, bytes);
    CHECK(build(&args, &packet, &tickets) == STATUS_SUCCESS && tickets == 1);
    staged[WRITERS] = packet.dma[0];
    QLG_PAGING_JOB *copy = QlgPagingFindLocked(&core.paging, &staged[WRITERS]);
    CHECK(copy && copy->dependencies.count == WRITERS && copy->dependencies.tail &&
          core.paging.job_count == 2 * WRITERS);
    CHECK(QlgExecutePaging(&core.paging, &staged[WRITERS]) == STATUS_DEVICE_BUSY);

    for (UINT index = 0; index <= WRITERS; ++index) {
        packet.dma[0] = staged[index];
        packet.data[0] = staged[index];
        CHECK(planned_submit(&submit, &packet, 1, first_fence + index) == STATUS_SUCCESS);
    }
    CHECK(QlgPagingExecuteBatch(&submit, WRITERS + 1) == STATUS_DEVICE_BUSY);
    for (UINT index = 0; index <= WRITERS; ++index) {
        completion.expected_fence = first_fence + index;
        CHECK(QlgPagingExecuteBatch(&submit, index + 1) == STATUS_SUCCESS);
    }
    CHECK(completion.reports == WRITERS + 1);
    CHECK(*(volatile UINT *)(bar + destination) == FIXTURE_FILL &&
          memcmp((const void *)(bar + source), (const void *)(bar + destination), bytes) == 0);

    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS && qlg_reset_quiesced(core.state) == STATUS_SUCCESS);
    CHECK(QlgResetMemory(&core) == STATUS_SUCCESS && core.paging.job_count == QLM_MAX_JOBS &&
          !core.paging.overflow && data_blocks == blocks);
    CHECK(qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);
    CHECK(QlgExecutePaging(&core.paging, &staged[WRITERS]) != STATUS_SUCCESS);
}

#if QLG_PAGING_SIGNALS
static void check_many_system_signal_aliases(void)
{
    enum { SIGNALS = QLM_MAX_PAGING_CAPACITY + 2 };
    static QLG_PAGING_TICKET staged[SIGNALS];
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES], encoded[QLG_GPUVA_TABLE_ENTRIES];
    const UINT blocks = data_blocks;
    const UINT64 destination = SYSTEM_SOURCE_LOCAL + QLG_GPUVA_PAGE_BYTES;
    FIXTURE_DMA packet;
    DXGKARG_BUILDPAGINGBUFFER args;
    UINT count;

    system_layout(saved, encoded);
    RtlZeroMemory(system_page(1), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    RtlZeroMemory((void *)(bar + destination), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    for (UINT index = 0; index < SIGNALS; ++index) {
        const UINT64 slot = (UINT64)min(index, SIGNALS - 2) * sizeof(UINT64);
        const UINT64 alias = index % 2 ? SYSTEM_ALIAS_VA : VA_SYSTEM + QLG_GPUVA_PAGE_BYTES;

        CHECK(build_signal(alias + slot, index + 1, &packet, &count) == STATUS_SUCCESS && count == 1);
        staged[index] = packet.dma[0];
    }
    planned_transfer(&args, QLG_GPUVA_PAGE_BYTES, SYSTEM_ALIAS_VA,
                     SYSTEM_SOURCE_VA + QLG_GPUVA_PAGE_BYTES, DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL);
    CHECK(build(&args, &packet, &count) == STATUS_SUCCESS && count == 1);
    QLG_PAGING_JOB *upload = QlgPagingFindLocked(&core.paging, &packet.dma[0]);
    CHECK(upload && upload->dependencies.count == SIGNALS - 1 && upload->dependencies.tail);
    CHECK(QlgExecutePaging(&core.paging, &packet.dma[0]) == STATUS_DEVICE_BUSY);
    for (UINT index = 0; index < SIGNALS; ++index)
        CHECK(QlgExecutePaging(&core.paging, &staged[index]) == STATUS_SUCCESS);

    /* The captured bytes come from the aliased producers and the newest store. */
    RtlZeroMemory(system_page(1), (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(QlgExecutePaging(&core.paging, &packet.dma[0]) == STATUS_SUCCESS);
    const UINT64 *result = (const UINT64 *)(bar + destination);
    for (UINT index = 0; index < SIGNALS - 2; ++index)
        CHECK(result[index] == index + 1);
    CHECK(result[SIGNALS - 2] == SIGNALS && !result[SIGNALS - 1]);
    CHECK(mdl_maps == mdl_unmaps);
    RtlCopyMemory((void *)(bar + LEAF_OFFSET), saved, (SIZE_T)QLG_GPUVA_PAGE_BYTES);

    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS && qlg_reset_quiesced(core.state) == STATUS_SUCCESS);
    CHECK(QlgResetMemory(&core) == STATUS_SUCCESS && data_blocks == blocks && !core.paging.overflow);
    CHECK(qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);
}
#endif

static void check_fragmented_beyond_initial_pool(UINT run_count, UINT packet_count)
{
    enum { SOURCE_ROOT = 2, DESTINATION_ROOT = 3, UNIQUE_PAGE_LIMIT = 32768,
           MAX_PACKETS = QLM_MAX_PAGING_CAPACITY / QLG_MAX_PAGING_PACKET_JOBS };
    const UINT table_count = (run_count + QLG_GPUVA_TABLE_ENTRIES - 1) / QLG_GPUVA_TABLE_ENTRIES;
    const UINT unique_pages = min(run_count, UNIQUE_PAGE_LIMIT);
    static struct {
        QLG_PAGING_TICKET dma[QLG_MAX_PAGING_PACKET_JOBS];
        QLG_PAGING_TICKET data[QLG_MAX_PAGING_PACKET_JOBS];
    } packets[MAX_PACKETS];
    static DXGK_PTE entries[QLG_GPUVA_TABLE_ENTRIES];
    static QLG_GPUVA_PTE saved[QLG_GPUVA_TABLE_ENTRIES];
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    const UINT64 source_base = 0x5000000ull, destination_base = 0x20000000ull;
    const UINT64 table_base = 0x2500000ull;
    const UINT64 page_bytes = QLG_GPUVA_PAGE_BYTES;
    const UINT64 leaf_bytes = QLG_GPUVA_TABLE_ENTRIES * page_bytes;
    const UINT64 root_span = QLG_GPUVA_TABLE_ENTRIES * leaf_bytes;
    const UINT64 source_va = SOURCE_ROOT * root_span;
    const UINT64 destination_va = DESTINATION_ROOT * root_span;
    const UINT first_fence = 181, blocks = data_blocks;
    PLANNED_COMPLETION completion = {0};
    QLG_PAGING_SUBMIT_SERVICES services = {
        .context = &completion, .enqueue = planned_enqueue, .complete = planned_complete,
    };
    DXGKARG_BUILDPAGINGBUFFER args;

    RtlCopyMemory(saved, (const void *)(bar + ROOT_OFFSET), (SIZE_T)page_bytes);
    for (UINT endpoint = 0; endpoint < 2; ++endpoint) {
        const UINT root = endpoint ? DESTINATION_ROOT : SOURCE_ROOT;
        const UINT64 middle = table_base + endpoint * page_bytes;
        const UINT64 data_base = endpoint ? destination_base : source_base;
        DXGK_PTE middle_pointer = pte(QLM_MEMORY_SEGMENT, middle);

        CHECK(update_table(ROOT_OFFSET, 2, root, &middle_pointer, 1, FALSE) == STATUS_SUCCESS);
        for (UINT table = 0; table < table_count; ++table) {
            const UINT64 table_offset = table_base + (2 + endpoint * table_count + table) * page_bytes;
            const UINT first_run = table * QLG_GPUVA_TABLE_ENTRIES;
            const UINT count = min(QLG_GPUVA_TABLE_ENTRIES, run_count - first_run);
            DXGK_PTE pointer = pte(QLM_MEMORY_SEGMENT, table_offset);

            for (UINT entry = 0; entry < count; ++entry) {
                const UINT run = first_run + entry;
                const UINT64 physical = data_base + (run % unique_pages) * TWO_PAGES;

                entries[entry] = pte(QLM_MEMORY_SEGMENT, physical);
                if (run < unique_pages)
                    RtlFillMemory((void *)(bar + physical), (SIZE_T)page_bytes,
                                  endpoint ? 0 : (UCHAR)(run | 1));
            }
            CHECK(update_table(table_offset, 0, 0, entries, count, FALSE) == STATUS_SUCCESS);
            CHECK(update_table(middle, 1, table, &pointer, 1, FALSE) == STATUS_SUCCESS);
        }
    }
    planned_transfer(&args, run_count * page_bytes, source_va, destination_va,
                     DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL);
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services,
                                    QLG_RENDER_NODE, QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    args.hSystemContext = &system_context;
    for (UINT packet = 0; packet < packet_count; ++packet) {
        const BOOLEAN last = packet == packet_count - 1;
        DXGKARG_SUBMITCOMMANDVIRTUAL virtual_args = {0};
        DXGKARG_SUBMITCOMMAND command;

        args.pDmaBuffer = packets[packet].dma;
        args.DmaSize = sizeof(packets[packet].dma);
        args.pDmaBufferPrivateData = packets[packet].data;
        args.DmaBufferPrivateDataSize = sizeof(packets[packet].data);
        CHECK(QlgWddm2BuildPaging(&core, &args) ==
              (last ? STATUS_SUCCESS : STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER));
        CHECK(last ? args.MultipassOffset == 0 : args.MultipassOffset != 0);
        virtual_args.Flags.Paging = 1;
        virtual_args.NodeOrdinal = QLG_RENDER_NODE;
        virtual_args.EngineOrdinal = QLG_RENDER_ENGINE;
        virtual_args.SubmissionFenceId = first_fence + packet;
        virtual_args.DmaBufferSize = sizeof(packets[packet].dma) - args.DmaSize;
        virtual_args.pDmaBufferPrivateData = packets[packet].data;
        virtual_args.DmaBufferPrivateDataSize = sizeof(packets[packet].data) - args.DmaBufferPrivateDataSize;
        CHECK(QlgWddm2TranslateSubmit(&virtual_args, &command) == STATUS_SUCCESS);
        CHECK(QlgPagingSubmit(&submit, &command) == STATUS_SUCCESS);
    }
    for (UINT packet = 0; packet < packet_count; ++packet) {
        completion.expected_fence = first_fence + packet;
        CHECK(QlgPagingExecuteBatch(&submit, packet + 1) == STATUS_SUCCESS);
    }
    for (UINT run = 0; run < unique_pages; ++run) {
        const UINT64 source = source_base + run * TWO_PAGES;
        const UINT64 destination = destination_base + run * TWO_PAGES;

        CHECK(memcmp((const void *)(bar + source), (const void *)(bar + destination),
                     (SIZE_T)page_bytes) == 0);
    }
    CHECK(completion.reports == packet_count && data_blocks == blocks + 1);
    CHECK(qlg_reset_begin(core.state) == STATUS_SUCCESS && qlg_reset_quiesced(core.state) == STATUS_SUCCESS);
    CHECK(QlgResetMemory(&core) == STATUS_SUCCESS && data_blocks == blocks && !core.paging.overflow);
    CHECK(qlg_restart_ready(core.state) == STATUS_SUCCESS && qlg_restart(core.state) == STATUS_SUCCESS);
    RtlCopyMemory((void *)(bar + ROOT_OFFSET), saved, (SIZE_T)page_bytes);
}

static void check_dependent_requeue(void)
{
    FIXTURE_DMA producer, consumer;
    DXGKARG_BUILDPAGINGBUFFER args;
    static QLG_PAGING_SUBMIT_CONTEXT submit;
    QLG_PAGING_SUBMIT_SERVICES services = {0};
    PLANNED_COMPLETION completion = {0};
    UINT count;
    KIRQL level;

    services.context = &completion;
    services.enqueue = planned_enqueue;
    services.complete = planned_complete;
    CHECK(QlgPagingSubmitInitialize(&submit, &core.paging, &services, QLG_RENDER_NODE,
                                    QLG_RENDER_ENGINE) == STATUS_SUCCESS);
    chain_fill(&args, CHAIN_MIDDLE);
    CHECK(build(&args, &producer, &count) == STATUS_SUCCESS);
    physical_copy(&args, CHAIN_MIDDLE, CHAIN_DESTINATION, (SIZE_T)QLG_GPUVA_PAGE_BYTES);
    CHECK(build(&args, &consumer, &count) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &producer, 1, CHAIN_RETRY_FENCE) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &consumer, 1, CHAIN_CONSUMER_FENCE) == STATUS_SUCCESS);
    level = core.paging.services.lock(core.paging.services.context);
    CHECK(QlgPagingRequeueLocked(&submit, 1) == STATUS_DEVICE_BUSY);
    CHECK(QlgPagingRequeueLocked(&submit, 2) == STATUS_SUCCESS);
    CHECK(QlgPagingRequeueLocked(&submit, 1) == STATUS_SUCCESS);
    core.paging.services.unlock(core.paging.services.context, level);
    CHECK(QlgExecutePaging(&core.paging, &consumer.dma[0]) == STATUS_DEVICE_BUSY);
    CHECK(planned_submit(&submit, &producer, 1, CHAIN_RETRY_FENCE) == STATUS_SUCCESS);
    CHECK(planned_submit(&submit, &consumer, 1, CHAIN_CONSUMER_FENCE) == STATUS_SUCCESS);
    completion.expected_fence = CHAIN_RETRY_FENCE;
    CHECK(QlgPagingExecuteBatch(&submit, 3) == STATUS_SUCCESS);
    completion.expected_fence = CHAIN_CONSUMER_FENCE;
    CHECK(QlgPagingExecuteBatch(&submit, 4) == STATUS_SUCCESS);
    CHECK(((UINT *)(bar + CHAIN_DESTINATION))[0] == FIXTURE_FILL);
    CHECK(completion.reports == 2);
    CHECK(qlm_reset_epoch(core.memory_arena, TRUE) == STATUS_SUCCESS);
}

static void check_other_operations(void)
{
    DXGKARG_BUILDPAGINGBUFFER args = {0};
    FIXTURE_DMA buffer;
    UINT tickets;

    args.Operation = DXGK_OPERATION_FLUSH_TLB;
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS && tickets == 0);
    args.Operation = DXGK_OPERATION_UPDATE_CONTEXT_ALLOCATION;
    CHECK(build(&args, &buffer, &tickets) == STATUS_SUCCESS && tickets == 0);
    args.Operation = DXGK_OPERATION_SPECIAL_LOCK_TRANSFER;
    CHECK(build(&args, &buffer, &tickets) == STATUS_NOT_SUPPORTED && tickets == 0);
    args.Operation = DXGK_OPERATION_FLUSH_TLB;
    args.MultipassOffset = 1;
    CHECK(build(&args, &buffer, &tickets) == STATUS_NOT_SUPPORTED && tickets == 0);
}

static void check_submit_translation(void)
{
    DXGKARG_SUBMITCOMMANDVIRTUAL virtual_args = {0};
    DXGKARG_SUBMITCOMMAND args;
    QLG_PAGING_TICKET tickets[2] = {{1, 2}, {1, 3}};

    virtual_args.hContext = &system_context;
    virtual_args.DmaBufferVirtualAddress = BASE_VA;
    virtual_args.DmaBufferSize = sizeof(tickets);
    virtual_args.pDmaBufferPrivateData = tickets;
    virtual_args.DmaBufferPrivateDataSize = sizeof(tickets);
    virtual_args.SubmissionFenceId = 41;
    virtual_args.Flags.Paging = 1;
    virtual_args.Flags.Resubmission = 1;
    virtual_args.NodeOrdinal = QLG_RENDER_NODE;
    virtual_args.EngineOrdinal = QLG_RENDER_ENGINE;
    CHECK(QlgWddm2TranslateSubmit(&virtual_args, &args) == STATUS_SUCCESS);
    CHECK(args.hContext == &system_context && args.DmaBufferSize == sizeof(tickets) &&
          args.DmaBufferSubmissionStartOffset == 0 &&
          args.DmaBufferSubmissionEndOffset == sizeof(tickets) &&
          args.pDmaBufferPrivateData == tickets &&
          args.DmaBufferPrivateDataSubmissionEndOffset == sizeof(tickets) &&
          args.SubmissionFenceId == 41 && args.Flags.Value == 1 &&
          args.NodeOrdinal == QLG_RENDER_NODE && args.EngineOrdinal == QLG_RENDER_ENGINE);
    virtual_args.DmaBufferSize = 0;
    CHECK(QlgWddm2TranslateSubmit(&virtual_args, &args) == STATUS_SUCCESS &&
          args.DmaBufferPrivateDataSubmissionEndOffset == 0);
    virtual_args.DmaBufferSize = sizeof(tickets) + 1;
    CHECK(QlgWddm2TranslateSubmit(&virtual_args, &args) == STATUS_INVALID_PARAMETER);
    virtual_args.DmaBufferSize = sizeof(tickets);
    virtual_args.DmaBufferPrivateDataSize = sizeof(tickets[0]);
    CHECK(QlgWddm2TranslateSubmit(&virtual_args, &args) == STATUS_INVALID_PARAMETER);
    virtual_args.DmaBufferPrivateDataSize = sizeof(tickets);
    /* W5: commands in a non-paging packet are refused ... */
    virtual_args.Flags.Paging = 0;
    virtual_args.Flags.Present = 1;
    CHECK(QlgWddm2TranslateSubmit(&virtual_args, &args) == STATUS_NOT_SUPPORTED);
    /* ... an empty present or flip marker becomes a fence-only packet, which
     * the engine completes in order (no private command reaches it) ... */
    virtual_args.DmaBufferSize = 0;
    virtual_args.Flags.Flip = 1;
    CHECK(QlgWddm2TranslateSubmit(&virtual_args, &args) == STATUS_SUCCESS);
    CHECK(args.hContext == &system_context && !args.pDmaBufferPrivateData &&
          args.DmaBufferSize == 0 && args.DmaBufferPrivateDataSubmissionEndOffset == 0 &&
          args.Flags.Present && args.Flags.Flip && !args.Flags.Resubmission &&
          !args.Flags.Paging && args.SubmissionFenceId == 41);
    /* ... but needs a context and is never a context switch. */
    virtual_args.Flags.ContextSwitch = 1;
    CHECK(QlgWddm2TranslateSubmit(&virtual_args, &args) == STATUS_NOT_SUPPORTED);
    virtual_args.Flags.ContextSwitch = 0;
    virtual_args.hContext = NULL;
    CHECK(QlgWddm2TranslateSubmit(&virtual_args, &args) == STATUS_NOT_SUPPORTED);
}

static void check_processes(void)
{
    DXGKARG_CREATEPROCESS args = {0};
    QLG_MINIPORT stopped = {0};

    args.Flags.SystemProcess = 1;
    CHECK(QlgMiniportCreateProcess(&miniport, &args) == STATUS_SUCCESS && args.hKmdProcess);
    CHECK(QlgMiniportCreateProcess(&stopped, &args) == STATUS_DEVICE_NOT_READY);
    CHECK(QlgMiniportDestroyProcess(&miniport, &core) == STATUS_INVALID_PARAMETER);
    CHECK(QlgMiniportDestroyProcess(&miniport, args.hKmdProcess) == STATUS_SUCCESS);
}

/* WDDM 2.x must retain the OS allocation while using its private data. */
static UINT handle_acquires, handle_releases;
static BOOLEAN handle_available;
static LONG handle_cookie;

static void *APIENTRY acquire_test_handle(const DXGKARGCB_GETHANDLEDATA *query,
                                           DXGKARG_RELEASE_HANDLE *reference)
{
    CHECK(query->hObject == 42 && query->Type == DXGK_HANDLE_ALLOCATION &&
          query->Flags.DeviceSpecific);
    ++handle_acquires;
    *reference = handle_available ? &handle_cookie : NULL;
    return handle_available ? &core : NULL;
}

static void APIENTRY release_test_handle(DXGKARGCB_RELEASEHANDLEDATA reference)
{
    CHECK(reference.Type == DXGK_HANDLE_ALLOCATION &&
          reference.ReleaseHandle == &handle_cookie);
    ++handle_releases;
}

static void *APIENTRY forbidden_legacy_handle(const DXGKARGCB_GETHANDLEDATA *query)
{
    UNREFERENCED_PARAMETER(query);
    CHECK(FALSE);
    return NULL;
}

static void check_handle_references(void)
{
    QLG_SERVICES services = {0};
    DXGKARGCB_GETHANDLEDATA query = {0};
    DXGKARGCB_RELEASEHANDLEDATA reference = {0};
    services.get_handle = forbidden_legacy_handle;
    services.acquire_handle = acquire_test_handle;
    services.release_handle = release_test_handle;
    query.hObject = 42;
    query.Type = DXGK_HANDLE_ALLOCATION;
    query.Flags.DeviceSpecific = TRUE;
    handle_available = TRUE;
    CHECK(QlgAcquireAllocationHandle(&services, &query, &reference) == &core);
    CHECK(handle_acquires == 1 && handle_releases == 0);
    QlgReleaseAllocationHandle(&services, &reference);
    CHECK(handle_releases == 1);
    handle_available = FALSE;
    CHECK(QlgAcquireAllocationHandle(&services, &query, &reference) == NULL);
    QlgReleaseAllocationHandle(&services, &reference);
    CHECK(handle_acquires == 2 && handle_releases == 1);
    services.acquire_handle = NULL;
    CHECK(QlgAcquireAllocationHandle(&services, &query, &reference) == NULL);
    QlgReleaseAllocationHandle(&services, &reference);
    CHECK(handle_acquires == 2 && handle_releases == 1);
}

static void check_adapter_queries(void)
{
    DXGKARG_QUERYADAPTERINFO query = {0};
    DXGK_DRIVERCAPS caps;
    DXGK_QUERYSEGMENTOUT4 segments4 = {0};
    DXGK_QUERYSEGMENTOUT3 segments3 = {0};
    DXGK_SEGMENTDESCRIPTOR3 descriptor3[2];
    UCHAR descriptor4[2 * sizeof(DXGK_SEGMENTDESCRIPTOR4) + 16];
    DXGK_QUERYPAGETABLELEVELDESCIN level = {0};
    DXGK_PAGE_TABLE_LEVEL_DESC desc;
    DXGK_PHYSICALADAPTERCAPS physical;
    DXGK_GPUMMUCAPS mmu;

    miniport.started = TRUE;
    query.Type = DXGKQAITYPE_DRIVERCAPS;
    query.pOutputData = &caps;
    query.OutputDataSize = sizeof(caps);
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_SUCCESS);
    CHECK(caps.WDDMVersion == (QLG_PAGING_SIGNALS ? DXGKDDI_WDDMv2_2 : DXGKDDI_WDDMv2) &&
          caps.MemoryManagementCaps.GpuMmuSupported &&
          caps.MemoryManagementCaps.VirtualAddressingSupported &&
          caps.MemoryManagementCaps.SectionBackedPrimary &&
          !caps.PresentationCaps.NoCacheCoherentApertureMemory &&
          !caps.PresentationCaps.SupportSoftwareDeviceBitmaps &&
          !caps.MemoryManagementCaps.IoMmuSupported &&
          caps.MemoryManagementCaps.PagingNode == QLG_RENDER_NODE &&
          caps.ApertureSegmentCommitLimit == QLM_APERTURE_BYTES);

    /* Required startup query: no optional display features, bounded writes. */
    struct {
        DXGK_DISPLAY_DRIVERCAPS_EXTENSION caps;
        ULONG canary;
    } display_extension = {{0}, 0xfeed1234u};
    display_extension.caps.Value = MAXUINT;
    query.Type = DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION;
    query.pOutputData = &display_extension.caps;
    query.OutputDataSize = sizeof(display_extension);
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_SUCCESS &&
          display_extension.caps.Value == 0 && display_extension.canary == 0xfeed1234u);
    display_extension.caps.Value = 0xa5a5a5a5u;
    query.OutputDataSize = sizeof(display_extension.caps) - 1;
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_BUFFER_TOO_SMALL &&
          display_extension.caps.Value == 0xa5a5a5a5u &&
          display_extension.canary == 0xfeed1234u);

    /* One local memory segment and one system-memory aperture, in the 3 and 4 layouts. */
    query.Type = DXGKQAITYPE_QUERYSEGMENT3;
    query.pOutputData = &segments3;
    query.OutputDataSize = sizeof(segments3);
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_SUCCESS &&
          segments3.NbSegment == 2);
    segments3.pSegmentDescriptor = descriptor3;
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_SUCCESS &&
          descriptor3[0].Flags.CpuVisible && !descriptor3[0].Flags.Aperture &&
          descriptor3[0].Size == QLM_SEGMENT_BYTES && descriptor3[1].Flags.Aperture &&
          descriptor3[1].Flags.CacheCoherent && descriptor3[1].Size == QLM_APERTURE_BYTES);
    query.Type = DXGKQAITYPE_QUERYSEGMENT4;
    query.pOutputData = &segments4;
    query.OutputDataSize = sizeof(segments4);
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_SUCCESS &&
          segments4.NbSegment == 2 &&
          segments4.SegmentDescriptorStride == sizeof(DXGK_SEGMENTDESCRIPTOR4) &&
          segments4.PagingBufferPrivateDataSize == QLG_PAGING_PACKET_BYTES);
    memset(descriptor4, FIXTURE_POISON, sizeof(descriptor4));
    segments4.pSegmentDescriptor = descriptor4;
    segments4.SegmentDescriptorStride = sizeof(DXGK_SEGMENTDESCRIPTOR4) + 16;
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_SUCCESS);
    CHECK(((DXGK_SEGMENTDESCRIPTOR4 *)descriptor4)->Size == QLM_SEGMENT_BYTES &&
          ((DXGK_SEGMENTDESCRIPTOR4 *)descriptor4)->Flags.CpuVisible &&
          descriptor4[sizeof(DXGK_SEGMENTDESCRIPTOR4)] == FIXTURE_POISON &&
          ((DXGK_SEGMENTDESCRIPTOR4 *)(descriptor4 + segments4.SegmentDescriptorStride))->Flags.Aperture);
    segments4.SegmentDescriptorStride = sizeof(DXGK_SEGMENTDESCRIPTOR4) - 8;
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_BUFFER_TOO_SMALL);
    segments4.SegmentDescriptorStride = 0;
    segments4.NbSegment = 0;
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_BUFFER_TOO_SMALL);

    /* The MMU queries route through the 2.0 handler. */
    query.Type = DXGKQAITYPE_GPUMMUCAPS;
    query.pOutputData = &mmu;
    query.OutputDataSize = sizeof(mmu);
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_SUCCESS &&
          mmu.PageTableLevelCount == 3);
    query.Type = DXGKQAITYPE_PAGETABLELEVELDESC;
    query.pInputData = &level;
    query.InputDataSize = sizeof(level);
    query.pOutputData = &desc;
    query.OutputDataSize = sizeof(desc);
    level.LevelIndex = 2;
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_SUCCESS &&
          desc.PageTableSizeInBytes == 8192);
    level.LevelIndex = 3;
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_INVALID_PARAMETER);
    level.LevelIndex = 0;
    level.PhysicalAdapterIndex = 1;
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_INVALID_PARAMETER);
    /* Use a distinct OS-owned cookie so returning the miniport cannot pass. */
    HANDLE original_handle = miniport.kernel.DeviceHandle;
    miniport.kernel.DeviceHandle = (HANDLE)(ULONG_PTR)0x67892000u;
    query.Type = DXGKQAITYPE_PHYSICALADAPTERCAPS;
    query.pInputData = NULL;
    query.InputDataSize = 0;
    query.pOutputData = &physical;
    query.OutputDataSize = sizeof(physical);
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_SUCCESS &&
          physical.NumExecutionNodes == 1 && physical.Flags.GpuMmuSupported &&
          !physical.Flags.IoMmuSupported && physical.DxgkPhysicalAdapterHandle == miniport.kernel.DeviceHandle &&
          physical.DxgkPhysicalAdapterHandle != &miniport);
    miniport.kernel.DeviceHandle = original_handle;
    query.Type = DXGKQAITYPE_SEGMENTMEMORYSTATE;
    CHECK(QlgMiniportQueryContracts(&miniport, &query) == STATUS_NOT_SUPPORTED);
    miniport.started = FALSE;
}

void main(void)
{
    static const QLG_MDL_SERVICES mdl = {&core, mock_map, mock_unmap, mock_address,
                                         mock_last_alias, mock_map_indexed_write};

    /* The 1 GiB "BAR" is reserved and committed lazily by the host. */
    bar = VirtualAlloc(NULL, (SIZE_T)QLM_ARENA_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    system_pages = VirtualAlloc(NULL, FIXTURE_SYSTEM_PAGES * QLG_GPUVA_PAGE_BYTES,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(bar && system_pages);
    core.services.context = &core;
    core.services.lock = fixture_lock;
    core.services.unlock = fixture_unlock;
    core.services.allocate = fixture_allocate;
    core.services.release = fixture_release;
    core.state = calloc(1, qlg_state_size(QLG_KIND_ADAPTER));
    core.memory_arena = calloc(1, qlm_arena_size());
    CHECK(core.state && core.memory_arena);
    InitializeListHead(&core.allocations);
    qlg_adapter_init(core.state);
    qlm_arena_init(core.memory_arena);
    CHECK(QlgAttachMemory(&core, bar, (SIZE_T)QLM_ARENA_BYTES, &mdl) == STATUS_SUCCESS);
    miniport.lifecycle.adapter = &core;
    system_context.adapter = &core;
    system_context.kind = QLG_KIND_CONTEXT;
    init_allocation(&allocation_a, TWO_PAGES);
    init_allocation(&allocation_b, TWO_PAGES);

    check_geometry_and_queries();
    check_encoding();
    check_page_table_updates();
    check_walks();
    check_virtual_paging();
    check_partial_virtual_fills();
    check_implicit_paging();
    check_residency_notifications();
    check_fence_pages();
#if QLG_PAGING_SIGNALS
    check_paging_fence_signals();
#endif
    check_planned_paging();
    check_planned_mutations();
    check_local_copy_chains();
    check_retained_pfn_prefix();
    check_system_snapshots();
#if QLG_PAGING_SIGNALS
    check_paging_fence_aliases();
#endif
    check_allocated_system_writer();
    check_allocated_local_writer();
    check_allocated_upload_snapshot();
    check_large_allocated_upload();
    check_physical_system_writers();
    check_multipass_system_transfer();
    check_fragmented_local_transfer();
    check_fragmented_multipass();
    check_sparse_fragmented_local_copy();
    check_virtual_buffer_paging();
    check_paging_packet_headroom();
    check_full_pool_flush();
    check_fresh_packet_grows_past_reserved_jobs();
#if QLG_PAGING_SIGNALS
    check_many_system_signal_aliases();
#endif
    check_fragmented_beyond_initial_pool(QLM_MAX_JOBS * QLM_MAX_JOBS + 1,
                                         QLG_PAGING_PACKET_BUDGET + 1);
    check_fragmented_beyond_initial_pool(QLM_MAX_PAGING_CAPACITY * QLM_MAX_PAGING_CAPACITY,
                                         QLM_MAX_PAGING_CAPACITY / QLG_MAX_PAGING_PACKET_JOBS);
    check_fragmented_beyond_initial_pool((UINT)QLM_MEMORY_PAGES,
                                         QLM_MAX_PAGING_CAPACITY / QLG_MAX_PAGING_PACKET_JOBS);
    check_dependent_requeue();
    check_other_operations();
    check_submit_translation();
    check_processes();
    check_handle_references();
    check_adapter_queries();
    CHECK(mdl_maps && mdl_maps == mdl_unmaps && locks && data_blocks == 0);
    ExitProcess(0);
}

/* Module: lightning.graphics_driver.miniport_contracts
 * Purpose: Marshal native segment queries, paging patching and allocation metadata.
 * Created: 2026-10-09
 * Architecture: This WDK ABI adapter exposes only live mapped PCI storage. Rust
 * owns object policy; experimental capabilities expose implemented DDIs and
 * cannot establish native application compatibility or certification.
 */
#include "miniport.h"
#include "internal.h"
#define QLG_NATIVE_SEGMENT_COUNT ((UINT)QLM_APERTURE_SEGMENT)
#define QLG_NATIVE_NODE_COUNT 1u
/* Redirection surfaces up to 2 << (11 + 3) = 16384 pixels per side. */
#define QLG_REDIRECTION_SHIFT 3u

NTSTATUS QlgMiniportIoRequest(void *context, ULONG source, VIDEO_REQUEST_PACKET *request)
{
    UNREFERENCED_PARAMETER(source);
    if (!context || !request)
        return STATUS_INVALID_PARAMETER;

    /* Legacy video-port requests carry no state for this virtual output.
     * Completing them empty matches the inbox display drivers; rejecting
     * them aborts Windows display-mode changes on the source. */
    if (request->StatusBlock) {
        request->StatusBlock->Status = STATUS_SUCCESS;
        request->StatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS QlgMiniportAcpi(void *context, DXGK_EVENT_TYPE type, ULONG event, void *argument,
                         ULONG *flags)
{
    UNREFERENCED_PARAMETER(type);
    UNREFERENCED_PARAMETER(event);
    UNREFERENCED_PARAMETER(argument);
    if (!context || !flags)
        return STATUS_INVALID_PARAMETER;
    *flags = 0;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS QlgMiniportInterface(void *context, QUERY_INTERFACE *query)
{
    if (!context || !query)
        return STATUS_INVALID_PARAMETER;
    /* No child or external bus interface is implemented. */
    return STATUS_NOT_SUPPORTED;
}

void QlgMiniportUnload(void)
{
    /* All resources belong to per-adapter PnP contexts released by RemoveDevice.
     * There is no global allocation, callback registration or thread to release. */
}

void QlgMiniportNoDisplayReset(void *context)
{
    UNREFERENCED_PARAMETER(context);
    /* This PCI function owns zero display sources, children and VGA state.
     * Nothing belongs to it for the HAL display-reset notification to restore.
     * The actual display owner remains untouched. This nonpageable callback is
     * safe at arbitrary IRQL: it performs no wait, memory access, engine reset
     * or TDR completion. Engine recovery uses the separate scheduling DDIs. */
}

NTSTATUS APIENTRY QlgMiniportEscape(HANDLE context, const DXGKARG_ESCAPE *args)
{
    QLG_MINIPORT *miniport = context;

    if (!miniport || !args)
        return STATUS_INVALID_PARAMETER;
    return QlgMiniportVenusEscape(miniport, args);
}

static NTSTATUS query_segment(QLG_MINIPORT *miniport, const DXGKARG_QUERYADAPTERINFO *args)
{
    DXGK_SEGMENTDESCRIPTOR segments[QLG_NATIVE_SEGMENT_COUNT] = {{0}};
    UINT index;
    NTSTATUS status =
        QlgLifecycleQuerySegment(&miniport->lifecycle, &segments[QLM_MEMORY_SEGMENT - 1]);

    if (status != STATUS_SUCCESS)
        return status;
    /* Logical addresses resolve to the retained runtime MDL mapping at copy
     * execution. There is no contiguous CPU physical window for this segment. */
    segments[QLM_APERTURE_SEGMENT - 1].Size = (SIZE_T)QLM_APERTURE_BYTES;
    segments[QLM_APERTURE_SEGMENT - 1].CommitLimit = (SIZE_T)QLM_APERTURE_BYTES;
    segments[QLM_APERTURE_SEGMENT - 1].Flags.Aperture = TRUE;
    segments[QLM_APERTURE_SEGMENT - 1].Flags.CacheCoherent = TRUE;
    if (args->Type == DXGKQAITYPE_QUERYSEGMENT) {
        DXGK_QUERYSEGMENTOUT *output = args->pOutputData;

        if (args->OutputDataSize < sizeof(*output))
            return STATUS_BUFFER_TOO_SMALL;

        if (output->pSegmentDescriptor) {
            if (output->NbSegment < QLG_NATIVE_SEGMENT_COUNT)
                return STATUS_BUFFER_TOO_SMALL;
            RtlCopyMemory(output->pSegmentDescriptor, segments, sizeof(segments));
        }
        output->NbSegment = QLG_NATIVE_SEGMENT_COUNT;
        output->PagingBufferSegmentId = 0;
        output->PagingBufferSize = (UINT)QLG_PAGE_BYTES;
        output->PagingBufferPrivateDataSize = QLG_PAGING_PACKET_BYTES;
        return STATUS_SUCCESS;
    }
    if (args->Type == DXGKQAITYPE_QUERYSEGMENT3) {
        DXGK_QUERYSEGMENTOUT3 *output = args->pOutputData;

        if (args->OutputDataSize < sizeof(*output))
            return STATUS_BUFFER_TOO_SMALL;

        if (output->pSegmentDescriptor) {
            DXGK_SEGMENTDESCRIPTOR3 *descriptor = output->pSegmentDescriptor;

            if (output->NbSegment < QLG_NATIVE_SEGMENT_COUNT)
                return STATUS_BUFFER_TOO_SMALL;

            for (index = 0; index < QLG_NATIVE_SEGMENT_COUNT; ++index) {
                RtlZeroMemory(&descriptor[index], sizeof(*descriptor));
                descriptor[index].Flags = segments[index].Flags;
                descriptor[index].BaseAddress = segments[index].BaseAddress;
                descriptor[index].CpuTranslatedAddress = segments[index].CpuTranslatedAddress;
                descriptor[index].Size = segments[index].Size;
                descriptor[index].CommitLimit = segments[index].CommitLimit;
            }
        }
        output->NbSegment = QLG_NATIVE_SEGMENT_COUNT;
        output->PagingBufferSegmentId = 0;
        output->PagingBufferSize = (UINT)QLG_PAGE_BYTES;
        output->PagingBufferPrivateDataSize = QLG_PAGING_PACKET_BYTES;
        return STATUS_SUCCESS;
    }
#if QLG_WDDM2
    if (args->Type == DXGKQAITYPE_QUERYSEGMENT4) {
        DXGK_QUERYSEGMENTOUT4 *output = args->pOutputData;

        if (args->OutputDataSize < sizeof(*output))
            return STATUS_BUFFER_TOO_SMALL;

        if (output->pSegmentDescriptor) {
            /* The runtime sizes the array; a zero stride means the 2.0 layout. */
            SIZE_T stride = output->SegmentDescriptorStride ? output->SegmentDescriptorStride
                                                            : sizeof(DXGK_SEGMENTDESCRIPTOR4);

            if (output->NbSegment < QLG_NATIVE_SEGMENT_COUNT ||
                stride < sizeof(DXGK_SEGMENTDESCRIPTOR4))
                return STATUS_BUFFER_TOO_SMALL;

            for (index = 0; index < QLG_NATIVE_SEGMENT_COUNT; ++index) {
                DXGK_SEGMENTDESCRIPTOR4 *descriptor =
                    (DXGK_SEGMENTDESCRIPTOR4 *)(output->pSegmentDescriptor + index * stride);

                RtlZeroMemory(descriptor, sizeof(*descriptor));
                descriptor->Flags = segments[index].Flags;
                descriptor->BaseAddress = segments[index].BaseAddress;
                descriptor->CpuTranslatedAddress = segments[index].CpuTranslatedAddress;
                descriptor->Size = segments[index].Size;
                descriptor->CommitLimit = segments[index].CommitLimit;
            }
        } else
            output->SegmentDescriptorStride = sizeof(DXGK_SEGMENTDESCRIPTOR4);
        output->NbSegment = QLG_NATIVE_SEGMENT_COUNT;
        output->PagingBufferSegmentId = 0;
        output->PagingBufferSize = (UINT)QLG_PAGE_BYTES;
        output->PagingBufferPrivateDataSize = QLG_PAGING_PACKET_BYTES;
        return STATUS_SUCCESS;
    }
#endif
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS QlgMiniportQueryContracts(QLG_MINIPORT *miniport, const DXGKARG_QUERYADAPTERINFO *args)
{
    if (!miniport->started || miniport->stopping)
        return STATUS_DEVICE_NOT_READY;

    switch (args->Type) {
    case DXGKQAITYPE_64BITONLYCAPS: {
        DXGK_64_BIT_ONLY_CAPS *caps = args->pOutputData;

        if (args->InputDataSize)
            return STATUS_INVALID_PARAMETER;

        if (args->OutputDataSize < sizeof(*caps))
            return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(caps, sizeof(*caps));
        /* The kernel stays native x64; the matching WOW64 UMD/ICD
         * package also serves 32-bit applications. */
        caps->SupportsOnly64Bit = FALSE;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_QUERYSEGMENT:
    case DXGKQAITYPE_QUERYSEGMENT3:
#if QLG_WDDM2
    case DXGKQAITYPE_QUERYSEGMENT4:
#endif
        return query_segment(miniport, args);
    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS *caps = args->pOutputData;

        if (args->OutputDataSize < sizeof(*caps))
            return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(caps, sizeof(*caps));
        caps->HighestAcceptableAddress.QuadPart = MAXLONGLONG;
        caps->ApertureSegmentCommitLimit = (SIZE_T)QLM_APERTURE_BYTES;
        caps->MaxAllocationListSlotId = QLG_BATCH_ALLOCATIONS - 1;
        caps->InterruptMessageNumber = QLG_MINIPORT_INTERRUPT_MESSAGE;
        /* Flips are MMIO address writes latched by the software vertical blank,
         * which reports CRTC_VSYNC. No immediate or queued flips. */
        caps->FlipCaps.FlipOnVSyncMmIo = TRUE;
        /* The adapter is never the legacy VGA device; it may still be the only display. */
        caps->SupportNonVGA = TRUE;
        /* Desktop presentation as in Microsoft's render-only display sample:
         * GDI redirection bitmaps are software bitmaps, screen-to-screen Blts
         * are slow CPU copies, and redirection surfaces may be 16K wide
         * (2 << (11 + shift)). */
        /* SupportSoftwareDeviceBitmaps is reserved on Windows 8 and later. */
        caps->PresentationCaps.NoScreenToScreenBlt = TRUE;
        caps->PresentationCaps.NoOverlapScreenBlt = TRUE;
        caps->PresentationCaps.MaxTextureWidthShift = QLG_REDIRECTION_SHIFT;
        caps->PresentationCaps.MaxTextureHeightShift = QLG_REDIRECTION_SHIFT;
        caps->SchedulingCaps.MultiEngineAware = 1;
        caps->SchedulingCaps.CancelCommandAware = 1;
        caps->SchedulingCaps.PreemptionAware = 1;
        caps->GpuEngineTopology.NbAsymetricProcessingNodes = QLG_NATIVE_NODE_COUNT;
        caps->PreemptionCaps.GraphicsPreemptionGranularity =
            D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        caps->PreemptionCaps.ComputePreemptionGranularity =
            D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        caps->SupportPerEngineTDR = TRUE;
#if QLG_WDDM2
        /* GpuMmu over the software MMU (gpuva.c); paging runs on the only node. */
#if DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_2
        caps->WDDMVersion = DXGKDDI_WDDMv2_2;
#else
        caps->WDDMVersion = DXGKDDI_WDDMv2;
#endif
        caps->MemoryManagementCaps.VirtualAddressingSupported = 1;
        caps->MemoryManagementCaps.GpuMmuSupported = 1;
        /* Let VidMm back primary CPU mappings with a section. The legacy IO
         * range path has no process mapping object for CDD's early boot Blt. */
        caps->MemoryManagementCaps.SectionBackedPrimary = 1;
        caps->MemoryManagementCaps.PagingNode = QLG_RENDER_NODE;
#else
        caps->WDDMVersion = DXGKDDI_WDDMv1_3;
#endif
        /* A hardware cursor keeps pointer moves from recompositing the desktop. */
        QlgCursorCaps(miniport, caps);
        /* Report the version of the registered native interface. No GDI,
         * cross-adapter or protected memory cap is inferred, nor GPU-VA in 1.3.
         * These experimental caps enable native testing, not certification. */
        return STATUS_SUCCESS;
    }
#if QLG_WDDM2
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION: {
        DXGK_DISPLAY_DRIVERCAPS_EXTENSION *caps = args->pOutputData;

        if (args->OutputDataSize < sizeof(*caps))
            return STATUS_BUFFER_TOO_SMALL;
        /* WDDM 2.x queries this at adapter startup. Optional display features
         * are absent, but the capability response itself must be valid. */
        RtlZeroMemory(caps, sizeof(*caps));
        return STATUS_SUCCESS;
    }
#endif
    case DXGKQAITYPE_NUMPOWERCOMPONENTS:
        if (args->OutputDataSize < sizeof(UINT))
            return STATUS_BUFFER_TOO_SMALL;
        *(UINT *)args->pOutputData = 0;
        return STATUS_SUCCESS;
    case DXGKQAITYPE_HISTORYBUFFERPRECISION:
        /* No GPU history/timestamp implementation exists. Leave output intact;
         * the native query wrapper recognizes this optional-DDI status. */
        return STATUS_NOT_IMPLEMENTED;
    default:
#if QLG_WDDM2
        return QlgWddm2Query(miniport, args);
#else
        return STATUS_NOT_SUPPORTED;
#endif
    }
}

/* Paging tickets and CPU present packets are the only DMA streams. */
NTSTATUS APIENTRY QlgMiniportPatch(HANDLE handle, const DXGKARG_PATCH *args)
{
    QLG_MINIPORT *miniport = handle;
    QLG_ADAPTER *adapter;

    if (!miniport || !args || !(adapter = miniport->lifecycle.adapter))
        return STATUS_DEVICE_NOT_READY;

    if (args->Flags.Paging)
        return QlgPagingPatch(QlgAdapterPaging(adapter), args);
    return QlgDisplayPatch(args);
}

NTSTATUS APIENTRY QlgMiniportDescribeAllocation(HANDLE handle, DXGKARG_DESCRIBEALLOCATION *args)
{
    QLG_MINIPORT *miniport = handle;
    QLG_OBJECT *allocation;

    if (!miniport || !args || !(allocation = args->hAllocation) ||
        allocation->kind != QLG_KIND_ALLOCATION ||
        allocation->adapter != miniport->lifecycle.adapter)
        return STATUS_INVALID_PARAMETER;
    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_DESCRIBE_CALLS]);
    /* Diagnostic: last described (width << 16) | height. */
    InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_DESCRIBE_LAST],
                        (LONG)((allocation->width << 16) | (allocation->height & 0xffffu)));
    args->Width = allocation->width;
    args->Height = allocation->height;
    /* Primaries echo the format and refresh rate they were created with, as
     * Dxgkrnl checks them against the mode being committed. */
    args->Format = allocation->primary_format ? (D3DDDIFORMAT)allocation->primary_format
                   : allocation->resource_kind == QLG_RESOURCE_BUFFER ? D3DDDIFMT_UNKNOWN
                   : allocation->format == QLG_FORMAT_BGRA8           ? D3DDDIFMT_A8R8G8B8
                                                                      : D3DDDIFMT_A8B8G8R8;
    args->MultisampleMethod.NumSamples = 1;
    args->MultisampleMethod.NumQualityLevels = 0;
    args->RefreshRate.Numerator = allocation->refresh_numerator;
    args->RefreshRate.Denominator =
        allocation->refresh_denominator ? allocation->refresh_denominator : 1;
    args->PrivateDriverFormatAttribute = 0;
    args->Rotation = D3DDDI_ROTATION_IDENTITY;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgMiniportStandardAllocation(HANDLE handle,
                                                DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *args)
{
    QLG_ALLOCATION_DESC description = {0};
    UINT64 bytes;

    if (!args || args->PhysicalAdapterIndex)
        return STATUS_NOT_SUPPORTED;

    if (handle)
        InterlockedExchange(&((QLG_MINIPORT *)handle)->display.counters[QLG_DISPLAY_STANDARD_TYPE],
                            (LONG)args->StandardAllocationType);
    description.magic = QLG_PRIVATE_MAGIC;
    description.version = QLG_PRIVATE_VERSION;
    if (args->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE &&
        args->pCreateStagingSurfaceData) {
        description.width = args->pCreateStagingSurfaceData->Width;
        description.height = args->pCreateStagingSurfaceData->Height;
        description.kind = QLG_RESOURCE_RGBA8;
        description.format = QLG_FORMAT_BGRA8;
    } else if (args->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE &&
               args->pCreateShadowSurfaceData) {
        /* CDD renders GDI into this lockable copy and Blts it to the primary. */
        description.width = args->pCreateShadowSurfaceData->Width;
        description.height = args->pCreateShadowSurfaceData->Height;
        description.kind = QLG_RESOURCE_RGBA8;
        description.format = QLG_FORMAT_BGRA8;
    } else if (args->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE &&
               args->pCreateSharedPrimarySurfaceData) {
        D3DKMDT_SHAREDPRIMARYSURFACEDATA *primary = args->pCreateSharedPrimarySurfaceData;

        if (primary->Format != D3DDDIFMT_A8R8G8B8 && primary->Format != D3DDDIFMT_X8R8G8B8)
            return STATUS_NOT_SUPPORTED;
        description.width = primary->Width;
        description.height = primary->Height;
        description.kind = QLG_RESOURCE_SHARED_PRIMARY;
        description.format = QLG_FORMAT_BGRA8;
        description.primary_format = (UINT32)primary->Format;
        description.refresh_numerator = primary->RefreshRate.Numerator;
        description.refresh_denominator = primary->RefreshRate.Denominator;
    } else
        return STATUS_NOT_SUPPORTED;

    if (description.width > MAXUINT / QLG_COLOR_BYTES)
        return STATUS_INVALID_PARAMETER;
    description.pitch = description.width * QLG_COLOR_BYTES;
    bytes = (UINT64)description.pitch * description.height;
    description.logical_bytes = bytes;
    if (bytes > QLG_MAX_ALLOCATION_BYTES)
        return STATUS_INVALID_PARAMETER;
    description.bytes = (bytes + QLG_PAGE_BYTES - 1) / QLG_PAGE_BYTES * QLG_PAGE_BYTES;
    if (!qlg_validate_allocation(&description))
        return STATUS_INVALID_PARAMETER;

    if (args->pAllocationPrivateDriverData &&
        args->AllocationPrivateDriverDataSize < sizeof(description))
        return STATUS_BUFFER_TOO_SMALL;
    args->AllocationPrivateDriverDataSize = sizeof(description);
    args->ResourcePrivateDriverDataSize = 0;
    if (args->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE)
        args->pCreateStagingSurfaceData->Pitch = description.pitch;
    if (args->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE)
        args->pCreateShadowSurfaceData->Pitch = description.pitch;
    if (args->pAllocationPrivateDriverData)
        RtlCopyMemory(args->pAllocationPrivateDriverData, &description, sizeof(description));
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgMiniportNodeMetadata(HANDLE handle, UINT ordinal,
                                          DXGKARG_GETNODEMETADATA *args)
{
    static const WCHAR name[] = L"QEMU Lightning graphics";
    UNREFERENCED_PARAMETER(handle);
    if (!args || ordinal != QLG_RENDER_NODE)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(args, sizeof(*args));
    args->EngineType = DXGK_ENGINE_TYPE_3D;
    RtlCopyMemory(args->FriendlyName, name, sizeof(name));
    return STATUS_SUCCESS;
}

/* No calibrated host timestamp/clock pair is exposed by the PCI protocol.
 * Failure leaves every output byte unchanged, including for malformed inputs. */
NTSTATUS APIENTRY QlgMiniportCalibrateGpuClock(HANDLE handle, UINT32 node, UINT32 engine,
                                               DXGKARG_CALIBRATEGPUCLOCK *output)
{
    if (!handle || !output || node != QLG_RENDER_NODE || engine != QLG_ENGINE_ORDINAL)
        return STATUS_INVALID_PARAMETER;
    return STATUS_NOT_SUPPORTED;
}

/* Atomic observations may span concurrent transitions; no locks, pointers or
 * device register reads are exposed to the runtime's level-zero TDR callback. */
void QlgMiniportDiagnosticSnapshot(QLG_MINIPORT *miniport, QLGA_STARTUP_DIAGNOSTICS *output)
{
    RtlZeroMemory(output, sizeof(*output));
    output->magic = QLGA_QUERY_STARTUP_DIAGNOSTICS;
    output->version = QLGA_DIAGNOSTIC_VERSION;
    output->size = sizeof(*output);
    output->start_stage = (UINT)InterlockedCompareExchange(&miniport->start_stage, 0, 0);
    output->start_status = InterlockedCompareExchange(&miniport->start_status, 0, 0);
    output->cleanup_status = InterlockedCompareExchange(&miniport->cleanup_status, 0, 0);
    output->interrupt_count = (UINT)InterlockedCompareExchange(&miniport->interrupt_count, 0, 0);
    output->dpc_count = (UINT)InterlockedCompareExchange(&miniport->dpc_count, 0, 0);
    output->flags = (UINT)InterlockedCompareExchange(&miniport->diagnostic_pnp_flags, 0, 0);
    if (InterlockedCompareExchange(&miniport->interrupts_ready, 0, 0))
        output->flags |= QLGA_DIAGNOSTIC_INTERRUPTS_READY;
}

NTSTATUS APIENTRY QlgMiniportDebug(HANDLE handle, const DXGKARG_COLLECTDBGINFO *args)
{
    QLG_MINIPORT *miniport = handle;
    QLGA_STARTUP_DIAGNOSTICS diagnostics;

    if (!miniport || !args || !args->pBuffer || args->BufferSize < sizeof(diagnostics))
        return STATUS_BUFFER_TOO_SMALL;
    QlgMiniportDiagnosticSnapshot(miniport, &diagnostics);
    RtlCopyMemory(args->pBuffer, &diagnostics, sizeof(diagnostics));
    return STATUS_SUCCESS;
}

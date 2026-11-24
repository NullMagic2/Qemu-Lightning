/* Module: lightning.graphics_driver.miniport_present
 * Purpose: Publish present DMA packets with complete VidMm residency references.
 * Created: 2026-10-11
 * Architecture: WDK Present and Patch callbacks describe and update one CPU
 * present packet; the engine executes its private command in fence order.
 */
#include "miniport.h"
#include "internal.h"

/* Allocation lists carry the per-device handle returned by OpenAllocation. */
static BOOLEAN describe(QLG_PRESENT_SURFACE *surface, const DXGK_ALLOCATIONLIST *entry)
{
    const QLG_OBJECT *opened = entry->hDeviceSpecificAllocation;
    const QLG_OBJECT *allocation = opened && opened->kind == QLG_KIND_OPEN
                                       ? opened->resource : NULL;
    RtlZeroMemory(surface, sizeof(*surface));

    if (!allocation || allocation->kind != QLG_KIND_ALLOCATION)
        return FALSE;
    surface->segment = entry->SegmentId;
    surface->offset = (UINT64)entry->PhysicalAddress.QuadPart;
    surface->pitch = allocation->pitch;
    surface->width = allocation->width;
    surface->height = allocation->height;
    return TRUE;
}

/* Writes one CPU present packet. A flip needs no work: Dxgkrnl programs the
 * address through SetVidPnSourceAddress once the packet retires. */
NTSTATUS APIENTRY QlgMiniportPresent(const HANDLE context, DXGKARG_PRESENT *args)
{
    QLG_PRESENT_COMMAND *command;
    NTSTATUS status = STATUS_SUCCESS;
    QLG_PRESENT_DMA *dma;
    UINT count, index, patches = 0;

    if (!context || !args || !args->pAllocationList)
        return STATUS_INVALID_PARAMETER;

    /* MMIO flips have no DMA packet. The scheduler orders the flip and calls
     * SetVidPnSourceAddress to latch it at the requested vertical blank. */
    if (args->Flags.Flip && args->Flags.FlipWithNoWait &&
        !args->Flags.Blt && !args->Flags.ColorFill &&
        !args->pDmaBuffer && !args->DmaSize &&
        !args->pDmaBufferPrivateData && !args->DmaBufferPrivateDataSize) {
        InterlockedIncrement(&((QLG_MINIPORT *)((const QLG_OBJECT *)context)->adapter->services.context)
                                  ->display.counters[QLG_DISPLAY_PRESENT]);
        return STATUS_SUCCESS;
    }

    if (!args->pDmaBuffer || !args->pDmaBufferPrivateData ||
        (args->SubRectCnt && (!args->pDstSubRects || args->MultipassOffset >= args->SubRectCnt)))
        return STATUS_INVALID_PARAMETER;

    for (index = DXGK_PRESENT_SOURCE_INDEX; index <= DXGK_PRESENT_DESTINATION_INDEX; ++index)
        if (args->pAllocationList[index].hDeviceSpecificAllocation)
            ++patches;

    if (!patches)
        return STATUS_INVALID_PARAMETER;

    if (args->DmaSize < sizeof(*dma) || args->DmaBufferPrivateDataSize < sizeof(*command) ||
        !args->pPatchLocationListOut || args->PatchLocationListOutSize < patches)
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    command = args->pDmaBufferPrivateData;
    /* hContext is the device context object; its adapter services carry the miniport. */
    InterlockedIncrement(&((QLG_MINIPORT *)((const QLG_OBJECT *)context)->adapter->services.context)
                              ->display.counters[QLG_DISPLAY_PRESENT]);
    RtlZeroMemory(command, sizeof(*command));
    if (args->Flags.Flip)
        command->operation = QLG_PRESENT_FLIP;
    else if (args->Flags.ColorFill)
        command->operation = QLG_PRESENT_FILL;
    else if (args->Flags.Blt)
        command->operation = QLG_PRESENT_BLT;
    else
        return STATUS_NOT_SUPPORTED;

    if (command->operation != QLG_PRESENT_FLIP) {
        if (!describe(&command->destination,
                      &args->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX]) ||
            (command->operation == QLG_PRESENT_BLT &&
             !describe(&command->source, &args->pAllocationList[DXGK_PRESENT_SOURCE_INDEX])))
            return STATUS_INVALID_PARAMETER;
        command->color = args->Color;
        command->src_rect = args->SrcRect;
        command->dst_rect = args->DstRect;
        if (!args->SubRectCnt) {
            command->rects[0] = args->DstRect;
            command->rect_count = 1;
        } else {
            count = args->SubRectCnt - args->MultipassOffset;

            if (count > QLG_PRESENT_MAX_RECTS)
                count = QLG_PRESENT_MAX_RECTS;
            RtlCopyMemory(command->rects, args->pDstSubRects + args->MultipassOffset,
                          count * sizeof(RECT));
            command->rect_count = count;
            args->MultipassOffset += count;
            /* Remaining rectangles arrive in another packet built by a new call. */
            command->more_rects = args->MultipassOffset < args->SubRectCnt;

            if (command->more_rects)
                status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
    }
    dma = args->pDmaBuffer;
    RtlZeroMemory(dma, sizeof(*dma));
    dma->operation = command->operation;
    dma->source = (UINT64)args->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].PhysicalAddress.QuadPart;
    dma->destination =
        (UINT64)args->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].PhysicalAddress.QuadPart;

    /* VidMm uses these references for residency even when no repatch is needed. */
    for (index = DXGK_PRESENT_SOURCE_INDEX; index <= DXGK_PRESENT_DESTINATION_INDEX; ++index) {
        D3DDDI_PATCHLOCATIONLIST *patch;

        if (!args->pAllocationList[index].hDeviceSpecificAllocation)
            continue;
        patch = args->pPatchLocationListOut++;
        RtlZeroMemory(patch, sizeof(*patch));
        patch->AllocationIndex = index;
        patch->SlotId = index;
        patch->PatchOffset = index == DXGK_PRESENT_SOURCE_INDEX
                                 ? FIELD_OFFSET(QLG_PRESENT_DMA, source)
                                 : FIELD_OFFSET(QLG_PRESENT_DMA, destination);
    }
    args->pDmaBuffer = dma + 1;
    args->pDmaBufferPrivateData = command + 1;
    return status;
}

/* Rewrites surface offsets after VidMm moved an allocation since Present. */
NTSTATUS QlgDisplayPatch(const DXGKARG_PATCH *args)
{
    QLG_PRESENT_COMMAND *command;
    QLG_PRESENT_DMA *dma;

    if (!args || !args->pDmaBuffer || !args->pDmaBufferPrivateData || !args->pAllocationList ||
        args->DmaBufferSubmissionStartOffset > args->DmaBufferSubmissionEndOffset ||
        args->DmaBufferSubmissionEndOffset > args->DmaBufferSize ||
        args->DmaBufferSubmissionEndOffset - args->DmaBufferSubmissionStartOffset < sizeof(*dma) ||
        args->DmaBufferPrivateDataSubmissionStartOffset >
            args->DmaBufferPrivateDataSubmissionEndOffset ||
        args->DmaBufferPrivateDataSubmissionEndOffset > args->DmaBufferPrivateDataSize)
        return STATUS_INVALID_PARAMETER;

    if (args->DmaBufferPrivateDataSubmissionEndOffset -
            args->DmaBufferPrivateDataSubmissionStartOffset <
        sizeof(*command))
        return STATUS_INVALID_PARAMETER;
    command = (QLG_PRESENT_COMMAND *)((UCHAR *)args->pDmaBufferPrivateData +
                                      args->DmaBufferPrivateDataSubmissionStartOffset);

    dma = (QLG_PRESENT_DMA *)((UCHAR *)args->pDmaBuffer + args->DmaBufferSubmissionStartOffset);

    if (args->AllocationListSize > DXGK_PRESENT_SOURCE_INDEX) {
        command->source.segment = args->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].SegmentId;
        command->source.offset =
            (UINT64)args->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].PhysicalAddress.QuadPart;
        dma->source = command->source.offset;
    }
    if (args->AllocationListSize > DXGK_PRESENT_DESTINATION_INDEX) {
        command->destination.segment =
            args->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].SegmentId;
        command->destination.offset =
            (UINT64)args->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].PhysicalAddress.QuadPart;
        dma->destination = command->destination.offset;
    }
    return STATUS_SUCCESS;
}

/* Module: lightning.graphics_driver.miniport_present_execute
 * Purpose: Copy complete CPU present updates into the guest display allocation.
 * Created: 2026-10-15
 * Architecture: The ordered engine worker executes WDK present packets here;
 * the display timer snapshots only a completed rectangle series.
 */
#include "miniport.h"
#include "internal.h"

/* Present residency references retain aperture mappings through fence retirement. */
static volatile UCHAR *surface_mapping(QLG_ADAPTER *adapter,
                                      const QLG_PRESENT_SURFACE *surface)
{
    UINT64 row_bytes = (UINT64)surface->width * LIGHTNING_SCANOUT_PIXEL_BYTES;
    UINT64 bytes = (UINT64)surface->pitch * surface->height;

    if (!surface->width || !surface->height || surface->pitch < row_bytes)
        return NULL;
    return QlgPresentMapping(adapter, surface->segment, surface->offset, bytes);
}

/* Clamps [low, high) into [0, limit); returns FALSE when empty. */
static BOOLEAN clamp(LONG *low, LONG *high, LONG limit)
{
    if (*low < 0)
        *low = 0;
    if (*high > limit)
        *high = limit;
    return *low < *high;
}

NTSTATUS QlgDisplayExecute(void *context, const QLG_PRESENT_COMMAND *command)
{
    QLG_MINIPORT *miniport = context;
    QLG_ADAPTER *adapter = miniport->lifecycle.adapter;
    const QLG_PRESENT_SURFACE *source = &command->source, *target = &command->destination;
    volatile UCHAR *source_memory, *target_memory;
    BOOLEAN primary;
    LONG dx = command->src_rect.left - command->dst_rect.left,
         dy = command->src_rect.top - command->dst_rect.top;
    LONG left, right, top, bottom, row, column;
    UINT index;
    KIRQL level;

    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_EXECUTE]);
    if (command->operation == QLG_PRESENT_FLIP)
        return STATUS_SUCCESS;

    target_memory = surface_mapping(adapter, target);
    source_memory = command->operation == QLG_PRESENT_BLT ? surface_mapping(adapter, source) : NULL;

    if (!target_memory || (command->operation == QLG_PRESENT_BLT && !source_memory))
        return STATUS_NOT_SUPPORTED;
    level = QlgKernelLock(&miniport->display.scanout_lock);
    primary = target->segment == QLM_MEMORY_SEGMENT &&
              target->offset == miniport->display.address;

    /* A multipass present must retain the previous host image until its last
     * rectangle packet completes. Cancel an older pending snapshot first. */
    if (primary)
        InterlockedExchange(&miniport->display.scanout_pending, FALSE);
    if (primary)
        miniport->display.gpu_resource = 0;

    for (index = 0; index < command->rect_count && index < QLG_PRESENT_MAX_RECTS; ++index) {
        left = command->rects[index].left;
        right = command->rects[index].right;
        top = command->rects[index].top;
        bottom = command->rects[index].bottom;

        if (!clamp(&left, &right, (LONG)target->width) ||
            !clamp(&top, &bottom, (LONG)target->height))
            continue;
        if (command->operation == QLG_PRESENT_BLT) {
            /* Keep the matching source rectangle inside the source surface. */
            if (left + dx < 0)
                left = -dx;

            if (top + dy < 0)
                top = -dy;

            if (right + dx > (LONG)source->width)
                right = (LONG)source->width - dx;

            if (bottom + dy > (LONG)source->height)
                bottom = (LONG)source->height - dy;

            if (left >= right || top >= bottom)
                continue;
        }
        for (row = top; row < bottom; ++row) {
            volatile UCHAR *line = target_memory + (SIZE_T)row * target->pitch;

            if (command->operation == QLG_PRESENT_BLT)
                RtlMoveMemory((void *)(line + (SIZE_T)left * LIGHTNING_SCANOUT_PIXEL_BYTES),
                              (const void *)(source_memory +
                                             (SIZE_T)(row + dy) * source->pitch +
                                             (SIZE_T)(left + dx) * LIGHTNING_SCANOUT_PIXEL_BYTES),
                              (SIZE_T)(right - left) * LIGHTNING_SCANOUT_PIXEL_BYTES);
            else
                for (column = left; column < right; ++column)
                    ((volatile UINT *)line)[column] = command->color;
        }
    }
    if (primary && !command->more_rects)
        InterlockedExchange(&miniport->display.scanout_pending, TRUE);
    QlgKernelUnlock(&miniport->display.scanout_lock, level);
    return STATUS_SUCCESS;
}

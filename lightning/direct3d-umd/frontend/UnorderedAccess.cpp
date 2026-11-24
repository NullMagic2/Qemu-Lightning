/*
 * Module: d3d10umd.UnorderedAccess (Qemu Lightning)
 * Purpose: D3D11 unordered access views: creation, binding for the compute
 *          stage and the output merger, clears, append/consume counters and
 *          CopyStructureCount.
 * Created: 2026-10-16
 * Architecture: See UnorderedAccess.h. Clear values are packed into the
 *          view's format on the CPU and written with clear_buffer or
 *          clear_texture, restricted to the logical 3D view W range.
 */

#include "UnorderedAccess.h"
#include "CounterLayout.h"

#include "Debug.h"
#include "Format.h"
#include "State.h"
#include "Resource.h"

#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_math.h"

/* Raw and structured views are arrays of 32-bit elements. */
static const unsigned RAW_ELEMENT_BYTES = 4;
/* Append/consume counters are one 32-bit value. */
static const unsigned COUNTER_BYTES = 4;
/* The largest format block a clear packs (R32G32B32A32). */
static const unsigned MAX_CLEAR_BYTES = 16;
/* pInitialCounts value that keeps a counter as it is. */
static const UINT KEEP_COUNTER = 0xffffffff;
static const unsigned BITS_PER_BYTE = 8;


/* Opaque Rust pool owns offsets; this concrete Gallium ABI adapter owns
 * GPU storage and ordered migrations. Calls follow the serialized DDI. */
extern "C" {
    QlgCounterPool *qlg_counter_pool_create(uint32_t max_bytes);
    uint32_t qlg_counter_pool_reserve(QlgCounterPool *pool);
    uint32_t qlg_counter_pool_release(QlgCounterPool *pool, uint32_t offset);
    uint32_t qlg_counter_pool_bytes(const QlgCounterPool *pool);
    void qlg_counter_pool_destroy(QlgCounterPool *pool);
}

static mesa_shader_stage
BindPointStage(UavBindPoint point)
{
    return point == UAV_BIND_COMPUTE ? MESA_SHADER_COMPUTE : MESA_SHADER_FRAGMENT;
}

HRESULT
OrderUnorderedAccess(Device *device, mesa_shader_stage stage)
{
    if (stage > MESA_SHADER_COMPUTE)
        return E_INVALIDARG;

    UavBindPoint point = stage == MESA_SHADER_COMPUTE ? UAV_BIND_COMPUTE : UAV_BIND_OUTPUT_MERGER;

    for (unsigned slot = 0; slot < UAV_SLOT_COUNT; ++slot) {
        if (device->uavs[point][slot])
            return OrderResourceAccess(device);
    }
    return S_OK;
}

/* Push shared graphics UAVs to every graphics stage, and CS UAVs to CS.
 * Counter-table words contain byte offsets, so the same view bound through
 * either namespace addresses exactly the same persistent atomic value. */
static void
BindUnorderedAccessViews(Device *device, UavBindPoint point)
{
    struct pipe_context *pipe = device->pipe;
    struct pipe_image_view images[UAV_SLOT_COUNT] = {};
    struct pipe_shader_buffer counters = {};
    uint32_t first_w[SHADER_NIR_MAX_UAV_SLICES] = {};
    uint32_t w_size[SHADER_NIR_MAX_UAV_SLICES] = {};
    uint32_t offsets[QLG_COUNTER_SLOTS] = {};

    static_assert(UAV_SLOT_COUNT == SHADER_NIR_MAX_UAV_SLICES &&
                  UAV_SLOT_COUNT == QLG_COUNTER_SLOTS,
                  "UAV images, counters and slice metadata must cover every slot");

    for (unsigned slot = 0; slot < UAV_SLOT_COUNT; ++slot) {
        const UnorderedAccessView *view = device->uavs[point][slot];

        if (!view)
            continue;
        images[slot] = view->image;
        first_w[slot] = view->first_w;
        w_size[slot] = view->w_size;
        offsets[slot] = view->counter_offset;
    }
    if (device->counter_storage) {
        unsigned table = point == UAV_BIND_COMPUTE ?
                         QLG_COUNTER_COMPUTE_TABLE : QLG_COUNTER_GRAPHICS_TABLE;
        unsigned table_offset = table * sizeof offsets;

        pipe->buffer_subdata(pipe, device->counter_storage, PIPE_MAP_WRITE,
                             table_offset, sizeof offsets, offsets);
        counters.buffer = device->counter_storage;
        counters.buffer_size = device->counter_storage->width0;
    }
    static const mesa_shader_stage graphics[] = {
#if SUPPORT_D3D11_1
        MESA_SHADER_VERTEX, MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL,
        MESA_SHADER_GEOMETRY,
#endif
        MESA_SHADER_FRAGMENT,
    };
    unsigned stages = point == UAV_BIND_COMPUTE ? 1 : ARRAY_SIZE(graphics);

    for (unsigned index = 0; index < stages; ++index) {
        mesa_shader_stage stage = point == UAV_BIND_COMPUTE ? MESA_SHADER_COMPUTE : graphics[index];

        BindUavSliceConstants(pipe, stage, first_w, w_size, &device->bindless_textures[stage]);
        /* Respect the pipe's actual array size even with a mismatched old
         * backend. Shader creation separately rejects inaccessible slots. */
        unsigned slots = MIN2(UAV_SLOT_COUNT, pipe->screen->shader_caps[stage].max_shader_images);

        pipe->set_shader_images(pipe, stage, 0, slots, 0, images);
        pipe->set_shader_buffers(pipe, stage, QLG_COUNTER_SSBO_BINDING, 1, &counters, 1);
    }
}

/* Grow without changing any view offset. The old arena stays referenced by
 * queued commands until Gallium has completed the migration. No CPU readback
 * participates in append/consume operations or CopyStructureCount. */
static bool
AllocateCounter(Device *device, UnorderedAccessView *view)
{
    struct pipe_context *pipe = device->pipe;

    if (!device->counter_pool)
        device->counter_pool = qlg_counter_pool_create(pipe->screen->caps.max_shader_buffer_size);
    if (!device->counter_pool)
        return false;

    uint32_t offset = qlg_counter_pool_reserve(device->counter_pool);

    if (!offset)
        return false;

    unsigned bytes = qlg_counter_pool_bytes(device->counter_pool);

    if (!device->counter_storage || device->counter_storage->width0 < bytes) {
        struct pipe_resource *storage = pipe_buffer_create(pipe->screen, PIPE_BIND_SHADER_BUFFER,
                                                           PIPE_USAGE_DEFAULT, bytes);

        if (!storage) {
            qlg_counter_pool_release(device->counter_pool, offset);
            return false;
        }
        if (device->counter_storage) {
            struct pipe_box box;

            if (FAILED(OrderResourceAccess(device))) {
                pipe_resource_reference(&storage, NULL);
                qlg_counter_pool_release(device->counter_pool, offset);
                return false;
            }
            u_box_1d(0, device->counter_storage->width0, &box);
            pipe->resource_copy_region(pipe, storage, 0, 0, 0, 0,
                                       device->counter_storage, 0, &box);
        }
        pipe_resource_reference(&device->counter_storage, storage);
        pipe_resource_reference(&storage, NULL);
        BindUnorderedAccessViews(device, UAV_BIND_COMPUTE);
        BindUnorderedAccessViews(device, UAV_BIND_OUTPUT_MERGER);
    }
    const UINT zero = 0;

    pipe->buffer_subdata(pipe, device->counter_storage, PIPE_MAP_WRITE,
                         offset, COUNTER_BYTES, &zero);
    view->counter_offset = offset;
    return true;
}

static void
SetCounter(Device *device, UnorderedAccessView *view, UINT value)
{
    if (view && view->counter_offset && value != KEEP_COUNTER)
        device->pipe->buffer_subdata(device->pipe, device->counter_storage, PIPE_MAP_WRITE,
                                     view->counter_offset, COUNTER_BYTES, &value);
}


SIZE_T APIENTRY
CalcPrivateUnorderedAccessViewSize(D3D10DDI_HDEVICE hDevice,
                                   const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *pCreate)
{
    return sizeof(UnorderedAccessView);
}


void APIENTRY
CreateUnorderedAccessView(D3D10DDI_HDEVICE hDevice,
                          const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *pCreate,
                          D3D11DDI_HUNORDEREDACCESSVIEW hView,
                          D3D11DDI_HRTUNORDEREDACCESSVIEW hRTView)
{
    LOG_ENTRYPOINT();

    struct pipe_context *pipe = CastPipeContext(hDevice);
    struct pipe_screen *screen = pipe->screen;
    UnorderedAccessView *view = CastUnorderedAccessView(hView);
    Resource *owner = CastResource(pCreate->hDrvResource);
    struct pipe_resource *resource = owner ? owner->resource : NULL;
    struct pipe_image_view image;

    memset(view, 0, sizeof *view);
    memset(&image, 0, sizeof image);
    if (!resource) {
        SetError(hDevice, E_INVALIDARG);
        return;
    }
    image.access = PIPE_IMAGE_ACCESS_READ_WRITE;
    image.shader_access = PIPE_IMAGE_ACCESS_READ_WRITE;

    switch (pCreate->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER: {
        UINT flags = pCreate->Buffer.Flags;
        unsigned element_bytes;

        if (flags & D3D11_DDI_BUFFER_UAV_FLAG_RAW) {
            image.format = PIPE_FORMAT_R32_UINT;
            element_bytes = RAW_ELEMENT_BYTES;
            view->raw_or_structured = true;
        } else if (pCreate->Format == DXGI_FORMAT_UNKNOWN) {
            if (!owner->structure_stride || owner->structure_stride % RAW_ELEMENT_BYTES) {
                SetError(hDevice, E_INVALIDARG);
                return;
            }
            image.format = PIPE_FORMAT_R32_UINT;
            element_bytes = owner->structure_stride;
            view->raw_or_structured = true;
        } else {
            image.format = FormatTranslate(pCreate->Format, false);
            element_bytes = util_format_get_blocksize(image.format);
        }
        image.u.buf.offset = pCreate->Buffer.FirstElement * element_bytes;
        image.u.buf.size = pCreate->Buffer.NumElements * element_bytes;

        break;
    }
    case D3D10DDIRESOURCE_TEXTURE1D:
    case D3D10DDIRESOURCE_TEXTURE2D:
        /* Tex1D and Tex2D share one layout. */
        image.format = FormatTranslate(pCreate->Format, false);
        image.u.tex.level = pCreate->Tex2D.MipSlice;
        image.u.tex.first_layer = pCreate->Tex2D.FirstArraySlice;
        image.u.tex.last_layer = pCreate->Tex2D.FirstArraySlice + pCreate->Tex2D.ArraySize - 1;
        break;
    case D3D10DDIRESOURCE_TEXTURE3D: {
        unsigned level = pCreate->Tex3D.MipSlice;
        unsigned first = pCreate->Tex3D.FirstW;
        unsigned depth;
        unsigned count;

        if (resource->target != PIPE_TEXTURE_3D || level > resource->last_level) {
            SetError(hDevice, E_INVALIDARG);
            return;
        }
        depth = u_minify(resource->depth0, level);
        if (first >= depth) {
            SetError(hDevice, E_INVALIDARG);
            return;
        }
        count = pCreate->Tex3D.WSize == (UINT)-1 ? depth - first : pCreate->Tex3D.WSize;
        if (!count || count > depth - first) {
            SetError(hDevice, E_INVALIDARG);
            return;
        }
        image.format = FormatTranslate(pCreate->Format, false);
        image.u.tex.level = level;
        /* Keep a genuine 3D image type. NIR translates view-relative W and
         * rejects accesses outside this logical range before image access. */
        image.u.tex.first_layer = 0;
        image.u.tex.last_layer = depth - 1;
        view->first_w = first;
        view->w_size = count;
        break;
    }
    default:
        SetError(hDevice, E_INVALIDARG);
        return;
    }

    if (image.format == PIPE_FORMAT_NONE ||
        !screen->is_format_supported(screen, image.format, resource->target, 0, 0,
                                     PIPE_BIND_SHADER_IMAGE)) {
        DebugPrintf("%s: format %s cannot be a storage image\n", __func__,
                    util_format_name(image.format));
        SetError(hDevice, E_OUTOFMEMORY);
        return;
    }

    if (pCreate->ResourceDimension == D3D10DDIRESOURCE_BUFFER &&
        (pCreate->Buffer.Flags & (D3D11_DDI_BUFFER_UAV_FLAG_APPEND | D3D11_DDI_BUFFER_UAV_FLAG_COUNTER)) &&
        !AllocateCounter(CastDevice(hDevice), view)) {
        SetError(hDevice, E_OUTOFMEMORY);
        return;
    }

    pipe_resource_reference(&image.resource, resource);
    view->image = image;
    view->owner = owner;
}


void APIENTRY
DestroyUnorderedAccessView(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hView)
{
    LOG_ENTRYPOINT();

    Device *pDevice = CastDevice(hDevice);
    UnorderedAccessView *view = CastUnorderedAccessView(hView);

    /* Never leave a destroyed view bound. */
    for (unsigned point = 0; point < UAV_BIND_POINT_COUNT; point++) {
        bool bound = false;

        for (unsigned slot = 0; slot < UAV_SLOT_COUNT; slot++) {
            if (pDevice->uavs[point][slot] == view) {
                pDevice->uavs[point][slot] = NULL;
                bound = true;
            }
        }
        if (bound)
            BindUnorderedAccessViews(pDevice, (UavBindPoint)point);
    }
    pipe_resource_reference(&view->image.resource, NULL);
    if (view->counter_offset) {
        qlg_counter_pool_release(pDevice->counter_pool, view->counter_offset);
        view->counter_offset = 0;
    }
}


void APIENTRY
CsSetUnorderedAccessViews(D3D10DDI_HDEVICE hDevice, UINT StartSlot, UINT NumViews,
                          const D3D11DDI_HUNORDEREDACCESSVIEW *phViews,
                          const UINT *pInitialCounts)
{
    LOG_ENTRYPOINT();

    Device *pDevice = CastDevice(hDevice);

    for (UINT i = 0; i < NumViews && StartSlot + i < UAV_SLOT_COUNT; i++) {
        UnorderedAccessView *view = CastUnorderedAccessView(phViews[i]);

        pDevice->uavs[UAV_BIND_COMPUTE][StartSlot + i] = view;
        if (pInitialCounts)
            SetCounter(pDevice, view, pInitialCounts[i]);
    }
    BindUnorderedAccessViews(pDevice, UAV_BIND_COMPUTE);
}


void
SetOutputMergerUnorderedAccessViews(D3D10DDI_HDEVICE hDevice,
                                    const D3D11DDI_HUNORDEREDACCESSVIEW *phViews,
                                    const UINT *pInitialCounts, UINT StartSlot, UINT NumViews,
                                    UINT RangeStart, UINT RangeSize)
{
    Device *pDevice = CastDevice(hDevice);
    UnorderedAccessView **bound = pDevice->uavs[UAV_BIND_OUTPUT_MERGER];

    if (!NumViews && !RangeSize)
        return;
    for (UINT slot = RangeStart; slot < RangeStart + RangeSize && slot < UAV_SLOT_COUNT; slot++)
        bound[slot] = NULL;
    for (UINT i = 0; i < NumViews && StartSlot + i < UAV_SLOT_COUNT; i++) {
        UnorderedAccessView *view = CastUnorderedAccessView(phViews[i]);

        bound[StartSlot + i] = view;
        if (pInitialCounts)
            SetCounter(pDevice, view, pInitialCounts[i]);
    }
    BindUnorderedAccessViews(pDevice, UAV_BIND_OUTPUT_MERGER);
}


void
UnbindUnorderedAccessViews(Device *pDevice)
{
    memset(pDevice->uavs, 0, sizeof pDevice->uavs);
    for (unsigned point = 0; point < UAV_BIND_POINT_COUNT; point++)
        BindUnorderedAccessViews(pDevice, (UavBindPoint)point);
    pipe_resource_reference(&pDevice->counter_storage, NULL);
    qlg_counter_pool_destroy(pDevice->counter_pool);
    pDevice->counter_pool = NULL;
}


/* Write each value's low bits into its channel, as ClearUAVUint does on
 * formats that are not pure integer. */
static void
PackChannelBits(const struct util_format_description *desc, const UINT values[4],
                uint8_t *element)
{
    for (unsigned channel = 0; channel < desc->nr_channels; channel++) {
        unsigned size = desc->channel[channel].size;
        unsigned shift = desc->channel[channel].shift;
        unsigned component = 0;

        /* The RGBA component that reads this channel. */
        while (component < 4 && desc->swizzle[component] != channel)
            component++;
        if (component == 4 || !size)
            continue;
        for (unsigned bit = 0; bit < size && bit < 32; bit++) {
            unsigned position = shift + bit;

            if (values[component] & (1u << bit))
                element[position / BITS_PER_BYTE] |= 1u << (position % BITS_PER_BYTE);
        }
    }
}


/* Fill the view, or one rectangle of it (D3D11.1 ClearView: elements of
 * a buffer view, texels of a texture view, clipped), with one packed
 * element. */
static void
ClearView(struct pipe_context *pipe, const UnorderedAccessView *view, const uint8_t *element,
          unsigned element_bytes, const D3D10_DDI_RECT *rect = NULL)
{
    struct pipe_resource *resource = view->image.resource;

    if (resource->target == PIPE_BUFFER) {
        unsigned offset = view->image.u.buf.offset, size = view->image.u.buf.size;

        if (rect) {
            unsigned elements = size / element_bytes;
            unsigned first = MIN2((unsigned)MAX2(rect->left, 0), elements);
            unsigned last = MIN2((unsigned)MAX2(rect->right, 0), elements);

            if (first >= last)
                return;
            offset += first * element_bytes;
            size = (last - first) * element_bytes;
        }
        pipe->clear_buffer(pipe, resource, offset, size, element, element_bytes);
        return;
    }

    unsigned level = view->image.u.tex.level;
    struct pipe_box box;

    memset(&box, 0, sizeof box);
    box.width = u_minify(resource->width0, level);
    box.height = resource->target == PIPE_TEXTURE_1D_ARRAY ? 1 : u_minify(resource->height0, level);
    if (resource->target == PIPE_TEXTURE_3D) {
        box.z = view->first_w;
        box.depth = view->w_size;
    } else {
        box.z = view->image.u.tex.first_layer;
        box.depth = view->image.u.tex.last_layer - view->image.u.tex.first_layer + 1;
        if (resource->target == PIPE_TEXTURE_1D_ARRAY) {
            box.y = box.z;
            box.height = box.depth;
            box.z = 0;
            box.depth = 1;
        }
    }
    if (rect) {
        int left = MAX2(rect->left, 0), top = MAX2(rect->top, 0);
        int right = MIN2(rect->right, box.width);
        int bottom = MIN2(rect->bottom, resource->target == PIPE_TEXTURE_1D_ARRAY ? 1 : box.height);

        if (left >= right || top >= bottom)
            return;
        box.x = left;
        box.width = right - left;
        if (resource->target != PIPE_TEXTURE_1D_ARRAY) {
            box.y = top;
            box.height = bottom - top;
        }
    }
    pipe->clear_texture(pipe, resource, level, &box, element);
}


void APIENTRY
ClearUnorderedAccessViewUint(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hView,
                             const UINT Values[4])
{
    LOG_ENTRYPOINT();

    struct pipe_context *pipe = CastPipeContext(hDevice);
    const UnorderedAccessView *view = CastUnorderedAccessView(hView);
    enum pipe_format format = view->image.format;
    uint8_t element[MAX_CLEAR_BYTES];
    unsigned element_bytes = util_format_get_blocksize(format);

    memset(element, 0, sizeof element);
    if (view->raw_or_structured) {
        /* Raw and structured views clear every dword to the first value. */
        memcpy(element, &Values[0], RAW_ELEMENT_BYTES);
        element_bytes = RAW_ELEMENT_BYTES;
    } else if (util_format_is_pure_integer(format)) {
        util_format_pack_rgba(format, element, Values, 1);
    } else {
        PackChannelBits(util_format_description(format), Values, element);
    }
    ClearView(pipe, view, element, element_bytes);
}


/* A float clear value as one element of the view's format. */
static unsigned
FloatClearElement(const UnorderedAccessView *view, const FLOAT Values[4], uint8_t *element)
{
    enum pipe_format format = view->image.format;
    unsigned element_bytes = util_format_get_blocksize(format);

    memset(element, 0, MAX_CLEAR_BYTES);
    if (view->raw_or_structured || util_format_is_pure_integer(format)) {
        /* Defined for float and normalized views only; integers take the
         * converted value. */
        UINT converted[4];

        LOG_UNSUPPORTED(true);
        for (unsigned c = 0; c < 4; c++)
            converted[c] = (UINT)Values[c];
        if (view->raw_or_structured) {
            memcpy(element, &converted[0], RAW_ELEMENT_BYTES);
            element_bytes = RAW_ELEMENT_BYTES;
        } else {
            util_format_pack_rgba(format, element, converted, 1);
        }
    } else {
        util_format_pack_rgba(format, element, Values, 1);
    }
    return element_bytes;
}


void APIENTRY
ClearUnorderedAccessViewFloat(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hView,
                              const FLOAT Values[4])
{
    LOG_ENTRYPOINT();

    const UnorderedAccessView *view = CastUnorderedAccessView(hView);
    uint8_t element[MAX_CLEAR_BYTES];
    unsigned element_bytes = FloatClearElement(view, Values, element);

    ClearView(CastPipeContext(hDevice), view, element, element_bytes);
}


/* D3D11.1 ClearView on a UAV: the float value, per rectangle. */
void
ClearUnorderedAccessViewRects(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hView,
                              const FLOAT Values[4], const D3D10_DDI_RECT *pRects, UINT NumRects)
{
    const UnorderedAccessView *view = CastUnorderedAccessView(hView);
    uint8_t element[MAX_CLEAR_BYTES];
    unsigned element_bytes = FloatClearElement(view, Values, element);
    struct pipe_context *pipe = CastPipeContext(hDevice);

    if (!pRects || !NumRects) {
        ClearView(pipe, view, element, element_bytes);
        return;
    }
    for (UINT i = 0; i < NumRects; i++)
        ClearView(pipe, view, element, element_bytes, &pRects[i]);
}


void APIENTRY
CopyStructureCount(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDstBuffer,
                   UINT DstAlignedByteOffset, D3D11DDI_HUNORDEREDACCESSVIEW hSrcView)
{
    LOG_ENTRYPOINT();

    struct pipe_context *pipe = CastPipeContext(hDevice);
    const UnorderedAccessView *view = CastUnorderedAccessView(hSrcView);
    struct pipe_resource *destination = CastPipeResource(hDstBuffer);
    struct pipe_box box;

    if (!view || !view->counter_offset || !destination) {
        LOG_UNSUPPORTED(true);
        return;
    }
    u_box_1d(view->counter_offset, COUNTER_BYTES, &box);
    pipe->resource_copy_region(pipe, destination, 0, DstAlignedByteOffset, 0, 0,
                               CastDevice(hDevice)->counter_storage, 0, &box);
}

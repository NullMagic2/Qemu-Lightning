/* Module: qemu_lightning.direct3d_umd.command_lists
 * Purpose: Marshal native D3D11 deferred calls without executing them early.
 * Created: 2026-11-07
 * Architecture: Typed Windows DDI adapters resolve context-local aliases and
 * snapshot concrete argument layouts into the Rust command recording owner.
 */

#include <array>
#include <initializer_list>
#include <limits>
#include <type_traits>
#include <tuple>
#include <utility>

#include "CommandLists.h"
#include "State.h"
#include "CommandJournal.h"
#include "Debug.h"
#include "Device.h"
#include "Draw.h"
#include "Resource.h"
#include "UnorderedAccess.h"
#include "DeviceLoss.h"
#include "util/format/u_format.h"

namespace {
constexpr UINT NativeHandleCount = D3D11DDI_HT_UNORDEREDACCESSVIEW + 1;
constexpr size_t MaxSnapshotBytes = QLG_COMMAND_MAX_PACKET_BYTES;
constexpr UINT DefaultSampleMask = ~UINT(0);
constexpr unsigned ColourWords = 4;

/* Runtime context-local handles point at this alias, never a backend object.
 * Microsoft guarantees the immediate object is opened first and closed last;
 * command-list runtime references keep its immediate handle alive for replay.
 * Resource layout is immutable metadata, copied when its local handle opens. */
struct DeferredAlias {
    void *immediate;
    pipe_format format;
    UINT width, height, depth, levels, subresources;
    bool buffer;
};

struct CommandList {
    QlgCommandJournal *journal;
    Device *owner;
};

template <class T, class = void> struct IsHandle : std::false_type {};
template <class T>
struct IsHandle<T, std::void_t<decltype(std::declval<T>().pDrvPrivate)>> : std::true_type {};

template <class T> T ImmediateHandle(T handle)
{
    if (handle.pDrvPrivate)
        handle.pDrvPrivate = static_cast<DeferredAlias *>(handle.pDrvPrivate)->immediate;

    return handle;
}

template <class T> uint64_t Word(T value)
{
    static_assert(sizeof(T) <= sizeof(uint64_t), "DDI scalar must fit the journal word");

    if constexpr (IsHandle<T>::value)
        value = ImmediateHandle(value);

    uint64_t word = 0;
    memcpy(&word, &value, sizeof value);
    return word;
}

template <class T> T Scalar(uint64_t word)
{
    T value;
    memcpy(&value, &word, sizeof value);
    return value;
}

template <class T>
void TransformHandles(uint8_t *destination, const uint8_t *source, size_t bytes)
{
    T *output = reinterpret_cast<T *>(destination);
    const T *input = reinterpret_cast<const T *>(source);

    for (size_t i = 0; i < bytes / sizeof(T); ++i)
        output[i] = ImmediateHandle(input[i]);
}

enum class CountKind { One, Four, Argument0, Argument1, Argument7, Upload };
struct PointerRule { size_t index; CountKind count; };
struct Recipe {
    CountKind counts[QLG_COMMAND_MAX_ARGS] = {};

    constexpr Recipe(std::initializer_list<PointerRule> rules)
    {
        for (PointerRule rule : rules)
            counts[rule.index] = rule.count;
    }
};

constexpr Recipe Single({});
constexpr Recipe Binding({{2, CountKind::Argument1}});
constexpr Recipe VertexBuffers({{2, CountKind::Argument1}, {3, CountKind::Argument1},
                               {4, CountKind::Argument1}});
constexpr Recipe Targets({{0, CountKind::Argument1}, {4, CountKind::Argument7},
                         {5, CountKind::Argument7}});
constexpr Recipe StreamTargets({{2, CountKind::Argument0}, {3, CountKind::Argument0}});
constexpr Recipe Viewports({{2, CountKind::Argument0}});
constexpr Recipe Colour({{1, CountKind::Four}});
constexpr Recipe Interfaces({{2, CountKind::Argument1}, {3, CountKind::Argument1}});
constexpr Recipe Update({{3, CountKind::Upload}});
constexpr Recipe CounterBindings({{2, CountKind::Argument1}, {3, CountKind::Argument1}});

void RecordingError(Device *context, HRESULT error)
{
    context->deferred_failed = true;
    context->recording_error = error;
    SetError(D3D10DDI_HDEVICE{context}, error);
}

bool Multiply(size_t a, size_t b, size_t *output)
{
    if (a && b > MaxSnapshotBytes / a)
        return false;

    *output = a * b;
    return *output <= MaxSnapshotBytes;
}

/* Exact source byte extent, including pitches but excluding trailing padding.
 * The runtime has already validated the D3D call. Bounds are still checked
 * here before snapshots allocate or read caller memory. */
size_t UploadBytes(const DeferredAlias *resource, UINT subresource,
                   const D3D10_DDI_BOX *box, UINT row_pitch, UINT depth_pitch)
{
    if (!resource || !resource->levels || subresource >= resource->subresources)
        return 0;

    const UINT mip = subresource % resource->levels;
    UINT width = u_minify(resource->width, mip);
    UINT height = u_minify(resource->height, mip);
    UINT depth = u_minify(resource->depth, mip);

    if (box) {
        if (box->right < box->left || box->bottom < box->top || box->back < box->front)
            return 0;

        width = box->right - box->left;
        height = box->bottom - box->top;
        depth = box->back - box->front;
    }

    if (resource->buffer)
        return width;

    const size_t rows = util_format_get_nblocksy(resource->format, height);
    const size_t row_bytes = util_format_get_stride(resource->format, width);
    size_t slices, row_offsets;

    if (!width || !height || !depth || row_pitch < row_bytes ||
        !Multiply(rows - 1, row_pitch, &row_offsets) ||
        !Multiply(depth - 1, depth_pitch, &slices) ||
        slices > MaxSnapshotBytes - row_offsets ||
        row_bytes > MaxSnapshotBytes - slices - row_offsets)
        return 0;

    return slices + row_offsets + row_bytes;
}

template <class T>
bool Snapshot(T value, size_t index, const Recipe &recipe, const uint64_t *words,
              const DeferredAlias *upload_resource, QlgCommandInput *input)
{
    if constexpr (!std::is_pointer<T>::value) {
        return true;
    } else {
        if (!value)
            return true;

        using Element = typename std::remove_cv<typename std::remove_pointer<T>::type>::type;
        size_t count = 1;

        switch (recipe.counts[index]) {
        case CountKind::Four: count = ColourWords; break;
        case CountKind::Argument0: count = words[0]; break;
        case CountKind::Argument1: count = words[1]; break;
        case CountKind::Argument7: count = words[7]; break;
        default: break;
        }

        if constexpr (std::is_void<Element>::value) {
            if (recipe.counts[index] != CountKind::Upload)
                return false;

            input->bytes = UploadBytes(upload_resource, UINT(words[1]),
                Scalar<const D3D10_DDI_BOX *>(words[2]), UINT(words[4]), UINT(words[5]));

            if (!input->bytes)
                return false;
        } else {
            if (!Multiply(count, sizeof(Element), &input->bytes))
                return false;

            if constexpr (IsHandle<Element>::value)
                input->transform = TransformHandles<Element>;
        }
        input->data = reinterpret_cast<const uint8_t *>(value);
        return true;
    }
}

template <class T> T Argument(uint64_t word, const QlgCommandRegion &region)
{
    if constexpr (std::is_pointer<T>::value)
        return reinterpret_cast<T>(const_cast<uint8_t *>(region.data));
    else
        return Scalar<T>(word);
}

template <class Signature, auto Member, const Recipe *Layout> struct Recorder;

template <class... Args, void (APIENTRY *D3D11DDI_DEVICEFUNCS::*Member)(D3D10DDI_HDEVICE, Args...),
          const Recipe *Layout>
struct Recorder<void (APIENTRY *)(D3D10DDI_HDEVICE, Args...), Member, Layout> {
    template <size_t... I>
    static int Replay(uintptr_t owner, const uint64_t *words,
                      const QlgCommandRegion *regions, std::index_sequence<I...>)
    {
        Device *device = reinterpret_cast<Device *>(owner);
        auto function = device->command_functions->*Member;
        function(D3D10DDI_HDEVICE{device}, Argument<Args>(words[I], regions[I])...);
        return DeviceRemoved(device) ? -1 : 1;
    }

    static int Invoke(uintptr_t owner, const uint64_t *words, const QlgCommandRegion *regions)
    {
        return Replay(owner, words, regions, std::index_sequence_for<Args...>{});
    }

    template <size_t... I>
    static bool Copy(std::array<QlgCommandInput, sizeof...(Args)> &inputs,
                     const uint64_t *words, const DeferredAlias *upload_resource,
                     std::index_sequence<I...>, Args... args)
    {
        return (Snapshot(args, I, *Layout, words, upload_resource, &inputs[I]) && ...);
    }

    static void APIENTRY Call(D3D10DDI_HDEVICE handle, Args... args)
    {
        Device *context = CastDevice(handle);

        if (!context->command_recording || context->deferred_failed)
            return;

        std::array<uint64_t, sizeof...(Args)> words = {{Word(args)...}};
        std::array<QlgCommandInput, sizeof...(Args)> inputs = {};
        const DeferredAlias *resource = nullptr;

        if constexpr (Layout == &Update) {
            // Keep the original local handle long enough to read its immutable layout.
            auto values = std::make_tuple(args...);
            resource = static_cast<DeferredAlias *>(std::get<0>(values).pDrvPrivate);
        }

        if (!Copy(inputs, words.data(), resource, std::index_sequence_for<Args...>{}, args...)) {
            RecordingError(context, E_INVALIDARG);
            return;
        }

        const int status = qlg_commands_append(context->command_recording, Invoke,
            words.data(), words.size(), inputs.data());

        if (status != 1)
            RecordingError(context, status == 0 ? E_OUTOFMEMORY : E_INVALIDARG);
    }
};

template <class Signature> struct AliasCreate;
template <class Description, class Handle, class Runtime, class... Rest>
struct AliasCreate<void (APIENTRY *)(D3D10DDI_HDEVICE, Description, Handle, Runtime, Rest...)> {
    static void APIENTRY Call(D3D10DDI_HDEVICE context, Description, Handle handle,
                              Runtime runtime, Rest...)
    {
        DeferredAlias *alias = static_cast<DeferredAlias *>(handle.pDrvPrivate);
        memset(alias, 0, sizeof *alias);
        alias->immediate = reinterpret_cast<void *>(runtime.handle);

        if constexpr (std::is_same<Handle, D3D10DDI_HRESOURCE>::value) {
            const Resource *resource = static_cast<const Resource *>(alias->immediate);

            if (!resource || !resource->resource) {
                RecordingError(CastDevice(context), E_INVALIDARG);
                return;
            }

            const pipe_resource *pipe = resource->resource;
            alias->format = pipe->format;
            alias->width = pipe->width0;
            alias->height = pipe->height0;
            alias->depth = pipe->target == PIPE_TEXTURE_3D ? pipe->depth0 : 1;
            alias->levels = resource->MipLevels;
            alias->subresources = resource->NumSubResources;
            alias->buffer = resource->buffer;
        }
    }
};

template <class Handle>
void APIENTRY CloseAlias(D3D10DDI_HDEVICE, Handle handle)
{
    memset(handle.pDrvPrivate, 0, sizeof(DeferredAlias));
}

/* A dynamic map writes private CPU bytes. Replay performs the actual Map/Unmap
 * in command order, so recording neither renames nor changes a GPU resource. */
int Upload(uintptr_t owner, const uint64_t *words, const QlgCommandRegion *regions)
{
    Device *device = reinterpret_cast<Device *>(owner);
    D3D10DDI_HDEVICE handle = {device};
    const D3D10DDI_HRESOURCE resource = Scalar<D3D10DDI_HRESOURCE>(words[0]);
    const UINT subresource = UINT(words[1]);
    D3D10DDI_MAPPED_SUBRESOURCE mapped = {};
    const uint64_t errors = device->error_serial;
    device->command_functions->pfnResourceMap(handle, resource, subresource,
        D3D10_DDI_MAP_WRITE_DISCARD, 0, &mapped);

    if (device->error_serial != errors || !mapped.pData)
        return -1;

    const UINT row_bytes = UINT(words[2]), rows = UINT(words[3]), slices = UINT(words[4]);
    const UINT source_depth_pitch = row_bytes * rows;

    for (UINT z = 0; z < slices; ++z) {
        for (UINT y = 0; y < rows; ++y) {
            memcpy(static_cast<uint8_t *>(mapped.pData) + z * mapped.DepthPitch + y * mapped.RowPitch,
                   regions[0].data + z * source_depth_pitch + y * row_bytes, row_bytes);
        }
    }
    device->command_functions->pfnResourceUnmap(handle, resource, subresource);
    return DeviceRemoved(device) ? -1 : 1;
}

void APIENTRY DeferredMap(D3D10DDI_HDEVICE handle, D3D10DDI_HRESOURCE local,
                          UINT subresource, D3D10_DDI_MAP access, UINT,
                          D3D10DDI_MAPPED_SUBRESOURCE *mapped)
{
    Device *context = CastDevice(handle);
    DeferredAlias *resource = static_cast<DeferredAlias *>(local.pDrvPrivate);
    memset(mapped, 0, sizeof *mapped);

    if (!context->command_recording || context->deferred_failed || !resource ||
        !resource->levels || subresource >= resource->subresources ||
        (access != D3D10_DDI_MAP_WRITE_DISCARD && access != D3D10_DDI_MAP_WRITE_NOOVERWRITE) ||
        (access == D3D10_DDI_MAP_WRITE_NOOVERWRITE && !resource->buffer)) {
        RecordingError(context, E_INVALIDARG);
        return;
    }

    const UINT mip = subresource % resource->levels;
    const UINT width = u_minify(resource->width, mip);
    const UINT row_bytes = resource->buffer ? width : util_format_get_stride(resource->format, width);
    const UINT rows = resource->buffer ? 1 : util_format_get_nblocksy(resource->format, u_minify(resource->height, mip));
    const UINT slices = u_minify(resource->depth, mip);
    size_t slice_bytes, bytes;

    if (!Multiply(row_bytes, rows, &slice_bytes) || !Multiply(slice_bytes, slices, &bytes)) {
        RecordingError(context, E_OUTOFMEMORY);
        return;
    }

    const D3D10DDI_HRESOURCE immediate = ImmediateHandle(local);
    const uint64_t words[] = {Word(immediate.pDrvPrivate), subresource, row_bytes, rows, slices};
    uint8_t *data = nullptr;
    int status = qlg_commands_map(context->command_recording,
        reinterpret_cast<uintptr_t>(resource->immediate), subresource, bytes,
        access == D3D10_DDI_MAP_WRITE_DISCARD, Upload, words, ARRAYSIZE(words), &data);

    if (status != 1) {
        RecordingError(context, status == 0 ? E_OUTOFMEMORY : E_INVALIDARG);
        return;
    }
    mapped->pData = data;
    mapped->RowPitch = row_bytes;
    mapped->DepthPitch = UINT(slice_bytes);
}

void APIENTRY DeferredUnmap(D3D10DDI_HDEVICE handle, D3D10DDI_HRESOURCE local, UINT subresource)
{
    Device *context = CastDevice(handle);
    const DeferredAlias *resource = static_cast<const DeferredAlias *>(local.pDrvPrivate);

    if (!resource || qlg_commands_unmap(context->command_recording,
        reinterpret_cast<uintptr_t>(resource->immediate), subresource) != 1)
        RecordingError(context, E_INVALIDARG);
}

void ClearPipeline(Device *device)
{
    D3D11DDI_DEVICEFUNCS *functions = device->command_functions;
    D3D10DDI_HDEVICE handle = {device};
    D3D10DDI_HRESOURCE resources[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    D3D10DDI_HSHADERRESOURCEVIEW views[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    D3D10DDI_HSAMPLER samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    UINT zeros[PIPE_MAX_ATTRIBS] = {};
    const FLOAT blend[ColourWords] = {1, 1, 1, 1};

#define CLEAR_STAGE(stage) \
    functions->pfn##stage##SetShader(handle, D3D10DDI_HSHADER{}); \
    functions->pfn##stage##SetShaderResources(handle, 0, ARRAYSIZE(views), views); \
    functions->pfn##stage##SetSamplers(handle, 0, ARRAYSIZE(samplers), samplers); \
    functions->pfn##stage##SetConstantBuffers(handle, 0, \
        D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, resources)
    CLEAR_STAGE(Vs);
    CLEAR_STAGE(Ps);
    CLEAR_STAGE(Gs);
    CLEAR_STAGE(Hs);
    CLEAR_STAGE(Ds);
    CLEAR_STAGE(Cs);
#undef CLEAR_STAGE

    // Retire backend texture bindings too, including bindless references.
    const pipe_constant_buffer empty = {};
    void *empty_samplers[PIPE_MAX_SAMPLERS] = {};
    static const mesa_shader_stage stages[] = {
        MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, MESA_SHADER_GEOMETRY,
        MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL, MESA_SHADER_COMPUTE
    };

    for (mesa_shader_stage stage : stages) {
        if (DeviceRemoved(device))
            return;

        BindStageResources(device, stage);
        device->pipe->bind_sampler_states(device->pipe, stage, 0,
                                          PIPE_MAX_SAMPLERS, empty_samplers);
        memset(device->bound_samplers[stage], 0, sizeof device->bound_samplers[stage]);
        device->pipe->set_constant_buffer(device->pipe, stage,
                                         SHADER_NIR_INTERFACE_DATA_SLOT, &empty);
    }

    functions->pfnSetPredication(handle, D3D10DDI_HQUERY{}, FALSE);
    functions->pfnSetRenderTargets(handle, nullptr, 0, PIPE_MAX_COLOR_BUFS,
        D3D10DDI_HDEPTHSTENCILVIEW{}, nullptr, nullptr, 0, 0, 0, 0);
    if (DeviceRemoved(device))
        return;

    UnbindUnorderedAccessViews(device);
    functions->pfnSoSetTargets(handle, 0, PIPE_MAX_SO_BUFFERS, nullptr, nullptr);
    pipe_so_target_reference(&device->draw_so_target, nullptr);
    functions->pfnIaSetInputLayout(handle, D3D10DDI_HELEMENTLAYOUT{});
    functions->pfnIaSetVertexBuffers(handle, 0, PIPE_MAX_ATTRIBS, resources, zeros, zeros);
    functions->pfnIaSetIndexBuffer(handle, D3D10DDI_HRESOURCE{}, DXGI_FORMAT_R16_UINT, 0);
    functions->pfnIaSetTopology(handle, D3D10_DDI_PRIMITIVE_TOPOLOGY_UNDEFINED);
    device->patch_vertices = 0;
    device->resource_binding_failed = false;
    functions->pfnSetBlendState(handle, D3D10DDI_HBLENDSTATE{}, blend, DefaultSampleMask);
    functions->pfnSetDepthStencilState(handle, D3D10DDI_HDEPTHSTENCILSTATE{}, 0);
    functions->pfnSetRasterizerState(handle, D3D10DDI_HRASTERIZERSTATE{});
    functions->pfnSetViewports(handle, 0, D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, nullptr);
    functions->pfnSetScissorRects(handle, 0, D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, nullptr);
}

void APIENTRY CheckHandleSizes(D3D10DDI_HDEVICE, UINT *count, D3D11DDI_HANDLESIZE *sizes)
{
    if (!sizes) {
        *count = NativeHandleCount;
        return;
    }

    const UINT available = *count;
    *count = NativeHandleCount;

    for (UINT i = 0; i < MIN2(available, NativeHandleCount); ++i)
        sizes[i] = {D3D11DDI_HANDLETYPE(i), sizeof(DeferredAlias)};
}

SIZE_T APIENTRY LocalHandleSize(D3D10DDI_HDEVICE, D3D11DDI_HANDLETYPE type, VOID *)
{
    return UINT(type) < NativeHandleCount ? sizeof(DeferredAlias) : 0;
}

SIZE_T APIENTRY DeferredSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE *)
{
    return sizeof(Device);
}

SIZE_T APIENTRY ListSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATECOMMANDLIST *)
{
    return sizeof(CommandList);
}

void FillDeferredFunctions(D3D11DDI_DEVICEFUNCS *functions);

HRESULT InitializeDeferred(Device *owner, const D3D11DDIARG_CREATEDEFERREDCONTEXT *data,
                           bool recycle)
{
    // Retained DDI11_1 has retyped appended commands and remains runtime emulated.
    if (owner->ddi_11_1 || owner->immediate_owner || !data || !data->hDrvContext.pDrvPrivate ||
        !data->p11ContextFuncs || !data->p11UMCallbacks)
        return E_INVALIDARG;

    if (DeviceRemoved(owner))
        return D3DDDIERR_DEVICEREMOVED;

    Device *context = CastDevice(data->hDrvContext);
    if (context == owner)
        return E_INVALIDARG;

    QlgCommandJournal *journal = qlg_commands_new(reinterpret_cast<uintptr_t>(owner));

    if (!journal)
        return E_OUTOFMEMORY;

    if (!owner->command_functions) {
        owner->command_functions = static_cast<D3D11DDI_DEVICEFUNCS *>(calloc(1, sizeof(D3D11DDI_DEVICEFUNCS)));

        if (!owner->command_functions) {
            qlg_commands_destroy(journal);
            return E_OUTOFMEMORY;
        }
        FillNativeDeviceFunctions(owner, owner->command_functions);
    }

    if (recycle) {
        if (context->immediate_owner != owner) {
            qlg_commands_destroy(journal);
            return E_INVALIDARG;
        }
        qlg_commands_destroy(context->command_recording);
    }
    memset(context, 0, sizeof *context);
    context->immediate_owner = owner;
    context->command_recording = journal;
    context->hRTCoreLayer = data->hRTCoreLayer;
    memcpy(&context->UMCallbacks, data->p11UMCallbacks, sizeof context->UMCallbacks);
    FillDeferredFunctions(data->p11ContextFuncs);

    if (DeviceRemoved(owner)) {
        qlg_commands_destroy(context->command_recording);
        memset(context, 0, sizeof *context);
        return D3DDDIERR_DEVICEREMOVED;
    }
    return S_OK;
}

void APIENTRY CreateDeferred(D3D10DDI_HDEVICE handle, const D3D11DDIARG_CREATEDEFERREDCONTEXT *data)
{
    SetError(handle, InitializeDeferred(CastDevice(handle), data, false));
}

HRESULT APIENTRY RecycleDeferred(D3D10DDI_HDEVICE handle, const D3D11DDIARG_CREATEDEFERREDCONTEXT *data)
{
    // Build-2 constructors return errors directly, never through SetErrorCb.
    if (DeviceRemoved(CastDevice(handle)))
        return D3DDDIERR_DEVICEREMOVED;

    return InitializeDeferred(CastDevice(handle), data, true);
}

void APIENTRY Abandon(D3D10DDI_HDEVICE handle)
{
    Device *context = CastDevice(handle);
    qlg_commands_destroy(context->command_recording);
    context->command_recording = nullptr;
    context->deferred_failed = false;
}

HRESULT Finish(Device *owner, const D3D11DDIARG_CREATECOMMANDLIST *data, CommandList *list)
{
    Device *context = data ? CastDevice(data->hDeferredContext) : nullptr;

    if (context && context->deferred_failed)
        return FAILED(context->recording_error) ? context->recording_error : E_FAIL;

    if (!context || context->immediate_owner != owner ||
        !context->command_recording || qlg_commands_seal(context->command_recording) != 1)
        return E_INVALIDARG;

    list->journal = context->command_recording;
    list->owner = owner;
    context->command_recording = nullptr;
    return S_OK;
}

void APIENTRY CreateList(D3D10DDI_HDEVICE handle, const D3D11DDIARG_CREATECOMMANDLIST *data,
                         D3D11DDI_HCOMMANDLIST list_handle, D3D11DDI_HRTCOMMANDLIST)
{
    CommandList *list = static_cast<CommandList *>(list_handle.pDrvPrivate);
    memset(list, 0, sizeof *list);
    SetError(handle, Finish(CastDevice(handle), data, list));
}

void APIENTRY DestroyList(D3D10DDI_HDEVICE, D3D11DDI_HCOMMANDLIST handle)
{
    CommandList *list = static_cast<CommandList *>(handle.pDrvPrivate);
    qlg_commands_destroy(list->journal);
    memset(list, 0, sizeof *list);
}

HRESULT APIENTRY RecycleList(D3D10DDI_HDEVICE handle, const D3D11DDIARG_CREATECOMMANDLIST *data,
                             D3D11DDI_HCOMMANDLIST list_handle, D3D11DDI_HRTCOMMANDLIST)
{
    if (DeviceRemoved(CastDevice(handle)))
        return D3DDDIERR_DEVICEREMOVED;

    CommandList *list = static_cast<CommandList *>(list_handle.pDrvPrivate);

    if (list->journal)
        return E_INVALIDARG;

    return Finish(CastDevice(handle), data, list);
}

void APIENTRY RecycleNotify(D3D10DDI_HDEVICE handle, D3D11DDI_HCOMMANDLIST list_handle)
{
    // This callback receives the IC handle directly, not a deferred alias.
    const Device *context = CastDevice(handle);
    const CommandList *list = static_cast<const CommandList *>(list_handle.pDrvPrivate);

    if (list->owner && list->owner != context->immediate_owner)
        RecordingError(CastDevice(handle), E_INVALIDARG);
}

void APIENTRY Execute(D3D10DDI_HDEVICE handle, D3D11DDI_HCOMMANDLIST list_handle)
{
    Device *device = CastDevice(handle);
    const CommandList *list = static_cast<const CommandList *>(list_handle.pDrvPrivate);

    if (!list || !list->journal || list->owner != device || !device->command_functions) {
        SetError(handle, E_INVALIDARG);
        return;
    }

    ClearPipeline(device);

    if (DeviceRemoved(device))
        return;

    const uint64_t errors = device->error_serial;
    const int status = qlg_commands_execute(list->journal, reinterpret_cast<uintptr_t>(device));

    if (!DeviceRemoved(device))
        ClearPipeline(device);

    if (status != 1 && device->error_serial == errors)
        SetError(handle, DeviceRemoved(device) ? D3DDDIERR_DEVICEREMOVED : E_FAIL);
}

void APIENTRY DeferredRelocate(D3D10DDI_HDEVICE, D3D11DDI_DEVICEFUNCS *) {}

void FillDeferredFunctions(D3D11DDI_DEVICEFUNCS *functions)
{
    memset(functions, 0, sizeof *functions);

#define RECORD(slot, layout) \
    functions->slot = QLG_SERIALIZE(Recorder<decltype(functions->slot), &D3D11DDI_DEVICEFUNCS::slot, &layout>::Call)
#define ALIAS(slot) functions->slot = QLG_SERIALIZE(AliasCreate<decltype(functions->slot)>::Call)
    RECORD(pfnDefaultConstantBufferUpdateSubresourceUP, Update);
    RECORD(pfnResourceUpdateSubresourceUP, Update);
    RECORD(pfnIaSetInputLayout, Single);
    RECORD(pfnIaSetVertexBuffers, VertexBuffers);
    RECORD(pfnIaSetIndexBuffer, Single);
    RECORD(pfnIaSetTopology, Single);
    RECORD(pfnDraw, Single);
    RECORD(pfnDrawIndexed, Single);
    RECORD(pfnDrawInstanced, Single);
    RECORD(pfnDrawIndexedInstanced, Single);
    RECORD(pfnDrawAuto, Single);
    RECORD(pfnDrawInstancedIndirect, Single);
    RECORD(pfnDrawIndexedInstancedIndirect, Single);
    RECORD(pfnDispatch, Single);
    RECORD(pfnDispatchIndirect, Single);
    RECORD(pfnSetRenderTargets, Targets);
    RECORD(pfnSetBlendState, Colour);
    RECORD(pfnSetDepthStencilState, Single);
    RECORD(pfnSetRasterizerState, Single);
    RECORD(pfnSetViewports, Viewports);
    RECORD(pfnSetScissorRects, Viewports);
    RECORD(pfnSoSetTargets, StreamTargets);
    RECORD(pfnSetPredication, Single);
    RECORD(pfnClearRenderTargetView, Colour);
    RECORD(pfnClearDepthStencilView, Single);
    RECORD(pfnClearUnorderedAccessViewUint, Colour);
    RECORD(pfnClearUnorderedAccessViewFloat, Colour);
    RECORD(pfnCsSetUnorderedAccessViews, CounterBindings);
    RECORD(pfnCopyStructureCount, Single);
    RECORD(pfnGenMips, Single);
    RECORD(pfnQueryBegin, Single);
    RECORD(pfnQueryEnd, Single);
    RECORD(pfnResourceCopy, Single);
    RECORD(pfnResourceCopyRegion, Single);
    RECORD(pfnResourceConvert, Single);
    RECORD(pfnResourceConvertRegion, Single);
    RECORD(pfnResourceResolveSubresource, Single);
    RECORD(pfnResourceReadAfterWriteHazard, Single);
    RECORD(pfnShaderResourceViewReadAfterWriteHazard, Single);
    RECORD(pfnSetTextFilterSize, Single);
#define RECORD_STAGE(stage) \
    RECORD(pfn##stage##SetShader, Single); \
    RECORD(pfn##stage##SetShaderResources, Binding); \
    RECORD(pfn##stage##SetSamplers, Binding); \
    RECORD(pfn##stage##SetConstantBuffers, Binding); \
    RECORD(pfn##stage##SetShaderWithIfaces, Interfaces)
    RECORD_STAGE(Vs);
    RECORD_STAGE(Ps);
    RECORD_STAGE(Gs);
    RECORD_STAGE(Hs);
    RECORD_STAGE(Ds);
    RECORD_STAGE(Cs);
#undef RECORD_STAGE
    ALIAS(pfnCreateResource);
    ALIAS(pfnCreateShaderResourceView);
    ALIAS(pfnCreateRenderTargetView);
    ALIAS(pfnCreateDepthStencilView);
    ALIAS(pfnCreateElementLayout);
    ALIAS(pfnCreateBlendState);
    ALIAS(pfnCreateDepthStencilState);
    ALIAS(pfnCreateRasterizerState);
    ALIAS(pfnCreateSampler);
    ALIAS(pfnCreateQuery);
    ALIAS(pfnCreateVertexShader);
    ALIAS(pfnCreatePixelShader);
    ALIAS(pfnCreateGeometryShader);
    ALIAS(pfnCreateGeometryShaderWithStreamOutput);
    ALIAS(pfnCreateHullShader);
    ALIAS(pfnCreateDomainShader);
    ALIAS(pfnCreateComputeShader);
    ALIAS(pfnCreateUnorderedAccessView);
#undef ALIAS
#undef RECORD

    functions->pfnDestroyResource = QLG_SERIALIZE(CloseAlias<D3D10DDI_HRESOURCE>);
    functions->pfnDestroyShaderResourceView = QLG_SERIALIZE(CloseAlias<D3D10DDI_HSHADERRESOURCEVIEW>);
    functions->pfnDestroyRenderTargetView = QLG_SERIALIZE(CloseAlias<D3D10DDI_HRENDERTARGETVIEW>);
    functions->pfnDestroyDepthStencilView = QLG_SERIALIZE(CloseAlias<D3D10DDI_HDEPTHSTENCILVIEW>);
    functions->pfnDestroyElementLayout = QLG_SERIALIZE(CloseAlias<D3D10DDI_HELEMENTLAYOUT>);
    functions->pfnDestroyBlendState = QLG_SERIALIZE(CloseAlias<D3D10DDI_HBLENDSTATE>);
    functions->pfnDestroyDepthStencilState = QLG_SERIALIZE(CloseAlias<D3D10DDI_HDEPTHSTENCILSTATE>);
    functions->pfnDestroyRasterizerState = QLG_SERIALIZE(CloseAlias<D3D10DDI_HRASTERIZERSTATE>);
    functions->pfnDestroySampler = QLG_SERIALIZE(CloseAlias<D3D10DDI_HSAMPLER>);
    functions->pfnDestroyQuery = QLG_SERIALIZE(CloseAlias<D3D10DDI_HQUERY>);
    functions->pfnDestroyShader = QLG_SERIALIZE(CloseAlias<D3D10DDI_HSHADER>);
    functions->pfnDestroyUnorderedAccessView = QLG_SERIALIZE(CloseAlias<D3D11DDI_HUNORDEREDACCESSVIEW>);
    functions->pfnDynamicIABufferMapNoOverwrite = QLG_SERIALIZE(DeferredMap);
    functions->pfnDynamicIABufferMapDiscard = QLG_SERIALIZE(DeferredMap);
    functions->pfnDynamicConstantBufferMapDiscard = QLG_SERIALIZE(DeferredMap);
    functions->pfnDynamicResourceMapDiscard = QLG_SERIALIZE(DeferredMap);
    functions->pfnDynamicIABufferUnmap = QLG_SERIALIZE(DeferredUnmap);
    functions->pfnDynamicConstantBufferUnmap = QLG_SERIALIZE(DeferredUnmap);
    functions->pfnDynamicResourceUnmap = QLG_SERIALIZE(DeferredUnmap);
    functions->pfnAbandonCommandList = QLG_SERIALIZE(Abandon);
    functions->pfnDestroyDevice = QLG_SERIALIZE(DestroyDevice);
    functions->pfnRelocateDeviceFuncs = QLG_SERIALIZE(DeferredRelocate);
    functions->pfnRecycleCommandList = QLG_SERIALIZE(RecycleNotify);
}

} // namespace

void CommandListsFillDeviceFunctions(D3D11DDI_DEVICEFUNCS *functions)
{
    functions->pfnAbandonCommandList = nullptr;
    functions->pfnRecycleCommandList = nullptr;
    functions->pfnCheckDeferredContextHandleSizes = QLG_SERIALIZE(QLG_DEVICE_CALL(CheckHandleSizes));
    functions->pfnCalcDeferredContextHandleSize = QLG_SERIALIZE(LocalHandleSize);
    functions->pfnCalcPrivateDeferredContextSize = QLG_SERIALIZE(DeferredSize);
    functions->pfnCreateDeferredContext = QLG_SERIALIZE(QLG_DEVICE_CALL(CreateDeferred));
    functions->pfnRecycleCreateDeferredContext = QLG_SERIALIZE(RecycleDeferred);
    functions->pfnCalcPrivateCommandListSize = QLG_SERIALIZE(ListSize);
    functions->pfnCreateCommandList = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateList, DestroyList));
    functions->pfnRecycleCreateCommandList = QLG_SERIALIZE(RecycleList);
    functions->pfnDestroyCommandList = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyList));
    functions->pfnRecycleDestroyCommandList = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyList));
    functions->pfnCommandListExecute = QLG_SERIALIZE(QLG_DEVICE_CALL(Execute));
}

void CommandListsDestroyDevice(Device *device)
{
    qlg_commands_destroy(device->command_recording);
    device->command_recording = nullptr;
    free(device->command_functions);
    device->command_functions = nullptr;
}

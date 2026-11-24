//! Module: lightning.direct3d12_umd.native_commands
//! Purpose: Own bounded genuine native COPY, COMPUTE and DIRECT state workflows.
//! Created: 2026-10-24
//! Architecture: Rust retains typed allocator/resource/runtime identities, while
//! native transfer records actual Vulkan work from the retained allocator pool.
//! Only observed completion or explicit safe drain permits pending retirement.
use crate::native_descriptors::{self, RtvLease};
use crate::native_images::{self, ImageLease};
use crate::native_objects::{self, AllocatorLease, KIND_COMMAND_LIST, KIND_COMMAND_QUEUE};
use crate::native_pipelines::{self, ComputePipelineLease, GraphicsPipelineLease};
use crate::native_resources::{self, CopyLease};
use crate::native_shaders::{self, EmptyRootBindingLease};
use crate::native_transfer::TransferResult;
use crate::transfer::TransferCommand;
use core::cell::UnsafeCell;

pub const BUNDLE_VERSION: u32 = 1;
pub const COMPUTE_BUNDLE_VERSION: u32 = 1;
pub const DIRECT_BUNDLE_VERSION: u32 = 1;
pub const DRAW_BUNDLE_VERSION: u32 = 1;
pub const DRAW_VERTEX_COUNT: u32 = 3;
pub const DRAW_INSTANCE_COUNT: u32 = 1;
// Internal by-value snapshot. Existing private DirectState and bundle layouts
// remain unchanged; genuine R0 callers use the full non-indexed scalar payload.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DrawArguments {
    pub vertices: u32,
    pub instances: u32,
    pub first_vertex: u32,
    pub first_instance: u32,
}
pub fn visit_draw_arguments(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields { ($($name:ident),*) => { $(field(stringify!($name), "u32", core::mem::offset_of!(DrawArguments, $name));)* }; }
    fields!(vertices, instances, first_vertex, first_instance);
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct IndexedArguments {
    pub count: u32,
    pub instances: u32,
    pub first_index: u32,
    pub base_vertex: i32,
    pub first_instance: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct IndexedWork {
    pub buffer_token: u64,
    pub offset: u64,
    pub bytes: u64,
    pub format: u32,
    pub reserved: u32,
    pub count: u32,
    pub instances: u32,
    pub first_index: u32,
    pub base_vertex: i32,
    pub first_instance: u32,
    pub reserved1: u32,
}
pub fn visit_indexed_work(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields {($($name:ident:$kind:ident),*)=>{$(field(stringify!($name),stringify!($kind),core::mem::offset_of!(IndexedWork,$name));)*};}
    fields!(buffer_token:u64,offset:u64,bytes:u64,format:u32,reserved:u32,count:u32,instances:u32,first_index:u32,base_vertex:i32,first_instance:u32,reserved1:u32);
}
struct IndexBinding {
    range: native_resources::BufferRangeLease,
    format: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct VertexViewInput {
    pub address: u64,
    pub bytes: u32,
    pub stride: u32,
}
pub fn visit_vertex_view_input(mut field: impl FnMut(&str, &str, usize)) {
    field(
        "address",
        "u64",
        core::mem::offset_of!(VertexViewInput, address),
    );
    field(
        "bytes",
        "u32",
        core::mem::offset_of!(VertexViewInput, bytes),
    );
    field(
        "stride",
        "u32",
        core::mem::offset_of!(VertexViewInput, stride),
    );
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct VertexWork {
    pub buffer_token: u64,
    pub offset: u64,
    pub bytes: u64,
    pub slot: u32,
    pub stride: u32,
}
pub fn visit_vertex_work(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields {($($name:ident:$kind:ident),*)=>{$(field(stringify!($name),stringify!($kind),core::mem::offset_of!(VertexWork,$name));)*};}
    fields!(buffer_token:u64,offset:u64,bytes:u64,slot:u32,stride:u32);
}
struct VertexBindingNode {
    next: usize,
    work: VertexWork,
    // A consumed unbound slot owns its node but no resource. It must still
    // issue an explicit null Vulkan binding so stale command state cannot leak.
    range: Option<native_resources::BufferRangeLease>,
}
#[derive(Default)]
struct VertexBindings {
    head: usize,
    tail: usize,
    count: u32,
}
impl VertexBindings {
    const fn new() -> Self {
        Self {
            head: 0,
            tail: 0,
            count: 0,
        }
    }
    fn append(&mut self, node: usize) {
        assert!(node != 0 && self.count < 32);
        unsafe {
            (*(node as *mut VertexBindingNode)).next = 0;
        }
        if self.tail == 0 {
            self.head = node;
        } else {
            unsafe {
                (*(self.tail as *mut VertexBindingNode)).next = node;
            }
        }
        self.tail = node;
        self.count += 1;
    }
    fn find(&self, slot: u32) -> Option<&VertexBindingNode> {
        let mut next = self.head;
        for _ in 0..self.count {
            let node = unsafe { &*(next as *const VertexBindingNode) };
            if node.work.slot == slot {
                return Some(node);
            }
            next = node.next;
        }
        None
    }
}
fn release_vertex_bindings(bindings: VertexBindings) {
    let mut next = bindings.head;
    for _ in 0..bindings.count {
        assert!(next != 0);
        let pointer = next as *mut VertexBindingNode;
        let node = unsafe { pointer.read() };
        next = node.next;
        if let Some(range) = node.range {
            native_resources::release_buffer_range(range);
        }
        unsafe {
            ql_dx12_native_recording_free(pointer.cast());
        }
    }
    assert!(next == 0);
}
fn retain_vertex_binding(
    device: usize,
    cookie: u64,
    slot: u32,
    view: VertexViewInput,
) -> Result<usize, i32> {
    let range = if view.address == 0 && view.bytes == 0 {
        None
    } else {
        Some(native_resources::retain_buffer_range(
            device,
            cookie,
            view.address,
            u64::from(view.bytes),
        )?)
    };
    let memory =
        unsafe { ql_dx12_native_recording_allocate(core::mem::size_of::<VertexBindingNode>()) }
            as *mut VertexBindingNode;
    if memory.is_null() {
        if let Some(range) = range {
            native_resources::release_buffer_range(range);
        }
        return Err(NO_MEMORY);
    }
    let work = match range.as_ref() {
        Some(range) => VertexWork {
            buffer_token: range.token,
            offset: range.offset,
            bytes: range.bytes,
            slot,
            stride: view.stride,
        },
        None => VertexWork {
            slot,
            ..VertexWork::default()
        },
    };
    unsafe {
        memory.write(VertexBindingNode {
            next: 0,
            work,
            range,
        });
    }
    Ok(memory as usize)
}
/// Pending recording ownership pins the entire immutable unique read chain.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_vertex_work(node: usize, output: *mut VertexWork) -> usize {
    let node = unsafe { &*(node as *const VertexBindingNode) };
    unsafe {
        output.write(node.work);
    }
    node.next
}
// POD normalized execution snapshots contain borrowed handles only. Every
// handle is backed by an independent unique owner in the corresponding node.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CbvWork {
    pub buffer_token: u64,
    pub offset: u64,
    pub bytes: u64,
    pub parameter: u32,
    pub word_offset: u32,
    pub required_bytes: u32,
    pub kind: u32,
}
/// Immutable resource-state facts at the consuming Draw/Dispatch, rather than
/// the final list state or descriptor contents frozen at recording time. Exact
/// BufferStateRecording owners travel with this entire recording generation.
#[derive(Clone, Copy)]
struct BufferStatePoint {
    resource: usize,
    cookie: u64,
    state: u32,
}
struct TableKey {
    heap: native_descriptors::ShaderHeapLease,
    handle: u64,
    offset: u32,
    states: [Option<BufferStatePoint>; BUFFER_STATE_LIMIT],
}
struct CbvBindingNode {
    next: usize,
    work: CbvWork,
    range: Option<native_resources::BufferRangeLease>,
    table: Option<TableKey>,
    slot: Option<native_descriptors::DescriptorSlotLease>,
}
#[derive(Default)]
struct CbvBindings {
    head: usize,
    tail: usize,
    count: u32,
}
impl CbvBindings {
    const fn new() -> Self {
        Self {
            head: 0,
            tail: 0,
            count: 0,
        }
    }
    fn append(&mut self, node: usize) {
        assert!(node != 0 && self.count < 64);
        unsafe {
            (*(node as *mut CbvBindingNode)).next = 0;
        }
        if self.tail == 0 {
            self.head = node;
        } else {
            unsafe {
                (*(self.tail as *mut CbvBindingNode)).next = node;
            }
        }
        self.tail = node;
        self.count += 1;
    }
    fn find(&self, parameter: u32) -> Option<&CbvBindingNode> {
        let mut next = self.head;
        for _ in 0..self.count {
            let node = unsafe { &*(next as *const CbvBindingNode) };
            if node.work.parameter == parameter {
                return Some(node);
            }
            next = node.next;
        }
        None
    }
}
fn release_cbv_bindings(bindings: CbvBindings) {
    let mut next = bindings.head;
    for _ in 0..bindings.count {
        assert!(next != 0);
        let pointer = next as *mut CbvBindingNode;
        let node = unsafe { pointer.read() };
        next = node.next;
        if let Some(range) = node.range {
            native_resources::release_buffer_range(range);
        }
        if let Some(slot) = node.slot {
            native_descriptors::release_shader_slot(slot);
        }
        if let Some(key) = node.table {
            native_descriptors::release_shader_heap(key.heap);
        }
        unsafe {
            ql_dx12_native_recording_free(pointer.cast());
        }
    }
    assert!(next == 0);
}
fn retain_cbv_binding(
    device: usize,
    cookie: u64,
    parameter: u32,
    word_offset: u32,
    kind: u32,
    address: u64,
    required_bytes: u32,
) -> Result<usize, i32> {
    if !matches!(kind, 2 | 3 | 4)
        || address % (if kind == 2 { 256 } else { 4 }) != 0
        || (address == 0 && required_bytes == 0)
    {
        return Err(INVALID);
    }
    let range = if address == 0 {
        None
    } else {
        Some(native_resources::retain_buffer_tail(
            device, cookie, address, 65536,
        )?)
    };
    if kind == 4
        && range
            .as_ref()
            .is_none_or(|range| range.kind != crate::resources::DEFAULT)
    {
        if let Some(range) = range {
            native_resources::release_buffer_range(range);
        }
        return Err(INVALID);
    }
    let bytes = range.as_ref().map_or(65536, |range| range.bytes);
    if u64::from(required_bytes) > bytes {
        if let Some(range) = range {
            native_resources::release_buffer_range(range);
        }
        return Err(NO_INTERFACE);
    }
    let memory =
        unsafe { ql_dx12_native_recording_allocate(core::mem::size_of::<CbvBindingNode>()) }
            as *mut CbvBindingNode;
    if memory.is_null() {
        if let Some(range) = range {
            native_resources::release_buffer_range(range);
        }
        return Err(NO_MEMORY);
    }
    // A consumed null descriptor owns no fabricated native resource or GPUVA.
    // Its retained PSO/root owns the real immutable zero-filled Vulkan backing.
    let work = CbvWork {
        buffer_token: range.as_ref().map_or(0, |range| range.token),
        offset: range.as_ref().map_or(0, |range| range.offset),
        bytes,
        parameter,
        word_offset,
        required_bytes,
        kind,
    };
    unsafe {
        memory.write(CbvBindingNode {
            next: 0,
            work,
            range,
            table: None,
            slot: None,
        });
    }
    Ok(memory as usize)
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_cbv_work(node: usize, output: *mut CbvWork) -> usize {
    let node = unsafe { &*(node as *const CbvBindingNode) };
    unsafe {
        output.write(node.work);
    }
    node.next
}
struct CbvSnapshots {
    states: [Option<BufferStatePoint>; BUFFER_STATE_LIMIT],
    heap: Option<native_descriptors::ShaderHeapIdentity>,
    tables: [u64; 64],
    table_offsets: [u32; 64],
    kinds: [u32; 64],
    addresses: [u64; 64],
    offsets: [u32; 64],
    required: [u32; 64],
}
fn snapshot_cbvs(list: &List, required: [u32; 64]) -> Result<CbvSnapshots, i32> {
    let plan = list.root.as_ref().ok_or(INVALID)?.plan();
    let mut snapshot = CbvSnapshots {
        states: core::array::from_fn(|i| {
            list.buffer_states[i]
                .as_ref()
                .map(|entry| BufferStatePoint {
                    resource: entry.owner.resource,
                    cookie: entry.owner.resource_cookie,
                    state: entry.current,
                })
        }),
        heap: None,
        tables: [0; 64],
        table_offsets: [0; 64],
        kinds: [0; 64],
        addresses: [0; 64],
        offsets: [0; 64],
        required,
    };
    for (index, bytes) in required.iter().enumerate() {
        if *bytes == 0 {
            continue;
        }
        if index >= plan.count as usize || !matches!(plan.parameters[index].kind, 2 | 3 | 4 | 5) {
            return Err(INVALID);
        }
        snapshot.offsets[index] = plan.parameters[index].offset;
        snapshot.kinds[index] = plan.parameters[index].kind;
        if snapshot.kinds[index] == 5 {
            let heap = list.shader_heap.as_ref().ok_or(INVALID)?;
            let handle = list.table_handles[index];
            let offset = plan.parameters[index].words;
            if handle == 0 || !unsafe { heap.contains_locked(handle, offset) } {
                return Err(INVALID);
            }
            snapshot.heap = Some(heap.identity());
            snapshot.tables[index] = handle;
            snapshot.table_offsets[index] = offset;
            continue;
        }
        let Some(node) = list.cbv_bindings.find(index as u32) else {
            if snapshot.kinds[index] == 4 {
                return Err(INVALID);
            }
            continue;
        };
        let range = node.range.as_ref().ok_or(INVALID)?;
        let state =
            buffer_state_for(list, range.resource, range.resource_cookie).unwrap_or(range.initial);
        let needed = if snapshot.kinds[index] == 4 {
            native_resources::UNORDERED_ACCESS
        } else if snapshot.kinds[index] == 2 {
            native_resources::VERTEX_BUFFER
        } else {
            match plan.parameters[index].visibility {
                1 => native_resources::NON_PIXEL_SHADER_RESOURCE,
                5 => native_resources::PIXEL_SHADER_RESOURCE,
                _ => {
                    native_resources::NON_PIXEL_SHADER_RESOURCE
                        | native_resources::PIXEL_SHADER_RESOURCE
                }
            }
        };
        if node.work.kind != snapshot.kinds[index]
            || !native_resources::buffer_state_allows(range.kind, state, needed)
            || (snapshot.kinds[index] == 4 && range.kind != crate::resources::DEFAULT)
        {
            return Err(INVALID);
        }
        if u64::from(*bytes) > range.bytes {
            return Err(NO_INTERFACE);
        }
        snapshot.addresses[index] = range.address;
    }
    Ok(snapshot)
}
fn retain_cbv_snapshots(
    device: usize,
    cookie: u64,
    snapshot: CbvSnapshots,
) -> Result<CbvBindings, i32> {
    let mut bindings = CbvBindings::new();
    for i in 0..64 {
        if snapshot.required[i] != 0 {
            let candidate = if snapshot.kinds[i] == 5 {
                snapshot.heap.map_or(Err(INVALID), |identity| {
                    retain_table_binding(
                        identity,
                        i as u32,
                        snapshot.offsets[i],
                        snapshot.tables[i],
                        snapshot.table_offsets[i],
                        snapshot.required[i],
                        snapshot.states,
                    )
                })
            } else {
                retain_cbv_binding(
                    device,
                    cookie,
                    i as u32,
                    snapshot.offsets[i],
                    snapshot.kinds[i],
                    snapshot.addresses[i],
                    snapshot.required[i],
                )
            };
            match candidate {
                Ok(node) => bindings.append(node),
                Err(e) => {
                    release_cbv_bindings(bindings);
                    return Err(e);
                }
            }
        }
    }
    Ok(bindings)
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RootConstants {
    pub words: u32,
    pub values: [u32; 64],
    pub cbv_head: u64,
    pub cbv_count: u32,
    pub cbv_reserved: u32,
}
impl Default for RootConstants {
    fn default() -> Self {
        Self {
            words: 0,
            values: [0; 64],
            cbv_head: 0,
            cbv_count: 0,
            cbv_reserved: 0,
        }
    }
}

/// Internal scalar query payload; it never changes a frozen callback record.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct QueryWork {
    pub owner: u64,
    pub heap_kind: u32,
    pub heap_count: u32,
    pub index: u32,
    pub count: u32,
    pub data_kind: u32,
    pub reserved: u32,
    pub buffer: u64,
    pub offset: u64,
    pub bytes: u64,
}
pub fn visit_query_work(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields{($($name:ident:$kind:ident),*)=>{$(field(stringify!($name),stringify!($kind),core::mem::offset_of!(QueryWork,$name));)*};}
    fields!(owner:u64,heap_kind:u32,heap_count:u32,index:u32,count:u32,data_kind:u32,reserved:u32,buffer:u64,offset:u64,bytes:u64);
}
#[derive(Clone, Copy, PartialEq, Eq)]
struct QueryKey {
    owner: usize,
    index: u32,
    kind: u32,
}
pub const QUERY_TIMESTAMP: u32 = 2;
pub const QUERY_BEGIN: u32 = 3;
pub const QUERY_END: u32 = 4;
pub const QUERY_RESOLVE: u32 = 5;
pub const INDIRECT_DRAW: u32 = 6;
pub const INDIRECT_INDEXED_DRAW:u32=7;
pub const BUFFER_UAV_CLEAR: u32 = 8;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct BufferFillWork {
    pub buffer: u64,
    pub offset: u64,
    pub bytes: u64,
    pub value: u32,
    pub reserved: u32,
}
pub fn visit_buffer_fill_work(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields{($($name:ident:$kind:ident),*)=>{$(field(stringify!($name),stringify!($kind),core::mem::offset_of!(BufferFillWork,$name));)*};}
    fields!(buffer:u64,offset:u64,bytes:u64,value:u32,reserved:u32);
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct IndirectWork {
    pub buffer_token: u64,
    pub offset: u64,
    pub bytes: u64,
    pub count: u32,
    pub stride: u32,
    pub reserved: u32,
    pub reserved1: u32,
}
pub fn visit_indirect_work(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields{($($name:ident:$kind:ident),*)=>{$(field(stringify!($name),stringify!($kind),core::mem::offset_of!(IndirectWork,$name));)*};}
    fields!(buffer_token:u64,offset:u64,bytes:u64,count:u32,stride:u32,reserved:u32,reserved1:u32);
}
#[derive(Clone, Copy)]
struct IndirectRequest {
    signature: usize,
    signature_cookie: u64,
    buffer: usize,
    buffer_cookie: u64,
    offset: u64,
    count: u32,
    count_buffer: usize,
    count_cookie: u64,
    count_offset: u64,
}
struct IndirectRecording {
    signature: crate::native_signatures::SignatureLease,
    arguments: native_resources::IndirectReadLease,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct GraphicsWork {
    pub pipeline: u64,
    pub target: u64,
    pub depth_target: u64,
    pub buffer: u64,
    pub kind: u32,
    pub reserved: u32,
    pub state: DirectState,
    pub arguments: DrawArguments,
    pub indexed: IndexedWork,
    pub constants: BlendConstants,
    pub root_constants: RootConstants,
    pub vertex_head: u64,
    pub vertex_count: u32,
    pub vertex_reserved: u32,
    pub target_count: u32,
    pub target_reserved: u32,
    pub targets_tail: [u64; 7],
    pub topology: u32,
    pub topology_reserved: u32,
    pub query: QueryWork,
    pub indirect: IndirectWork,
    pub fill: BufferFillWork,
}
pub fn visit_graphics_work(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields {($($name:ident:$kind:ident),*)=>{$(field(stringify!($name),stringify!($kind),core::mem::offset_of!(GraphicsWork,$name));)*};}
    fields!(pipeline:u64,target:u64,depth_target:u64,buffer:u64,kind:u32,reserved:u32,state:DirectState,arguments:DrawArguments,indexed:IndexedWork,constants:BlendConstants,root_constants:RootConstants,vertex_head:u64,vertex_count:u32,vertex_reserved:u32,target_count:u32,target_reserved:u32,targets_tail:TargetOwnerArray,topology:u32,topology_reserved:u32,query:QueryWork,indirect:IndirectWork,fill:BufferFillWork);
}
struct GraphicsRecording {
    next: usize,
    pipeline: Option<GraphicsPipelineLease>,
    root: Option<EmptyRootBindingLease>,
    target: Option<RtvLease>,
    extra_targets: [Option<RtvLease>; 7],
    depth: Option<RtvLease>,
    index: Option<IndexBinding>,
    work: GraphicsWork,
    query: Option<crate::native_queries::QueryLease>,
    destination_write: Option<native_resources::BufferWriteLease>,
    indirect: Option<IndirectRecording>,
    vertices: VertexBindings,
    cbvs: CbvBindings,
}
// Unique growable chain. Moving this value transfers all nodes; it is never
// cloned or copied. No large owner array is constructed on the Windows stack.
#[derive(Default)]
struct GraphicsStream {
    head: usize,
    tail: usize,
    count: u32,
}
impl GraphicsStream {
    const fn new() -> Self {
        Self {
            head: 0,
            tail: 0,
            count: 0,
        }
    }
    fn append(&mut self, node: usize) {
        assert!(node != 0 && self.count < u32::MAX);
        if self.tail == 0 {
            self.head = node;
        } else {
            unsafe {
                (*(self.tail as *mut GraphicsRecording)).next = node;
            }
        }
        self.tail = node;
        self.count += 1;
    }
}
unsafe extern "C" {
    pub fn ql_dx12_native_recording_allocate(bytes: usize) -> *mut core::ffi::c_void;
    pub fn ql_dx12_native_recording_free(memory: *mut core::ffi::c_void);
    fn ql_dx12_native_graphics_submit(
        adapter: usize,
        pool: usize,
        head: usize,
        count: u32,
        result: *mut TransferResult,
    ) -> i32;
}
fn release_extra_targets(targets: [Option<RtvLease>; 7]) {
    for target in targets.into_iter().flatten() {
        native_descriptors::release_rtv(target);
    }
}
fn release_graphics_recording(command: GraphicsRecording) {
    if let Some(indirect) = command.indirect {
        crate::native_signatures::release_signature(indirect.signature);
        native_resources::release_indirect_read(indirect.arguments);
    }

    if let Some(query) = command.query {
        crate::native_queries::release_query(query);
    }
    if let Some(destination) = command.destination_write {
        native_resources::release_buffer_write(destination);
    }
    release_vertex_bindings(command.vertices);
    release_cbv_bindings(command.cbvs);
    if let Some(index) = command.index {
        native_resources::release_index_range(index.range);
    }
    if let Some(depth) = command.depth {
        native_descriptors::release_rtv(depth);
    }
    release_extra_targets(command.extra_targets);
    if let Some(target) = command.target {
        native_descriptors::release_rtv(target);
    }
    if let Some(root) = command.root {
        native_shaders::release_empty_root_binding(root);
    }
    if let Some(pipeline) = command.pipeline {
        native_pipelines::release_graphics_pipeline(pipeline);
    }
}
fn release_graphics_stream(stream: GraphicsStream) {
    let mut next = stream.head;
    for _ in 0..stream.count {
        assert!(
            next != 0,
            "unique draw stream has exact retained node count"
        );
        let pointer = next as *mut GraphicsRecording;
        let node = unsafe { pointer.read() };
        next = node.next;
        release_graphics_recording(node);
        unsafe {
            ql_dx12_native_recording_free(pointer.cast());
        }
    }
    assert!(
        next == 0,
        "unique draw stream terminates at its recorded count"
    );
}
/// The queue's Operating pending owner pins this immutable chain throughout
/// synchronous normalization and actual recording. No external caller owns it.
pub unsafe fn graphics_work_at(node: usize) -> (GraphicsWork, usize) {
    let node = unsafe { &*(node as *const GraphicsRecording) };
    (node.work, node.next)
}
pub const RENDER_TARGET_BUNDLE_VERSION: u32 = 1;
pub const RENDER_TARGET_BIND: u32 = 1;
pub const RENDER_TARGET_CLEAR: u32 = 2;
pub const RENDER_TARGET_CLEAR_RECTS: u32 = 4;
pub const RENDER_TARGET_DEPTH_CLEAR: u32 = 8;
pub const RENDER_TARGET_STENCIL_CLEAR: u32 = 16;
pub const DISPATCH_GROUP_LIMIT: u32 = 65_535;
pub const COMPUTE_STREAM_LIMIT: usize = 8;
struct ComputeRecording {
    pipeline: ComputePipelineLease,
    root: EmptyRootBindingLease,
    arguments: DispatchArguments,
    constants: RootConstants,
    cbvs: CbvBindings,
    indirect:Option<IndirectRecording>,
    indirect_work:IndirectWork,
}
fn release_compute_recording(command:ComputeRecording){
    if let Some(indirect)=command.indirect {crate::native_signatures::release_signature(indirect.signature);native_resources::release_indirect_read(indirect.arguments);}
    release_cbv_bindings(command.cbvs);native_pipelines::release_compute_pipeline(command.pipeline);native_shaders::release_empty_root_binding(command.root);
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DispatchArguments {
    pub x: u32,
    pub y: u32,
    pub z: u32,
}
impl DispatchArguments {
    pub fn supported(&self) -> bool {
        self.x <= DISPATCH_GROUP_LIMIT
            && self.y <= DISPATCH_GROUP_LIMIT
            && self.z <= DISPATCH_GROUP_LIMIT
    }
}
pub fn visit_dispatch_arguments(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields { ($($name:ident),*) => { $(field(stringify!($name), "u32", core::mem::offset_of!(DispatchArguments, $name));)* }; }
    fields!(x, y, z);
}

// Keep caller float bits intact. Format-dependent conversion belongs to the
// selected Vulkan blend stage, not a global UNORM clamp in command recording.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BlendConstants {
    pub red: u32,
    pub green: u32,
    pub blue: u32,
    pub alpha: u32,
}
impl BlendConstants {
    pub const DEFAULT: Self = Self {
        red: 0x3f800000,
        green: 0x3f800000,
        blue: 0x3f800000,
        alpha: 0x3f800000,
    };
}
impl Default for BlendConstants {
    fn default() -> Self {
        Self::DEFAULT
    }
}
pub fn visit_blend_constants(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields { ($($name:ident),*) => { $(field(stringify!($name), "u32", core::mem::offset_of!(BlendConstants, $name));)* }; }
    fields!(red, green, blue, alpha);
}
// Private bounded subset, independent of the genuine R0 ABI. Caller input is
// copied during the callback; no pointer survives recording or submission.
pub const CLEAR_RECT_LIMIT: u32 = 16;
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq)]
pub struct ClearRect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}
impl ClearRect {
    fn clipped(self, extent: (u32, u32)) -> Option<Self> {
        let width = extent.0 as i32;
        let height = extent.1 as i32;
        let rect = Self {
            left: self.left.clamp(0, width),
            top: self.top.clamp(0, height),
            right: self.right.clamp(0, width),
            bottom: self.bottom.clamp(0, height),
        };
        (rect.left < rect.right && rect.top < rect.bottom).then_some(rect)
    }
}
#[derive(Clone, Copy, Default)]
struct ClearRegions {
    explicit: bool,
    count: u32,
    rectangles: [ClearRect; CLEAR_RECT_LIMIT as usize],
}
pub fn visit_clear_rect(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields { ($($name:ident),*) => {
        $(field(stringify!($name), "i32", core::mem::offset_of!(ClearRect, $name));)*
    }; }
    fields!(left, top, right, bottom);
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ClearColor {
    pub red: f32,
    pub green: f32,
    pub blue: f32,
    pub alpha: f32,
}
impl ClearColor {
    fn within(&self, range: core::ops::RangeInclusive<f32>) -> bool {
        [self.red, self.green, self.blue, self.alpha]
            .into_iter()
            .all(|value| range.contains(&value))
    }
}

const HALF_FLOAT_FINITE_RANGE: core::ops::RangeInclusive<f32> = -65_504.0..=65_504.0;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RenderTargetWork {
    pub flags: u32,
    pub reserved: u32,
    pub clear_red: f32,
    pub clear_green: f32,
    pub clear_blue: f32,
    pub clear_alpha: f32,
    pub rect_count: u32,
    pub rectangles: [ClearRect; CLEAR_RECT_LIMIT as usize],
}
impl RenderTargetWork {
    pub fn supported(&self) -> bool {
        if self.flags == 0
            || self.flags
                & !(RENDER_TARGET_BIND
                    | RENDER_TARGET_CLEAR
                    | RENDER_TARGET_CLEAR_RECTS
                    | RENDER_TARGET_DEPTH_CLEAR
                    | RENDER_TARGET_STENCIL_CLEAR)
                != 0
            || self.reserved != 0
        {
            return false;
        }
        if self.rect_count > CLEAR_RECT_LIMIT
            || (self.flags & RENDER_TARGET_CLEAR_RECTS != 0
                && self.flags & RENDER_TARGET_CLEAR == 0)
            || (self.flags & RENDER_TARGET_CLEAR_RECTS == 0 && self.rect_count != 0)
        {
            return false;
        }
        for (index, rect) in self.rectangles.iter().enumerate() {
            if index < self.rect_count as usize {
                let max = native_images::IMAGE_DIMENSION_LIMIT;
                if rect.clipped((max, max)) != Some(*rect) {
                    return false;
                }
            } else if *rect != ClearRect::default() {
                return false;
            }
        }
        let color = ClearColor {
            red: self.clear_red,
            green: self.clear_green,
            blue: self.clear_blue,
            alpha: self.clear_alpha,
        };

        if self.flags & (RENDER_TARGET_DEPTH_CLEAR | RENDER_TARGET_STENCIL_CLEAR) != 0 {
            return self.flags & RENDER_TARGET_CLEAR != 0
                && color.red.is_finite()
                && (0.0..=1.0).contains(&color.red)
                && (self.flags & RENDER_TARGET_DEPTH_CLEAR != 0 || color.red == 0.0)
                && color.green.is_finite()
                && (0.0..=255.0).contains(&color.green)
                && color.green == color.green as u32 as f32
                && (self.flags & RENDER_TARGET_STENCIL_CLEAR != 0 || color.green == 0.0)
                && color.blue == 0.0
                && color.alpha == 0.0;
        }
        if self.flags & RENDER_TARGET_CLEAR != 0 {
            color.within(HALF_FLOAT_FINITE_RANGE)
        } else {
            color.red == 0.0 && color.green == 0.0 && color.blue == 0.0 && color.alpha == 0.0
        }
    }
}
pub fn visit_clear_color(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields { ($($name:ident),*) => {
        $(field(stringify!($name), "f32", core::mem::offset_of!(ClearColor, $name));)*
    }; }
    fields!(red, green, blue, alpha);
}
pub fn visit_render_target_work(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields { ($($name:ident:$kind:ident),*) => {
        $(field(stringify!($name), stringify!($kind), core::mem::offset_of!(RenderTargetWork, $name));)*
    }; }
    fields!(flags: u32, reserved: u32, clear_red: f32, clear_green: f32,
        clear_blue: f32, clear_alpha: f32, rect_count: u32);
    field(
        "rectangles",
        "ClearRectArray",
        core::mem::offset_of!(RenderTargetWork, rectangles),
    );
}

pub const IMAGE_COPY_ROW_ALIGNMENT: u32 = 256;
pub const IMAGE_COPY_PLACEMENT_ALIGNMENT: u64 = 512;
pub const IMAGE_COPY_DEPTH: u32 = 1;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ImageReadbackFootprint {
    pub offset: u64,
    pub source_subresource: u64,
    pub row_pitch: u32,
    pub slice_pitch: u32,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub format: u32,
    pub destination_x: u32,
    pub destination_y: u32,
    pub destination_z: u32,
    pub reserved: u32,
}
impl ImageReadbackFootprint {
    pub fn byte_extent(&self) -> Option<u64> {
        let texel_bytes = native_images::color_texel_bytes(self.format)?;
        let row_bytes = self.width.checked_mul(texel_bytes)?;
        let slice_bytes = self.row_pitch.checked_mul(self.height)?;
        // The buffer footprint covers each preceding slice and the final
        // slice through its last texel, excluding the final row's padding.
        let preceding_slices = u64::from(self.depth.checked_sub(1)?);
        let preceding_rows = u64::from(self.height.checked_sub(1)?);
        let active_bytes = u64::from(slice_bytes)
            .checked_mul(preceding_slices)?
            .checked_add(u64::from(self.row_pitch).checked_mul(preceding_rows)?)?
            .checked_add(u64::from(row_bytes))?;
        if self.destination_x != 0
            || self.destination_y != 0
            || self.destination_z != 0
            || self.reserved != 0
            || !native_images::supported_extent(self.width, self.height)
            || self.depth > native_images::IMAGE_VOLUME_DIMENSION_LIMIT
            || self.row_pitch < row_bytes
            || self.row_pitch % IMAGE_COPY_ROW_ALIGNMENT != 0
            || self.slice_pitch != slice_bytes
            || self.offset % IMAGE_COPY_PLACEMENT_ALIGNMENT != 0
            || self.offset.checked_add(active_bytes)? > crate::resources::MAX_BUFFER_BYTES
        {
            return None;
        }
        Some(active_bytes)
    }
}
pub fn visit_image_readback_footprint(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields { ($($name:ident:$kind:ident),*) => {
        $(field(stringify!($name), stringify!($kind), core::mem::offset_of!(ImageReadbackFootprint, $name));)*
    }; }
    fields!(offset:u64, source_subresource:u64, row_pitch:u32, slice_pitch:u32, width:u32, height:u32, depth:u32, format:u32, destination_x:u32, destination_y:u32, destination_z:u32, reserved:u32);
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ImageUploadFootprint {
    pub offset: u64,
    pub destination_subresource: u64,
    pub row_pitch: u32,
    pub slice_pitch: u32,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub format: u32,
    pub destination_x: u32,
    pub destination_y: u32,
    pub destination_z: u32,
    pub reserved: u32,
}
impl ImageUploadFootprint {
    pub fn byte_extent(&self) -> Option<u64> {
        // Both directions obey identical physical buffer placement/row/slice
        // constraints. Preserve separate genuine source/destination naming.
        ImageReadbackFootprint {
            source_subresource: self.destination_subresource,
            offset: self.offset,
            row_pitch: self.row_pitch,
            slice_pitch: self.slice_pitch,
            width: self.width,
            height: self.height,
            depth: self.depth,
            format: self.format,
            destination_x: self.destination_x,
            destination_y: self.destination_y,
            destination_z: self.destination_z,
            reserved: self.reserved,
        }
        .byte_extent()
    }
}
pub fn visit_image_upload_footprint(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields { ($($name:ident:$kind:ident),*) => {
        $(field(stringify!($name), stringify!($kind), core::mem::offset_of!(ImageUploadFootprint, $name));)*
    }; }
    fields!(offset:u64, destination_subresource:u64, row_pitch:u32, slice_pitch:u32, width:u32, height:u32, depth:u32, format:u32, destination_x:u32, destination_y:u32, destination_z:u32, reserved:u32);
}

pub const DIRECT_TOPOLOGY_TRIANGLELIST: u32 = 4;
// Vulkan guarantees at least 4096 in maxViewportDimensions. This conservative
// floor subset needs no extra physical-limit query for state-only recording.
pub const DIRECT_VIEWPORT_FLOOR_BOUND: u32 = 4096;
const DIRECT_TOPOLOGY_SET: u32 = 1;
const DIRECT_VIEWPORT_SET: u32 = 2;
const DIRECT_SCISSOR_SET: u32 = 4;
const DIRECT_COMPLETE_STATE: u32 = DIRECT_TOPOLOGY_SET | DIRECT_VIEWPORT_SET | DIRECT_SCISSOR_SET;

/// Immutable linked-in command snapshot. C receives a generated exact layout;
/// Rust validates the guaranteed-floor subset; C converts D3D positive-height
/// viewport coordinates into Vulkan negative-height coordinates. No image,
/// render pass, draw or rasterized output behavior is implied by these states.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DirectState {
    pub viewport_x: f32,
    pub viewport_y: f32,
    pub viewport_width: f32,
    pub viewport_height: f32,
    pub min_depth: f32,
    pub max_depth: f32,
    pub scissor_left: i32,
    pub scissor_top: i32,
    pub scissor_right: i32,
    pub scissor_bottom: i32,
    pub stencil_reference: u32,
}
impl DirectState {
    fn viewport_valid(&self) -> bool {
        let values = [
            self.viewport_x,
            self.viewport_y,
            self.viewport_width,
            self.viewport_height,
            self.min_depth,
            self.max_depth,
        ];
        values.into_iter().all(f32::is_finite)
            && self.viewport_x >= 0.0
            && self.viewport_y >= 0.0
            && self.viewport_width > 0.0
            && self.viewport_height > 0.0
            && f64::from(self.viewport_x) + f64::from(self.viewport_width)
                <= f64::from(DIRECT_VIEWPORT_FLOOR_BOUND)
            && f64::from(self.viewport_y) + f64::from(self.viewport_height)
                <= f64::from(DIRECT_VIEWPORT_FLOOR_BOUND)
            && self.min_depth >= 0.0
            && self.min_depth <= self.max_depth
            && self.max_depth <= 1.0
    }
    fn scissor_valid(&self) -> bool {
        self.scissor_left >= 0
            && self.scissor_top >= 0
            && self.scissor_left < self.scissor_right
            && self.scissor_top < self.scissor_bottom
            && self.scissor_right <= DIRECT_VIEWPORT_FLOOR_BOUND as i32
            && self.scissor_bottom <= DIRECT_VIEWPORT_FLOOR_BOUND as i32
    }
    pub fn supported(&self) -> bool {
        self.viewport_valid() && self.scissor_valid()
    }
}
pub fn visit_direct_state(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields {
        ($($name:ident:$kind:ident),* $(,)?) => {
            $(field(stringify!($name), stringify!($kind),
                core::mem::offset_of!(DirectState, $name));)*
        };
    }
    fields!(viewport_x:f32, viewport_y:f32, viewport_width:f32, viewport_height:f32,
        min_depth:f32, max_depth:f32, scissor_left:i32, scissor_top:i32,
        scissor_right:i32, scissor_bottom:i32, stencil_reference:u32);
}

pub const COMPUTE_DISPATCH_DIMENSION: u32 = 1;
pub const COMPLETION_VERSION: u32 = 1;
pub const RECORDING_INFO_VERSION: u32 = 1;
pub const RESET_SLOT: u32 = 0;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RecordingInfo {
    pub version: u32,
    pub bytes: u32,
    pub recording: u32,
    pub reserved: u32,
    pub generation: u64,
    pub id: u64,
}
pub fn visit_recording_info(mut field: impl FnMut(&str, &str, usize)) {
    field(
        "version",
        "u32",
        core::mem::offset_of!(RecordingInfo, version),
    );
    field("bytes", "u32", core::mem::offset_of!(RecordingInfo, bytes));
    field(
        "recording",
        "u32",
        core::mem::offset_of!(RecordingInfo, recording),
    );
    field(
        "reserved",
        "u32",
        core::mem::offset_of!(RecordingInfo, reserved),
    );
    field(
        "generation",
        "u64",
        core::mem::offset_of!(RecordingInfo, generation),
    );
    field("id", "u64", core::mem::offset_of!(RecordingInfo, id));
}
pub const IDLE: u32 = 0;
pub const PENDING: u32 = 1;
pub const COMPLETE: u32 = 2;
const CAPACITY: usize = 16;
const COPY_BATCH_LIMIT: usize = crate::native_transfer::COPY_BATCH_LIMIT as usize;
const COPY_EXTRA_LIMIT: usize = COPY_BATCH_LIMIT - 1;
const INVALID: i32 = 0x8007_0057u32 as i32;
const NO_INTERFACE: i32 = 0x8000_4002u32 as i32;
const NO_MEMORY: i32 = 0x8007_000Eu32 as i32;
const FAIL: i32 = 0x8000_4005u32 as i32;
const BUSY: i32 = 0x887A_000Au32 as i32;

unsafe extern "C" {
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_queue_prepare(adapter: usize) -> i32;
    fn ql_dx12_native_parent_status(adapter: usize) -> i32;
    fn ql_dx12_native_parent_status_locked(adapter: usize) -> i32;
    fn ql_dx12_native_empty_submit(
        adapter: usize,
        pool: usize,
        result: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_transfer_submit(
        adapter: usize,
        pool: usize,
        command: *const TransferCommand,
        result: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_compute_submit(
        adapter: usize,
        pool: usize,
        count: u32,
        pipelines: *const usize,
        arguments: *const DispatchArguments,
        constants: *const RootConstants,
        indirect:*const IndirectWork,
        result: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_render_target_submit(
        adapter: usize,
        pool: usize,
        binding: usize,
        clear: usize,
        work: *const RenderTargetWork,
        output: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_direct_submit(
        adapter: usize,
        pool: usize,
        pipeline: usize,
        state: *const DirectState,
        result: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_image_copy_submit(
        adapter: usize,
        pool: usize,
        source: u64,
        destination: u64,
        output: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_color_resolve_submit(
        adapter: usize,
        pool: usize,
        source: u64,
        destination: u64,
        source_layer: u32,
        destination_layer: u32,
        output: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_image_transition_submit(
        adapter: usize,
        pool: usize,
        image_token: u64,
        layer: u32,
        output: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_image_readback_submit(
        adapter: usize,
        pool: usize,
        image_token: u64,
        layer: u32,
        buffer_token: u64,
        footprint: *const ImageReadbackFootprint,
        output: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_draw_indexed_submit(
        adapter: usize,
        pool: usize,
        pipeline: usize,
        target: usize,
        depth_target: usize,
        state: *const DirectState,
        indexed: *const IndexedWork,
        constants: *const BlendConstants,
        output: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_draw_submit(
        adapter: usize,
        pool: usize,
        pipeline: usize,
        target: usize,
        depth_target: usize,
        state: *const DirectState,
        arguments: *const DrawArguments,
        constants: *const BlendConstants,
        output: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_image_upload_submit(
        adapter: usize,
        pool: usize,
        image_token: u64,
        layer: u32,
        buffer_token: u64,
        footprint: *const ImageUploadFootprint,
        output: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_wddm_context_create(
        callbacks: *const native_objects::WddmQueueCallbacks,
        runtime_queue: usize,
        owner: *mut usize,
    ) -> i32;
    fn ql_dx12_wddm_paging_prepare(owner: usize, output: *mut core::ffi::c_void) -> i32;
    fn ql_dx12_wddm_queue_close(owner: usize) -> i32;
    fn ql_dx12_native_transfer_submit_many(
        adapter: usize,
        pool: usize,
        count: u32,
        commands: *const TransferCommand,
        result: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_transfer_poll(adapter: usize, result: *mut TransferResult) -> i32;
    fn ql_dx12_native_transfer_drain(adapter: usize, result: *mut TransferResult) -> i32;
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Completion {
    pub version: u32,
    pub bytes: u32,
    pub state: u32,
    pub reserved: u32,
    pub submitted: u64,
    pub completed: u64,
}
#[derive(Clone, Copy, PartialEq)]
enum State {
    Vacant,
    Creating,
    Live,
    Retiring,
}
#[derive(Clone, Copy, PartialEq)]
enum Phase {
    Recording,
    Invalid,
    Closed,
    Pending,
    Complete,
}
#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Copy,
    Compute,
    Direct,
}

#[derive(Clone, Copy, PartialEq)]
enum DirectMode {
    BufferCopy,
    ImageCopy,
    State,
    RenderTarget,
    ImageTransition,
    ImageReadback,
    ImageUpload,
    Draw,
}
// Net-state transaction: no closed or completed list leaves an image in an
// intermediate state. Upload/readback modes own their single validated copy
// between genuine transitions; the transition-only mode preserves its prior ABI.
struct ImageReadbackRecording {
    buffer: native_resources::ReadbackLease,
    footprint: ImageReadbackFootprint,
}
struct ImageUploadRecording {
    buffer: native_resources::UploadLease,
    footprint: ImageUploadFootprint,
}
struct ImageTransitionRecording {
    copy_state: u32,
    image: ImageLease,
    resource: usize,
    cookie: u64,
    layer: u32,
    closed: bool,
    readback: Option<ImageReadbackRecording>,
    upload: Option<ImageUploadRecording>,
}
impl ImageTransitionRecording {
    fn covers_layer(&self, layer: u64) -> bool {
        layer < u64::from(self.image.array_layers())
            && (self.layer == u32::MAX || layer == u64::from(self.layer))
    }
}

fn transition_layer(image: &ImageLease, subresource: u32) -> Option<u32> {
    match subresource {
        u32::MAX if image.array_layers() == 1 => Some(0),
        u32::MAX => Some(u32::MAX),
        layer if layer < image.array_layers() => Some(layer),
        _ => None,
    }
}

const RESOLVE_DEST: u32 = 0x1000;
const RESOLVE_SOURCE: u32 = 0x2000;
struct ResolveTransition {
    image: ImageLease,
    resource: usize,
    cookie: u64,
    layer: u32,
    closed: bool,
}

struct ResolveRecording {
    source: ImageLease,
    destination: ImageLease,
    source_layer: u32,
    destination_layer: u32,
}

struct ClearRecording {
    target: RtvLease,
    depth_stencil_flags: u32,
    color: ClearColor,
    regions: ClearRegions,
}

/// A bound pipeline keeps its stage kind in the owner type; a DIRECT command
/// cannot reinterpret a compute owner or release a graphics lease as compute.
enum PipelineBinding {
    Compute(ComputePipelineLease),
    Graphics(GraphicsPipelineLease),
}
impl PipelineBinding {
    fn adapter(&self) -> usize {
        match self {
            Self::Compute(lease) => lease.adapter(),
            Self::Graphics(lease) => lease.adapter(),
        }
    }
    fn owner(&self) -> usize {
        match self {
            Self::Compute(lease) => lease.owner(),
            Self::Graphics(lease) => lease.owner(),
        }
    }
    fn root_identity(&self) -> native_shaders::RootIdentity {
        match self {
            Self::Compute(lease) => lease.root_identity(),
            Self::Graphics(lease) => lease.root_identity(),
        }
    }
    fn matches_rtv(&self, target: &RtvLease) -> bool {
        matches!(self, Self::Graphics(lease) if lease.render_target_count()==1 && lease.render_target_format() == target.format() && lease.sample_count() == target.sample_count())
    }

    fn matches_dsv(&self, depth: Option<&RtvLease>) -> bool {
        match self {
            Self::Graphics(lease) => match depth {
                Some(depth) => {
                    lease.depth_format() == depth.format()
                        && lease.sample_count() == depth.sample_count()
                        && (depth.read_only_flags() & native_descriptors::READ_ONLY_DEPTH == 0
                            || !lease.depth_write())
                        && (depth.read_only_flags() & native_descriptors::READ_ONLY_STENCIL == 0
                            || !lease.stencil_write())
                }
                None => lease.depth_format() == 0,
            },
            Self::Compute(_) => depth.is_none(),
        }
    }
    fn matches_topology(&self, topology: u32) -> bool {
        match self {
            Self::Graphics(p) => {
                native_pipelines::topology_class(topology) == Some(p.topology_class())
            }
            _ => false,
        }
    }
    fn kind(&self) -> Kind {
        match self {
            Self::Compute(_) => Kind::Compute,
            Self::Graphics(_) => Kind::Direct,
        }
    }
    fn release(self) {
        match self {
            Self::Compute(lease) => native_pipelines::release_compute_pipeline(lease),
            Self::Graphics(lease) => native_pipelines::release_graphics_pipeline(lease),
        }
    }
}
struct List {
    scheduled_owner: u64,
    state: State,
    phase: Phase,
    kind: Kind,
    operating: bool,
    device: usize,
    device_cookie: u64,
    storage: usize,
    runtime_list: usize,
    runtime_error: usize,
    cookie: u64,
    claim: u64,
    allocator: Option<AllocatorLease>,
    copy: Option<CopyLease>,
    extra_copies: [Option<CopyLease>; COPY_EXTRA_LIMIT],
    buffer_states: [Option<BufferStateRecording>; BUFFER_STATE_LIMIT],
    pipeline: Option<PipelineBinding>,
    root: Option<EmptyRootBindingLease>,
    root_values: [u32; 64],
    cbv_bindings: CbvBindings,
    shader_heap: Option<native_descriptors::ShaderHeapLease>,
    table_handles: [u64; 64],
    dispatched: bool,
    direct_state: DirectState,
    draw_arguments: DrawArguments,
    index_binding: Option<IndexBinding>,
    vertex_bindings: VertexBindings,
    indexed_arguments: Option<IndexedArguments>,
    dispatch_arguments: DispatchArguments,
    compute_stream: [Option<ComputeRecording>; COMPUTE_STREAM_LIMIT],
    graphics_stream: GraphicsStream,
    active_queries: [Option<QueryKey>; 2],
    blend_constants: BlendConstants,
    direct_flags: u32,
    direct_topology: u32,
    direct_mode: DirectMode,
    render_target: Option<RtvLease>,
    extra_render_targets: [Option<RtvLease>; 7],
    depth_target: Option<RtvLease>,
    clear: Option<ClearRecording>,
    image_transition: Option<ImageTransitionRecording>,
    image_copy_peer: Option<ImageTransitionRecording>,
    image_copy_recorded: bool,
    resolve: Option<ResolveRecording>,
    resolve_transitions: [Option<ResolveTransition>; 2],
    generation: u64,
    id: u64,
    recording: Option<native_objects::AllocatorRecordingAssociation>,
}
impl List {
    const fn vacant() -> Self {
        Self {
            scheduled_owner: 0,
            state: State::Vacant,
            phase: Phase::Recording,
            kind: Kind::Copy,
            operating: false,
            device: 0,
            device_cookie: 0,
            storage: 0,
            runtime_list: 0,
            runtime_error: 0,
            cookie: 0,
            claim: 0,
            allocator: None,
            copy: None,
            extra_copies: [const { None }; COPY_EXTRA_LIMIT],
            buffer_states: [const { None }; BUFFER_STATE_LIMIT],
            pipeline: None,
            root: None,
            root_values: [0; 64],
            cbv_bindings: CbvBindings::new(),
            shader_heap: None,
            table_handles: [0; 64],
            dispatched: false,
            direct_state: DirectState {
                viewport_x: 0.0,
                viewport_y: 0.0,
                viewport_width: 0.0,
                viewport_height: 0.0,
                min_depth: 0.0,
                max_depth: 0.0,
                scissor_left: 0,
                scissor_top: 0,
                scissor_right: 0,
                scissor_bottom: 0,
                stencil_reference: 0,
            },
            draw_arguments: DrawArguments {
                vertices: 0,
                instances: 0,
                first_vertex: 0,
                first_instance: 0,
            },
            index_binding: None,
            vertex_bindings: VertexBindings::new(),
            indexed_arguments: None,
            blend_constants: BlendConstants::DEFAULT,
            dispatch_arguments: DispatchArguments { x: 0, y: 0, z: 0 },
            compute_stream: [const { None }; COMPUTE_STREAM_LIMIT],
            graphics_stream: GraphicsStream::new(),
            active_queries: [None; 2],
            direct_flags: 0,
            direct_topology: 0,
            direct_mode: DirectMode::State,
            render_target: None,
            extra_render_targets: [const { None }; 7],
            depth_target: None,
            clear: None,
            image_transition: None,
            image_copy_peer: None,
            image_copy_recorded: false,
            resolve: None,
            resolve_transitions: [const { None }; 2],
            generation: 0,
            id: 0,
            recording: None,
        }
    }
}
/// A queue owns these unique typed references after submission is attempted.
/// Returning them to an unchanged completed list preserves its previous lifetime
/// contract; reset/reused origins instead release them after actual completion.
struct RecordingOwners {
    allocator: Option<AllocatorLease>,
    copy: Option<CopyLease>,
    extra_copies: [Option<CopyLease>; COPY_EXTRA_LIMIT],
    buffer_states: [Option<BufferStateRecording>; BUFFER_STATE_LIMIT],
    pipeline: Option<PipelineBinding>,
    root: Option<EmptyRootBindingLease>,
    root_values: [u32; 64],
    cbv_bindings: CbvBindings,
    shader_heap: Option<native_descriptors::ShaderHeapLease>,
    table_handles: [u64; 64],
    dispatched: bool,
    direct_state: DirectState,
    draw_arguments: DrawArguments,
    index_binding: Option<IndexBinding>,
    vertex_bindings: VertexBindings,
    indexed_arguments: Option<IndexedArguments>,
    dispatch_arguments: DispatchArguments,
    compute_stream: [Option<ComputeRecording>; COMPUTE_STREAM_LIMIT],
    graphics_stream: GraphicsStream,
    active_queries: [Option<QueryKey>; 2],
    blend_constants: BlendConstants,
    direct_flags: u32,
    direct_topology: u32,
    render_target: Option<RtvLease>,
    extra_render_targets: [Option<RtvLease>; 7],
    depth_target: Option<RtvLease>,
    clear: Option<ClearRecording>,
    image_transition: Option<ImageTransitionRecording>,
    image_copy_peer: Option<ImageTransitionRecording>,
    image_copy_recorded: bool,
    resolve: Option<ResolveRecording>,
    resolve_transitions: [Option<ResolveTransition>; 2],
}
#[derive(Clone, Copy)]
struct ListOrigin {
    index: usize,
    storage: usize,
    cookie: u64,
    generation: u64,
}
const BUFFER_STATE_LIMIT: usize = 16;
struct BufferStateRecording {
    owner: native_resources::BufferStateLease,
    current: u32,
}
// A genuine DIRECT list may select the copy workflow while still empty.
// Existing graphics/attachment state cannot be silently discarded or mixed.
fn buffer_copy_allowed(list: &List) -> bool {
    list.kind == Kind::Copy
        || (list.kind == Kind::Direct
            && list.runtime_error != 0
            && (list.direct_mode == DirectMode::BufferCopy
                || (list.direct_mode == DirectMode::Draw
                    && !list.dispatched
                    && list.pipeline.is_none()
                    && list.root.is_none()
                    && list.direct_flags == 0
                    && list.render_target.is_none()
                    && list.depth_target.is_none()
                    && list.clear.is_none()
                    && list.image_transition.is_none()
                    && list.resolve.is_none()
                    && list.resolve_transitions.iter().all(Option::is_none))))
}
fn select_buffer_copy(list: &mut List) {
    if list.kind == Kind::Direct {
        list.direct_mode = DirectMode::BufferCopy;
    }
}
fn buffer_transactions_ready(list: &List) -> bool {
    list.buffer_states.iter().flatten().all(|state| {
        state.current == state.owner.initial
            || (state.owner.initial == 0
                && native_resources::promotable_buffer_state(state.current))
    })
}
fn buffer_state_for(list: &List, resource: usize, cookie: u64) -> Option<u32> {
    list.buffer_states
        .iter()
        .flatten()
        .find(|state| state.owner.resource == resource && state.owner.resource_cookie == cookie)
        .map(|state| state.current)
}
struct PendingSubmission {
    origin: ListOrigin,
    owners: RecordingOwners,
    accepted: bool,
    value: u64,
}
fn take_owners(list: &mut List) -> RecordingOwners {
    RecordingOwners {
        allocator: list.allocator.take(),
        copy: list.copy.take(),
        extra_copies: core::mem::replace(
            &mut list.extra_copies,
            [const { None }; COPY_EXTRA_LIMIT],
        ),
        buffer_states: core::mem::replace(
            &mut list.buffer_states,
            [const { None }; BUFFER_STATE_LIMIT],
        ),
        pipeline: list.pipeline.take(),
        root: list.root.take(),
        root_values: core::mem::replace(&mut list.root_values, [0; 64]),
        cbv_bindings: core::mem::take(&mut list.cbv_bindings),
        shader_heap: list.shader_heap.take(),
        table_handles: core::mem::replace(&mut list.table_handles, [0; 64]),
        dispatched: core::mem::take(&mut list.dispatched),
        direct_state: core::mem::take(&mut list.direct_state),
        draw_arguments: core::mem::take(&mut list.draw_arguments),
        index_binding: list.index_binding.take(),
        vertex_bindings: core::mem::take(&mut list.vertex_bindings),
        indexed_arguments: list.indexed_arguments.take(),
        dispatch_arguments: core::mem::take(&mut list.dispatch_arguments),
        compute_stream: core::mem::replace(
            &mut list.compute_stream,
            [const { None }; COMPUTE_STREAM_LIMIT],
        ),
        graphics_stream: core::mem::take(&mut list.graphics_stream),
        active_queries: core::mem::replace(&mut list.active_queries, [None; 2]),
        blend_constants: core::mem::take(&mut list.blend_constants),
        direct_flags: core::mem::take(&mut list.direct_flags),
        direct_topology: core::mem::take(&mut list.direct_topology),
        render_target: list.render_target.take(),
        extra_render_targets: core::mem::replace(
            &mut list.extra_render_targets,
            [const { None }; 7],
        ),
        depth_target: list.depth_target.take(),
        clear: list.clear.take(),
        image_transition: list.image_transition.take(),
        image_copy_peer: list.image_copy_peer.take(),
        image_copy_recorded: core::mem::take(&mut list.image_copy_recorded),
        resolve: list.resolve.take(),
        resolve_transitions: core::mem::replace(&mut list.resolve_transitions, [const { None }; 2]),
    }
}
fn restore_owners(list: &mut List, owners: RecordingOwners) {
    assert!(
        list.allocator.is_none()
            && list.copy.is_none()
            && list.extra_copies.iter().all(Option::is_none)
            && list.buffer_states.iter().all(Option::is_none)
            && list.compute_stream.iter().all(Option::is_none)
            && list.graphics_stream.head == 0
            && list.active_queries.iter().all(Option::is_none)
            && list.index_binding.is_none()
            && list.vertex_bindings.head == 0
            && list.pipeline.is_none()
            && list.root.is_none()
            && list.shader_heap.is_none()
            && list.render_target.is_none()
            && list.extra_render_targets.iter().all(Option::is_none)
            && list.depth_target.is_none()
            && list.clear.is_none()
            && list.image_transition.is_none()
            && list.image_copy_peer.is_none()
            && list.resolve.is_none()
            && list.resolve_transitions.iter().all(Option::is_none),
        "origin list has no duplicate recording owners"
    );
    list.allocator = owners.allocator;
    list.copy = owners.copy;
    list.extra_copies = owners.extra_copies;
    list.buffer_states = owners.buffer_states;
    list.pipeline = owners.pipeline;
    list.root = owners.root;
    list.root_values = owners.root_values;
    list.cbv_bindings = owners.cbv_bindings;
    list.shader_heap = owners.shader_heap;
    list.table_handles = owners.table_handles;
    list.dispatched = owners.dispatched;
    list.direct_state = owners.direct_state;
    list.draw_arguments = owners.draw_arguments;
    list.index_binding = owners.index_binding;
    list.vertex_bindings = owners.vertex_bindings;
    list.indexed_arguments = owners.indexed_arguments;
    list.dispatch_arguments = owners.dispatch_arguments;
    list.compute_stream = owners.compute_stream;
    list.graphics_stream = owners.graphics_stream;
    list.active_queries = owners.active_queries;
    list.blend_constants = owners.blend_constants;
    list.direct_flags = owners.direct_flags;
    list.direct_topology = owners.direct_topology;
    list.render_target = owners.render_target;
    list.extra_render_targets = owners.extra_render_targets;
    list.depth_target = owners.depth_target;
    list.clear = owners.clear;
    list.image_transition = owners.image_transition;
    list.image_copy_peer = owners.image_copy_peer;
    list.image_copy_recorded = owners.image_copy_recorded;
    list.resolve = owners.resolve;
    list.resolve_transitions = owners.resolve_transitions;
}
fn release_owners(owners: RecordingOwners) {
    if let Some(heap) = owners.shader_heap {
        native_descriptors::release_shader_heap(heap);
    }
    release_cbv_bindings(owners.cbv_bindings);
    release_vertex_bindings(owners.vertex_bindings);
    release_graphics_stream(owners.graphics_stream);
    if let Some(binding) = owners.index_binding {
        native_resources::release_index_range(binding.range);
    }
    if let Some(peer) = owners.image_copy_peer {
        assert!(peer.upload.is_none() && peer.readback.is_none());
        native_images::release_initialized_image(peer.image);
    }
    for command in owners.compute_stream.into_iter().flatten() {
        release_compute_recording(command);
    }
    for state in owners.buffer_states.into_iter().flatten() {
        native_resources::release_buffer_state(state.owner);
    }
    for copy in owners.extra_copies.into_iter().flatten() {
        native_resources::release_copy(copy);
    }
    for transition in owners.resolve_transitions.into_iter().flatten() {
        native_images::release_initialized_image(transition.image);
    }
    if let Some(resolve) = owners.resolve {
        native_images::release_initialized_image(resolve.source);
        native_images::release_initialized_image(resolve.destination);
    }
    if let Some(copy) = owners.copy {
        native_resources::release_copy(copy);
    }
    if let Some(pipeline) = owners.pipeline {
        pipeline.release();
    }
    if let Some(root) = owners.root {
        native_shaders::release_empty_root_binding(root);
    }
    if let Some(target) = owners.depth_target {
        native_descriptors::release_rtv(target);
    }
    release_extra_targets(owners.extra_render_targets);
    if let Some(target) = owners.render_target {
        native_descriptors::release_rtv(target);
    }
    if let Some(clear) = owners.clear {
        native_descriptors::release_rtv(clear.target);
    }
    if let Some(transition) = owners.image_transition {
        if let Some(readback) = transition.readback {
            native_resources::release_readback(readback.buffer);
        }
        if let Some(upload) = transition.upload {
            native_resources::release_upload(upload.buffer);
        }
        native_images::release_initialized_image(transition.image);
    }
    if let Some(allocator) = owners.allocator {
        native_objects::release_command_allocator(allocator);
    }
}
fn origin_matches(registry: &Registry, origin: ListOrigin) -> bool {
    let list = &registry.lists[origin.index];
    list.state == State::Live
        && list.storage == origin.storage
        && list.cookie == origin.cookie
        && list.generation == origin.generation
}
struct Queue {
    managed_fences: bool,
    presentation_references: u32,
    state: State,
    kind: Kind,
    operating: bool,
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    claim: u64,
    adapter: usize,
    wddm_owner: usize,
    pending: Option<PendingSubmission>,
    fault: i32,
    has_submission: bool,
}
impl Queue {
    const fn vacant() -> Self {
        Self {
            managed_fences: false,
            presentation_references: 0,
            state: State::Vacant,
            kind: Kind::Copy,
            operating: false,
            device: 0,
            device_cookie: 0,
            storage: 0,
            cookie: 0,
            claim: 0,
            adapter: 0,
            wddm_owner: 0,
            pending: None,
            fault: 0,
            has_submission: false,
        }
    }
}
struct Registry {
    lists: [List; CAPACITY],
    queues: [Queue; CAPACITY],
    next_generation: u64,
}
impl Registry {
    const fn new() -> Self {
        Self {
            lists: [const { List::vacant() }; CAPACITY],
            queues: [const { Queue::vacant() }; CAPACITY],
            next_generation: 1,
        }
    }
    fn list(&self, storage: usize, cookie: u64) -> Option<usize> {
        self.lists.iter().position(|slot| {
            slot.state == State::Live && slot.storage == storage && slot.cookie == cookie
        })
    }
    fn queue(&self, storage: usize, cookie: u64) -> Option<usize> {
        self.queues.iter().position(|slot| {
            slot.state == State::Live && slot.storage == storage && slot.cookie == cookie
        })
    }
    fn queue_for_retirement(&self, storage: usize, cookie: u64) -> Option<usize> {
        self.queues.iter().position(|slot| {
            matches!(slot.state, State::Live | State::Retiring)
                && slot.storage == storage
                && slot.cookie == cookie
        })
    }
}
struct Shared(UnsafeCell<Registry>);
// All registries use the common policy lock. Never call a second registry,
// backend operation or runtime callback while holding it.
unsafe impl Sync for Shared {}
static REGISTRY: Shared = Shared(UnsafeCell::new(Registry::new()));
fn with_registry<T>(operation: impl FnOnce(&mut Registry) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let result = operation(&mut *REGISTRY.0.get());
        ql_dx12_policy_unlock();
        result
    }
}
fn release_claim(claim: u64) {
    assert!(
        native_objects::ql_dx12_native_external_child_release(claim) >= 0,
        "native command object owns child claim"
    );
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_queue_create(
    device: usize,
    device_cookie: u64,
    storage: usize,
    runtime_queue: usize,
    flags: u32,
    output: *mut u64,
) -> i32 {
    if output.is_null() || runtime_queue == 0 {
        return INVALID;
    }
    let kind = match flags {
        native_objects::QUEUE_FLAG_COPY => Kind::Copy,
        native_objects::QUEUE_FLAG_COMPUTE => Kind::Compute,
        native_objects::QUEUE_FLAG_3D => Kind::Direct,
        _ => return INVALID,
    };
    let callbacks = match native_objects::wddm_queue_callbacks(device, device_cookie) {
        Ok(callbacks) => callbacks,
        Err(status) => return status,
    };
    let (claim, adapter, cookie) =
        match native_objects::claim_one(device, device_cookie, storage, KIND_COMMAND_QUEUE) {
            Ok(claim) => claim,
            Err(status) => return status,
        };
    let index = with_registry(|registry| {
        if registry.queues.iter().any(|slot| {
            slot.state != State::Vacant && slot.adapter == adapter && !slot.managed_fences
        }) {
            return Err(BUSY);
        }
        let index = registry
            .queues
            .iter()
            .position(|slot| slot.state == State::Vacant)
            .ok_or(NO_MEMORY)?;
        registry.queues[index] = Queue {
            state: State::Creating,
            kind,
            adapter,
            ..Queue::vacant()
        };
        Ok(index)
    });
    let index = match index {
        Ok(index) => index,
        Err(status) => {
            release_claim(claim);
            return status;
        }
    };
    let mut status = unsafe { ql_dx12_native_queue_prepare(adapter) };
    if status >= 0 {
        status = native_objects::ql_dx12_native_external_child_publish(claim);
    }
    // The WDK shim pins the runtime device until it publishes the cookie.
    // Acquire the context last so success has an infallible commit, even if
    // the runtime reports device loss during its callback.
    let mut wddm_owner = 0;
    if status >= 0 {
        if let Some(callbacks) = callbacks {
            status =
                unsafe { ql_dx12_wddm_context_create(&callbacks, runtime_queue, &mut wddm_owner) };
        }
    }
    if status < 0 {
        with_registry(|registry| registry.queues[index] = Queue::vacant());
        release_claim(claim);
        return status;
    }
    with_registry(|registry| {
        registry.queues[index] = Queue {
            state: State::Live,
            kind,
            device,
            device_cookie,
            storage,
            cookie,
            claim,
            adapter,
            wddm_owner,
            ..Queue::vacant()
        }
    });
    unsafe { output.write(cookie) };
    0
}

unsafe fn create_list(
    device: usize,
    device_cookie: u64,
    storage: usize,
    allocator: usize,
    allocator_cookie: u64,
    kind: Kind,
    direct_mode: DirectMode,
    output: *mut u64,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    let (claim, _, cookie) =
        match native_objects::claim_one(device, device_cookie, storage, KIND_COMMAND_LIST) {
            Ok(claim) => claim,
            Err(status) => return status,
        };
    let retained = match kind {
        Kind::Copy => native_objects::retain_copy_allocator(
            device,
            device_cookie,
            allocator,
            allocator_cookie,
        ),
        Kind::Compute => native_objects::retain_compute_allocator(
            device,
            device_cookie,
            allocator,
            allocator_cookie,
        ),
        Kind::Direct => native_objects::retain_direct_allocator(
            device,
            device_cookie,
            allocator,
            allocator_cookie,
        ),
    };
    let allocator = match retained {
        Ok(allocator) => allocator,
        Err(status) => {
            release_claim(claim);
            return status;
        }
    };
    let recording = match native_objects::begin_allocator_recording(&allocator) {
        Ok(recording) => recording,
        Err(status) => {
            native_objects::release_command_allocator(allocator);
            release_claim(claim);
            return status;
        }
    };
    let index = with_registry(|registry| {
        let index = registry
            .lists
            .iter()
            .position(|slot| slot.state == State::Vacant)?;
        let generation = registry.next_generation;
        registry.next_generation = generation.checked_add(1)?;
        registry.lists[index].state = State::Creating;
        Some((index, generation))
    });
    let Some((index, generation)) = index else {
        native_objects::end_allocator_recording(recording);
        native_objects::release_command_allocator(allocator);
        release_claim(claim);
        return NO_MEMORY;
    };
    let status = native_objects::ql_dx12_native_external_child_publish(claim);
    if status < 0 {
        with_registry(|registry| registry.lists[index] = List::vacant());
        native_objects::end_allocator_recording(recording);
        native_objects::release_command_allocator(allocator);
        release_claim(claim);
        return status;
    }
    with_registry(|registry| {
        registry.lists[index] = List {
            state: State::Live,
            kind,
            direct_mode,
            device,
            device_cookie,
            storage,
            cookie,
            claim,
            allocator: Some(allocator),
            recording: Some(recording),
            generation,
            ..List::vacant()
        }
    });
    unsafe { output.write(cookie) };
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_copy_list_poison(storage: usize, cookie: u64) {
    with_registry(|registry| {
        if let Some(index) = registry.list(storage, cookie) {
            let slot = &mut registry.lists[index];
            if !slot.operating && !matches!(slot.phase, Phase::Pending | Phase::Complete) {
                slot.phase = Phase::Invalid;
            }
        }
    });
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_copy_record(
    storage: usize,
    cookie: u64,
    source: usize,
    source_cookie: u64,
    destination: usize,
    destination_cookie: u64,
    source_offset: u64,
    destination_offset: u64,
    bytes: u64,
) -> i32 {
    record_buffer_copy(
        storage,
        cookie,
        source,
        source_cookie,
        destination,
        destination_cookie,
        source_offset,
        destination_offset,
        bytes,
        false,
    )
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_resource_copy_record(
    storage: usize,
    cookie: u64,
    source: usize,
    source_cookie: u64,
    destination: usize,
    destination_cookie: u64,
) -> i32 {
    let image_copy = with_registry(|registry| {
        registry
            .list(storage, cookie)
            .is_some_and(|index| registry.lists[index].direct_mode == DirectMode::ImageCopy)
    });
    if image_copy {
        return record_image_copy(
            storage,
            cookie,
            source,
            source_cookie,
            destination,
            destination_cookie,
        );
    }
    record_buffer_copy(
        storage,
        cookie,
        source,
        source_cookie,
        destination,
        destination_cookie,
        0,
        0,
        0,
        true,
    )
}
fn record_buffer_copy(
    storage: usize,
    cookie: u64,
    source: usize,
    source_cookie: u64,
    destination: usize,
    destination_cookie: u64,
    source_offset: u64,
    destination_offset: u64,
    bytes: u64,
    whole_resource: bool,
) -> i32 {
    let owned = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        if !buffer_copy_allowed(list)
            || list.phase != Phase::Recording
            || (list.copy.is_some() && list.runtime_error == 0)
        {
            if !matches!(list.phase, Phase::Pending | Phase::Complete) {
                list.phase = Phase::Invalid;
            }
            return Err(INVALID);
        }
        let target = if list.copy.is_none() {
            0
        } else {
            match list.extra_copies.iter().position(Option::is_none) {
                Some(slot) => slot + 1,
                None => {
                    list.phase = Phase::Invalid;
                    return Err(NO_MEMORY);
                }
            }
        };
        let source_state = buffer_state_for(list, source, source_cookie);
        let destination_state = buffer_state_for(list, destination, destination_cookie);
        list.operating = true;
        Ok((
            index,
            list.device,
            list.device_cookie,
            target,
            list.runtime_error != 0,
            source_state,
            destination_state,
        ))
    });
    let (index, device, device_cookie, target, unified, source_state, destination_state) =
        match owned {
            Ok(owned) => owned,
            Err(status) => return status,
        };
    let copy = match native_resources::retain_copy(
        device,
        device_cookie,
        source,
        source_cookie,
        destination,
        destination_cookie,
        source_offset,
        destination_offset,
        bytes,
        unified,
        whole_resource,
        source_state,
        destination_state,
    ) {
        Ok(copy) => copy,
        Err(status) => {
            binding_failed(index);
            return status;
        }
    };
    let mut candidate = Some(copy);
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        // The exact list stays Operating across unlocked typed resource pinning.
        // Fault admission and publication share the common policy lock.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        assert!(list.cookie == cookie && list.operating && list.phase == Phase::Recording);
        select_buffer_copy(list);
        if target == 0 {
            list.copy = candidate.take();
        } else {
            list.extra_copies[target - 1] = candidate.take();
        }
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(status) => {
            native_resources::release_copy(
                candidate.expect("failed copy admission retains candidate"),
            );
            binding_failed(index);
            status
        }
    }
}
/// Install immutable runtime identity before the shim publishes the list cookie.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_list_set_runtime(
    storage: usize,
    cookie: u64,
    runtime: usize,
    callback: usize,
) {
    with_registry(|registry| {
        let index = registry
            .list(storage, cookie)
            .expect("new command list has unpublished storage");
        let list = &mut registry.lists[index];
        list.runtime_list = runtime;
        list.runtime_error = callback;
    });
}

/// Snapshot recorded callback values while the shim holds a device lease.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_list_runtime_error(
    storage: usize,
    cookie: u64,
    runtime: *mut usize,
) -> usize {
    with_registry(|registry| {
        let Some(index) = registry.list(storage, cookie) else {
            return 0;
        };
        let list = &registry.lists[index];
        unsafe { runtime.write(list.runtime_list) };
        list.runtime_error
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_list_queue_flags(storage: usize, cookie: u64) -> u32 {
    with_registry(|registry| {
        registry
            .list(storage, cookie)
            .map_or(0, |index| match registry.lists[index].kind {
                Kind::Copy => native_objects::QUEUE_FLAG_COPY,
                Kind::Compute => native_objects::QUEUE_FLAG_COMPUTE,
                Kind::Direct => native_objects::QUEUE_FLAG_3D,
            })
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_copy_close(storage: usize, cookie: u64) -> i32 {
    let device = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        Ok((
            registry.lists[index].device,
            registry.lists[index].device_cookie,
        ))
    });
    let (device, device_cookie) = match device {
        Ok(device) => device,
        Err(status) => return status,
    };
    let closed = native_objects::with_device_admission(device, device_cookie, || {
        // The same policy lock protects native loss admission and command state.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        // Genuine unified R0 lists start in draw mode. An OM-only recording
        // still has a valid LOAD-preserving target operation without a draw.
        if list.kind == Kind::Direct
            && list.direct_mode == DirectMode::Draw
            && list.runtime_error != 0
            && !list.dispatched
            && list.render_target.is_some()
        {
            list.direct_mode = DirectMode::RenderTarget;
        }
        if !buffer_transactions_ready(list) {
            return Err(INVALID);
        }
        let recorded = empty_recording(list) || match list.kind {
            Kind::Copy => list.copy.is_some() && buffer_transactions_ready(list),
            Kind::Direct if list.direct_mode == DirectMode::BufferCopy => {
                list.copy.is_some() && buffer_transactions_ready(list)
            }
            Kind::Compute => list.dispatched,
            Kind::Direct if list.direct_mode == DirectMode::ImageCopy => image_copy_closed(list),
            Kind::Direct if list.direct_mode == DirectMode::RenderTarget => {
                (list.resolve.is_some() && resolve_transaction_closed(list))
                    || (list.resolve_transitions.iter().all(Option::is_none)
                        && render_target_work(list).is_some())
            }
            Kind::Direct
                if matches!(
                    list.direct_mode,
                    DirectMode::ImageTransition
                        | DirectMode::ImageReadback
                        | DirectMode::ImageUpload
                ) =>
            {
                list.image_transition.as_ref().is_some_and(|transition| {
                    transition.closed
                        && (list.direct_mode != DirectMode::ImageReadback
                            || transition.readback.is_some())
                        && (list.direct_mode != DirectMode::ImageUpload
                            || transition.upload.is_some())
                })
            }
            Kind::Direct if list.direct_mode == DirectMode::Draw && list.runtime_error != 0 => {
                list.graphics_stream.count != 0 && list.active_queries.iter().all(Option::is_none)
            }
            Kind::Direct => {
                (list.direct_mode != DirectMode::Draw
                    || (list.dispatched && list.render_target.is_some()))
                    && list.direct_flags == DIRECT_COMPLETE_STATE
                    && list.direct_state.supported()
                    && list.pipeline.as_ref().zip(list.root.as_ref()).is_some_and(
                        |(pipeline, root)| {
                            pipeline.kind() == Kind::Direct
                                && pipeline.root_identity() == root.identity()
                        },
                    )
            }
        };
        if list.phase != Phase::Recording || !recorded {
            if list.phase != Phase::Pending {
                list.phase = Phase::Invalid;
            }
            return Err(INVALID);
        }
        list.phase = Phase::Closed;
        list.operating = true;
        Ok((
            index,
            list.recording
                .take()
                .expect("recording list owns allocator association"),
        ))
    });
    let (index, recording) = match closed {
        Ok(closed) => closed,
        Err(status) => return status,
    };
    native_objects::end_allocator_recording(recording);
    with_registry(|registry| {
        registry.lists[index].operating = false;
    });
    0
}

fn valid_result(result: TransferResult) -> bool {
    result.attempted <= 1
        && result.accepted <= result.attempted
        && result.drain_complete <= 1
        && result.reserved == 0
        && (result.accepted == 0 || result.submitted != 0)
        && result.completed <= result.submitted
}
fn observe(
    registry: &mut Registry,
    queue: usize,
    result: TransferResult,
    status: i32,
    attempted_call: bool,
) -> Option<RecordingOwners> {
    let origin = registry.queues[queue]
        .pending
        .as_ref()
        .map(|pending| pending.origin);
    registry.queues[queue].operating = false;
    if attempted_call {
        if let Some(origin) = origin {
            if origin_matches(registry, origin) {
                registry.lists[origin.index].operating = false;
            }
        }
    }
    let slot = &mut registry.queues[queue];
    let valid = valid_result(result);
    if status < 0
        && slot.fault >= 0
        && (result.attempted != 0 || (!attempted_call && status != BUSY))
    {
        slot.fault = status;
    }
    if !valid {
        if slot.fault >= 0 {
            slot.fault = FAIL;
        }
        return None;
    }
    if attempted_call && result.attempted != 0 {
        slot.has_submission = true;
        if let Some(pending) = slot.pending.as_mut() {
            pending.accepted = result.accepted == 1;
            pending.value = if pending.accepted {
                result.submitted
            } else {
                0
            };
        }
    }
    let complete = result.drain_complete != 0
        || (status >= 0
            && slot.pending.as_ref().is_some_and(|pending| {
                pending.accepted && pending.value != 0 && result.completed >= pending.value
            }));
    let no_attempt = attempted_call && result.attempted == 0;
    if !complete && !no_attempt {
        return None;
    }
    let pending = slot.pending.take()?;
    if origin_matches(registry, pending.origin) {
        let list = &mut registry.lists[pending.origin.index];
        assert!(
            !list.operating && list.phase == Phase::Pending,
            "submit receipt freezes exact origin until commit"
        );
        restore_owners(list, pending.owners);
        list.phase = if no_attempt {
            Phase::Closed
        } else if status == native_objects::DEVICE_REMOVED {
            Phase::Invalid
        } else {
            Phase::Complete
        };
        None
    } else {
        Some(pending.owners)
    }
}

// State setters without consumed GPU work form a valid genuine empty list.
// Closed resource-state transactions are checked separately by Close/Execute.
fn empty_recording(list: &List) -> bool {
    list.runtime_error != 0
        && match list.kind {
            Kind::Copy => list.copy.is_none(),
            Kind::Compute => !list.dispatched,
            Kind::Direct => {
                list.direct_mode == DirectMode::Draw
                    && list.graphics_stream.count == 0
                    && list.render_target.is_none()
            }
        }
}

enum Submission {
    Empty { pool: usize },
    ImageCopy {
        pool: usize,
        source: u64,
        destination: u64,
    },
    Resolve {
        pool: usize,
        source: u64,
        destination: u64,
        source_layer: u32,
        destination_layer: u32,
    },
    ImageTransition {
        pool: usize,
        image_token: u64,
        layer: u32,
        readback: Option<(u64, ImageReadbackFootprint)>,
        upload: Option<(u64, ImageUploadFootprint)>,
    },
    RenderTarget {
        pool: usize,
        binding: usize,
        clear: usize,
        work: RenderTargetWork,
    },
    Copy {
        pool: usize,
        count: u32,
        commands: [TransferCommand; COPY_BATCH_LIMIT],
    },
    Compute {
        pool: usize,
        count: u32,
        pipelines: [usize; COMPUTE_STREAM_LIMIT],
        arguments: [DispatchArguments; COMPUTE_STREAM_LIMIT],
        constants: [RootConstants; COMPUTE_STREAM_LIMIT],
        indirect:[IndirectWork;COMPUTE_STREAM_LIMIT],
    },
    Graphics {
        pool: usize,
        head: usize,
        count: u32,
    },
    Direct {
        pool: usize,
        pipeline: usize,
        target: Option<usize>,
        depth_target: usize,
        state: DirectState,
        draw_arguments: DrawArguments,
        blend_constants: BlendConstants,
        indexed: Option<IndexedWork>,
    },
}
fn buffer_copy_submission(list: &List, adapter: usize, pool: usize) -> Result<Submission, i32> {
    if !buffer_transactions_ready(list) {
        return Err(INVALID);
    }
    let primary = list.copy.as_ref().ok_or(INVALID)?;
    let mut commands = [TransferCommand::default(); COPY_BATCH_LIMIT];
    let mut count = 0;
    for copy in core::iter::once(primary).chain(list.extra_copies.iter().flatten()) {
        if copy.adapter != adapter {
            return Err(INVALID);
        }
        commands[count] = TransferCommand {
            version: crate::transfer::VERSION,
            size: core::mem::size_of::<TransferCommand>() as u32,
            source_token: copy.source_token,
            destination_token: copy.destination_token,
            source_offset: copy.source_offset,
            destination_offset: copy.destination_offset,
            byte_size: copy.bytes,
            ..TransferCommand::default()
        };
        count += 1;
    }
    Ok(Submission::Copy {
        pool,
        count: count as u32,
        commands,
    })
}
unsafe extern "C" {
    fn ql_dx12_fence_queue_execute(queue: usize, cookie: u64, list: usize, list_cookie: u64)
    -> i32;
    fn ql_dx12_fence_queue_detach(queue: usize, cookie: u64) -> i32;
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_copy_execute(
    queue: usize,
    queue_cookie: u64,
    list: usize,
    list_cookie: u64,
) -> i32 {
    let managed = with_registry(|registry| {
        registry
            .queue(queue, queue_cookie)
            .is_some_and(|index| registry.queues[index].managed_fences)
    });
    if managed {
        unsafe { ql_dx12_fence_queue_execute(queue, queue_cookie, list, list_cookie) }
    } else {
        execute_immediate(queue, queue_cookie, list, list_cookie, None)
    }
}
// Genuine native lists can execute again after their previous receipt is
// observed complete. Pending/uncertain work is never eligible; private fixed
// bundles preserve their historical close-only execution policy.
fn executable(list: &List) -> bool {
    list.phase == Phase::Closed || (list.runtime_error != 0 && list.phase == Phase::Complete)
}
fn execute_immediate(
    queue: usize,
    queue_cookie: u64,
    list: usize,
    list_cookie: u64,
    scheduled: Option<u64>,
) -> i32 {
    let owned = with_registry(|registry| {
        let qi = registry.queue(queue, queue_cookie).ok_or(INVALID)?;
        let li = registry.list(list, list_cookie).ok_or(INVALID)?;
        let queue = &registry.queues[qi];
        let list = &registry.lists[li];
        if queue.device != list.device
            || queue.device_cookie != list.device_cookie
            || queue.kind != list.kind
        {
            return Err(INVALID);
        }
        if queue.operating
            || queue.pending.is_some()
            || (list.operating && !(scheduled.is_some() && list.scheduled_owner == queue_cookie))
            || list.phase == Phase::Pending
        {
            return Err(BUSY);
        }
        if queue.fault < 0 {
            return Err(queue.fault);
        }
        if scheduled.is_some_and(|generation| {
            list.generation != generation || list.scheduled_owner != queue_cookie
        }) {
            return Err(INVALID);
        }
        if !executable(list) {
            return Err(INVALID);
        }
        let allocator = list
            .allocator
            .as_ref()
            .expect("closed list retains allocator");
        if allocator.adapter != queue.adapter {
            return Err(INVALID);
        }
        if !buffer_transactions_ready(list) {
            return Err(INVALID);
        }
        let submission = match list.kind {
            _ if empty_recording(list) => Submission::Empty { pool: allocator.pool },
            Kind::Copy => buffer_copy_submission(list, queue.adapter, allocator.pool)?,
            Kind::Direct if list.direct_mode == DirectMode::BufferCopy => {
                buffer_copy_submission(list, queue.adapter, allocator.pool)?
            }
            Kind::Direct if list.direct_mode == DirectMode::ImageCopy => {
                if !image_copy_closed(list) {
                    return Err(INVALID);
                }
                let (source, destination) = image_copy_operands(list).ok_or(INVALID)?;
                if source.image.adapter() != queue.adapter
                    || destination.image.adapter() != queue.adapter
                {
                    return Err(INVALID);
                }
                Submission::ImageCopy {
                    pool: allocator.pool,
                    source: source.image.token(),
                    destination: destination.image.token(),
                }
            }
            Kind::Compute if list.graphics_stream.count != 0 => Submission::Graphics {
                pool: allocator.pool, head: list.graphics_stream.head, count: list.graphics_stream.count,
            },
            Kind::Compute => {
                if !list.dispatched {
                    return Err(INVALID);
                }
                let mut pipelines = [0; COMPUTE_STREAM_LIMIT];
                let mut arguments = [DispatchArguments::default(); COMPUTE_STREAM_LIMIT];
                let mut constants = [RootConstants::default(); COMPUTE_STREAM_LIMIT];
                let mut indirect=[IndirectWork::default();COMPUTE_STREAM_LIMIT];
                let mut count = 0;
                if list.runtime_error != 0 {
                    for command in list.compute_stream.iter().flatten() {
                        if command.pipeline.adapter() != queue.adapter
                            || !unsafe {
                                command.root.matches_pipeline_locked(command.pipeline.root_identity())
                            }
                        {
                            return Err(INVALID);
                        }
                        pipelines[count] = command.pipeline.owner();
                        arguments[count] = command.arguments;
                        constants[count] = command.constants;
                        indirect[count]=command.indirect_work;
                        count += 1;
                    }
                    if count == 0 {
                        return Err(INVALID);
                    }
                } else {
                    let pipeline = list.pipeline.as_ref().ok_or(INVALID)?;
                    let root = list.root.as_ref().ok_or(INVALID)?;
                    if pipeline.kind() != Kind::Compute
                        || pipeline.adapter() != queue.adapter
                        || pipeline.root_identity() != root.identity()
                    {
                        return Err(INVALID);
                    }
                    pipelines[0] = pipeline.owner();
                    arguments[0] = list.dispatch_arguments;
                    count = 1;
                }
                Submission::Compute {
                    pool: allocator.pool,
                    count: count as u32,
                    pipelines,
                    arguments,
                    constants,
                    indirect,
                }
            }
            Kind::Direct
                if matches!(
                    list.direct_mode,
                    DirectMode::ImageTransition
                        | DirectMode::ImageReadback
                        | DirectMode::ImageUpload
                ) =>
            {
                let transition = list.image_transition.as_ref().ok_or(INVALID)?;
                if !transition.closed || transition.image.adapter() != queue.adapter {
                    return Err(INVALID);
                }
                Submission::ImageTransition {
                    pool: allocator.pool,
                    image_token: transition.image.token(),
                    layer: transition.layer,
                    readback: transition
                        .readback
                        .as_ref()
                        .map(|readback| (readback.buffer.token(), readback.footprint)),
                    upload: transition
                        .upload
                        .as_ref()
                        .map(|upload| (upload.buffer.token(), upload.footprint)),
                }
            }
            Kind::Direct
                if list.direct_mode == DirectMode::RenderTarget && list.resolve.is_some() =>
            {
                let resolve = list.resolve.as_ref().ok_or(INVALID)?;
                if !resolve_transaction_closed(list) {
                    return Err(INVALID);
                }
                if resolve.source.adapter() != queue.adapter
                    || resolve.destination.adapter() != queue.adapter
                {
                    return Err(INVALID);
                }
                Submission::Resolve {
                    pool: allocator.pool,
                    source: resolve.source.token(),
                    destination: resolve.destination.token(),
                    source_layer: resolve.source_layer,
                    destination_layer: resolve.destination_layer,
                }
            }
            Kind::Direct if list.direct_mode == DirectMode::RenderTarget => {
                let work = render_target_work(list).ok_or(INVALID)?;

                if list
                    .render_target
                    .as_ref()
                    .is_some_and(|target| target.adapter() != queue.adapter)
                    || list
                        .clear
                        .as_ref()
                        .is_some_and(|clear| clear.target.adapter() != queue.adapter)
                {
                    return Err(INVALID);
                }
                Submission::RenderTarget {
                    pool: allocator.pool,
                    binding: list
                        .render_target
                        .as_ref()
                        .map_or(0, |target| target.owner()),
                    clear: list.clear.as_ref().map_or(0, |clear| clear.target.owner()),
                    work,
                }
            }
            Kind::Direct if list.direct_mode == DirectMode::Draw && list.runtime_error != 0 => {
                if list.graphics_stream.count == 0 || !list.dispatched {
                    return Err(INVALID);
                }
                Submission::Graphics {
                    pool: allocator.pool,
                    head: list.graphics_stream.head,
                    count: list.graphics_stream.count,
                }
            }
            Kind::Direct => {
                let pipeline = list.pipeline.as_ref().ok_or(INVALID)?;
                let root = list.root.as_ref().ok_or(INVALID)?;
                if list.direct_flags != DIRECT_COMPLETE_STATE
                    || !list.direct_state.supported()
                    || pipeline.kind() != Kind::Direct
                    || pipeline.adapter() != queue.adapter
                    || pipeline.root_identity() != root.identity()
                {
                    return Err(INVALID);
                }
                let target = if list.direct_mode == DirectMode::Draw {
                    let target = list.render_target.as_ref().ok_or(INVALID)?;

                    if !list.dispatched
                        || target.adapter() != queue.adapter
                        || !pipeline.matches_rtv(target)
                        || !pipeline.matches_dsv(list.depth_target.as_ref())
                    {
                        return Err(INVALID);
                    }
                    Some(target.owner())
                } else {
                    None
                };
                Submission::Direct {
                    pool: allocator.pool,
                    pipeline: pipeline.owner(),
                    target,
                    depth_target: list.depth_target.as_ref().map_or(0, |depth| depth.owner()),
                    state: list.direct_state,
                    draw_arguments: list.draw_arguments,
                    blend_constants: list.blend_constants,
                    indexed: indexed_work(list)?,
                }
            }
        };
        let adapter = queue.adapter;
        let origin = ListOrigin {
            index: li,
            storage: list.storage,
            cookie: list.cookie,
            generation: list.generation,
        };
        registry.lists[li].scheduled_owner = 0;
        registry.lists[li].operating = true;
        registry.lists[li].phase = Phase::Pending;
        let owners = take_owners(&mut registry.lists[li]);
        registry.queues[qi].operating = true;
        registry.queues[qi].pending = Some(PendingSubmission {
            origin,
            owners,
            accepted: false,
            value: 0,
        });
        Ok((qi, adapter, submission))
    });
    let (index, adapter, submission) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let mut result = TransferResult::default();
    let hydrated = hydrate_pending_tables(index);
    let status = if let Err(error) = hydrated {
        error
    } else {
        match submission {
            Submission::Empty { pool } => unsafe {
                ql_dx12_native_empty_submit(adapter, pool, &mut result)
            },
            Submission::ImageCopy {
                pool,
                source,
                destination,
            } => unsafe {
                ql_dx12_native_image_copy_submit(adapter, pool, source, destination, &mut result)
            },
            Submission::Resolve {
                pool,
                source,
                destination,
                source_layer,
                destination_layer,
            } => unsafe {
                ql_dx12_native_color_resolve_submit(
                    adapter,
                    pool,
                    source,
                    destination,
                    source_layer,
                    destination_layer,
                    &mut result,
                )
            },
            Submission::ImageTransition {
                pool,
                image_token,
                layer,
                readback,
                upload,
            } => unsafe {
                if let Some((buffer_token, footprint)) = upload {
                    ql_dx12_native_image_upload_submit(
                        adapter,
                        pool,
                        image_token,
                        layer,
                        buffer_token,
                        &footprint,
                        &mut result,
                    )
                } else if let Some((buffer_token, footprint)) = readback {
                    ql_dx12_native_image_readback_submit(
                        adapter,
                        pool,
                        image_token,
                        layer,
                        buffer_token,
                        &footprint,
                        &mut result,
                    )
                } else {
                    ql_dx12_native_image_transition_submit(
                        adapter,
                        pool,
                        image_token,
                        layer,
                        &mut result,
                    )
                }
            },
            Submission::RenderTarget {
                pool,
                binding,
                clear,
                work,
            } => unsafe {
                ql_dx12_native_render_target_submit(
                    adapter,
                    pool,
                    binding,
                    clear,
                    &work,
                    &mut result,
                )
            },
            Submission::Copy {
                pool,
                count,
                commands,
            } => unsafe {
                if count == 1 {
                    ql_dx12_native_transfer_submit(adapter, pool, commands.as_ptr(), &mut result)
                } else {
                    ql_dx12_native_transfer_submit_many(
                        adapter,
                        pool,
                        count,
                        commands.as_ptr(),
                        &mut result,
                    )
                }
            },
            Submission::Compute {
                pool,
                count,
                pipelines,
                arguments,
                constants,
                indirect,
            } => unsafe {
                ql_dx12_native_compute_submit(
                    adapter,
                    pool,
                    count,
                    pipelines.as_ptr(),
                    arguments.as_ptr(),
                    constants.as_ptr(),
                    indirect.as_ptr(),
                    &mut result,
                )
            },
            Submission::Graphics { pool, head, count } => unsafe {
                ql_dx12_native_graphics_submit(adapter, pool, head, count, &mut result)
            },
            Submission::Direct {
                pool,
                pipeline,
                target,
                depth_target,
                state,
                draw_arguments,
                blend_constants,
                indexed,
            } => unsafe {
                if let Some(indexed) = indexed {
                    ql_dx12_native_draw_indexed_submit(
                        adapter,
                        pool,
                        pipeline,
                        target.unwrap_or(0),
                        depth_target,
                        &state,
                        &indexed,
                        &blend_constants,
                        &mut result,
                    )
                } else if let Some(target) = target {
                    ql_dx12_native_draw_submit(
                        adapter,
                        pool,
                        pipeline,
                        target,
                        depth_target,
                        &state,
                        &draw_arguments,
                        &blend_constants,
                        &mut result,
                    )
                } else {
                    ql_dx12_native_direct_submit(adapter, pool, pipeline, &state, &mut result)
                }
            },
        }
    };
    let status = if status >= 0 && (result.attempted != 1 || result.accepted != 1) {
        FAIL
    } else {
        status
    };
    clear_finished_tables(index, result, status, true);
    let released = with_registry(|registry| {
        let released = observe(registry, index, result, status, true);
        if scheduled.is_some() && result.attempted == 0 {
            if let Some(li) = registry.list(list, list_cookie) {
                registry.lists[li].scheduled_owner = queue_cookie;
                registry.lists[li].operating = true;
            }
        }
        released
    });
    if let Some(owners) = released {
        release_owners(owners);
    }
    if valid_result(result) { status } else { FAIL }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_copy_poll(
    queue: usize,
    cookie: u64,
    output: *mut Completion,
    drain: u8,
) -> i32 {
    if output.is_null() || drain > 1 {
        return INVALID;
    }
    let owned = with_registry(|registry| {
        let index = registry.queue(queue, cookie).ok_or(INVALID)?;
        if registry.queues[index].operating {
            return Err(BUSY);
        }
        registry.queues[index].operating = true;
        Ok((index, registry.queues[index].adapter))
    });
    let (index, adapter) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let mut result = TransferResult::default();
    let status = if drain != 0 {
        unsafe { ql_dx12_native_transfer_drain(adapter, &mut result) }
    } else {
        unsafe { ql_dx12_native_transfer_poll(adapter, &mut result) }
    };
    clear_finished_tables(index, result, status, false);
    let report = with_registry(|registry| {
        let released = observe(registry, index, result, status, false);
        let queue = &registry.queues[index];
        let report = (|| {
            if !valid_result(result) {
                return Err(FAIL);
            }
            if queue.fault < 0 {
                return Err(queue.fault);
            }
            if status < 0 {
                return Err(status);
            }
            Ok(Completion {
                version: COMPLETION_VERSION,
                bytes: core::mem::size_of::<Completion>() as u32,
                state: if queue.pending.is_some() {
                    PENDING
                } else if queue.has_submission {
                    COMPLETE
                } else {
                    IDLE
                },
                reserved: 0,
                submitted: result.submitted,
                completed: result.completed,
            })
        })();
        (report, released)
    });
    let (report, released) = report;
    if let Some(owners) = released {
        release_owners(owners);
    }

    match report {
        Ok(report) => {
            unsafe { output.write(report) };
            0
        }
        Err(status) => status,
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_copy_list_retire(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> i32 {
    let owned = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let slot = &mut registry.lists[index];
        if slot.device != device || slot.device_cookie != device_cookie {
            return Err(INVALID);
        }
        if slot.operating || slot.phase == Phase::Pending {
            return Err(BUSY);
        }
        let list = core::mem::replace(
            slot,
            List {
                state: State::Retiring,
                ..List::vacant()
            },
        );
        Ok((index, list))
    });
    let (index, mut list) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    if let Some(recording) = list.recording.take() {
        native_objects::end_allocator_recording(recording);
    }
    release_owners(take_owners(&mut list));
    release_claim(list.claim);
    with_registry(|registry| registry.lists[index] = List::vacant());
    0
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_copy_queue_retire(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> i32 {
    let acquire = |try_drain| {
        with_registry(|registry| {
            let index = registry
                .queue_for_retirement(storage, cookie)
                .ok_or(INVALID)?;
            let queue = &mut registry.queues[index];

            if queue.device != device || queue.device_cookie != device_cookie {
                return Err(INVALID);
            }
            if queue.operating || queue.presentation_references != 0 {
                return Err(BUSY);
            }
            if queue.pending.is_some() {
                let status = unsafe { ql_dx12_native_parent_status_locked(queue.adapter) };

                if try_drain && status == native_objects::DEVICE_REMOVED {
                    return Ok(None);
                }
                return Err(BUSY);
            }
            queue.operating = true;
            Ok(Some(queue.state == State::Live && queue.managed_fences))
        })
    };
    let managed = match acquire(true) {
        Ok(None) => {
            let mut completion = Completion::default();

            // A failed or incomplete drain leaves the exact pending owner
            // attached. The next admission observes that pin and returns BUSY.
            let _ = unsafe { ql_dx12_native_copy_poll(storage, cookie, &mut completion, 1) };
            acquire(false)
        }
        other => other,
    };
    match managed {
        Err(status) => return status,
        Ok(Some(true)) => {
            let status = unsafe { ql_dx12_fence_queue_detach(storage, cookie) };
            if status < 0 {
                end_queue_operation(storage, cookie, 0);
                return status;
            }
        }
        Ok(Some(false)) => (),
        Ok(None) => unreachable!("no second drain attempt"),
    }
    let owned = with_registry(|registry| {
        let index = registry
            .queue_for_retirement(storage, cookie)
            .ok_or(INVALID)?;
        let slot = &mut registry.queues[index];
        if slot.device != device || slot.device_cookie != device_cookie {
            return Err(INVALID);
        }
        assert!(slot.operating && slot.pending.is_none() && slot.presentation_references == 0);
        slot.state = State::Retiring;
        Ok((index, slot.claim, slot.wddm_owner))
    });
    let (index, claim, wddm_owner) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let status = unsafe { ql_dx12_wddm_queue_close(wddm_owner) };
    if status < 0 {
        with_registry(|registry| registry.queues[index].operating = false);
        return status;
    }
    release_claim(claim);
    with_registry(|registry| registry.queues[index] = Queue::vacant());
    0
}

/// Once a queue context is published, acquire its paging queue explicitly.
/// A runtime callback can reenter, so only retain scalar ownership under the
/// policy lock and keep the native queue busy until actual callback completion.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_prepare_paging(
    storage: usize,
    cookie: u64,
    output: *mut core::ffi::c_void,
) -> i32 {
    let owner = with_registry(|registry| {
        let index = registry.queue(storage, cookie).ok_or(INVALID)?;
        let queue = &mut registry.queues[index];
        if queue.operating {
            return Err(BUSY);
        }
        if queue.wddm_owner == 0 {
            return Err(NO_INTERFACE);
        }
        let status = unsafe { ql_dx12_native_parent_status_locked(queue.adapter) };
        if status < 0 {
            return Err(status);
        }
        queue.operating = true;
        Ok(queue.wddm_owner)
    });
    let owner = match owner {
        Ok(owner) => owner,
        Err(status) => return status,
    };
    let status = unsafe { ql_dx12_wddm_paging_prepare(owner, output) };
    with_registry(|registry| {
        let index = registry
            .queue(storage, cookie)
            .expect("paging operation owns the queue");
        let queue = &mut registry.queues[index];
        assert!(queue.operating);
        queue.operating = false;
        if status < 0 {
            status
        } else {
            unsafe { ql_dx12_native_parent_status_locked(queue.adapter) }
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_copy_list_size(
    device: usize,
    device_cookie: u64,
    allocator: usize,
    allocator_cookie: u64,
) -> usize {
    match native_objects::retain_copy_allocator(device, device_cookie, allocator, allocator_cookie)
    {
        Ok(lease) => {
            native_objects::release_command_allocator(lease);
            native_objects::STORAGE_BYTES
        }
        Err(_) => 0,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_copy_list_create(
    device: usize,
    cookie: u64,
    storage: usize,
    allocator: usize,
    allocator_cookie: u64,
    output: *mut u64,
) -> i32 {
    unsafe {
        create_list(
            device,
            cookie,
            storage,
            allocator,
            allocator_cookie,
            Kind::Copy,
            DirectMode::State,
            output,
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_compute_list_create(
    device: usize,
    cookie: u64,
    storage: usize,
    allocator: usize,
    allocator_cookie: u64,
    output: *mut u64,
) -> i32 {
    unsafe {
        create_list(
            device,
            cookie,
            storage,
            allocator,
            allocator_cookie,
            Kind::Compute,
            DirectMode::State,
            output,
        )
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_compute_list_size(
    device: usize,
    cookie: u64,
    allocator: usize,
    allocator_cookie: u64,
) -> usize {
    match native_objects::retain_compute_allocator(device, cookie, allocator, allocator_cookie) {
        Ok(lease) => {
            native_objects::release_command_allocator(lease);
            native_objects::STORAGE_BYTES
        }
        Err(_) => 0,
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_direct_list_create(
    device: usize,
    cookie: u64,
    storage: usize,
    allocator: usize,
    allocator_cookie: u64,
    output: *mut u64,
) -> i32 {
    unsafe {
        create_list(
            device,
            cookie,
            storage,
            allocator,
            allocator_cookie,
            Kind::Direct,
            DirectMode::State,
            output,
        )
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_direct_list_size(
    device: usize,
    cookie: u64,
    allocator: usize,
    allocator_cookie: u64,
) -> usize {
    match native_objects::retain_direct_allocator(device, cookie, allocator, allocator_cookie) {
        Ok(lease) => {
            native_objects::release_command_allocator(lease);
            native_objects::STORAGE_BYTES
        }
        Err(_) => 0,
    }
}
fn binding_begin(storage: usize, cookie: u64, kind: Kind) -> Result<(usize, usize, u64), i32> {
    with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        if list.kind != kind
            || list.phase != Phase::Recording
            || (list.dispatched
                && !(list.runtime_error != 0
                    && (kind == Kind::Compute
                        || (kind == Kind::Direct && list.direct_mode == DirectMode::Draw))))
            || (kind == Kind::Direct
                && !matches!(list.direct_mode, DirectMode::State | DirectMode::Draw))
        {
            if !matches!(list.phase, Phase::Pending | Phase::Complete) {
                list.phase = Phase::Invalid;
            }
            return Err(INVALID);
        }
        list.operating = true;
        Ok((index, list.device, list.device_cookie))
    })
}
fn set_pipeline(
    storage: usize,
    cookie: u64,
    pipeline: usize,
    pipeline_cookie: u64,
    kind: Kind,
) -> i32 {
    let (index, device, device_cookie) = match binding_begin(storage, cookie, kind) {
        Ok(owner) => owner,
        Err(status) => return status,
    };
    let retained = match kind {
        Kind::Compute => native_pipelines::retain_compute_pipeline(
            device,
            device_cookie,
            pipeline,
            pipeline_cookie,
        )
        .map(PipelineBinding::Compute),
        Kind::Direct => native_pipelines::retain_graphics_pipeline(
            device,
            device_cookie,
            pipeline,
            pipeline_cookie,
        )
        .map(PipelineBinding::Graphics),
        Kind::Copy => Err(INVALID),
    };
    let mut candidate = match retained {
        Ok(lease) => Some(lease),
        Err(status) => {
            binding_failed(index);
            return status;
        }
    };
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        // Admission holds the common policy lock; the operating list cannot
        // retire/reset while acquiring this exact typed owner outside the lock.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        let previous = list
            .pipeline
            .replace(candidate.take().expect("candidate owns pipeline"));
        list.operating = false;
        Ok(previous)
    });
    match committed {
        Ok(previous) => {
            if let Some(previous) = previous {
                previous.release();
            }
            0
        }
        Err(status) => {
            binding_failed(index);
            candidate
                .expect("failed admission retains candidate")
                .release();
            status
        }
    }
}

fn binding_failed(index: usize) {
    with_registry(|registry| {
        registry.lists[index].operating = false;
        registry.lists[index].phase = Phase::Invalid;
    });
}

/// Genuine ClearRootArguments zeroes all root argument storage regardless of
/// the currently selected root or command family. Consumed command snapshots
/// own independent references and are intentionally untouched. Bound heaps,
/// root signature, pipeline and non-root dynamic state remain selected.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_clear_root_arguments(storage: usize, cookie: u64) -> i32 {
    let prepared = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        if list.phase != Phase::Recording || list.runtime_error == 0 {
            if !matches!(list.phase, Phase::Pending | Phase::Complete) {
                list.phase = Phase::Invalid;
            }
            return Err(INVALID);
        }
        list.operating = true;
        Ok((index, list.device, list.device_cookie))
    });
    let (index, device, device_cookie) = match prepared {
        Ok(value) => value,
        Err(status) => return status,
    };
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        list.root_values = [0; 64];
        list.table_handles = [0; 64];
        let removed = core::mem::take(&mut list.cbv_bindings);
        list.operating = false;
        Ok(removed)
    });
    match committed {
        Ok(removed) => {
            release_cbv_bindings(removed);
            0
        }
        Err(status) => {
            binding_failed(index);
            status
        }
    }
}

fn set_root(storage: usize, cookie: u64, root: usize, root_cookie: u64, kind: Kind) -> i32 {
    let (index, device, device_cookie) = match binding_begin(storage, cookie, kind) {
        Ok(owner) => owner,
        Err(status) => return status,
    };
    let mut candidate =
        match native_shaders::retain_empty_root_binding(device, device_cookie, root, root_cookie) {
            Ok(lease) => Some(lease),
            Err(status) => {
                binding_failed(index);
                return status;
            }
        };
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        // Only a pure state swap runs while native admission owns the lock.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        let next = candidate.take().expect("candidate owns root");
        let changed = list
            .root
            .as_ref()
            .is_none_or(|previous| previous.identity() != next.identity());
        let removed = if changed {
            core::mem::take(&mut list.cbv_bindings)
        } else {
            CbvBindings::new()
        };
        if changed {
            // D3D leaves a changed root's arguments undefined. Select zero as
            // initial internal storage; rebinding the same root preserves bits.
            list.root_values = [0; 64];
            list.table_handles = [0; 64];
        }
        let previous = list.root.replace(next);
        list.operating = false;
        Ok((previous, removed))
    });
    match committed {
        Ok((previous, removed)) => {
            release_cbv_bindings(removed);
            if let Some(previous) = previous {
                native_shaders::release_empty_root_binding(previous);
            }
            0
        }
        Err(status) => {
            binding_failed(index);
            native_shaders::release_empty_root_binding(
                candidate.expect("failed admission retains root"),
            );
            status
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_graphics_root_cbv(
    storage: usize,
    cookie: u64,
    parameter: u32,
    address: u64,
) -> i32 {
    set_root_buffer(storage, cookie, Kind::Direct, parameter, address, 2)
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_compute_root_cbv(
    storage: usize,
    cookie: u64,
    parameter: u32,
    address: u64,
) -> i32 {
    set_root_buffer(storage, cookie, Kind::Compute, parameter, address, 2)
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_graphics_root_srv(
    storage: usize,
    cookie: u64,
    parameter: u32,
    address: u64,
) -> i32 {
    set_root_buffer(storage, cookie, Kind::Direct, parameter, address, 3)
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_compute_root_srv(
    storage: usize,
    cookie: u64,
    parameter: u32,
    address: u64,
) -> i32 {
    set_root_buffer(storage, cookie, Kind::Compute, parameter, address, 3)
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_graphics_root_uav(
    storage: usize,
    cookie: u64,
    parameter: u32,
    address: u64,
) -> i32 {
    set_root_buffer(storage, cookie, Kind::Direct, parameter, address, 4)
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_compute_root_uav(
    storage: usize,
    cookie: u64,
    parameter: u32,
    address: u64,
) -> i32 {
    set_root_buffer(storage, cookie, Kind::Compute, parameter, address, 4)
}
fn set_root_buffer(
    storage: usize,
    cookie: u64,
    kind: Kind,
    parameter: u32,
    address: u64,
    buffer_kind: u32,
) -> i32 {
    if address % (if buffer_kind == 2 { 256 } else { 4 }) != 0 {
        return INVALID;
    }
    let (index, device, device_cookie) = match binding_begin(storage, cookie, kind) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let selected = with_registry(|r| {
        let list = &r.lists[index];
        if list.runtime_error == 0 || (kind == Kind::Direct && list.direct_mode != DirectMode::Draw)
        {
            return Err(NO_INTERFACE);
        }
        let plan = list.root.as_ref().ok_or(INVALID)?.plan();
        if parameter >= plan.count || plan.parameters[parameter as usize].kind != buffer_kind {
            return Err(INVALID);
        }
        Ok(plan.parameters[parameter as usize].offset)
    });
    let offset = match selected {
        Ok(v) => v,
        Err(e) => {
            binding_failed(index);
            return e;
        }
    };
    let candidate = if address == 0 {
        None
    } else {
        match retain_cbv_binding(
            device,
            device_cookie,
            parameter,
            offset,
            buffer_kind,
            address,
            0,
        ) {
            Ok(node) => Some(node),
            Err(e) => {
                binding_failed(index);
                return e;
            }
        }
    };
    let mut candidate = candidate;
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        let old = core::mem::take(&mut list.cbv_bindings);
        let mut next = old.head;
        let mut removed = CbvBindings::new();
        for _ in 0..old.count {
            let node = next;
            let entry = unsafe { &*(node as *const CbvBindingNode) };
            next = entry.next;
            if entry.work.parameter == parameter {
                removed.append(node);
            } else {
                list.cbv_bindings.append(node);
            }
        }
        assert!(next == 0);
        if let Some(node) = candidate.take() {
            list.cbv_bindings.append(node);
        }
        list.operating = false;
        Ok(removed)
    });
    match committed {
        Ok(old) => {
            release_cbv_bindings(old);
            0
        }
        Err(e) => {
            if let Some(node) = candidate {
                let mut c = CbvBindings::new();
                c.append(node);
                release_cbv_bindings(c);
            }
            binding_failed(index);
            e
        }
    }
}

/// Genuine graphics root constants snapshot caller bytes while Operating pins
/// the selected current root. Only pure state publication holds native admission.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_graphics_root_constants(
    storage: usize,
    cookie: u64,
    parameter: u32,
    count: u32,
    values: *const u32,
    offset: u32,
) -> i32 {
    unsafe {
        set_root_constants(
            storage,
            cookie,
            Kind::Direct,
            parameter,
            count,
            values,
            offset,
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_compute_root_constants(
    storage: usize,
    cookie: u64,
    parameter: u32,
    count: u32,
    values: *const u32,
    offset: u32,
) -> i32 {
    unsafe {
        set_root_constants(
            storage,
            cookie,
            Kind::Compute,
            parameter,
            count,
            values,
            offset,
        )
    }
}
unsafe fn set_root_constants(
    storage: usize,
    cookie: u64,
    kind: Kind,
    parameter: u32,
    count: u32,
    values: *const u32,
    offset: u32,
) -> i32 {
    if count > 64 || (count != 0 && values.is_null()) {
        return INVALID;
    }
    let (index, device, device_cookie) = match binding_begin(storage, cookie, kind) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let selected = with_registry(|registry| {
        let list = &registry.lists[index];
        if list.runtime_error == 0 || (kind == Kind::Direct && list.direct_mode != DirectMode::Draw)
        {
            return Err(NO_INTERFACE);
        }
        let root = list.root.as_ref().ok_or(INVALID)?;
        let plan = root.plan();
        if parameter >= plan.count {
            return Err(INVALID);
        }
        let entry = plan.parameters[parameter as usize];
        if entry.kind != 1 {
            return Err(INVALID);
        }
        if offset > entry.words || count > entry.words - offset {
            return Err(INVALID);
        }
        Ok((entry.offset + offset) as usize)
    });
    let destination = match selected {
        Ok(v) => v,
        Err(e) => {
            binding_failed(index);
            return e;
        }
    };
    let mut copied = [0; 64];
    for i in 0..count as usize {
        copied[i] = unsafe { values.add(i).read_unaligned() };
    }
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        list.root_values[destination..destination + count as usize]
            .copy_from_slice(&copied[..count as usize]);
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(e) => {
            binding_failed(index);
            e
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_compute_set_pipeline(
    storage: usize,
    cookie: u64,
    pipeline: usize,
    pipeline_cookie: u64,
) -> i32 {
    set_pipeline(storage, cookie, pipeline, pipeline_cookie, Kind::Compute)
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_compute_set_root(
    storage: usize,
    cookie: u64,
    root: usize,
    root_cookie: u64,
) -> i32 {
    set_root(storage, cookie, root, root_cookie, Kind::Compute)
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_direct_set_pipeline(
    storage: usize,
    cookie: u64,
    pipeline: usize,
    pipeline_cookie: u64,
) -> i32 {
    set_pipeline(storage, cookie, pipeline, pipeline_cookie, Kind::Direct)
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_direct_set_root(
    storage: usize,
    cookie: u64,
    root: usize,
    root_cookie: u64,
) -> i32 {
    set_root(storage, cookie, root, root_cookie, Kind::Direct)
}

fn direct_state_commit(
    storage: usize,
    cookie: u64,
    commit: impl FnOnce(&mut List) -> Result<(), i32>,
) -> i32 {
    let (index, device, device_cookie) = match binding_begin(storage, cookie, Kind::Direct) {
        Ok(owner) => owner,
        Err(status) => return status,
    };
    let adapter = with_registry(|registry| {
        registry.lists[index]
            .allocator
            .as_ref()
            .expect("recording list retains allocator")
            .adapter
    });
    let status = unsafe { ql_dx12_native_parent_status(adapter) };
    if status < 0 {
        binding_failed(index);
        return status;
    }
    let result = native_objects::with_device_admission(device, device_cookie, || {
        // The operating owner pins list identity while this pure native-fault
        // admitted state update owns the one common policy lock.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        commit(list)?;
        list.operating = false;
        Ok(())
    });
    match result {
        Ok(()) => 0,
        Err(status) => {
            binding_failed(index);
            status
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_direct_set_topology(
    storage: usize,
    cookie: u64,
    topology: u32,
) -> i32 {
    direct_state_commit(storage, cookie, |list| {
        if native_pipelines::topology_class(topology).is_none()
            || (list.runtime_error == 0 && topology != DIRECT_TOPOLOGY_TRIANGLELIST)
        {
            return Err(INVALID);
        }
        list.direct_topology = topology;
        list.direct_flags |= DIRECT_TOPOLOGY_SET;
        Ok(())
    })
}

/// C passes one immutable copied official viewport normalized into this local
/// scalar snapshot. Only viewport members are consumed by this setter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_direct_set_viewport(
    storage: usize,
    cookie: u64,
    state: *const DirectState,
) -> i32 {
    if state.is_null() {
        return INVALID;
    }
    let state = unsafe { state.read() };
    direct_state_commit(storage, cookie, |list| {
        if !state.viewport_valid() {
            return Err(INVALID);
        }
        list.direct_state.viewport_x = state.viewport_x;
        list.direct_state.viewport_y = state.viewport_y;
        list.direct_state.viewport_width = state.viewport_width;
        list.direct_state.viewport_height = state.viewport_height;
        list.direct_state.min_depth = state.min_depth;
        list.direct_state.max_depth = state.max_depth;
        list.direct_flags |= DIRECT_VIEWPORT_SET;
        Ok(())
    })
}

/// Only scissor members of the immutable C snapshot are consumed here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_direct_set_scissor(
    storage: usize,
    cookie: u64,
    state: *const DirectState,
) -> i32 {
    if state.is_null() {
        return INVALID;
    }
    let state = unsafe { state.read() };
    direct_state_commit(storage, cookie, |list| {
        if !state.scissor_valid() {
            return Err(INVALID);
        }
        list.direct_state.scissor_left = state.scissor_left;
        list.direct_state.scissor_top = state.scissor_top;
        list.direct_state.scissor_right = state.scissor_right;
        list.direct_state.scissor_bottom = state.scissor_bottom;
        list.direct_flags |= DIRECT_SCISSOR_SET;
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_compute_dispatch(
    storage: usize,
    cookie: u64,
    x: u32,
    y: u32,
    z: u32,
) -> i32 {
    record_compute_dispatch(storage, cookie, x, y, z, None)
}
fn record_compute_dispatch(
    storage: usize,
    cookie: u64,
    x: u32,
    y: u32,
    z: u32,
    indirect: Option<IndirectRequest>,
) -> i32 {
    let (index, device, device_cookie) = match binding_begin(storage, cookie, Kind::Compute) {
        Ok(owner) => owner,
        Err(status) => return status,
    };
    let arguments = DispatchArguments { x, y, z };
    let snapshot = with_registry(|registry| {
        let list = &registry.lists[index];
        if list.graphics_stream.count != 0 {
            return Err(NO_INTERFACE);
        }
        if indirect.is_some() && list.runtime_error == 0 {
            return Err(INVALID);
        }
        let pipeline = match list.pipeline.as_ref() {
            Some(PipelineBinding::Compute(p)) => p,
            _ => return Err(INVALID),
        };
        let root = list.root.as_ref().ok_or(INVALID)?;
        if pipeline
            .resource_bytes()
            .iter()
            .enumerate()
            .any(|(i, bytes)| *bytes != 0 && root.plan().parameters[i].kind == 4)
            && (indirect.is_some_and(|r| r.count != 0)
                || (indirect.is_none() && [x, y, z] != [1, 1, 1]))
        {
            return Err(NO_INTERFACE);
        }
        if !arguments.supported()
            || !(if list.runtime_error != 0 {
                unsafe { root.matches_pipeline_locked(pipeline.root_identity()) }
            } else {
                pipeline.root_identity() == root.identity()
            })
            || (list.runtime_error == 0 && (root.plan().words != 0 || x != 1 || y != 1 || z != 1))
        {
            return Err(INVALID);
        }
        if list.runtime_error != 0 && list.compute_stream.iter().all(Option::is_some) {
            return Err(NO_MEMORY);
        }
        Ok((
            list.runtime_error != 0,
            pipeline.adapter(),
            pipeline.caller_identity(),
            root.caller_identity(),
            RootConstants {
                words: root.plan().words,
                values: list.root_values,
                ..RootConstants::default()
            },
            snapshot_cbvs(list, pipeline.resource_bytes())?,
            indirect.and_then(|r| buffer_state_for(list, r.buffer, r.buffer_cookie)),
        ))
    });
    let (genuine, adapter, pipeline_id, root_id, mut constants, cbv_snapshots, indirect_state) =
        match snapshot {
            Ok(snapshot) => snapshot,
            Err(status) => {
                binding_failed(index);
                return status;
            }
        };
    let parent = unsafe { ql_dx12_native_parent_status(adapter) };
    if parent < 0 {
        binding_failed(index);
        return parent;
    }
    // Operating pins the selected current bindings while independent unique
    // references are acquired outside the common admission lock. A failed
    // acquisition/commit releases only these candidates, never old commands.
    let mut candidate = if genuine {
        let pipeline = match native_pipelines::retain_compute_pipeline(
            device,
            device_cookie,
            pipeline_id.0,
            pipeline_id.1,
        ) {
            Ok(p) => p,
            Err(status) => {
                binding_failed(index);
                return status;
            }
        };
        let root = match native_shaders::retain_empty_root_binding(
            device,
            device_cookie,
            root_id.0,
            root_id.1,
        ) {
            Ok(r) => r,
            Err(status) => {
                native_pipelines::release_compute_pipeline(pipeline);
                binding_failed(index);
                return status;
            }
        };
        let cbvs = match retain_cbv_snapshots(device, device_cookie, cbv_snapshots) {
            Ok(c) => c,
            Err(e) => {
                native_shaders::release_empty_root_binding(root);
                native_pipelines::release_compute_pipeline(pipeline);
                binding_failed(index);
                return e;
            }
        };
        constants.cbv_head = cbvs.head as u64;
        constants.cbv_count = cbvs.count;
        Some(ComputeRecording {
            pipeline,
            root,
            arguments,
            constants,
            cbvs,
            indirect: None,
            indirect_work: IndirectWork::default(),
        })
    } else {
        None
    };
    if let Some(request) = indirect {
        let retained = (|| -> Result<(IndirectRecording, IndirectWork), i32> {
            if request.count_buffer != 0 || request.count_cookie != 0 {
                return Err(NO_INTERFACE);
            }
            let signature = crate::native_signatures::retain_signature(
                device,
                device_cookie,
                request.signature,
                request.signature_cookie,
            )?;
            if signature.kind != 2 {
                crate::native_signatures::release_signature(signature);
                return Err(NO_INTERFACE);
            }
            let arguments = match u64::from(request.count)
                .checked_mul(u64::from(signature.stride))
                .ok_or(INVALID)
                .and_then(|bytes| {
                    native_resources::retain_indirect_read(
                        device,
                        device_cookie,
                        request.buffer,
                        request.buffer_cookie,
                        request.offset,
                        bytes,
                        indirect_state,
                    )
                }) {
                Ok(v) => v,
                Err(e) => {
                    crate::native_signatures::release_signature(signature);
                    return Err(e);
                }
            };
            if arguments.adapter != signature.adapter {
                native_resources::release_indirect_read(arguments);
                crate::native_signatures::release_signature(signature);
                return Err(INVALID);
            }
            let work = IndirectWork {
                buffer_token: arguments.token,
                offset: arguments.offset,
                bytes: arguments.bytes,
                count: request.count,
                stride: signature.stride,
                ..IndirectWork::default()
            };
            Ok((
                IndirectRecording {
                    signature,
                    arguments,
                },
                work,
            ))
        })();
        match retained {
            Ok((owners, work)) => {
                let command = candidate
                    .as_mut()
                    .expect("genuine indirect owns compute snapshot");
                command.indirect = Some(owners);
                command.indirect_work = work;
            }
            Err(status) => {
                if let Some(command) = candidate {
                    release_compute_recording(command);
                }
                binding_failed(index);
                return status;
            }
        }
    }
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        assert!(list.cookie == cookie && list.operating && list.phase == Phase::Recording);
        if genuine {
            let next = list
                .compute_stream
                .iter_mut()
                .find(|slot| slot.is_none())
                .ok_or(NO_MEMORY)?;
            *next = candidate.take();
        }
        list.dispatch_arguments = arguments;
        list.dispatched = true;
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(status) => {
            if let Some(command) = candidate {
                release_compute_recording(command);
            }
            binding_failed(index);
            status
        }
    }
}

/// Start a fresh recording without resetting its Vulkan allocator pool. Closed
/// and pending recordings retain independent lifetime leases. Candidate ownership
/// is acquired before the final identity/epoch check, so failure leaves the old
/// recording unchanged even if a concurrent observation completes its GPU work.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_command_list_reset(
    storage: usize,
    cookie: u64,
    allocator: usize,
    allocator_cookie: u64,
    slot: u32,
    id: u64,
) -> i32 {
    if slot != RESET_SLOT {
        return INVALID;
    }
    let snapshot = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &registry.lists[index];

        if list.operating {
            return Err(BUSY);
        }
        if !matches!(list.phase, Phase::Closed | Phase::Pending | Phase::Complete) {
            return Err(INVALID);
        }
        Ok((
            index,
            list.generation,
            list.kind,
            list.device,
            list.device_cookie,
        ))
    });
    let (index, generation, kind, device, device_cookie) = match snapshot {
        Ok(snapshot) => snapshot,
        Err(status) => return status,
    };
    let retained = match kind {
        Kind::Copy => native_objects::retain_copy_allocator(
            device,
            device_cookie,
            allocator,
            allocator_cookie,
        ),
        Kind::Compute => native_objects::retain_compute_allocator(
            device,
            device_cookie,
            allocator,
            allocator_cookie,
        ),
        Kind::Direct => native_objects::retain_direct_allocator(
            device,
            device_cookie,
            allocator,
            allocator_cookie,
        ),
    };
    let candidate = match retained {
        Ok(candidate) => candidate,
        Err(status) => return status,
    };
    let association = match native_objects::begin_allocator_recording(&candidate) {
        Ok(association) => association,
        Err(status) => {
            native_objects::release_command_allocator(candidate);
            return status;
        }
    };
    let status = unsafe { ql_dx12_native_parent_status(candidate.adapter) };

    if status < 0 {
        native_objects::end_allocator_recording(association);
        native_objects::release_command_allocator(candidate);
        return status;
    }
    let mut candidate = Some(candidate);
    let mut association = Some(association);
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        // with_device_admission owns the same policy lock used by this registry.
        // Only this pure commit accesses it directly; no nested lock or callback.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let current = registry.list(storage, cookie).ok_or(INVALID)?;

        if current != index || registry.lists[index].generation != generation {
            return Err(BUSY);
        }
        let list = &registry.lists[index];

        if list.operating {
            return Err(BUSY);
        }
        if !matches!(list.phase, Phase::Closed | Phase::Pending | Phase::Complete) {
            return Err(INVALID);
        }
        let next_generation = registry.next_generation;
        registry.next_generation = next_generation.checked_add(1).ok_or(NO_MEMORY)?;
        let list = &mut registry.lists[index];
        assert!(
            list.recording.is_none(),
            "closed or pending list ended recording association"
        );
        let previous = take_owners(list);
        // Clear/OM-only promotion belongs to the previous recording. A unified
        // R0 reset must again admit drawing, or a fresh clear, on this list.
        if list.kind == Kind::Direct && list.runtime_error != 0 {
            list.direct_mode = DirectMode::Draw;
        }
        list.allocator = candidate.take();
        list.recording = association.take();
        list.phase = Phase::Recording;
        list.generation = next_generation;
        list.id = id;
        Ok(previous)
    });
    match committed {
        Ok(previous) => {
            release_owners(previous);
            0
        }
        Err(status) => {
            native_objects::end_allocator_recording(
                association.expect("failed reset retains candidate association"),
            );
            native_objects::release_command_allocator(
                candidate.expect("failed reset retains candidate lifetime"),
            );
            status
        }
    }
}

/// Metadata only: ID never selects an object or derives generation. Output is
/// published only after exact identity, operation exclusion and length checks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_command_recording(
    storage: usize,
    cookie: u64,
    bytes: u32,
    output: *mut RecordingInfo,
) -> i32 {
    if output.is_null() || bytes as usize != core::mem::size_of::<RecordingInfo>() {
        return INVALID;
    }
    let report = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &registry.lists[index];

        if list.operating {
            return Err(BUSY);
        }
        Ok(RecordingInfo {
            version: RECORDING_INFO_VERSION,
            bytes: core::mem::size_of::<RecordingInfo>() as u32,
            recording: u32::from(list.recording.is_some()),
            reserved: 0,
            generation: list.generation,
            id: list.id,
        })
    });
    match report {
        Ok(report) => {
            unsafe { output.write(report) };
            0
        }
        Err(status) => status,
    }
}

fn render_target_work(list: &List) -> Option<RenderTargetWork> {
    let color = list
        .clear
        .as_ref()
        .map_or(ClearColor::default(), |clear| clear.color);
    let work = RenderTargetWork {
        flags: if list.render_target.is_some() {
            RENDER_TARGET_BIND
        } else {
            0
        } | if let Some(clear) = list.clear.as_ref() {
            RENDER_TARGET_CLEAR | clear.depth_stencil_flags
        } else {
            0
        },
        reserved: 0,
        clear_red: color.red,
        clear_green: color.green,
        clear_blue: color.blue,
        clear_alpha: color.alpha,
        rect_count: list.clear.as_ref().map_or(0, |clear| clear.regions.count),
        rectangles: list
            .clear
            .as_ref()
            .map_or([ClearRect::default(); CLEAR_RECT_LIMIT as usize], |clear| {
                clear.regions.rectangles
            }),
    };
    let work = RenderTargetWork {
        flags: work.flags
            | if list
                .clear
                .as_ref()
                .is_some_and(|clear| clear.regions.explicit)
            {
                RENDER_TARGET_CLEAR_RECTS
            } else {
                0
            },
        ..work
    };
    work.supported().then_some(work)
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_render_target_list_size(
    device: usize,
    cookie: u64,
    allocator: usize,
    allocator_cookie: u64,
) -> usize {
    ql_dx12_native_direct_list_size(device, cookie, allocator, allocator_cookie)
}

/// A separate genuine DIRECT recording mode supports one optional explicit OM
/// binding and one optional independent clear. No PSO, root or viewport is needed.
/// At least one operation must be recorded; clear never manufactures a binding.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_render_target_list_create(
    device: usize,
    cookie: u64,
    storage: usize,
    allocator: usize,
    allocator_cookie: u64,
    output: *mut u64,
) -> i32 {
    unsafe {
        create_list(
            device,
            cookie,
            storage,
            allocator,
            allocator_cookie,
            Kind::Direct,
            DirectMode::RenderTarget,
            output,
        )
    }
}

#[derive(Clone, Copy)]
enum TargetOperation {
    Bind,
    Clear,
}
fn render_target_begin(
    storage: usize,
    cookie: u64,
    operation: TargetOperation,
) -> Result<(usize, usize, u64), i32> {
    with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];

        if list.operating {
            return Err(BUSY);
        }
        if list.resolve.is_some() || list.resolve_transitions.iter().any(Option::is_some) {
            return Err(INVALID);
        }
        let duplicate = match operation {
            TargetOperation::Bind => {
                list.render_target.is_some()
                    && !(list.runtime_error != 0 && list.direct_mode == DirectMode::Draw)
            }
            TargetOperation::Clear => list.clear.is_some(),
        };

        if list.kind != Kind::Direct
            || !(list.direct_mode == DirectMode::RenderTarget
                || (list.direct_mode == DirectMode::Draw
                    && (matches!(operation, TargetOperation::Bind) || list.runtime_error != 0)
                    && (!list.dispatched
                        || (list.runtime_error != 0
                            && matches!(operation, TargetOperation::Bind)))))
            || list.phase != Phase::Recording
            || duplicate
        {
            if !matches!(list.phase, Phase::Pending | Phase::Complete) {
                list.phase = Phase::Invalid;
            }
            return Err(INVALID);
        }
        list.operating = true;
        Ok((index, list.device, list.device_cookie))
    })
}

fn record_render_target(
    storage: usize,
    cookie: u64,
    address: usize,
    clear: Option<(ClearColor, ClearRegions)>,
    depth_stencil_flags: u32,
) -> i32 {
    let operation = if clear.is_some() {
        TargetOperation::Clear
    } else {
        TargetOperation::Bind
    };
    let (index, device, device_cookie) = match render_target_begin(storage, cookie, operation) {
        Ok(owner) => owner,
        Err(status) => return status,
    };
    let target = match if depth_stencil_flags != 0 {
        native_descriptors::retain_dsv(device, device_cookie, address)
    } else {
        native_descriptors::retain_rtv(device, device_cookie, address)
    } {
        Ok(target) => target,
        Err(status) => {
            binding_failed(index);
            return status;
        }
    };
    if let Some((color, _)) = clear
        && depth_stencil_flags == 0
        && target.format() != native_images::IMAGE_FORMAT_RGBA16_FLOAT
        && !color.within(0.0..=1.0)
    {
        native_descriptors::release_rtv(target);
        binding_failed(index);
        return INVALID;
    }
    if depth_stencil_flags & RENDER_TARGET_STENCIL_CLEAR != 0
        && !native_images::supported_stencil_format(target.format())
    {
        native_descriptors::release_rtv(target);
        binding_failed(index);
        return INVALID;
    }
    if (depth_stencil_flags & RENDER_TARGET_DEPTH_CLEAR != 0
        && target.read_only_flags() & native_descriptors::READ_ONLY_DEPTH != 0)
        || (depth_stencil_flags & RENDER_TARGET_STENCIL_CLEAR != 0
            && target.read_only_flags() & native_descriptors::READ_ONLY_STENCIL != 0)
    {
        native_descriptors::release_rtv(target);
        binding_failed(index);
        return INVALID;
    }
    let status = unsafe { ql_dx12_native_parent_status(target.adapter()) };
    let mut candidate = Some(target);

    if status < 0 {
        native_descriptors::release_rtv(candidate.expect("failed selected admission retains RTV"));
        binding_failed(index);
        return status;
    }
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        // Operating pins exact list identity while the native fault check and
        // pure command-registry commit share the one common policy lock.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        let target = candidate.take().expect("unique candidate RTV");

        let mut previous = (None, None, [const { None }; 7]);
        if let Some((color, regions)) = clear {
            // This is the unified runtime's bounded clear path. Existing
            // private draw recordings remain draw-only. Bound unused state
            // retains its owners but does not create an invented draw.
            if list.direct_mode == DirectMode::Draw {
                assert!(list.runtime_error != 0 && !list.dispatched);
                list.direct_mode = DirectMode::RenderTarget;
            }
            let mut clipped = ClearRegions {
                explicit: regions.explicit,
                ..ClearRegions::default()
            };
            for rectangle in &regions.rectangles[..regions.count as usize] {
                if let Some(rectangle) = rectangle.clipped(target.extent()) {
                    clipped.rectangles[clipped.count as usize] = rectangle;
                    clipped.count += 1;
                }
            }
            assert!(list.clear.is_none(), "one explicit clear per recording");
            list.clear = Some(ClearRecording {
                target,
                depth_stencil_flags,
                color,
                regions: clipped,
            });
        } else {
            previous.0 = core::mem::replace(&mut list.render_target, Some(target));
            previous.1 = list.depth_target.take();
            previous.2 = core::mem::replace(&mut list.extra_render_targets, [const { None }; 7]);
        }
        list.operating = false;
        Ok(previous)
    });
    match committed {
        Ok((color, depth, extras)) => {
            release_extra_targets(extras);
            if let Some(d) = depth {
                native_descriptors::release_rtv(d);
            }
            if let Some(c) = color {
                native_descriptors::release_rtv(c);
            }
            0
        }
        Err(status) => {
            native_descriptors::release_rtv(
                candidate.expect("failed native admission retains RTV"),
            );
            binding_failed(index);
            status
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_render_target_bind(
    storage: usize,
    cookie: u64,
    address: usize,
) -> i32 {
    record_render_target(storage, cookie, address, None, 0)
}

/// C snapshots genuine scalar/rectangle input; Rust clips signed rectangles to
/// the supported view extent, dropping empty/inverted/outside rectangles. An
/// explicit list with no remaining rectangles stays a no-op, never a full clear.
/// This pointer is valid/aligned for the call only; no caller pointer survives
/// recording. The explicit clear target owns a reference independent of OM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_render_target_clear(
    storage: usize,
    cookie: u64,
    address: usize,
    color: *const ClearColor,
    rect_count: u32,
    rectangles: *const ClearRect,
) -> i32 {
    if color.is_null() || rect_count > CLEAR_RECT_LIMIT || (rect_count != 0 && rectangles.is_null())
    {
        return INVALID;
    }
    let color = unsafe { color.read() };

    if !color.within(HALF_FLOAT_FINITE_RANGE) {
        return INVALID;
    }
    let mut regions = ClearRegions {
        explicit: rect_count != 0,
        count: rect_count,
        ..ClearRegions::default()
    };
    for index in 0..rect_count as usize {
        regions.rectangles[index] = unsafe { rectangles.add(index).read() };
    }
    record_render_target(storage, cookie, address, Some((color, regions)), 0)
}

/// Genuine DIRECT transition prerequisite. Two explicit callbacks must form
/// RENDER_TARGET -> COPY_SOURCE/COPY_DEST -> RENDER_TARGET on one initialized
/// image, as selected by the genuine transition/readback/upload bundle.
/// The subset never persists an intermediate resource state across submissions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_transition_list_create(
    device: usize,
    cookie: u64,
    storage: usize,
    allocator: usize,
    allocator_cookie: u64,
    output: *mut u64,
) -> i32 {
    unsafe {
        create_list(
            device,
            cookie,
            storage,
            allocator,
            allocator_cookie,
            Kind::Direct,
            DirectMode::ImageTransition,
            output,
        )
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_image_transition_record(
    storage: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
    subresource: u32,
    before: u32,
    after: u32,
) -> i32 {
    let paired = with_registry(|registry| {
        registry.list(storage, cookie).is_some_and(|index| {
            let list = &registry.lists[index];
            list.direct_mode == DirectMode::ImageCopy
                || (list.runtime_error != 0
                    && list.image_transition.as_ref().is_some_and(|entry| {
                        !entry.closed
                            && entry.upload.is_none()
                            && entry.readback.is_none()
                            && entry.resource != resource
                            && before == native_images::IMAGE_STATE_RENDER_TARGET
                            && matches!(
                                after,
                                native_resources::COPY_SOURCE | native_resources::COPY_DEST
                            )
                    }))
        })
    });
    if paired {
        return image_copy_transition(
            storage,
            cookie,
            resource,
            resource_cookie,
            subresource,
            before,
            after,
        );
    }
    let owned = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        // The genuine unified runtime list starts draw-capable. Its first
        // supported image barrier selects the existing retained transaction;
        // unrelated private workflows keep their original mode contracts.
        if list.kind == Kind::Direct
            && list.runtime_error != 0
            && list.direct_mode == DirectMode::Draw
            && list.phase == Phase::Recording
            && !list.dispatched
            && list.image_transition.is_none()
            && before == native_images::IMAGE_STATE_RENDER_TARGET
        {
            if after == native_resources::COPY_SOURCE {
                list.direct_mode = DirectMode::ImageTransition;
            } else if after == native_resources::COPY_DEST {
                list.direct_mode = DirectMode::ImageUpload;
            }
        }
        if list.kind != Kind::Direct
            || !matches!(
                list.direct_mode,
                DirectMode::ImageTransition | DirectMode::ImageReadback | DirectMode::ImageUpload
            )
            || list.phase != Phase::Recording
        {
            return Err(INVALID);
        }
        let copy_state = if list.direct_mode == DirectMode::ImageUpload {
            native_resources::COPY_DEST
        } else {
            native_resources::COPY_SOURCE
        };
        let closing = if let Some(transition) = &list.image_transition {
            if transition.closed
                || (list.direct_mode == DirectMode::ImageReadback && transition.readback.is_none())
                || (list.direct_mode == DirectMode::ImageUpload && transition.upload.is_none())
                || transition.resource != resource
                || transition.cookie != resource_cookie
                || transition_layer(&transition.image, subresource) != Some(transition.layer)
                || before != copy_state
                || after != native_images::IMAGE_STATE_RENDER_TARGET
            {
                return Err(INVALID);
            }
            true
        } else {
            if before != native_images::IMAGE_STATE_RENDER_TARGET || after != copy_state {
                return Err(INVALID);
            }
            false
        };
        list.operating = true;
        Ok((index, list.device, list.device_cookie, closing))
    });
    let (index, device, device_cookie, closing) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let mut candidate = if closing {
        None
    } else {
        match native_images::retain_initialized_image(
            device,
            device_cookie,
            resource,
            resource_cookie,
        ) {
            Ok(image) => Some(image),
            Err(status) => {
                binding_failed(index);
                return status;
            }
        }
    };
    if candidate
        .as_ref()
        .is_some_and(|image| native_images::supported_depth_format(image.format()))
    {
        native_images::release_initialized_image(candidate.take().expect("retained depth image"));
        binding_failed(index);
        return INVALID;
    }
    let layer = if let Some(image) = &candidate {
        match transition_layer(image, subresource) {
            Some(layer) => layer,
            None => {
                native_images::release_initialized_image(
                    candidate.take().expect("unique initialized image"),
                );
                binding_failed(index);
                return INVALID;
            }
        }
    } else {
        0 // The existing transition already admits the identical layer or range.
    };
    let adapter = if let Some(image) = &candidate {
        image.adapter()
    } else {
        with_registry(|registry| {
            registry.lists[index]
                .image_transition
                .as_ref()
                .expect("pinned transition")
                .image
                .adapter()
        })
    };
    let status = unsafe { ql_dx12_native_parent_status(adapter) };
    if status < 0 {
        if let Some(image) = candidate {
            native_images::release_initialized_image(image);
        }
        binding_failed(index);
        return status;
    }
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        if closing {
            list.image_transition
                .as_mut()
                .expect("exact closing transition")
                .closed = true;
        } else {
            list.image_transition = Some(ImageTransitionRecording {
                copy_state: after,
                image: candidate.take().expect("unique initialized image"),
                resource,
                cookie: resource_cookie,
                layer,
                closed: false,
                readback: None,
                upload: None,
            });
        }
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(status) => {
            if let Some(image) = candidate {
                native_images::release_initialized_image(image);
            }
            binding_failed(index);
            status
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_readback_list_create(
    device: usize,
    cookie: u64,
    storage: usize,
    allocator: usize,
    allocator_cookie: u64,
    output: *mut u64,
) -> i32 {
    unsafe {
        create_list(
            device,
            cookie,
            storage,
            allocator,
            allocator_cookie,
            Kind::Direct,
            DirectMode::ImageReadback,
            output,
        )
    }
}

/// Genuine R0 copy inputs are scalar snapshots. This operation only fits between
/// the explicit opening and closing barriers on the exact same initialized image.
/// The READBACK lease excludes other GPU writers and prevents retirement until
/// actual owners release it. Private Map remains excluded; genuine public
/// coherent READBACK maps retain an independent address and require application
/// synchronization with actual completion before CPU reads.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_readback_record(
    storage: usize,
    cookie: u64,
    source: usize,
    source_cookie: u64,
    destination: usize,
    destination_cookie: u64,
    footprint: *const ImageReadbackFootprint,
) -> i32 {
    if footprint.is_null() {
        return INVALID;
    }
    let footprint = unsafe { footprint.read() };
    let bytes = match footprint.byte_extent() {
        Some(bytes) => bytes,
        None => return INVALID,
    };
    let owned = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        if list.kind != Kind::Direct
            || !(list.direct_mode == DirectMode::ImageReadback
                || (list.runtime_error != 0 && list.direct_mode == DirectMode::ImageTransition))
            || list.phase != Phase::Recording
        {
            return Err(INVALID);
        }
        let transition = list.image_transition.as_ref().ok_or(INVALID)?;
        if transition.closed
            || transition.readback.is_some()
            || transition.resource != source
            || transition.cookie != source_cookie
            || !transition.covers_layer(footprint.source_subresource)
            || transition.image.sample_count() != 1
            || transition.image.format() != footprint.format
            || transition.image.extent() != (footprint.width, footprint.height)
            || transition.image.depth() != footprint.depth
        {
            return Err(INVALID);
        }
        let adapter = transition.image.adapter();
        // A barrier-only COPY_SOURCE transaction remains valid. Only an
        // admitted copy selects the stricter readback workflow for Close.
        list.direct_mode = DirectMode::ImageReadback;
        list.operating = true;
        Ok((
            index,
            list.device,
            list.device_cookie,
            adapter,
            list.runtime_error != 0,
        ))
    });
    let (index, device, device_cookie, adapter, public_write) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let buffer = match native_resources::retain_readback(
        device,
        device_cookie,
        destination,
        destination_cookie,
        footprint.offset,
        bytes,
        public_write,
    ) {
        Ok(buffer) => buffer,
        Err(status) => {
            binding_failed(index);
            return status;
        }
    };
    let status = if buffer.adapter() == adapter {
        unsafe { ql_dx12_native_parent_status(adapter) }
    } else {
        INVALID
    };
    if status < 0 {
        native_resources::release_readback(buffer);
        binding_failed(index);
        return status;
    }
    let mut candidate = Some(buffer);
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        list.image_transition
            .as_mut()
            .expect("pinned exact open image transition")
            .readback = Some(ImageReadbackRecording {
            buffer: candidate.take().expect("unique exact readback owner"),
            footprint,
        });
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(status) => {
            native_resources::release_readback(
                candidate.expect("failed publication retains readback"),
            );
            binding_failed(index);
            status
        }
    }
}

fn graphics_targets_match(list: &List, pipeline: &PipelineBinding) -> bool {
    if list.runtime_error == 0 {
        return list
            .render_target
            .as_ref()
            .is_some_and(|target| pipeline.matches_rtv(target));
    }
    let PipelineBinding::Graphics(pipeline) = pipeline else {
        return false;
    };
    let count = pipeline.render_target_count() as usize;
    let formats = pipeline.render_target_formats();
    if !(1..=8).contains(&count) {
        return false;
    }
    for i in 0..8 {
        let target = if i == 0 {
            list.render_target.as_ref()
        } else {
            list.extra_render_targets[i - 1].as_ref()
        };
        if i < count {
            if !target.is_some_and(|target| {
                target.format() == formats[i] && target.sample_count() == pipeline.sample_count()
            }) {
                return false;
            }
        } else if target.is_some() {
            return false;
        }
    }
    true
}
fn graphics_ready(list: &List) -> bool {
    list.direct_mode == DirectMode::Draw
        && list.direct_flags == DIRECT_COMPLETE_STATE
        && list.direct_state.supported()
        && list
            .pipeline
            .as_ref()
            .zip(list.root.as_ref())
            .is_some_and(|(pipeline, root)| {
                (list.runtime_error!=0 || (root.plan().words==0 && matches!(pipeline,PipelineBinding::Graphics(p) if p.vertex_requirements().slots==0)))
                    && (if list.runtime_error != 0 {
                        unsafe { root.matches_pipeline_locked(pipeline.root_identity()) }
                    } else {
                        pipeline.root_identity() == root.identity()
                    })
                    && pipeline.matches_topology(list.direct_topology)
                    && graphics_targets_match(list, pipeline)
                    && pipeline.matches_dsv(list.depth_target.as_ref())
            })
}

fn genuine_draw_list(storage: usize, cookie: u64) -> bool {
    with_registry(|registry| {
        registry.list(storage, cookie).is_some_and(|i| {
            let list = &registry.lists[i];
            list.runtime_error != 0 && list.direct_mode == DirectMode::Draw
        })
    })
}
fn record_graphics_command(
    storage: usize,
    cookie: u64,
    arguments: DrawArguments,
    indexed: Option<IndexedArguments>,
    indirect: Option<IndirectRequest>,
) -> i32 {
    let (slot, device, device_cookie) = match binding_begin(storage, cookie, Kind::Direct) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let snapshot = with_registry(|registry| {
        let list = &registry.lists[slot];
        if !graphics_ready(list) || (indirect.is_some() && list.runtime_error == 0) {
            return Err(INVALID);
        }
        if list.graphics_stream.count == u32::MAX {
            return Err(NO_MEMORY);
        }
        let pipeline = match list.pipeline.as_ref() {
            Some(PipelineBinding::Graphics(p)) => p,
            _ => return Err(INVALID),
        };
        let required = pipeline.vertex_requirements();
        let mut vertices = [VertexViewInput::default(); 32];
        for slot in 0..32 {
            if required.slots & (1u32 << slot) == 0 {
                continue;
            }
            let Some(binding) = list.vertex_bindings.find(slot as u32) else {
                continue; // Snapshot an explicit consumed null binding below.
            };
            let Some(range) = binding.range.as_ref() else {
                continue;
            };
            let state = buffer_state_for(list, range.resource, range.resource_cookie)
                .unwrap_or(range.initial);
            if !native_resources::buffer_state_allows(
                range.kind,
                state,
                native_resources::VERTEX_BUFFER,
            ) {
                return Err(INVALID);
            }
            if binding.work.offset % 4 != 0 || binding.work.stride % 4 != 0 {
                return Err(NO_INTERFACE);
            }
            let stride = u64::from(binding.work.stride);
            let extent = u64::from(required.bytes[slot]);
            if stride != 0 && stride < extent {
                return Err(NO_INTERFACE);
            }
            // The selected device requires and enables robustBufferAccess2.
            // Exact bound views, not allocation padding, govern all indexed and
            // non-indexed GPU fetches. GPU-written indices must never be read
            // back or guessed at recording time; out-of-range attributes read
            // zero through Vulkan's real vertex-input robustness contract.
            vertices[slot] = VertexViewInput {
                address: range.address,
                bytes: binding.work.bytes as u32,
                stride: binding.work.stride,
            };
        }
        let index = if let Some(args) = indexed {
            let binding = list.index_binding.as_ref().ok_or(INVALID)?;
            let range = &binding.range;
            let state = buffer_state_for(list, range.resource, range.resource_cookie)
                .unwrap_or(range.initial);
            if !native_resources::buffer_state_allows(
                range.kind,
                state,
                native_resources::INDEX_BUFFER,
            ) || u64::from(args.first_index)
                .checked_add(u64::from(args.count))
                .and_then(|end| end.checked_mul(index_stride(binding.format)?))
                .is_none_or(|bytes| bytes > range.bytes)
            {
                return Err(INVALID);
            }
            Some((range.address, range.bytes, binding.format))
        } else {
            None
        };
        let mut targets = [0usize; 8];
        targets[0] = list.render_target.as_ref().unwrap().caller_address();
        for i in 0..7 {
            targets[i + 1] = list.extra_render_targets[i]
                .as_ref()
                .map_or(0, RtvLease::caller_address);
        }
        Ok((
            pipeline.caller_identity(),
            list.root.as_ref().unwrap().caller_identity(),
            targets,
            list.depth_target.as_ref().map(|d| d.caller_address()),
            index,
            vertices,
            required.slots,
            snapshot_cbvs(list, pipeline.resource_bytes())?,
            indirect.map(|request| buffer_state_for(list, request.buffer, request.buffer_cookie)),
            indirect.and_then(|_|list.index_binding.as_ref().map(|binding|{
                let range=&binding.range;
                (range.address,range.bytes,binding.format,native_resources::buffer_state_allows(range.kind,buffer_state_for(list,range.resource,range.resource_cookie).unwrap_or(range.initial),native_resources::INDEX_BUFFER))
            })),
            GraphicsWork {
                topology: list.direct_topology,
                state: list.direct_state,
                arguments,
                constants: list.blend_constants,
                root_constants: RootConstants {
                    words: list.root.as_ref().unwrap().plan().words,
                    values: list.root_values,
                    ..RootConstants::default()
                },
                ..GraphicsWork::default()
            },
        ))
    });
    let (
        pipeline,
        root,
        target,
        depth,
        index,
        vertices,
        vertex_slots,
        cbv_snapshots,
        indirect_state,
        indirect_index,
        work,
    ) = match snapshot {
        Ok(v) => v,
        Err(e) => {
            binding_failed(slot);
            return e;
        }
    };
    let mut candidate = GraphicsRecording {
        next: 0,
        pipeline: None,
        root: None,
        target: None,
        extra_targets: [const { None }; 7],
        depth: None,
        index: None,
        work,
        query: None,
        destination_write: None,
        indirect: None,
        vertices: VertexBindings::new(),
        cbvs: CbvBindings::new(),
    };
    let retained = (|| -> Result<(), i32> {
        if let Some(request) = indirect {
            if request.count_buffer != 0 || request.count_cookie != 0 {
                return Err(NO_INTERFACE);
            }
            let _ignored_count_offset = request.count_offset; // No count buffer: the offset is unconsumed.
            if indexed.is_some() {
                return Err(INVALID);
            }
            let signature = crate::native_signatures::retain_signature(
                device,
                device_cookie,
                request.signature,
                request.signature_cookie,
            )?;
            if signature.kind > 1 {
                crate::native_signatures::release_signature(signature);
                return Err(NO_INTERFACE);
            }
            let index_snapshot=if signature.kind==1 {
                match indirect_index {
                    Some((address,bytes,format,true))=>Some((address,bytes,format)),
                    _=>{crate::native_signatures::release_signature(signature);return Err(INVALID);}
                }
            }else{None};
            let bytes = u64::from(request.count)
                .checked_mul(u64::from(signature.stride))
                .ok_or(INVALID);
            let arguments = match bytes.and_then(|bytes| {
                native_resources::retain_indirect_read(
                    device,
                    device_cookie,
                    request.buffer,
                    request.buffer_cookie,
                    request.offset,
                    bytes,
                    indirect_state.flatten(),
                )
            }) {
                Ok(v) => v,
                Err(e) => {
                    crate::native_signatures::release_signature(signature);
                    return Err(e);
                }
            };
            if arguments.adapter != signature.adapter {
                native_resources::release_indirect_read(arguments);
                crate::native_signatures::release_signature(signature);
                return Err(INVALID);
            }
            candidate.work.kind = if index_snapshot.is_some(){INDIRECT_INDEXED_DRAW}else{INDIRECT_DRAW};
            candidate.work.indirect = IndirectWork {
                buffer_token: arguments.token,
                offset: arguments.offset,
                bytes: arguments.bytes,
                count: request.count,
                stride: signature.stride,
                ..IndirectWork::default()
            };
            candidate.indirect = Some(IndirectRecording {
                signature,
                arguments,
            });
            if let Some((address,bytes,format))=index_snapshot {
                let range=native_resources::retain_index_range(device,device_cookie,address,bytes)?;
                candidate.work.indexed=IndexedWork{buffer_token:range.token,offset:range.offset,bytes:range.bytes,format,..IndexedWork::default()};
                candidate.index=Some(IndexBinding{range,format});
            }
        }
        candidate.pipeline = Some(native_pipelines::retain_graphics_pipeline(
            device,
            device_cookie,
            pipeline.0,
            pipeline.1,
        )?);
        candidate.root = Some(native_shaders::retain_empty_root_binding(
            device,
            device_cookie,
            root.0,
            root.1,
        )?);
        candidate.target = Some(native_descriptors::retain_rtv(
            device,
            device_cookie,
            target[0],
        )?);
        for i in 0..7 {
            if target[i + 1] != 0 {
                candidate.extra_targets[i] = Some(native_descriptors::retain_rtv(
                    device,
                    device_cookie,
                    target[i + 1],
                )?);
            }
        }
        if let Some(depth) = depth {
            candidate.depth = Some(native_descriptors::retain_dsv(
                device,
                device_cookie,
                depth,
            )?);
        }
        if let Some((address, bytes, format)) = index {
            candidate.index = Some(IndexBinding {
                range: native_resources::retain_index_range(device, device_cookie, address, bytes)?,
                format,
            });
        }
        for slot in 0..32 {
            if vertex_slots & (1u32 << slot) != 0 {
                candidate.vertices.append(retain_vertex_binding(
                    device,
                    device_cookie,
                    slot as u32,
                    vertices[slot],
                )?);
            }
        }
        candidate.cbvs = retain_cbv_snapshots(device, device_cookie, cbv_snapshots)?;
        Ok(())
    })();
    if let Err(status) = retained {
        release_graphics_recording(candidate);
        binding_failed(slot);
        return status;
    }
    candidate.work.root_constants.cbv_head = candidate.cbvs.head as u64;
    candidate.work.root_constants.cbv_count = candidate.cbvs.count;
    candidate.work.vertex_head = candidate.vertices.head as u64;
    candidate.work.vertex_count = candidate.vertices.count;
    candidate.work.pipeline = candidate.pipeline.as_ref().unwrap().owner() as u64;
    candidate.work.target = candidate.target.as_ref().unwrap().owner() as u64;
    candidate.work.depth_target = candidate.depth.as_ref().map_or(0, |d| d.owner() as u64);
    candidate.work.target_count = 1;
    for i in 0..7 {
        if let Some(target) = candidate.extra_targets[i].as_ref() {
            candidate.work.targets_tail[i] = target.owner() as u64;
            candidate.work.target_count += 1;
        }
    }
    if let Some(args) = indexed {
        let binding = candidate.index.as_ref().unwrap();
        candidate.work.kind = 1;
        candidate.work.indexed = IndexedWork {
            buffer_token: binding.range.token,
            offset: binding.range.offset,
            bytes: binding.range.bytes,
            format: binding.format,
            count: args.count,
            instances: args.instances,
            first_index: args.first_index,
            base_vertex: args.base_vertex,
            first_instance: args.first_instance,
            ..IndexedWork::default()
        };
    }
    let memory =
        unsafe { ql_dx12_native_recording_allocate(core::mem::size_of::<GraphicsRecording>()) }
            as *mut GraphicsRecording;
    if memory.is_null() {
        release_graphics_recording(candidate);
        binding_failed(slot);
        return NO_MEMORY;
    }
    unsafe {
        memory.write(candidate);
    }
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[slot];
        assert!(list.operating && list.phase == Phase::Recording && list.cookie == cookie);
        list.graphics_stream.append(memory as usize);
        list.dispatched = true;
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(status) => {
            release_graphics_recording(unsafe { memory.read() });
            unsafe {
                ql_dx12_native_recording_free(memory.cast());
            }
            binding_failed(slot);
            status
        }
    }
}
/// Genuine draws append independent immutable consumed snapshots. Private
/// version1 retains its original fixed triangle and frozen binding contract.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_draw_record(
    storage: usize,
    cookie: u64,
    vertices: u32,
    instances: u32,
    first_vertex: u32,
    first_instance: u32,
) -> i32 {
    if genuine_draw_list(storage, cookie) {
        return record_graphics_command(
            storage,
            cookie,
            DrawArguments {
                vertices,
                instances,
                first_vertex,
                first_instance,
            },
            None,
            None,
        );
    }
    direct_state_commit(storage, cookie, |list| {
        if list.direct_mode != DirectMode::Draw
            || (list.runtime_error == 0
                && (vertices != DRAW_VERTEX_COUNT
                    || instances != DRAW_INSTANCE_COUNT
                    || first_vertex != 0
                    || first_instance != 0))
            || list.direct_flags != DIRECT_COMPLETE_STATE
            || !list.direct_state.supported()
            || !graphics_ready(list)
        {
            return Err(INVALID);
        }
        list.draw_arguments = DrawArguments {
            vertices,
            instances,
            first_vertex,
            first_instance,
        };
        list.dispatched = true;
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_draw_list_create(
    device: usize,
    cookie: u64,
    storage: usize,
    allocator: usize,
    allocator_cookie: u64,
    output: *mut u64,
) -> i32 {
    unsafe {
        create_list(
            device,
            cookie,
            storage,
            allocator,
            allocator_cookie,
            Kind::Direct,
            DirectMode::Draw,
            output,
        )
    }
}

/// One explicit closed RT/COPY_DEST/RT transaction writes the initialized image
/// from a real native UPLOAD source. No hidden persistent state is introduced.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_upload_list_create(
    device: usize,
    cookie: u64,
    storage: usize,
    allocator: usize,
    allocator_cookie: u64,
    output: *mut u64,
) -> i32 {
    unsafe {
        create_list(
            device,
            cookie,
            storage,
            allocator,
            allocator_cookie,
            Kind::Direct,
            DirectMode::ImageUpload,
            output,
        )
    }
}

/// C snapshots the active genuine placed source layout. The non-Copy upload
/// read lease blocks Map/retire and survives all attempted submissions/reset.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_upload_record(
    storage: usize,
    cookie: u64,
    source: usize,
    source_cookie: u64,
    destination: usize,
    destination_cookie: u64,
    footprint: *const ImageUploadFootprint,
) -> i32 {
    if footprint.is_null() {
        return INVALID;
    }
    let footprint = unsafe { footprint.read() };
    let bytes = match footprint.byte_extent() {
        Some(bytes) => bytes,
        None => return INVALID,
    };
    let owned = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        if list.kind != Kind::Direct
            || list.direct_mode != DirectMode::ImageUpload
            || list.phase != Phase::Recording
        {
            return Err(INVALID);
        }
        let transition = list.image_transition.as_ref().ok_or(INVALID)?;
        if transition.closed
            || transition.upload.is_some()
            || transition.resource != destination
            || transition.cookie != destination_cookie
            || !transition.covers_layer(footprint.destination_subresource)
            || transition.image.sample_count() != 1
            || transition.image.format() != footprint.format
            || transition.image.extent() != (footprint.width, footprint.height)
            || transition.image.depth() != footprint.depth
        {
            return Err(INVALID);
        }
        list.operating = true;
        Ok((
            index,
            list.device,
            list.device_cookie,
            transition.image.adapter(),
        ))
    });
    let (index, device, device_cookie, adapter) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let buffer = match native_resources::retain_upload(
        device,
        device_cookie,
        source,
        source_cookie,
        footprint.offset,
        bytes,
    ) {
        Ok(buffer) => buffer,
        Err(status) => {
            binding_failed(index);
            return status;
        }
    };
    let status = if buffer.adapter() == adapter {
        unsafe { ql_dx12_native_parent_status(adapter) }
    } else {
        INVALID
    };
    if status < 0 {
        native_resources::release_upload(buffer);
        binding_failed(index);
        return status;
    }
    let mut candidate = Some(buffer);
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        list.image_transition
            .as_mut()
            .expect("pinned exact open image transition")
            .upload = Some(ImageUploadRecording {
            buffer: candidate.take().expect("unique exact upload source owner"),
            footprint,
        });
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(status) => {
            native_resources::release_upload(
                candidate.expect("failed publication retains upload source"),
            );
            binding_failed(index);
            status
        }
    }
}

#[derive(Clone, Copy)]
pub struct FenceQueueIdentity {
    pub storage: usize,
    pub cookie: u64,
    pub device: usize,
    pub device_cookie: u64,
    pub adapter: usize,
}
fn fence_identity(queue: &Queue) -> FenceQueueIdentity {
    FenceQueueIdentity {
        storage: queue.storage,
        cookie: queue.cookie,
        device: queue.device,
        device_cookie: queue.device_cookie,
        adapter: queue.adapter,
    }
}
/// Reserve the real DIRECT queue while the selected Vulkan device presents
/// one frame. A separate per-target pin prevents premature queue retirement.
pub fn begin_present_operation(
    storage: usize,
    cookie: u64,
    device: usize,
    device_cookie: u64,
) -> Result<(FenceQueueIdentity, bool), i32> {
    with_registry(|registry| {
        let index = registry.queue(storage, cookie).ok_or(INVALID)?;
        let queue = &mut registry.queues[index];

        if queue.kind != Kind::Direct
            || queue.device != device
            || queue.device_cookie != device_cookie
        {
            return Err(INVALID);
        }
        if queue.fault < 0 {
            return Err(queue.fault);
        }
        if queue.operating || queue.pending.is_some() {
            return Err(BUSY);
        }
        queue.operating = true;
        Ok((fence_identity(queue), queue.managed_fences))
    })
}

pub fn end_queue_operation(storage: usize, cookie: u64, fault: i32) {
    with_registry(|registry| {
        let index = registry
            .queue(storage, cookie)
            .expect("active operation owns queue");
        let queue = &mut registry.queues[index];
        assert!(queue.operating);
        if fault < 0 && queue.fault >= 0 {
            queue.fault = fault;
        }
        queue.operating = false;
    });
}

pub fn retain_presentation_queue(identity: FenceQueueIdentity) {
    with_registry(|registry| {
        let index = registry
            .queue(identity.storage, identity.cookie)
            .expect("present operation pins queue");
        let queue = &mut registry.queues[index];
        assert!(queue.operating);
        // The adapter's fixed presentation capacity bounds this count.
        queue.presentation_references += 1;
    });
}

pub fn release_presentation_queue(identity: FenceQueueIdentity) {
    with_registry(|registry| {
        let index = registry
            .queue(identity.storage, identity.cookie)
            .expect("presentation owns queue reference");
        let queue = &mut registry.queues[index];
        assert!(queue.presentation_references > 0);
        queue.presentation_references -= 1;
    });
}

pub fn begin_fence_attach(storage: usize, cookie: u64) -> Result<FenceQueueIdentity, i32> {
    with_registry(|registry| {
        let index = registry.queue(storage, cookie).ok_or(INVALID)?;
        let queue = &mut registry.queues[index];
        if queue.operating || queue.pending.is_some() || queue.managed_fences {
            return Err(BUSY);
        }
        if queue.fault < 0 {
            return Err(queue.fault);
        }
        queue.operating = true;
        Ok(fence_identity(queue))
    })
}
pub fn finish_fence_attach(storage: usize, cookie: u64, success: bool) {
    with_registry(|registry| {
        let index = registry
            .queue(storage, cookie)
            .expect("attach pins native queue");
        let queue = &mut registry.queues[index];
        assert!(queue.operating && !queue.managed_fences);
        queue.managed_fences = success;
        queue.operating = false;
    });
}
pub struct ScheduledListLease {
    queue: usize,
    queue_cookie: u64,
    list: usize,
    list_cookie: u64,
    generation: u64,
}
pub fn reserve_scheduled_list(
    queue: FenceQueueIdentity,
    list: usize,
    list_cookie: u64,
) -> Result<ScheduledListLease, i32> {
    with_registry(|registry| {
        let qi = registry.queue(queue.storage, queue.cookie).ok_or(INVALID)?;
        let li = registry.list(list, list_cookie).ok_or(INVALID)?;
        let q = &registry.queues[qi];
        let slot = &registry.lists[li];
        if !q.managed_fences
            || q.device != slot.device
            || q.device_cookie != slot.device_cookie
            || q.kind != slot.kind
            || !executable(slot)
        {
            return Err(INVALID);
        }
        if slot.operating || slot.scheduled_owner != 0 {
            return Err(BUSY);
        }
        if q.fault < 0 {
            return Err(q.fault);
        }
        let generation = registry.lists[li].generation;
        registry.lists[li].operating = true;
        registry.lists[li].scheduled_owner = queue.cookie;
        Ok(ScheduledListLease {
            queue: queue.storage,
            queue_cookie: queue.cookie,
            list,
            list_cookie,
            generation,
        })
    })
}
pub fn release_scheduled_list(lease: ScheduledListLease) {
    with_registry(|registry| {
        if let Some(index) = registry.list(lease.list, lease.list_cookie) {
            let list = &mut registry.lists[index];
            if list.scheduled_owner == lease.queue_cookie && list.generation == lease.generation {
                list.scheduled_owner = 0;
                list.operating = false;
            }
        }
    });
}
#[derive(Clone, Copy)]
pub struct ScheduledListKey {
    queue: usize,
    queue_cookie: u64,
    list: usize,
    list_cookie: u64,
    generation: u64,
}
pub fn scheduled_list_key(lease: &ScheduledListLease) -> ScheduledListKey {
    ScheduledListKey {
        queue: lease.queue,
        queue_cookie: lease.queue_cookie,
        list: lease.list,
        list_cookie: lease.list_cookie,
        generation: lease.generation,
    }
}
/// The scheduling owner retains the unique lease while this copied key is used.
pub fn execute_scheduled(key: ScheduledListKey) -> i32 {
    let status = execute_immediate(
        key.queue,
        key.queue_cookie,
        key.list,
        key.list_cookie,
        Some(key.generation),
    );
    // Only a still-owned pre-submit reservation makes BUSY retryable. Any
    // terminal failure poisons this logical queue before its later signals can
    // run, including errors rejected before the host attempted submission.
    let status = if status == BUSY && !scheduled_list_reserved(key) {
        FAIL
    } else {
        status
    };
    if status < 0 && status != BUSY {
        with_registry(|registry| {
            if let Some(index) = registry.queue(key.queue, key.queue_cookie) {
                let queue = &mut registry.queues[index];
                if queue.fault >= 0 || queue.fault == BUSY {
                    queue.fault = status;
                }
            }
        });
    }
    status
}
/// A busy return is retryable only while the exact pre-submit reservation
/// remains held. Accepted or uncertain work must never be submitted again.
pub fn scheduled_list_reserved(key: ScheduledListKey) -> bool {
    with_registry(|registry| {
        registry
            .list(key.list, key.list_cookie)
            .is_some_and(|index| {
                let list = &registry.lists[index];
                list.generation == key.generation
                    && executable(list)
                    && list.operating
                    && list.scheduled_owner == key.queue_cookie
            })
    })
}
/// Every logical queue shares the selected VkQueue. The scheduler waits for the
/// accepted owner before starting another command; blocked CPU waits consume no
/// VkQueue slot and unrelated logical queues remain eligible.
pub fn fence_adapter_idle(adapter: usize) -> bool {
    with_registry(|registry| {
        !registry.queues.iter().any(|queue| {
            queue.state != State::Vacant
                && queue.adapter == adapter
                && (queue.operating || queue.pending.is_some())
        })
    })
}
pub fn fence_queue_status(storage: usize, cookie: u64) -> Result<(), i32> {
    with_registry(|registry| {
        let index = registry.queue(storage, cookie).ok_or(INVALID)?;
        let queue = &registry.queues[index];
        if queue.fault < 0 {
            Err(queue.fault)
        } else if queue.operating || queue.pending.is_some() {
            Err(BUSY)
        } else {
            Ok(())
        }
    })
}

/// Private net-state resolve. Both resources enter and leave in RENDER_TARGET.
/// This consumes one entire source array layer and its matching destination.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_color_resolve_record(
    storage: usize,
    cookie: u64,
    source: usize,
    source_cookie: u64,
    destination: usize,
    destination_cookie: u64,
    source_layer: u32,
    destination_layer: u32,
) -> i32 {
    color_resolve_record(
        storage,
        cookie,
        source,
        source_cookie,
        destination,
        destination_cookie,
        source_layer,
        destination_layer,
        None,
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_color_resolve_record_typed(
    storage: usize,
    cookie: u64,
    source: usize,
    source_cookie: u64,
    destination: usize,
    destination_cookie: u64,
    source_layer: u32,
    destination_layer: u32,
    format: u32,
) -> i32 {
    if format == 0 {
        return INVALID;
    }
    color_resolve_record(
        storage,
        cookie,
        source,
        source_cookie,
        destination,
        destination_cookie,
        source_layer,
        destination_layer,
        Some(format),
    )
}

fn color_resolve_record(
    storage: usize,
    cookie: u64,
    source: usize,
    source_cookie: u64,
    destination: usize,
    destination_cookie: u64,
    source_layer: u32,
    destination_layer: u32,
    format: Option<u32>,
) -> i32 {
    let owned = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        if list.kind != Kind::Direct
            || list.direct_mode != DirectMode::RenderTarget
            || list.phase != Phase::Recording
            || list.resolve.is_some()
            || list.render_target.is_some()
            || list.clear.is_some()
            || source == destination
        {
            return Err(INVALID);
        }
        if list.runtime_error != 0 || format.is_some() {
            let src = list.resolve_transitions[0].as_ref().ok_or(INVALID)?;
            let dst = list.resolve_transitions[1].as_ref().ok_or(INVALID)?;
            if src.closed
                || dst.closed
                || src.resource != source
                || src.cookie != source_cookie
                || dst.resource != destination
                || dst.cookie != destination_cookie
                || src.layer != source_layer
                || dst.layer != destination_layer
            {
                return Err(INVALID);
            }
        }
        list.operating = true;
        Ok((index, list.device, list.device_cookie))
    });
    let (index, device, device_cookie) = match owned {
        Ok(value) => value,
        Err(status) => return status,
    };
    let source =
        match native_images::retain_initialized_image(device, device_cookie, source, source_cookie)
        {
            Ok(value) => value,
            Err(status) => {
                binding_failed(index);
                return status;
            }
        };
    let destination = match native_images::retain_initialized_image(
        device,
        device_cookie,
        destination,
        destination_cookie,
    ) {
        Ok(value) => value,
        Err(status) => {
            native_images::release_initialized_image(source);
            binding_failed(index);
            return status;
        }
    };
    if !native_images::supported_color_format(source.format())
        || source.sample_count() <= 1
        || destination.sample_count() != 1
        || source.format() != destination.format()
        || format.is_some_and(|format| format != source.format())
        || source.extent() != destination.extent()
        || source.resource_type() != native_images::IMAGE_RESOURCE_TEXTURE2D
        || destination.resource_type() != native_images::IMAGE_RESOURCE_TEXTURE2D
        || source_layer >= source.array_layers()
        || destination_layer >= destination.array_layers()
    {
        native_images::release_initialized_image(source);
        native_images::release_initialized_image(destination);
        binding_failed(index);
        return INVALID;
    }
    let mut resolve = Some(ResolveRecording {
        source,
        destination,
        source_layer,
        destination_layer,
    });
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        registry.lists[index].resolve = resolve.take();
        registry.lists[index].operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(status) => {
            let resolve = resolve.expect("failed admission retains resolve operands");
            native_images::release_initialized_image(resolve.source);
            native_images::release_initialized_image(resolve.destination);
            binding_failed(index);
            status
        }
    }
}

/// A bounded independent depth clear retains its typed DSV until the same
/// command-generation completion/drain boundary as a color clear.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_depth_clear(
    storage: usize,
    cookie: u64,
    address: usize,
    flags: u32,
    depth: f32,
    stencil: u32,
    rect_count: u32,
    rectangles: *const ClearRect,
) -> i32 {
    if rect_count > CLEAR_RECT_LIMIT || (rect_count != 0 && rectangles.is_null()) {
        return INVALID;
    }
    if flags == 0
        || flags & !3 != 0
        || stencil > 255
        || (flags & 1 != 0 && (!depth.is_finite() || !(0.0..=1.0).contains(&depth)))
    {
        return INVALID;
    }
    let depth_stencil_flags = if flags & 1 != 0 {
        RENDER_TARGET_DEPTH_CLEAR
    } else {
        0
    } | if flags & 2 != 0 {
        RENDER_TARGET_STENCIL_CLEAR
    } else {
        0
    };
    let mut regions = ClearRegions {
        explicit: rect_count != 0,
        count: rect_count,
        ..ClearRegions::default()
    };
    for index in 0..rect_count as usize {
        regions.rectangles[index] = unsafe { rectangles.add(index).read() };
    }
    record_render_target(
        storage,
        cookie,
        address,
        Some((
            ClearColor {
                red: if flags & 1 != 0 { depth } else { 0.0 },
                green: if flags & 2 != 0 { stencil as f32 } else { 0.0 },
                blue: 0.0,
                alpha: 0.0,
            },
            regions,
        )),
        depth_stencil_flags,
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_depth_binding_allowed(storage: usize, cookie: u64) -> u32 {
    with_registry(|registry| {
        registry.list(storage, cookie).is_some_and(|index| {
            let list = &registry.lists[index];
            list.kind == Kind::Direct
                && list.direct_mode == DirectMode::Draw
                && list.phase == Phase::Recording
                && !list.operating
                && (!list.dispatched || list.runtime_error != 0)
        })
    }) as u32
}
/// One atomic OM snapshot owns both exact descriptor generations. No partial
/// binding escapes if either operand is invalid or native fault admission fails.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_render_targets_bind(
    storage: usize,
    cookie: u64,
    color_address: usize,
    depth_address: usize,
) -> i32 {
    if ql_dx12_native_depth_binding_allowed(storage, cookie) == 0 {
        return INVALID;
    }
    let (index, device, device_cookie) =
        match render_target_begin(storage, cookie, TargetOperation::Bind) {
            Ok(v) => v,
            Err(status) => return status,
        };
    let color = match native_descriptors::retain_rtv(device, device_cookie, color_address) {
        Ok(v) => v,
        Err(status) => {
            binding_failed(index);
            return status;
        }
    };
    let depth = match native_descriptors::retain_dsv(device, device_cookie, depth_address) {
        Ok(v) => v,
        Err(status) => {
            native_descriptors::release_rtv(color);
            binding_failed(index);
            return status;
        }
    };
    if color.extent() != depth.extent()
        || color.sample_count() != depth.sample_count()
        || color.layer_count() != depth.layer_count()
        || color.adapter() != depth.adapter()
    {
        native_descriptors::release_rtv(depth);
        native_descriptors::release_rtv(color);
        binding_failed(index);
        return INVALID;
    }
    let status = unsafe { ql_dx12_native_parent_status(color.adapter()) };
    if status < 0 {
        native_descriptors::release_rtv(depth);
        native_descriptors::release_rtv(color);
        binding_failed(index);
        return status;
    }
    let mut operands = Some((color, depth));
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        let (color, depth) = operands.take().expect("unique atomic OM operands");
        let old_color = core::mem::replace(&mut list.render_target, Some(color));
        let old_depth = core::mem::replace(&mut list.depth_target, Some(depth));
        list.operating = false;
        let old_extras = core::mem::replace(&mut list.extra_render_targets, [const { None }; 7]);
        Ok((old_color, old_depth, old_extras))
    });
    match committed {
        Ok((color, depth, extras)) => {
            release_extra_targets(extras);
            if let Some(d) = depth {
                native_descriptors::release_rtv(d);
            }
            if let Some(c) = color {
                native_descriptors::release_rtv(c);
            }
            0
        }
        Err(status) => {
            let (color, depth) = operands.expect("failed OM admission retains both operands");
            native_descriptors::release_rtv(depth);
            native_descriptors::release_rtv(color);
            binding_failed(index);
            status
        }
    }
}

/// Genuine OM binding is an atomic snapshot of all active descriptor
/// generations. Failed validation releases only candidates. Consumed draw nodes
/// retain their own references across subsequent unbinding and rebinding.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_mrt_bind(
    storage: usize,
    cookie: u64,
    count: u32,
    addresses: *const usize,
    single_range: u32,
    depth_address: usize,
) -> i32 {
    if count > 8 || single_range > 1 || (count != 0 && addresses.is_null()) {
        return INVALID;
    }
    if count == 0 && depth_address != 0 {
        return NO_INTERFACE;
    }
    if !with_registry(|r| {
        r.list(storage, cookie)
            .is_some_and(|i| r.lists[i].runtime_error != 0)
    }) {
        return INVALID;
    }
    let (index, device, device_cookie) =
        match render_target_begin(storage, cookie, TargetOperation::Bind) {
            Ok(v) => v,
            Err(e) => return e,
        };
    let adapter = with_registry(|r| {
        r.lists[index]
            .allocator
            .as_ref()
            .expect("live recording allocator")
            .adapter
    });
    let mut colors: [Option<RtvLease>; 8] = [const { None }; 8];
    let mut depth = None;
    let retained = (|| -> Result<(), i32> {
        let stride = if single_range != 0 && count != 0 {
            let stride = native_descriptors::ql_dx12_native_descriptor_increment(
                device,
                device_cookie,
                native_descriptors::HEAP_TYPE_RTV,
            );
            if stride == 0 {
                return Err(INVALID);
            }
            stride as usize
        } else {
            0
        };
        for i in 0..count as usize {
            let address = if single_range != 0 {
                unsafe { addresses.read() }
                    .checked_add(i.checked_mul(stride).ok_or(INVALID)?)
                    .ok_or(INVALID)?
            } else {
                unsafe { addresses.add(i).read() }
            };
            colors[i] = Some(native_descriptors::retain_rtv(
                device,
                device_cookie,
                address,
            )?);
        }
        if depth_address != 0 {
            depth = Some(native_descriptors::retain_dsv(
                device,
                device_cookie,
                depth_address,
            )?);
        }
        if let Some(first) = colors[0].as_ref() {
            for target in colors.iter().skip(1).flatten().chain(depth.iter()) {
                if target.extent() != first.extent()
                    || target.sample_count() != first.sample_count()
                    || target.layer_count() != first.layer_count()
                    || target.adapter() != first.adapter()
                {
                    return Err(INVALID);
                }
            }
            for i in 0..count as usize {
                for other in colors.iter().skip(i + 1).flatten().chain(depth.iter()) {
                    if colors[i].as_ref().unwrap().overlaps(other) {
                        return Err(INVALID);
                    }
                }
            }
        }
        let status = unsafe { ql_dx12_native_parent_status(adapter) };
        if status < 0 {
            return Err(status);
        }
        Ok(())
    })();
    let committed = retained.and_then(|()| {
        native_objects::with_device_admission(device, device_cookie, || {
            let list = unsafe { &mut (*REGISTRY.0.get()).lists[index] };
            let old_color = core::mem::replace(&mut list.render_target, colors[0].take());
            let mut extra: [Option<RtvLease>; 7] = [const { None }; 7];
            for i in 0..7 {
                extra[i] = colors[i + 1].take();
            }
            let old_extra = core::mem::replace(&mut list.extra_render_targets, extra);
            let old_depth = core::mem::replace(&mut list.depth_target, depth.take());
            list.operating = false;
            Ok((old_color, old_extra, old_depth))
        })
    });
    match committed {
        Ok((color, extra, depth)) => {
            release_extra_targets(extra);
            if let Some(value) = color {
                native_descriptors::release_rtv(value);
            }
            if let Some(value) = depth {
                native_descriptors::release_rtv(value);
            }
            0
        }
        Err(status) => {
            for value in colors.into_iter().flatten().chain(depth) {
                native_descriptors::release_rtv(value);
            }
            binding_failed(index);
            status
        }
    }
}

/// Genuine R0 OM stencil reference is independent dynamic graphics state. The
/// complete UINT is retained; the eight-bit stencil masks govern comparison.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_direct_set_stencil_reference(
    storage: usize,
    cookie: u64,
    reference: u32,
) -> i32 {
    direct_state_commit(storage, cookie, |list| {
        list.direct_state.stencil_reference = reference;
        Ok(())
    })
}

fn resolve_transaction_closed(list: &List) -> bool {
    if list.runtime_error == 0 && list.resolve_transitions.iter().all(Option::is_none) {
        return true; // Original private net-RT resolve contract.
    }
    list.resolve_transitions
        .iter()
        .all(|entry| entry.as_ref().is_some_and(|entry| entry.closed))
}

/// Genuine R0 resolves require two exact initialized image/subresource states.
/// Both explicit state pairs remain owned until their actual resolve submission
/// completes or safely drains; no intermediate state escapes this bounded list.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_resolve_transition_record(
    storage: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
    subresource: u32,
    before: u32,
    after: u32,
) -> i32 {
    let (role, closing) = match (before, after) {
        (native_images::IMAGE_STATE_RENDER_TARGET, RESOLVE_SOURCE) => (0, false),
        (native_images::IMAGE_STATE_RENDER_TARGET, RESOLVE_DEST) => (1, false),
        (RESOLVE_SOURCE, native_images::IMAGE_STATE_RENDER_TARGET) => (0, true),
        (RESOLVE_DEST, native_images::IMAGE_STATE_RENDER_TARGET) => (1, true),
        _ => return INVALID,
    };
    let acquired = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        if list.kind != Kind::Direct
            || list.runtime_error == 0
            || list.phase != Phase::Recording
            || !matches!(
                list.direct_mode,
                DirectMode::Draw | DirectMode::RenderTarget
            )
            || list.dispatched
            || list.image_transition.is_some()
            || list.render_target.is_some()
            || list.clear.is_some()
        {
            return Err(INVALID);
        }
        let adapter = if closing {
            let entry = list.resolve_transitions[role].as_ref().ok_or(INVALID)?;
            let layer = if subresource == u32::MAX && entry.image.array_layers() == 1 {
                0
            } else {
                subresource
            };
            if entry.closed
                || list.resolve.is_none()
                || entry.resource != resource
                || entry.cookie != resource_cookie
                || entry.layer != layer
            {
                return Err(INVALID);
            }
            entry.image.adapter()
        } else {
            if list.resolve.is_some()
                || list.resolve_transitions[role].is_some()
                || list.resolve_transitions[1 - role]
                    .as_ref()
                    .is_some_and(|entry| entry.resource == resource)
            {
                return Err(INVALID);
            }
            0
        };
        list.operating = true;
        Ok((index, list.device, list.device_cookie, adapter))
    });
    let (index, device, device_cookie, mut adapter) = match acquired {
        Ok(value) => value,
        Err(status) => return status,
    };
    let mut candidate = None;
    if !closing {
        let image = match native_images::retain_initialized_image(
            device,
            device_cookie,
            resource,
            resource_cookie,
        ) {
            Ok(image) => image,
            Err(status) => {
                binding_failed(index);
                return status;
            }
        };
        let layer = if subresource == u32::MAX && image.array_layers() == 1 {
            0
        } else {
            subresource
        };
        if image.resource_type() != native_images::IMAGE_RESOURCE_TEXTURE2D
            || !native_images::supported_color_format(image.format())
            || layer >= image.array_layers()
            || (role == 0 && image.sample_count() <= 1)
            || (role == 1 && image.sample_count() != 1)
        {
            native_images::release_initialized_image(image);
            binding_failed(index);
            return INVALID;
        }
        adapter = image.adapter();
        candidate = Some(ResolveTransition {
            image,
            resource,
            cookie: resource_cookie,
            layer,
            closed: false,
        });
    }
    let status = unsafe { ql_dx12_native_parent_status(adapter) };
    if status < 0 {
        if let Some(entry) = candidate {
            native_images::release_initialized_image(entry.image);
        }
        binding_failed(index);
        return status;
    }
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        if closing {
            list.resolve_transitions[role]
                .as_mut()
                .expect("pinned resolve transition")
                .closed = true;
        } else {
            list.resolve_transitions[role] = candidate.take();
            list.direct_mode = DirectMode::RenderTarget;
        }
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(status) => {
            if let Some(entry) = candidate {
                native_images::release_initialized_image(entry.image);
            }
            binding_failed(index);
            status
        }
    }
}

/// Genuine buffer state transaction. Non-COMMON initial states still require
/// the existing closed round trip. COMMON-created buffers may end in a supported
/// promotable state and decay after actual Execute completion; reset never
/// fabricates a GPU transition or completion.
/// Real Vulkan per-copy memory barriers order each admitted role change.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_buffer_transition_record(
    storage: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
    subresource: u32,
    before: u32,
    after: u32,
) -> i32 {
    let read_state = |state: u32| state != 0 && state & !native_resources::GENERIC_READ == 0;
    let shader_state = |state: u32| {
        (state != native_resources::COPY_SOURCE && read_state(state))
            || state == native_resources::UNORDERED_ACCESS
    };
    if !matches!(subresource, 0 | u32::MAX)
        || before == after
        || (!matches!(
            before,
            0 | native_resources::COPY_SOURCE | native_resources::COPY_DEST
        ) && !shader_state(before))
        || (!matches!(
            after,
            0 | native_resources::COPY_SOURCE | native_resources::COPY_DEST
        ) && !shader_state(after))
    {
        return INVALID;
    }
    let acquired = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        let graphics = list.runtime_error != 0
            && ((list.kind == Kind::Direct
                && matches!(list.direct_mode, DirectMode::Draw | DirectMode::BufferCopy))
                || list.kind == Kind::Compute)
            && (shader_state(before) || shader_state(after) || !buffer_copy_allowed(list));
        if (!buffer_copy_allowed(list) && !graphics)
            || list.runtime_error == 0
            || list.phase != Phase::Recording
            || (!graphics && (shader_state(before) || shader_state(after)))
        {
            return Err(INVALID);
        }
        let slot = list.buffer_states.iter().position(|state| {
            state.as_ref().is_some_and(|state| {
                state.owner.resource == resource && state.owner.resource_cookie == resource_cookie
            })
        });
        let post_copy = list.dispatched && !graphics;
        let (slot, existing) = match slot {
            Some(slot) => {
                if list.dispatched
                    && !graphics
                    && after != list.buffer_states[slot].as_ref().unwrap().owner.initial
                    && list.buffer_states[slot].as_ref().unwrap().owner.initial != 0
                {
                    return Err(INVALID);
                }
                let current = list.buffer_states[slot].as_ref().unwrap().current;
                if current != before
                    && !(current == 0 && native_resources::promotable_buffer_state(before))
                {
                    return Err(INVALID);
                }
                (slot, true)
            }
            None => (
                list.buffer_states
                    .iter()
                    .position(Option::is_none)
                    .ok_or(NO_MEMORY)?,
                false,
            ),
        };
        list.operating = true;
        Ok((
            index,
            list.device,
            list.device_cookie,
            slot,
            existing,
            graphics,
            post_copy,
        ))
    });
    let (index, device, device_cookie, slot, existing, graphics, post_copy) = match acquired {
        Ok(v) => v,
        Err(s) => return s,
    };
    let owner = match native_resources::retain_buffer_state(
        device,
        device_cookie,
        resource,
        resource_cookie,
    ) {
        Ok(owner) => owner,
        Err(status) => {
            binding_failed(index);
            return status;
        }
    };
    if !existing
        && owner.initial != before
        && !(owner.initial == 0 && native_resources::promotable_buffer_state(before))
    {
        native_resources::release_buffer_state(owner);
        binding_failed(index);
        return INVALID;
    }
    if !existing && post_copy && owner.initial != 0 {
        native_resources::release_buffer_state(owner);
        binding_failed(index);
        return INVALID;
    }
    let mut candidate = Some(owner);
    let result = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        assert!(list.cookie == cookie && list.operating && list.phase == Phase::Recording);
        if existing {
            list.buffer_states[slot].as_mut().unwrap().current = after;
        } else {
            list.buffer_states[slot] = Some(BufferStateRecording {
                owner: candidate.take().unwrap(),
                current: after,
            });
        }
        if !graphics {
            select_buffer_copy(list);
        }
        list.operating = false;
        Ok(())
    });
    if let Some(owner) = candidate {
        native_resources::release_buffer_state(owner);
    }
    match result {
        Ok(()) => 0,
        Err(status) => {
            binding_failed(index);
            status
        }
    }
}

/// Genuine dynamic blend factors use exact by-value caller bits; null/default
/// normalization is in C. State commits use the same native fault/phase admission
/// as other dynamic graphics fields and move with exact recording generations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_direct_set_blend_constants(
    storage: usize,
    cookie: u64,
    constants: *const BlendConstants,
) -> i32 {
    if constants.is_null() {
        return INVALID;
    }
    let snapshot = unsafe { constants.read() };
    direct_state_commit(storage, cookie, |list| {
        list.blend_constants = snapshot;
        Ok(())
    })
}

// A paired whole-image transaction owns both original initialized generations.
// ResourceCopy consumes their states; both explicit closing barriers are required
// before Close. No unobserved intermediate layout persists across submissions.
fn image_copy_operands(
    list: &List,
) -> Option<(&ImageTransitionRecording, &ImageTransitionRecording)> {
    let a = list.image_transition.as_ref()?;
    let b = list.image_copy_peer.as_ref()?;
    if a.copy_state == native_resources::COPY_SOURCE && b.copy_state == native_resources::COPY_DEST
    {
        Some((a, b))
    } else if b.copy_state == native_resources::COPY_SOURCE
        && a.copy_state == native_resources::COPY_DEST
    {
        Some((b, a))
    } else {
        None
    }
}
fn whole_image_range(entry: &ImageTransitionRecording) -> bool {
    entry.layer
        == if entry.image.array_layers() == 1 {
            0
        } else {
            u32::MAX
        }
}
fn image_copy_closed(list: &List) -> bool {
    list.image_copy_recorded
        && image_copy_operands(list)
            .is_some_and(|(source, destination)| source.closed && destination.closed)
}
fn image_copy_transition(
    storage: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
    subresource: u32,
    before: u32,
    after: u32,
) -> i32 {
    let acquired = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &mut registry.lists[index];
        if list.operating {
            return Err(BUSY);
        }
        if list.kind != Kind::Direct
            || list.runtime_error == 0
            || list.phase != Phase::Recording
            || !matches!(
                list.direct_mode,
                DirectMode::ImageTransition | DirectMode::ImageUpload | DirectMode::ImageCopy
            )
        {
            return Err(INVALID);
        }
        let first = list.image_transition.as_ref().ok_or(INVALID)?;
        if first.upload.is_some() || first.readback.is_some() {
            return Err(INVALID);
        }
        let closing = after == native_images::IMAGE_STATE_RENDER_TARGET;
        let target = if closing {
            if !list.image_copy_recorded {
                return Err(INVALID);
            }
            let (slot, entry) = if first.resource == resource {
                (0, first)
            } else {
                (1, list.image_copy_peer.as_ref().ok_or(INVALID)?)
            };
            if entry.closed
                || entry.resource != resource
                || entry.cookie != resource_cookie
                || before != entry.copy_state
                || transition_layer(&entry.image, subresource) != Some(entry.layer)
            {
                return Err(INVALID);
            }
            slot
        } else {
            if list.image_copy_recorded
                || first.closed
                || list.image_copy_peer.is_some()
                || first.resource == resource
                || !whole_image_range(first)
                || before != native_images::IMAGE_STATE_RENDER_TARGET
                || !matches!(
                    after,
                    native_resources::COPY_SOURCE | native_resources::COPY_DEST
                )
                || after == first.copy_state
            {
                return Err(INVALID);
            }
            1
        };
        list.operating = true;
        Ok((index, list.device, list.device_cookie, closing, target))
    });
    let (index, device, device_cookie, closing, target) = match acquired {
        Ok(v) => v,
        Err(status) => return status,
    };
    let mut candidate = if closing {
        None
    } else {
        match native_images::retain_initialized_image(
            device,
            device_cookie,
            resource,
            resource_cookie,
        ) {
            Ok(image) => Some(image),
            Err(status) => {
                binding_failed(index);
                return status;
            }
        }
    };
    let layer = if let Some(image) = candidate.as_ref() {
        let layer = transition_layer(image, subresource);
        if native_images::supported_depth_format(image.format())
            || layer
                != Some(if image.array_layers() == 1 {
                    0
                } else {
                    u32::MAX
                })
        {
            native_images::release_initialized_image(candidate.take().unwrap());
            binding_failed(index);
            return INVALID;
        }
        layer.unwrap()
    } else {
        0
    };
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        if closing {
            if target == 0 {
                list.image_transition.as_mut().unwrap().closed = true;
            } else {
                list.image_copy_peer.as_mut().unwrap().closed = true;
            }
        } else {
            list.image_copy_peer = Some(ImageTransitionRecording {
                copy_state: after,
                image: candidate.take().unwrap(),
                resource,
                cookie: resource_cookie,
                layer,
                closed: false,
                readback: None,
                upload: None,
            });
            list.direct_mode = DirectMode::ImageCopy;
        }
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(status) => {
            if let Some(image) = candidate {
                native_images::release_initialized_image(image);
            }
            binding_failed(index);
            status
        }
    }
}
fn record_image_copy(
    storage: usize,
    cookie: u64,
    source: usize,
    source_cookie: u64,
    destination: usize,
    destination_cookie: u64,
) -> i32 {
    let owner = with_registry(|registry| {
        let index = registry.list(storage, cookie).ok_or(INVALID)?;
        let list = &registry.lists[index];
        Ok((index, list.device, list.device_cookie))
    });
    let (index, device, device_cookie) = match owner {
        Ok(v) => v,
        Err(status) => return status,
    };
    native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        if list.cookie != cookie
            || list.operating
            || list.phase != Phase::Recording
            || list.direct_mode != DirectMode::ImageCopy
            || list.image_copy_recorded
        {
            return Err(INVALID);
        }
        let (src, dst) = image_copy_operands(list).ok_or(INVALID)?;
        if src.closed
            || dst.closed
            || src.resource != source
            || src.cookie != source_cookie
            || dst.resource != destination
            || dst.cookie != destination_cookie
            || !whole_image_range(src)
            || !whole_image_range(dst)
            || src.image.adapter() != dst.image.adapter()
            || src.image.format() != dst.image.format()
            || src.image.resource_type() != dst.image.resource_type()
            || src.image.extent() != dst.image.extent()
            || src.image.depth() != dst.image.depth()
            || src.image.array_layers() != dst.image.array_layers()
            || src.image.sample_count() != dst.image.sample_count()
        {
            return Err(INVALID);
        }
        list.image_copy_recorded = true;
        Ok(())
    })
    .map_or_else(|status| status, |()| 0)
}

fn index_stride(format: u32) -> Option<u64> {
    match format {
        57 => Some(2),
        42 => Some(4),
        _ => None,
    }
}
fn indexed_work(list: &List) -> Result<Option<IndexedWork>, i32> {
    let Some(args) = list.indexed_arguments else {
        return Ok(None);
    };
    let binding = list.index_binding.as_ref().ok_or(INVALID)?;
    Ok(Some(IndexedWork {
        buffer_token: binding.range.token,
        offset: binding.range.offset,
        bytes: binding.range.bytes,
        format: binding.format,
        count: args.count,
        instances: args.instances,
        first_index: args.first_index,
        base_vertex: args.base_vertex,
        first_instance: args.first_instance,
        ..IndexedWork::default()
    }))
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_direct_set_index_buffer(
    storage: usize,
    cookie: u64,
    address: u64,
    bytes: u32,
    format: u32,
    unbind: u32,
) -> i32 {
    if unbind > 1 || (unbind == 0 && index_stride(format).is_none()) {
        return INVALID;
    }
    let (index, device, device_cookie) = match binding_begin(storage, cookie, Kind::Direct) {
        Ok(v) => v,
        Err(status) => return status,
    };
    let genuine = with_registry(|registry| {
        let list = &registry.lists[index];
        list.runtime_error != 0 && list.direct_mode == DirectMode::Draw
    });
    if !genuine {
        binding_failed(index);
        return NO_INTERFACE;
    }
    let mut candidate = if unbind == 1 {
        None
    } else {
        let range = match native_resources::retain_index_range(
            device,
            device_cookie,
            address,
            u64::from(bytes),
        ) {
            Ok(lease) => lease,
            Err(status) => {
                binding_failed(index);
                return status;
            }
        };
        if range.offset % index_stride(format).unwrap() != 0 {
            native_resources::release_index_range(range);
            binding_failed(index);
            return INVALID;
        }
        Some(IndexBinding { range, format })
    };
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        let previous = core::mem::replace(&mut list.index_binding, candidate.take());
        list.operating = false;
        Ok(previous)
    });
    match committed {
        Ok(previous) => {
            if let Some(binding) = previous {
                native_resources::release_index_range(binding.range);
            }
            0
        }
        Err(status) => {
            if let Some(binding) = candidate {
                native_resources::release_index_range(binding.range);
            }
            binding_failed(index);
            status
        }
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_draw_indexed_record(
    storage: usize,
    cookie: u64,
    count: u32,
    instances: u32,
    first_index: u32,
    base_vertex: i32,
    first_instance: u32,
) -> i32 {
    if genuine_draw_list(storage, cookie) {
        return record_graphics_command(
            storage,
            cookie,
            DrawArguments::default(),
            Some(IndexedArguments {
                count,
                instances,
                first_index,
                base_vertex,
                first_instance,
            }),
            None,
        );
    }
    direct_state_commit(storage, cookie, |list| {
        if list.runtime_error == 0 || !graphics_ready(list) {
            return Err(INVALID);
        }
        let binding = list.index_binding.as_ref().ok_or(INVALID)?;
        let range = &binding.range;
        let state =
            buffer_state_for(list, range.resource, range.resource_cookie).unwrap_or(range.initial);
        if !native_resources::buffer_state_allows(range.kind, state, native_resources::INDEX_BUFFER)
            || u64::from(first_index)
                .checked_add(u64::from(count))
                .and_then(|end| end.checked_mul(index_stride(binding.format)?))
                .is_none_or(|bytes| bytes > range.bytes)
        {
            return Err(INVALID);
        }
        list.indexed_arguments = Some(IndexedArguments {
            count,
            instances,
            first_index,
            base_vertex,
            first_instance,
        });
        list.dispatched = true;
        Ok(())
    })
}

/// Snapshot the official IA view array only during this callback. Build every
/// candidate outside the lock before replacing any current slot generation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_direct_set_vertex_buffers(
    storage: usize,
    cookie: u64,
    start: u32,
    count: u32,
    views: *const VertexViewInput,
) -> i32 {
    if start > 32 || count > 32 - start {
        return INVALID;
    }
    let (index, device, device_cookie) = match binding_begin(storage, cookie, Kind::Direct) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let genuine = with_registry(|r| {
        r.lists[index].runtime_error != 0 && r.lists[index].direct_mode == DirectMode::Draw
    });
    if !genuine {
        binding_failed(index);
        return NO_INTERFACE;
    }
    let mut candidate = VertexBindings::new();
    let retained = (|| -> Result<(), i32> {
        for slot in 0..count {
            if views.is_null() {
                continue;
            }
            let view = unsafe { views.add(slot as usize).read() };
            if view.address == 0 && view.bytes == 0 {
                continue;
            }
            if view.stride > 2048 {
                return Err(INVALID);
            }
            candidate.append(retain_vertex_binding(
                device,
                device_cookie,
                start + slot,
                view,
            )?);
        }
        Ok(())
    })();
    if let Err(status) = retained {
        release_vertex_bindings(candidate);
        binding_failed(index);
        return status;
    }
    let mut candidate = Some(candidate);
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        let old = core::mem::take(&mut list.vertex_bindings);
        let mut next = old.head;
        let mut removed = VertexBindings::new();
        for _ in 0..old.count {
            let node = next;
            let entry = unsafe { &*(node as *const VertexBindingNode) };
            next = entry.next;
            let slot = entry.work.slot;
            if slot >= start && slot < start + count {
                removed.append(node);
            } else {
                list.vertex_bindings.append(node);
            }
        }
        assert!(next == 0);
        let owned = candidate.take().unwrap();
        next = owned.head;
        for _ in 0..owned.count {
            let node = next;
            next = unsafe { (*(node as *const VertexBindingNode)).next };
            list.vertex_bindings.append(node);
        }
        assert!(next == 0);
        list.operating = false;
        Ok(removed)
    });
    match committed {
        Ok(old) => {
            release_vertex_bindings(old);
            0
        }
        Err(status) => {
            release_vertex_bindings(candidate.unwrap());
            binding_failed(index);
            status
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_set_descriptor_heap(
    storage: usize,
    cookie: u64,
    heap: usize,
    heap_cookie: u64,
) -> i32 {
    if (heap == 0) != (heap_cookie == 0) {
        return INVALID;
    }
    let kind = match with_registry(|registry| {
        registry
            .list(storage, cookie)
            .map(|i| registry.lists[i].kind)
    }) {
        Some(kind @ (Kind::Direct | Kind::Compute)) => kind,
        _ => return INVALID,
    };
    let (index, device, device_cookie) = match binding_begin(storage, cookie, kind) {
        Ok(v) => v,
        Err(e) => return e,
    };
    if with_registry(|registry| registry.lists[index].runtime_error == 0) {
        binding_failed(index);
        return NO_INTERFACE;
    }
    let mut candidate = if heap == 0 {
        None
    } else {
        match native_descriptors::retain_shader_heap(device, device_cookie, heap, heap_cookie) {
            Ok(v) => Some(v),
            Err(e) => {
                binding_failed(index);
                return e;
            }
        }
    };
    let result = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        let changed = match (&list.shader_heap, &candidate) {
            (Some(a), Some(b)) => !a.same_owner(b),
            (None, None) => false,
            _ => true,
        };
        if changed {
            list.table_handles = [0; 64];
        }
        let old = core::mem::replace(&mut list.shader_heap, candidate.take());
        list.operating = false;
        Ok(old)
    });
    match result {
        Ok(old) => {
            if let Some(v) = old {
                native_descriptors::release_shader_heap(v);
            }
            0
        }
        Err(e) => {
            if let Some(v) = candidate {
                native_descriptors::release_shader_heap(v);
            }
            binding_failed(index);
            e
        }
    }
}

fn retain_table_binding(
    identity: native_descriptors::ShaderHeapIdentity,
    parameter: u32,
    binding: u32,
    handle: u64,
    offset: u32,
    required_bytes: u32,
    states: [Option<BufferStatePoint>; BUFFER_STATE_LIMIT],
) -> Result<usize, i32> {
    let heap = native_descriptors::retain_shader_heap_identity(identity)?;
    let memory =
        unsafe { ql_dx12_native_recording_allocate(core::mem::size_of::<CbvBindingNode>()) }
            as *mut CbvBindingNode;
    if memory.is_null() {
        native_descriptors::release_shader_heap(heap);
        return Err(NO_MEMORY);
    }
    unsafe {
        memory.write(CbvBindingNode {
            next: 0,
            work: CbvWork {
                parameter,
                word_offset: binding,
                required_bytes,
                kind: 5,
                ..CbvWork::default()
            },
            range: None,
            slot: None,
            table: Some(TableKey {
                heap,
                handle,
                offset,
                states,
            }),
        });
    }
    Ok(memory as usize)
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_root_descriptor_table(
    storage: usize,
    cookie: u64,
    parameter: u32,
    handle: u64,
    graphics: u8,
) -> i32 {
    let kind = match graphics {
        0 => Kind::Compute,
        1 => Kind::Direct,
        _ => return INVALID,
    };
    let (index, device, device_cookie) = match binding_begin(storage, cookie, kind) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let result = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let list = &mut registry.lists[index];
        if list.runtime_error == 0 || (kind == Kind::Direct && list.direct_mode != DirectMode::Draw)
        {
            return Err(NO_INTERFACE);
        }
        let plan = list.root.as_ref().ok_or(INVALID)?.plan();
        if parameter >= plan.count || plan.parameters[parameter as usize].kind != 5 {
            return Err(INVALID);
        }
        let heap = list.shader_heap.as_ref().ok_or(INVALID)?;
        if !unsafe { heap.contains_locked(handle, plan.parameters[parameter as usize].words) } {
            return Err(INVALID);
        }
        list.table_handles[parameter as usize] = handle;
        list.operating = false;
        Ok(())
    });
    match result {
        Ok(()) => 0,
        Err(e) => {
            binding_failed(index);
            e
        }
    }
}

fn hydrate_table_chain(device: usize, cookie: u64, bindings: &mut CbvBindings) -> Result<(), i32> {
    let mut next = bindings.head;
    for _ in 0..bindings.count {
        let node = unsafe { &mut *(next as *mut CbvBindingNode) };
        next = node.next;
        if node.work.kind != 5 {
            continue;
        }
        assert!(
            node.slot.is_none() && node.range.is_none(),
            "table contents are acquired afresh for each submission"
        );
        let key = node
            .table
            .as_ref()
            .expect("recorded table owns heap identity");
        let slot = native_descriptors::retain_shader_slot(&key.heap, key.handle, key.offset, 2)?;
        let shape = slot.shape;
        let range = if shape.resource == 0 {
            None
        } else {
            let range = match native_resources::retain_buffer_range(
                device,
                cookie,
                shape.address,
                shape.bytes,
            ) {
                Ok(v) => v,
                Err(e) => {
                    native_descriptors::release_shader_slot(slot);
                    return Err(e);
                }
            };
            // Descriptors may legally change after Close. Resolve current
            // contents now, but check the exact resource's state at this
            // particular consumed command, never a later transition or another
            // resource generation with the same caller storage address.
            let state = key
                .states
                .iter()
                .flatten()
                .find(|point| {
                    point.resource == range.resource && point.cookie == range.resource_cookie
                })
                .map_or(range.initial, |point| point.state);
            if range.resource != shape.resource
                || range.resource_cookie != shape.resource_cookie
                || !native_resources::buffer_state_allows(
                    range.kind,
                    state,
                    native_resources::VERTEX_BUFFER,
                )
            {
                native_resources::release_buffer_range(range);
                native_descriptors::release_shader_slot(slot);
                return Err(NO_INTERFACE);
            }
            Some(range)
        };
        node.work.buffer_token = range.as_ref().map_or(0, |v| v.token);
        node.work.offset = range.as_ref().map_or(0, |v| v.offset);
        node.work.bytes = range.as_ref().map_or(0, |v| v.bytes);
        node.range = range;
        node.slot = Some(slot);
    }
    assert!(next == 0);
    Ok(())
}
fn clear_table_chain(bindings: &mut CbvBindings) {
    let mut next = bindings.head;
    for _ in 0..bindings.count {
        let node = unsafe { &mut *(next as *mut CbvBindingNode) };
        next = node.next;
        if node.work.kind != 5 {
            continue;
        }
        if let Some(range) = node.range.take() {
            native_resources::release_buffer_range(range);
        }
        if let Some(slot) = node.slot.take() {
            native_descriptors::release_shader_slot(slot);
        }
        node.work.buffer_token = 0;
        node.work.offset = 0;
        node.work.bytes = 0;
    }
    assert!(next == 0);
}
fn recording_table_chains(
    owners: &mut RecordingOwners,
    mut operation: impl FnMut(&mut CbvBindings) -> Result<(), i32>,
) -> Result<(), i32> {
    let mut next = owners.graphics_stream.head;
    for _ in 0..owners.graphics_stream.count {
        let node = unsafe { &mut *(next as *mut GraphicsRecording) };
        next = node.next;
        operation(&mut node.cbvs)?;
    }
    assert!(next == 0);
    for command in owners.compute_stream.iter_mut().flatten() {
        operation(&mut command.cbvs)?;
    }
    Ok(())
}
/// Queue Operating owns this pending payload exclusively. Concurrent origin
/// reset may detach its generation, but cannot take or mutate pending owners.
fn hydrate_pending_tables(index: usize) -> Result<(), i32> {
    let (device, cookie, pointer) = with_registry(|registry| {
        let queue = &mut registry.queues[index];
        assert!(queue.operating);
        let pending = queue
            .pending
            .as_mut()
            .expect("submission owns pending generation");
        (
            queue.device,
            queue.device_cookie,
            &mut pending.owners as *mut RecordingOwners as usize,
        )
    });
    let owners = unsafe { &mut *(pointer as *mut RecordingOwners) };
    let result = recording_table_chains(owners, |chain| hydrate_table_chain(device, cookie, chain));
    if result.is_err() {
        recording_table_chains(owners, |chain| {
            clear_table_chain(chain);
            Ok(())
        })
        .expect("cleanup cannot fail");
    }
    result
}
fn clear_finished_tables(index: usize, result: TransferResult, status: i32, attempted_call: bool) {
    if !valid_result(result) {
        return;
    }
    let pointer = with_registry(|registry| {
        let queue = &mut registry.queues[index];
        assert!(queue.operating);
        let pending = queue.pending.as_mut()?;
        let (accepted, value) = if attempted_call && result.attempted != 0 {
            (result.accepted == 1, result.submitted)
        } else {
            (pending.accepted, pending.value)
        };
        let complete = result.drain_complete != 0
            || (status >= 0 && accepted && value != 0 && result.completed >= value);
        (complete || (attempted_call && result.attempted == 0))
            .then_some(&mut pending.owners as *mut RecordingOwners as usize)
    });
    if let Some(pointer) = pointer {
        let owners = unsafe { &mut *(pointer as *mut RecordingOwners) };
        recording_table_chains(owners, |chain| {
            clear_table_chain(chain);
            Ok(())
        })
        .expect("cleanup cannot fail");
    }
}

/// Prove same-recording availability in linear time. Heap count alone cannot
/// prove that a result was initialized; an intervening begin invalidates it.
/// Operating pins the immutable chain while temporary proof storage is used.
fn query_range_initialized(
    head: usize,
    nodes: u32,
    owner: usize,
    first: u32,
    count: u32,
) -> Result<(), i32> {
    if count == 0 {
        return Ok(());
    }
    if count > nodes {
        return Err(INVALID);
    }
    let memory = unsafe { ql_dx12_native_recording_allocate(count as usize) } as *mut u8;
    if memory.is_null() {
        return Err(NO_MEMORY);
    }
    unsafe {
        core::ptr::write_bytes(memory, 0, count as usize);
    }
    let checked = (|| {
        let mut cursor = head;
        for _ in 0..nodes {
            if cursor == 0 {
                return Err(INVALID);
            }
            let node = unsafe { &*(cursor as *const GraphicsRecording) };
            cursor = node.next;
            let q = node.work.query;
            if q.owner as usize != owner || q.index < first || q.index - first >= count {
                continue;
            }
            match node.work.kind {
                QUERY_BEGIN => unsafe { memory.add((q.index - first) as usize).write(0) },
                QUERY_END | QUERY_TIMESTAMP => unsafe {
                    memory.add((q.index - first) as usize).write(1)
                },
                _ => {}
            }
        }
        if cursor != 0 {
            return Err(INVALID);
        }
        for i in 0..count as usize {
            if unsafe { memory.add(i).read() } == 0 {
                return Err(INVALID);
            }
        }
        Ok(())
    })();
    unsafe { ql_dx12_native_recording_free(memory.cast()) };
    checked
}
/// Genuine DIRECT query events use the same unique growable history as draws.
/// The C shim owns official types; every supplied key is checked against the
/// exact live device generation before immutable command ownership can commit.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_query_record(
    storage: usize,
    cookie: u64,
    heap: usize,
    heap_cookie: u64,
    operation: u32,
    data_kind: u32,
    index: u32,
    count: u32,
    destination: usize,
    destination_cookie: u64,
    offset: u64,
) -> i32 {
    let (slot, device, device_cookie) = match binding_begin(storage, cookie, Kind::Direct) {
        Ok(v) => v,
        Err(s) => return s,
    };
    let snapshot = with_registry(|registry| {
        let list = &registry.lists[slot];
        if list.runtime_error == 0 {
            return Err(NO_INTERFACE);
        }
        if list.graphics_stream.count == u32::MAX {
            return Err(NO_MEMORY);
        }
        Ok((
            list.graphics_stream.head,
            list.graphics_stream.count,
            list.active_queries,
            buffer_state_for(list, destination, destination_cookie),
        ))
    });
    let (head, nodes, active, destination_state) = match snapshot {
        Ok(v) => v,
        Err(s) => {
            binding_failed(slot);
            return s;
        }
    };
    let query = match crate::native_queries::retain_query(device, device_cookie, heap, heap_cookie)
    {
        Ok(v) => v,
        Err(s) => {
            binding_failed(slot);
            return s;
        }
    };
    let mut candidate = GraphicsRecording {
        next: 0,
        pipeline: None,
        root: None,
        target: None,
        extra_targets: [const { None }; 7],
        depth: None,
        index: None,
        vertices: VertexBindings::new(),
        cbvs: CbvBindings::new(),
        query: Some(query),
        destination_write: None,
        indirect: None,
        work: GraphicsWork::default(),
    };
    let prepared = (|| -> Result<([Option<QueryKey>; 2], bool), i32> {
        let q = candidate.query.as_ref().unwrap();
        let expected = match data_kind {
            0 => 0,
            2 => 1,
            3 => 2,
            _ => return Err(NO_INTERFACE),
        };
        if q.kind != expected
            || index > q.count
            || count > q.count - index
            || !matches!(operation, QUERY_BEGIN | QUERY_END | QUERY_RESOLVE)
        {
            return Err(INVALID);
        }
        if operation != QUERY_RESOLVE
            && (count != 1 || destination != 0 || destination_cookie != 0 || offset != 0)
        {
            return Err(INVALID);
        }
        let key = QueryKey {
            owner: q.owner,
            index,
            kind: q.kind,
        };
        let mut next_active = active;
        let kind = match operation {
            QUERY_BEGIN => {
                if q.kind == 1 {
                    return Err(INVALID);
                }
                let slot = if q.kind == 0 { 0 } else { 1 };
                if next_active[slot].is_some() {
                    return Err(INVALID);
                }
                next_active[slot] = Some(key);
                QUERY_BEGIN
            }
            QUERY_END if q.kind == 1 => QUERY_TIMESTAMP,
            QUERY_END => {
                let slot = if q.kind == 0 { 0 } else { 1 };
                if next_active[slot] != Some(key) {
                    return Err(INVALID);
                }
                next_active[slot] = None;
                QUERY_END
            }
            QUERY_RESOLVE => {
                query_range_initialized(head, nodes, q.owner, index, count)?;
                QUERY_RESOLVE
            }
            _ => return Err(INVALID),
        };
        candidate.work.kind = kind;
        candidate.work.query = QueryWork {
            owner: q.owner as u64,
            heap_kind: q.kind,
            heap_count: q.count,
            index,
            count,
            data_kind,
            ..QueryWork::default()
        };
        if kind == QUERY_RESOLVE {
            let bytes = u64::from(count)
                .checked_mul(if data_kind == 3 { 88 } else { 8 })
                .ok_or(INVALID)?;
            let writer = native_resources::retain_query_write(
                device,
                device_cookie,
                destination,
                destination_cookie,
                offset,
                bytes,
                destination_state,
            )?;
            if writer.adapter != q.adapter {
                native_resources::release_buffer_write(writer);
                return Err(INVALID);
            }
            candidate.work.query.buffer = writer.token;
            candidate.work.query.offset = offset;
            candidate.work.query.bytes = bytes;
            candidate.destination_write = Some(writer);
        }
        Ok((next_active, kind == QUERY_RESOLVE && count == 0))
    })();
    let (next_active, no_work) = match prepared {
        Ok(v) => v,
        Err(s) => {
            release_graphics_recording(candidate);
            binding_failed(slot);
            return s;
        }
    };
    if no_work {
        release_graphics_recording(candidate);
        let admitted = native_objects::with_device_admission(device, device_cookie, || {
            let list = unsafe { &mut *REGISTRY.0.get() }
                .lists
                .get_mut(slot)
                .unwrap();
            assert!(list.operating && list.cookie == cookie && list.phase == Phase::Recording);
            list.operating = false;
            Ok(())
        });
        return admitted.map_or_else(
            |s| {
                binding_failed(slot);
                s
            },
            |()| 0,
        );
    }
    let memory =
        unsafe { ql_dx12_native_recording_allocate(core::mem::size_of::<GraphicsRecording>()) }
            as *mut GraphicsRecording;
    if memory.is_null() {
        release_graphics_recording(candidate);
        binding_failed(slot);
        return NO_MEMORY;
    }
    unsafe {
        memory.write(candidate);
    }
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let list = &mut unsafe { &mut *REGISTRY.0.get() }.lists[slot];
        assert!(list.operating && list.cookie == cookie && list.phase == Phase::Recording);
        list.graphics_stream.append(memory as usize);
        list.active_queries = next_active;
        list.direct_mode = DirectMode::Draw;
        list.dispatched = true;
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(s) => {
            release_graphics_recording(unsafe { memory.read() });
            unsafe { ql_dx12_native_recording_free(memory.cast()) };
            binding_failed(slot);
            s
        }
    }
}

/// Genuine original R0 ExecuteIndirect records opaque GPU argument reads. The
/// graphics path is rootless indexed/non-indexed graphics with no count buffer.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_execute_indirect_record(
    storage: usize,
    cookie: u64,
    signature: usize,
    signature_cookie: u64,
    count: u32,
    buffer: usize,
    buffer_cookie: u64,
    offset: u64,
    count_buffer: usize,
    count_cookie: u64,
    count_offset: u64,
) -> i32 {
    let request=IndirectRequest{signature,signature_cookie,buffer,buffer_cookie,offset,count,count_buffer,count_cookie,count_offset};
    let compute=with_registry(|r|r.list(storage,cookie).is_some_and(|i|r.lists[i].kind==Kind::Compute));
    if compute{record_compute_dispatch(storage,cookie,0,0,0,Some(request))}
    else{record_graphics_command(storage,cookie,DrawArguments::default(),None,Some(request))}
}

/// Raw UAV clears are fixed-function fills. Snapshot the non-shader-visible CPU
/// descriptor, then retain only the independently owned destination interval.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_clear_raw_uav(
    storage: usize,
    cookie: u64,
    gpu: u64,
    cpu: usize,
    resource: usize,
    resource_cookie: u64,
    value: u32,
) -> i32 {
    let kind = match with_registry(|r| {
        let i = r.list(storage, cookie).ok_or(INVALID)?;
        match r.lists[i].kind {
            Kind::Direct => Ok(Kind::Direct),
            Kind::Compute => Ok(Kind::Compute),
            _ => Err(NO_INTERFACE),
        }
    }) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let (slot, device, device_cookie) = match binding_begin(storage, cookie, kind) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let snapshot = with_registry(|r| {
        let list = &r.lists[slot];
        if list.runtime_error == 0
            || (kind == Kind::Direct && list.direct_mode != DirectMode::Draw)
            || (kind == Kind::Compute && list.compute_stream.iter().any(Option::is_some))
        {
            return Err(NO_INTERFACE);
        }
        if list.graphics_stream.count == u32::MAX {
            return Err(NO_MEMORY);
        }
        Ok((
            list.shader_heap.as_ref().ok_or(INVALID)?.identity(),
            buffer_state_for(list, resource, resource_cookie),
        ))
    });
    let (heap, state) = match snapshot {
        Ok(v) => v,
        Err(e) => {
            binding_failed(slot);
            return e;
        }
    };
    let writer = (|| -> Result<native_resources::BufferWriteLease, i32> {
        let heap = native_descriptors::retain_shader_heap_identity(heap)?;
        let view =
            native_descriptors::retain_raw_clear_view(&heap, gpu, cpu, resource, resource_cookie);
        native_descriptors::release_shader_heap(heap);
        let view = view?;
        let writer = native_resources::retain_uav_clear_write(
            device,
            device_cookie,
            resource,
            resource_cookie,
            view.shape.offset,
            view.shape.bytes,
            state,
        );
        native_descriptors::release_shader_slot(view);
        writer
    })();
    let writer = match writer {
        Ok(v) => v,
        Err(e) => {
            binding_failed(slot);
            return e;
        }
    };
    let work = GraphicsWork {
        kind: BUFFER_UAV_CLEAR,
        fill: BufferFillWork {
            buffer: writer.token,
            offset: writer.offset,
            bytes: writer.bytes,
            value,
            reserved: 0,
        },
        ..GraphicsWork::default()
    };
    let candidate = GraphicsRecording {
        next: 0,
        pipeline: None,
        root: None,
        target: None,
        extra_targets: [const { None }; 7],
        depth: None,
        index: None,
        work,
        query: None,
        destination_write: Some(writer),
        indirect: None,
        vertices: VertexBindings::new(),
        cbvs: CbvBindings::new(),
    };
    let memory =
        unsafe { ql_dx12_native_recording_allocate(core::mem::size_of::<GraphicsRecording>()) }
            as *mut GraphicsRecording;
    if memory.is_null() {
        release_graphics_recording(candidate);
        binding_failed(slot);
        return NO_MEMORY;
    }
    unsafe {
        memory.write(candidate);
    }
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let list = &mut unsafe { &mut *REGISTRY.0.get() }.lists[slot];
        assert!(list.operating && list.cookie == cookie && list.phase == Phase::Recording);
        list.graphics_stream.append(memory as usize);
        list.dispatched = true;
        list.operating = false;
        Ok(())
    });
    match committed {
        Ok(()) => 0,
        Err(e) => {
            release_graphics_recording(unsafe { memory.read() });
            unsafe {
                ql_dx12_native_recording_free(memory.cast());
            }
            binding_failed(slot);
            e
        }
    }
}

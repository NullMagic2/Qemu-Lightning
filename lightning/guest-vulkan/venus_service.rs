//! Module: lightning.guest_vulkan.venus_service
//! Purpose: Own bounded Venus contexts, exported blobs and real completion syncs.
//! Created: 2026-10-09
//! Architecture: The Windows transport uses logical IDs only. This host service
//! validates ownership before calling the maintained renderer, retires guest
//! syncs from fences the backend observed, and bounds every blocking wait.
//! QEMU must map exported file descriptors into the guest aperture separately.
pub mod aperture;
pub mod sync_publish;
pub mod sync_watch;
pub mod transport_contract;
pub mod venus_backend;
pub mod wire_service;
use std::{
    collections::BTreeMap,
    ffi::c_void,
    os::fd::{FromRawFd, OwnedFd},
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc, Mutex, MutexGuard,
    },
    time::{Duration, Instant},
};
use sync_publish::Publisher;
use sync_watch::{FencePoints, Watch, WatchPoint, Watches};
use transport_contract::{
    SyncPoint, APERTURE_BYTES, COMMAND_WORD_BYTES, MAX_CAPSET_BYTES, MAX_PAYLOAD_BYTES,
    MAX_SUBMIT_SYNCS, MAX_SYNCS, MAX_TIMELINES, MAX_WATCHES, PAGE_BYTES, RING_TIMELINE_SLOTS,
    WAIT_RETRY_MICROSECONDS,
};
use venus_backend::{Backend, Observer};
const MAX_CONTEXTS: usize = 64;
const MAX_RESOURCES: usize = 4096;
// The aperture bounds simultaneously mapped CPU views, not all GPU memory.
// Keep a separate per-VM allocation budget, including unmapped shared images.
// The default; --blob-budget-mib sets another (stage-3 D7).
pub const BLOB_BUDGET_BYTES: u64 = 4096 * 1024 * 1024;

fn blob_capacity(allocated_bytes: u64, resources: usize, bytes: u64, budget: u64) -> bool {
    resources < MAX_RESOURCES
        && allocated_bytes
            .checked_add(bytes)
            .is_some_and(|total| total <= budget)
}

/// Per-VM sizes chosen at launch (the manager passes QEMU the same aperture).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HostOptions {
    pub aperture_bytes: u64,
    pub blob_budget_bytes: u64,
}
impl Default for HostOptions {
    fn default() -> Self {
        Self {
            aperture_bytes: APERTURE_BYTES,
            blob_budget_bytes: BLOB_BUDGET_BYTES,
        }
    }
}
const MAX_PENDING_FENCES: usize = 4096;
const CPU_TIMELINE: u32 = 0;
const IMPORT_ATTACH_TIMEOUT: Duration = Duration::from_secs(1);
const MAX_BLOB_REFERENCES: u32 = 4096;
const HANDLE_GENERATION_SHIFT: u32 = 32;
const MAX_NATIVE_ID: u32 = i32::MAX as u32;
const CONTEXT_NAME: &[u8] = b"Lightning native Windows Vulkan";
// Actual Vulkan memory property flags reported by the maintained renderer for
// the selected allocation type, never a guest assertion.
const HOST_VISIBLE: u32 = 1 << 1;
const HOST_COHERENT: u32 = 1 << 2;
const VISIBLE_COHERENT: u32 = HOST_VISIBLE | HOST_COHERENT;
unsafe extern "C" {
    fn qlv_venus_memory_properties(resource: u32, properties: *mut u32) -> i32;
    fn qlv_venus_cache_mode(resource: u32, mode: *mut u32) -> i32;
    fn qlv_venus_context_create(identity: u32, name: *const u8, name_bytes: u32) -> i32;
    fn qlv_venus_context_destroy(identity: u32);
    fn qlv_venus_memory_create(
        context: u32,
        resource: u32,
        memory: u64,
        bytes: u64,
        mappable: bool,
        shareable: bool,
    ) -> i32;
    fn qlv_venus_blob_destroy(resource: u32);
    fn qlv_venus_blob_attach(context: u32, resource: u32);
    fn qlv_venus_blob_detach(context: u32, resource: u32);
    fn qlv_venus_blob_export(resource: u32, kind: *mut u32, fd: *mut i32) -> i32;
    fn qlv_venus_fd_is_shared(kind: u32) -> bool;
    fn qlv_venus_fd_is_dmabuf(kind: u32) -> bool;
    fn qlv_venus_submit(context: u32, commands: *mut c_void, words: u32) -> i32;
    fn qlv_venus_fence(context: u32, ring: u32, identity: u64) -> i32;
    fn qlv_venus_capset(version: *mut u32, bytes: *mut u32);
    fn qlv_venus_caps(version: u32, output: *mut c_void, bytes: u32) -> i32;
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ExportKind {
    SharedMemory,
    DmaBuf,
}
pub struct ExportedBlob {
    pub descriptor: OwnedFd,
    pub kind: ExportKind,
    pub cache_mode: u32,
    pub bytes: u64,
}
struct Blob {
    /// One renderer attachment per context, with one reference per guest BO.
    holders: BTreeMap<u64, u32>,
    shareable: bool,
    mappable: bool,
    bytes: u64,
    memory_id: u64,
}
impl Blob {
    /// Return true only for the first renderer attachment in this context.
    fn retain(&mut self, context: u64) -> Result<bool, String> {
        let references = self.holders.entry(context).or_insert(0);

        if *references >= MAX_BLOB_REFERENCES {
            return Err("Shared blob reference quota exceeded".into());
        }
        *references += 1;
        Ok(*references == 1)
    }
    /// Return true when the renderer attachment can be removed. Its zero count
    /// remains until detach completes; context teardown removes all references.
    fn release(&mut self, context: u64) -> Result<bool, String> {
        let references = self
            .holders
            .get_mut(&context)
            .ok_or("Unknown blob holder")?;

        if *references == 0 {
            return Err("Unbalanced shared blob release".into());
        }
        *references -= 1;
        Ok(*references == 0)
    }
}
struct PendingPoint {
    value: u64,
    counter: Arc<AtomicU64>,
}
struct PendingFence {
    ring: u32,
    fence: u64,
    points: Vec<PendingPoint>,
}
fn retire_completed_fences(
    pending: &mut Vec<PendingFence>,
    completed: impl Fn(u32) -> u64,
) -> Vec<(u32, u64)> {
    let mut retired = Vec::new();
    pending.retain(|fence| {
        if completed(fence.ring) < fence.fence {
            return true;
        }
        for point in &fence.points {
            point.counter.fetch_max(point.value, Ordering::Release);
        }
        retired.push((fence.ring, fence.fence));
        false
    });
    retired
}

struct Context {
    syncs: BTreeMap<u64, Arc<AtomicU64>>,
    pending: Vec<PendingFence>,
    next_fence: u64,
    lost: bool,
}
pub struct HostService {
    renderer: Backend,
    contexts: BTreeMap<u64, Context>,
    blobs: BTreeMap<u64, Blob>,
    next_handle: u32,
    generation: u64,
    allocated_bytes: u64,
    blob_budget_bytes: u64,
    profile_blobs: bool,
    created_blobs: u64,
    destroyed_blobs: u64,
    rejected_blobs: u64,
    /// Armed sync watches, shared with the connection's notifier thread.
    watches: Arc<Mutex<Watches>>,
    /// Timeline-page slots of SYNC_PUBLISH syncs, shared with the notifier.
    publisher: Arc<Mutex<Publisher>>,
}
/// What the notifier thread needs: the watch table and the fence observer
/// (both outlive the service's renderer use; see Backend::observer).
#[derive(Clone)]
pub struct SyncEvents {
    pub watches: Arc<Mutex<Watches>>,
    pub observer: Arc<Observer>,
    pub publisher: Arc<Mutex<Publisher>>,
}
impl SyncEvents {
    pub fn lock(&self) -> MutexGuard<'_, Watches> {
        self.watches.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
    /// Hold this while changing a published counter or emitting its frames.
    pub fn publisher(&self) -> MutexGuard<'_, Publisher> {
        self.publisher.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}
fn fence_points(pending: &[PendingFence]) -> Vec<FencePoints<'_>> {
    pending
        .iter()
        .map(|fence| FencePoints {
            ring: fence.ring,
            fence: fence.fence,
            points: fence.points.iter().map(|point| (&point.counter, point.value)).collect(),
        })
        .collect()
}
fn token(native: u32, generation: u64) -> u64 {
    (generation << HANDLE_GENERATION_SHIFT) | native as u64
}
fn native(handle: u64, generation: u64) -> Result<u32, String> {
    let id = handle as u32;

    if handle >> HANDLE_GENERATION_SHIFT != generation || id == 0 || id > MAX_NATIVE_ID {
        return Err("Invalid or stale Venus handle".into());
    }
    Ok(id)
}
fn take_id(next: &mut u32) -> Result<u32, String> {
    if *next == 0 || *next > MAX_NATIVE_ID {
        return Err("Venus handle space exhausted".into());
    }
    let id = *next;
    *next += 1;
    Ok(id)
}
impl HostService {
    /// Export only owned, shareable DMA memory with a bounded linear layout.
    /// GPU images need no CPU map or HOST_VISIBLE/HOST_COHERENT requirement.
    pub fn export_scanout(
        &self,
        context: u64,
        resource: u64,
        layout: transport_contract::ScanoutLayout,
    ) -> Result<OwnedFd, String> {
        let blob = self.blob(context, resource)?;

        if !blob.shareable || !layout.fits(blob.bytes) {
            return Err("Display image exceeds its owned shareable allocation".into());
        }
        let (mut kind, mut fd) = (0, -1);
        let status = unsafe {
            qlv_venus_blob_export(native(resource, self.generation)?, &mut kind, &mut fd)
        };
        // Adopt error descriptors too so every received duplicate is closed.
        let descriptor = (fd >= 0).then(|| unsafe { OwnedFd::from_raw_fd(fd) });

        if status != 0 || !unsafe { qlv_venus_fd_is_dmabuf(kind) } {
            return Err("Display image is not exportable DMA memory".into());
        }
        descriptor.ok_or_else(|| "Display export returned no descriptor".into())
    }
    pub fn open_generation(generation: u64) -> Result<Self, String> {
        Self::open(generation, HostOptions::default())
    }
    pub fn open(generation: u64, options: HostOptions) -> Result<Self, String> {
        if generation == 0 || generation > u32::MAX as u64 {
            return Err("Venus connection generation exhausted".into());
        }
        Ok(Self {
            renderer: Backend::open()?,
            contexts: BTreeMap::new(),
            blobs: BTreeMap::new(),
            next_handle: 1,
            generation,
            allocated_bytes: 0,
            blob_budget_bytes: options.blob_budget_bytes,
            profile_blobs: std::env::var_os("QLV_BLOB_PROFILE").is_some(),
            created_blobs: 0,
            destroyed_blobs: 0,
            rejected_blobs: 0,
            watches: Arc::default(),
            publisher: Arc::new(Mutex::new(Publisher::new(
                RING_TIMELINE_SLOTS as usize,
                MAX_PENDING_FENCES,
            ))),
        })
    }
    pub fn sync_events(&self) -> SyncEvents {
        SyncEvents {
            watches: self.watches.clone(),
            observer: self.renderer.observer(),
            publisher: self.publisher.clone(),
        }
    }
    fn events_lock(&self) -> MutexGuard<'_, Watches> {
        self.watches.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
    fn publisher_lock(&self) -> MutexGuard<'_, Publisher> {
        self.publisher.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
    /// Arm one SYNC_EVENT for the points (stage-3 W4). Ok(true) means they
    /// are already reached: nothing is armed and no event follows.
    pub fn watch(
        &mut self,
        context: u64,
        points: &[SyncPoint],
        any: bool,
        token: u32,
    ) -> Result<bool, String> {
        self.poll(context)?;
        let native = native(context, self.generation)?;
        let owner = self.context(context)?;
        let mut watch_points = Vec::with_capacity(points.len());

        for point in points {
            watch_points.push(WatchPoint {
                counter: owner.syncs.get(&point.sync).ok_or("Unknown Venus sync")?.clone(),
                value: point.value,
                bound: None,
            });
        }
        let watch = Watch { context, native, token, any, points: watch_points };
        let pending = fence_points(&owner.pending);
        let renderer = &self.renderer;
        let satisfied = self.events_lock().add(
            watch,
            &pending,
            &|native, ring| renderer.completed_fence(native, ring),
            MAX_WATCHES,
        )?;
        // A fence may have retired between the check and arming: re-evaluate.
        self.renderer.observer().notify();
        Ok(satisfied)
    }
    pub fn create_context(&mut self) -> Result<u64, String> {
        if self.contexts.len() >= MAX_CONTEXTS {
            return Err("Venus context quota exceeded".into());
        }
        let id = take_id(&mut self.next_handle)?;
        let status = unsafe {
            qlv_venus_context_create(id, CONTEXT_NAME.as_ptr(), CONTEXT_NAME.len() as u32)
        };

        if status != 0 {
            return Err(format!("Renderer context creation failed: {status}"));
        }
        let handle = token(id, self.generation);
        self.contexts.insert(
            handle,
            Context {
                syncs: BTreeMap::new(),
                pending: Vec::new(),
                next_fence: 1,
                lost: false,
            },
        );
        Ok(handle)
    }
    fn context(&self, handle: u64) -> Result<&Context, String> {
        native(handle, self.generation)?;
        let context = self.contexts.get(&handle).ok_or("Unknown Venus context")?;

        if context.lost {
            return Err("Venus context is lost; close it before reuse".into());
        }
        Ok(context)
    }
    pub fn blob_budget(&self, context: u64) -> Result<(u64, u64), String> {
        self.context(context)?;
        Ok((self.blob_budget_bytes, self.allocated_bytes))
    }
    pub fn caps(&self, context: u64) -> Result<Vec<u8>, String> {
        self.context(context)?;
        let (mut version, mut bytes) = (0, 0);
        unsafe {
            qlv_venus_capset(&mut version, &mut bytes);
        }
        if bytes == 0 || bytes as usize > MAX_CAPSET_BYTES {
            return Err("Invalid Venus capset length".into());
        }
        let mut words = vec![0u32; (bytes as usize).div_ceil(COMMAND_WORD_BYTES)];
        let status = unsafe { qlv_venus_caps(version, words.as_mut_ptr().cast(), bytes) };

        if status != 0 {
            return Err(format!("Venus capset read failed: {status}"));
        }
        let data = words
            .iter()
            .flat_map(|word| word.to_le_bytes())
            .take(bytes as usize)
            .collect();
        Ok(data)
    }
    pub fn create_blob(
        &mut self,
        context: u64,
        memory_id: u64,
        bytes: u64,
        mappable: bool,
        shareable: bool,
    ) -> Result<Option<u64>, String> {
        self.context(context)?;
        if bytes == 0 || !bytes.is_multiple_of(PAGE_BYTES) || bytes > APERTURE_BYTES {
            return Err("Invalid Venus blob size or alignment".into());
        }
        if !blob_capacity(self.allocated_bytes, self.blobs.len(), bytes, self.blob_budget_bytes) {
            self.rejected_blobs += 1;
            if self.profile_blobs && (self.rejected_blobs <= 3 || self.rejected_blobs.is_multiple_of(128)) {
                eprintln!(
                    "Lightning Venus blobs: reject={} create={} destroy={} live={} bytes={} requested={} budget={}",
                    self.rejected_blobs,
                    self.created_blobs,
                    self.destroyed_blobs,
                    self.blobs.len(),
                    self.allocated_bytes,
                    bytes,
                    self.blob_budget_bytes
                );
                if self.rejected_blobs == 1 {
                    let mut counts = [0u64; 6];
                    let mut totals = [0u64; 6];
                    let mut mappable_counts = [0u64; 6];
                    let mut mappable_bytes = [0u64; 6];
                    for blob in self.blobs.values() {
                        let bucket = match blob.bytes {
                            0..=65536 => 0,
                            65537..=262144 => 1,
                            262145..=1048576 => 2,
                            1048577..=2097152 => 3,
                            2097153..=8388608 => 4,
                            _ => 5,
                        };
                        counts[bucket] += 1;
                        totals[bucket] += blob.bytes;
                        if blob.mappable {
                            mappable_counts[bucket] += 1;
                            mappable_bytes[bucket] += blob.bytes;
                        }
                    }
                    eprintln!(
                        "Lightning Venus blobs: size buckets <=64K/<=256K/<=1M/<=2M/<=8M/>8M counts={counts:?} bytes={totals:?} mappable_counts={mappable_counts:?} mappable_bytes={mappable_bytes:?}"
                    );
                }
            }
            return Ok(None);
        }
        let id = take_id(&mut self.next_handle)?;
        let status = unsafe {
            qlv_venus_memory_create(
                native(context, self.generation)?,
                id,
                memory_id,
                bytes,
                mappable,
                shareable,
            )
        };

        if status != 0 {
            return Err(format!(
                "Real Venus blob creation failed: {status} (memory {memory_id:#x}, {bytes} bytes, mappable {mappable})"
            ));
        }
        unsafe {
            qlv_venus_blob_attach(native(context, self.generation)?, id);
        }
        let handle = token(id, self.generation);
        self.blobs.insert(
            handle,
            Blob {
                holders: BTreeMap::from([(context, 1)]),
                shareable,
                mappable,
                bytes,
                memory_id,
            },
        );
        self.allocated_bytes += bytes;
        self.created_blobs += 1;
        if self.profile_blobs && self.created_blobs.is_multiple_of(128) {
            eprintln!(
                "Lightning Venus blobs: create={} destroy={} live={} bytes={}",
                self.created_blobs,
                self.destroyed_blobs,
                self.blobs.len(),
                self.allocated_bytes
            );
        }
        Ok(Some(handle))
    }
    fn blob(&self, context: u64, resource: u64) -> Result<&Blob, String> {
        self.context(context)?;
        native(resource, self.generation)?;
        let blob = self.blobs.get(&resource).ok_or("Unknown Venus blob")?;

        if !blob.holders.contains_key(&context) {
            return Err("Venus blob belongs to another context".into());
        }
        Ok(blob)
    }
    /// Coherent memory needs no VkFlushMappedMemoryRanges/Invalidate call.
    /// Prove the actual allocation type and bound the retained blob range first.
    /// The guest KMD must preserve the renderer cache mode in the aperture mapping.
    pub fn coherent_range(
        &self,
        context: u64,
        resource: u64,
        offset: u64,
        bytes: u64,
    ) -> Result<(), String> {
        let blob = self.blob(context, resource)?;

        if offset
            .checked_add(bytes)
            .filter(|end| *end <= blob.bytes)
            .is_none()
        {
            return Err("Venus cache range exceeds its allocation".into());
        }
        self.verified_coherent(resource)
    }
    fn verified_coherent(&self, resource: u64) -> Result<(), String> {
        let mut properties = 0;
        let status = unsafe {
            qlv_venus_memory_properties(native(resource, self.generation)?, &mut properties)
        };

        if status != 0 || properties & VISIBLE_COHERENT != VISIBLE_COHERENT {
            return Err("Host allocation lacks verified host-visible coherent properties".into());
        }
        Ok(())
    }
    pub fn export_blob(&self, context: u64, resource: u64) -> Result<ExportedBlob, String> {
        let blob = self.blob(context, resource)?;
        self.verified_coherent(resource)?;
        let mut cache_mode = 0;
        let status =
            unsafe { qlv_venus_cache_mode(native(resource, self.generation)?, &mut cache_mode) };

        if status != 0 {
            return Err("Host allocation has no supported CPU cache mode".into());
        }
        let (mut kind, mut fd) = (0, -1);
        let status = unsafe {
            qlv_venus_blob_export(native(resource, self.generation)?, &mut kind, &mut fd)
        };
        // The library returns an owned duplicate. Adopt it even on an error
        // before returning, so unexpected error descriptors cannot leak.
        let descriptor = if fd >= 0 {
            Some(unsafe { OwnedFd::from_raw_fd(fd) })
        } else {
            None
        };

        if status != 0 {
            return Err(format!("Venus blob export failed: {status}"));
        }
        let descriptor = descriptor.ok_or("Venus export returned no descriptor")?;
        let export_kind = if unsafe { qlv_venus_fd_is_shared(kind) } {
            ExportKind::SharedMemory
        } else if unsafe { qlv_venus_fd_is_dmabuf(kind) } {
            ExportKind::DmaBuf
        } else {
            return Err("Venus blob is not directly mappable shared memory or DMA memory".into());
        };

        if blob.memory_id == 0 && export_kind != ExportKind::SharedMemory {
            return Err("Venus transport allocation is not shared memory".into());
        }
        Ok(ExportedBlob {
            descriptor,
            kind: export_kind,
            cache_mode,
            bytes: blob.bytes,
        })
    }
    /// Attach a shareable blob of another context in this VM to context.
    pub fn import_blob(&mut self, context: u64, resource: u64) -> Result<(), String> {
        self.context(context)?;
        let blob = self.blobs.get_mut(&resource).ok_or("Unknown Venus blob")?;

        if !blob.shareable {
            return Err("Venus blob is not shareable".into());
        }
        if !blob.retain(context)? {
            return Ok(());
        }
        unsafe {
            qlv_venus_blob_attach(
                native(context, self.generation)?,
                native(resource, self.generation)?,
            );
        }
        // The proxy attach uses an asynchronous socket while the guest writes
        // Vulkan commands directly into a shared ring. A CPU-timeline fence on
        // that socket proves attachment before the guest may submit its import.
        let fence = self.context(context)?.next_fence;
        self.submit(context, CPU_TIMELINE, &[], &[])?;
        let id = native(context, self.generation)?;
        let deadline = Instant::now() + IMPORT_ATTACH_TIMEOUT;

        while self.renderer.completed_fence(id, CPU_TIMELINE) < fence {
            if Instant::now() >= deadline {
                self.contexts.get_mut(&context).unwrap().lost = true;
                return Err("Renderer shared-image attachment timed out".into());
            }
            std::thread::sleep(Duration::from_micros(WAIT_RETRY_MICROSECONDS));
        }
        self.poll(context)?;
        Ok(())
    }
    pub fn destroy_blob(&mut self, context: u64, resource: u64) -> Result<(), String> {
        self.blob(context, resource)?;

        if !self.blobs.get_mut(&resource).unwrap().release(context)? {
            return Ok(());
        }
        unsafe {
            qlv_venus_blob_detach(
                native(context, self.generation)?,
                native(resource, self.generation)?,
            );
        }
        self.release_blob(context, resource)
    }
    /// Drop one holder; the renderer resource dies with its last holder.
    fn release_blob(&mut self, context: u64, resource: u64) -> Result<(), String> {
        let blob = self.blobs.get_mut(&resource).ok_or("Unknown Venus blob")?;
        blob.holders.remove(&context);
        if blob.holders.is_empty() {
            let bytes = blob.bytes;
            self.blobs.remove(&resource);
            unsafe {
                qlv_venus_blob_destroy(native(resource, self.generation)?);
            }
            self.allocated_bytes -= bytes;
            self.destroyed_blobs += 1;
            if self.profile_blobs && self.destroyed_blobs.is_multiple_of(128) {
                eprintln!(
                    "Lightning Venus blobs: create={} destroy={} live={} bytes={}",
                    self.created_blobs,
                    self.destroyed_blobs,
                    self.blobs.len(),
                    self.allocated_bytes
                );
            }
        }
        Ok(())
    }
    /// With publish, also try to give the sync a timeline slot (None when
    /// the page is full: the guest then keeps exchanging for it).
    pub fn create_sync(
        &mut self,
        context: u64,
        value: u64,
        publish: bool,
    ) -> Result<(u64, Option<u32>), String> {
        if self.context(context)?.syncs.len() >= MAX_SYNCS {
            return Err("Venus sync quota exceeded".into());
        }
        let native_context = native(context, self.generation)?;
        let handle = token(take_id(&mut self.next_handle)?, self.generation);
        let counter = Arc::new(AtomicU64::new(value));
        let slot = if publish {
            self.publisher_lock()
                .allocate(context, native_context, handle, counter.clone())
        } else {
            None
        };
        self.contexts
            .get_mut(&context)
            .unwrap()
            .syncs
            .insert(handle, counter);
        Ok((handle, slot))
    }
    pub fn write_sync(&mut self, context: u64, sync: u64, value: u64) -> Result<(), String> {
        self.poll(context)?;
        let counter = self
            .context(context)?
            .syncs
            .get(&sync)
            .ok_or("Unknown Venus sync")?;
        let previous = counter.load(Ordering::Acquire);

        if value < previous && value != 0 {
            return Err("Venus timeline cannot move backwards".into());
        }
        if value == 0
            && self.context(context)?.pending.iter().any(|fence| {
                fence
                    .points
                    .iter()
                    .any(|point| Arc::ptr_eq(counter, &point.counter))
            })
        {
            return Err("Cannot reset a sync retained by in-flight renderer work".into());
        }
        counter.store(value, Ordering::Release);
        self.renderer.observer().notify();
        Ok(())
    }
    pub fn destroy_sync(&mut self, context: u64, sync: u64) -> Result<(), String> {
        let owner = self.context(context)?;

        if !owner.syncs.contains_key(&sync) {
            return Err("Unknown Venus sync".into());
        }
        self.contexts.get_mut(&context).unwrap().syncs.remove(&sync);
        self.publisher_lock().release(sync);
        Ok(())
    }
    pub fn submit(
        &mut self,
        context: u64,
        ring: u32,
        commands: &[u8],
        points: &[SyncPoint],
    ) -> Result<(), String> {
        // Mapped completion counters can satisfy guest waits without read_sync.
        // Reap backend-completed work before enforcing the in-flight quota.
        self.poll(context)?;
        let owner = self.context(context)?;

        if ring >= MAX_TIMELINES
            || !commands.len().is_multiple_of(COMMAND_WORD_BYTES)
            || commands.len() > MAX_PAYLOAD_BYTES
            || points.len() > MAX_SUBMIT_SYNCS
            || owner.pending.len() >= MAX_PENDING_FENCES
            || owner.next_fence == u64::MAX
        {
            return Err("Venus submission framing or quota is invalid".into());
        }
        for point in points {
            let previous = owner
                .syncs
                .get(&point.sync)
                .ok_or("Unknown submitted Venus sync")?;

            if point.value < previous.load(Ordering::Acquire) {
                return Err("Submitted Venus sync moves backwards".into());
            }
        }
        // A sync-only submission is valid: enqueue a real renderer fence after
        // prior work, without passing an empty stream to the command decoder.
        if !commands.is_empty() {
            let mut words: Vec<u32> = commands
                .chunks_exact(COMMAND_WORD_BYTES)
                .map(|word| u32::from_le_bytes(word.try_into().unwrap()))
                .collect();
            let status = unsafe {
                qlv_venus_submit(
                    native(context, self.generation)?,
                    words.as_mut_ptr().cast(),
                    words.len() as u32,
                )
            };

            if status != 0 {
                self.contexts.get_mut(&context).unwrap().lost = true;
                return Err(format!("Venus command submission failed: {status}"));
            }
        }
        let fence = self.contexts.get(&context).unwrap().next_fence;
        let status = unsafe { qlv_venus_fence(native(context, self.generation)?, ring, fence) };

        if status != 0 {
            self.contexts.get_mut(&context).unwrap().lost = true;
            return Err(format!("Venus renderer fence submission failed: {status}"));
        }
        let bound: Vec<(u64, u64)> = points.iter().map(|point| (point.sync, point.value)).collect();
        let bound = self.publisher_lock().bind(context, ring, fence, &bound);
        if let Err(error) = bound {
            self.contexts.get_mut(&context).unwrap().lost = true;
            return Err(error);
        }
        let owner = self.contexts.get_mut(&context).unwrap();
        owner.next_fence += 1;
        owner.pending.push(PendingFence {
            ring,
            fence,
            points: points
                .iter()
                .map(|point| PendingPoint {
                    value: point.value,
                    counter: owner.syncs.get(&point.sync).unwrap().clone(),
                })
                .collect(),
        });
        // Watches waiting for this signal can now bind to its fence.
        let pending = fence_points(&self.contexts[&context].pending);
        self.events_lock().bind_context(context, &pending);
        drop(pending);
        self.renderer.observer().notify();
        Ok(())
    }
    /// Advance guest syncs whose renderer fence the backend sync thread has
    /// already reported; this never calls into the renderer.
    pub fn poll(&mut self, context: u64) -> Result<(), String> {
        let id = native(context, self.generation)?;
        self.context(context)?;
        let owner = self.contexts.get_mut(&context).unwrap();
        let renderer = &self.renderer;
        let retired = retire_completed_fences(&mut owner.pending, |ring| {
            renderer.completed_fence(id, ring)
        });
        if !retired.is_empty() {
            let mut publisher = self.publisher_lock();
            for (ring, fence) in retired {
                publisher.retired(context, ring, fence);
            }
        }
        Ok(())
    }
    pub fn read_sync(&mut self, context: u64, sync: u64) -> Result<u64, String> {
        self.poll(context)?;
        self.context(context)?
            .syncs
            .get(&sync)
            .map(|counter| counter.load(Ordering::Acquire))
            .ok_or("Unknown Venus sync".into())
    }
    /// Report whether the points are satisfied right now. The guest polls with
    /// zero-timeout checks so the serialized mailbox stays free between them.
    pub fn syncs_ready(
        &mut self,
        context: u64,
        points: &[SyncPoint],
        any: bool,
    ) -> Result<bool, String> {
        self.poll(context)?;
        let syncs = &self.context(context)?.syncs;
        let mut satisfied = 0usize;

        for point in points {
            let counter = syncs.get(&point.sync).ok_or("Unknown Venus sync")?;

            if counter.load(Ordering::Acquire) >= point.value {
                satisfied += 1;
            }
        }
        Ok(satisfied == points.len() || (any && satisfied != 0))
    }
    /// Join renderer work before freeing resources. QEMU must first revoke and
    /// flush its process/aperture mappings, retaining exported descriptors until
    /// each mapping is gone; this method does not promise guest mapping cleanup.
    pub fn close_context(&mut self, context: u64) -> Result<(), String> {
        let id = native(context, self.generation)?;

        if self.contexts.remove(&context).is_none() {
            return Err("Unknown Venus context".into());
        }
        self.events_lock().drop_context(context);
        self.publisher_lock().drop_context(context);
        unsafe {
            qlv_venus_context_destroy(id);
        }
        self.renderer.forget_context(id);
        let resources: Vec<u64> = self
            .blobs
            .iter()
            .filter_map(|(id, blob)| blob.holders.contains_key(&context).then_some(*id))
            .collect();

        for resource in resources {
            self.release_blob(context, resource)?;
        }
        Ok(())
    }
}
impl Drop for HostService {
    fn drop(&mut self) {
        let contexts: Vec<u64> = self.contexts.keys().copied().collect();

        for context in contexts {
            let _cleanup = self.close_context(context);
        }
        // Backend cleanup runs only after every context has stopped using it.
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    const HANDLE_GENERATION: u64 = 1;
    #[test]
    fn completed_submissions_release_the_in_flight_quota_without_guest_reads() {
        let counter = Arc::new(AtomicU64::new(0));
        let mut pending: Vec<_> = (1..=MAX_PENDING_FENCES as u64)
            .map(|fence| PendingFence {
                ring: CPU_TIMELINE,
                fence,
                points: vec![PendingPoint {
                    value: fence,
                    counter: counter.clone(),
                }],
            })
            .collect();
        let retired = retire_completed_fences(&mut pending, |_| MAX_PENDING_FENCES as u64);
        assert_eq!(retired.len(), MAX_PENDING_FENCES);
        assert!(pending.is_empty());
        assert_eq!(counter.load(Ordering::Acquire), MAX_PENDING_FENCES as u64);
    }

    #[test]
    fn retirement_preserves_unfinished_rings_and_never_regresses_sync_values() {
        const COMPLETE_RING: u32 = 0;
        const WAITING_RING: u32 = 1;
        const FENCE: u64 = 3;
        const LATER_VALUE: u64 = 9;
        let completed_counter = Arc::new(AtomicU64::new(LATER_VALUE));
        let waiting_counter = Arc::new(AtomicU64::new(0));
        let mut pending = vec![
            PendingFence {
                ring: COMPLETE_RING,
                fence: FENCE,
                points: vec![PendingPoint {
                    value: FENCE,
                    counter: completed_counter.clone(),
                }],
            },
            PendingFence {
                ring: WAITING_RING,
                fence: FENCE,
                points: vec![PendingPoint {
                    value: FENCE,
                    counter: waiting_counter.clone(),
                }],
            },
        ];
        let retired = retire_completed_fences(&mut pending, |ring| {
            if ring == COMPLETE_RING {
                FENCE
            } else {
                FENCE - 1
            }
        });
        assert_eq!(retired, vec![(COMPLETE_RING, FENCE)]);
        assert_eq!(pending.len(), 1);
        assert_eq!(pending[0].ring, WAITING_RING);
        assert_eq!(completed_counter.load(Ordering::Acquire), LATER_VALUE);
        assert_eq!(waiting_counter.load(Ordering::Acquire), 0);
    }
    #[test]
    fn repeated_imports_keep_one_attachment_until_the_last_release() {
        let mut blob = Blob {
            holders: BTreeMap::from([(1, 1)]),
            shareable: true,
            mappable: true,
            bytes: PAGE_BYTES,
            memory_id: 1,
        };
        assert!(!blob.retain(1).unwrap());
        assert!(blob.retain(2).unwrap());
        assert!(!blob.retain(2).unwrap());
        assert!(!blob.release(1).unwrap());
        assert!(!blob.release(2).unwrap());
        assert!(blob.release(2).unwrap());
        assert_eq!(blob.holders[&1], 1);
        assert!(blob.release(2).is_err());
        assert!(blob.release(3).is_err());
        blob.holders.insert(1, MAX_BLOB_REFERENCES);
        assert!(blob.retain(1).is_err());
        assert_eq!(blob.holders[&1], MAX_BLOB_REFERENCES);
    }
    #[test]
    fn blob_budget_is_independent_of_mapping_window_and_still_bounded() {
        let budget = BLOB_BUDGET_BYTES;
        assert!(blob_capacity(APERTURE_BYTES, 32, PAGE_BYTES, budget));
        assert!(blob_capacity(budget - PAGE_BYTES, MAX_RESOURCES - 1, PAGE_BYTES, budget));
        assert!(!blob_capacity(budget, 1, PAGE_BYTES, budget));
        assert!(!blob_capacity(0, MAX_RESOURCES, PAGE_BYTES, budget));
        assert!(!blob_capacity(u64::MAX, 1, PAGE_BYTES, budget));
        // A larger launch budget (stage-3 D7) moves only the byte limit.
        assert!(blob_capacity(budget, 1, PAGE_BYTES, 4 * budget));
        assert!(!blob_capacity(0, MAX_RESOURCES, PAGE_BYTES, 4 * budget));
    }
    #[test]
    fn generation_tags_and_native_id_bounds_reject_forged_and_old_handles() {
        assert_eq!(
            native(token(1, HANDLE_GENERATION), HANDLE_GENERATION),
            Ok(1)
        );
        assert_eq!(native(token(1, 2), 2), Ok(1));
        assert!(native(token(1, 1), 2).is_err());
        for handle in [
            0,
            1,
            token(0, HANDLE_GENERATION),
            token(MAX_NATIVE_ID + 1, HANDLE_GENERATION),
            token(1, HANDLE_GENERATION) + (1 << HANDLE_GENERATION_SHIFT),
        ] {
            assert!(native(handle, HANDLE_GENERATION).is_err());
        }
    }
    #[test]
    fn id_exhaustion_never_wraps_into_reusable_or_negative_renderer_ids() {
        let mut next = MAX_NATIVE_ID;
        assert_eq!(take_id(&mut next), Ok(MAX_NATIVE_ID));
        assert!(take_id(&mut next).is_err());
    }
}

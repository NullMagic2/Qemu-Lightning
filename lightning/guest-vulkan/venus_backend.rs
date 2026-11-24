//! Module: lightning.guest_vulkan.venus_backend
//! Purpose: Own the process-wide Venus renderer and its observed fence timelines.
//! Created: 2026-10-09
//! Architecture: Real host ABI only. The renderer sync thread reports retired
//! fences here; venus_service maps them to guest syncs and bounds every wait.
use std::{
    collections::BTreeMap,
    ffi::c_void,
    marker::PhantomData,
    rc::Rc,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Condvar, Mutex, MutexGuard,
    },
    time::Duration,
};
// virglrenderer owns process-wide state. Only one owner may initialize it, and
// Rc phantom ownership keeps every call on the original renderer thread.
static BACKEND_OWNED: AtomicBool = AtomicBool::new(false);
#[derive(Default)]
struct Timelines {
    completed: BTreeMap<(u32, u32), u64>,
}
/// Retired renderer timelines, shared with the sync-event notifier (stage-3
/// W4): every retired fence or service-side change bumps an epoch the notifier
/// waits on, so it never polls the renderer.
#[derive(Default)]
pub struct Observer {
    timelines: Mutex<Timelines>,
    epoch: Mutex<u64>,
    changed: Condvar,
}
impl Observer {
    fn lock(&self) -> MutexGuard<'_, Timelines> {
        self.timelines
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
    pub fn completed_fence(&self, context: u32, ring: u32) -> u64 {
        self.lock().completed.get(&(context, ring)).copied().unwrap_or(0)
    }
    /// Wake the notifier: a fence retired, or a sync, watch or context changed.
    pub fn notify(&self) {
        let mut epoch = self.epoch.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        *epoch = epoch.wrapping_add(1);
        self.changed.notify_all();
    }
    /// Wait until the epoch moves past seen, or the timeout passes; return the
    /// current epoch.
    pub fn wait(&self, seen: u64, timeout: Duration) -> u64 {
        let epoch = self.epoch.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let (epoch, _) = self
            .changed
            .wait_timeout_while(epoch, timeout, |epoch| *epoch == seen)
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        *epoch
    }
}
// The renderer sync thread calls this while the owner thread serves requests.
// The boxed cookie remains stable until renderer cleanup has joined that thread.
#[no_mangle]
unsafe extern "C" fn qlv_observe_fence(cookie: *mut c_void, context: u32, ring: u32, fence: u64) {
    let observer = unsafe { &*cookie.cast::<Observer>() };
    {
        let mut timelines = observer.lock();
        let completed = timelines.completed.entry((context, ring)).or_insert(0);
        *completed = (*completed).max(fence);
    }
    observer.notify();
}
unsafe extern "C" {
    fn qlv_venus_initialize(cookie: *mut c_void) -> i32;
    fn qlv_venus_cleanup(cookie: *mut c_void);
}
pub struct Backend {
    // Shared with the notifier; the renderer's raw cookie stays valid until
    // cleanup joins its sync thread, and the Arc outlives that.
    observer: Arc<Observer>,
    thread_owner: PhantomData<Rc<()>>,
}
impl Backend {
    pub fn open() -> Result<Self, String> {
        if BACKEND_OWNED
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Err("The process already owns a Venus renderer".into());
        }
        let mut value = Self {
            observer: Arc::default(),
            thread_owner: PhantomData,
        };
        let status = unsafe { qlv_venus_initialize(value.cookie()) };

        if status != 0 {
            // The library may have partly initialized; the drop cleanup is
            // valid after a failed init and no submitted work exists here.
            return Err(format!(
                "Venus host backend initialization failed: {status}"
            ));
        }
        Ok(value)
    }
    fn cookie(&mut self) -> *mut c_void {
        Arc::as_ptr(&self.observer).cast_mut().cast()
    }
    /// Return actual retired renderer timeline values; never submitted values.
    pub fn completed_fence(&self, context: u32, ring: u32) -> u64 {
        self.observer.completed_fence(context, ring)
    }
    pub fn observer(&self) -> Arc<Observer> {
        self.observer.clone()
    }
    /// Call after context destruction has joined its sync thread, so completed
    /// timeline storage cannot accumulate across repeatedly opened applications.
    pub fn forget_context(&self, context: u32) {
        self.observer
            .lock()
            .completed
            .retain(|(owner, _), _| *owner != context);
    }
}
impl Drop for Backend {
    fn drop(&mut self) {
        // Cleanup destroys every remaining renderer context and joins its sync
        // thread before the observer storage drops.
        unsafe {
            qlv_venus_cleanup(self.cookie());
        }
        BACKEND_OWNED.store(false, Ordering::Release);
    }
}

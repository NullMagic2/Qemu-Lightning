//! Module: lightning.manager.drag
//! Purpose: Share explicit display drag gestures with the authenticated guest service.
//! Created: 2026-10-09
//! Architecture: Per-VM copy-only state; GTK and the backend share no native pointers.
use std::{
    collections::HashMap,
    path::PathBuf,
    sync::{atomic::AtomicBool, Arc, Mutex, Weak},
    time::Instant,
};
#[derive(Default)]
pub struct State {
    pub armed: AtomicBool,
    pub incoming: Mutex<Vec<Drop>>,
    pub outgoing: Mutex<Option<(Instant, Vec<PathBuf>)>>,
}
pub struct Drop {
    pub files: Vec<(String, String)>,
    pub x: u64,
    pub y: u64,
}
pub type Registry = Arc<Mutex<HashMap<uuid::Uuid, Weak<State>>>>;

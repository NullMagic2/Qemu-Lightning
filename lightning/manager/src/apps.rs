//! Module: lightning.manager.apps
//! Purpose: Keep authenticated guest-window snapshots and per-application permissions.
//! Created: 2026-10-08
//! Architecture: The tools service owns I/O; GTK only receives bounded snapshots.
#[path = "../../transfers/src/apps_wire.rs"]
pub mod wire;
use anyhow::{ensure, Result};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeSet,
    fs,
    io::Write,
    net::TcpStream,
    path::PathBuf,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Snapshot {
    pub connected: bool,
    pub windows: Vec<wire::AppWindow>,
    pub enabled: Vec<String>,
    pub frames: Vec<wire::Frame>,
}
pub struct State {
    path: PathBuf,
    enabled: BTreeSet<String>,
    windows: Vec<wire::AppWindow>,
    frames: Vec<wire::Frame>,
    inputs: Vec<wire::Input>,
    seen: Option<Instant>,
}
impl State {
    pub fn open(root: &std::path::Path) -> Result<Self> {
        let path = root.join("applications.json");
        let enabled: BTreeSet<String> = if path.exists() {
            serde_json::from_slice(&fs::read(&path)?)?
        } else {
            BTreeSet::new()
        };
        ensure!(
            enabled.len() <= wire::MAX_WINDOWS && enabled.iter().all(|s| s.len() <= wire::MAX_TEXT),
            "Invalid application preferences"
        );
        Ok(Self {
            path,
            enabled,
            windows: Vec::new(),
            frames: Vec::new(),
            inputs: Vec::new(),
            seen: None,
        })
    }
    pub fn snapshot(
        &mut self,
        change: Option<(String, bool)>,
        inputs: Vec<wire::Input>,
    ) -> Result<Snapshot> {
        let connected = self
            .seen
            .is_some_and(|seen| seen.elapsed() < Duration::from_secs(3));

        if let Some((application, enabled)) = change {
            ensure!(
                self.windows.iter().any(|w| w.application == application)
                    || self.enabled.contains(&application),
                "Application is no longer available"
            );
            let mut next = self.enabled.clone();

            if enabled {
                ensure!(
                    next.len() < wire::MAX_SHARED || next.contains(&application),
                    "Share up to four applications at a time"
                );
                next.insert(application);
            } else {
                next.remove(&application);
            }
            let mut file = tempfile::NamedTempFile::new_in(self.path.parent().unwrap())?;
            file.write_all(&serde_json::to_vec(&next)?)?;
            file.as_file().sync_all()?;
            file.persist(&self.path)?;
            self.enabled = next;
        }
        ensure!(inputs.len() <= wire::MAX_INPUT, "Input queue too large");
        for input in inputs {
            ensure!(wire::valid_input(&input), "Invalid application input");
            if connected
                && self
                    .windows
                    .iter()
                    .any(|w| w.id == input.id && self.enabled.contains(&w.application))
                && self.inputs.len() < wire::MAX_INPUT
            {
                self.inputs.push(input);
            }
        }
        Ok(Snapshot {
            connected,
            windows: if connected {
                self.windows.clone()
            } else {
                Vec::new()
            },
            enabled: self.enabled.iter().cloned().collect(),
            frames: if connected {
                std::mem::take(&mut self.frames)
            } else {
                self.frames.clear();
                Vec::new()
            },
        })
    }
}
/// No mutex is held during guest socket I/O. Only this authenticated operation
/// accepts frames; a guest cannot choose the host files or native window handles.
pub fn serve(stream: &mut TcpStream, state: &Arc<Mutex<State>>) -> Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(3)))?;
    stream.set_write_timeout(Some(Duration::from_secs(3)))?;
    stream.write_all(&[crate::transfers::wire::SUCCESS])?;
    let windows = wire::read_windows(stream)?;
    let (selected, inputs) = {
        let mut state = state
            .lock()
            .map_err(|_| anyhow::anyhow!("Application state unavailable"))?;
        state.windows = windows;
        state.seen = Some(Instant::now());
        let selected: Vec<u64> = state
            .windows
            .iter()
            .filter(|w| state.enabled.contains(&w.application))
            .take(wire::MAX_SHARED)
            .map(|w| w.id)
            .collect();
        let inputs = std::mem::take(&mut state.inputs);
        (selected, inputs)
    };
    wire::put(stream, selected.len() as u32)?;
    for id in &selected {
        wire::put_identity(stream, *id)?;
    }
    wire::write_inputs(stream, &inputs)?;
    let count = wire::count(stream, wire::MAX_SHARED)?;
    let mut frames = Vec::new();

    for _ in 0..count {
        let frame = wire::read_frame(stream)?;
        ensure!(
            selected.contains(&frame.id) && !frames.iter().any(|f: &wire::Frame| f.id == frame.id),
            "Unrequested application frame"
        );
        frames.push(frame);
    }
    let mut state = state
        .lock()
        .map_err(|_| anyhow::anyhow!("Application state unavailable"))?;
    state.frames = frames;
    state.seen = Some(Instant::now());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        io::{Cursor, Read},
        net::TcpListener,
        thread,
    };
    #[test]
    fn frame_codec_rejects_oversized_dimensions_and_runs() {
        let frame = wire::Frame {
            id: 7,
            width: 2,
            height: 2,
            rgb: vec![33; 12],
        };
        let mut bytes = Vec::new();
        wire::write_frame(&mut bytes, &frame).unwrap();
        assert_eq!(
            wire::read_frame(&mut Cursor::new(&bytes)).unwrap().rgb,
            frame.rgb
        );
        let mut truncated = bytes.clone();
        truncated.pop();
        assert!(wire::read_frame(&mut Cursor::new(truncated)).is_err());
        bytes[16..20].copy_from_slice(&5u32.to_le_bytes());
        assert!(wire::read_frame(&mut Cursor::new(bytes)).is_err());
        assert!(wire::pixel_bytes(1281, 720).is_err());
        assert!(wire::pixel_bytes(0, 2).is_err());
        assert!(!wire::valid_input(&wire::Input {
            id: 7,
            kind: wire::KEY_DOWN,
            a: 256,
            b: 0
        }));
    }
    fn exchange(state: Arc<Mutex<State>>, send_frame: bool) -> (Vec<u64>, Vec<wire::Input>, bool) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let server =
            thread::spawn(move || serve(&mut listener.accept().unwrap().0, &state).is_ok());
        let mut client = TcpStream::connect(address).unwrap();
        let mut status = [0];
        client.read_exact(&mut status).unwrap();
        assert_eq!(status, [0]);
        wire::write_windows(
            &mut client,
            &[wire::AppWindow {
                id: 7,
                application: "C:\\App.exe".into(),
                title: "Test app".into(),
            }],
        )
        .unwrap();
        let selected = (0..wire::count(&mut client, wire::MAX_SHARED).unwrap())
            .map(|_| wire::identity(&mut client).unwrap())
            .collect();
        let inputs = wire::read_inputs(&mut client).unwrap();
        wire::put(&mut client, u32::from(send_frame)).unwrap();
        if send_frame {
            wire::write_frame(
                &mut client,
                &wire::Frame {
                    id: 7,
                    width: 2,
                    height: 2,
                    rgb: vec![33; 12],
                },
            )
            .unwrap();
        }
        (selected, inputs, server.join().unwrap())
    }
    #[test]
    fn sharing_is_per_application_persistent_and_input_is_scoped() {
        let root = tempfile::tempdir().unwrap();
        let state = Arc::new(Mutex::new(State::open(root.path()).unwrap()));
        let (ids, _, ok) = exchange(state.clone(), false);
        assert!(ok && ids.is_empty());
        state
            .lock()
            .unwrap()
            .snapshot(
                Some(("C:\\App.exe".into(), true)),
                vec![
                    wire::Input {
                        id: 7,
                        kind: wire::POINTER,
                        a: 10,
                        b: 20,
                    },
                    wire::Input {
                        id: 99,
                        kind: wire::KEY_DOWN,
                        a: 13,
                        b: 0,
                    },
                ],
            )
            .unwrap();
        let (ids, inputs, ok) = exchange(state.clone(), true);
        assert!(ok);
        assert_eq!(ids, [7]);
        assert_eq!(inputs.len(), 1);
        assert_eq!(inputs[0].id, 7);
        let snapshot = state.lock().unwrap().snapshot(None, Vec::new()).unwrap();
        assert_eq!(snapshot.frames.len(), 1);
        assert!(State::open(root.path())
            .unwrap()
            .enabled
            .contains("C:\\App.exe"));
        state
            .lock()
            .unwrap()
            .snapshot(Some(("C:\\App.exe".into(), false)), Vec::new())
            .unwrap();
        let (_, _, ok) = exchange(state, true);
        assert!(!ok, "Unselected frames must be rejected");
    }
}

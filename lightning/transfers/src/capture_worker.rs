//! Module: lightning.transfers.capture_worker
//! Purpose: Bound capture latency and recover from a stalled native capture call.
//! Created: 2026-10-08
//! Architecture: The guest owns selection and input. This supervisor owns one
//! disposable capture process and its pipe reader; it never terminates target apps.
use super::apps_wire as wire;
use std::{
    collections::HashMap,
    io::{self, BufReader, Write},
    process::{Child, ChildStdin, Command, Stdio},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

pub const WORKER_ARGUMENT: &str = "--capture-worker";
const CAPTURE_TIMEOUT: Duration = Duration::from_millis(500);
const CANCEL_POLL: Duration = Duration::from_millis(10);
const RETRY_DELAY: Duration = Duration::from_secs(5);

struct Process {
    child: Child,
    #[cfg(windows)]
    job: Option<super::windows_support::Owned>,
    input: ChildStdin,
    frames: mpsc::Receiver<io::Result<Option<wire::Frame>>>,
    reader: Option<JoinHandle<()>>,
}
impl Process {
    fn spawn() -> io::Result<Self> {
        #[cfg(not(test))]
        let executable = std::env::current_exe()?;
        #[cfg(test)]
        let executable = std::env::var_os("QEMU_CAPTURE_TEST_HELPER")
            .map(std::path::PathBuf::from)
            .unwrap_or(std::env::current_exe()?);
        let mut command = Command::new(executable);
        command.arg(WORKER_ARGUMENT);
        Self::command(command)
    }
    fn command(mut command: Command) -> io::Result<Self> {
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x08000000;
            command.creation_flags(CREATE_NO_WINDOW);
        }
        let mut child = command
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()?;
        #[cfg(windows)]
        let job = match super::windows_support::capture_job(&child) {
            Ok(job) => job,
            Err(error) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(error);
            }
        };
        let input = child.stdin.take().unwrap();
        let output = child.stdout.take().unwrap();
        let (send, frames) = mpsc::channel();
        let reader = thread::spawn(move || {
            let mut output = BufReader::new(output);

            loop {
                let frame = wire::number(&mut output).and_then(|present| match present {
                    0 => Ok(None),
                    1 => wire::read_frame(&mut output).map(Some),
                    _ => Err(wire::invalid()),
                });
                let failed = frame.is_err();

                if send.send(frame).is_err() || failed {
                    break;
                }
            }
        });
        Ok(Self {
            child,
            #[cfg(windows)]
            job: Some(job),
            input,
            frames,
            reader: Some(reader),
        })
    }
}
impl Drop for Process {
    fn drop(&mut self) {
        // Killing only our worker closes its pipe, unblocks the reader and reaps
        // its native GDI resources even when PrintWindow never returns.
        let _ = self.child.kill();
        #[cfg(windows)]
        drop(self.job.take());
        let _ = self.child.wait();

        if let Some(reader) = self.reader.take() {
            let _ = reader.join();
        }
    }
}

fn receive<T>(
    receiver: &mpsc::Receiver<io::Result<T>>,
    enabled: &AtomicBool,
    timeout: Duration,
) -> io::Result<T> {
    let deadline = Instant::now() + timeout;

    loop {
        if !enabled.load(Ordering::Acquire) {
            return Err(io::Error::new(
                io::ErrorKind::Interrupted,
                "Capture disabled",
            ));
        }
        let remaining = deadline.saturating_duration_since(Instant::now());

        if remaining.is_zero() {
            return Err(io::Error::new(io::ErrorKind::TimedOut, "Capture timed out"));
        }
        match receiver.recv_timeout(remaining.min(CANCEL_POLL)) {
            Ok(value) => return value,
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                return Err(io::Error::new(
                    io::ErrorKind::BrokenPipe,
                    "Capture worker exited",
                ));
            }
        }
    }
}

#[derive(Default)]
pub struct Capture {
    process: Option<Process>,
    retry_after: HashMap<u64, Instant>,
}
impl Capture {
    pub fn stop(&mut self) {
        self.process = None;
        self.retry_after.clear();
    }
    pub fn frame(&mut self, id: u64, enabled: &AtomicBool) -> io::Result<Option<wire::Frame>> {
        if !enabled.load(Ordering::Acquire) {
            self.stop();
            return Ok(None);
        }
        self.retry_after
            .retain(|_, deadline| *deadline > Instant::now());
        if self.retry_after.contains_key(&id) {
            return Ok(None);
        }
        if self.process.is_none() {
            self.process = Some(Process::spawn()?);
        }
        let process = self.process.as_mut().unwrap();
        let result = (|| {
            wire::put_identity(&mut process.input, id)?;
            process.input.flush()?;
            let frame = receive(&process.frames, enabled, CAPTURE_TIMEOUT)?;

            if frame.as_ref().is_some_and(|frame| frame.id != id) {
                return Err(wire::invalid());
            }
            Ok(frame)
        })();

        if result.is_err() {
            self.process = None;
            // Bounded by the current enumeration, never an unlimited history of HWNDs.
            if self.retry_after.len() >= wire::MAX_WINDOWS {
                self.retry_after.clear();
            }
            self.retry_after.insert(id, Instant::now() + RETRY_DELAY);
        }
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const SHORT_TIMEOUT: Duration = Duration::from_millis(30);
    #[test]
    fn stalled_worker_times_out_and_disable_cancels() {
        let (_send, receive_pipe) = mpsc::channel::<io::Result<()>>();
        let enabled = AtomicBool::new(true);
        assert_eq!(
            receive(&receive_pipe, &enabled, SHORT_TIMEOUT)
                .unwrap_err()
                .kind(),
            io::ErrorKind::TimedOut
        );
        enabled.store(false, Ordering::Release);
        assert_eq!(
            receive(&receive_pipe, &enabled, CAPTURE_TIMEOUT)
                .unwrap_err()
                .kind(),
            io::ErrorKind::Interrupted
        );
    }
    #[test]
    fn worker_exit_is_reported_and_response_is_preserved() {
        let (send, pipe) = mpsc::channel();
        let enabled = AtomicBool::new(true);
        send.send(Ok(42)).unwrap();
        assert_eq!(receive(&pipe, &enabled, SHORT_TIMEOUT).unwrap(), 42);
        drop(send);
        assert_eq!(
            receive(&pipe, &enabled, SHORT_TIMEOUT).unwrap_err().kind(),
            io::ErrorKind::BrokenPipe
        );
    }
    #[cfg(windows)]
    #[test]
    fn blocked_process_is_terminated_and_next_worker_starts() {
        let mut command = Command::new("powershell.exe");
        command.args([
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "Start-Sleep -Seconds 30",
        ]);
        let process = Process::command(command).unwrap();
        let pid = process.child.id();
        let mut capture = Capture {
            process: Some(process),
            retry_after: HashMap::new(),
        };
        let enabled = AtomicBool::new(true);
        const WINDOW_FIXTURE: u64 = 42;
        assert!(
            matches!(capture.frame(WINDOW_FIXTURE, &enabled), Err(error) if error.kind() == io::ErrorKind::TimedOut)
        );
        assert!(capture.process.is_none());
        super::super::windows_support::wait_process(pid).unwrap();
        capture.stop();
        // No HWND zero is eligible, but a fresh real helper must reply without stalling.
        assert!(capture.frame(0, &enabled).unwrap().is_none());
        assert!(capture.process.is_some());
        capture.stop();
    }
}

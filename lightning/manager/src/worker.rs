//! Module: lightning.manager.worker
//! Purpose: Keep QMP, disk operations and SSH off the GTK main thread.
//! Created: 2026-10-08
//! Architecture: Local and remote management use the same typed request protocol.
use crate::{
    backend::{self, Backend},
    model::{Reply, Request},
};
use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    io::{BufRead, BufReader, Write},
    path::PathBuf,
    process::{Child, ChildStdin, ChildStdout, Stdio},
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

#[derive(Clone, Serialize, Deserialize)]
pub struct Remote {
    pub host: String,
    pub executable: String,
    pub directory: String,
}
pub enum Command {
    Connect(Option<Remote>),
    Run(Request),
}
pub type Response = std::result::Result<Reply, String>;

enum Session {
    Local(Backend),
    Remote {
        child: Child,
        input: ChildStdin,
        output: BufReader<ChildStdout>,
        remote: Remote,
        tunnels: Vec<Tunnel>,
    },
}
impl Drop for Session {
    fn drop(&mut self) {
        if let Self::Remote { child, .. } = self {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}
impl Session {
    fn run(&mut self, request: Request) -> Result<Reply> {
        match self {
            Self::Local(backend) => backend.handle(request),
            Self::Remote {
                input,
                output,
                remote,
                tunnels,
                ..
            } => {
                writeln!(input, "{}", serde_json::to_string(&request)?)?;
                input.flush()?;
                let mut line = String::new();
                anyhow::ensure!(
                    output.read_line(&mut line)? > 0,
                    "Remote agent disconnected"
                );
                let reply = serde_json::from_str::<Response>(&line)?.map_err(anyhow::Error::msg)?;

                if let Reply::Console(path) = reply {
                    let tunnel = Tunnel::open(&remote.host, &path)?;
                    let local = tunnel.directory.path().join("vnc");
                    tunnels.push(tunnel);
                    Ok(Reply::Console(local))
                } else {
                    Ok(reply)
                }
            }
        }
    }
}

pub fn spawn(
    directory: PathBuf,
    binaries: PathBuf,
    drag_registry: crate::drag::Registry,
    accelerated_display: bool,
) -> (
    mpsc::Sender<Command>,
    mpsc::Receiver<Response>,
    thread::JoinHandle<()>,
) {
    let (commands, receiver) = mpsc::channel();
    let (events, responses) = mpsc::channel();
    let worker = thread::spawn(move || {
        let mut session: Option<Session> = None;

        while let Ok(command) = receiver.recv() {
            let result = (|| match command {
                Command::Connect(remote) => {
                    if let Some(current) = session.as_mut() {
                        if let Reply::Machines(views) = current.run(Request::List)? {
                            anyhow::ensure!(
                                views.iter().all(|view| view.state == "stopped"),
                                "Shut down the VMs in this workspace before changing workspace."
                            );
                        }
                    }
                    session = None;
                    session = Some(if let Some(remote) = remote {
                        let mut child = backend::ssh_command(
                            &remote.host,
                            &remote.executable,
                            &remote.directory,
                        )?
                        .stdin(Stdio::piped())
                        .stdout(Stdio::piped())
                        .stderr(Stdio::inherit())
                        .spawn()?;
                        let input = child.stdin.take().context("Missing SSH stdin")?;
                        let output =
                            BufReader::new(child.stdout.take().context("Missing SSH stdout")?);
                        Session::Remote {
                            child,
                            input,
                            output,
                            remote,
                            tunnels: Vec::new(),
                        }
                    } else {
                        {
                            let mut backend = Backend::new(&directory, binaries.clone())?;
                            backend.drag_registry = drag_registry.clone();
                            backend
                                .runtime
                                .prefer_accelerated_display(accelerated_display);
                            Session::Local(backend)
                        }
                    });
                    session.as_mut().unwrap().run(Request::List)
                }
                Command::Run(request) => {
                    if matches!(request, Request::CloseWorkspace { .. }) && session.is_none() {
                        Ok(Reply::Closed)
                    } else {
                        session
                            .as_mut()
                            .context("Connect to a workspace first")?
                            .run(request)
                    }
                }
            })();
            let closed = matches!(&result, Ok(Reply::Closed));

            if closed {
                // Release SSH agents/tunnels and workspace state before notifying GTK.
                session = None;
            }
            if events
                .send(result.map_err(|error| format!("{error:#}")))
                .is_err()
            {
                break;
            }
            if closed {
                break;
            }
        }
    });
    (commands, responses, worker)
}

struct Tunnel {
    child: Child,
    directory: tempfile::TempDir,
}
impl Drop for Tunnel {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
impl Tunnel {
    fn open(host: &str, remote: &std::path::Path) -> Result<Self> {
        const TIMEOUT: Duration = Duration::from_secs(12);
        const RETRY: Duration = Duration::from_millis(50);
        let remote = remote
            .to_str()
            .context("Remote console path is not UTF-8")?;
        anyhow::ensure!(
            remote.starts_with('/')
                && !remote.contains(':')
                && !remote.chars().any(char::is_control),
            "Invalid remote console path"
        );
        let directory = tempfile::tempdir()?;
        let local = directory.path().join("vnc");
        let child = std::process::Command::new("ssh")
            .args([
                "-N",
                "-T",
                "-oBatchMode=yes",
                "-oConnectTimeout=10",
                "-oExitOnForwardFailure=yes",
                "-L",
            ])
            .arg(format!("{}:{remote}", local.display()))
            .arg(host)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::inherit())
            .spawn()?;
        let mut tunnel = Self { child, directory };
        let started = Instant::now();

        while !local.exists() {
            anyhow::ensure!(
                tunnel.child.try_wait()?.is_none(),
                "SSH console tunnel failed"
            );
            anyhow::ensure!(started.elapsed() < TIMEOUT, "SSH console tunnel timed out");
            thread::sleep(RETRY);
        }
        Ok(tunnel)
    }
}

pub fn agent(directory: PathBuf, binaries: PathBuf) -> Result<()> {
    let mut backend = Backend::new(&directory, binaries)?;

    for line in std::io::stdin().lock().lines() {
        let response: Response = (|| backend.handle(serde_json::from_str(&line?)?))()
            .map_err(|error: anyhow::Error| format!("{error:#}"));
        println!("{}", serde_json::to_string(&response)?);
        std::io::stdout().flush()?;
    }
    Ok(())
}

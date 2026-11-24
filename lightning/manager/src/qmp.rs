//! Module: lightning.manager.qmp
//! Purpose: Negotiate QMP and match replies while tolerating asynchronous events.
//! Created: 2026-10-08
//! Architecture: Bounded Unix-socket I/O; backend decides which commands to issue.
use anyhow::{bail, ensure, Context, Result};
use serde_json::{json, Value};
use std::{
    io::{BufRead, BufReader, Read, Write},
    os::unix::net::UnixStream,
    path::Path,
    time::{Duration, Instant},
};

const IO_TIMEOUT: Duration = Duration::from_secs(5);
const DISK_FLUSH_TIMEOUT: Duration = Duration::from_secs(120);
const MAX_MESSAGE_BYTES: u64 = 4 * 1024 * 1024;

pub struct Qmp {
    stream: BufReader<UnixStream>,
    sequence: u64,
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::net::UnixListener;
    use std::thread;

    /// A disk flush error must stop the shutdown sequence before quit is sent.
    #[test]
    fn forced_poweroff_requires_successful_flush() {
        for fail_flush in [false, true] {
            let root = tempfile::tempdir().unwrap();
            let path = root.path().join("qmp");
            let listener = UnixListener::bind(&path).unwrap();
            let server = thread::spawn(move || {
                let (stream, _) = listener.accept().unwrap();
                let mut stream = BufReader::new(stream);
                writeln!(stream.get_mut(), "{}", json!({"QMP":{}})).unwrap();
                let mut commands = Vec::new();

                loop {
                    let mut line = String::new();

                    if stream.read_line(&mut line).unwrap() == 0 {
                        break;
                    }
                    let request: Value = serde_json::from_str(&line).unwrap();
                    let command = request["execute"].as_str().unwrap().to_owned();
                    commands.push(command.clone());
                    let response = if command == "stop" && fail_flush {
                        json!({"id":request["id"],"error":{"class":"GenericError","desc":"disk flush failed"}})
                    } else {
                        json!({"id":request["id"],"return":{}})
                    };
                    writeln!(stream.get_mut(), "{response}").unwrap();
                }
                commands
            });
            let mut qmp = Qmp::connect(&path).unwrap();
            assert_eq!(qmp.power_off().is_err(), fail_flush);
            drop(qmp);
            let commands = server.join().unwrap();

            if fail_flush {
                assert_eq!(commands, ["qmp_capabilities", "stop"]);
            } else {
                assert_eq!(commands, ["qmp_capabilities", "stop", "quit"]);
            }
        }
    }
}

impl Qmp {
    /// Pause guest writes, drain outstanding I/O and check the disk flush before
    /// requesting process exit. A failed flush leaves the paused VM available.
    pub fn power_off(&mut self) -> Result<()> {
        self.stream
            .get_ref()
            .set_read_timeout(Some(DISK_FLUSH_TIMEOUT))?;
        self.stream
            .get_ref()
            .set_write_timeout(Some(DISK_FLUSH_TIMEOUT))?;
        let result = self
            .execute_with_timeout("stop", json!({}), DISK_FLUSH_TIMEOUT)
            .and_then(|_| self.execute_with_timeout("quit", json!({}), DISK_FLUSH_TIMEOUT))
            .map(|_| ());
        self.stream.get_ref().set_read_timeout(Some(IO_TIMEOUT))?;
        self.stream.get_ref().set_write_timeout(Some(IO_TIMEOUT))?;
        result
    }
    pub fn connect(path: &Path) -> Result<Self> {
        let stream =
            UnixStream::connect(path).context("Could not connect to the VM control socket")?;
        stream.set_read_timeout(Some(IO_TIMEOUT))?;
        stream.set_write_timeout(Some(IO_TIMEOUT))?;
        let mut qmp = Self {
            stream: BufReader::new(stream),
            sequence: 0,
        };
        ensure!(qmp.read()?.get("QMP").is_some(), "Invalid QMP greeting");
        qmp.execute("qmp_capabilities", json!({}))?;
        Ok(qmp)
    }

    fn read(&mut self) -> Result<Value> {
        let mut line = String::new();
        let size = self
            .stream
            .by_ref()
            .take(MAX_MESSAGE_BYTES)
            .read_line(&mut line)?;
        ensure!(
            size > 0 && line.ends_with('\n'),
            "QMP disconnected or exceeded the message limit"
        );
        Ok(serde_json::from_str(&line)?)
    }

    pub fn execute(&mut self, command: &str, arguments: Value) -> Result<Value> {
        self.execute_with_timeout(command, arguments, IO_TIMEOUT)
    }

    fn execute_with_timeout(
        &mut self,
        command: &str,
        arguments: Value,
        timeout: Duration,
    ) -> Result<Value> {
        self.sequence += 1;
        let id = self.sequence;
        writeln!(
            self.stream.get_mut(),
            "{}",
            json!({"execute":command,"arguments":arguments,"id":id})
        )?;
        let start = Instant::now();

        while start.elapsed() < timeout {
            let response = self.read()?;

            if response["id"].as_u64() != Some(id) {
                continue;
            }
            if let Some(error) = response.get("error") {
                bail!("QEMU: {}", error["desc"]);
            }
            return response
                .get("return")
                .cloned()
                .context("QMP reply has no result");
        }
        bail!("QMP command timed out")
    }
}

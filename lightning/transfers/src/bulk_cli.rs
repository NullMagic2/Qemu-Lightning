//! Module: lightning.transfers.bulk_cli
//! Purpose: Expose bounded, scriptable file transfers over the existing authenticated TCP bridge.
//! Created: 2026-11-10
//! Architecture: Serial may invoke these commands, but binary payloads use Client PUT/GET.
use super::{configured_client, wire};
use std::{
    ffi::OsString,
    fs::{self, File, OpenOptions},
    io::{self, Write},
    path::PathBuf,
    time::Instant,
};

const EXIT_FAILURE: i32 = 1;
const EXIT_USAGE: i32 = 2;
const USAGE: &str = "QemuTransfers --send FILE [--config FILE] [--report FILE]\nQemuTransfers --receive-once --destination NEW_DIRECTORY [--config FILE] [--report FILE]\nPayloads use authenticated binary TCP, never the diagnostic serial channel.\nFiles are limited to 8 GiB; the host inbox retains its 8 GiB aggregate quota.\nReceive requires a new directory and exclusive guest-tools ownership.\nReports are created exclusively; existing files are never replaced.";

#[derive(Debug, PartialEq)]
enum Operation {
    Send(PathBuf),
    Receive(PathBuf),
}

#[derive(Debug)]
struct Options {
    operation: Operation,
    configuration: PathBuf,
    report: Option<PathBuf>,
}

fn path_argument(arguments: &mut impl Iterator<Item = OsString>) -> io::Result<PathBuf> {
    arguments
        .next()
        .filter(|path| !path.is_empty())
        .map(PathBuf::from)
        .ok_or_else(|| wire::fail("A file or directory argument is required"))
}

fn parse(mut arguments: impl Iterator<Item = OsString>) -> io::Result<Options> {
    let mut send = None;
    let mut receive = false;
    let mut destination = None;
    let mut configuration = None;
    let mut report = None;

    while let Some(argument) = arguments.next() {
        match argument.to_str() {
            Some("--send") if send.is_none() && !receive => {
                send = Some(path_argument(&mut arguments)?);
            }
            Some("--receive-once") if send.is_none() && !receive => receive = true,
            Some("--destination") if destination.is_none() => {
                destination = Some(path_argument(&mut arguments)?);
            }
            Some("--config") if configuration.is_none() => {
                configuration = Some(path_argument(&mut arguments)?);
            }
            Some("--report") if report.is_none() => {
                report = Some(path_argument(&mut arguments)?);
            }
            _ => {
                return Err(wire::fail(
                    "Unknown, duplicate or conflicting transfer option",
                ))
            }
        }
    }
    let operation = match (send, receive, destination) {
        (Some(path), false, None) => Operation::Send(path),
        (None, true, Some(path)) => Operation::Receive(path),
        _ => {
            return Err(wire::fail(
                "Choose --send FILE, or --receive-once --destination NEW_DIRECTORY",
            ))
        }
    };
    let configuration = match configuration {
        Some(path) => path,
        None => std::env::current_exe()?
            .parent()
            .ok_or_else(|| wire::fail("Missing guest-tools directory"))?
            .join("transfer.conf"),
    };
    Ok(Options {
        operation,
        configuration,
        report,
    })
}

/// Use caller-owned report output only; never truncate an existing file or token configuration.
fn report_file(path: Option<&PathBuf>) -> io::Result<Option<File>> {
    path.map(|path| OpenOptions::new().write(true).create_new(true).open(path))
        .transpose()
}

fn run(options: &Options) -> io::Result<(u64, u64)> {
    match &options.operation {
        Operation::Send(path) => {
            // The unused download directory already exists, so sending creates no inbox.
            let directory = options
                .configuration
                .parent()
                .filter(|path| !path.as_os_str().is_empty())
                .unwrap_or_else(|| std::path::Path::new("."))
                .to_owned();
            let client = configured_client(&options.configuration, directory)?;
            let bytes = client.send_bytes(path)?;
            Ok((1, bytes))
        }
        Operation::Receive(destination) => {
            #[cfg(windows)]
            super::windows_support::require_no_other_transfer_helpers()?;
            // Exclusive creation prevents accidental overwrite and pre-existing redirection.
            fs::create_dir(destination)?;
            let client = configured_client(&options.configuration, destination.clone())?;
            let count = client.receive()?;
            let mut bytes = 0u64;

            for directory in fs::read_dir(destination)? {
                for file in fs::read_dir(directory?.path())? {
                    bytes = bytes
                        .checked_add(file?.metadata()?.len())
                        .ok_or_else(|| wire::fail("Received byte count overflow"))?;
                }
            }
            Ok((count as u64, bytes))
        }
    }
}

/// None leaves GUI/setup dispatch untouched; bulk errors always return a nonzero exit code.
pub(super) fn dispatch(arguments: impl Iterator<Item = OsString>) -> Option<i32> {
    let arguments: Vec<_> = arguments.collect();
    let first = arguments.first()?.to_str()?;

    // Existing setup/capture/background switches retain their original dispatch.
    if matches!(
        first,
        "--background" | "--capture-worker" | "--install" | "--uninstall" | "--finish-uninstall"
    ) {
        return None;
    }
    if first == "--help" && arguments.len() == 1 {
        println!("{USAGE}");
        return Some(0);
    }
    let options = match parse(arguments.into_iter()) {
        Ok(options) => options,
        Err(error) => {
            eprintln!("{error}\n{USAGE}");
            return Some(EXIT_USAGE);
        }
    };
    let mut report = match report_file(options.report.as_ref()) {
        Ok(report) => report,
        Err(error) => {
            eprintln!("Cannot create transfer report: {error}");
            return Some(EXIT_FAILURE);
        }
    };
    let started = Instant::now();
    let result = run(&options);
    let milliseconds = started.elapsed().as_millis();
    let message = match &result {
        Ok((files, bytes)) => format!(
            "{{\"status\":\"ok\",\"files\":{files},\"bytes\":{bytes},\"elapsed_milliseconds\":{milliseconds}}}\n"
        ),
        Err(error) => format!(
            "{{\"status\":\"error\",\"error_kind\":\"{:?}\",\"elapsed_milliseconds\":{milliseconds}}}\n", error.kind()
        ),
    };

    if let Some(file) = report.as_mut() {
        if let Err(error) = file
            .write_all(message.as_bytes())
            .and_then(|_| file.sync_all())
        {
            eprintln!("Cannot finish transfer report: {error}");
            return Some(EXIT_FAILURE);
        }
    }
    print!("{message}");

    if let Err(error) = result {
        eprintln!("Transfer failed: {error}");
        return Some(EXIT_FAILURE);
    }
    Some(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(values: &[&str]) -> impl Iterator<Item = OsString> {
        values
            .iter()
            .map(OsString::from)
            .collect::<Vec<_>>()
            .into_iter()
    }

    #[test]
    fn rejects_conflicting_and_incomplete_commands() {
        for values in [
            vec!["--send"],
            vec!["--receive-once"],
            vec!["--send", "a", "--receive-once"],
            vec!["--send", "a", "--destination", "b"],
            vec!["--send", "a", "--config", "b", "--config", "c"],
        ] {
            assert!(parse(args(&values)).is_err());
        }
    }

    #[test]
    fn supports_unicode_and_spaces_without_command_interpretation() {
        let options = parse(args(&[
            "--send",
            "C:\\jogos\\BOPL Battle 1,4 GB.zip",
            "--config",
            "local.conf",
        ]))
        .unwrap();
        assert_eq!(
            options.operation,
            Operation::Send(PathBuf::from("C:\\jogos\\BOPL Battle 1,4 GB.zip"))
        );
    }

    #[test]
    fn report_output_never_overwrites_an_existing_file() {
        let root = std::env::temp_dir().join(format!("ql-transfer-report-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        let path = root.join("token.conf");
        fs::write(&path, b"existing contents").unwrap();
        assert!(report_file(Some(&path)).is_err());
        assert_eq!(fs::read(&path).unwrap(), b"existing contents");
        fs::remove_file(path).unwrap();
        fs::remove_dir(root).unwrap();
    }
}

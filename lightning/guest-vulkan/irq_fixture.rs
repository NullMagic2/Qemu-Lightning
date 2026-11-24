//! Module: lightning.guest_vulkan.irq_fixture
//! Purpose: Verify opt-in Venus interrupts, host sync events and bounded Lightning display output.
//! Created: 2026-10-09
//! Architecture: Real diskless QEMU; deterministic protocol peer, no GPU claims.
//! The peer also sends SYNC_EVENT frames around and after a reply (stage-3 W4),
//! and the device layout is checked with the default and a large aperture (D7)
//! and its MSI-X capability (D5); MSI delivery itself needs a guest.
#[path = "../core/src/memory_contract.rs"]
pub mod memory_contract;
#[path = "../core/src/scanout_contract.rs"]
pub mod scanout_contract;
#[allow(dead_code)]
mod transport_contract;
use std::{
    fs,
    io::{BufRead, BufReader, Read, Write},
    os::unix::net::{UnixListener, UnixStream},
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    thread,
    time::{Duration, Instant},
};
use transport_contract::*;
const MMIO: u64 = 0x10000000;
const REQUEST: u64 = 0x100000;
const RESPONSE: u64 = 0x110000;
const PCI_ADDRESS: u32 = 0x80002000;
const PCI_STATUS_INTERRUPT: u64 = 1 << 19;
const PCI_CAPABILITY_POINTER: u32 = 0x34;
const PCI_CAPABILITY_MSIX: u64 = 0x11;
const PCI_BAR0: u32 = 0x10;
const PCI_BAR_BYTES: u32 = 4;
const PCI_BAR_MEMORY_TYPE_64: u64 = 0x4;
/// Tokens the peer sends with the second OPEN (two before its reply, one
/// after) and the overflowing burst with the third.
const EARLY_TOKENS: [u32; 2] = [0x11, 0x12];
const LATE_TOKEN: u32 = 0x13;
const BURST_BASE: u32 = 0x100;
const TIMEOUT: Duration = Duration::from_secs(5);
const RETRY: Duration = Duration::from_millis(2);
struct Owned {
    child: Option<Child>,
    directory: PathBuf,
}
impl Drop for Owned {
    fn drop(&mut self) {
        if let Some(child) = &mut self.child {
            let _ = child.kill();
            let _ = child.wait();
        }
        let _ = fs::remove_dir_all(&self.directory);
    }
}
struct Guest(BufReader<UnixStream>);
impl Guest {
    fn command(&mut self, command: &str) -> Result<String, String> {
        writeln!(self.0.get_mut(), "{command}").map_err(|e| e.to_string())?;
        let mut reply = String::new();
        self.0.read_line(&mut reply).map_err(|e| e.to_string())?;
        if !reply.starts_with("OK") {
            return Err(format!("{command}: {reply}"));
        }
        Ok(reply)
    }
    fn number(&mut self, command: &str) -> Result<u64, String> {
        let reply = self.command(command)?;
        u64::from_str_radix(
            reply
                .split_whitespace()
                .nth(1)
                .ok_or("Missing value")?
                .trim_start_matches("0x"),
            16,
        )
        .map_err(|e| e.to_string())
    }
    fn read(&mut self, offset: usize) -> Result<u64, String> {
        self.number(&format!("readl {:#x}", MMIO + offset as u64))
    }
    fn write(&mut self, offset: usize, value: u64) -> Result<(), String> {
        self.command(&format!("writel {:#x} {value:#x}", MMIO + offset as u64))
            .map(|_| ())
    }
    fn asserted(&mut self) -> Result<bool, String> {
        self.command(&format!("outl 0xcf8 {:#x}", PCI_ADDRESS + 4))?;
        Ok(self.number("inl 0xcfc")? & PCI_STATUS_INTERRUPT != 0)
    }
    fn config(&mut self, offset: u32) -> Result<u64, String> {
        self.command(&format!("outl 0xcf8 {:#x}", PCI_ADDRESS + (offset & !3)))?;
        Ok(self.number("inl 0xcfc")? >> ((offset & 3) * 8))
    }
    fn set_config(&mut self, offset: u32, value: u64) -> Result<(), String> {
        self.command(&format!("outl 0xcf8 {:#x}", PCI_ADDRESS + offset))?;
        self.command(&format!("outl 0xcfc {value:#x}")).map(|_| ())
    }
    /// Size a BAR the way firmware does; returns (size, 64-bit).
    fn bar(&mut self, index: u32) -> Result<(u64, bool), String> {
        let register = PCI_BAR0 + index * PCI_BAR_BYTES;
        let original = self.config(register)? & 0xffff_ffff;
        self.set_config(register, 0xffff_ffff)?;
        let low = self.config(register)? & 0xffff_ffff;
        self.set_config(register, original)?;
        let wide = low & 0x6 == PCI_BAR_MEMORY_TYPE_64;
        let mut mask = low & !0xf;
        if wide {
            let high_register = register + PCI_BAR_BYTES;
            let high_original = self.config(high_register)? & 0xffff_ffff;
            self.set_config(high_register, 0xffff_ffff)?;
            mask |= (self.config(high_register)? & 0xffff_ffff) << 32;
            self.set_config(high_register, high_original)?;
        } else if mask != 0 {
            mask |= 0xffff_ffff_0000_0000;
        }
        Ok(((!mask).wrapping_add(1), wide))
    }
    /// The MSI-X capability: (table size, table BAR).
    fn msix(&mut self) -> Result<Option<(u64, u64)>, String> {
        let mut next = self.config(PCI_CAPABILITY_POINTER)? & 0xfc;
        while next != 0 {
            let id = self.config(next as u32)? & 0xff;
            if id == PCI_CAPABILITY_MSIX {
                let control = (self.config(next as u32 + 2)? & 0xffff) + 1;
                let table = self.config(next as u32 + 4)? & 0x7;
                return Ok(Some((control & 0x7ff, table)));
            }
            next = (self.config(next as u32 + 1)? & 0xfc) as u64;
        }
        Ok(None)
    }
    /// Wait until the event FIFO reports a pending token (or overflow).
    fn await_events(&mut self) -> Result<(), String> {
        let deadline = Instant::now() + TIMEOUT;
        while self.read(MMIO_REG_IRQ)? & MMIO_SYNC_EVENT_IRQ as u64 == 0 {
            if Instant::now() >= deadline {
                return Err("Sync events never arrived".into());
            }
            thread::sleep(RETRY);
        }
        Ok(())
    }
    fn submit(&mut self) -> Result<(), String> {
        let packet = Header::request(OPEN, 0).to_le_bytes();
        let hex: String = packet.iter().map(|byte| format!("{byte:02x}")).collect();
        self.command(&format!("write {REQUEST:#x} {HEADER_BYTES:#x} 0x{hex}"))?;
        for (offset, value) in [
            (MMIO_REG_REQUEST_LO, REQUEST),
            (MMIO_REG_REQUEST_BYTES, HEADER_BYTES as u64),
            (MMIO_REG_RESPONSE_LO, RESPONSE),
            (MMIO_REG_RESPONSE_CAPACITY, HEADER_BYTES as u64),
            (MMIO_REG_COMMAND, MMIO_SUBMIT as u64),
        ] {
            self.write(offset, value)?;
        }
        let deadline = Instant::now() + TIMEOUT;

        while self.read(MMIO_REG_STATUS)? != MMIO_COMPLETE as u64 {
            if Instant::now() >= deadline {
                return Err("Completion deadline".into());
            }
            thread::sleep(RETRY);
        }
        Ok(())
    }
}
fn peer(listener: UnixListener) -> Result<(), String> {
    listener.set_nonblocking(true).map_err(|e| e.to_string())?;
    let deadline = Instant::now() + TIMEOUT;
    let (mut stream, _) = loop {
        match listener.accept() {
            Ok(connection) => break connection,
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock && Instant::now() < deadline => {
                thread::sleep(RETRY)
            }
            Err(e) => return Err(e.to_string()),
        }
    };
    stream
        .set_read_timeout(Some(TIMEOUT))
        .map_err(|e| e.to_string())?;
    let event = |context: u64, token: u32| Header {
        value: token as u64,
        status: STATUS_SUCCESS,
        ..Header::request(SYNC_EVENT, context)
    };
    let mut opens = 0;
    loop {
        let mut packet = [0; HEADER_BYTES];

        match stream.read_exact(&mut packet) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(e) => return Err(e.to_string()),
        }
        let mut request = Header::from_le_bytes(&packet).ok_or("Invalid peer packet")?;

        if request.operation != OPEN || request.payload_bytes != 0 {
            return Err("Unexpected peer request".into());
        }
        request.context = 1;
        opens += 1;
        // Events may come before the reply (the peer's notifier is another
        // thread), after it, and in bursts larger than QEMU's FIFO.
        let (before, after): (Vec<u32>, Vec<u32>) = match opens {
            2 => (EARLY_TOKENS.to_vec(), vec![LATE_TOKEN]),
            3 => ((0..=EVENT_QUEUE_ENTRIES as u32).map(|i| BURST_BASE + i).collect(), vec![]),
            _ => (vec![], vec![]),
        };
        let mut frames = Vec::new();
        for token in before {
            frames.extend(event(1, token).to_le_bytes());
        }
        frames.extend(request.to_le_bytes());
        for token in after {
            frames.extend(event(1, token).to_le_bytes());
        }
        stream.write_all(&frames).map_err(|e| e.to_string())?;
    }
}

/// Stage-3 W4: tokens queue in order around a reply; the source masks,
/// acknowledges and overflows like the others.
fn check_events(guest: &mut Guest) -> Result<(), String> {
    guest.submit()?;
    guest.write(MMIO_REG_IRQ, MMIO_COMPLETION_IRQ as u64)?;
    guest.await_events()?;
    if guest.asserted()? {
        return Err("Masked sync events asserted an interrupt".into());
    }
    guest.write(MMIO_REG_IRQ_ENABLE, (MMIO_COMPLETION_IRQ | MMIO_SYNC_EVENT_IRQ) as u64)?;
    if !guest.asserted()? {
        return Err("Pending sync events did not assert INTx on enable".into());
    }
    // An acknowledge alone does not drop the source: tokens must be popped.
    guest.write(MMIO_REG_IRQ, MMIO_SYNC_EVENT_IRQ as u64)?;
    if !guest.asserted()? {
        return Err("Acknowledge dropped unread sync events".into());
    }
    let mut tokens = Vec::new();
    let deadline = Instant::now() + TIMEOUT;
    while tokens.len() < EARLY_TOKENS.len() + 1 {
        match guest.read(MMIO_REG_EVENT)? {
            0 if Instant::now() < deadline => thread::sleep(RETRY),
            0 => return Err(format!("Missing sync events: {tokens:?}")),
            token => tokens.push(token as u32),
        }
    }
    if tokens != [EARLY_TOKENS[0], EARLY_TOKENS[1], LATE_TOKEN]
        || guest.read(MMIO_REG_EVENT)? != 0
        || guest.read(MMIO_REG_EVENT_STATUS)? != 0
    {
        return Err(format!("Sync events out of order or extra: {tokens:?}"));
    }
    if guest.read(MMIO_REG_IRQ)? != 0 || guest.asserted()? {
        return Err("Drained sync events left the interrupt pending".into());
    }
    // One more than the FIFO holds: the oldest stay, overflow is reported.
    guest.submit()?;
    guest.write(MMIO_REG_IRQ, MMIO_COMPLETION_IRQ as u64)?;
    guest.await_events()?;
    if guest.read(MMIO_REG_EVENT_STATUS)? != MMIO_EVENT_OVERFLOW as u64 {
        return Err("A full FIFO did not report overflow".into());
    }
    for index in 0..EVENT_QUEUE_ENTRIES as u64 {
        if guest.read(MMIO_REG_EVENT)? != BURST_BASE as u64 + index {
            return Err("Overflow lost the oldest tokens".into());
        }
    }
    if guest.read(MMIO_REG_EVENT)? != 0 || !guest.asserted()? {
        return Err("Overflow did not keep the interrupt pending".into());
    }
    guest.write(MMIO_REG_EVENT_STATUS, MMIO_EVENT_OVERFLOW as u64)?;
    if guest.read(MMIO_REG_IRQ)? != 0 || guest.asserted()? {
        return Err("Clearing overflow left the interrupt pending".into());
    }
    guest.write(MMIO_REG_IRQ_ENABLE, MMIO_COMPLETION_IRQ as u64)?;
    Ok(())
}

/// Stage-3 D5/D7: one MSI-X vector in its own BAR; the aperture is the
/// 32-bit BAR3 by default and a 64-bit BAR1 when larger.
fn check_layout(guest: &mut Guest, aperture_mib: u64) -> Result<(), String> {
    let large = aperture_mib != APERTURE_BYTES >> MIB_SHIFT;
    let capabilities = guest.read(MMIO_REG_CAPABILITIES)?;
    let bar = if large { LARGE_APERTURE_BAR } else { APERTURE_BAR };
    if (capabilities & MMIO_LARGE_APERTURE as u64 != 0) != large
        || guest.read(MMIO_REG_APERTURE_BAR)? != bar as u64
        || guest.read(MMIO_REG_APERTURE_MIB)? != aperture_mib
    {
        return Err("Aperture registers disagree with the device option".into());
    }
    if guest.bar(bar)? != (aperture_mib << MIB_SHIFT, large) {
        return Err(format!("Aperture BAR{bar} has the wrong size or width"));
    }
    // With a large aperture BAR3 holds only the MSI-X table, never a window.
    if large && guest.bar(APERTURE_BAR)?.0 >= APERTURE_BYTES {
        return Err("A large aperture left the legacy BAR3 window".into());
    }
    match guest.msix()? {
        Some((1, table)) if table == if large { 3 } else { 2 } => Ok(()),
        other => Err(format!("Unexpected MSI-X capability {other:?}")),
    }
}
/// Exercise only this fixture's synthetic console; no guest disk or user UI.
fn qmp(stream: &mut BufReader<UnixStream>, command: &str) -> Result<(), String> {
    writeln!(stream.get_mut(), "{command}").map_err(|e| e.to_string())?;
    loop {
        let mut reply = String::new();

        if stream.read_line(&mut reply).map_err(|e| e.to_string())? == 0 {
            return Err("QMP closed".into());
        }
        if reply.contains("\"error\"") {
            return Err(reply);
        }
        if reply.contains("\"return\"") {
            return Ok(());
        }
    }
}

fn check_scanout(
    guest: &mut Guest,
    qmp_path: &Path,
    directory: &Path,
    enabled: bool,
) -> Result<(), String> {
    use scanout_contract::*;
    // A BAR is aligned to its size; the 64 MiB guest RAM stays below it.
    const VIDEO_BASE: u64 = memory_contract::MEMORY_BYTES;
    const PCI_BAR_BASE: u32 = 0x10;
    const PCI_BAR_BYTES: u32 = 4;
    const VIDEO_BAR_REGISTER: u32 = PCI_BAR_BASE + memory_contract::MEMORY_BAR * PCI_BAR_BYTES;
    const VIDEO_BYTES: u64 = memory_contract::MEMORY_BYTES;
    const WIDTH: u32 = 16;
    const HEIGHT: u32 = 16;
    const STRIDE: u32 = WIDTH * PIXEL_BYTES + 16;
    const OFFSET: u32 = 4096;
    const RESIZED_WIDTH: u32 = WIDTH / 2;
    const PADDING_SENTINEL: u8 = 0xee;
    const RGB_BYTES: usize = 3;
    const RED: u8 = 0xc0;
    const GREEN: u8 = 0x40;
    const BLUE: u8 = 0x20;

    if !enabled {
        guest.write(REG_COMMAND as usize, PRESENT as u64)?;
        if guest.read(REG_MAGIC as usize)? != 0 || guest.read(REG_STATUS as usize)? != 0 {
            return Err("Default-off scanout exposed registers".into());
        }
        return Ok(());
    }
    if guest.read(REG_MAGIC as usize)? != MAGIC as u64
        || guest.read(REG_VERSION as usize)? != VERSION as u64
    {
        return Err("Missing scanout contract".into());
    }
    guest.command(&format!(
        "outl 0xcf8 {:#x}",
        PCI_ADDRESS + VIDEO_BAR_REGISTER
    ))?;
    guest.command(&format!("outl 0xcfc {VIDEO_BASE:#x}"))?;
    // BAR4 is 64-bit: its high dword is the next BAR register.
    guest.command(&format!(
        "outl 0xcf8 {:#x}",
        PCI_ADDRESS + VIDEO_BAR_REGISTER + PCI_BAR_BYTES
    ))?;
    guest.command("outl 0xcfc 0")?;
    let mut pixels = vec![PADDING_SENTINEL; (STRIDE * HEIGHT) as usize];

    for row in 0..HEIGHT {
        for column in 0..WIDTH {
            let offset = (row * STRIDE + column * PIXEL_BYTES) as usize;
            pixels[offset..offset + PIXEL_BYTES as usize].copy_from_slice(&[
                BLUE,
                GREEN + row as u8,
                RED + column as u8,
                0,
            ]);
        }
    }
    let hex: String = pixels.iter().map(|byte| format!("{byte:02x}")).collect();
    guest.command(&format!(
        "write {:#x} {:#x} 0x{hex}",
        VIDEO_BASE + OFFSET as u64,
        pixels.len()
    ))?;
    for (register, value) in [
        (REG_WIDTH, WIDTH),
        (REG_HEIGHT, HEIGHT),
        (REG_STRIDE, STRIDE),
        (REG_OFFSET, OFFSET),
    ] {
        guest.write(register as usize, value as u64)?;
    }
    guest.write(REG_COMMAND as usize, PRESENT as u64)?;
    if guest.read(REG_STATUS as usize)? != ACTIVE as u64 {
        return Err("Valid frame rejected".into());
    }
    let socket = UnixStream::connect(qmp_path).map_err(|e| e.to_string())?;
    socket
        .set_read_timeout(Some(TIMEOUT))
        .map_err(|e| e.to_string())?;
    let mut monitor = BufReader::new(socket);
    monitor
        .read_line(&mut String::new())
        .map_err(|e| e.to_string())?;
    qmp(&mut monitor, r#"{"execute":"qmp_capabilities"}"#)?;
    let path = directory.join("frame.ppm");
    let path_text = path.to_str().ok_or("Non-UTF8 fixture path")?;

    if !path_text.is_ascii()
        || path_text
            .chars()
            .any(|c| c.is_control() || c == '"' || c == '\\')
    {
        return Err("Fixture path cannot be used in QMP".into());
    }
    let dump = format!(
        r#"{{"execute":"screendump","arguments":{{"filename":"{path_text}","device":"fixture-display","format":"ppm"}}}}"#
    );
    let expected_frame = |width: u32| {
        let mut frame = format!("P6\n{width} {HEIGHT}\n255\n").into_bytes();

        for row in 0..HEIGHT {
            for column in 0..width {
                frame.extend([RED + column as u8, GREEN + row as u8, BLUE]);
            }
        }
        frame
    };
    let expected = expected_frame(WIDTH);
    qmp(&mut monitor, &dump)?;
    if fs::read(&path).map_err(|e| e.to_string())? != expected {
        return Err("Scanout pixels differ".into());
    }
    // Rejected geometry must leave the complete previous surface intact.
    for (register, value) in [
        (REG_STRIDE, WIDTH * PIXEL_BYTES - PIXEL_BYTES),
        (REG_OFFSET, VIDEO_BYTES as u32),
    ] {
        guest.write(register as usize, value as u64)?;
        guest.write(REG_COMMAND as usize, PRESENT as u64)?;
        if guest.read(REG_STATUS as usize)? != INVALID as u64 {
            return Err("Invalid frame accepted".into());
        }
        qmp(&mut monitor, &dump)?;
        if fs::read(&path).map_err(|e| e.to_string())? != expected {
            return Err("Rejected frame changed display".into());
        }
        guest.write(REG_STRIDE as usize, STRIDE as u64)?;
        guest.write(REG_OFFSET as usize, OFFSET as u64)?;
    }
    guest.write(REG_WIDTH as usize, RESIZED_WIDTH as u64)?;
    guest.write(REG_COMMAND as usize, PRESENT as u64)?;
    qmp(&mut monitor, &dump)?;
    if fs::read(&path).map_err(|e| e.to_string())? != expected_frame(RESIZED_WIDTH) {
        return Err("Resized output differs".into());
    }
    guest.write(REG_WIDTH as usize, WIDTH as u64)?;
    guest.write(REG_COMMAND as usize, PRESENT as u64)?;
    let mut blank = format!("P6\n{WIDTH} {HEIGHT}\n255\n").into_bytes();
    blank.resize(blank.len() + (WIDTH * HEIGHT) as usize * RGB_BYTES, 0);
    guest.write(REG_COMMAND as usize, DISABLE as u64)?;
    qmp(&mut monitor, &dump)?;
    if fs::read(&path).map_err(|e| e.to_string())? != blank {
        return Err("Disable retained pixels".into());
    }
    guest.write(REG_COMMAND as usize, PRESENT as u64)?;
    qmp(&mut monitor, r#"{"execute":"system_reset"}"#)?;
    qmp(&mut monitor, &dump)?;
    if fs::read(&path).map_err(|e| e.to_string())? != blank {
        return Err("Reset retained pixels".into());
    }
    Ok(())
}

/// The device runs as in a started VM: the qtest accelerator executes no guest
/// code, and the Venus ring IOThread (paused while the VM is stopped) is live.
fn check(
    qemu: &Path,
    firmware: &Path,
    scanout: bool,
    aperture_mib: u64,
    ring_iothread: bool,
) -> Result<(), String> {
    let directory = std::env::temp_dir().join(format!("qlv-irq-{}", std::process::id()));
    fs::create_dir(&directory).map_err(|e| e.to_string())?;
    let mut owned = Owned {
        child: None,
        directory,
    };
    let endpoint = owned.directory.join("venus.sock");
    let qtest = owned.directory.join("qtest.sock");
    let qmp_path = owned.directory.join("qmp.sock");
    let listener = UnixListener::bind(&endpoint).map_err(|e| e.to_string())?;
    owned.child = Some(
        Command::new(qemu)
            .args([
                "-machine",
                "q35",
                "-accel",
                "qtest",
                "-m",
                "64",
                "-nodefaults",
                "-display",
                "none",
                "-L",
            ])
            .arg(firmware)
            .arg("-device")
            .arg(format!(
                "lightning-pci,id=fixture-display,addr=4.0,scanout={},venus-socket={},aperture-mib={},ring-iothread={}",
                if scanout { "on" } else { "off" },
                endpoint.display(),
                aperture_mib,
                if ring_iothread { "on" } else { "off" }
            ))
            .arg("-qmp")
            .arg(format!("unix:{},server=on,wait=off", qmp_path.display()))
            .arg("-qtest")
            .arg(format!("unix:{},server=on,wait=off", qtest.display()))
            .args(["-qtest-log", "/dev/null"])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::inherit())
            .spawn()
            .map_err(|e| e.to_string())?,
    );
    let worker = thread::spawn(move || peer(listener));
    let result = (|| {
        let deadline = Instant::now() + TIMEOUT;
        let stream = loop {
            match UnixStream::connect(&qtest) {
                Ok(s) => break s,
                Err(_) if Instant::now() < deadline => thread::sleep(RETRY),
                Err(e) => return Err(e.to_string()),
            }
        };
        stream
            .set_read_timeout(Some(TIMEOUT))
            .map_err(|e| e.to_string())?;
        let mut guest = Guest(BufReader::new(stream));

        for (offset, value) in [(0x10, MMIO), (4, 6)] {
            guest.command(&format!("outl 0xcf8 {:#x}", PCI_ADDRESS + offset))?;
            guest.command(&format!("outl 0xcfc {value:#x}"))?;
        }
        let capabilities = guest.read(MMIO_REG_CAPABILITIES)?;
        if capabilities & (MMIO_COMPLETION_IRQ | MMIO_SYNC_EVENTS) as u64
            != (MMIO_COMPLETION_IRQ | MMIO_SYNC_EVENTS) as u64
            || guest.read(MMIO_REG_IRQ_ENABLE)? != 0
        {
            return Err("Missing opt-in capability/default mask".into());
        }
        check_layout(&mut guest, aperture_mib)?;
        guest.submit()?;
        if guest.read(MMIO_REG_IRQ)? != 1 || guest.asserted()? {
            return Err("Masked completion asserted an interrupt".into());
        }
        guest.write(MMIO_REG_IRQ_ENABLE, 1)?;
        if !guest.asserted()? {
            return Err("Pending completion did not assert INTx on enable".into());
        }
        guest.write(MMIO_REG_IRQ, 2)?;
        if !guest.asserted()? {
            return Err("Unrelated W1C acknowledged completion".into());
        }
        guest.write(MMIO_REG_IRQ, 1)?;
        if guest.asserted()? {
            return Err("Venus acknowledgement left INTx asserted".into());
        }
        check_events(&mut guest)?;
        guest.submit()?;
        guest.write(MMIO_REG_IRQ_ENABLE, 0)?;
        if guest.asserted()? || guest.read(MMIO_REG_IRQ)? != 1 {
            return Err("Mask did not preserve pending cause".into());
        }
        guest.write(MMIO_REG_COMMAND, MMIO_RESET as u64)?;
        if guest.asserted()? || guest.read(MMIO_REG_IRQ)? != 0 {
            return Err("Reset retained stale interrupt".into());
        }
        check_scanout(&mut guest, &qmp_path, &owned.directory, scanout)?;
        Ok(())
    })();
    drop(owned);
    let peer_result = worker.join().map_err(|_| "Peer panicked")?;
    result.and(peer_result)
}
fn main() {
    let args: Vec<_> = std::env::args_os().collect();

    if args.len() != 3 {
        eprintln!("Usage: venus-irq-fixture QEMU FIRMWARE");
        std::process::exit(1);
    }
    let default_mib = APERTURE_BYTES >> MIB_SHIFT;
    let (qemu, firmware) = (Path::new(&args[1]), Path::new(&args[2]));
    if let Err(e) = check(qemu, firmware, false, default_mib, true)
        .and_then(|_| check(qemu, firmware, true, default_mib, true))
        .and_then(|_| check(qemu, firmware, false, 2 * default_mib, true))
        .and_then(|_| check(qemu, firmware, true, default_mib, false))
    {
        eprintln!("{e}");
        std::process::exit(1);
    }
    println!(
        "{{\"scanout_pixels_bounds_reset_verified\":true,\"opt_in_completion_intx_verified\":true,\"masked_default_verified\":true,\"reset_stale_irq_revocation_verified\":true,\"sync_events_order_mask_overflow_verified\":true,\"aperture_and_msix_layout_verified\":true,\"msix_delivery_verified\":false,\"native_windows_event_verified\":false,\"gpu_execution_measured\":false}}"
    );
}

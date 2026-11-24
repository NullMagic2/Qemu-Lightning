//! Module: lightning.transfers.apps_wire
//! Purpose: Bound the application-window stream shared by the host and guest agent.
//! Created: 2026-10-08
//! Architecture: Authentication belongs to the existing tools connection; all lengths
//! and image dimensions are checked before allocating or forwarding guest data.
use std::io::{self, Read, Write};
pub const OPERATION: u8 = 5;
pub const MAX_WINDOWS: usize = 32;
pub const MAX_SHARED: usize = 4;
pub const MAX_WIDTH: u32 = 1280;
pub const MAX_HEIGHT: u32 = 720;
pub const MAX_INPUT: usize = 128;
pub const MAX_TEXT: usize = 2048;
#[cfg(windows)]
pub const TICK_MS: u64 = 100;
#[derive(Clone, Debug)]
#[cfg_attr(not(windows), derive(serde::Serialize, serde::Deserialize))]
pub struct AppWindow {
    pub id: u64,
    pub application: String,
    pub title: String,
}
#[derive(Clone, Debug)]
#[cfg_attr(not(windows), derive(serde::Serialize, serde::Deserialize))]
pub struct Frame {
    pub id: u64,
    pub width: u32,
    pub height: u32,
    pub rgb: Vec<u8>,
}
#[derive(Clone, Debug)]
#[cfg_attr(not(windows), derive(serde::Serialize, serde::Deserialize))]
pub struct Input {
    pub id: u64,
    pub kind: u32,
    pub a: u32,
    pub b: u32,
}
pub const POINTER: u32 = 1;
pub const BUTTON_DOWN: u32 = 2;
pub const BUTTON_UP: u32 = 3;
pub const KEY_DOWN: u32 = 4;
pub const KEY_UP: u32 = 5;
pub const TEXT: u32 = 6;
pub const RELEASE: u32 = 7;
pub const WHEEL: u32 = 8;
pub const RESIZE: u32 = 9;
pub fn invalid() -> io::Error {
    io::Error::other("Invalid application stream")
}
pub fn number(reader: &mut impl Read) -> io::Result<u32> {
    let mut bytes = [0; 4];
    reader.read_exact(&mut bytes)?;
    Ok(u32::from_le_bytes(bytes))
}
pub fn put(writer: &mut impl Write, value: u32) -> io::Result<()> {
    writer.write_all(&value.to_le_bytes())
}
pub fn identity(reader: &mut impl Read) -> io::Result<u64> {
    let mut bytes = [0; 8];
    reader.read_exact(&mut bytes)?;
    Ok(u64::from_le_bytes(bytes))
}
pub fn put_identity(writer: &mut impl Write, value: u64) -> io::Result<()> {
    writer.write_all(&value.to_le_bytes())
}
pub fn count(reader: &mut impl Read, maximum: usize) -> io::Result<usize> {
    let n = number(reader)? as usize;

    if n > maximum {
        return Err(invalid());
    }
    Ok(n)
}
#[cfg(any(not(windows), test))]
pub fn text(reader: &mut impl Read) -> io::Result<String> {
    let size = count(reader, MAX_TEXT)?;
    let mut bytes = vec![0; size];
    reader.read_exact(&mut bytes)?;
    String::from_utf8(bytes).map_err(io::Error::other)
}
#[cfg(any(windows, test))]
pub fn put_text(writer: &mut impl Write, text: &str) -> io::Result<()> {
    if text.len() > MAX_TEXT {
        return Err(invalid());
    }
    put(writer, text.len() as u32)?;
    writer.write_all(text.as_bytes())
}
#[cfg(any(not(windows), test))]
pub fn read_windows(reader: &mut impl Read) -> io::Result<Vec<AppWindow>> {
    let mut windows = Vec::new();

    for _ in 0..count(reader, MAX_WINDOWS)? {
        let window = AppWindow {
            id: identity(reader)?,
            application: text(reader)?,
            title: text(reader)?,
        };

        if window.id == 0
            || window.application.is_empty()
            || windows.iter().any(|w: &AppWindow| w.id == window.id)
        {
            return Err(invalid());
        }
        windows.push(window);
    }
    Ok(windows)
}
#[cfg(any(windows, test))]
pub fn write_windows(writer: &mut impl Write, windows: &[AppWindow]) -> io::Result<()> {
    if windows.len() > MAX_WINDOWS {
        return Err(invalid());
    }
    put(writer, windows.len() as u32)?;
    for window in windows {
        put_identity(writer, window.id)?;
        put_text(writer, &window.application)?;
        put_text(writer, &window.title)?;
    }
    Ok(())
}
pub fn pixel_bytes(width: u32, height: u32) -> io::Result<usize> {
    if width == 0 || height == 0 || width > MAX_WIDTH || height > MAX_HEIGHT {
        return Err(invalid());
    }
    Ok(width as usize * height as usize * 3)
}
/// Run lengths cannot exceed the declared pixel count; expansion is at most 720p.
pub fn read_frame(reader: &mut impl Read) -> io::Result<Frame> {
    let id = identity(reader)?;
    let width = number(reader)?;
    let height = number(reader)?;
    let size = pixel_bytes(width, height)?;
    let mut rgb = Vec::with_capacity(size);

    while rgb.len() < size {
        let n = number(reader)? as usize;

        if n == 0 || n > (size - rgb.len()) / 3 {
            return Err(invalid());
        }
        let mut pixel = [0; 3];
        reader.read_exact(&mut pixel)?;
        for _ in 0..n {
            rgb.extend_from_slice(&pixel);
        }
    }
    Ok(Frame {
        id,
        width,
        height,
        rgb,
    })
}
#[cfg(any(windows, test))]
pub fn write_frame(writer: &mut impl Write, frame: &Frame) -> io::Result<()> {
    if frame.rgb.len() != pixel_bytes(frame.width, frame.height)? {
        return Err(invalid());
    }
    put_identity(writer, frame.id)?;
    put(writer, frame.width)?;
    put(writer, frame.height)?;
    let mut start = 0;

    while start < frame.rgb.len() {
        let mut end = start + 3;

        while end < frame.rgb.len() && frame.rgb[start..start + 3] == frame.rgb[end..end + 3] {
            end += 3;
        }
        put(writer, ((end - start) / 3) as u32)?;
        writer.write_all(&frame.rgb[start..start + 3])?;
        start = end;
    }
    Ok(())
}
pub fn valid_input(input: &Input) -> bool {
    match input.kind {
        POINTER => input.a <= 65535 && input.b <= 65535,
        BUTTON_DOWN | BUTTON_UP => matches!(input.a, 1..=3),
        KEY_DOWN | KEY_UP => input.a > 0 && input.a < 256,
        TEXT => char::from_u32(input.a).is_some(),
        RELEASE => true,
        WHEEL => input.a <= 1 && (1..=10).contains(&input.b),
        RESIZE => (100..=4096).contains(&input.a) && (100..=2160).contains(&input.b),
        _ => false,
    }
}
#[cfg(any(windows, test))]
pub fn read_inputs(reader: &mut impl Read) -> io::Result<Vec<Input>> {
    let mut inputs = Vec::new();

    for _ in 0..count(reader, MAX_INPUT)? {
        let input = Input {
            id: identity(reader)?,
            kind: number(reader)?,
            a: number(reader)?,
            b: number(reader)?,
        };

        if !valid_input(&input) {
            return Err(invalid());
        }
        inputs.push(input);
    }
    Ok(inputs)
}
#[cfg(any(not(windows), test))]
pub fn write_inputs(writer: &mut impl Write, inputs: &[Input]) -> io::Result<()> {
    if inputs.len() > MAX_INPUT {
        return Err(invalid());
    }
    put(writer, inputs.len() as u32)?;
    for input in inputs {
        if !valid_input(input) {
            return Err(invalid());
        }
        put_identity(writer, input.id)?;
        put(writer, input.kind)?;
        put(writer, input.a)?;
        put(writer, input.b)?;
    }
    Ok(())
}

#[cfg(test)]
mod guest_protocol_tests {
    use super::*;
    #[test]
    fn window_and_input_round_trip() {
        let windows = vec![AppWindow {
            id: 42,
            application: "C:\\Test.exe".into(),
            title: "Fixture".into(),
        }];
        let mut bytes = Vec::new();
        write_windows(&mut bytes, &windows).unwrap();
        let restored = read_windows(&mut bytes.as_slice()).unwrap();
        assert_eq!(restored[0].title, windows[0].title);
        let input = Input {
            id: 42,
            kind: POINTER,
            a: 10,
            b: 20,
        };
        bytes.clear();
        write_inputs(&mut bytes, &[input]).unwrap();
        let restored = read_inputs(&mut bytes.as_slice()).unwrap();
        assert_eq!((restored[0].a, restored[0].b), (10, 20));
    }
}

//! Module: lightning.transfers.native_tests
//! Purpose: Exercise real Win32 image capture, cancellation and worker restart.
//! Created: 2026-10-08
//! Architecture: Runs in a disposable Windows test guest using the built helper.

use super::*;
use std::{ffi::c_void, ptr, sync::atomic::AtomicBool, time::Duration};
type Handle = *mut c_void;
const WM_CLOSE: u32 = 0x10;
const WM_DESTROY: u32 = 2;
const FIXTURE_WIDTH: i32 = 160;
const FIXTURE_HEIGHT: i32 = 100;
const CAPTURE_DEADLINE: Duration = Duration::from_secs(2);
#[repr(C)]
struct Class {
    style: u32,
    procedure: Option<unsafe extern "system" fn(Handle, u32, usize, isize) -> isize>,
    class_extra: i32,
    window_extra: i32,
    instance: Handle,
    icon: Handle,
    cursor: Handle,
    background: Handle,
    menu: *const u16,
    name: *const u16,
}
use super::window::Message;
#[link(name = "user32")]
extern "system" {
    fn RegisterClassW(class: *const Class) -> u16;
    fn CreateWindowExW(
        ex: u32,
        class: *const u16,
        title: *const u16,
        style: u32,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        parent: Handle,
        menu: Handle,
        instance: Handle,
        param: Handle,
    ) -> Handle;
    fn DefWindowProcW(w: Handle, m: u32, a: usize, b: isize) -> isize;
    fn GetMessageW(message: *mut Message, w: Handle, min: u32, max: u32) -> i32;
    fn DispatchMessageW(message: *const Message) -> isize;
    fn PostMessageW(w: Handle, m: u32, a: usize, b: isize) -> i32;
    fn PostQuitMessage(code: i32);
    fn UpdateWindow(window: Handle) -> i32;
}
#[link(name = "kernel32")]
extern "system" {
    fn GetModuleHandleW(name: *const u16) -> Handle;
}
unsafe extern "system" fn procedure(w: Handle, m: u32, a: usize, b: isize) -> isize {
    if m == WM_DESTROY {
        PostQuitMessage(0);
        return 0;
    }
    DefWindowProcW(w, m, a, b)
}
#[test]
#[ignore = "Requires an interactive Windows desktop; session zero cannot capture windows"]
fn native_capture_roundtrip_and_worker_restart() {
    assert!(
        std::env::var_os("QEMU_CAPTURE_TEST_HELPER").is_some(),
        "Set QEMU_CAPTURE_TEST_HELPER to the built helper"
    );
    let (send, recv) = mpsc::channel();
    let ui = thread::spawn(move || unsafe {
        let name: Vec<_> = "Qemu capture validation"
            .encode_utf16()
            .chain(Some(0))
            .collect();
        let instance = GetModuleHandleW(ptr::null());
        let class = Class {
            style: 0,
            procedure: Some(procedure),
            class_extra: 0,
            window_extra: 0,
            instance,
            icon: ptr::null_mut(),
            cursor: ptr::null_mut(),
            background: 6usize as Handle,
            menu: ptr::null(),
            name: name.as_ptr(),
        };
        assert_ne!(RegisterClassW(&class), 0);
        const VISIBLE_OVERLAPPED: u32 = 0x10cf0000;
        let window = CreateWindowExW(
            0,
            name.as_ptr(),
            name.as_ptr(),
            VISIBLE_OVERLAPPED,
            0,
            0,
            FIXTURE_WIDTH,
            FIXTURE_HEIGHT,
            ptr::null_mut(),
            ptr::null_mut(),
            instance,
            ptr::null_mut(),
        );
        assert!(!window.is_null());
        let static_class: Vec<_> = "STATIC".encode_utf16().chain(Some(0)).collect();
        const CHILD_VISIBLE: u32 = 0x50000000;
        let child = CreateWindowExW(
            0,
            static_class.as_ptr(),
            name.as_ptr(),
            CHILD_VISIBLE,
            8,
            8,
            120,
            40,
            window,
            ptr::null_mut(),
            instance,
            ptr::null_mut(),
        );
        assert!(!child.is_null());
        UpdateWindow(window);
        UpdateWindow(child);

        send.send(window as u64).unwrap();
        let mut message: Message = std::mem::zeroed();

        while GetMessageW(&mut message, ptr::null_mut(), 0, 0) > 0 {
            DispatchMessageW(&message);
        }
    });
    let id = recv.recv_timeout(CAPTURE_DEADLINE).unwrap();
    let enabled = AtomicBool::new(true);
    let mut capture = capture_worker::Capture::default();
    let first = capture.frame(id, &enabled).unwrap().unwrap();
    assert!(first.width > 0 && first.height > 0);
    assert_eq!(
        first.rgb.len(),
        first.width as usize * first.height as usize * 3
    );
    let next = capture.frame(id, &enabled).unwrap().unwrap();
    assert_eq!((next.width, next.height), (first.width, first.height));
    enabled.store(false, std::sync::atomic::Ordering::Release);
    assert!(capture.frame(id, &enabled).unwrap().is_none());
    enabled.store(true, std::sync::atomic::Ordering::Release);
    let recovered = capture.frame(id, &enabled).unwrap().unwrap();
    assert_eq!(
        (recovered.width, recovered.height),
        (first.width, first.height)
    );
    capture.stop();
    unsafe {
        PostMessageW(id as Handle, WM_CLOSE, 0, 0);
    }
    ui.join().unwrap();
}

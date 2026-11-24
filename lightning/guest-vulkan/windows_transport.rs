//! Module: lightning.guest_vulkan.windows_transport
//! Purpose: Validate and sequence native Venus kernel transport operations.
//! Created: 2026-10-09
//! Architecture: Mesa owns public Vulkan objects. A small WDK shim supplies
//! D3DKMT Escape and heap calls; this module owns packet policy and error handling.

use crate::transport_contract as wire;
use core::ffi::c_void;
pub const VK_SUCCESS: i32 = 0;
pub const VK_TIMEOUT: i32 = 2;
pub const VK_ERROR_OUT_OF_HOST_MEMORY: i32 = -1;
pub const VK_ERROR_INITIALIZATION_FAILED: i32 = -3;
pub const VK_ERROR_DEVICE_LOST: i32 = -4;
pub const VK_ERROR_FEATURE_NOT_PRESENT: i32 = -8;
pub const VK_ERROR_MEMORY_MAP_FAILED: i32 = -5;

pub type Invoke = unsafe extern "C" fn(
    cookie: *mut c_void,
    header: *mut wire::Header,
    input: *const c_void,
    output: *mut c_void,
    output_bytes: usize,
) -> i32;

/// The caller serializes mutable connection lifetime against all operations.
/// Invoke copies each input before crossing into the kernel. No input pointer
/// is retained; only the Windows ABI shim sees process-local pointers.
#[repr(C)]
pub struct Connection {
    pub context: u64,
    pub cookie: *mut c_void,
    pub invoke: Option<Invoke>,
}

fn result(status: i32) -> i32 {
    match status {
        wire::STATUS_SUCCESS => VK_SUCCESS,
        wire::STATUS_TIMEOUT => VK_TIMEOUT,
        wire::STATUS_NO_MEMORY => VK_ERROR_OUT_OF_HOST_MEMORY,
        wire::STATUS_UNSUPPORTED => VK_ERROR_FEATURE_NOT_PRESENT,
        _ => VK_ERROR_DEVICE_LOST,
    }
}

impl Connection {
    unsafe fn execute(
        &self,
        header: &mut wire::Header,
        input: *const c_void,
        output: *mut c_void,
        output_bytes: usize,
    ) -> i32 {
        let Some(invoke) = self.invoke else {
            return VK_ERROR_DEVICE_LOST;
        };

        if !header.valid_request(header.payload_bytes as usize)
            || output_bytes > wire::MAX_PAYLOAD_BYTES
            || (header.payload_bytes != 0 && input.is_null())
            || (output_bytes != 0 && output.is_null())
        {
            return VK_ERROR_DEVICE_LOST;
        }
        let operation = header.operation;
        let outer = invoke(self.cookie, header, input, output, output_bytes);

        if outer != 0
            || header.magic != wire::MAGIC
            || header.version != wire::VERSION
            || header.operation != operation
            || header.reserved != 0
            || (operation != wire::OPEN && header.context != self.context)
        {
            return VK_ERROR_DEVICE_LOST;
        }
        result(header.status)
    }
}

/// Writes a fresh connection only after a real kernel context opens. The output
/// storage and service cookie must remain valid until close completes.
unsafe fn connection_open(
    connection: *mut Connection,
    cookie: *mut c_void,
    invoke: Option<Invoke>,
    aperture_bytes: *mut u64,
) -> i32 {
    let Some(connection) = connection.as_mut() else {
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    *connection = Connection {
        context: 0,
        cookie,
        invoke,
    };
    let mut header = wire::Header::request(wire::OPEN, 0);
    let status = connection.execute(&mut header, core::ptr::null(), core::ptr::null_mut(), 0);

    if status != VK_SUCCESS {
        return status;
    }
    if header.context == 0 {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if !aperture_bytes.is_null() {
        let capacity = if header.flags & wire::HOST_APERTURE_SIZE != 0 {
            header.bytes
        } else {
            wire::APERTURE_BYTES as u64
        };
        if capacity < wire::APERTURE_BYTES as u64
            || capacity > wire::MAX_APERTURE_BYTES as u64
            || !capacity.is_power_of_two()
        {
            connection.context = header.context;
            let _ = qlv_connection_close(connection);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        *aperture_bytes = capacity;
    }
    connection.context = header.context;
    VK_SUCCESS
}

#[no_mangle]
pub unsafe extern "C" fn qlv_connection_open(
    connection: *mut Connection,
    cookie: *mut c_void,
    invoke: Option<Invoke>,
) -> i32 {
    connection_open(connection, cookie, invoke, core::ptr::null_mut())
}

/// The ReactOS platform keeps the same Connection ABI while retaining the
/// kernel-validated PCI aperture capacity from OPEN for Mesa's map bounds.
#[no_mangle]
pub unsafe extern "C" fn qlv_connection_open_with_aperture(
    connection: *mut Connection,
    cookie: *mut c_void,
    invoke: Option<Invoke>,
    aperture_bytes: *mut u64,
) -> i32 {
    if aperture_bytes.is_null() {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *aperture_bytes = 0;
    connection_open(connection, cookie, invoke, aperture_bytes)
}

/// Context close is a host drain/cancel operation, not a collection of local
/// successful frees. The owner must retain mappings if close fails.
#[no_mangle]
pub unsafe extern "C" fn qlv_connection_close(connection: *mut Connection) -> i32 {
    let Some(connection) = connection.as_mut() else {
        return VK_ERROR_DEVICE_LOST;
    };

    if connection.context == 0 {
        return VK_SUCCESS;
    }
    let mut header = wire::Header::request(wire::CLOSE, connection.context);
    let status = connection.execute(&mut header, core::ptr::null(), core::ptr::null_mut(), 0);

    if status == VK_SUCCESS {
        connection.context = 0;
    }
    status
}

/// Callers provide a packet-specific immutable payload and writable reply span.
/// Returned output fields are copied only after exact framing validation.
#[no_mangle]
pub unsafe extern "C" fn qlv_connection_call(
    connection: *const Connection,
    header: *mut wire::Header,
    input: *const c_void,
    output: *mut c_void,
    output_bytes: usize,
) -> i32 {
    let (Some(connection), Some(header)) = (connection.as_ref(), header.as_mut()) else {
        return VK_ERROR_DEVICE_LOST;
    };

    if connection.context == 0 || header.operation == wire::OPEN || header.operation == wire::CLOSE
    {
        return VK_ERROR_DEVICE_LOST;
    }
    header.magic = wire::MAGIC;
    header.version = wire::VERSION;
    header.context = connection.context;
    header.status = wire::STATUS_UNSUPPORTED;
    if !wire::valid_operation(header) {
        return VK_ERROR_DEVICE_LOST;
    }
    connection.execute(header, input, output, output_bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    unsafe extern "C" fn unavailable(
        _: *mut c_void,
        _: *mut wire::Header,
        _: *const c_void,
        _: *mut c_void,
        _: usize,
    ) -> i32 {
        -1
    }
    unsafe extern "C" fn real_fixture(
        _: *mut c_void,
        header: *mut wire::Header,
        _: *const c_void,
        _: *mut c_void,
        _: usize,
    ) -> i32 {
        let header = &mut *header;

        if header.operation == wire::OPEN {
            header.context = 1;
        }
        header.status = wire::STATUS_SUCCESS;
        0
    }
    unsafe extern "C" fn large_aperture_fixture(
        _: *mut c_void,
        header: *mut wire::Header,
        _: *const c_void,
        _: *mut c_void,
        _: usize,
    ) -> i32 {
        let header = &mut *header;
        if header.operation == wire::OPEN {
            header.context = 2;
            header.flags = wire::HOST_APERTURE_SIZE;
            header.bytes = 1024 * 1024 * 1024;
        }
        header.status = wire::STATUS_SUCCESS;
        0
    }
    #[test]
    fn failed_escape_never_opens_or_fabricates_a_context() {
        let mut connection = Connection {
            context: 99,
            cookie: core::ptr::null_mut(),
            invoke: None,
        };
        assert_eq!(
            unsafe {
                qlv_connection_open(&mut connection, core::ptr::null_mut(), Some(unavailable))
            },
            VK_ERROR_DEVICE_LOST
        );
        assert_eq!(connection.context, 0);
    }
    #[test]
    fn close_and_validation_are_transactional() {
        let mut connection = Connection {
            context: 0,
            cookie: core::ptr::null_mut(),
            invoke: None,
        };
        assert_eq!(
            unsafe {
                qlv_connection_open(&mut connection, core::ptr::null_mut(), Some(real_fixture))
            },
            VK_SUCCESS
        );
        connection.invoke = Some(unavailable);
        assert_eq!(
            unsafe { qlv_connection_close(&mut connection) },
            VK_ERROR_DEVICE_LOST
        );
        assert_eq!(connection.context, 1);
        connection.invoke = Some(real_fixture);
        assert_eq!(unsafe { qlv_connection_close(&mut connection) }, VK_SUCCESS);
        assert_eq!(connection.context, 0);
    }
    #[test]
    fn negotiated_aperture_preserves_legacy_and_large_map_bounds() {
        let mut connection = Connection {
            context: 0,
            cookie: core::ptr::null_mut(),
            invoke: None,
        };
        let mut bytes = 0;
        assert_eq!(unsafe {
            qlv_connection_open_with_aperture(&mut connection, core::ptr::null_mut(),
                Some(real_fixture), &mut bytes)
        }, VK_SUCCESS);
        assert_eq!(bytes, wire::APERTURE_BYTES);
        assert_eq!(unsafe { qlv_connection_close(&mut connection) }, VK_SUCCESS);
        assert_eq!(unsafe {
            qlv_connection_open_with_aperture(&mut connection, core::ptr::null_mut(),
                Some(large_aperture_fixture), &mut bytes)
        }, VK_SUCCESS);
        assert_eq!(bytes, 1024 * 1024 * 1024);
        assert_eq!(unsafe { qlv_connection_close(&mut connection) }, VK_SUCCESS);
    }
}

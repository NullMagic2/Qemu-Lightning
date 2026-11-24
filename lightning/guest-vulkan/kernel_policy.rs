//! Module: lightning.guest_vulkan.kernel_policy
//! Purpose: Validate immutable native Escape snapshots and host replies.
//! Created: 2026-10-09
//! Architecture: The WDK shim owns process references, MDLs, DMA and teardown;
//! this module shares the exact host protocol without accepting native pointers.

#[path = "transport_contract.rs"]
pub mod wire;

/// Both spans describe private stable kernel storage, never concurrently mutable
/// application memory. The WDK owner checks their actual allocations first.
#[no_mangle]
pub unsafe extern "C" fn qlv_kernel_request(header: *const wire::Header, capacity: usize) -> bool {
    let Some(header) = header.as_ref() else {
        return false;
    };
    capacity >= wire::HEADER_BYTES
        && capacity <= wire::HEADER_BYTES + wire::MAX_PAYLOAD_BYTES
        && header.payload_bytes as usize <= capacity - wire::HEADER_BYTES
        && wire::valid_operation(header)
        && (header.operation != wire::GET_CAPS
            || (header.bytes == wire::CAPS_BYTES as u64
                && capacity >= wire::HEADER_BYTES + wire::CAPS_BYTES))
}

/// A MAP response from the host never supplies a virtual address. Only the WDK
/// shim may create a process-local VA after this immutable response is accepted.
#[no_mangle]
pub unsafe extern "C" fn qlv_kernel_reply(
    request: *const wire::Header,
    reply: *const wire::Header,
    bytes: usize,
    capacity: usize,
) -> bool {
    unsafe { qlv_kernel_reply_aperture(request, reply, bytes, capacity, wire::APERTURE_BYTES) }
}

/// As qlv_kernel_reply, with MAP_BLOB bounded by the adapter's discovered
/// aperture (a power of two from APERTURE_BYTES to MAX_APERTURE_BYTES). One
/// blob never exceeds APERTURE_BYTES.
#[no_mangle]
pub unsafe extern "C" fn qlv_kernel_reply_aperture(
    request: *const wire::Header,
    reply: *const wire::Header,
    bytes: usize,
    capacity: usize,
    aperture: u64,
) -> bool {
    let (Some(request), Some(reply)) = (request.as_ref(), reply.as_ref()) else {
        return false;
    };
    if !aperture.is_power_of_two()
        || !(wire::APERTURE_BYTES..=wire::MAX_APERTURE_BYTES).contains(&aperture)
    {
        return false;
    }

    if bytes < wire::HEADER_BYTES
        || bytes > capacity
        || bytes != wire::HEADER_BYTES + reply.payload_bytes as usize
        || reply.magic != wire::MAGIC
        || reply.version != wire::VERSION
        || reply.operation != request.operation
        || reply.reserved != 0
        || !(wire::STATUS_SUCCESS..=wire::STATUS_TIMEOUT).contains(&reply.status)
        || (request.operation != wire::OPEN && reply.context != request.context)
    {
        return false;
    }
    if reply.status != wire::STATUS_SUCCESS {
        return reply.payload_bytes == 0;
    }
    match request.operation {
        wire::OPEN => reply.context != 0 && reply.payload_bytes == 0,
        wire::CREATE_BLOB | wire::CREATE_SYNC => reply.resource != 0 && reply.payload_bytes == 0,
        wire::GET_CAPS => reply.payload_bytes as usize == wire::CAPS_BYTES,
        wire::QUERY_BLOB_BUDGET => {
            reply.resource == 0
                && reply.payload_bytes == 0
                && reply.flags == 0
                && reply.offset == 0
                && reply.bytes != 0
                && reply.value <= reply.bytes
        }
        wire::MAP_BLOB => {
            reply.resource == request.resource
                && matches!(
                    reply.flags,
                    wire::MAP_CACHE_CACHED | wire::MAP_CACHE_WRITE_COMBINED
                )
                && reply.value == 0
                && reply.payload_bytes == 0
                && reply.bytes != 0
                && reply.bytes <= wire::APERTURE_BYTES
                && reply.bytes % wire::PAGE_BYTES == 0
                && reply.offset % wire::PAGE_BYTES == 0
                && reply.offset <= aperture - reply.bytes
        }
        wire::WATCH_SYNCS => {
            reply.resource == request.resource
                && reply.payload_bytes == 0
                && (reply.value == 0 || reply.value == wire::WATCH_SATISFIED)
        }
        _ => reply.resource == request.resource && reply.payload_bytes == 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn large_apertures_bound_mappings_and_watch_replies_are_strict() {
        let mut request = wire::Header::request(wire::MAP_BLOB, 1);
        request.resource = 2;
        let mut reply = request;
        reply.status = wire::STATUS_SUCCESS;
        reply.flags = wire::MAP_CACHE_CACHED;
        reply.bytes = wire::PAGE_BYTES;
        reply.offset = wire::APERTURE_BYTES;
        let size = wire::HEADER_BYTES;
        // Beyond 512 MiB: only with a larger aperture.
        assert!(!unsafe { qlv_kernel_reply(&request, &reply, size, size) });
        assert!(unsafe {
            qlv_kernel_reply_aperture(&request, &reply, size, size, 2 * wire::APERTURE_BYTES)
        });
        reply.offset = 2 * wire::APERTURE_BYTES - wire::PAGE_BYTES;
        assert!(unsafe {
            qlv_kernel_reply_aperture(&request, &reply, size, size, 2 * wire::APERTURE_BYTES)
        });
        reply.offset += wire::PAGE_BYTES;
        assert!(!unsafe {
            qlv_kernel_reply_aperture(&request, &reply, size, size, 2 * wire::APERTURE_BYTES)
        });
        // Apertures are powers of two in range.
        reply.offset = 0;
        for aperture in [0, wire::APERTURE_BYTES + wire::PAGE_BYTES, 2 * wire::MAX_APERTURE_BYTES] {
            assert!(!unsafe { qlv_kernel_reply_aperture(&request, &reply, size, size, aperture) });
        }
        // One blob still never exceeds the legacy window.
        reply.bytes = 2 * wire::APERTURE_BYTES;
        assert!(!unsafe {
            qlv_kernel_reply_aperture(&request, &reply, size, size, 4 * wire::APERTURE_BYTES)
        });

        let mut watch = wire::Header::request(wire::WATCH_SYNCS, 1);
        watch.value = 9;
        let mut answer = watch;
        answer.status = wire::STATUS_SUCCESS;
        answer.value = 0;
        assert!(unsafe { qlv_kernel_reply(&watch, &answer, size, size) });
        answer.value = wire::WATCH_SATISFIED;
        assert!(unsafe { qlv_kernel_reply(&watch, &answer, size, size) });
        answer.value = 2;
        assert!(!unsafe { qlv_kernel_reply(&watch, &answer, size, size) });
    }
    #[test]
    fn caps_reply_requires_real_output_capacity_and_matching_context() {
        let mut request = wire::Header::request(wire::GET_CAPS, 1);
        request.bytes = wire::CAPS_BYTES as u64;
        assert!(!unsafe { qlv_kernel_request(&request, wire::HEADER_BYTES) });
        let capacity = wire::HEADER_BYTES + wire::CAPS_BYTES;
        assert!(unsafe { qlv_kernel_request(&request, capacity) });
        let mut reply = request;
        reply.status = wire::STATUS_SUCCESS;
        reply.payload_bytes = wire::CAPS_BYTES as u32;
        assert!(unsafe { qlv_kernel_reply(&request, &reply, capacity, capacity) });
        reply.context += 1;
        assert!(!unsafe { qlv_kernel_reply(&request, &reply, capacity, capacity) });
    }
    #[test]
    fn host_map_cannot_supply_process_pointer_or_escape_aperture() {
        let mut request = wire::Header::request(wire::MAP_BLOB, 1);
        request.resource = 2;
        let mut reply = request;
        reply.status = wire::STATUS_SUCCESS;
        reply.bytes = wire::PAGE_BYTES;

        for flags in [0, u32::MAX] {
            reply.flags = flags;
            assert!(!unsafe {
                qlv_kernel_reply(&request, &reply, wire::HEADER_BYTES, wire::HEADER_BYTES)
            });
        }
        for flags in [wire::MAP_CACHE_CACHED, wire::MAP_CACHE_WRITE_COMBINED] {
            reply.flags = flags;
            assert!(unsafe {
                qlv_kernel_reply(&request, &reply, wire::HEADER_BYTES, wire::HEADER_BYTES)
            });
        }
        assert!(unsafe {
            qlv_kernel_reply(&request, &reply, wire::HEADER_BYTES, wire::HEADER_BYTES)
        });
        reply.value = wire::PAGE_BYTES;
        assert!(!unsafe {
            qlv_kernel_reply(&request, &reply, wire::HEADER_BYTES, wire::HEADER_BYTES)
        });
        reply.value = 0;
        reply.offset = wire::APERTURE_BYTES;
        assert!(!unsafe {
            qlv_kernel_reply(&request, &reply, wire::HEADER_BYTES, wire::HEADER_BYTES)
        });
    }
}

//! Module: lightning.direct3d12_umd.shader
//! Purpose: Expose the native UMD's bounded private DXBC compile-copy operation.
//! Created: 2026-10-24
//! Architecture: No DDI/capability is advertised; exact-sibling compiler loading
//! occurs only on this explicit call. Output belongs to caller on return.
use super::shader_contract::{INVALID, MAX_INPUT, MAX_OUTPUT, MIN_INPUT, STAGE_COMPUTE};
use core::ffi::c_void;
unsafe extern "C" {
    fn ql_dx12_shader_compile_copy(
        input: *const c_void,
        bytes: usize,
        stage: u32,
        output: *mut c_void,
        capacity: usize,
        required: *mut usize,
    ) -> i32;
}
/// Caller supplies live disjoint input/output/size storage throughout this call.
/// No pointer is retained, and no compiler-owned pointer is ever returned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_shader_compile(
    input: *const c_void,
    bytes: usize,
    stage: u32,
    output: *mut c_void,
    capacity: usize,
    required: *mut usize,
) -> i32 {
    if input.is_null()
        || bytes < MIN_INPUT
        || bytes > MAX_INPUT
        || stage > STAGE_COMPUTE
        || capacity > MAX_OUTPUT
        || required.is_null()
        || (capacity != 0 && output.is_null())
    {
        return INVALID;
    }
    unsafe { ql_dx12_shader_compile_copy(input, bytes, stage, output, capacity, required) }
}

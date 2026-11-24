//! Module: lightning.direct3d12_umd.shader_native_contract
//! Purpose: Define a distinct raw R0 token/signature compiler ABI.
//! Created: 2026-10-24
//! Architecture: Native owners validate same-device empty roots and retain their
//! objects. Compiler snapshots tokens/signatures; no runtime or root pointer is
//! retained. The original full-container compiler ABI remains unchanged.
use super::shader_contract::{MAX_INPUT, STAGE_COMPUTE, STAGE_PIXEL, STAGE_VERTEX};
pub const NATIVE_VERSION: u32 = 2;
pub const SIGNATURE_LIMIT: u32 = 32;
pub const TOKEN_MINIMUM: u32 = 2;
pub const TOKEN_LIMIT: u32 = (MAX_INPUT / core::mem::size_of::<u32>()) as u32;
pub const NATIVE_STAGE_MASK: u32 = (1 << STAGE_VERTEX) | (1 << STAGE_PIXEL) | (1 << STAGE_COMPUTE);
pub const SYSTEM_NONE: u32 = 0;
pub const PIXEL_COLOR_REGISTER: u32 = 0;
pub const SYSTEM_POSITION: u32 = 1;
pub const SYSTEM_LAYER: u32 = 4;
pub const FEATURE_VERTEX_LAYER: u32 = 1;
pub const FEATURE_VERTEX_INPUT: u32 = 2;
pub const SYSTEM_VERTEX_ID: u32 = 6;
pub const SYSTEM_INSTANCE_ID: u32 = 8;
pub const COMPONENT_UINT: u32 = 1;
pub const COMPONENT_FLOAT: u32 = 3;
pub const COMPONENT_SINT: u32 = 2;
pub const PRECISION_DEFAULT: u32 = 0;
pub const MASK_SCALAR: u32 = 1;
pub const MASK_VECTOR4: u32 = 15;
pub const REGISTER_LIMIT: u32 = 32;
pub const DECLARATION_INDIRECT: u32 = 1;
pub const DECLARATION_REGISTER_KIND: u32 = 2;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NativeSignature {
    pub system_value: u32,
    pub register: u32,
    pub mask: u32,
    pub component_type: u32,
    pub minimum_precision: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NativeRequest {
    pub version: u32,
    pub bytes: u32,
    pub stage: u32,
    pub flags: u32,
    pub token_count: u32,
    pub input_count: u32,
    pub output_count: u32,
    pub reserved: u32,
    pub tokens: *const u32,
    pub inputs: *const NativeSignature,
    pub outputs: *const NativeSignature,
}
#[repr(C)]
pub struct NativeAbi {
    pub version: u32,
    pub bytes: u32,
    pub stage_mask: u32,
    pub signature_limit: u32,
    pub token_limit: u32,
    pub input_limit: u32,
    pub output_limit: u32,
    pub reserved: u32,
    pub base_commit: [u8; 20],
    pub integration_patch_sha256: [u8; 32],
    pub alignment: u32,
}

pub const USE_INPUT_READ: u32 = 0;
pub const USE_OUTPUT_WRITE: u32 = 1;
pub const USE_OUTPUT_READ: u32 = 2;
pub const USE_INPUT_WRITE: u32 = 3;
pub const VECTOR_COMPONENTS: u32 = 4;
pub const SWIZZLE_COMPONENT_BITS: u32 = 2;
pub const SWIZZLE_COMPONENT_MASK: u32 = 3;

// Additive root-constant compiler protocol. Original NativeRequest ABI2 stays
// frozen, and old entry points never accept constant-buffer resources.
pub const FEATURE_ROOT_CONSTANTS: u32 = 4;
pub const ROOT_CONSTANT_LIMIT: usize = 64;
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct NativeConstantBinding {
    pub register: u32,
    pub space: u32,
    pub words: u32,
    pub visibility: u32,
    pub offset: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NativeConstantPlan {
    pub count: u32,
    pub words: u32,
    pub bindings: [NativeConstantBinding; ROOT_CONSTANT_LIMIT],
}
impl NativeConstantPlan {
    pub fn valid(&self) -> bool {
        if self.count as usize > ROOT_CONSTANT_LIMIT || self.words > 64 {
            return false;
        }
        let mut words = 0u32;
        for (i, p) in self.bindings[..self.count as usize].iter().enumerate() {
            if p.words == 0
                || p.words > 64
                || p.offset != words
                || !matches!(p.visibility, 0 | 1 | 5)
            {
                return false;
            }
            let Some(end) = words.checked_add(p.words) else {
                return false;
            };
            if end > 64 {
                return false;
            }
            if self.bindings[..i].iter().any(|previous| {
                previous.register == p.register
                    && previous.space == p.space
                    && (previous.visibility == 0
                        || p.visibility == 0
                        || previous.visibility == p.visibility)
            }) {
                return false;
            }
            words = end;
        }
        words == self.words
            && self.bindings[self.count as usize..]
                .iter()
                .all(|p| *p == NativeConstantBinding::default())
    }
}

// Distinct additive binding protocol; the ABI2 request and feature4 constant
// plan stay frozen. Root CBVs carry no Windows/Vulkan address into compilation.
pub const FEATURE_ROOT_CBV: u32 = 8;
pub const FEATURE_ROOT_SRV: u32 = 16;
pub const FEATURE_ROOT_UAV: u32 = 32;
pub const BINDING_CONSTANTS: u32 = 1;
pub const BINDING_CBV: u32 = 2;
pub const BINDING_SRV: u32 = 3;
pub const BINDING_UAV: u32 = 4;
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct NativeResourceBinding {
    pub kind: u32,
    pub register: u32,
    pub space: u32,
    pub words: u32,
    pub visibility: u32,
    pub offset: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NativeBindingPlan {
    pub count: u32,
    pub words: u32,
    pub descriptors: u32,
    pub reserved: u32,
    pub bindings: [NativeResourceBinding; 64],
}
impl NativeBindingPlan {
    pub fn valid(&self) -> bool {
        self.valid_kinds(false, false, false)
    }
    pub fn valid_root_buffers(&self) -> bool {
        self.valid_kinds(true, false, false)
    }
    pub fn valid_storage_bindings(&self) -> bool {
        self.valid_kinds(true, true, false)
    }
    pub fn valid_tables(&self) -> bool {
        self.valid_kinds(true, true, true)
    }
    fn valid_kinds(&self, srv: bool, uav: bool, tables: bool) -> bool {
        if self.count > 64 || self.words > 64 || self.descriptors > 32 || self.reserved != 0 {
            return false;
        }
        let descriptors = self.bindings[..self.count as usize]
            .iter()
            .filter(|p| {
                p.kind == BINDING_CBV
                    || (srv && p.kind == BINDING_SRV)
                    || (uav && p.kind == BINDING_UAV)
            })
            .count() as u32;
        if descriptors != self.descriptors {
            return false;
        }
        let mut words = 2 * descriptors;
        let mut descriptor = 0u32;
        for (i, p) in self.bindings[..self.count as usize].iter().enumerate() {
            if !matches!(p.visibility, 0 | 1 | 5) {
                return false;
            }
            match p.kind {
                BINDING_CONSTANTS => {
                    if p.words == 0 || p.words > 64 || p.offset != words {
                        return false;
                    }
                    let Some(end) = words.checked_add(p.words) else {
                        return false;
                    };
                    if end > 64 {
                        return false;
                    }
                    words = end;
                }
                kind if kind == BINDING_CBV
                    || (srv && kind == BINDING_SRV)
                    || (uav && kind == BINDING_UAV) =>
                {
                    if p.words != 0 || p.offset != 2 * descriptor {
                        return false;
                    }
                    descriptor += 1;
                }
                BINDING_TABLE_CBV if tables => {
                    if p.words > 65535 || p.offset != i as u32 {
                        return false;
                    }
                }
                _ => return false,
            }
            if self.bindings[..i].iter().any(|previous| {
                (if matches!(previous.kind, BINDING_SRV | BINDING_UAV) {
                    previous.kind
                } else {
                    BINDING_CBV
                }) == (if matches!(p.kind, BINDING_SRV | BINDING_UAV) {
                    p.kind
                } else {
                    BINDING_CBV
                }) && previous.register == p.register
                    && previous.space == p.space
                    && (previous.visibility == 0
                        || p.visibility == 0
                        || previous.visibility == p.visibility)
            }) {
                return false;
            }
        }
        words == self.words
            && words
                + self.bindings[..self.count as usize]
                    .iter()
                    .filter(|p| p.kind == BINDING_TABLE_CBV)
                    .count() as u32
                <= 64
            && self.bindings[self.count as usize..]
                .iter()
                .all(|p| *p == NativeResourceBinding::default())
    }
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NativeBindingRequirements {
    pub resource_bytes: [u32; 64],
}
impl Default for NativeBindingRequirements {
    fn default() -> Self {
        Self {
            resource_bytes: [0; 64],
        }
    }
}

// A distinct versioned additive protocol preserves every earlier binding plan.
pub const FEATURE_STRUCTURED_BUFFERS: u32 = 64;
pub const STRUCTURED_VERSION: u32 = 1;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NativeStructuredPlan {
    pub version: u32,
    pub bytes: u32,
    pub flags: u32,
    pub reserved: u32,
    pub bindings: NativeBindingPlan,
}
impl NativeStructuredPlan {
    pub fn valid(&self) -> bool {
        self.version == STRUCTURED_VERSION
            && self.bytes as usize == core::mem::size_of::<Self>()
            && self.flags == 0
            && self.reserved == 0
            && self.bindings.valid_storage_bindings()
    }
}

// A distinct entry/frame admits bounded true descriptor-backed CBV tables.
pub const FEATURE_DESCRIPTOR_TABLES: u32 = 128;
pub const BINDING_TABLE_CBV: u32 = 5;
pub const TABLE_VERSION: u32 = 1;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NativeTablePlan {
    pub version: u32,
    pub bytes: u32,
    pub flags: u32,
    pub reserved: u32,
    pub bindings: NativeBindingPlan,
}
impl NativeTablePlan {
    pub fn valid(&self) -> bool {
        self.version == TABLE_VERSION
            && self.bytes as usize == core::mem::size_of::<Self>()
            && self.flags == 0
            && self.reserved == 0
            && self.bindings.valid_tables()
    }
}

// Additive entry uses the exact version1 table/binding frame; original entries stay strict.
pub const FEATURE_PIXEL_ARITHMETIC: u32 = 256;

// Distinct optional entry extends pixel math; feature256 arithmetic stays frozen.
pub const FEATURE_PIXEL_MATH: u32 = 512;

// Destination saturation is a separate negotiated policy from math512.
pub const FEATURE_PIXEL_SATURATION: u32 = 1024;

// Distinct stage-interface result; every older request/frame/result stays frozen.
pub const FEATURE_STAGE_IO: u32 = 2048;
pub const IO_VERSION: u32 = 1;
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct NativeIoRequirements {
    pub version: u32,
    pub bytes: u32,
    pub resource_bytes: [u32; 64],
    pub inputs: [u32; 32],
    pub outputs: [u32; 32],
}
impl Default for NativeIoRequirements {
    fn default() -> Self {
        Self {
            version: 0,
            bytes: 0,
            resource_bytes: [0; 64],
            inputs: [0; 32],
            outputs: [0; 32],
        }
    }
}

// Vector reductions have distinct source-lane proofs from scalar output masks.
pub const FEATURE_PIXEL_DOT_PRODUCTS: u32 = 4096;

// Exact scalar/vector FLOAT source NEG/ABS/ABSNEG operand extension policy.
pub const FEATURE_PIXEL_SOURCE_MODIFIERS: u32 = 8192;

// Fragment-only standard/fine/coarse screen-space derivatives.
pub const FEATURE_PIXEL_DERIVATIVES: u32 = 16384;

// Original native PS output registers o0..o7, with decoded per-output writes.
pub const FEATURE_PIXEL_MULTI_OUTPUT: u32 = 32768;

// Structured native pixel selections with definite initialized lane proofs.
pub const FEATURE_PIXEL_CONTROL_FLOW: u32 = 65536;
/// Typed NaN-alpha discard, enabled only for alpha-to-coverage.
pub const FEATURE_PIXEL_COVERAGE_GUARD: u32 = 131072;
/// Verified actual vertex PointSize=1 builtin, preserving source IO metadata.
pub const FEATURE_VERTEX_POINT_SIZE: u32 = 262144;

// Separate native vertex arithmetic policy; all previous entries stay frozen.
pub const FEATURE_VERTEX_OPERATIONS: u32 = 524288;

// Structured native vertex selections with definite assignment at every merge.
pub const FEATURE_VERTEX_CONTROL_FLOW: u32 = 1048576;

//! Module: lightning.direct3d12_umd.generate_requirements_header
//! Purpose: Generate private evidence/report/inventory C shapes from owning Rust.
//! Created: 2026-10-24
//! Architecture: This generator never defines an official Vulkan or D3D ABI.
#[path = "backend_contract.rs"]
pub mod backend_contract;
#[path = "requirements.rs"]
pub mod requirements;
fn main() {
    println!(
        "/* Generated from direct3d12-umd/requirements.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        ("OBSERVATION_VERSION", requirements::OBSERVATION_VERSION),
        ("PROFILE_ID", requirements::PROFILE_ID),
        ("RULE_COUNT", requirements::RULE_COUNT),
        (
            "EXTENSION_NAME_BYTES",
            requirements::EXTENSION_NAME_BYTES as u32,
        ),
        (
            "EXTENSION_PAGE_CAPACITY",
            requirements::EXTENSION_PAGE_CAPACITY as u32,
        ),
        ("HARD", requirements::HARD),
        ("CONDITIONAL", requirements::CONDITIONAL),
        ("OPTIONAL", requirements::OPTIONAL),
        ("ADVISORY", requirements::ADVISORY),
        ("GUEST_BOUNDARY", requirements::GUEST_BOUNDARY),
        ("INTEGRATION", requirements::INTEGRATION),
        ("SATISFIED", requirements::SATISFIED),
        ("MISSING", requirements::MISSING),
        ("UNOBSERVED", requirements::UNOBSERVED),
    ] {
        println!("#define QLDX12_{name} {value}u");
    }
    requirements::visit_rules(|name, value| println!("#define QLDX12_RULE_{name} {value}u"));
    for kind in ["EVIDENCE", "REQUIREMENT_RESULT"] {
        println!("typedef struct QLDX12_{kind} {{");
        if kind == "EVIDENCE" {
            requirements::visit_evidence(|name, _| println!("    uint32_t {name};"));
        } else {
            requirements::visit_result(|name, _| println!("    uint32_t {name};"));
        }
        println!("}} QLDX12_{kind};");
        let check = |name: &str, offset| {
            println!(
                "_Static_assert(offsetof(QLDX12_{kind}, {name}) == {offset}u, \"private Rust/C {name}\");"
            )
        };
        if kind == "EVIDENCE" {
            requirements::visit_evidence(check);
        } else {
            requirements::visit_result(check);
        }
    }
    println!(
        "typedef struct QLDX12_BACKEND_EXTENSION {{ uint32_t version, spec_version; uint8_t name[{}]; }} QLDX12_BACKEND_EXTENSION;",
        requirements::EXTENSION_NAME_BYTES
    );
    println!(
        "_Static_assert(sizeof(QLDX12_BACKEND_EXTENSION) == {}u, \"private extension size\");",
        core::mem::size_of::<requirements::BackendExtension>()
    );
    println!(
        "_Static_assert(offsetof(QLDX12_BACKEND_EXTENSION, name) == {}u, \"private extension name offset\");",
        core::mem::offset_of!(requirements::BackendExtension, name)
    );
    println!(
        "#define QLDX12_EVIDENCE_BYTES {}u",
        core::mem::size_of::<requirements::Evidence>()
    );
    println!(
        "#define QLDX12_REQUIREMENT_RESULT_BYTES {}u",
        core::mem::size_of::<requirements::RequirementResult>()
    );
    println!(
        "_Static_assert(sizeof(QLDX12_EVIDENCE) == {}u, \"private evidence total size\");",
        core::mem::size_of::<requirements::Evidence>()
    );
    println!(
        "_Static_assert(sizeof(QLDX12_REQUIREMENT_RESULT) == {}u, \"private report total size\");",
        core::mem::size_of::<requirements::RequirementResult>()
    );
}

/*
Module: qemu_lightning.opengl.icd_registration
Purpose: Register a validated AMD64 or WOW64 ICD with reversible per-adapter ownership.
Created: 2026-10-21
Architecture: Separate architecture-specific values and backups; no system loader replacement, driver restart or reboot.
*/
#pragma once
#include "runtime.hpp"
#include "icd_validation.hpp"
namespace qemu_lightning::opengl {
DWORD register_icd(const wchar_t* module, icd::Architecture architecture);
DWORD unregister_icd(icd::Architecture architecture);
DWORD register_native_icd(const wchar_t* module);
DWORD unregister_native_icd();
}

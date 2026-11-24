/*
Module: qemu_lightning.opengl.reactos_icd_bootstrap
Purpose: Expose the C ABI boundary for the existing Mesa ICD and Zink loader.
Created: 2026-10-22
Architecture: C++ owns runtime selection and the native loader-callout policy.
*/
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int __stdcall ql_nt5_icd_validate_version(unsigned long version);
void* ql_nt5_open_vulkan_loader(void);
int ql_nt5_context_may_thread(void);
#ifdef __cplusplus
}
#endif

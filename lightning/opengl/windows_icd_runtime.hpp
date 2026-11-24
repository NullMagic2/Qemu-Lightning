/*
Module: qemu_lightning.opengl.windows_icd_runtime
Purpose: Bind installed and application-local WGL to its matching native runtime.
Created: 2026-11-10
Architecture: Thin Mesa callbacks, called outside DllMain; no ReactOS dependency.
*/
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int __stdcall ql_windows_icd_validate_version(unsigned long version);
void* ql_windows_open_vulkan_loader(void);
#ifdef __cplusplus
}
#endif

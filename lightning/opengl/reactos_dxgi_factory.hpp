/*
Module: qemu_lightning.opengl.reactos_dxgi_factory
Purpose: Fail optional DXGI discovery safely when NT5 has no DXGI factory.
Created: 2026-10-22
Architecture: C ABI fallback for the pinned loader; never a renderer or COM factory.
*/
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#define QL_DXGI_NOEXCEPT noexcept
#else
#define QL_DXGI_NOEXCEPT
#endif

// The loader calls this only when dynamic CreateDXGIFactory1 lookup failed.
// Clear the borrowed output and report no interface; never invent an adapter.
HRESULT APIENTRY ql_reactos_missing_dxgi_factory(REFIID interface_id, void** factory) QL_DXGI_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef QL_DXGI_NOEXCEPT

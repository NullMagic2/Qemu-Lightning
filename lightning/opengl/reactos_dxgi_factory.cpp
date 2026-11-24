/*
Module: qemu_lightning.opengl.reactos_dxgi_factory
Purpose: Give unavailable optional DXGI discovery a checked failure result.
Created: 2026-10-22
Architecture: The loader retains normal Vulkan manifest discovery and device order.
*/
#include "reactos_dxgi_factory.hpp"

extern "C" HRESULT APIENTRY ql_reactos_missing_dxgi_factory(REFIID interface_id, void** factory) noexcept {
    (void)interface_id;

    if (factory == nullptr) {
        return E_POINTER;
    }

    *factory = nullptr;
    return E_NOINTERFACE;
}

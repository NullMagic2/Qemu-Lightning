/* Module: lightning.opengl.windows_wsi_bridge
 * Purpose: Own native D3D11 shared images and GPU-only composition presents.
 * Created: 2026-11-08
 * Architecture: C ABI between Mesa and the Windows SDK C++17 ABI adapter.
 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct qlg_wsi_bridge;
struct qlg_wsi_image;
/* LUID and format must come from the actual Vulkan physical device/image.
 * Returned HRESULTs preserve device loss and unsupported configurations. */
int32_t qlg_wsi_create(const void *luid, uint32_t width, uint32_t height,
                       uint32_t dxgi_format, struct qlg_wsi_bridge **bridge);
void qlg_wsi_destroy(struct qlg_wsi_bridge *bridge);
void *qlg_wsi_content(struct qlg_wsi_bridge *bridge);
int32_t qlg_wsi_image_create(struct qlg_wsi_bridge *bridge,
                            struct qlg_wsi_image **image, uint32_t *host_blob);
void qlg_wsi_image_destroy(struct qlg_wsi_bridge *bridge, struct qlg_wsi_image *image);
/* The Vulkan producer must release to EXTERNAL/GENERAL and finish before
 * this call. Success guarantees the shared-image read has retired on the GPU. */
int32_t qlg_wsi_present(struct qlg_wsi_bridge *bridge, struct qlg_wsi_image *image,
                       uint32_t interval);
#ifdef __cplusplus
}
#endif

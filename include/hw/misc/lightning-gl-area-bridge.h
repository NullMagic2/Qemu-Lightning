/*
 * Module: lightning.display.gl_area_bridge_boundary
 * Purpose: Transfer immutable images and retirement across GTK context groups.
 * Created: 2026-10-21
 * Architecture: Borrow existing producer/consumer contexts; never create one.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_LIGHTNING_GL_AREA_BRIDGE_H
#define HW_MISC_LIGHTNING_GL_AREA_BRIDGE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct LightningGlAreaBridge LightningGlAreaBridge;
LightningGlAreaBridge *lightning_gl_area_bridge_new(void);
/* Capture the existing, current producer context. Its GDK owner must retain it. */
bool lightning_gl_area_bridge_capture(LightningGlAreaBridge *bridge);
/* Probe in the current consumer; validate the real fence in its producer group. */
bool lightning_gl_area_bridge_supported(LightningGlAreaBridge *bridge, void *producer_fence);
/* Stage in producer context, import in consumer context. EGLImage storage is
 * retained for replay after consumer recreation; pixels are never read by CPU. */
bool lightning_gl_area_bridge_stage(LightningGlAreaBridge *bridge,
    uint32_t texture, uint32_t width, uint32_t height);
bool lightning_gl_area_bridge_import(LightningGlAreaBridge *bridge, uint32_t *texture);
bool lightning_gl_area_bridge_has_image(const LightningGlAreaBridge *bridge);
bool lightning_gl_area_bridge_has_consumer(const LightningGlAreaBridge *bridge);
/* Mark use BEFORE primary raster; retirement must cover even uncertain draws. */
bool lightning_gl_area_bridge_consumer_used(LightningGlAreaBridge *bridge);
/* Consumer fence -> EGL server wait -> real producer-group GLsync. Ownership
 * of the returned GLsync transfers to the existing publication retirement pool.
 * This never waits on the CPU or passes a foreign GLsync to the consumer. */
void *lightning_gl_area_bridge_retire(LightningGlAreaBridge *bridge, void *producer_fence);
/* Before consumer context destruction, after its FBOs have been detached. */
bool lightning_gl_area_bridge_release_consumer(LightningGlAreaBridge *bridge);
/* Free only after consumer release. EGLImage destruction is context-independent. */
bool lightning_gl_area_bridge_release(LightningGlAreaBridge *bridge);
#ifdef __cplusplus
}
#endif
#endif

/* Module: lightning.guest_vulkan.windows_platform
 * Purpose: Expose the native Windows service boundary to maintained Mesa.
 * Created: 2026-10-09
 * Architecture: No host API pointers cross this boundary. Windows adapter and
 * process mapping lifetime remain below the Mesa renderer object interface.
 */
#ifndef QLV_WINDOWS_PLATFORM_H
#define QLV_WINDOWS_PLATFORM_H
#include "transport.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct QLV_NATIVE_SESSION QLV_NATIVE_SESSION;
/* Adapter/session-scoped cooperation. Only the registered owner callback can
 * revoke its idle views. Reclaim holds no Mesa/native locks and is bounded;
 * stop joins the worker before the renderer cookie or session is destroyed. */
typedef struct QLV_MAP_PRESSURE QLV_MAP_PRESSURE;
typedef uint32_t (*QLV_MAP_PRESSURE_TRIM)(void *cookie);
QLV_MAP_PRESSURE *qlv_map_pressure_create(const uint8_t luid[QLV_LUID_BYTES],
                                          uint64_t aperture_bytes, void *cookie,
                                          QLV_MAP_PRESSURE_TRIM trim);
int qlv_map_pressure_reclaim(QLV_MAP_PRESSURE *pressure);
void qlv_map_pressure_stop(QLV_MAP_PRESSURE *pressure);

struct vn_renderer;
struct vn_renderer_bo;
/* Borrow fields from live dependency objects; no lifetime or GPU completion is
 * transferred. The C++ NT5 adapter owns image-registration policy. */
int qlv_dependency_present_binding(struct vn_renderer *renderer, struct vn_renderer_bo *buffer,
                                   QLV_NATIVE_SESSION **session, uint64_t *resource,
                                   uint32_t *flags, uint64_t *bytes);
/* async-present: the native token of a live renderer sync, for a producer
 * sync point; nothing about its value or completion is implied. */
struct vn_renderer_sync;
int qlv_dependency_sync_binding(struct vn_renderer *renderer, struct vn_renderer_sync *sync,
                                uint64_t *resource);
int32_t qlv_native_open(QLV_NATIVE_SESSION **session, uint8_t luid[QLV_LUID_BYTES],
                        uint64_t *aperture_bytes);
int32_t qlv_native_call(QLV_NATIVE_SESSION *session, QLV_HEADER *header, const void *input,
                        void *output, size_t output_bytes);
void qlv_native_close(QLV_NATIVE_SESSION *session);
/* Stage-3 W4: an auto-reset event the kernel signals when a sync watch fires.
 * The handle value travels in WATCH_SYNCS; the kernel keeps its own reference,
 * so closing the handle while a watch is armed is safe. */
uint64_t qlv_native_event_create(void);
/* TRUE when signalled within milliseconds. */
int qlv_native_event_wait(uint64_t event, uint32_t milliseconds);
void qlv_native_event_close(uint64_t event);
/* venus-supervise: the kernel's view of this session (ioctl_session_state on
 * NT5): 1 lost (its Venus worker epoch is gone), 0 live, -1 unsupported (an
 * older kernel; do not ask again), -2 a transient failure (ask later). Never
 * reaches the host. Safe from any thread; takes no session lock. */
int qlv_native_session_lost(QLV_NATIVE_SESSION *session);
/* venus-supervise (round 3/4): called when this process's device is lost. On
 * NT5 a fault filter is installed at the first session already: an unhandled
 * fault of a GL process ends it quietly instead of a csrss hard-error dialog,
 * which on ReactOS blocks every new GUI process until dismissed. An
 * application's own top-level filter still runs first. */
void qlv_native_device_lost_notice(void);
#ifdef __cplusplus
}

#endif
#endif

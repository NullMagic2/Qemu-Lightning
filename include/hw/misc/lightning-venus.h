/* Module: lightning.qemu.venus
 * Purpose: Own the native Vulkan DMA mailbox and real exported-FD aperture.
 * Created: 2026-10-09. Architecture: QEMU glue only; Rust owns renderer policy.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_LIGHTNING_VENUS_H
#define HW_MISC_LIGHTNING_VENUS_H
#include "hw/pci/pci_device.h"
#include "../../../lightning/guest-vulkan/transport.h"
typedef struct LightningVenus LightningVenus;
enum {
    LIGHTNING_VENUS_REG_BASE = 0x200,
    LIGHTNING_VENUS_REG_MAGIC = QLV_MMIO_REG_MAGIC,
    LIGHTNING_VENUS_REG_CAPABILITIES = QLV_MMIO_REG_CAPABILITIES,
    LIGHTNING_VENUS_REG_REQUEST_LO = QLV_MMIO_REG_REQUEST_LO,
    LIGHTNING_VENUS_REG_REQUEST_HI = QLV_MMIO_REG_REQUEST_HI,
    LIGHTNING_VENUS_REG_REQUEST_BYTES = QLV_MMIO_REG_REQUEST_BYTES,
    LIGHTNING_VENUS_REG_RESPONSE_LO = QLV_MMIO_REG_RESPONSE_LO,
    LIGHTNING_VENUS_REG_RESPONSE_HI = QLV_MMIO_REG_RESPONSE_HI,
    LIGHTNING_VENUS_REG_RESPONSE_CAPACITY = QLV_MMIO_REG_RESPONSE_CAPACITY,
    LIGHTNING_VENUS_REG_RESPONSE_BYTES = QLV_MMIO_REG_RESPONSE_BYTES,
    LIGHTNING_VENUS_REG_STATUS = QLV_MMIO_REG_STATUS,
    LIGHTNING_VENUS_REG_COMMAND = QLV_MMIO_REG_COMMAND,
    LIGHTNING_VENUS_REG_IRQ = QLV_MMIO_REG_IRQ,
    LIGHTNING_VENUS_REG_IRQ_ENABLE = QLV_MMIO_REG_IRQ_ENABLE,
    LIGHTNING_VENUS_REG_EVENT = QLV_MMIO_REG_EVENT,
    LIGHTNING_VENUS_REG_EVENT_STATUS = QLV_MMIO_REG_EVENT_STATUS,
    LIGHTNING_VENUS_REG_APERTURE_BAR = QLV_MMIO_REG_APERTURE_BAR,
    LIGHTNING_VENUS_REG_APERTURE_MIB = QLV_MMIO_REG_APERTURE_MIB,
    LIGHTNING_VENUS_REG_RING_LO = QLV_MMIO_REG_RING_LO,
    LIGHTNING_VENUS_REG_RING_HI = QLV_MMIO_REG_RING_HI,
    LIGHTNING_VENUS_REG_RING_DATA_LO = QLV_MMIO_REG_RING_DATA_LO,
    LIGHTNING_VENUS_REG_RING_DATA_HI = QLV_MMIO_REG_RING_DATA_HI,
    LIGHTNING_VENUS_REG_RING_STATUS = QLV_MMIO_REG_RING_STATUS,
    LIGHTNING_VENUS_REG_DOORBELL = QLV_MMIO_REG_DOORBELL,
    LIGHTNING_VENUS_REG_WORKER_STATE = QLV_MMIO_REG_WORKER_STATE,
    LIGHTNING_VENUS_REG_WORKER_EPOCH = QLV_MMIO_REG_WORKER_EPOCH,
    LIGHTNING_VENUS_REG_END = QLV_MMIO_REG_END,
    LIGHTNING_VENUS_IDLE = QLV_MMIO_IDLE,
    LIGHTNING_VENUS_PENDING = QLV_MMIO_PENDING,
    LIGHTNING_VENUS_COMPLETE = QLV_MMIO_COMPLETE,
    LIGHTNING_VENUS_ERROR = QLV_MMIO_ERROR,
    LIGHTNING_VENUS_SUBMIT = QLV_MMIO_SUBMIT,
    LIGHTNING_VENUS_RESET = QLV_MMIO_RESET,
};
/* asserted: some enabled source is pending. retrigger: the guest just
 * acknowledged, so a still-pending source needs another MSI-X message. */
typedef void (*LightningVenusIrq)(void *opaque, bool asserted, bool retrigger);
/* The display adapter retains its own descriptor duplicate. Revocation runs
 * before context/blob destruction, reset, disconnect and device teardown. */
typedef bool (*LightningVenusScanout)(void *opaque, uint64_t context, uint64_t resource,
                                     uint32_t width, uint32_t height, uint32_t stride,
                                     uint32_t offset, uint32_t format, int fd);
typedef void (*LightningVenusRevoke)(void *opaque, uint64_t context, uint64_t resource);
void lightning_venus_set_scanout(LightningVenus *venus, LightningVenusScanout scanout,
                                 LightningVenusRevoke revoke, void *opaque);
/* aperture_bytes: QLV_APERTURE_BYTES keeps BAR3; a larger power of two moves
 * the aperture to BAR1. low_mmio permits a 1 GiB 32-bit BAR1 for x86 guests
 * whose kernel cannot map a physical address above 4 GiB. ring_iothread runs
 * the doorbell, ring and worker socket on a device-owned IOThread without the
 * BQL; false keeps them on the main loop. supervise (venus-supervise) lets the
 * device start without a worker, probes the endpoint after a loss, parks a
 * restarted worker as a standby and adopts it after the guest's RESET
 * (QLV_MMIO_WORKER_SUPERVISION); false keeps the original one-shot reconnect
 * after RESET and refuses to start without a worker. Every function here,
 * and every callback above, is called or calls with the BQL held. */
LightningVenus *lightning_venus_new(PCIDevice *device, const char *endpoint, LightningVenusIrq irq,
                                    void *irq_opaque, uint64_t aperture_bytes, bool low_mmio,
                                    bool ring_iothread, bool supervise, Error **errp);
void lightning_venus_free(LightningVenus *venus);
void lightning_venus_reset(LightningVenus *venus);
uint64_t lightning_venus_read(LightningVenus *venus, hwaddr address);
void lightning_venus_write(LightningVenus *venus, hwaddr address, uint32_t value);
#endif

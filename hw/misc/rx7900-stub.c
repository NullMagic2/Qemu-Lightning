/*
 * Module: lightning.qemu.rx7900_stub
 * Purpose: Inert PCI stand-in for one AMD Radeon RX 7900 (Navi31) GPU function.
 * Created: 2026-10-24
 * Architecture: Test-only device for exercising the rx7900-reactos physical
 * installer (radeonctl) inside a ReactOS VM that has no Navi31. It owns only
 * PCI identity and four BARs whose layout follows the user's card
 * (0000:03:00.0): BAR0 64-bit prefetchable VRAM aperture, BAR2 64-bit
 * prefetchable doorbells, BAR4 I/O and BAR5 32-bit register MMIO. Every BAR
 * read returns zero and every write is discarded; there is no option ROM, no
 * interrupt, no DMA and no Lightning transport. It is never created unless the
 * command line names -device rx7900-stub.
 *
 * Driver contract this relies on (rx7900-reactos src/reactos/video.cpp and
 * hardware.cpp): HwFindAdapter only reads PCI config through VideoPort and maps
 * BAR5; it never touches a register. HwInitialize with PhysicalAsicInit=0 reads
 * only the administrator rx7900.rom file and returns FALSE when it is absent.
 * With PhysicalAsicInit=1 the legacy shadow and ROM BAR sources fail (no ROM
 * here), the SMUIO ROM read sees zero words, so no ATOM signature validates and
 * HwInitialize returns FALSE before any ASIC sequence. All-zero registers are
 * therefore the complete register model; no register needs a special value.
 *
 * Address space: SeaBIOS (i440fx, 1 GiB RAM, 32-bit CPU) places the 32-bit
 * window at 0x80000000..0xfec00000 and keeps 64-bit BARs below 4 GiB while
 * everything fits. With the recovery VM's Lightning low-mmio layout
 * (1 GiB + 512 MiB prefetchable) and the boot VGA (16 MiB), the default
 * 256 MiB BAR0 leaves about 210 MiB of the window unused. Larger apertures
 * would push the 64-bit BARs above 4 GiB, which an i386 ReactOS without PAE
 * cannot map, so vram-mib is limited to 256.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "migration/vmstate.h"

#define TYPE_RX7900_STUB "rx7900-stub"
OBJECT_DECLARE_SIMPLE_TYPE(Rx7900StubState, RX7900_STUB)

/* PCI identity of the user's board (sysfs vendor/device/revision/subsystem). */
enum {
    RX7900_STUB_VENDOR_ID = 0x1002,
    RX7900_STUB_DEFAULT_DEVICE_ID = 0x744c,
    RX7900_STUB_DEFAULT_REVISION = 0xc8,
    RX7900_STUB_SUBSYSTEM_VENDOR_ID = 0x1da2,
    RX7900_STUB_SUBSYSTEM_ID = 0x471e,
};

/*
 * Navi31 GPU functions, the same set as rx7900-reactos src/common/pci.hpp
 * navi31_device_ids and radeondisplay.inf. The card's USB and HDMI audio
 * functions are separate PCI functions and are not modelled.
 */
static const uint16_t rx7900_stub_navi31_ids[] = {
    0x7448, 0x744a, 0x744b, 0x744c, 0x745e,
};

/* BAR indices and sizes. The real BAR0 is 32 GiB (resized); see the header. */
enum {
    RX7900_STUB_VRAM_BAR = 0,
    RX7900_STUB_DOORBELL_BAR = 2,
    RX7900_STUB_IO_BAR = 4,
    RX7900_STUB_MMIO_BAR = 5,
    RX7900_STUB_DOORBELL_BYTES = 2 * MiB,
    RX7900_STUB_IO_BYTES = 256,
    RX7900_STUB_MMIO_BYTES = 1 * MiB,
    RX7900_STUB_DEFAULT_VRAM_MIB = 256,
    RX7900_STUB_MINIMUM_VRAM_MIB = 16,
    RX7900_STUB_MAXIMUM_VRAM_MIB = 256,
    RX7900_STUB_ACCESS_BYTES_MIN = 1,
    RX7900_STUB_ACCESS_BYTES_MAX = 8,
};

/* No INTx pin: the stub never raises an interrupt. */
#define RX7900_STUB_NO_INTERRUPT_PIN 0

typedef enum Rx7900StubRegion {
    RX7900_STUB_REGION_VRAM,
    RX7900_STUB_REGION_DOORBELL,
    RX7900_STUB_REGION_IO,
    RX7900_STUB_REGION_MMIO,
    RX7900_STUB_REGION_COUNT,
} Rx7900StubRegion;

/* Per-BAR access counters, readable through QOM for diagnostics. */
typedef struct Rx7900StubCounters {
    uint64_t reads;
    uint64_t writes;
} Rx7900StubCounters;

struct Rx7900StubState {
    PCIDevice parent_obj;

    uint16_t device_id;
    uint8_t revision;
    uint32_t vram_mib;

    MemoryRegion regions[RX7900_STUB_REGION_COUNT];
    Rx7900StubCounters counters[RX7900_STUB_REGION_COUNT];
};

/* One opaque per region so the shared callbacks can attribute accesses. */
typedef struct Rx7900StubRegionInfo {
    const char *name;
    const char *reads_property;
    const char *writes_property;
} Rx7900StubRegionInfo;

static const Rx7900StubRegionInfo rx7900_stub_regions[RX7900_STUB_REGION_COUNT] = {
    [RX7900_STUB_REGION_VRAM] = {"rx7900-stub-vram", "vram-reads", "vram-writes"},
    [RX7900_STUB_REGION_DOORBELL] = {"rx7900-stub-doorbell", "doorbell-reads", "doorbell-writes"},
    [RX7900_STUB_REGION_IO] = {"rx7900-stub-io", "io-reads", "io-writes"},
    [RX7900_STUB_REGION_MMIO] = {"rx7900-stub-mmio", "mmio-reads", "mmio-writes"},
};

static Rx7900StubCounters *rx7900_stub_counter(void *opaque)
{
    return opaque;
}

/* Every register reads as zero. */
static uint64_t rx7900_stub_read(void *opaque, hwaddr offset, unsigned size)
{
    rx7900_stub_counter(opaque)->reads++;
    return 0;
}

/* Every write is discarded; nothing in the device changes state. */
static void rx7900_stub_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    rx7900_stub_counter(opaque)->writes++;
}

static const MemoryRegionOps rx7900_stub_ops = {
    .read = rx7900_stub_read,
    .write = rx7900_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = RX7900_STUB_ACCESS_BYTES_MIN,
        .max_access_size = RX7900_STUB_ACCESS_BYTES_MAX,
    },
    .impl = {
        .min_access_size = RX7900_STUB_ACCESS_BYTES_MIN,
        .max_access_size = RX7900_STUB_ACCESS_BYTES_MAX,
    },
};

static bool rx7900_stub_known_device(uint16_t device_id)
{
    for (size_t index = 0; index < ARRAY_SIZE(rx7900_stub_navi31_ids); index++) {
        if (rx7900_stub_navi31_ids[index] == device_id) {
            return true;
        }
    }
    return false;
}

static void rx7900_stub_init_region(Rx7900StubState *s, Rx7900StubRegion region, uint64_t bytes)
{
    memory_region_init_io(&s->regions[region], OBJECT(s), &rx7900_stub_ops, &s->counters[region],
                          rx7900_stub_regions[region].name, bytes);
    object_property_add_uint64_ptr(OBJECT(s), rx7900_stub_regions[region].reads_property,
                                   &s->counters[region].reads, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), rx7900_stub_regions[region].writes_property,
                                   &s->counters[region].writes, OBJ_PROP_FLAG_READ);
}

static void rx7900_stub_realize(PCIDevice *pdev, Error **errp)
{
    Rx7900StubState *s = RX7900_STUB(pdev);
    const uint8_t memory64_prefetch = PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64 |
                                      PCI_BASE_ADDRESS_MEM_PREFETCH;

    if (!rx7900_stub_known_device(s->device_id)) {
        error_setg(errp, "rx7900-stub device-id 0x%04x is not a Navi31 GPU function "
                   "(0x7448, 0x744a, 0x744b, 0x744c, 0x745e)", s->device_id);
        return;
    }
    if (s->vram_mib < RX7900_STUB_MINIMUM_VRAM_MIB || s->vram_mib > RX7900_STUB_MAXIMUM_VRAM_MIB ||
        !is_power_of_2(s->vram_mib)) {
        error_setg(errp, "rx7900-stub vram-mib must be a power of two from %u to %u",
                   RX7900_STUB_MINIMUM_VRAM_MIB, RX7900_STUB_MAXIMUM_VRAM_MIB);
        return;
    }

    pci_config_set_device_id(pdev->config, s->device_id);
    pci_config_set_revision(pdev->config, s->revision);
    pdev->config[PCI_INTERRUPT_PIN] = RX7900_STUB_NO_INTERRUPT_PIN;

    rx7900_stub_init_region(s, RX7900_STUB_REGION_VRAM, (uint64_t)s->vram_mib * MiB);
    rx7900_stub_init_region(s, RX7900_STUB_REGION_DOORBELL, RX7900_STUB_DOORBELL_BYTES);
    rx7900_stub_init_region(s, RX7900_STUB_REGION_IO, RX7900_STUB_IO_BYTES);
    rx7900_stub_init_region(s, RX7900_STUB_REGION_MMIO, RX7900_STUB_MMIO_BYTES);

    pci_register_bar(pdev, RX7900_STUB_VRAM_BAR, memory64_prefetch, &s->regions[RX7900_STUB_REGION_VRAM]);
    pci_register_bar(pdev, RX7900_STUB_DOORBELL_BAR, memory64_prefetch,
                     &s->regions[RX7900_STUB_REGION_DOORBELL]);
    pci_register_bar(pdev, RX7900_STUB_IO_BAR, PCI_BASE_ADDRESS_SPACE_IO, &s->regions[RX7900_STUB_REGION_IO]);
    pci_register_bar(pdev, RX7900_STUB_MMIO_BAR, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->regions[RX7900_STUB_REGION_MMIO]);
}

static const VMStateDescription rx7900_stub_vmstate = {
    .name = TYPE_RX7900_STUB,
    .unmigratable = true,
};

static const Property rx7900_stub_properties[] = {
    DEFINE_PROP_UINT16("device-id", Rx7900StubState, device_id, RX7900_STUB_DEFAULT_DEVICE_ID),
    DEFINE_PROP_UINT8("revision", Rx7900StubState, revision, RX7900_STUB_DEFAULT_REVISION),
    DEFINE_PROP_UINT32("vram-mib", Rx7900StubState, vram_mib, RX7900_STUB_DEFAULT_VRAM_MIB),
};

static void rx7900_stub_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = rx7900_stub_realize;
    pc->vendor_id = RX7900_STUB_VENDOR_ID;
    pc->device_id = RX7900_STUB_DEFAULT_DEVICE_ID;
    pc->revision = RX7900_STUB_DEFAULT_REVISION;
    pc->class_id = PCI_CLASS_DISPLAY_VGA;
    pc->subsystem_vendor_id = RX7900_STUB_SUBSYSTEM_VENDOR_ID;
    pc->subsystem_id = RX7900_STUB_SUBSYSTEM_ID;
    dc->vmsd = &rx7900_stub_vmstate;
    device_class_set_props(dc, rx7900_stub_properties);
    dc->desc = "Inert AMD Navi31 PCI stand-in for installer tests (no GPU behaviour)";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo rx7900_stub_info = {
    .name = TYPE_RX7900_STUB,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Rx7900StubState),
    .class_init = rx7900_stub_class_init,
    .interfaces = (const InterfaceInfo[]){{INTERFACE_CONVENTIONAL_PCI_DEVICE}, {}},
};

static void rx7900_stub_register_types(void)
{
    type_register_static(&rx7900_stub_info);
}

type_init(rx7900_stub_register_types)

/*
 * Module: lightning.qtest.rx7900_stub
 * Purpose: Config-space and inert-BAR smoke test of the rx7900-stub device.
 * Created: 2026-10-24
 * Architecture: libqtest/libqos PC fixture under the qtest accelerator; no
 * guest code runs. Checks the PCI identity, BAR kinds and sizes, the absence
 * of an option ROM and interrupt pin, zero reads and discarded writes, and the
 * realize-time property validation of hw/misc/rx7900-stub.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"
#include "hw/pci/pci_regs.h"

/* Expected identity: the user's RX 7900 XTX board. */
#define STUB_SLOT 0x6
#define STUB_VENDOR_ID 0x1002
#define STUB_DEVICE_ID 0x744c
#define STUB_REVISION 0xc8
#define STUB_CLASS_VGA 0x0300
#define STUB_PROG_IF 0x00
#define STUB_SUBSYSTEM_VENDOR_ID 0x1da2
#define STUB_SUBSYSTEM_ID 0x471e

/* BAR sizing masks after writing all ones (low dword, type bits included). */
#define BAR_ALL_ONES 0xffffffffu
#define VRAM_BAR_MASK_256MIB 0xf000000cu   /* 256 MiB, 64-bit, prefetchable */
#define DOORBELL_BAR_MASK_2MIB 0xffe0000cu /* 2 MiB, 64-bit, prefetchable */
#define IO_BAR_MASK_256B 0xffffff01u       /* 256 bytes of I/O space */
#define MMIO_BAR_MASK_1MIB 0xfff00000u     /* 1 MiB, 32-bit, non-prefetchable */
#define BAR_UPPER_ALL_ONES 0xffffffffu     /* upper half of a sub-4 GiB 64-bit BAR */
#define ROM_SIZING_PATTERN 0xfffff800u

#define VRAM_BAR 0
#define DOORBELL_BAR 2
#define IO_BAR 4
#define MMIO_BAR 5
#define PROBE_OFFSET 0x40
#define PROBE_VALUE 0xdeadbeefu

static uint8_t bar_offset(int bar)
{
    return PCI_BASE_ADDRESS_0 + bar * sizeof(uint32_t);
}

static uint32_t size_bar(QPCIDevice *dev, int bar)
{
    const uint8_t offset = bar_offset(bar);
    const uint32_t original = qpci_config_readl(dev, offset);
    uint32_t mask;

    qpci_config_writel(dev, offset, BAR_ALL_ONES);
    mask = qpci_config_readl(dev, offset);
    qpci_config_writel(dev, offset, original);
    return mask;
}

static QPCIDevice *find_stub(QTestState *qts, QPCIBus **bus)
{
    QPCIDevice *dev;

    *bus = qpci_new_pc(qts, NULL);
    dev = qpci_device_find(*bus, QPCI_DEVFN(STUB_SLOT, 0));
    g_assert(dev != NULL);
    return dev;
}

static void test_identity(void)
{
    QTestState *qts = qtest_init("-device rx7900-stub,addr=06.0");
    QPCIBus *bus;
    QPCIDevice *dev = find_stub(qts, &bus);

    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==, STUB_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, STUB_DEVICE_ID);
    g_assert_cmphex(qpci_config_readb(dev, PCI_REVISION_ID), ==, STUB_REVISION);
    g_assert_cmphex(qpci_config_readw(dev, PCI_CLASS_DEVICE), ==, STUB_CLASS_VGA);
    g_assert_cmphex(qpci_config_readb(dev, PCI_CLASS_PROG), ==, STUB_PROG_IF);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_VENDOR_ID), ==, STUB_SUBSYSTEM_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_ID), ==, STUB_SUBSYSTEM_ID);
    g_assert_cmphex(qpci_config_readb(dev, PCI_HEADER_TYPE) & PCI_HEADER_TYPE_MASK, ==,
                    PCI_HEADER_TYPE_NORMAL);
    g_assert_cmphex(qpci_config_readb(dev, PCI_INTERRUPT_PIN), ==, 0);

    g_free(dev);
    qpci_free_pc(bus);
    qtest_quit(qts);
}

static void test_bar_layout(void)
{
    QTestState *qts = qtest_init("-device rx7900-stub,addr=06.0");
    QPCIBus *bus;
    QPCIDevice *dev = find_stub(qts, &bus);

    g_assert_cmphex(size_bar(dev, VRAM_BAR), ==, VRAM_BAR_MASK_256MIB);
    g_assert_cmphex(size_bar(dev, VRAM_BAR + 1), ==, BAR_UPPER_ALL_ONES);
    g_assert_cmphex(size_bar(dev, DOORBELL_BAR), ==, DOORBELL_BAR_MASK_2MIB);
    g_assert_cmphex(size_bar(dev, DOORBELL_BAR + 1), ==, BAR_UPPER_ALL_ONES);
    g_assert_cmphex(size_bar(dev, IO_BAR), ==, IO_BAR_MASK_256B);
    g_assert_cmphex(size_bar(dev, MMIO_BAR), ==, MMIO_BAR_MASK_1MIB);

    /* No expansion ROM: the ROM BAR stays hard-wired to zero. */
    qpci_config_writel(dev, PCI_ROM_ADDRESS, ROM_SIZING_PATTERN);
    g_assert_cmphex(qpci_config_readl(dev, PCI_ROM_ADDRESS), ==, 0);

    g_free(dev);
    qpci_free_pc(bus);
    qtest_quit(qts);
}

static void check_inert_bar(QPCIDevice *dev, int bar, bool memory)
{
    QPCIBar mapped = qpci_iomap(dev, bar, NULL);

    g_assert_cmphex(qpci_io_readl(dev, mapped, 0), ==, 0);
    qpci_io_writel(dev, mapped, PROBE_OFFSET, PROBE_VALUE);
    g_assert_cmphex(qpci_io_readl(dev, mapped, PROBE_OFFSET), ==, 0);
    if (memory) {
        qpci_io_writeq(dev, mapped, PROBE_OFFSET, PROBE_VALUE);
        g_assert_cmphex(qpci_io_readq(dev, mapped, PROBE_OFFSET), ==, 0);
    }
    qpci_iounmap(dev, mapped);
}

static void test_inert_registers(void)
{
    QTestState *qts = qtest_init("-device rx7900-stub,addr=06.0,vram-mib=16");
    QPCIBus *bus;
    QPCIDevice *dev = find_stub(qts, &bus);

    qpci_device_enable(dev);
    check_inert_bar(dev, VRAM_BAR, true);
    check_inert_bar(dev, DOORBELL_BAR, true);
    check_inert_bar(dev, IO_BAR, false);
    check_inert_bar(dev, MMIO_BAR, true);

    g_free(dev);
    qpci_free_pc(bus);
    qtest_quit(qts);
}

static void test_device_ids(void)
{
    static const uint16_t navi31_ids[] = {0x7448, 0x744a, 0x744b, 0x744c, 0x745e};

    for (size_t index = 0; index < G_N_ELEMENTS(navi31_ids); index++) {
        g_autofree char *arguments = g_strdup_printf("-device rx7900-stub,addr=06.0,device-id=0x%04x",
                                                     navi31_ids[index]);
        QTestState *qts = qtest_init(arguments);
        QPCIBus *bus;
        QPCIDevice *dev = find_stub(qts, &bus);

        g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, navi31_ids[index]);
        g_free(dev);
        qpci_free_pc(bus);
        qtest_quit(qts);
    }
}

/* Realize must refuse IDs outside Navi31 and apertures that cannot stay below 4 GiB. */
static void test_rejected_property(gconstpointer data)
{
    const char *arguments = data;

    if (g_test_subprocess()) {
        QTestState *qts = qtest_init(arguments);
        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*rx7900-stub*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx7900-stub/identity", test_identity);
    qtest_add_func("/rx7900-stub/bar-layout", test_bar_layout);
    qtest_add_func("/rx7900-stub/inert-registers", test_inert_registers);
    qtest_add_func("/rx7900-stub/device-ids", test_device_ids);
    qtest_add_data_func("/rx7900-stub/reject/device-id", "-device rx7900-stub,device-id=0x1234",
                        test_rejected_property);
    qtest_add_data_func("/rx7900-stub/reject/vram-mib", "-device rx7900-stub,vram-mib=512",
                        test_rejected_property);
    qtest_add_data_func("/rx7900-stub/reject/vram-mib-not-power-of-two", "-device rx7900-stub,vram-mib=96",
                        test_rejected_property);
    return g_test_run();
}

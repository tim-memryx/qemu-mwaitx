/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/i386/amd_iommu.h"

#define CMDBUF_ADDR       0x200000
#define CMDBUF_LEN_FIELD  8
#define CMDBUF_ENTRIES    (1U << CMDBUF_LEN_FIELD)

static inline uint64_t amdvi_reg_readq(QTestState *s, uint64_t offset)
{
    return qtest_readq(s, AMDVI_BASE_ADDR + offset);
}

static inline void amdvi_reg_writeq(QTestState *s, uint64_t offset,
                                    uint64_t val)
{
    qtest_writeq(s, AMDVI_BASE_ADDR + offset, val);
}

static void test_cmdbuf_head_wrap(void)
{
    QTestState *s;
    uint64_t head;
    int i;
    /* 16 bytes per command */
    struct {
        uint64_t qw0;
        uint64_t qw1;
    } cmdbuf[CMDBUF_ENTRIES];

    if (!qtest_has_machine("q35")) {
        g_test_skip("q35 machine not available");
        return;
    }

    s = qtest_init("-M q35 -device amd-iommu");

    /* fill the command buffer with COMPLETION_WAIT (no-op) commands */
    for (i = 0; i < CMDBUF_ENTRIES; i++) {
        cmdbuf[i].qw0 = (uint64_t)AMDVI_CMD_COMPLETION_WAIT << 60;
        cmdbuf[i].qw1 = 0;
    }
    qtest_memwrite(s, CMDBUF_ADDR, cmdbuf, sizeof(cmdbuf));

    /* point the IOMMU at the command buffer and set its length */
    amdvi_reg_writeq(s, AMDVI_MMIO_COMMAND_BASE,
                     CMDBUF_ADDR | ((uint64_t)CMDBUF_LEN_FIELD << 56));

    /* enable the IOMMU and its command buffer processor */
    amdvi_reg_writeq(s, AMDVI_MMIO_CONTROL,
                     AMDVI_MMIO_CONTROL_AMDVIEN | AMDVI_MMIO_CONTROL_CMDBUFLEN);

    /* advance tail to the last entry, consuming all but the final entry */
    amdvi_reg_writeq(s, AMDVI_MMIO_COMMAND_TAIL,
                     (CMDBUF_ENTRIES - 1) * AMDVI_COMMAND_SIZE);

    /* wrap tail to 0, consuming the final entry and completing the buffer */
    amdvi_reg_writeq(s, AMDVI_MMIO_COMMAND_TAIL, 0);

    /* after consuming all entries the IOMMU must wrap CmdHeadPtr to 0 */
    head = amdvi_reg_readq(s, AMDVI_MMIO_COMMAND_HEAD);
    g_assert((head & AMDVI_MMIO_CMDBUF_HEAD_MASK) == 0);

    qtest_quit(s);
}

/* Real downstream EDU DMA must preserve the bus byte and all address bits. */
#define TEST_RID 0x100
#define TEST_BAR 0x10000000
#define TEST_DT  0x400000
#define TEST_PT  0x600000
#define TEST_EV  0x610000

static void config_write(QTestState *s, unsigned rid, unsigned off, uint32_t value)
{
    qtest_outl(s, 0xcf8, 0x80000000 | (rid << 8) | off);
    qtest_outl(s, 0xcfc, value);
}

static void dma(QTestState *s, uint64_t iova, bool write)
{
    qtest_writeq(s, TEST_BAR + 0x80, write ? 0x40000 : iova);
    qtest_writeq(s, TEST_BAR + 0x88, write ? iova : 0x40000);
    qtest_writeq(s, TEST_BAR + 0x90, 1);
    qtest_writeq(s, TEST_BAR + 0x98, write ? 3 : 1);
    qtest_clock_step(s, 100000000);
    g_assert_cmphex(qtest_readq(s, TEST_BAR + 0x98) & 1, ==, 0);
}

static QTestState *fault_fixture(void)
{
    QTestState *s = qtest_init("-M q35 -nodefaults "
        "-device amd-iommu,id=vi,dma-remap=on,intremap=on "
        "-device pcie-root-port,id=rp,addr=2,chassis=1 "
        "-device edu,id=edu,bus=rp,addr=0");
    config_write(s, 2 << 3, 0x18, 0x00010100);
    config_write(s, 2 << 3, 0x20, 0x10001000);
    config_write(s, 2 << 3, 4, 6);
    config_write(s, TEST_RID, 0x10, TEST_BAR);
    config_write(s, TEST_RID, 4, 0x406);
    g_assert_cmphex(qtest_readl(s, TEST_BAR), ==, 0x010000ed);
    /* Single-level I/O page table, read/write DTE, domain 7. */
    qtest_writeq(s, TEST_DT + TEST_RID * 32,
                 TEST_PT | (3ULL << 61) | (1 << 9) | 3);
    qtest_writeq(s, TEST_DT + TEST_RID * 32 + 8, 7);
    /* A read-only leaf: read it first to warm the emulator's IOTLB. */
    qtest_writeq(s, TEST_PT + 8, 0x620000 | (1ULL << 61) | 1);
    amdvi_reg_writeq(s, AMDVI_MMIO_DEVICE_TABLE, TEST_DT | 511);
    amdvi_reg_writeq(s, AMDVI_MMIO_EVENT_BASE, TEST_EV | (8ULL << 56));
    amdvi_reg_writeq(s, AMDVI_MMIO_COMMAND_BASE, CMDBUF_ADDR | (8ULL << 56));
    amdvi_reg_writeq(s, AMDVI_MMIO_CONTROL,
                    AMDVI_MMIO_CONTROL_AMDVIEN | AMDVI_MMIO_CONTROL_EVENTLOGEN |
                    AMDVI_MMIO_CONTROL_CMDBUFLEN);
    qtest_writeq(s, CMDBUF_ADDR, (2ULL << 60) | TEST_RID);
    amdvi_reg_writeq(s, AMDVI_MMIO_COMMAND_TAIL, 16);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_COMMAND_HEAD), ==, 16);
    return s;
}

static void test_dma_fault_fields(void)
{
    QTestState *s = fault_fixture();
    dma(s, 0x4123, true);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 16);
    uint64_t low = qtest_readq(s, TEST_EV);
    g_assert_cmphex(low & 0xffff, ==, TEST_RID);
    g_assert_cmphex(qtest_readq(s, TEST_EV + 8), ==, 0x4123);
    g_assert_cmphex(low >> 60, ==, 2);
    g_assert_cmphex((low >> 32) & 0xffff, ==, 7);
    g_assert_cmphex(low & (1ULL << 51), ==, 0); /* Memory, not interrupt. */
    g_assert_cmphex(low & (1ULL << 52), ==, 0); /* Not-present leaf. */
    qtest_quit(s);
}

static void test_cached_permission_fault(void)
{
    QTestState *s = fault_fixture();
    dma(s, 0x1000, false);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 0);
    dma(s, 0x1000, true);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 16);
    uint64_t low = qtest_readq(s, TEST_EV);
    g_assert_cmphex(low & 0xffff, ==, TEST_RID);
    g_assert_cmphex(qtest_readq(s, TEST_EV + 8), ==, 0x1000);
    g_assert_cmphex((low >> 51) & 15, ==, 14); /* I=0, PR=RW=PE=1. */
    g_assert_cmphex((low >> 32) & 0xffff, ==, 7);
    qtest_quit(s);
}

static void test_polled_overflow(void)
{
    QTestState *s = fault_fixture();
    for (unsigned i = 0; i < 256; ++i) {
        dma(s, 0x4000, true);
    }
    /* Masking interrupts must not suppress the architected overflow status. */
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_STATUS) & AMDVI_MMIO_STATUS_EVT_OVF,
                    ==, AMDVI_MMIO_STATUS_EVT_OVF);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 4080);
    qtest_quit(s);
}

static void test_parent_permissions(void)
{
    QTestState *s = fault_fixture();
    /* A writable leaf must not override the DTE's read-only permission. */
    qtest_writeq(s, TEST_DT + TEST_RID * 32,
                 TEST_PT | (1ULL << 61) | (1 << 9) | 3);
    qtest_writeq(s, TEST_PT + 8, 0x620000 | (3ULL << 61) | 1);
    qtest_writeb(s, 0x620000, 0xa5);
    dma(s, 0x1000, true);
    g_assert_cmphex(qtest_readb(s, 0x620000), ==, 0xa5);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 16);
    g_assert_cmphex((qtest_readq(s, TEST_EV) >> 51) & 15, ==, 14);
    qtest_quit(s);
}

static uint32_t config_read(QTestState *s, unsigned rid, unsigned off)
{
    qtest_outl(s, 0xcf8, 0x80000000 | (rid << 8) | off);
    return qtest_inl(s, 0xcfc);
}

static void test_interrupt_bounds(void)
{
    QTestState *s = fault_fixture();
    unsigned cap = config_read(s, TEST_RID, 0x34) & 0xff;
    uint32_t header = config_read(s, TEST_RID, cap);

    g_assert_cmphex(header & 0xff, ==, 5); /* EDU's 64-bit MSI capability. */
    config_write(s, TEST_RID, cap + 4, 0xfee00000);
    config_write(s, TEST_RID, cap + 8, 0);
    config_write(s, TEST_RID, cap, header | (1 << 16));
    qtest_writeq(s, TEST_DT + TEST_RID * 32 + 16,
                 0x630000 | (2ULL << 60) | (1 << 1) | 1);
    /* The table has two entries. A valid-looking entry outside it must never
     * be fetched/delivered, and rejection must retain the real requester. */
    qtest_writel(s, 0x630000 + 8 * 4, 1 | (0x60 << 16));
    config_write(s, TEST_RID, cap + 12, 8);
    qtest_writel(s, TEST_BAR + 0x60, 1);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 16);
    uint64_t low = qtest_readq(s, TEST_EV);
    g_assert_cmphex(low & 0xffff, ==, TEST_RID);
    g_assert_cmphex(low >> 60, ==, 2);
    g_assert_cmphex((low >> 32) & 0xffff, ==, 7);
    g_assert_cmphex((low >> 51) & 3, ==, 1); /* I=1, PR=0. */
    g_assert_cmphex(qtest_readq(s, TEST_EV + 8), ==, 0xfee00000);
    /* A blocked in-range entry also logs, unless SupIOPF or DTE.IG asks not to. */
    config_write(s, TEST_RID, cap + 12, 0);
    qtest_writel(s, TEST_BAR + 0x60, 1);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 32);
    qtest_writel(s, 0x630000, 2);
    qtest_writel(s, TEST_BAR + 0x60, 1);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 32);
    qtest_writeq(s, TEST_DT + TEST_RID * 32 + 16,
                 0x630000 | (2ULL << 60) | (1 << 5) | (1 << 1) | 1);
    config_write(s, TEST_RID, cap + 12, 8);
    qtest_writel(s, TEST_BAR + 0x60, 1);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 32);
    /* Reserved lengths are illegal DTEs, not suppressed IO_PAGE_FAULTs. */
    qtest_writeq(s, TEST_DT + TEST_RID * 32 + 16,
                 0x630000 | (2ULL << 60) | (1 << 5) | (12 << 1) | 1);
    qtest_writel(s, TEST_BAR + 0x60, 1);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 48);
    g_assert_cmphex(qtest_readq(s, TEST_EV + 32) >> 60, ==, 1);
    qtest_quit(s);
}

static void test_host_controls(void)
{
    QTestState *s = fault_fixture();
    const uint64_t completion = 0x640000;

    qtest_writeq(s, CMDBUF_ADDR + 16, (1ULL << 60) | completion | 5);
    qtest_writeq(s, CMDBUF_ADDR + 24, 0x12345678);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/vi',"
                            "'property':'tunix-qi-stall','value':true}}");
    amdvi_reg_writeq(s, AMDVI_MMIO_COMMAND_TAIL, 32);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_COMMAND_HEAD), ==, 16);
    g_assert_cmphex(qtest_readq(s, completion), ==, 0);
    /* Queue consumption is stopped; DMA and its normal fault path stay live. */
    dma(s, 0x4123, true);
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_EVENT_TAIL), ==, 16);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/vi',"
                            "'property':'tunix-qi-stall','value':false}}");
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_COMMAND_HEAD), ==, 32);
    g_assert_cmphex(qtest_readq(s, completion), ==, 0x12345678);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/vi',"
                            "'property':'tunix-fault-overflow','value':true}}");
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_STATUS) & 1, ==, 1);
    amdvi_reg_writeq(s, AMDVI_MMIO_STATUS, 1); /* Normal guest W1C semantics. */
    g_assert_cmphex(amdvi_reg_readq(s, AMDVI_MMIO_STATUS) & 1, ==, 0);
    qtest_quit(s);
}

/* Both engines see the same IOVAs, but must use independent tables/caches and
 * completion streams. A stopped secondary queue cannot stall primary work. */
static void test_split_engines(gconstpointer data)
{
    bool segments = GPOINTER_TO_INT(data);
    g_autofree char *args = g_strdup_printf("-M q35 -nodefaults "
        "-device amd-iommu,id=vi,dma-remap=on,intremap=on,tunix-split-devfn=24%s "
        "-device pcie-root-port,id=rp1,bus=pcie.0,addr=2,chassis=1 "
        "-device edu,id=edu0,bus=rp1,addr=0 "
        "-device pcie-root-port,id=rp2,bus=%s,addr=3,chassis=2 "
        "-device edu,id=edu1,bus=rp2,addr=0",
        segments ? ",tunix-secondary-segment=on" : "",
        segments ? "tunix-pcie.1" : "pcie.0");
    QTestState *s = qtest_init(args);
    for (unsigned u = 0; u < 2; ++u) {
        uint64_t base = AMDVI_BASE_ADDR + u * AMDVI_MMIO_SIZE;
        unsigned rid = (segments ? 1 : u + 1) << 8;
        uint64_t bar = TEST_BAR + u * 0x100000;
        uint64_t dt = TEST_DT + u * 0x100000;
        uint64_t pt = TEST_PT + u * 4096;
        uint64_t input = 0x620000 + u * 0x2000;
        uint64_t output = input + 4096;
        unsigned port = (u + 2) << 3;
        if (segments && u) {
            /* Equal requester IDs decoded through distinct host ECAM roots. */
            uint64_t ecam = 0x50000000;
            qtest_writel(s, ecam + (port << 12) + 0x18, 0x00010100);
            qtest_writel(s, ecam + (port << 12) + 0x20, bar | (bar >> 16));
            qtest_writew(s, ecam + (port << 12) + 4, 6);
            qtest_writel(s, ecam + (rid << 12) + 0x10, bar);
            qtest_writew(s, ecam + (rid << 12) + 4, 0x406);
        } else {
            config_write(s, port, 0x18, (rid << 8) | rid);
            config_write(s, port, 0x20, bar | (bar >> 16));
            config_write(s, port, 4, 6);
            config_write(s, rid, 0x10, bar);
            config_write(s, rid, 4, 0x406);
        }
        g_assert_cmphex(qtest_readl(s, bar), ==, 0x010000ed);
        qtest_writeq(s, dt + rid * 32, pt | (3ULL << 61) | (1 << 9) | 3);
        qtest_writeq(s, dt + rid * 32 + 8, 7 + u);
        qtest_writeq(s, pt + 8, input | (3ULL << 61) | 1);
        qtest_writeq(s, pt + 16, output | (3ULL << 61) | 1);
        qtest_writeb(s, input, 0x41 + u);
        qtest_writeq(s, base + AMDVI_MMIO_DEVICE_TABLE, dt | 511);
        qtest_writeq(s, base + AMDVI_MMIO_COMMAND_BASE,
                     (CMDBUF_ADDR + u * 4096) | (8ULL << 56));
        qtest_writeq(s, base + AMDVI_MMIO_CONTROL,
                     AMDVI_MMIO_CONTROL_AMDVIEN | AMDVI_MMIO_CONTROL_CMDBUFLEN);
        /* Publish the DTE through the real invalidation command before DMA. */
        qtest_writeq(s, CMDBUF_ADDR + u * 4096, (2ULL << 60) | rid);
        qtest_writeq(s, base + AMDVI_MMIO_COMMAND_TAIL, 16);
        g_assert_cmphex(qtest_readq(s, base + AMDVI_MMIO_COMMAND_HEAD), ==, 16);
        for (unsigned write = 0; write < 2; ++write) {
            qtest_writeq(s, bar + 0x80, write ? 0x40000 : 0x1000);
            qtest_writeq(s, bar + 0x88, write ? 0x2000 : 0x40000);
            qtest_writeq(s, bar + 0x90, 1);
            qtest_writeq(s, bar + 0x98, write ? 3 : 1);
            qtest_clock_step(s, 100000000);
            g_assert_cmphex(qtest_readq(s, bar + 0x98) & 1, ==, 0);
        }
        g_assert_cmphex(qtest_readb(s, output), ==, 0x41 + u);
    }
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/vi/tunix-secondary',"
                            "'property':'tunix-qi-stall','value':true}}");
    for (unsigned u = 0; u < 2; ++u) {
        uint64_t queue = CMDBUF_ADDR + u * 4096;
        uint64_t completion = 0x640000 + u * 8;
        uint64_t base = AMDVI_BASE_ADDR + u * AMDVI_MMIO_SIZE;
        qtest_writeq(s, queue + 16, (1ULL << 60) | completion | 5);
        qtest_writeq(s, queue + 24, 0x1234 + u);
        qtest_writeq(s, base + AMDVI_MMIO_COMMAND_TAIL, 32);
    }
    g_assert_cmphex(qtest_readq(s, 0x640000), ==, 0x1234);
    g_assert_cmphex(qtest_readq(s, 0x640008), ==, 0);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/vi/tunix-secondary',"
                            "'property':'tunix-qi-stall','value':false}}");
    g_assert_cmphex(qtest_readq(s, 0x640008), ==, 0x1235);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/vi/tunix-secondary',"
                            "'property':'tunix-fault-overflow','value':true}}");
    g_assert_cmphex(qtest_readq(s, AMDVI_BASE_ADDR + AMDVI_MMIO_STATUS) & 1, ==, 0);
    g_assert_cmphex(qtest_readq(s, AMDVI_BASE_ADDR + AMDVI_MMIO_SIZE +
                              AMDVI_MMIO_STATUS) & 1, ==, 1);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("/amd-iommu/split-engines", GINT_TO_POINTER(0), test_split_engines);
    qtest_add_data_func("/amd-iommu/two-segments", GINT_TO_POINTER(1), test_split_engines);
    qtest_add_func("/q35/amd-iommu/cmdbuf-head-wrap", test_cmdbuf_head_wrap);
    qtest_add_func("/q35/amd-iommu/dma-fault-fields", test_dma_fault_fields);
    qtest_add_func("/q35/amd-iommu/cached-permission-fault", test_cached_permission_fault);
    qtest_add_func("/q35/amd-iommu/polled-overflow", test_polled_overflow);
    qtest_add_func("/q35/amd-iommu/parent-permissions", test_parent_permissions);
    qtest_add_func("/q35/amd-iommu/interrupt-bounds", test_interrupt_bounds);
    qtest_add_func("/q35/amd-iommu/host-controls", test_host_controls);
    return g_test_run();
}

/*
 * QTest testcase for intel-iommu
 *
 * Copyright (c) 2024 Intel, Inc.
 *
 * Author: Zhenzhong Duan <zhenzhong.duan@intel.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/i386/intel_iommu_internal.h"
#include "libqos/fw_cfg.h"
#include "qemu/bswap.h"

#define CAP_STAGE_1_FIXED1    (VTD_CAP_FRO | VTD_CAP_NFR | VTD_CAP_ND | \
                              VTD_CAP_MAMV | VTD_CAP_PSI | VTD_CAP_SSLPS)
#define ECAP_STAGE_1_FIXED1   (VTD_ECAP_QI |  VTD_ECAP_IR | VTD_ECAP_IRO | \
                              VTD_ECAP_MHMV | VTD_ECAP_SMTS | VTD_ECAP_FSTS)

static inline uint32_t vtd_reg_readl(QTestState *s, uint64_t offset)
{
    return qtest_readl(s, Q35_HOST_BRIDGE_IOMMU_ADDR + offset);
}

static inline uint64_t vtd_reg_readq(QTestState *s, uint64_t offset)
{
    return qtest_readq(s, Q35_HOST_BRIDGE_IOMMU_ADDR + offset);
}

static inline void vtd_reg_writeq(QTestState *s, uint64_t offset,
                                  uint64_t value)
{
    qtest_writeq(s, Q35_HOST_BRIDGE_IOMMU_ADDR + offset, value);
}

static void test_intel_iommu_8byte_access(void)
{
    QTestState *s;
    uint64_t off;

    s = qtest_init("-M q35 -device intel-iommu");

    for (off = 0; off < DMAR_REG_SIZE; off += 4) {
        vtd_reg_readq(s, off);
        vtd_reg_writeq(s, off, 0);
    }

    g_assert_cmpuint(vtd_reg_readl(s, DMAR_VER_REG), !=, 0);

    qtest_quit(s);
}

static void test_intel_iommu_stage_1(void)
{
    uint8_t init_csr[DMAR_REG_SIZE];     /* register values */
    uint8_t post_reset_csr[DMAR_REG_SIZE];     /* register values */
    uint64_t cap, ecap, tmp;
    QTestState *s;

    s = qtest_init("-M q35 -device intel-iommu,scalable-mode=on,fsts=on");

    cap = vtd_reg_readq(s, DMAR_CAP_REG);
    g_assert((cap & CAP_STAGE_1_FIXED1) == CAP_STAGE_1_FIXED1);

    tmp = cap & VTD_CAP_SAGAW_MASK;
    g_assert(tmp == (VTD_CAP_SAGAW_39bit | VTD_CAP_SAGAW_48bit));

    tmp = VTD_MGAW_FROM_CAP(cap);
    g_assert(tmp == VTD_HOST_AW_48BIT - 1);

    ecap = vtd_reg_readq(s, DMAR_ECAP_REG);
    g_assert((ecap & ECAP_STAGE_1_FIXED1) == ECAP_STAGE_1_FIXED1);

    qtest_memread(s, Q35_HOST_BRIDGE_IOMMU_ADDR, init_csr, DMAR_REG_SIZE);

    qobject_unref(qtest_qmp(s, "{ 'execute': 'system_reset' }"));
    qtest_qmp_eventwait(s, "RESET");

    qtest_memread(s, Q35_HOST_BRIDGE_IOMMU_ADDR, post_reset_csr, DMAR_REG_SIZE);
    /* Ensure registers are consistent after hard reset */
    g_assert(!memcmp(init_csr, post_reset_csr, DMAR_REG_SIZE));

    qtest_quit(s);
}


/* Exercise real PCI MSI writes, not CPU writes to the APIC address range. */
#define EDU_BAR 0xfeb00000
#define EDU_RID (4 << 3)
#define IR_TABLE 0x200000

static uint32_t edu_config_read(QTestState *s, unsigned offset)
{
    qtest_outl(s, 0xcf8, 0x80000000 | (EDU_RID << 8) | (offset & ~3));
    return qtest_inl(s, 0xcfc);
}

static void edu_config_write(QTestState *s, unsigned offset, uint32_t value)
{
    qtest_outl(s, 0xcf8, 0x80000000 | (EDU_RID << 8) | offset);
    qtest_outl(s, 0xcfc, value);
}

static void ir_command(QTestState *s, uint32_t value)
{
    qtest_writel(s, Q35_HOST_BRIDGE_IOMMU_ADDR + DMAR_GCMD_REG, value);
    g_assert_cmphex(vtd_reg_readl(s, DMAR_GSTS_REG) &
                   (VTD_GSTS_CFIS | VTD_GSTS_IRES), ==,
                   value & (VTD_GCMD_CFI | VTD_GCMD_IRE));
}

static void edu_interrupt(QTestState *s, unsigned cap, uint32_t address,
                          unsigned reason)
{
    uint64_t fault;

    edu_config_write(s, cap + 4, address);
    qtest_writel(s, EDU_BAR + 0x64, UINT32_MAX);
    qtest_writel(s, EDU_BAR + 0x60, 1);
    fault = vtd_reg_readq(s, DMAR_FRCD_REG_0_2);
    if (reason) {
        g_assert_cmphex(fault & VTD_FRCD_F, !=, 0);
        g_assert_cmphex(fault & VTD_FRCD_SID_MASK, ==, EDU_RID);
        g_assert_cmphex((fault >> 32) & 255, ==, reason);
        g_assert_cmphex(vtd_reg_readl(s, DMAR_FSTS_REG) & VTD_FSTS_PPF, !=, 0);
        vtd_reg_writeq(s, DMAR_FRCD_REG_0_2, VTD_FRCD_F);
    } else {
        g_assert_cmphex(fault & VTD_FRCD_F, ==, 0);
    }
    g_assert_cmphex(vtd_reg_readl(s, DMAR_FSTS_REG), ==, 0);
}

static void test_intel_iommu_compat_interrupt(void)
{
    QTestState *s;
    unsigned cap, count;

    if (!qtest_has_device("edu")) {
        g_test_skip("EDU device not available");
        return;
    }
    s = qtest_init("-M q35,kernel-irqchip=split -device intel-iommu,intremap=on,eim=on "
                   "-device edu,addr=4 -nodefaults");
    g_assert_cmphex(edu_config_read(s, 0), ==, 0x11e81234);
    edu_config_write(s, 0x10, EDU_BAR);
    edu_config_write(s, 4, 0x406); /* memory decode, bus master, INTx disabled */
    cap = edu_config_read(s, 0x34) & 255;
    for (count = 0; cap && count < 48; count++) {
        uint32_t header = edu_config_read(s, cap);
        if ((header & 255) == 5) {
            break;
        }
        cap = (header >> 8) & 255;
    }
    g_assert_cmpuint(count, <, 48);
    g_assert_cmpuint(cap, !=, 0);
    g_assert_cmphex(edu_config_read(s, cap) & (1 << 23), !=, 0); /* 64-bit MSI */
    edu_config_write(s, cap + 8, 0);
    edu_config_write(s, cap + 12, 0x40);
    edu_config_write(s, cap, edu_config_read(s, cap) | (1 << 16));

    /* IR disabled: CFI does not restrict ordinary compatibility interrupts. */
    ir_command(s, 0);
    edu_interrupt(s, cap, 0xfee00000, 0);
    ir_command(s, VTD_GCMD_CFI);
    edu_interrupt(s, cap, 0xfee00000, 0);

    vtd_reg_writeq(s, DMAR_IRTA_REG, IR_TABLE);
    ir_command(s, VTD_GCMD_SIRTP | VTD_GCMD_IRE);
    edu_interrupt(s, cap, 0xfee00000, VTD_FR_IR_REQ_COMPAT);
    ir_command(s, VTD_GCMD_IRE | VTD_GCMD_CFI);
    edu_interrupt(s, cap, 0xfee00000, 0);
    ir_command(s, VTD_GCMD_IRE);
    edu_interrupt(s, cap, 0xfee00000, VTD_FR_IR_REQ_COMPAT);

    /* Valid remappable messages are independent of CFI; bad SID still faults. */
    qtest_writeq(s, IR_TABLE, (0x41 << 16) | 1);
    qtest_writeq(s, IR_TABLE + 8, EDU_RID | (1 << 18));
    edu_config_write(s, cap + 12, 0);
    edu_interrupt(s, cap, 0xfee00018, 0);
    qtest_writeq(s, IR_TABLE + 8, (EDU_RID + 8) | (1 << 18));
    edu_interrupt(s, cap, 0xfee00018, VTD_FR_IR_SID_ERR);
    edu_config_write(s, cap + 12, 0x42);

    /* EIME blocks compatibility messages even when CFI is set. */
    ir_command(s, VTD_GCMD_CFI);
    vtd_reg_writeq(s, DMAR_IRTA_REG, IR_TABLE | VTD_IRTA_EIME);
    ir_command(s, VTD_GCMD_SIRTP | VTD_GCMD_IRE | VTD_GCMD_CFI);
    edu_interrupt(s, cap, 0xfee00000, VTD_FR_IR_REQ_COMPAT);
    ir_command(s, VTD_GCMD_IRE);
    edu_interrupt(s, cap, 0xfee00000, VTD_FR_IR_REQ_COMPAT);
    ir_command(s, 0);
    edu_interrupt(s, cap, 0xfee00000, 0);

    /* The architected register already participates in reset/migration state. */
    ir_command(s, VTD_GCMD_CFI);
    qobject_unref(qtest_qmp(s, "{ 'execute': 'system_reset' }"));
    qtest_qmp_eventwait(s, "RESET");
    g_assert_cmphex(vtd_reg_readl(s, DMAR_GSTS_REG) & VTD_GSTS_CFIS, ==, 0);
    qtest_quit(s);
}

static void test_tunix_qi_stall(void)
{
    const uint64_t queue = 0x300000, completion = 0x301000;
    QTestState *s = qtest_init("-M q35 -device intel-iommu,id=vi -nodefaults");

    qtest_writeq(s, queue, (0x12345678ULL << 32) | VTD_INV_DESC_WAIT |
                          VTD_INV_DESC_WAIT_SW);
    qtest_writeq(s, queue + 8, completion);
    vtd_reg_writeq(s, DMAR_IQA_REG, queue);
    qtest_writel(s, Q35_HOST_BRIDGE_IOMMU_ADDR + DMAR_GCMD_REG, VTD_GCMD_QIE);
    g_assert_cmphex(vtd_reg_readl(s, DMAR_GSTS_REG) & VTD_GSTS_QIES, !=, 0);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/vi',"
                            "'property':'tunix-qi-stall','value':true}}");
    vtd_reg_writeq(s, DMAR_IQT_REG, 16);
    g_assert_cmphex(vtd_reg_readq(s, DMAR_IQH_REG), ==, 0);
    g_assert_cmphex(qtest_readl(s, completion), ==, 0);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/vi',"
                            "'property':'tunix-qi-stall','value':false}}");
    g_assert_cmphex(vtd_reg_readq(s, DMAR_IQH_REG), ==, 16);
    g_assert_cmphex(qtest_readl(s, completion), ==, 0x12345678);
    g_assert_cmphex(vtd_reg_readl(s, DMAR_FSTS_REG), ==, 0);
    qtest_quit(s);
}

static void test_tunix_edu_delay(void)
{
    QTestState *s;
    const uint64_t source = 0x300000, destination = 0x301000;

    if (!qtest_has_device("edu")) {
        g_test_skip("EDU device not available");
        return;
    }
    s = qtest_init("-M q35 -device edu,id=edu,addr=4 -nodefaults");
    edu_config_write(s, 0x10, EDU_BAR);
    edu_config_write(s, 4, 0x406);
    qtest_writel(s, source, 0xdecafbad);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/edu',"
                            "'property':'tunix-dma-delay-ms','value':500}}");
    qtest_writeq(s, EDU_BAR + 0x80, source);
    qtest_writeq(s, EDU_BAR + 0x88, 0x40000);
    qtest_writeq(s, EDU_BAR + 0x90, 4);
    qtest_writeq(s, EDU_BAR + 0x98, 1);
    qtest_clock_step(s, 499000000);
    g_assert_cmphex(qtest_readq(s, EDU_BAR + 0x98) & 1, !=, 0);
    qtest_clock_step(s, 1000000);
    g_assert_cmphex(qtest_readq(s, EDU_BAR + 0x98) & 1, ==, 0);
    qtest_writeq(s, EDU_BAR + 0x80, 0x40000);
    qtest_writeq(s, EDU_BAR + 0x88, destination);
    qtest_writeq(s, EDU_BAR + 0x98, 3);
    qtest_clock_step(s, 500000000);
    g_assert_cmphex(qtest_readl(s, destination), ==, 0xdecafbad);
    /* A late callback after BME removal retires without touching RAM. */
    edu_config_write(s, 4, 0x402);
    qtest_writel(s, destination, 0xfeedface);
    qtest_writeq(s, EDU_BAR + 0x98, 3);
    qtest_clock_step(s, 500000000);
    g_assert_cmphex(qtest_readl(s, destination), ==, 0xfeedface);
    QDict *result = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/edu',"
        "'property':'tunix-dma-completions'}}");
    g_assert_cmpint(qdict_get_int(result, "return"), ==, 3);
    qobject_unref(result);
    result = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/edu',"
        "'property':'tunix-dma-failures'}}");
    g_assert_cmpint(qdict_get_int(result, "return"), ==, 1);
    qobject_unref(result);
    qtest_quit(s);
}

static void test_tunix_fault_overflow(void)
{
    QTestState *s = qtest_init("-M q35 -device intel-iommu,id=vi -nodefaults");

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/vi',"
                            "'property':'tunix-fault-overflow','value':true}}");
    g_assert_cmphex(vtd_reg_readl(s, DMAR_FSTS_REG) & VTD_FSTS_PFO, !=, 0);
    qtest_writel(s, Q35_HOST_BRIDGE_IOMMU_ADDR + DMAR_FSTS_REG, VTD_FSTS_PFO);
    g_assert_cmphex(vtd_reg_readl(s, DMAR_FSTS_REG), ==, 0);
    qtest_quit(s);
}

static void test_tunix_edu_posted(void)
{
    QTestState *s;
    QDict *result;
    const uint64_t source = 0x300000, destination = 0x301000;

    if (!qtest_has_device("edu")) {
        g_test_skip("EDU device not available");
        return;
    }
    s = qtest_init("-M q35 -device edu,id=edu,addr=4 -nodefaults");
    edu_config_write(s, 0x10, EDU_BAR);
    edu_config_write(s, 4, 0x406);
    qtest_writel(s, source, 0xdecafbad);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/edu',"
                            "'property':'tunix-posted-delay-ms','value':5000}}");
    qtest_writeq(s, EDU_BAR + 0x80, source);
    qtest_writeq(s, EDU_BAR + 0x88, 0x40000);
    qtest_writeq(s, EDU_BAR + 0x90, 4);
    qtest_writeq(s, EDU_BAR + 0x98, 1);
    qtest_clock_step(s, 4999000000ULL);
    g_assert_cmphex(qtest_readq(s, EDU_BAR + 0x98), ==, 0);
    g_assert_cmphex(qtest_readl(s, EDU_BAR), ==, 0x010000ed);
    g_assert_cmphex(qtest_readq(s, EDU_BAR + 0x98) & 1, ==, 1);
    qtest_clock_step(s, 100000000);
    g_assert_cmphex(qtest_readq(s, EDU_BAR + 0x98) & 1, ==, 0);
    qtest_writeq(s, EDU_BAR + 0x80, 0x40000);
    qtest_writeq(s, EDU_BAR + 0x88, destination);
    qtest_writeq(s, EDU_BAR + 0x98, 3);
    qtest_clock_step(s, 100000000);
    g_assert_cmphex(qtest_readl(s, destination), ==, 0xdecafbad);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/edu',"
                            "'property':'tunix-posted-delay-ms','value':5000}}");
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/peripheral/edu',"
                            "'property':'tunix-readback-failure','value':1}}");
    qtest_writel(s, destination, 0xfeedface);
    qtest_writeq(s, EDU_BAR + 0x98, 3);
    g_assert_cmphex(qtest_readq(s, EDU_BAR + 0x98) & 1, ==, 0);
    g_assert_cmphex(qtest_readl(s, EDU_BAR), ==, 0xffffffff);
    edu_config_write(s, 4, 0x402);
    qtest_clock_step(s, 5000000000ULL);
    qtest_clock_step(s, 100000000);
    g_assert_cmphex(qtest_readl(s, destination), ==, 0xfeedface);
    result = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/edu',"
        "'property':'tunix-posted-flushes'}}");
    g_assert_cmpint(qdict_get_int(result, "return"), ==, 1);
    qobject_unref(result);
    result = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/edu',"
        "'property':'tunix-dma-completions'}}");
    g_assert_cmpint(qdict_get_int(result, "return"), ==, 3);
    qobject_unref(result);
    result = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/edu',"
        "'property':'tunix-dma-failures'}}");
    g_assert_cmpint(qdict_get_int(result, "return"), ==, 1);
    qobject_unref(result);
    qtest_quit(s);
}

static void split_config(QTestState *s, unsigned rid, unsigned offset, uint32_t value)
{
    qtest_outl(s, 0xcf8, 0x80000000 | (rid << 8) | offset);
    qtest_outl(s, 0xcfc, value);
}

static uint32_t split_read(QTestState *s, unsigned rid, unsigned offset)
{
    qtest_outl(s, 0xcf8, 0x80000000 | (rid << 8) | offset);
    return qtest_inl(s, 0xcfc);
}

static void split_dma(QTestState *s, uint64_t bar, bool to_ram)
{
    qtest_writeq(s, bar + 0x80, to_ram ? 0x40000 : 0x1000);
    qtest_writeq(s, bar + 0x88, to_ram ? 0x1000 : 0x40000);
    qtest_writeq(s, bar + 0x90, 8);
    qtest_writeq(s, bar + 0x98, to_ram ? 3 : 1);
    qtest_clock_step(s, 100000000);
    g_assert_cmphex(qtest_readq(s, bar + 0x98) & 1, ==, 0);
}

static void test_tunix_split_engines(void)
{
    const uint64_t units[] = {Q35_HOST_BRIDGE_IOMMU_ADDR, TUNIX_SECOND_IOMMU_ADDR};
    const uint64_t bars[] = {0x10000000, 0x10200000};
    const uint64_t roots[] = {0x400000, 0x410000};
    unsigned caps[2];
    QTestState *s = qtest_init("-M q35 -nodefaults "
        "-device intel-iommu,id=vi,intremap=on,tunix-split-devfn=24 "
        "-device pcie-root-port,id=rp1,addr=2,chassis=1 "
        "-device pcie-root-port,id=rp2,addr=3,chassis=2 "
        "-device edu,id=edu0,bus=rp1,addr=0 "
        "-device edu,id=edu1,bus=rp2,addr=0");

    for (unsigned i = 0; i < 2; i++) {
        unsigned bus = i + 1, rid = bus << 8;
        uint64_t root = roots[i], unit = units[i];
        unsigned port = (i + 2) << 3;

        split_config(s, port, 0x18, (bus << 8) | (bus << 16));
        split_config(s, port, 0x20, ((bars[i] >> 16) & 0xfff0) | (bars[i] & 0xfff00000));
        split_config(s, port, 4, 6);
        g_assert_cmphex(split_read(s, rid, 0), ==, 0x11e81234);
        split_config(s, rid, 0x10, bars[i]);
        split_config(s, rid, 4, 0x406);
        g_assert_cmphex(qtest_readl(s, bars[i]), ==, 0x010000ed);
        /* Equal domain IDs/IOVAs, different physical translations per engine. */
        qtest_writeq(s, root + bus * 16, (root + 0x1000) | 1);
        qtest_writeq(s, root + 0x1000, (root + 0x2000) | 1);
        qtest_writeq(s, root + 0x1008, (1 << 8) | 1);
        qtest_writeq(s, root + 0x2000, (root + 0x3000) | 3);
        qtest_writeq(s, root + 0x3000, (root + 0x4000) | 3);
        qtest_writeq(s, root + 0x4008, (root + 0x5000) | 3);
        qtest_writeq(s, root + 0x6000, (0x41 << 16) | 1);
        qtest_writeq(s, root + 0x6008, rid | (1 << 18));
        qtest_writeq(s, unit + DMAR_RTADDR_REG, root);
        qtest_writeq(s, unit + DMAR_IRTA_REG, root + 0x6000);
        qtest_writeq(s, unit + DMAR_IQA_REG, root + 0x7000);
        qtest_writel(s, unit + DMAR_GCMD_REG, VTD_GCMD_SRTP | VTD_GCMD_SIRTP |
                     VTD_GCMD_TE | VTD_GCMD_IRE | VTD_GCMD_QIE);
        g_assert_cmphex(qtest_readl(s, unit + DMAR_GSTS_REG) &
                       (VTD_GSTS_TES | VTD_GSTS_IRES | VTD_GSTS_QIES), ==,
                       VTD_GSTS_TES | VTD_GSTS_IRES | VTD_GSTS_QIES);
        qtest_writeq(s, root + 0x5000, 0xdecafbad00000000ULL + i);
        split_dma(s, bars[i], false);
        qtest_writeq(s, root + 0x5000, 0);
        split_dma(s, bars[i], true);
        g_assert_cmphex(qtest_readq(s, root + 0x5000), ==, 0xdecafbad00000000ULL + i);
        caps[i] = split_read(s, rid, 0x34) & 255;
        unsigned steps = 0;
        while (caps[i] && (split_read(s, rid, caps[i]) & 255) != 5 && ++steps < 48) {
            caps[i] = (split_read(s, rid, caps[i]) >> 8) & 255;
        }
        g_assert_cmpuint(caps[i], !=, 0);
        g_assert_cmpuint(steps, <, 48);
        split_config(s, rid, caps[i] + 4, 0xfee00018);
        split_config(s, rid, caps[i] + 8, 0);
        split_config(s, rid, caps[i] + 12, 0);
        split_config(s, rid, caps[i], split_read(s, rid, caps[i]) | (1 << 16));
        qtest_writel(s, bars[i] + 0x60, 1);
        g_assert_cmphex(qtest_readl(s, unit + DMAR_FSTS_REG), ==, 0);
    }
    /* MSI source rejection belongs only to the unit receiving that PCI write. */
    qtest_writeq(s, roots[1] + 0x6010, (0x41 << 16) | 1);
    qtest_writeq(s, roots[1] + 0x6018, 0x100 | (1 << 18));
    split_config(s, 0x200, caps[1] + 4, 0xfee00038); /* Fresh, uncached index. */
    qtest_writel(s, bars[1] + 0x64, UINT32_MAX);
    qtest_writel(s, bars[1] + 0x60, 1);
    uint64_t fault = qtest_readq(s, units[1] + DMAR_FRCD_REG_0_2);
    g_assert_cmphex(fault & VTD_FRCD_F, !=, 0);
    g_assert_cmphex(fault & VTD_FRCD_SID_MASK, ==, 0x200);
    g_assert_cmphex((fault >> 32) & 255, ==, VTD_FR_IR_SID_ERR);
    g_assert_cmphex(qtest_readl(s, units[0] + DMAR_FSTS_REG), ==, 0);
    qtest_writeq(s, units[1] + DMAR_FRCD_REG_0_2, VTD_FRCD_F);

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/vi/tunix-secondary',"
        "'property':'tunix-qi-stall','value':true}}");
    for (unsigned i = 0; i < 2; i++) {
        qtest_writeq(s, roots[i] + 0x7000, (0x12345678ULL << 32) |
                     VTD_INV_DESC_WAIT | VTD_INV_DESC_WAIT_SW);
        qtest_writeq(s, roots[i] + 0x7008, roots[i] + 0x8000);
        qtest_writeq(s, units[i] + DMAR_IQT_REG, 16);
        g_assert_cmphex(qtest_readq(s, units[i] + DMAR_IQH_REG), ==, i ? 0 : 16);
        g_assert_cmphex(qtest_readl(s, roots[i] + 0x8000), ==, i ? 0 : 0x12345678);
    }
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/vi/tunix-secondary',"
        "'property':'tunix-qi-stall','value':false}}");
    g_assert_cmphex(qtest_readl(s, roots[1] + 0x8000), ==, 0x12345678);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/vi/tunix-secondary',"
        "'property':'tunix-fault-overflow','value':true}}");
    g_assert_cmphex(qtest_readl(s, units[1] + DMAR_FSTS_REG) & VTD_FSTS_PFO, !=, 0);
    g_assert_cmphex(qtest_readl(s, units[0] + DMAR_FSTS_REG), ==, 0);

    /* The firmware bytes must describe the actual routing partition above. */
    QFWCFG *fw = pc_fw_cfg_init(s);
    size_t length = qfw_cfg_get_file(fw, "etc/acpi/tables", NULL, 0);
    g_autofree uint8_t *tables = g_malloc(length);
    g_assert_cmpuint(qfw_cfg_get_file(fw, "etc/acpi/tables", tables, length), ==, length);
    bool found = false;
    for (size_t i = 0; i + 96 <= length; i++) {
        if (!memcmp(tables + i, "DMAR", 4)) {
            const uint8_t *p = tables + i;
            g_assert_cmpuint(ldl_le_p(p + 4), ==, 96);
            g_assert_cmpuint(lduw_le_p(p + 50), ==, 24);
            g_assert_cmphex(ldq_le_p(p + 56), ==, units[1]);
            g_assert_cmpuint(p[64], ==, 2);
            g_assert_cmpuint(p[70], ==, 3);
            g_assert_cmpuint(p[71], ==, 0);
            g_assert_cmpuint(p[76], ==, 1); /* Default INCLUDE_ALL */
            g_assert_cmphex(ldq_le_p(p + 80), ==, units[0]);
            g_assert_cmpuint(p[88], ==, 3); /* IOAPIC belongs only here. */
            found = true;
        }
    }
    g_assert_true(found);
    pc_fw_cfg_uninit(fw);
    qobject_unref(qtest_qmp(s, "{'execute':'system_reset'}"));
    qtest_qmp_eventwait(s, "RESET");
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmphex(qtest_readl(s, units[i] + DMAR_GSTS_REG), ==, 0);
        g_assert_cmphex(qtest_readl(s, units[i] + DMAR_FSTS_REG), ==, 0);
    }
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/q35/intel-iommu/8byte-access",
                   test_intel_iommu_8byte_access);
    qtest_add_func("/q35/intel-iommu/stage-1", test_intel_iommu_stage_1);

    qtest_add_func("/q35/intel-iommu/compat-interrupt",
                   test_intel_iommu_compat_interrupt);
    qtest_add_func("/q35/intel-iommu/tunix-qi-stall", test_tunix_qi_stall);
    qtest_add_func("/q35/intel-iommu/tunix-edu-delay", test_tunix_edu_delay);
    qtest_add_func("/q35/intel-iommu/tunix-edu-posted", test_tunix_edu_posted);
    qtest_add_func("/q35/intel-iommu/tunix-fault-overflow", test_tunix_fault_overflow);
    qtest_add_func("/q35/intel-iommu/tunix-split-engines", test_tunix_split_engines);
    return g_test_run();
}

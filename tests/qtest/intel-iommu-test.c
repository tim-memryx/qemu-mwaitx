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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/q35/intel-iommu/8byte-access",
                   test_intel_iommu_8byte_access);
    qtest_add_func("/q35/intel-iommu/stage-1", test_intel_iommu_stage_1);

    qtest_add_func("/q35/intel-iommu/compat-interrupt", test_intel_iommu_compat_interrupt);
    return g_test_run();
}

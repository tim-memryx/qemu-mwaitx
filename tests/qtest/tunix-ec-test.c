/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

static uint64_t property(QTestState *s, const char *name)
{
    QDict *reply = qtest_qmp(s,
                             "{'execute':'qom-get','arguments':{"
                             "'path':'/machine/peripheral/ec0','property':%s}}",
                             name);
    uint64_t value = qdict_get_int(reply, "return");
    qobject_unref(reply);
    return value;
}

static void test_transactions(void)
{
    QTestState *s = qtest_init("-M q35 -device tunix-ec,id=ec0");
    for (unsigned i = 0; i < 256; i++) {
        qtest_outb(s, 0x66, 0x81);
        qtest_outb(s, 0x62, i);
        qtest_outb(s, 0x62, i ^ 0x5a);
        g_assert_cmphex(qtest_inb(s, 0x66), ==, 0);
        qtest_outb(s, 0x66, 0x80);
        qtest_outb(s, 0x62, i);
        g_assert_cmphex(qtest_inb(s, 0x66), ==, 1);
        g_assert_cmphex(qtest_inb(s, 0x62), ==, i ^ 0x5a);
        g_assert_cmphex(qtest_inb(s, 0x66), ==, 0);
    }
    g_assert_cmpuint(property(s, "reads"), ==, 256);
    g_assert_cmpuint(property(s, "errors"), ==, 0);
    qtest_qmp_assert_success(
        s, "{'execute':'qom-set','arguments':{"
           "'path':'/machine/peripheral/ec0','property':'query','value':66}}");
    g_assert_cmphex(qtest_inb(s, 0x66), ==, 0x20);
    qtest_outb(s, 0x66, 0x84);
    g_assert_cmphex(qtest_inb(s, 0x66), ==, 1);
    g_assert_cmphex(qtest_inb(s, 0x62), ==, 66);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    qtest_qmp_eventwait(s, "RESET");
    g_assert_cmphex(qtest_inb(s, 0x66), ==, 0);
    g_assert_cmpuint(property(s, "value"), ==, 0);
    g_assert_cmpuint(property(s, "reads"), ==, 0);
    qtest_quit(s);
}

static void test_framing(void)
{
    QTestState *s = qtest_init("-M q35 -device tunix-ec,id=ec0");
    qtest_inb(s, 0x62); /* No output was requested. */
    qtest_outb(s, 0x66, 0x80);
    qtest_outb(s, 0x62, 0);
    qtest_outb(s, 0x66, 0x81); /* Output has not been consumed. */
    g_assert_cmpuint(property(s, "errors"), ==, 2);
    qtest_quit(s);
    s = qtest_init("-M q35 -device tunix-ec,id=ec0,stuck=on");
    g_assert_cmphex(qtest_inb(s, 0x66), ==, 2);
    qtest_outb(s, 0x66, 0x80);
    g_assert_cmpuint(property(s, "errors"), ==, 1);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/tunix-ec/transactions-query-reset", test_transactions);
    qtest_add_func("/tunix-ec/framing-stuck", test_framing);
    return g_test_run();
}

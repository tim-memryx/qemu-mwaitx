/* Bounded ACPI EC qualification fixture, not a physical controller model.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/isa/isa.h"
#include "hw/core/qdev-properties.h"
#include "hw/i386/pc.h"
#include "hw/pci-host/q35.h"
#include "hw/i386/tunix_ec.h"
#include "hw/acpi/acpi.h"
#include "migration/vmstate.h"

#define TYPE_TUNIX_EC "tunix-ec"
OBJECT_DECLARE_SIMPLE_TYPE(TunixEC, TUNIX_EC)

struct TunixEC {
    ISADevice parent;
    MemoryRegion data, control;
    uint8_t bytes[256], command, address, output, query;
    unsigned phase;
    bool obf, stuck, ecdt, global_lock;
    uint64_t errors, reads;
};

static TunixEC *fixture(void)
{
    return (TunixEC *)object_resolve_path_type("/machine/peripheral/ec0",
                                               TYPE_TUNIX_EC, NULL);
}

static uint64_t ec_read(void *opaque, hwaddr address, unsigned size)
{
    TunixEC *s = opaque;
    if (address) {
        return (s->obf ? 1 : 0) | (s->stuck ? 2 : 0) | (s->query ? 0x20 : 0);
    }
    if (!s->obf) {
        s->errors++;
    }
    s->obf = false;
    s->reads++;
    return s->output;
}

static void ec_write(void *opaque, hwaddr address, uint64_t value,
                     unsigned size)
{
    TunixEC *s = opaque;
    if (s->stuck || s->obf) {
        s->errors++;
        return;
    }
    if (address) {
        s->command = value;
        s->phase = 0;
        if (value == 0x84) {
            s->output = s->query;
            s->query = 0;
            s->obf = true;
        } else if (value != 0x80 && value != 0x81) {
            s->errors++;
        }
    } else if (!s->phase++) {
        s->address = value;
        if (s->command == 0x80) {
            s->output = s->bytes[s->address];
            s->obf = true;
        } else if (s->command != 0x81) {
            s->errors++;
        }
    } else if (s->command == 0x81 && s->phase == 2) {
        s->bytes[s->address] = value;
    } else {
        s->errors++;
    }
}

static uint64_t control_read(void *opaque, hwaddr address, unsigned size)
{
    return ec_read(opaque, 1, size);
}

static void control_write(void *opaque, hwaddr address, uint64_t value,
                          unsigned size)
{
    ec_write(opaque, 1, value, size);
}

static const MemoryRegionOps data_ops = {
    .read = ec_read,
    .write = ec_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 1, .max_access_size = 1},
};
static const MemoryRegionOps control_ops = {
    .read = control_read,
    .write = control_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 1, .max_access_size = 1},
};

static void ec_query(Object *obj, Visitor *v, const char *name, void *opaque,
                     Error **errp)
{
    TunixEC *s = TUNIX_EC(obj);
    uint64_t value;
    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (!value || value > 255 || s->query) {
        error_setg(errp, "EC query must be one pending nonzero byte");
        return;
    }
    s->query = value;
    /* GPE 7 is unused by the bounded Q35 fixture's ordinary devices. */
    acpi_send_event(DEVICE(X86_MACHINE(qdev_get_machine())->acpi_dev),
                    ACPI_GENERIC_ERROR);
}

static void ec_value(Object *obj, Visitor *v, const char *name, void *opaque,
                     Error **errp)
{
    TunixEC *s = TUNIX_EC(obj);
    uint64_t value = (uint32_t)ldl_le_p(s->bytes);
    visit_type_uint64(v, name, &value, errp);
}

static void ec_reads(Object *obj, Visitor *v, const char *name, void *opaque,
                     Error **errp)
{
    visit_type_uint64(v, name, &TUNIX_EC(obj)->reads, errp);
}

static void ec_reg(Object *obj, Visitor *v, const char *name, void *opaque,
                   Error **errp)
{
    uint64_t value = TUNIX_EC(obj)->bytes[4];
    visit_type_uint64(v, name, &value, errp);
}

static void ec_errors(Object *obj, Visitor *v, const char *name, void *opaque,
                      Error **errp)
{
    visit_type_uint64(v, name, &TUNIX_EC(obj)->errors, errp);
}

static void ec_realize(DeviceState *dev, Error **errp)
{
    TunixEC *s = TUNIX_EC(dev);
    X86MachineState *machine = X86_MACHINE(qdev_get_machine());
    if (!object_resolve_path_type("/machine/q35", TYPE_Q35_HOST_DEVICE, NULL) ||
        !machine->acpi_dev || !fixture() || fixture() != s) {
        error_setg(errp, "tunix-ec requires Q35 with ACPI and id=ec0");
        return;
    }
    memory_region_init_io(&s->data, OBJECT(s), &data_ops, s, "tunix-ec-data",
                          1);
    memory_region_init_io(&s->control, OBJECT(s), &control_ops, s,
                          "tunix-ec-control", 1);
    isa_register_ioport(ISA_DEVICE(dev), &s->data, 0x62);
    isa_register_ioport(ISA_DEVICE(dev), &s->control, 0x66);
}

static void ec_reset(DeviceState *dev)
{
    TunixEC *s = TUNIX_EC(dev);
    memset(s->bytes, 0, sizeof(s->bytes));
    s->command = s->address = s->output = s->query = 0;
    s->phase = 0;
    s->obf = false;
    s->errors = s->reads = 0;
}

static const Property ec_properties[] = {
    DEFINE_PROP_BOOL("stuck", TunixEC, stuck, false),
    DEFINE_PROP_BOOL("ecdt", TunixEC, ecdt, true),
    DEFINE_PROP_BOOL("global-lock", TunixEC, global_lock, false),
};
static const VMStateDescription ec_vmstate = {.name = TYPE_TUNIX_EC,
                                              .unmigratable = true};

static void ec_class(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = ec_realize;
    device_class_set_legacy_reset(dc, ec_reset);
    dc->vmsd = &ec_vmstate;
    dc->hotpluggable = false;
    device_class_set_props(dc, ec_properties);
    object_class_property_add(klass, "query", "uint64", NULL, ec_query, NULL,
                              NULL);
    object_class_property_add(klass, "value", "uint64", ec_value, NULL, NULL,
                              NULL);
    object_class_property_add(klass, "reads", "uint64", ec_reads, NULL, NULL,
                              NULL);
    object_class_property_add(klass, "region-ready", "uint64", ec_reg, NULL,
                              NULL, NULL);
    object_class_property_add(klass, "errors", "uint64", ec_errors, NULL, NULL,
                              NULL);
}

static const TypeInfo ec_type = {
    .name = TYPE_TUNIX_EC,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(TunixEC),
    .class_init = ec_class,
};

static void register_ec(void)
{
    type_register_static(&ec_type);
}

type_init(register_ec)

    void tunix_ec_build_aml(Aml *dsdt)
{
    TunixEC *s = fixture();
    if (!s) {
        return;
    }
    Aml *scope = aml_scope("\\_SB");
    Aml *ec = aml_device("EC0");
    Aml *crs = aml_resource_template();
    aml_append(ec, aml_name_decl("_HID", aml_eisaid("PNP0C09")));
    aml_append(ec, aml_name_decl("_UID", aml_int(0)));
    aml_append(ec, aml_name_decl("_GPE", aml_int(7)));
    aml_append(ec, aml_name_decl("_GLK", aml_int(s->global_lock)));
    aml_append(crs, aml_io(AML_DECODE16, 0x62, 0x62, 1, 1));
    aml_append(crs, aml_io(AML_DECODE16, 0x66, 0x66, 1, 1));
    aml_append(ec, aml_name_decl("_CRS", crs));
    aml_append(
        ec, aml_operation_region("EREG", (AmlRegionSpace)3, aml_int(0), 256));
    Aml *field = aml_field("EREG", AML_BYTE_ACC, AML_NOLOCK, AML_PRESERVE);
    aml_append(field, aml_named_field("DATA", 32));
    aml_append(field, aml_named_field("REGS", 8));
    aml_append(ec, field);
    if (s->ecdt) {
        /* Module-level access requires the ECDT handler before AML loads. */
        aml_append(ec, aml_name_decl("EARL", aml_int(0)));
        aml_append(ec, aml_store(aml_int(0x11223344), aml_name("DATA")));
        aml_append(ec, aml_store(aml_name("DATA"), aml_name("EARL")));
    }
    aml_append(ec, aml_name_decl("INIV", aml_int(0)));
    aml_append(ec, aml_name_decl("REGD", aml_int(0)));
    aml_append(ec, aml_name_decl("QUER", aml_int(0)));
    Aml *method = aml_method("_REG", 2, AML_NOTSERIALIZED);
    aml_append(method, aml_store(aml_arg(1), aml_name("REGD")));
    aml_append(method, aml_store(aml_arg(1), aml_name("REGS")));
    aml_append(ec, method);
    method = aml_method("_INI", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_store(aml_int(0x12345678), aml_name("DATA")));
    aml_append(method, aml_store(aml_name("DATA"), aml_name("INIV")));
    aml_append(ec, method);
    method = aml_method("_Q42", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_store(aml_int(0x87654321), aml_name("DATA")));
    aml_append(method, aml_increment(aml_name("QUER")));
    aml_append(ec, method);
    aml_append(scope, ec);
    aml_append(dsdt, scope);
}

bool tunix_ec_has_ecdt(void)
{
    return fixture() && fixture()->ecdt;
}

void tunix_ec_build_ecdt(GArray *data, BIOSLinker *linker, const char *oem,
                         const char *id)
{
    AcpiTable table = {
        .sig = "ECDT", .rev = 1, .oem_id = oem, .oem_table_id = id};
    acpi_table_begin(&table, data);
    build_append_gas(data, AML_AS_SYSTEM_IO, 8, 0, 1, 0x66);
    build_append_gas(data, AML_AS_SYSTEM_IO, 8, 0, 1, 0x62);
    build_append_int_noprefix(data, 0, 4);
    build_append_int_noprefix(data, 7, 1);
    const char path[] = "\\_SB.EC0";
    g_array_append_vals(data, path, sizeof(path));
    acpi_table_end(linker, &table);
}

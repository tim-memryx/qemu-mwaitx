#ifndef HW_I386_TUNIX_EC_H
#define HW_I386_TUNIX_EC_H
#include "hw/acpi/aml-build.h"
void tunix_ec_build_aml(Aml *dsdt);
bool tunix_ec_has_ecdt(void);
void tunix_ec_build_ecdt(GArray *, BIOSLinker *, const char *, const char *);
#endif

#ifndef PROSENJIT_PE_INSPECT_H
#define PROSENJIT_PE_INSPECT_H

#include <stdint.h>
#include "driver.h"

#define PROS_PE_MACHINE_I386 0x014cu
#define PROS_PE_MACHINE_AMD64 0x8664u
#define PROS_PE_MAX_SECTIONS 32u

enum pros_pe_subsystem {
    PROS_PE_SUBSYSTEM_UNKNOWN = 0,
    PROS_PE_SUBSYSTEM_NATIVE = 1,
    PROS_PE_SUBSYSTEM_WINDOWS_GUI = 2,
    PROS_PE_SUBSYSTEM_WINDOWS_CUI = 3
};

struct pros_pe_section {
    uint32_t virtual_address;
    uint32_t virtual_size;
    uint32_t raw_offset;
    uint32_t raw_size;
};

struct pros_pe_image_info {
    uint16_t machine;
    uint16_t sections;
    uint32_t image_size;
    uint32_t entry_rva;
    uint16_t subsystem;
    uint32_t import_rva;
    uint32_t import_size;
    uint32_t section_count;
};

int pros_pe_inspect(const uint8_t *image, uint32_t image_size,
                    struct pros_pe_image_info *out);
int pros_pe_selftest(void);

#endif

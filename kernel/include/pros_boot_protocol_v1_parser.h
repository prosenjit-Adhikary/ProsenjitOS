#ifndef PROSENJITOS_BOOT_PROTOCOL_V1_PARSER_H
#define PROSENJITOS_BOOT_PROTOCOL_V1_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include "pros_boot_protocol_v1.h"

struct pros_boot_v1_view {
    const struct pros_boot_v1_header *header;
    const struct pros_boot_v1_mmap_entry *memory_map;
    const struct pros_boot_v1_module_entry *modules;
    const uint8_t *boot_device;
};

/* Pure validation only: does not dereference external physical objects. */
int pros_boot_v1_validate(const void *allocation,
                          size_t accessible_size,
                          uint64_t allocation_phys,
                          uint64_t kernel_start,
                          uint64_t kernel_end,
                          struct pros_boot_v1_view *view);

#endif

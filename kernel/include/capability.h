#ifndef PROSENJIT_CAPABILITY_H
#define PROSENJIT_CAPABILITY_H

#include <stdint.h>
#include "../../include/abi/capability.h"

#define PROSENJIT_CAPABILITY_MAX_OBJECTS 64u
#define PROSENJIT_CAPABILITY_MAX_HANDLES 128u

struct pros_kernel_object {
    uint16_t type;
    uint16_t generation;
    uint32_t references;
    uint64_t object_id;
};

struct pros_handle_entry {
    uint16_t object_slot;
    uint16_t generation;
    uint64_t rights;
};

struct pros_capability_table {
    struct pros_kernel_object objects[PROSENJIT_CAPABILITY_MAX_OBJECTS];
    struct pros_handle_entry handles[PROSENJIT_CAPABILITY_MAX_HANDLES];
};

int capability_table_init(struct pros_capability_table *table);
uint64_t capability_create(struct pros_capability_table *table,
                           uint16_t type, uint64_t rights);
int capability_lookup(const struct pros_capability_table *table,
                      uint64_t handle, uint16_t type, uint64_t required_rights);
int capability_close(struct pros_capability_table *table, uint64_t handle);

#endif

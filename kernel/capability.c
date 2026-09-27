#include "capability.h"

#define HANDLE_SLOT_BITS 7u
#define HANDLE_SLOT_MASK ((1ULL << HANDLE_SLOT_BITS) - 1ULL)
#define HANDLE_GENERATION_SHIFT HANDLE_SLOT_BITS
#define HANDLE_GENERATION_MASK 0xFFFFULL

static uint64_t make_handle(uint16_t slot, uint16_t generation) {
    return ((uint64_t)generation << HANDLE_GENERATION_SHIFT) |
           (uint64_t)slot;
}

static uint16_t handle_slot(uint64_t handle) {
    return (uint16_t)(handle & HANDLE_SLOT_MASK);
}

static uint16_t handle_generation(uint64_t handle) {
    return (uint16_t)((handle >> HANDLE_GENERATION_SHIFT) &
                      HANDLE_GENERATION_MASK);
}

int capability_table_init(struct pros_capability_table *table) {
    if (!table) return -1;

    for (uint32_t i = 0; i < PROSENJIT_CAPABILITY_MAX_OBJECTS; ++i) {
        table->objects[i].type = PROSENJIT_OBJECT_NONE;
        table->objects[i].generation = 1;
        table->objects[i].references = 0;
        table->objects[i].object_id = 0;
    }

    for (uint32_t i = 0; i < PROSENJIT_CAPABILITY_MAX_HANDLES; ++i) {
        table->handles[i].object_slot = 0;
        table->handles[i].generation = 0;
        table->handles[i].rights = 0;
    }

    return 0;
}

uint64_t capability_create(struct pros_capability_table *table,
                           uint16_t type, uint64_t rights) {
    if (!table || type == PROSENJIT_OBJECT_NONE ||
        rights == 0 || (rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
        return 0;

    uint32_t object_slot = PROSENJIT_CAPABILITY_MAX_OBJECTS;
    uint32_t handle_slot = PROSENJIT_CAPABILITY_MAX_HANDLES;

    for (uint32_t i = 0; i < PROSENJIT_CAPABILITY_MAX_OBJECTS; ++i) {
        if (table->objects[i].type == PROSENJIT_OBJECT_NONE) {
            object_slot = i;
            break;
        }
    }
    if (object_slot == PROSENJIT_CAPABILITY_MAX_OBJECTS) return 0;

    for (uint32_t i = 0; i < PROSENJIT_CAPABILITY_MAX_HANDLES; ++i) {
        if (table->handles[i].generation == 0) {
            handle_slot = i;
            break;
        }
    }
    if (handle_slot == PROSENJIT_CAPABILITY_MAX_HANDLES) return 0;

    struct pros_kernel_object *object = &table->objects[object_slot];
    struct pros_handle_entry *entry = &table->handles[handle_slot];

    object->type = type;
    object->references = 1;
    object->object_id = ((uint64_t)object->generation << 32) |
                        (uint64_t)object_slot;

    entry->object_slot = (uint16_t)object_slot;
    entry->generation = object->generation;
    entry->rights = rights;

    return make_handle((uint16_t)handle_slot, entry->generation);
}

int capability_lookup(const struct pros_capability_table *table,
                      uint64_t handle, uint16_t type, uint64_t required_rights) {
    if (!table || handle == 0) return -1;

    uint16_t slot = handle_slot(handle);
    uint16_t generation = handle_generation(handle);

    if (slot >= PROSENJIT_CAPABILITY_MAX_HANDLES || generation == 0)
        return -1;

    const struct pros_handle_entry *entry = &table->handles[slot];
    if (entry->generation != generation ||
        entry->object_slot >= PROSENJIT_CAPABILITY_MAX_OBJECTS)
        return -1;

    const struct pros_kernel_object *object =
        &table->objects[entry->object_slot];

    if (object->type == PROSENJIT_OBJECT_NONE ||
        object->generation != generation ||
        (type != PROSENJIT_OBJECT_NONE && object->type != type) ||
        (entry->rights & required_rights) != required_rights)
        return -1;

    return 0;
}

int capability_close(struct pros_capability_table *table, uint64_t handle) {
    if (!table || handle == 0) return -1;

    uint16_t slot = handle_slot(handle);
    uint16_t generation = handle_generation(handle);
    if (slot >= PROSENJIT_CAPABILITY_MAX_HANDLES || generation == 0)
        return -1;

    struct pros_handle_entry *entry = &table->handles[slot];
    if (entry->generation != generation ||
        entry->object_slot >= PROSENJIT_CAPABILITY_MAX_OBJECTS)
        return -1;

    struct pros_kernel_object *object =
        &table->objects[entry->object_slot];
    if (object->type == PROSENJIT_OBJECT_NONE ||
        object->generation != generation || object->references == 0)
        return -1;

    --object->references;
    entry->generation = 0;
    entry->rights = 0;
    entry->object_slot = 0;

    if (object->references == 0) {
        object->type = PROSENJIT_OBJECT_NONE;
        ++object->generation;
        if (object->generation == 0) object->generation = 1;
        object->object_id = 0;
    }

    return 0;
}

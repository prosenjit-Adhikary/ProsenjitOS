#include "object.h"

#define HANDLE_SLOT_BITS 8u
#define HANDLE_SLOT_MASK 0xFFULL
#define HANDLE_GENERATION_SHIFT HANDLE_SLOT_BITS
#define HANDLE_VALUE_MASK 0xFFFFFFULL

static int handle_value_canonical(uint64_t handle)
{
    return (handle & ~HANDLE_VALUE_MASK) == 0u;
}

static int object_type_valid(uint16_t type)
{
    switch (type) {
    case PROS_OBJECT_PROCESS:
    case PROS_OBJECT_THREAD:
    case PROS_OBJECT_ADDRESS_SPACE:
    case PROS_OBJECT_FILE:
    case PROS_OBJECT_DEVICE:
    case PROS_OBJECT_SOCKET:
    case PROS_OBJECT_IPC:
        return 1;
    default:
        return 0;
    }
}

static uint64_t make_handle(uint32_t slot, uint16_t generation) {
    return ((uint64_t)generation << HANDLE_GENERATION_SHIFT) |
           (uint64_t)slot;
}

static uint32_t handle_slot(uint64_t handle) {
    return (uint32_t)(handle & HANDLE_SLOT_MASK);
}

static uint16_t handle_generation(uint64_t handle) {
    return (uint16_t)(handle >> HANDLE_GENERATION_SHIFT);
}

void object_init(struct pros_object *object, uint16_t type) {
    if (!object) return;
    object->type = PROS_OBJECT_NONE;
    object->reserved = 0u;
    object->refcount = 0u;
    if (!object_type_valid(type))
        return;
    object->type = type;
    object->refcount = 1u;
}

int object_retain(struct pros_object *object) {
    if (!object || !object_type_valid(object->type) ||
        object->reserved != 0u || object->refcount == 0u ||
        object->refcount == UINT32_MAX)
        return -1;
    ++object->refcount;
    return 0;
}

int object_release(struct pros_object *object) {
    if (!object || !object_type_valid(object->type) ||
        object->reserved != 0u || object->refcount == 0u)
        return -1;
    --object->refcount;
    if (object->refcount == 0u) object->type = PROS_OBJECT_NONE;
    return 0;
}

void handle_table_init(struct pros_handle_table *table) {
    if (!table) return;

    for (uint32_t i = 0; i < PROSENJIT_HANDLE_MAX; ++i) {
        table->entries[i].object = 0;
        table->entries[i].generation = 1;
        table->entries[i].reserved = 0;
        table->entries[i].rights = 0;
    }
}

static int handle_table_object_aliases_valid(
    const struct pros_handle_table *table, const struct pros_object *object)
{
    uint32_t aliases = 0u;
    if (!table || !object || !object_type_valid(object->type) ||
        object->reserved != 0u || object->refcount == 0u)
        return 0;

    for (uint32_t i = 1u; i < PROSENJIT_HANDLE_MAX; ++i) {
        const struct pros_handle_entry *entry = &table->entries[i];
        if (entry->object != object)
            continue;
        if (entry->generation == 0u ||
            entry->generation == UINT16_MAX ||
            entry->reserved != 0u || entry->rights == 0u ||
            (entry->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u)
            return 0;
        ++aliases;
    }
    return object->refcount >= aliases;
}

uint64_t handle_alloc(struct pros_handle_table *table,
                      struct pros_object *object,
                      uint64_t rights) {
    if (!table || !object || object->refcount == 0 ||
        !object_type_valid(object->type) || object->reserved != 0u ||
        rights == 0 || (rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0 ||
        !handle_table_object_aliases_valid(table, object))
        return PROSENJIT_HANDLE_INVALID;

    for (uint32_t i = 1; i < PROSENJIT_HANDLE_MAX; ++i) {
        struct pros_handle_entry *entry = &table->entries[i];
        if (!entry->object && entry->generation != 0u &&
            entry->generation != UINT16_MAX && entry->reserved == 0u &&
            entry->rights == 0u) {
            if (object_retain(object) != 0) return PROSENJIT_HANDLE_INVALID;
            entry->object = object;
            entry->rights = rights;
            return make_handle(i, entry->generation);
        }
    }

    return PROSENJIT_HANDLE_INVALID;
}

int handle_check(const struct pros_handle_table *table,
                 uint64_t handle,
                 uint64_t required_rights,
                 uint16_t expected_type) {
    if (!table || handle == PROSENJIT_HANDLE_INVALID ||
        !handle_value_canonical(handle) || required_rights == 0u)
        return -1;

    uint32_t slot = handle_slot(handle);
    uint16_t generation = handle_generation(handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return -1;

    const struct pros_handle_entry *entry = &table->entries[slot];
    if (!entry->object || entry->generation != generation ||
        entry->generation == 0u || entry->generation == UINT16_MAX ||
        entry->reserved != 0u ||
        entry->object->refcount == 0u ||
        entry->object->reserved != 0u ||
        !object_type_valid(entry->object->type) ||
        entry->rights == 0u ||
        (entry->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u ||
        (required_rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u)
        return -1;

    if (expected_type != PROS_OBJECT_NONE &&
        (!object_type_valid(expected_type) ||
         entry->object->type != expected_type))
        return -1;

    if ((entry->rights & required_rights) != required_rights ||
        !handle_table_object_aliases_valid(table, entry->object))
        return -1;

    return 0;
}

int handle_table_close_all(struct pros_handle_table *table) {
    if (!table) return -1;

    /*
     * Preflight the entire table before releasing any reference. A malformed
     * later slot must not cause an early subset of handles to be closed while
     * the caller assumes resource cleanup failed atomically.
     */
    const struct pros_handle_entry *reserved = &table->entries[0];
    if (reserved->object != 0 || reserved->generation != 1u ||
        reserved->reserved != 0u || reserved->rights != 0u)
        return -1;

    for (uint32_t i = 1u; i < PROSENJIT_HANDLE_MAX; ++i) {
        const struct pros_handle_entry *entry = &table->entries[i];
        if (entry->generation == 0u || entry->reserved != 0u)
            return -1;
        if (!entry->object) {
            if (entry->rights != 0u)
                return -1;
            continue;
        }
        if (entry->generation == UINT16_MAX ||
            entry->rights == 0u ||
            (entry->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u ||
            !object_type_valid(entry->object->type) ||
            entry->object->reserved != 0u ||
            entry->object->refcount == 0u)
            return -1;

        /* Ensure aliased entries cannot exhaust one shared refcount mid-loop. */
        uint32_t references = 0u;
        for (uint32_t j = 1u; j < PROSENJIT_HANDLE_MAX; ++j)
            if (table->entries[j].object == entry->object)
                ++references;
        if (entry->object->refcount < references)
            return -1;
    }

    for (uint32_t i = 1u; i < PROSENJIT_HANDLE_MAX; ++i) {
        if (table->entries[i].object) {
            uint64_t handle = make_handle(i, table->entries[i].generation);
            if (handle_close(table, handle) != 0)
                return -1;
        }
    }
    return 0;
}

int handle_close(struct pros_handle_table *table, uint64_t handle) {
    if (!table || handle == PROSENJIT_HANDLE_INVALID ||
        !handle_value_canonical(handle))
        return -1;

    uint32_t slot = handle_slot(handle);
    uint16_t generation = handle_generation(handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return -1;

    struct pros_handle_entry *entry = &table->entries[slot];
    if (!entry->object || entry->generation != generation ||
        entry->generation == UINT16_MAX || entry->reserved != 0u ||
        entry->object->reserved != 0u ||
        !object_type_valid(entry->object->type) ||
        entry->object->refcount == 0u || entry->rights == 0u ||
        (entry->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u)
        return -1;

    /*
     * A corrupted under-counted object must not be released through one alias
     * while another live slot still points at it. Count aliases in this table
     * before mutating the entry; close_all performs a whole-table preflight,
     * but direct close paths need this invariant too.
     */
    if (!handle_table_object_aliases_valid(table, entry->object))
        return -1;

    struct pros_object *object = entry->object;
    entry->object = 0;
    entry->rights = 0;
    /*
     * Never wrap a handle generation. A wrapped generation would allow a
     * very old stale handle value to become valid again after enough reuse.
     * Retire that slot permanently instead; other slots remain available.
     */
    if (entry->generation != UINT16_MAX)
        ++entry->generation;

    return object_release(object);
}

uint64_t handle_duplicate(struct pros_handle_table *table,
                          uint64_t source_handle,
                          uint64_t requested_rights) {
    if (!table || source_handle == PROSENJIT_HANDLE_INVALID ||
        !handle_value_canonical(source_handle) ||
        requested_rights == 0 ||
        (requested_rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
        return PROSENJIT_HANDLE_INVALID;

    uint32_t slot = handle_slot(source_handle);
    uint16_t generation = handle_generation(source_handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return PROSENJIT_HANDLE_INVALID;

    struct pros_handle_entry *source = &table->entries[slot];
    if (!source->object || source->generation != generation ||
        source->generation == 0u || source->generation == UINT16_MAX ||
        source->reserved != 0u ||
        source->object->refcount == 0u ||
        source->object->reserved != 0u ||
        !object_type_valid(source->object->type) ||
        source->rights == 0u ||
        (source->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u ||
        !handle_table_object_aliases_valid(table, source->object))
        return PROSENJIT_HANDLE_INVALID;

    /* Duplication can only attenuate authority; it can never add rights. */
    if ((source->rights & PROSENJIT_CAP_DUPLICATE) == 0 ||
        (requested_rights & ~source->rights) != 0)
        return PROSENJIT_HANDLE_INVALID;

    return handle_alloc(table, source->object, requested_rights);
}

uint64_t handle_transfer_copy(struct pros_handle_table *source_table,
                              struct pros_handle_table *target_table,
                              uint64_t source_handle,
                              uint64_t requested_rights) {
    if (!source_table || !target_table ||
        source_handle == PROSENJIT_HANDLE_INVALID ||
        !handle_value_canonical(source_handle) ||
        requested_rights == 0 ||
        (requested_rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
        return PROSENJIT_HANDLE_INVALID;

    uint32_t slot = handle_slot(source_handle);
    uint16_t generation = handle_generation(source_handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return PROSENJIT_HANDLE_INVALID;

    struct pros_handle_entry *source = &source_table->entries[slot];
    if (!source->object || source->generation != generation ||
        source->generation == 0u || source->generation == UINT16_MAX ||
        source->reserved != 0u || source->object->refcount == 0u ||
        source->object->reserved != 0u ||
        !object_type_valid(source->object->type) ||
        source->rights == 0u ||
        (source->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u ||
        !handle_table_object_aliases_valid(source_table, source->object))
        return PROSENJIT_HANDLE_INVALID;

    /* Copy-transfer attenuates authority; the source remains valid. */
    if ((source->rights & PROSENJIT_CAP_TRANSFER) == 0 ||
        (requested_rights & ~source->rights) != 0)
        return PROSENJIT_HANDLE_INVALID;

    return handle_alloc(target_table, source->object, requested_rights);
}

uint64_t handle_transfer_move(struct pros_handle_table *source_table,
                              struct pros_handle_table *target_table,
                              uint64_t source_handle,
                              uint64_t requested_rights) {
    if (!source_table || !target_table ||
        source_table == target_table ||
        source_handle == PROSENJIT_HANDLE_INVALID ||
        !handle_value_canonical(source_handle) ||
        requested_rights == 0 ||
        (requested_rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
        return PROSENJIT_HANDLE_INVALID;

    uint32_t slot = handle_slot(source_handle);
    uint16_t generation = handle_generation(source_handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return PROSENJIT_HANDLE_INVALID;

    struct pros_handle_entry *source = &source_table->entries[slot];
    if (!source->object || source->generation != generation ||
        source->generation == 0u || source->generation == UINT16_MAX ||
        source->reserved != 0u || source->object->refcount == 0u ||
        source->object->reserved != 0u ||
        !object_type_valid(source->object->type) ||
        source->rights == 0u ||
        (source->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u ||
        !handle_table_object_aliases_valid(source_table, source->object))
        return PROSENJIT_HANDLE_INVALID;

    /*
     * Move-transfer requires TRANSFER authority and can only attenuate the
     * rights carried by the source capability. Allocate the destination
     * handle first so a full target table never destroys the source handle.
     */
    if ((source->rights & PROSENJIT_CAP_TRANSFER) == 0 ||
        (requested_rights & ~source->rights) != 0)
        return PROSENJIT_HANDLE_INVALID;

    uint64_t target_handle = handle_alloc(target_table, source->object,
                                          requested_rights);
    if (target_handle == PROSENJIT_HANDLE_INVALID)
        return PROSENJIT_HANDLE_INVALID;

    if (handle_close(source_table, source_handle) != 0) {
        (void)handle_close(target_table, target_handle);
        return PROSENJIT_HANDLE_INVALID;
    }

    return target_handle;
}

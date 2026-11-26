#include "object.h"

#define HANDLE_SLOT_BITS 8u
#define HANDLE_SLOT_MASK 0xFFULL
#define HANDLE_GENERATION_SHIFT HANDLE_SLOT_BITS

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
    if (!object || type == PROS_OBJECT_NONE || type > PROS_OBJECT_IPC) return;
    object->type = type;
    object->reserved = 0;
    object->refcount = 1;
}

int object_retain(struct pros_object *object) {
    if (!object || object->type == PROS_OBJECT_NONE ||
        object->type > PROS_OBJECT_IPC || object->reserved != 0 ||
        object->refcount == 0 || object->refcount == UINT32_MAX)
        return -1;
    ++object->refcount;
    return 0;
}

int object_release(struct pros_object *object) {
    if (!object || object->type == PROS_OBJECT_NONE ||
        object->type > PROS_OBJECT_IPC || object->reserved != 0 ||
        object->refcount == 0) return -1;
    --object->refcount;
    if (object->refcount == 0) object->type = PROS_OBJECT_NONE;
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

/*
 * Read-only whole-table validation. It checks table/object metadata and
 * ensures refcounts cover all aliases in this table. It cannot validate
 * arbitrary pointer mappings or account for owners outside this table.
 * Callers must supply a trusted table; lifecycle mutation is single-CPU.
 */
int handle_table_validate(const struct pros_handle_table *table) {
    if (!table) return -1;
    for (uint32_t i = 0; i < PROSENJIT_HANDLE_MAX; ++i) {
        const struct pros_handle_entry *entry = &table->entries[i];
        if (entry->generation == 0 || entry->reserved != 0) return -1;
        if (i == 0) {
            if (entry->object || entry->rights != 0) return -1;
            continue;
        }
        if (!entry->object) {
            if (entry->rights != 0) return -1;
            continue;
        }
        const struct pros_object *object = entry->object;
        if (object->type == PROS_OBJECT_NONE ||
            object->type > PROS_OBJECT_IPC || object->reserved != 0 ||
            object->refcount == 0 || entry->rights == 0 ||
            (entry->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
            return -1;
        uint32_t table_refs = 0;
        for (uint32_t j = 1; j < PROSENJIT_HANDLE_MAX; ++j)
            if (table->entries[j].object == entry->object)
                ++table_refs;
        if (object->refcount < table_refs) return -1;
    }
    return 0;
}

uint64_t handle_alloc(struct pros_handle_table *table,
                      struct pros_object *object,
                      uint64_t rights) {
    if (!table || !object || object->type == PROS_OBJECT_NONE ||
        object->type > PROS_OBJECT_IPC || object->reserved != 0 ||
        object->refcount == 0 ||
        rights == 0 || (rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
        return PROSENJIT_HANDLE_INVALID;

    for (uint32_t i = 1; i < PROSENJIT_HANDLE_MAX; ++i) {
        struct pros_handle_entry *entry = &table->entries[i];
        if (!entry->object && entry->generation != UINT16_MAX) {
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
    if (!table || handle == PROSENJIT_HANDLE_INVALID) return -1;

    uint32_t slot = handle_slot(handle);
    uint16_t generation = handle_generation(handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return -1;

    const struct pros_handle_entry *entry = &table->entries[slot];
    if (!entry->object || entry->generation != generation ||
        entry->object->refcount == 0)
        return -1;

    if (expected_type != PROS_OBJECT_NONE &&
        entry->object->type != expected_type)
        return -1;

    if ((entry->rights & required_rights) != required_rights)
        return -1;

    return 0;
}

int handle_table_close_all(struct pros_handle_table *table) {
    if (handle_table_validate(table) != 0) return -1;
    /* No callbacks or concurrent mutation are permitted in this single-CPU
     * commit phase; validation guarantees every release has a live ref. */
    for (uint32_t i = 1; i < PROSENJIT_HANDLE_MAX; ++i) {
        if (table->entries[i].object) {
            uint64_t handle = make_handle(i, table->entries[i].generation);
            if (handle_close(table, handle) != 0) return -1;
        }
    }
    return 0;
}

int handle_close(struct pros_handle_table *table, uint64_t handle) {
    if (!table || handle == PROSENJIT_HANDLE_INVALID) return -1;

    uint32_t slot = handle_slot(handle);
    uint16_t generation = handle_generation(handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return -1;

    struct pros_handle_entry *entry = &table->entries[slot];
    if (!entry->object || entry->generation != generation ||
        entry->reserved != 0 || entry->rights == 0 ||
        (entry->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
        return -1;

    struct pros_object *object = entry->object;
    if (object->type == PROS_OBJECT_NONE ||
        object->type > PROS_OBJECT_IPC || object->reserved != 0 ||
        object->refcount == 0)
        return -1;
    /* Release first; a failed release must never destroy the handle slot. */
    if (object_release(object) != 0) return -1;
    entry->object = 0;
    entry->rights = 0;
    /*
     * Never wrap a handle generation. A wrapped generation would allow a
     * very old stale handle value to become valid again after enough reuse.
     * Retire that slot permanently instead; other slots remain available.
     */
    if (entry->generation != UINT16_MAX)
        ++entry->generation;

    return 0;
}

uint64_t handle_duplicate(struct pros_handle_table *table,
                          uint64_t source_handle,
                          uint64_t requested_rights) {
    if (!table || source_handle == PROSENJIT_HANDLE_INVALID ||
        requested_rights == 0 ||
        (requested_rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
        return PROSENJIT_HANDLE_INVALID;

    uint32_t slot = handle_slot(source_handle);
    uint16_t generation = handle_generation(source_handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return PROSENJIT_HANDLE_INVALID;

    struct pros_handle_entry *source = &table->entries[slot];
    if (!source->object || source->generation != generation ||
        source->object->refcount == 0)
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
        requested_rights == 0 ||
        (requested_rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
        return PROSENJIT_HANDLE_INVALID;

    uint32_t slot = handle_slot(source_handle);
    uint16_t generation = handle_generation(source_handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return PROSENJIT_HANDLE_INVALID;

    struct pros_handle_entry *source = &source_table->entries[slot];
    if (!source->object || source->generation != generation ||
        source->object->refcount == 0)
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
        requested_rights == 0 ||
        (requested_rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
        return PROSENJIT_HANDLE_INVALID;

    uint32_t slot = handle_slot(source_handle);
    uint16_t generation = handle_generation(source_handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return PROSENJIT_HANDLE_INVALID;

    struct pros_handle_entry *source = &source_table->entries[slot];
    if (!source->object || source->generation != generation ||
        source->object->refcount == 0)
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

/* Deterministic corruption-injection regression fixture for boot self-tests. */
int handle_table_selftest(void) {
    struct pros_handle_table table;
    struct pros_object live_object;
    struct pros_object corrupt_object;
    uint64_t handle;

    handle_table_init(&table);
    object_init(&live_object, PROS_OBJECT_FILE);
    handle = handle_alloc(&table, &live_object, PROSENJIT_CAP_READ);
    if (handle == PROSENJIT_HANDLE_INVALID || live_object.refcount != 2)
        return -1;

    object_init(&corrupt_object, PROS_OBJECT_FILE);
    corrupt_object.refcount = 0;
    table.entries[2].object = &corrupt_object;
    table.entries[2].rights = PROSENJIT_CAP_READ;
    if (handle_table_validate(&table) == 0 ||
        handle_table_close_all(&table) == 0)
        return -2;
    if (table.entries[1].object != &live_object ||
        table.entries[1].generation != (uint16_t)(handle >> HANDLE_SLOT_BITS) ||
        table.entries[1].rights != PROSENJIT_CAP_READ ||
        live_object.refcount != 2)
        return -3;

    table.entries[2].object = 0;
    table.entries[2].rights = 0;
    if (handle_table_validate(&table) != 0 ||
        handle_table_close_all(&table) != 0)
        return -4;
    if (table.entries[1].object || table.entries[1].rights != 0 ||
        table.entries[1].generation != 2 || live_object.refcount != 1)
        return -5;

    table.entries[3].generation = UINT16_MAX;
    if (handle_table_validate(&table) != 0)
        return -6;
    return 0;
}

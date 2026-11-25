#include "object.h"
#include "paging.h"

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

/*
 * Handle entries are kernel-owned, but corruption can replace an object
 * pointer with an arbitrary value. Prove the complete object is mapped in
 * supervisor-writable memory before preflight reads fields or teardown
 * decrements its reference count.
 */
int object_pointer_is_supervisor_writable(const struct pros_object *object)
{
    if (!object)
        return 0;

    uint64_t start = (uint64_t)(uintptr_t)object;
    if (start > UINT64_MAX - (uint64_t)sizeof(*object) + 1u)
        return 0;
    uint64_t end = start + (uint64_t)sizeof(*object) - 1u;
    uint64_t page = start & ~0xFFFULL;
    uint64_t last_page = end & ~0xFFFULL;

    for (;;) {
        uint64_t flags = 0u;
        if (paging_query_page(page, &flags) != 0 ||
            !(flags & PROSENJIT_PAGE_PRESENT) ||
            !(flags & PROSENJIT_PAGE_WRITABLE) ||
            (flags & PROSENJIT_PAGE_USER))
            return 0;
        if (page == last_page)
            break;
        if (page > UINT64_MAX - 0x1000ULL)
            return 0;
        page += 0x1000ULL;
    }
    return 1;
}

void object_init(struct pros_object *object, uint16_t type) {
    if (!object || type == PROS_OBJECT_NONE) return;
    object->type = type;
    object->reserved = 0;
    object->refcount = 1;
}

int object_retain(struct pros_object *object) {
    if (!object || object->refcount == 0 || object->refcount == UINT32_MAX)
        return -1;
    ++object->refcount;
    return 0;
}

int object_release(struct pros_object *object) {
    if (!object || object->refcount == 0) return -1;
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

int handle_table_close_all_preflight(const struct pros_handle_table *table) {
    if (!table) return -1;

    for (uint32_t i = 0; i < PROSENJIT_HANDLE_MAX; ++i) {
        const struct pros_handle_entry *entry = &table->entries[i];
        if (entry->reserved != 0u || entry->generation == 0u)
            return -1;
        if (i == 0u && entry->object) return -1;
        if (!entry->object) {
            if (entry->rights != 0u) return -1;
            continue;
        }
        /* A live UINT16_MAX generation is valid: close retires the slot
         * without wrapping it, and teardown must still release its object. */
        const struct pros_object *object = entry->object;
        if (!object_pointer_is_supervisor_writable(object))
            return -1;
        if (object->type == PROS_OBJECT_NONE ||
            object->type > PROS_OBJECT_IPC ||
            object->reserved != 0u || object->refcount == 0u ||
            entry->rights == 0u ||
            (entry->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u)
            return -1;

        uint32_t references = 0u;
        for (uint32_t j = 1; j < PROSENJIT_HANDLE_MAX; ++j)
            if (table->entries[j].object == object)
                ++references;
        if (object->refcount < references)
            return -1;
    }
    return 0;
}

uint64_t handle_alloc(struct pros_handle_table *table,
                      struct pros_object *object,
                      uint64_t rights) {
    if (!table || !object || object->refcount == 0 ||
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
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    if (handle_table_close_all_preflight(table) != 0) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }
    for (uint32_t i = 1; i < PROSENJIT_HANDLE_MAX; ++i) {
        if (table->entries[i].object) {
            uint64_t handle = make_handle(i, table->entries[i].generation);
            if (handle_close(table, handle) != 0) {
                __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
                return -1;
            }
        }
    }
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return 0;
}

int handle_close(struct pros_handle_table *table, uint64_t handle) {
    if (!table || handle == PROSENJIT_HANDLE_INVALID) return -1;

    uint32_t slot = handle_slot(handle);
    uint16_t generation = handle_generation(handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return -1;

    struct pros_handle_entry *entry = &table->entries[slot];
    if (!entry->object || entry->generation != generation)
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

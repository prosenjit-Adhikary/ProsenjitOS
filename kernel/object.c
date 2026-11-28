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
        if (paging_query_effective_page(page, &flags) != 0 ||
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

int object_validate_header(const struct pros_object *object)
{
    return object_pointer_is_supervisor_writable(object) &&
           object->type != PROS_OBJECT_NONE &&
           object->type <= PROS_OBJECT_IPC &&
           object->reserved == 0u &&
           object->refcount != 0u;
}

static int object_header_valid(const struct pros_object *object)
{
    return object_validate_header(object);
}

void object_init_impl(struct pros_object *object, uint16_t type) {
    if (!object_pointer_is_supervisor_writable(object) ||
        type == PROS_OBJECT_NONE || type > PROS_OBJECT_IPC) return;
    object->type = type;
    object->reserved = 0;
    object->refcount = 1;
}

int object_retain_impl(struct pros_object *object) {
    if (!object_header_valid(object) ||
        object->refcount == UINT32_MAX)
        return -1;
    ++object->refcount;
    return 0;
}

int object_release_impl(struct pros_object *object) {
    if (!object_header_valid(object)) return -1;
    --object->refcount;
    if (object->refcount == 0) object->type = PROS_OBJECT_NONE;
    return 0;
}

void handle_table_init_impl(struct pros_handle_table *table) {
    if (!table) return;

    for (uint32_t i = 0; i < PROSENJIT_HANDLE_MAX; ++i) {
        table->entries[i].object = 0;
        table->entries[i].generation = 1;
        table->entries[i].reserved = 0;
        table->entries[i].rights = 0;
    }
}

int handle_table_close_all_preflight_impl(const struct pros_handle_table *table) {
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

uint64_t handle_alloc_impl(struct pros_handle_table *table,
                      struct pros_object *object,
                      uint64_t rights) {
    if (!table || !object_header_valid(object) ||
        rights == 0 || (rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
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

int handle_check_impl(const struct pros_handle_table *table,
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
        entry->reserved != 0u || !object_header_valid(entry->object) ||
        entry->rights == 0u ||
        (entry->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u)
        return -1;

    if (expected_type != PROS_OBJECT_NONE &&
        entry->object->type != expected_type)
        return -1;

    if ((entry->rights & required_rights) != required_rights)
        return -1;

    return 0;
}

int handle_table_close_all_impl(struct pros_handle_table *table) {
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

int handle_close_impl(struct pros_handle_table *table, uint64_t handle) {
    if (!table || handle == PROSENJIT_HANDLE_INVALID) return -1;

    uint32_t slot = handle_slot(handle);
    uint16_t generation = handle_generation(handle);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return -1;

    struct pros_handle_entry *entry = &table->entries[slot];
    if (!entry->object || entry->generation != generation ||
        entry->reserved != 0u || !object_header_valid(entry->object) ||
        entry->rights == 0u ||
        (entry->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u)
        return -1;

    struct pros_object *object = entry->object;
    /*
     * Release first: if object metadata rejects the decrement, preserve the
     * handle as the owner's retry path. The caller holds local IRQ exclusion,
     * so no observer can see the transient released-object/live-slot state.
     */
    if (object_release(object) != 0)
        return -1;
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

uint64_t handle_duplicate_impl(struct pros_handle_table *table,
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
        source->reserved != 0u || !object_header_valid(source->object) ||
        source->rights == 0u ||
        (source->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u)
        return PROSENJIT_HANDLE_INVALID;

    /* Duplication can only attenuate authority; it can never add rights. */
    if ((source->rights & PROSENJIT_CAP_DUPLICATE) == 0 ||
        (requested_rights & ~source->rights) != 0)
        return PROSENJIT_HANDLE_INVALID;

    return handle_alloc(table, source->object, requested_rights);
}

uint64_t handle_transfer_copy_impl(struct pros_handle_table *source_table,
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
        source->reserved != 0u || !object_header_valid(source->object) ||
        source->rights == 0u ||
        (source->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u)
        return PROSENJIT_HANDLE_INVALID;

    /* Copy-transfer attenuates authority; the source remains valid. */
    if ((source->rights & PROSENJIT_CAP_TRANSFER) == 0 ||
        (requested_rights & ~source->rights) != 0)
        return PROSENJIT_HANDLE_INVALID;

    return handle_alloc(target_table, source->object, requested_rights);
}

uint64_t handle_transfer_move_impl(struct pros_handle_table *source_table,
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
        source->reserved != 0u || !object_header_valid(source->object) ||
        source->rights == 0u ||
        (source->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u)
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

/*
 * The current scheduler is single-CPU and timer-preemptible. Handle tables and
 * object reference counts are shared by threads in a process, so every public
 * operation must be one interrupt-excluded transaction. Save/restore preserves
 * an outer critical section when these APIs call one another. This is not an
 * SMP lock; add a real per-table/object lock before enabling multiple CPUs.
 */
static uint64_t object_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void object_irq_restore(uint64_t flags) {
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

void object_init(struct pros_object *object, uint16_t type) {
    uint64_t flags = object_irq_save();
    object_init_impl(object, type);
    object_irq_restore(flags);
}

int object_retain(struct pros_object *object) {
    uint64_t flags = object_irq_save();
    int result = object_retain_impl(object);
    object_irq_restore(flags);
    return result;
}

int object_release(struct pros_object *object) {
    uint64_t flags = object_irq_save();
    int result = object_release_impl(object);
    object_irq_restore(flags);
    return result;
}

void handle_table_init(struct pros_handle_table *table) {
    uint64_t flags = object_irq_save();
    handle_table_init_impl(table);
    object_irq_restore(flags);
}

int handle_table_close_all_preflight(const struct pros_handle_table *table) {
    uint64_t flags = object_irq_save();
    int result = handle_table_close_all_preflight_impl(table);
    object_irq_restore(flags);
    return result;
}

uint64_t handle_alloc(struct pros_handle_table *table, struct pros_object *object, uint64_t rights) {
    uint64_t flags = object_irq_save();
    uint64_t result = handle_alloc_impl(table, object, rights);
    object_irq_restore(flags);
    return result;
}

int handle_check(const struct pros_handle_table *table, uint64_t handle, uint64_t required_rights, uint16_t expected_type) {
    uint64_t flags = object_irq_save();
    int result = handle_check_impl(table, handle, required_rights, expected_type);
    object_irq_restore(flags);
    return result;
}

int handle_table_close_all(struct pros_handle_table *table) {
    uint64_t flags = object_irq_save();
    int result = handle_table_close_all_impl(table);
    object_irq_restore(flags);
    return result;
}

int handle_close(struct pros_handle_table *table, uint64_t handle) {
    uint64_t flags = object_irq_save();
    int result = handle_close_impl(table, handle);
    object_irq_restore(flags);
    return result;
}

uint64_t handle_duplicate(struct pros_handle_table *table, uint64_t source_handle, uint64_t requested_rights) {
    uint64_t flags = object_irq_save();
    uint64_t result = handle_duplicate_impl(table, source_handle, requested_rights);
    object_irq_restore(flags);
    return result;
}

uint64_t handle_transfer_copy(struct pros_handle_table *source_table, struct pros_handle_table *target_table, uint64_t source_handle, uint64_t requested_rights) {
    uint64_t flags = object_irq_save();
    uint64_t result = handle_transfer_copy_impl(source_table, target_table, source_handle, requested_rights);
    object_irq_restore(flags);
    return result;
}

uint64_t handle_transfer_move(struct pros_handle_table *source_table, struct pros_handle_table *target_table, uint64_t source_handle, uint64_t requested_rights) {
    uint64_t flags = object_irq_save();
    uint64_t result = handle_transfer_move_impl(source_table, target_table, source_handle, requested_rights);
    object_irq_restore(flags);
    return result;
}


/*
 * Focused lifecycle regression for close ordering and close-all preflight.
 * This runs in the kernel self-test environment where paging queries can
 * validate the mapped object pointers used by the fixture.
 */
int handle_table_selftest(void) {
    static struct pros_handle_table table;
    static struct pros_object live_object;
    static struct pros_object corrupt_object;
    uint64_t live_handle;

    handle_table_init(&table);
    object_init(&live_object, PROS_OBJECT_FILE);
    live_handle = handle_alloc(&table, &live_object, PROSENJIT_CAP_READ);
    if (live_handle == PROSENJIT_HANDLE_INVALID || live_object.refcount != 2u)
        return -1;

    object_init(&corrupt_object, PROS_OBJECT_FILE);
    corrupt_object.refcount = 0u;
    table.entries[2].object = &corrupt_object;
    table.entries[2].generation = 7u;
    table.entries[2].rights = PROSENJIT_CAP_READ;

    struct pros_handle_entry saved_live = table.entries[1];
    struct pros_handle_entry saved_corrupt = table.entries[2];
    if (handle_table_close_all_preflight(&table) == 0 ||
        handle_table_close_all(&table) == 0)
        return -2;
    if (table.entries[1].object != saved_live.object ||
        table.entries[1].generation != saved_live.generation ||
        table.entries[1].reserved != saved_live.reserved ||
        table.entries[1].rights != saved_live.rights ||
        table.entries[2].object != saved_corrupt.object ||
        table.entries[2].generation != saved_corrupt.generation ||
        table.entries[2].reserved != saved_corrupt.reserved ||
        table.entries[2].rights != saved_corrupt.rights ||
        live_object.refcount != 2u || corrupt_object.refcount != 0u)
        return -3;

    table.entries[2].object = 0;
    table.entries[2].rights = 0;
    if (handle_table_close_all_preflight(&table) != 0 ||
        handle_table_close_all(&table) != 0)
        return -4;
    if (table.entries[1].object != 0 ||
        table.entries[1].rights != 0 ||
        table.entries[1].generation !=
            (uint16_t)(handle_generation(live_handle) + 1u) ||
        live_object.refcount != 1u)
        return -5;

    /* A rejected individual close leaves its slot and corrupt object intact. */
    static struct pros_object malformed_object;
    object_init(&malformed_object, PROS_OBJECT_FILE);
    malformed_object.reserved = 1u;
    table.entries[3].object = &malformed_object;
    table.entries[3].generation = 9u;
    table.entries[3].rights = PROSENJIT_CAP_READ;
    struct pros_handle_entry saved_malformed = table.entries[3];
    if (handle_close(&table, make_handle(3u, 9u)) == 0 ||
        table.entries[3].object != saved_malformed.object ||
        table.entries[3].generation != saved_malformed.generation ||
        table.entries[3].reserved != saved_malformed.reserved ||
        table.entries[3].rights != saved_malformed.rights ||
        malformed_object.type != PROS_OBJECT_FILE ||
        malformed_object.reserved != 1u ||
        malformed_object.refcount != 1u)
        return -6;

    /* A forged pointer must fail before access or slot mutation. */
    static struct pros_handle_table forged_table;
    struct pros_object *forged_object =
        (struct pros_object *)(uintptr_t)0x0000800000000000ULL;
    handle_table_init(&forged_table);
    forged_table.entries[1].object = forged_object;
    forged_table.entries[1].generation = 13u;
    forged_table.entries[1].rights = PROSENJIT_CAP_READ;
    struct pros_handle_entry saved_forged = forged_table.entries[1];
    if (handle_table_close_all_preflight(&forged_table) == 0 ||
        handle_table_close_all(&forged_table) == 0 ||
        handle_close(&forged_table, make_handle(1u, 13u)) == 0 ||
        forged_table.entries[1].object != saved_forged.object ||
        forged_table.entries[1].generation != saved_forged.generation ||
        forged_table.entries[1].reserved != saved_forged.reserved ||
        forged_table.entries[1].rights != saved_forged.rights)
        return -7;

    /* Final release succeeds before the handle slot is retired. */
    table.entries[3].object = 0;
    table.entries[3].rights = 0;
    static struct pros_object final_object;
    object_init(&final_object, PROS_OBJECT_FILE);
    table.entries[4].object = &final_object;
    table.entries[4].generation = 11u;
    table.entries[4].rights = PROSENJIT_CAP_READ;
    if (handle_close(&table, make_handle(4u, 11u)) != 0 ||
        table.entries[4].object != 0 ||
        table.entries[4].rights != 0 ||
        table.entries[4].generation != 12u ||
        final_object.type != PROS_OBJECT_NONE ||
        final_object.refcount != 0u)
        return -8;

    table.entries[5].generation = UINT16_MAX;
    if (handle_table_close_all_preflight(&table) != 0)
        return -9;
    return 0;
}

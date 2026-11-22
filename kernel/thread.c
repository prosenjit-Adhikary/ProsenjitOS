#include <stddef.h>
#include "thread.h"
#include "address_space.h"
#include "ipc.h"
#include "heap.h"
#include "memory.h"
#include "paging.h"

static uint64_t thread_irq_save(void);
static void thread_irq_restore(uint64_t flags);

static struct pros_thread *runq_head;
static struct pros_thread *runq_tail;
static struct pros_thread *current_thread;
static struct pros_thread *thread_registry;
/* Trusted identity map: authenticate registry links before dereferencing. */
static struct pros_thread *thread_registry_slots[64];
/* Single-CPU aggregate process-reap workspace: 64 threads x 4 stack pages. */
static struct pros_thread *process_reap_threads[64];
static uint64_t process_reap_virtual_pages[256];
static uint64_t process_reap_physical_pages[256];

extern int cpu_set_kernel_stack(uint64_t stack_top);
extern uint64_t cpu_get_kernel_stack(void);
extern char stack_top[];

#define PROS_THREAD_STACK_ARENA_BASE 0xFFFF800800000000ULL
#define PROS_THREAD_STACK_SLOT_PAGES 6ULL
#define PROS_THREAD_STACK_SLOT_COUNT 64u
#define PROS_THREAD_STACK_PAGE_COUNT 4u

/* Single-CPU allocator until scheduler/run-queue locking is implemented. */
static uint64_t thread_stack_slots[(PROS_THREAD_STACK_SLOT_COUNT + 63u) / 64u];
/* Slots whose cleanup could not be proven remain permanently quarantined. */
static uint64_t thread_stack_quarantined_slots[(PROS_THREAD_STACK_SLOT_COUNT + 63u) / 64u];

static int thread_kernel_stack_allocate(struct pros_thread *thread) {
    uint32_t slot = PROS_THREAD_STACK_SLOT_COUNT;
    for (uint32_t i = 0; i < PROS_THREAD_STACK_SLOT_COUNT; ++i) {
        uint64_t mask = 1ULL << (i & 63u);
        if (!(thread_stack_slots[i >> 6] & mask)) {
            thread_stack_slots[i >> 6] |= mask;
            slot = i;
            break;
        }
    }
    if (slot == PROS_THREAD_STACK_SLOT_COUNT) return -1;

    uint64_t slot_base = PROS_THREAD_STACK_ARENA_BASE +
                         (uint64_t)slot * PROS_THREAD_STACK_SLOT_PAGES *
                         PROSENJIT_PAGE_SIZE;
    uint64_t stack_base = slot_base + PROSENJIT_PAGE_SIZE;
    uint64_t pages[PROS_THREAD_STACK_PAGE_COUNT] = {0};
    uint32_t mapped = 0u;

    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i) {
        pages[i] = physical_page_alloc();
        if (!pages[i]) goto rollback;
        if (paging_map_page(stack_base + (uint64_t)i * PROSENJIT_PAGE_SIZE,
                            pages[i],
                            PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0)
            goto rollback;
        ++mapped;
    }

    thread->kernel_stack_base = (void *)(uintptr_t)stack_base;
    thread->kernel_stack_size = PROS_THREAD_KERNEL_STACK_SIZE;
    thread->kernel_stack_slot = slot;
    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i)
        thread->kernel_stack_pages[i] = pages[i];
    return 0;

rollback: {
    uint64_t rollback_vas[PROS_THREAD_STACK_PAGE_COUNT];
    uint64_t rollback_pas[PROS_THREAD_STACK_PAGE_COUNT];
    uint64_t allocated_pages[PROS_THREAD_STACK_PAGE_COUNT];
    uint32_t allocated_count = 0u;
    for (uint32_t i = 0; i < mapped; ++i) {
        rollback_vas[i] = stack_base + (uint64_t)i * PROSENJIT_PAGE_SIZE;
        rollback_pas[i] = pages[i];
    }
    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i)
        if (pages[i]) allocated_pages[allocated_count++] = pages[i];

    /*
     * If either complete operation cannot be proven, retain the slot as a
     * quarantine reservation. Never recycle a VA whose cleanup is uncertain.
     */
    if (allocated_count != 0u &&
        paging_unmap_pages_expected_and_free(
            rollback_vas, rollback_pas, mapped,
            allocated_pages, allocated_count) != 0) {
        thread_stack_quarantined_slots[slot >> 6] |= 1ULL << (slot & 63u);
        return -1;
    }

    thread_stack_slots[slot >> 6] &= ~(1ULL << (slot & 63u));
    return -1;
    }
}

/*
 * A registry pointer is not safe to dereference merely because it matches the
 * parallel slot table: both kernel-resident links can be corrupted together.
 * Validate the complete descriptor mapping before reading any descriptor field.
 */
static int thread_registry_node_readable(const struct pros_thread *thread) {
    if (!thread)
        return 0;
    uint64_t start = (uint64_t)(uintptr_t)thread;
    if (start > UINT64_MAX - (uint64_t)sizeof(*thread) + 1u)
        return 0;
    uint64_t end = start + (uint64_t)sizeof(*thread) - 1u;
    uint64_t page = start & ~(PROSENJIT_PAGE_SIZE - 1u);
    uint64_t last_page = end & ~(PROSENJIT_PAGE_SIZE - 1u);
    for (;;) {
        uint64_t flags = 0u;
        if (paging_query_page(page, &flags) != 0 ||
            !(flags & PROSENJIT_PAGE_PRESENT) ||
            (flags & PROSENJIT_PAGE_USER))
            return 0;
        if (page == last_page)
            break;
        if (page > UINT64_MAX - PROSENJIT_PAGE_SIZE)
            return 0;
        page += PROSENJIT_PAGE_SIZE;
    }
    return 1;
}

static int thread_kernel_stack_valid(const struct pros_thread *thread) {
    if (!thread || thread->kernel_stack_slot >= PROS_THREAD_STACK_SLOT_COUNT ||
        !thread->kernel_stack_base ||
        thread->kernel_stack_size != PROS_THREAD_KERNEL_STACK_SIZE)
        return 0;

    uint32_t slot = thread->kernel_stack_slot;
    if (!(thread_stack_slots[slot >> 6] & (1ULL << (slot & 63u))))
        return 0;

    uint64_t slot_base = PROS_THREAD_STACK_ARENA_BASE +
                         (uint64_t)slot * PROS_THREAD_STACK_SLOT_PAGES *
                         PROSENJIT_PAGE_SIZE;
    uint64_t expected_base = slot_base + PROSENJIT_PAGE_SIZE;
    uint64_t guard_physical = UINT64_MAX;
    if ((uint64_t)(uintptr_t)thread->kernel_stack_base != expected_base ||
        paging_query_physical_checked(slot_base, &guard_physical) != 0 ||
        guard_physical != 0u ||
        paging_query_physical_checked(slot_base + 5u * PROSENJIT_PAGE_SIZE,
                                      &guard_physical) != 0 ||
        guard_physical != 0u)
        return 0;

    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i) {
        uint64_t va = expected_base + (uint64_t)i * PROSENJIT_PAGE_SIZE;
        uint64_t effective_flags = 0u;
        if (!thread->kernel_stack_pages[i] ||
            paging_query_physical_checked(va, &guard_physical) != 0 ||
            guard_physical != thread->kernel_stack_pages[i] ||
            paging_query_effective_page(va, &effective_flags) != 0 ||
            !(effective_flags & PROSENJIT_PAGE_PRESENT) ||
            !(effective_flags & PROSENJIT_PAGE_WRITABLE) ||
            (effective_flags & PROSENJIT_PAGE_USER) ||
            !(effective_flags & PROSENJIT_PAGE_NX) ||
            physical_page_is_page_table(thread->kernel_stack_pages[i]) ||
            !physical_page_is_allocated(thread->kernel_stack_pages[i]))
            return 0;
    }
    return 1;
}

/*
 * Bounded, read-only global registry validation. The fixed slot count is the
 * hard upper bound; repeated nodes/cycles, duplicate slots, cross-thread
 * physical-page aliases, and allocator/registry bitmap disagreement reject.
 */
static int thread_registry_validate(void) {
    struct pros_thread *nodes[PROS_THREAD_STACK_SLOT_COUNT];
    uint64_t physical_pages[PROS_THREAD_STACK_SLOT_COUNT *
                            PROS_THREAD_STACK_PAGE_COUNT];
    uint64_t seen_slots = 0u;
    uint32_t node_count = 0u;
    uint32_t page_count = 0u;

    for (struct pros_thread *it = thread_registry; it;) {
        uint32_t trusted_slot = PROS_THREAD_STACK_SLOT_COUNT;
        for (uint32_t s = 0u; s < PROS_THREAD_STACK_SLOT_COUNT; ++s)
            if (thread_registry_slots[s] == it) {
                if (trusted_slot != PROS_THREAD_STACK_SLOT_COUNT)
                    return -1;
                trusted_slot = s;
            }
        if (trusted_slot == PROS_THREAD_STACK_SLOT_COUNT ||
            node_count >= PROS_THREAD_STACK_SLOT_COUNT ||
            !thread_registry_node_readable(it))
            return -1;
        for (uint32_t i = 0u; i < node_count; ++i)
            if (nodes[i] == it)
                return -1;
        nodes[node_count++] = it;

        if (it->owner_kind > PROS_THREAD_OWNER_KERNEL ||
            (it->owner_kind == PROS_THREAD_OWNER_PROCESS && !it->process) ||
            (it->owner_kind == PROS_THREAD_OWNER_KERNEL && it->process) ||
            it->kernel_stack_slot >= PROS_THREAD_STACK_SLOT_COUNT ||
            !it->kernel_stack_base ||
            it->kernel_stack_size != PROS_THREAD_KERNEL_STACK_SIZE)
            return -1;
        uint32_t slot = it->kernel_stack_slot;
        if (slot != trusted_slot || thread_registry_slots[slot] != it)
            return -1;
        uint64_t slot_mask = 1ULL << slot;
        if (seen_slots & slot_mask)
            return -1;
        seen_slots |= slot_mask;
        if (!(thread_stack_slots[0] & slot_mask))
            return -1;

        uint64_t expected_base = PROS_THREAD_STACK_ARENA_BASE +
            (uint64_t)slot * PROS_THREAD_STACK_SLOT_PAGES * PROSENJIT_PAGE_SIZE +
            PROSENJIT_PAGE_SIZE;
        if ((uint64_t)(uintptr_t)it->kernel_stack_base != expected_base)
            return -1;

        int quarantined =
            (thread_stack_quarantined_slots[0] & slot_mask) != 0u;
        if (!quarantined && !thread_kernel_stack_valid(it))
            return -1;

        for (uint32_t p = 0u; p < PROS_THREAD_STACK_PAGE_COUNT; ++p) {
            uint64_t physical = it->kernel_stack_pages[p];
            if (!physical || physical_page_is_page_table(physical) ||
                !physical_page_is_allocated(physical))
                return -1;
            for (uint32_t q = 0u; q < page_count; ++q)
                if (physical_pages[q] == physical)
                    return -1;
            physical_pages[page_count++] = physical;
        }
        it = it->registry_next;
    }

    /* Every trusted identity slot must be represented exactly once. */
    for (uint32_t s = 0u; s < PROS_THREAD_STACK_SLOT_COUNT; ++s) {
        uint64_t mask = 1ULL << s;
        if (thread_registry_slots[s]) {
            if (!(seen_slots & mask)) return -1;
        } else if (seen_slots & mask) {
            return -1;
        }
    }

    /* The scheduler's current pointer is also a trusted registry identity. */
    if (current_thread) {
        int current_registered = 0;
        for (uint32_t i = 0u; i < node_count; ++i) {
            if (nodes[i] == current_thread) {
                current_registered = 1;
                break;
            }
        }
        if (!current_registered)
            return -1;
    }

    /* Any reserved slot not owned by a registry node must be quarantined. */
    uint64_t quarantined_slots = thread_stack_quarantined_slots[0];
    if ((quarantined_slots & ~thread_stack_slots[0]) != 0u ||
        (thread_stack_slots[0] & ~(seen_slots | quarantined_slots)) != 0u)
        return -1;
    return 0;
}

/*
 * An address space must never claim ownership of a physical page that is
 * simultaneously owned by any registered supervisor kernel stack. Separate
 * address-space and thread preflights cannot detect this cross-subsystem
 * alias on their own; freeing either side first would invalidate the other.
 * The registry is bounded/validated before the cross-check.
 */
int thread_address_space_disjoint_preflight(
    const struct pros_address_space *space) {
    uint64_t irq_flags = thread_irq_save();
    int result = -1;
    if (!space || thread_registry_validate() != 0 ||
        address_space_owned_pages_preflight(space) != 0)
        goto out;

    uint64_t owned_count = 0u;
    const uint64_t max_owned_pages =
        PROSENJIT_PHYS_LIMIT / PROSENJIT_PAGE_SIZE;
    for (const struct pros_owned_page *owned = space->owned_pages;
         owned; owned = owned->next) {
        uint64_t physical = owned->physical_address;
        if (++owned_count > max_owned_pages ||
            physical == 0u ||
            (physical & (PROSENJIT_PAGE_SIZE - 1u)) != 0u)
            goto out;

        for (struct pros_thread *thread = thread_registry; thread;
             thread = thread->registry_next) {
            for (uint32_t page = 0u;
                 page < PROS_THREAD_STACK_PAGE_COUNT; ++page) {
                if (thread->kernel_stack_pages[page] == physical)
                    goto out;
            }
        }
    }
    result = 0;
out:
    thread_irq_restore(irq_flags);
    return result;
}

/*
 * Boot regression for the bounded registry/stack ownership validator.
 * Each corruption is applied to valid, trusted fixture nodes and restored
 * before returning. No allocator, PTE, queue, or process state is modified.
 */

int thread_stack_allocator_snapshot(uint64_t *allocated_slots,
                                    uint64_t *quarantined_slots);

int thread_registry_integrity_selftest(void) {
    struct pros_thread *nodes[PROS_THREAD_STACK_SLOT_COUNT];
    struct pros_thread *saved_next[PROS_THREAD_STACK_SLOT_COUNT];
    uint64_t saved_slots;
    uint64_t saved_quarantine;
    uint64_t irq_flags = thread_irq_save();
    uint32_t count = 0u;
    int result = -1;

    if (thread_registry_validate() != 0 ||
        thread_stack_allocator_snapshot(&saved_slots, &saved_quarantine) != 0)
        goto out;

    for (struct pros_thread *it = thread_registry; it; it = it->registry_next) {
        if (count >= PROS_THREAD_STACK_SLOT_COUNT)
            goto out;
        nodes[count] = it;
        saved_next[count] = it->registry_next;
        ++count;
    }
    if (count == 0u)
        goto out;

    /*
     * Lifecycle helpers dereference current_thread after registry validation.
     * A forged current pointer must therefore be rejected by the same bounded
     * identity gate, without dereferencing the forged address.
     */
    struct pros_thread *saved_current = current_thread;
    current_thread = (struct pros_thread *)(uintptr_t)1u;
    int rejected_current_alias = thread_registry_validate() != 0;
    current_thread = saved_current;
    if (!rejected_current_alias || thread_registry_validate() != 0)
        goto out;

    /*
     * Corrupt both copies of the registry head identity at once. Matching a
     * pointer in the trusted slot table is not sufficient if that table entry
     * and the list head are damaged together: the full descriptor mapping
     * check must reject the non-canonical address before field dereference.
     */
    struct pros_thread *saved_registry_head = thread_registry;
    uint32_t head_slot = nodes[0]->kernel_stack_slot;
    struct pros_thread *saved_head_slot = thread_registry_slots[head_slot];
    struct pros_thread *invalid_node =
        (struct pros_thread *)(uintptr_t)0x0000800000000000ULL;
    thread_registry = invalid_node;
    thread_registry_slots[head_slot] = invalid_node;
    int rejected_joint_pointer_corruption =
        thread_registry_validate() != 0;
    thread_registry = saved_registry_head;
    thread_registry_slots[head_slot] = saved_head_slot;
    if (!rejected_joint_pointer_corruption ||
        thread_registry_validate() != 0)
        goto out;

    /* Public lifecycle APIs must authenticate candidate pointers first. */
    if (thread_set_state(invalid_node, PROS_THREAD_READY) == 0 ||
        thread_reap_kernel(invalid_node) == 0 ||
        thread_abort_unpublished(invalid_node) == 0)
        goto out;

    /* Owner kind and process-pointer nullability are registry invariants. */
    uint32_t saved_owner_kind = nodes[0]->owner_kind;
    struct pros_process *saved_owner_process = nodes[0]->process;
    nodes[0]->owner_kind = UINT32_MAX;
    int rejected_owner_kind = thread_registry_validate() != 0;
    nodes[0]->owner_kind = saved_owner_kind;
    if (!rejected_owner_kind || thread_registry_validate() != 0)
        goto out;
    if (saved_owner_kind == PROS_THREAD_OWNER_PROCESS)
        nodes[0]->process = 0;
    else
        nodes[0]->process = (struct pros_process *)(uintptr_t)1u;
    int rejected_owner_relation = thread_registry_validate() != 0;
    nodes[0]->process = saved_owner_process;
    if (!rejected_owner_relation || thread_registry_validate() != 0)
        goto out;

    /*
     * Cross-subsystem ownership regression: a page recorded as address-space
     * owned must not alias a live registered thread's supervisor stack page.
     * Use a real heap-owned metadata node so the allocation-identity contract
     * is exercised without changing any mapping or allocator ownership state.
     */
    struct pros_owned_page *stack_alias =
        (struct pros_owned_page *)kmalloc(sizeof(*stack_alias));
    if (!stack_alias)
        goto out;
    stack_alias->virtual_address = 0x400000u;
    stack_alias->physical_address = nodes[0]->kernel_stack_pages[0];
    stack_alias->next = 0;
    struct pros_address_space alias_space = {
        .root = 0,
        .owned_pages = stack_alias
    };
    int stack_alias_rejected =
        thread_address_space_disjoint_preflight(&alias_space) != 0;
    alias_space.owned_pages = (struct pros_owned_page *)(uintptr_t)1u;
    int invalid_owned_link_rejected =
        thread_address_space_disjoint_preflight(&alias_space) != 0;
    kfree(stack_alias);
    if (!stack_alias_rejected || !invalid_owned_link_rejected)
        goto out;

    /*
     * Stack pages are ordinary allocator pages, never page-table-typed pages.
     * Prove the registry preflight rejects this ownership-type mismatch before
     * stack release can partially commit a multi-thread reap.
     */
    {
        struct pros_thread *target = 0;
        uint64_t rsp;
        uint64_t active_tss_stack = cpu_get_kernel_stack();
        __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
        for (uint32_t i = 0u; i < count; ++i) {
            uint64_t base = (uint64_t)(uintptr_t)nodes[i]->kernel_stack_base;
            uint64_t top = base + nodes[i]->kernel_stack_size;
            if (nodes[i] == current_thread ||
                (active_tss_stack >= base && active_tss_stack <= top) ||
                (rsp >= base && rsp < top))
                continue;
            target = nodes[i];
            break;
        }
        if (!target)
            goto out;

        uint64_t stack_va =
            (uint64_t)(uintptr_t)target->kernel_stack_base;
        uint64_t original_page = target->kernel_stack_pages[0];
        uint64_t table_page = physical_page_alloc_table();
        if (!table_page)
            goto out;

        if (paging_unmap_pages_expected(&stack_va, &original_page, 1u) != 0) {
            (void)physical_page_free_table(table_page);
            goto out;
        }
        if (paging_map_page(stack_va, table_page,
                            PROSENJIT_PAGE_WRITABLE |
                            PROSENJIT_PAGE_NX) != 0) {
            (void)paging_map_page(stack_va, original_page,
                                  PROSENJIT_PAGE_WRITABLE |
                                  PROSENJIT_PAGE_NX);
            (void)physical_page_free_table(table_page);
            goto out;
        }

        target->kernel_stack_pages[0] = table_page;
        int rejected = thread_registry_validate() != 0;
        target->kernel_stack_pages[0] = original_page;

        int unmap_rc = paging_unmap_pages_expected(&stack_va, &table_page, 1u);
        int remap_rc = paging_map_page(stack_va, original_page,
                                       PROSENJIT_PAGE_WRITABLE |
                                       PROSENJIT_PAGE_NX);
        int free_rc = physical_page_free_table(table_page);
        if (!rejected || unmap_rc != 0 || remap_rc != 0 || free_rc != 0 ||
            thread_registry_validate() != 0)
            goto out;
    }

    /* A cycle, including a one-node self-cycle, must fail in bounded time. */
    nodes[count - 1u]->registry_next = nodes[0];
    if (thread_registry_validate() == 0) {
        nodes[count - 1u]->registry_next = saved_next[count - 1u];
        goto out;
    }
    nodes[count - 1u]->registry_next = saved_next[count - 1u];

    /* A forged wild next pointer must be rejected before dereference. */
    nodes[count - 1u]->registry_next = (struct pros_thread *)(uintptr_t)1u;
    if (thread_registry_validate() == 0) {
        nodes[count - 1u]->registry_next = saved_next[count - 1u];
        goto out;
    }
    nodes[count - 1u]->registry_next = saved_next[count - 1u];

    if (count >= 2u) {
        uint32_t saved_slot = nodes[1]->kernel_stack_slot;
        nodes[1]->kernel_stack_slot = nodes[0]->kernel_stack_slot;
        if (thread_registry_validate() == 0) {
            nodes[1]->kernel_stack_slot = saved_slot;
            goto out;
        }
        nodes[1]->kernel_stack_slot = saved_slot;

        uint64_t saved_page = nodes[1]->kernel_stack_pages[0];
        nodes[1]->kernel_stack_pages[0] = nodes[0]->kernel_stack_pages[0];
        if (thread_registry_validate() == 0) {
            nodes[1]->kernel_stack_pages[0] = saved_page;
            goto out;
        }
        nodes[1]->kernel_stack_pages[0] = saved_page;
    }

    /* The global slot bitmap must agree with the registry's ownership set. */
    uint32_t slot = nodes[0]->kernel_stack_slot;
    uint64_t mask = 1ULL << slot;
    thread_stack_slots[0] &= ~mask;
    if (thread_registry_validate() == 0) {
        thread_stack_slots[0] |= mask;
        goto out;
    }
    thread_stack_slots[0] |= mask;

    if (thread_stack_slots[0] != saved_slots ||
        thread_stack_quarantined_slots[0] != saved_quarantine)
        goto out;
    for (uint32_t i = 0u; i < count; ++i)
        if (nodes[i]->registry_next != saved_next[i])
            goto out;
    if (thread_registry_validate() != 0)
        goto out;

    result = 0;
out:
    thread_irq_restore(irq_flags);
    return result;
}

static int thread_kernel_stack_release(struct pros_thread *thread) {
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    if (!thread_kernel_stack_valid(thread)) {
        if (thread && thread->kernel_stack_slot < PROS_THREAD_STACK_SLOT_COUNT)
            thread_stack_quarantined_slots[thread->kernel_stack_slot >> 6] |=
                1ULL << (thread->kernel_stack_slot & 63u);
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }

    uint32_t slot = thread->kernel_stack_slot;
    uint64_t stack_base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
    uint64_t stack_top = stack_base + thread->kernel_stack_size;
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    uint64_t active_tss_stack = cpu_get_kernel_stack();
    if ((active_tss_stack >= stack_base && active_tss_stack <= stack_top) ||
        (rsp >= stack_base && rsp < stack_top)) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }

    uint64_t virtual_pages[PROS_THREAD_STACK_PAGE_COUNT];
    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i)
        virtual_pages[i] = stack_base + (uint64_t)i * PROSENJIT_PAGE_SIZE;

    /*
     * The page-table operation validates every expected VA->PA mapping before
     * clearing any PTE. Interrupt exclusion keeps the subsequent allocator
     * batch-free preflight/commit stable on the supported single-CPU model.
     */
    if (paging_unmap_pages_expected_and_free(
            virtual_pages, thread->kernel_stack_pages,
            PROS_THREAD_STACK_PAGE_COUNT,
            thread->kernel_stack_pages,
            PROS_THREAD_STACK_PAGE_COUNT) != 0) {
        thread_stack_quarantined_slots[slot >> 6] |= 1ULL << (slot & 63u);
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }

    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i)
        thread->kernel_stack_pages[i] = 0u;
    thread_stack_slots[slot >> 6] &= ~(1ULL << (slot & 63u));
    thread_stack_quarantined_slots[slot >> 6] &= ~(1ULL << (slot & 63u));
    if (thread_registry_slots[slot] == thread)
        thread_registry_slots[slot] = 0;
    thread->kernel_stack_base = 0;
    thread->kernel_stack_size = 0;
    thread->kernel_stack_slot = PROS_THREAD_STACK_SLOT_COUNT;
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return 0;
}

/*
 * Publish only a validated, owned supervisor stack as the TSS privilege-entry
 * stack. CPL3 INT 0x80 now uses IST=0 and therefore enters through this RSP0;
 * the timer IRQ remains on its current interrupt-entry path.
 */
static int thread_activate_kernel_stack(const struct pros_thread *thread) {
    if (!thread || !thread->kernel_stack_base ||
        thread->kernel_stack_size < PROS_THREAD_KERNEL_STACK_SIZE)
        return -1;

    uint64_t base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
    if (base > UINT64_MAX - thread->kernel_stack_size)
        return -1;
    uint64_t top = base + thread->kernel_stack_size;
    /* Guarded kernel stack mappings live in the canonical high-half supervisor range. */
    if (top <= base || (top & 0xFULL) != 0 ||
        top < 0xFFFF800000000000ULL)
        return -1;

    return cpu_set_kernel_stack(top);
}

static int thread_activate_bootstrap_stack(void) {
    uint64_t top = (uint64_t)(uintptr_t)stack_top;
    if (!top || (top & 0xFULL) != 0)
        return -1;
    return cpu_set_kernel_stack(top);
}

static void thread_restore_kernel_stack(const struct pros_thread *thread) {
    if (thread && thread_activate_kernel_stack(thread) == 0)
        return;
    (void)thread_activate_bootstrap_stack();
}

/* Queue membership and thread state must change as one scheduler operation. */
static int thread_is_queued(const struct pros_thread *thread) {
    return thread && (thread == runq_head || thread->next || thread->prev);
}

/*
 * Membership is established by pointer comparison against the already
 * validated, bounded registry. Do not dereference an untrusted queue pointer
 * until this predicate succeeds.
 */
static int thread_registry_contains(const struct pros_thread *candidate) {
    if (!candidate) return 0;
    uint32_t count = 0u;
    for (struct pros_thread *node = thread_registry; node;
         node = node->registry_next) {
        if (count++ >= PROS_THREAD_STACK_SLOT_COUNT) return 0;
        if (node == candidate) return 1;
    }
    return 0;
}

/*
 * IPC waiter entries are external references to thread descriptors. Authenticate
 * pointer identity against the bounded registry before reading the descriptor,
 * then require the BLOCKED state and exact channel back-pointer.
 */
int thread_ipc_waiter_validate(const struct pros_thread *candidate,
                               const void *channel) {
    if (!candidate || !channel || thread_registry_validate() != 0 ||
        !thread_registry_contains(candidate))
        return -1;
    return candidate->state == PROS_THREAD_BLOCKED &&
           candidate->wait_channel == channel ? 0 : -1;
}

/*
 * Read-only run-queue integrity gate used before reap and spawn rollback.
 * Registry membership is checked before dereferencing each queue pointer;
 * traversal is bounded so cycles fail closed.
 */
static int thread_runq_validate(void) {
    if (thread_registry_validate() != 0 ||
        ((!runq_head) != (!runq_tail)))
        return -1;
    if (!runq_head) return 0;

    if (!thread_registry_contains(runq_head) ||
        !thread_registry_contains(runq_tail))
        return -1;
    if (runq_head->prev || runq_tail->next) return -1;

    uint32_t count = 0u;
    struct pros_thread *previous = 0;
    struct pros_thread *it = runq_head;
    while (it) {
        if (count >= PROS_THREAD_STACK_SLOT_COUNT ||
            !thread_registry_contains(it))
            return -1;
        if (it->prev != previous || it->state != PROS_THREAD_READY)
            return -1;
        previous = it;
        it = it->next;
        ++count;
    }
    return previous == runq_tail ? 0 : -1;
}

/* Unlink a specific queued thread without disturbing other runnable entries. */
int thread_remove_queued(struct pros_thread *thread) {
    if (!thread || thread_runq_validate() != 0) return -1;
    struct pros_thread *it = runq_head;
    while (it && it != thread) it = it->next;
    if (!it) return -1;

    if (it->prev) it->prev->next = it->next;
    else runq_head = it->next;
    if (it->next) it->next->prev = it->prev;
    else runq_tail = it->prev;
    it->next = 0;
    it->prev = 0;
    return 0;
}

static int thread_process_runnable(const struct pros_thread *thread) {
    if (!thread) return 0;
    if (thread->owner_kind == PROS_THREAD_OWNER_KERNEL)
        return thread->process == 0;
    if (thread->owner_kind != PROS_THREAD_OWNER_PROCESS || !thread->process)
        return 0;

    const struct pros_process *process = thread->process;
    uint32_t state = process_state(process);

    /* Only live, scheduler-eligible processes may own runnable threads.
     * In particular, BLOCKED and quarantined processes must never execute. */
    if (process->quarantined ||
        (state != PROS_PROCESS_READY && state != PROS_PROCESS_RUNNING))
        return 0;
    return 1;
}

/* Preserve a kernel worker's BLOCKED state so a later wake can requeue it. */
static uint32_t thread_ineligible_state(const struct pros_thread *thread) {
    if (thread && thread->owner_kind == PROS_THREAD_OWNER_KERNEL &&
        !thread->process)
        return PROS_THREAD_BLOCKED;
    if (!thread || thread->owner_kind != PROS_THREAD_OWNER_PROCESS ||
        !thread->process)
        return PROS_THREAD_DEAD;
    uint32_t state = process_state(thread->process);
    return (thread->process->quarantined ||
            state == PROS_PROCESS_BLOCKED)
        ? PROS_THREAD_BLOCKED : PROS_THREAD_DEAD;
}


/* Only the still-running interrupted thread may resume after a failed switch. */
static int thread_can_resume_interrupted(const struct pros_thread *thread) {
    return thread && thread->state == PROS_THREAD_RUNNING &&
           thread_process_runnable(thread);
}

/*
 * Keep the interrupt scheduler's no-successor and failed-switch park
 * decisions independently testable. These helpers select policy only; the
 * production caller still owns the non-returning interruptible idle loop.
 */
static int thread_interrupt_should_park_without_successor(
    const struct pros_thread *thread) {
    return thread && (thread->state != PROS_THREAD_RUNNING ||
                      !thread_process_runnable(thread));
}

static int thread_interrupt_should_park_after_switch_failure(
    const struct pros_thread *thread) {
    return thread && !thread_can_resume_interrupted(thread);
}

void thread_bootstrap(struct pros_thread *thread) {
    if (!thread || !thread->entry) for (;;) __asm__ volatile ("cli; hlt");
    ((void (*)(void))(uintptr_t)thread->entry)();
    thread->state = PROS_THREAD_DEAD;
    current_thread = 0;
    (void)thread_activate_bootstrap_stack();
    for (;;) __asm__ volatile ("cli; hlt");
}
extern void thread_start_trampoline(void);

int thread_manager_init(void) {
    if (runq_head || runq_tail || current_thread || thread_registry)
        return -1;
    for (uint32_t i = 0; i < (PROS_THREAD_STACK_SLOT_COUNT + 63u) / 64u; ++i)
        if (thread_stack_slots[i] || thread_stack_quarantined_slots[i])
            return -1;
    return 0;
}

static uint64_t thread_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void thread_irq_restore(uint64_t flags) {
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

/*
 * Test/diagnostic snapshot of the fixed 64-slot allocator bitmaps. This
 * intentionally does not call thread_registry_validate(): corruption-injection
 * tests need to snapshot allocator ownership while the registry is malformed.
 */
int thread_stack_allocator_snapshot(uint64_t *allocated_slots,
                                    uint64_t *quarantined_slots) {
    if (!allocated_slots || !quarantined_slots)
        return -1;
    uint64_t irq_flags = thread_irq_save();
    *allocated_slots = thread_stack_slots[0];
    *quarantined_slots = thread_stack_quarantined_slots[0];
    thread_irq_restore(irq_flags);
    return 0;
}

/*
 * Caller holds the single-CPU lifecycle exclusion from validation through
 * publication. Optional user_frame is copied into owned storage before the
 * registry link becomes visible.
 */
static int thread_initialize(struct pros_thread *thread,
                              struct pros_process *process,
                              uint32_t owner_kind,
                              uint64_t tid, uint64_t entry,
                              const struct pros_interrupt_frame *user_frame) {
    if (!thread || tid == 0 || entry == 0 ||
        (owner_kind != PROS_THREAD_OWNER_PROCESS &&
         owner_kind != PROS_THREAD_OWNER_KERNEL) ||
        (owner_kind == PROS_THREAD_OWNER_PROCESS && !process) ||
        (owner_kind == PROS_THREAD_OWNER_KERNEL && process))
        return -1;
    /*
     * Spawn constructs a thread while its owning process is still NEW and
     * intentionally absent from the published process registry. The public
     * process_state() query maps every unregistered object to DEAD, so it is
     * not a valid lifecycle check here. Inspect the initialized object state
     * directly and reject only terminal owners; publication is handled later
     * by process_register() after the complete thread has been constructed.
     */
    if (process && (process->state == PROS_PROCESS_DEAD ||
                    process->state == PROS_PROCESS_ZOMBIE))
        return -1;
    if (thread_registry_validate() != 0)
        return -1;
    for (struct pros_thread *existing = thread_registry; existing; existing = existing->registry_next) {
        /* Registry nodes are embedded in caller-owned thread objects. Reusing
         * an already-registered object would overwrite registry_next and can
         * detach the remainder of the registry (or create a self-cycle). */
        if (existing == thread)
            return -1;
        if (existing->tid == tid && existing->state != PROS_THREAD_DEAD)
            return -1;
    }

    if (thread_kernel_stack_allocate(thread) != 0) return -1;
    uint64_t stack_top = (uint64_t)(uintptr_t)thread->kernel_stack_base +
                         thread->kernel_stack_size;

    thread->tid = tid;
    thread->process = process;
    thread->owner_kind = owner_kind;
    thread->state = PROS_THREAD_NEW;
    thread->priority = 0;
    thread->entry = entry;
    thread->interrupt_frame = 0;
    thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    for (size_t i = 0; i < sizeof(thread->user_interrupt_frame_storage) / sizeof(uint64_t); ++i)
        ((uint64_t *)&thread->user_interrupt_frame_storage)[i] = 0;
    thread->wait_channel = 0;
    thread->blocked_syscall = 0;
    thread->context.r15 = 0;
    thread->context.r14 = 0;
    thread->context.r13 = 0;
    thread->context.r12 = 0;
    thread->context.rbx = 0;
    thread->context.rbp = 0;
    uint64_t *stack = (uint64_t *)(uintptr_t)stack_top;
    /*
     * thread_context_switch restores in this order:
     *   r15, r14, r13, r12, rbp, rbx, ret
     * Build the initial stack in reverse so the first POP sees r15.
     */
    *(--stack) = (uint64_t)(uintptr_t)thread_start_trampoline; /* ret */
    *(--stack) = 0; /* rbx */
    *(--stack) = 0; /* rbp */
    *(--stack) = (uint64_t)(uintptr_t)thread; /* r12 */
    *(--stack) = 0; /* r13 */
    *(--stack) = 0; /* r14 */
    *(--stack) = 0; /* r15 */
    thread->context.rsp = (uint64_t)(uintptr_t)stack;
    thread->context.rip = entry;
    thread->context.rflags = 0x202;
    if (user_frame) {
        thread->user_interrupt_frame_storage = *user_frame;
        thread->interrupt_frame = &thread->user_interrupt_frame_storage;
        thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_USER;
    }
    thread->next = 0;
    thread->prev = 0;

    /* Registry insertion is the final publication write. */
    thread_registry_slots[thread->kernel_stack_slot] = thread;
    thread->registry_next = thread_registry;
    thread_registry = thread;
    return 0;
}

int thread_init(struct pros_thread *thread, struct pros_process *process,
                uint64_t tid, uint64_t entry) {
    uint64_t irq_flags = thread_irq_save();
    int rc = thread_initialize(thread, process, PROS_THREAD_OWNER_PROCESS,
                               tid, entry, 0);
    thread_irq_restore(irq_flags);
    return rc;
}

int thread_init_kernel(struct pros_thread *thread, uint64_t tid,
                       uint64_t entry) {
    uint64_t irq_flags = thread_irq_save();
    int rc = thread_initialize(thread, 0, PROS_THREAD_OWNER_KERNEL,
                               tid, entry, 0);
    thread_irq_restore(irq_flags);
    return rc;
}

int thread_init_user(struct pros_thread *thread, struct pros_process *process,
                     uint64_t tid, uint64_t entry, uint64_t user_rsp) {
    uint64_t irq_flags = thread_irq_save();
    uint64_t physical = 0, flags = 0;
    struct pros_interrupt_frame frame;

    if (!thread || !process || !entry || user_rsp == 0 ||
        user_rsp <= PROSENJIT_USER_STACK_VA ||
        user_rsp > PROSENJIT_USER_STACK_TOP || (user_rsp & 0xFULL) != 0 ||
        address_space_translate_user(&process->address_space, entry,
                                     &physical, &flags) != 0 ||
        !(flags & PROSENJIT_PAGE_USER) ||
        (flags & PROSENJIT_PAGE_NX) != 0 ||
        user_rsp <= PROSENJIT_USER_CODE_VA ||
        address_space_validate_user_range(&process->address_space,
                                          user_rsp - 1u, 1u, 1) != 0) {
        thread_irq_restore(irq_flags);
        return -1;
    }

    /* Zero every saved GPR and reserved field before setting the IRET tail. */
    for (size_t i = 0; i < sizeof(frame) / sizeof(uint64_t); ++i)
        ((uint64_t *)&frame)[i] = 0u;
    /* Construct and validate the complete CPL3 IRET frame before allocation. */
    frame.rip = entry;
    frame.cs = 0x1Bu;
    /* Keep IF clear until the complete CPL3 timer-preemption gate is passed. */
    frame.rflags = 0x2u;
    frame.user_rsp = user_rsp;
    frame.user_ss = 0x23u;

    int rc = thread_initialize(thread, process, PROS_THREAD_OWNER_PROCESS,
                               tid, entry, &frame);
    thread_irq_restore(irq_flags);
    return rc;
}

/*
 * Validate the complete run queue and target stack before a spawn rollback
 * removes any queue/registry link or releases a page. The 64-slot stack arena
 * is also the hard upper bound for registered and queued threads.
 */
int thread_spawn_rollback_preflight(struct pros_thread *thread, int queued) {
    uint64_t irq_flags = thread_irq_save();
    int valid = 0;
    uint32_t expected_state = queued ? PROS_THREAD_READY : PROS_THREAD_NEW;

    if (!thread || (queued != 0 && queued != 1) ||
        thread_registry_validate() != 0 || thread_runq_validate() != 0 ||
        !thread_registry_contains(thread) ||
        thread->state != expected_state || thread == current_thread ||
        thread->owner_kind != PROS_THREAD_OWNER_PROCESS || !thread->process ||
        thread->wait_channel || thread->blocked_syscall ||
        !thread_kernel_stack_valid(thread))
        goto out;

    uint32_t registry_target_count = 0u;
    for (struct pros_thread *node = thread_registry; node;
         node = node->registry_next)
        if (node == thread)
            ++registry_target_count;
    if (registry_target_count != 1u)
        goto out;

    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    uint64_t base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
    if (base > UINT64_MAX - thread->kernel_stack_size)
        goto out;
    uint64_t top = base + thread->kernel_stack_size;
    uint64_t active_tss_stack = cpu_get_kernel_stack();
    if ((active_tss_stack >= base && active_tss_stack <= top) ||
        (rsp >= base && rsp < top))
        goto out;

    /* Prove this exact stack can be unmapped and freed before rollback mutates. */
    uint64_t stack_virtual_pages[PROS_THREAD_STACK_PAGE_COUNT];
    for (uint32_t page = 0u; page < PROS_THREAD_STACK_PAGE_COUNT; ++page)
        stack_virtual_pages[page] =
            base + (uint64_t)page * PROSENJIT_PAGE_SIZE;
    if (paging_unmap_pages_expected_and_free_preflight(
            stack_virtual_pages, thread->kernel_stack_pages,
            PROS_THREAD_STACK_PAGE_COUNT, thread->kernel_stack_pages,
            PROS_THREAD_STACK_PAGE_COUNT) != 0)
        goto out;

    if ((!runq_head) != (!runq_tail))
        goto out;
    uint32_t queue_count = 0u;
    uint32_t target_count = 0u;
    struct pros_thread *previous = 0;
    for (struct pros_thread *it = runq_head; it; it = it->next) {
        if (queue_count >= PROS_THREAD_STACK_SLOT_COUNT ||
            it->prev != previous)
            goto out;
        int registered = 0;
        for (struct pros_thread *node = thread_registry; node;
             node = node->registry_next) {
            if (node == it) {
                registered = 1;
                break;
            }
        }
        if (!registered)
            goto out;
        if (it == thread)
            ++target_count;
        previous = it;
        ++queue_count;
    }
    if (previous != runq_tail || target_count != (queued ? 1u : 0u))
        goto out;
    if (!queued && (thread->next || thread->prev || runq_head == thread))
        goto out;

    valid = 1;
out:
    thread_irq_restore(irq_flags);
    return valid ? 0 : -1;
}

int thread_abort_unpublished(struct pros_thread *thread) {
    /*
     * Keep rollback preflight and its first destructive operation in one
     * single-CPU lifecycle transaction. Callers may already hold IRQ
     * exclusion; save/restore preserves that outer state.
     */
    uint64_t irq_flags = thread_irq_save();
    if (thread_spawn_rollback_preflight(thread, 0) != 0) {
        thread_irq_restore(irq_flags);
        return -1;
    }

    struct pros_thread **link = &thread_registry;
    while (*link && *link != thread) link = &(*link)->registry_next;
    if (!*link || thread_kernel_stack_release(thread) != 0) {
        thread_irq_restore(irq_flags);
        return -1;
    }
    *link = thread->registry_next;
    thread->registry_next = 0;
    thread->process = 0;
    thread->interrupt_frame = 0;
    thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    thread->wait_channel = 0;
    thread->blocked_syscall = 0;
    thread->state = PROS_THREAD_DEAD;
    thread_irq_restore(irq_flags);
    return 0;
}


int thread_process_is_current(const struct pros_process *process) {
    if (!process) return 0;
    uint64_t irq_flags = thread_irq_save();
    /* Authenticate the current pointer before dereferencing it. */
    int result = thread_registry_validate() == 0 && current_thread &&
                 current_thread->process == process;
    thread_irq_restore(irq_flags);
    return result;
}

int thread_process_has_live(const struct pros_process *process) {
    if (!process) return 0;
    uint64_t irq_flags = thread_irq_save();
    /* Validate trusted identities and acyclicity before following any link. */
    if (thread_registry_validate() != 0) {
        thread_irq_restore(irq_flags);
        return 1;
    }

    uint32_t visited = 0u;
    for (struct pros_thread *thread = thread_registry; thread;
         thread = thread->registry_next) {
        if (visited++ >= PROS_THREAD_STACK_SLOT_COUNT) {
            thread_irq_restore(irq_flags);
            return 1;
        }
        if (thread->process == process && thread->state != PROS_THREAD_DEAD) {
            thread_irq_restore(irq_flags);
            return 1;
        }
    }
    thread_irq_restore(irq_flags);
    return 0;
}

/*
 * Exercise the actual direct process_unregister() entry point while the
 * global thread registry contains a cycle. The production guard must reject
 * before unlinking the process or releasing its object reference. The cycle
 * is restored before IRQs are re-enabled.
 */
int thread_process_unregister_registry_cycle_selftest(
    struct pros_process *process) {
    if (!process || thread_registry_validate() != 0)
        return -1;

    uint64_t irq_flags = thread_irq_save();
    if (thread_registry_validate() != 0) {
        thread_irq_restore(irq_flags);
        return -2;
    }

    struct pros_thread *first = thread_registry;
    struct pros_thread *last = first;
    uint32_t visited = 0u;
    while (last && last->registry_next) {
        if (visited++ >= PROS_THREAD_STACK_SLOT_COUNT) {
            thread_irq_restore(irq_flags);
            return -3;
        }
        last = last->registry_next;
    }
    if (!first || !last || visited >= PROS_THREAD_STACK_SLOT_COUNT) {
        thread_irq_restore(irq_flags);
        return -4;
    }

    struct pros_thread *saved_next = last->registry_next;
    last->registry_next = first;
    int unregister_rc = process_unregister(process);
    last->registry_next = saved_next;

    int registry_valid = thread_registry_validate() == 0;
    int process_preserved =
        process->state == PROS_PROCESS_DEAD &&
        process->address_space.root == 0 &&
        process->object.refcount == 1u;
    thread_irq_restore(irq_flags);

    return unregister_rc != 0 && registry_valid && process_preserved ? 0 : -5;
}

/*
 * Boot regression: drive the real process_wait() path with deliberately
 * invalid run-queue endpoints. The queue is restored before IRQs are
 * re-enabled; process_wait must reject in its read-only preflight.
 */
int thread_runq_corruption_wait_selftest(struct pros_process *parent,
                                         struct pros_process *child) {
    /*
     * Keep the snapshot in static test storage: the complete handle table plus
     * all registered thread ownership records is intentionally larger than a
     * small kernel bootstrap stack. This fixture runs only during boot tests.
     */
    static struct pros_handle_entry saved_handles[PROSENJIT_HANDLE_MAX];
    static struct {
        struct pros_thread *thread;
        struct pros_thread *next;
        struct pros_thread *prev;
        struct pros_thread *registry_next;
        void *wait_channel;
        void *stack_base;
        uint64_t stack_size;
        uint64_t stack_pages[PROS_THREAD_STACK_PAGE_COUNT];
        uint32_t stack_slot;
        uint32_t state;
        uint8_t page_allocated[PROS_THREAD_STACK_PAGE_COUNT];
    } saved_threads[PROS_THREAD_STACK_SLOT_COUNT];

    if (!parent || !child || !runq_head || !runq_tail ||
        thread_registry_validate() != 0 || thread_runq_validate() != 0)
        return -1;

    uint64_t irq_flags = thread_irq_save();
    struct pros_thread *saved_head = runq_head;
    struct pros_thread *saved_tail = runq_tail;
    struct pros_thread *saved_registry = thread_registry;
    struct pros_thread *saved_current = current_thread;
    uint64_t saved_rsp0 = cpu_get_kernel_stack();
    uint64_t saved_free_pages = physical_memory_free_pages();
    uint64_t saved_runnable = thread_runnable_count();
    uint64_t saved_blocked = thread_blocked_count();
    uint64_t saved_dead = thread_dead_count();
    uint64_t saved_slots = 0u;
    uint64_t saved_quarantine = 0u;
    uint32_t thread_count = 0u;
    int snapshot_ok =
        thread_stack_allocator_snapshot(&saved_slots, &saved_quarantine) == 0;

    for (struct pros_thread *it = thread_registry;
         snapshot_ok && it; it = it->registry_next) {
        if (thread_count >= PROS_THREAD_STACK_SLOT_COUNT) {
            snapshot_ok = 0;
            break;
        }
        uint32_t n = thread_count++;
        saved_threads[n].thread = it;
        saved_threads[n].next = it->next;
        saved_threads[n].prev = it->prev;
        saved_threads[n].registry_next = it->registry_next;
        saved_threads[n].wait_channel = it->wait_channel;
        saved_threads[n].stack_base = it->kernel_stack_base;
        saved_threads[n].stack_size = it->kernel_stack_size;
        saved_threads[n].stack_slot = it->kernel_stack_slot;
        saved_threads[n].state = it->state;
        for (uint32_t p = 0u; p < PROS_THREAD_STACK_PAGE_COUNT; ++p) {
            saved_threads[n].stack_pages[p] = it->kernel_stack_pages[p];
            saved_threads[n].page_allocated[p] =
                physical_page_is_allocated(it->kernel_stack_pages[p]) ? 1u : 0u;
        }
    }
    for (uint32_t i = 0u; i < PROSENJIT_HANDLE_MAX; ++i)
        saved_handles[i] = child->handles.entries[i];

    uint64_t saved_root = (uint64_t)(uintptr_t)child->address_space.root;
    struct pros_owned_page *saved_owned_pages = child->address_space.owned_pages;
    struct pros_process *saved_child_next = child->next;
    struct pros_process *saved_child_parent = child->parent;
    uint32_t saved_child_state = child->state;
    uint32_t saved_object_refs = child->object.refcount;
    int32_t saved_child_exit = child->exit_code;
    int32_t sentinel = 0x61616161;
    int result = snapshot_ok ? 0 : -1;

    /*
     * Test each endpoint independently. The invalid value must be rejected
     * before process resources, thread ownership, queue links or caller output
     * can be changed.
     */
    for (uint32_t which = 0u; result == 0 && which < 4u; ++which) {
        int32_t exit_code = sentinel;
        if (which == 0u)
            runq_head = (struct pros_thread *)(uintptr_t)1u;
        else if (which == 1u)
            runq_tail = (struct pros_thread *)(uintptr_t)1u;
        else if (which == 2u)
            runq_tail->next = saved_head;
        else
            runq_head->prev = saved_tail;

        int wait_rc = process_wait(parent, child->pid, &exit_code);
        int scheduler_rc = thread_schedule();
        int runnable_rc = thread_has_runnable();
        int enqueue_rc = thread_enqueue(saved_head);
        struct pros_thread *dequeued = thread_dequeue();
        int remove_rc = thread_remove_queued(saved_head);
        /* Restore only the deliberately corrupted link before inspecting state. */
        if (which == 2u) saved_tail->next = 0;
        if (which == 3u) saved_head->prev = 0;
        runq_head = saved_head;
        runq_tail = saved_tail;
        if (wait_rc != -3 || exit_code != sentinel ||
            scheduler_rc != -1 || runnable_rc != 0 || enqueue_rc != -1 ||
            dequeued != 0 || remove_rc != -1 ||
            thread_registry != saved_registry || current_thread != saved_current ||
            cpu_get_kernel_stack() != saved_rsp0 ||
            physical_memory_free_pages() != saved_free_pages ||
            thread_runnable_count() != saved_runnable ||
            thread_blocked_count() != saved_blocked ||
            thread_dead_count() != saved_dead ||
            child->state != saved_child_state ||
            child->exit_code != saved_child_exit ||
            child->object.refcount != saved_object_refs ||
            child->address_space.root != (uint64_t *)(uintptr_t)saved_root ||
            child->address_space.owned_pages != saved_owned_pages ||
            child->next != saved_child_next || child->parent != saved_child_parent ||
            process_lookup(child->pid) != child) {
            result = -1;
            break;
        }

        uint64_t slots_now = 0u, quarantine_now = 0u;
        if (thread_stack_allocator_snapshot(&slots_now, &quarantine_now) != 0 ||
            slots_now != saved_slots || quarantine_now != saved_quarantine) {
            result = -1;
            break;
        }

        for (uint32_t i = 0u; i < PROSENJIT_HANDLE_MAX; ++i) {
            const struct pros_handle_entry *a = &child->handles.entries[i];
            const struct pros_handle_entry *b = &saved_handles[i];
            if (a->object != b->object || a->generation != b->generation ||
                a->reserved != b->reserved || a->rights != b->rights) {
                result = -1;
                break;
            }
        }
        for (uint32_t n = 0u; result == 0 && n < thread_count; ++n) {
            struct pros_thread *it = saved_threads[n].thread;
            if (it->next != saved_threads[n].next ||
                it->prev != saved_threads[n].prev ||
                it->registry_next != saved_threads[n].registry_next ||
                it->wait_channel != saved_threads[n].wait_channel ||
                it->kernel_stack_base != saved_threads[n].stack_base ||
                it->kernel_stack_size != saved_threads[n].stack_size ||
                it->kernel_stack_slot != saved_threads[n].stack_slot ||
                it->state != saved_threads[n].state) {
                result = -1;
                break;
            }
            for (uint32_t p = 0u; p < PROS_THREAD_STACK_PAGE_COUNT; ++p) {
                uint64_t physical = it->kernel_stack_pages[p];
                if (physical != saved_threads[n].stack_pages[p] ||
                    (physical_page_is_allocated(physical) ? 1u : 0u) !=
                        saved_threads[n].page_allocated[p]) {
                    result = -1;
                    break;
                }
            }
        }
    }

    /* A forward-link cycle must be rejected without entering an unbounded probe. */
    if (result == 0) {
        saved_tail->next = saved_head;
        if (thread_has_runnable() != 0)
            result = -1;
        saved_tail->next = 0;
        if (thread_runq_validate() != 0)
            result = -1;
    }

    runq_head = saved_head;
    runq_tail = saved_tail;
    thread_irq_restore(irq_flags);
    return result;
}

static int thread_process_reapable_with_context(
    const struct pros_process *process, uint64_t rsp,
    uint64_t active_tss_stack) {
    /*
     * This is the aggregate read-only gate used before process_wait() releases
     * handles/address-space pages. Keep registry, queue, stack mappings, CPU
     * stack pointers, page-table hierarchy, and PMM ownership stable together.
     */
    uint64_t irq_flags = thread_irq_save();
    uint32_t thread_count = 0u;
    uint64_t page_count = 0u;
    int valid = 0;

    if (thread_registry_validate() != 0 || thread_runq_validate() != 0 ||
        !process || thread_process_is_current(process) ||
        thread_process_has_live(process))
        goto out;

    for (struct pros_thread *thread = thread_registry; thread;
         thread = thread->registry_next) {
        if (thread->process != process) continue;
        if (thread_count >= PROS_THREAD_STACK_SLOT_COUNT ||
            thread->state != PROS_THREAD_DEAD || thread->wait_channel ||
            thread_is_queued(thread) || !thread_kernel_stack_valid(thread))
            goto out;

        uint64_t stack_base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
        if (thread->kernel_stack_size != PROS_THREAD_KERNEL_STACK_SIZE ||
            stack_base > UINT64_MAX - thread->kernel_stack_size ||
            page_count > 256u - PROS_THREAD_STACK_PAGE_COUNT)
            goto out;
        uint64_t stack_top = stack_base + thread->kernel_stack_size;

        /* Neither the live C stack nor TSS.RSP0 may enter memory being freed. */
        if ((active_tss_stack >= stack_base && active_tss_stack <= stack_top) ||
            (rsp >= stack_base && rsp < stack_top))
            goto out;

        process_reap_threads[thread_count++] = thread;
        for (uint32_t page = 0u; page < PROS_THREAD_STACK_PAGE_COUNT; ++page) {
            process_reap_virtual_pages[page_count] =
                stack_base + (uint64_t)page * PROSENJIT_PAGE_SIZE;
            process_reap_physical_pages[page_count] =
                thread->kernel_stack_pages[page];
            ++page_count;
        }
    }

    /*
     * thread_reap_process() uses this exact checked batch as its commit. Prove
     * every expected VA->PA leaf, unique kernel page-table hierarchy, and the
     * complete PMM free set before process_wait() destroys any other resource.
     */
    if (page_count != 0u &&
        paging_unmap_pages_expected_and_free_preflight(
            process_reap_virtual_pages, process_reap_physical_pages,
            page_count, process_reap_physical_pages, page_count) != 0)
        goto out;

    valid = 1;
out:
    thread_irq_restore(irq_flags);
    return valid;
}

int thread_process_reapable(const struct pros_process *process) {
    /* Capture both CPU stack pointers inside the same lifecycle exclusion as
     * the registry/queue/mapping validation; otherwise a timer switch between
     * the RSP read and RSP0 read could validate a mixed-context snapshot. */
    uint64_t irq_flags = thread_irq_save();
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    uint64_t active_tss_stack = cpu_get_kernel_stack();
    int result = thread_process_reapable_with_context(
        process, rsp, active_tss_stack);
    thread_irq_restore(irq_flags);
    return result;
}

/*
 * Deterministic boot regression for both CPU stack pointers. Inject synthetic
 * values into the same read-only validator used by production reap; no real
 * stack switch is attempted and no ownership state is mutated.
 */
int thread_process_reap_context_selftest(const struct pros_process *process) {
    if (!process || !thread_process_reapable_with_context(process, 0u, 0u))
        return -1;

    uint32_t checked = 0u;
    for (struct pros_thread *thread = thread_registry; thread;
         thread = thread->registry_next) {
        if (thread->process != process) continue;
        uint64_t base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
        if (!base || thread->kernel_stack_size != PROS_THREAD_KERNEL_STACK_SIZE ||
            base > UINT64_MAX - thread->kernel_stack_size)
            return -2;
        uint64_t top = base + thread->kernel_stack_size;

        if (thread_process_reapable_with_context(process, base, 0u) ||
            thread_process_reapable_with_context(process, 0u, base + 1u) ||
            thread_process_reapable_with_context(process, 0u, top))
            return -3;
        ++checked;
    }
    if (checked == 0u ||
        !thread_process_reapable_with_context(process, 0u, 0u))
        return -4;

    /*
     * Prove the aggregate reapability gate includes the exact paging batch
     * preflight. A temporary duplicate PML4 subtree leaves each target stack's
     * own mapping valid but makes the kernel hierarchy non-unique. The gate
     * must reject without freeing a page; restore the root before IRQ restore.
     */
    uint64_t irq_flags = thread_irq_save();
    uint64_t *root = paging_kernel_root();
    uint32_t source_index = 512u;
    uint32_t empty_index = 512u;
    if (!root) {
        thread_irq_restore(irq_flags);
        return -5;
    }
    for (uint32_t index = 256u; index < 512u; ++index) {
        if (source_index == 512u &&
            (root[index] & PROSENJIT_PAGE_PRESENT))
            source_index = index;
        if (empty_index == 512u &&
            !(root[index] & PROSENJIT_PAGE_PRESENT))
            empty_index = index;
    }
    if (source_index == 512u || empty_index == 512u) {
        thread_irq_restore(irq_flags);
        return -6;
    }

    uint64_t saved_entry = root[empty_index];
    uint64_t saved_free_pages = physical_memory_free_pages();
    root[empty_index] = root[source_index];
    int rejected_alias = !thread_process_reapable_with_context(
        process, 0u, 0u);
    root[empty_index] = saved_entry;
    int restored_valid = thread_process_reapable_with_context(
        process, 0u, 0u);
    int registry_valid = thread_registry_validate() == 0;
    uint64_t restored_free_pages = physical_memory_free_pages();
    thread_irq_restore(irq_flags);

    return rejected_alias && restored_valid && registry_valid &&
           restored_free_pages == saved_free_pages ? 0 : -7;
}

static int thread_terminate_process_preflight_locked(
    struct pros_process *process) {
    if (!process || thread_registry_validate() != 0 ||
        thread_runq_validate() != 0)
        return -1;

    /*
     * A blocked thread's channel waiter link is an external reference to its
     * thread descriptor. Prove every such link can be removed before touching
     * the run queue or changing any thread state.
     */
    for (struct pros_thread *thread = thread_registry; thread;
         thread = thread->registry_next) {
        if (thread->process != process)
            continue;
        if (thread->state > PROS_THREAD_DEAD ||
            (thread->state == PROS_THREAD_DEAD &&
             (thread->wait_channel || thread_is_queued(thread))))
            return -1;
        if (thread->state == PROS_THREAD_DEAD || !thread->wait_channel)
            continue;
        if (thread->state != PROS_THREAD_BLOCKED ||
            ipc_channel_cancel_waiter_preflight(
                (struct pros_ipc_channel *)thread->wait_channel, thread) !=
                PROSENJIT_IPC_OK)
            return -1;
    }
    return 0;
}

int thread_terminate_process_preflight(struct pros_process *process) {
    uint64_t irq_flags = thread_irq_save();
    int rc = thread_terminate_process_preflight_locked(process);
    thread_irq_restore(irq_flags);
    return rc;
}

int thread_terminate_process(struct pros_process *process) {
    uint64_t irq_flags = thread_irq_save();
    if (thread_terminate_process_preflight_locked(process) != 0) {
        thread_irq_restore(irq_flags);
        return -1;
    }

    int terminated_current = 0;

    /* Remove process-owned threads from the run queue before clearing their
     * next/prev links. These fields are the run-queue links, so clearing them
     * first can truncate traversal of unrelated runnable threads. */
    struct pros_thread *prev = 0;
    struct pros_thread *it = runq_head;
    while (it) {
        struct pros_thread *next = it->next;
        if (it->process == process) {
            if (prev) prev->next = next; else runq_head = next;
            if (next) next->prev = prev; else runq_tail = prev;
            it->next = 0;
            it->prev = 0;
        } else {
            prev = it;
        }
        it = next;
    }

    for (struct pros_thread *thread = thread_registry; thread;
         thread = thread->registry_next) {
        if (thread->process != process || thread->state == PROS_THREAD_DEAD)
            continue;
        if (thread == current_thread)
            terminated_current = 1;
        if (thread->wait_channel) {
            /*
             * Exact membership was preflighted for every process thread while
             * interrupts remained disabled. A failure here means the
             * single-CPU transaction invariant was violated; do not clear the
             * only channel reference or publish a reapable state.
             */
            if (ipc_channel_cancel_waiter(
                    (struct pros_ipc_channel *)thread->wait_channel,
                    thread) != PROSENJIT_IPC_OK) {
                thread_irq_restore(irq_flags);
                return -1;
            }
            thread->wait_channel = 0;
        }
        thread->blocked_syscall = 0;
        thread->interrupt_frame = 0;
        thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
        thread->next = 0;
        thread->prev = 0;
        thread->state = PROS_THREAD_DEAD;
    }

    /* No dead process thread may remain visible as the current thread. */
    if (terminated_current)
        current_thread = 0;
    /* A NULL current-thread state must never leave RSP0 pointing at a
     * process-owned stack, even if an earlier scheduler path detached it. */
    if (!current_thread)
        (void)thread_activate_bootstrap_stack();

    thread_irq_restore(irq_flags);
    return 0;
}

int thread_reap_process(struct pros_process *process) {
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    if (!thread_process_reapable(process)) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }

    /*
     * Build one complete teardown plan before mutation. The fixed stack arena
     * bounds this to 64 threads / 256 physical pages. A single paging+PMM
     * transaction prevents an early thread stack being freed if a later
     * thread has a bad mapping or allocator ownership record.
     */
    uint32_t thread_count = 0u;
    uint64_t page_count = 0u;
    for (struct pros_thread *it = thread_registry; it; it = it->registry_next) {
        if (it->process != process) continue;
        if (thread_count >= PROS_THREAD_STACK_SLOT_COUNT ||
            it->state != PROS_THREAD_DEAD ||
            !thread_kernel_stack_valid(it)) {
            __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
            return -1;
        }
        process_reap_threads[thread_count++] = it;
        uint64_t base = (uint64_t)(uintptr_t)it->kernel_stack_base;
        for (uint32_t p = 0u; p < PROS_THREAD_STACK_PAGE_COUNT; ++p) {
            if (page_count >= 256u) {
                __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
                return -1;
            }
            process_reap_virtual_pages[page_count] =
                base + (uint64_t)p * PROSENJIT_PAGE_SIZE;
            process_reap_physical_pages[page_count] = it->kernel_stack_pages[p];
            ++page_count;
        }
    }

    if (thread_count == 0u) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return 0;
    }

    if (paging_unmap_pages_expected_and_free(
            process_reap_virtual_pages, process_reap_physical_pages,
            page_count, process_reap_physical_pages, page_count) != 0) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }

    /*
     * The complete mapping/free transaction has committed. The remaining
     * operations are bounded metadata writes and registry unlink operations;
     * no further fallible resource release is attempted.
     */
    for (uint32_t i = 0u; i < thread_count; ++i) {
        struct pros_thread *thread = process_reap_threads[i];
        uint32_t slot = thread->kernel_stack_slot;
        for (uint32_t p = 0u; p < PROS_THREAD_STACK_PAGE_COUNT; ++p)
            thread->kernel_stack_pages[p] = 0u;
        thread_stack_slots[slot >> 6] &= ~(1ULL << (slot & 63u));
        thread_stack_quarantined_slots[slot >> 6] &= ~(1ULL << (slot & 63u));
        if (thread_registry_slots[slot] == thread)
            thread_registry_slots[slot] = 0;
        thread->kernel_stack_base = 0;
        thread->kernel_stack_size = 0;
        thread->kernel_stack_slot = PROS_THREAD_STACK_SLOT_COUNT;
    }

    struct pros_thread **link = &thread_registry;
    while (*link) {
        struct pros_thread *thread = *link;
        if (thread->process != process) {
            link = &thread->registry_next;
            continue;
        }
        *link = thread->registry_next;
        thread->registry_next = 0;
        thread->process = 0;
        thread->interrupt_frame = 0;
        thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
        thread->wait_channel = 0;
        thread->blocked_syscall = 0;
    }

    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return 0;
}
int thread_reap_kernel(struct pros_thread *thread) {
    /*
     * The registry, run queue, TSS and stack allocator form one single-CPU
     * lifecycle transaction. Own interrupt exclusion here rather than
     * relying on a caller to have entered with IF clear.
     */
    uint64_t irq_flags = thread_irq_save();
    if (thread_registry_validate() != 0 || !thread ||
        !thread_registry_contains(thread) ||
        thread->owner_kind != PROS_THREAD_OWNER_KERNEL ||
        thread->process || thread == current_thread ||
        thread->state != PROS_THREAD_DEAD || thread->wait_channel ||
        thread_is_queued(thread) || !thread_kernel_stack_valid(thread))
        goto reject;

    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    uint64_t stack_base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
    if (stack_base > UINT64_MAX - thread->kernel_stack_size)
        goto reject;
    uint64_t stack_top = stack_base + thread->kernel_stack_size;
    if (cpu_get_kernel_stack() == stack_top ||
        (rsp >= stack_base && rsp < stack_top))
        goto reject;

    struct pros_thread **link = &thread_registry;
    while (*link && *link != thread) link = &(*link)->registry_next;
    if (!*link || thread_kernel_stack_release(thread) != 0)
        goto reject;

    *link = thread->registry_next;
    thread->registry_next = 0;
    thread->interrupt_frame = 0;
    thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    thread->wait_channel = 0;
    thread->blocked_syscall = 0;
    thread_irq_restore(irq_flags);
    return 0;

reject:
    thread_irq_restore(irq_flags);
    return -1;
}

int thread_set_state(struct pros_thread *thread, uint32_t state) {
    if (!thread || state > PROS_THREAD_DEAD)
        return -1;
    uint64_t irq_flags = thread_irq_save();
    if (thread_registry_validate() != 0 ||
        !thread_registry_contains(thread)) {
        thread_irq_restore(irq_flags);
        return -1;
    }
    if (thread->state == PROS_THREAD_DEAD && state != PROS_THREAD_DEAD) {
        thread_irq_restore(irq_flags);
        return -1;
    }
    /* A queued thread cannot become BLOCKED/DEAD behind the run queue's back.
     * Callers must use the scheduler/wait/termination paths to unlink it first. */
    if (state != thread->state && thread_is_queued(thread)) {
        thread_irq_restore(irq_flags);
        return -1;
    }
    thread->state = state;
    thread_irq_restore(irq_flags);
    return 0;
}

uint32_t thread_state(const struct pros_thread *thread) {
    return thread ? thread->state : PROS_THREAD_DEAD;
}

int thread_enqueue(struct pros_thread *thread) {
    /*
     * Authenticate the candidate by pointer comparison before dereferencing
     * its fields, and reject a damaged queue before mutating its links.
     */
    if (!thread || thread_runq_validate() != 0 ||
        !thread_registry_contains(thread) ||
        thread->state == PROS_THREAD_DEAD ||
        thread->state == PROS_THREAD_BLOCKED ||
        !thread_process_runnable(thread))
        return -1;
    if (thread->next || thread->prev || runq_head == thread)
        return -1;

    thread->state = PROS_THREAD_READY;
    thread->next = 0;
    thread->prev = runq_tail;
    if (runq_tail) runq_tail->next = thread;
    else runq_head = thread;
    runq_tail = thread;
    return 0;
}

struct pros_thread *thread_dequeue(void) {
    if (thread_runq_validate() != 0) return 0;
    struct pros_thread *thread = runq_head;
    if (!thread) return 0;
    runq_head = thread->next;
    if (runq_head) runq_head->prev = 0;
    else runq_tail = 0;
    thread->next = 0;
    thread->prev = 0;
    return thread;
}

struct pros_thread *thread_current(void) {
    return current_thread;
}

int thread_schedule(void) {
    struct pros_thread *next;
    struct pros_thread *prev = current_thread;

    /* Reject corrupt/cyclic queue state before the dequeue/retry loop. */
    if (thread_runq_validate() != 0) return -1;

    do {
        next = thread_dequeue();
        if (!next) return -1;
        if (!thread_process_runnable(next)) {
            /* A blocked kernel worker remains wakeable; invalid owners die. */
            next->state = thread_ineligible_state(next);
        }
    } while (next->state == PROS_THREAD_DEAD ||
             next->state == PROS_THREAD_BLOCKED);

    /*
     * Queue the previous runnable thread before changing address spaces.
     * This keeps the old current thread/address space authoritative if either
     * queueing or the subsequent CR3 switch fails.
     */
    if (prev && prev != next && prev->state == PROS_THREAD_RUNNING) {
        if (thread_enqueue(prev) != 0) {
            (void)thread_enqueue(next);
            return -1;
        }
    }

    if (thread_activate_kernel_stack(next) != 0) {
        /* A malformed/unowned stack must never become the CPU's RSP0. */
        if (prev && prev != next && prev->state == PROS_THREAD_READY &&
            thread_remove_queued(prev) == 0)
            prev->state = PROS_THREAD_RUNNING;
        (void)thread_enqueue(next);
        return -1;
    }

    process_set_current(next->process);
    if (process_current() != next->process) {
        /* The candidate address-space switch failed. Restore the old RSP0
         * before rolling the outgoing thread back to RUNNING. */
        thread_restore_kernel_stack(prev);
        if (prev && prev != next && prev->state == PROS_THREAD_READY &&
            thread_remove_queued(prev) == 0)
            prev->state = PROS_THREAD_RUNNING;
        (void)thread_enqueue(next);
        return -1;
    }

    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;
    return 0;
}

/*
 * A CPL3 IRET frame is copied into storage owned by the thread object.
 * Its user RSP/SS tail is consumed by IRETQ, so it need not remain on the
 * transient syscall-entry stack. CPL0 frames stay on the kernel stack because
 * ring-0 IRET does not switch RSP.
 */
static struct pros_interrupt_frame *thread_capture_interrupt_frame(
    struct pros_thread *thread, struct pros_interrupt_frame *frame,
    enum pros_interrupt_frame_kind frame_kind) {
    if (!thread || !frame) return 0;
    if (frame_kind != PROS_INTERRUPT_FRAME_USER) return frame;

    uint64_t *destination = (uint64_t *)&thread->user_interrupt_frame_storage;
    const uint64_t *source = (const uint64_t *)frame;
    for (size_t i = 0; i < sizeof(thread->user_interrupt_frame_storage) / sizeof(uint64_t); ++i)
        destination[i] = source[i];
    return &thread->user_interrupt_frame_storage;
}

static int thread_interrupt_frame_valid(
    const struct pros_interrupt_frame *frame,
    enum pros_interrupt_frame_kind frame_kind) {
    if (!frame) return 0;
    if (frame_kind == PROS_INTERRUPT_FRAME_KERNEL)
        return frame->cs == 0x08u;
    if (frame_kind == PROS_INTERRUPT_FRAME_USER)
        return frame->cs == 0x1Bu &&
               frame->user_ss == 0x23u &&
               frame->user_rsp != 0u &&
               frame->user_rsp < 0x0000800000000000ULL &&
               (frame->rflags & 0x2u) != 0u &&
               (frame->rflags & ((3ULL << 12) | (1ULL << 14) |
                                 (1ULL << 17))) == 0u;
    return 0;
}

/*
 * Invalid interrupt frames cannot be resumed. If the thread is still linked
 * from an IPC channel, detach it only after the channel proves the waiter
 * array is sound and the target occurs exactly once (or is soundly absent).
 * On any integrity/cancellation failure, keep the external ownership link and
 * quarantine the thread as BLOCKED with no resumable frame.
 */
static int thread_invalid_frame_cleanup(struct pros_thread *thread) {
    if (!thread) return -1;

    if (thread->wait_channel) {
        struct pros_ipc_channel *channel =
            (struct pros_ipc_channel *)thread->wait_channel;
        int preflight = ipc_channel_cancel_waiter_preflight(channel, thread);
        if (preflight == PROSENJIT_IPC_OK) {
            if (ipc_channel_cancel_waiter(channel, thread) !=
                PROSENJIT_IPC_OK)
                goto quarantine;
        } else if (preflight != PROSENJIT_IPC_EEMPTY) {
            goto quarantine;
        }
        thread->wait_channel = 0;
    }

    thread->blocked_syscall = 0;
    thread->interrupt_frame = 0;
    thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    thread->state = PROS_THREAD_DEAD;
    return 0;

quarantine:
    thread->interrupt_frame = 0;
    thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    thread->state = PROS_THREAD_BLOCKED;
    return 1;
}

struct pros_interrupt_frame *thread_schedule_interrupt(
    struct pros_interrupt_frame *current_frame,
    enum pros_interrupt_frame_kind frame_kind) {
    struct pros_thread *prev = current_thread;
    if (!current_frame) return current_frame;
    /* The selector privilege level must agree with the entry stub's tag. */
    if (!thread_interrupt_frame_valid(current_frame, frame_kind)) {
        /*
         * Never IRET through a frame whose kind/selectors disagree. A linked
         * IPC waiter is detached only after exact channel integrity preflight;
         * otherwise its ownership link is retained and the thread is parked
         * BLOCKED without a resumable frame.
         */
        if (prev)
            (void)thread_invalid_frame_cleanup(prev);
        current_thread = 0;
        (void)thread_activate_bootstrap_stack();
        for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
    }

    /*
     * current_thread is NULL only while the scheduler is parked in its
     * interruptible idle loop because no runnable thread existed. In that
     * case this frame belongs to the idle loop, not to a blocked thread.
     */
    if (prev) {
        prev->interrupt_frame = thread_capture_interrupt_frame(
            prev, current_frame, frame_kind);
        if (!prev->interrupt_frame) {
            prev->state = PROS_THREAD_DEAD;
            current_thread = 0;
            (void)thread_activate_bootstrap_stack();
            for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
        }
        prev->interrupt_frame_kind = (uint32_t)frame_kind;

        /*
         * A sender may have queued a message while this thread's old frame
         * was invalidated but before this syscall-entry frame was saved.
         * Such an early wake cannot safely rewind RIP and leaves the waiter
         * registered. Retry only after publishing the fresh USER frame.
         */
        if (prev->state == PROS_THREAD_BLOCKED &&
            prev->blocked_syscall != 0 &&
            prev->wait_channel != 0 &&
            frame_kind == PROS_INTERRUPT_FRAME_USER) {
            (void)ipc_channel_wake_one(
                (struct pros_ipc_channel *)prev->wait_channel);
        }
    }

    /* Do not let a corrupt/cyclic run queue trap the interrupt scheduler. */
    if (thread_runq_validate() != 0) {
        if (thread_interrupt_should_park_without_successor(prev)) {
            if (prev && prev->state == PROS_THREAD_RUNNING)
                prev->state = thread_ineligible_state(prev);
            current_thread = 0;
            (void)thread_activate_bootstrap_stack();
            for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
        }
        return current_frame;
    }

    struct pros_thread *next;
    do {
        next = thread_dequeue();
        if (!next) {
            /*
             * There is not yet a dedicated idle thread. Returning the saved
             * interrupt frame is safe only while the interrupted thread is
             * still eligible to execute. A process may have been blocked or
             * quarantined while its thread was running; resuming that frame
             * would bypass the process policy. Park the CPU until an interrupt
             * makes a runnable thread available instead of returning to it.
             */
            if (thread_interrupt_should_park_without_successor(prev)) {
                if (prev->state == PROS_THREAD_RUNNING) {
                    prev->state = thread_ineligible_state(prev);
                }
                /*
                 * A BLOCKED thread must never fall through to IRET merely
                 * because its process remains runnable. Detach it before
                 * enabling interrupts so an idle IRQ cannot overwrite its
                 * saved syscall frame.
                 */
                current_thread = 0;
                (void)thread_activate_bootstrap_stack();
                for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
            }
            return current_frame;
        }
        if (!thread_process_runnable(next)) {
            /* Match the cooperative scheduler's owner-specific policy. */
            next->state = thread_ineligible_state(next);
        }
        if (next->state != PROS_THREAD_DEAD &&
            next->state != PROS_THREAD_BLOCKED &&
            ((next->interrupt_frame &&
              !thread_interrupt_frame_valid(
                  next->interrupt_frame,
                  (enum pros_interrupt_frame_kind)next->interrupt_frame_kind)) ||
             (!next->interrupt_frame &&
              next->interrupt_frame_kind != PROS_INTERRUPT_FRAME_NONE))) {
            /* Never restore a stale/mistagged frame belonging to another ABI. */
            next->interrupt_frame = 0;
            next->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
            next->blocked_syscall = 0;
            next->state = PROS_THREAD_DEAD;
        }
    } while (next->state == PROS_THREAD_DEAD ||
             next->state == PROS_THREAD_BLOCKED);

    /*
     * Queue the interrupted thread before changing address spaces. If the
     * queue operation fails, the interrupted frame and current address space
     * remain authoritative. If the CR3 switch fails, the previous thread is
     * already safely queued and the candidate is requeued.
     */
    /*
     * BLOCKED predecessors are not queued. A predecessor that became READY
     * through the post-publication IPC wake retry is already on the run queue;
     * only the still-running interrupted predecessor needs enqueueing here.
     */
    if (prev && prev->state == PROS_THREAD_RUNNING) {
        if (!thread_process_runnable(prev)) {
            prev->state = thread_ineligible_state(prev);
        } else if (thread_enqueue(prev) != 0) {
            (void)thread_enqueue(next);
            return current_frame;
        }
    }

    if (thread_activate_kernel_stack(next) != 0) {
        /* Reject a candidate whose supervisor stack cannot safely own RSP0. */
        if (prev && prev->state == PROS_THREAD_READY &&
            thread_remove_queued(prev) == 0)
            prev->state = PROS_THREAD_RUNNING;
        (void)thread_enqueue(next);
        if (thread_interrupt_should_park_after_switch_failure(prev)) {
            prev->state = thread_ineligible_state(prev);
            current_thread = 0;
            (void)thread_activate_bootstrap_stack();
            for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
        }
        thread_restore_kernel_stack(prev);
        return current_frame;
    }

    process_set_current(next->process);
    if (process_current() != next->process) {
        /* If the interrupted thread was requeued, undo that transition before
         * returning its frame; otherwise it would be both executing and READY. */
        if (prev && prev->state == PROS_THREAD_READY &&
            thread_remove_queued(prev) == 0)
            prev->state = PROS_THREAD_RUNNING;
        (void)thread_enqueue(next);

        /* Never iret back into a process that became blocked/quarantined while
         * running, even if switching to its replacement failed. Keep the CPU
         * in an interruptible idle loop; a later IRQ can retry scheduling. */
        if (thread_interrupt_should_park_after_switch_failure(prev)) {
            prev->state = thread_ineligible_state(prev);
            current_thread = 0;
            (void)thread_activate_bootstrap_stack();
            for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
        }
        thread_restore_kernel_stack(prev);
        return current_frame;
    }

    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;

    if (next->interrupt_frame)
        return next->interrupt_frame;

    /* First interrupt-driven run: synthesize a CPL0 iret frame. */
    /*
     * A first-run kernel frame has only the 15-GPR prefix plus the
     * three-qword CPL0 IRET tail. Do not reserve the CPL3-only RSP/SS words:
     * IRETQ will not consume them for a ring-0 return.
     */
    const size_t kernel_frame_size =
        __builtin_offsetof(struct pros_interrupt_frame, user_rsp);
    struct pros_interrupt_frame *frame =
        (struct pros_interrupt_frame *)(uintptr_t)(next->context.rsp -
                                                   kernel_frame_size);
    frame->rax = frame->rbx = frame->rcx = frame->rdx = 0;
    frame->rsi = frame->rdi = frame->rbp = 0;
    frame->r8 = frame->r9 = frame->r10 = frame->r11 = 0;
    frame->r12 = (uint64_t)(uintptr_t)next;
    frame->r13 = frame->r14 = frame->r15 = 0;
    frame->rip = (uint64_t)(uintptr_t)thread_start_trampoline;
    frame->cs = 0x08;
    frame->rflags = 0x202;
    next->interrupt_frame = frame;
    next->interrupt_frame_kind = PROS_INTERRUPT_FRAME_KERNEL;
    return frame;
}

int thread_block(void *wait_channel) {
    if (!current_thread || !wait_channel ||
        current_thread->state != PROS_THREAD_RUNNING)
        return -1;
    current_thread->wait_channel = wait_channel;
    current_thread->state = PROS_THREAD_BLOCKED;
    return 0;
}

int thread_wake(struct pros_thread *thread) {
    /*
     * The wake target can originate from an IPC waiter link. Authenticate
     * registry membership and queue integrity before reading or changing it.
     */
    if (!thread || thread_registry_validate() != 0 ||
        !thread_registry_contains(thread) || thread_runq_validate() != 0 ||
        thread->state != PROS_THREAD_BLOCKED ||
        !thread_process_runnable(thread))
        return -1;

    if (thread->wait_channel != 0 &&
        (thread->interrupt_frame_kind != PROS_INTERRUPT_FRAME_USER ||
         !thread_interrupt_frame_valid(
             thread->interrupt_frame, PROS_INTERRUPT_FRAME_USER) ||
         thread->interrupt_frame->rip < 2u))
        return -1;

    /*
     * Publish the completed syscall continuation before exposing this thread
     * on the run queue. Save the old words so a failed enqueue leaves the
     * blocked receiver's frame and wake state unchanged.
     */
    uint64_t blocked_syscall = thread->blocked_syscall;
    uint64_t original_rax = 0;
    uint64_t original_rip = 0;
    if (blocked_syscall != 0) {
        original_rax = thread->interrupt_frame->rax;
        original_rip = thread->interrupt_frame->rip;
        thread->interrupt_frame->rax = blocked_syscall;
        thread->interrupt_frame->rip = original_rip - 2u;
    }

    thread->state = PROS_THREAD_READY;
    if (thread_enqueue(thread) != 0) {
        thread->state = PROS_THREAD_BLOCKED;
        if (blocked_syscall != 0) {
            thread->interrupt_frame->rax = original_rax;
            thread->interrupt_frame->rip = original_rip;
        }
        return -1;
    }
    if (blocked_syscall != 0)
        thread->blocked_syscall = 0;
    thread->wait_channel = 0;
    return 0;
}

static void thread_kernel_selftest_entry(void) { }

int thread_kernel_selftest(void) {
    static struct pros_thread probe;
    if (probe.registry_next || probe.kernel_stack_base) return -1;
    if (thread_init_kernel(&probe, UINT64_MAX,
                           (uint64_t)(uintptr_t)thread_kernel_selftest_entry) != 0)
        return -2;
    if (probe.process || probe.owner_kind != PROS_THREAD_OWNER_KERNEL ||
        probe.state != PROS_THREAD_NEW ||
        !thread_kernel_stack_valid(&probe))
        return -3;

    /*
     * Stack permission corruption must be rejected at every paging level.
     * The probe is not running, and IRQs remain disabled while entries are
     * modified and restored.
     */
    uint64_t permission_irq = thread_irq_save();
    uint64_t probe_stack_va = (uint64_t)(uintptr_t)probe.kernel_stack_base;
    for (uint32_t level = 0u; level < 4u; ++level) {
        if (paging_test_modify_level_flags(probe_stack_va, level,
                                           PROSENJIT_PAGE_WRITABLE, 0u) != 0 ||
            thread_kernel_stack_valid(&probe) ||
            paging_test_modify_level_flags(probe_stack_va, level, 0u,
                                           PROSENJIT_PAGE_WRITABLE) != 0) {
            thread_irq_restore(permission_irq);
            return -29;
        }
    }
    for (uint32_t level = 0u; level < 4u; ++level)
        if (paging_test_modify_level_flags(probe_stack_va, level, 0u,
                                           PROSENJIT_PAGE_USER) != 0) {
            for (uint32_t restore = 0u; restore <= level; ++restore)
                (void)paging_test_modify_level_flags(probe_stack_va, restore,
                                                     PROSENJIT_PAGE_USER, 0u);
            thread_irq_restore(permission_irq);
            return -29;
        }
    int user_stack_rejected = !thread_kernel_stack_valid(&probe);
    for (uint32_t level = 0u; level < 4u; ++level)
        (void)paging_test_modify_level_flags(probe_stack_va, level,
                                             PROSENJIT_PAGE_USER, 0u);
    if (!user_stack_rejected ||
        paging_test_modify_level_flags(probe_stack_va, 3u,
                                       PROSENJIT_PAGE_NX, 0u) != 0 ||
        thread_kernel_stack_valid(&probe)) {
        (void)paging_test_modify_level_flags(probe_stack_va, 3u, 0u,
                                             PROSENJIT_PAGE_NX);
        thread_irq_restore(permission_irq);
        return -29;
    }
    (void)paging_test_modify_level_flags(probe_stack_va, 3u, 0u,
                                         PROSENJIT_PAGE_NX);
    thread_irq_restore(permission_irq);
    if (!thread_kernel_stack_valid(&probe)) return -29;

    /*
     * Invalid-frame cleanup must retain IPC ownership when the waiter array
     * cannot prove exact membership, then detach cleanly after repair.
     */
    struct pros_ipc_channel invalid_frame_channel;
    static struct pros_interrupt_frame invalid_frame_storage;
    if (ipc_channel_init(&invalid_frame_channel) != PROSENJIT_IPC_OK)
        return -32;
    probe.state = PROS_THREAD_BLOCKED;
    probe.wait_channel = &invalid_frame_channel;
    probe.blocked_syscall = 0x55u;
    probe.interrupt_frame = &invalid_frame_storage;
    probe.interrupt_frame_kind = PROS_INTERRUPT_FRAME_USER;
    invalid_frame_channel.waiters[0] = &probe;
    invalid_frame_channel.waiter_count = 2u; /* malformed NULL active slot */
    if (thread_invalid_frame_cleanup(&probe) != 1 ||
        probe.state != PROS_THREAD_BLOCKED ||
        probe.wait_channel != &invalid_frame_channel ||
        probe.blocked_syscall != 0x55u ||
        probe.interrupt_frame != 0 ||
        probe.interrupt_frame_kind != PROS_INTERRUPT_FRAME_NONE ||
        thread_wake(&probe) == 0)
        return -32;
    invalid_frame_channel.waiter_count = 1u;
    if (thread_invalid_frame_cleanup(&probe) != 0 ||
        probe.state != PROS_THREAD_DEAD ||
        probe.wait_channel != 0 ||
        probe.blocked_syscall != 0 ||
        invalid_frame_channel.waiter_count != 0u ||
        invalid_frame_channel.waiters[0] != 0)
        return -32;
    probe.state = PROS_THREAD_NEW;

    /*
     * Inject failure at each of the four physical-page allocations and each
     * of the four stack-page mappings. Rollback must restore allocator state,
     * free-page count, and registry integrity exactly.
     */
    static struct pros_thread allocation_failure_probe;
    uint64_t slots_before, quarantine_before;
    uint64_t free_before;
    for (uint32_t fail_after = 0u; fail_after < 4u; ++fail_after) {
        if (allocation_failure_probe.kernel_stack_base ||
            thread_stack_allocator_snapshot(&slots_before, &quarantine_before) != 0)
            return -30;
        free_before = physical_memory_free_pages();
        physical_memory_test_fail_alloc_after(fail_after);
        int alloc_rc = thread_init_kernel(
            &allocation_failure_probe, UINT64_MAX - 10u - fail_after,
            (uint64_t)(uintptr_t)thread_kernel_selftest_entry);
        physical_memory_test_fail_clear();
        uint64_t slots_after, quarantine_after;
        if (alloc_rc == 0 ||
            thread_stack_allocator_snapshot(&slots_after, &quarantine_after) != 0 ||
            slots_after != slots_before || quarantine_after != quarantine_before ||
            physical_memory_free_pages() != free_before ||
            allocation_failure_probe.kernel_stack_base ||
            thread_registry_validate() != 0)
            return -30;
    }
    for (uint32_t fail_after = 0u; fail_after < 4u; ++fail_after) {
        if (thread_stack_allocator_snapshot(&slots_before, &quarantine_before) != 0)
            return -31;
        free_before = physical_memory_free_pages();
        paging_test_map_fail_after(fail_after);
        int map_rc = thread_init_kernel(
            &allocation_failure_probe, UINT64_MAX - 20u - fail_after,
            (uint64_t)(uintptr_t)thread_kernel_selftest_entry);
        paging_test_map_fail_clear();
        uint64_t slots_after, quarantine_after;
        if (map_rc == 0 ||
            thread_stack_allocator_snapshot(&slots_after, &quarantine_after) != 0 ||
            slots_after != slots_before || quarantine_after != quarantine_before ||
            physical_memory_free_pages() != free_before ||
            allocation_failure_probe.kernel_stack_base ||
            thread_registry_validate() != 0)
            return -31;
    }

    /* Invalid wake targets are compared against the registry, never dereferenced. */
    if (thread_wake((struct pros_thread *)(uintptr_t)1u) == 0)
        return -22;

    /* The bounded registry validator must reject cycles and page aliases. */
    struct pros_thread *saved_registry_next = probe.registry_next;
    if (thread_registry_validate() != 0)
        return -9;
    probe.registry_next = &probe;
    if (thread_registry_validate() == 0)
        return -10;
    probe.registry_next = saved_registry_next;
    uint64_t saved_stack_page = probe.kernel_stack_pages[1];
    probe.kernel_stack_pages[1] = probe.kernel_stack_pages[0];
    if (thread_registry_validate() == 0)
        return -11;
    probe.kernel_stack_pages[1] = saved_stack_page;
    if (thread_registry_validate() != 0)
        return -12;

    /*
     * Exercise the cross-record alias check with two otherwise valid stacks.
     * Replace one page in the second stack with a mapping of the first stack's
     * page, keeping its metadata consistent so per-thread validation succeeds.
     */
    static struct pros_thread alias_probe;
    if (alias_probe.registry_next || alias_probe.kernel_stack_base ||
        thread_init_kernel(&alias_probe, UINT64_MAX - 2u,
                           (uint64_t)(uintptr_t)thread_kernel_selftest_entry) != 0)
        return -15;
    uint32_t alias_page_index = 0u;
    uint64_t alias_va =
        (uint64_t)(uintptr_t)alias_probe.kernel_stack_base +
        (uint64_t)alias_page_index * PROSENJIT_PAGE_SIZE;
    uint64_t original_alias_page =
        alias_probe.kernel_stack_pages[alias_page_index];
    uint64_t shared_page = probe.kernel_stack_pages[0];
    uint64_t alias_vas[1] = {alias_va};
    uint64_t alias_pas[1] = {original_alias_page};
    if (paging_unmap_pages_expected(alias_vas, alias_pas, 1u) != 0) {
        alias_probe.state = PROS_THREAD_DEAD;
        (void)thread_reap_kernel(&alias_probe);
        return -16;
    }
    physical_page_free(original_alias_page);
    if (paging_map_page(alias_va, shared_page,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        uint64_t replacement = physical_page_alloc();
        if (replacement &&
            paging_map_page(alias_va, replacement,
                            PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) == 0)
            alias_probe.kernel_stack_pages[alias_page_index] = replacement;
        else
            return -17; /* Preserve the slot rather than claim clean recovery. */
        alias_probe.state = PROS_THREAD_DEAD;
        (void)thread_reap_kernel(&alias_probe);
        return -17;
    }
    alias_probe.kernel_stack_pages[alias_page_index] = shared_page;
    uint64_t alias_free_before = physical_memory_free_pages();
    uint32_t alias_probe_state = alias_probe.state;
    alias_probe.state = PROS_THREAD_DEAD;
    int alias_reap_rc = thread_reap_kernel(&alias_probe);
    alias_probe.state = alias_probe_state;
    if (!thread_kernel_stack_valid(&probe) ||
        !thread_kernel_stack_valid(&alias_probe) ||
        thread_registry_validate() == 0 || alias_reap_rc == 0 ||
        alias_probe.kernel_stack_pages[alias_page_index] != shared_page ||
        physical_memory_free_pages() != alias_free_before) {
        /* Restore a valid, independently-owned page before reporting failure. */
        paging_unmap_page(alias_va);
        uint64_t replacement = physical_page_alloc();
        if (!replacement ||
            paging_map_page(alias_va, replacement,
                            PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0)
            return -18;
        alias_probe.kernel_stack_pages[alias_page_index] = replacement;
        alias_probe.state = PROS_THREAD_DEAD;
        (void)thread_reap_kernel(&alias_probe);
        return -18;
    }
    paging_unmap_page(alias_va);
    uint64_t restored_page = physical_page_alloc();
    if (!restored_page ||
        paging_map_page(alias_va, restored_page,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0)
        return -19;
    alias_probe.kernel_stack_pages[alias_page_index] = restored_page;
    if (thread_registry_validate() != 0) return -20;
    alias_probe.state = PROS_THREAD_DEAD;
    if (thread_reap_kernel(&alias_probe) != 0 ||
        alias_probe.registry_next || alias_probe.kernel_stack_base ||
        thread_registry_validate() != 0)
        return -21;

    /*
     * A detached, quarantined unpublished target must not make abort search
     * loop forever when the remaining registry is cyclic. The quarantined
     * slot is a valid baseline without the cycle; the cycle itself must make
     * the bounded preflight reject before touching the target's stack.
     */
    static struct pros_thread abort_cycle_probe;
    if (abort_cycle_probe.registry_next || abort_cycle_probe.kernel_stack_base ||
        thread_init_kernel(&abort_cycle_probe, UINT64_MAX - 3u,
                           (uint64_t)(uintptr_t)thread_kernel_selftest_entry) != 0)
        return -23;
    abort_cycle_probe.owner_kind = PROS_THREAD_OWNER_PROCESS;
    abort_cycle_probe.process = (struct pros_process *)(uintptr_t)1u;
    struct pros_thread *abort_saved_head = abort_cycle_probe.registry_next;
    uint32_t abort_slot = abort_cycle_probe.kernel_stack_slot;
    uint64_t abort_slot_mask = 1ULL << (abort_slot & 63u);
    uint64_t abort_pages[PROS_THREAD_STACK_PAGE_COUNT];
    for (uint32_t p = 0u; p < PROS_THREAD_STACK_PAGE_COUNT; ++p)
        abort_pages[p] = abort_cycle_probe.kernel_stack_pages[p];
    uint64_t abort_free_before = physical_memory_free_pages();

    if (!abort_saved_head || thread_registry != &abort_cycle_probe)
        return -24;
    thread_registry = abort_saved_head;
    thread_registry_slots[abort_slot] = 0;
    thread_stack_quarantined_slots[abort_slot >> 6] |= abort_slot_mask;
    if (thread_registry_validate() != 0) {
        thread_registry = &abort_cycle_probe;
        thread_registry_slots[abort_slot] = &abort_cycle_probe;
        thread_stack_quarantined_slots[abort_slot >> 6] &= ~abort_slot_mask;
        return -25;
    }

    struct pros_thread *abort_cycle_tail = thread_registry;
    while (abort_cycle_tail->registry_next)
        abort_cycle_tail = abort_cycle_tail->registry_next;
    abort_cycle_tail->registry_next = thread_registry;
    int abort_cycle_rc = thread_abort_unpublished(&abort_cycle_probe);

    /*
     * Exercise the other single-target reaper against the same malformed
     * registry. It must reject before releasing a quarantined, detached stack.
     */
    abort_cycle_probe.owner_kind = PROS_THREAD_OWNER_KERNEL;
    abort_cycle_probe.process = 0;
    abort_cycle_probe.state = PROS_THREAD_DEAD;
    int kernel_reap_cycle_rc = thread_reap_kernel(&abort_cycle_probe);
    abort_cycle_tail->registry_next = 0;

    thread_registry = &abort_cycle_probe;
    abort_cycle_probe.registry_next = abort_saved_head;
    thread_registry_slots[abort_slot] = &abort_cycle_probe;
    thread_stack_quarantined_slots[abort_slot >> 6] &= ~abort_slot_mask;
    if (abort_cycle_rc == 0 || kernel_reap_cycle_rc == 0 ||
        abort_cycle_probe.owner_kind != PROS_THREAD_OWNER_KERNEL ||
        abort_cycle_probe.process != 0 ||
        abort_cycle_probe.state != PROS_THREAD_DEAD ||
        abort_cycle_probe.kernel_stack_slot != abort_slot ||
        !abort_cycle_probe.kernel_stack_base ||
        abort_cycle_probe.kernel_stack_size != PROS_THREAD_KERNEL_STACK_SIZE ||
        physical_memory_free_pages() != abort_free_before ||
        !(thread_stack_slots[abort_slot >> 6] & abort_slot_mask) ||
        thread_registry_validate() != 0)
        return -26;
    for (uint32_t p = 0u; p < PROS_THREAD_STACK_PAGE_COUNT; ++p)
        if (abort_cycle_probe.kernel_stack_pages[p] != abort_pages[p] ||
            !physical_page_is_allocated(abort_pages[p]))
            return -27;

    abort_cycle_probe.owner_kind = PROS_THREAD_OWNER_KERNEL;
    abort_cycle_probe.process = 0;
    abort_cycle_probe.state = PROS_THREAD_DEAD;
    if (thread_reap_kernel(&abort_cycle_probe) != 0 ||
        abort_cycle_probe.kernel_stack_base ||
        thread_registry_validate() != 0)
        return -28;

    /*
     * Wait/reap must reject scheduler corruption before any process resource
     * can be released. Exercise each queue boundary and restore it exactly.
     */
    if (runq_head && runq_tail) {
        struct pros_thread *saved_head = runq_head;
        struct pros_thread *saved_tail = runq_tail;
        runq_head = (struct pros_thread *)(uintptr_t)1u;
        if (thread_runq_validate() == 0 ||
            thread_schedule() == 0 ||
            thread_has_runnable() != 0 ||
            thread_dequeue() != 0 ||
            thread_remove_queued(saved_head) == 0 ||
            thread_enqueue(saved_head) == 0) {
            runq_head = saved_head;
            return -14;
        }
        runq_head = saved_head;
        runq_tail = (struct pros_thread *)(uintptr_t)1u;
        if (thread_runq_validate() == 0) {
            runq_tail = saved_tail;
            return -14;
        }
        runq_tail = saved_tail;

        struct pros_thread *saved_head_prev = runq_head->prev;
        runq_head->prev = &probe;
        int bad_head_prev_rejected = thread_runq_validate() != 0;
        runq_head->prev = saved_head_prev;
        if (!bad_head_prev_rejected) return -14;

        struct pros_thread *saved_head_next = runq_head->next;
        runq_head->next = (struct pros_thread *)(uintptr_t)1u;
        if (thread_runq_validate() == 0) {
            runq_head->next = saved_head_next;
            return -14;
        }
        runq_head->next = saved_head_next;

        struct pros_thread *saved_tail_next = runq_tail->next;
        runq_tail->next = &probe;
        if (thread_runq_validate() == 0) {
            runq_tail->next = saved_tail_next;
            return -14;
        }
        runq_tail->next = saved_tail_next;

        runq_tail->next = runq_head;
        if (thread_runq_validate() == 0) {
            runq_tail->next = 0;
            return -14;
        }
        runq_tail->next = 0;
        if (thread_runq_validate() != 0) return -14;
    }

    /* Reinitialization must fail closed while ownership is live. */
    struct pros_thread *saved_registry_head = thread_registry;
    if (thread_manager_init() == 0 || thread_registry != saved_registry_head ||
        !thread_stack_slots[probe.kernel_stack_slot >> 6] ||
        !thread_kernel_stack_valid(&probe))
        return -13;
    /* Reject owner/process mismatches before they can enter the run queue. */
    struct pros_process *saved_process = process_current();
    if (!saved_process) return -4;
    probe.process = saved_process;
    if (thread_process_runnable(&probe)) return -4;
    probe.process = 0;
    probe.owner_kind = UINT32_MAX;
    if (thread_process_runnable(&probe)) return -4;
    probe.owner_kind = PROS_THREAD_OWNER_KERNEL;
    if (thread_init(&probe, 0, UINT64_MAX - 1u,
                    (uint64_t)(uintptr_t)thread_kernel_selftest_entry) == 0)
        return -4;
    if (thread_set_state(&probe, PROS_THREAD_READY) != 0 ||
        !thread_process_runnable(&probe) ||
        thread_enqueue(&probe) != 0 ||
        !thread_is_queued(&probe) ||
        thread_remove_queued(&probe) != 0 ||
        thread_state(&probe) != PROS_THREAD_READY)
        return -4;

    /* Both scheduler rollback paths must preserve a blocked kernel owner. */
    probe.state = PROS_THREAD_BLOCKED;
    if (thread_ineligible_state(&probe) != PROS_THREAD_BLOCKED ||
        thread_can_resume_interrupted(&probe))
        return -5;
    probe.state = PROS_THREAD_RUNNING;
    if (!thread_can_resume_interrupted(&probe))
        return -5;
    if (thread_interrupt_should_park_without_successor(&probe) ||
        thread_interrupt_should_park_after_switch_failure(&probe))
        return -5;
    probe.state = PROS_THREAD_BLOCKED;
    if (!thread_interrupt_should_park_without_successor(&probe) ||
        !thread_interrupt_should_park_after_switch_failure(&probe))
        return -5;
    probe.state = PROS_THREAD_RUNNING;
    probe.owner_kind = UINT32_MAX;
    if (!thread_interrupt_should_park_without_successor(&probe) ||
        !thread_interrupt_should_park_after_switch_failure(&probe))
        return -5;
    probe.state = PROS_THREAD_BLOCKED;
    probe.owner_kind = UINT32_MAX;
    if (thread_ineligible_state(&probe) != PROS_THREAD_DEAD)
        return -5;
    probe.owner_kind = PROS_THREAD_OWNER_KERNEL;

    /* A blocked kernel worker must remain eligible for a later wake. */
    probe.state = PROS_THREAD_BLOCKED;
    if (thread_wake(&probe) != 0 ||
        probe.state != PROS_THREAD_READY ||
        !thread_is_queued(&probe) ||
        thread_remove_queued(&probe) != 0)
        return -5;

    /*
     * A blocked user syscall must be repaired before queue publication, and
     * an enqueue rejection must restore the original continuation exactly.
     */
    static struct pros_interrupt_frame wake_frame;
    probe.owner_kind = PROS_THREAD_OWNER_PROCESS;
    probe.process = saved_process;
    probe.state = PROS_THREAD_BLOCKED;
    probe.interrupt_frame = &wake_frame;
    probe.interrupt_frame_kind = PROS_INTERRUPT_FRAME_USER;
    probe.wait_channel = &probe;
    probe.blocked_syscall = 0x55u;
    wake_frame = (struct pros_interrupt_frame){0};
    wake_frame.rip = 0x1002u;
    wake_frame.cs = 0x1Bu;
    wake_frame.rflags = 0x202u;
    wake_frame.user_rsp = 0x70000000u;
    wake_frame.user_ss = 0x23u;
    wake_frame.rax = 0xDEADBEEFu;
    probe.next = &probe; /* Force thread_enqueue() to reject this attempt. */
    if (thread_wake(&probe) == 0 ||
        probe.state != PROS_THREAD_BLOCKED ||
        probe.blocked_syscall != 0x55u ||
        wake_frame.rax != 0xDEADBEEFu ||
        wake_frame.rip != 0x1002u ||
        probe.wait_channel != &probe)
        return -6;
    probe.next = 0;
    if (thread_wake(&probe) != 0 ||
        probe.state != PROS_THREAD_READY ||
        probe.blocked_syscall != 0 ||
        wake_frame.rax != 0x55u ||
        wake_frame.rip != 0x1000u ||
        probe.wait_channel != 0 ||
        !thread_is_queued(&probe) ||
        thread_remove_queued(&probe) != 0)
        return -6;
    probe.owner_kind = PROS_THREAD_OWNER_KERNEL;
    probe.process = 0;
    probe.state = PROS_THREAD_BLOCKED;
    probe.interrupt_frame = 0;
    probe.interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    probe.wait_channel = 0;
    probe.blocked_syscall = 0;

    /*
     * Kernel workers must execute with the kernel page tables, not retain the
     * outgoing user process context. Restore the original process afterwards.
     */
    process_set_current(0);
    if (process_current() != 0) return -6;
    process_set_current(saved_process);
    if (process_current() != saved_process) return -6;

    probe.state = PROS_THREAD_DEAD;
    if (cpu_get_kernel_stack() ==
        (uint64_t)(uintptr_t)probe.kernel_stack_base + probe.kernel_stack_size)
        return -7;
    if (thread_reap_kernel(&probe) != 0 ||
        probe.registry_next || probe.kernel_stack_base ||
        probe.kernel_stack_size)
        return -8;
    return 0;
}

uint64_t thread_runnable_count(void) {
    if (thread_registry_validate() != 0) return 0;
    uint64_t count = 0;
    for (struct pros_thread *thread = thread_registry; thread; thread = thread->registry_next)
        if (thread_process_runnable(thread) &&
            (thread->state == PROS_THREAD_READY || thread->state == PROS_THREAD_RUNNING))
            ++count;
    return count;
}

uint64_t thread_blocked_count(void) {
    if (thread_registry_validate() != 0) return 0;
    uint64_t count = 0;
    for (struct pros_thread *thread = thread_registry; thread; thread = thread->registry_next)
        if (thread->state == PROS_THREAD_BLOCKED)
            ++count;
    return count;
}

uint64_t thread_dead_count(void) {
    if (thread_registry_validate() != 0) return 0;
    uint64_t count = 0;
    for (struct pros_thread *thread = thread_registry; thread; thread = thread->registry_next)
        if (thread->state == PROS_THREAD_DEAD)
            ++count;
    return count;
}

int thread_has_runnable(void) {
    /* A cyclic or malformed queue must not turn this probe into a hang. */
    if (thread_runq_validate() != 0) return 0;
    uint32_t count = 0u;
    for (struct pros_thread *thread = runq_head; thread; thread = thread->next) {
        if (count++ >= PROS_THREAD_STACK_SLOT_COUNT) return 0;
        if (thread_process_runnable(thread)) return 1;
    }
    return 0;
}

int thread_schedule_preempt(void) {
    struct pros_thread *prev = current_thread;
    if (thread_schedule() != 0) return -1;
    if (prev && prev != current_thread)
        thread_context_switch(prev, current_thread);
    return 0;
}

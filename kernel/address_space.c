#include "address_space.h"
#include "memory.h"
#include "paging.h"
#include "heap.h"

#define PAGE_MASK 0x000FFFFFFFFFF000ULL
#define ENTRIES 512u
#define PT_INDEX(va) (((va) >> 12) & 0x1FFu)
#define PD_INDEX(va) (((va) >> 21) & 0x1FFu)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FFu)
#define PML4_INDEX(va) (((va) >> 39) & 0x1FFu)
#define LARGE_PAGE_FLAG (1ULL << 7)
#define USER_MIN_VA PROSENJIT_USER_CODE_VA
#define DESTROY_BITMAP_WORDS \
    (((PROSENJIT_PHYS_LIMIT / PROSENJIT_PAGE_SIZE) + 63ULL) / 64ULL)

/* Temporary preflight marks; protected by local interrupt exclusion. */
static uint64_t destroy_table_seen[DESTROY_BITMAP_WORDS];
static uint64_t destroy_leaf_seen[DESTROY_BITMAP_WORDS];
static int address_space_last_map_failure_stage;

int address_space_last_map_failure(void) {
    return address_space_last_map_failure_stage;
}

static int destroy_bitmap_mark(uint64_t *bitmap, uint64_t physical) {
    if (!bitmap || (physical & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0)
        return -1;
    uint64_t page = physical / PROSENJIT_PAGE_SIZE;
    if (page >= PROSENJIT_PHYS_LIMIT / PROSENJIT_PAGE_SIZE)
        return -1;
    uint64_t mask = 1ULL << (page & 63ULL);
    uint64_t *word = &bitmap[page >> 6];
    if (*word & mask) return -1;
    *word |= mask;
    return 0;
}

extern uint64_t *paging_kernel_root(void);

static void zero_page(uint64_t *page) {
    for (uint32_t i = 0; i < ENTRIES; ++i) page[i] = 0;
}

static int user_va_valid(uint64_t virtual_address) {
    return virtual_address >= USER_MIN_VA &&
           virtual_address < 0x0000800000000000ULL &&
           (virtual_address & 0xFFFULL) == 0;
}

static int physical_already_mapped(const struct pros_address_space *space,
                                   uint64_t physical_address) {
    if (!space || !space->root) return 1;
    for (uint32_t pml4 = 1; pml4 < 256; ++pml4) {
        uint64_t pml4e = space->root[pml4];
        if (!(pml4e & PROSENJIT_PAGE_PRESENT) || (pml4e & LARGE_PAGE_FLAG)) continue;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PAGE_MASK);
        for (uint32_t pdpti = 0; pdpti < ENTRIES; ++pdpti) {
            uint64_t pdpte = pdpt[pdpti];
            if (!(pdpte & PROSENJIT_PAGE_PRESENT) || (pdpte & LARGE_PAGE_FLAG)) continue;
            uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PAGE_MASK);
            for (uint32_t pdi = 0; pdi < ENTRIES; ++pdi) {
                uint64_t pde = pd[pdi];
                if (!(pde & PROSENJIT_PAGE_PRESENT) || (pde & LARGE_PAGE_FLAG)) continue;
                uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PAGE_MASK);
                for (uint32_t pti = 0; pti < ENTRIES; ++pti) {
                    uint64_t pte = pt[pti];
                    if ((pte & PROSENJIT_PAGE_PRESENT) &&
                        (pte & PAGE_MASK) == physical_address)
                        return 1;
                }
            }
        }
    }
    return 0;
}

static int table_empty(const uint64_t *table) {
    if (!table) return 1;
    for (uint32_t i = 0; i < ENTRIES; ++i)
        if (table[i] & PROSENJIT_PAGE_PRESENT)
            return 0;
    return 1;
}

static uint64_t *alloc_table(void) {
    uint64_t physical = physical_page_alloc();
    if (!physical) return 0;
    uint64_t *table = (uint64_t *)(uintptr_t)physical;
    zero_page(table);
    return table;
}

static uint64_t *ensure_table(uint64_t *parent, uint32_t index) {
    uint64_t entry = parent[index];
    if (entry & PROSENJIT_PAGE_PRESENT) {
        if (entry & LARGE_PAGE_FLAG) return 0;
        return (uint64_t *)(uintptr_t)(entry & PAGE_MASK);
    }

    uint64_t *table = alloc_table();
    if (!table) return 0;
    parent[index] = ((uint64_t)(uintptr_t)table & PAGE_MASK) |
                    PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_WRITABLE |
                    PROSENJIT_PAGE_USER;
    return table;
}

int address_space_create(struct pros_address_space *space) {
    if (!space) return -1;
    uint64_t *root = alloc_table();
    if (!root) return -1;

    uint64_t *kernel_root = paging_kernel_root();
    if (!kernel_root) {
        physical_page_free((uint64_t)(uintptr_t)root);
        return -1;
    }

    /*
     * PML4[0] is shared with the kernel's low identity map so CPL3
     * interrupts can still enter kernel code and use the TSS RSP0 stack.
     * Keep this top-level entry supervisor-only: user pages live at
     * PML4[1] (0x0000008000000000), so they do not need USER on PML4[0].
     */ 
    root[0] = kernel_root[0] & ~PROSENJIT_PAGE_USER;
    for (uint32_t i = 256; i < ENTRIES; ++i)
        root[i] = kernel_root[i];

    space->root = root;
    space->owned_pages = 0;
    return 0;
}

int address_space_map_user_page(struct pros_address_space *space,
                                uint64_t virtual_address,
                                uint64_t physical_address,
                                uint64_t flags) {
    address_space_last_map_failure_stage = 0;
    if (!space || !space->root || !user_va_valid(virtual_address)) {
        address_space_last_map_failure_stage = 11;
        return -1;
    }
    if ((physical_address & 0xFFFULL) ||
        (physical_address & ~PAGE_MASK) != 0 ||
        physical_address == 0 ||
        physical_address >= PROSENJIT_PHYS_LIMIT) {
        address_space_last_map_failure_stage = 12;
        return -1;
    }
    if (!physical_page_is_user_mappable(physical_address)) {
        address_space_last_map_failure_stage = 13;
        return -1;
    }
    if ((flags & ~(PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX)) != 0 ||
        ((flags & PROSENJIT_PAGE_WRITABLE) && !(flags & PROSENJIT_PAGE_NX))) {
        address_space_last_map_failure_stage = 14;
        return -1;
    }

    uint32_t pml4i = PML4_INDEX(virtual_address);
    uint32_t pdpti = PDPT_INDEX(virtual_address);
    uint32_t pdi = PD_INDEX(virtual_address);
    uint32_t pti = PT_INDEX(virtual_address);
    uint64_t *root = space->root;
    uint64_t old_pml4e = root[pml4i];
    /* Every paging level covering a user mapping must carry USER=1.
     * Never turn an existing supervisor-only hierarchy into a user mapping. */
    if ((old_pml4e & PROSENJIT_PAGE_PRESENT) &&
        (!(old_pml4e & PROSENJIT_PAGE_USER) ||
         (old_pml4e & LARGE_PAGE_FLAG) ||
         ((flags & PROSENJIT_PAGE_WRITABLE) &&
          !(old_pml4e & PROSENJIT_PAGE_WRITABLE))))
        { address_space_last_map_failure_stage = 15; return -1; }
    uint64_t *pdpt = ensure_table(root, pml4i);
    if (!pdpt) { address_space_last_map_failure_stage = 16; return -1; }
    int created_pdpt = !(old_pml4e & PROSENJIT_PAGE_PRESENT);

    uint64_t old_pdpte = pdpt[pdpti];
    if ((old_pdpte & PROSENJIT_PAGE_PRESENT) &&
        (!(old_pdpte & PROSENJIT_PAGE_USER) ||
         (old_pdpte & LARGE_PAGE_FLAG) ||
         ((flags & PROSENJIT_PAGE_WRITABLE) &&
          !(old_pdpte & PROSENJIT_PAGE_WRITABLE))))
        { address_space_last_map_failure_stage = 17; return -1; }
    uint64_t *pd = ensure_table(pdpt, pdpti);
    if (!pd) {
        if (created_pdpt) {
            physical_page_free((uint64_t)(uintptr_t)pdpt);
            root[pml4i] = 0;
        }
        address_space_last_map_failure_stage = 18;
        return -1;
    }
    int created_pd = !(old_pdpte & PROSENJIT_PAGE_PRESENT);

    uint64_t old_pde = pd[pdi];
    if ((old_pde & PROSENJIT_PAGE_PRESENT) &&
        (!(old_pde & PROSENJIT_PAGE_USER) ||
         (old_pde & LARGE_PAGE_FLAG) ||
         ((flags & PROSENJIT_PAGE_WRITABLE) &&
          !(old_pde & PROSENJIT_PAGE_WRITABLE)))) {
        address_space_last_map_failure_stage = 19;
        return -1;
    }
    uint64_t *pt = ensure_table(pd, pdi);
    if (!pt) {
        if (created_pd) {
            physical_page_free((uint64_t)(uintptr_t)pd);
            pdpt[pdpti] = 0;
        }
        if (created_pdpt) {
            physical_page_free((uint64_t)(uintptr_t)pdpt);
            root[pml4i] = 0;
        }
        address_space_last_map_failure_stage = 20;
        return -1;
    }
    int created_pt = !(old_pde & PROSENJIT_PAGE_PRESENT);

    if (pt[pti] & PROSENJIT_PAGE_PRESENT) {
        if (created_pt) {
            physical_page_free((uint64_t)(uintptr_t)pt);
            pd[pdi] = 0;
        }
        if (created_pd) {
            physical_page_free((uint64_t)(uintptr_t)pd);
            pdpt[pdpti] = 0;
        }
        if (created_pdpt) {
            physical_page_free((uint64_t)(uintptr_t)pdpt);
            root[pml4i] = 0;
        }
        address_space_last_map_failure_stage = 21;
        return -1;
    }

    pt[pti] =
        (physical_address & PAGE_MASK) |
        PROSENJIT_PAGE_PRESENT |
        PROSENJIT_PAGE_USER |
        (flags & (PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX));
    return 0;
}

int address_space_map_user_page_owned(struct pros_address_space *space,
                                      uint64_t virtual_address,
                                      uint64_t physical_address,
                                      uint64_t flags) {
    address_space_last_map_failure_stage = 20;
    if (!space || !space->root || !user_va_valid(virtual_address)) {
        address_space_last_map_failure_stage = 31;
        return -1;
    }
    if ((physical_address & 0xFFFULL) ||
        (physical_address & ~PAGE_MASK) != 0 ||
        physical_address == 0 ||
        physical_address >= PROSENJIT_PHYS_LIMIT) {
        address_space_last_map_failure_stage = 32;
        return -1;
    }
    if ((flags & ~(PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX)) != 0 ||
        ((flags & PROSENJIT_PAGE_WRITABLE) && !(flags & PROSENJIT_PAGE_NX))) {
        address_space_last_map_failure_stage = 33;
        return -1;
    }
    if (physical_already_mapped(space, physical_address)) {
        address_space_last_map_failure_stage = 34;
        return -1;
    }
    struct pros_owned_page *owned = (struct pros_owned_page *)kmalloc(sizeof(*owned));
    if (!owned) {
        address_space_last_map_failure_stage = 35;
        return -1;
    }
    if (address_space_map_user_page(space, virtual_address, physical_address, flags) != 0) {
        kfree(owned);
        return -1;
    }
    owned->virtual_address = virtual_address;
    owned->physical_address = physical_address;
    owned->next = space->owned_pages;
    space->owned_pages = owned;
    return 0;
}

static void destroy_table(uint64_t *table, uint32_t level) {
    if (!table) return;

    for (uint32_t i = 0; i < ENTRIES; ++i) {
        uint64_t entry = table[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT)) continue;
        if (entry & LARGE_PAGE_FLAG) continue;

        uint64_t *child = (uint64_t *)(uintptr_t)(entry & PAGE_MASK);
        if (level > 1) {
            destroy_table(child, level - 1);
            physical_page_free((uint64_t)(uintptr_t)child);
        }
        /* At level 1, entries are leaf mappings; their physical pages
         * are owned by the caller and are intentionally not freed here. */
    }
}

/* Validate the complete user-owned page-table subtree before destruction. */
static int destroy_table_preflight(const uint64_t *table, uint32_t level,
                                   const uint64_t *ancestors,
                                   uint32_t ancestor_count) {
    if (!table || level == 0u || ancestor_count >= 4u) return -1;

    uint64_t table_physical = (uint64_t)(uintptr_t)table;
    if ((table_physical & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0 ||
        table_physical >= PROSENJIT_PHYS_LIMIT ||
        !physical_page_is_allocated(table_physical) ||
        physical_page_is_user_mappable(table_physical))
        return -1;
    for (uint32_t i = 0; i < ancestor_count; ++i)
        if (ancestors[i] == table_physical) return -1;
    /* Reject a page-table node referenced from any second parent branch. */
    if (destroy_bitmap_mark(destroy_table_seen, table_physical) != 0)
        return -1;

    uint64_t next_ancestors[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < ancestor_count; ++i)
        next_ancestors[i] = ancestors[i];
    next_ancestors[ancestor_count] = table_physical;

    for (uint32_t i = 0; i < ENTRIES; ++i) {
        uint64_t entry = table[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT)) continue;
        if (!(entry & PROSENJIT_PAGE_USER)) return -1;

        uint64_t physical = entry & PAGE_MASK;
        if (!physical || physical >= PROSENJIT_PHYS_LIMIT ||
            !physical_page_is_allocated(physical))
            return -1;

        if (level > 1u) {
            if ((entry & LARGE_PAGE_FLAG) ||
                physical_page_is_user_mappable(physical))
                return -1;
            for (uint32_t ancestor = 0; ancestor <= ancestor_count; ++ancestor)
                if (next_ancestors[ancestor] == physical) return -1;
            for (uint32_t previous = 0; previous < i; ++previous)
                if ((table[previous] & PROSENJIT_PAGE_PRESENT) &&
                    (table[previous] & PAGE_MASK) == physical)
                    return -1;

            const uint64_t *child = (const uint64_t *)(uintptr_t)physical;
            if (destroy_table_preflight(child, level - 1u,
                                        next_ancestors,
                                        ancestor_count + 1u) != 0)
                return -1;
        } else if (!physical_page_is_user_mappable(physical) ||
                   destroy_bitmap_mark(destroy_leaf_seen, physical) != 0) {
            /* One physical user page may back only one virtual leaf mapping. */
            return -1;
        }
    }
    return 0;
}

static int address_space_destroy_preflight_locked(
    const struct pros_address_space *space) {
    if (!space || !space->root) return -1;

    uint64_t root = (uint64_t)(uintptr_t)space->root;
    if ((root & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0 ||
        root >= PROSENJIT_PHYS_LIMIT || !physical_page_is_allocated(root) ||
        physical_page_is_user_mappable(root))
        return -1;
    if (destroy_bitmap_mark(destroy_table_seen, root) != 0)
        return -1;

    uint64_t current_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(current_cr3) : : "memory");
    if ((current_cr3 & PAGE_MASK) == (root & PAGE_MASK))
        return -2;

    /* PML4[0] and PML4[256..511] are shared kernel mappings. */
    for (uint32_t i = 1; i < 256; ++i) {
        uint64_t entry = space->root[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT)) continue;
        if (!(entry & PROSENJIT_PAGE_USER) || (entry & LARGE_PAGE_FLAG))
            return -1;

        uint64_t pdpt_physical = entry & PAGE_MASK;
        if (!pdpt_physical || pdpt_physical >= PROSENJIT_PHYS_LIMIT ||
            !physical_page_is_allocated(pdpt_physical))
            return -1;
        for (uint32_t previous = 1; previous < i; ++previous)
            if ((space->root[previous] & PROSENJIT_PAGE_PRESENT) &&
                (space->root[previous] & PAGE_MASK) == pdpt_physical)
                return -1;
        uint64_t ancestors[4] = {root, 0, 0, 0};
        if (destroy_table_preflight(
                (const uint64_t *)(uintptr_t)pdpt_physical, 3u,
                ancestors, 1u) != 0)
            return -1;
    }

    /* Detect cycles and stale/mismatched ownership records before freeing. */
    struct pros_owned_page *slow = space->owned_pages;
    struct pros_owned_page *fast = space->owned_pages;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) return -1;
    }

    /* Reuse the leaf bitmap to reject duplicate ownership/free records. */
    for (uint64_t i = 0; i < DESTROY_BITMAP_WORDS; ++i)
        destroy_leaf_seen[i] = 0u;

    for (struct pros_owned_page *owned = space->owned_pages;
         owned; owned = owned->next) {
        uint64_t mapped = 0;
        if (destroy_bitmap_mark(destroy_leaf_seen,
                                owned->physical_address) != 0)
            return -1;
        if (!user_va_valid(owned->virtual_address) ||
            (owned->physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0 ||
            owned->physical_address == 0 ||
            owned->physical_address >= PROSENJIT_PHYS_LIMIT ||
            !physical_page_is_allocated(owned->physical_address) ||
            !physical_page_is_user_mappable(owned->physical_address) ||
            address_space_translate_user(space, owned->virtual_address,
                                         &mapped, 0) != 0 ||
            mapped != owned->physical_address)
            return -1;
    }
    return 0;
}

int address_space_destroy_preflight(const struct pros_address_space *space) {
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    for (uint64_t i = 0; i < DESTROY_BITMAP_WORDS; ++i) {
        destroy_table_seen[i] = 0u;
        destroy_leaf_seen[i] = 0u;
    }
    int rc = address_space_destroy_preflight_locked(space);
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return rc;
}

int address_space_unmap_user_page(struct pros_address_space *space,
                                  uint64_t virtual_address,
                                  uint64_t *physical_address_out) {
    if (!space || !space->root || !user_va_valid(virtual_address))
        return -1;
    uint64_t e = space->root[PML4_INDEX(virtual_address)];
    if (!(e & PROSENJIT_PAGE_PRESENT) || !(e & PROSENJIT_PAGE_USER) ||
        (e & LARGE_PAGE_FLAG)) return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(e & PAGE_MASK);
    e = pdpt[PDPT_INDEX(virtual_address)];
    if (!(e & PROSENJIT_PAGE_PRESENT) || !(e & PROSENJIT_PAGE_USER) ||
        (e & LARGE_PAGE_FLAG)) return -1;
    uint64_t *pd = (uint64_t *)(uintptr_t)(e & PAGE_MASK);
    e = pd[PD_INDEX(virtual_address)];
    if (!(e & PROSENJIT_PAGE_PRESENT) || !(e & PROSENJIT_PAGE_USER) ||
        (e & LARGE_PAGE_FLAG)) return -1;
    uint64_t *pt = (uint64_t *)(uintptr_t)(e & PAGE_MASK);
    e = pt[PT_INDEX(virtual_address)];
    if (!(e & PROSENJIT_PAGE_PRESENT) || !(e & PROSENJIT_PAGE_USER))
        return -1;
    uint64_t physical = e & PAGE_MASK;
    /*
     * An ownership record is authoritative only when it names this exact
     * physical mapping. Reject duplicate or mismatched records before
     * changing either the page table or the ownership list. Untracked
     * mappings remain supported by address_space_map_user_page().
     */
    struct pros_owned_page **owned_link = &space->owned_pages;
    struct pros_owned_page *owned_match = 0;
    while (*owned_link) {
        if ((*owned_link)->virtual_address == virtual_address) {
            if (owned_match || (*owned_link)->physical_address != physical)
                return -1;
            owned_match = *owned_link;
        }
        owned_link = &(*owned_link)->next;
    }
    if (physical_address_out) *physical_address_out = physical;
    if (owned_match) {
        struct pros_owned_page **link = &space->owned_pages;
        while (*link != owned_match) link = &(*link)->next;
        *link = owned_match->next;
        kfree(owned_match);
    }
    pt[PT_INDEX(virtual_address)] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");

    /* Reclaim empty user page-table levels; otherwise repeated map/unmap
     * cycles would leak four KiB paging pages even though the leaf page
     * itself was released by the caller. */
    uint32_t pdi = PD_INDEX(virtual_address);
    uint32_t pdpti = PDPT_INDEX(virtual_address);
    uint32_t pml4i = PML4_INDEX(virtual_address);
    uint64_t *pde_table = pd;
    uint64_t *pdpt_table = pdpt;
    if (table_empty(pt)) {
        pd[pdi] = 0;
        physical_page_free((uint64_t)(uintptr_t)pt);
    }
    if (table_empty(pde_table)) {
        pdpt[pdpti] = 0;
        physical_page_free((uint64_t)(uintptr_t)pd);
    }
    if (table_empty(pdpt_table)) {
        space->root[pml4i] = 0;
        physical_page_free((uint64_t)(uintptr_t)pdpt);
    }
    return 0;
}

int address_space_destroy(struct pros_address_space *space) {
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    int preflight_rc = address_space_destroy_preflight(space);
    if (preflight_rc != 0) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return preflight_rc;
    }

    /*
     * PML4[0] and PML4[256..511] reference shared kernel mappings and are
     * never owned by this address space. Only user-space PML4[1..255] is
     * recursively owned and released.
     */
    for (uint32_t i = 1; i < 256; ++i) {
        uint64_t entry = space->root[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT) ||
            (entry & LARGE_PAGE_FLAG))
            continue;

        uint64_t *pdpt = (uint64_t *)(uintptr_t)(entry & PAGE_MASK);
        destroy_table(pdpt, 3);
        physical_page_free((uint64_t)(uintptr_t)pdpt);
    }

    struct pros_owned_page *owned = space->owned_pages;
    while (owned) {
        struct pros_owned_page *next = owned->next;
        /* Only still-mapped pages remain in the ownership list. */
        physical_page_free(owned->physical_address);
        kfree(owned);
        owned = next;
    }
    space->owned_pages = 0;
    uint64_t root = (uint64_t)(uintptr_t)space->root;
    space->root = 0;
    physical_page_free(root);
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return 0;
}

int address_space_switch(struct pros_address_space *space) {
    if (!space || !space->root) return -1;
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)(uintptr_t)space->root) : "memory");
    return 0;
}

int address_space_translate_user(const struct pros_address_space *space,
                                 uint64_t virtual_address,
                                 uint64_t *physical_address_out,
                                 uint64_t *flags_out) {
    if (!space || !space->root || !physical_address_out ||
        virtual_address < USER_MIN_VA || virtual_address >= 0x0000800000000000ULL)
        return -1;
    uint64_t pml4e = space->root[PML4_INDEX(virtual_address)];
    if ((pml4e & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
        return -1;
    if (pml4e & LARGE_PAGE_FLAG) return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PAGE_MASK);
    uint64_t pdpte = pdpt[PDPT_INDEX(virtual_address)];
    if ((pdpte & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
        return -1;
    if (pdpte & LARGE_PAGE_FLAG) return -1;
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PAGE_MASK);
    uint64_t pde = pd[PD_INDEX(virtual_address)];
    if ((pde & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
        return -1;
    if (pde & LARGE_PAGE_FLAG) return -1;
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PAGE_MASK);
    uint64_t pte = pt[PT_INDEX(virtual_address)];
    if ((pte & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
        return -1;
    *physical_address_out = (pte & PAGE_MASK) | (virtual_address & 0xFFFULL);
    if (flags_out)
        *flags_out = pte & (PROSENJIT_PAGE_USER | PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX);
    return 0;
}

int address_space_validate_user_range(const struct pros_address_space *space,
                                      uint64_t virtual_address,
                                      uint64_t size,
                                      int write_access) {
    if (!space || !space->root || size == 0 || virtual_address < USER_MIN_VA) return -1;

    uint64_t end = virtual_address + size - 1ULL;
    if (end < virtual_address || (virtual_address >> 47) != 0 ||
        (end >> 47) != 0)
        return -1;

    for (uint64_t va = virtual_address & PAGE_MASK;; va += 0x1000ULL) {
        uint64_t pml4e = space->root[PML4_INDEX(va)];
        if ((pml4e & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
            return -1;
        if (write_access && !(pml4e & PROSENJIT_PAGE_WRITABLE))
            return -1;

        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PAGE_MASK);
        uint64_t pdpte = pdpt[PDPT_INDEX(va)];
        if ((pdpte & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER) ||
            (pdpte & LARGE_PAGE_FLAG))
            return -1;
        if (write_access && !(pdpte & PROSENJIT_PAGE_WRITABLE))
            return -1;

        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PAGE_MASK);
        uint64_t pde = pd[PD_INDEX(va)];
        if ((pde & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER) ||
            (pde & LARGE_PAGE_FLAG))
            return -1;
        if (write_access && !(pde & PROSENJIT_PAGE_WRITABLE))
            return -1;

        uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PAGE_MASK);
        uint64_t pte = pt[PT_INDEX(va)];
        if ((pte & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
            return -1;
        if (write_access && !(pte & PROSENJIT_PAGE_WRITABLE))
            return -1;

        if (va >= (end & PAGE_MASK)) break;
    }

    return 0;
}


int address_space_selftest(void) {
    struct pros_address_space space = { 0, 0 };
    uint64_t physical = 0;
    uint64_t translated = 0;
    uint64_t flags = 0;

    if (address_space_create(&space) != 0)
        return -1;

    physical = physical_page_alloc_user();
    if (!physical) {
        address_space_destroy(&space);
        return -2;
    }

    if (address_space_map_user_page_owned(&space, PROSENJIT_USER_TEST_VA,
                                          physical,
                                          PROSENJIT_PAGE_WRITABLE |
                                          PROSENJIT_PAGE_NX) != 0) {
        physical_page_free(physical);
        address_space_destroy(&space);
        return -3;
    }

    if (address_space_translate_user(&space, PROSENJIT_USER_TEST_VA,
                                     &translated, &flags) != 0 ||
        translated != physical ||
        !(flags & PROSENJIT_PAGE_WRITABLE) ||
        !(flags & PROSENJIT_PAGE_NX) ||
        address_space_validate_user_range(&space, PROSENJIT_USER_TEST_VA,
                                          PROSENJIT_PAGE_SIZE, 1) != 0) {
        address_space_destroy(&space);
        return -4;
    }

    if (address_space_map_user_page(&space, PROSENJIT_USER_TEST_VA,
                                    physical, PROSENJIT_PAGE_NX) == 0 ||
        address_space_map_user_page_owned(&space, PROSENJIT_USER_TEST_VA + 0x1000ULL,
                                          physical, PROSENJIT_PAGE_WRITABLE |
                                          PROSENJIT_PAGE_NX) == 0 ||
        address_space_validate_user_range(&space, PROSENJIT_USER_TEST_VA,
                                          PROSENJIT_PAGE_SIZE, 0) != 0) {
        address_space_destroy(&space);
        return -5;
    }

    if (address_space_unmap_user_page(&space, PROSENJIT_USER_TEST_VA,
                                      &translated) != 0 ||
        translated != physical ||
        address_space_translate_user(&space, PROSENJIT_USER_TEST_VA,
                                     &translated, &flags) == 0) {
        physical_page_free(physical);
        address_space_destroy(&space);
        return -6;
    }

    physical_page_free(physical);

    /* Generic kernel pages must never be accepted by the user mapper. */
    uint64_t kernel_physical = physical_page_alloc();
    if (!kernel_physical ||
        physical_page_is_user_mappable(kernel_physical) ||
        address_space_map_user_page(&space, PROSENJIT_USER_TEST_VA,
                                    kernel_physical, PROSENJIT_PAGE_NX) == 0) {
        if (kernel_physical) physical_page_free(kernel_physical);
        address_space_destroy(&space);
        return -7;
    }
    physical_page_free(kernel_physical);

    /* A read-only ancestor must not permit or report a writable leaf. */
    uint64_t second_physical = physical_page_alloc_user();
    if (!second_physical)
        return -7;
    if (address_space_map_user_page(&space,
                                    PROSENJIT_USER_TEST_VA,
                                    second_physical,
                                    PROSENJIT_PAGE_WRITABLE |
                                    PROSENJIT_PAGE_NX) != 0) {
        physical_page_free(second_physical);
        address_space_destroy(&space);
        return -7;
    }
    uint32_t test_pml4i = PML4_INDEX(PROSENJIT_USER_TEST_VA);
    space.root[test_pml4i] &= ~PROSENJIT_PAGE_WRITABLE;
    if (address_space_map_user_page(&space,
                                    PROSENJIT_USER_TEST_VA + 0x1000ULL,
                                    second_physical,
                                    PROSENJIT_PAGE_WRITABLE |
                                    PROSENJIT_PAGE_NX) == 0 ||
        address_space_validate_user_range(&space,
                                          PROSENJIT_USER_TEST_VA,
                                          PROSENJIT_PAGE_SIZE,
                                          1) == 0 ||
        address_space_unmap_user_page(&space,
                                     PROSENJIT_USER_TEST_VA,
                                     0) != 0) {
        physical_page_free(second_physical);
        address_space_destroy(&space);
        return -7;
    }
    physical_page_free(second_physical);

    if (address_space_validate_user_range(&space,
                                           PROSENJIT_USER_TEST_VA,
                                           0, 0) == 0 ||
        address_space_validate_user_range(&space,
                                           0x00007FFFFFFFF000ULL,
                                           0x2000ULL, 0) == 0 ||
        address_space_map_user_page(&space,
                                    0x0000800000000000ULL,
                                    physical,
                                    PROSENJIT_PAGE_NX) == 0 ||
        address_space_map_user_page(&space,
                                    PROSENJIT_USER_TEST_VA,
                                    PROSENJIT_PHYS_LIMIT,
                                    PROSENJIT_PAGE_NX) == 0) {
        address_space_destroy(&space);
        return -7;
    }

    /* A stale ownership record must fail preflight without freeing anything. */
    uint64_t owned_probe_physical = physical_page_alloc_user();
    if (!owned_probe_physical ||
        address_space_map_user_page_owned(&space,
                                          PROSENJIT_USER_TEST_VA + 0x2000ULL,
                                          owned_probe_physical,
                                          PROSENJIT_PAGE_WRITABLE |
                                          PROSENJIT_PAGE_NX) != 0) {
        if (owned_probe_physical) physical_page_free(owned_probe_physical);
        address_space_destroy(&space);
        return -8;
    }
    struct pros_owned_page *owned_probe = space.owned_pages;
    uint64_t saved_owned_physical = owned_probe->physical_address;
    owned_probe->physical_address = saved_owned_physical + PROSENJIT_PAGE_SIZE;
    int corrupt_preflight_rc = address_space_destroy_preflight(&space);
    owned_probe->physical_address = saved_owned_physical;
    if (corrupt_preflight_rc == 0 || space.root == 0 ||
        space.owned_pages != owned_probe ||
        !physical_page_is_allocated(saved_owned_physical))
        return -8;

    /*
     * A mismatched ownership record must not be discarded by unmap: doing so
     * would orphan its real page while returning the unrelated mapped page.
     */
    /* Re-introduce the mismatch after the destroy-preflight probe restored it. */
    owned_probe->physical_address = saved_owned_physical + PROSENJIT_PAGE_SIZE;
    uint64_t unmap_probe_physical = 0xBAD0BAD0BAD0BAD0ULL;
    int mismatched_unmap_rc = address_space_unmap_user_page(
        &space, owned_probe->virtual_address, &unmap_probe_physical);
    /*
     * Check rejection before touching owned_probe again: a broken unmap
     * implementation may already have freed that record, which would make
     * restoring the corrupted field itself a use-after-free in this test.
     */
    if (mismatched_unmap_rc == 0 || space.owned_pages != owned_probe ||
        unmap_probe_physical != 0xBAD0BAD0BAD0BAD0ULL)
        return -8;
    owned_probe->physical_address = saved_owned_physical;
    uint64_t still_mapped_physical = 0;
    if (address_space_translate_user(&space, owned_probe->virtual_address,
                                     &still_mapped_physical, 0) != 0 ||
        still_mapped_physical != saved_owned_physical ||
        !physical_page_is_allocated(saved_owned_physical))
        return -8;

    /* Duplicate ownership metadata must not permit a double physical free. */
    struct pros_owned_page *duplicate_owned =
        (struct pros_owned_page *)kmalloc(sizeof(*duplicate_owned));
    if (!duplicate_owned) return -8;
    duplicate_owned->virtual_address = owned_probe->virtual_address;
    duplicate_owned->physical_address = owned_probe->physical_address;
    duplicate_owned->next = space.owned_pages;
    space.owned_pages = duplicate_owned;
    int duplicate_preflight_rc = address_space_destroy_preflight(&space);
    space.owned_pages = duplicate_owned->next;
    kfree(duplicate_owned);
    if (duplicate_preflight_rc == 0 || space.root == 0 ||
        space.owned_pages != owned_probe ||
        !physical_page_is_allocated(saved_owned_physical))
        return -8;

    /* A shared page-table subtree across different PDPT branches is invalid. */
    uint64_t alias_physical = physical_page_alloc_user();
    uint64_t alias_va = PROSENJIT_USER_TEST_VA + 0x40000000ULL;
    if (!alias_physical ||
        address_space_map_user_page(&space, alias_va, alias_physical,
                                    PROSENJIT_PAGE_WRITABLE |
                                    PROSENJIT_PAGE_NX) != 0) {
        if (alias_physical) physical_page_free(alias_physical);
        return -8;
    }
    uint64_t root_entry = space.root[PML4_INDEX(alias_va)];
    uint64_t *alias_pdpt = (uint64_t *)(uintptr_t)(root_entry & PAGE_MASK);
    uint64_t first_pdpte = alias_pdpt[PDPT_INDEX(PROSENJIT_USER_TEST_VA)];
    uint64_t second_pdpte = alias_pdpt[PDPT_INDEX(alias_va)];
    uint64_t *first_pd = (uint64_t *)(uintptr_t)(first_pdpte & PAGE_MASK);
    uint64_t *second_pd = (uint64_t *)(uintptr_t)(second_pdpte & PAGE_MASK);
    uint32_t alias_pdi = PD_INDEX(alias_va);
    uint64_t saved_second_pde = second_pd[alias_pdi];
    uint64_t *alias_pt = (uint64_t *)(uintptr_t)(saved_second_pde & PAGE_MASK);
    uint32_t alias_pti = PT_INDEX(alias_va);
    uint64_t saved_alias_pte = alias_pt[alias_pti];
    uint64_t kernel_alias_physical = physical_page_alloc();
    if (!kernel_alias_physical ||
        physical_page_is_user_mappable(kernel_alias_physical)) {
        if (kernel_alias_physical) physical_page_free(kernel_alias_physical);
        return -8;
    }
    alias_pt[alias_pti] = (kernel_alias_physical & PAGE_MASK) |
                          PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER |
                          PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX;
    int kernel_alias_rc = address_space_destroy_preflight(&space);
    alias_pt[alias_pti] = saved_alias_pte;
    physical_page_free(kernel_alias_physical);
    if (kernel_alias_rc == 0 || space.root == 0 ||
        !physical_page_is_allocated(alias_physical))
        return -8;
    second_pd[alias_pdi] = first_pd[PD_INDEX(PROSENJIT_USER_TEST_VA)];
    int shared_table_rc = address_space_destroy_preflight(&space);
    second_pd[alias_pdi] = saved_second_pde;
    uint64_t unmapped_alias = 0;
    int unmap_alias_rc = address_space_unmap_user_page(&space, alias_va,
                                                       &unmapped_alias);
    if (unmap_alias_rc != 0 || unmapped_alias != alias_physical)
        return -8;
    physical_page_free(unmapped_alias);
    if (shared_table_rc == 0 || space.root == 0 ||
        !physical_page_is_allocated(saved_owned_physical))
        return -8;

    /* The active CR3 must be rejected while leaving the address space intact. */
    int switch_rc = address_space_switch(&space);
    int active_preflight_rc = address_space_destroy_preflight(&space);
    int restore_rc = paging_switch_kernel();
    if (switch_rc != 0 || active_preflight_rc != -2 || restore_rc != 0 ||
        address_space_destroy_preflight(&space) != 0)
        return -8;

    if (address_space_destroy(&space) != 0 || space.root != 0 ||
        space.owned_pages != 0)
        return -9;

    return 0;
}

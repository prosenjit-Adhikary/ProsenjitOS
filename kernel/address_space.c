#include "address_space.h"
#include "memory.h"
#include "paging.h"
#include "heap.h"

struct pros_owned_page {
    uint64_t virtual_address;
    uint64_t physical_address;
    struct pros_owned_page *next;
};

#define PAGE_MASK 0x000FFFFFFFFFF000ULL
#define ENTRIES 512u
#define PT_INDEX(va) (((va) >> 12) & 0x1FFu)
#define PD_INDEX(va) (((va) >> 21) & 0x1FFu)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FFu)
#define PML4_INDEX(va) (((va) >> 39) & 0x1FFu)
#define LARGE_PAGE_FLAG (1ULL << 7)
#define USER_MIN_VA PROSENJIT_USER_CODE_VA

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
    if (!space || !space->root || !user_va_valid(virtual_address)) return -1;
    if ((physical_address & 0xFFFULL) ||
        (physical_address & ~PAGE_MASK) != 0 ||
        physical_address == 0 ||
        physical_address >= PROSENJIT_PHYS_LIMIT ||
        !physical_page_is_user_mappable(physical_address) ||
        (flags & ~(PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX)) != 0 ||
        ((flags & PROSENJIT_PAGE_WRITABLE) && !(flags & PROSENJIT_PAGE_NX)))
        return -1;

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
        return -1;
    uint64_t *pdpt = ensure_table(root, pml4i);
    if (!pdpt) return -1;
    int created_pdpt = !(old_pml4e & PROSENJIT_PAGE_PRESENT);

    uint64_t old_pdpte = pdpt[pdpti];
    if ((old_pdpte & PROSENJIT_PAGE_PRESENT) &&
        (!(old_pdpte & PROSENJIT_PAGE_USER) ||
         (old_pdpte & LARGE_PAGE_FLAG) ||
         ((flags & PROSENJIT_PAGE_WRITABLE) &&
          !(old_pdpte & PROSENJIT_PAGE_WRITABLE))))
        return -1;
    uint64_t *pd = ensure_table(pdpt, pdpti);
    if (!pd) {
        if (created_pdpt) {
            physical_page_free((uint64_t)(uintptr_t)pdpt);
            root[pml4i] = 0;
        }
        return -1;
    }
    int created_pd = !(old_pdpte & PROSENJIT_PAGE_PRESENT);

    uint64_t old_pde = pd[pdi];
    if ((old_pde & PROSENJIT_PAGE_PRESENT) &&
        (!(old_pde & PROSENJIT_PAGE_USER) ||
         (old_pde & LARGE_PAGE_FLAG) ||
         ((flags & PROSENJIT_PAGE_WRITABLE) &&
          !(old_pde & PROSENJIT_PAGE_WRITABLE))))
        return -1;
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
    if (!space || !space->root || !user_va_valid(virtual_address) ||
        (physical_address & 0xFFFULL) ||
        (physical_address & ~PAGE_MASK) != 0 ||
        physical_address == 0 ||
        physical_address >= PROSENJIT_PHYS_LIMIT ||
        (flags & ~(PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX)) != 0 ||
        ((flags & PROSENJIT_PAGE_WRITABLE) && !(flags & PROSENJIT_PAGE_NX)))
        return -1;
    if (physical_already_mapped(space, physical_address))
        return -1;
    struct pros_owned_page *owned = (struct pros_owned_page *)kmalloc(sizeof(*owned));
    if (!owned)
        return -1;
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

/*
 * Teardown uses fixed physical-page bitmaps so the complete release set can
 * be validated before the first handle, mapping, or allocator mutation.
 * The mapped bitmap also rejects leaf aliases and table/leaf overlap.
 */
static uint64_t address_space_destroy_user_bits[PROSENJIT_PHYS_BITMAP_WORDS];
static uint64_t address_space_destroy_table_bits[PROSENJIT_PHYS_BITMAP_WORDS];
static uint64_t address_space_destroy_mapped_bits[PROSENJIT_PHYS_BITMAP_WORDS];
static uint64_t address_space_destroy_other_table_bits[PROSENJIT_PHYS_BITMAP_WORDS];
static uint64_t address_space_destroy_other_owned_bits[PROSENJIT_PHYS_BITMAP_WORDS];

static int address_space_destroy_mark(uint64_t *bits, uint64_t physical) {
    if (!bits || physical == 0u ||
        (physical & (PROSENJIT_PAGE_SIZE - 1u)) != 0u ||
        physical >= PROSENJIT_PHYS_LIMIT)
        return -1;
    uint64_t page = physical / PROSENJIT_PAGE_SIZE;
    uint64_t mask = 1ULL << (page & 63u);
    uint64_t *word = &bits[page >> 6];
    if (*word & mask)
        return -1;
    *word |= mask;
    return 0;
}

static int address_space_destroy_collect_table(uint64_t physical,
                                               uint32_t level) {
    if (level == 0u || !physical_page_is_page_table(physical) ||
        address_space_destroy_mark(address_space_destroy_table_bits,
                                   physical) != 0)
        return -1;

    uint64_t *table = (uint64_t *)(uintptr_t)physical;
    for (uint32_t i = 0u; i < ENTRIES; ++i) {
        uint64_t entry = table[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT)) {
            if (entry != 0u)
                return -1;
            continue;
        }
        if (level == 1u) {
            uint64_t leaf = entry & PAGE_MASK;
            if (!(entry & PROSENJIT_PAGE_USER) ||
                !physical_page_is_user_mappable(leaf) ||
                address_space_destroy_mark(
                    address_space_destroy_mapped_bits, leaf) != 0)
                return -1;
            continue;
        }
        if (!(entry & PROSENJIT_PAGE_USER) ||
            (entry & LARGE_PAGE_FLAG) ||
            address_space_destroy_collect_table(entry & PAGE_MASK,
                                                 level - 1u) != 0)
            return -1;
    }
    return 0;
}

static int address_space_destroy_metadata_readable(
    const struct pros_owned_page *owned) {
    if (!owned)
        return 0;
    uint64_t start = (uint64_t)(uintptr_t)owned;
    if (start > UINT64_MAX - (uint64_t)sizeof(*owned) + 1u)
        return 0;
    uint64_t end = start + (uint64_t)sizeof(*owned) - 1u;
    uint64_t page = start & ~(PROSENJIT_PAGE_SIZE - 1u);
    uint64_t last_page = end & ~(PROSENJIT_PAGE_SIZE - 1u);
    for (;;) {
        uint64_t flags = 0u;
        if (paging_query_effective_page(page, &flags) != 0 ||
            !(flags & PROSENJIT_PAGE_PRESENT) ||
            !(flags & PROSENJIT_PAGE_WRITABLE) ||
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

static int address_space_owned_list_acyclic(
    const struct pros_owned_page *head) {
    const struct pros_owned_page *slow = head;
    const struct pros_owned_page *fast = head;
    while (fast) {
        if (!address_space_destroy_metadata_readable(fast))
            return 0;
        fast = fast->next;
        if (!fast)
            break;
        if (!address_space_destroy_metadata_readable(fast))
            return 0;
        fast = fast->next;

        if (!slow || !address_space_destroy_metadata_readable(slow))
            return 0;
        slow = slow->next;
        if (slow && slow == fast)
            return 0;
    }
    return 1;
}

static int address_space_destroy_preflight_unlocked(
    const struct pros_address_space *space) {
    if (!space)
        return -1;
    /* A fully destroyed address space is a valid retry/idempotent state. */
    if (!space->root)
        return space->owned_pages ? -1 : 0;
    if (((uint64_t)(uintptr_t)space->root &
         (PROSENJIT_PAGE_SIZE - 1u)) != 0u)
        return -1;

    uint64_t current_cr3 = 0u;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(current_cr3) : : "memory");
    uint64_t root = (uint64_t)(uintptr_t)space->root;
    if ((current_cr3 & PAGE_MASK) == (root & PAGE_MASK) ||
        !physical_page_is_page_table(root))
        return -1;

    for (uint64_t i = 0u; i < PROSENJIT_PHYS_BITMAP_WORDS; ++i) {
        address_space_destroy_user_bits[i] = 0u;
        address_space_destroy_table_bits[i] = 0u;
        address_space_destroy_mapped_bits[i] = 0u;
    }

    if (address_space_destroy_mark(address_space_destroy_table_bits,
                                   root) != 0)
        return -1;

    /* PML4[0] and PML4[256..511] are shared kernel mappings. */
    for (uint32_t i = 1u; i < 256u; ++i) {
        uint64_t entry = space->root[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT)) {
            if (entry != 0u)
                return -1;
            continue;
        }
        if (!(entry & PROSENJIT_PAGE_USER) ||
            (entry & LARGE_PAGE_FLAG) ||
            address_space_destroy_collect_table(entry & PAGE_MASK, 3u) != 0)
            return -1;
    }

    if (!address_space_owned_list_acyclic(space->owned_pages))
        return -1;

    uint64_t owned_count = 0u;
    for (const struct pros_owned_page *owned = space->owned_pages;
         owned; owned = owned->next) {
        if (owned_count++ >= PROSENJIT_PHYS_PAGE_COUNT ||
            !address_space_destroy_metadata_readable(owned) ||
            !user_va_valid(owned->virtual_address) ||
            (owned->physical_address & (PROSENJIT_PAGE_SIZE - 1u)) != 0u ||
            owned->physical_address >= PROSENJIT_PHYS_LIMIT)
            return -1;

        uint64_t translated = 0u;
        if (address_space_translate_user(space, owned->virtual_address,
                                         &translated, 0) != 0 ||
            translated != owned->physical_address)
            return -1;

        /*
         * kfree() runs after the physical-page commit. Its metadata backing
         * must remain allocated and must not alias any page in this release.
         */
        uint64_t metadata_va =
            (uint64_t)(uintptr_t)owned & ~(PROSENJIT_PAGE_SIZE - 1u);
        uint64_t metadata_pa = paging_translate(metadata_va) & PAGE_MASK;
        if (!metadata_pa || metadata_pa >= PROSENJIT_PHYS_LIMIT ||
            !physical_page_is_allocated(metadata_pa) ||
            physical_page_is_page_table(metadata_pa) ||
            physical_page_is_user_mappable(metadata_pa))
            return -1;
        uint64_t metadata_page = metadata_pa / PROSENJIT_PAGE_SIZE;
        uint64_t metadata_mask = 1ULL << (metadata_page & 63u);
        if ((address_space_destroy_user_bits[metadata_page >> 6] &
             metadata_mask) ||
            (address_space_destroy_table_bits[metadata_page >> 6] &
             metadata_mask) ||
            (address_space_destroy_mapped_bits[metadata_page >> 6] &
             metadata_mask))
            return -1;

        uint64_t page = owned->physical_address / PROSENJIT_PAGE_SIZE;
        uint64_t mask = 1ULL << (page & 63u);
        if (!(address_space_destroy_mapped_bits[page >> 6] & mask) ||
            (address_space_destroy_table_bits[page >> 6] & mask) ||
            address_space_destroy_mark(address_space_destroy_user_bits,
                                       owned->physical_address) != 0)
            return -1;
    }

    for (uint64_t i = 0u; i < PROSENJIT_PHYS_BITMAP_WORDS; ++i) {
        /*
         * Every user leaf must have exactly one ownership record, and every
         * ownership record must resolve to exactly one user leaf. Reject
         * both untracked mappings and stale ownership-list entries.
         */
        if (address_space_destroy_user_bits[i] !=
                address_space_destroy_mapped_bits[i] ||
            (address_space_destroy_table_bits[i] &
             address_space_destroy_mapped_bits[i]) != 0u)
            return -1;
    }

    if (paging_kernel_table_bitmap_disjoint(
            address_space_destroy_table_bits) != 0)
        return -1;

    return physical_page_free_mixed_bitmaps_preflight(
        address_space_destroy_user_bits,
        address_space_destroy_table_bits);
}

int address_space_destroy_preflight(const struct pros_address_space *space) {
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    int rc = address_space_destroy_preflight_unlocked(space);
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return rc;
}

static int address_space_destroy_target_releases(uint64_t physical) {
    if (!physical || (physical & (PROSENJIT_PAGE_SIZE - 1u)) != 0u ||
        physical >= PROSENJIT_PHYS_LIMIT)
        return 1;
    uint64_t page = physical / PROSENJIT_PAGE_SIZE;
    uint64_t mask = 1ULL << (page & 63u);
    return ((address_space_destroy_user_bits[page >> 6] |
             address_space_destroy_table_bits[page >> 6]) & mask) != 0u;
}

static int address_space_destroy_check_other_table(uint64_t physical,
                                                   uint32_t level) {
    if (level == 0u || !physical_page_is_page_table(physical) ||
        address_space_destroy_target_releases(physical) ||
        address_space_destroy_mark(address_space_destroy_other_table_bits,
                                   physical) != 0)
        return -1;

    uint64_t *table = (uint64_t *)(uintptr_t)physical;
    for (uint32_t i = 0u; i < ENTRIES; ++i) {
        uint64_t entry = table[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT)) {
            if (entry != 0u)
                return -1;
            continue;
        }
        if (level == 1u) {
            uint64_t leaf = entry & PAGE_MASK;
            if (!(entry & PROSENJIT_PAGE_USER) ||
                !physical_page_is_user_mappable(leaf) ||
                address_space_destroy_target_releases(leaf) ||
                address_space_destroy_mark(
                    address_space_destroy_mapped_bits, leaf) != 0)
                return -1;
            continue;
        }
        if (!(entry & PROSENJIT_PAGE_USER) ||
            (entry & LARGE_PAGE_FLAG) ||
            address_space_destroy_check_other_table(entry & PAGE_MASK,
                                                     level - 1u) != 0)
            return -1;
    }
    return 0;
}

static int address_space_destroy_check_other_space(
    const struct pros_address_space *other) {
    if (!other)
        return -1;
    if (!other->root)
        return other->owned_pages ? -1 : 0;
    uint64_t root = (uint64_t)(uintptr_t)other->root;
    if (!physical_page_is_page_table(root) ||
        address_space_destroy_target_releases(root) ||
        !address_space_owned_list_acyclic(other->owned_pages))
        return -1;

    for (uint64_t i = 0u; i < PROSENJIT_PHYS_BITMAP_WORDS; ++i) {
        address_space_destroy_mapped_bits[i] = 0u;
        address_space_destroy_other_owned_bits[i] = 0u;
        address_space_destroy_other_table_bits[i] = 0u;
    }

    uint64_t owned_count = 0u;
    for (const struct pros_owned_page *owned = other->owned_pages;
         owned; owned = owned->next) {
        if (owned_count++ >= PROSENJIT_PHYS_PAGE_COUNT ||
            !address_space_destroy_metadata_readable(owned) ||
            !user_va_valid(owned->virtual_address) ||
            (owned->physical_address & (PROSENJIT_PAGE_SIZE - 1u)) != 0u ||
            !physical_page_is_user_mappable(owned->physical_address) ||
            address_space_destroy_target_releases(owned->physical_address) ||
            address_space_destroy_mark(address_space_destroy_other_owned_bits,
                                       owned->physical_address) != 0)
            return -1;

        uint64_t metadata_va =
            (uint64_t)(uintptr_t)owned & ~(PROSENJIT_PAGE_SIZE - 1u);
        uint64_t metadata_pa = paging_translate(metadata_va) & PAGE_MASK;
        if (!metadata_pa || metadata_pa >= PROSENJIT_PHYS_LIMIT ||
            !physical_page_is_allocated(metadata_pa) ||
            physical_page_is_page_table(metadata_pa) ||
            physical_page_is_user_mappable(metadata_pa) ||
            address_space_destroy_target_releases(metadata_pa))
            return -1;

        uint64_t translated = 0u;
        if (address_space_translate_user(other, owned->virtual_address,
                                         &translated, 0) != 0 ||
            translated != owned->physical_address)
            return -1;
    }

    if (address_space_destroy_mark(address_space_destroy_other_table_bits,
                                   root) != 0)
        return -1;

    /* PML4[0] and PML4[256..511] are shared kernel mappings. */
    for (uint32_t i = 1u; i < 256u; ++i) {
        uint64_t entry = other->root[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT)) {
            if (entry != 0u)
                return -1;
            continue;
        }
        if (!(entry & PROSENJIT_PAGE_USER) ||
            (entry & LARGE_PAGE_FLAG) ||
            address_space_destroy_check_other_table(entry & PAGE_MASK,
                                                     3u) != 0)
            return -1;
    }

    for (uint64_t i = 0u; i < PROSENJIT_PHYS_BITMAP_WORDS; ++i) {
        if ((address_space_destroy_other_table_bits[i] &
             address_space_destroy_mapped_bits[i]) != 0u ||
            (address_space_destroy_other_owned_bits[i] &
             ~address_space_destroy_mapped_bits[i]) != 0u ||
            (address_space_destroy_other_owned_bits[i] &
             address_space_destroy_other_table_bits[i]) != 0u)
            return -1;
    }
    return paging_kernel_table_bitmap_disjoint(
        address_space_destroy_other_table_bits);
}

/*
 * Validate the target release set against all other live address spaces.
 * This prevents freeing a page-table or owned data page still reachable
 * through a sibling process, even when the target's own tree is consistent.
 */
int address_space_destroy_preflight_against(
    const struct pros_address_space *space,
    const struct pros_address_space *const *others,
    uint32_t other_count) {
    if (other_count > 64u || (other_count != 0u && !others))
        return -1;
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    int rc = address_space_destroy_preflight_unlocked(space);
    if (rc == 0 && !space->root && !space->owned_pages) {
        __asm__ volatile ("pushq %0; popfq"
                          : : "r"(irq_flags) : "memory", "cc");
        return 0;
    }
    for (uint32_t i = 0u; rc == 0 && i < other_count; ++i) {
        if (others[i] == space ||
            address_space_destroy_check_other_space(others[i]) != 0)
            rc = -1;
    }
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
    if (physical_address_out) *physical_address_out = physical;
    struct pros_owned_page **link = &space->owned_pages;
    while (*link) {
        if ((*link)->virtual_address == virtual_address) {
            struct pros_owned_page *owned = *link;
            *link = owned->next;
            kfree(owned);
            break;
        }
        link = &(*link)->next;
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

    if (address_space_destroy_preflight_unlocked(space) != 0) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }
    if (!space->root && !space->owned_pages) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return 0;
    }

    /*
     * The allocator revalidates and releases the complete owned-user/table
     * page sets atomically. No page-table walk or fallible page free remains
     * after this commit point.
     */
    struct pros_owned_page *owned = space->owned_pages;
    if (physical_page_free_mixed_bitmaps(address_space_destroy_user_bits,
                                         address_space_destroy_table_bits) != 0) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }

    space->root = 0;
    space->owned_pages = 0;
    while (owned) {
        struct pros_owned_page *next = owned->next;
        kfree(owned);
        owned = next;
    }

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

    /*
     * A user leaf without a matching owned-page record must reject the
     * complete teardown preflight without changing the mapping or allocator.
     * Removing the untracked leaf repairs the state and permits a retry.
     */
    uint64_t untracked_physical = physical_page_alloc_user();
    if (!untracked_physical ||
        address_space_map_user_page(&space, PROSENJIT_USER_TEST_VA,
                                    untracked_physical,
                                    PROSENJIT_PAGE_WRITABLE |
                                    PROSENJIT_PAGE_NX) != 0) {
        if (untracked_physical)
            physical_page_free(untracked_physical);
        address_space_destroy(&space);
        return -10;
    }
    uint64_t untracked_free_before = physical_memory_free_pages();
    uint64_t untracked_root_before = (uint64_t)(uintptr_t)space.root;
    if (address_space_destroy_preflight(&space) == 0 ||
        (uint64_t)(uintptr_t)space.root != untracked_root_before ||
        paging_translate(PROSENJIT_USER_TEST_VA) != untracked_physical ||
        !physical_page_is_allocated(untracked_physical) ||
        physical_memory_free_pages() != untracked_free_before) {
        (void)address_space_unmap_user_page(&space,
                                            PROSENJIT_USER_TEST_VA, 0);
        if (physical_page_is_allocated(untracked_physical))
            physical_page_free(untracked_physical);
        (void)address_space_destroy(&space);
        return -11;
    }
    if (address_space_unmap_user_page(&space,
                                      PROSENJIT_USER_TEST_VA, 0) != 0 ||
        paging_translate(PROSENJIT_USER_TEST_VA) != 0u ||
        !physical_page_is_allocated(untracked_physical)) {
        if (physical_page_is_allocated(untracked_physical))
            physical_page_free(untracked_physical);
        (void)address_space_destroy(&space);
        return -12;
    }
    physical_page_free(untracked_physical);
    if (physical_page_is_allocated(untracked_physical) ||
        address_space_destroy_preflight(&space) != 0)
        return -13;

    if (address_space_destroy(&space) != 0 || space.root != 0 ||
        space.owned_pages != 0 ||
        address_space_destroy_preflight(&space) != 0 ||
        address_space_destroy(&space) != 0)
        return -9;

    return 0;
}

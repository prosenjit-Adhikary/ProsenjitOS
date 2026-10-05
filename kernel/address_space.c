#include "address_space.h"
#include "memory.h"
#include "paging.h"

#define PAGE_MASK 0x000FFFFFFFFFF000ULL
#define ENTRIES 512u
#define PT_INDEX(va) (((va) >> 12) & 0x1FFu)
#define PD_INDEX(va) (((va) >> 21) & 0x1FFu)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FFu)
#define PML4_INDEX(va) (((va) >> 39) & 0x1FFu)
#define LARGE_PAGE_FLAG (1ULL << 7)
#define PAGE_OWNED_FLAG (1ULL << 9)

extern uint64_t *paging_kernel_root(void);

static void zero_page(uint64_t *page) {
    for (uint32_t i = 0; i < ENTRIES; ++i) page[i] = 0;
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
     * Share only supervisor kernel mappings. User processes get a private
     * PML4[1..255] for their own pages; the shared low and high kernel
     * mappings must never become user-accessible through inherited PML4 bits.
     */
    root[0] = kernel_root[0] & ~PROSENJIT_PAGE_USER;
    for (uint32_t i = 256; i < ENTRIES; ++i)
        root[i] = kernel_root[i] & ~PROSENJIT_PAGE_USER;

    space->root = root;
    return 0;
}

static int address_space_map_user_page_internal(struct pros_address_space *space,
                                uint64_t virtual_address,
                                uint64_t physical_address,
                                uint64_t flags,
                                int owned) {
    if (!space || !space->root) return -1;
    if ((virtual_address >> 47) != 0 || virtual_address < PROSENJIT_USER_CODE_VA)
        return -1;
    if ((virtual_address & 0xFFFULL) || (physical_address & 0xFFFULL))
        return -1;
    if (physical_address >= PROSENJIT_PHYS_LIMIT)
        return -1;

    /*
     * PML4[0] is supervisor-only kernel space in every user address space.
     * User mappings therefore must live in PML4[1..255] and can never alias
     * the kernel's low identity map.
     */
    uint32_t pml4_index = PML4_INDEX(virtual_address);
    if (pml4_index == 0 || pml4_index >= 256u)
        return -1;

    uint64_t *pdpt = ensure_table(space->root, pml4_index);
    if (!pdpt) return -1;
    uint64_t *pd = ensure_table(pdpt, PDPT_INDEX(virtual_address));
    if (!pd) return -1;
    uint64_t *pt = ensure_table(pd, PD_INDEX(virtual_address));
    if (!pt) return -1;

    uint64_t *pte = &pt[PT_INDEX(virtual_address)];
    if (*pte & PROSENJIT_PAGE_PRESENT)
        return -1; /* never silently replace an existing user mapping */

    *pte = (physical_address & PAGE_MASK) |
           PROSENJIT_PAGE_PRESENT |
           PROSENJIT_PAGE_USER |
           (flags & (PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX));
    if (owned) *pte |= PAGE_OWNED_FLAG;
    return 0;
}

int address_space_map_user_page(struct pros_address_space *space,
                                uint64_t virtual_address,
                                uint64_t physical_address,
                                uint64_t flags) {
    return address_space_map_user_page_internal(space, virtual_address,
                                                physical_address, flags, 0);
}

int address_space_map_user_page_owned(struct pros_address_space *space,
                                      uint64_t virtual_address,
                                      uint64_t physical_address,
                                      uint64_t flags) {
    return address_space_map_user_page_internal(space, virtual_address,
                                                physical_address, flags, 1);
}

static int table_empty(const uint64_t *table) {
    for (uint32_t i = 0; i < ENTRIES; ++i)
        if (table[i] & PROSENJIT_PAGE_PRESENT) return 0;
    return 1;
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
        /* At level 1, only mappings explicitly marked as owned by this
         * address space may be reclaimed. Kernel-backed/user-test mappings
         * remain caller-owned and must never be freed here. */
        if (level == 1 && (entry & PAGE_OWNED_FLAG))
            physical_page_free(entry & PAGE_MASK);
    }
}

int address_space_unmap_user_page(struct pros_address_space *space,
                                  uint64_t virtual_address,
                                  uint64_t *physical_out) {
    if (!space || !space->root || (virtual_address & 0xFFFULL) ||
        (virtual_address >> 47) != 0 || virtual_address < PROSENJIT_USER_CODE_VA)
        return -1;
    uint64_t *pml4 = space->root;
    uint64_t e1 = pml4[PML4_INDEX(virtual_address)];
    if ((e1 & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(e1 & PAGE_MASK);
    uint64_t e2 = pdpt[PDPT_INDEX(virtual_address)];
    if ((e2 & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) return -1;
    uint64_t *pd = (uint64_t *)(uintptr_t)(e2 & PAGE_MASK);
    uint64_t e3 = pd[PD_INDEX(virtual_address)];
    if ((e3 & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) return -1;
    uint64_t *pt = (uint64_t *)(uintptr_t)(e3 & PAGE_MASK);
    uint64_t *pte = &pt[PT_INDEX(virtual_address)];
    if ((*pte & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) return -1;
    if (physical_out) *physical_out = *pte & PAGE_MASK;
    *pte = 0;

    /*
     * If this address space is active, invalidate the removed user mapping
     * before returning to user mode. Without this, a stale TLB entry could
     * keep the unmapped page accessible until CR3 is switched.
     */
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");

    /*
     * Rollback/unmap must not leak page-table pages. User PML4 entries are
     * private to this address space, so empty descendants can be reclaimed
     * immediately without touching shared kernel mappings.
     */
    if (table_empty(pt)) {
        pd[PD_INDEX(virtual_address)] = 0;
        physical_page_free((uint64_t)(uintptr_t)pt);
        if (table_empty(pd)) {
            pdpt[PDPT_INDEX(virtual_address)] = 0;
            physical_page_free((uint64_t)(uintptr_t)pd);
            if (table_empty(pdpt)) {
                pml4[PML4_INDEX(virtual_address)] = 0;
                physical_page_free((uint64_t)(uintptr_t)pdpt);
            }
        }
    }
    return 0;
}

int address_space_destroy(struct pros_address_space *space) {
    if (!space || !space->root) return -1;

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

    uint64_t root = (uint64_t)(uintptr_t)space->root;
    space->root = 0;
    physical_page_free(root);
    return 0;
}

int address_space_switch(struct pros_address_space *space) {
    if (!space || !space->root) return -1;
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)(uintptr_t)space->root) : "memory");
    return 0;
}

int address_space_validate_user_range(const struct pros_address_space *space,
                                      uint64_t virtual_address,
                                      uint64_t size,
                                      int write_access) {
    if (!space || !space->root || size == 0) return -1;
    if (virtual_address < PROSENJIT_USER_CODE_VA)
        return -1;

    uint64_t end = virtual_address + size - 1ULL;
    if (end < virtual_address || (virtual_address >> 47) != 0 ||
        (end >> 47) != 0 || end >= 0x0000800000000000ULL)
        return -1;

    for (uint64_t va = virtual_address & PAGE_MASK;; va += 0x1000ULL) {
        uint64_t pml4e = space->root[PML4_INDEX(va)];
        if ((pml4e & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
            return -1;

        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PAGE_MASK);
        uint64_t pdpte = pdpt[PDPT_INDEX(va)];
        if ((pdpte & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER) ||
            (pdpte & LARGE_PAGE_FLAG))
            return -1;

        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PAGE_MASK);
        uint64_t pde = pd[PD_INDEX(va)];
        if ((pde & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER) ||
            (pde & LARGE_PAGE_FLAG))
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

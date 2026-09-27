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
    if (!kernel_root) return -1;

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
    return 0;
}

int address_space_map_user_page(struct pros_address_space *space,
                                uint64_t virtual_address,
                                uint64_t physical_address,
                                uint64_t flags) {
    if (!space || !space->root) return -1;
    if ((virtual_address >> 47) != 0) return -1;
    if ((virtual_address & 0xFFFULL) || (physical_address & 0xFFFULL)) return -1;

    uint64_t *pdpt = ensure_table(space->root, PML4_INDEX(virtual_address));
    if (!pdpt) return -1;
    uint64_t *pd = ensure_table(pdpt, PDPT_INDEX(virtual_address));
    if (!pd) return -1;
    uint64_t *pt = ensure_table(pd, PD_INDEX(virtual_address));
    if (!pt) return -1;

    pt[PT_INDEX(virtual_address)] =
        (physical_address & PAGE_MASK) |
        PROSENJIT_PAGE_PRESENT |
        PROSENJIT_PAGE_USER |
        (flags & (PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX));
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

    uint64_t end = virtual_address + size - 1ULL;
    if (end < virtual_address || (virtual_address >> 47) != 0 ||
        (end >> 47) != 0)
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

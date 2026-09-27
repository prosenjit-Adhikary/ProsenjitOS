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
     * PML4[0] is shared with the kernel's low identity map because the
     * bootstrap kernel currently executes below the first 512 GiB.
     * Keep the USER bit at the top level so CPL3 can reach explicitly
     * user-marked leaf pages. Kernel identity pages remain protected by
     * supervisor-only PTEs; therefore setting USER here does not expose
     * those kernel pages to user mode.
     */ 
    root[0] = kernel_root[0];
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

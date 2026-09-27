#include "paging.h"
#include "memory.h"

#define PAGE_MASK 0x000FFFFFFFFFF000ULL
#define ENTRIES 512u
#define PT_INDEX(va) (((va) >> 12) & 0x1FFu)
#define PD_INDEX(va) (((va) >> 21) & 0x1FFu)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FFu)
#define PML4_INDEX(va) (((va) >> 39) & 0x1FFu)
#define LARGE_PAGE_FLAG (1ULL << 7)

static uint64_t *kernel_pml4;

static void page_zero(uint64_t *page) {
    for (uint32_t i = 0; i < ENTRIES; ++i) page[i] = 0;
}

static uint64_t *alloc_table(void) {
    uint64_t physical = physical_page_alloc();
    if (physical == 0) return 0;
    uint64_t *table = (uint64_t *)physical;
    page_zero(table);
    return table;
}

static uint64_t *ensure_table(uint64_t *parent, uint32_t index, uint64_t flags) {
    uint64_t entry = parent[index];
    if (entry & PROSENJIT_PAGE_PRESENT) return (uint64_t *)(entry & PAGE_MASK);

    uint64_t *table = alloc_table();
    if (!table) return 0;
    parent[index] = ((uint64_t)table & PAGE_MASK) | flags | PROSENJIT_PAGE_PRESENT;
    return table;
}

int paging_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    if ((virtual_address & 0xFFFULL) != 0 || (physical_address & 0xFFFULL) != 0)
        return -1;
    if ((virtual_address >> 48) != 0xFFFF && (virtual_address >> 47) != 0)
        return -1;

    uint64_t table_flags = PROSENJIT_PAGE_WRITABLE;
    if (flags & PROSENJIT_PAGE_USER) table_flags |= PROSENJIT_PAGE_USER;

    uint64_t *pdpt = ensure_table(kernel_pml4, PML4_INDEX(virtual_address), table_flags);
    if (!pdpt) return -1;
    uint64_t *pd = ensure_table(pdpt, PDPT_INDEX(virtual_address), table_flags);
    if (!pd) return -1;
    uint64_t *pt = ensure_table(pd, PD_INDEX(virtual_address), table_flags);
    if (!pt) return -1;

    pt[PT_INDEX(virtual_address)] = (physical_address & PAGE_MASK) |
                                    (flags & (PROSENJIT_PAGE_WRITABLE |
                                              PROSENJIT_PAGE_USER |
                                              PROSENJIT_PAGE_NX)) |
                                    PROSENJIT_PAGE_PRESENT;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return 0;
}

void paging_unmap_page(uint64_t virtual_address) {
    if (!kernel_pml4 || (virtual_address & 0xFFFULL)) return;
    uint64_t *pdpt = (uint64_t *)(kernel_pml4[PML4_INDEX(virtual_address)] & PAGE_MASK);
    if (!(kernel_pml4[PML4_INDEX(virtual_address)] & PROSENJIT_PAGE_PRESENT)) return;
    uint64_t *pd = (uint64_t *)(pdpt[PDPT_INDEX(virtual_address)] & PAGE_MASK);
    if (!(pdpt[PDPT_INDEX(virtual_address)] & PROSENJIT_PAGE_PRESENT)) return;
    if (pd[PD_INDEX(virtual_address)] & LARGE_PAGE_FLAG) return;
    uint64_t *pt = (uint64_t *)(pd[PD_INDEX(virtual_address)] & PAGE_MASK);
    if (!(pd[PD_INDEX(virtual_address)] & PROSENJIT_PAGE_PRESENT)) return;
    pt[PT_INDEX(virtual_address)] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

uint64_t paging_translate(uint64_t virtual_address) {
    if (!kernel_pml4) return 0;
    uint64_t entry = kernel_pml4[PML4_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)(entry & PAGE_MASK);
    entry = pdpt[PDPT_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)(entry & PAGE_MASK);
    entry = pd[PD_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT)) return 0;
    if (entry & LARGE_PAGE_FLAG)
        return (entry & 0x000FFFFFFFE00000ULL) | (virtual_address & 0x1FFFFFULL);
    uint64_t *pt = (uint64_t *)(entry & PAGE_MASK);
    entry = pt[PT_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT)) return 0;
    return (entry & PAGE_MASK) | (virtual_address & 0xFFFULL);
}

int paging_init(void) {
    kernel_pml4 = alloc_table();
    if (!kernel_pml4) return -1;

    uint64_t *pdpt = alloc_table();
    uint64_t *pd = alloc_table();
    if (!pdpt || !pd) return -1;

    kernel_pml4[0] = ((uint64_t)pdpt & PAGE_MASK) | PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_WRITABLE;
    pdpt[0] = ((uint64_t)pd & PAGE_MASK) | PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_WRITABLE;

    for (uint32_t i = 0; i < ENTRIES; ++i)
        pd[i] = ((uint64_t)i << 21) | PROSENJIT_PAGE_PRESENT |
                PROSENJIT_PAGE_WRITABLE | LARGE_PAGE_FLAG;

    uint64_t test_page = physical_page_alloc();
    if (!test_page) return -1;
    if (paging_map_page(PROSENJIT_KERNEL_TEST_VA, test_page,
                        PROSENJIT_PAGE_WRITABLE)) return -1;

    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)kernel_pml4) : "memory");
    return 0;
}

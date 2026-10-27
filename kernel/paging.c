#include "paging.h"
#include "memory.h"

extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];
extern char __kernel_bss_start[];
extern char __kernel_bss_end[];
extern char stack_guard_bottom[];
extern char stack_bottom[];
extern char stack_top[];
extern char ist1_stack_guard_bottom[];
extern char ist1_stack_bottom[];
extern char ist1_stack_top[];
extern char ist2_stack_guard_bottom[];
extern char ist2_stack_bottom[];
extern char ist2_stack_top[];
extern char ist3_stack_guard_bottom[];
extern char ist3_stack_bottom[];
extern char ist3_stack_top[];

#define PAGE_MASK 0x000FFFFFFFFFF000ULL
#define ENTRIES 512u
#define PT_INDEX(va) (((va) >> 12) & 0x1FFu)
#define PD_INDEX(va) (((va) >> 21) & 0x1FFu)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FFu)
#define PML4_INDEX(va) (((va) >> 39) & 0x1FFu)
#define LARGE_PAGE_FLAG (1ULL << 7)

static uint64_t *kernel_pml4;

/*
 * Deterministic fault injection is used only by paging_selftest(). A value
 * of N permits N successful table allocations, then fails the next one.
 * -1 disables injection. This is not exposed as a runtime kernel interface.
 */
static int paging_test_alloc_fail_after = -1;

static int canonical_address(uint64_t address) {
    uint64_t upper = address >> 48;
    uint64_t sign = (address >> 47) & 1ULL;
    return sign ? upper == 0xFFFFULL : upper == 0;
}

uint64_t *paging_kernel_root(void) {
    return kernel_pml4;
}

int paging_switch_kernel(void) {
    if (!kernel_pml4) return -1;
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)kernel_pml4) : "memory");
    return 0;
}

static void page_zero(uint64_t *page) {
    for (uint32_t i = 0; i < ENTRIES; ++i) page[i] = 0;
}

static uint64_t *alloc_table(void) {
    if (paging_test_alloc_fail_after == 0) {
        paging_test_alloc_fail_after = -1;
        return 0;
    }
    if (paging_test_alloc_fail_after > 0)
        --paging_test_alloc_fail_after;

    uint64_t physical = physical_page_alloc();
    if (physical == 0) return 0;
    uint64_t *table = (uint64_t *)physical;
    page_zero(table);
    return table;
}

static int page_table_empty(const uint64_t *table) {
    if (!table) return 1;
    for (uint32_t i = 0; i < ENTRIES; ++i)
        if (table[i] & PROSENJIT_PAGE_PRESENT)
            return 0;
    return 1;
}

static uint64_t *ensure_table(uint64_t *parent, uint32_t index, uint64_t flags) {
    uint64_t entry = parent[index];
    if (entry & PROSENJIT_PAGE_PRESENT) {
        if (entry & LARGE_PAGE_FLAG) return 0;
        if ((flags & PROSENJIT_PAGE_USER) && !(entry & PROSENJIT_PAGE_USER)) return 0;
        if ((flags & PROSENJIT_PAGE_WRITABLE) && !(entry & PROSENJIT_PAGE_WRITABLE)) return 0;
        return (uint64_t *)(entry & PAGE_MASK);
    }

    uint64_t *table = alloc_table();
    if (!table) return 0;
    parent[index] = ((uint64_t)table & PAGE_MASK) | flags | PROSENJIT_PAGE_PRESENT;
    return table;
}

int paging_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    if (!kernel_pml4 ||
        (virtual_address & 0xFFFULL) != 0 || (physical_address & 0xFFFULL) != 0)
        return -1;
    if (!canonical_address(virtual_address) ||
        physical_address >= PROSENJIT_PHYS_LIMIT ||
        (flags & ~(PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_USER |
                   PROSENJIT_PAGE_NX)) != 0)
        return -1;

    /*
     * This API mutates the shared kernel root. User mappings belong in a
     * process-specific root through address_space_map_user_page(); admitting
     * USER here could make a low-half mapping visible in the kernel root and
     * weaken the process/kernel address-space boundary. Reject USER for every
     * virtual address, including both lower and upper canonical halves.
     */
    if (flags & PROSENJIT_PAGE_USER)
        return -1;

    uint64_t table_flags = PROSENJIT_PAGE_WRITABLE;
    if (flags & PROSENJIT_PAGE_USER) table_flags |= PROSENJIT_PAGE_USER;

    uint32_t pml4_index = PML4_INDEX(virtual_address);
    uint32_t pdpt_index = PDPT_INDEX(virtual_address);
    uint32_t pd_index = PD_INDEX(virtual_address);
    uint64_t old_pml4e = kernel_pml4[pml4_index];
    uint64_t *pdpt = ensure_table(kernel_pml4, pml4_index, table_flags);
    if (!pdpt) return -1;
    int created_pdpt = !(old_pml4e & PROSENJIT_PAGE_PRESENT);

    uint64_t old_pdpte = pdpt[pdpt_index];
    uint64_t *pd = ensure_table(pdpt, pdpt_index, table_flags);
    if (!pd) {
        if (created_pdpt) {
            physical_page_free((uint64_t)(uintptr_t)pdpt);
            kernel_pml4[pml4_index] = 0;
        }
        return -1;
    }
    int created_pd = !(old_pdpte & PROSENJIT_PAGE_PRESENT);

    uint64_t old_pde = pd[pd_index];
    uint64_t *pt = ensure_table(pd, pd_index, table_flags);
    if (!pt) {
        if (created_pd) {
            physical_page_free((uint64_t)(uintptr_t)pd);
            pdpt[pdpt_index] = 0;
        }
        if (created_pdpt) {
            physical_page_free((uint64_t)(uintptr_t)pdpt);
            kernel_pml4[pml4_index] = 0;
        }
        return -1;
    }
    int created_pt = !(old_pde & PROSENJIT_PAGE_PRESENT);

    if (pt[PT_INDEX(virtual_address)] & PROSENJIT_PAGE_PRESENT) {
        if (created_pt) {
            physical_page_free((uint64_t)(uintptr_t)pt);
            pd[pd_index] = 0;
        }
        if (created_pd) {
            physical_page_free((uint64_t)(uintptr_t)pd);
            pdpt[pdpt_index] = 0;
        }
        if (created_pdpt) {
            physical_page_free((uint64_t)(uintptr_t)pdpt);
            kernel_pml4[pml4_index] = 0;
        }
        return -2;
    }

    pt[PT_INDEX(virtual_address)] = (physical_address & PAGE_MASK) |
                                    (flags & (PROSENJIT_PAGE_WRITABLE |
                                              PROSENJIT_PAGE_USER |
                                              PROSENJIT_PAGE_NX)) |
                                    PROSENJIT_PAGE_PRESENT;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return 0;
}

void paging_unmap_page(uint64_t virtual_address) {
    if (!kernel_pml4 || (virtual_address & 0xFFFULL) ||
        !canonical_address(virtual_address)) return;
    uint64_t pml4e = kernel_pml4[PML4_INDEX(virtual_address)];
    if (!(pml4e & PROSENJIT_PAGE_PRESENT) || (pml4e & LARGE_PAGE_FLAG)) return;
    uint64_t *pdpt = (uint64_t *)(pml4e & PAGE_MASK);
    uint64_t pdpte = pdpt[PDPT_INDEX(virtual_address)];
    if (!(pdpte & PROSENJIT_PAGE_PRESENT) || (pdpte & LARGE_PAGE_FLAG)) return;
    uint64_t *pd = (uint64_t *)(pdpte & PAGE_MASK);
    uint64_t pde = pd[PD_INDEX(virtual_address)];
    if (!(pde & PROSENJIT_PAGE_PRESENT) || (pde & LARGE_PAGE_FLAG)) return;
    uint64_t *pt = (uint64_t *)(pde & PAGE_MASK);
    uint32_t pti = PT_INDEX(virtual_address);
    uint32_t pdi = PD_INDEX(virtual_address);
    uint32_t pdpti = PDPT_INDEX(virtual_address);
    uint32_t pml4i = PML4_INDEX(virtual_address);
    if (!(pt[pti] & PROSENJIT_PAGE_PRESENT)) return;
    pt[pti] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");

    /*
     * Reclaim only completely empty page-table pages owned by this root.
     * Existing identity/kernel mappings keep their parent tables alive.
     * The kernel currently runs without SMP page-table sharing; once secondary
     * CPUs can execute, this path must be paired with remote TLB shootdowns
     * before any table page is reclaimed.
     */
    if (page_table_empty(pt)) {
        pd[pdi] = 0;
        physical_page_free((uint64_t)(uintptr_t)pt);
    }
    if (page_table_empty(pd)) {
        pdpt[pdpti] = 0;
        physical_page_free((uint64_t)(uintptr_t)pd);
    }
    if (page_table_empty(pdpt)) {
        kernel_pml4[pml4i] = 0;
        physical_page_free((uint64_t)(uintptr_t)pdpt);
    }
}

uint64_t paging_translate(uint64_t virtual_address) {
    if (!kernel_pml4 || !canonical_address(virtual_address)) return 0;
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

extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];

int paging_query_page(uint64_t virtual_address, uint64_t *flags_out) {
    if (!kernel_pml4 || !flags_out || !canonical_address(virtual_address))
        return -1;

    uint64_t entry = kernel_pml4[PML4_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT))
        return -1;
    uint64_t *pdpt = (uint64_t *)(entry & PAGE_MASK);

    entry = pdpt[PDPT_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT))
        return -1;
    uint64_t *pd = (uint64_t *)(entry & PAGE_MASK);

    entry = pd[PD_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT))
        return -1;
    if (entry & LARGE_PAGE_FLAG) {
        *flags_out = entry & (PROSENJIT_PAGE_PRESENT |
                              PROSENJIT_PAGE_WRITABLE |
                              PROSENJIT_PAGE_USER |
                              PROSENJIT_PAGE_NX);
        return 0;
    }

    uint64_t *pt = (uint64_t *)(entry & PAGE_MASK);
    entry = pt[PT_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT))
        return -1;

    *flags_out = entry & (PROSENJIT_PAGE_PRESENT |
                          PROSENJIT_PAGE_WRITABLE |
                          PROSENJIT_PAGE_USER |
                          PROSENJIT_PAGE_NX);
    return 0;
}

static int map_identity_4k(uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    uint64_t *pdpt = ensure_table(kernel_pml4, PML4_INDEX(virtual_address),
                                   PROSENJIT_PAGE_WRITABLE);
    if (!pdpt) return -1;
    uint64_t *pd = ensure_table(pdpt, PDPT_INDEX(virtual_address),
                                PROSENJIT_PAGE_WRITABLE);
    if (!pd) return -1;
    uint64_t *pt = ensure_table(pd, PD_INDEX(virtual_address),
                                PROSENJIT_PAGE_WRITABLE);
    if (!pt) return -1;
    pt[PT_INDEX(virtual_address)] = (physical_address & PAGE_MASK) |
                                    PROSENJIT_PAGE_PRESENT |
                                    (flags & (PROSENJIT_PAGE_WRITABLE |
                                              PROSENJIT_PAGE_NX));
    return 0;
}

static int map_identity_range(uint64_t start, uint64_t end, uint64_t flags) {
    if (start >= end || end > UINT64_MAX - (PROSENJIT_PAGE_SIZE - 1ULL))
        return -1;
    start &= ~0xFFFULL;
    end = (end + PROSENJIT_PAGE_SIZE - 1ULL) & ~(PROSENJIT_PAGE_SIZE - 1ULL);
    if (start >= end || end > PROSENJIT_PHYS_LIMIT)
        return -1;
    for (uint64_t address = start; address < end; address += PROSENJIT_PAGE_SIZE) {
        if (map_identity_4k(address, address, flags) != 0) return -1;
    }
    return 0;
}

int paging_init(void) {
    kernel_pml4 = alloc_table();
    if (!kernel_pml4) return -1;

    uint64_t *pdpt = alloc_table();
    if (!pdpt) return -1;
    kernel_pml4[0] = ((uint64_t)pdpt & PAGE_MASK) |
                     PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_WRITABLE;

    /*
     * Replace the bootstrap 2 MiB identity mapping with 4 KiB mappings.
     * All identity-mapped pages default to writable + NX. Kernel text is
     * subsequently changed to read/execute only, giving the kernel a
     * section-aware W^X foundation.
     */
    if (map_identity_range(0, PROSENJIT_PHYS_LIMIT,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0)
        return -1;

    uint64_t text_start = (uint64_t)(uintptr_t)__kernel_text_start;
    uint64_t text_end = (uint64_t)(uintptr_t)__kernel_text_end;
    if (map_identity_range(text_start, text_end, 0) != 0) return -1;

    uint64_t rodata_start = (uint64_t)(uintptr_t)__kernel_rodata_start;
    uint64_t rodata_end = (uint64_t)(uintptr_t)__kernel_rodata_end;
    if (map_identity_range(rodata_start, rodata_end, PROSENJIT_PAGE_NX) != 0)
        return -1;

    /* BSS contains bootstrap stacks and other mutable kernel state. Keep it supervisor-writable. */
    uint64_t bss_start = (uint64_t)(uintptr_t)__kernel_bss_start;
    uint64_t bss_end = (uint64_t)(uintptr_t)__kernel_bss_end;
    if (map_identity_range(bss_start, bss_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0)
        return -1;

    /*
     * The bootstrap and IST stacks must remain mapped after CR3 switches.
     * Map them explicitly as supervisor RW + NX. Keep the linker symbols as
     * integer addresses before doing end-1 arithmetic so -Warray-bounds does
     * not treat stack_top as an actual C array object.
     */
    uint64_t stack_guard = (uint64_t)(uintptr_t)stack_guard_bottom;
    uint64_t stack_start = (uint64_t)(uintptr_t)stack_bottom;
    uint64_t stack_end = (uint64_t)(uintptr_t)stack_top;
    uint64_t ist1_guard = (uint64_t)(uintptr_t)ist1_stack_guard_bottom;
    uint64_t ist1_start = (uint64_t)(uintptr_t)ist1_stack_bottom;
    uint64_t ist1_end = (uint64_t)(uintptr_t)ist1_stack_top;
    uint64_t ist2_guard = (uint64_t)(uintptr_t)ist2_stack_guard_bottom;
    uint64_t ist2_start = (uint64_t)(uintptr_t)ist2_stack_bottom;
    uint64_t ist2_end = (uint64_t)(uintptr_t)ist2_stack_top;
    uint64_t ist3_guard = (uint64_t)(uintptr_t)ist3_stack_guard_bottom;
    uint64_t ist3_start = (uint64_t)(uintptr_t)ist3_stack_bottom;
    uint64_t ist3_end = (uint64_t)(uintptr_t)ist3_stack_top;

    if (map_identity_range(stack_start, stack_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        map_identity_range(ist1_start, ist1_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        map_identity_range(ist2_start, ist2_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        map_identity_range(ist3_start, ist3_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0)
        return -1;

    /*
     * The broad identity map above includes BSS. Remove the dedicated page
     * immediately below each critical stack before activating this root.
     * Guard pages remain reserved kernel-image physical memory; they are
     * intentionally unmapped, never returned to the page allocator.
     */
    paging_unmap_page(stack_guard);
    paging_unmap_page(ist1_guard);
    paging_unmap_page(ist2_guard);
    paging_unmap_page(ist3_guard);

    uint64_t test_page = physical_page_alloc();
    if (!test_page) return -1;
    if (paging_map_page(PROSENJIT_KERNEL_TEST_VA, test_page,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX)) {
        physical_page_free(test_page);
        return -1;
    }

    if (stack_end <= stack_start || ist1_end <= ist1_start ||
        ist2_end <= ist2_start || ist3_end <= ist3_start ||
        (stack_guard & 0xFFFULL) != 0 ||
        (ist1_guard & 0xFFFULL) != 0 ||
        (ist2_guard & 0xFFFULL) != 0 ||
        (ist3_guard & 0xFFFULL) != 0 ||
        paging_translate(stack_guard) != 0 ||
        paging_translate(ist1_guard) != 0 ||
        paging_translate(ist2_guard) != 0 ||
        paging_translate(ist3_guard) != 0 ||
        paging_translate(stack_end - 1u) == 0 ||
        paging_translate(ist1_end - 1u) == 0 ||
        paging_translate(ist2_end - 1u) == 0 ||
        paging_translate(ist3_end - 1u) == 0) {
        paging_unmap_page(PROSENJIT_KERNEL_TEST_VA);
        physical_page_free(test_page);
        return -1;
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)kernel_pml4) : "memory");
    paging_unmap_page(PROSENJIT_KERNEL_TEST_VA);
    if (paging_translate(PROSENJIT_KERNEL_TEST_VA) != 0) {
        physical_page_free(test_page);
        return -1;
    }
    physical_page_free(test_page);
    return 0;
}


int paging_selftest(void) {
    static const uint64_t va = PROSENJIT_KERNEL_TEST_VA + 0x1000ULL;
    uint64_t free_before_map = physical_memory_free_pages();
    uint64_t physical = physical_page_alloc();
    if (!physical) return -1;

    if (paging_map_page(va, physical, PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        physical_page_free(physical);
        return -2;
    }

    uint64_t translated = paging_translate(va);
    uint64_t flags = 0;
    if (translated != physical || paging_query_page(va, &flags) != 0 ||
        !(flags & PROSENJIT_PAGE_PRESENT) ||
        !(flags & PROSENJIT_PAGE_WRITABLE) ||
        !(flags & PROSENJIT_PAGE_NX)) {
        paging_unmap_page(va);
        physical_page_free(physical);
        return -3;
    }

    /*
     * A duplicate mapping must neither replace the leaf nor leak/reclaim
     * intermediate tables. Capture accounting and the translation before
     * attempting the invalid remap.
     */
    uint64_t free_before_duplicate = physical_memory_free_pages();
    if (paging_map_page(va, physical, PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) == 0) {
        paging_unmap_page(va);
        physical_page_free(physical);
        return -4;
    }
    if (paging_translate(va) != physical ||
        physical_memory_free_pages() != free_before_duplicate) {
        paging_unmap_page(va);
        physical_page_free(physical);
        return -4;
    }

    paging_unmap_page(va);
    if (paging_translate(va) != 0 || paging_query_page(va, &flags) == 0 ||
        physical_memory_free_pages() != free_before_map - 1u) {
        physical_page_free(physical);
        return -5;
    }
    /* Unmapping a virtual mapping must not release the caller-owned leaf page. */
    if (!physical_page_is_allocated(physical)) {
        physical_page_free(physical);
        return -5;
    }

    physical_page_free(physical);
    if (physical_memory_free_pages() != free_before_map)
        return -11;

    /*
     * Two adjacent mappings share the same page-table hierarchy. Unmapping
     * the first leaf must preserve the sibling translation; unmapping the
     * final leaf must reclaim exactly the three now-empty intermediate tables.
     */
    {
        static const uint64_t va_a = PROSENJIT_KERNEL_TEST_VA + 0x2000ULL;
        static const uint64_t va_b = PROSENJIT_KERNEL_TEST_VA + 0x3000ULL;
        uint64_t baseline = physical_memory_free_pages();
        uint64_t page_a = physical_page_alloc();
        uint64_t page_b = physical_page_alloc();
        int shared_ok = page_a != 0 && page_b != 0;
        uint64_t mapped_free = 0;

        if (shared_ok &&
            paging_map_page(va_a, page_a,
                            PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) == 0 &&
            paging_map_page(va_b, page_b,
                            PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) == 0) {
            mapped_free = physical_memory_free_pages();
            if (mapped_free != baseline - 5u)
                shared_ok = 0;
            paging_unmap_page(va_a);
            if (paging_translate(va_a) != 0 ||
                paging_translate(va_b) != page_b ||
                physical_memory_free_pages() != mapped_free)
                shared_ok = 0;
            paging_unmap_page(va_b);
            if (paging_translate(va_b) != 0 ||
                physical_memory_free_pages() != baseline - 2u)
                shared_ok = 0;
        } else {
            shared_ok = 0;
        }

        /* Cleanup is safe whether either mapping was successfully installed. */
        paging_unmap_page(va_a);
        paging_unmap_page(va_b);
        if (page_a) physical_page_free(page_a);
        if (page_b) physical_page_free(page_b);
        if (physical_memory_free_pages() != baseline)
            shared_ok = 0;
        if (!shared_ok)
            return -12;
    }

    /*
     * Force failure at each of the three intermediate page-table allocations
     * for an otherwise empty kernel virtual-address subtree. Every failure
     * must leave the address unmapped and return all temporary table pages.
     */
    {
        static const uint64_t fault_va = 0xFFFF808000000000ULL;
        for (int fail_after = 0; fail_after < 3; ++fail_after) {
            uint64_t free_before = physical_memory_free_pages();
            if (paging_translate(fault_va) != 0)
                return -8;
            paging_test_alloc_fail_after = fail_after;
            int map_result = paging_map_page(
                fault_va, 0x2000ULL,
                PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX);
            paging_test_alloc_fail_after = -1;
            if (map_result == 0 ||
                paging_translate(fault_va) != 0 ||
                physical_memory_free_pages() != free_before)
                return -9;
        }
    }

    if (paging_map_page(0x0000800000000000ULL, physical,
                         PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) == 0)
        return -6;
    /*
     * The kernel-root mapper must never create user-accessible mappings in
     * either canonical half. User pages are installed only in process roots.
     */
    if (paging_map_page(0xFFFF800000001000ULL, physical,
                         PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_USER |
                         PROSENJIT_PAGE_NX) == 0 ||
        paging_map_page(0x0000008000001000ULL, physical,
                         PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_USER |
                         PROSENJIT_PAGE_NX) == 0)
        return -10;
    if (paging_map_page(PROSENJIT_KERNEL_TEST_VA, PROSENJIT_PHYS_LIMIT,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) == 0)
        return -7;

    return 0;
}

#include "paging.h"
#include "memory.h"
#include "include/panic_handoff.h"

extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];
extern char __kernel_bss_start[];
extern char __kernel_bss_end[];
extern char __panic_stack_bottom[];
extern char __panic_stack_top[];
extern char __panic_frame_storage[];
extern char __panic_frame_storage_end[];
extern char stack_bottom[];
extern char stack_top[];
extern char ist1_stack_bottom[];
extern char ist1_stack_top[];
extern char ist2_stack_bottom[];
extern char ist2_stack_top[];

#define PAGE_MASK 0x000FFFFFFFFFF000ULL
#define ENTRIES 512u
#define PT_INDEX(va) (((va) >> 12) & 0x1FFu)
#define PD_INDEX(va) (((va) >> 21) & 0x1FFu)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FFu)
#define PML4_INDEX(va) (((va) >> 39) & 0x1FFu)
#define LARGE_PAGE_FLAG (1ULL << 7)
#define MMIO_VIRTUAL_BASE  0xFFFFC00000000000ULL
#define MMIO_VIRTUAL_LIMIT 0xFFFFD00000000000ULL

static uint64_t mmio_next_virtual;
/* One-shot page-table allocation fault injection for paging self-tests. */
static uint32_t paging_test_fail_after;
static uint32_t paging_test_fail_armed;
static uint32_t paging_test_map_fail_remaining;
static uint32_t paging_test_map_fail_armed;

void paging_test_table_alloc_fail_after(uint32_t successful_allocations) {
    __atomic_store_n(&paging_test_fail_after, successful_allocations,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&paging_test_fail_armed, 1u, __ATOMIC_RELEASE);
}

void paging_test_table_alloc_fail_clear(void) {
    __atomic_store_n(&paging_test_fail_armed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&paging_test_fail_after, 0u, __ATOMIC_RELEASE);
}


void paging_test_map_fail_after(uint32_t successful_mappings) {
    __atomic_store_n(&paging_test_map_fail_remaining, successful_mappings,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&paging_test_map_fail_armed, 1u, __ATOMIC_RELEASE);
}

void paging_test_map_fail_clear(void) {
    __atomic_store_n(&paging_test_map_fail_armed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&paging_test_map_fail_remaining, 0u, __ATOMIC_RELEASE);
}

static int paging_test_should_fail_map(void) {
    if (!__atomic_load_n(&paging_test_map_fail_armed, __ATOMIC_ACQUIRE))
        return 0;
    uint32_t remaining = __atomic_load_n(&paging_test_map_fail_remaining,
                                         __ATOMIC_ACQUIRE);
    while (remaining != 0u) {
        if (__atomic_compare_exchange_n(&paging_test_map_fail_remaining, &remaining,
                                        remaining - 1u, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 0;
    }
    uint32_t expected = 1u;
    return __atomic_compare_exchange_n(&paging_test_map_fail_armed, &expected, 0u,
                                       0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static int paging_test_should_fail_table_alloc(void) {
    if (!__atomic_load_n(&paging_test_fail_armed, __ATOMIC_ACQUIRE))
        return 0;
    uint32_t remaining = __atomic_load_n(&paging_test_fail_after,
                                         __ATOMIC_ACQUIRE);
    while (remaining != 0u) {
        if (__atomic_compare_exchange_n(&paging_test_fail_after, &remaining,
                                        remaining - 1u, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 0;
    }
    uint32_t expected = 1u;
    return __atomic_compare_exchange_n(&paging_test_fail_armed, &expected, 0u,
                                       0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static uint64_t *kernel_pml4;

static int paging_checked_leaf(uint64_t virtual_address, uint64_t **leaf_out);

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
    if (paging_test_should_fail_table_alloc())
        return 0;
    uint64_t physical = physical_page_alloc_table();
    if (physical == 0) return 0;
    uint64_t *table = (uint64_t *)physical;
    page_zero(table);
    return table;
}

static uint64_t *ensure_table(uint64_t *parent, uint32_t index, uint64_t flags) {
    uint64_t entry = parent[index];
    if (entry & PROSENJIT_PAGE_PRESENT) {
        if (entry & LARGE_PAGE_FLAG) return 0;
        if (!physical_page_is_page_table(entry & PAGE_MASK)) return 0;
        if ((flags & PROSENJIT_PAGE_USER) && !(entry & PROSENJIT_PAGE_USER)) return 0;
        if ((flags & PROSENJIT_PAGE_WRITABLE) && !(entry & PROSENJIT_PAGE_WRITABLE)) return 0;
        return (uint64_t *)(entry & PAGE_MASK);
    }

    uint64_t *table = alloc_table();
    if (!table) return 0;
    parent[index] = ((uint64_t)table & PAGE_MASK) | flags | PROSENJIT_PAGE_PRESENT;
    return table;
}

static int paging_map_page_internal(uint64_t virtual_address,
                                      uint64_t physical_address,
                                      uint64_t flags, int allow_mmio) {
    uint64_t allowed = PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_USER |
                       PROSENJIT_PAGE_NX;
    if (allow_mmio)
        allowed |= PROSENJIT_PAGE_WRITE_THROUGH |
                   PROSENJIT_PAGE_CACHE_DISABLE;
    if (!kernel_pml4 ||
        (virtual_address & 0xFFFULL) != 0 ||
        (physical_address & 0xFFFULL) != 0)
        return -1;
    if (!canonical_address(virtual_address) ||
        (allow_mmio ? (physical_address & ~PAGE_MASK) != 0 :
                      physical_address >= PROSENJIT_PHYS_LIMIT) ||
        (flags & ~allowed) != 0)
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
            (void)physical_page_free_table((uint64_t)(uintptr_t)pdpt);
            kernel_pml4[pml4_index] = 0;
        }
        return -1;
    }
    int created_pd = !(old_pdpte & PROSENJIT_PAGE_PRESENT);

    uint64_t old_pde = pd[pd_index];
    uint64_t *pt = ensure_table(pd, pd_index, table_flags);
    if (!pt) {
        if (created_pd) {
            (void)physical_page_free_table((uint64_t)(uintptr_t)pd);
            pdpt[pdpt_index] = 0;
        }
        if (created_pdpt) {
            (void)physical_page_free_table((uint64_t)(uintptr_t)pdpt);
            kernel_pml4[pml4_index] = 0;
        }
        return -1;
    }
    int created_pt = !(old_pde & PROSENJIT_PAGE_PRESENT);

    if (pt[PT_INDEX(virtual_address)] & PROSENJIT_PAGE_PRESENT) {
        if (created_pt) {
            (void)physical_page_free_table((uint64_t)(uintptr_t)pt);
            pd[pd_index] = 0;
        }
        if (created_pd) {
            (void)physical_page_free_table((uint64_t)(uintptr_t)pd);
            pdpt[pdpt_index] = 0;
        }
        if (created_pdpt) {
            (void)physical_page_free_table((uint64_t)(uintptr_t)pdpt);
            kernel_pml4[pml4_index] = 0;
        }
        return -2;
    }

    pt[PT_INDEX(virtual_address)] = (physical_address & PAGE_MASK) |
                                    (flags & (PROSENJIT_PAGE_WRITABLE |
                                              PROSENJIT_PAGE_USER |
                                              PROSENJIT_PAGE_NX |
                                              PROSENJIT_PAGE_WRITE_THROUGH |
                                              PROSENJIT_PAGE_CACHE_DISABLE)) |
                                    PROSENJIT_PAGE_PRESENT;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return 0;
}

int paging_map_page(uint64_t virtual_address, uint64_t physical_address,
                    uint64_t flags) {
    if (paging_test_should_fail_map())
        return -3;
    return paging_map_page_internal(virtual_address, physical_address, flags, 0);
}

int paging_map_mmio_region(uint64_t physical_address, uint64_t length,
                           uint64_t *virtual_address) {
    uint64_t offset, aligned_physical, span, rounded_span;
    uint64_t virtual_base, pages, mapped = 0u;

    if (!kernel_pml4 || !virtual_address || length == 0u)
        return -1;
    const uint64_t max_physical_address = PAGE_MASK | 0xFFFu;
    if (physical_address > max_physical_address ||
        length - 1u > max_physical_address - physical_address)
        return -1;

    offset = physical_address & 0xFFFu;
    aligned_physical = physical_address & ~0xFFFULL;
    if (length > UINT64_MAX - offset)
        return -1;
    span = offset + length;
    if (span > UINT64_MAX - 0xFFFu)
        return -1;
    rounded_span = (span + 0xFFFu) & ~0xFFFULL;
    pages = rounded_span / PROSENJIT_PAGE_SIZE;

    if (mmio_next_virtual == 0u)
        mmio_next_virtual = MMIO_VIRTUAL_BASE;
    virtual_base = (mmio_next_virtual + 0xFFFu) & ~0xFFFULL;
    if (virtual_base >= MMIO_VIRTUAL_LIMIT ||
        rounded_span > MMIO_VIRTUAL_LIMIT - virtual_base)
        return -1;

    while (mapped < pages) {
        int rc = paging_map_page_internal(
            virtual_base + mapped * PROSENJIT_PAGE_SIZE,
            aligned_physical + mapped * PROSENJIT_PAGE_SIZE,
            PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX |
                PROSENJIT_PAGE_WRITE_THROUGH |
                PROSENJIT_PAGE_CACHE_DISABLE,
            1);
        if (rc != 0) {
            while (mapped != 0u) {
                --mapped;
                paging_unmap_page(virtual_base +
                                  mapped * PROSENJIT_PAGE_SIZE);
            }
            return rc;
        }
        ++mapped;
    }

    mmio_next_virtual = virtual_base + rounded_span;
    *virtual_address = virtual_base + offset;
    return 0;
}

void paging_unmap_mmio_region(uint64_t virtual_address, uint64_t length) {
    uint64_t offset, span, rounded_span, virtual_base;
    if (length == 0u || virtual_address < MMIO_VIRTUAL_BASE ||
        virtual_address >= MMIO_VIRTUAL_LIMIT)
        return;
    offset = virtual_address & 0xFFFu;
    virtual_base = virtual_address & ~0xFFFULL;
    if (length > UINT64_MAX - offset)
        return;
    span = offset + length;
    if (span > UINT64_MAX - 0xFFFu)
        return;
    rounded_span = (span + 0xFFFu) & ~0xFFFULL;
    if (rounded_span > MMIO_VIRTUAL_LIMIT - virtual_base)
        return;
    for (uint64_t offset_pages = 0u;
         offset_pages < rounded_span;
         offset_pages += PROSENJIT_PAGE_SIZE)
        paging_unmap_page(virtual_base + offset_pages);
}

void paging_unmap_page(uint64_t virtual_address) {
    uint64_t *leaf = 0;
    if ((virtual_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u ||
        paging_checked_leaf(virtual_address, &leaf) != 0 ||
        !(*leaf & PROSENJIT_PAGE_PRESENT))
        return;
    *leaf = 0u;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

/*
 * Resolve a kernel page-table leaf only after authenticating every table
 * page as aligned, allocator-owned memory. This helper is intentionally for
 * the shared kernel page-table root used by supervisor stack mappings.
 */
static int paging_checked_leaf(uint64_t virtual_address, uint64_t **leaf_out) {
    if (!kernel_pml4 || !leaf_out || !canonical_address(virtual_address) ||
        !physical_page_is_page_table((uint64_t)(uintptr_t)kernel_pml4))
        return -1;

    uint64_t entry = kernel_pml4[PML4_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT) || (entry & LARGE_PAGE_FLAG))
        return -1;
    uint64_t table_pa = entry & PAGE_MASK;
    if (!physical_page_is_page_table(table_pa))
        return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)table_pa;

    entry = pdpt[PDPT_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT) || (entry & LARGE_PAGE_FLAG))
        return -1;
    table_pa = entry & PAGE_MASK;
    if (!physical_page_is_page_table(table_pa))
        return -1;
    uint64_t *pd = (uint64_t *)(uintptr_t)table_pa;

    entry = pd[PD_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT) || (entry & LARGE_PAGE_FLAG))
        return -1;
    table_pa = entry & PAGE_MASK;
    if (!physical_page_is_page_table(table_pa))
        return -1;
    uint64_t *pt = (uint64_t *)(uintptr_t)table_pa;
    *leaf_out = &pt[PT_INDEX(virtual_address)];
    return 0;
}

/*
 * Validate every VA->PA pair, including uniqueness, before clearing any PTE.
 * The bounded operation is used for fixed-size kernel resource groups.
 */
/* Checked translation for shared kernel mappings; absent leaf returns PA 0. */
int paging_query_physical_checked(uint64_t virtual_address,
                                  uint64_t *physical_address_out) {
    uint64_t *leaf = 0;
    if (!physical_address_out ||
        paging_checked_leaf(virtual_address, &leaf) != 0)
        return -1;
    uint64_t entry = *leaf;
    if (!(entry & PROSENJIT_PAGE_PRESENT)) {
        *physical_address_out = 0u;
        return 0;
    }
    *physical_address_out = (entry & PAGE_MASK) |
                            (virtual_address & (PROSENJIT_PAGE_SIZE - 1ULL));
    return 0;
}

int paging_unmap_pages_expected(const uint64_t *virtual_addresses,
                                const uint64_t *expected_physical,
                                uint64_t page_count) {
    uint64_t *leaves[64];
    if (!virtual_addresses || !expected_physical ||
        page_count == 0u || page_count > 64u)
        return -1;

    for (uint64_t i = 0u; i < page_count; ++i) {
        uint64_t va = virtual_addresses[i];
        uint64_t pa = expected_physical[i];
        if ((va & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u ||
            (pa & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u ||
            pa >= PROSENJIT_PHYS_LIMIT ||
            paging_checked_leaf(va, &leaves[i]) != 0 ||
            !(*leaves[i] & PROSENJIT_PAGE_PRESENT) ||
            ((*leaves[i] & PAGE_MASK) != pa))
            return -1;
        for (uint64_t j = 0u; j < i; ++j) {
            if (virtual_addresses[j] == va ||
                expected_physical[j] == pa)
                return -1;
        }
    }

    for (uint64_t i = 0u; i < page_count; ++i) {
        *leaves[i] = 0u;
        __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addresses[i]) : "memory");
    }
    return 0;
}

/*
 * Single-CPU atomic teardown for owned stack groups. Validate every mapping
 * and every allocator page before the first mutation. IRQ exclusion keeps
 * the PMM preflight valid through the unmap/free commit on this uniprocessor
 * kernel; this is not an SMP locking primitive.
 */
/*
 * A page-table page must have exactly one parent in the active kernel tree.
 * Otherwise clearing a leaf through one VA can silently clear an alias path.
 * The fixed bound makes validation deterministic and fail-closed; it is sized
 * well above the current kernel hierarchy and avoids allocator use in teardown.
 */
#define PAGING_MAX_TRACKED_TABLES 2048u
#define PAGING_MAX_TRANSACTION_PAGES 256u
#define LARGE_1G_RESERVED_MASK 0x000000003FFFE000ULL
#define LARGE_2M_RESERVED_MASK 0x00000000001FE000ULL
#define LARGE_1G_ADDRESS_MASK 0x000FFFFFC0000000ULL
#define LARGE_2M_ADDRESS_MASK 0x000FFFFFFFE00000ULL

struct paging_owned_page_index {
    uint64_t physical;
    uint64_t expected_virtual;
    uint8_t has_expected_virtual;
};

static uint64_t paging_seen_table_pages[PAGING_MAX_TRACKED_TABLES];
static uint32_t paging_seen_table_count;
static struct paging_owned_page_index
    paging_owned_page_index[PAGING_MAX_TRANSACTION_PAGES];
static uint32_t paging_owned_page_index_count;

static uint64_t paging_canonicalize_prefix(uint64_t prefix) {
    prefix &= 0x0000FFFFFFFFFFFFULL;
    return (prefix & (1ULL << 47)) ?
        (prefix | 0xFFFF000000000000ULL) : prefix;
}

static uint32_t paging_owned_page_lower_bound(uint64_t physical) {
    uint32_t low = 0u, high = paging_owned_page_index_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (paging_owned_page_index[middle].physical < physical)
            low = middle + 1u;
        else
            high = middle;
    }
    return low;
}

static int paging_check_leaf_aliases(uint64_t virtual_base,
                                     uint64_t physical_base,
                                     uint64_t page_size) {
    if (physical_base > UINT64_MAX - page_size)
        return -1;
    uint64_t physical_end = physical_base + page_size;
    uint32_t index = paging_owned_page_lower_bound(physical_base);
    while (index < paging_owned_page_index_count) {
        const struct paging_owned_page_index *owned =
            &paging_owned_page_index[index];
        if (owned->physical >= physical_end) break;
        uint64_t virtual_address =
            virtual_base + (owned->physical - physical_base);
        if (virtual_address != owned->physical &&
            (!owned->has_expected_virtual ||
             virtual_address != owned->expected_virtual))
            return -1;
        ++index;
    }
    return 0;
}

/* Authenticated, bounded hierarchy walk with complete-leaf VA reconstruction. */
static int paging_collect_unique_tables(uint64_t *table, uint32_t level,
                                        uint64_t virtual_prefix) {
    uint64_t physical = (uint64_t)(uintptr_t)table;
    if (!table || level == 0u || level > 4u ||
        !physical_page_is_page_table(physical))
        return -1;

    for (uint32_t i = 0u; i < paging_seen_table_count; ++i)
        if (paging_seen_table_pages[i] == physical)
            return -1;
    if (paging_seen_table_count >= PAGING_MAX_TRACKED_TABLES)
        return -1;
    paging_seen_table_pages[paging_seen_table_count++] = physical;

    for (uint32_t i = 0u; i < ENTRIES; ++i) {
        uint64_t entry = table[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT)) continue;

        if (level == 4u) {
            if (entry & LARGE_PAGE_FLAG) return -1;
            uint64_t child_physical = entry & PAGE_MASK;
            if (!physical_page_is_page_table(child_physical) ||
                paging_collect_unique_tables(
                    (uint64_t *)(uintptr_t)child_physical, 3u,
                    virtual_prefix | ((uint64_t)i << 39)) != 0)
                return -1;
            continue;
        }

        if (level == 3u && (entry & LARGE_PAGE_FLAG)) {
            if (entry & LARGE_1G_RESERVED_MASK) return -1;
            if (paging_owned_page_index_count != 0u &&
                paging_check_leaf_aliases(
                    paging_canonicalize_prefix(
                        virtual_prefix | ((uint64_t)i << 30)),
                    entry & LARGE_1G_ADDRESS_MASK, 1ULL << 30) != 0)
                return -1;
            continue;
        }

        if (level == 2u && (entry & LARGE_PAGE_FLAG)) {
            if (entry & LARGE_2M_RESERVED_MASK) return -1;
            if (paging_owned_page_index_count != 0u &&
                paging_check_leaf_aliases(
                    paging_canonicalize_prefix(
                        virtual_prefix | ((uint64_t)i << 21)),
                    entry & LARGE_2M_ADDRESS_MASK, 1ULL << 21) != 0)
                return -1;
            continue;
        }

        if (level == 1u) {
            if (paging_owned_page_index_count != 0u &&
                paging_check_leaf_aliases(
                    paging_canonicalize_prefix(
                        virtual_prefix | ((uint64_t)i << 12)),
                    entry & PAGE_MASK, PROSENJIT_PAGE_SIZE) != 0)
                return -1;
            continue;
        }

        uint64_t child_physical = entry & PAGE_MASK;
        uint32_t child_level = level - 1u;
        uint32_t shift = level == 3u ? 30u : 21u;
        if (!physical_page_is_page_table(child_physical) ||
            paging_collect_unique_tables(
                (uint64_t *)(uintptr_t)child_physical, child_level,
                virtual_prefix | ((uint64_t)i << shift)) != 0)
            return -1;
    }
    return 0;
}

static void paging_reset_alias_scratch(void) {
    paging_seen_table_count = 0u;
    paging_owned_page_index_count = 0u;
}

static int paging_kernel_hierarchy_is_unique(void) {
    if (!kernel_pml4 ||
        !physical_page_is_page_table((uint64_t)(uintptr_t)kernel_pml4))
        return -1;
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    paging_reset_alias_scratch();
    int rc = paging_collect_unique_tables(kernel_pml4, 4u, 0u);
    paging_reset_alias_scratch();
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return rc;
}

static int paging_validate_owned_page_aliases(
    const uint64_t *virtual_addresses,
    const uint64_t *expected_physical,
    uint64_t mapped_count,
    const uint64_t *allocated_physical,
    uint64_t allocated_count) {
    if (!allocated_physical || allocated_count == 0u ||
        allocated_count > PAGING_MAX_TRANSACTION_PAGES ||
        mapped_count > PAGING_MAX_TRANSACTION_PAGES ||
        (mapped_count != 0u && (!virtual_addresses || !expected_physical)))
        return -1;

    paging_owned_page_index_count = 0u;
    for (uint64_t i = 0u; i < allocated_count; ++i) {
        uint64_t pa = allocated_physical[i];
        if ((pa & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u ||
            pa >= PROSENJIT_PHYS_LIMIT || !physical_page_is_allocated(pa))
            return -1;
        struct paging_owned_page_index *slot =
            &paging_owned_page_index[paging_owned_page_index_count++];
        slot->physical = pa;
        slot->expected_virtual = 0u;
        slot->has_expected_virtual = 0u;
        for (uint64_t j = 0u; j < i; ++j)
            if (allocated_physical[j] == pa) return -1;
    }

    for (uint64_t i = 0u; i < mapped_count; ++i) {
        uint64_t va = virtual_addresses[i];
        uint64_t pa = expected_physical[i];
        if ((va & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u ||
            !canonical_address(va) ||
            (pa & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u ||
            pa >= PROSENJIT_PHYS_LIMIT)
            return -1;
        for (uint64_t j = 0u; j < i; ++j)
            if (virtual_addresses[j] == va ||
                expected_physical[j] == pa)
                return -1;
    }

    for (uint32_t i = 1u; i < paging_owned_page_index_count; ++i) {
        struct paging_owned_page_index value = paging_owned_page_index[i];
        uint32_t j = i;
        while (j != 0u &&
               paging_owned_page_index[j - 1u].physical > value.physical) {
            paging_owned_page_index[j] = paging_owned_page_index[j - 1u];
            --j;
        }
        paging_owned_page_index[j] = value;
    }

    for (uint64_t i = 0u; i < mapped_count; ++i) {
        uint32_t found = paging_owned_page_lower_bound(expected_physical[i]);
        if (found >= paging_owned_page_index_count ||
            paging_owned_page_index[found].physical != expected_physical[i] ||
            paging_owned_page_index[found].has_expected_virtual)
            return -1;
        paging_owned_page_index[found].expected_virtual = virtual_addresses[i];
        paging_owned_page_index[found].has_expected_virtual = 1u;
    }

    paging_seen_table_count = 0u;
    int walk_rc = (!kernel_pml4 ||
        !physical_page_is_page_table((uint64_t)(uintptr_t)kernel_pml4)) ?
        -1 : paging_collect_unique_tables(kernel_pml4, 4u, 0u);
    paging_reset_alias_scratch();
    return walk_rc;
}

static int paging_unmap_pages_expected_and_free_preflight_unlocked(
    const uint64_t *virtual_addresses,
    const uint64_t *expected_physical,
    uint64_t mapped_count,
    const uint64_t *allocated_physical,
    uint64_t allocated_count,
    uint64_t **leaves_out,
    uint64_t *saved_entries_out) {
    if (mapped_count > 256u || allocated_count == 0u ||
        allocated_count > 256u || !allocated_physical ||
        (mapped_count != 0u && (!virtual_addresses || !expected_physical)))
        return -1;

    if (paging_validate_owned_page_aliases(
            virtual_addresses, expected_physical, mapped_count,
            allocated_physical, allocated_count) != 0) {
        paging_reset_alias_scratch();
        return -1;
    }

    for (uint64_t i = 0u; i < mapped_count; ++i) {
        uint64_t va = virtual_addresses[i];
        uint64_t pa = expected_physical[i];
        uint64_t *leaf = 0;
        if ((va & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u ||
            (pa & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u ||
            pa >= PROSENJIT_PHYS_LIMIT ||
            paging_checked_leaf(va, &leaf) != 0 ||
            !(*leaf & PROSENJIT_PAGE_PRESENT) ||
            ((*leaf & PAGE_MASK) != pa))
            return -1;

        int owned_for_free = 0;
        for (uint64_t j = 0u; j < allocated_count; ++j)
            if (allocated_physical[j] == pa)
                owned_for_free = 1;
        if (!owned_for_free)
            return -1;

        for (uint64_t j = 0u; j < i; ++j) {
            if (virtual_addresses[j] == va || expected_physical[j] == pa)
                return -1;
        }
        if (leaves_out)
            leaves_out[i] = leaf;
        if (saved_entries_out)
            saved_entries_out[i] = *leaf;
    }

    int free_preflight_rc = physical_page_free_batch_preflight(
        allocated_physical, allocated_count);
    paging_reset_alias_scratch();
    return free_preflight_rc;
}

/*
 * Read-only half of the checked unmap/free transaction. Callers that must
 * compose this release with other resource teardown can establish the entire
 * stack/page ownership gate before their first destructive operation.
 */
int paging_unmap_pages_expected_and_free_preflight(
    const uint64_t *virtual_addresses,
    const uint64_t *expected_physical,
    uint64_t mapped_count,
    const uint64_t *allocated_physical,
    uint64_t allocated_count) {
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    int rc = paging_unmap_pages_expected_and_free_preflight_unlocked(
        virtual_addresses, expected_physical, mapped_count,
        allocated_physical, allocated_count, 0, 0);
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return rc;
}

int paging_unmap_pages_expected_and_free(
    const uint64_t *virtual_addresses,
    const uint64_t *expected_physical,
    uint64_t mapped_count,
    const uint64_t *allocated_physical,
    uint64_t allocated_count) {
    /* Static workspace avoids consuming a large fraction of a 16 KiB kernel stack. */
    static uint64_t *leaves[256];
    static uint64_t saved_entries[256];
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");

    if (paging_unmap_pages_expected_and_free_preflight_unlocked(
            virtual_addresses, expected_physical, mapped_count,
            allocated_physical, allocated_count,
            leaves, saved_entries) != 0) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }

    for (uint64_t i = 0u; i < mapped_count; ++i) {
        *leaves[i] = 0u;
        __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addresses[i]) : "memory");
    }
    if (physical_page_free_batch(allocated_physical, allocated_count) != 0) {
        /* PMM batch-free is all-or-nothing; restore PTEs on unexpected reject. */
        for (uint64_t i = 0u; i < mapped_count; ++i) {
            *leaves[i] = saved_entries[i];
            __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addresses[i]) : "memory");
        }
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return 0;
}

uint64_t paging_translate(uint64_t virtual_address) {
    if (!kernel_pml4 || !canonical_address(virtual_address) ||
        !physical_page_is_page_table((uint64_t)(uintptr_t)kernel_pml4))
        return 0;
    uint64_t entry = kernel_pml4[PML4_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT) || (entry & LARGE_PAGE_FLAG))
        return 0;
    uint64_t table_pa = entry & PAGE_MASK;
    if (!physical_page_is_page_table(table_pa)) return 0;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)table_pa;

    entry = pdpt[PDPT_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT) || (entry & LARGE_PAGE_FLAG))
        return 0;
    table_pa = entry & PAGE_MASK;
    if (!physical_page_is_page_table(table_pa)) return 0;
    uint64_t *pd = (uint64_t *)(uintptr_t)table_pa;

    entry = pd[PD_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT)) return 0;
    if (entry & LARGE_PAGE_FLAG)
        return (entry & 0x000FFFFFFFE00000ULL) |
               (virtual_address & 0x1FFFFFULL);
    table_pa = entry & PAGE_MASK;
    if (!physical_page_is_page_table(table_pa)) return 0;
    uint64_t *pt = (uint64_t *)(uintptr_t)table_pa;

    entry = pt[PT_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT)) return 0;
    return (entry & PAGE_MASK) | (virtual_address & 0xFFFULL);
}

extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];

int paging_query_page(uint64_t virtual_address, uint64_t *flags_out) {
    if (!kernel_pml4 || !flags_out || !canonical_address(virtual_address) ||
        !physical_page_is_page_table((uint64_t)(uintptr_t)kernel_pml4))
        return -1;

    uint64_t entry = kernel_pml4[PML4_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT) || (entry & LARGE_PAGE_FLAG))
        return -1;
    uint64_t table_pa = entry & PAGE_MASK;
    if (!physical_page_is_page_table(table_pa)) return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)table_pa;

    entry = pdpt[PDPT_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT) || (entry & LARGE_PAGE_FLAG))
        return -1;
    table_pa = entry & PAGE_MASK;
    if (!physical_page_is_page_table(table_pa)) return -1;
    uint64_t *pd = (uint64_t *)(uintptr_t)table_pa;

    entry = pd[PD_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT))
        return -1;
    if (entry & LARGE_PAGE_FLAG) {
        *flags_out = entry & (PROSENJIT_PAGE_PRESENT |
                              PROSENJIT_PAGE_WRITABLE |
                              PROSENJIT_PAGE_USER |
                              PROSENJIT_PAGE_NX |
                              PROSENJIT_PAGE_WRITE_THROUGH |
                              PROSENJIT_PAGE_CACHE_DISABLE);
        return 0;
    }

    table_pa = entry & PAGE_MASK;
    if (!physical_page_is_page_table(table_pa)) return -1;
    uint64_t *pt = (uint64_t *)(uintptr_t)table_pa;
    entry = pt[PT_INDEX(virtual_address)];
    if (!(entry & PROSENJIT_PAGE_PRESENT))
        return -1;

    *flags_out = entry & (PROSENJIT_PAGE_PRESENT |
                          PROSENJIT_PAGE_WRITABLE |
                          PROSENJIT_PAGE_USER |
                          PROSENJIT_PAGE_NX |
                          PROSENJIT_PAGE_WRITE_THROUGH |
                          PROSENJIT_PAGE_CACHE_DISABLE);
    return 0;
}

/*
 * Return effective x86-64 permissions for a 4-KiB leaf mapping.
 * RW and U/S are ANDed across all four levels; NX is ORed across levels.
 * Large-page mappings are deliberately rejected so callers cannot mistake
 * a partial 4-KiB walk for a complete permission result.
 */
/* Test-only controlled permission corruption for kernel regression tests. */
int paging_test_modify_level_flags(uint64_t virtual_address, uint32_t level,
                                   uint64_t clear_flags, uint64_t set_flags) {
    const uint64_t allowed = PROSENJIT_PAGE_WRITABLE |
                             PROSENJIT_PAGE_USER | PROSENJIT_PAGE_NX;
    uint64_t irq_flags;
    if (level > 3u || (clear_flags & ~allowed) != 0u ||
        (set_flags & ~allowed) != 0u || (clear_flags & set_flags) != 0u ||
        (virtual_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u)
        return -1;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    uint64_t *table = kernel_pml4;
    const uint32_t indices[4] = {
        PML4_INDEX(virtual_address), PDPT_INDEX(virtual_address),
        PD_INDEX(virtual_address), PT_INDEX(virtual_address)
    };
    int rc = -1;
    if (!table || !canonical_address(virtual_address) ||
        !physical_page_is_page_table((uint64_t)(uintptr_t)table))
        goto done;
    for (uint32_t current = 0u; current <= level; ++current) {
        uint64_t entry = table[indices[current]];
        if (!(entry & PROSENJIT_PAGE_PRESENT) ||
            (current < 3u && (entry & LARGE_PAGE_FLAG)))
            goto done;
        if (current == level) {
            table[indices[current]] = (entry & ~clear_flags) | set_flags;
            __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
            rc = 0;
            goto done;
        }
        uint64_t child = entry & PAGE_MASK;
        if (!physical_page_is_page_table(child))
            goto done;
        table = (uint64_t *)(uintptr_t)child;
    }
done:
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return rc;
}

int paging_query_effective_page(uint64_t virtual_address, uint64_t *flags_out) {
    if (!kernel_pml4 || !flags_out ||
        (virtual_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u ||
        !canonical_address(virtual_address) ||
        !physical_page_is_page_table((uint64_t)(uintptr_t)kernel_pml4))
        return -1;

    uint64_t *tables[4];
    uint64_t entries[4];
    tables[0] = kernel_pml4;
    const uint32_t indices[4] = {
        PML4_INDEX(virtual_address), PDPT_INDEX(virtual_address),
        PD_INDEX(virtual_address), PT_INDEX(virtual_address)
    };
    for (uint32_t level = 0u; level < 4u; ++level) {
        entries[level] = tables[level][indices[level]];
        if (!(entries[level] & PROSENJIT_PAGE_PRESENT) ||
            (level < 3u && (entries[level] & LARGE_PAGE_FLAG)))
            return -1;
        if (level < 3u) {
            uint64_t child = entries[level] & PAGE_MASK;
            if (!physical_page_is_page_table(child))
                return -1;
            tables[level + 1u] = (uint64_t *)(uintptr_t)child;
        }
    }

    uint64_t effective = PROSENJIT_PAGE_PRESENT;
    if ((entries[0] & PROSENJIT_PAGE_WRITABLE) &&
        (entries[1] & PROSENJIT_PAGE_WRITABLE) &&
        (entries[2] & PROSENJIT_PAGE_WRITABLE) &&
        (entries[3] & PROSENJIT_PAGE_WRITABLE))
        effective |= PROSENJIT_PAGE_WRITABLE;
    if ((entries[0] & PROSENJIT_PAGE_USER) &&
        (entries[1] & PROSENJIT_PAGE_USER) &&
        (entries[2] & PROSENJIT_PAGE_USER) &&
        (entries[3] & PROSENJIT_PAGE_USER))
        effective |= PROSENJIT_PAGE_USER;
    if ((entries[0] | entries[1] | entries[2] | entries[3]) &
        PROSENJIT_PAGE_NX)
        effective |= PROSENJIT_PAGE_NX;
    effective |= entries[3] & (PROSENJIT_PAGE_WRITE_THROUGH |
                               PROSENJIT_PAGE_CACHE_DISABLE);
    *flags_out = effective;
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

    /*
     * Thread-stack virtual slots live below PML4[256], PDPT[32]. Establish
     * this supervisor-only hierarchy before address spaces copy the kernel
     * PML4[256] entry, so later stack mappings are shared by every process.
     */
    uint64_t *kernel_high_pdpt =
        ensure_table(kernel_pml4, 256u, PROSENJIT_PAGE_WRITABLE);
    if (!kernel_high_pdpt ||
        !ensure_table(kernel_high_pdpt, 32u, PROSENJIT_PAGE_WRITABLE))
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
    uint64_t panic_stack_start =
        (uint64_t)(uintptr_t)__panic_stack_bottom;
    uint64_t panic_stack_end =
        (uint64_t)(uintptr_t)__panic_stack_top;
    uint64_t panic_frame_start =
        (uint64_t)(uintptr_t)__panic_frame_storage;
    uint64_t panic_frame_end =
        (uint64_t)(uintptr_t)__panic_frame_storage_end;
    if (map_identity_range(bss_start, bss_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0)
        return -1;

    /*
     * These are linker-owned, non-allocating panic resources. Validate their
     * shape explicitly even though the enclosing BSS range is already mapped.
     * No exception path uses them yet; Phase A only establishes safe storage.
     */
    if (panic_stack_start >= panic_stack_end ||
        (panic_stack_start & (PROSENJIT_PAGE_SIZE - 1u)) != 0u ||
        (panic_stack_end & 0xFULL) != 0u ||
        panic_stack_end - panic_stack_start != PROS_PANIC_STACK_SIZE ||
        panic_frame_start >= panic_frame_end ||
        (panic_frame_start & 0xFULL) != 0u ||
        panic_frame_end - panic_frame_start != PROS_PANIC_FRAME_STORAGE_SIZE ||
        panic_stack_start < bss_start || panic_stack_end > bss_end ||
        panic_frame_start < bss_start || panic_frame_end > bss_end ||
        map_identity_range(panic_stack_start, panic_stack_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        map_identity_range(panic_frame_start, panic_frame_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0)
        return -1;

    /*
     * The bootstrap and IST stacks must remain mapped after CR3 switches.
     * Map them explicitly as supervisor RW + NX. Keep the linker symbols as
     * integer addresses before doing end-1 arithmetic so -Warray-bounds does
     * not treat stack_top as an actual C array object.
     */
    uint64_t stack_start = (uint64_t)(uintptr_t)stack_bottom;
    uint64_t stack_end = (uint64_t)(uintptr_t)stack_top;
    uint64_t ist1_start = (uint64_t)(uintptr_t)ist1_stack_bottom;
    uint64_t ist1_end = (uint64_t)(uintptr_t)ist1_stack_top;
    uint64_t ist2_start = (uint64_t)(uintptr_t)ist2_stack_bottom;
    uint64_t ist2_end = (uint64_t)(uintptr_t)ist2_stack_top;

    if (map_identity_range(stack_start, stack_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        map_identity_range(ist1_start, ist1_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        map_identity_range(ist2_start, ist2_end,
                           PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0)
        return -1;

    uint64_t test_page = physical_page_alloc();
    if (!test_page) return -1;
    if (paging_map_page(PROSENJIT_KERNEL_TEST_VA, test_page,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX)) {
        physical_page_free(test_page);
        return -1;
    }

    if (stack_end <= stack_start || ist1_end <= ist1_start ||
        ist2_end <= ist2_start ||
        paging_translate(stack_end - 1u) == 0 ||
        paging_translate(ist1_end - 1u) == 0 ||
        paging_translate(ist2_end - 1u) == 0 ||
        paging_translate(panic_stack_start) == 0 ||
        paging_translate(panic_stack_end - 1u) == 0 ||
        paging_translate(panic_frame_start) == 0 ||
        paging_translate(panic_frame_end - 1u) == 0) {
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
    static const uint64_t va = 0xFFFFE08000000000ULL;
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

    /* Effective permission aggregation and hierarchy corruption regressions. */
    uint64_t *permission_tables[4];
    uint64_t permission_saved[4];
    permission_tables[0] = kernel_pml4;
    permission_saved[0] = permission_tables[0][PML4_INDEX(va)];
    permission_tables[1] = (uint64_t *)(uintptr_t)(permission_saved[0] & PAGE_MASK);
    permission_saved[1] = permission_tables[1][PDPT_INDEX(va)];
    permission_tables[2] = (uint64_t *)(uintptr_t)(permission_saved[1] & PAGE_MASK);
    permission_saved[2] = permission_tables[2][PD_INDEX(va)];
    permission_tables[3] = (uint64_t *)(uintptr_t)(permission_saved[2] & PAGE_MASK);
    permission_saved[3] = permission_tables[3][PT_INDEX(va)];
    uint32_t permission_indices[4] = {
        PML4_INDEX(va), PDPT_INDEX(va), PD_INDEX(va), PT_INDEX(va)
    };
    uint64_t effective_flags = 0u;
    if (paging_query_effective_page(va, &effective_flags) != 0 ||
        !(effective_flags & PROSENJIT_PAGE_PRESENT) ||
        !(effective_flags & PROSENJIT_PAGE_WRITABLE) ||
        (effective_flags & PROSENJIT_PAGE_USER) ||
        !(effective_flags & PROSENJIT_PAGE_NX))
        goto permission_test_failed;

    for (uint32_t level = 0u; level < 4u; ++level) {
        permission_tables[level][permission_indices[level]] =
            permission_saved[level] & ~PROSENJIT_PAGE_WRITABLE;
        if (paging_query_effective_page(va, &effective_flags) != 0 ||
            (effective_flags & PROSENJIT_PAGE_WRITABLE))
            goto permission_test_failed;
        permission_tables[level][permission_indices[level]] =
            permission_saved[level];
    }
    /* U/S is effective only when every level grants user access. */
    for (uint32_t level = 0u; level < 4u; ++level)
        permission_tables[level][permission_indices[level]] =
            permission_saved[level] | PROSENJIT_PAGE_USER;
    if (paging_query_effective_page(va, &effective_flags) != 0 ||
        !(effective_flags & PROSENJIT_PAGE_USER))
        goto permission_test_failed;
    for (uint32_t level = 0u; level < 4u; ++level)
        permission_tables[level][permission_indices[level]] = permission_saved[level];
    permission_tables[2][permission_indices[2]] |= PROSENJIT_PAGE_USER;
    if (paging_query_effective_page(va, &effective_flags) != 0 ||
        (effective_flags & PROSENJIT_PAGE_USER))
        goto permission_test_failed;
    permission_tables[2][permission_indices[2]] = permission_saved[2];

    /* NX at any level disables execution; clearing the leaf NX restores it. */
    permission_tables[3][permission_indices[3]] =
        permission_saved[3] & ~PROSENJIT_PAGE_NX;
    if (paging_query_effective_page(va, &effective_flags) != 0 ||
        (effective_flags & PROSENJIT_PAGE_NX))
        goto permission_test_failed;
    permission_tables[3][permission_indices[3]] = permission_saved[3];
    permission_tables[1][permission_indices[1]] |= PROSENJIT_PAGE_NX;
    if (paging_query_effective_page(va, &effective_flags) != 0 ||
        !(effective_flags & PROSENJIT_PAGE_NX))
        goto permission_test_failed;
    permission_tables[1][permission_indices[1]] = permission_saved[1];
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
    goto permission_tests_done;

permission_test_failed:
    for (uint32_t level = 0u; level < 4u; ++level)
        permission_tables[level][permission_indices[level]] = permission_saved[level];
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
    paging_unmap_page(va);
    physical_page_free(physical);
    return -30;
permission_tests_done:

    /* Expected-PA batch unmap rejects the entire batch before mutation. */
    uint64_t batch_va[2] = {va, va + PROSENJIT_PAGE_SIZE};
    uint64_t batch_pa[2] = {physical, 0u};
    uint64_t second_physical = physical_page_alloc();
    if (!second_physical ||
        paging_map_page(batch_va[1], second_physical,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        if (second_physical) physical_page_free(second_physical);
        paging_unmap_page(va);
        physical_page_free(physical);
        return -13;
    }
    batch_pa[1] = second_physical + PROSENJIT_PAGE_SIZE; /* Wrong expected PA. */
    if (paging_unmap_pages_expected(batch_va, batch_pa, 2u) == 0 ||
        paging_translate(batch_va[0]) != physical ||
        paging_translate(batch_va[1]) != second_physical) {
        paging_unmap_page(batch_va[1]);
        paging_unmap_page(batch_va[0]);
        physical_page_free(second_physical);
        physical_page_free(physical);
        return -14;
    }
    batch_pa[1] = second_physical;
    if (paging_unmap_pages_expected(batch_va, batch_pa, 2u) != 0 ||
        paging_translate(batch_va[0]) != 0u ||
        paging_translate(batch_va[1]) != 0u) {
        paging_unmap_page(batch_va[1]);
        paging_unmap_page(batch_va[0]);
        physical_page_free(second_physical);
        physical_page_free(physical);
        return -15;
    }
    physical_page_free(second_physical);
    physical_page_free(physical);
    physical = physical_page_alloc();
    if (!physical) return -16;
    if (paging_map_page(va, physical,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        physical_page_free(physical);
        return -17;
    }

    /* A second mapping must never silently replace an existing live PTE. */
    if (paging_map_page(va, physical, PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) == 0) {
        paging_unmap_page(va);
        physical_page_free(physical);
        return -4;
    }

    paging_unmap_page(va);
    if (paging_translate(va) != 0 || paging_query_page(va, &flags) == 0) {
        physical_page_free(physical);
        return -5;
    }

    physical_page_free(physical);

    /* MMIO mappings must preserve the physical offset and be uncached. */
    uint64_t mmio_page = physical_page_alloc();
    uint64_t mmio_va = 0u;
    if (!mmio_page ||
        paging_map_mmio_region(mmio_page + 0x80u, 0x800u, &mmio_va) != 0) {
        if (mmio_page) physical_page_free(mmio_page);
        return -8;
    }
    if ((mmio_va & 0xFFFu) != 0x80u ||
        paging_translate(mmio_va) != mmio_page + 0x80u ||
        paging_query_page(mmio_va, &flags) != 0 ||
        !(flags & PROSENJIT_PAGE_CACHE_DISABLE) ||
        !(flags & PROSENJIT_PAGE_WRITE_THROUGH)) {
        paging_unmap_mmio_region(mmio_va, 0x800u);
        physical_page_free(mmio_page);
        return -9;
    }
    paging_unmap_mmio_region(mmio_va, 0x800u);
    if (paging_translate(mmio_va) != 0u) {
        physical_page_free(mmio_page);
        return -10;
    }
    physical_page_free(mmio_page);

    /* PCI MMIO BARs commonly live above the allocator's 1-GiB RAM window. */
    if (paging_map_mmio_region(0x100000080ULL, 0x800u, &mmio_va) != 0 ||
        paging_translate(mmio_va) != 0x100000080ULL)
        return -11;
    paging_unmap_mmio_region(mmio_va, 0x800u);
    if (paging_translate(mmio_va) != 0u)
        return -12;

    if (paging_map_page(0x0000800000000000ULL, physical,
                         PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) == 0)
        return -6;
    if (paging_map_page(PROSENJIT_KERNEL_TEST_VA, PROSENJIT_PHYS_LIMIT,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) == 0)
        return -7;

    /*
     * A fresh PML4 slot exercises failures while creating PDPT, PD and PT.
     * Every partial hierarchy must be detached and its table pages returned.
     */
    const uint64_t hierarchy_test_va = 0xFFFFE00000000000ULL;
    const uint32_t hierarchy_test_index = PML4_INDEX(hierarchy_test_va);
    uint64_t hierarchy_data = physical_page_alloc();
    if (!hierarchy_data || kernel_pml4[hierarchy_test_index] != 0u) {
        if (hierarchy_data) physical_page_free(hierarchy_data);
        return -18;
    }
    for (uint32_t fail_after = 0u; fail_after < 3u; ++fail_after) {
        uint64_t free_before = physical_memory_free_pages();
        paging_test_table_alloc_fail_after(fail_after);
        int map_rc = paging_map_page(hierarchy_test_va, hierarchy_data,
                                     PROSENJIT_PAGE_WRITABLE |
                                     PROSENJIT_PAGE_NX);
        paging_test_table_alloc_fail_clear();
        if (map_rc == 0 || kernel_pml4[hierarchy_test_index] != 0u ||
            physical_memory_free_pages() != free_before) {
            paging_test_table_alloc_fail_clear();
            physical_page_free(hierarchy_data);
            return -19;
        }
        uint64_t absent_physical = 0u;
        if (paging_query_physical_checked(hierarchy_test_va,
                                          &absent_physical) == 0) {
            physical_page_free(hierarchy_data);
            return -20;
        }
    }
    /* Point an unused root slot at a normal data page. Checked lookup must
     * reject the page type before dereferencing it as a PDPT. */
    kernel_pml4[hierarchy_test_index] =
        hierarchy_data | PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_WRITABLE;
    uint64_t rejected_physical = 0u;
    int corrupt_walk_rc =
        paging_query_physical_checked(hierarchy_test_va, &rejected_physical);
    kernel_pml4[hierarchy_test_index] = 0u;
    if (corrupt_walk_rc == 0 ||
        !physical_page_is_allocated(hierarchy_data) ||
        physical_page_is_page_table(hierarchy_data)) {
        physical_page_free(hierarchy_data);
        return -21;
    }
    physical_page_free(hierarchy_data);

    /* A failed allocator preflight must leave the mapped VA and all pages intact. */
    const uint64_t transaction_va = hierarchy_test_va;
    uint64_t transaction_mapped = physical_page_alloc();
    uint64_t transaction_unmapped = physical_page_alloc();
    if (!transaction_mapped || !transaction_unmapped ||
        paging_map_page(transaction_va, transaction_mapped,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        if (transaction_mapped) physical_page_free(transaction_mapped);
        if (transaction_unmapped) physical_page_free(transaction_unmapped);
        return -22;
    }
    uint64_t duplicate_free[2] = {transaction_mapped, transaction_mapped};
    if (paging_unmap_pages_expected_and_free(
            &transaction_va, &transaction_mapped, 1u,
            duplicate_free, 2u) == 0 ||
        paging_query_physical_checked(transaction_va, &rejected_physical) != 0 ||
        rejected_physical != transaction_mapped ||
        !physical_page_is_allocated(transaction_mapped) ||
        !physical_page_is_allocated(transaction_unmapped)) {
        return -23;
    }
    uint64_t transaction_pages[2] = {transaction_mapped, transaction_unmapped};
    if (paging_unmap_pages_expected_and_free(
            &transaction_va, &transaction_mapped, 1u,
            transaction_pages, 2u) != 0 ||
        paging_query_physical_checked(transaction_va, &rejected_physical) != 0 ||
        rejected_physical != 0u ||
        physical_page_is_allocated(transaction_mapped) ||
        physical_page_is_allocated(transaction_unmapped))
        return -24;

    /*
     * Reverse-map ownership regression: an extra allocated page must not be
     * freed while a nonidentity leaf still aliases it. Exercise both the
     * read-only gate and the commit's independent full preflight.
     */
    uint64_t reverse_mapped = physical_page_alloc();
    uint64_t reverse_extra = physical_page_alloc();
    const uint64_t reverse_va_a = transaction_va;
    const uint64_t reverse_va_b = transaction_va + PROSENJIT_PAGE_SIZE;
    uint64_t reverse_expected[1];
    uint64_t reverse_allocated[2];
    if (!reverse_mapped || !reverse_extra) {
        if (reverse_mapped) physical_page_free(reverse_mapped);
        if (reverse_extra) physical_page_free(reverse_extra);
        return -36;
    }
    if (paging_map_page(reverse_va_a, reverse_mapped,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        paging_map_page(reverse_va_b, reverse_extra,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        paging_unmap_page(reverse_va_b);
        paging_unmap_page(reverse_va_a);
        physical_page_free(reverse_mapped);
        physical_page_free(reverse_extra);
        return -37;
    }
    reverse_expected[0] = reverse_mapped;
    reverse_allocated[0] = reverse_mapped;
    reverse_allocated[1] = reverse_extra;
    if (paging_unmap_pages_expected_and_free_preflight(
            &reverse_va_a, reverse_expected, 1u,
            reverse_allocated, 2u) == 0 ||
        paging_unmap_pages_expected_and_free(
            &reverse_va_a, reverse_expected, 1u,
            reverse_allocated, 2u) == 0 ||
        paging_translate(reverse_va_a) != reverse_mapped ||
        paging_translate(reverse_va_b) != reverse_extra ||
        !physical_page_is_allocated(reverse_mapped) ||
        !physical_page_is_allocated(reverse_extra)) {
        paging_unmap_page(reverse_va_b);
        paging_unmap_page(reverse_va_a);
        if (physical_page_is_allocated(reverse_mapped))
            physical_page_free(reverse_mapped);
        if (physical_page_is_allocated(reverse_extra))
            physical_page_free(reverse_extra);
        return -38;
    }

    paging_unmap_page(reverse_va_b);
    if (paging_translate(reverse_va_b) != 0u ||
        paging_unmap_pages_expected_and_free_preflight(
            &reverse_va_a, reverse_expected, 1u,
            reverse_allocated, 2u) != 0 ||
        paging_unmap_pages_expected_and_free(
            &reverse_va_a, reverse_expected, 1u,
            reverse_allocated, 2u) != 0 ||
        paging_translate(reverse_va_a) != 0u ||
        physical_page_is_allocated(reverse_mapped) ||
        physical_page_is_allocated(reverse_extra) ||
        paging_translate(reverse_mapped) != reverse_mapped ||
        paging_translate(reverse_extra) != reverse_extra)
        return -39;

    /* A zero-mapped rollback must reject an allocated page with any alias. */
    uint64_t zero_mapped_extra = physical_page_alloc();
    if (!zero_mapped_extra ||
        paging_map_page(reverse_va_b, zero_mapped_extra,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        if (zero_mapped_extra) physical_page_free(zero_mapped_extra);
        return -40;
    }
    if (paging_unmap_pages_expected_and_free_preflight(
            0, 0, 0u, &zero_mapped_extra, 1u) == 0 ||
        paging_unmap_pages_expected_and_free(
            0, 0, 0u, &zero_mapped_extra, 1u) == 0 ||
        paging_translate(reverse_va_b) != zero_mapped_extra ||
        !physical_page_is_allocated(zero_mapped_extra)) {
        paging_unmap_page(reverse_va_b);
        if (physical_page_is_allocated(zero_mapped_extra))
            physical_page_free(zero_mapped_extra);
        return -41;
    }
    paging_unmap_page(reverse_va_b);
    if (paging_unmap_pages_expected_and_free(
            0, 0, 0u, &zero_mapped_extra, 1u) != 0 ||
        physical_page_is_allocated(zero_mapped_extra) ||
        paging_translate(zero_mapped_extra) != zero_mapped_extra)
        return -42;

    /*
     * Large-leaf reverse-map regressions. Temporarily replace the existing
     * test VA's PDE/PDPTE with a 2-MiB/1-GiB leaf covering an allocated page.
     * The target page is reachable through a nonidentity VA only while the
     * large leaf is installed. Both scanners must reject that alias; after
     * restoring the original hierarchy, the zero-map release must succeed.
     */
    uint64_t large_page_target = physical_page_alloc();
    if (!large_page_target) return -43;

    uint64_t large_pml4e = kernel_pml4[PML4_INDEX(hierarchy_test_va)];
    uint64_t *large_pdpt = (uint64_t *)(uintptr_t)(large_pml4e & PAGE_MASK);
    uint64_t large_pdpte = large_pdpt[PDPT_INDEX(hierarchy_test_va)];
    uint64_t *large_pd = (uint64_t *)(uintptr_t)(large_pdpte & PAGE_MASK);
    uint32_t large_pd_index = PD_INDEX(hierarchy_test_va);
    uint64_t saved_large_pde = large_pd[large_pd_index];
    if (!(large_pml4e & PROSENJIT_PAGE_PRESENT) ||
        !(large_pdpte & PROSENJIT_PAGE_PRESENT) ||
        (large_pdpte & LARGE_PAGE_FLAG) ||
        !(saved_large_pde & PROSENJIT_PAGE_PRESENT) ||
        (saved_large_pde & LARGE_PAGE_FLAG)) {
        physical_page_free(large_page_target);
        return -44;
    }

    large_pd[large_pd_index] =
        (large_page_target & LARGE_2M_ADDRESS_MASK) |
        PROSENJIT_PAGE_PRESENT | LARGE_PAGE_FLAG;
    int large_2m_rejected =
        paging_unmap_pages_expected_and_free_preflight(
            0, 0, 0u, &large_page_target, 1u) != 0;
    large_pd[large_pd_index] = saved_large_pde;
    if (!large_2m_rejected ||
        paging_unmap_pages_expected_and_free_preflight(
            0, 0, 0u, &large_page_target, 1u) != 0 ||
        paging_unmap_pages_expected_and_free(
            0, 0, 0u, &large_page_target, 1u) != 0 ||
        physical_page_is_allocated(large_page_target) ||
        paging_translate(large_page_target) != large_page_target)
        return -45;

    large_page_target = physical_page_alloc();
    if (!large_page_target) return -46;
    uint32_t large_pdpt_index = PDPT_INDEX(hierarchy_test_va);
    uint64_t saved_large_pdpte = large_pdpt[large_pdpt_index];
    if (!(saved_large_pdpte & PROSENJIT_PAGE_PRESENT) ||
        (saved_large_pdpte & LARGE_PAGE_FLAG)) {
        physical_page_free(large_page_target);
        return -47;
    }

    large_pdpt[large_pdpt_index] =
        (large_page_target & LARGE_1G_ADDRESS_MASK) |
        PROSENJIT_PAGE_PRESENT | LARGE_PAGE_FLAG;
    int large_1g_rejected =
        paging_unmap_pages_expected_and_free_preflight(
            0, 0, 0u, &large_page_target, 1u) != 0;
    large_pdpt[large_pdpt_index] = saved_large_pdpte;
    if (!large_1g_rejected ||
        paging_unmap_pages_expected_and_free_preflight(
            0, 0, 0u, &large_page_target, 1u) != 0 ||
        paging_unmap_pages_expected_and_free(
            0, 0, 0u, &large_page_target, 1u) != 0 ||
        physical_page_is_allocated(large_page_target) ||
        paging_translate(large_page_target) != large_page_target)
        return -48;

    /*
     * Corrupt each page-table parent level with a duplicate child pointer.
     * The owned teardown must reject before clearing the PTE or freeing data.
     */
    const uint32_t alias_pml4_index = PML4_INDEX(hierarchy_test_va);
    const uint32_t alias_pdpt_index = PDPT_INDEX(hierarchy_test_va);
    const uint32_t alias_pd_index = PD_INDEX(hierarchy_test_va);
    uint64_t alias_pml4e = kernel_pml4[alias_pml4_index];
    if (!(alias_pml4e & PROSENJIT_PAGE_PRESENT) ||
        paging_kernel_hierarchy_is_unique() != 0)
        return -25;
    uint64_t *alias_pdpt =
        (uint64_t *)(uintptr_t)(alias_pml4e & PAGE_MASK);
    uint64_t alias_pdpte = alias_pdpt[alias_pdpt_index];
    if (!(alias_pdpte & PROSENJIT_PAGE_PRESENT)) return -26;
    uint64_t *alias_pd =
        (uint64_t *)(uintptr_t)(alias_pdpte & PAGE_MASK);
    uint64_t alias_pde = alias_pd[alias_pd_index];
    if (!(alias_pde & PROSENJIT_PAGE_PRESENT) ||
        (alias_pde & LARGE_PAGE_FLAG))
        return -27;

    uint32_t free_index = ENTRIES;
    for (uint32_t i = 0u; i < ENTRIES; ++i)
        if (i != alias_pml4_index && kernel_pml4[i] == 0u) {
            free_index = i;
            break;
        }
    if (free_index == ENTRIES) return -28;
    uint64_t alias_data = physical_page_alloc();
    if (!alias_data ||
        paging_map_page(hierarchy_test_va, alias_data,
                        PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        if (alias_data) physical_page_free(alias_data);
        return -29;
    }
    const uint64_t alias_va = hierarchy_test_va;
    const uint64_t alias_pages[1] = {alias_data};
    const uint64_t alias_vas[1] = {alias_va};

    kernel_pml4[free_index] = alias_pml4e;
    int alias_rc = paging_unmap_pages_expected_and_free(
        alias_vas, alias_pages, 1u, alias_pages, 1u);
    kernel_pml4[free_index] = 0u;
    if (alias_rc == 0 || paging_translate(alias_va) != alias_data ||
        !physical_page_is_allocated(alias_data)) {
        paging_unmap_page(alias_va);
        physical_page_free(alias_data);
        return -30;
    }

    free_index = ENTRIES;
    for (uint32_t i = 0u; i < ENTRIES; ++i)
        if (i != alias_pdpt_index && alias_pdpt[i] == 0u) {
            free_index = i;
            break;
        }
    if (free_index == ENTRIES) {
        paging_unmap_page(alias_va);
        physical_page_free(alias_data);
        return -31;
    }
    alias_pdpt[free_index] = alias_pdpte;
    alias_rc = paging_unmap_pages_expected_and_free(
        alias_vas, alias_pages, 1u, alias_pages, 1u);
    alias_pdpt[free_index] = 0u;
    if (alias_rc == 0 || paging_translate(alias_va) != alias_data ||
        !physical_page_is_allocated(alias_data)) {
        paging_unmap_page(alias_va);
        physical_page_free(alias_data);
        return -32;
    }

    free_index = ENTRIES;
    for (uint32_t i = 0u; i < ENTRIES; ++i)
        if (i != alias_pd_index && alias_pd[i] == 0u) {
            free_index = i;
            break;
        }
    if (free_index == ENTRIES) {
        paging_unmap_page(alias_va);
        physical_page_free(alias_data);
        return -33;
    }
    alias_pd[free_index] = alias_pde;
    alias_rc = paging_unmap_pages_expected_and_free(
        alias_vas, alias_pages, 1u, alias_pages, 1u);
    alias_pd[free_index] = 0u;
    if (alias_rc == 0 || paging_translate(alias_va) != alias_data ||
        !physical_page_is_allocated(alias_data)) {
        paging_unmap_page(alias_va);
        physical_page_free(alias_data);
        return -34;
    }
    if (paging_unmap_pages_expected_and_free(
            alias_vas, alias_pages, 1u, alias_pages, 1u) != 0 ||
        paging_translate(alias_va) != 0u ||
        physical_page_is_allocated(alias_data))
        return -35;

    return 0;
}

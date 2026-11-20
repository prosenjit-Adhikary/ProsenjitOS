#ifndef PROSENJIT_PAGING_H
#define PROSENJIT_PAGING_H

#include <stdint.h>

#define PROSENJIT_PAGE_PRESENT  (1ULL << 0)
#define PROSENJIT_PAGE_WRITABLE (1ULL << 1)
#define PROSENJIT_PAGE_USER     (1ULL << 2)
#define PROSENJIT_PAGE_WRITE_THROUGH (1ULL << 3)
#define PROSENJIT_PAGE_CACHE_DISABLE (1ULL << 4)
#define PROSENJIT_PAGE_NX       (1ULL << 63)

#define PROSENJIT_KERNEL_TEST_VA 0xFFFF800000000000ULL

int paging_init(void);
/* One-shot page-table allocation failure injection for kernel self-tests. */
void paging_test_table_alloc_fail_after(uint32_t successful_allocations);
void paging_test_table_alloc_fail_clear(void);
int paging_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
/* Map device MMIO uncached in a reserved high-half virtual window. */
int paging_map_mmio_region(uint64_t physical_address, uint64_t length,
                           uint64_t *virtual_address);
void paging_unmap_mmio_region(uint64_t virtual_address, uint64_t length);
void paging_unmap_page(uint64_t virtual_address);
/* Atomically unmap up to 64 unique kernel VA->expected-PA mappings. */
/* Checked kernel translation; returns PA 0 for an absent leaf. */
int paging_query_physical_checked(uint64_t virtual_address,
                                  uint64_t *physical_address_out);
int paging_unmap_pages_expected(const uint64_t *virtual_addresses,
                                const uint64_t *expected_physical,
                                uint64_t page_count);
/* Read-only preflight for a later unmap/free commit; no PTE or PMM state changes. */
int paging_unmap_pages_expected_and_free_preflight(
    const uint64_t *virtual_addresses,
    const uint64_t *expected_physical,
    uint64_t mapped_count,
    const uint64_t *allocated_physical,
    uint64_t allocated_count);
/* Preflight and atomically unmap mappings plus free their complete owned page set. */
int paging_unmap_pages_expected_and_free(
    const uint64_t *virtual_addresses,
    const uint64_t *expected_physical,
    uint64_t mapped_count,
    const uint64_t *allocated_physical,
    uint64_t allocated_count);
uint64_t paging_translate(uint64_t virtual_address);
int paging_query_page(uint64_t virtual_address, uint64_t *flags_out);
uint64_t *paging_kernel_root(void);
int paging_switch_kernel(void);
int paging_selftest(void);

#endif

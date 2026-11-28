#ifndef PROSENJIT_MEMORY_H
#define PROSENJIT_MEMORY_H

#include <stdint.h>

struct pros_boot_v1_view;

#define PROSENJIT_PAGE_SIZE 4096ULL
#define PROSENJIT_PHYS_LIMIT (1ULL << 30)
#define PROSENJIT_PHYS_PAGE_COUNT (PROSENJIT_PHYS_LIMIT / PROSENJIT_PAGE_SIZE)
#define PROSENJIT_PHYS_BITMAP_WORDS ((PROSENJIT_PHYS_PAGE_COUNT + 63ULL) / 64ULL)

int physical_memory_init(uint64_t multiboot_info_address);
int physical_memory_init_from_boot_v1(const struct pros_boot_v1_view *view,
                                      uint64_t handoff_physical_address);
uint64_t physical_page_alloc(void);
/* Page-table pages have an explicit allocator type; generic frees reject them. */
uint64_t physical_page_alloc_table(void);
int physical_page_is_page_table(uint64_t physical_address);
int physical_page_free_table(uint64_t physical_address);
/* One-shot allocation failure injection for kernel self-tests. */
void physical_memory_test_fail_alloc_after(uint32_t successful_allocations);
void physical_memory_test_fail_clear(void);
uint64_t physical_page_alloc_user(void);
/* Allocate/free physically contiguous pages aligned to an N-page boundary. */
uint64_t physical_page_alloc_contiguous(uint64_t page_count,
                                        uint64_t alignment_pages);
int physical_page_free_contiguous(uint64_t physical_address,
                                  uint64_t page_count);
int physical_page_is_allocated(uint64_t physical_address);
int physical_page_is_user_mappable(uint64_t physical_address);
void physical_page_free(uint64_t physical_address);
/* Validate or atomically free up to 256 distinct, non-contiguous pages. */
int physical_page_free_batch_preflight(const uint64_t *physical_addresses,
                                       uint64_t page_count);
int physical_page_free_batch(const uint64_t *physical_addresses,
                             uint64_t page_count);

/*
 * Atomically release up to 256 user/data pages and any bounded set of
 * page-table pages, validating type and cross-array uniqueness together.
 */
int physical_page_free_mixed_preflight(const uint64_t *user_pages,
                                       uint64_t user_count,
                                       const uint64_t *table_pages,
                                       uint64_t table_count);
int physical_page_free_mixed(const uint64_t *user_pages,
                             uint64_t user_count,
                             const uint64_t *table_pages,
                             uint64_t table_count);

/*
 * Bitmap form for allocation-free teardown plans. Each input must contain
 * exactly PROSENJIT_PHYS_BITMAP_WORDS words; the combined unique set cannot
 * exceed the physical page count.
 */
int physical_page_free_mixed_bitmaps_preflight(
    const uint64_t *user_page_bits, const uint64_t *table_page_bits);
int physical_page_free_mixed_bitmaps(
    const uint64_t *user_page_bits, const uint64_t *table_page_bits);
uint64_t physical_memory_total_pages(void);
uint64_t physical_memory_free_pages(void);
uint64_t physical_memory_failed_allocations(void);
int physical_memory_selftest(void);

#endif

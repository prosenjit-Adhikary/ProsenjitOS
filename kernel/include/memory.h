#ifndef PROSENJIT_MEMORY_H
#define PROSENJIT_MEMORY_H

#include <stdint.h>

#define PROSENJIT_PAGE_SIZE 4096ULL
#define PROSENJIT_PHYS_LIMIT (1ULL << 30)

int physical_memory_init(uint64_t multiboot_info_address);
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
/* Atomically free up to 64 distinct, non-contiguous allocated pages. */
int physical_page_free_batch(const uint64_t *physical_addresses,
                             uint64_t page_count);
uint64_t physical_memory_total_pages(void);
uint64_t physical_memory_free_pages(void);
uint64_t physical_memory_failed_allocations(void);
int physical_memory_selftest(void);

#endif

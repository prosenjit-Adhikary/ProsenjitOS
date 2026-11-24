#ifndef PROSENJIT_MEMORY_H
#define PROSENJIT_MEMORY_H

#include <stdint.h>

struct pros_boot_v1_view;

#define PROSENJIT_PAGE_SIZE 4096ULL
#define PROSENJIT_PHYS_LIMIT (1ULL << 30)

int physical_memory_init(uint64_t multiboot_info_address);
int physical_memory_init_from_boot_v1(const struct pros_boot_v1_view *view,
                                      uint64_t handoff_physical_address);
uint64_t physical_page_alloc(void);
uint64_t physical_page_alloc_user(void);
/* Allocate/free physically contiguous pages aligned to an N-page boundary. */
uint64_t physical_page_alloc_contiguous(uint64_t page_count,
                                        uint64_t alignment_pages);
int physical_page_free_contiguous(uint64_t physical_address,
                                  uint64_t page_count);
int physical_page_is_allocated(uint64_t physical_address);
int physical_page_is_user_mappable(uint64_t physical_address);
void physical_page_free(uint64_t physical_address);
uint64_t physical_memory_total_pages(void);
uint64_t physical_memory_free_pages(void);
uint64_t physical_memory_failed_allocations(void);
int physical_memory_selftest(void);

#endif

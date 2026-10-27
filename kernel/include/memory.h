#ifndef PROSENJIT_MEMORY_H
#define PROSENJIT_MEMORY_H

#include <stdint.h>

#define PROSENJIT_PAGE_SIZE 4096ULL
#define PROSENJIT_PHYS_LIMIT (1ULL << 30)

/*
 * Initialize once from a validated Multiboot2 information structure. Returns
 * zero on success; a second initialization is rejected without resetting
 * existing allocations. The current allocator manages only the first 1 GiB.
 */
int physical_memory_init(uint64_t multiboot_info_address);

/* Return a page-aligned physical address, or zero on failure/exhaustion. */
uint64_t physical_page_alloc(void);
/* As above, but explicitly classifies the allocated page as user-mappable. */
uint64_t physical_page_alloc_user(void);
int physical_page_is_allocated(uint64_t physical_address);
int physical_page_is_user_mappable(uint64_t physical_address);

/*
 * Release a page allocated by this allocator and owned by the caller. Invalid,
 * reserved, unaligned and already-free addresses are ignored. Unmapping alone
 * does not transfer page ownership; callers must obey paging/address-space
 * ownership rules before releasing a physical page.
 */
void physical_page_free(uint64_t physical_address);

uint64_t physical_memory_total_pages(void);
uint64_t physical_memory_free_pages(void);
uint64_t physical_memory_failed_allocations(void);

/* Boot-time diagnostic; not a concurrent workload test. */
int physical_memory_selftest(void);

#endif

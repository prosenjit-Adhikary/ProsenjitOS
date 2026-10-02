#ifndef PROSENJIT_MEMORY_H
#define PROSENJIT_MEMORY_H

#include <stdint.h>

#define PROSENJIT_PAGE_SIZE 4096ULL
#define PROSENJIT_PHYS_LIMIT (1ULL << 30)

int physical_memory_init(uint64_t multiboot_info_address);
int physical_memory_reserve(uint64_t base, uint64_t length);
uint64_t physical_page_alloc(void);
void physical_page_free(uint64_t physical_address);
uint64_t physical_memory_total_pages(void);
uint64_t physical_memory_free_pages(void);
uint64_t physical_memory_failed_allocations(void);

#endif

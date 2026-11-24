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

/* Install the boot CPU's validated exclusive MAXPHYADDR limit before MMIO maps. */
int paging_set_physical_limit(uint64_t exclusive_limit);
int paging_init(void);
int paging_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
/* Map device MMIO uncached in a reserved high-half virtual window. */
int paging_map_mmio_region(uint64_t physical_address, uint64_t length,
                           uint64_t *virtual_address);
void paging_unmap_mmio_region(uint64_t virtual_address, uint64_t length);
void paging_unmap_page(uint64_t virtual_address);
uint64_t paging_translate(uint64_t virtual_address);
int paging_query_page(uint64_t virtual_address, uint64_t *flags_out);
uint64_t *paging_kernel_root(void);
int paging_switch_kernel(void);
int paging_selftest(void);

#endif

#ifndef PROSENJIT_HEAP_H
#define PROSENJIT_HEAP_H

#include <stdint.h>

#define PROSENJIT_HEAP_BASE 0xFFFF800000100000ULL
#define PROSENJIT_HEAP_MAX  (16ULL * 1024ULL * 1024ULL)

int heap_init(void);
void *kmalloc(uint64_t size);
void kfree(void *address);
/* Validate an exact live heap allocation without dereferencing an untrusted pointer. */
int heap_is_live_allocation(const void *address, uint64_t minimum_size);
uint64_t heap_used_bytes(void);
int heap_selftest(void);
/* Inject a one-shot failure after N successful page mappings in heap growth. */
void heap_test_growth_fail_after(uint32_t successful_mappings);
int heap_test_growth_fail_was_triggered(void);

/* One-shot allocation fault injection, restricted to kernel self-tests. */
void heap_test_fail_after(uint32_t successful_allocations);
void heap_test_fail_clear(void);
int heap_test_fail_was_triggered(void);

#endif

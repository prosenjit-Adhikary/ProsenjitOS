#ifndef PROSENJIT_HEAP_H
#define PROSENJIT_HEAP_H

#include <stdint.h>

#define PROSENJIT_HEAP_BASE 0xFFFF800000100000ULL
#define PROSENJIT_HEAP_MAX  (16ULL * 1024ULL * 1024ULL)

int heap_init(void);
void *kmalloc(uint64_t size);
void kfree(void *address);
uint64_t heap_used_bytes(void);
int heap_selftest(void);

/* One-shot allocation fault injection, restricted to kernel self-tests. */
void heap_test_fail_after(uint32_t successful_allocations);
void heap_test_fail_clear(void);
int heap_test_fail_was_triggered(void);

#endif

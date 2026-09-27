#ifndef PROSENJIT_HEAP_H
#define PROSENJIT_HEAP_H

#include <stdint.h>

#define PROSENJIT_HEAP_BASE 0xFFFF800000100000ULL
#define PROSENJIT_HEAP_MAX  (16ULL * 1024ULL * 1024ULL)

int heap_init(void);
void *kmalloc(uint64_t size);
void kfree(void *address);
uint64_t heap_used_bytes(void);

#endif

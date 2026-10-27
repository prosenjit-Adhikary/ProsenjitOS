#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_BYTES 4096ULL
#define HEAP_TEST_PAGES 8u
#define PHYS_POOL_PAGES 32u
#define MAPPING_SLOTS 16u

_Alignas(4096) static uint8_t host_heap_arena[HEAP_TEST_PAGES * PAGE_BYTES];

#include "../kernel/include/heap.h"
#undef PROSENJIT_HEAP_BASE
#define PROSENJIT_HEAP_BASE ((uint64_t)(uintptr_t)host_heap_arena)
#undef PROSENJIT_HEAP_MAX
#define PROSENJIT_HEAP_MAX ((uint64_t)sizeof(host_heap_arena))
#include "../kernel/heap.c"

struct phys_slot { uint64_t address; int allocated; };
struct mapping { uint64_t virtual_address; uint64_t physical_address; int present; };

static struct phys_slot physical_pool[PHYS_POOL_PAGES];
static struct mapping mappings[MAPPING_SLOTS];
static int alloc_fail_after = -1;
static int map_fail_after = -1;
static unsigned int bad_frees;
static unsigned int bad_unmaps;

static int check(int condition, const char *label) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    printf("PASS: %s\n", label);
    return 0;
}
static unsigned int allocated_pages(void) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < PHYS_POOL_PAGES; ++i)
        if (physical_pool[i].allocated) ++count;
    return count;
}
static unsigned int mapped_pages(void) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < MAPPING_SLOTS; ++i)
        if (mappings[i].present) ++count;
    return count;
}
uint64_t physical_page_alloc(void) {
    if (alloc_fail_after == 0) { alloc_fail_after = -1; return 0; }
    if (alloc_fail_after > 0) --alloc_fail_after;
    for (unsigned int i = 0; i < PHYS_POOL_PAGES; ++i) {
        if (!physical_pool[i].allocated) {
            physical_pool[i].allocated = 1;
            return physical_pool[i].address;
        }
    }
    return 0;
}
void physical_page_free(uint64_t physical_address) {
    for (unsigned int i = 0; i < PHYS_POOL_PAGES; ++i) {
        if (physical_pool[i].address != physical_address) continue;
        if (!physical_pool[i].allocated) { ++bad_frees; return; }
        physical_pool[i].allocated = 0;
        return;
    }
    ++bad_frees;
}
int paging_map_page(uint64_t virtual_address, uint64_t physical_address,
                    uint64_t flags) {
    (void)flags;
    if (map_fail_after == 0) { map_fail_after = -1; return -1; }
    if (map_fail_after > 0) --map_fail_after;
    for (unsigned int i = 0; i < MAPPING_SLOTS; ++i)
        if (mappings[i].present && mappings[i].virtual_address == virtual_address)
            return -1;
    for (unsigned int i = 0; i < MAPPING_SLOTS; ++i) {
        if (!mappings[i].present) {
            mappings[i].virtual_address = virtual_address;
            mappings[i].physical_address = physical_address;
            mappings[i].present = 1;
            return 0;
        }
    }
    return -1;
}
void paging_unmap_page(uint64_t virtual_address) {
    for (unsigned int i = 0; i < MAPPING_SLOTS; ++i) {
        if (mappings[i].present && mappings[i].virtual_address == virtual_address) {
            mappings[i].present = 0;
            return;
        }
    }
    ++bad_unmaps;
}
uint64_t paging_translate(uint64_t virtual_address) {
    uint64_t page = virtual_address & ~(PAGE_BYTES - 1ULL);
    uint64_t offset = virtual_address & (PAGE_BYTES - 1ULL);
    for (unsigned int i = 0; i < MAPPING_SLOTS; ++i)
        if (mappings[i].present && mappings[i].virtual_address == page)
            return mappings[i].physical_address + offset;
    return 0;
}
int main(void) {
    int failures = 0;
    memset(physical_pool, 0, sizeof(physical_pool));
    memset(mappings, 0, sizeof(mappings));
    for (unsigned int i = 0; i < PHYS_POOL_PAGES; ++i)
        physical_pool[i].address = (uint64_t)(i + 1u) * PAGE_BYTES;
    failures += check(heap_init() == 0, "heap initializes on host test arena");

    alloc_fail_after = 2;
    failures += check(kmalloc(3u * PAGE_BYTES) == 0,
                      "allocation failure after partial heap growth returns NULL");
    failures += check(heap_end == PROSENJIT_HEAP_BASE &&
                      mapped_pages() == 0 && allocated_pages() == 0,
                      "allocation failure rolls back the virtual frontier and all frames");

    map_fail_after = 2;
    failures += check(kmalloc(3u * PAGE_BYTES) == 0,
                      "mapping failure after partial heap growth returns NULL");
    failures += check(heap_end == PROSENJIT_HEAP_BASE &&
                      mapped_pages() == 0 && allocated_pages() == 0,
                      "mapping failure rolls back every successful page mapping");
    failures += check(bad_frees == 0 && bad_unmaps == 0,
                      "rollback avoids double frees and unmaps only live mappings");

    void *a = kmalloc(64u);
    failures += check(a != NULL && heap_end == PROSENJIT_HEAP_BASE + PAGE_BYTES &&
                      mapped_pages() == 1 && allocated_pages() == 1,
                      "successful growth commits a mapped heap page");
    kfree(a);
    void *b = kmalloc(64u);
    failures += check(b == a && mapped_pages() == 1 && allocated_pages() == 1,
                      "free-list reuse keeps committed pages mapped without leaks");
    kfree(b);

    if (failures) {
        fprintf(stderr, "%d heap growth regression test(s) failed\n", failures);
        return 1;
    }
    puts("All heap growth regression tests passed.");
    return 0;
}

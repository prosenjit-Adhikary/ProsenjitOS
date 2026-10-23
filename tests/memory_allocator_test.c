#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "memory.h"

char __kernel_start[1];
char __kernel_end[1];

#define THREADS 8u
#define PAGES_PER_THREAD 512u
#define TOTAL_ALLOCATIONS (THREADS * PAGES_PER_THREAD)

struct mmap_tag_header { uint32_t type, size, entry_size, entry_version; };
struct mmap_entry { uint64_t base_addr, length; uint32_t type, reserved; };
struct end_tag { uint32_t type, size; };
struct multiboot_fixture {
    uint32_t total_size, reserved;
    struct mmap_tag_header mmap;
    struct mmap_entry entry;
    struct end_tag end;
} __attribute__((packed));

static uint64_t allocated[TOTAL_ALLOCATIONS];
static uint32_t next_slot;
static int allocation_failed;

static void *allocator_worker(void *unused) {
    (void)unused;
    for (uint32_t i = 0; i < PAGES_PER_THREAD; ++i) {
        uint64_t page = physical_page_alloc();
        uint32_t slot = __atomic_fetch_add(&next_slot, 1u, __ATOMIC_RELAXED);
        if (!page || slot >= TOTAL_ALLOCATIONS) {
            __atomic_store_n(&allocation_failed, 1, __ATOMIC_RELAXED);
            if (page) physical_page_free(page);
            continue;
        }
        allocated[slot] = page;
    }
    return NULL;
}

int main(void) {
    struct multiboot_fixture fixture = {
        .total_size = sizeof(struct multiboot_fixture),
        .reserved = 0,
        .mmap = { .type = 6u, .size = 40u, .entry_size = 24u, .entry_version = 0u },
        .entry = { .base_addr = 0x100000u, .length = 63u * 1024u * 1024u, .type = 1u, .reserved = 0u },
        .end = { .type = 0u, .size = 8u }
    };
    if (physical_memory_init((uint64_t)(uintptr_t)&fixture) != 0) {
        fputs("allocator test: physical memory init failed\n", stderr);
        return 1;
    }
    if (physical_memory_selftest() != 0) {
        fputs("allocator test: sequential invariant self-test failed\n", stderr);
        return 2;
    }

    uint64_t before = physical_memory_free_pages();
    pthread_t threads[THREADS];
    for (uint32_t i = 0; i < THREADS; ++i) {
        if (pthread_create(&threads[i], NULL, allocator_worker, NULL) != 0) {
            fputs("allocator test: pthread_create failed\n", stderr);
            return 3;
        }
    }
    for (uint32_t i = 0; i < THREADS; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            fputs("allocator test: pthread_join failed\n", stderr);
            return 4;
        }
    }
    if (allocation_failed || next_slot != TOTAL_ALLOCATIONS) {
        fputs("allocator test: allocation failed during concurrent workload\n", stderr);
        return 5;
    }
    for (uint32_t i = 0; i < TOTAL_ALLOCATIONS; ++i) {
        if (!physical_page_is_allocated(allocated[i])) {
            fputs("allocator test: allocated page not marked allocated\n", stderr);
            return 6;
        }
        for (uint32_t j = i + 1u; j < TOTAL_ALLOCATIONS; ++j) {
            if (allocated[i] == allocated[j]) {
                fputs("allocator test: duplicate physical page allocation\n", stderr);
                return 7;
            }
        }
    }
    for (uint32_t i = 0; i < TOTAL_ALLOCATIONS; ++i)
        physical_page_free(allocated[i]);

    if (physical_memory_free_pages() != before) {
        fputs("allocator test: free-page count did not recover\n", stderr);
        return 8;
    }
    /* Reject a memory-map tag whose payload ends with a partial entry. */
    fixture.mmap.entry_size = 25u;
    if (physical_memory_init((uint64_t)(uintptr_t)&fixture) != -4) {
        fputs("allocator test: malformed partial memory-map entry was accepted\\n", stderr);
        return 9;
    }

    puts("physical page allocator concurrency tests passed");
    return 0;
}

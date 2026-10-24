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
static uint64_t contiguous_allocated[THREADS];
static uint32_t contiguous_next_slot;
static int contiguous_allocation_failed;

static void *contiguous_allocator_worker(void *unused) {
    (void)unused;
    uint64_t run = physical_pages_alloc_contiguous(4u, 4u,
                                                    8ULL * 1024ULL * 1024ULL);
    uint32_t slot = __atomic_fetch_add(&contiguous_next_slot, 1u, __ATOMIC_RELAXED);
    if (!run || slot >= THREADS) {
        __atomic_store_n(&contiguous_allocation_failed, 1, __ATOMIC_RELAXED);
        if (run) (void)physical_pages_free_contiguous(run, 4u);
        return NULL;
    }
    contiguous_allocated[slot] = run;
    return NULL;
}

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
        .entry = { .base_addr = 0x100000u, .length = PROSENJIT_PHYS_LIMIT + 4ULL * 1024ULL * 1024ULL - 0x100000ULL, .type = 1u, .reserved = 0u },
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

    /* Managed RAM includes both free and allocated pages; reserved low
     * memory and physical ranges above the allocator's 1 GiB scope are not managed. */
    if (physical_range_is_managed(0x200000u, PROSENJIT_PAGE_SIZE) != 1 ||
        physical_range_is_managed(0x1000u, PROSENJIT_PAGE_SIZE) != 0 ||
        physical_range_is_managed(PROSENJIT_PHYS_LIMIT, PROSENJIT_PAGE_SIZE) != 0 ||
        physical_range_is_managed(UINT64_MAX - 0xFFFu, 0x2000u) != -1 ||
        physical_range_is_managed(0x200000u, 0u) != -1) {
        fputs("allocator test: managed physical range query contract failed\n", stderr);
        return 18;
    }

    if (physical_range_is_mmio_safe(0x200000u, PROSENJIT_PAGE_SIZE) != 0 ||
        physical_range_is_mmio_safe((uint64_t)(uintptr_t)&fixture, sizeof(fixture)) != 0 ||
        physical_range_is_mmio_safe(0x1000u, PROSENJIT_PAGE_SIZE) != 1 ||
        physical_range_is_mmio_safe(PROSENJIT_PHYS_LIMIT, PROSENJIT_PAGE_SIZE) != 0 ||
        physical_range_is_mmio_safe(0xFEC00000ULL, PROSENJIT_PAGE_SIZE) != 1 ||
        physical_range_is_mmio_safe(UINT64_MAX - 0xFFFu, 0x2000u) != -1 ||
        physical_range_is_mmio_safe(0x200000u, 0u) != -1) {
        fputs("allocator test: MMIO-safe physical range query contract failed\n", stderr);
        return 19;
    }

    uint64_t before = physical_memory_free_pages();
    uint64_t failed_before = physical_memory_failed_allocations();
    if (physical_pages_alloc_contiguous(1u, 1u, PROSENJIT_PAGE_SIZE) != 0 ||
        physical_memory_free_pages() != before ||
        physical_memory_failed_allocations() != failed_before + 1u) {
        fputs("allocator test: exclusive address ceiling rejection contract failed\n", stderr);
        return 17;
    }
    failed_before = physical_memory_failed_allocations();
    uint64_t run = physical_pages_alloc_contiguous(4u, 4u, 8ULL * 1024ULL * 1024ULL);
    if (!run || (run & ((4u * PROSENJIT_PAGE_SIZE) - 1ULL)) != 0 ||
        run + 4u * PROSENJIT_PAGE_SIZE > 8ULL * 1024ULL * 1024ULL ||
        physical_memory_free_pages() != before - 4u) {
        fputs("allocator test: contiguous allocation alignment/limit/count failed\n", stderr);
        return 10;
    }
    for (uint64_t i = 0; i < 4u; ++i) {
        if (!physical_page_is_allocated(run + i * PROSENJIT_PAGE_SIZE)) {
            fputs("allocator test: contiguous page was not allocated\n", stderr);
            return 11;
        }
    }
    /* Individual free must not break ownership of a contiguous run. */
    physical_page_free(run + PROSENJIT_PAGE_SIZE);
    if (!physical_page_is_allocated(run + PROSENJIT_PAGE_SIZE) ||
        physical_memory_free_pages() != before - 4u) {
        fputs("allocator test: individual free broke contiguous ownership\n", stderr);
        return 12;
    }
    /* Wrong base, wrong length and partial release must be atomic failures. */
    if (physical_pages_free_contiguous(run + PROSENJIT_PAGE_SIZE, 3u) == 0 ||
        physical_pages_free_contiguous(run, 3u) == 0 ||
        physical_pages_free_contiguous(run, 5u) == 0 ||
        physical_memory_free_pages() != before - 4u) {
        fputs("allocator test: invalid contiguous release changed allocator state\n", stderr);
        return 13;
    }
    for (uint64_t i = 0; i < 4u; ++i) {
        if (!physical_page_is_allocated(run + i * PROSENJIT_PAGE_SIZE)) {
            fputs("allocator test: rejected release partially freed the run\n", stderr);
            return 14;
        }
    }
    if (physical_pages_free_contiguous(run, 4u) != 0 ||
        physical_memory_free_pages() != before) {
        fputs("allocator test: contiguous release did not restore free count\n", stderr);
        return 15;
    }
    if (physical_pages_free_contiguous(run, 4u) == 0) {
        fputs("allocator test: double release of contiguous run was accepted\n", stderr);
        return 16;
    }
    if (physical_pages_alloc_contiguous(4u, 3u, 8ULL * 1024ULL * 1024ULL) != 0 ||
        physical_memory_failed_allocations() != failed_before + 1u) {
        fputs("allocator test: non-power-of-two alignment rejection contract failed\n", stderr);
        return 25;
    }
    failed_before = physical_memory_failed_allocations();
    if (physical_pages_alloc_contiguous(0u, 1u, 8ULL * 1024ULL * 1024ULL) != 0 ||
        physical_memory_failed_allocations() != failed_before + 1u) {
        fputs("allocator test: zero-page rejection contract failed\n", stderr);
        return 26;
    }
    failed_before = physical_memory_failed_allocations();
    if (physical_pages_alloc_contiguous(1u, 1u, PROSENJIT_PAGE_SIZE) != 0 ||
        physical_memory_failed_allocations() != failed_before + 1u) {
        fputs("allocator test: exclusive ceiling rejection contract failed\n", stderr);
        return 27;
    }
    failed_before = physical_memory_failed_allocations();
    if (physical_pages_alloc_contiguous((uint64_t)UINT16_MAX + 1u, 1u,
                                        PROSENJIT_PHYS_LIMIT) != 0 ||
        physical_memory_failed_allocations() != failed_before + 1u) {
        fputs("allocator test: oversized allocation rejection contract failed\n", stderr);
        return 28;
    }
    if (physical_memory_free_pages() != before) {
        fputs("allocator test: rejected request changed free-page count\n", stderr);
        return 29;
    }
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

    /* Concurrent contiguous allocations must be aligned, unique and releasable. */
    pthread_t contiguous_threads[THREADS];
    contiguous_next_slot = 0;
    contiguous_allocation_failed = 0;
    for (uint32_t i = 0; i < THREADS; ++i) {
        if (pthread_create(&contiguous_threads[i], NULL,
                           contiguous_allocator_worker, NULL) != 0) {
            fputs("allocator test: contiguous pthread_create failed\n", stderr);
            return 18;
        }
    }
    for (uint32_t i = 0; i < THREADS; ++i) {
        if (pthread_join(contiguous_threads[i], NULL) != 0) {
            fputs("allocator test: contiguous pthread_join failed\n", stderr);
            return 19;
        }
    }
    if (contiguous_allocation_failed || contiguous_next_slot != THREADS ||
        physical_memory_free_pages() != before - (THREADS * 4u)) {
        fputs("allocator test: concurrent contiguous allocation failed\n", stderr);
        return 20;
    }
    for (uint32_t i = 0; i < THREADS; ++i) {
        uint64_t a0 = contiguous_allocated[i];
        uint64_t a1 = a0 + 4u * PROSENJIT_PAGE_SIZE;
        if ((a0 & ((4u * PROSENJIT_PAGE_SIZE) - 1ULL)) != 0 ||
            a1 > 8ULL * 1024ULL * 1024ULL) {
            fputs("allocator test: concurrent run alignment/limit failed\n", stderr);
            return 21;
        }
        for (uint32_t j = i + 1u; j < THREADS; ++j) {
            uint64_t b0 = contiguous_allocated[j];
            uint64_t b1 = b0 + 4u * PROSENJIT_PAGE_SIZE;
            if (a0 < b1 && b0 < a1) {
                fputs("allocator test: concurrent contiguous runs overlap\n", stderr);
                return 22;
            }
        }
    }
    for (uint32_t i = 0; i < THREADS; ++i) {
        if (physical_pages_free_contiguous(contiguous_allocated[i], 4u) != 0) {
            fputs("allocator test: concurrent contiguous release failed\n", stderr);
            return 23;
        }
    }
    if (physical_memory_free_pages() != before) {
        fputs("allocator test: contiguous concurrent free count did not recover\n", stderr);
        return 24;
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

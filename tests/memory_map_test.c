#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define PROSENJIT_MEMORY_HOST_TEST 1
#include "../kernel/memory.c"

/* These linker symbols are outside the allocator's 1 GiB managed range. */
char __kernel_start[1];
char __kernel_end[1];

struct test_info_header {
    uint32_t total_size;
    uint32_t reserved;
};

struct test_tag {
    uint32_t type;
    uint32_t size;
};

struct test_mmap_tag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct test_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

static _Alignas(8) uint8_t info[256];

static void begin_info(void) {
    memset(info, 0, sizeof(info));
    ((struct test_info_header *)info)->total_size = 8u;
}

static uint32_t append_map(const struct test_mmap_entry *entries,
                           uint32_t count, uint32_t entry_version) {
    uint32_t offset = ((struct test_info_header *)info)->total_size;
    struct test_mmap_tag *tag = (struct test_mmap_tag *)(info + offset);
    tag->type = 6u;
    tag->entry_size = (uint32_t)sizeof(struct test_mmap_entry);
    tag->entry_version = entry_version;
    tag->size = 16u + count * (uint32_t)sizeof(struct test_mmap_entry);
    memcpy(info + offset + 16u, entries,
           count * sizeof(struct test_mmap_entry));
    ((struct test_info_header *)info)->total_size = offset + tag->size;
    return offset + tag->size;
}

static void append_end(uint32_t offset) {
    struct test_tag *tag = (struct test_tag *)(info + offset);
    tag->type = 0u;
    tag->size = 8u;
    ((struct test_info_header *)info)->total_size = offset + 8u;
}

static int check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 1;
    }
    printf("PASS: %s\n", message);
    return 0;
}

#define ALLOC_THREADS 8
#define ALLOCS_PER_THREAD 64

struct concurrent_worker {
    uint64_t pages[ALLOCS_PER_THREAD];
    int failed;
};

static void *concurrent_allocate(void *opaque) {
    struct concurrent_worker *worker = (struct concurrent_worker *)opaque;
    for (uint32_t i = 0; i < ALLOCS_PER_THREAD; ++i) {
        worker->pages[i] = physical_page_alloc();
        if (worker->pages[i] == 0) worker->failed = 1;
    }
    return NULL;
}

static void *concurrent_free(void *opaque) {
    struct concurrent_worker *worker = (struct concurrent_worker *)opaque;
    for (uint32_t i = 0; i < ALLOCS_PER_THREAD; ++i)
        if (worker->pages[i] != 0) physical_page_free(worker->pages[i]);
    return NULL;
}

int main(void) {
    int failures = 0;
    const struct test_mmap_entry overlapping[] = {
        { 0x00100000ULL, 0x00700000ULL, 1u, 0u },
        { 0x00200000ULL, 0x00100000ULL, 2u, 0u }
    };

    begin_info();
    uint32_t end_offset = append_map(overlapping, 2u, 0u);
    append_end(end_offset);
    failures += check(physical_memory_init((uint64_t)(uintptr_t)info) == 0,
                      "valid Multiboot2 map initializes");
    failures += check(physical_memory_total_pages() == 1536u,
                      "reserved overlap overrides available range");
    failures += check(physical_memory_free_pages() == 1536u,
                      "free-page accounting excludes reserved overlap");
    uint64_t first_page = physical_page_alloc();
    failures += check(first_page == 0x00100000ULL,
                      "allocator returns first available page");

    uint64_t before_reinit = physical_memory_free_pages();
    begin_info();
    end_offset = append_map(overlapping, 2u, 0u);
    append_end(end_offset);
    failures += check(physical_memory_init((uint64_t)(uintptr_t)info) == -6,
                      "reinitialization cannot reset live allocator state");
    failures += check(physical_memory_free_pages() == before_reinit,
                      "rejected reinitialization preserves free-page accounting");

    uint64_t user_page = physical_page_alloc_user();
    failures += check(user_page != 0 &&
                      physical_page_is_allocated(user_page) &&
                      physical_page_is_user_mappable(user_page),
                      "user allocation sets allocated and user-mappable state");
    failures += check(!physical_page_is_user_mappable(user_page + 1u) &&
                      !physical_page_is_user_mappable(0x40000000ULL),
                      "invalid user-mapping queries are rejected safely");
    if (user_page) {
        uint64_t before_user_free = physical_memory_free_pages();
        physical_page_free(user_page);
        failures += check(!physical_page_is_user_mappable(user_page) &&
                          !physical_page_is_allocated(user_page) &&
                          physical_memory_free_pages() == before_user_free + 1u,
                          "free clears user classification and restores accounting");
        physical_page_free(user_page);
        failures += check(physical_memory_free_pages() == before_user_free + 1u,
                          "double free does not inflate free-page accounting");
    }

    uint64_t free_before_invalid = physical_memory_free_pages();
    physical_page_free(0);
    physical_page_free(0x1000ULL);
    physical_page_free(0x00100001ULL);
    physical_page_free(0x40000000ULL);
    failures += check(physical_memory_free_pages() == free_before_invalid,
                      "page-zero, reserved, unaligned and out-of-range frees are ignored");

    uint64_t allocated[2048];
    uint64_t allocated_count = 0;
    uint64_t page;
    while ((page = physical_page_alloc()) != 0 &&
           allocated_count < (sizeof(allocated) / sizeof(allocated[0]))) {
        allocated[allocated_count++] = page;
    }
    failures += check(physical_memory_free_pages() == 0,
                      "allocator exhausts available pages without underflow");
    uint64_t failed_before = physical_memory_failed_allocations();
    failures += check(physical_page_alloc() == 0 &&
                      physical_memory_free_pages() == 0 &&
                      physical_memory_failed_allocations() == failed_before + 1u,
                      "allocation failure preserves accounting");
    for (uint64_t i = 0; i < allocated_count; ++i)
        physical_page_free(allocated[i]);
    physical_page_free(first_page);
    failures += check(physical_memory_free_pages() == physical_memory_total_pages(),
                      "repeated allocation/free cycle restores all managed pages");

    /*
     * Concurrent allocation must never hand the same page to two callers.
     * Keep every page allocated until all workers have joined, then free in
     * parallel and verify exact accounting recovery.
     */
    {
        pthread_t threads[ALLOC_THREADS];
        struct concurrent_worker workers[ALLOC_THREADS];
        uint64_t baseline = physical_memory_free_pages();
        memset(workers, 0, sizeof(workers));
        for (uint32_t i = 0; i < ALLOC_THREADS; ++i)
            if (pthread_create(&threads[i], NULL, concurrent_allocate,
                               &workers[i]) != 0) return 2;
        for (uint32_t i = 0; i < ALLOC_THREADS; ++i)
            if (pthread_join(threads[i], NULL) != 0) return 2;

        int unique = 1;
        int all_allocated = 1;
        for (uint32_t i = 0; i < ALLOC_THREADS; ++i) {
            if (workers[i].failed) all_allocated = 0;
            for (uint32_t j = 0; j < ALLOCS_PER_THREAD; ++j) {
                uint64_t page = workers[i].pages[j];
                if (page == 0 || !physical_page_is_allocated(page))
                    all_allocated = 0;
                for (uint32_t k = 0; k <= i; ++k) {
                    uint32_t limit = (k == i) ? j : ALLOCS_PER_THREAD;
                    for (uint32_t n = 0; n < limit; ++n)
                        if (workers[k].pages[n] == page) unique = 0;
                }
            }
        }
        failures += check(all_allocated, "concurrent allocation returns valid allocated pages");
        failures += check(unique, "concurrent allocation never returns duplicate pages");
        for (uint32_t i = 0; i < ALLOC_THREADS; ++i)
            if (pthread_create(&threads[i], NULL, concurrent_free,
                               &workers[i]) != 0) return 2;
        for (uint32_t i = 0; i < ALLOC_THREADS; ++i)
            if (pthread_join(threads[i], NULL) != 0) return 2;
        failures += check(physical_memory_free_pages() == baseline,
                          "concurrent free restores exact free-page accounting");
    }

    /* Exhaust the finite managed pool and prove failure is non-destructive. */
    {
        uint64_t pages[2048];
        uint64_t baseline = physical_memory_free_pages();
        uint64_t failed_before = physical_memory_failed_allocations();
        uint32_t count = 0;
        while (count < (uint32_t)(sizeof(pages) / sizeof(pages[0]))) {
            uint64_t page = physical_page_alloc();
            if (page == 0) break;
            pages[count++] = page;
        }
        failures += check(count == baseline && physical_memory_free_pages() == 0,
                          "allocator exhausts exactly the remaining managed pages");
        failures += check(physical_page_alloc() == 0 &&
                          physical_memory_failed_allocations() > failed_before,
                          "exhaustion fails safely and records failed allocation");
        physical_page_free(0);
        physical_page_free(1);
        physical_page_free(0x40000000ULL);
        failures += check(physical_memory_free_pages() == 0,
                          "reserved, unaligned and out-of-range frees are ignored");
        for (uint32_t i = 0; i < count; ++i) physical_page_free(pages[i]);
        failures += check(physical_memory_free_pages() == baseline,
                          "freeing the exhausted pool restores exact accounting");
    }

    /* Repeated cycles must not leak pages or drift accounting. */
    {
        uint64_t baseline = physical_memory_free_pages();
        int cycles_ok = 1;
        for (uint32_t i = 0; i < 1000; ++i) {
            uint64_t page = physical_page_alloc();
            if (page == 0 || !physical_page_is_allocated(page)) {
                cycles_ok = 0;
                break;
            }
            physical_page_free(page);
            if (physical_page_is_allocated(page)) {
                cycles_ok = 0;
                break;
            }
        }
        failures += check(cycles_ok && physical_memory_free_pages() == baseline,
                          "repeated allocation/free cycles preserve accounting");
    }

    begin_info();
    struct test_mmap_tag *oversized =
        (struct test_mmap_tag *)(info + sizeof(struct test_info_header));
    oversized->type = 6u;
    oversized->size = 40u;
    oversized->entry_size = (uint32_t)sizeof(struct test_mmap_entry);
    oversized->entry_version = 0u;
    ((struct test_info_header *)info)->total_size = 32u;
    failures += check(physical_memory_init((uint64_t)(uintptr_t)info) == -3,
                      "mmap tag extending beyond info boundary is rejected");

    begin_info();
    oversized = (struct test_mmap_tag *)(info + sizeof(struct test_info_header));
    oversized->type = 6u;
    oversized->size = 41u;
    oversized->entry_size = (uint32_t)sizeof(struct test_mmap_entry);
    oversized->entry_version = 0u;
    ((struct test_info_header *)info)->total_size = 64u;
    append_end(56u);
    failures += check(physical_memory_init((uint64_t)(uintptr_t)info) == -4,
                      "mmap payload with a partial entry is rejected");

    begin_info();
    struct test_tag *truncated =
        (struct test_tag *)(info + sizeof(struct test_info_header));
    truncated->type = 6u;
    truncated->size = 40u;
    ((struct test_info_header *)info)->total_size = 24u;
    failures += check(physical_memory_init((uint64_t)(uintptr_t)info) == -3,
                      "truncated tag is rejected");

    begin_info();
    end_offset = append_map(overlapping, 2u, 0u);
    ((struct test_info_header *)info)->total_size = end_offset;
    failures += check(physical_memory_init((uint64_t)(uintptr_t)info) == -3,
                      "missing end tag is rejected");

    begin_info();
    end_offset = append_map(overlapping, 2u, 1u);
    append_end(end_offset);
    failures += check(physical_memory_init((uint64_t)(uintptr_t)info) == -4,
                      "unsupported mmap entry version is rejected");

    begin_info();
    struct test_tag *zero_size =
        (struct test_tag *)(info + sizeof(struct test_info_header));
    zero_size->type = 1u;
    zero_size->size = 0u;
    ((struct test_info_header *)info)->total_size = 16u;
    failures += check(physical_memory_init((uint64_t)(uintptr_t)info) == -3,
                      "zero-sized tag is rejected");

    begin_info();
    struct test_tag *bad_end =
        (struct test_tag *)(info + sizeof(struct test_info_header));
    bad_end->type = 0u;
    bad_end->size = 16u;
    ((struct test_info_header *)info)->total_size = 24u;
    failures += check(physical_memory_init((uint64_t)(uintptr_t)info) == -3,
                      "end tag with invalid size is rejected");

    begin_info();
    struct test_mmap_tag *bad_stride =
        (struct test_mmap_tag *)(info + sizeof(struct test_info_header));
    bad_stride->type = 6u;
    bad_stride->size = 16u;
    bad_stride->entry_size = 0u;
    bad_stride->entry_version = 0u;
    ((struct test_info_header *)info)->total_size = 32u;
    append_end(24u);
    failures += check(physical_memory_init((uint64_t)(uintptr_t)info) == -4,
                      "zero-sized mmap entry stride is rejected");

    if (failures != 0) {
        fprintf(stderr, "%d memory-map regression test(s) failed\n", failures);
        return 1;
    }
    puts("All Multiboot2 memory-map regression tests passed.");
    return 0;
}

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
    failures += check(physical_page_alloc() == 0x00100000ULL,
                      "allocator returns first available page");

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

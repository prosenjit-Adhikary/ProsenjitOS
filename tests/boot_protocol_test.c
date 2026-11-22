#include "abi/boot.h"

#include <stdio.h>
#include <string.h>

static uint8_t block[256] __attribute__((aligned(8)));

static void make_valid(void)
{
    struct pros_boot_v1_header *h = (struct pros_boot_v1_header *)block;
    struct pros_boot_v1_mmap_entry *m;
    memset(block, 0, sizeof(block));
    h->magic = PROSENJIT_BOOT_V1_MAGIC;
    h->version = PROSENJIT_BOOT_V1_VERSION;
    h->header_size = PROSENJIT_BOOT_V1_HEADER_SIZE;
    h->total_size = 152u;
    h->self_phys = 0x200000u;
    h->mmap_offset = 128u;
    h->mmap_count = 1u;
    h->mmap_entry_size = 24u;
    m = (struct pros_boot_v1_mmap_entry *)(block + 128u);
    m->base = 0x100000u;
    m->length = 0x100000u;
    m->type = PROSENJIT_BOOT_MEMORY_USABLE;
}

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

int main(void)
{
    struct pros_boot_v1_view view;
    struct pros_boot_v1_mmap_entry *m;
    make_valid();
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) == 0);
    CHECK(view.header->mmap_count == 1u && view.modules == 0 && view.boot_device == 0);

    ((struct pros_boot_v1_header *)block)->magic = 0u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->total_size = 127u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->self_phys = 0x200001u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->mmap_offset = 129u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->mmap_count = 0xffffffffu;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    m = (struct pros_boot_v1_mmap_entry *)(block + 128u);
    m->length = 0u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    m = (struct pros_boot_v1_mmap_entry *)(block + 128u);
    m->base = UINT64_MAX - 3u;
    m->length = 8u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    m = (struct pros_boot_v1_mmap_entry *)(block + 128u);
    m->attributes = 1u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->reserved[0] = 1u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->total_size = 151u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    puts("Boot Protocol v1 structural parser tests passed.");
    return 0;
}

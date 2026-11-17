#include "pros_boot_protocol_v1_parser.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static union {
    uint64_t align;
    uint8_t bytes[176];
} fixture;

static void make_valid(void)
{
    memset(&fixture, 0, sizeof(fixture));
    struct pros_boot_v1_header *h =
        (struct pros_boot_v1_header *)fixture.bytes;
    h->magic = PROS_BOOT_V1_MAGIC;
    h->version = PROS_BOOT_V1_VERSION;
    h->header_size = PROS_BOOT_V1_HEADER_SIZE;
    h->total_size = sizeof(fixture.bytes);
    h->self_phys = UINT64_C(0x200000);
    h->mmap_offset = 128;
    h->mmap_count = 2;
    h->mmap_entry_size = PROS_BOOT_V1_MMAP_ENTRY_SIZE;

    struct pros_boot_v1_mmap_entry *m =
        (struct pros_boot_v1_mmap_entry *)(fixture.bytes + 128);
    m[0].base = UINT64_C(0x200000);
    m[0].length = UINT64_C(0x1000);
    m[0].type = 2; /* reserved handoff storage */
    m[1].base = UINT64_C(0x201000);
    m[1].length = UINT64_C(0x100000);
    m[1].type = PROS_BOOT_V1_MEMORY_USABLE;
}

#define EXPECT_FAIL() do { \
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes), \
            UINT64_C(0x200000), UINT64_C(0x100000), \
            UINT64_C(0x110000), NULL, &view) == 0) return __LINE__; \
} while (0)

int main(void)
{
    struct pros_boot_v1_view view;
    make_valid();
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), &view) != 0 ||
        view.header == NULL || view.memory_map == NULL ||
        view.modules != NULL || view.boot_device != NULL)
        return 1;

    make_valid();
    ((struct pros_boot_v1_header *)fixture.bytes)->magic = 0;
    EXPECT_FAIL();

    make_valid();
    ((struct pros_boot_v1_header *)fixture.bytes)->total_size = UINT32_MAX;
    EXPECT_FAIL();

    make_valid();
    ((struct pros_boot_v1_header *)fixture.bytes)->self_phys++;
    EXPECT_FAIL();

    make_valid();
    ((struct pros_boot_v1_header *)fixture.bytes)->mmap_offset = 129;
    EXPECT_FAIL();

    make_valid();
    ((struct pros_boot_v1_header *)fixture.bytes)->reserved[0] = 1;
    EXPECT_FAIL();

    make_valid();
    ((struct pros_boot_v1_mmap_entry *)(fixture.bytes + 128))[0].type =
        PROS_BOOT_V1_MEMORY_USABLE;
    EXPECT_FAIL();

    make_valid();
    ((struct pros_boot_v1_mmap_entry *)(fixture.bytes + 128))[1].base =
        UINT64_C(0x200800);
    EXPECT_FAIL();

    make_valid();
    ((struct pros_boot_v1_header *)fixture.bytes)->framebuffer_phys =
        UINT64_C(0xE0000000);
    EXPECT_FAIL();

    make_valid();
    ((struct pros_boot_v1_header *)fixture.bytes)->acpi_rsdp_phys =
        UINT64_C(0x300000);
    ((struct pros_boot_v1_header *)fixture.bytes)->acpi_rsdp_size = 36;
    EXPECT_FAIL();

    (void)puts("ProsenjitOS: Boot Protocol v1 parser negative tests passed");
    return 0;
}

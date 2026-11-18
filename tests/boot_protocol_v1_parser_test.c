#include "pros_boot_protocol_v1_parser.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static union {
    uint64_t align;
    uint8_t bytes[256];
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
    h->mmap_count = 3;
    h->mmap_entry_size = PROS_BOOT_V1_MMAP_ENTRY_SIZE;

    struct pros_boot_v1_mmap_entry *m =
        (struct pros_boot_v1_mmap_entry *)(fixture.bytes + 128);
    m[0].base = UINT64_C(0x200000);
    m[0].length = UINT64_C(0x1000);
    m[0].type = 2; /* reserved handoff storage */
    m[1].base = UINT64_C(0x201000);
    m[1].length = UINT64_C(0x100000);
    m[1].type = PROS_BOOT_V1_MEMORY_USABLE;
    m[2].base = UINT64_C(0x400000);
    m[2].length = UINT64_C(0x1000);
    m[2].type = 2; /* reserved module storage */
}

#define EXPECT_FAIL() do { \
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes), \
            UINT64_C(0x200000), UINT64_C(0x100000), \
            UINT64_C(0x110000), NULL, 0u, &view) == 0) return __LINE__; \
} while (0)

int main(void)
{
    struct pros_boot_v1_view view;
    memset(&view, 0xA5, sizeof(view));
    if (pros_boot_v1_validate(NULL, 0u, UINT64_C(0x200000),
            UINT64_C(0x100000), UINT64_C(0x110000),
            NULL, 0u, &view) == 0 ||
        view.header != NULL || view.memory_map != NULL ||
        view.modules != NULL || view.boot_device != NULL)
        return 32;

    make_valid();
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) != 0 ||
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

    /* Usable RAM must never overlap the live kernel image. */
    make_valid();
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x202000),
            UINT64_C(0x203000), NULL, 0u, &view) == 0)
        return 33;

    make_valid();
    ((struct pros_boot_v1_header *)fixture.bytes)->acpi_rsdp_phys =
        UINT64_C(0x300000);
    ((struct pros_boot_v1_header *)fixture.bytes)->acpi_rsdp_size = 36;
    EXPECT_FAIL();


    /* A module is accepted only inside reserved, non-overlapping storage. */
    make_valid();
    struct pros_boot_v1_header *mh =
        (struct pros_boot_v1_header *)fixture.bytes;
    mh->modules_offset = 200;
    mh->modules_count = 1;
    mh->module_entry_size = PROS_BOOT_V1_MODULE_ENTRY_SIZE;
    struct pros_boot_v1_module_entry *mod =
        (struct pros_boot_v1_module_entry *)(fixture.bytes + 200);
    mod->start = UINT64_C(0x400000);
    mod->length = UINT64_C(0x1000);
    mod->type = PROS_BOOT_V1_MODULE_INITRAMFS;
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) != 0 ||
        view.modules == NULL || view.modules[0].start != mod->start)
        return 22;

    /* Unknown mandatory module types are fatal. */
    make_valid();
    mh = (struct pros_boot_v1_header *)fixture.bytes;
    mh->modules_offset = 200;
    mh->modules_count = 1;
    mh->module_entry_size = PROS_BOOT_V1_MODULE_ENTRY_SIZE;
    mod = (struct pros_boot_v1_module_entry *)(fixture.bytes + 200);
    mod->start = UINT64_C(0x400000);
    mod->length = UINT64_C(0x1000);
    mod->type = 99;
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) == 0)
        return 23;

    /* Unknown optional module types may be skipped after range validation. */
    mod->flags = PROS_BOOT_V1_MODULE_OPTIONAL;
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) != 0)
        return 24;

    /* Optional modules still require reserved ownership and valid flags. */
    mod->start = UINT64_C(0x300000); /* usable RAM, not reserved */
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) == 0)
        return 25;
    mod->start = UINT64_C(0x400000);
    mod->flags = UINT32_C(2);
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) == 0)
        return 26;

    /* A second module may not alias an already accepted module range. */
    make_valid();
    mh = (struct pros_boot_v1_header *)fixture.bytes;
    mh->modules_offset = 200;
    mh->modules_count = 2;
    mh->module_entry_size = PROS_BOOT_V1_MODULE_ENTRY_SIZE;
    mod = (struct pros_boot_v1_module_entry *)(fixture.bytes + 200);
    mod[0].start = UINT64_C(0x400000);
    mod[0].length = UINT64_C(0x1000);
    mod[0].type = PROS_BOOT_V1_MODULE_INITRAMFS;
    mod[1] = mod[0];
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) == 0)
        return 27;

    /* Boot-device bytes must occupy a disjoint, aligned in-allocation region. */
    make_valid();
    mh = (struct pros_boot_v1_header *)fixture.bytes;
    mh->boot_device_offset = 200;
    mh->boot_device_size = 8;
    mh->boot_device_kind = PROS_BOOT_V1_DEVICE_OPAQUE_V1;
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) != 0 ||
        view.boot_device != fixture.bytes + 200)
        return 28;

    /* Device payload aliasing the mandatory memory map must be rejected. */
    mh->boot_device_offset = 128;
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) == 0)
        return 29;

    /* Allocation physical alignment is part of the handoff contract. */
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200004), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) == 0)
        return 30;

    /* A mapped ACPI 2.0 RSDP must pass both legacy and extended checksums. */
    uint8_t rsdp[36] = {0};
    make_valid();
    memcpy(rsdp, "RSD PTR ", 8);
    rsdp[15] = 2;
    rsdp[20] = 36;
    uint8_t sum = 0;
    for (size_t i = 0; i < 20; ++i) sum = (uint8_t)(sum + rsdp[i]);
    rsdp[9] = (uint8_t)(0u - sum);
    sum = 0;
    for (size_t i = 0; i < sizeof(rsdp); ++i) sum = (uint8_t)(sum + rsdp[i]);
    rsdp[32] = (uint8_t)(0u - sum);
    ((struct pros_boot_v1_header *)fixture.bytes)->acpi_rsdp_phys =
        UINT64_C(0x200200);
    ((struct pros_boot_v1_header *)fixture.bytes)->acpi_rsdp_size = 36;
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), rsdp, sizeof(rsdp), &view) != 0)
        return 20;

    /* A mapping shorter than the untrusted handoff length must fail. */
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), rsdp, sizeof(rsdp) - 1u, &view) == 0)
        return 31;

    rsdp[32]++;
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), rsdp, sizeof(rsdp), &view) == 0)
        return 21;

    /* Adjacent reserved records may jointly cover the handoff allocation. */
    make_valid();
    struct pros_boot_v1_header *bh =
        (struct pros_boot_v1_header *)fixture.bytes;
    bh->mmap_count = 4;
    struct pros_boot_v1_mmap_entry *bm =
        (struct pros_boot_v1_mmap_entry *)(fixture.bytes + 128);
    bm[0].length = UINT64_C(0x80);
    bm[1].base = UINT64_C(0x200080);
    bm[1].length = UINT64_C(0xF80);
    bm[1].type = 2;
    bm[2].base = UINT64_C(0x201000);
    bm[2].length = UINT64_C(0x100000);
    bm[2].type = PROS_BOOT_V1_MEMORY_USABLE;
    bm[3].base = UINT64_C(0x400000);
    bm[3].length = UINT64_C(0x1000);
    bm[3].type = 2;
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x400000),
            UINT64_C(0x400100), NULL, 0u, &view) != 0)
        return 34; /* reserved kernel pages are permitted */

    /* A one-byte hole between reserved records must not be treated as covered. */
    bm[1].base = UINT64_C(0x200081);
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x400000),
            UINT64_C(0x400100), NULL, 0u, &view) == 0)
        return 35;

    /* Reject a memory-map interval whose exclusive end overflows uint64_t. */
    make_valid();
    struct pros_boot_v1_mmap_entry *overflow_map =
        (struct pros_boot_v1_mmap_entry *)(fixture.bytes + 128);
    overflow_map[2].base = UINT64_MAX - UINT64_C(0x7);
    overflow_map[2].length = UINT64_C(0x10);
    if (pros_boot_v1_validate(fixture.bytes, sizeof(fixture.bytes),
            UINT64_C(0x200000), UINT64_C(0x100000),
            UINT64_C(0x110000), NULL, 0u, &view) == 0)
        return 36;

    (void)puts("ProsenjitOS: Boot Protocol v1 parser validation tests passed");
    return 0;
}

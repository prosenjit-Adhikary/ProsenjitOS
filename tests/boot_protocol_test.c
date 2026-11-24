#include "abi/boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t block[256] __attribute__((aligned(8)));

static void set_checksum(uint8_t *bytes, size_t length, size_t checksum_offset)
{
    uint8_t sum = 0u;
    bytes[checksum_offset] = 0u;
    for (size_t i = 0u; i < length; ++i)
        sum = (uint8_t)(sum + bytes[i]);
    bytes[checksum_offset] = (uint8_t)(0u - sum);
}

static void make_rsdp_v0(uint8_t rsdp[20])
{
    memset(rsdp, 0, 20u);
    memcpy(rsdp, "RSD PTR ", 8u);
    rsdp[15] = 0u;
    set_checksum(rsdp, 20u, 8u);
}

static void make_rsdp_v2(uint8_t *rsdp, size_t length)
{
    memset(rsdp, 0, length);
    memcpy(rsdp, "RSD PTR ", 8u);
    rsdp[15] = 2u;
    rsdp[20] = (uint8_t)length;
    rsdp[21] = (uint8_t)(length >> 8);
    rsdp[22] = (uint8_t)(length >> 16);
    rsdp[23] = (uint8_t)(length >> 24);
    set_checksum(rsdp, 20u, 8u);
    set_checksum(rsdp, length, 32u);
}

static void make_valid(void)
{
    struct pros_boot_v1_header *h = (struct pros_boot_v1_header *)block;
    struct pros_boot_v1_mmap_entry *m;
    memset(block, 0, sizeof(block));
    h->magic = PROSENJIT_BOOT_V1_MAGIC;
    h->version = PROSENJIT_BOOT_V1_VERSION;
    h->header_size = PROSENJIT_BOOT_V1_HEADER_SIZE;
    h->total_size = 176u;
    h->self_phys = 0x200000u;
    h->mmap_offset = 128u;
    h->mmap_count = 2u;
    h->mmap_entry_size = 24u;
    m = (struct pros_boot_v1_mmap_entry *)(block + 128u);
    m[0].base = 0x100000u;
    m[0].length = 0x100000u;
    m[0].type = PROSENJIT_BOOT_MEMORY_USABLE;
    m[1].base = 0x200000u;
    m[1].length = 0x1000u;
    m[1].type = 2u;
}

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

int main(void)
{
    struct pros_boot_v1_view view;
    struct pros_boot_v1_mmap_entry *m;
    make_valid();
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) == 0);
    CHECK(view.header->mmap_count == 2u && view.modules == 0 && view.boot_device == 0);

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
    make_valid();
    CHECK(pros_boot_v1_parse(block, 151u, 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->total_size = 153u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->version = 2u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->flags = 1u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->mmap_offset = 120u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->mmap_count = 2u;
    ((struct pros_boot_v1_header *)block)->total_size = 176u;
    m = (struct pros_boot_v1_mmap_entry *)(block + 128u);
    m[0].base = 0x100000u; m[0].length = 0x2000u; m[0].type = 1u;
    m[1].base = 0x101000u; m[1].length = 0x1000u; m[1].type = 2u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->mmap_count = 2u;
    ((struct pros_boot_v1_header *)block)->total_size = 176u;
    m = (struct pros_boot_v1_mmap_entry *)(block + 128u);
    m[0].base = 0x200000u; m[0].length = 0x1000u; m[0].type = 1u;
    m[1].base = 0x100000u; m[1].length = 0x1000u; m[1].type = 2u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x40000000u, &view) != 0);
    make_valid();
    m = (struct pros_boot_v1_mmap_entry *)(block + 128u);
    m[1].type = PROSENJIT_BOOT_MEMORY_USABLE;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->modules_offset = 176u;
    ((struct pros_boot_v1_header *)block)->modules_count = 1u;
    ((struct pros_boot_v1_header *)block)->module_entry_size = 24u;
    ((struct pros_boot_v1_header *)block)->total_size = 200u;
    {
        struct pros_boot_v1_module_entry *mod =
            (struct pros_boot_v1_module_entry *)(block + 176u);
        mod->start = 0x300000u; mod->length = 0x2000u;
        mod->type = PROSENJIT_BOOT_MODULE_INITRAMFS;
    }
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->mmap_count = 3u;
    ((struct pros_boot_v1_header *)block)->total_size = 224u;
    m = (struct pros_boot_v1_mmap_entry *)(block + 128u);
    m[2].base = 0x300000u; m[2].length = 0x2000u; m[2].type = 2u;
    ((struct pros_boot_v1_header *)block)->modules_offset = 200u;
    ((struct pros_boot_v1_header *)block)->modules_count = 1u;
    ((struct pros_boot_v1_header *)block)->module_entry_size = 24u;
    {
        struct pros_boot_v1_module_entry *mod =
            (struct pros_boot_v1_module_entry *)(block + 200u);
        mod->start = 0x300000u; mod->length = 0x2000u;
        mod->type = PROSENJIT_BOOT_MODULE_INITRAMFS;
    }
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) == 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->modules_offset = 176u;
    ((struct pros_boot_v1_header *)block)->modules_count = 1u;
    ((struct pros_boot_v1_header *)block)->module_entry_size = 24u;
    ((struct pros_boot_v1_header *)block)->total_size = 200u;
    {
        struct pros_boot_v1_module_entry *mod =
            (struct pros_boot_v1_module_entry *)(block + 176u);
        mod->start = 0x200080u; mod->length = 0x1000u;
        mod->type = PROSENJIT_BOOT_MODULE_INITRAMFS;
    }
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->modules_offset = 176u;
    ((struct pros_boot_v1_header *)block)->modules_count = 1u;
    ((struct pros_boot_v1_header *)block)->module_entry_size = 24u;
    ((struct pros_boot_v1_header *)block)->total_size = 200u;
    {
        struct pros_boot_v1_module_entry *mod =
            (struct pros_boot_v1_module_entry *)(block + 176u);
        mod->start = 0x3ffff000u; mod->length = 0x2000u;
        mod->type = PROSENJIT_BOOT_MODULE_INITRAMFS;
    }
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->framebuffer_phys = 0x3ffff000u;
    ((struct pros_boot_v1_header *)block)->framebuffer_size = 0x2000u;
    ((struct pros_boot_v1_header *)block)->framebuffer_width = 640u;
    ((struct pros_boot_v1_header *)block)->framebuffer_height = 480u;
    ((struct pros_boot_v1_header *)block)->framebuffer_pitch = 2560u;
    ((struct pros_boot_v1_header *)block)->framebuffer_format =
        PROSENJIT_BOOT_FRAMEBUFFER_RGBX8888;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);


    /* Exact maximum mmap count is accepted with complete reserved handoff coverage. */
    {
        const uint32_t count = PROSENJIT_BOOT_V1_MAX_MMAP_ENTRIES;
        const size_t total = 128u + (size_t)count * 24u;
        uint8_t *large = (uint8_t *)calloc(1u, total);
        struct pros_boot_v1_header *h;
        struct pros_boot_v1_mmap_entry *entries;
        CHECK(large != 0);
        h = (struct pros_boot_v1_header *)large;
        h->magic = PROSENJIT_BOOT_V1_MAGIC;
        h->version = PROSENJIT_BOOT_V1_VERSION;
        h->header_size = PROSENJIT_BOOT_V1_HEADER_SIZE;
        h->total_size = (uint32_t)total;
        h->self_phys = 0x200000u;
        h->mmap_offset = 128u;
        h->mmap_count = count;
        h->mmap_entry_size = 24u;
        entries = (struct pros_boot_v1_mmap_entry *)(large + 128u);
        entries[0].base = 0x100000u; entries[0].length = 0x100000u;
        entries[0].type = PROSENJIT_BOOT_MEMORY_USABLE;
        entries[1].base = 0x200000u; entries[1].length = 0x20000u;
        entries[1].type = 2u;
        for (uint32_t i = 2u; i < count; ++i) {
            entries[i].base = 0x220000u + (uint64_t)(i - 2u) * 0x2000u;
            entries[i].length = 0x1000u;
            entries[i].type = 2u;
        }
        CHECK(pros_boot_v1_parse(large, total, 0x200000u, &view) == 0);
        free(large);
    }

    /* Exact maximum module count is accepted with bounded alias validation. */
    {
        const uint32_t count = PROSENJIT_BOOT_V1_MAX_MODULES;
        const size_t total = 176u + (size_t)count * 24u;
        uint8_t *large = (uint8_t *)calloc(1u, total);
        struct pros_boot_v1_header *h;
        struct pros_boot_v1_mmap_entry *entries;
        struct pros_boot_v1_module_entry *modules;
        CHECK(large != 0);
        h = (struct pros_boot_v1_header *)large;
        h->magic = PROSENJIT_BOOT_V1_MAGIC;
        h->version = PROSENJIT_BOOT_V1_VERSION;
        h->header_size = PROSENJIT_BOOT_V1_HEADER_SIZE;
        h->total_size = (uint32_t)total;
        h->self_phys = 0x200000u;
        h->mmap_offset = 128u;
        h->mmap_count = 2u;
        h->mmap_entry_size = 24u;
        h->modules_offset = 176u;
        h->modules_count = count;
        h->module_entry_size = 24u;
        entries = (struct pros_boot_v1_mmap_entry *)(large + 128u);
        entries[0].base = 0x100000u; entries[0].length = 0x100000u;
        entries[0].type = PROSENJIT_BOOT_MEMORY_USABLE;
        entries[1].base = 0x200000u; entries[1].length = 0x300000u;
        entries[1].type = 2u;
        modules = (struct pros_boot_v1_module_entry *)(large + 176u);
        for (uint32_t i = 0u; i < count; ++i) {
            modules[i].start = 0x300000u + (uint64_t)i * 0x1000u;
            modules[i].length = 0x1000u;
            modules[i].type = PROSENJIT_BOOT_MODULE_INITRAMFS;
        }
        CHECK(pros_boot_v1_parse(large, total, 0x200000u, &view) == 0);
        free(large);
    }

    /* Count caps reject adversarial loop bounds before any count-sized walk. */
    make_valid();
    ((struct pros_boot_v1_header *)block)->mmap_count =
        PROSENJIT_BOOT_V1_MAX_MMAP_ENTRIES + 1u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);
    make_valid();
    ((struct pros_boot_v1_header *)block)->modules_offset = 176u;
    ((struct pros_boot_v1_header *)block)->modules_count =
        PROSENJIT_BOOT_V1_MAX_MODULES + 1u;
    ((struct pros_boot_v1_header *)block)->module_entry_size = 24u;
    ((struct pros_boot_v1_header *)block)->total_size = 224u;
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);

    /* A framebuffer fully inside usable RAM must be rejected. */
    make_valid();
    {
        struct pros_boot_v1_header *h = (struct pros_boot_v1_header *)block;
        h->framebuffer_phys = 0x180000u;
        h->framebuffer_size = 0x1000u;
        h->framebuffer_width = 16u;
        h->framebuffer_height = 16u;
        h->framebuffer_pitch = 64u;
        h->framebuffer_format = PROSENJIT_BOOT_FRAMEBUFFER_RGBX8888;
    }
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);

    /* Partial overlap with usable RAM is also rejected. */
    make_valid();
    {
        struct pros_boot_v1_header *h = (struct pros_boot_v1_header *)block;
        h->framebuffer_phys = 0x1ff000u;
        h->framebuffer_size = 0x2000u;
        h->framebuffer_width = 16u;
        h->framebuffer_height = 16u;
        h->framebuffer_pitch = 64u;
        h->framebuffer_format = PROSENJIT_BOOT_FRAMEBUFFER_RGBX8888;
    }
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) != 0);

    /* A framebuffer immediately adjacent to usable RAM is valid when it
     * does not alias the handoff or any other retained object. */
    make_valid();
    {
        struct pros_boot_v1_header *h = (struct pros_boot_v1_header *)block;
        h->self_phys = 0x201000u;
        ((struct pros_boot_v1_mmap_entry *)(block + 128u))[1].length = 0x2000u;
        h->framebuffer_phys = 0x200000u;
        h->framebuffer_size = 0x1000u;
        h->framebuffer_width = 16u;
        h->framebuffer_height = 16u;
        h->framebuffer_pitch = 64u;
        h->framebuffer_format = PROSENJIT_BOOT_FRAMEBUFFER_RGBX8888;
    }
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x201000u, &view) == 0);

    /* MMIO outside every RAM-map record remains valid; do not require
     * framebuffer coverage by a reserved RAM record. */
    make_valid();
    {
        struct pros_boot_v1_header *h = (struct pros_boot_v1_header *)block;
        h->framebuffer_phys = 0x300000u;
        h->framebuffer_size = 0x1000u;
        h->framebuffer_width = 16u;
        h->framebuffer_height = 16u;
        h->framebuffer_pitch = 64u;
        h->framebuffer_format = PROSENJIT_BOOT_FRAMEBUFFER_RGBX8888;
    }
    CHECK(pros_boot_v1_parse(block, sizeof(block), 0x200000u, &view) == 0);


    /* ACPI RSDP revision 0: exact 20-byte structure and legacy checksum. */
    {
        uint8_t rsdp[20];
        make_rsdp_v0(rsdp);
        CHECK(pros_boot_v1_validate_rsdp(rsdp, sizeof(rsdp)) == 0);
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 19u) != 0);
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 21u) != 0);
        rsdp[9] ^= 1u;
        CHECK(pros_boot_v1_validate_rsdp(rsdp, sizeof(rsdp)) != 0);
    }

    /* Revision 1 is reserved; extended revisions validate both checksums. */
    {
        uint8_t rsdp[40];
        make_rsdp_v2(rsdp, 36u);
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 36u) == 0);
        make_rsdp_v2(rsdp, 40u);
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 40u) == 0);
        rsdp[0] ^= 1u;
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 40u) != 0);
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 35u) != 0);
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 40u) != 0);
        rsdp[15] = 1u;
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 36u) != 0);
        make_rsdp_v2(rsdp, 36u);
        rsdp[10] ^= 1u;
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 36u) != 0);
        make_rsdp_v2(rsdp, 36u);
        rsdp[30] ^= 1u;
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 36u) != 0);
        make_rsdp_v2(rsdp, 36u);
        rsdp[20] = 37u;
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 36u) != 0);
        make_rsdp_v2(rsdp, 36u);
        rsdp[20] = 35u;
        CHECK(pros_boot_v1_validate_rsdp(rsdp, 36u) != 0);
        CHECK(pros_boot_v1_validate_rsdp(0, 36u) != 0);
    }

    puts("Boot Protocol v1 structural parser tests passed.");
    return 0;
}

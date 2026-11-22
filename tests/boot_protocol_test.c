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
    puts("Boot Protocol v1 structural parser tests passed.");
    return 0;
}

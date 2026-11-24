#include "abi/boot.h"
#include "memory.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define HANDOFF_PHYS 0x200000ULL
#define MODULE_PHYS  0x300000ULL
#define RSDP_PHYS    0x350000ULL
#define PAGE         4096ULL

static uint8_t block[512] __attribute__((aligned(8)));

static void put_map(struct pros_boot_v1_mmap_entry *m, unsigned i,
                    uint64_t base, uint64_t length, uint32_t type)
{
    m[i].base = base;
    m[i].length = length;
    m[i].type = type;
    m[i].attributes = 0u;
}

static int make_valid_view(struct pros_boot_v1_view *view)
{
    struct pros_boot_v1_header *h = (struct pros_boot_v1_header *)block;
    struct pros_boot_v1_mmap_entry *m =
        (struct pros_boot_v1_mmap_entry *)(block + 128u);
    struct pros_boot_v1_module_entry *mod =
        (struct pros_boot_v1_module_entry *)(block + 296u);

    memset(block, 0, sizeof(block));
    h->magic = PROSENJIT_BOOT_V1_MAGIC;
    h->version = PROSENJIT_BOOT_V1_VERSION;
    h->header_size = PROSENJIT_BOOT_V1_HEADER_SIZE;
    h->total_size = 320u;
    h->self_phys = HANDOFF_PHYS;
    h->mmap_offset = 128u;
    h->mmap_count = 7u;
    h->mmap_entry_size = sizeof(*m);
    h->modules_offset = 296u;
    h->modules_count = 1u;
    h->module_entry_size = sizeof(*mod);
    h->acpi_rsdp_phys = RSDP_PHYS;
    h->acpi_rsdp_size = 36u;

    put_map(m, 0u, 0x100000u, 0x100000u, PROSENJIT_BOOT_MEMORY_USABLE);
    put_map(m, 1u, HANDOFF_PHYS, PAGE, 2u);
    put_map(m, 2u, 0x201000u, 0x0ff000u, PROSENJIT_BOOT_MEMORY_USABLE);
    put_map(m, 3u, MODULE_PHYS, PAGE, 2u);
    put_map(m, 4u, 0x301000u, 0x04f000u, PROSENJIT_BOOT_MEMORY_USABLE);
    put_map(m, 5u, RSDP_PHYS, PAGE, 2u);
    put_map(m, 6u, 0x351000u, 0x0af000u, PROSENJIT_BOOT_MEMORY_USABLE);

    mod->start = MODULE_PHYS;
    mod->length = PAGE;
    mod->type = PROSENJIT_BOOT_MODULE_INITRAMFS;
    mod->flags = 0u;

    return pros_boot_v1_parse_with_limit(block, sizeof(block), HANDOFF_PHYS,
                                         1ULL << 36, view);
}

static int is_usable_page(uint64_t p)
{
    if (p >= 0x100000u && p < HANDOFF_PHYS) return 1;
    if (p >= 0x201000u && p < MODULE_PHYS) return 1;
    if (p >= 0x301000u && p < RSDP_PHYS) return 1;
    if (p >= 0x351000u && p < 0x400000u) return 1;
    return 0;
}

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

int main(void)
{
    struct pros_boot_v1_view view;
    CHECK(make_valid_view(&view) == 0);
    CHECK(physical_memory_init_from_boot_v1(&view, HANDOFF_PHYS) == 0);
    CHECK(physical_memory_total_pages() != 0u);

    uint64_t allocated = 0u;
    for (;;) {
        uint64_t p = physical_page_alloc();
        if (p == 0u) break;
        CHECK((p & (PAGE - 1u)) == 0u);
        CHECK(is_usable_page(p));
        CHECK(p != HANDOFF_PHYS);
        CHECK(p != MODULE_PHYS);
        CHECK(p != RSDP_PHYS);
        ++allocated;
    }
    CHECK(allocated == physical_memory_total_pages());
    CHECK(physical_memory_free_pages() == 0u);

    /* A malformed borrowed view must be rejected before allocator state is
     * reset or any newly advertised pages become allocatable. */
    CHECK(make_valid_view(&view) == 0);
    ((struct pros_boot_v1_mmap_entry *)(block + 128u))[2].base =\n        ((struct pros_boot_v1_mmap_entry *)(block + 128u))[1].base;
    CHECK(physical_memory_init_from_boot_v1(&view, HANDOFF_PHYS) != 0);
    CHECK(physical_memory_free_pages() == 0u);

    puts("Boot Protocol v1 PMM reservation tests passed.");
    return 0;
}

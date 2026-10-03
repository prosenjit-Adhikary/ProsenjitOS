#include "elf.h"
#include "memory.h"
#include "paging.h"

#define ELF_IDENT_SIZE 16u
#define ELF_CLASS_64 2u
#define ELF_DATA_LSB 1u
#define ELF_VERSION_CURRENT 1u
#define ELF_OSABI_SYSV 0u
#define ELF_PAGE_MASK (~(PROSENJIT_PAGE_SIZE - 1ULL))
#define ELF_MAX_PHNUM 128u

struct elf64_ehdr {
    uint8_t ident[ELF_IDENT_SIZE];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} __attribute__((packed));

static int range_ok(uint64_t start, uint64_t size, uint64_t limit)
{
    return start <= limit && size <= limit - start;
}

static int canonical_user(uint64_t va)
{
    return (va >> 47) == 0u;
}

static int header_ok(const struct elf64_ehdr *eh, uint64_t size)
{
    if (!eh || size < sizeof(*eh)) return 0;
    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != ELF_CLASS_64 || eh->ident[5] != ELF_DATA_LSB ||
        eh->ident[6] != ELF_VERSION_CURRENT ||
        eh->ident[7] != ELF_OSABI_SYSV)
        return 0;
    if ((eh->type != PROS_ELF_ET_EXEC && eh->type != PROS_ELF_ET_DYN) ||
        eh->machine != PROS_ELF_EM_X86_64 ||
        eh->version != ELF_VERSION_CURRENT ||
        eh->ehsize != sizeof(*eh) ||
        eh->phentsize != sizeof(struct elf64_phdr) ||
        eh->phnum == 0u || eh->phnum > ELF_MAX_PHNUM)
        return 0;
    if (!range_ok(eh->phoff,
                  (uint64_t)eh->phnum * sizeof(struct elf64_phdr), size))
        return 0;
    if (!canonical_user(eh->entry) || eh->entry == 0u)
        return 0;
    return 1;
}

static int validate_load_segments(const struct elf64_ehdr *eh,
                                  const uint8_t *image, uint64_t size,
                                  uint64_t *start_out, uint64_t *end_out,
                                  uint32_t *count_out)
{
    uint64_t start = UINT64_MAX, end = 0u;
    uint32_t count = 0u;

    for (uint32_t i = 0u; i < eh->phnum; ++i) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(image + eh->phoff +
                                        (uint64_t)i * sizeof(*ph));
        if (ph->type != PROS_ELF_PT_LOAD) continue;
        if (ph->memsz == 0u || ph->filesz > ph->memsz ||
            !range_ok(ph->offset, ph->filesz, size))
            return PROS_ELF_EFORMAT;
        if (ph->align != 0u &&
            (ph->align & (ph->align - 1u)) != 0u)
            return PROS_ELF_EFORMAT;
        if (ph->align > 1u &&
            ((ph->vaddr - ph->offset) & (ph->align - 1u)) != 0u)
            return PROS_ELF_EFORMAT;
        if (!canonical_user(ph->vaddr) || ph->vaddr == 0u)
            return PROS_ELF_EFORMAT;
        if (!range_ok(ph->vaddr, ph->memsz, 0x0000800000000000ULL))
            return PROS_ELF_EOVERFLOW;
        uint64_t seg_end = ph->vaddr + ph->memsz;
        uint64_t seg_start_page = ph->vaddr & ELF_PAGE_MASK;
        uint64_t seg_end_page = (seg_end + PROSENJIT_PAGE_SIZE - 1ULL) &
                                ELF_PAGE_MASK;
        if (seg_end_page < seg_end || seg_end_page == 0u)
            return PROS_ELF_EOVERFLOW;
        if (start == UINT64_MAX || seg_start_page < start) start = seg_start_page;
        if (seg_end_page > end) end = seg_end_page;
        ++count;
    }

    if (count == 0u || start == UINT64_MAX || end <= start)
        return PROS_ELF_EFORMAT;
    if (eh->entry < start || eh->entry >= end)
        return PROS_ELF_EFORMAT;
    *start_out = start;
    *end_out = end;
    *count_out = count;
    return PROS_ELF_OK;
}

int pros_elf64_validate(const void *image, uint64_t image_size,
                        uint64_t *entry_out)
{
    if (!image || !entry_out) return PROS_ELF_EINVAL;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    if (!header_ok(eh, image_size)) return PROS_ELF_EFORMAT;
    int rc = validate_load_segments(eh, (const uint8_t *)image, image_size,
                                    &(uint64_t){0}, &(uint64_t){0}, &(uint32_t){0});
    if (rc != PROS_ELF_OK) return rc;
    *entry_out = eh->entry;
    return PROS_ELF_OK;
}

static int page_range_overlap(uint64_t a_start, uint64_t a_end,
                              uint64_t b_start, uint64_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

int pros_elf64_load(struct pros_address_space *space,
                    const void *image, uint64_t image_size,
                    struct pros_elf_load_result *result)
{
    if (!space || !space->root || !image || !result)
        return PROS_ELF_EINVAL;

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    if (!header_ok(eh, image_size)) return PROS_ELF_EFORMAT;

    uint64_t image_start, image_end;
    uint32_t segment_count;
    int rc = validate_load_segments(eh, (const uint8_t *)image, image_size,
                                    &image_start, &image_end, &segment_count);
    if (rc != PROS_ELF_OK) return rc;

    for (uint32_t i = 0u; i < eh->phnum; ++i) {
        const struct elf64_phdr *a =
            (const struct elf64_phdr *)((const uint8_t *)image + eh->phoff +
                                        (uint64_t)i * sizeof(*a));
        if (a->type != PROS_ELF_PT_LOAD) continue;
        uint64_t a_end = a->vaddr + a->memsz;
        uint64_t a_start_page = a->vaddr & ELF_PAGE_MASK;
        uint64_t a_end_page = (a_end + PROSENJIT_PAGE_SIZE - 1ULL) & ELF_PAGE_MASK;

        for (uint32_t j = i + 1u; j < eh->phnum; ++j) {
            const struct elf64_phdr *b =
                (const struct elf64_phdr *)((const uint8_t *)image + eh->phoff +
                                            (uint64_t)j * sizeof(*b));
            if (b->type != PROS_ELF_PT_LOAD) continue;
            uint64_t b_end = b->vaddr + b->memsz;
            uint64_t b_start_page = b->vaddr & ELF_PAGE_MASK;
            uint64_t b_end_page = (b_end + PROSENJIT_PAGE_SIZE - 1ULL) & ELF_PAGE_MASK;
            if (page_range_overlap(a_start_page, a_end_page,
                                   b_start_page, b_end_page))
                return PROS_ELF_EUNSUPPORTED;
        }
    }

    for (uint32_t i = 0u; i < eh->phnum; ++i) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)((const uint8_t *)image + eh->phoff +
                                        (uint64_t)i * sizeof(*ph));
        if (ph->type != PROS_ELF_PT_LOAD) continue;

        uint64_t seg_end = ph->vaddr + ph->memsz;
        uint64_t page_start = ph->vaddr & ELF_PAGE_MASK;
        uint64_t page_end = (seg_end + PROSENJIT_PAGE_SIZE - 1ULL) & ELF_PAGE_MASK;
        uint64_t flags = 0u;
        if (ph->flags & PROS_ELF_PF_W) flags |= PROSENJIT_PAGE_WRITABLE;
        if (!(ph->flags & PROS_ELF_PF_X)) flags |= PROSENJIT_PAGE_NX;

        for (uint64_t va = page_start; va < page_end; va += PROSENJIT_PAGE_SIZE) {
            uint64_t physical = physical_page_alloc();
            if (!physical) return PROS_ELF_ENOMEM;
            uint8_t *dst = (uint8_t *)(uintptr_t)physical;
            for (uint64_t k = 0u; k < PROSENJIT_PAGE_SIZE; ++k) dst[k] = 0u;

            uint64_t file_start = ph->vaddr;
            uint64_t file_end = ph->vaddr + ph->filesz;
            uint64_t copy_start = va > file_start ? va : file_start;
            uint64_t copy_end = (va + PROSENJIT_PAGE_SIZE) < file_end
                                ? (va + PROSENJIT_PAGE_SIZE) : file_end;
            if (copy_end > copy_start) {
                uint64_t src_off = ph->offset + (copy_start - ph->vaddr);
                const uint8_t *src = (const uint8_t *)image + src_off;
                uint64_t dst_off = copy_start - va;
                for (uint64_t k = 0u; k < copy_end - copy_start; ++k)
                    dst[dst_off + k] = src[k];
            }

            if (address_space_map_user_page(space, va, physical, flags) != 0) {
                physical_page_free(physical);
                return PROS_ELF_EINVAL;
            }
        }
    }

    result->entry = eh->entry;
    result->image_start = image_start;
    result->image_end = image_end;
    result->loaded_segments = segment_count;
    return PROS_ELF_OK;
}

int pros_elf64_selftest(void)
{
    uint8_t image[sizeof(struct elf64_ehdr) + sizeof(struct elf64_phdr) + 4u] = {0};
    struct elf64_ehdr *eh = (struct elf64_ehdr *)image;
    struct elf64_phdr *ph = (struct elf64_phdr *)(image + sizeof(*eh));
    uint64_t entry = 0u;

    eh->ident[0] = 0x7f; eh->ident[1] = 'E'; eh->ident[2] = 'L'; eh->ident[3] = 'F';
    eh->ident[4] = ELF_CLASS_64; eh->ident[5] = ELF_DATA_LSB;
    eh->ident[6] = ELF_VERSION_CURRENT; eh->ident[7] = ELF_OSABI_SYSV;
    eh->type = PROS_ELF_ET_EXEC; eh->machine = PROS_ELF_EM_X86_64;
    eh->version = ELF_VERSION_CURRENT; eh->entry = PROSENJIT_USER_CODE_VA;
    eh->phoff = sizeof(*eh); eh->ehsize = sizeof(*eh);
    eh->phentsize = sizeof(*ph); eh->phnum = 1u;

    ph->type = PROS_ELF_PT_LOAD; ph->flags = PROS_ELF_PF_R | PROS_ELF_PF_X;
    ph->offset = sizeof(*eh) + sizeof(*ph); ph->vaddr = PROSENJIT_USER_CODE_VA;
    ph->filesz = 4u; ph->memsz = PROSENJIT_PAGE_SIZE; ph->align = PROSENJIT_PAGE_SIZE;
    image[ph->offset] = 0xC3u; image[ph->offset + 1u] = 0x90u;
    image[ph->offset + 2u] = 0x90u; image[ph->offset + 3u] = 0x90u;

    if (pros_elf64_validate(image, sizeof(image), &entry) != PROS_ELF_OK ||
        entry != eh->entry)
        return PROS_ELF_EFORMAT;

    ph->filesz = ph->memsz + 1u;
    if (pros_elf64_validate(image, sizeof(image), &entry) == PROS_ELF_OK)
        return PROS_ELF_EFORMAT;

    return PROS_ELF_OK;
}

#include "elf_loader.h"
#include "address_space.h"
#include "memory.h"
#include "paging.h"

#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define EV_CURRENT 1u
#define ELFOSABI_SYSV 0u
#define EM_X86_64 62u
#define ET_EXEC 2u
#define ET_DYN 3u
#define PT_LOAD 1u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define ELF_PAGE 4096ULL
#define ELF_USER_MAX 0x0000800000000000ULL
#define ELF_MAX_PHNUM 128u
#define ELF_MAX_IMAGE 0x02000000ULL

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

static int range_ok(uint64_t start, uint64_t length, uint64_t limit,
                    uint64_t *end_out)
{
    if (length > limit || start > limit - length)
        return 0;
    if (end_out) *end_out = start + length;
    return 1;
}

static int canonical_user(uint64_t va)
{
    return va < ELF_USER_MAX;
}

static int phdr_table_ok(const struct elf64_ehdr *eh, uint64_t size)
{
    uint64_t bytes;
    if (eh->phnum == 0 || eh->phnum > ELF_MAX_PHNUM ||
        eh->phentsize != sizeof(struct elf64_phdr))
        return 0;
    if ((uint64_t)eh->phnum > UINT64_MAX / sizeof(struct elf64_phdr))
        return 0;
    bytes = (uint64_t)eh->phnum * sizeof(struct elf64_phdr);
    return eh->phoff <= size && bytes <= size - eh->phoff;
}

static int validate_load_segments(const uint8_t *image, uint64_t size,
                                  uint64_t *entry_out,
                                  uint64_t *image_start,
                                  uint64_t *image_end)
{
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    uint64_t start = UINT64_MAX, end = 0;
    int entry_exec = 0;
    uint32_t loads = 0;

    if (!phdr_table_ok(eh, size))
        return PROS_ELF_EBOUNDS;

    for (uint16_t i = 0; i < eh->phnum; ++i) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(image + eh->phoff +
                                        (uint64_t)i * sizeof(*ph));
        uint64_t seg_end, page_start, page_end;

        if (ph->type != PT_LOAD)
            continue;
        ++loads;

        if (ph->filesz > ph->memsz ||
            ph->offset > size || ph->filesz > size - ph->offset)
            return PROS_ELF_EBOUNDS;
        if (ph->vaddr > ELF_USER_MAX ||
            !range_ok(ph->vaddr, ph->memsz, ELF_USER_MAX, &seg_end) ||
            ph->vaddr + ph->memsz == 0)
            return PROS_ELF_EBOUNDS;
        if ((ph->flags & ~(PF_R | PF_W | PF_X)) != 0 ||
            (ph->flags & (PF_W | PF_X)) == (PF_W | PF_X))
            return PROS_ELF_EPERM;
        if (ph->align != 0 && (ph->align & (ph->align - 1)) != 0)
            return PROS_ELF_EFORMAT;
        if (ph->align > 1 &&
            ((ph->vaddr & (ph->align - 1)) !=
             (ph->offset & (ph->align - 1))))
            return PROS_ELF_EFORMAT;

        page_start = ph->vaddr & ~(ELF_PAGE - 1);
        if (seg_end > UINT64_MAX - (ELF_PAGE - 1))
            return PROS_ELF_EOVERFLOW;
        page_end = (seg_end + ELF_PAGE - 1) & ~(ELF_PAGE - 1);
        if (page_end > ELF_USER_MAX)
            return PROS_ELF_EBOUNDS;

        if (eh->entry >= ph->vaddr && eh->entry < seg_end &&
            (ph->flags & PF_X))
            entry_exec = 1;
        if (page_start < start) start = page_start;
        if (page_end > end) end = page_end;
    }

    if (loads == 0 || !entry_exec)
        return PROS_ELF_EFORMAT;
    if (!canonical_user(eh->entry))
        return PROS_ELF_EBOUNDS;
    if (entry_out) *entry_out = eh->entry;
    if (image_start) *image_start = start;
    if (image_end) *image_end = end;
    return PROS_ELF_OK;
}

static int segment_page_flags(const struct elf64_phdr *ph, uint64_t va)
{
    uint64_t end;
    if (!range_ok(ph->vaddr, ph->memsz, ELF_USER_MAX, &end))
        return -1;
    if (va < (ph->vaddr & ~(ELF_PAGE - 1)) ||
        va >= ((end + ELF_PAGE - 1) & ~(ELF_PAGE - 1)))
        return -1;
    return (ph->flags & PF_W ? PROSENJIT_PAGE_WRITABLE : 0) |
           ((ph->flags & PF_X) ? 0 : PROSENJIT_PAGE_NX);
}

int pros_elf_validate(const uint8_t *image, uint64_t size)
{
    const struct elf64_ehdr *eh;
    if (!image || size < sizeof(struct elf64_ehdr) || size > ELF_MAX_IMAGE)
        return PROS_ELF_EBOUNDS;
    eh = (const struct elf64_ehdr *)image;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != ELFCLASS64 || eh->ident[5] != ELFDATA2LSB ||
        eh->ident[6] != EV_CURRENT ||
        (eh->ident[7] != ELFOSABI_SYSV && eh->ident[7] != 3) ||
        (eh->type != ET_EXEC && eh->type != ET_DYN) ||
        eh->machine != EM_X86_64 || eh->version != EV_CURRENT ||
        eh->ehsize != sizeof(struct elf64_ehdr))
        return PROS_ELF_EFORMAT;
    return validate_load_segments(image, size, 0, 0, 0);
}

static int page_mapped(const struct pros_address_space *space, uint64_t va)
{
    return address_space_validate_user_range(space, va, ELF_PAGE, 0) == 0;
}

int pros_elf_load(struct pros_process *process, const uint8_t *image,
                  uint64_t size, struct pros_elf_load_result *result)
{
    const struct elf64_ehdr *eh;
    uint64_t start, end, entry;
    uint64_t mapped = 0;

    if (!process || !image || !result ||
        process->state == PROS_PROCESS_DEAD ||
        process->state == PROS_PROCESS_ZOMBIE)
        return PROS_ELF_EINVAL;
    if (pros_elf_validate(image, size) != PROS_ELF_OK)
        return PROS_ELF_EFORMAT;

    eh = (const struct elf64_ehdr *)image;
    (void)validate_load_segments(image, size, &entry, &start, &end);

    /* Map each page only after the complete image has passed structural checks. */
    for (uint64_t va = start; va < end; va += ELF_PAGE) {
        int flags = -1;
        for (uint16_t i = 0; i < eh->phnum; ++i) {
            const struct elf64_phdr *ph =
                (const struct elf64_phdr *)(image + eh->phoff +
                                            (uint64_t)i * sizeof(*ph));
            if (ph->type != PT_LOAD)
                continue;
            int f = segment_page_flags(ph, va);
            if (f < 0) continue;
            if (flags < 0) flags = f;
            else if (flags != f) return PROS_ELF_ECONFLICT;
        }
        if (flags < 0) continue;
        if (page_mapped(&process->address_space, va))
            return PROS_ELF_ECONFLICT;

        uint64_t physical = physical_page_alloc();
        if (!physical)
            return PROS_ELF_ENOMEM;
        for (uint64_t off = 0; off < ELF_PAGE; ++off)
            ((uint8_t *)(uintptr_t)physical)[off] = 0;
        if (address_space_map_user_page(&process->address_space, va,
                                        physical, (uint64_t)flags) != 0) {
            physical_page_free(physical);
            return PROS_ELF_EINVAL;
        }
        ++mapped;

        for (uint16_t i = 0; i < eh->phnum; ++i) {
            const struct elf64_phdr *ph =
                (const struct elf64_phdr *)(image + eh->phoff +
                                            (uint64_t)i * sizeof(*ph));
            if (ph->type != PT_LOAD || va < (ph->vaddr & ~(ELF_PAGE - 1)))
                continue;
            if (va >= ((ph->vaddr + ph->memsz + ELF_PAGE - 1) &
                       ~(ELF_PAGE - 1)))
                continue;
            uint64_t page_off = (va > ph->vaddr) ? va - ph->vaddr : 0;
            uint64_t file_off = (ph->offset > 0 && page_off < ph->offset) ? 0 :
                                ph->offset + page_off;
            uint64_t copy_len = 0;
            if (page_off < ph->filesz)
                copy_len = ph->filesz - page_off;
            if (copy_len > ELF_PAGE) copy_len = ELF_PAGE;
            if (copy_len != 0) {
                if (file_off > size || copy_len > size - file_off)
                    return PROS_ELF_EBOUNDS;
                for (uint64_t j = 0; j < copy_len; ++j)
                    ((uint8_t *)(uintptr_t)physical)[
                        (va - (va & ~(ELF_PAGE - 1))) + j] =
                        image[file_off + j];
            }
        }
    }

    result->entry = entry;
    result->image_start = start;
    result->image_end = end;
    result->stack_top = PROSENJIT_USER_STACK_TOP;
    result->mapped_pages = mapped;
    return PROS_ELF_OK;
}

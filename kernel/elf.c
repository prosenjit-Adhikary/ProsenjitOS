#include "elf.h"
#include "memory.h"

#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define EV_CURRENT 1u
#define ET_EXEC 2u
#define EM_X86_64 62u
#define PT_LOAD 1u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define ELF_PAGES_MAX 4096ULL
#define USER_BASE 0x0000008000000000ULL
#define USER_LIMIT 0x0000010000000000ULL

struct elf64_ehdr {
    uint8_t ident[16];
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

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a)
        return 1;
    *out = a + b;
    return 0;
}

static int range_in_image(uint64_t offset, uint64_t size, uint64_t image_size)
{
    return offset <= image_size && size <= image_size - offset;
}

static int is_power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1ULL)) == 0;
}

static int segments_overlap(const struct elf64_phdr *a,
                            const struct elf64_phdr *b)
{
    uint64_t a_end;
    uint64_t b_end;

    if (a->type != PT_LOAD || b->type != PT_LOAD ||
        a->memsz == 0 || b->memsz == 0)
        return 0;
    if (add_overflow_u64(a->vaddr, a->memsz, &a_end) ||
        add_overflow_u64(b->vaddr, b->memsz, &b_end))
        return 1;
    return a->vaddr < b_end && b->vaddr < a_end;
}

static int validate_segment(const struct elf64_phdr *ph,
                            uint64_t image_size, uint64_t *end_out)
{
    uint64_t end;

    if (ph->type != PT_LOAD || ph->memsz == 0)
        return PROS_ELF_OK;
    if (ph->filesz > ph->memsz ||
        !range_in_image(ph->offset, ph->filesz, image_size))
        return PROS_ELF_EBOUNDS;
    if ((ph->vaddr & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0 ||
        (ph->offset & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0)
        return PROS_ELF_EINVAL;
    if (ph->align > 1ULL) {
        if (!is_power_of_two(ph->align) ||
            (ph->vaddr & (ph->align - 1ULL)) !=
            (ph->offset & (ph->align - 1ULL)))
            return PROS_ELF_EINVAL;
    }
    if ((ph->flags & PF_W) && (ph->flags & PF_X))
        return PROS_ELF_EINVAL;
    if (ph->vaddr < USER_BASE)
        return PROS_ELF_EINVAL;
    if (add_overflow_u64(ph->vaddr, ph->memsz, &end) ||
        end > USER_LIMIT || end <= ph->vaddr)
        return PROS_ELF_EBOUNDS;
    *end_out = end;
    return PROS_ELF_OK;
}

int pros_elf64_load(const void *image, uint64_t image_size,
                    struct pros_address_space *space,
                    uint64_t *entry_out)
{
    const struct elf64_ehdr *eh;
    uint32_t load_count = 0;
    uint64_t total_pages = 0;
    uint64_t entry;
    uint16_t i;
    uint64_t allocated_pages[ELF_PAGES_MAX];
    uint64_t allocated_vas[ELF_PAGES_MAX];
    uint64_t allocated_count = 0;

    if (!image || !space || !space->root || !entry_out ||
        image_size < sizeof(struct elf64_ehdr))
        return PROS_ELF_EINVAL;

    eh = (const struct elf64_ehdr *)image;
    if (eh->ident[0] != 0x7Fu || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != ELFCLASS64 || eh->ident[5] != ELFDATA2LSB ||
        eh->ident[6] != EV_CURRENT)
        return PROS_ELF_EINVAL;
    if (eh->type != ET_EXEC || eh->machine != EM_X86_64 ||
        eh->version != EV_CURRENT || eh->ehsize != sizeof(*eh) ||
        eh->phentsize != sizeof(struct elf64_phdr) || eh->phnum == 0 ||
        eh->phnum > 128u)
        return PROS_ELF_ENOTSUP;
    if (eh->phoff > image_size ||
        (uint64_t)eh->phnum * eh->phentsize > image_size - eh->phoff)
        return PROS_ELF_EBOUNDS;

    entry = eh->entry;

    for (i = 0; i < eh->phnum; ++i) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)((const uint8_t *)image +
                                        eh->phoff + (uint64_t)i * eh->phentsize);
        uint64_t end;
        uint64_t first;
        uint64_t pages;

        if (validate_segment(ph, image_size, &end) != PROS_ELF_OK)
            return PROS_ELF_EINVAL;
        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;
        first = ph->vaddr;
        pages = (end - first + PROSENJIT_PAGE_SIZE - 1ULL) /
                PROSENJIT_PAGE_SIZE;
        if (pages > ELF_PAGES_MAX - total_pages)
            return PROS_ELF_ENOMEM;
        total_pages += pages;
        ++load_count;
    }

    /* PT_LOAD memory ranges must not overlap. Rejecting them up front keeps
     * the page ownership model deterministic and prevents one segment from
     * silently changing another segment's permissions or contents. */
    for (i = 0; i < eh->phnum; ++i) {
        const struct elf64_phdr *a =
            (const struct elf64_phdr *)((const uint8_t *)image +
                                        eh->phoff + (uint64_t)i * eh->phentsize);
        uint16_t j;
        for (j = (uint16_t)(i + 1u); j < eh->phnum; ++j) {
            const struct elf64_phdr *b =
                (const struct elf64_phdr *)((const uint8_t *)image +
                                            eh->phoff + (uint64_t)j * eh->phentsize);
            if (segments_overlap(a, b))
                return PROS_ELF_EINVAL;
        }
    }

    if (load_count == 0 || entry < USER_BASE || entry >= USER_LIMIT)
        return PROS_ELF_EINVAL;

    for (i = 0; i < eh->phnum; ++i) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)((const uint8_t *)image +
                                        eh->phoff + (uint64_t)i * eh->phentsize);
        uint64_t end;
        uint64_t va;

        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;
        if (validate_segment(ph, image_size, &end) != PROS_ELF_OK)
            return PROS_ELF_EINVAL;

        for (va = ph->vaddr; va < end; va += PROSENJIT_PAGE_SIZE) {
            uint64_t physical;
            uint64_t copy_start = va > ph->vaddr ? va : ph->vaddr;
            uint64_t seg_file_end = ph->vaddr + ph->filesz;
            uint64_t copy_end = seg_file_end < va + PROSENJIT_PAGE_SIZE ?
                                seg_file_end : va + PROSENJIT_PAGE_SIZE;
            uint64_t flags = (ph->flags & PF_W) ? PROSENJIT_PAGE_WRITABLE : 0;
            if (!(ph->flags & PF_X))
                flags |= PROSENJIT_PAGE_NX;
            if (address_space_validate_user_range(space, va, 1u, 0) == 0)
                return PROS_ELF_EINVAL;
            physical = physical_page_alloc();
            if (!physical || address_space_map_user_page(space, va, physical, flags) != 0) {
                if (physical)
                    physical_page_free(physical);
                goto load_fail;
            }
            if (allocated_count >= ELF_PAGES_MAX) {
                (void)address_space_unmap_user_page(space, va);
                physical_page_free(physical);
                goto load_fail;
            }
            allocated_pages[allocated_count] = physical;
            allocated_vas[allocated_count] = va;
            ++allocated_count;

            for (uint64_t j = 0; j < PROSENJIT_PAGE_SIZE; ++j)
                ((uint8_t *)(uintptr_t)physical)[j] = 0;

            if (copy_end > copy_start) {
                uint64_t src_offset = ph->offset + (copy_start - ph->vaddr);
                uint64_t count = copy_end - copy_start;
                for (uint64_t j = 0; j < count; ++j)
                    ((uint8_t *)(uintptr_t)physical)[copy_start - va + j] =
                        ((const uint8_t *)image)[src_offset + j];
            }
        }
    }

    *entry_out = entry;
    return PROS_ELF_OK;

load_fail:
    while (allocated_count != 0) {
        --allocated_count;
        (void)address_space_unmap_user_page(space, allocated_vas[allocated_count]);
        physical_page_free(allocated_pages[allocated_count]);
    }
    return PROS_ELF_ENOMEM;
}

int pros_elf64_selftest(void)
{
    uint8_t image[0x1100] = {0};
    struct elf64_ehdr *eh = (struct elf64_ehdr *)image;
    struct elf64_phdr *ph = (struct elf64_phdr *)(image + 64);
    struct pros_address_space space = {0};
    uint64_t entry = 0;
    eh->ident[0] = 0x7F; eh->ident[1] = 'E'; eh->ident[2] = 'L'; eh->ident[3] = 'F';
    eh->ident[4] = ELFCLASS64; eh->ident[5] = ELFDATA2LSB; eh->ident[6] = EV_CURRENT;
    eh->type = ET_EXEC; eh->machine = EM_X86_64; eh->version = EV_CURRENT;
    eh->entry = USER_BASE; eh->phoff = 64u; eh->ehsize = sizeof(*eh);
    eh->phentsize = sizeof(*ph); eh->phnum = 1u;
    ph->type = PT_LOAD; ph->flags = PF_R | PF_X; ph->offset = 0x1000u;
    ph->vaddr = USER_BASE; ph->filesz = 4u; ph->memsz = 0x1000u;
    ph->align = PROSENJIT_PAGE_SIZE;
    image[0x1000] = 0xC3; image[0x1001] = 0x90; image[0x1002] = 0xCC; image[0x1003] = 0xF4;

    if (address_space_create(&space) != 0)
        return PROS_ELF_ENOMEM;
    if (pros_elf64_load(image, sizeof(image), &space, &entry) != PROS_ELF_OK ||
        entry != USER_BASE) {
        (void)address_space_destroy(&space);
        return PROS_ELF_EINVAL;
    }
    if (address_space_switch(&space) != 0 ||
        ((volatile uint8_t *)(uintptr_t)USER_BASE)[0] != 0xC3 ||
        ((volatile uint8_t *)(uintptr_t)USER_BASE)[3] != 0xF4 ||
        ((volatile uint8_t *)(uintptr_t)(USER_BASE + 0x800))[0] != 0) {
        (void)paging_switch_kernel();
        (void)address_space_destroy(&space);
        return PROS_ELF_EINVAL;
    }
    (void)paging_switch_kernel();
    if (address_space_destroy(&space) != 0)
        return PROS_ELF_EINVAL;
    return PROS_ELF_OK;
}

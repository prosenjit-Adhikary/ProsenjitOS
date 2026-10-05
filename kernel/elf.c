#include "elf.h"

static uint64_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint64_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (rd16(p + 2) << 16); }
static uint64_t rd64(const uint8_t *p) {
    return rd32(p) | (rd32(p + 4) << 32);
}

static int range_ok(uint64_t offset, uint64_t length, uint64_t size) {
    return offset <= size && length <= size - offset;
}

int pros_elf_validate_image(const void *image, uint64_t size,
                            struct pros_elf_header64 *header_out)
{
    const uint8_t *p = (const uint8_t *)image;
    struct pros_elf_header64 h = {0};
    if (!p || size < 64u || !header_out) return PROS_ELF_EINVAL;
    if (p[0] != 0x7Fu || p[1] != 'E' || p[2] != 'L' || p[3] != 'F')
        return PROS_ELF_EFORMAT;
    if (p[4] != PROS_ELF_CLASS64 || p[5] != PROS_ELF_DATA_LSB ||
        p[6] != PROS_ELF_VERSION)
        return PROS_ELF_EFORMAT;

    h.type = (uint16_t)rd16(p + 16);
    h.machine = (uint16_t)rd16(p + 18);
    h.version = (uint32_t)rd32(p + 20);
    h.entry = rd64(p + 24);
    h.phoff = rd64(p + 32);
    h.shoff = rd64(p + 40);
    h.flags = (uint32_t)rd32(p + 48);
    h.ehsize = (uint16_t)rd16(p + 52);
    h.phentsize = (uint16_t)rd16(p + 54);
    h.phnum = (uint16_t)rd16(p + 56);
    h.shentsize = (uint16_t)rd16(p + 58);
    h.shnum = (uint16_t)rd16(p + 60);
    h.shstrndx = (uint16_t)rd16(p + 62);

    if ((h.type != PROS_ELF_ET_EXEC && h.type != PROS_ELF_ET_DYN) ||
        h.machine != PROS_ELF_MACHINE_X86_64 || h.version != PROS_ELF_VERSION ||
        h.ehsize != 64u || h.phentsize != 56u || h.phnum == 0u)
        return PROS_ELF_EFORMAT;
    if (!range_ok(h.phoff, (uint64_t)h.phnum * h.phentsize, size))
        return PROS_ELF_ERANGE;
    if (h.shnum && h.shentsize &&
        !range_ok(h.shoff, (uint64_t)h.shnum * h.shentsize, size))
        return PROS_ELF_ERANGE;

    *header_out = h;
    return PROS_ELF_OK;
}

int pros_elf_validate_load_segments(const void *image, uint64_t size,
                                    const struct pros_elf_header64 *header)
{
    const uint8_t *p = (const uint8_t *)image;
    uint64_t min_load = UINT64_MAX, max_load = 0;
    int has_load = 0, entry_ok = 0;

    if (!p || !header) return PROS_ELF_EINVAL;
    if (pros_elf_validate_image(image, size, (struct pros_elf_header64 *)header) != PROS_ELF_OK)
        return PROS_ELF_EFORMAT;

    for (uint16_t i = 0; i < header->phnum; ++i) {
        const uint8_t *q = p + header->phoff + (uint64_t)i * header->phentsize;
        uint32_t type = (uint32_t)rd32(q);
        uint32_t flags = (uint32_t)rd32(q + 4);
        uint64_t offset = rd64(q + 8), vaddr = rd64(q + 16);
        uint64_t filesz = rd64(q + 32), memsz = rd64(q + 40), align = rd64(q + 48);

        if (type != PROS_ELF_PT_LOAD) continue;
        has_load = 1;
        if (memsz < filesz || !range_ok(offset, filesz, size))
            return PROS_ELF_ERANGE;
        if (vaddr + memsz < vaddr || vaddr + memsz >= (1ULL << 47))
            return PROS_ELF_ERANGE;
        if (align && (align & (align - 1u)) != 0)
            return PROS_ELF_EFORMAT;
        if (align > 1u && ((vaddr - offset) & (align - 1u)) != 0)
            return PROS_ELF_EFORMAT;
        if ((flags & PROS_ELF_PF_W) && (flags & PROS_ELF_PF_X))
            return PROS_ELF_EFORMAT;
        if (vaddr < min_load) min_load = vaddr;
        if (vaddr + memsz > max_load) max_load = vaddr + memsz;
        if (header->entry >= vaddr && header->entry < vaddr + memsz &&
            (flags & PROS_ELF_PF_X))
            entry_ok = 1;
    }
    if (!has_load || min_load == UINT64_MAX || max_load <= min_load || !entry_ok)
        return PROS_ELF_EFORMAT;
    return PROS_ELF_OK;
}

int pros_elf_selftest(void)
{
    uint8_t image[256] = {0};
    struct pros_elf_header64 h = {0};
    image[0]=0x7F; image[1]='E'; image[2]='L'; image[3]='F';
    image[4]=2; image[5]=1; image[6]=1;
    image[16]=2; image[18]=62; image[20]=1;
    image[24]=0x00; image[25]=0x10;
    image[32]=64; image[52]=64; image[54]=56; image[56]=1;
    image[64]=1; image[68]=5;
    image[72]=0; image[80]=0x00; image[81]=0x10;
    image[96]=1; image[104]=1; image[112]=1;
    image[120]=0x10;
    if (pros_elf_validate_image(image, sizeof(image), &h) != PROS_ELF_OK) return -1;
    if (pros_elf_validate_load_segments(image, sizeof(image), &h) != PROS_ELF_OK) return -2;
    image[0] = 0;
    if (pros_elf_validate_image(image, sizeof(image), &h) != PROS_ELF_EFORMAT) return -3;
    return 0;
}

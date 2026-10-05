#include "elf.h"

#define PROS_ELF_USER_MIN 0x0000008000000000ULL
#define PROS_ELF_USER_LIMIT 0x0000800000000000ULL
#define PROS_ELF_PAGE_SIZE 4096ULL

static uint64_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint64_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (rd16(p + 2) << 16); }
static uint64_t rd64(const uint8_t *p) {
    return rd32(p) | (rd32(p + 4) << 32);
}

static int range_ok(uint64_t offset, uint64_t length, uint64_t size) {
    return offset <= size && length <= size - offset;
}

static int add_ok(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) return 0;
    if (out) *out = a + b;
    return 1;
}

static uint64_t page_down(uint64_t v) {
    return v & ~(PROS_ELF_PAGE_SIZE - 1ULL);
}

static int page_range_overlap(uint64_t a, uint64_t asz,
                              uint64_t b, uint64_t bsz) {
    uint64_t ae, be;
    if (!add_ok(a, asz, &ae) || !add_ok(b, bsz, &be)) return 1;
    a = page_down(a);
    b = page_down(b);
    ae = (ae + PROS_ELF_PAGE_SIZE - 1ULL) & ~(PROS_ELF_PAGE_SIZE - 1ULL);
    be = (be + PROS_ELF_PAGE_SIZE - 1ULL) & ~(PROS_ELF_PAGE_SIZE - 1ULL);
    return a < be && b < ae;
}

int pros_elf_validate_image(const void *image, uint64_t size,
                            struct pros_elf_header64 *header_out)
{
    const uint8_t *p = (const uint8_t *)image;
    struct pros_elf_header64 h = {0};
    uint64_t ph_bytes = 0, sh_bytes = 0;

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

    /*
     * ET_DYN/PIE is intentionally rejected until the process loader has a
     * complete load-bias contract. Loading a PIE at its unrelocated address
     * would be unsafe; failing closed is preferable to silently misloading it.
     */
    if (h.type != PROS_ELF_ET_EXEC ||
        h.machine != PROS_ELF_MACHINE_X86_64 || h.version != PROS_ELF_VERSION ||
        h.ehsize != 64u || h.phentsize != 56u || h.phnum == 0u)
        return PROS_ELF_EFORMAT;

    ph_bytes = (uint64_t)h.phnum * (uint64_t)h.phentsize;
    if (!range_ok(h.phoff, ph_bytes, size))
        return PROS_ELF_ERANGE;

    if (h.shnum != 0u) {
        if (h.shentsize == 0u) return PROS_ELF_EFORMAT;
        sh_bytes = (uint64_t)h.shnum * (uint64_t)h.shentsize;
        if (!range_ok(h.shoff, sh_bytes, size))
            return PROS_ELF_ERANGE;
        if (h.shstrndx != 0xFFFFu && h.shstrndx >= h.shnum)
            return PROS_ELF_EFORMAT;
    }

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
    struct pros_elf_header64 validated;
    if (pros_elf_validate_image(image, size, &validated) != PROS_ELF_OK)
        return PROS_ELF_EFORMAT;
    if (validated.phoff != header->phoff || validated.phnum != header->phnum ||
        validated.phentsize != header->phentsize || validated.entry != header->entry ||
        validated.type != header->type)
        return PROS_ELF_EFORMAT;

    for (uint16_t i = 0; i < header->phnum; ++i) {
        const uint8_t *q = p + header->phoff + (uint64_t)i * header->phentsize;
        uint32_t type = (uint32_t)rd32(q);
        uint32_t flags = (uint32_t)rd32(q + 4);
        uint64_t offset = rd64(q + 8), vaddr = rd64(q + 16);
        uint64_t filesz = rd64(q + 32), memsz = rd64(q + 40), align = rd64(q + 48);
        uint64_t vend = 0;

        if (type != PROS_ELF_PT_LOAD) continue;
        has_load = 1;

        if (memsz == 0 || memsz < filesz || !range_ok(offset, filesz, size))
            return PROS_ELF_ERANGE;
        if (vaddr < PROS_ELF_USER_MIN || !add_ok(vaddr, memsz, &vend) ||
            vend > PROS_ELF_USER_LIMIT)
            return PROS_ELF_ERANGE;
        if ((flags & PROS_ELF_PF_W) && (flags & PROS_ELF_PF_X))
            return PROS_ELF_EFORMAT;
        if (align != 0u && (align & (align - 1u)) != 0u)
            return PROS_ELF_EFORMAT;
        if (align > 1u && ((vaddr & (align - 1u)) != (offset & (align - 1u))))
            return PROS_ELF_EFORMAT;

        if (header->entry >= vaddr && header->entry < vend &&
            (flags & PROS_ELF_PF_X))
            entry_ok = 1;
        if (vaddr < min_load) min_load = vaddr;
        if (vend > max_load) max_load = vend;

        /* A conservative loader policy: PT_LOAD mappings may not share a
         * page. This avoids ambiguous page permissions and partial overlap. */
        for (uint16_t j = 0; j < i; ++j) {
            const uint8_t *prev = p + header->phoff +
                                  (uint64_t)j * header->phentsize;
            if ((uint32_t)rd32(prev) != PROS_ELF_PT_LOAD) continue;
            uint64_t pv = rd64(prev + 16), pm = rd64(prev + 40);
            if (page_range_overlap(vaddr, memsz, pv, pm))
                return PROS_ELF_EFORMAT;
        }
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
    image[24]=0x00; image[25]=0x00; image[26]=0x00; image[27]=0x00; image[28]=0x80;
    image[32]=64; image[52]=64; image[54]=56; image[56]=1;
    image[64]=1; image[68]=5;
    image[72]=0x00; image[80]=0x00; image[81]=0x00; image[82]=0x00; image[83]=0x80;
    image[96]=0x10; image[104]=0x10; image[112]=0x10;
    image[120]=0x10;

    if (pros_elf_validate_image(image, sizeof(image), &h) != PROS_ELF_OK) return -1;
    if (pros_elf_validate_load_segments(image, sizeof(image), &h) != PROS_ELF_OK) return -2;

    image[0] = 0;
    if (pros_elf_validate_image(image, sizeof(image), &h) != PROS_ELF_EFORMAT) return -3;
    image[0] = 0x7F;

    image[4] = 1; /* ELF32 is never accepted by the x86-64 loader. */
    if (pros_elf_validate_image(image, sizeof(image), &h) != PROS_ELF_EFORMAT) return -4;
    image[4] = 2;
    image[18] = 3; /* wrong machine */
    if (pros_elf_validate_image(image, sizeof(image), &h) != PROS_ELF_EFORMAT) return -5;
    image[18] = 62;

    if (pros_elf_validate_image(image, 100u, &h) != PROS_ELF_ERANGE) return -6;

    image[16] = 3; /* ET_DYN: explicit fail-closed policy. */
    if (pros_elf_validate_image(image, sizeof(image), &h) != PROS_ELF_EFORMAT) return -7;
    image[16] = 2;

    image[112] = 3; /* PT_LOAD alignment must be zero or a power of two. */
    if (pros_elf_validate_load_segments(image, sizeof(image), &h) != PROS_ELF_EFORMAT) return -8;
    image[112] = 0x10;

    image[68] = 3; /* PT_LOAD with W+X. */
    if (pros_elf_validate_load_segments(image, sizeof(image), &h) != PROS_ELF_EFORMAT) return -9;
    image[68] = 5;

    image[96] = 0x20; /* filesz > memsz. */
    image[104] = 0x10;
    if (pros_elf_validate_load_segments(image, sizeof(image), &h) != PROS_ELF_ERANGE) return -10;
    image[96] = 0x10;

    image[24] = 0x10; /* entry exactly at the end of the executable mapping. */
    if (pros_elf_validate_load_segments(image, sizeof(image), &h) != PROS_ELF_EFORMAT) return -11;
    image[24] = 0x00;

    /* Force vaddr + memsz to overflow rather than wrapping into user space. */
    for (uint32_t i = 0; i < 8u; ++i) image[80 + i] = 0xFFu;
    if (pros_elf_validate_load_segments(image, sizeof(image), &h) != PROS_ELF_ERANGE) return -12;

    return 0;
}

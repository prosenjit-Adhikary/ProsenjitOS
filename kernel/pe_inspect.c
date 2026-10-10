#include "pe_inspect.h"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void clear_info(struct pros_pe_image_info *out)
{
    uint8_t *p = (uint8_t *)out;
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof(*out); ++i)
        p[i] = 0;
}

static int range_ok(uint32_t off, uint32_t len, uint32_t total)
{
    return off <= total && len <= total - off;
}

int pros_pe_inspect(const uint8_t *image, uint32_t size,
                    struct pros_pe_image_info *out)
{
    uint32_t pe_off, opt_off, sec_off, i;
    uint16_t magic, opt_size, sections;
    uint32_t directory_count = 0;

    if (!out)
        return PROS_DRIVER_EINVAL;
    clear_info(out);
    if (!image || size < 0x40u)
        return PROS_DRIVER_EINVAL;
    if (image[0] != 'M' || image[1] != 'Z')
        return PROS_DRIVER_ENODEV;

    pe_off = rd32(image + 0x3cu);
    if (!range_ok(pe_off, 24u, size))
        return PROS_DRIVER_EIO;
    if (image[pe_off] != 'P' || image[pe_off + 1u] != 'E' ||
        image[pe_off + 2u] != 0 || image[pe_off + 3u] != 0)
        return PROS_DRIVER_ENODEV;

    sections = rd16(image + pe_off + 6u);
    opt_size = rd16(image + pe_off + 20u);
    if (!sections || sections > PROS_PE_MAX_SECTIONS)
        return PROS_DRIVER_ENOTSUP;

    opt_off = pe_off + 24u;
    /*
     * Subsystem occupies bytes 68..69 of the optional header. Requiring
     * 70 bytes avoids reading a field that lies outside a truncated header.
     */
    if (!range_ok(opt_off, opt_size, size) || opt_size < 70u)
        return PROS_DRIVER_EIO;

    magic = rd16(image + opt_off);
    if (magic != 0x10bu && magic != 0x20bu)
        return PROS_DRIVER_ENOTSUP;

    out->machine = rd16(image + pe_off + 4u);
    out->sections = sections;
    out->entry_rva = rd32(image + opt_off + 16u);
    out->image_size = rd32(image + opt_off + 56u);
    out->subsystem = rd16(image + opt_off + 68u);
    out->section_count = sections;
    out->is_pe32_plus = (magic == 0x20bu) ? 1u : 0u;

    /*
     * The import directory is data-directory entry 1. Only read it when
     * both NumberOfRvaAndSizes and the complete entry are inside SizeOfOptionalHeader.
     */
    if (magic == 0x10bu) {
        if (opt_size >= 96u)
            directory_count = rd32(image + opt_off + 92u);
        if (directory_count > 1u && opt_size >= 112u) {
            out->import_rva = rd32(image + opt_off + 104u);
            out->import_size = rd32(image + opt_off + 108u);
        }
    } else {
        if (opt_size >= 112u)
            directory_count = rd32(image + opt_off + 108u);
        if (directory_count > 1u && opt_size >= 128u) {
            out->import_rva = rd32(image + opt_off + 120u);
            out->import_size = rd32(image + opt_off + 124u);
        }
    }

    sec_off = opt_off + opt_size;
    if (!range_ok(sec_off, (uint32_t)sections * 40u, size))
        return PROS_DRIVER_EIO;

    for (i = 0; i < sections; ++i) {
        const uint8_t *s = image + sec_off + i * 40u;
        uint32_t raw_size = rd32(s + 16u);
        uint32_t raw_off = rd32(s + 20u);
        if (raw_size && !range_ok(raw_off, raw_size, size))
            return PROS_DRIVER_EIO;
    }

    if ((out->import_rva == 0u) != (out->import_size == 0u))
        return PROS_DRIVER_EIO;
    out->has_imports = (out->import_rva != 0u) ? 1u : 0u;

    /*
     * This is inspection only, not an execution authorization decision.
     * A future loader must validate architecture, imports, relocations,
     * section mappings, permissions, and the supported ABI separately.
     */
    if (out->image_size == 0u || out->entry_rva >= out->image_size)
        return PROS_DRIVER_EIO;

    return PROS_DRIVER_OK;
}

static void make_minimal_pe32(uint8_t *pe, uint32_t size)
{
    uint32_t p = 0x80u;
    uint32_t i;

    for (i = 0; i < size; ++i)
        pe[i] = 0;
    pe[0] = 'M';
    pe[1] = 'Z';
    pe[0x3c] = (uint8_t)p;
    pe[p] = 'P';
    pe[p + 1u] = 'E';
    pe[p + 4u] = (uint8_t)PROS_PE_MACHINE_I386;
    pe[p + 5u] = (uint8_t)(PROS_PE_MACHINE_I386 >> 8);
    pe[p + 6u] = 1u;
    pe[p + 20u] = 0xf0u;
    pe[p + 24u] = 0x0bu;
    pe[p + 25u] = 0x01u; /* PE32 (0x010b), not PE32+ (0x020b). */
    pe[p + 40u] = 0x00u;
    pe[p + 41u] = 0x10u; /* AddressOfEntryPoint = 0x1000. */
    pe[p + 80u] = 0x00u;
    pe[p + 81u] = 0x10u; /* SizeOfImage = 0x1000. */
    pe[p + 92u] = PROS_PE_SUBSYSTEM_WINDOWS_CUI;
}

int pros_pe_selftest(void)
{
    uint8_t pe[512];
    struct pros_pe_image_info info;
    uint32_t p = 0x80u;
    uint16_t saved_opt_size;

    make_minimal_pe32(pe, sizeof(pe));
    if (pros_pe_inspect(pe, sizeof(pe), &info) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (info.machine != PROS_PE_MACHINE_I386 ||
        info.sections != 1u || info.entry_rva != 0x1000u ||
        info.is_pe32_plus || info.has_imports)
        return PROS_DRIVER_EIO;

    /* A truncated optional header must not expose subsystem/import fields. */
    saved_opt_size = rd16(pe + p + 20u);
    pe[p + 20u] = 69u;
    pe[p + 21u] = 0u;
    if (pros_pe_inspect(pe, sizeof(pe), &info) != PROS_DRIVER_EIO)
        return PROS_DRIVER_EIO;
    pe[p + 20u] = (uint8_t)saved_opt_size;
    pe[p + 21u] = (uint8_t)(saved_opt_size >> 8);

    /*
     * With no complete import-directory entry in the optional header,
     * section-table bytes must not be misread as an import directory.
     */
    pe[p + 20u] = 104u;
    pe[p + 21u] = 0u;
    if (pros_pe_inspect(pe, sizeof(pe), &info) != PROS_DRIVER_OK ||
        info.has_imports)
        return PROS_DRIVER_EIO;

    /* Inconsistent RVA/size pairs are rejected. */
    pe[p + 20u] = (uint8_t)saved_opt_size;
    pe[p + 21u] = (uint8_t)(saved_opt_size >> 8);
    pe[p + 24u + 92u] = 2u; /* NumberOfRvaAndSizes = 2. */
    pe[p + 24u + 104u] = 1u; /* Import RVA nonzero, size remains zero. */
    if (pros_pe_inspect(pe, sizeof(pe), &info) != PROS_DRIVER_EIO)
        return PROS_DRIVER_EIO;

    /* A non-PE buffer must be rejected. */
    pe[0] = 'N';
    if (pros_pe_inspect(pe, sizeof(pe), &info) != PROS_DRIVER_ENODEV)
        return PROS_DRIVER_EIO;

    return PROS_DRIVER_OK;
}

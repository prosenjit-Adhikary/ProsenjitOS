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
static int range_ok(uint32_t off, uint32_t len, uint32_t total)
{
    return off <= total && len <= total - off;
}

int pros_pe_inspect(const uint8_t *image, uint32_t size,
                    struct pros_pe_image_info *out)
{
    uint32_t pe_off, opt_off, sec_off, i;
    uint16_t magic, opt_size, sections;

    if (!image || !out || size < 0x40u) return PROS_DRIVER_EINVAL;
    if (image[0] != 'M' || image[1] != 'Z') return PROS_DRIVER_ENODEV;
    pe_off = rd32(image + 0x3cu);
    if (!range_ok(pe_off, 24u, size)) return PROS_DRIVER_EIO;
    if (image[pe_off] != 'P' || image[pe_off + 1u] != 'E' ||
        image[pe_off + 2u] != 0 || image[pe_off + 3u] != 0)
        return PROS_DRIVER_ENODEV;

    sections = rd16(image + pe_off + 6u);
    opt_size = rd16(image + pe_off + 20u);
    if (!sections || sections > PROS_PE_MAX_SECTIONS) return PROS_DRIVER_ENOTSUP;
    opt_off = pe_off + 24u;
    if (!range_ok(opt_off, opt_size, size) || opt_size < 68u)
        return PROS_DRIVER_EIO;

    magic = rd16(image + opt_off);
    if (magic != 0x10bu && magic != 0x20bu) return PROS_DRIVER_ENOTSUP;

    out->machine = rd16(image + pe_off + 4u);
    out->sections = sections;
    out->entry_rva = rd32(image + opt_off + 16u);
    out->image_size = rd32(image + opt_off + 56u);
    out->subsystem = rd16(image + opt_off + 68u);
    out->import_rva = 0;
    out->import_size = 0;
    out->section_count = sections;

    if (magic == 0x10bu) {
        if (opt_size >= 104u) {
            out->import_rva = rd32(image + opt_off + 104u);
            out->import_size = rd32(image + opt_off + 108u);
        }
    } else {
        if (opt_size >= 120u) {
            out->import_rva = rd32(image + opt_off + 120u);
            out->import_size = rd32(image + opt_off + 124u);
        }
    }

    sec_off = opt_off + opt_size;
    if (!range_ok(sec_off, (uint32_t)sections * 40u, size))
        return PROS_DRIVER_EIO;
    for (i = 0; i < sections; ++i) {
        const uint8_t *s = image + sec_off + i * 40u;
        uint32_t raw_off = rd32(s + 20u);
        uint32_t raw_size = rd32(s + 16u);
        if (raw_size && !range_ok(raw_off, raw_size, size))
            return PROS_DRIVER_EIO;
    }
    if (out->image_size == 0 || out->entry_rva >= out->image_size)
        return PROS_DRIVER_EIO;
    return PROS_DRIVER_OK;
}

int pros_pe_selftest(void)
{
    uint8_t pe[512] = {0};
    struct pros_pe_image_info info;
    uint32_t p = 0x80u;
    pe[0]='M'; pe[1]='Z';
    pe[0x3c]=(uint8_t)p;
    pe[p]='P'; pe[p+1]='E';
    pe[p+6]=1;
    pe[p+20]=0xf0; pe[p+21]=0;
    pe[p+24]=0x0b; pe[p+25]=0x02;
    pe[p+40]=0x00; pe[p+41]=0x10; pe[p+42]=0x00; pe[p+43]=0x00;
    pe[p+80]=0x00; pe[p+81]=0x10; pe[p+82]=0x00; pe[p+83]=0x00;
    pe[p+92]=3;
    pe[p+24+68]=1;
    if (pros_pe_inspect(pe, sizeof(pe), &info) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (info.machine != 0 || info.sections != 1 || info.entry_rva != 0x1000u)
        return PROS_DRIVER_EIO;
    return PROS_DRIVER_OK;
}

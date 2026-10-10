#include "pe_image.h"

#define WB_PE_MAX_FILE (64u * 1024u * 1024u)
#define WB_SCN_MEM_EXECUTE 0x20000000u
#define WB_SCN_MEM_WRITE 0x80000000u

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int span_ok(size_t off, size_t len, size_t total) {
    return off <= total && len <= total - off;
}
static int range32(uint32_t start, uint32_t len, uint32_t limit) {
    return start <= limit && len <= limit - start;
}
static void zero_image(struct wb_pe_image *out) {
    uint8_t *p = (uint8_t *)out;
    size_t i;
    for (i = 0; i < sizeof(*out); ++i) p[i] = 0;
}

int wb_pe_inspect(const uint8_t *data, size_t size, struct wb_pe_image *out) {
    struct wb_pe_image tmp;
    uint32_t peoff, optoff, sectoff, dircount = 0;
    uint16_t optsize, magic, sections;
    uint32_t i, dir_base;
    int entry_exec = 0;
    if (!out) return WB_PE_EINVAL;
    zero_image(out);
    zero_image(&tmp);
    if (!data || size < 0x40u) return WB_PE_EINVAL;
    if (size > WB_PE_MAX_FILE) return WB_PE_ENOTSUP;
    if (data[0] != 'M' || data[1] != 'Z') return WB_PE_EFORMAT;
    peoff = rd32(data + 0x3c);
    if (!span_ok((size_t)peoff, 24u, size)) return WB_PE_EBOUNDS;
    if (data[peoff] != 'P' || data[peoff+1] != 'E' || data[peoff+2] || data[peoff+3]) return WB_PE_EFORMAT;
    tmp.machine = rd16(data + peoff + 4u);
    sections = rd16(data + peoff + 6u);
    optsize = rd16(data + peoff + 20u);
    if (!sections || sections > WB_PE_MAX_SECTIONS) return WB_PE_ENOTSUP;
    optoff = peoff + 24u;
    if (!span_ok(optoff, optsize, size) || optsize < 70u) return WB_PE_EBOUNDS;
    magic = rd16(data + optoff);
    if (magic == 0x10bu) {
        if (tmp.machine != WB_PE_MACHINE_I386) return WB_PE_ENOTSUP;
        if (optsize < 96u) return WB_PE_EBOUNDS;
        dir_base = 96u;
        dircount = rd32(data + optoff + 92u);
        if (dircount > 16u || dircount > (optsize - dir_base) / 8u) return WB_PE_EFORMAT;
        if (dircount > 1u && optsize < 112u) return WB_PE_EBOUNDS;
        if (dircount > 1u) { tmp.import_rva=rd32(data+optoff+104u); tmp.import_size=rd32(data+optoff+108u); }
        if (dircount > 5u) { tmp.reloc_rva=rd32(data+optoff+136u); tmp.reloc_size=rd32(data+optoff+140u); }
        tmp.pe32_plus=0;
    } else if (magic == 0x20bu) {
        if (tmp.machine != WB_PE_MACHINE_AMD64) return WB_PE_ENOTSUP;
        if (optsize < 112u) return WB_PE_EBOUNDS;
        dir_base = 112u;
        dircount = rd32(data + optoff + 108u);
        if (dircount > 16u || dircount > (optsize - dir_base) / 8u) return WB_PE_EFORMAT;
        if (dircount > 1u && optsize < 128u) return WB_PE_EBOUNDS;
        if (dircount > 1u) { tmp.import_rva=rd32(data+optoff+120u); tmp.import_size=rd32(data+optoff+124u); }
        if (dircount > 5u) { tmp.reloc_rva=rd32(data+optoff+152u); tmp.reloc_size=rd32(data+optoff+156u); }
        tmp.pe32_plus=1;
    } else return WB_PE_ENOTSUP;
    tmp.entry_rva=rd32(data+optoff+16u);
    tmp.image_size=rd32(data+optoff+56u);
    tmp.headers_size=rd32(data+optoff+60u);
    tmp.subsystem=rd16(data+optoff+68u);
    tmp.section_count=sections;
    if (!tmp.image_size || tmp.image_size > 0x40000000u ||
        !tmp.headers_size || tmp.headers_size > size || tmp.headers_size > tmp.image_size ||
        tmp.entry_rva >= tmp.image_size) return WB_PE_EFORMAT;
    sectoff=optoff+optsize;
    if (!span_ok(sectoff, (size_t)sections*40u, size) ||
        sectoff+(size_t)sections*40u > tmp.headers_size) return WB_PE_EBOUNDS;
    for (i=0;i<sections;i++) {
        const uint8_t *s=data+sectoff+(size_t)i*40u;
        struct wb_pe_section *d=&tmp.sections[i];
        uint32_t virtual_size=rd32(s+8u), va=rd32(s+12u), raw_size=rd32(s+16u), raw=rd32(s+20u);
        uint32_t extent=virtual_size > raw_size ? virtual_size : raw_size;
        d->virtual_address=va; d->virtual_size=virtual_size; d->raw_offset=raw; d->raw_size=raw_size; d->characteristics=rd32(s+36u);
        if (raw_size && (!span_ok(raw,raw_size,size) || raw < tmp.headers_size)) return WB_PE_EBOUNDS;
        if (extent && (!range32(va,extent,tmp.image_size) || va < tmp.headers_size)) return WB_PE_EBOUNDS;
        if ((d->characteristics & (WB_SCN_MEM_WRITE|WB_SCN_MEM_EXECUTE)) == (WB_SCN_MEM_WRITE|WB_SCN_MEM_EXECUTE)) return WB_PE_ECONFLICT;
        if (tmp.entry_rva >= va && tmp.entry_rva-va < extent && (d->characteristics & WB_SCN_MEM_EXECUTE)) entry_exec=1;
        for (uint32_t j=0;j<i;j++) {
            const struct wb_pe_section *p=&tmp.sections[j];
            uint32_t pextent=p->virtual_size > p->raw_size ? p->virtual_size : p->raw_size;
            if (extent && pextent && va < p->virtual_address+pextent && p->virtual_address < va+extent) return WB_PE_ECONFLICT;
            if (raw_size && p->raw_size && raw < p->raw_offset+p->raw_size && p->raw_offset < raw+raw_size) return WB_PE_ECONFLICT;
        }
    }
    if (!entry_exec) return WB_PE_EFORMAT;
    if ((tmp.import_rva==0u)!=(tmp.import_size==0u) || (tmp.reloc_rva==0u)!=(tmp.reloc_size==0u)) return WB_PE_EFORMAT;
    if (tmp.import_rva && !range32(tmp.import_rva,tmp.import_size,tmp.image_size)) return WB_PE_EBOUNDS;
    if (tmp.reloc_rva && !range32(tmp.reloc_rva,tmp.reloc_size,tmp.image_size)) return WB_PE_EBOUNDS;
    *out=tmp;
    return WB_PE_OK;
}

int wb_pe_rva_to_file(const uint8_t *data, size_t size, const struct wb_pe_image *pe,
                      uint32_t rva, uint32_t length, size_t *offset_out) {
    uint32_t i;
    if (!data || !pe || !offset_out) return WB_PE_EINVAL;
    if (!range32(rva,length,pe->image_size)) return WB_PE_EBOUNDS;
    if (rva < pe->headers_size) {
        if (!span_ok(rva,length,size) || !range32(rva,length,pe->headers_size)) return WB_PE_EBOUNDS;
        *offset_out=(size_t)rva; return WB_PE_OK;
    }
    for (i=0;i<pe->section_count;i++) {
        const struct wb_pe_section *s=&pe->sections[i];
        if (rva >= s->virtual_address && rva-s->virtual_address <= s->raw_size &&
            length <= s->raw_size-(rva-s->virtual_address)) {
            uint64_t off=(uint64_t)s->raw_offset+(rva-s->virtual_address);
            if (off > SIZE_MAX || !span_ok((size_t)off,length,size)) return WB_PE_EBOUNDS;
            *offset_out=(size_t)off; return WB_PE_OK;
        }
    }
    return WB_PE_EBOUNDS; /* RVA may point into zero-filled virtual tail. */
}

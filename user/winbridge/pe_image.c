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
        /* The entry point must be backed by file bytes, not a zero-filled tail. */
        if (tmp.entry_rva >= va && tmp.entry_rva-va < raw_size &&
            (d->characteristics & WB_SCN_MEM_EXECUTE)) entry_exec=1;
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
    /* This is a public helper: do not assume callers only pass parser output. */
    if (pe->section_count == 0u || pe->section_count > WB_PE_MAX_SECTIONS ||
        pe->image_size == 0u || pe->headers_size == 0u ||
        pe->headers_size > pe->image_size)
        return WB_PE_EINVAL;
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

int wb_pe_import_at(const uint8_t *data, size_t size,
                    const struct wb_pe_image *pe, uint32_t index,
                    struct wb_pe_import *out)
{
    size_t off;
    uint64_t descriptor_rva;
    uint32_t fields[5];
    uint32_t i;
    if (!out) return WB_PE_EINVAL;
    out->original_first_thunk = 0;
    out->timestamp = 0;
    out->forwarder_chain = 0;
    out->name_rva = 0;
    out->first_thunk = 0;
    if (!data || !pe) return WB_PE_EINVAL;
    if (pe->import_rva == 0u || pe->import_size < 20u ||
        (pe->import_size % 20u) != 0u)
        return WB_PE_EFORMAT;
    if (index >= pe->import_size / 20u) return WB_PE_EBOUNDS;
    descriptor_rva = (uint64_t)pe->import_rva + (uint64_t)index * 20u;
    if (descriptor_rva > UINT32_MAX) return WB_PE_EBOUNDS;
    if (wb_pe_rva_to_file(data, size, pe, (uint32_t)descriptor_rva,
                          20u, &off) != WB_PE_OK)
        return WB_PE_EBOUNDS;
    for (i = 0; i < 5u; ++i) fields[i] = rd32(data + off + i * 4u);
    if ((fields[0] | fields[1] | fields[2] | fields[3] | fields[4]) == 0u)
        return WB_PE_ENOTFOUND;
    if (fields[3] == 0u || fields[4] == 0u) return WB_PE_EFORMAT;
    out->original_first_thunk = fields[0];
    out->timestamp = fields[1];
    out->forwarder_chain = fields[2];
    out->name_rva = fields[3];
    out->first_thunk = fields[4];
    return WB_PE_OK;
}

int wb_pe_read_ascii_string(const uint8_t *data, size_t size,
                            const struct wb_pe_image *pe, uint32_t rva,
                            char *out, size_t capacity)
{
    size_t i;
    if (!out || capacity == 0u) return WB_PE_EINVAL;
    out[0] = '\0';
    if (!data || !pe || rva == 0u) return WB_PE_EINVAL;
    for (i = 0; i < capacity; ++i) {
        size_t off;
        uint64_t current = (uint64_t)rva + i;
        uint8_t ch;
        if (current > UINT32_MAX ||
            wb_pe_rva_to_file(data, size, pe, (uint32_t)current, 1u,
                              &off) != WB_PE_OK) {
            out[0] = '\0';
            return WB_PE_EBOUNDS;
        }
        ch = data[off];
        if (ch == 0u) { out[i] = '\0'; return WB_PE_OK; }
        if (ch < 0x20u || ch > 0x7eu) {
            out[0] = '\0';
            return WB_PE_EFORMAT;
        }
        out[i] = (char)ch;
    }
    out[0] = '\0';
    return WB_PE_EBOUNDS; /* No NUL terminator within the caller's bound. */
}

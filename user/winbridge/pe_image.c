#include "pe_image.h"

#define WB_PE_MAX_FILE (64u * 1024u * 1024u)
#define WB_PE_MAX_IMPORT_DESCRIPTORS 4096u
#define WB_PE_MAX_STRING 4096u
#define WB_SCN_MEM_EXECUTE 0x20000000u
#define WB_SCN_MEM_WRITE 0x80000000u

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4u) << 32);
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
    if (tmp.import_rva) {
        size_t directory_offset;
        if (tmp.import_size < 20u || (tmp.import_size % 20u) != 0u) return WB_PE_EFORMAT;
        if (tmp.import_size / 20u > WB_PE_MAX_IMPORT_DESCRIPTORS) return WB_PE_ENOTSUP;
        if (!range32(tmp.import_rva,tmp.import_size,tmp.image_size)) return WB_PE_EBOUNDS;
        /* A directory must be backed by bytes in the file, not merely fit
         * inside the virtual image or point into a zero-filled section tail. */
        if (wb_pe_rva_to_file(data, size, &tmp, tmp.import_rva,
                              tmp.import_size, &directory_offset) != WB_PE_OK)
            return WB_PE_EBOUNDS;
    }
    if (tmp.reloc_rva) {
        size_t directory_offset;
        size_t cursor = 0u;
        if (tmp.reloc_size < 8u) return WB_PE_EFORMAT;
        if (!range32(tmp.reloc_rva,tmp.reloc_size,tmp.image_size)) return WB_PE_EBOUNDS;
        if (wb_pe_rva_to_file(data, size, &tmp, tmp.reloc_rva,
                              tmp.reloc_size, &directory_offset) != WB_PE_OK)
            return WB_PE_EBOUNDS;
        /* Validate relocation-block framing without applying relocations.
         * Each block has an 8-byte header followed by 16-bit entries. */
        while (cursor < tmp.reloc_size) {
            uint32_t page_rva, block_size;
            size_t remaining = (size_t)tmp.reloc_size - cursor;
            if (remaining < 8u) return WB_PE_EFORMAT;
            page_rva = rd32(data + directory_offset + cursor);
            block_size = rd32(data + directory_offset + cursor + 4u);
            if ((page_rva & 0xfffu) != 0u || page_rva >= tmp.image_size ||
                block_size < 8u || (block_size & 3u) != 0u ||
                (size_t)block_size > remaining)
                return WB_PE_EFORMAT;
            cursor += block_size;
        }
    }
    *out=tmp;
    return WB_PE_OK;
}

int wb_pe_rva_to_file(const uint8_t *data, size_t size, const struct wb_pe_image *pe,
                      uint32_t rva, uint32_t length, size_t *offset_out) {
    uint32_t i;
    if (!data || !pe || !offset_out) return WB_PE_EINVAL;
    /* Keep the public mapping contract aligned with the parser's input cap.
     * Besides limiting work, this keeps raw-range arithmetic safely bounded
     * even when callers provide forged section metadata. */
    if (size > WB_PE_MAX_FILE) return WB_PE_ENOTSUP;
    if (length == 0u) return WB_PE_EINVAL;
    /* This is a public helper: do not assume callers only pass parser output. */
    if (pe->section_count == 0u || pe->section_count > WB_PE_MAX_SECTIONS ||
        pe->image_size == 0u || pe->image_size > 0x40000000u ||
        pe->headers_size == 0u || pe->headers_size > pe->image_size ||
        pe->headers_size > size)
        return WB_PE_EINVAL;
    if (!range32(rva,length,pe->image_size)) return WB_PE_EBOUNDS;
    /* Validate the complete caller-supplied section map before returning any
     * match, including header mappings. Otherwise an early header match could
     * hide malformed or overlapping metadata in caller-forged section entries. */
    for (i=0;i<pe->section_count;i++) {
        const struct wb_pe_section *s=&pe->sections[i];
        uint32_t extent = s->virtual_size > s->raw_size ?
                          s->virtual_size : s->raw_size;
        uint32_t j;
        if (s->raw_size != 0u &&
            (!span_ok(s->raw_offset, s->raw_size, size) ||
             s->raw_offset < pe->headers_size))
            return WB_PE_EBOUNDS;
        if (extent != 0u &&
            (!range32(s->virtual_address, extent, pe->image_size) ||
             s->virtual_address < pe->headers_size))
            return WB_PE_EBOUNDS;
        for (j=0;j<i;j++) {
            const struct wb_pe_section *p=&pe->sections[j];
            uint32_t pextent=p->virtual_size > p->raw_size ?
                             p->virtual_size : p->raw_size;
            if (extent && pextent &&
                s->virtual_address < p->virtual_address + pextent &&
                p->virtual_address < s->virtual_address + extent)
                return WB_PE_ECONFLICT;
            if (s->raw_size && p->raw_size &&
                s->raw_offset < p->raw_offset + p->raw_size &&
                p->raw_offset < s->raw_offset + s->raw_size)
                return WB_PE_ECONFLICT;
        }
    }
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
    uint32_t i, descriptor_count;
    struct wb_pe_import requested;
    int requested_found = 0;
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
    if (pe->import_size / 20u > WB_PE_MAX_IMPORT_DESCRIPTORS)
        return WB_PE_ENOTSUP;
    /* Validate the declared directory as one complete, image-bounded,
     * file-backed range before indexing any descriptor. */
    if (!range32(pe->import_rva, pe->import_size, pe->image_size))
        return WB_PE_EBOUNDS;
    if (wb_pe_rva_to_file(data, size, pe, pe->import_rva,
                          pe->import_size, &off) != WB_PE_OK)
        return WB_PE_EBOUNDS;
    descriptor_count = pe->import_size / 20u;
    if (index >= descriptor_count) return WB_PE_EBOUNDS;

    /* PE import descriptors must end with an all-zero descriptor. Scan the
     * entire declared table so an early successful lookup cannot hide a
     * missing terminator later in a truncated or forged directory. */
    {
        int terminator_seen = 0;
        for (i = 0; i < descriptor_count; ++i) {
            size_t descriptor_off = off + (size_t)i * 20u;
            uint32_t fields[5];
            uint32_t j;
            for (j = 0; j < 5u; ++j)
                fields[j] = rd32(data + descriptor_off + j * 4u);
            if ((fields[0] | fields[1] | fields[2] | fields[3] | fields[4]) == 0u) {
                terminator_seen = 1;
                continue;
            }
            /* A descriptor after the required null terminator is malformed;
             * do not silently ignore hidden trailing import records. */
            if (terminator_seen) return WB_PE_EFORMAT;
            if (i == index) {
                if (fields[3] == 0u || fields[4] == 0u) return WB_PE_EFORMAT;
                requested.original_first_thunk = fields[0];
                requested.timestamp = fields[1];
                requested.forwarder_chain = fields[2];
                requested.name_rva = fields[3];
                requested.first_thunk = fields[4];
                requested_found = 1;
            }
        }
        if (!terminator_seen) return WB_PE_EFORMAT;
        if (!requested_found) return WB_PE_ENOTFOUND;
        *out = requested;
        return WB_PE_OK;
    }
}

int wb_pe_read_ascii_string(const uint8_t *data, size_t size,
                            const struct wb_pe_image *pe, uint32_t rva,
                            char *out, size_t capacity)
{
    size_t i;
    if (!out || capacity == 0u) return WB_PE_EINVAL;
    out[0] = '\0';
    if (!data || !pe || rva == 0u) return WB_PE_EINVAL;
    if (capacity > WB_PE_MAX_STRING) return WB_PE_ENOTSUP;
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

int wb_pe_import_symbol_at(const uint8_t *data, size_t size,
                           const struct wb_pe_image *pe,
                           const struct wb_pe_import *import_entry,
                           uint32_t index, struct wb_pe_import_symbol *symbol,
                           char *name, size_t name_capacity)
{
    uint32_t thunk_rva, i;
    uint64_t value, ordinal_flag;
    size_t off;
    uint32_t width, table_bytes;
    uint64_t prefix_bytes;
    if (!symbol || !name || name_capacity == 0u) return WB_PE_EINVAL;
    symbol->is_ordinal = 0;
    symbol->ordinal = 0;
    symbol->hint = 0;
    name[0] = '\0';
    if (!data || !pe || !import_entry) return WB_PE_EINVAL;
    if (pe->pe32_plus > 1u || pe->section_count == 0u ||
        pe->section_count > WB_PE_MAX_SECTIONS) return WB_PE_EINVAL;
    width = pe->pe32_plus ? 8u : 4u;
    thunk_rva = import_entry->original_first_thunk ?
                import_entry->original_first_thunk : import_entry->first_thunk;
    if (thunk_rva == 0u) return WB_PE_EFORMAT;
    /* Bound the prefix using wide arithmetic before narrowing its length.
     * The input file limit also prevents caller-forged image metadata from
     * causing an enormous scan or a truncated uint32_t byte count. */
    prefix_bytes = ((uint64_t)index + 1u) * width;
    if (thunk_rva >= pe->image_size ||
        prefix_bytes > (uint64_t)pe->image_size - thunk_rva ||
        prefix_bytes > WB_PE_MAX_FILE || prefix_bytes > UINT32_MAX)
        return WB_PE_EBOUNDS;
    table_bytes = (uint32_t)prefix_bytes;
    if (wb_pe_rva_to_file(data, size, pe, thunk_rva, table_bytes, &off) != WB_PE_OK)
        return WB_PE_EBOUNDS;
    ordinal_flag = pe->pe32_plus ? 0x8000000000000000ULL : 0x80000000ULL;
    /* A caller-supplied index must not jump over a null thunk terminator. */
    for (i = 0; i <= index; ++i) {
        size_t slot_off = off + (size_t)i * width;
        value = width == 8u ? rd64(data + slot_off) : rd32(data + slot_off);
        if (value == 0u) return WB_PE_ENOTFOUND;
    }
    if (value & ordinal_flag) {
        if ((value & ~(ordinal_flag | 0xffffULL)) != 0u) return WB_PE_EFORMAT;
        symbol->is_ordinal = 1u;
        symbol->ordinal = (uint16_t)(value & 0xffffu);
        return WB_PE_OK;
    }
    if (value > UINT32_MAX || value > UINT32_MAX - 2u) return WB_PE_EBOUNDS;
    if (wb_pe_rva_to_file(data, size, pe, (uint32_t)value, 2u, &off) != WB_PE_OK)
        return WB_PE_EBOUNDS;
    symbol->hint = rd16(data + off);
    {
        int rc = wb_pe_read_ascii_string(data, size, pe, (uint32_t)value + 2u,
                                         name, name_capacity);
        if (rc != WB_PE_OK) {
            symbol->hint = 0;
            name[0] = '\0';
            return rc;
        }
    }
    return WB_PE_OK;
}

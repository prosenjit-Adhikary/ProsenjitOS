#ifndef PROS_WINBRIDGE_PE_IMAGE_H
#define PROS_WINBRIDGE_PE_IMAGE_H
#include <stddef.h>
#include <stdint.h>

#define WB_PE_MAX_SECTIONS 32u
#define WB_PE_MACHINE_I386 0x014cu
#define WB_PE_MACHINE_AMD64 0x8664u
#define WB_PE_OK 0
#define WB_PE_EINVAL -1
#define WB_PE_EFORMAT -2
#define WB_PE_EBOUNDS -3
#define WB_PE_ENOTSUP -4
#define WB_PE_ECONFLICT -5

struct wb_pe_section {
    uint32_t virtual_address;
    uint32_t virtual_size;
    uint32_t raw_offset;
    uint32_t raw_size;
    uint32_t characteristics;
};
struct wb_pe_image {
    uint16_t machine;
    uint16_t section_count;
    uint16_t subsystem;
    uint8_t pe32_plus;
    uint32_t entry_rva;
    uint32_t image_size;
    uint32_t headers_size;
    uint32_t import_rva;
    uint32_t import_size;
    uint32_t reloc_rva;
    uint32_t reloc_size;
    struct wb_pe_section sections[WB_PE_MAX_SECTIONS];
};
int wb_pe_inspect(const uint8_t *data, size_t size, struct wb_pe_image *out);
int wb_pe_rva_to_file(const uint8_t *data, size_t size,
                      const struct wb_pe_image *pe, uint32_t rva,
                      uint32_t length, size_t *offset_out);
#endif

#include "pe_image.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct wb_pe_image image;
    size_t offset = 0;
    int rc = wb_pe_inspect(data, size, &image);
    if (rc == WB_PE_OK) {
        (void)wb_pe_rva_to_file(data, size, &image, image.entry_rva, 1u,
                                &offset);
        if (image.import_rva && image.import_size) {
            struct wb_pe_import imp;
            struct wb_pe_import_symbol symbol;
            char dll[128], symbol_name[128];
            if (wb_pe_import_at(data, size, &image, 0u, &imp) == WB_PE_OK) {
                (void)wb_pe_read_ascii_string(data, size, &image, imp.name_rva,
                                              dll, sizeof dll);
                (void)wb_pe_import_symbol_at(data, size, &image, &imp, 0u,
                                              &symbol, symbol_name,
                                              sizeof symbol_name);
            }
            (void)wb_pe_rva_to_file(data, size, &image, image.import_rva,
                                    image.import_size, &offset);
        }
        if (image.reloc_rva && image.reloc_size)
            (void)wb_pe_rva_to_file(data, size, &image, image.reloc_rva,
                                    image.reloc_size, &offset);
    }
    return 0;
}

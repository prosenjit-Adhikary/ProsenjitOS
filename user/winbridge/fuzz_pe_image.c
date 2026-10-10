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
        if (image.import_rva && image.import_size)
            (void)wb_pe_rva_to_file(data, size, &image, image.import_rva,
                                    image.import_size, &offset);
        if (image.reloc_rva && image.reloc_size)
            (void)wb_pe_rva_to_file(data, size, &image, image.reloc_rva,
                                    image.reloc_size, &offset);
    }
    return 0;
}

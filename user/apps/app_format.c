#include "app_format.h"
#include "app_manager.h"

#define APP_FORMAT_EINVAL (-1)
#define APP_FORMAT_EFORMAT (-2)
#define ELF_IDENT_SIZE 16u
#define ELF_CLASS_32 1u
#define ELF_CLASS_64 2u
#define ELF_DATA_LSB 1u
#define ELF_DATA_MSB 2u
#define ELF_VERSION_CURRENT 1u
#define PE_DOS_HEADER_MIN 64u
#define PE_SIGNATURE_SIZE 4u
#define PE_COFF_HEADER_SIZE 20u
#define PE_OPTIONAL_MAGIC_OFFSET 24u
#define PE_OPTIONAL_MAGIC_32 0x10bu
#define PE_OPTIONAL_MAGIC_64 0x20bu

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] |
                      ((uint16_t)data[1] << 8));
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static int has_prefix(const uint8_t *data, uint32_t length,
                      const uint8_t *prefix, uint32_t prefix_length)
{
    if (!data || !prefix || length < prefix_length)
        return 0;
    for (uint32_t i = 0u; i < prefix_length; ++i) {
        if (data[i] != prefix[i])
            return 0;
    }
    return 1;
}

int pros_app_detect_format(const uint8_t *data,
                           uint32_t length,
                           enum pros_app_file_format *format_out)
{
    static const uint8_t elf_magic[4] = {0x7fu, 'E', 'L', 'F'};
    static const uint8_t pe_magic[4] = {'P', 'E', 0u, 0u};
    static const uint8_t ole_magic[8] = {
        0xd0u, 0xcfu, 0x11u, 0xe0u, 0xa1u, 0xb1u, 0x1au, 0xe1u
    };

    if (!data || !format_out)
        return APP_FORMAT_EINVAL;
    *format_out = PROS_APP_FORMAT_UNKNOWN;

    if (length >= sizeof(elf_magic) &&
        has_prefix(data, length, elf_magic, sizeof(elf_magic))) {
        if (length < ELF_IDENT_SIZE ||
            (data[4] != ELF_CLASS_32 && data[4] != ELF_CLASS_64) ||
            (data[5] != ELF_DATA_LSB && data[5] != ELF_DATA_MSB) ||
            data[6] != ELF_VERSION_CURRENT)
            return APP_FORMAT_EFORMAT;
        *format_out = data[4] == ELF_CLASS_32
                          ? PROS_APP_FORMAT_ELF32
                          : PROS_APP_FORMAT_ELF64;
        return 0;
    }

    if (length >= 2u && data[0] == 'M' && data[1] == 'Z') {
        uint32_t pe_offset;
        uint16_t optional_size;
        uint16_t optional_magic;

        if (length < PE_DOS_HEADER_MIN)
            return APP_FORMAT_EFORMAT;
        pe_offset = read_le32(&data[0x3cu]);
        if (pe_offset > length || length - pe_offset < PE_SIGNATURE_SIZE)
            return APP_FORMAT_EFORMAT;
        if (!has_prefix(&data[pe_offset], length - pe_offset,
                        pe_magic, sizeof(pe_magic)))
            return APP_FORMAT_EFORMAT;
        if (length - pe_offset < PE_SIGNATURE_SIZE + PE_COFF_HEADER_SIZE)
            return APP_FORMAT_EFORMAT;

        optional_size = read_le16(&data[pe_offset + PE_SIGNATURE_SIZE + 16u]);
        if (optional_size < 2u ||
            length - pe_offset < PE_OPTIONAL_MAGIC_OFFSET + 2u ||
            optional_size > length - pe_offset - PE_OPTIONAL_MAGIC_OFFSET)
            return APP_FORMAT_EFORMAT;
        optional_magic = read_le16(
            &data[pe_offset + PE_OPTIONAL_MAGIC_OFFSET]);
        if (optional_magic == PE_OPTIONAL_MAGIC_32)
            *format_out = PROS_APP_FORMAT_PE32;
        else if (optional_magic == PE_OPTIONAL_MAGIC_64)
            *format_out = PROS_APP_FORMAT_PE32_PLUS;
        else
            return APP_FORMAT_EFORMAT;
        return 0;
    }

    if (has_prefix(data, length, ole_magic, sizeof(ole_magic))) {
        *format_out = PROS_APP_FORMAT_OLE_CONTAINER;
        return 0;
    }

    return 0;
}

int pros_app_format_compatible_with_backend(uint32_t backend,
                                            enum pros_app_file_format format)
{
    if (backend == PROS_APP_BACKEND_WINDOWS)
        return format == PROS_APP_FORMAT_PE32 ||
               format == PROS_APP_FORMAT_PE32_PLUS;
    if (backend == PROS_APP_BACKEND_NATIVE ||
        backend == PROS_APP_BACKEND_LINUX)
        return format == PROS_APP_FORMAT_ELF32 ||
               format == PROS_APP_FORMAT_ELF64;
    return 0;
}

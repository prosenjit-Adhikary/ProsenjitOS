#include "app_format.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, code) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "FAIL app-format line %d: %s\n", \
                      __LINE__, #condition); \
        return (code); \
    } \
} while (0)

int main(void)
{
    enum pros_app_file_format format = PROS_APP_FORMAT_PE32;
    uint8_t elf[64] = {0};
    uint8_t pe[128] = {0};
    static const uint8_t ole[8] = {
        0xd0u, 0xcfu, 0x11u, 0xe0u, 0xa1u, 0xb1u, 0x1au, 0xe1u
    };
    uint8_t malformed[64] = {0};

    CHECK(pros_app_detect_format(NULL, 0u, &format) == -1, 1);
    CHECK(pros_app_detect_format(elf, sizeof(elf), NULL) == -1, 2);
    CHECK(pros_app_detect_format(elf, sizeof(elf), &format) == 0, 3);
    CHECK(format == PROS_APP_FORMAT_UNKNOWN, 4);

    elf[0] = 0x7fu;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2u;
    elf[5] = 1u;
    elf[6] = 1u;
    CHECK(pros_app_detect_format(elf, 15u, &format) == -2, 5);
    CHECK(pros_app_detect_format(elf, sizeof(elf), &format) == 0, 6);
    CHECK(format == PROS_APP_FORMAT_ELF64, 7);
    elf[4] = 1u;
    CHECK(pros_app_detect_format(elf, sizeof(elf), &format) == 0, 8);
    CHECK(format == PROS_APP_FORMAT_ELF32, 9);
    elf[4] = 3u;
    CHECK(pros_app_detect_format(elf, sizeof(elf), &format) == -2, 10);

    pe[0] = 'M';
    pe[1] = 'Z';
    pe[0x3cu] = 0x40u;
    pe[0x40u] = 'P';
    pe[0x41u] = 'E';
    pe[0x42u] = 0u;
    pe[0x43u] = 0u;
    /* COFF optional-header size: 2 bytes at PE + 4 + 16. */
    pe[0x54u] = 2u;
    pe[0x58u] = 0x0bu;
    pe[0x59u] = 0x01u;
    CHECK(pros_app_detect_format(pe, sizeof(pe), &format) == 0, 11);
    CHECK(format == PROS_APP_FORMAT_PE32, 12);
    pe[0x58u] = 0x0bu;
    pe[0x59u] = 0x02u;
    CHECK(pros_app_detect_format(pe, sizeof(pe), &format) == 0, 13);
    CHECK(format == PROS_APP_FORMAT_PE32_PLUS, 14);

    memset(malformed, 0, sizeof(malformed));
    malformed[0] = 'M';
    malformed[1] = 'Z';
    malformed[0x3cu] = 0xffu;
    malformed[0x3du] = 0xffu;
    malformed[0x3eu] = 0xffu;
    malformed[0x3fu] = 0xffu;
    CHECK(pros_app_detect_format(malformed, sizeof(malformed), &format) == -2,
          15);
    CHECK(format == PROS_APP_FORMAT_UNKNOWN, 16);
    CHECK(pros_app_detect_format(pe, 64u, &format) == -2, 17);

    CHECK(pros_app_detect_format(ole, sizeof(ole), &format) == 0, 18);
    CHECK(format == PROS_APP_FORMAT_OLE_CONTAINER, 19);

    CHECK(pros_app_format_compatible_with_backend(
              1u, PROS_APP_FORMAT_ELF64) == 1, 20);
    CHECK(pros_app_format_compatible_with_backend(
              2u, PROS_APP_FORMAT_ELF32) == 1, 21);
    CHECK(pros_app_format_compatible_with_backend(
              3u, PROS_APP_FORMAT_PE32_PLUS) == 1, 22);
    CHECK(pros_app_format_compatible_with_backend(
              3u, PROS_APP_FORMAT_ELF64) == 0, 23);
    CHECK(pros_app_format_compatible_with_backend(
              1u, PROS_APP_FORMAT_PE32) == 0, 24);
    CHECK(pros_app_format_compatible_with_backend(
              99u, PROS_APP_FORMAT_ELF64) == 0, 25);
    CHECK(pros_app_format_compatible_with_backend(
              3u, PROS_APP_FORMAT_OLE_CONTAINER) == 0, 26);

    (void)puts("application format tests passed");
    return 0;
}

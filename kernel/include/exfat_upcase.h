#ifndef PROSENJITOS_EXFAT_UPCASE_H
#define PROSENJITOS_EXFAT_UPCASE_H

#include <stdint.h>

#define PROS_EXFAT_UPCASE_OK       0
#define PROS_EXFAT_UPCASE_EINVAL  -1
#define PROS_EXFAT_UPCASE_ECORRUPT -2

#define PROS_EXFAT_UPCASE_UNCOMPRESSED 0u
#define PROS_EXFAT_UPCASE_COMPRESSED   1u

struct pros_exfat_upcase {
    const uint8_t *data;
    uint32_t length;
    uint8_t compressed;
};

int exfat_upcase_parse(const uint8_t *data, uint32_t length,
                       uint8_t compressed,
                       struct pros_exfat_upcase *table);

int exfat_upcase_lookup(const struct pros_exfat_upcase *table,
                        uint16_t codepoint, uint16_t *mapped);

uint32_t exfat_upcase_checksum(const uint8_t *data, uint32_t length);

int exfat_upcase_selftest(void);

#endif

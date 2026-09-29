#include "exfat_upcase.h"

#define EXFAT_UPCASE_CODEPOINTS 0x10000u
#define EXFAT_UPCASE_UNCOMPRESSED_BYTES (EXFAT_UPCASE_CODEPOINTS * 2u)

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t exfat_upcase_checksum(const uint8_t *data, uint32_t length)
{
    uint32_t checksum = 0u;

    if (!data)
        return 0u;

    for (uint32_t i = 0u; i < length; ++i)
        checksum = (checksum & 1u ? 0x80000000u : 0u) +
                   (checksum >> 1) + (uint32_t)data[i];

    return checksum;
}

static int exfat_upcase_validate_mandatory(const struct pros_exfat_upcase *table)
{
    for (uint16_t codepoint = 0u; codepoint < 0x80u; ++codepoint) {
        uint16_t mapped;
        if (exfat_upcase_lookup(table, codepoint, &mapped) !=
                PROS_EXFAT_UPCASE_OK)
            return PROS_EXFAT_UPCASE_ECORRUPT;
        if (codepoint >= (uint16_t)'a' && codepoint <= (uint16_t)'z') {
            uint16_t expected =
                (uint16_t)(codepoint - (uint16_t)'a' + (uint16_t)'A');
            if (mapped != expected)
                return PROS_EXFAT_UPCASE_ECORRUPT;
        } else if (mapped != codepoint) {
            return PROS_EXFAT_UPCASE_ECORRUPT;
        }
    }
    return PROS_EXFAT_UPCASE_OK;
}

int exfat_upcase_parse(const uint8_t *data, uint32_t length,
                       uint8_t compressed,
                       struct pros_exfat_upcase *table)
{
    uint32_t covered = 0u;
    uint32_t offset = 0u;

    if (!data || !table || (length & 1u) != 0u ||
        compressed > PROS_EXFAT_UPCASE_COMPRESSED)
        return PROS_EXFAT_UPCASE_EINVAL;

    if (!compressed) {
        if (length != EXFAT_UPCASE_UNCOMPRESSED_BYTES)
            return PROS_EXFAT_UPCASE_ECORRUPT;

        table->data = data;
        table->length = length;
        table->compressed = 0u;
        return exfat_upcase_validate_mandatory(table);
    }

    while (offset < length) {
        uint16_t value;

        if (length - offset < 2u)
            return PROS_EXFAT_UPCASE_ECORRUPT;

        value = le16(data + offset);
        offset += 2u;

        if (value == 0xFFFFu) {
            uint16_t count;

            if (length - offset < 2u)
                return PROS_EXFAT_UPCASE_ECORRUPT;

            count = le16(data + offset);
            offset += 2u;
            if (count == 0u || count > EXFAT_UPCASE_CODEPOINTS - covered)
                return PROS_EXFAT_UPCASE_ECORRUPT;
            covered += count;
        } else {
            if (covered >= EXFAT_UPCASE_CODEPOINTS)
                return PROS_EXFAT_UPCASE_ECORRUPT;
            ++covered;
        }
    }

    if (covered != EXFAT_UPCASE_CODEPOINTS)
        return PROS_EXFAT_UPCASE_ECORRUPT;

    table->data = data;
    table->length = length;
    table->compressed = 1u;
    return exfat_upcase_validate_mandatory(table);
}

int exfat_upcase_lookup(const struct pros_exfat_upcase *table,
                        uint16_t codepoint, uint16_t *mapped)
{
    uint32_t offset = 0u;
    uint32_t covered = 0u;

    if (!table || !table->data || !mapped || (table->length & 1u) != 0u ||
        table->compressed > PROS_EXFAT_UPCASE_COMPRESSED)
        return PROS_EXFAT_UPCASE_EINVAL;

    if (!table->compressed) {
        if (table->length != EXFAT_UPCASE_UNCOMPRESSED_BYTES)
            return PROS_EXFAT_UPCASE_ECORRUPT;
        *mapped = le16(table->data + (uint32_t)codepoint * 2u);
        return PROS_EXFAT_UPCASE_OK;
    }

    while (offset < table->length) {
        uint16_t value = le16(table->data + offset);
        offset += 2u;

        if (value == 0xFFFFu) {
            uint16_t count;
            if (table->length - offset < 2u)
                return PROS_EXFAT_UPCASE_ECORRUPT;
            count = le16(table->data + offset);
            offset += 2u;
            if (count == 0u ||
                (uint32_t)count > EXFAT_UPCASE_CODEPOINTS - covered)
                return PROS_EXFAT_UPCASE_ECORRUPT;
            if ((uint32_t)codepoint < covered + count) {
                *mapped = codepoint;
                return PROS_EXFAT_UPCASE_OK;
            }
            covered += count;
        } else {
            if (covered >= EXFAT_UPCASE_CODEPOINTS)
                return PROS_EXFAT_UPCASE_ECORRUPT;
            if (covered == codepoint) {
                *mapped = value;
                return PROS_EXFAT_UPCASE_OK;
            }
            ++covered;
        }
    }

    return PROS_EXFAT_UPCASE_ECORRUPT;
}

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

int exfat_upcase_selftest(void)
{
    static uint8_t uncompressed[EXFAT_UPCASE_UNCOMPRESSED_BYTES];
    static const uint8_t compressed[] = {
        0xFFu, 0xFFu, 0x61u, 0x00u,
        0x41u, 0x00u,
        0x42u, 0x00u,
        0x43u, 0x00u,
        0x44u, 0x00u,
        0x45u, 0x00u,
        0x46u, 0x00u,
        0x47u, 0x00u,
        0x48u, 0x00u,
        0x49u, 0x00u,
        0x4Au, 0x00u,
        0x4Bu, 0x00u,
        0x4Cu, 0x00u,
        0x4Du, 0x00u,
        0x4Eu, 0x00u,
        0x4Fu, 0x00u,
        0x50u, 0x00u,
        0x51u, 0x00u,
        0x52u, 0x00u,
        0x53u, 0x00u,
        0x54u, 0x00u,
        0x55u, 0x00u,
        0x56u, 0x00u,
        0x57u, 0x00u,
        0x58u, 0x00u,
        0x59u, 0x00u,
        0x5Au, 0x00u,
        0xFFu, 0xFFu, 0x85u, 0xFFu
    };
    struct pros_exfat_upcase table;
    uint16_t mapped;

    for (uint32_t i = 0u; i < EXFAT_UPCASE_CODEPOINTS; ++i)
        put_le16(uncompressed + i * 2u,
                 (uint16_t)((i >= 'a' && i <= 'z') ? i - 'a' + 'A' : i));

    if (exfat_upcase_parse(uncompressed, sizeof(uncompressed),
                           PROS_EXFAT_UPCASE_UNCOMPRESSED, &table) !=
            PROS_EXFAT_UPCASE_OK)
        return -1;
    if (exfat_upcase_lookup(&table, (uint16_t)'a', &mapped) !=
            PROS_EXFAT_UPCASE_OK || mapped != (uint16_t)'A')
        return -2;
    if (exfat_upcase_lookup(&table, 0x20ACu, &mapped) !=
            PROS_EXFAT_UPCASE_OK || mapped != 0x20ACu)
        return -3;
    if (exfat_upcase_lookup(&table, 0xFFFFu, &mapped) !=
            PROS_EXFAT_UPCASE_OK || mapped != 0xFFFFu)
        return -4;

    if (exfat_upcase_parse(compressed, sizeof(compressed),
                           PROS_EXFAT_UPCASE_COMPRESSED, &table) !=
            PROS_EXFAT_UPCASE_OK)
        return -5;
    if (exfat_upcase_lookup(&table, (uint16_t)'a', &mapped) !=
            PROS_EXFAT_UPCASE_OK || mapped != (uint16_t)'A')
        return -6;
    if (exfat_upcase_lookup(&table, (uint16_t)'c', &mapped) !=
            PROS_EXFAT_UPCASE_OK || mapped != (uint16_t)'C')
        return -7;
    if (exfat_upcase_lookup(&table, 0x80u, &mapped) !=
            PROS_EXFAT_UPCASE_OK || mapped != 0x80u)
        return -8;
    if (exfat_upcase_lookup(&table, 0xFFFFu, &mapped) !=
            PROS_EXFAT_UPCASE_OK || mapped != 0xFFFFu)
        return -9;

    if (exfat_upcase_parse(compressed, sizeof(compressed) - 2u,
                           PROS_EXFAT_UPCASE_COMPRESSED, &table) !=
            PROS_EXFAT_UPCASE_ECORRUPT)
        return -10;

    {
        static const uint8_t zero_run[] = {0xFFu, 0xFFu, 0x00u, 0x00u};
        if (exfat_upcase_parse(zero_run, sizeof(zero_run),
                               PROS_EXFAT_UPCASE_COMPRESSED, &table) !=
                PROS_EXFAT_UPCASE_ECORRUPT)
            return -11;
    }

    {
        static const uint8_t bad_first[] = {
            0xFFu, 0xFFu, 0x80u, 0x00u,
            0x00u, 0x00u
        };
        if (exfat_upcase_parse(bad_first, sizeof(bad_first),
                               PROS_EXFAT_UPCASE_COMPRESSED, &table) !=
                PROS_EXFAT_UPCASE_ECORRUPT)
            return -12;
    }

    return 0;
}

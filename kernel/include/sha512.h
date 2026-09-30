#ifndef PROSENJIT_SHA512_H
#define PROSENJIT_SHA512_H

#include <stdint.h>

#define PROS_SHA512_BYTES 64u

void pros_sha512(const uint8_t *data, uint64_t length,
                 uint8_t digest[PROS_SHA512_BYTES]);
void pros_sha512_prefix(const uint8_t prefix[64], const uint8_t *data,
                        uint64_t length, uint8_t digest[PROS_SHA512_BYTES]);

int pros_sha512_selftest(void);

#endif

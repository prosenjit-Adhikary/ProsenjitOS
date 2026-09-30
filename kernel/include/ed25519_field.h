#ifndef PROSENJIT_ED25519_FIELD_H
#define PROSENJIT_ED25519_FIELD_H

#include <stdint.h>

/*
 * Field elements are represented modulo p = 2^255 - 19 using five
 * unsigned 51-bit limbs:
 *
 *   x = v[0] + v[1] 2^51 + ... + v[4] 2^204
 *
 * The representation is an internal arithmetic form. Callers exchange
 * field elements through the fixed-width 32-byte little-endian encoding.
 */
#define PROS_ED25519_FIELD_BYTES 32u

struct pros_ed25519_fe {
    uint64_t v[5];
};

void pros_ed25519_fe_zero(struct pros_ed25519_fe *r);
void pros_ed25519_fe_one(struct pros_ed25519_fe *r);
void pros_ed25519_fe_from_bytes(struct pros_ed25519_fe *r,
                                 const uint8_t in[PROS_ED25519_FIELD_BYTES]);
void pros_ed25519_fe_to_bytes(uint8_t out[PROS_ED25519_FIELD_BYTES],
                              const struct pros_ed25519_fe *a);

void pros_ed25519_fe_add(struct pros_ed25519_fe *r,
                         const struct pros_ed25519_fe *a,
                         const struct pros_ed25519_fe *b);
void pros_ed25519_fe_sub(struct pros_ed25519_fe *r,
                         const struct pros_ed25519_fe *a,
                         const struct pros_ed25519_fe *b);
void pros_ed25519_fe_mul(struct pros_ed25519_fe *r,
                         const struct pros_ed25519_fe *a,
                         const struct pros_ed25519_fe *b);
void pros_ed25519_fe_square(struct pros_ed25519_fe *r,
                            const struct pros_ed25519_fe *a);
/* Returns 0 on success; zero has no multiplicative inverse and returns -1. */
int pros_ed25519_fe_invert(struct pros_ed25519_fe *r,
                           const struct pros_ed25519_fe *a);

int pros_ed25519_fe_equal(const struct pros_ed25519_fe *a,
                          const struct pros_ed25519_fe *b);
int pros_ed25519_fe_selftest(void);

#endif

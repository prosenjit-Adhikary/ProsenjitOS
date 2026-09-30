#ifndef PROSENJIT_ED25519_SCALAR_H
#define PROSENJIT_ED25519_SCALAR_H

#include <stdint.h>
#include "ed25519_point.h"

/*
 * Scalars are encoded little-endian modulo the Ed25519 subgroup order
 * L = 2^252 + 27742317777372353535851937790883648493.
 */
void pros_ed25519_scalar_reduce(uint8_t out[32], const uint8_t in[64]);
int pros_ed25519_scalar_is_canonical(const uint8_t scalar[32]);

/* Variable-time multiplication is acceptable for public verification scalars. */
void pros_ed25519_point_scalar_mul(struct pros_ed25519_point *r,
                                   const struct pros_ed25519_point *p,
                                   const uint8_t scalar[32]);
void pros_ed25519_basepoint(struct pros_ed25519_point *p);

int pros_ed25519_scalar_selftest(void);

#endif

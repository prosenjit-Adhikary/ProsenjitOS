#ifndef PROSENJIT_ED25519_POINT_H
#define PROSENJIT_ED25519_POINT_H

#include <stdint.h>
#include "ed25519_field.h"

/*
 * Extended Edwards coordinates for the Ed25519 curve:
 *
 *   -x^2 + y^2 = 1 + d x^2 y^2
 *
 * A point is represented as (X:Y:Z:T), where affine coordinates are
 * x = X/Z, y = Y/Z and T = XY/Z.  The representation permits complete
 * addition formulas without field inversion in the hot path.
 */
struct pros_ed25519_point {
    struct pros_ed25519_fe x;
    struct pros_ed25519_fe y;
    struct pros_ed25519_fe z;
    struct pros_ed25519_fe t;
};

void pros_ed25519_point_identity(struct pros_ed25519_point *p);
int pros_ed25519_point_is_identity(const struct pros_ed25519_point *p);

int pros_ed25519_point_from_bytes(struct pros_ed25519_point *p,
                                  const uint8_t in[32]);
void pros_ed25519_point_to_bytes(uint8_t out[32],
                                 const struct pros_ed25519_point *p);

void pros_ed25519_point_add(struct pros_ed25519_point *r,
                            const struct pros_ed25519_point *p,
                            const struct pros_ed25519_point *q);
void pros_ed25519_point_double(struct pros_ed25519_point *r,
                               const struct pros_ed25519_point *p);

int pros_ed25519_point_on_curve(const struct pros_ed25519_point *p);
int pros_ed25519_point_selftest(void);

#endif

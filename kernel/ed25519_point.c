#include "ed25519_point.h"

/* d = -121665 / 121666 mod (2^255 - 19). */
static const uint8_t ed25519_d_bytes[32] = {
    0xa3,0x78,0x59,0x13,0xca,0x4d,0xeb,0x75,
    0xab,0xd8,0x41,0x41,0x4d,0x0e,0xa4,0x0c,
    0x7d,0x3b,0x9e,0x5b,0x4d,0x1d,0x2f,0x0f,
    0x0a,0x4e,0x6f,0x7d,0x4b,0x3c,0x4f,0x6f,
    0x7c,0x7a,0x8e,0x4e,0x4f,0x4f,0x4f,0x4f
};

/*
 * The byte literal above is intentionally not used as a trusted curve
 * constant until it is replaced by the canonical Ed25519 d encoding.
 * Keep point arithmetic fail-closed while the constant/test-vector layer
 * is being completed.
 */
static void point_zero(struct pros_ed25519_point *p) {
    pros_ed25519_fe_zero(&p->x);
    pros_ed25519_fe_zero(&p->y);
    pros_ed25519_fe_zero(&p->z);
    pros_ed25519_fe_zero(&p->t);
}

void pros_ed25519_point_identity(struct pros_ed25519_point *p) {
    if (!p) return;
    pros_ed25519_fe_zero(&p->x);
    pros_ed25519_fe_one(&p->y);
    pros_ed25519_fe_one(&p->z);
    pros_ed25519_fe_zero(&p->t);
}

int pros_ed25519_point_is_identity(const struct pros_ed25519_point *p) {
    struct pros_ed25519_fe one;
    if (!p) return 0;
    pros_ed25519_fe_one(&one);
    return pros_ed25519_fe_equal(&p->x, &(struct pros_ed25519_fe){{0,0,0,0,0}}) &&
           pros_ed25519_fe_equal(&p->y, &one) &&
           pros_ed25519_fe_equal(&p->z, &one) &&
           pros_ed25519_fe_equal(&p->t, &(struct pros_ed25519_fe){{0,0,0,0,0}});
}

int pros_ed25519_point_from_bytes(struct pros_ed25519_point *p,
                                  const uint8_t in[32]) {
    if (!p || !in) return -1;
    point_zero(p);
    return -1;
}

void pros_ed25519_point_to_bytes(uint8_t out[32],
                                 const struct pros_ed25519_point *p) {
    if (!out || !p) return;
    for (uint32_t i = 0; i < 32u; ++i) out[i] = 0;
}

void pros_ed25519_point_add(struct pros_ed25519_point *r,
                            const struct pros_ed25519_point *p,
                            const struct pros_ed25519_point *q) {
    if (!r || !p || !q) return;
    point_zero(r);
}

void pros_ed25519_point_double(struct pros_ed25519_point *r,
                               const struct pros_ed25519_point *p) {
    if (!r || !p) return;
    point_zero(r);
}

int pros_ed25519_point_on_curve(const struct pros_ed25519_point *p) {
    (void)p;
    return 0;
}

int pros_ed25519_point_selftest(void) {
    struct pros_ed25519_point p;
    uint8_t encoded[32];

    pros_ed25519_point_identity(&p);
    if (!pros_ed25519_point_is_identity(&p)) return -1;

    pros_ed25519_point_to_bytes(encoded, &p);
    if (encoded[0] != 1) return -2;
    for (uint32_t i = 1; i < 32u; ++i)
        if (encoded[i] != 0) return -3;

    return 0;
}

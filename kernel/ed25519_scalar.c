#include "ed25519_scalar.h"

static const uint8_t scalar_l[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,
    0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10
};

static int scalar_cmp(const uint8_t a[32], const uint8_t b[32]) {
    for (int32_t i = 31; i >= 0; --i) {
        if (a[(uint32_t)i] < b[(uint32_t)i]) return -1;
        if (a[(uint32_t)i] > b[(uint32_t)i]) return 1;
    }
    return 0;
}

static void scalar_sub_l(uint8_t a[32]) {
    uint16_t borrow = 0;
    for (uint32_t i = 0; i < 32u; ++i) {
        uint16_t ai = a[i];
        uint16_t bi = (uint16_t)scalar_l[i] + borrow;
        a[i] = (uint8_t)(ai - bi);
        borrow = (ai < bi) ? 1u : 0u;
    }
}

static void scalar_shl1_add(uint8_t a[32], uint8_t bit) {
    uint16_t carry = bit & 1u;
    for (uint32_t i = 0; i < 32u; ++i) {
        uint16_t v = ((uint16_t)a[i] << 1) | carry;
        a[i] = (uint8_t)v;
        carry = (uint16_t)(v >> 8);
    }
}

void pros_ed25519_scalar_reduce(uint8_t out[32], const uint8_t in[64]) {
    for (uint32_t i = 0; i < 32u; ++i) out[i] = 0;

    /* Long division by L, processing the 512-bit input MSB first. */
    for (int32_t bit = 511; bit >= 0; --bit) {
        uint8_t v = (uint8_t)((in[(uint32_t)bit >> 3] >>
                               ((uint32_t)bit & 7u)) & 1u);
        scalar_shl1_add(out, v);
        if (scalar_cmp(out, scalar_l) >= 0)
            scalar_sub_l(out);
    }
}

int pros_ed25519_scalar_is_canonical(const uint8_t scalar[32]) {
    if (!scalar) return 0;
    return scalar_cmp(scalar, scalar_l) < 0;
}

void pros_ed25519_point_scalar_mul(struct pros_ed25519_point *r,
                                   const struct pros_ed25519_point *p,
                                   const uint8_t scalar[32]) {
    struct pros_ed25519_point acc, base;
    if (!r || !p || !scalar) return;

    pros_ed25519_point_identity(&acc);
    base = *p;

    /*
     * Verification scalars are public. Use the straightforward
     * left-to-right double-and-add schedule; no secret is processed here.
     */
    for (int32_t bit = 255; bit >= 0; --bit) {
        struct pros_ed25519_point next;
        pros_ed25519_point_double(&next, &acc);
        acc = next;
        if ((scalar[(uint32_t)bit >> 3] >> ((uint32_t)bit & 7u)) & 1u) {
            pros_ed25519_point_add(&next, &acc, &base);
            acc = next;
        }
    }
    *r = acc;
}

void pros_ed25519_basepoint(struct pros_ed25519_point *p) {
    static const uint8_t encoding[32] = {
        0x58,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
        0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
        0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
        0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66
    };
    if (!p) return;
    if (pros_ed25519_point_from_bytes(p, encoding) != 0)
        pros_ed25519_point_identity(p);
}

int pros_ed25519_scalar_selftest(void) {
    uint8_t reduced[32];
    static const uint8_t zero[64] = { 0 };
    static const uint8_t one[64] = { 1 };
    static const uint8_t l64[64] = {
        0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,
        0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };
    struct pros_ed25519_point b, p;
    uint8_t encoded[32];

    pros_ed25519_scalar_reduce(reduced, zero);
    for (uint32_t i = 0; i < 32u; ++i)
        if (reduced[i] != 0) return -1;

    pros_ed25519_scalar_reduce(reduced, one);
    if (reduced[0] != 1)
        return -2;
    for (uint32_t i = 1; i < 32u; ++i)
        if (reduced[i] != 0) return -3;

    pros_ed25519_scalar_reduce(reduced, l64);
    for (uint32_t i = 0; i < 32u; ++i)
        if (reduced[i] != 0) return -4;

    pros_ed25519_basepoint(&b);
    pros_ed25519_point_scalar_mul(&p, &b, one);
    if (!pros_ed25519_point_on_curve(&p)) return -5;
    pros_ed25519_point_to_bytes(encoded, &p);
    for (uint32_t i = 0; i < 32u; ++i) {
        static const uint8_t expected[32] = {
            0x58,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
            0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
            0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
            0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66
        };
        if (encoded[i] != expected[i]) return -6;
    }
    return 0;
}

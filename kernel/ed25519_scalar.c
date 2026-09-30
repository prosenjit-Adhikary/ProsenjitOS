#i
static void scalar_add_raw(uint8_t out[32], const uint8_t a[32],
                           const uint8_t b[32]) {
    uint16_t carry = 0;
    for (uint32_t i = 0; i < 32u; ++i) {
        uint16_t v = (uint16_t)a[i] + b[i] + carry;
        out[i] = (uint8_t)v;
        carry = (uint16_t)(v >> 8);
    }
    out[32] = (uint8_t)carry;
}

void pros_ed25519_scalar_add(uint8_t out[32], const uint8_t a[32],
                             const uint8_t b[32]) {
    uint8_t wide[64] = {0};
    if (!out || !a || !b) return;
    scalar_add_raw(wide, a, b);
    pros_ed25519_scalar_reduce(out, wide);
}

void pros_ed25519_scalar_sub(uint8_t out[32], const uint8_t a[32],
                             const uint8_t b[32]) {
    uint8_t tmp[32];
    uint16_t borrow = 0;
    if (!out || !a || !b) return;
    for (uint32_t i = 0; i < 32u; ++i) {
        uint16_t ai = a[i];
        uint16_t bi = (uint16_t)b[i] + borrow;
        tmp[i] = (uint8_t)(ai - bi);
        borrow = (ai < bi) ? 1u : 0u;
    }
    if (borrow) {
        uint16_t carry = 0;
        for (uint32_t i = 0; i < 32u; ++i) {
            uint16_t v = (uint16_t)tmp[i] + scalar_l[i] + carry;
            tmp[i] = (uint8_t)v;
            carry = (uint16_t)(v >> 8);
        }
    }
    /* Inputs are reduced scalars in normal use; normalize defensively. */
    {
        uint8_t wide[64] = {0};
        for (uint32_t i = 0; i < 32u; ++i) wide[i] = tmp[i];
        pros_ed25519_scalar_reduce(out, wide);
    }
}

void pros_ed25519_scalar_mul(uint8_t out[32], const uint8_t a[32],
                             const uint8_t b[32]) {
    uint8_t wide[64] = {0};
    if (!out || !a || !b) return;

    for (uint32_t i = 0; i < 32u; ++i) {
        uint16_t carry = 0;
        for (uint32_t j = 0; j < 32u - i; ++j) {
            uint32_t k = i + j;
            uint32_t v = (uint32_t)wide[k] + (uint32_t)a[i] * b[j] + carry;
            wide[k] = (uint8_t)v;
            carry = (uint16_t)(v >> 8);
        }
        uint32_t k = i + (32u - i);
        while (carry && k < 64u) {
            uint32_t v = (uint32_t)wide[k] + carry;
            wide[k] = (uint8_t)v;
            carry = (uint16_t)(v >> 8);
            ++k;
        }
    }
    pros_ed25519_scalar_reduce(out, wide);
}
nclude "ed25519_scalar.h"

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

static int scalar_cmp33(const uint8_t a[33], const uint8_t b[33]) {
    for (int32_t i = 32; i >= 0; --i) {
        if (a[(uint32_t)i] < b[(uint32_t)i]) return -1;
        if (a[(uint32_t)i] > b[(uint32_t)i]) return 1;
    }
    return 0;
}

static void scalar_sub_l33(uint8_t a[33]) {
    uint16_t borrow = 0;
    for (uint32_t i = 0; i < 33u; ++i) {
        uint16_t ai = a[i];
        uint16_t bi = (i < 32u) ? (uint16_t)scalar_l[i] + borrow : borrow;
        a[i] = (uint8_t)(ai - bi);
        borrow = (ai < bi) ? 1u : 0u;
    }
}

static void scalar_shl1_add33(uint8_t a[33], uint8_t bit) {
    uint16_t carry = bit & 1u;
    for (uint32_t i = 0; i < 33u; ++i) {
        uint16_t v = ((uint16_t)a[i] << 1) | carry;
        a[i] = (uint8_t)v;
        carry = (uint16_t)(v >> 8);
    }
}

void pros_ed25519_scalar_reduce(uint8_t out[32], const uint8_t in[64]) {
    uint8_t rem[33] = { 0 };
    uint8_t modulus[33] = { 0 };

    for (uint32_t i = 0; i < 32u; ++i)
        modulus[i] = scalar_l[i];

    /* Long division by L, retaining the 33rd bit of the remainder. */
    for (int32_t bit = 511; bit >= 0; --bit) {
        uint8_t v = (uint8_t)((in[(uint32_t)bit >> 3] >>
                               ((uint32_t)bit & 7u)) & 1u);
        scalar_shl1_add33(rem, v);
        if (scalar_cmp33(rem, modulus) >= 0)
            scalar_sub_l33(rem);
    }

    for (uint32_t i = 0; i < 32u; ++i)
        out[i] = rem[i];
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

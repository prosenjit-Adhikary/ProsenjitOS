#include "ed25519_point.h"

/* d = -121665 / 121666 mod (2^255 - 19), little-endian. */
static const uint8_t ed25519_d_bytes[32] = {
    0xa3,0x78,0x59,0x13,0xca,0x4d,0xeb,0x75,
    0xab,0xd8,0x41,0x41,0x4d,0x0a,0x70,0x80,
    0x98,0x9e,0x77,0x77,0x79,0x40,0xc7,0x8c,
    0x73,0xfe,0x6f,0x2b,0xee,0x6c,0x03,0x52
};

/* sqrt(-1) in F_p, used by the p = 5 (mod 8) square-root recovery. */
static const uint8_t ed25519_sqrt_m1_bytes[32] = {
    0xb0,0xa0,0x0e,0x4a,0xe5,0x7a,0x3b,0x9e,
    0x1f,0x5e,0x8e,0x4d,0x3d,0x5a,0x6a,0x2b,
    0x6a,0x5b,0x3b,0x8b,0x6d,0x2b,0x8a,0x6f,
    0x7c,0x0d,0x0a,0x4e,0x4f,0x3a,0x4d,0x4e
};

static void point_zero(struct pros_ed25519_point *p) {
    pros_ed25519_fe_zero(&p->x);
    pros_ed25519_fe_zero(&p->y);
    pros_ed25519_fe_zero(&p->z);
    pros_ed25519_fe_zero(&p->t);
}

static int fe_is_zero(const struct pros_ed25519_fe *a) {
    struct pros_ed25519_fe z;
    pros_ed25519_fe_zero(&z);
    return pros_ed25519_fe_equal(a, &z);
}

static int fe_is_odd(const struct pros_ed25519_fe *a) {
    uint8_t encoded[32];
    pros_ed25519_fe_to_bytes(encoded, a);
    return encoded[0] & 1u;
}

static void fe_neg(struct pros_ed25519_fe *r,
                   const struct pros_ed25519_fe *a) {
    struct pros_ed25519_fe zero;
    pros_ed25519_fe_zero(&zero);
    pros_ed25519_fe_sub(r, &zero, a);
}

/* Exponentiation by (p+3)/8 = 2^252 - 2. */
static void fe_pow_p3_over_8(struct pros_ed25519_fe *r,
                             const struct pros_ed25519_fe *a) {
    static const uint8_t exponent[32] = {
        0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x0f
    };
    struct pros_ed25519_fe result, base;
    pros_ed25519_fe_one(&result);
    base = *a;

    for (int32_t bit = 254; bit >= 0; --bit) {
        pros_ed25519_fe_square(&result, &result);
        if ((exponent[(uint32_t)bit >> 3] >>
             ((uint32_t)bit & 7u)) & 1u)
            pros_ed25519_fe_mul(&result, &result, &base);
    }
    *r = result;
}

void pros_ed25519_point_identity(struct pros_ed25519_point *p) {
    if (!p) return;
    pros_ed25519_fe_zero(&p->x);
    pros_ed25519_fe_one(&p->y);
    pros_ed25519_fe_one(&p->z);
    pros_ed25519_fe_zero(&p->t);
}

int pros_ed25519_point_is_identity(const struct pros_ed25519_point *p) {
    struct pros_ed25519_fe one, zero;
    if (!p) return 0;
    pros_ed25519_fe_one(&one);
    pros_ed25519_fe_zero(&zero);
    return pros_ed25519_fe_equal(&p->x, &zero) &&
           pros_ed25519_fe_equal(&p->y, &one) &&
           pros_ed25519_fe_equal(&p->z, &one) &&
           pros_ed25519_fe_equal(&p->t, &zero);
}

int pros_ed25519_point_from_bytes(struct pros_ed25519_point *p,
                                  const uint8_t in[32]) {
    struct pros_ed25519_fe y, y2, u, v, inv_v, x, check;
    struct pros_ed25519_fe d, one, sqrt_m1;
    uint8_t y_bytes[32], canonical[32];
    uint8_t sign;

    if (!p || !in) return -1;

    sign = (uint8_t)(in[31] >> 7);
    for (uint32_t i = 0; i < 32u; ++i) y_bytes[i] = in[i];
    y_bytes[31] &= 0x7fu;

    /*
     * Strictly reject non-canonical encodings of y.  Field decoding itself
     * reduces modulo p, so compare the original 255-bit value with its
     * canonical re-encoding after decoding.
     */
    pros_ed25519_fe_from_bytes(&y, y_bytes);
    pros_ed25519_fe_to_bytes(canonical, &y);
    for (uint32_t i = 0; i < 32u; ++i)
        if (canonical[i] != y_bytes[i]) {
            point_zero(p);
            return -2;
        }

    pros_ed25519_fe_one(&one);
    pros_ed25519_fe_from_bytes(&d, ed25519_d_bytes);

    /*
     * x^2 = (y^2 - 1) / (d*y^2 + 1)
     * for -x^2 + y^2 = 1 + d*x^2*y^2.
     */
    pros_ed25519_fe_square(&y2, &y);
    pros_ed25519_fe_sub(&u, &y2, &one);
    pros_ed25519_fe_mul(&v, &d, &y2);
    pros_ed25519_fe_add(&v, &v, &one);

    if (fe_is_zero(&v) || pros_ed25519_fe_invert(&inv_v, &v) != 0) {
        point_zero(p);
        return -3;
    }

    pros_ed25519_fe_mul(&check, &u, &inv_v);
    fe_pow_p3_over_8(&x, &check);

    pros_ed25519_fe_square(&check, &x);
    if (!pros_ed25519_fe_equal(&check, &u)) {
        pros_ed25519_fe_mul(&sqrt_m1,
                            &(struct pros_ed25519_fe){{
                                0,0,0,0,0
                            }},
                            &(struct pros_ed25519_fe){{
                                0,0,0,0,0
                            }});
        pros_ed25519_fe_from_bytes(&sqrt_m1, ed25519_sqrt_m1_bytes);
        pros_ed25519_fe_mul(&x, &x, &sqrt_m1);
        pros_ed25519_fe_square(&check, &x);
        if (!pros_ed25519_fe_equal(&check, &u)) {
            point_zero(p);
            return -4;
        }
    }

    if (fe_is_zero(&x) && sign) {
        point_zero(p);
        return -5;
    }

    if (fe_is_odd(&x) != (int)sign)
        fe_neg(&x, &x);

    p->x = x;
    p->y = y;
    pros_ed25519_fe_one(&p->z);
    pros_ed25519_fe_mul(&p->t, &x, &y);

    if (!pros_ed25519_point_on_curve(p)) {
        point_zero(p);
        return -6;
    }
    return 0;
}

void pros_ed25519_point_to_bytes(uint8_t out[32],
                                 const struct pros_ed25519_point *p) {
    struct pros_ed25519_fe z_inv, x, y;
    if (!out || !p) return;

    for (uint32_t i = 0; i < 32u; ++i) out[i] = 0;
    if (pros_ed25519_fe_invert(&z_inv, &p->z) != 0) return;

    pros_ed25519_fe_mul(&x, &p->x, &z_inv);
    pros_ed25519_fe_mul(&y, &p->y, &z_inv);
    pros_ed25519_fe_to_bytes(out, &y);
    if (fe_is_odd(&x)) out[31] |= 0x80u;
}

void pros_ed25519_point_add(struct pros_ed25519_point *r,
                            const struct pros_ed25519_point *p,
                            const struct pros_ed25519_point *q) {
    struct pros_ed25519_fe A,B,C,D,E,F,G,H,t1,t2,d,two;
    struct pros_ed25519_point out;

    if (!r || !p || !q) return;

    pros_ed25519_fe_from_bytes(&d, ed25519_d_bytes);
    pros_ed25519_fe_from_bytes(&two, (const uint8_t[32]){2});
    pros_ed25519_fe_sub(&t1, &p->y, &p->x);
    pros_ed25519_fe_sub(&t2, &q->y, &q->x);
    pros_ed25519_fe_mul(&A, &t1, &t2);
    pros_ed25519_fe_add(&t1, &p->y, &p->x);
    pros_ed25519_fe_add(&t2, &q->y, &q->x);
    pros_ed25519_fe_mul(&B, &t1, &t2);
    pros_ed25519_fe_mul(&t1, &p->t, &q->t);
    pros_ed25519_fe_mul(&t1, &t1, &d);
    pros_ed25519_fe_mul(&C, &t1, &two);
    pros_ed25519_fe_mul(&D, &p->z, &q->z);
    pros_ed25519_fe_mul(&D, &D, &two);
    pros_ed25519_fe_sub(&E, &B, &A);
    pros_ed25519_fe_sub(&F, &D, &C);
    pros_ed25519_fe_add(&G, &D, &C);
    pros_ed25519_fe_add(&H, &B, &A);

    pros_ed25519_fe_mul(&out.x, &E, &F);
    pros_ed25519_fe_mul(&out.y, &G, &H);
    pros_ed25519_fe_mul(&out.t, &E, &H);
    pros_ed25519_fe_mul(&out.z, &F, &G);
    *r = out;
}

void pros_ed25519_point_double(struct pros_ed25519_point *r,
                               const struct pros_ed25519_point *p) {
    struct pros_ed25519_fe A,B,C,D,E,F,G,H,z2,zero;
    struct pros_ed25519_point out;

    if (!r || !p) return;

    pros_ed25519_fe_square(&A, &p->x);
    pros_ed25519_fe_square(&B, &p->y);
    pros_ed25519_fe_square(&z2, &p->z);
    pros_ed25519_fe_add(&C, &z2, &z2);
    pros_ed25519_fe_zero(&zero);
    pros_ed25519_fe_sub(&D, &zero, &A);
    pros_ed25519_fe_add(&E, &p->x, &p->y);
    pros_ed25519_fe_square(&E, &E);
    pros_ed25519_fe_sub(&E, &E, &A);
    pros_ed25519_fe_sub(&E, &E, &B);
    pros_ed25519_fe_add(&G, &D, &B);
    pros_ed25519_fe_sub(&F, &G, &C);
    pros_ed25519_fe_sub(&H, &D, &B);

    pros_ed25519_fe_mul(&out.x, &E, &F);
    pros_ed25519_fe_mul(&out.y, &G, &H);
    pros_ed25519_fe_mul(&out.t, &E, &H);
    pros_ed25519_fe_mul(&out.z, &F, &G);
    *r = out;
}

int pros_ed25519_point_on_curve(const struct pros_ed25519_point *p) {
    struct pros_ed25519_fe x2,y2,z2,t2,left,right,d;
    if (!p) return 0;

    pros_ed25519_fe_from_bytes(&d, ed25519_d_bytes);
    pros_ed25519_fe_square(&x2, &p->x);
    pros_ed25519_fe_square(&y2, &p->y);
    pros_ed25519_fe_square(&z2, &p->z);
    pros_ed25519_fe_square(&t2, &p->t);

    /* Homogeneous form: -X² + Y² = Z² + d T². */
    pros_ed25519_fe_sub(&left, &y2, &x2);
    pros_ed25519_fe_mul(&right, &d, &t2);
    pros_ed25519_fe_add(&right, &right, &z2);
    return pros_ed25519_fe_equal(&left, &right);
}

int pros_ed25519_point_selftest(void) {
    static const uint8_t base_encoding[32] = {
        0x58,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
        0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
        0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
        0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66
    };
    struct pros_ed25519_point p, decoded;
    uint8_t encoded[32];

    pros_ed25519_point_identity(&p);
    if (!pros_ed25519_point_is_identity(&p)) return -1;
    if (!pros_ed25519_point_on_curve(&p)) return -2;

    pros_ed25519_point_to_bytes(encoded, &p);
    if (encoded[0] != 1) return -3;
    for (uint32_t i = 1; i < 32u; ++i)
        if (encoded[i] != 0) return -4;

    if (pros_ed25519_point_from_bytes(&decoded, encoded) != 0) return -5;
    if (!pros_ed25519_point_is_identity(&decoded)) return -6;

    if (pros_ed25519_point_from_bytes(&decoded, base_encoding) != 0) return -7;
    if (!pros_ed25519_point_on_curve(&decoded)) return -8;
    pros_ed25519_point_to_bytes(encoded, &decoded);
    for (uint32_t i = 0; i < 32u; ++i)
        if (encoded[i] != base_encoding[i]) return -9;

    pros_ed25519_point_add(&p, &p, &p);
    if (!pros_ed25519_point_is_identity(&p)) return -10;
    if (!pros_ed25519_point_on_curve(&p)) return -11;

    pros_ed25519_point_double(&p, &p);
    if (!pros_ed25519_point_is_identity(&p)) return -12;
    if (!pros_ed25519_point_on_curve(&p)) return -13;

    return 0;
}

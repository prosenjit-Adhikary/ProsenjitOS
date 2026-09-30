#include "ed25519_point.h"

/* d = -121665 / 121666 mod (2^255 - 19), little-endian. */
static const uint8_t ed25519_d_bytes[32] = {
    0xa3,0x78,0x59,0x13,0xca,0x4d,0xeb,0x75,
    0xab,0xd8,0x41,0x41,0x4d,0x0a,0x70,0x80,
    0x98,0x9e,0x77,0x77,0x79,0x40,0xc7,0x8c,
    0x73,0xfe,0x6f,0x2b,0xee,0x6c,0x03,0x52
};

static void point_zero(struct pros_ed25519_point *p) {
    pros_ed25519_fe_zero(&p->x);
    pros_ed25519_fe_zero(&p->y);
    pros_ed25519_fe_zero(&p->z);
    pros_ed25519_fe_zero(&p->t);
}

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
    struct pros_ed25519_fe A,B,C,D,E,F,G,H,z2;
    struct pros_ed25519_point out;

    if (!r || !p) return;

    pros_ed25519_fe_square(&A, &p->x);
    pros_ed25519_fe_square(&B, &p->y);
    pros_ed25519_fe_square(&z2, &p->z);
    pros_ed25519_fe_add(&C, &z2, &z2);
    pros_ed25519_fe_sub(&D, &(struct pros_ed25519_fe){{0,0,0,0,0}}, &A);
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

    /* Homogeneous form:
     * -X² + Y² = Z² + d T².
     */
    pros_ed25519_fe_sub(&left, &y2, &x2);
    pros_ed25519_fe_mul(&right, &d, &t2);
    pros_ed25519_fe_add(&right, &right, &z2);
    return pros_ed25519_fe_equal(&left, &right);
}

int pros_ed25519_point_selftest(void) {
    struct pros_ed25519_point p;
    uint8_t encoded[32];

    pros_ed25519_point_identity(&p);
    if (!pros_ed25519_point_is_identity(&p)) return -1;
    if (!pros_ed25519_point_on_curve(&p)) return -2;

    pros_ed25519_point_to_bytes(encoded, &p);
    if (encoded[0] != 1) return -3;
    for (uint32_t i = 1; i < 32u; ++i)
        if (encoded[i] != 0) return -4;

    pros_ed25519_point_add(&p, &p, &p);
    if (!pros_ed25519_point_is_identity(&p)) return -5;
    if (!pros_ed25519_point_on_curve(&p)) return -6;

    pros_ed25519_point_double(&p, &p);
    if (!pros_ed25519_point_is_identity(&p)) return -7;
    if (!pros_ed25519_point_on_curve(&p)) return -8;

    return 0;
}

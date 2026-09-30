#include "ed25519_field.h"

#define FE_MASK ((uint64_t)((1ULL << 51) - 1ULL))

static void fe_carry(struct pros_ed25519_fe *r) {
    for (uint32_t pass = 0; pass < 2u; ++pass) {
        uint64_t c;
        c = r->v[0] >> 51; r->v[0] &= FE_MASK; r->v[1] += c;
        c = r->v[1] >> 51; r->v[1] &= FE_MASK; r->v[2] += c;
        c = r->v[2] >> 51; r->v[2] &= FE_MASK; r->v[3] += c;
        c = r->v[3] >> 51; r->v[3] &= FE_MASK; r->v[4] += c;
        c = r->v[4] >> 51; r->v[4] &= FE_MASK; r->v[0] += c * 19u;
    }
}

static void fe_reduce(struct pros_ed25519_fe *r) {
    fe_carry(r);
    struct pros_ed25519_fe p = {{
        FE_MASK - 18u, FE_MASK, FE_MASK, FE_MASK, (1ULL << 51) - 1ULL
    }};
    /* The first limb of p is 2^51-19; remaining limbs are 2^51-1. */
    uint64_t borrow = 0;
    struct pros_ed25519_fe t;
    for (uint32_t i = 0; i < 5u; ++i) {
        uint64_t ai = r->v[i];
        uint64_t bi = p.v[i] + borrow;
        uint64_t nb = (ai < bi);
        t.v[i] = ai - bi;
        borrow = nb;
    }
    if (!borrow) *r = t;
}

void pros_ed25519_fe_zero(struct pros_ed25519_fe *r) {
    for (uint32_t i=0;i<5u;++i) r->v[i]=0;
}

void pros_ed25519_fe_one(struct pros_ed25519_fe *r) {
    pros_ed25519_fe_zero(r); r->v[0]=1;
}

static uint64_t load_le64_partial(const uint8_t *p, uint32_t n) {
    uint64_t x=0;
    for (uint32_t i=0;i<n;++i) x |= (uint64_t)p[i] << (8u*i);
    return x;
}

void pros_ed25519_fe_from_bytes(struct pros_ed25519_fe *r,const uint8_t in[32]) {
    uint64_t t0=load_le64_partial(in,8);
    uint64_t t1=load_le64_partial(in+8,8);
    uint64_t t2=load_le64_partial(in+16,8);
    uint64_t t3=load_le64_partial(in+24,8);
    t3 &= (1ULL<<63)-1ULL;
    r->v[0]=t0 & FE_MASK;
    r->v[1]=((t0>>51) | (t1<<13)) & FE_MASK;
    r->v[2]=((t1>>38) | (t2<<26)) & FE_MASK;
    r->v[3]=((t2>>25) | (t3<<39)) & FE_MASK;
    r->v[4]=(t3>>12) & FE_MASK;
    fe_reduce(r);
}

static void store_le64(uint8_t *p,uint64_t x) {
    for(uint32_t i=0;i<8u;++i) p[i]=(uint8_t)(x>>(8u*i));
}

void pros_ed25519_fe_to_bytes(uint8_t out[32],const struct pros_ed25519_fe *a) {
    struct pros_ed25519_fe t=*a;
    fe_reduce(&t);
    uint64_t x0=t.v[0] | (t.v[1]<<51);
    uint64_t x1=(t.v[1]>>13) | (t.v[2]<<38);
    uint64_t x2=(t.v[2]>>26) | (t.v[3]<<25);
    uint64_t x3=(t.v[3]>>39) | (t.v[4]<<12);
    store_le64(out,x0); store_le64(out+8,x1);
    store_le64(out+16,x2); store_le64(out+24,x3);
}

void pros_ed25519_fe_add(struct pros_ed25519_fe *r,const struct pros_ed25519_fe *a,const struct pros_ed25519_fe *b) {
    for(uint32_t i=0;i<5u;++i) r->v[i]=a->v[i]+b->v[i];
    fe_reduce(r);
}

void pros_ed25519_fe_sub(struct pros_ed25519_fe *r,const struct pros_ed25519_fe *a,const struct pros_ed25519_fe *b) {
    /*
     * Add 2p before subtraction so unsigned limb arithmetic cannot underflow.
     * The final reduction returns the canonical representative.
     */
    static const uint64_t two_p[5] = {
        (1ULL<<51)-38ULL, (1ULL<<52)-2ULL, (1ULL<<52)-2ULL,
        (1ULL<<52)-2ULL, (1ULL<<52)-2ULL
    };
    uint64_t borrow=0;
    for(uint32_t i=0;i<5u;++i) {
        uint64_t bi=b->v[i]+borrow;
        uint64_t nb=(two_p[i]+a->v[i] < bi);
        r->v[i]=two_p[i]+a->v[i]-bi;
        borrow=nb;
    }
    fe_reduce(r);
}

void pros_ed25519_fe_mul(struct pros_ed25519_fe *r,const struct pros_ed25519_fe *a,const struct pros_ed25519_fe *b) {
    __uint128_t t0, t1, t2, t3, t4;
    uint64_t c;

    /*
     * 5x51 multiplication with the 2^255 = 19 reduction folded into
     * the low limbs.  Keep the high-limb fold explicit so the carry from
     * limb 4 is propagated through limb 0 before canonical reduction.
     */
    t0 = (__uint128_t)a->v[0]*b->v[0]
       + 19u*((__uint128_t)a->v[1]*b->v[4]
            + (__uint128_t)a->v[2]*b->v[3]
            + (__uint128_t)a->v[3]*b->v[2]
            + (__uint128_t)a->v[4]*b->v[1]);

    t1 = (__uint128_t)a->v[0]*b->v[1]
       + (__uint128_t)a->v[1]*b->v[0]
       + 19u*((__uint128_t)a->v[2]*b->v[4]
            + (__uint128_t)a->v[3]*b->v[3]
            + (__uint128_t)a->v[4]*b->v[2]);

    t2 = (__uint128_t)a->v[0]*b->v[2]
       + (__uint128_t)a->v[1]*b->v[1]
       + (__uint128_t)a->v[2]*b->v[0]
       + 19u*((__uint128_t)a->v[3]*b->v[4]
            + (__uint128_t)a->v[4]*b->v[3]);

    t3 = (__uint128_t)a->v[0]*b->v[3]
       + (__uint128_t)a->v[1]*b->v[2]
       + (__uint128_t)a->v[2]*b->v[1]
       + (__uint128_t)a->v[3]*b->v[0]
       + 19u*((__uint128_t)a->v[4]*b->v[4]);

    t4 = (__uint128_t)a->v[0]*b->v[4]
       + (__uint128_t)a->v[1]*b->v[3]
       + (__uint128_t)a->v[2]*b->v[2]
       + (__uint128_t)a->v[3]*b->v[1]
       + (__uint128_t)a->v[4]*b->v[0];

    c = (uint64_t)(t0 >> 51); t0 &= FE_MASK; t1 += c;
    c = (uint64_t)(t1 >> 51); t1 &= FE_MASK; t2 += c;
    c = (uint64_t)(t2 >> 51); t2 &= FE_MASK; t3 += c;
    c = (uint64_t)(t3 >> 51); t3 &= FE_MASK; t4 += c;
    c = (uint64_t)(t4 >> 51); t4 &= FE_MASK; t0 += (__uint128_t)c * 19u;
    c = (uint64_t)(t0 >> 51); t0 &= FE_MASK; t1 += c;

    r->v[0]=(uint64_t)t0;
    r->v[1]=(uint64_t)t1;
    r->v[2]=(uint64_t)t2;
    r->v[3]=(uint64_t)t3;
    r->v[4]=(uint64_t)t4;
    fe_reduce(r);
}

void pros_ed25519_fe_square(struct pros_ed25519_fe *r,const struct pros_ed25519_fe *a) {
    pros_ed25519_fe_mul(r,a,a);
}

/*
 * Compute a^(p-2) with p = 2^255 - 19, which is the multiplicative
 * inverse for every non-zero field element. The exponent is fixed, so
 * the operation schedule is independent of the input value.
 */
int pros_ed25519_fe_invert(struct pros_ed25519_fe *r,const struct pros_ed25519_fe *a) {
    static const uint8_t exponent[32] = {
        0xeb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f
    };
    struct pros_ed25519_fe result, base;
    uint8_t nonzero = 0;

    for (uint32_t i = 0; i < 5u; ++i)
        nonzero |= (uint8_t)(a->v[i] != 0);
    if (!nonzero) {
        pros_ed25519_fe_zero(r);
        return -1;
    }

    result.v[0] = 1;
    result.v[1] = result.v[2] = result.v[3] = result.v[4] = 0;
    base = *a;

    for (int32_t bit = 254; bit >= 0; --bit) {
        pros_ed25519_fe_square(&result, &result);
        if ((exponent[(uint32_t)bit >> 3] >>
             ((uint32_t)bit & 7u)) & 1u)
            pros_ed25519_fe_mul(&result, &result, &base);
    }
    *r = result;
    return 0;
}

int pros_ed25519_fe_equal(const struct pros_ed25519_fe *a,const struct pros_ed25519_fe *b) {
    uint8_t aa[32],bb[32],diff=0;
    pros_ed25519_fe_to_bytes(aa,a);
    pros_ed25519_fe_to_bytes(bb,b);
    for(uint32_t i=0;i<32u;++i) diff|=(uint8_t)(aa[i]^bb[i]);
    return diff==0;
}

int pros_ed25519_fe_selftest(void) {
    static const uint8_t two[32] = { 2 };
    static const uint8_t three[32] = { 3 };
    static const uint8_t five[32] = { 5 };
    static const uint8_t p_minus_one[32] = {
        0xec,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f
    };
    static const uint8_t p[32] = {
        0xed,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f
    };
    struct pros_ed25519_fe a,b,r,expected;
    uint8_t out[32];

    /* 2 + 2 = 4. */
    pros_ed25519_fe_from_bytes(&a, two);
    pros_ed25519_fe_from_bytes(&b, two);
    pros_ed25519_fe_add(&r, &a, &b);
    pros_ed25519_fe_to_bytes(out, &r);
    if (out[0] != 4) return -1;
    for (uint32_t i = 1; i < 32u; ++i)
        if (out[i] != 0) return -2;

    /* (p - 1) + 1 = 0. */
    pros_ed25519_fe_from_bytes(&a, p_minus_one);
    pros_ed25519_fe_one(&b);
    pros_ed25519_fe_add(&r, &a, &b);
    pros_ed25519_fe_to_bytes(out, &r);
    for (uint32_t i = 0; i < 32u; ++i)
        if (out[i] != 0) return -3;

    /* 2^2 = 4. */
    pros_ed25519_fe_from_bytes(&a, two);
    pros_ed25519_fe_square(&r, &a);
    pros_ed25519_fe_from_bytes(&expected, (const uint8_t[32]){ 4 });
    if (!pros_ed25519_fe_equal(&r, &expected)) return -4;

    /* 2 * 3 = 6. */
    pros_ed25519_fe_from_bytes(&a, two);
    pros_ed25519_fe_from_bytes(&b, three);
    pros_ed25519_fe_mul(&r, &a, &b);
    pros_ed25519_fe_to_bytes(out, &r);
    if (out[0] != 6) return -5;
    for (uint32_t i = 1; i < 32u; ++i)
        if (out[i] != 0) return -6;

    /* 5 - 3 = 2. */
    pros_ed25519_fe_from_bytes(&a, five);
    pros_ed25519_fe_from_bytes(&b, three);
    pros_ed25519_fe_sub(&r, &a, &b);
    pros_ed25519_fe_from_bytes(&expected, two);
    if (!pros_ed25519_fe_equal(&r, &expected)) return -7;

    /* Encoding the modulus must canonicalize to zero. */
    pros_ed25519_fe_from_bytes(&a, p);
    pros_ed25519_fe_zero(&expected);
    if (!pros_ed25519_fe_equal(&a, &expected)) return -8;

    /* Decode/encode of a canonical small value must be stable. */
    pros_ed25519_fe_from_bytes(&a, three);
    pros_ed25519_fe_to_bytes(out, &a);
    if (out[0] != 3) return -9;
    for (uint32_t i = 1; i < 32u; ++i)
        if (out[i] != 0) return -10;

    /* 3 * inverse(3) = 1. */
    if (pros_ed25519_fe_invert(&r, &a) != 0) return -11;
    pros_ed25519_fe_mul(&r, &r, &a);
    pros_ed25519_fe_one(&expected);
    if (!pros_ed25519_fe_equal(&r, &expected)) return -12;

    /* Zero has no inverse and must fail closed. */
    pros_ed25519_fe_zero(&a);
    if (pros_ed25519_fe_invert(&r, &a) != -1) return -13;

    return 0;
}

#include "security_file.h"

#include <stdint.h>

static uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t bsig0(uint32_t x) {
    return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}

static uint32_t bsig1(uint32_t x) {
    return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}

static uint32_t ssig0(uint32_t x) {
    return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}

static uint32_t ssig1(uint32_t x) {
    return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

static uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static const uint32_t k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void sha256_transform(struct pros_sha256_ctx *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a,b,c,d,e,f,g,h;

    for (uint32_t i = 0; i < 16; ++i)
        w[i] = load_be32(block + i * 4u);
    for (uint32_t i = 16; i < 64; ++i)
        w[i] = ssig1(w[i - 2]) + w[i - 7] +
               ssig0(w[i - 15]) + w[i - 16];

    a=ctx->h[0]; b=ctx->h[1]; c=ctx->h[2]; d=ctx->h[3];
    e=ctx->h[4]; f=ctx->h[5]; g=ctx->h[6]; h=ctx->h[7];

    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t t1 = h + bsig1(e) + ch(e,f,g) + k[i] + w[i];
        uint32_t t2 = bsig0(a) + maj(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }

    ctx->h[0]+=a; ctx->h[1]+=b; ctx->h[2]+=c; ctx->h[3]+=d;
    ctx->h[4]+=e; ctx->h[5]+=f; ctx->h[6]+=g; ctx->h[7]+=h;
}

static void sha256_init(struct pros_sha256_ctx *ctx) {
    ctx->h[0]=0x6a09e667u; ctx->h[1]=0xbb67ae85u;
    ctx->h[2]=0x3c6ef372u; ctx->h[3]=0xa54ff53au;
    ctx->h[4]=0x510e527fu; ctx->h[5]=0x9b05688cu;
    ctx->h[6]=0x1f83d9abu; ctx->h[7]=0x5be0cd19u;
    ctx->bit_count=0;
    ctx->used=0;
}

static void sha256_update(struct pros_sha256_ctx *ctx,
                          const uint8_t *data, uint64_t length) {
    while (length != 0) {
        uint32_t room = 64u - ctx->used;
        uint32_t take = (length < room) ? (uint32_t)length : room;
        for (uint32_t i = 0; i < take; ++i)
            ctx->block[ctx->used + i] = data[i];
        ctx->used += take;
        ctx->bit_count += (uint64_t)take * 8u;
        data += take;
        length -= take;
        if (ctx->used == 64u) {
            sha256_transform(ctx, ctx->block);
            ctx->used=0;
        }
    }
}

static void sha256_final(struct pros_sha256_ctx *ctx,
                         uint8_t digest[PROS_FILE_SHA256_BYTES]) {
    uint64_t bits = ctx->bit_count;
    ctx->block[ctx->used++] = 0x80u;

    if (ctx->used > 56u) {
        while (ctx->used < 64u) ctx->block[ctx->used++] = 0;
        sha256_transform(ctx, ctx->block);
        ctx->used=0;
    }
    while (ctx->used < 56u) ctx->block[ctx->used++] = 0;

    for (uint32_t i = 0; i < 8; ++i)
        ctx->block[56u + i] = (uint8_t)(bits >> (56u - i * 8u));
    sha256_transform(ctx, ctx->block);

    for (uint32_t i = 0; i < 8; ++i)
        store_be32(digest + i * 4u, ctx->h[i]);
}

void pros_sha256(const uint8_t *data, uint64_t length,
                 uint8_t digest[PROS_FILE_SHA256_BYTES]) {
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    if (length != 0 && data != 0)
        sha256_update(&ctx, data, length);
    sha256_final(&ctx, digest);
}

int pros_file_admit(const struct pros_file_security_metadata *metadata,
                    struct pros_file_security_result *result) {
    uint32_t evidence = PROS_FILE_EVIDENCE_NONE;

    if (!metadata || !result)
        return -1;

    if (metadata->format_valid)
        evidence |= PROS_FILE_EVIDENCE_FORMAT_VALID;
    if (metadata->signature_valid)
        evidence |= PROS_FILE_EVIDENCE_SIGNATURE_VALID;
    if (metadata->signature_present && !metadata->signature_valid)
        evidence |= PROS_FILE_EVIDENCE_SIGNATURE_INVALID;
    if (metadata->local_trusted)
        evidence |= PROS_FILE_EVIDENCE_LOCAL_TRUST;
    if (metadata->local_denied)
        evidence |= PROS_FILE_EVIDENCE_LOCAL_DENY;
    if (metadata->provenance_known)
        evidence |= PROS_FILE_EVIDENCE_PROVENANCE_KNOWN;

    switch (metadata->reputation) {
    case PROS_FILE_REPUTATION_CLEAN:
        evidence |= PROS_FILE_EVIDENCE_REPUTATION_CLEAN;
        break;
    case PROS_FILE_REPUTATION_SUSPICIOUS:
        evidence |= PROS_FILE_EVIDENCE_REPUTATION_SUSPICIOUS;
        break;
    case PROS_FILE_REPUTATION_MALICIOUS:
        evidence |= PROS_FILE_EVIDENCE_REPUTATION_MALICIOUS;
        break;
    default:
        break;
    }

    result->evidence = evidence;

    if (!metadata->format_valid || metadata->local_denied ||
        metadata->reputation == PROS_FILE_REPUTATION_MALICIOUS) {
        result->state = PROS_FILE_MALICIOUS;
        result->action = PROS_FILE_BLOCK;
        return 0;
    }

    if (metadata->reputation == PROS_FILE_REPUTATION_SUSPICIOUS ||
        (metadata->signature_present && !metadata->signature_valid)) {
        result->state = PROS_FILE_SUSPICIOUS;
        result->action = PROS_FILE_QUARANTINE;
        return 0;
    }

    /*
     * Local trust is an explicit policy decision and may authorize a package
     * even when no signature is present.  It is deliberately distinct from
     * cryptographic verification: callers must not turn an arbitrary
     * signature_valid flag into a cryptographic proof.
     */
    if (metadata->local_trusted && metadata->format_valid &&
        !metadata->signature_present) {
        result->state = PROS_FILE_TRUSTED;
        result->action = PROS_FILE_ALLOW;
        return 0;
    }

    if (metadata->local_trusted && metadata->signature_valid) {
        result->state = PROS_FILE_TRUSTED;
        result->action = PROS_FILE_ALLOW;
        return 0;
    }

    if (metadata->signature_valid && metadata->format_valid) {
        result->state = PROS_FILE_VERIFIED;
        result->action = PROS_FILE_ALLOW;
        return 0;
    }

    result->state = PROS_FILE_UNKNOWN;
    result->action = PROS_FILE_QUARANTINE;
    return 0;
}

static int digest_equals(const uint8_t *a, const uint8_t *b) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < PROS_FILE_SHA256_BYTES; ++i)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

int pros_file_security_selftest(void) {
    static const uint8_t expected[PROS_FILE_SHA256_BYTES] = {
        0xBA,0x78,0x16,0xBF,0x8F,0x01,0xCF,0xEA,
        0x41,0x41,0x40,0xDE,0x5D,0xAE,0x22,0x23,
        0xB0,0x03,0x61,0xA3,0x96,0x17,0x7A,0x9C,
        0xB4,0x10,0xFF,0x61,0xF2,0x00,0x15,0xAD
    };
    static const uint8_t abc[] = { 'a','b','c' };
    uint8_t digest[PROS_FILE_SHA256_BYTES];
    struct pros_file_security_metadata m;
    struct pros_file_security_result r;

    pros_sha256(abc, 3, digest);
    if (!digest_equals(digest, expected))
        return -1;

    m = (struct pros_file_security_metadata){
        .format_valid=1, .signature_valid=1, .signature_present=1,
        .local_trusted=1, .local_denied=0,
        .reputation=PROS_FILE_REPUTATION_CLEAN, .provenance_known=1
    };
    if (pros_file_admit(&m, &r) != 0 ||
        r.state != PROS_FILE_TRUSTED || r.action != PROS_FILE_ALLOW)
        return -1;

    m = (struct pros_file_security_metadata){
        .format_valid=1, .signature_valid=0, .signature_present=0,
        .local_trusted=1, .local_denied=0,
        .reputation=PROS_FILE_REPUTATION_CLEAN, .provenance_known=1
    };
    if (pros_file_admit(&m, &r) != 0 ||
        r.state != PROS_FILE_TRUSTED || r.action != PROS_FILE_ALLOW)
        return -1;

    m = (struct pros_file_security_metadata){
        .format_valid=1, .signature_valid=1, .signature_present=1,
        .local_trusted=0, .local_denied=0,
        .reputation=PROS_FILE_REPUTATION_CLEAN, .provenance_known=1
    };

    m.reputation = PROS_FILE_REPUTATION_SUSPICIOUS;
    if (pros_file_admit(&m, &r) != 0 ||
        r.state != PROS_FILE_SUSPICIOUS ||
        r.action != PROS_FILE_QUARANTINE)
        return -1;

    m.reputation = PROS_FILE_REPUTATION_MALICIOUS;
    if (pros_file_admit(&m, &r) != 0 ||
        r.state != PROS_FILE_MALICIOUS || r.action != PROS_FILE_BLOCK)
        return -1;

    m.reputation = PROS_FILE_REPUTATION_UNAVAILABLE;
    m.local_trusted = 0;
    m.signature_valid = 0;
    m.signature_present = 0;
    if (pros_file_admit(&m, &r) != 0 ||
        r.state != PROS_FILE_UNKNOWN ||
        r.action != PROS_FILE_QUARANTINE)
        return -1;

    return 0;
}

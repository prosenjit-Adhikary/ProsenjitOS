#include "sha512.h"

struct sha512_ctx {
    uint64_t h[8];
    uint64_t bit_hi;
    uint64_t bit_lo;
    uint8_t block[128];
    uint32_t used;
};

static uint64_t rotr64(uint64_t x, uint32_t n) {
    return (x >> n) | (x << (64u - n));
}

static uint64_t ch64(uint64_t x, uint64_t y, uint64_t z) {
    return (x & y) ^ (~x & z);
}

static uint64_t maj64(uint64_t x, uint64_t y, uint64_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint64_t bsig0(uint64_t x) {
    return rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39);
}

static uint64_t bsig1(uint64_t x) {
    return rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41);
}

static uint64_t ssig0(uint64_t x) {
    return rotr64(x, 1) ^ rotr64(x, 8) ^ (x >> 7);
}

static uint64_t ssig1(uint64_t x) {
    return rotr64(x, 19) ^ rotr64(x, 61) ^ (x >> 6);
}

static uint64_t load_be64(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static void store_be64(uint8_t *p, uint64_t v) {
    p[0]=(uint8_t)(v>>56); p[1]=(uint8_t)(v>>48);
    p[2]=(uint8_t)(v>>40); p[3]=(uint8_t)(v>>32);
    p[4]=(uint8_t)(v>>24); p[5]=(uint8_t)(v>>16);
    p[6]=(uint8_t)(v>>8); p[7]=(uint8_t)v;
}

static const uint64_t k[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,
0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,
0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,0xd807aa98a3030242ULL,
0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,
0xc19bf174cf692694ULL,0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,
0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,
0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,
0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,
0x06ca6351e003826fULL,0x142929670a0e6e70ULL,0x27b70a8546d22ffcULL,
0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,
0x92722c851482353bULL,0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,
0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,
0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,
0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,
0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,0x748f82ee5defb2fcULL,
0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,
0xc67178f2e372532bULL,0xca273eceea26619cULL,0xd186b8c721c0c207ULL,
0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,
0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,
0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,
0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

static void sha512_transform(struct sha512_ctx *ctx, const uint8_t block[128]) {
    uint64_t w[80],a,b,c,d,e,f,g,h;
    for (uint32_t i=0;i<16u;++i) w[i]=load_be64(block+i*8u);
    for (uint32_t i=16;i<80u;++i)
        w[i]=ssig1(w[i-2u])+w[i-7u]+ssig0(w[i-15u])+w[i-16u];
    a=ctx->h[0];b=ctx->h[1];c=ctx->h[2];d=ctx->h[3];
    e=ctx->h[4];f=ctx->h[5];g=ctx->h[6];h=ctx->h[7];
    for (uint32_t i=0;i<80u;++i) {
        uint64_t t1=h+bsig1(e)+ch64(e,f,g)+k[i]+w[i];
        uint64_t t2=bsig0(a)+maj64(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    ctx->h[0]+=a;ctx->h[1]+=b;ctx->h[2]+=c;ctx->h[3]+=d;
    ctx->h[4]+=e;ctx->h[5]+=f;ctx->h[6]+=g;ctx->h[7]+=h;
}

static void sha512_init(struct sha512_ctx *ctx) {
    static const uint64_t iv[8] = {
        0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL
    };
    for (uint32_t i=0;i<8u;++i) ctx->h[i]=iv[i];
    ctx->bit_hi=0;ctx->bit_lo=0;ctx->used=0;
}

static void sha512_update(struct sha512_ctx *ctx,const uint8_t *data,uint64_t length) {
    while (length) {
        uint32_t room=128u-ctx->used;
        uint32_t take=(length<room)?(uint32_t)length:room;
        for (uint32_t i=0;i<take;++i) ctx->block[ctx->used+i]=data[i];
        ctx->used+=take;
        uint64_t add=(uint64_t)take*8u;
        uint64_t old=ctx->bit_lo;
        ctx->bit_lo+=add;
        if (ctx->bit_lo<old) ++ctx->bit_hi;
        data+=take;length-=take;
        if (ctx->used==128u) { sha512_transform(ctx,ctx->block);ctx->used=0; }
    }
}

static void sha512_final(struct sha512_ctx *ctx,uint8_t digest[PROS_SHA512_BYTES]) {
    ctx->block[ctx->used++]=0x80u;
    if (ctx->used>112u) {
        while(ctx->used<128u) ctx->block[ctx->used++]=0;
        sha512_transform(ctx,ctx->block);ctx->used=0;
    }
    while(ctx->used<112u) ctx->block[ctx->used++]=0;
    store_be64(ctx->block+112u,ctx->bit_hi);
    store_be64(ctx->block+120u,ctx->bit_lo);
    sha512_transform(ctx,ctx->block);
    for(uint32_t i=0;i<8u;++i) store_be64(digest+i*8u,ctx->h[i]);
}

void pros_sha512(const uint8_t *data,uint64_t length,uint8_t digest[PROS_SHA512_BYTES]) {
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    if (length && data) sha512_update(&ctx,data,length);
    sha512_final(&ctx,digest);
}

void pros_sha512_prefix(const uint8_t prefix[64], const uint8_t *data,
                        uint64_t length, uint8_t digest[PROS_SHA512_BYTES]) {
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, prefix, 64u);
    if (length && data) sha512_update(&ctx, data, length);
    sha512_final(&ctx, digest);
}

static int digest_equal(const uint8_t *a,const uint8_t *b) {
    uint8_t diff=0;
    for(uint32_t i=0;i<PROS_SHA512_BYTES;++i) diff|=(uint8_t)(a[i]^b[i]);
    return diff==0;
}

static int digest_matches_hex(const uint8_t digest[PROS_SHA512_BYTES],
                              const char *hex) {
    static const char digits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < PROS_SHA512_BYTES; ++i) {
        if (digits[digest[i] >> 4] != hex[i * 2u] ||
            digits[digest[i] & 0x0fu] != hex[i * 2u + 1u])
            return 0;
    }
    return hex[PROS_SHA512_BYTES * 2u] == '\0';
}

int pros_sha512_selftest(void) {
    static const uint8_t abc[] = {'a','b','c'};
    static const char abc_expected[] =
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
    static const struct {
        uint32_t length;
        const char *expected;
    } boundary_vectors[] = {
        { 0u, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
              "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e" },
        { 111u, "68cffa6d0d76f309c9ce0d35280939f8e25990c43b7b086ccdf709be35b07d4d"
                "dba599541ff2b1c19d34ea49aeafb9659adb7ac3c0b078bb30a22d57fc6687ef" },
        { 112u, "d0865c524d1dddf7c23b799c413f5adcd7caefd3f66a9b49750ec81066012c25"
                "a8bcf94ddea6dc525691673097ca40e0101e897fc97218cfdb0704084e2bef4b" },
        { 127u, "e0b6a20f1c0c88970a9340152cd5a1c1ecf3d3b8de5510274187943807947354"
                "0133b812706e5dbec322c8c9523b6fc8c6d16ee626e87ad5fe3d2916afedc369" },
        { 128u, "99b16f17aa0b969a5b8f08f367719d516e330ccd2660b6f0688ec031dbc783de"
                "50a1cd185a2568dba75070a2403d17d4741d163578515dfd2ff756ddfe4d47b1" },
        { 129u, "a1556e29185778aa5991e34b8884c840d589f0fbb4b8ed590e51e9ac4eb03a008"
                "125000db2671f8fe7f485b59a77b518670078ecb41a54b4cd02a7f1d2ca4c6d" }
    };
    uint8_t digest[PROS_SHA512_BYTES];
    uint8_t message[129];

    pros_sha512(abc, sizeof(abc), digest);
    if (!digest_matches_hex(digest, abc_expected))
        return -1;

    /* Exercise both sides of SHA-512's 112-byte padding threshold and block
     * transitions. Bytes are deterministic: message[i] = (i * 7 + 3) mod 256. */
    for (uint32_t i = 0; i < sizeof(message); ++i)
        message[i] = (uint8_t)(i * 7u + 3u);

    for (uint32_t i = 0;
         i < sizeof(boundary_vectors) / sizeof(boundary_vectors[0]); ++i) {
        pros_sha512(message, boundary_vectors[i].length, digest);
        if (!digest_matches_hex(digest, boundary_vectors[i].expected))
            return -2 - (int)i;
    }

    /* A null digest must not be dereferenced. A non-empty null input is
     * deliberately not tested as a valid hash because the API has no error
     * return channel for malformed arguments. */
    pros_sha512(abc, sizeof(abc), 0);
    pros_sha512(0, 0, digest);
    if (!digest_matches_hex(digest, boundary_vectors[0].expected))
        return -8;

    return 0;
}

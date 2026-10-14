#include "security_signature.h"
#include "ed25519_point.h"
#include "ed25519_scalar.h"
#include "sha512.h"

static int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t length) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < length; ++i)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static int point_is_identity_times_eight(const struct pros_ed25519_point *p) {
    struct pros_ed25519_point t = *p;
    for (uint32_t i = 0; i < 3u; ++i)
        pros_ed25519_point_double(&t, &t);
    return pros_ed25519_point_is_identity(&t);
}

/* Reject mixed-order points, not just points killed by multiplication by 8. */
static int point_in_prime_order_subgroup(const struct pros_ed25519_point *p) {
    static const uint8_t subgroup_order[32] = {
        0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,
        0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10
    };
    struct pros_ed25519_point multiple;
    if (!p)
        return 0;
    pros_ed25519_point_scalar_mul(&multiple, p, subgroup_order);
    return pros_ed25519_point_is_identity(&multiple);
}

static int ed25519_verify(const uint8_t *data, uint64_t data_length,
                          const uint8_t signature[64],
                          const uint8_t public_key[32]) {
    struct pros_ed25519_point r_point, a_point, k_a, s_b, rhs, base;
    uint8_t encoded[32];
    uint8_t digest[PROS_SHA512_BYTES];
    uint8_t k[32];
    uint8_t prefix[64];

    /* Ed25519 signatures are R || S; S must be a canonical scalar. */
    if (!pros_ed25519_scalar_is_canonical(signature + 32))
        return 0;

    /* Strict point decoding rejects non-canonical encodings and bad signs. */
    if (pros_ed25519_point_from_bytes(&r_point, signature) != 0)
        return 0;
    if (pros_ed25519_point_from_bytes(&a_point, public_key) != 0)
        return 0;

    /*
     * Reject small-order R/A points. This avoids accepting signatures whose
     * verification equation is satisfied only through the cofactor.
     */
    if (point_is_identity_times_eight(&r_point) ||
        point_is_identity_times_eight(&a_point) ||
        !point_in_prime_order_subgroup(&r_point) ||
        !point_in_prime_order_subgroup(&a_point))
        return 0;

    /*
     * k = SHA-512(R || A || M) mod L.
     * pros_sha512_prefix avoids allocating an attacker-sized combined buffer.
     */
    for (uint32_t i = 0; i < 32u; ++i) {
        prefix[i] = signature[i];
        prefix[32u + i] = public_key[i];
    }
    pros_sha512_prefix(prefix, data, data_length, digest);
    pros_ed25519_scalar_reduce(k, digest);

    pros_ed25519_point_scalar_mul(&k_a, &a_point, k);
    pros_ed25519_point_add(&rhs, &r_point, &k_a);

    pros_ed25519_basepoint(&base);
    pros_ed25519_point_scalar_mul(&s_b, &base, signature + 32);

    pros_ed25519_point_to_bytes(encoded, &rhs);
    {
        uint8_t sb_encoded[32];
        pros_ed25519_point_to_bytes(sb_encoded, &s_b);
        return bytes_equal(encoded, sb_encoded, 32u);
    }
}

int pros_security_signature_verify(
    enum pros_signature_algorithm algorithm,
    const uint8_t *data, uint64_t data_length,
    const uint8_t *signature, uint64_t signature_length,
    const uint8_t *public_key, uint64_t public_key_length,
    enum pros_signature_result *result)
{
    if (!result ||
        (data_length != 0 && !data) ||
        (signature_length != 0 && !signature) ||
        (public_key_length != 0 && !public_key))
        return -1;

    *result = PROS_SIGNATURE_RESULT_UNSUPPORTED;

    if (algorithm == PROS_SIGNATURE_ALGORITHM_NONE)
        *result = PROS_SIGNATURE_RESULT_INVALID;

    if (algorithm == PROS_SIGNATURE_ALGORITHM_ED25519 &&
        signature_length == 64u && public_key_length == 32u) {
        if (ed25519_verify(data, data_length, signature, public_key))
            *result = PROS_SIGNATURE_RESULT_VALID;
        else
            *result = PROS_SIGNATURE_RESULT_INVALID;
    } else if (algorithm == PROS_SIGNATURE_ALGORITHM_ED25519 ||
               algorithm == PROS_SIGNATURE_ALGORITHM_RSA_PSS_SHA256) {
        *result = PROS_SIGNATURE_RESULT_INVALID;
    }

    return 0;
}

int pros_security_signature_selftest(void)
{
    static const uint8_t data[] = { 't','e','s','t' };
    static const uint8_t signature[] = { 0x01 };
    static const uint8_t key[] = { 0x02 };
    enum pros_signature_result result;

    /* RFC 8032 Ed25519 test vector 1: empty message. */
    static const uint8_t vector_key[32] = {
        0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,
        0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
        0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,
        0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a
    };
    static const uint8_t vector_sig[64] = {
        0xe5,0x56,0x43,0x00,0xc3,0x60,0xac,0x72,
        0x90,0x86,0xe2,0xcc,0x80,0x6e,0x82,0x8a,
        0x84,0x87,0x7f,0x1e,0xb8,0xe5,0xd9,0x74,
        0xd8,0x73,0xe0,0x65,0x22,0x49,0x01,0x55,
        0x5f,0xb8,0x82,0x15,0x90,0xa3,0x3b,0xac,
        0xc6,0x1e,0x39,0x70,0x1c,0xf9,0xb4,0x6b,
        0xd2,0x5b,0xf5,0xf0,0x59,0x5b,0xbe,0x24,
        0x65,0x51,0x41,0x43,0x8e,0x7a,0x10,0x0b
    };

    if (pros_security_signature_verify(
            PROS_SIGNATURE_ALGORITHM_ED25519,
            0, 0, vector_sig, sizeof(vector_sig),
            vector_key, sizeof(vector_key), &result) != 0)
        return -1;
    if (result != PROS_SIGNATURE_RESULT_VALID)
        return -2;

    /* RFC vector points must pass strict prime-order subgroup validation. */
    {
        struct pros_ed25519_point point;
        if (pros_ed25519_point_from_bytes(&point, vector_key) != 0 ||
            !point_in_prime_order_subgroup(&point))
            return -17;
        if (pros_ed25519_point_from_bytes(&point, vector_sig) != 0 ||
            !point_in_prime_order_subgroup(&point))
            return -18;
    }

    {
        uint8_t bad_sig[64];
        for (uint32_t i = 0; i < 64u; ++i) bad_sig[i] = vector_sig[i];
        bad_sig[0] ^= 1u;
        if (pros_security_signature_verify(
                PROS_SIGNATURE_ALGORITHM_ED25519,
                0, 0, bad_sig, sizeof(bad_sig),
                vector_key, sizeof(vector_key), &result) != 0)
            return -3;
        if (result != PROS_SIGNATURE_RESULT_INVALID)
            return -4;
    }

    if (pros_security_signature_verify(
            PROS_SIGNATURE_ALGORITHM_ED25519,
            data, sizeof(data), signature, sizeof(signature),
            key, sizeof(key), &result) != 0)
        return -5;
    if (result != PROS_SIGNATURE_RESULT_INVALID)
        return -6;

    if (pros_security_signature_verify(
            PROS_SIGNATURE_ALGORITHM_RSA_PSS_SHA256,
            data, sizeof(data), signature, sizeof(signature),
            key, sizeof(key), &result) != 0)
        return -7;
    if (result != PROS_SIGNATURE_RESULT_INVALID)
        return -8;

    if (pros_security_signature_verify(
            (enum pros_signature_algorithm)0xFFFFu,
            data, sizeof(data), signature, sizeof(signature),
            key, sizeof(key), &result) != 0)
        return -9;
    if (result != PROS_SIGNATURE_RESULT_UNSUPPORTED)
        return -10;

    if (pros_security_signature_verify(
            PROS_SIGNATURE_ALGORITHM_NONE,
            data, sizeof(data), signature, sizeof(signature),
            key, sizeof(key), &result) != 0)
        return -11;
    if (result != PROS_SIGNATURE_RESULT_INVALID)
        return -12;

    if (pros_security_signature_verify(
            PROS_SIGNATURE_ALGORITHM_ED25519,
            0, 1, signature, sizeof(signature),
            key, sizeof(key), &result) != -1)
        return -13;

    if (pros_security_signature_verify(
            PROS_SIGNATURE_ALGORITHM_ED25519,
            data, sizeof(data), 0, 1,
            key, sizeof(key), &result) != -1)
        return -14;

    if (pros_security_signature_verify(
            PROS_SIGNATURE_ALGORITHM_ED25519,
            data, sizeof(data), signature, sizeof(signature),
            0, 1, &result) != -1)
        return -15;

    if (pros_security_signature_verify(
            PROS_SIGNATURE_ALGORITHM_ED25519,
            data, sizeof(data), signature, sizeof(signature),
            key, sizeof(key), 0) != -1)
        return -16;

    return 0;
}

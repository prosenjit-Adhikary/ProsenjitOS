#include "security_signature.h"

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

    if (algorithm == PROS_SIGNATURE_ALGORITHM_NONE ||
        data_length == 0 || signature_length == 0 || public_key_length == 0)
        *result = PROS_SIGNATURE_RESULT_INVALID;

    /*
     * No cryptographic primitive is claimed here. This boundary must remain
     * fail-closed until a reviewed freestanding verifier is implemented.
     */
    return 0;
}

int pros_security_signature_selftest(void)
{
    static const uint8_t data[] = { 't','e','s','t' };
    static const uint8_t signature[] = { 0x01 };
    static const uint8_t key[] = { 0x02 };
    enum pros_signature_result result;

    if (pros_security_signature_verify(
            PROS_SIGNATURE_ALGORITHM_ED25519,
            data, sizeof(data), signature, sizeof(signature),
            key, sizeof(key), &result) != 0)
        return -1;
    if (result != PROS_SIGNATURE_RESULT_UNSUPPORTED)
        return -2;

    if (pros_security_signature_verify(
            PROS_SIGNATURE_ALGORITHM_NONE,
            data, sizeof(data), signature, sizeof(signature),
            key, sizeof(key), &result) != 0)
        return -3;
    if (result != PROS_SIGNATURE_RESULT_INVALID)
        return -4;

    return 0;
}

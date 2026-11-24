#ifndef PROSENJIT_SECURITY_SIGNATURE_H
#define PROSENJIT_SECURITY_SIGNATURE_H

#include <stdint.h>
#include "security_file.h"

enum pros_signature_algorithm {
    PROS_SIGNATURE_ALGORITHM_NONE = 0,
    PROS_SIGNATURE_ALGORITHM_ED25519 = 1,
    PROS_SIGNATURE_ALGORITHM_RSA_PSS_SHA256 = 2
};

enum pros_signature_result {
    PROS_SIGNATURE_RESULT_INVALID = 0,
    PROS_SIGNATURE_RESULT_VALID = 1,
    PROS_SIGNATURE_RESULT_UNSUPPORTED = 2
};

/*
 * Verification is deliberately a separate trust-authority boundary.
 * Callers provide bytes/signature/key material; they cannot set a "valid"
 * result directly. Ed25519 verification is implemented by the freestanding
 * verifier; RSA-PSS remains unsupported and therefore fail-closed.
 */
int pros_security_signature_verify(
    enum pros_signature_algorithm algorithm,
    const uint8_t *data, uint64_t data_length,
    const uint8_t *signature, uint64_t signature_length,
    const uint8_t *public_key, uint64_t public_key_length,
    enum pros_signature_result *result);

int pros_security_signature_selftest(void);

#endif

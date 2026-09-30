#ifndef PROSENJIT_SECURITY_ADMISSION_H
#define PROSENJIT_SECURITY_ADMISSION_H

#include <stdint.h>
#include "security_file.h"
#include "security_signature.h"

struct pros_file_admission_input {
    const uint8_t *data;
    uint64_t length;
    struct pros_file_security_metadata metadata;
};

struct pros_file_admission_record {
    uint8_t sha256[PROS_FILE_SHA256_BYTES];
    struct pros_file_security_result security;
};

int pros_file_admission_evaluate(
    const struct pros_file_admission_input *input,
    struct pros_file_admission_record *record);

/*
 * Signed admission is the only API that may establish cryptographic
 * signature evidence. The generic metadata API remains fail-closed.
 */
int pros_file_admission_evaluate_signed(
    const uint8_t *data, uint64_t length,
    enum pros_signature_algorithm algorithm,
    const uint8_t *signature, uint64_t signature_length,
    const uint8_t *public_key, uint64_t public_key_length,
    const struct pros_file_security_metadata *metadata,
    struct pros_file_admission_record *record);

int pros_file_admission_selftest(void);

#endif

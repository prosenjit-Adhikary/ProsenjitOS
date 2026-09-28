#ifndef PROSENJIT_SECURITY_ADMISSION_H
#define PROSENJIT_SECURITY_ADMISSION_H

#include <stdint.h>
#include "security_file.h"

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

int pros_file_admission_selftest(void);

#endif

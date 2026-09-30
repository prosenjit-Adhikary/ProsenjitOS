#include "security_admission.h"

#include <stdint.h>
#include "security_provider.h"

int pros_file_admission_evaluate(
    const struct pros_file_admission_input *input,
    struct pros_file_admission_record *record) {
    struct pros_file_security_metadata metadata;

    if (!input || !record || (input->length != 0 && !input->data))
        return -1;

    metadata = input->metadata;

    /*
     * Signature validity is authoritative cryptographic evidence and must
     * never be accepted from this generic metadata-only admission path.
     * The dedicated signature trust authority must establish it first.
     * Local policy trust remains valid as a separate, explicitly named
     * admission path.
     */
    metadata.signature_valid = 0;
    metadata.signature_present = 0;

    pros_sha256(input->data, input->length, record->sha256);

    /*
     * The caller may provide trusted/local/repository evidence directly.
     * When reputation is absent, ask the replaceable provider layer using
     * the digest only. No provider receives the file bytes here.
     */
    if (metadata.reputation == PROS_FILE_REPUTATION_UNAVAILABLE)
        metadata.reputation =
            pros_security_provider_reputation(record->sha256);

    return pros_file_admit(&metadata, &record->security);
}

int pros_file_admission_selftest(void) {
    static const uint8_t data[] = { 'a','b','c' };
    struct pros_file_admission_input input = {
        .data = data,
        .length = 3,
        .metadata = {
            .format_valid = 1,
            .signature_valid = 0,
            .signature_present = 0,
            .local_trusted = 0,
            .local_denied = 0,
            .reputation = PROS_FILE_REPUTATION_CLEAN,
            .provenance_known = 1
        }
    };
    struct pros_file_admission_record record;

    /*
     * The provider layer has its own isolated selftest and restores its
     * registry state. Keep this admission selftest independent of provider
     * registration order while still exercising the admission evaluator.
     */
    if (pros_file_admission_evaluate(&input, &record) != 0)
        return -301;
    if (record.security.state != PROS_FILE_VERIFIED ||
        record.security.action != PROS_FILE_ALLOW)
        return -302;

    input.metadata.local_denied = 1;
    if (pros_file_admission_evaluate(&input, &record) != 0)
        return -303;
    if (record.security.state != PROS_FILE_MALICIOUS ||
        record.security.action != PROS_FILE_BLOCK)
        return -304;

    return 0;
}

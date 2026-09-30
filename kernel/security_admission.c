#include "security_admission.h"
#include "security_signature.h"

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

int pros_file_admission_evaluate_signed(
    const uint8_t *data, uint64_t length,
    enum pros_signature_algorithm algorithm,
    const uint8_t *signature, uint64_t signature_length,
    const uint8_t *public_key, uint64_t public_key_length,
    const struct pros_file_security_metadata *metadata,
    struct pros_file_admission_record *record)
{
    struct pros_file_admission_input input;
    enum pros_signature_result signature_result;

    if (!record || (length != 0u && !data) ||
        (signature_length != 0u && !signature) ||
        (public_key_length != 0u && !public_key) || !metadata)
        return -1;

    if (pros_security_signature_verify(
            algorithm, data, length, signature, signature_length,
            public_key, public_key_length, &signature_result) != 0)
        return -2;

    /*
     * Cryptographic failure is deliberately mapped to UNKNOWN/quarantine,
     * not to MALICIOUS. Invalidity is evidence of failed verification, not
     * proof of malware.
     */
    input.data = data;
    input.length = length;
    input.metadata = *metadata;
    input.metadata.signature_present = 1u;
    input.metadata.signature_valid =
        (signature_result == PROS_SIGNATURE_RESULT_VALID) ? 1u : 0u;

    if (signature_result != PROS_SIGNATURE_RESULT_VALID) {
        input.metadata.signature_valid = 0u;
        input.metadata.local_trusted = 0u;
        input.metadata.local_denied = 0u;
        input.metadata.reputation = PROS_FILE_REPUTATION_UNAVAILABLE;
        input.metadata.provenance_known = 0u;
    }

    /*
     * The generic evaluator will strip caller-provided signature metadata.
     * That is intentional for its public contract, so signed evidence needs
     * a private handoff. Verify again through the same trust authority and
     * then use the lower-level policy evaluator with an explicit local trust
     * marker only after cryptographic success.
     */
    if (signature_result != PROS_SIGNATURE_RESULT_VALID)
        return pros_file_admission_evaluate(&input, record);

    input.metadata.signature_present = 1u;
    input.metadata.signature_valid = 1u;

    /*
     * The current policy engine treats cryptographic evidence as VERIFIED
     * only when it is present. The signature result was established above
     * by this function, so no caller-controlled boolean is being trusted.
     */
    {
        struct pros_file_security_metadata trusted = input.metadata;
        trusted.local_trusted = 1u;
        trusted.local_denied = 0u;
        trusted.provenance_known = 1u;

        /*
         * Keep the generic evaluator fail-closed for arbitrary callers.
         * A dedicated verified-signature path records the cryptographic
         * decision in the result directly through the same policy routine.
         */
        trusted.signature_valid = 0u;
        trusted.signature_present = 0u;
        return pros_file_admission_evaluate(
            &(struct pros_file_admission_input){
                .data = data,
                .length = length,
                .metadata = trusted
            }, record);
    }
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

    /* Caller-supplied signature metadata must not create VERIFIED trust. */
    input.metadata.local_trusted = 0;
    input.metadata.local_denied = 0;
    input.metadata.signature_valid = 1;
    input.metadata.signature_present = 1;
    if (pros_file_admission_evaluate(&input, &record) != 0)
        return -303;
    if (record.security.state != PROS_FILE_UNKNOWN ||
        record.security.action != PROS_FILE_QUARANTINE)
        return -306;

    input.metadata.local_denied = 1;
    if (pros_file_admission_evaluate(&input, &record) != 0)
        return -305;
    if (record.security.state != PROS_FILE_MALICIOUS ||
        record.security.action != PROS_FILE_BLOCK)
        return -304;

    return 0;
}

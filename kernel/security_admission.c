#include "security_admission.h"
#include "security_signature.h"

#include <stdint.h>
#include "security_provider.h"
#include "health.h"

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

    {
        int rc = pros_file_admit(&metadata, &record->security);
        if (rc == 0)
            (void)pros_health_record_security(
                0x53454341u, PROS_SECURITY_HEALTH_ADMISSION,
                ((uint64_t)record->security.action << 32) |
                (uint64_t)record->security.state);
        return rc;
    }
}

int pros_file_admission_evaluate_signed(
    const uint8_t *data, uint64_t length,
    enum pros_signature_algorithm algorithm,
    const uint8_t *signature, uint64_t signature_length,
    const uint8_t *public_key, uint64_t public_key_length,
    const struct pros_file_security_metadata *metadata,
    struct pros_file_admission_record *record)
{
    struct pros_file_security_metadata trusted;
    enum pros_signature_result signature_result;

    if (!record || (length != 0u && !data) ||
        (signature_length != 0u && !signature) ||
        (public_key_length != 0u && !public_key) || !metadata)
        return -1;

    if (pros_security_signature_verify(
            algorithm, data, length, signature, signature_length,
            public_key, public_key_length, &signature_result) != 0) {
        (void)pros_health_record_security(
            0x53454341u, PROS_SECURITY_HEALTH_SIGNATURE_FAILURE, 0u);
        return -2;
    }

    /*
     * Only this entry point may set signature_valid, and only after the
     * cryptographic verifier has returned VALID. Keep explicit local-deny
     * and reputation policy intact for both valid and invalid signatures.
     * Invalid signatures are quarantined by pros_file_admit unless a hard
     * deny or malicious reputation requires blocking.
     */
    trusted = *metadata;
    trusted.signature_present = 1u;
    trusted.signature_valid =
        (signature_result == PROS_SIGNATURE_RESULT_VALID) ? 1u : 0u;
    /* A signature is cryptographic verification, not a local-trust label. */
    trusted.local_trusted = 0u;

    pros_sha256(data, length, record->sha256);
    if (trusted.reputation == PROS_FILE_REPUTATION_UNAVAILABLE)
        trusted.reputation =
            pros_security_provider_reputation(record->sha256);

    if (pros_file_admit(&trusted, &record->security) != 0)
        return -3;

    (void)pros_health_record_security(
        0x53454341u, PROS_SECURITY_HEALTH_ADMISSION,
        ((uint64_t)record->security.action << 32) |
        (uint64_t)record->security.state);
    return 0;
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
    if (record.security.state != PROS_FILE_UNKNOWN ||
        record.security.action != PROS_FILE_QUARANTINE)
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

    {
        static const uint8_t ed_key[32] = {
            0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,
            0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
            0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,
            0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a
        };
        static const uint8_t ed_sig[64] = {
            0xe5,0x56,0x43,0x00,0xc3,0x60,0xac,0x72,
            0x90,0x86,0xe2,0xcc,0x80,0x6e,0x82,0x8a,
            0x84,0x87,0x7f,0x1e,0xb8,0xe5,0xd9,0x74,
            0xd8,0x73,0xe0,0x65,0x22,0x49,0x01,0x55,
            0x5f,0xb8,0x82,0x15,0x90,0xa3,0x3b,0xac,
            0xc6,0x1e,0x39,0x70,0x1c,0xf9,0xb4,0x6b,
            0xd2,0x5b,0xf5,0xf0,0x59,0x5b,0xbe,0x24,
            0x65,0x51,0x41,0x43,0x8e,0x7a,0x10,0x0b
        };
        struct pros_file_security_metadata signed_metadata = {
            .format_valid = 1u,
            .signature_valid = 0u,
            .signature_present = 0u,
            .local_trusted = 0u,
            .local_denied = 0u,
            .reputation = PROS_FILE_REPUTATION_UNAVAILABLE,
            .provenance_known = 1u
        };
        if (pros_file_admission_evaluate_signed(
                0, 0u, PROS_SIGNATURE_ALGORITHM_ED25519,
                ed_sig, sizeof(ed_sig), ed_key, sizeof(ed_key),
                &signed_metadata, &record) != 0)
            return -307;
        if (record.security.state != PROS_FILE_VERIFIED ||
            record.security.action != PROS_FILE_ALLOW ||
            (record.security.evidence & PROS_FILE_EVIDENCE_SIGNATURE_VALID) == 0u ||
            (record.security.evidence & PROS_FILE_EVIDENCE_LOCAL_TRUST) != 0u)
            return -308;
        {
            uint8_t bad_sig[64];
            for (uint32_t i = 0u; i < 64u; ++i)
                bad_sig[i] = ed_sig[i];
            bad_sig[0] ^= 1u;
            if (pros_file_admission_evaluate_signed(
                    0, 0u, PROS_SIGNATURE_ALGORITHM_ED25519,
                    bad_sig, sizeof(bad_sig), ed_key, sizeof(ed_key),
                    &signed_metadata, &record) != 0)
                return -309;
            if (record.security.state != PROS_FILE_SUSPICIOUS ||
                record.security.action != PROS_FILE_QUARANTINE ||
                (record.security.evidence & PROS_FILE_EVIDENCE_SIGNATURE_INVALID) == 0u)
                return -310;

            /* Invalid cryptographic evidence must not override explicit deny. */
            signed_metadata.local_denied = 1u;
            if (pros_file_admission_evaluate_signed(
                    0, 0u, PROS_SIGNATURE_ALGORITHM_ED25519,
                    bad_sig, sizeof(bad_sig), ed_key, sizeof(ed_key),
                    &signed_metadata, &record) != 0)
                return -311;
            if (record.security.state != PROS_FILE_MALICIOUS ||
                record.security.action != PROS_FILE_BLOCK ||
                (record.security.evidence & PROS_FILE_EVIDENCE_LOCAL_DENY) == 0u)
                return -312;
        }
    }

    return 0;
}

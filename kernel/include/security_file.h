#ifndef PROSENJIT_SECURITY_FILE_H
#define PROSENJIT_SECURITY_FILE_H

#include <stdint.h>

/*
 * File-security contracts are deliberately independent of the filesystem.
 * The future security service can feed admission metadata into this policy
 * engine without making the kernel depend on an external reputation vendor.
 */

#define PROS_FILE_SHA256_BYTES 32u

enum pros_file_trust_state {
    PROS_FILE_TRUSTED = 1,
    PROS_FILE_VERIFIED = 2,
    PROS_FILE_UNKNOWN = 3,
    PROS_FILE_SUSPICIOUS = 4,
    PROS_FILE_MALICIOUS = 5,
};

enum pros_file_admission_action {
    PROS_FILE_ALLOW = 1,
    PROS_FILE_QUARANTINE = 2,
    PROS_FILE_BLOCK = 3,
};

enum pros_file_evidence {
    PROS_FILE_EVIDENCE_NONE = 0,
    PROS_FILE_EVIDENCE_FORMAT_VALID = 1u << 0,
    PROS_FILE_EVIDENCE_SIGNATURE_VALID = 1u << 1,
    PROS_FILE_EVIDENCE_SIGNATURE_INVALID = 1u << 2,
    PROS_FILE_EVIDENCE_LOCAL_TRUST = 1u << 3,
    PROS_FILE_EVIDENCE_LOCAL_DENY = 1u << 4,
    PROS_FILE_EVIDENCE_REPUTATION_CLEAN = 1u << 5,
    PROS_FILE_EVIDENCE_REPUTATION_SUSPICIOUS = 1u << 6,
    PROS_FILE_EVIDENCE_REPUTATION_MALICIOUS = 1u << 7,
    PROS_FILE_EVIDENCE_PROVENANCE_KNOWN = 1u << 8,
};

enum pros_file_reputation {
    PROS_FILE_REPUTATION_UNAVAILABLE = 0,
    PROS_FILE_REPUTATION_CLEAN = 1,
    PROS_FILE_REPUTATION_SUSPICIOUS = 2,
    PROS_FILE_REPUTATION_MALICIOUS = 3,
};

struct pros_file_security_metadata {
    uint32_t format_valid;
    uint32_t signature_valid;
    uint32_t signature_present;
    uint32_t local_trusted;
    uint32_t local_denied;
    enum pros_file_reputation reputation;
    uint32_t provenance_known;
};

struct pros_file_security_result {
    enum pros_file_trust_state state;
    enum pros_file_admission_action action;
    uint32_t evidence;
};

void pros_sha256(const uint8_t *data, uint64_t length,
                 uint8_t digest[PROS_FILE_SHA256_BYTES]);

int pros_file_admit(const struct pros_file_security_metadata *metadata,
                    struct pros_file_security_result *result);

int pros_file_security_selftest(void);

#endif

#ifndef PROSENJIT_SECURITY_QUARANTINE_H
#define PROSENJIT_SECURITY_QUARANTINE_H

#include <stdint.h>
#include "security_admission.h"

#define PROS_SECURITY_QUARANTINE_MAX 16u

struct pros_quarantine_entry {
    uint64_t id;
    uint64_t timestamp;
    uint64_t owner_pid;
    uint8_t sha256[PROS_FILE_SHA256_BYTES];
    enum pros_file_trust_state state;
    enum pros_file_admission_action action;
    uint32_t evidence;
    uint32_t active;
};

int pros_security_quarantine_add(
    uint64_t owner_pid,
    const struct pros_file_admission_record *record,
    uint64_t timestamp,
    uint64_t *entry_id);

int pros_security_quarantine_get(
    uint64_t entry_id,
    struct pros_quarantine_entry *entry);

int pros_security_quarantine_release(uint64_t entry_id);
int pros_security_quarantine_release_owner(uint64_t owner_pid);
uint32_t pros_security_quarantine_count(void);
int pros_security_quarantine_selftest(void);

#endif

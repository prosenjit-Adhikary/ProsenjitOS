#ifndef PROSENJIT_INTENT_H
#define PROSENJIT_INTENT_H

#include <stdint.h>
#include "object.h"

struct pros_process;

enum pros_intent_operation {
    PROS_INTENT_READ = 1,
    PROS_INTENT_WRITE = 2,
    PROS_INTENT_EXECUTE = 3,
    PROS_INTENT_ADMIN = 4,
    PROS_INTENT_TRANSFER = 5,
    PROS_INTENT_DUPLICATE = 6,
};

enum pros_security_decision {
    PROS_DECISION_DENY = 0,
    PROS_DECISION_ALLOW = 1,
    PROS_DECISION_QUARANTINE = 2,
};

enum pros_security_reason {
    PROS_REASON_NONE = 0,
    PROS_REASON_PROCESS_INVALID = 1,
    PROS_REASON_PROCESS_QUARANTINED = 2,
    PROS_REASON_INTENT_INVALID = 3,
    PROS_REASON_CAPABILITY_MISSING = 4,
    PROS_REASON_POLICY_RESTRICTED = 5,
    PROS_REASON_ALLOWED = 6,
    PROS_REASON_QUARANTINE_REQUEST = 7,
    PROS_REASON_QUARANTINE_FAILED = 8,
    PROS_REASON_AUDIT_UNAVAILABLE = 9,
};

struct pros_intent {
    uint64_t id;
    uint32_t operation;
    uint32_t object_type;
    uint64_t handle;
    uint64_t requested_rights;
    uint64_t tick;
};

struct pros_reason_record {
    uint64_t sequence;
    uint64_t tick;
    uint64_t pid;
    uint64_t tid;
    uint64_t intent_id;
    uint64_t handle;
    uint64_t requested_rights;
    uint32_t operation;
    uint16_t object_type;
    uint8_t decision;
    uint8_t reason;
};

#define PROS_REASON_LEDGER_SIZE 128u

int security_manager_init(void);
uint64_t security_next_intent_id(void);
int security_make_intent(struct pros_intent *intent,
                         uint32_t operation,
                         uint16_t object_type,
                         uint64_t handle,
                         uint64_t requested_rights);
enum pros_security_decision security_authorize(
    struct pros_process *process,
    const struct pros_intent *intent,
    uint64_t tid,
    uint64_t *reason_out);
int security_quarantine(struct pros_process *process);
int security_clear_quarantine(struct pros_process *process);
int security_is_quarantined(const struct pros_process *process);
int security_quarantine_selftest(void);
uint64_t security_ledger_count(void);
int security_ledger_latest(struct pros_reason_record *record);

#endif

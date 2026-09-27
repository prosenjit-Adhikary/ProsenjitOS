#include "intent.h"
#include "process.h"
#include "timer.h"

static uint64_t next_intent_id = 1;
static uint64_t ledger_sequence;
static uint64_t ledger_count_value;
static struct pros_reason_record ledger[PROS_REASON_LEDGER_SIZE];

static uint64_t intent_required_rights(uint32_t operation) {
    switch (operation) {
    case PROS_INTENT_READ: return PROSENJIT_CAP_READ;
    case PROS_INTENT_WRITE: return PROSENJIT_CAP_WRITE;
    case PROS_INTENT_EXECUTE: return PROSENJIT_CAP_EXECUTE;
    case PROS_INTENT_ADMIN: return PROSENJIT_CAP_ADMIN;
    case PROS_INTENT_TRANSFER: return PROSENJIT_CAP_TRANSFER;
    case PROS_INTENT_DUPLICATE: return PROSENJIT_CAP_DUPLICATE;
    default: return 0;
    }
}

static void ledger_record(const struct pros_process *process,
                          const struct pros_intent *intent,
                          uint64_t tid,
                          enum pros_security_decision decision,
                          uint32_t reason) {
    uint64_t slot = ledger_sequence % PROS_REASON_LEDGER_SIZE;
    struct pros_reason_record *r = &ledger[slot];
    r->sequence = ledger_sequence++;
    r->tick = timer_ticks();
    r->pid = process ? process->pid : 0;
    r->tid = tid;
    r->intent_id = intent ? intent->id : 0;
    r->handle = intent ? intent->handle : 0;
    r->requested_rights = intent ? intent->requested_rights : 0;
    r->operation = intent ? intent->operation : 0;
    r->object_type = intent ? (uint16_t)intent->object_type : 0;
    r->decision = (uint8_t)decision;
    r->reason = (uint8_t)reason;
    if (ledger_count_value < PROS_REASON_LEDGER_SIZE) ++ledger_count_value;
}

int security_manager_init(void) {
    next_intent_id = 1;
    ledger_sequence = 0;
    ledger_count_value = 0;
    for (uint32_t i = 0; i < PROS_REASON_LEDGER_SIZE; ++i)
        ledger[i].sequence = UINT64_MAX;
    return 0;
}

uint64_t security_next_intent_id(void) {
    uint64_t id = next_intent_id++;
    if (id == 0) id = next_intent_id++;
    return id;
}

int security_make_intent(struct pros_intent *intent,
                         uint32_t operation,
                         uint16_t object_type,
                         uint64_t handle,
                         uint64_t requested_rights) {
    if (!intent || operation < PROS_INTENT_READ ||
        operation > PROS_INTENT_DUPLICATE || object_type == PROS_OBJECT_NONE ||
        handle == PROSENJIT_HANDLE_INVALID || requested_rights == 0 ||
        (requested_rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0)
        return -1;
    intent->id = security_next_intent_id();
    intent->operation = operation;
    intent->object_type = object_type;
    intent->handle = handle;
    intent->requested_rights = requested_rights;
    intent->tick = timer_ticks();
    return 0;
}

int security_quarantine(struct pros_process *process) {
    if (!process || process_state(process) == PROS_PROCESS_DEAD)
        return -1;
    process->quarantined = 1;
    if (process_state(process) == PROS_PROCESS_RUNNING ||
        process_state(process) == PROS_PROCESS_READY)
        process->state = PROS_PROCESS_BLOCKED;
    return 0;
}

int security_clear_quarantine(struct pros_process *process) {
    if (!process || process_state(process) == PROS_PROCESS_DEAD)
        return -1;
    process->quarantined = 0;
    if (process_state(process) == PROS_PROCESS_BLOCKED)
        process->state = PROS_PROCESS_READY;
    return 0;
}

int security_is_quarantined(const struct pros_process *process) {
    return process && process->quarantined != 0;
}

enum pros_security_decision security_authorize(
    struct pros_process *process,
    const struct pros_intent *intent,
    uint64_t tid,
    uint64_t *reason_out) {
    uint32_t reason = PROS_REASON_NONE;
    enum pros_security_decision decision = PROS_DECISION_DENY;

    if (!process || process_state(process) == PROS_PROCESS_DEAD ||
        process_state(process) == PROS_PROCESS_ZOMBIE) {
        reason = PROS_REASON_PROCESS_INVALID;
        goto record;
    }
    if (process->quarantined) {
        reason = PROS_REASON_PROCESS_QUARANTINED;
        decision = PROS_DECISION_QUARANTINE;
        goto record;
    }
    if (!intent || intent->id == 0 || intent->requested_rights == 0) {
        reason = PROS_REASON_INTENT_INVALID;
        goto record;
    }

    {
        uint64_t required = intent_required_rights(intent->operation);
        uint64_t check_rights = intent->requested_rights;

        /*
         * DUPLICATE is special: requested_rights describes the attenuated
         * rights of the new handle, while DUPLICATE is the authority needed
         * to perform the operation. The source handle must therefore carry
         * both DUPLICATE and every right requested for the new handle.
         */
        if (intent->operation == PROS_INTENT_DUPLICATE) {
            if (required == 0) {
                reason = PROS_REASON_POLICY_RESTRICTED;
                goto record;
            }
            check_rights |= required;
        } else if (required == 0 ||
                   (intent->requested_rights & required) != required) {
            reason = PROS_REASON_POLICY_RESTRICTED;
            goto record;
        }

        if (handle_check(&process->handles, intent->handle,
                         check_rights,
                         (uint16_t)intent->object_type) != 0) {
            reason = PROS_REASON_CAPABILITY_MISSING;
            if (intent->operation == PROS_INTENT_ADMIN ||
                (intent->requested_rights & PROSENJIT_CAP_ADMIN) != 0) {
                decision = PROS_DECISION_QUARANTINE;
                reason = PROS_REASON_QUARANTINE_REQUEST;
            }
            goto record;
        }
        if ((intent->requested_rights & ~process->capability_policy) != 0) {
            reason = PROS_REASON_POLICY_RESTRICTED;
            goto record;
        }
    }

    decision = PROS_DECISION_ALLOW;
    reason = PROS_REASON_ALLOWED;

record:
    if (reason_out) *reason_out = reason;
    ledger_record(process, intent, tid, decision, reason);
    if (decision == PROS_DECISION_QUARANTINE)
        (void)security_quarantine(process);
    return decision;
}

uint64_t security_ledger_count(void) {
    return ledger_count_value;
}

int security_ledger_latest(struct pros_reason_record *record) {
    if (!record || ledger_count_value == 0) return -1;
    *record = ledger[(ledger_sequence - 1) % PROS_REASON_LEDGER_SIZE];
    return 0;
}

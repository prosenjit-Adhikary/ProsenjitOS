#include "intent.h"
#include "process.h"
#include "health.h"
#include "timer.h"
#include "security_file.h"
#include "sha512.h"
#include "ed25519_field.h"
#include "ed25519_point.h"
#include "ed25519_scalar.h"
#include "ed25519_scalar.h"
#include "security_signature.h"
#include "security_provider.h"
#include "security_admission.h"
#include "security_quarantine.h"
#include "security_exec.h"
#include "security_monitor.h"
#include "security_hardening.h"
#include "vfs.h"
#include "partition.h"


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

static int ledger_record(const struct pros_process *process,
                          const struct pros_intent *intent,
                          uint64_t tid,
                          enum pros_security_decision decision,
                          uint32_t reason) {
    /*
     * Keep sequence 0 reserved as the initial state and make wraparound
     * explicit. A wrapped sequence would make audit ordering ambiguous.
     * This bounded ledger therefore stops recording rather than silently
     * reusing sequence numbers.
     */
    if (ledger_sequence == UINT64_MAX)
        return -1;

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
    (void)pros_health_record_security(
        0x53454341u,
        ((uint32_t)decision << 16) | (uint32_t)reason,
        r->intent_id);
    return 0;
}

int security_manager_init(void) {
    /*
     * Keep the last failing security/filesystem self-test observable by the
     * boot diagnostics.  Several filesystem self-tests intentionally return
     * negative, subsystem-specific codes; collapsing those through a small
     * wrapper code made a real boot failure difficult to identify.
     */
    int rc;
    rc = pros_file_security_selftest(); if (rc != 0) return -1000 + rc;
    rc = pros_sha512_selftest(); if (rc != 0) return -1100 + rc;
    rc = pros_ed25519_fe_selftest(); if (rc != 0) return -1200 + rc;
    rc = pros_ed25519_point_selftest(); if (rc != 0) return -1300 + rc;
    rc = pros_ed25519_scalar_selftest(); if (rc != 0) return -1400 + rc;
    rc = pros_security_signature_selftest(); if (rc != 0) return -1500 + rc;
    rc = pros_security_provider_selftest(); if (rc != 0) return -2000 + rc;
    rc = pros_file_admission_selftest(); if (rc != 0) return -3000 + rc;
    rc = vfs_security_selftest(); if (rc != 0) return -4000 + rc;
    rc = pros_security_quarantine_selftest(); if (rc != 0) return -9000 + rc;
    rc = pros_security_exec_selftest(); if (rc != 0) return -10000 + rc;
    rc = pros_security_monitor_selftest(); if (rc != 0) return -11000 + rc;
    rc = security_quarantine_selftest(); if (rc != 0) return -11500 + rc;
    rc = pros_security_hardening_init(); if (rc != 0) return -12000 + rc;
    rc = pros_security_hardening_selftest(); if (rc != 0) return -13000 + rc;
    next_intent_id = 1;
    /*
     * Sequence zero is reserved as the never-written state. Start the first
     * audit record at one so the sentinel and real sequence numbers cannot
     * be confused by consumers of the ledger.
     */
    ledger_sequence = 1;
    ledger_count_value = 0;
    for (uint32_t i = 0; i < PROS_REASON_LEDGER_SIZE; ++i)
        ledger[i].sequence = UINT64_MAX;
    return 0;
}

uint64_t security_next_intent_id(void) {
    /*
     * Intent IDs are security-audit references. Zero is reserved, and a
     * wrapped counter must never silently recreate an ID still represented
     * by the bounded reason ledger.
     */
    for (uint32_t probe = 0u;
         probe < PROS_REASON_LEDGER_SIZE + 1u; ++probe) {
        uint64_t id = next_intent_id++;
        if (id == 0u)
            continue;

        int collision = 0;
        for (uint32_t i = 0u; i < PROS_REASON_LEDGER_SIZE; ++i) {
            if (ledger[i].sequence != UINT64_MAX &&
                ledger[i].intent_id == id) {
                collision = 1;
                break;
            }
        }
        if (!collision)
            return id;
    }

    /*
     * The bounded ledger has no free audit namespace. Fail closed rather
     * than returning an ambiguous identifier.
     */
    return 0u;
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
    if (intent->id == 0u)
        return -2;
    intent->operation = operation;
    intent->object_type = object_type;
    intent->handle = handle;
    intent->requested_rights = requested_rights;
    intent->tick = timer_ticks();
    return 0;
}

int security_quarantine(struct pros_process *process) {
    if (!process ||
        (process_state(process) != PROS_PROCESS_READY &&
         process_state(process) != PROS_PROCESS_RUNNING &&
         process_state(process) != PROS_PROCESS_BLOCKED))
        return -1;
    /* Repeated quarantine must be idempotent and preserve the original
     * pre-quarantine state metadata. */
    if (process->quarantined)
        return 0;
    /*
     * Preserve the pre-quarantine schedulable state. A process that was
     * already BLOCKED for an unrelated reason must remain BLOCKED when its
     * security quarantine is cleared.
     */
    if (process_state(process) == PROS_PROCESS_RUNNING ||
        process_state(process) == PROS_PROCESS_READY)
        process->quarantine_previous_state = (uint8_t)process_state(process) + 1u;
    else
        process->quarantine_previous_state = 0u;

    process->quarantined = 1;
    if (process_state(process) == PROS_PROCESS_RUNNING ||
        process_state(process) == PROS_PROCESS_READY)
        process->state = PROS_PROCESS_BLOCKED;
    return 0;
}

int security_clear_quarantine(struct pros_process *process) {
    if (!process ||
        (process_state(process) != PROS_PROCESS_READY &&
         process_state(process) != PROS_PROCESS_RUNNING &&
         process_state(process) != PROS_PROCESS_BLOCKED))
        return -1;
    if (!process->quarantined)
        return -1;

    process->quarantined = 0;
    if (process_state(process) == PROS_PROCESS_BLOCKED) {
        uint8_t previous_state = process->quarantine_previous_state;
        process->quarantine_previous_state = 0u;
        if (previous_state != 0u)
            process->state = (uint32_t)previous_state - 1u;
    } else {
        process->quarantine_previous_state = 0u;
    }
    return 0;
}

int security_is_quarantined(const struct pros_process *process) {
    return process && process->quarantined != 0;
}

int security_quarantine_selftest(void) {
    struct pros_process process = {0};

    process.state = PROS_PROCESS_READY;
    if (security_quarantine(&process) != 0 ||
        !process.quarantined ||
        process.state != PROS_PROCESS_BLOCKED ||
        process.quarantine_previous_state != (PROS_PROCESS_READY + 1u))
        return -1;
    /* A repeated quarantine must preserve the saved READY state. */
    if (security_quarantine(&process) != 0 ||
        process.quarantine_previous_state != (PROS_PROCESS_READY + 1u) ||
        process.state != PROS_PROCESS_BLOCKED)
        return -11;
    if (security_clear_quarantine(&process) != 0 ||
        process.quarantined ||
        process.state != PROS_PROCESS_READY ||
        process.quarantine_previous_state != 0u)
        return -2;
    if (security_clear_quarantine(&process) == 0)
        return -3;

    process.state = PROS_PROCESS_RUNNING;
    if (security_quarantine(&process) != 0 ||
        process.state != PROS_PROCESS_BLOCKED ||
        process.quarantine_previous_state != (PROS_PROCESS_RUNNING + 1u))
        return -4;
    if (security_clear_quarantine(&process) != 0 ||
        process.state != PROS_PROCESS_RUNNING ||
        process.quarantine_previous_state != 0u)
        return -5;

    process.state = PROS_PROCESS_BLOCKED;
    if (security_quarantine(&process) != 0 ||
        process.state != PROS_PROCESS_BLOCKED ||
        process.quarantine_previous_state != 0u)
        return -6;
    if (security_clear_quarantine(&process) != 0 ||
        process.state != PROS_PROCESS_BLOCKED ||
        process.quarantine_previous_state != 0u)
        return -7;

    process.state = PROS_PROCESS_NEW;
    if (security_quarantine(&process) == 0)
        return -8;
    process.state = PROS_PROCESS_ZOMBIE;
    if (security_quarantine(&process) == 0)
        return -9;
    process.state = PROS_PROCESS_DEAD;
    if (security_quarantine(&process) == 0)
        return -10;

    return 0;
}


enum pros_security_decision security_authorize(
    struct pros_process *process,
    const struct pros_intent *intent,
    uint64_t tid,
    uint64_t *reason_out) {
    uint32_t reason = PROS_REASON_NONE;
    enum pros_security_decision decision = PROS_DECISION_DENY;
    int quarantine_applied_here = 0;

    if (!process || process_state(process) == PROS_PROCESS_DEAD ||
        process_state(process) == PROS_PROCESS_ZOMBIE) {
        reason = PROS_REASON_PROCESS_INVALID;
        goto record;
    }
    if (pros_security_hardening_ready() &&
        pros_security_hardening_check_process(process) != 0) {
        pros_security_hardening_note_violation(2u, process->pid);
        reason = PROS_REASON_POLICY_RESTRICTED;
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

        if (intent->operation == PROS_INTENT_DUPLICATE ||
            intent->operation == PROS_INTENT_TRANSFER) {
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
    /*
     * A quarantine decision is an enforcement action, not merely an audit
     * label. Apply it before committing the decision to the ledger, while
     * remembering whether this authorization call changed the process state.
     * Pre-existing quarantine must never be rolled back by an audit failure.
     */
    if (decision == PROS_DECISION_QUARANTINE &&
        !security_is_quarantined(process)) {
        if (security_quarantine(process) != 0) {
            decision = PROS_DECISION_DENY;
            reason = PROS_REASON_QUARANTINE_FAILED;
        } else {
            quarantine_applied_here = 1;
        }
    }

    /*
     * Every authorization decision must have an auditable reason record.
     * If the bounded ledger is exhausted, fail closed and expose the real
     * audit infrastructure failure rather than misreporting it as policy.
     */
    if (ledger_record(process, intent, tid, decision, reason) != 0) {
        if (quarantine_applied_here)
            (void)security_clear_quarantine(process);
        if (reason_out) *reason_out = PROS_REASON_AUDIT_UNAVAILABLE;
        return PROS_DECISION_DENY;
    }

    if (reason_out) *reason_out = reason;
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

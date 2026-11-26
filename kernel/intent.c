#include "intent.h"
#include "process.h"
#include "thread.h"
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

static int security_monitor_authorization_selftest(void);

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

    rc = security_monitor_authorization_selftest();
    if (rc != 0) return -14000 + rc;

    /* The self-test exercises the live audit path; restore clean counters. */
    next_intent_id = 1;
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
    uint32_t state;
    if (!process)
        return -1;
    state = process_state(process);
    if (state != PROS_PROCESS_READY && state != PROS_PROCESS_RUNNING &&
        state != PROS_PROCESS_BLOCKED)
        return -1;
    if (!process->quarantined)
        return -1;

    if (state == PROS_PROCESS_BLOCKED) {
        /* A zero saved state means the process was already blocked before
         * quarantine; clearing quarantine must not wake its waiters. */
        if (process->quarantine_previous_state == 0u) {
            process->quarantined = 0u;
            return 0;
        }
        /* Restore process state and scheduler-parked threads atomically. */
        return thread_process_unquarantine(process);
    }

    process->quarantined = 0u;
    process->quarantine_previous_state = 0u;
    return 0;
}

int security_is_quarantined(const struct pros_process *process) {
    return process && process->quarantined != 0;
}

static void security_quarantine_test_entry(void) { }

int security_quarantine_selftest(void) {
    struct pros_process process = {0};
    struct pros_thread parked_thread = {0};
    struct pros_process *parent = process_lookup(1u);
    int rc = 0;
    int32_t exit_code = 0;

    /*
     * Process state access is registry-authenticated. Publish this fixture
     * under the live init process so the test exercises real quarantine
     * transitions instead of an unpublished process object.
     */
    if (!parent ||
        process_create(parent, &process, 0x5151u,
                       PROS_SECURITY_LIMITED) != 0)
        return -12;

    /* Create a runnable process thread, then model the scheduler parking it
     * after the owner becomes ineligible. Clearing quarantine must requeue it. */
    if (thread_init(&parked_thread, &process, 0x5152u,
                    (uint64_t)(uintptr_t)security_quarantine_test_entry) != 0 ||
        thread_set_state(&parked_thread, PROS_THREAD_READY) != 0 ||
        thread_enqueue(&parked_thread) != 0) {
        rc = -15;
        goto out;
    }
    if (thread_remove_queued(&parked_thread) != 0) {
        rc = -16;
        goto out;
    }
    parked_thread.state = PROS_THREAD_BLOCKED;
    parked_thread.quarantine_parked = 1u;

    if (security_quarantine(&process) != 0 ||
        !process.quarantined ||
        process.state != PROS_PROCESS_BLOCKED ||
        process.quarantine_previous_state != (PROS_PROCESS_READY + 1u)) {
        rc = -1;
        goto out;
    }
    /* A repeated quarantine must preserve the saved READY state. */
    if (security_quarantine(&process) != 0 ||
        process.quarantine_previous_state != (PROS_PROCESS_READY + 1u) ||
        process.state != PROS_PROCESS_BLOCKED) {
        rc = -11;
        goto out;
    }
    if (security_clear_quarantine(&process) != 0 ||
        process.quarantined ||
        process.state != PROS_PROCESS_READY ||
        process.quarantine_previous_state != 0u ||
        parked_thread.state != PROS_THREAD_READY) {
        rc = -2;
        goto out;
    }
    if (security_clear_quarantine(&process) == 0) {
        rc = -3;
        goto out;
    }

    if (process_set_state(&process, PROS_PROCESS_RUNNING) != 0 ||
        security_quarantine(&process) != 0 ||
        process.state != PROS_PROCESS_BLOCKED ||
        process.quarantine_previous_state != (PROS_PROCESS_RUNNING + 1u)) {
        rc = -4;
        goto out;
    }
    if (security_clear_quarantine(&process) != 0 ||
        process.state != PROS_PROCESS_RUNNING ||
        process.quarantine_previous_state != 0u) {
        rc = -5;
        goto out;
    }

    /* A process that was already blocked must not have its wait state
     * converted into a runnable thread when quarantine is cleared. */
    if (thread_remove_queued(&parked_thread) != 0) {
        rc = -17;
        goto out;
    }
    parked_thread.state = PROS_THREAD_BLOCKED;
    if (process_set_state(&process, PROS_PROCESS_BLOCKED) != 0 ||
        security_quarantine(&process) != 0 ||
        process.state != PROS_PROCESS_BLOCKED ||
        process.quarantine_previous_state != 0u) {
        rc = -6;
        goto out;
    }
    if (security_clear_quarantine(&process) != 0 ||
        process.state != PROS_PROCESS_BLOCKED ||
        process.quarantine_previous_state != 0u ||
        parked_thread.state != PROS_THREAD_BLOCKED ||
        parked_thread.next || parked_thread.prev) {
        rc = -7;
        goto out;
    }

    process.state = PROS_PROCESS_NEW;
    if (security_quarantine(&process) == 0) {
        rc = -8;
        goto out;
    }
    process.state = PROS_PROCESS_ZOMBIE;
    if (security_quarantine(&process) == 0) {
        rc = -9;
        goto out;
    }
    process.state = PROS_PROCESS_DEAD;
    if (security_quarantine(&process) == 0) {
        rc = -10;
        goto out;
    }

out:
    /* Restore a reapable published state and release through the normal
     * process lifecycle; never manually destroy a registered process. */
    if (process.quarantined)
        (void)security_clear_quarantine(&process);
    process.state = PROS_PROCESS_READY;
    if (process_exit(&process, rc) != 0)
        return rc != 0 ? rc : -13;
    if (process_wait(parent, process.pid, &exit_code) != 0 ||
        exit_code != rc)
        return rc != 0 ? rc : -14;
    return rc;
}


enum pros_security_decision security_authorize(
    struct pros_process *process,
    const struct pros_intent *intent,
    uint64_t tid,
    uint64_t *reason_out) {
    uint32_t reason = PROS_REASON_NONE;
    enum pros_security_decision decision = PROS_DECISION_DENY;
    int quarantine_applied_here = 0;

    if (!process) {
        reason = PROS_REASON_PROCESS_INVALID;
        if (reason_out)
            *reason_out = reason;
        return PROS_DECISION_DENY;
    }

    /*
     * process_state() authenticates registry identity before reading process
     * fields. An unregistered/forged pointer returns DEAD; do not send that
     * pointer to the audit ledger, quarantine helpers, or monitor, all of
     * which legitimately dereference a registered process object.
     */
    uint32_t lifecycle_state = process_state(process);
    if (lifecycle_state == PROS_PROCESS_DEAD) {
        reason = PROS_REASON_PROCESS_INVALID;
        if (reason_out)
            *reason_out = reason;
        return PROS_DECISION_DENY;
    }
    if (lifecycle_state == PROS_PROCESS_ZOMBIE) {
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

    /*
     * A denied attempt to obtain ADMIN rights without a matching capability
     * is a high-confidence privilege-escalation signal. The authorization
     * path has already quarantined the process; feed the monitor the same
     * event so its score/action and the reason ledger agree.
     */
    if (process && reason == PROS_REASON_QUARANTINE_REQUEST) {
        const struct pros_security_event event = {
            .pid = process->pid,
            .timestamp = timer_ticks(),
            .type = PROS_SECURITY_EVENT_PRIVILEGE_ESCALATION,
            .severity = 2u
        };
        (void)pros_security_monitor_enforce(process, &event, 0);
    } else if (process && reason == PROS_REASON_POLICY_RESTRICTED) {
        /*
         * A rejected request that violates the operation/capability policy
         * is a monitor signal. One event contributes 70 points; repeated
         * violations cross the existing quarantine threshold. Keep the
         * authorization decision DENY even when monitoring adds quarantine.
         */
        const struct pros_security_event event = {
            .pid = process->pid,
            .timestamp = timer_ticks(),
            .type = PROS_SECURITY_EVENT_POLICY_VIOLATION,
            .severity = 1u
        };
        (void)pros_security_monitor_enforce(process, &event, 0);
    }

    if (reason_out) *reason_out = reason;
    return decision;
}

static int security_monitor_authorization_selftest(void) {
    const uint64_t pid = 0x534D4155u;
    struct pros_process process = {0};
    struct pros_process *parent = process_lookup(1u);
    struct pros_intent intent;
    struct pros_security_monitor_state monitor_state;
    struct pros_reason_record latest;
    uint64_t reason = PROS_REASON_NONE;
    int32_t exit_code = 0;
    int result = 0;

    if (!parent)
        return -1;

    {
        struct pros_process *forged =
            (struct pros_process *)(uintptr_t)0x0000800000000000ULL;
        uint64_t invalid_reason = PROS_REASON_NONE;
        if (security_authorize(0, 0, 0u, &invalid_reason) !=
                PROS_DECISION_DENY ||
            invalid_reason != PROS_REASON_PROCESS_INVALID)
            return -16;
        invalid_reason = PROS_REASON_NONE;
        if (security_authorize(forged, 0, 0u, &invalid_reason) !=
                PROS_DECISION_DENY ||
            invalid_reason != PROS_REASON_PROCESS_INVALID)
            return -17;
    }

    if (process_create(parent, &process, pid, PROS_SECURITY_LIMITED) != 0)
        return -1;

    if (security_make_intent(&intent, PROS_INTENT_ADMIN,
                             PROS_OBJECT_PROCESS, 1u,
                             PROSENJIT_CAP_ADMIN) != 0) {
        result = -2;
        goto cleanup;
    }
    if (security_authorize(&process, &intent, 1u, &reason) !=
            PROS_DECISION_QUARANTINE ||
        reason != PROS_REASON_QUARANTINE_REQUEST ||
        !security_is_quarantined(&process)) {
        result = -3;
        goto cleanup;
    }
    if (pros_security_monitor_get(pid, &monitor_state) != 0 ||
        monitor_state.pid != pid ||
        monitor_state.event_count != 1u ||
        monitor_state.score != 180u ||
        monitor_state.action != PROS_SECURITY_MONITOR_QUARANTINE) {
        result = -4;
        goto cleanup;
    }
    if (security_ledger_latest(&latest) != 0 ||
        latest.pid != pid ||
        latest.reason != PROS_REASON_QUARANTINE_REQUEST ||
        latest.decision != PROS_DECISION_QUARANTINE) {
        result = -5;
        goto cleanup;
    }

cleanup:
    if (security_is_quarantined(&process) &&
        security_clear_quarantine(&process) != 0 && result == 0)
        result = -6;
    (void)pros_security_monitor_clear(pid);
    /*
     * This test process is published in the real process registry. Reap it
     * through the normal lifecycle so no test-only teardown bypasses the
     * process identity, resource preflight, or unregister invariants.
     */
    if (process_exit(&process, result) != 0)
        return result != 0 ? result : -7;
    if (process_wait(parent, pid, &exit_code) != 0 || exit_code != result)
        return result != 0 ? result : -8;
    if (result != 0)
        return result;

    /*
     * Policy violations are scored without quarantining on the first event.
     * The second independent denied request crosses the 100-point threshold
     * and must quarantine the same registered process.
     */
    {
        const uint64_t policy_pid = pid + 1u;
        struct pros_process policy_process = {0};
        struct pros_security_monitor_state policy_state;
        struct pros_intent policy_intent;
        uint64_t policy_reason = PROS_REASON_NONE;
        int32_t policy_exit_code = 0;

        if (process_create(parent, &policy_process, policy_pid,
                           PROS_SECURITY_LIMITED) != 0)
            return -9;

        if (security_make_intent(&policy_intent, PROS_INTENT_READ,
                                 PROS_OBJECT_PROCESS, 1u,
                                 PROSENJIT_CAP_WRITE) != 0) {
            result = -10;
            goto policy_cleanup;
        }
        if (security_authorize(&policy_process, &policy_intent, 2u,
                               &policy_reason) != PROS_DECISION_DENY ||
            policy_reason != PROS_REASON_POLICY_RESTRICTED ||
            pros_security_monitor_get(policy_pid, &policy_state) != 0 ||
            policy_state.score != 70u || policy_state.event_count != 1u ||
            policy_state.action != PROS_SECURITY_MONITOR_ALLOW ||
            security_is_quarantined(&policy_process)) {
            result = -11;
            goto policy_cleanup;
        }

        if (security_make_intent(&policy_intent, PROS_INTENT_READ,
                                 PROS_OBJECT_PROCESS, 1u,
                                 PROSENJIT_CAP_WRITE) != 0 ||
            security_authorize(&policy_process, &policy_intent, 3u,
                               &policy_reason) != PROS_DECISION_DENY ||
            policy_reason != PROS_REASON_POLICY_RESTRICTED ||
            pros_security_monitor_get(policy_pid, &policy_state) != 0 ||
            policy_state.score != 140u || policy_state.event_count != 2u ||
            policy_state.action != PROS_SECURITY_MONITOR_QUARANTINE ||
            !security_is_quarantined(&policy_process)) {
            result = -12;
            goto policy_cleanup;
        }

policy_cleanup:
        if (security_is_quarantined(&policy_process) &&
            security_clear_quarantine(&policy_process) != 0 && result == 0)
            result = -13;
        (void)pros_security_monitor_clear(policy_pid);
        if (process_exit(&policy_process, result) != 0)
            return result != 0 ? result : -14;
        if (process_wait(parent, policy_pid, &policy_exit_code) != 0 ||
            policy_exit_code != result)
            return result != 0 ? result : -15;
    }
    return result;
}

uint64_t security_ledger_count(void) {
    return ledger_count_value;
}

int security_ledger_latest(struct pros_reason_record *record) {
    if (!record || ledger_count_value == 0) return -1;
    *record = ledger[(ledger_sequence - 1) % PROS_REASON_LEDGER_SIZE];
    return 0;
}

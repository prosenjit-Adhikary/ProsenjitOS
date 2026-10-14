#include "security_exec.h"

#include <stdint.h>
#include "security_quarantine.h"
#include "intent.h"

static void exec_force_containment(struct pros_process *process)
{
    if (!process)
        return;
    if (process->state != PROS_PROCESS_READY &&
        process->state != PROS_PROCESS_RUNNING &&
        process->state != PROS_PROCESS_BLOCKED)
        return;
    process->state = PROS_PROCESS_BLOCKED;
    process->quarantined = 1u;
    process->quarantine_previous_state = 0u;
}

int pros_security_exec_admit(
    struct pros_process *process,
    const struct pros_file_admission_record *record,
    uint64_t timestamp,
    uint64_t *quarantine_id) {
    if (!process || !record)
        return -1;
    if (process->state == PROS_PROCESS_DEAD ||
        process->state == PROS_PROCESS_ZOMBIE ||
        process->state == PROS_PROCESS_NEW ||
        process->state > PROS_PROCESS_BLOCKED)
        return -1;
    if (process_state(process) == PROS_PROCESS_DEAD) {
        exec_force_containment(process);
        return -1;
    }
    /*
     * File trust must never override process containment. A corrupt quarantine
     * marker/restore target is not equivalent to a valid quarantine and must
     * be canonicalized before returning from the execution boundary.
     */
    if (process->quarantined != 0u) {
        if (process->quarantined != 1u ||
            process->state != PROS_PROCESS_BLOCKED ||
            (process->quarantine_previous_state != 0u &&
             process->quarantine_previous_state !=
                 (uint8_t)(PROS_PROCESS_READY + 1u) &&
             process->quarantine_previous_state !=
                 (uint8_t)(PROS_PROCESS_RUNNING + 1u)))
            exec_force_containment(process);
        return PROS_SECURITY_EXEC_BLOCK;
    }
    if (process->quarantine_previous_state != 0u) {
        exec_force_containment(process);
        return PROS_SECURITY_EXEC_BLOCK;
    }

    if (!pros_file_admission_record_valid(record))
        return PROS_SECURITY_EXEC_BLOCK;

    if (record->security.action == PROS_FILE_ALLOW)
        return PROS_SECURITY_EXEC_ALLOW;

    if (record->security.action == PROS_FILE_QUARANTINE) {
        uint64_t id = 0;
        if (pros_security_quarantine_add(process->pid, record, timestamp, &id) != 0)
            return -1;
        if (security_quarantine(process) != 0) {
            (void)pros_security_quarantine_release(id);
            return -1;
        }
        if (quarantine_id) *quarantine_id = id;
        return PROS_SECURITY_EXEC_QUARANTINE;
    }

    if (record->security.action == PROS_FILE_BLOCK)
        return PROS_SECURITY_EXEC_BLOCK;

    return -1;
}

int pros_security_exec_selftest(void) {
    struct pros_process process;
    struct pros_file_admission_record record;
    struct pros_quarantine_entry entry;
    uint64_t quarantine_id = 0;

    /*
     * Keep this test local to the execution-security boundary. It proves
     * that ALLOW passes through, BLOCK creates no quarantine state, and
     * QUARANTINE records the evidence and contains the process.
     */
    if (process_init(&process, 0x1001u, PROS_SECURITY_LIMITED) != 0)
        return -1;
    if (process_set_state(&process, PROS_PROCESS_READY) != 0)
        return -2;

    record.security.state = PROS_FILE_TRUSTED;
    record.security.action = PROS_FILE_ALLOW;
    record.security.evidence = PROS_FILE_EVIDENCE_LOCAL_TRUST;
    for (uint32_t i = 0u; i < PROS_FILE_SHA256_BYTES; ++i)
        record.sha256[i] = (uint8_t)i;

    if (pros_security_exec_admit(&process, &record, 1u, 0) !=
            PROS_SECURITY_EXEC_ALLOW ||
        security_is_quarantined(&process))
        goto fail;

    /* Contradictory trust/action metadata must never authorize execution. */
    record.security.state = PROS_FILE_MALICIOUS;
    record.security.action = PROS_FILE_ALLOW;
    record.security.evidence = PROS_FILE_EVIDENCE_REPUTATION_MALICIOUS;
    if (pros_security_exec_admit(&process, &record, 2u, &quarantine_id) !=
            PROS_SECURITY_EXEC_BLOCK ||
        security_is_quarantined(&process) || process.state != PROS_PROCESS_READY)
        goto fail;

    record.security.state = PROS_FILE_MALICIOUS;
    record.security.action = PROS_FILE_BLOCK;
    record.security.evidence = PROS_FILE_EVIDENCE_REPUTATION_MALICIOUS;
    if (pros_security_exec_admit(&process, &record, 2u, &quarantine_id) !=
            PROS_SECURITY_EXEC_BLOCK ||
        quarantine_id != 0u ||
        security_is_quarantined(&process))
        goto fail;

    record.security.state = PROS_FILE_SUSPICIOUS;
    record.security.action = PROS_FILE_QUARANTINE;
    record.security.evidence = PROS_FILE_EVIDENCE_REPUTATION_SUSPICIOUS;
    if (pros_security_exec_admit(&process, &record, 3u, &quarantine_id) !=
            PROS_SECURITY_EXEC_QUARANTINE ||
        quarantine_id == 0u ||
        !security_is_quarantined(&process) ||
        process_state(&process) != PROS_PROCESS_BLOCKED)
        goto fail;

    if (pros_security_quarantine_get(quarantine_id, &entry) != 0 ||
        entry.owner_pid != process.pid ||
        entry.timestamp != 3u ||
        entry.state != PROS_FILE_SUSPICIOUS ||
        entry.action != PROS_FILE_QUARANTINE ||
        entry.evidence != PROS_FILE_EVIDENCE_REPUTATION_SUSPICIOUS)
        goto fail;

    if (pros_security_quarantine_release(quarantine_id) != 0 ||
        pros_security_quarantine_get(quarantine_id, &entry) == 0 ||
        security_clear_quarantine(&process) != 0 ||
        security_is_quarantined(&process) ||
        process_state(&process) != PROS_PROCESS_READY)
        goto fail;

    if (address_space_destroy(&process.address_space) != 0)
        return -3;

    /* A pre-existing quarantine outranks even a trusted file decision. */
    if (process_init(&process, 0x1002u, PROS_SECURITY_LIMITED) != 0)
        return -4;
    if (process_set_state(&process, PROS_PROCESS_READY) != 0 ||
        security_quarantine(&process) != 0)
        goto fail;
    record.security.state = PROS_FILE_TRUSTED;
    record.security.action = PROS_FILE_ALLOW;
    if (pros_security_exec_admit(&process, &record, 4u, 0) !=
            PROS_SECURITY_EXEC_BLOCK ||
        process.state != PROS_PROCESS_BLOCKED ||
        !security_is_quarantined(&process) ||
        security_clear_quarantine(&process) != 0)
        goto fail;

    process.state = PROS_PROCESS_READY;
    process.quarantined = 2u;
    process.quarantine_previous_state = UINT8_MAX;
    if (pros_security_exec_admit(&process, &record, 5u, 0) !=
            PROS_SECURITY_EXEC_BLOCK ||
        process.state != PROS_PROCESS_BLOCKED ||
        process.quarantined != 1u ||
        process.quarantine_previous_state != 0u ||
        security_clear_quarantine(&process) != 0 ||
        process.state != PROS_PROCESS_BLOCKED)
        goto fail;
    if (address_space_destroy(&process.address_space) != 0)
        return -5;

    /* Corrupt object metadata cannot leave a scheduler-visible process runnable. */
    if (process_init(&process, 0x1003u, PROS_SECURITY_LIMITED) != 0)
        return -6;
    if (process_set_state(&process, PROS_PROCESS_READY) != 0)
        goto fail;
    process.object.reserved = 1u;
    if (pros_security_exec_admit(&process, &record, 5u, 0) ==
            PROS_SECURITY_EXEC_ALLOW ||
        process.state != PROS_PROCESS_BLOCKED ||
        !security_is_quarantined(&process))
        goto fail;
    process.object.reserved = 0u;
    if (security_clear_quarantine(&process) != 0 ||
        process.state != PROS_PROCESS_BLOCKED)
        goto fail;
    if (address_space_destroy(&process.address_space) != 0)
        return -7;

    /*
     * A zombie has completed execution and is awaiting reap; it must never
     * cross the execution-admission boundary again.
     */
    if (process_init(&process, 0x1004u, PROS_SECURITY_LIMITED) != 0)
        return -8;
    /* Build a terminal-state fixture directly; the generic state API
     * intentionally cannot manufacture terminal lifecycle transitions. */
    process.state = PROS_PROCESS_ZOMBIE;
    record.security.state = PROS_FILE_TRUSTED;
    record.security.action = PROS_FILE_ALLOW;
    if (pros_security_exec_admit(&process, &record, 6u, 0) ==
        PROS_SECURITY_EXEC_ALLOW)
        goto zombie_fail;
    if (address_space_destroy(&process.address_space) != 0)
        return -5;

    return 0;

zombie_fail:
    (void)address_space_destroy(&process.address_space);
    return -6;

fail:
    (void)address_space_destroy(&process.address_space);
    return -4;
}

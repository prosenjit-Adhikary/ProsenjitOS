#include "security_exec.h"

#include <stdint.h>
#include "security_quarantine.h"

int pros_security_exec_admit(
    struct pros_process *process,
    const struct pros_file_admission_record *record,
    uint64_t timestamp,
    uint64_t *quarantine_id) {
    if (!process || !record || process_state(process) == PROS_PROCESS_DEAD)
        return -1;

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
    return 0;
}

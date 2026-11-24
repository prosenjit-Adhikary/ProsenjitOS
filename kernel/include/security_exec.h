#ifndef PROSENJIT_SECURITY_EXEC_H
#define PROSENJIT_SECURITY_EXEC_H

#include <stdint.h>
#include "process.h"
#include "security_admission.h"

enum pros_security_exec_result {
    PROS_SECURITY_EXEC_ALLOW = 0,
    PROS_SECURITY_EXEC_QUARANTINE = 1,
    PROS_SECURITY_EXEC_BLOCK = 2,
};

int pros_security_exec_admit(
    struct pros_process *process,
    const struct pros_file_admission_record *record,
    uint64_t timestamp,
    uint64_t *quarantine_id);

int pros_security_exec_selftest(void);

#endif

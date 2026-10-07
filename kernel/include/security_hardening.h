#ifndef PROSENJIT_SECURITY_HARDENING_H
#define PROSENJIT_SECURITY_HARDENING_H

#include <stdint.h>
#include "process.h"

int pros_security_hardening_init(void);
int pros_security_hardening_check_process(const struct pros_process *process);
void pros_security_hardening_note_violation(uint32_t code, uint64_t value);
uint64_t pros_security_hardening_violations(void);
int pros_security_hardening_ready(void);
int pros_security_hardening_selftest(void);

#endif

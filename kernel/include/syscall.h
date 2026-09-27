#ifndef PROSENJIT_SYSCALL_H
#define PROSENJIT_SYSCALL_H

#include <stdint.h>

void syscall_dispatch(uint64_t *registers, uint64_t *cpu_frame);

#endif

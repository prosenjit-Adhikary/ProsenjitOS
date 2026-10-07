#ifndef PROSENJIT_SYSCALL_H
#define PROSENJIT_SYSCALL_H

#include <stdint.h>

struct pros_process;

/*
 * These buffers are shared with the assembly interrupt return path.
 * Volatile prevents the compiler from removing stores that IRETQ consumes.
 */
void syscall_dispatch(volatile uint64_t *registers, volatile uint64_t *cpu_frame);
int syscall_selftest(struct pros_process *process);

#endif

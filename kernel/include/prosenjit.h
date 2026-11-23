#ifndef PROSENJIT_H
#define PROSENJIT_H

#include <stdint.h>

#define PROSENJIT_OS_VERSION "0.1.0"

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void kernel_main(uint32_t boot_magic, uint32_t boot_info);
void idt_init(void);
uint64_t cpu_security_init(void);
/*
 * Assembly exception entry returns either the raw ISR frame (skip the
 * vector/error pair) or a normalized scheduler frame (no pair to skip).
 * SysV AMD64 returns these two qwords in RAX and RDX respectively.
 */
struct exception_dispatch_return {
    uint64_t frame;
    uint64_t skip_vector_error;
};
struct exception_dispatch_return exception_dispatch(uint64_t *frame);
void syscall_dispatch(volatile uint64_t *registers, volatile uint64_t *cpu_frame);

#endif

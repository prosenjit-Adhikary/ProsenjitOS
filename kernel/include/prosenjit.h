#ifndef PROSENJIT_H
#define PROSENJIT_H

#include <stdint.h>

#define PROSENJIT_OS_VERSION "0.1.0"

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Boot entry passes the captured CPUID values alongside the legacy handoff. */
void kernel_main(uint32_t boot_magic, uint32_t boot_info,
                 uint32_t max_extended_leaf,
                 uint32_t extended_leaf_8_eax,
                 uint32_t leaf_1_edx);
void idt_init(void);
uint64_t cpu_security_init(void);
void exception_dispatch(uint64_t *frame);
void syscall_dispatch(volatile uint64_t *registers, volatile uint64_t *cpu_frame);

#endif

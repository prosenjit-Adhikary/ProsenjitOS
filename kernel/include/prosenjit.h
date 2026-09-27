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
void exception_dispatch(uint64_t *frame);

#endif

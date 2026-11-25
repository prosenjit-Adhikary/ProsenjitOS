#ifndef PROSENJIT_USERCOPY_H
#define PROSENJIT_USERCOPY_H

#include <stdint.h>

struct pros_address_space;

/*
 * Validated user-memory access foundation.
 * These helpers validate the complete range before copying and never hold
 * kernel locks while touching user memory.
 */
int copy_from_user(const struct pros_address_space *space,
                   void *kernel_destination,
                   uint64_t user_source,
                   uint64_t size);

int copy_to_user(const struct pros_address_space *space,
                 uint64_t user_destination,
                 const void *kernel_source,
                 uint64_t size);

/* Per-CPU state used by the fault-recovering assembly copy paths. */
int percpu_init(void);
uint64_t usercopy_fault_active(void);
uint64_t usercopy_fault_rip(void);
int usercopy_fault_matches(uint64_t rip, uint64_t cr2);
void usercopy_fault_clear(void);

#endif

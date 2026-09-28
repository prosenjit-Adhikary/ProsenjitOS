#ifndef PROSENJIT_USERCOPY_H
#define PROSENJIT_USERCOPY_H

#include <stdint.h>

struct pros_address_space;

int copy_from_user(const struct pros_address_space *space,
                   void *kernel_destination,
                   uint64_t user_source,
                   uint64_t size);

int copy_to_user(const struct pros_address_space *space,
                 uint64_t user_destination,
                 const void *kernel_source,
                 uint64_t size);

/* Initializes the current CPU's usercopy fault-fixup state. */
int percpu_init(void);

/* Page-fault handler queries the state belonging to the current CPU. */
uint64_t usercopy_fault_active(void);
uint64_t usercopy_fault_rip(void);
void usercopy_fault_clear(void);

#endif

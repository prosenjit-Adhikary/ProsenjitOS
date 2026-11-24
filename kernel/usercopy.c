#include "usercopy.h"
#include "address_space.h"

/*
 * Fault-recovery state for the current execution context.
 *
 * The kernel is currently single-CPU, so a single state record is sufficient
 * and avoids depending on GS-base state while handling the page fault itself.
 * When SMP/per-CPU scheduling is enabled, this state must move back behind a
 * real per-CPU pointer that is switched atomically with the CPU context.
 */
volatile uint64_t usercopy_fault_active_state __attribute__((aligned(16)));
volatile uint64_t usercopy_fault_rip_state;

int percpu_init(void)
{
    usercopy_fault_active_state = 0;
    usercopy_fault_rip_state = 0;
    return 0;
}

extern int copy_from_user_asm(void *destination, const void *source, uint64_t size);
extern int copy_to_user_asm(void *destination, const void *source, uint64_t size);

uint64_t usercopy_fault_active(void) {
    return usercopy_fault_active_state;
}

uint64_t usercopy_fault_rip(void) {
    return usercopy_fault_rip_state;
}

void usercopy_fault_clear(void) {
    usercopy_fault_active_state = 0;
    usercopy_fault_rip_state = 0;
}

int copy_from_user(const struct pros_address_space *space,
                   void *kernel_destination,
                   uint64_t user_source,
                   uint64_t size) {
    if (!space || !kernel_destination || size == 0)
        return -1;
    if (address_space_validate_user_range(space, user_source, size, 0) != 0)
        return -1;
    return copy_from_user_asm(kernel_destination,
                               (const void *)(uintptr_t)user_source, size);
}

int copy_to_user(const struct pros_address_space *space,
                 uint64_t user_destination,
                 const void *kernel_source,
                 uint64_t size) {
    if (!space || !kernel_source || size == 0)
        return -1;
    if (address_space_validate_user_range(space, user_destination, size, 1) != 0)
        return -1;
    return copy_to_user_asm((void *)(uintptr_t)user_destination,
                            kernel_source, size);
}

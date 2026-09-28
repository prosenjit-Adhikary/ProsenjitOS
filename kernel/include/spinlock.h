#ifndef PROSENJIT_SPINLOCK_H
#define PROSENJIT_SPINLOCK_H

#include <stdint.h>

struct pros_spinlock {
    volatile uint32_t value;
};

#define PROS_SPINLOCK_INIT { 0 }

uint64_t pros_spin_lock_irqsave(struct pros_spinlock *lock);
void pros_spin_unlock_irqrestore(struct pros_spinlock *lock, uint64_t flags);

#endif

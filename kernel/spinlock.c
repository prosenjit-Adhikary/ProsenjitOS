#include "spinlock.h"

static inline uint64_t read_rflags(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_disable(void) {
    __asm__ volatile ("cli" : : : "memory");
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

uint64_t pros_spin_lock_irqsave(struct pros_spinlock *lock) {
    if (!lock) return 0;
    uint64_t flags = read_rflags();
    irq_disable();
    while (__sync_lock_test_and_set(&lock->value, 1u) != 0u)
        __asm__ volatile ("pause" : : : "memory");
    return flags;
}

void pros_spin_unlock_irqrestore(struct pros_spinlock *lock, uint64_t flags) {
    if (!lock) return;
    __sync_lock_release(&lock->value);
    irq_restore(flags);
}

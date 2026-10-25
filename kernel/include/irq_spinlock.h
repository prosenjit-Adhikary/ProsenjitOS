#ifndef PROSENJIT_IRQ_SPINLOCK_H
#define PROSENJIT_IRQ_SPINLOCK_H

#include <stdint.h>

/*
 * x86-64 IRQ-save spinlock primitive for kernel-owned volatile uint32_t words.
 *
 * Save the caller's RFLAGS and disable local interrupts before contending.
 * The caller-local token is required: storing saved flags in the shared lock
 * would break nested acquisition (for example, VFS handle lock -> filesystem
 * lock). Unlock releases the word before restoring IF, and never enables
 * interrupts when the caller entered with IF clear.
 *
 * This primitive is for short, non-sleeping critical sections only. It is
 * currently used around synchronous filesystem callbacks for correctness, but
 * that can hold local interrupts off for a long time; replace those call sites
 * with a scheduler-aware mutex before callbacks may block or the kernel needs
 * bounded interrupt latency. Atomic test-and-set/release provides lock-word
 * exclusion; cli only masks interrupts on the current CPU.
 */
static inline uint64_t pros_irq_spin_lock(volatile uint32_t *word)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (__sync_lock_test_and_set(word, 1u) != 0u)
        __asm__ volatile ("pause");
    __sync_synchronize();
    return flags;
}

static inline void pros_irq_spin_unlock(volatile uint32_t *word,
                                        uint64_t flags)
{
    __sync_synchronize();
    __sync_lock_release(word);
    if ((flags & (1ULL << 9)) != 0u)
        __asm__ volatile ("sti" : : : "memory");
}

#endif

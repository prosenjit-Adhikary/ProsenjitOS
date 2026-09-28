#include <stdint.h>
#include "usercopy.h"

#define PROSENJIT_MSR_GS_BASE 0xC0000101u
#define PROSENJIT_MAX_CPUS 256u

struct pros_percpu_state {
    volatile uint64_t usercopy_fault_active;
    volatile uint64_t usercopy_fault_rip;
} __attribute__((aligned(64)));

static struct pros_percpu_state percpu_states[PROSENJIT_MAX_CPUS];

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(subleaf));
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high) : "memory");
}

int percpu_init(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    uint32_t cpu_id = (ebx >> 24) & 0xFFu;
    struct pros_percpu_state *state = &percpu_states[cpu_id];
    state->usercopy_fault_active = 0;
    state->usercopy_fault_rip = 0;
    wrmsr(PROSENJIT_MSR_GS_BASE, (uint64_t)(uintptr_t)state);
    return 0;
}

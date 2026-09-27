#include "timer.h"

static volatile uint64_t ticks;
static uint32_t frequency;

static inline void timer_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

int timer_init(uint32_t hz) {
    if (hz == 0 || hz > 1193182u) return -1;
    uint32_t divisor = 1193182u / hz;
    if (divisor == 0 || divisor > 65535u) return -1;
    frequency = 1193182u / divisor;
    ticks = 0;
    timer_out8(0x43, 0x36);
    timer_out8(0x40, (uint8_t)(divisor & 0xFFu));
    timer_out8(0x40, (uint8_t)((divisor >> 8) & 0xFFu));
    return 0;
}

uint64_t timer_ticks(void) {
    return ticks;
}

void timer_dispatch(void) {
    ++ticks;
}

#include "timer.h"
#include "pic.h"

static volatile uint64_t ticks;
static uint64_t last_preempt_tick;
static volatile uint64_t irq_count;
static volatile uint64_t preempt_count;
static inline void timer_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

int timer_init(uint32_t hz) {
    if (hz == 0 || hz > 1193182u) return -1;
    uint32_t divisor = 1193182u / hz;
    if (divisor == 0 || divisor > 65535u) return -1;
    ticks = 0;
    last_preempt_tick = 0;
    irq_count = 0;
    preempt_count = 0;
    timer_out8(0x43, 0x36);
    timer_out8(0x40, (uint8_t)(divisor & 0xFFu));
    timer_out8(0x40, (uint8_t)((divisor >> 8) & 0xFFu));
    return pic_init_for_timer();
}

uint64_t timer_ticks(void) {
    return ticks;
}

void timer_dispatch(uint64_t *frame) {
    (void)frame;
    ++ticks;
    ++irq_count;
}

int timer_should_preempt(void) {
    if (ticks == 0 || ticks == last_preempt_tick) return 0;
    last_preempt_tick = ticks;
    if ((ticks % 10u) == 0) {
        ++preempt_count;
        return 1;
    }
    return 0;
}

uint64_t timer_irq_count(void) { return irq_count; }
uint64_t timer_preempt_count(void) { return preempt_count; }

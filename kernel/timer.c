#include "timer.h"
#include "pic.h"
#include "health.h"

#define PROSENJIT_PIT_BASE_HZ 1193182u

#define PROSENJIT_PIT_BASE_HZ 1193182u

static volatile uint64_t ticks;
static uint64_t last_preempt_tick;
static volatile uint64_t irq_count;
static volatile uint64_t preempt_count;
static uint32_t timer_hz;
static uint32_t timer_divisor;
static uint32_t timer_hz;
static uint32_t timer_divisor;
static inline void timer_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

int timer_init(uint32_t hz) {
    if (hz == 0 || hz > 1193182u) return -1;
    uint32_t divisor = PROSENJIT_PIT_BASE_HZ / hz;
    if (divisor == 0 || divisor > 65535u) return -1;
    ticks = 0;
    timer_divisor = divisor;
    timer_hz = PROSENJIT_PIT_BASE_HZ / divisor;
    timer_divisor = divisor;
    timer_hz = PROSENJIT_PIT_BASE_HZ / divisor;
    last_preempt_tick = 0;
    irq_count = 0;
    preempt_count = 0;
    timer_out8(0x43, 0x36);
    timer_out8(0x40, (uint8_t)(divisor & 0xFFu));
    timer_out8(0x40, (uint8_t)((divisor >> 8) & 0xFFu));
    return pic_init_for_timer();
}

uint64_t timer_ticks(void) {
    /* ticks is a monotonic counter; it is never reset after initialization. */
    return ticks;
}

uint32_t timer_frequency_hz(void) {
    return timer_hz;
}

uint64_t timer_uptime_ms(void) {
    uint64_t hz = timer_hz;
    if (hz == 0u) return 0u;
    return (timer_ticks() * 1000ULL) / hz;
}

int timer_selftest(void) {
    uint64_t first;
    uint64_t second;
    if (timer_hz == 0u || timer_divisor == 0u)
        return -1;
    first = timer_ticks();
    second = timer_ticks();
    if (second < first)
        return -1;
    if (timer_uptime_ms() == UINT64_MAX && second != UINT64_MAX)
        return -1;
    return 0;
}

void timer_dispatch(uint64_t *frame) {
    (void)frame;
    ++ticks;
    ++irq_count;

    /*
     * The timer is the real kernel heartbeat. The health watchdog is checked
     * from this interrupt path so watchdog state cannot depend on a userspace
     * service or a periodic polling thread that may itself be unhealthy.
     * Service 1 is the kernel health monitor registered during boot.
     */
    if ((ticks % 100u) == 0u) {
        (void)pros_health_watchdog_kick(1u);
        (void)pros_health_watchdog_poll();
    }
    if ((ticks % 1000u) == 0u)
        (void)pros_health_sample();
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

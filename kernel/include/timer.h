#ifndef PROSENJIT_TIMER_H
#define PROSENJIT_TIMER_H
#include <stdint.h>
int timer_init(uint32_t hz);
uint64_t timer_ticks(void);
/* Actual PIT-derived tick rate used by the monotonic uptime source. */
uint32_t timer_frequency_hz(void);
/* Monotonic uptime derived from timer_ticks(), rounded down to milliseconds. */
uint64_t timer_uptime_ms(void);
/* Validate the monotonic time source contract after timer initialization. */
int timer_selftest(void);
/* Actual PIT-derived tick rate used by the monotonic uptime source. */
uint32_t timer_frequency_hz(void);
/* Monotonic uptime derived from timer_ticks(), rounded down to milliseconds. */
uint64_t timer_uptime_ms(void);
/* Validate the monotonic time source contract after timer initialization. */
int timer_selftest(void);
void timer_dispatch(uint64_t *frame);

/* Return non-zero when a scheduler tick should request a context switch. */
int timer_should_preempt(void);
uint64_t timer_irq_count(void);
uint64_t timer_preempt_count(void);
#endif

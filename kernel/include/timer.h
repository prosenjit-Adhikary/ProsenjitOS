#ifndef PROSENJIT_TIMER_H
#define PROSENJIT_TIMER_H
#include <stdint.h>
int timer_init(uint32_t hz);
uint64_t timer_ticks(void);
void timer_dispatch(void);
#endif

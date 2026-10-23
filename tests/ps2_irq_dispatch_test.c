#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "input_service.h"
#include "pic.h"
#include "timer.h"

static int input_ready;
static unsigned poll_calls;
static unsigned eoi_calls;
static uint8_t last_eoi;

int pros_input_service_is_ready(void) { return input_ready; }
int pros_input_service_poll_hardware(void) { ++poll_calls; return 0; }
int pic_init_for_timer(void) { return 0; }
void pic_eoi_irq(uint8_t irq) { ++eoi_calls; last_eoi = irq; }
void pic_eoi_master(void) {}

int main(void)
{
    /* Unknown vectors must not poll hardware or acknowledge an unrelated IRQ. */
    ps2_irq_dispatch(0u);
    ps2_irq_dispatch(32u);
    ps2_irq_dispatch(33u + 1u);
    assert(poll_calls == 0u && eoi_calls == 0u);

    /* Valid PS/2 vectors always EOI; polling is conditional on service readiness. */
    input_ready = 0;
    ps2_irq_dispatch(1u);
    assert(poll_calls == 0u && eoi_calls == 1u && last_eoi == 1u);
    ps2_irq_dispatch(12u);
    assert(poll_calls == 0u && eoi_calls == 2u && last_eoi == 12u);

    input_ready = 1;
    ps2_irq_dispatch(1u);
    assert(poll_calls == 1u && eoi_calls == 3u && last_eoi == 1u);
    ps2_irq_dispatch(12u);
    assert(poll_calls == 2u && eoi_calls == 4u && last_eoi == 12u);

    puts("PS/2 IRQ dispatcher hosted contract tests passed");
    return 0;
}

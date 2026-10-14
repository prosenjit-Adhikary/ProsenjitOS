#ifndef PROSENJIT_KERNEL_INPUT_H
#define PROSENJIT_KERNEL_INPUT_H

#include <stdint.h>
#include "../../include/abi/input.h"

#define PROS_INPUT_OK          0
#define PROS_INPUT_EMPTY       1
#define PROS_INPUT_EINVAL     -1

/*
 * Poll the legacy PS/2 controller using a bounded read budget, enqueue decoded
 * set-1 keyboard events, then return at most one event. No IRQ is enabled by
 * this module; it does not alter the existing PIC/IDT configuration.
 */
int pros_input_poll(struct pros_input_event *event);
int pros_input_selftest(void);

#endif

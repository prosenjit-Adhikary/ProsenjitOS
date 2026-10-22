#ifndef PROSENJIT_INPUT_SERVICE_H
#define PROSENJIT_INPUT_SERVICE_H

#include <stdint.h>
#include "hw_stack.h"

#define PROS_INPUT_SERVICE_ENDPOINT_ID 0x50534931u
#define PROS_INPUT_SERVICE_QUEUE_CAPACITY 128u

/* Initialize the bounded kernel input queue and register its native endpoint.
 * Call after pros_hw_stack_init() and before enabling hardware interrupts. */
int pros_input_service_init(void);

/* Poll the legacy PS/2 controller from a bounded safe kernel context.
 * The timer heartbeat calls this once per tick after service initialization. */
int pros_input_service_poll_hardware(void);

/* Read one queued event. Returns PROS_DRIVER_EBUSY when the queue is empty. */
int pros_input_service_read(struct pros_hw_input_event *out);
uint32_t pros_input_service_dropped_events(void);
int pros_input_service_is_ready(void);

#endif

#ifndef PROSENJIT_INPUT_SERVICE_H
#define PROSENJIT_INPUT_SERVICE_H

#include <stdint.h>
#include "hw_stack.h"

#define PROS_INPUT_SERVICE_ENDPOINT_ID 0x50534931u
#define PROS_INPUT_SERVICE_QUEUE_CAPACITY 128u

struct pros_process;

/* Initialize the bounded kernel input queue and register its native endpoint.
 * Call after pros_hw_stack_init() and before enabling hardware interrupts. */
int pros_input_service_init(void);

/* Unregister the input endpoint. Returns PROS_DRIVER_EBUSY if a transfer is
 * in flight (retry later), or PROS_DRIVER_ENODEV if already stopped. */
int pros_input_service_shutdown(void);

/* Kernel-only capability provisioning: grant a process a READ-only handle to
 * this input endpoint. Never expose the underlying object pointer to userspace. */
int pros_input_service_grant_read(struct pros_process *process,
                                  uint64_t *handle_out);

/* Consume one event only when the caller presents a valid READ handle to this
 * exact input endpoint. Returns EBUSY for an empty queue and EPERM for denial. */
int pros_input_service_read_capability(const struct pros_process *process,
                                       uint64_t handle,
                                       struct pros_hw_input_event *out);

/* Poll the legacy PS/2 controller from a bounded safe kernel context.
 * The timer heartbeat invokes this every four ticks after service initialization. */
/* Atomically queue a decoded hardware packet (1..queue-capacity events).
 * On ENOSPC the whole batch is dropped and counted; no partial packet appears. */
int pros_input_service_send_batch(uint32_t endpoint_id,
                                  const struct pros_hw_input_event *events,
                                  uint32_t count);

int pros_input_service_poll_hardware(void);

/* Kernel-only queue access. Returns PROS_DRIVER_EBUSY when the queue is empty. */
int pros_input_service_read(struct pros_hw_input_event *out);
uint32_t pros_input_service_dropped_events(void);
int pros_input_service_is_ready(void);

#endif

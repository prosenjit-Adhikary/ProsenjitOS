#include <assert.h>
#include <stdint.h>
#include "input_service.h"
#include "ps2_keyboard.h"
#include "process.h"

/* Host test isolates the queue from the kernel process manager. */
int process_grant_handle(struct pros_process *process, struct pros_object *object,
                         uint64_t rights, uint64_t *handle_out)
{
    (void)process; (void)object; (void)rights; (void)handle_out;
    return -1;
}
int process_check_handle(const struct pros_process *process, uint64_t handle,
                         uint64_t required_rights, uint16_t expected_type)
{
    (void)process; (void)handle; (void)required_rights; (void)expected_type;
    return -1;
}

int main(void)
{
    struct pros_hw_input_event event;
    pros_ps2_keyboard_test_reset_fifo();
    pros_ps2_keyboard_reset_decoder();

    assert(pros_input_service_is_ready() == 0);
    assert(pros_input_service_grant_read(0, 0) == PROS_DRIVER_EINVAL);
    assert(pros_input_service_read_capability(0, 1u, &event) == PROS_DRIVER_EINVAL);
    assert(pros_hw_stack_init() == PROS_DRIVER_OK);
    assert(pros_input_service_init() == PROS_DRIVER_OK);
    assert(pros_input_service_is_ready() == 1);
    assert(pros_input_service_read(&event) == PROS_DRIVER_EBUSY);
    assert(pros_input_service_read(0) == PROS_DRIVER_EINVAL);

    assert(pros_ps2_keyboard_test_push(0x01u, 0x1Eu) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x9Eu) == PROS_DRIVER_OK);
    assert(pros_input_service_poll_hardware() == 2);
    assert(pros_input_service_read(&event) == PROS_DRIVER_OK);
    assert(event.type == PROS_HW_INPUT_KEY);
    assert(event.value0 == 0x1E && event.flags == PROS_HW_INPUT_PRESS);
    assert(pros_input_service_read(&event) == PROS_DRIVER_OK);
    assert(event.value0 == 0x1E && event.flags == PROS_HW_INPUT_RELEASE);
    assert(pros_input_service_read(&event) == PROS_DRIVER_EBUSY);

    /* The callback must reject malformed events even when a caller bypasses
     * pros_hw_input_send() and submits directly through the generic endpoint. */
    event.version = PROS_HW_STACK_ABI_VERSION;
    event.type = PROS_HW_INPUT_KEY;
    event.flags = PROS_HW_INPUT_PRESS | PROS_HW_INPUT_RELEASE;
    event.reserved = 0u;
    event.timestamp = 0u;
    event.value0 = 0x20;
    event.value1 = 0;
    assert(pros_hw_endpoint_submit(PROS_INPUT_SERVICE_ENDPOINT_ID,
                                   PROS_HW_ENDPOINT_INPUT, &event,
                                   sizeof(event)) == PROS_DRIVER_EINVAL);
    event.flags = PROS_HW_INPUT_PRESS | PROS_HW_INPUT_RELATIVE;
    assert(pros_hw_endpoint_submit(PROS_INPUT_SERVICE_ENDPOINT_ID,
                                   PROS_HW_ENDPOINT_INPUT, &event,
                                   sizeof(event)) == PROS_DRIVER_EINVAL);
    event.flags = PROS_HW_INPUT_PRESS;
    event.reserved = 1u;
    assert(pros_hw_endpoint_submit(PROS_INPUT_SERVICE_ENDPOINT_ID,
                                   PROS_HW_ENDPOINT_INPUT, &event,
                                   sizeof(event)) == PROS_DRIVER_EINVAL);
    event.reserved = 0u;
    event.flags = 0x80000000u;
    assert(pros_hw_endpoint_submit(PROS_INPUT_SERVICE_ENDPOINT_ID,
                                   PROS_HW_ENDPOINT_INPUT, &event,
                                   sizeof(event)) == PROS_DRIVER_EINVAL);

    /* Fill the queue through the same public endpoint used by the PS/2 poller. */
    event.version = PROS_HW_STACK_ABI_VERSION;
    event.type = PROS_HW_INPUT_KEY;
    event.flags = PROS_HW_INPUT_PRESS;
    event.reserved = 0u;
    event.timestamp = 0u;
    event.value0 = 0x20;
    event.value1 = 0;
    for (uint32_t i = 0u; i < PROS_INPUT_SERVICE_QUEUE_CAPACITY; ++i)
        assert(pros_hw_input_send(PROS_INPUT_SERVICE_ENDPOINT_ID, &event) == PROS_DRIVER_OK);
    assert(pros_hw_input_send(PROS_INPUT_SERVICE_ENDPOINT_ID, &event) == PROS_DRIVER_ENOSPC);
    assert(pros_input_service_dropped_events() == 1u);

    for (uint32_t i = 0u; i < PROS_INPUT_SERVICE_QUEUE_CAPACITY; ++i)
        assert(pros_input_service_read(&event) == PROS_DRIVER_OK);
    assert(pros_input_service_read(&event) == PROS_DRIVER_EBUSY);
    assert(pros_hw_endpoint_unregister(PROS_INPUT_SERVICE_ENDPOINT_ID,
                                      PROS_HW_ENDPOINT_INPUT) == PROS_DRIVER_OK);
    assert(pros_input_service_is_ready() == 0);
    return 0;
}

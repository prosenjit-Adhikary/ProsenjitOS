#include <assert.h>
#include <stdint.h>
#include "input_service.h"
#include "ps2_keyboard.h"
#include "process.h"

/* Deterministic stand-in for the kernel's monotonic PIT tick source. */
uint64_t timer_ticks(void) { return 1234u; }

/* Host test isolates the queue from the kernel process manager. */
int process_grant_handle(struct pros_process *process, struct pros_object *object,
                         uint64_t rights, uint64_t *handle_out)
{
    if (!process || !object || !handle_out) return -1;
    uint64_t handle = handle_alloc(&process->handles, object, rights);
    if (handle == PROSENJIT_HANDLE_INVALID) return -1;
    *handle_out = handle;
    return 0;
}
int process_check_handle(const struct pros_process *process, uint64_t handle,
                         uint64_t required_rights, uint16_t expected_type)
{
    if (!process) return -1;
    return handle_check(&process->handles, handle, required_rights, expected_type);
}
int process_close_handle(struct pros_process *process, uint64_t handle)
{
    if (!process) return -1;
    return handle_close(&process->handles, handle);
}

int main(void)
{
    struct pros_hw_input_event event;
    struct pros_process process = {0};
    uint64_t input_handle = PROSENJIT_HANDLE_INVALID;
    handle_table_init(&process.handles);
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
    assert(event.timestamp == 1234u);
    assert(pros_input_service_read(&event) == PROS_DRIVER_OK);
    assert(event.value0 == 0x1E && event.flags == PROS_HW_INPUT_RELEASE);
    assert(pros_input_service_read(&event) == PROS_DRIVER_EBUSY);

    /* AUX bytes from the shared PS/2 controller must reach the pointer queue,
     * not be mistaken for keyboard scancodes or silently discarded. */
    assert(pros_ps2_keyboard_test_push(0x21u, 0x08u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 10u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 5u) == PROS_DRIVER_OK);
    assert(pros_input_service_poll_hardware() == 1);
    assert(pros_input_service_read(&event) == PROS_DRIVER_OK);
    assert(event.type == PROS_HW_INPUT_POINTER);
    assert(event.flags == PROS_HW_INPUT_RELATIVE);
    assert(event.value0 == 10 && event.value1 == -5);
    assert(event.timestamp == 1234u);
    assert(pros_input_service_read(&event) == PROS_DRIVER_EBUSY);

    /* Button state transitions use the same queue and keep keyboard events
     * separate from pointer events. */
    assert(pros_ps2_keyboard_test_push(0x21u, 0x09u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0u) == PROS_DRIVER_OK);
    assert(pros_input_service_poll_hardware() == 1);
    assert(pros_input_service_read(&event) == PROS_DRIVER_OK);
    assert(event.type == PROS_HW_INPUT_POINTER);
    assert(event.flags == PROS_HW_INPUT_PRESS && event.value0 == 1);
    assert(event.timestamp == 1234u);
    assert(pros_input_service_read(&event) == PROS_DRIVER_EBUSY);

    /* A parity error drops a partial packet but must not forget the held
     * left button: the next clean packet reports its release edge. */
    assert(pros_ps2_keyboard_test_push(0xA1u, 0u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0x08u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0u) == PROS_DRIVER_OK);
    assert(pros_input_service_poll_hardware() == 1);
    assert(pros_input_service_read(&event) == PROS_DRIVER_OK);
    assert(event.type == PROS_HW_INPUT_POINTER);
    assert(event.flags == PROS_HW_INPUT_RELEASE && event.value0 == 1);
    assert(event.timestamp == 1234u);
    assert(pros_input_service_read(&event) == PROS_DRIVER_EBUSY);

    /* AUX bytes are a separate stream: they must neither consume nor cancel
     * the five remaining bytes of the keyboard Pause sequence. */
    assert(pros_ps2_keyboard_test_push(0x01u, 0xE1u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0x08u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x1Du) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x45u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0xE1u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x9Du) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0xC5u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x1Eu) == PROS_DRIVER_OK);
    assert(pros_input_service_poll_hardware() == 1);
    assert(pros_input_service_read(&event) == PROS_DRIVER_OK);
    assert(event.type == PROS_HW_INPUT_KEY);
    assert(event.flags == PROS_HW_INPUT_PRESS && event.value0 == 0x1E);
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

    /* Pointer events must be either motion-only or a single button edge. */
    event.type = PROS_HW_INPUT_POINTER;
    event.flags = 0u;
    assert(pros_hw_endpoint_submit(PROS_INPUT_SERVICE_ENDPOINT_ID,
                                   PROS_HW_ENDPOINT_INPUT, &event,
                                   sizeof(event)) == PROS_DRIVER_EINVAL);
    event.flags = PROS_HW_INPUT_RELATIVE;
    assert(pros_hw_endpoint_submit(PROS_INPUT_SERVICE_ENDPOINT_ID,
                                   PROS_HW_ENDPOINT_INPUT, &event,
                                   sizeof(event)) == PROS_DRIVER_OK);
    assert(pros_input_service_read(&event) == PROS_DRIVER_OK);
    assert(event.type == PROS_HW_INPUT_POINTER &&
           event.flags == PROS_HW_INPUT_RELATIVE);

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

    /* A live READ capability pins this service object's lifetime. Stopping
     * and restarting it must never reactivate a handle held across teardown. */
    assert(pros_input_service_grant_read(&process, &input_handle) == PROS_DRIVER_OK);
    assert(input_handle != PROSENJIT_HANDLE_INVALID);
    assert(pros_input_service_shutdown() == PROS_DRIVER_EBUSY);
    assert(pros_input_service_is_ready() == 1);
    assert(process_close_handle(&process, input_handle) == 0);
    assert(pros_input_service_shutdown() == PROS_DRIVER_OK);
    assert(pros_input_service_is_ready() == 0);
    assert(pros_input_service_read(&event) == PROS_DRIVER_ENODEV);
    assert(pros_input_service_poll_hardware() == PROS_DRIVER_ENODEV);
    assert(pros_input_service_grant_read(0, 0) == PROS_DRIVER_EINVAL);
    assert(pros_input_service_shutdown() == PROS_DRIVER_ENODEV);

    /* The same managed service can be initialized again after clean teardown. */
    assert(pros_input_service_init() == PROS_DRIVER_OK);
    assert(pros_input_service_is_ready() == 1);
    assert(pros_input_service_read_capability(&process, input_handle, &event) == PROS_DRIVER_EPERM);
    assert(pros_input_service_read(&event) == PROS_DRIVER_EBUSY);
    assert(pros_input_service_shutdown() == PROS_DRIVER_OK);
    assert(pros_input_service_is_ready() == 0);
    return 0;
}

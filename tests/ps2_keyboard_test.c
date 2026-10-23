#include <assert.h>
#include <stdint.h>
#include "ps2_keyboard.h"

/* Deterministic hosted stub for the kernel timer dependency. */
uint64_t timer_ticks(void) { return 1234u; }

struct capture {
    uint32_t count;
    struct pros_hw_input_event events[16];
};
static int capture_init(void *context)
{
    return context ? PROS_DRIVER_OK : PROS_DRIVER_EINVAL;
}
static int capture_transfer(void *context, const void *buffer, uint32_t length)
{
    struct capture *c = (struct capture *)context;
    if (!c || !buffer || length != sizeof(struct pros_hw_input_event))
        return PROS_DRIVER_EINVAL;
    if (c->count >= 16u) return PROS_DRIVER_ENOSPC;
    c->events[c->count++] = *(const struct pros_hw_input_event *)buffer;
    return PROS_DRIVER_OK;
}
static int capture_shutdown(void *context)
{
    return context ? PROS_DRIVER_OK : PROS_DRIVER_EINVAL;
}

int main(void)
{
    static const struct pros_hw_endpoint_ops ops = {
        capture_init, capture_transfer, capture_shutdown
    };
    struct pros_hw_input_event event;
    struct capture captured = {0};
    pros_ps2_keyboard_reset_decoder();

    assert(pros_ps2_keyboard_decode_byte(0x1Eu, &event) == PROS_DRIVER_OK);
    assert(event.version == PROS_HW_STACK_ABI_VERSION);
    assert(event.type == PROS_HW_INPUT_KEY);
    assert(event.flags == PROS_HW_INPUT_PRESS);
    assert(event.value0 == 0x1E);

    assert(pros_ps2_keyboard_decode_byte(0x9Eu, &event) == PROS_DRIVER_OK);
    assert(event.flags == PROS_HW_INPUT_RELEASE);
    assert(event.value0 == 0x1E);

    assert(pros_ps2_keyboard_decode_byte(0xE0u, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_keyboard_decode_byte(0x48u, &event) == PROS_DRIVER_OK);
    assert(event.value0 == 0x148);
    assert(event.flags == PROS_HW_INPUT_PRESS);

    assert(pros_ps2_keyboard_decode_byte(0xE0u, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_keyboard_decode_byte(0xC8u, &event) == PROS_DRIVER_OK);
    assert(event.value0 == 0x148);
    assert(event.flags == PROS_HW_INPUT_RELEASE);

    assert(pros_ps2_keyboard_decode_byte(0xE1u, &event) == PROS_DRIVER_ENOTSUP);
    for (uint32_t i = 0u; i < 5u; ++i)
        assert(pros_ps2_keyboard_decode_byte((uint8_t)i, &event) == PROS_DRIVER_ENOTSUP);

    assert(pros_ps2_keyboard_decode_byte(0xE0u, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_keyboard_decode_byte(0x2Au, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_keyboard_decode_byte(0x00u, &event) == PROS_DRIVER_EIO);
    assert(pros_ps2_keyboard_decode_byte(0x1Eu, 0) == PROS_DRIVER_EINVAL);
    assert(pros_ps2_keyboard_decode_byte(0xE0u, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_keyboard_decode_byte(0xFFu, &event) == PROS_DRIVER_EIO);
    assert(pros_ps2_keyboard_decode_byte(0x1Eu, &event) == PROS_DRIVER_OK);
    assert(event.value0 == 0x1E); /* malformed byte cleared the stale prefix */

    /* Exercise the actual polling and endpoint-delivery path with a fake FIFO. */
    pros_ps2_keyboard_reset_decoder();
    pros_ps2_keyboard_test_reset_fifo();
    assert(pros_ps2_keyboard_poll(0u) == PROS_DRIVER_EINVAL);
    assert(pros_hw_stack_init() == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == PROS_DRIVER_ENODEV);
    assert(pros_hw_endpoint_register(77u, PROS_HW_ENDPOINT_INPUT, &ops, &captured) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x1Eu) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == 1);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x9Eu) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == 1);
    assert(captured.count == 2u);
    assert(captured.events[0].value0 == 0x1E && captured.events[0].flags == PROS_HW_INPUT_PRESS);
    assert(captured.events[1].value0 == 0x1E && captured.events[1].flags == PROS_HW_INPUT_RELEASE);

    assert(pros_ps2_keyboard_test_push(0x01u, 0xE0u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x48u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == 1);
    assert(captured.count == 3u && captured.events[2].value0 == 0x148);

    /* An AUX byte between E0 and a key must clear the stale extended prefix. */
    assert(pros_ps2_keyboard_test_push(0x01u, 0xE0u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0x33u) == PROS_DRIVER_OK); /* mouse/AUX */
    assert(pros_ps2_keyboard_test_push(0x01u, 0x48u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == 1);
    assert(captured.count == 4u && captured.events[3].value0 == 0x48);

    assert(pros_ps2_keyboard_test_push(0x01u, 0xE0u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x81u, 0x33u) == PROS_DRIVER_OK); /* parity error */
    assert(pros_ps2_keyboard_test_push(0x01u, 0x1Eu) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == 1); /* clears partial prefix on parity error */
    assert(captured.count == 5u && captured.events[4].value0 == 0x1E);

    /* An AUX byte must not cancel the remainder of a Pause sequence. */
    assert(pros_ps2_keyboard_test_push(0x01u, 0xE1u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0x33u) == PROS_DRIVER_OK); /* mouse/AUX */
    assert(pros_ps2_keyboard_test_push(0x01u, 0x1Du) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x45u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0xE1u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x9Du) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0xC5u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x1Eu) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == 1);
    assert(captured.count == 6u && captured.events[5].value0 == 0x1E);

    /* Polling is bounded: a 33rd byte remains for the next invocation. */
    for (uint32_t i = 0u; i < 32u; ++i)
        assert(pros_ps2_keyboard_test_push(0x01u, 0xE0u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x01u, 0x1Eu) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == 0);
    assert(pros_ps2_keyboard_poll(77u) == 1);
    assert(captured.count == 7u && captured.events[6].value0 == 0x11E);

    /* Complete AUX packet must produce a pointer event through the same endpoint. */
    assert(pros_ps2_keyboard_test_push(0x21u, 0x08u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0x02u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0x01u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == 1);
    assert(captured.count == 8u);
    assert(captured.events[7].type == PROS_HW_INPUT_POINTER);
    assert(captured.events[7].flags == PROS_HW_INPUT_RELATIVE);
    assert(captured.events[7].value0 == 2 && captured.events[7].value1 == -1);
    assert(captured.events[7].timestamp == 1234u);

    /* AUX button edge is also routed, independently of movement. */
    assert(pros_ps2_keyboard_test_push(0x21u, 0x09u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0x00u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_test_push(0x21u, 0x00u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == 1);
    assert(captured.count == 9u);
    assert(captured.events[8].type == PROS_HW_INPUT_POINTER);
    assert(captured.events[8].flags == PROS_HW_INPUT_PRESS);
    assert(captured.events[8].value0 == 1);

    assert(pros_ps2_keyboard_test_push(0x01u, 0x00u) == PROS_DRIVER_OK);
    assert(pros_ps2_keyboard_poll(77u) == PROS_DRIVER_EIO);
    assert(pros_hw_endpoint_unregister(77u, PROS_HW_ENDPOINT_INPUT) == PROS_DRIVER_OK);
    return 0;
}

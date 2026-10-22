#include <assert.h>
#include <stdint.h>
#include "ps2_keyboard.h"

int main(void)
{
    struct pros_hw_input_event event;
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

    /* Hosted builds do not perform privileged port I/O. */
    pros_ps2_keyboard_reset_decoder();
    assert(pros_ps2_keyboard_poll(1u) == 0);
    return 0;
}

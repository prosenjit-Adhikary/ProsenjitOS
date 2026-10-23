#include <assert.h>
#include <stdint.h>
#include "ps2_mouse.h"

int main(void)
{
    struct pros_ps2_mouse_decoder decoder = {{0u, 0u, 0u}, 0u};
    struct pros_hw_input_event event;

    /* Bytes without the mandatory sync bit are discarded while seeking sync. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x00u, &event) == PROS_DRIVER_ENOTSUP);
    assert(decoder.packet_used == 0u);

    /* +10 X, +5 Y -> screen-relative (+10, -5). */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x08u, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 10u, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 5u, &event) == PROS_DRIVER_OK);
    assert(event.version == PROS_HW_STACK_ABI_VERSION);
    assert(event.type == PROS_HW_INPUT_POINTER);
    assert(event.flags == PROS_HW_INPUT_RELATIVE);
    assert(event.reserved == 0u);
    assert(event.value0 == 10 && event.value1 == -5);
    assert(decoder.packet_used == 0u);

    /* Negative X/Y require consistent header sign bits. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x38u, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0xFEu, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0xFDu, &event) == PROS_DRIVER_OK);
    assert(event.value0 == -2 && event.value1 == 3);

    /* Overflow packets are discarded and the next packet starts cleanly. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x48u, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 1u, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 1u, &event) == PROS_DRIVER_EIO);
    assert(decoder.packet_used == 0u);

    /* Bad sign metadata is rejected, not converted to bogus movement. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x08u, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0xFFu, &event) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, &event) == PROS_DRIVER_EIO);
    assert(decoder.packet_used == 0u);

    assert(pros_ps2_mouse_decode_byte(&decoder, 0x08u, 0) == PROS_DRIVER_EINVAL);
    pros_ps2_mouse_reset(&decoder);
    assert(decoder.packet_used == 0u);
    assert(pros_ps2_mouse_decode_byte(0, 0u, &event) == PROS_DRIVER_EINVAL);
    return 0;
}

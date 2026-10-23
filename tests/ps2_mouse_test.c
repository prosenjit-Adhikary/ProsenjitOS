#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "ps2_mouse.h"

int main(void)
{
    struct pros_ps2_mouse_decoder decoder = {{0u, 0u, 0u}, 0u, 0u};
    struct pros_hw_input_event events[PROS_PS2_MOUSE_MAX_EVENTS];
    uint32_t count = 0u;

    /* Bytes without the mandatory sync bit are discarded while seeking sync. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x00u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(decoder.packet_used == 0u);

    /* +10 X, +5 Y -> screen-relative (+10, -5). */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x08u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 10u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 5u, events, &count) == PROS_DRIVER_OK);
    assert(count == 1u);
    assert(events[0].version == PROS_HW_STACK_ABI_VERSION);
    assert(events[0].type == PROS_HW_INPUT_POINTER);
    assert(events[0].flags == PROS_HW_INPUT_RELATIVE);
    assert(events[0].reserved == 0u);
    assert(events[0].value0 == 10 && events[0].value1 == -5);
    assert(decoder.packet_used == 0u);

    /* Negative X/Y require consistent header sign bits. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x38u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0xFEu, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0xFDu, events, &count) == PROS_DRIVER_OK);
    assert(count == 1u && events[0].value0 == -2 && events[0].value1 == 3);

    /* Overflow packets are discarded and the next packet starts cleanly. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x48u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 1u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 1u, events, &count) == PROS_DRIVER_EIO);
    assert(decoder.packet_used == 0u);

    /* Bad sign metadata is rejected, not converted to bogus movement. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x08u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0xFFu, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_EIO);
    assert(decoder.packet_used == 0u);

    /* Left button press and release are independent edge events. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x09u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_OK);
    assert(count == 1u && events[0].flags == PROS_HW_INPUT_PRESS && events[0].value0 == 1);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x08u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_OK);
    assert(count == 1u && events[0].flags == PROS_HW_INPUT_RELEASE && events[0].value0 == 1);

    /* Corrupt/partial transport packets must not forget held buttons. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x09u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_OK);
    assert(count == 1u && events[0].flags == PROS_HW_INPUT_PRESS && events[0].value0 == 1);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x08u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_ENOTSUP);
    pros_ps2_mouse_discard_packet(&decoder);
    assert(decoder.packet_used == 0u && decoder.buttons == 1u);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x08u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_OK);
    assert(count == 1u && events[0].flags == PROS_HW_INPUT_RELEASE && events[0].value0 == 1);

    /* A malformed packet discards packet bytes but preserves last button state. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x09u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_OK);
    assert(count == 1u && events[0].flags == PROS_HW_INPUT_PRESS && events[0].value0 == 1);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x18u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_EIO);
    assert(decoder.buttons == 1u && decoder.packet_used == 0u);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x09u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_OK);
    assert(count == 0u); /* no duplicate press edge after resynchronization */

    /* Start a fresh simulated device before the independent batch-size case. */
    pros_ps2_mouse_reset(&decoder);

    /* Isolate this packet from the held-button recovery test above. */
    pros_ps2_mouse_reset(&decoder);

    /* Motion and two button transitions fit within the bounded event array. */
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x0Bu, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 1u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_OK);
    if (count != 3u) fprintf(stderr, "mouse batch diagnostic: count=%u buttons=%u packet_used=%u\n", count, decoder.buttons, decoder.packet_used);
    assert(count == 3u);
    assert(events[0].flags == PROS_HW_INPUT_RELATIVE && events[0].value0 == 1);
    assert(events[1].flags == PROS_HW_INPUT_PRESS && events[1].value0 == 1);
    assert(events[2].flags == PROS_HW_INPUT_PRESS && events[2].value0 == 2);

    /* Worst-case standard packet: motion plus all three button edges (4 events). */
    pros_ps2_mouse_reset(&decoder);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x0Fu, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 1u, events, &count) == PROS_DRIVER_ENOTSUP);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0u, events, &count) == PROS_DRIVER_OK);
    assert(count == PROS_PS2_MOUSE_MAX_EVENTS);
    assert(events[0].flags == PROS_HW_INPUT_RELATIVE && events[0].value0 == 1);
    assert(events[1].flags == PROS_HW_INPUT_PRESS && events[1].value0 == 1);
    assert(events[2].flags == PROS_HW_INPUT_PRESS && events[2].value0 == 2);
    assert(events[3].flags == PROS_HW_INPUT_PRESS && events[3].value0 == 3);

    assert(pros_ps2_mouse_decode_byte(&decoder, 0x08u, 0, &count) == PROS_DRIVER_EINVAL);
    assert(pros_ps2_mouse_decode_byte(&decoder, 0x08u, events, 0) == PROS_DRIVER_EINVAL);
    pros_ps2_mouse_reset(&decoder);
    assert(decoder.packet_used == 0u && decoder.buttons == 0u);
    assert(pros_ps2_mouse_decode_byte(0, 0u, events, &count) == PROS_DRIVER_EINVAL);
    return 0;
}

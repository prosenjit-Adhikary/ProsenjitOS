#ifndef PROSENJIT_PS2_MOUSE_H
#define PROSENJIT_PS2_MOUSE_H

#include <stdint.h>
#include "hw_stack.h"

/*
 * Pure decoder for the standard three-byte PS/2 mouse packet format.
 * This does not enable the controller, route IRQs, or claim physical-device
 * support. Feed bytes from a separately validated polling/IRQ transport.
 */
#define PROS_PS2_MOUSE_PACKET_BYTES 3u

struct pros_ps2_mouse_decoder {
    uint8_t packet[PROS_PS2_MOUSE_PACKET_BYTES];
    uint8_t packet_used;
};

/* Reset partial packet state after transport errors or device reset. */
void pros_ps2_mouse_reset(struct pros_ps2_mouse_decoder *decoder);

/*
 * Consume one raw PS/2 mouse byte.
 * Returns PROS_DRIVER_ENOTSUP while waiting for a complete packet,
 * PROS_DRIVER_OK with a relative pointer event for a complete valid packet,
 * or a negative PROS_DRIVER_* error for invalid arguments/transport data.
 */
int pros_ps2_mouse_decode_byte(struct pros_ps2_mouse_decoder *decoder,
                               uint8_t byte,
                               struct pros_hw_input_event *out);

#endif

#ifndef PROSENJIT_PS2_MOUSE_H
#define PROSENJIT_PS2_MOUSE_H

#include <stdint.h>
#include "hw_stack.h"

#define PROS_PS2_MOUSE_PACKET_BYTES 3u
#define PROS_PS2_MOUSE_MAX_EVENTS 4u

/*
 * Pure decoder for standard three-byte PS/2 mouse packets.
 * This does not enable the controller, route IRQs, or claim physical-device
 * support. Feed bytes from a separately validated polling/IRQ transport.
 */
struct pros_ps2_mouse_decoder {
    uint8_t packet[PROS_PS2_MOUSE_PACKET_BYTES];
    uint8_t packet_used;
    uint8_t buttons;
};

/* Reset packet and button state when starting a fresh decoder/device. */
void pros_ps2_mouse_reset(struct pros_ps2_mouse_decoder *decoder);

/* Discard a partial/corrupted packet without losing last known button state. */
void pros_ps2_mouse_discard_packet(struct pros_ps2_mouse_decoder *decoder);

/* Recover after transport reset/disconnect: discard partial packet state and
 * emit RELEASE edges for buttons that were held. Does not perform controller
 * I/O or claim that a physical disconnect was detected. */
int pros_ps2_mouse_recover(struct pros_ps2_mouse_decoder *decoder,
                           struct pros_hw_input_event out[3],
                           uint32_t *out_count);

/*
 * Consume one raw byte. Returns ENOTSUP while awaiting a complete packet.
 * On a complete valid packet, returns OK and sets *out_count to 0..4 events:
 * relative movement plus any left/right/middle button state transitions.
 * Button event value0 is 1=left, 2=right, 3=middle; value1 is zero.
 */
int pros_ps2_mouse_decode_byte(struct pros_ps2_mouse_decoder *decoder,
                               uint8_t byte,
                               struct pros_hw_input_event out[PROS_PS2_MOUSE_MAX_EVENTS],
                               uint32_t *out_count);

#endif

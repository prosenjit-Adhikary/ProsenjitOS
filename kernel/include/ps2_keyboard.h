#ifndef PROSENJIT_PS2_KEYBOARD_H
#define PROSENJIT_PS2_KEYBOARD_H

#include <stdint.h>
#include "hw_stack.h"

/*
 * Legacy PS/2 set-1 scancode polling backend.
 * Decoded value0 is the set-1 scancode; bit 8 marks an E0-prefixed key.
 * This is a polling source, not an IRQ-driven or USB HID implementation.
 */
int pros_ps2_keyboard_decode_byte(uint8_t byte, struct pros_hw_input_event *out);
void pros_ps2_keyboard_reset_decoder(void);

/* Publish synthetic mouse-button releases after an explicit transport reset or
 * disconnect, then clear decoder state. If delivery fails, retain decoder state
 * so a caller can retry. This does not probe or reset the physical controller. */
int pros_ps2_keyboard_recover(uint32_t input_endpoint_id);

/* Poll up to 32 controller bytes and submit decoded events to an input endpoint.
 * Returns the number of events submitted, zero if no event is ready, or a
 * negative PROS_DRIVER_* error. Call from a safe kernel polling context.
 */
int pros_ps2_keyboard_poll(uint32_t input_endpoint_id);

#if __STDC_HOSTED__
/* Hosted-only controller FIFO injection used by deterministic unit tests. */
int pros_ps2_keyboard_test_push(uint8_t status, uint8_t byte);
void pros_ps2_keyboard_test_reset_fifo(void);
#endif

#endif

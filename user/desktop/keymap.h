#ifndef PROSENJIT_USER_KEYMAP_H
#define PROSENJIT_USER_KEYMAP_H

#include <stdint.h>
#include "../../include/abi/input.h"

/* Stateful US-QWERTY translator for raw PS/2 set-1 scan-code events.
 * value0 stores the scan code in bits 0..7 and E0-extension in bit 8.
 * Returns an ASCII character for printable key presses, or 0 otherwise.
 * Call from a single input-service thread; this state is not internally locked. */
struct pros_keymap_state {
    uint8_t left_shift;
    uint8_t right_shift;
    uint8_t caps_lock;
    uint8_t control;
    uint8_t alt;
};

void pros_keymap_reset(struct pros_keymap_state *state);
uint32_t pros_keymap_translate(struct pros_keymap_state *state,
                               const struct pros_input_event *event);

#endif

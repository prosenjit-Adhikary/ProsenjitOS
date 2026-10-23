#ifndef PROSENJIT_USB_HID_H
#define PROSENJIT_USB_HID_H

#include <stdint.h>
#include "hw_stack.h"

/* Pure boot-protocol report decoders; no USB controller/transport implied. */
#define PROS_USB_HID_KEYBOARD_REPORT_BYTES 8u
#define PROS_USB_HID_MOUSE_REPORT_BYTES 3u
#define PROS_USB_HID_KEYBOARD_MAX_EVENTS 20u
#define PROS_USB_HID_MOUSE_MAX_EVENTS 4u

struct pros_usb_hid_keyboard_state {
    uint8_t modifiers;
    uint8_t keys[6];
};

struct pros_usb_hid_mouse_state {
    uint8_t buttons;
};

void pros_usb_hid_keyboard_reset(struct pros_usb_hid_keyboard_state *state);
void pros_usb_hid_mouse_reset(struct pros_usb_hid_mouse_state *state);

/* Disconnect/error recovery: synthesize releases for held controls, then
 * clear decoder state. Output arrays must have the capacities below. */
int pros_usb_hid_keyboard_recover(
    struct pros_usb_hid_keyboard_state *state,
    struct pros_hw_input_event out[PROS_USB_HID_KEYBOARD_MAX_EVENTS],
    uint32_t *out_count);
int pros_usb_hid_mouse_recover(
    struct pros_usb_hid_mouse_state *state,
    struct pros_hw_input_event out[PROS_USB_HID_MOUSE_MAX_EVENTS],
    uint32_t *out_count);

/* Decode and atomically publish a USB HID report to the kernel input queue.
 * State is rolled back if the queue rejects the complete event batch. */
int pros_usb_hid_keyboard_submit_report(
    uint32_t endpoint_id, struct pros_usb_hid_keyboard_state *state,
    const uint8_t report[PROS_USB_HID_KEYBOARD_REPORT_BYTES]);
int pros_usb_hid_mouse_submit_report(
    uint32_t endpoint_id, struct pros_usb_hid_mouse_state *state,
    const uint8_t report[PROS_USB_HID_MOUSE_REPORT_BYTES]);
int pros_usb_hid_keyboard_submit_recovery(
    uint32_t endpoint_id, struct pros_usb_hid_keyboard_state *state);
int pros_usb_hid_mouse_submit_recovery(
    uint32_t endpoint_id, struct pros_usb_hid_mouse_state *state);

/*
 * Boot keyboard report: modifier bitmap, reserved byte, six key usages.
 * Emits usage-code press/release events (modifier usages E0..E7 included).
 * Rollover/error usages 1..3 are rejected without changing previous state.
 */
int pros_usb_hid_keyboard_report(
    struct pros_usb_hid_keyboard_state *state,
    const uint8_t report[PROS_USB_HID_KEYBOARD_REPORT_BYTES],
    struct pros_hw_input_event out[PROS_USB_HID_KEYBOARD_MAX_EVENTS],
    uint32_t *out_count);

/* Boot mouse report: buttons bitmap and signed 8-bit X/Y relative motion. */
int pros_usb_hid_mouse_report(
    struct pros_usb_hid_mouse_state *state,
    const uint8_t report[PROS_USB_HID_MOUSE_REPORT_BYTES],
    struct pros_hw_input_event out[PROS_USB_HID_MOUSE_MAX_EVENTS],
    uint32_t *out_count);

#endif

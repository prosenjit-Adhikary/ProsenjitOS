#include <assert.h>
#include <stdint.h>
#include "usb_hid.h"
#include "input_service.h"


static struct pros_hw_input_event queued[PROS_INPUT_SERVICE_QUEUE_CAPACITY];
static uint32_t queued_count;
static int queue_result;

uint64_t timer_ticks(void) { return 5678u; }

int pros_input_service_send_batch(uint32_t endpoint_id,
                                  const struct pros_hw_input_event *events,
                                  uint32_t count)
{
    if (endpoint_id != PROS_INPUT_SERVICE_ENDPOINT_ID || !events || count == 0u)
        return PROS_DRIVER_EINVAL;
    if (queue_result != PROS_DRIVER_OK) return queue_result;
    assert(count <= PROS_INPUT_SERVICE_QUEUE_CAPACITY);
    for (uint32_t i = 0u; i < count; ++i) queued[i] = events[i];
    queued_count = count;
    return PROS_DRIVER_OK;
}

int main(void)
{
    struct pros_usb_hid_keyboard_state keyboard = {0u, {0u,0u,0u,0u,0u,0u}};
    struct pros_usb_hid_mouse_state mouse = {0u};
    struct pros_hw_input_event events[PROS_USB_HID_KEYBOARD_MAX_EVENTS];
    uint32_t count = 0u;
    uint8_t report[8] = {0u,0u,4u,0u,0u,0u,0u,0u};

    assert(pros_usb_hid_keyboard_report(&keyboard, report, events, &count) == PROS_DRIVER_OK);
    assert(count == 1u && events[0].type == PROS_HW_INPUT_KEY);
    assert(events[0].flags == PROS_HW_INPUT_PRESS && events[0].value0 == 4);
    assert(pros_usb_hid_keyboard_report(&keyboard, report, events, &count) == PROS_DRIVER_OK);
    assert(count == 0u); /* held key does not repeat on every unchanged report */

    report[0] = 0x02u; /* left shift modifier */
    report[2] = 5u;    /* release usage 4 and press usage 5 */
    assert(pros_usb_hid_keyboard_report(&keyboard, report, events, &count) == PROS_DRIVER_OK);
    assert(count == 3u);
    assert(events[0].value0 == 0xE1 && events[0].flags == PROS_HW_INPUT_PRESS);
    assert(events[1].value0 == 4 && events[1].flags == PROS_HW_INPUT_RELEASE);
    assert(events[2].value0 == 5 && events[2].flags == PROS_HW_INPUT_PRESS);

    uint8_t saved_key = keyboard.keys[0];
    report[2] = 1u; /* boot-protocol rollover/error usage */
    assert(pros_usb_hid_keyboard_report(&keyboard, report, events, &count) == PROS_DRIVER_EIO);
    assert(keyboard.keys[0] == saved_key);
    report[2] = 5u;
    report[3] = 5u; /* duplicate usage */
    assert(pros_usb_hid_keyboard_report(&keyboard, report, events, &count) == PROS_DRIVER_EINVAL);
    report[3] = 0u;

    /* A nonzero boot-protocol reserved byte is malformed and must not change
     * the tracked modifiers/keys or emit any partial transition. */
    uint8_t saved_modifiers = keyboard.modifiers;
    saved_key = keyboard.keys[0];
    report[1] = 1u;
    assert(pros_usb_hid_keyboard_report(&keyboard, report, events, &count) == PROS_DRIVER_EINVAL);
    assert(count == 0u);
    assert(keyboard.modifiers == saved_modifiers && keyboard.keys[0] == saved_key);
    report[1] = 0u;

    uint8_t mouse_report[3] = {0u, 10u, 5u};
    assert(pros_usb_hid_mouse_report(&mouse, mouse_report, events, &count) == PROS_DRIVER_OK);
    assert(count == 1u && events[0].flags == PROS_HW_INPUT_RELATIVE);
    assert(events[0].value0 == 10 && events[0].value1 == -5);
    mouse_report[0] = 3u; mouse_report[1] = 0u; mouse_report[2] = 0u;
    assert(pros_usb_hid_mouse_report(&mouse, mouse_report, events, &count) == PROS_DRIVER_OK);
    assert(count == 2u);
    assert(events[0].flags == PROS_HW_INPUT_PRESS && events[0].value0 == 1);
    assert(events[1].flags == PROS_HW_INPUT_PRESS && events[1].value0 == 2);
    mouse_report[0] = 0u;
    assert(pros_usb_hid_mouse_report(&mouse, mouse_report, events, &count) == PROS_DRIVER_OK);
    assert(count == 2u);
    assert(events[0].flags == PROS_HW_INPUT_RELEASE && events[0].value0 == 1);
    assert(events[1].flags == PROS_HW_INPUT_RELEASE && events[1].value0 == 2);

    /* Reserved boot-mouse button bits must not be silently ignored. */
    uint8_t saved_buttons = mouse.buttons;
    mouse_report[0] = 0x08u;
    assert(pros_usb_hid_mouse_report(&mouse, mouse_report, events, &count) == PROS_DRIVER_EINVAL);
    assert(count == 0u && mouse.buttons == saved_buttons);
    mouse_report[0] = 0u;

    assert(pros_usb_hid_keyboard_report(&keyboard, report, 0, &count) == PROS_DRIVER_EINVAL);
    assert(pros_usb_hid_keyboard_report(&keyboard, report, events, 0) == PROS_DRIVER_EINVAL);
    assert(pros_usb_hid_mouse_report(&mouse, mouse_report, 0, &count) == PROS_DRIVER_EINVAL);

    /* Disconnect recovery must synthesize releases before clearing state. */
    assert(pros_usb_hid_keyboard_recover(&keyboard, events, &count) == PROS_DRIVER_OK);
    assert(count == 2u);
    assert(events[0].type == PROS_HW_INPUT_KEY &&
           events[0].flags == PROS_HW_INPUT_RELEASE && events[0].value0 == 0xE1);
    assert(events[1].type == PROS_HW_INPUT_KEY &&
           events[1].flags == PROS_HW_INPUT_RELEASE && events[1].value0 == 5);
    assert(keyboard.modifiers == 0u && keyboard.keys[0] == 0u);
    assert(pros_usb_hid_keyboard_recover(&keyboard, events, &count) == PROS_DRIVER_OK);
    assert(count == 0u);
    assert(pros_usb_hid_keyboard_recover(0, events, &count) == PROS_DRIVER_EINVAL);

    mouse_report[0] = 5u; mouse_report[1] = 0u; mouse_report[2] = 0u;
    assert(pros_usb_hid_mouse_report(&mouse, mouse_report, events, &count) == PROS_DRIVER_OK);
    assert(count == 2u);
    assert(pros_usb_hid_mouse_recover(&mouse, events, &count) == PROS_DRIVER_OK);
    assert(count == 2u);
    assert(events[0].type == PROS_HW_INPUT_POINTER &&
           events[0].flags == PROS_HW_INPUT_RELEASE && events[0].value0 == 1);
    assert(events[1].type == PROS_HW_INPUT_POINTER &&
           events[1].flags == PROS_HW_INPUT_RELEASE && events[1].value0 == 3);
    assert(mouse.buttons == 0u);
    assert(pros_usb_hid_mouse_recover(&mouse, events, &count) == PROS_DRIVER_OK);
    assert(count == 0u);
    assert(pros_usb_hid_mouse_recover(0, events, &count) == PROS_DRIVER_EINVAL);

    pros_usb_hid_keyboard_reset(&keyboard);
    pros_usb_hid_mouse_reset(&mouse);

    /* Queue bridge: successful report delivery timestamps events; queue
     * failure rolls decoder state back so the caller may retry the report. */
    queued_count = 0u; queue_result = PROS_DRIVER_OK;
    report[0] = 0u; report[2] = 4u;
    assert(pros_usb_hid_keyboard_submit_report(
               PROS_INPUT_SERVICE_ENDPOINT_ID, &keyboard, report) == PROS_DRIVER_OK);
    assert(queued_count == 1u && queued[0].value0 == 4);
    assert(queued[0].flags == PROS_HW_INPUT_PRESS && queued[0].timestamp == 5678u);
    queue_result = PROS_DRIVER_ENOSPC;
    report[2] = 5u;
    assert(pros_usb_hid_keyboard_submit_report(
               PROS_INPUT_SERVICE_ENDPOINT_ID, &keyboard, report) == PROS_DRIVER_ENOSPC);
    assert(keyboard.keys[0] == 4u); /* previous report state preserved */
    assert(pros_usb_hid_keyboard_submit_recovery(
               PROS_INPUT_SERVICE_ENDPOINT_ID, &keyboard) == PROS_DRIVER_ENOSPC);
    assert(keyboard.keys[0] == 4u); /* held key retained until release is queued */
    queue_result = PROS_DRIVER_OK;
    assert(pros_usb_hid_keyboard_submit_recovery(
               PROS_INPUT_SERVICE_ENDPOINT_ID, &keyboard) == PROS_DRIVER_OK);
    assert(queued_count == 1u && queued[0].flags == PROS_HW_INPUT_RELEASE);
    assert(queued[0].value0 == 4 && keyboard.keys[0] == 0u);
    assert(pros_usb_hid_keyboard_submit_report(
               0u, &keyboard, report) == PROS_DRIVER_EINVAL);

    mouse_report[0] = 1u; mouse_report[1] = 0u; mouse_report[2] = 0u;
    assert(pros_usb_hid_mouse_submit_report(
               PROS_INPUT_SERVICE_ENDPOINT_ID, &mouse, mouse_report) == PROS_DRIVER_OK);
    assert(queued_count == 1u && queued[0].type == PROS_HW_INPUT_POINTER);
    assert(queued[0].flags == PROS_HW_INPUT_PRESS && queued[0].value0 == 1);
    queue_result = PROS_DRIVER_ENOSPC;
    mouse_report[0] = 3u; /* second button transition must be retryable */
    assert(pros_usb_hid_mouse_submit_report(
               PROS_INPUT_SERVICE_ENDPOINT_ID, &mouse, mouse_report) == PROS_DRIVER_ENOSPC);
    assert(mouse.buttons == 1u); /* rejected button edge was not committed */
    assert(pros_usb_hid_mouse_submit_recovery(
               PROS_INPUT_SERVICE_ENDPOINT_ID, &mouse) == PROS_DRIVER_ENOSPC);
    assert(mouse.buttons == 1u); /* release not lost when queue is full */
    queue_result = PROS_DRIVER_OK;
    assert(pros_usb_hid_mouse_submit_recovery(
               PROS_INPUT_SERVICE_ENDPOINT_ID, &mouse) == PROS_DRIVER_OK);
    assert(queued_count == 1u && queued[0].flags == PROS_HW_INPUT_RELEASE);
    assert(queued[0].value0 == 1 && mouse.buttons == 0u);

    pros_usb_hid_keyboard_reset(&keyboard);
    pros_usb_hid_mouse_reset(&mouse);
    assert(keyboard.modifiers == 0u && mouse.buttons == 0u);
    return 0;
}

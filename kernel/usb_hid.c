#include "usb_hid.h"

static void input_event(struct pros_hw_input_event *event, uint32_t type,
                        uint32_t flags, int32_t value0, int32_t value1)
{
    event->version = PROS_HW_STACK_ABI_VERSION;
    event->type = type;
    event->flags = flags;
    event->reserved = 0u;
    event->timestamp = 0u;
    event->value0 = value0;
    event->value1 = value1;
}

void pros_usb_hid_keyboard_reset(struct pros_usb_hid_keyboard_state *state)
{
    if (!state) return;
    state->modifiers = 0u;
    for (uint32_t i = 0u; i < 6u; ++i) state->keys[i] = 0u;
}

void pros_usb_hid_mouse_reset(struct pros_usb_hid_mouse_state *state)
{
    if (state) state->buttons = 0u;
}

static int key_present(const uint8_t keys[6], uint8_t usage)
{
    for (uint32_t i = 0u; i < 6u; ++i)
        if (keys[i] == usage) return 1;
    return 0;
}

int pros_usb_hid_keyboard_report(
    struct pros_usb_hid_keyboard_state *state,
    const uint8_t report[PROS_USB_HID_KEYBOARD_REPORT_BYTES],
    struct pros_hw_input_event out[PROS_USB_HID_KEYBOARD_MAX_EVENTS],
    uint32_t *out_count)
{
    if (!state || !report || !out || !out_count) return PROS_DRIVER_EINVAL;
    *out_count = 0u;

    /* Boot-protocol byte 1 is reserved and must remain zero. Reject malformed
     * reports before deriving transitions or mutating the previous key state. */
    if (report[1] != 0u) return PROS_DRIVER_EINVAL;

    uint8_t next_keys[6];
    for (uint32_t i = 0u; i < 6u; ++i) {
        uint8_t usage = report[2u + i];
        if (usage >= 1u && usage <= 3u) return PROS_DRIVER_EIO;
        if (usage != 0u) {
            for (uint32_t j = 0u; j < i; ++j)
                if (next_keys[j] == usage) return PROS_DRIVER_EINVAL;
        }
        next_keys[i] = usage;
    }

    /* Modifier usages are E0..E7; transitions are reported independently. */
    uint8_t modifier_changes = (uint8_t)(state->modifiers ^ report[0]);
    for (uint32_t bit = 0u; bit < 8u; ++bit) {
        uint8_t mask = (uint8_t)(1u << bit);
        if ((modifier_changes & mask) == 0u) continue;
        input_event(&out[*out_count], PROS_HW_INPUT_KEY,
                    (report[0] & mask) ? PROS_HW_INPUT_PRESS : PROS_HW_INPUT_RELEASE,
                    (int32_t)(0xE0u + bit), 0);
        ++*out_count;
    }

    for (uint32_t i = 0u; i < 6u; ++i) {
        uint8_t usage = state->keys[i];
        if (usage != 0u && !key_present(next_keys, usage)) {
            input_event(&out[*out_count], PROS_HW_INPUT_KEY,
                        PROS_HW_INPUT_RELEASE, usage, 0);
            ++*out_count;
        }
    }
    for (uint32_t i = 0u; i < 6u; ++i) {
        uint8_t usage = next_keys[i];
        if (usage != 0u && !key_present(state->keys, usage)) {
            input_event(&out[*out_count], PROS_HW_INPUT_KEY,
                        PROS_HW_INPUT_PRESS, usage, 0);
            ++*out_count;
        }
    }

    state->modifiers = report[0];
    for (uint32_t i = 0u; i < 6u; ++i) state->keys[i] = next_keys[i];
    return PROS_DRIVER_OK;
}

int pros_usb_hid_mouse_report(
    struct pros_usb_hid_mouse_state *state,
    const uint8_t report[PROS_USB_HID_MOUSE_REPORT_BYTES],
    struct pros_hw_input_event out[PROS_USB_HID_MOUSE_MAX_EVENTS],
    uint32_t *out_count)
{
    if (!state || !report || !out || !out_count) return PROS_DRIVER_EINVAL;
    *out_count = 0u;
    uint8_t buttons = (uint8_t)(report[0] & 0x07u);
    int32_t dx = (int32_t)(int8_t)report[1];
    int32_t dy = -(int32_t)(int8_t)report[2];

    if (dx != 0 || dy != 0) {
        input_event(&out[*out_count], PROS_HW_INPUT_POINTER,
                    PROS_HW_INPUT_RELATIVE, dx, dy);
        ++*out_count;
    }

    uint8_t changed = (uint8_t)(state->buttons ^ buttons);
    for (uint32_t bit = 0u; bit < 3u; ++bit) {
        uint8_t mask = (uint8_t)(1u << bit);
        if ((changed & mask) == 0u) continue;
        input_event(&out[*out_count], PROS_HW_INPUT_POINTER,
                    (buttons & mask) ? PROS_HW_INPUT_PRESS : PROS_HW_INPUT_RELEASE,
                    (int32_t)(bit + 1u), 0);
        ++*out_count;
    }
    state->buttons = buttons;
    return PROS_DRIVER_OK;
}

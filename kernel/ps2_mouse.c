#include "ps2_mouse.h"

void pros_ps2_mouse_reset(struct pros_ps2_mouse_decoder *decoder)
{
    if (!decoder) return;
    decoder->packet[0] = 0u;
    decoder->packet[1] = 0u;
    decoder->packet[2] = 0u;
    decoder->packet_used = 0u;
    decoder->buttons = 0u;
}

static void mouse_event_init(struct pros_hw_input_event *event,
                             uint32_t flags, int32_t value0, int32_t value1)
{
    event->version = PROS_HW_STACK_ABI_VERSION;
    event->type = PROS_HW_INPUT_POINTER;
    event->flags = flags;
    event->reserved = 0u;
    event->timestamp = 0u;
    event->value0 = value0;
    event->value1 = value1;
}

int pros_ps2_mouse_decode_byte(struct pros_ps2_mouse_decoder *decoder,
                               uint8_t byte,
                               struct pros_hw_input_event out[PROS_PS2_MOUSE_MAX_EVENTS],
                               uint32_t *out_count)
{
    if (!decoder || !out || !out_count) return PROS_DRIVER_EINVAL;
    *out_count = 0u;
    if (decoder->packet_used >= PROS_PS2_MOUSE_PACKET_BYTES) {
        pros_ps2_mouse_reset(decoder);
        return PROS_DRIVER_EIO;
    }

    /* Bit 3 is always set in the first byte; use it to recover packet sync. */
    if (decoder->packet_used == 0u && (byte & 0x08u) == 0u)
        return PROS_DRIVER_ENOTSUP;

    decoder->packet[decoder->packet_used++] = byte;
    if (decoder->packet_used < PROS_PS2_MOUSE_PACKET_BYTES)
        return PROS_DRIVER_ENOTSUP;

    const uint8_t flags = decoder->packet[0];
    const uint8_t raw_x = decoder->packet[1];
    const uint8_t raw_y = decoder->packet[2];
    pros_ps2_mouse_reset(decoder);

    /* Overflow means the device lost motion information; never fabricate it. */
    if ((flags & 0xC0u) != 0u)
        return PROS_DRIVER_EIO;

    /* Reject inconsistent sign-extension metadata rather than misdecode. */
    if ((((flags & 0x10u) != 0u) != ((raw_x & 0x80u) != 0u)) ||
        (((flags & 0x20u) != 0u) != ((raw_y & 0x80u) != 0u)))
        return PROS_DRIVER_EIO;

    int32_t dx = (int32_t)(int8_t)raw_x;
    int32_t dy = -(int32_t)(int8_t)raw_y;
    if (dx != 0 || dy != 0) {
        mouse_event_init(&out[*out_count], PROS_HW_INPUT_RELATIVE, dx, dy);
        ++*out_count;
    }

    uint8_t new_buttons = (uint8_t)(flags & 0x07u);
    uint8_t changed = (uint8_t)(decoder->buttons ^ new_buttons);
    for (uint32_t button = 0u; button < 3u; ++button) {
        uint8_t mask = (uint8_t)(1u << button);
        if ((changed & mask) == 0u) continue;
        uint32_t edge = (new_buttons & mask) != 0u
            ? PROS_HW_INPUT_PRESS : PROS_HW_INPUT_RELEASE;
        mouse_event_init(&out[*out_count], edge, (int32_t)(button + 1u), 0);
        ++*out_count;
    }
    decoder->buttons = new_buttons;
    return PROS_DRIVER_OK;
}

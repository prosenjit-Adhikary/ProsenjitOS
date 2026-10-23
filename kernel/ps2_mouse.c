#include "ps2_mouse.h"

void pros_ps2_mouse_reset(struct pros_ps2_mouse_decoder *decoder)
{
    if (!decoder) return;
    decoder->packet[0] = 0u;
    decoder->packet[1] = 0u;
    decoder->packet[2] = 0u;
    decoder->packet_used = 0u;
}

int pros_ps2_mouse_decode_byte(struct pros_ps2_mouse_decoder *decoder,
                               uint8_t byte,
                               struct pros_hw_input_event *out)
{
    if (!decoder || !out) return PROS_DRIVER_EINVAL;
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

    out->version = PROS_HW_STACK_ABI_VERSION;
    out->type = PROS_HW_INPUT_POINTER;
    out->flags = PROS_HW_INPUT_RELATIVE;
    out->reserved = 0u;
    out->timestamp = 0u;
    out->value0 = (int32_t)(int8_t)raw_x;
    /* PS/2 positive Y is upward; invert it for screen-coordinate movement. */
    out->value1 = -(int32_t)(int8_t)raw_y;
    return PROS_DRIVER_OK;
}

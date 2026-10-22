#include "ps2_keyboard.h"

#define PS2_STATUS_PORT 0x64u
#define PS2_DATA_PORT   0x60u
#define PS2_STATUS_OUTPUT_FULL 0x01u
#define PS2_STATUS_AUX_DATA    0x20u
#define PS2_STATUS_TIMEOUT     0x40u
#define PS2_STATUS_PARITY      0x80u
#define PS2_POLL_BUDGET        32u

static uint8_t extended_prefix;
static uint8_t pause_bytes_remaining;

static uint8_t ps2_in8(uint16_t port)
{
#if __STDC_HOSTED__
    (void)port;
    return 0u;
#else
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
#endif
}

void pros_ps2_keyboard_reset_decoder(void)
{
    extended_prefix = 0u;
    pause_bytes_remaining = 0u;
}

int pros_ps2_keyboard_decode_byte(uint8_t byte, struct pros_hw_input_event *out)
{
    uint8_t extended;
    uint8_t released;
    if (!out) return PROS_DRIVER_EINVAL;

    if (pause_bytes_remaining != 0u) {
        --pause_bytes_remaining;
        return PROS_DRIVER_ENOTSUP;
    }
    if (byte == 0xE1u) {
        extended_prefix = 0u;
        pause_bytes_remaining = 5u;
        return PROS_DRIVER_ENOTSUP;
    }
    if (byte == 0xE0u) {
        extended_prefix = 1u;
        return PROS_DRIVER_ENOTSUP;
    }
    if (byte == 0x00u || byte == 0xFFu)
        return PROS_DRIVER_EIO;

    extended = extended_prefix;
    extended_prefix = 0u;
    released = (byte & 0x80u) != 0u;
    byte &= 0x7Fu;

    /* Ignore the fake shift transitions embedded in Print Screen sequences. */
    if (extended && (byte == 0x2Au || byte == 0x36u))
        return PROS_DRIVER_ENOTSUP;

    out->version = PROS_HW_STACK_ABI_VERSION;
    out->type = PROS_HW_INPUT_KEY;
    out->flags = released ? PROS_HW_INPUT_RELEASE : PROS_HW_INPUT_PRESS;
    out->reserved = 0u;
    out->timestamp = 0u;
    out->value0 = (int32_t)byte | (extended ? 0x100 : 0);
    out->value1 = 0;
    return PROS_DRIVER_OK;
}

int pros_ps2_keyboard_poll(uint32_t input_endpoint_id)
{
    uint32_t bytes_seen = 0u;
    int submitted = 0;
    while (bytes_seen < PS2_POLL_BUDGET) {
        uint8_t status = ps2_in8(PS2_STATUS_PORT);
        if ((status & PS2_STATUS_OUTPUT_FULL) == 0u)
            break;

        uint8_t byte = ps2_in8(PS2_DATA_PORT);
        ++bytes_seen;
        /* Drop controller parity/timeout errors and auxiliary (mouse) bytes. */
        if ((status & (PS2_STATUS_TIMEOUT | PS2_STATUS_PARITY | PS2_STATUS_AUX_DATA)) != 0u)
            continue;

        struct pros_hw_input_event event;
        int rc = pros_ps2_keyboard_decode_byte(byte, &event);
        if (rc == PROS_DRIVER_ENOTSUP)
            continue;
        if (rc != PROS_DRIVER_OK)
            return rc;
        rc = pros_hw_input_send(input_endpoint_id, &event);
        if (rc != PROS_DRIVER_OK)
            return rc;
        ++submitted;
    }
    return submitted;
}

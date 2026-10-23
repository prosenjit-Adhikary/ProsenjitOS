#include "ps2_keyboard.h"
#include "ps2_mouse.h"
#include "input_service.h"
#include "timer.h"

#define PS2_STATUS_PORT 0x64u
#define PS2_DATA_PORT   0x60u
#define PS2_STATUS_OUTPUT_FULL 0x01u
#define PS2_STATUS_AUX_DATA    0x20u
#define PS2_STATUS_TIMEOUT     0x40u
#define PS2_STATUS_PARITY      0x80u
#define PS2_POLL_BUDGET        32u

static uint8_t extended_prefix;
static uint8_t pause_bytes_remaining;
static struct pros_ps2_mouse_decoder mouse_decoder;

#if __STDC_HOSTED__
#define PS2_TEST_FIFO_CAPACITY 64u
static uint8_t test_status[PS2_TEST_FIFO_CAPACITY];
static uint8_t test_data[PS2_TEST_FIFO_CAPACITY];
static uint32_t test_head;
static uint32_t test_tail;

int pros_ps2_keyboard_test_push(uint8_t status, uint8_t byte)
{
    if ((uint32_t)(test_tail - test_head) >= PS2_TEST_FIFO_CAPACITY)
        return PROS_DRIVER_ENOSPC;
    test_status[test_tail % PS2_TEST_FIFO_CAPACITY] = status;
    test_data[test_tail % PS2_TEST_FIFO_CAPACITY] = byte;
    ++test_tail;
    return PROS_DRIVER_OK;
}

void pros_ps2_keyboard_test_reset_fifo(void)
{
    test_head = 0u;
    test_tail = 0u;
}

static uint8_t ps2_in8(uint16_t port)
{
    if (port == PS2_STATUS_PORT) {
        if (test_head == test_tail) return 0u;
        return test_status[test_head % PS2_TEST_FIFO_CAPACITY];
    }
    if (port == PS2_DATA_PORT) {
        if (test_head == test_tail) return 0u;
        return test_data[test_head++ % PS2_TEST_FIFO_CAPACITY];
    }
    return 0u;
}
#else
static uint8_t ps2_in8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
#endif

void pros_ps2_keyboard_reset_decoder(void)
{
    extended_prefix = 0u;
    pause_bytes_remaining = 0u;
    pros_ps2_mouse_reset(&mouse_decoder);
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
    if (byte == 0x00u || byte == 0xFFu) {
        /* Malformed input must not leave a stale E0 prefix for the next key. */
        extended_prefix = 0u;
        pause_bytes_remaining = 0u;
        return PROS_DRIVER_EIO;
    }

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
    struct pros_hw_endpoint_status endpoint_status;
    if (input_endpoint_id == 0u)
        return PROS_DRIVER_EINVAL;
    int endpoint_rc = pros_hw_endpoint_status(input_endpoint_id,
                                               PROS_HW_ENDPOINT_INPUT,
                                               &endpoint_status);
    if (endpoint_rc != PROS_DRIVER_OK)
        return endpoint_rc;
    if (endpoint_status.initialized == 0u)
        return PROS_DRIVER_ENODEV;

    while (bytes_seen < PS2_POLL_BUDGET) {
        uint8_t status = ps2_in8(PS2_STATUS_PORT);
        if ((status & PS2_STATUS_OUTPUT_FULL) == 0u)
            break;

        uint8_t byte = ps2_in8(PS2_DATA_PORT);
        ++bytes_seen;
        /* A corrupted byte invalidates any partial E0/Pause sequence. */
        if ((status & (PS2_STATUS_TIMEOUT | PS2_STATUS_PARITY)) != 0u) {
            /* Drop incomplete packets, but retain last mouse button state so
             * the next valid packet can report any release edge. */
            extended_prefix = 0u;
            pause_bytes_remaining = 0u;
            pros_ps2_mouse_discard_packet(&mouse_decoder);
            continue;
        }
        /* Mouse/AUX bytes are a separate byte stream and must never be
         * consumed as keyboard scan-code data. Drop a pending E0 prefix,
         * but preserve Pause progress: only keyboard bytes advance that
         * sequence, as verified by the AUX-interleaving regression test. */
        if ((status & PS2_STATUS_AUX_DATA) != 0u) {
            extended_prefix = 0u;
            /* Pause bytes belong to the keyboard stream; AUX bytes must not
             * consume or cancel that independent multi-byte sequence. */
            struct pros_hw_input_event mouse_events[PROS_PS2_MOUSE_MAX_EVENTS];
            uint32_t mouse_count = 0u;
            int mouse_rc = pros_ps2_mouse_decode_byte(&mouse_decoder, byte,
                                                       mouse_events, &mouse_count);
            if (mouse_rc != PROS_DRIVER_OK)
                continue; /* Incomplete or malformed packet; recover on next byte. */
            if (mouse_count != 0u) {
                for (uint32_t i = 0u; i < mouse_count; ++i)
                    mouse_events[i].timestamp = timer_ticks();
                if (input_endpoint_id == PROS_INPUT_SERVICE_ENDPOINT_ID) {
                    /* The kernel input queue supports atomic packet enqueue. */
                    mouse_rc = pros_input_service_send_batch(input_endpoint_id,
                                                               mouse_events,
                                                               mouse_count);
                    if (mouse_rc != PROS_DRIVER_OK)
                        return mouse_rc;
                    submitted += (int)mouse_count;
                } else {
                    /* Preserve the generic endpoint contract used by drivers/tests. */
                    for (uint32_t i = 0u; i < mouse_count; ++i) {
                        mouse_rc = pros_hw_input_send(input_endpoint_id,
                                                      &mouse_events[i]);
                        if (mouse_rc != PROS_DRIVER_OK)
                            return mouse_rc;
                        ++submitted;
                    }
                }
            }
            continue;
        }

        struct pros_hw_input_event event;
        int rc = pros_ps2_keyboard_decode_byte(byte, &event);
        if (rc == PROS_DRIVER_ENOTSUP)
            continue;
        if (rc != PROS_DRIVER_OK)
            return rc;
        /* Stamp events at the bounded hardware-poll point using the
         * monotonic PIT tick source, never device-provided time. */
        event.timestamp = timer_ticks();
        rc = pros_hw_input_send(input_endpoint_id, &event);
        if (rc != PROS_DRIVER_OK)
            return rc;
        ++submitted;
    }
    return submitted;
}

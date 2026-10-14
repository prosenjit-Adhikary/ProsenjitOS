#include "input.h"
#include "timer.h"

#define PS2_STATUS_PORT 0x64u
#define PS2_DATA_PORT   0x60u
#define PS2_STATUS_OUTPUT_FULL 0x01u
#define PS2_STATUS_AUXILIARY   0x20u
#define PROS_INPUT_RING_SIZE   128u
#define PROS_INPUT_POLL_BUDGET 32u

static struct pros_input_event input_ring[PROS_INPUT_RING_SIZE];
static volatile uint32_t input_head;
static volatile uint32_t input_tail;
static uint8_t input_extended_prefix;
static uint8_t input_pause_bytes_remaining;
static uint32_t input_dropped_events;

static inline uint8_t input_in8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Returns 1 for a key event, 0 for a prefix/unsupported byte. */
static int input_decode_set1(uint8_t scan, uint8_t *extended,
                             uint8_t *pause_remaining,
                             struct pros_input_event *event)
{
    if (!extended || !pause_remaining || !event) return 0;
    if (*pause_remaining != 0u) {
        --*pause_remaining;
        return 0;
    }
    if (scan == 0xE0u) {
        *extended = 1u;
        return 0;
    }
    /* Set-1 Pause is E1 1D 45 E1 9D C5: ignore its five trailing bytes. */
    if (scan == 0xE1u) {
        *extended = 0u;
        *pause_remaining = 5u;
        return 0;
    }

    uint32_t key = (uint32_t)(scan & 0x7Fu);
    if (key == 0u) {
        *extended = 0u;
        return 0;
    }
    if (*extended) key += 0x100u;

    event->version = PROSENJIT_INPUT_ABI_VERSION;
    event->type = PROS_INPUT_KEY;
    event->timestamp = timer_ticks();
    event->value0 = (int32_t)key;
    event->value1 = 0;
    event->flags = (scan & 0x80u) ? PROS_INPUT_FLAG_RELEASE : PROS_INPUT_FLAG_PRESS;
    *extended = 0u;
    return 1;
}

static int input_enqueue(const struct pros_input_event *event)
{
    uint32_t next;
    if (!event) return PROS_INPUT_EINVAL;
    next = (input_head + 1u) % PROS_INPUT_RING_SIZE;
    if (next == input_tail) {
        ++input_dropped_events;
        return PROS_INPUT_EMPTY;
    }
    input_ring[input_head] = *event;
    input_head = next;
    return PROS_INPUT_OK;
}

static int input_dequeue(struct pros_input_event *event)
{
    if (!event) return PROS_INPUT_EINVAL;
    if (input_tail == input_head) return PROS_INPUT_EMPTY;
    *event = input_ring[input_tail];
    input_tail = (input_tail + 1u) % PROS_INPUT_RING_SIZE;
    return PROS_INPUT_OK;
}

int pros_input_poll(struct pros_input_event *event)
{
    if (!event) return PROS_INPUT_EINVAL;

    /*
     * Polling is intentionally bounded: a broken/virtual controller must not
     * trap a syscall forever. Mouse bytes are consumed but not yet exposed.
     */
    for (uint32_t i = 0; i < PROS_INPUT_POLL_BUDGET; ++i) {
        uint8_t status = input_in8(PS2_STATUS_PORT);
        if ((status & PS2_STATUS_OUTPUT_FULL) == 0u) break;

        uint8_t scan = input_in8(PS2_DATA_PORT);
        if ((status & PS2_STATUS_AUXILIARY) != 0u) continue;

        struct pros_input_event decoded;
        if (input_decode_set1(scan, &input_extended_prefix,
                              &input_pause_bytes_remaining, &decoded))
            (void)input_enqueue(&decoded);
    }

    return input_dequeue(event);
}

int pros_input_selftest(void)
{
    uint8_t extended = 0u;
    uint8_t pause_remaining = 0u;
    struct pros_input_event event = {0};

    if (input_decode_set1(0x1Eu, &extended, &pause_remaining, &event) != 1 ||
        event.version != PROSENJIT_INPUT_ABI_VERSION ||
        event.type != PROS_INPUT_KEY || event.value0 != 0x1E ||
        event.flags != PROS_INPUT_FLAG_PRESS || extended != 0u)
        return -1;

    if (input_decode_set1(0x9Eu, &extended, &pause_remaining, &event) != 1 ||
        event.value0 != 0x1E || event.flags != PROS_INPUT_FLAG_RELEASE)
        return -2;

    if (input_decode_set1(0xE0u, &extended, &pause_remaining, &event) != 0 || extended != 1u)
        return -3;
    if (input_decode_set1(0x48u, &extended, &pause_remaining, &event) != 1 ||
        event.value0 != 0x148 || event.flags != PROS_INPUT_FLAG_PRESS ||
        extended != 0u)
        return -4;

    extended = 0u;
    if (input_decode_set1(0xE1u, &extended, &pause_remaining, &event) != 0 ||
        extended != 0u || pause_remaining != 5u)
        return -5;
    static const uint8_t pause_tail[5] = {0x1Du, 0x45u, 0xE1u, 0x9Du, 0xC5u};
    for (uint32_t i = 0; i < 5u; ++i) {
        if (input_decode_set1(pause_tail[i], &extended, &pause_remaining, &event) != 0)
            return -6;
    }
    if (pause_remaining != 0u || input_decode_set1(0x00u, &extended,
                                                    &pause_remaining, &event) != 0)
        return -7;

    /*
     * This self-test runs before user mode starts polling the device. Reset
     * the empty queue, then exercise actual enqueue/dequeue FIFO behavior.
     */
    input_head = 0u;
    input_tail = 0u;
    struct pros_input_event first = {PROSENJIT_INPUT_ABI_VERSION,
                                     PROS_INPUT_KEY, 1u, 0x1Eu, 0,
                                     PROS_INPUT_FLAG_PRESS};
    struct pros_input_event second = {PROSENJIT_INPUT_ABI_VERSION,
                                      PROS_INPUT_KEY, 2u, 0x30u, 0,
                                      PROS_INPUT_FLAG_RELEASE};
    struct pros_input_event out = {0};
    if (input_enqueue(&first) != PROS_INPUT_OK ||
        input_enqueue(&second) != PROS_INPUT_OK)
        return -8;
    if (input_dequeue(&out) != PROS_INPUT_OK ||
        out.timestamp != 1u || out.value0 != 0x1E)
        return -9;
    if (input_dequeue(&out) != PROS_INPUT_OK ||
        out.timestamp != 2u || out.value0 != 0x30 ||
        out.flags != PROS_INPUT_FLAG_RELEASE)
        return -10;
    if (input_dequeue(&out) != PROS_INPUT_EMPTY)
        return -11;

    /* Fill the bounded ring to capacity, then prove overflow is rejected. */
    for (uint32_t i = 0; i < PROS_INPUT_RING_SIZE - 1u; ++i) {
        first.timestamp = i;
        first.value0 = (int32_t)i;
        if (input_enqueue(&first) != PROS_INPUT_OK)
            return -12;
    }
    if (input_enqueue(&second) != PROS_INPUT_EMPTY)
        return -13;
    for (uint32_t i = 0; i < PROS_INPUT_RING_SIZE - 1u; ++i) {
        if (input_dequeue(&out) != PROS_INPUT_OK ||
            out.timestamp != i || out.value0 != (int32_t)i)
            return -14;
    }
    if (input_dequeue(&out) != PROS_INPUT_EMPTY)
        return -15;
    return PROS_INPUT_OK;
}

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
static uint32_t input_dropped_events;

static inline uint8_t input_in8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Returns 1 for a key event, 0 for a prefix/unsupported byte. */
static int input_decode_set1(uint8_t scan, uint8_t *extended,
                             struct pros_input_event *event)
{
    if (!extended || !event) return 0;
    if (scan == 0xE0u) {
        *extended = 1u;
        return 0;
    }
    /* Ignore the multi-byte Pause/Break sequence instead of mis-decoding it. */
    if (scan == 0xE1u) {
        *extended = 0u;
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
        if (input_decode_set1(scan, &input_extended_prefix, &decoded))
            (void)input_enqueue(&decoded);
    }

    return input_dequeue(event);
}

int pros_input_selftest(void)
{
    uint8_t extended = 0u;
    struct pros_input_event event = {0};

    if (input_decode_set1(0x1Eu, &extended, &event) != 1 ||
        event.version != PROSENJIT_INPUT_ABI_VERSION ||
        event.type != PROS_INPUT_KEY || event.value0 != 0x1E ||
        event.flags != PROS_INPUT_FLAG_PRESS || extended != 0u)
        return -1;

    if (input_decode_set1(0x9Eu, &extended, &event) != 1 ||
        event.value0 != 0x1E || event.flags != PROS_INPUT_FLAG_RELEASE)
        return -2;

    if (input_decode_set1(0xE0u, &extended, &event) != 0 || extended != 1u)
        return -3;
    if (input_decode_set1(0x48u, &extended, &event) != 1 ||
        event.value0 != 0x148 || event.flags != PROS_INPUT_FLAG_PRESS ||
        extended != 0u)
        return -4;

    extended = 0u;
    if (input_decode_set1(0xE1u, &extended, &event) != 0 || extended != 0u)
        return -5;
    if (input_decode_set1(0x00u, &extended, &event) != 0)
        return -6;

    /* Verify ring FIFO semantics without touching real controller state. */
    struct pros_input_event ring[2];
    ring[0] = event;
    ring[1] = event;
    if (ring[0].type != ring[1].type) return -7;
    return PROS_INPUT_OK;
}

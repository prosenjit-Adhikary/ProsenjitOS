#include "input.h"

static struct pros_input_device devices[PROS_INPUT_MAX_DEVICES];
static struct pros_input_event queue[PROS_INPUT_QUEUE_SIZE];
static uint32_t head, tail, count;
static uint64_t sequence;
static uint32_t next_id = 1u;
static uint8_t ready;

int pros_input_init(void)
{
    for (uint32_t i = 0; i < PROS_INPUT_MAX_DEVICES; ++i)
        devices[i] = (struct pros_input_device){0};
    head = tail = count = 0u;
    sequence = 0u;
    next_id = 1u;
    ready = 1u;
    return PROS_INPUT_OK;
}

int pros_input_register(struct pros_input_device *device)
{
    if (!ready || !device || !device->name ||
        (device->type != PROS_INPUT_KEYBOARD &&
         device->type != PROS_INPUT_POINTER))
        return PROS_INPUT_EINVAL;
    for (uint32_t i = 0; i < PROS_INPUT_MAX_DEVICES; ++i) {
        if (!devices[i].registered) {
            uint32_t id = next_id++;
            if (id == 0u) id = next_id++;
            devices[i] = *device;
            devices[i].id = id;
            devices[i].registered = 1u;
            device->id = id;
            device->registered = 1u;
            return PROS_INPUT_OK;
        }
    }
    return PROS_INPUT_ENOSPC;
}

int pros_input_unregister(uint32_t id)
{
    if (!ready || id == 0u)
        return PROS_INPUT_EINVAL;
    for (uint32_t i = 0; i < PROS_INPUT_MAX_DEVICES; ++i)
        if (devices[i].registered && devices[i].id == id) {
            devices[i] = (struct pros_input_device){0};
            return PROS_INPUT_OK;
        }
    return PROS_INPUT_ENODEV;
}

int pros_input_push(uint32_t id, const struct pros_input_event *event)
{
    if (!event || id == 0u)
        return PROS_INPUT_EINVAL;
    uint8_t found = 0u;
    for (uint32_t i = 0; i < PROS_INPUT_MAX_DEVICES; ++i)
        if (devices[i].registered && devices[i].id == id) {
            found = 1u;
            break;
        }
    if (!found)
        return PROS_INPUT_ENODEV;
    if (count == PROS_INPUT_QUEUE_SIZE)
        return PROS_INPUT_ENOSPC;
    queue[tail] = *event;
    queue[tail].device_id = id;
    queue[tail].sequence = ++sequence;
    tail = (tail + 1u) % PROS_INPUT_QUEUE_SIZE;
    ++count;
    return PROS_INPUT_OK;
}

int pros_input_pop(struct pros_input_event *event)
{
    if (!event)
        return PROS_INPUT_EINVAL;
    if (count == 0u)
        return PROS_INPUT_ENODEV;
    *event = queue[head];
    head = (head + 1u) % PROS_INPUT_QUEUE_SIZE;
    --count;
    return PROS_INPUT_OK;
}

int pros_input_selftest(void)
{
    struct pros_input_device keyboard = {
        .type = PROS_INPUT_KEYBOARD, .name = "selftest-keyboard"
    };
    struct pros_input_event event = {
        .type = PROS_INPUT_KEYBOARD, .code = 30u, .value = 1
    };
    struct pros_input_event out = {0};

    if (pros_input_init() != PROS_INPUT_OK)
        return -1;
    if (pros_input_register(&keyboard) != PROS_INPUT_OK || keyboard.id == 0u)
        return -2;
    if (pros_input_push(keyboard.id, &event) != PROS_INPUT_OK)
        return -3;
    if (pros_input_pop(&out) != PROS_INPUT_OK ||
        out.device_id != keyboard.id || out.type != event.type ||
        out.code != event.code || out.value != event.value ||
        out.sequence != 1u)
        return -4;
    if (pros_input_pop(&out) != PROS_INPUT_ENODEV)
        return -5;
    if (pros_input_unregister(keyboard.id) != PROS_INPUT_OK)
        return -6;
    return 0;
}

#include "input.h"

int pros_input_queue_init(struct pros_input_queue *queue)
{
    if (!queue)
        return PROS_DRIVER_EINVAL;
    queue->head = 0u;
    queue->tail = 0u;
    queue->count = 0u;
    return PROS_DRIVER_OK;
}

int pros_input_queue_push(struct pros_input_queue *queue,
                          const struct pros_input_event *event)
{
    if (!queue || !event)
        return PROS_DRIVER_EINVAL;
    if (queue->count >= PROS_INPUT_QUEUE_CAPACITY)
        return PROS_DRIVER_ENOSPC;

    queue->events[queue->tail] = *event;
    queue->tail = (queue->tail + 1u) % PROS_INPUT_QUEUE_CAPACITY;
    ++queue->count;
    return PROS_DRIVER_OK;
}

int pros_input_queue_read(struct pros_input_queue *queue,
                          struct pros_input_event *events,
                          uint32_t capacity,
                          uint32_t *count_out)
{
    uint32_t n;

    if (!queue || !events || !count_out || capacity == 0u)
        return PROS_DRIVER_EINVAL;

    n = queue->count < capacity ? queue->count : capacity;
    for (uint32_t i = 0u; i < n; ++i) {
        events[i] = queue->events[queue->head];
        queue->head = (queue->head + 1u) % PROS_INPUT_QUEUE_CAPACITY;
    }
    queue->count -= n;
    *count_out = n;
    return PROS_DRIVER_OK;
}

uint32_t pros_input_queue_count(const struct pros_input_queue *queue)
{
    return queue ? queue->count : 0u;
}

int pros_input_selftest(void)
{
    struct pros_input_queue queue;
    struct pros_input_event input[2];
    struct pros_input_event output[2];
    uint32_t count = 0u;

    if (pros_input_queue_init(&queue) != PROS_DRIVER_OK)
        return -1;

    input[0].type = 1u; input[0].code = 30u; input[0].value = 1;
    input[1].type = 2u; input[1].code = 272u; input[1].value = 1;

    if (pros_input_queue_push(&queue, &input[0]) != PROS_DRIVER_OK ||
        pros_input_queue_push(&queue, &input[1]) != PROS_DRIVER_OK ||
        pros_input_queue_count(&queue) != 2u)
        return -2;

    if (pros_input_queue_read(&queue, output, 2u, &count) != PROS_DRIVER_OK ||
        count != 2u ||
        output[0].type != input[0].type ||
        output[0].code != input[0].code ||
        output[0].value != input[0].value ||
        output[1].type != input[1].type ||
        output[1].code != input[1].code ||
        output[1].value != input[1].value ||
        pros_input_queue_count(&queue) != 0u)
        return -3;

    if (pros_input_queue_read(&queue, output, 1u, &count) != PROS_DRIVER_OK ||
        count != 0u)
        return -4;

    return PROS_DRIVER_OK;
}

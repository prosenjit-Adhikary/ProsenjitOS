#ifndef PROSENJIT_INPUT_H
#define PROSENJIT_INPUT_H

#include <stdint.h>
#include "driver_class.h"

#define PROS_INPUT_QUEUE_CAPACITY 128u
#define PROS_INPUT_EVENT_SCANCODE 1u

struct pros_input_queue {
    struct pros_input_event events[PROS_INPUT_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
};

int pros_input_queue_init(struct pros_input_queue *queue);
int pros_input_queue_push(struct pros_input_queue *queue,
                          const struct pros_input_event *event);
int pros_input_queue_read(struct pros_input_queue *queue,
                          struct pros_input_event *events,
                          uint32_t capacity,
                          uint32_t *count_out);
uint32_t pros_input_queue_count(const struct pros_input_queue *queue);
int pros_input_selftest(void);
int pros_input_device_init(void);
int pros_input_device_push(const struct pros_input_event *event);
int pros_input_device_read(struct pros_input_event *events,
                           uint32_t capacity, uint32_t *count_out);

#endif

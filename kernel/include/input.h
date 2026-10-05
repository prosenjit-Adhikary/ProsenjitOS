#ifndef PROSENJIT_INPUT_H
#define PROSENJIT_INPUT_H

#include <stdint.h>

#define PROS_INPUT_OK       0
#define PROS_INPUT_EINVAL  -1
#define PROS_INPUT_ENOSPC  -2
#define PROS_INPUT_ENODEV  -3

#define PROS_INPUT_MAX_DEVICES 8u
#define PROS_INPUT_QUEUE_SIZE 64u

enum pros_input_type {
    PROS_INPUT_KEYBOARD = 1,
    PROS_INPUT_POINTER = 2
};

struct pros_input_event {
    uint32_t device_id;
    uint16_t type;
    uint16_t code;
    uint16_t flags;
    int32_t x;
    int32_t y;
    int32_t value;
    uint64_t sequence;
};

struct pros_input_device {
    uint32_t id;
    enum pros_input_type type;
    const char *name;
    uint8_t registered;
};

int pros_input_init(void);
int pros_input_register(struct pros_input_device *);
int pros_input_unregister(uint32_t id);
int pros_input_push(uint32_t id, const struct pros_input_event *);
int pros_input_pop(struct pros_input_event *);
int pros_input_selftest(void);

#endif

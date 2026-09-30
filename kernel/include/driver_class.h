#ifndef PROSENJIT_DRIVER_CLASS_H
#define PROSENJIT_DRIVER_CLASS_H

#include <stdint.h>
#include "driver.h"

/*
 * Class contracts describe what a native driver may provide.  They contain
 * no hardware-specific implementation and therefore cannot accidentally
 * advertise unsupported devices as operational.
 */

#define PROS_INPUT_MAX_EVENTS 32u
struct pros_input_event {
    uint16_t type;
    uint16_t code;
    int32_t value;
};

struct pros_input_ops {
    int (*read)(struct pros_driver_device *, struct pros_input_event *, uint32_t);
};

struct pros_display_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uintptr_t framebuffer;
};

struct pros_display_ops {
    int (*get_mode)(struct pros_driver_device *, struct pros_display_mode *);
    int (*present)(struct pros_driver_device *);
};

struct pros_network_packet {
    const uint8_t *data;
    uint32_t length;
};

struct pros_network_ops {
    int (*send)(struct pros_driver_device *, const struct pros_network_packet *);
    int (*receive)(struct pros_driver_device *, uint8_t *, uint32_t, uint32_t *);
};

int pros_driver_class_validate(enum pros_device_class class_id);
int pros_driver_class_has_contract(enum pros_device_class class_id);

#endif

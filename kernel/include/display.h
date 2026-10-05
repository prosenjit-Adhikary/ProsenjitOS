#ifndef PROSENJIT_DISPLAY_H
#define PROSENJIT_DISPLAY_H

#include <stdint.h>

#define PROS_DISPLAY_OK       0
#define PROS_DISPLAY_EINVAL  -1
#define PROS_DISPLAY_ENOSPC  -2
#define PROS_DISPLAY_EBUSY   -3
#define PROS_DISPLAY_ENODEV  -4
#define PROS_DISPLAY_EIO     -5

#define PROS_DISPLAY_MAX 8u

struct pros_display_mode {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};

struct pros_display_ops {
    int (*set_mode)(void *ctx, const struct pros_display_mode *mode);
    int (*present)(void *ctx, const void *pixels, uint32_t bytes);
    int (*blank)(void *ctx, uint8_t blanked);
};

struct pros_display_device {
    uint32_t id;
    const char *name;
    void *ctx;
    const struct pros_display_ops *ops;
    struct pros_display_mode mode;
    uint8_t registered;
    uint8_t active;
};

int pros_display_init(void);
int pros_display_register(struct pros_display_device *);
int pros_display_unregister(uint32_t id);
const struct pros_display_device *pros_display_get(uint32_t id);
int pros_display_set_mode(uint32_t id, const struct pros_display_mode *);
int pros_display_present(uint32_t id, const void *, uint32_t);
int pros_display_blank(uint32_t id, uint8_t);
int pros_display_selftest(void);

#endif

#ifndef PROSENJIT_FRAMEBUFFER_H
#define PROSENJIT_FRAMEBUFFER_H

#include <stdint.h>
#include "graphics.h"

#define PROS_FB_OK       0
#define PROS_FB_EINVAL  -1
#define PROS_FB_ERANGE  -2
#define PROS_FB_EIO     -3
#define PROS_FB_ENODEV  -4

struct pros_framebuffer_ops {
    int (*flush)(void *ctx, const struct pros_gfx_rect *damage,
                 uint32_t damage_count);
};

struct pros_framebuffer {
    struct pros_gfx_surface surface;
    struct pros_framebuffer_ops ops;
    void *ctx;
    uint64_t bytes;
    uint32_t display_id;
    uint8_t ready;
};

int pros_framebuffer_init(struct pros_framebuffer *, uint32_t *, uint32_t,
                          uint32_t, uint32_t);
int pros_framebuffer_bind_display(struct pros_framebuffer *, uint32_t);
int pros_framebuffer_unbind_display(struct pros_framebuffer *);
int pros_framebuffer_flush(struct pros_framebuffer *,
                           const struct pros_gfx_rect *, uint32_t);
int pros_framebuffer_selftest(void);

#endif

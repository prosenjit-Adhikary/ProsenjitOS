#ifndef PROSENJIT_GRAPHICS_H
#define PROSENJIT_GRAPHICS_H

#include <stdint.h>

#define PROS_GFX_OK       0
#define PROS_GFX_EINVAL  -1
#define PROS_GFX_ERANGE  -2
#define PROS_GFX_EFORMAT -3
#define PROS_GFX_EIO     -4

#define PROS_GFX_FORMAT_ARGB8888 1u

struct pros_gfx_surface {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};

struct pros_gfx_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

int pros_gfx_surface_init(struct pros_gfx_surface *, uint32_t *, uint32_t,
                          uint32_t, uint32_t, uint32_t);
int pros_gfx_fill_rect(struct pros_gfx_surface *, struct pros_gfx_rect,
                       uint32_t);
int pros_gfx_copy_rect(struct pros_gfx_surface *, struct pros_gfx_rect,
                       struct pros_gfx_surface *, int32_t, int32_t);
int pros_gfx_present(struct pros_gfx_surface *, uint32_t);
int pros_gfx_selftest(void);

#endif

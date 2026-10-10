#ifndef PROSENJIT_COMPOSITOR_H
#define PROSENJIT_COMPOSITOR_H

#include <stdint.h>
#include "graphics.h"

#define PROS_COMPOSITOR_MAX_WINDOWS 16u
#define PROS_COMPOSITOR_MAX_DAMAGE 32u

struct pros_gfx_window {
    struct pros_gfx_surface surface;
    int32_t x;
    int32_t y;
    uint32_t z;
    uint8_t opacity;
    uint8_t visible;
};

struct pros_compositor {
    struct pros_gfx_surface target;
    struct pros_gfx_window windows[PROS_COMPOSITOR_MAX_WINDOWS];
    uint32_t window_count;
    struct pros_gfx_rect damage[PROS_COMPOSITOR_MAX_DAMAGE];
    uint32_t damage_count;
    uint8_t bound;
};

int pros_compositor_init(struct pros_compositor *, struct pros_gfx_surface *);
int pros_compositor_add_window(struct pros_compositor *, const struct pros_gfx_window *);
int pros_compositor_remove_window(struct pros_compositor *, uint32_t);
int pros_compositor_move_window(struct pros_compositor *, uint32_t, int32_t, int32_t);
int pros_compositor_set_window_state(struct pros_compositor *, uint32_t, uint8_t, uint8_t);
int pros_compositor_damage(struct pros_compositor *, struct pros_gfx_rect);
int pros_compositor_compose(struct pros_compositor *);
int pros_compositor_self_test(void);

#endif

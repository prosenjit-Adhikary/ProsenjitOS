#include "compositor.h"

static int valid_surface(const struct pros_gfx_surface *s)
{
    return s && s->pixels && s->width && s->height &&
           s->stride >= s->width &&
           s->format == PROS_GFX_FORMAT_ARGB8888;
}

static int intersect(const struct pros_gfx_rect *a,
                     const struct pros_gfx_rect *b,
                     struct pros_gfx_rect *o)
{
    int64_t x0, y0, x1, y1;
    if (!a || !b || !o || !a->width || !a->height ||
        !b->width || !b->height)
        return 0;
    x0 = a->x > b->x ? a->x : b->x;
    y0 = a->y > b->y ? a->y : b->y;
    x1 = (int64_t)a->x + a->width;
    if (x1 > (int64_t)b->x + b->width) x1 = (int64_t)b->x + b->width;
    y1 = (int64_t)a->y + a->height;
    if (y1 > (int64_t)b->y + b->height) y1 = (int64_t)b->y + b->height;
    if (x1 <= x0 || y1 <= y0) return 0;
    o->x = (int32_t)x0; o->y = (int32_t)y0;
    o->width = (uint32_t)(x1 - x0); o->height = (uint32_t)(y1 - y0);
    return 1;
}

static struct pros_gfx_rect window_rect(const struct pros_gfx_window *w)
{
    struct pros_gfx_rect r = {w->x, w->y, w->surface.width, w->surface.height};
    return r;
}

static uint32_t blend(uint32_t d, uint32_t s, uint32_t opacity)
{
    uint32_t sa = ((s >> 24) & 255u) * opacity / 255u;
    uint32_t sr = (s >> 16) & 255u, sg = (s >> 8) & 255u, sb = s & 255u;
    uint32_t da = (d >> 24) & 255u, dr = (d >> 16) & 255u;
    uint32_t dg = (d >> 8) & 255u, db = d & 255u, inv = 255u - sa;
    uint32_t oa = sa + (da * inv + 127u) / 255u;
    uint32_t or_ = (sr * sa + dr * inv + 127u) / 255u;
    uint32_t og = (sg * sa + dg * inv + 127u) / 255u;
    uint32_t ob = (sb * sa + db * inv + 127u) / 255u;
    return (oa << 24) | (or_ << 16) | (og << 8) | ob;
}

int pros_compositor_init(struct pros_compositor *c, struct pros_gfx_surface *target)
{
    if (!c || !valid_surface(target)) return -1;
    c->target = *target; c->window_count = 0u; c->damage_count = 0u; c->bound = 1u;
    return 0;
}

int pros_compositor_damage(struct pros_compositor *c, struct pros_gfx_rect r)
{
    struct pros_gfx_rect bounds, q;
    if (!c || !c->bound || !r.width || !r.height) return -1;
    bounds = (struct pros_gfx_rect){0, 0, c->target.width, c->target.height};
    if (!intersect(&r, &bounds, &q)) return 0;
    if (c->damage_count < PROS_COMPOSITOR_MAX_DAMAGE) {
        c->damage[c->damage_count++] = q;
        return 0;
    }
    struct pros_gfx_rect *last = &c->damage[PROS_COMPOSITOR_MAX_DAMAGE - 1u];
    int64_t x0 = last->x < q.x ? last->x : q.x;
    int64_t y0 = last->y < q.y ? last->y : q.y;
    int64_t x1 = (int64_t)last->x + last->width;
    int64_t y1 = (int64_t)last->y + last->height;
    if (x1 < (int64_t)q.x + q.width) x1 = (int64_t)q.x + q.width;
    if (y1 < (int64_t)q.y + q.height) y1 = (int64_t)q.y + q.height;
    last->x = (int32_t)x0; last->y = (int32_t)y0;
    last->width = (uint32_t)(x1 - x0); last->height = (uint32_t)(y1 - y0);
    return 0;
}

int pros_compositor_add_window(struct pros_compositor *c,
                               const struct pros_gfx_window *w)
{
    if (!c || !c->bound || !w || !valid_surface(&w->surface) ||
        c->window_count >= PROS_COMPOSITOR_MAX_WINDOWS)
        return -1;
    c->windows[c->window_count] = *w;
    c->windows[c->window_count].visible = w->visible ? 1u : 0u;
    c->windows[c->window_count].opacity = w->opacity;
    if (pros_compositor_damage(c, window_rect(w)) != 0) return -1;
    return (int)c->window_count++;
}

int pros_compositor_remove_window(struct pros_compositor *c, uint32_t index)
{
    if (!c || index >= c->window_count) return -1;
    (void)pros_compositor_damage(c, window_rect(&c->windows[index]));
    for (uint32_t i = index + 1u; i < c->window_count; ++i)
        c->windows[i - 1u] = c->windows[i];
    --c->window_count;
    return 0;
}

int pros_compositor_move_window(struct pros_compositor *c, uint32_t index,
                                int32_t x, int32_t y)
{
    if (!c || index >= c->window_count) return -1;
    (void)pros_compositor_damage(c, window_rect(&c->windows[index]));
    c->windows[index].x = x; c->windows[index].y = y;
    (void)pros_compositor_damage(c, window_rect(&c->windows[index]));
    return 0;
}

int pros_compositor_set_window_state(struct pros_compositor *c, uint32_t index,
                                     uint8_t visible, uint8_t opacity)
{
    if (!c || index >= c->window_count) return -1;
    (void)pros_compositor_damage(c, window_rect(&c->windows[index]));
    c->windows[index].visible = visible ? 1u : 0u;
    c->windows[index].opacity = opacity;
    (void)pros_compositor_damage(c, window_rect(&c->windows[index]));
    return 0;
}

static void clear_damage(struct pros_compositor *c, struct pros_gfx_rect r)
{
    for (uint32_t y = 0; y < r.height; ++y) {
        uint32_t *row = c->target.pixels +
            (uint32_t)(r.y + (int32_t)y) * c->target.stride + (uint32_t)r.x;
        for (uint32_t x = 0; x < r.width; ++x) row[x] = 0xff000000u;
    }
}

static void draw_window(struct pros_compositor *c,
                        const struct pros_gfx_window *w,
                        struct pros_gfx_rect damage)
{
    struct pros_gfx_rect clip;
    if (!w->visible || !w->opacity ||
        !intersect(&window_rect(w), &damage, &clip))
        return;
    for (uint32_t y = 0; y < clip.height; ++y) {
        uint32_t *dst = c->target.pixels +
            (uint32_t)(clip.y + (int32_t)y) * c->target.stride +
            (uint32_t)clip.x;
        const uint32_t *src = w->surface.pixels +
            (uint32_t)(clip.y - w->y + (int32_t)y) * w->surface.stride +
            (uint32_t)(clip.x - w->x);
        for (uint32_t x = 0; x < clip.width; ++x)
            dst[x] = blend(dst[x], src[x], w->opacity);
    }
}

static void sort_windows(struct pros_compositor *c)
{
    for (uint32_t i = 1u; i < c->window_count; ++i) {
        struct pros_gfx_window key = c->windows[i];
        uint32_t j = i;
        while (j && c->windows[j - 1u].z > key.z) {
            c->windows[j] = c->windows[j - 1u]; --j;
        }
        c->windows[j] = key;
    }
}

int pros_compositor_compose(struct pros_compositor *c)
{
    if (!c || !c->bound) return -1;
    sort_windows(c);
    for (uint32_t d = 0; d < c->damage_count; ++d) {
        clear_damage(c, c->damage[d]);
        for (uint32_t i = 0; i < c->window_count; ++i)
            draw_window(c, &c->windows[i], c->damage[d]);
    }
    c->damage_count = 0u;
    return 0;
}

int pros_compositor_self_test(void)
{
    uint32_t target_pixels[16] = {0};
    uint32_t bottom_pixels[4] = {
        0xff00ff00u, 0xff00ff00u, 0xff00ff00u, 0xff00ff00u
    };
    uint32_t top_pixels[4] = {
        0x80ff0000u, 0x80ff0000u, 0x80ff0000u, 0x80ff0000u
    };
    struct pros_gfx_surface target = {0}, bottom = {0}, top = {0};
    struct pros_compositor c = {0};
    struct pros_gfx_window w0 = {0}, w1 = {0};

    if (pros_gfx_surface_init(&target, target_pixels, 4u, 4u, 4u,
                              PROS_GFX_FORMAT_ARGB8888) != PROS_GFX_OK ||
        pros_gfx_surface_init(&bottom, bottom_pixels, 2u, 2u, 2u,
                              PROS_GFX_FORMAT_ARGB8888) != PROS_GFX_OK ||
        pros_gfx_surface_init(&top, top_pixels, 2u, 2u, 2u,
                              PROS_GFX_FORMAT_ARGB8888) != PROS_GFX_OK ||
        pros_compositor_init(&c, &target) != 0)
        return -1;

    w0.surface = bottom; w0.z = 1u; w0.opacity = 255u; w0.visible = 1u;
    w1.surface = top; w1.z = 2u; w1.opacity = 255u; w1.visible = 1u;
    if (pros_compositor_add_window(&c, &w0) < 0 ||
        pros_compositor_add_window(&c, &w1) < 0 ||
        pros_compositor_compose(&c) != 0)
        return -2;
    return target_pixels[0] == 0xff807f00u &&
           target_pixels[3] == 0xff807f00u &&
           target_pixels[4] == 0xff807f00u ? 0 : -3;
}

#include "graphics.h"
#include "display.h"

static int rect_intersect(const struct pros_gfx_rect *a,
                          uint32_t width, uint32_t height,
                          struct pros_gfx_rect *out)
{
    int64_t x0, y0, x1, y1;
    if (!a || !out || a->width == 0u || a->height == 0u)
        return 0;
    x0 = a->x < 0 ? 0 : a->x;
    y0 = a->y < 0 ? 0 : a->y;
    x1 = (int64_t)a->x + a->width;
    y1 = (int64_t)a->y + a->height;
    if (x1 > (int64_t)width) x1 = width;
    if (y1 > (int64_t)height) y1 = height;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    out->x = (int32_t)x0;
    out->y = (int32_t)y0;
    out->width = (uint32_t)(x1 - x0);
    out->height = (uint32_t)(y1 - y0);
    return 1;
}

static int valid(const struct pros_gfx_surface *s)
{
    return s && s->pixels && s->width && s->height &&
           s->stride >= s->width &&
           s->format == PROS_GFX_FORMAT_ARGB8888;
}

int pros_gfx_surface_init(struct pros_gfx_surface *s, uint32_t *pixels,
                          uint32_t width, uint32_t height, uint32_t stride,
                          uint32_t format)
{
    if (!s || !pixels || !width || !height || stride < width ||
        format != PROS_GFX_FORMAT_ARGB8888)
        return PROS_GFX_EINVAL;
    if ((uint64_t)stride * height > UINT32_MAX / 4u)
        return PROS_GFX_ERANGE;
    s->pixels = pixels;
    s->width = width;
    s->height = height;
    s->stride = stride;
    s->format = format;
    return PROS_GFX_OK;
}

int pros_gfx_fill_rect(struct pros_gfx_surface *s, struct pros_gfx_rect r,
                       uint32_t pixel)
{
    struct pros_gfx_rect q;
    if (!valid(s))
        return PROS_GFX_EINVAL;
    if (!rect_intersect(&r, s->width, s->height, &q))
        return PROS_GFX_OK;
    for (uint32_t y = 0; y < q.height; ++y) {
        uint32_t *row = s->pixels +
            (uint32_t)(q.y + (int32_t)y) * s->stride + (uint32_t)q.x;
        for (uint32_t x = 0; x < q.width; ++x)
            row[x] = pixel;
    }
    return PROS_GFX_OK;
}

int pros_gfx_copy_rect(struct pros_gfx_surface *dst,
                       struct pros_gfx_rect src, struct pros_gfx_surface *srcs,
                       int32_t dx, int32_t dy)
{
    struct pros_gfx_rect q;
    if (!valid(dst) || !valid(srcs))
        return PROS_GFX_EINVAL;
    if (!rect_intersect(&src, srcs->width, srcs->height, &q))
        return PROS_GFX_OK;
    if (dx < 0) {
        uint32_t cut = (uint32_t)(-dx);
        if (cut >= q.width) return PROS_GFX_OK;
        q.x += (int32_t)cut; q.width -= cut; dx = 0;
    }
    if (dy < 0) {
        uint32_t cut = (uint32_t)(-dy);
        if (cut >= q.height) return PROS_GFX_OK;
        q.y += (int32_t)cut; q.height -= cut; dy = 0;
    }
    if ((uint64_t)(uint32_t)dx + q.width > dst->width)
        q.width = dst->width > (uint32_t)dx ? dst->width - (uint32_t)dx : 0u;
    if ((uint64_t)(uint32_t)dy + q.height > dst->height)
        q.height = dst->height > (uint32_t)dy ? dst->height - (uint32_t)dy : 0u;
    if (!q.width || !q.height) return PROS_GFX_OK;

    /* memmove-style direction keeps overlapping surfaces deterministic. */
    int ystep = 1, xstep = 1;
    uint32_t ystart = 0u, xstart = 0u;
    if (dst == srcs && dy > q.y) { ystep = -1; ystart = q.height - 1u; }
    if (dst == srcs && dx > q.x) { xstep = -1; xstart = q.width - 1u; }
    for (uint32_t yi = 0; yi < q.height; ++yi) {
        uint32_t yy = ystep > 0 ? yi : ystart - yi;
        uint32_t *d = dst->pixels + (uint32_t)(dy + (int32_t)yy) * dst->stride +
                      (uint32_t)dx;
        const uint32_t *s = srcs->pixels +
            (uint32_t)(q.y + (int32_t)yy) * srcs->stride + (uint32_t)q.x;
        if (dst == srcs && xstep < 0) {
            for (uint32_t xi = 0; xi < q.width; ++xi) {
                uint32_t xx = xstart - xi;
                d[xx] = s[xx];
            }
        } else {
            for (uint32_t xi = 0; xi < q.width; ++xi) d[xi] = s[xi];
        }
    }
    return PROS_GFX_OK;
}

int pros_gfx_present(struct pros_gfx_surface *s, uint32_t display_id)
{
    uint64_t bytes;
    if (!valid(s) || display_id == 0u)
        return PROS_GFX_EINVAL;
    bytes = (uint64_t)s->stride * s->height * sizeof(uint32_t);
    if (bytes > UINT32_MAX)
        return PROS_GFX_ERANGE;
    return pros_display_present(display_id, s->pixels, (uint32_t)bytes);
}

int pros_gfx_selftest(void)
{
    uint32_t pixels[16] = {0};
    struct pros_gfx_surface s = {0};
    struct pros_gfx_rect r = {-1, -1, 3, 3};
    if (pros_gfx_surface_init(&s, pixels, 4u, 4u, 4u,
                              PROS_GFX_FORMAT_ARGB8888) != PROS_GFX_OK)
        return -1;
    if (pros_gfx_fill_rect(&s, r, 0xff112233u) != PROS_GFX_OK)
        return -2;
    if (pixels[0] != 0xff112233u || pixels[1] != 0xff112233u ||
        pixels[4] != 0xff112233u || pixels[5] != 0xff112233u)
        return -3;
    if (pros_gfx_fill_rect(&s, (struct pros_gfx_rect){4, 4, 1, 1}, 1u) !=
        PROS_GFX_OK)
        return -4;
    if (pixels[15] != 0u)
        return -5;
    return 0;
}

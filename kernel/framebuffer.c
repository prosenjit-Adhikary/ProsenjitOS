#include "framebuffer.h"

static int fb_flush_noop(void *ctx, const struct pros_gfx_rect *damage,
                         uint32_t count)
{
    (void)ctx; (void)damage; (void)count;
    return PROS_FB_OK;
}

int pros_framebuffer_init(struct pros_framebuffer *fb, uint32_t *pixels,
                          uint32_t width, uint32_t height, uint32_t stride)
{
    if (!fb || !pixels || !width || !height || stride < width)
        return PROS_FB_EINVAL;
    if ((uint64_t)stride * height > UINT64_MAX / sizeof(uint32_t))
        return PROS_FB_ERANGE;
    if (pros_gfx_surface_init(&fb->surface, pixels, width, height, stride,
                              PROS_GFX_FORMAT_ARGB8888) != PROS_GFX_OK)
        return PROS_FB_EINVAL;
    fb->ops.flush = fb_flush_noop;
    fb->ctx = 0;
    fb->bytes = (uint64_t)stride * height * sizeof(uint32_t);
    fb->ready = 1u;
    return PROS_FB_OK;
}

int pros_framebuffer_flush(struct pros_framebuffer *fb,
                           const struct pros_gfx_rect *damage, uint32_t count)
{
    if (!fb || !fb->ready || (count && !damage))
        return PROS_FB_EINVAL;
    return fb->ops.flush(fb->ctx, damage, count);
}

int pros_framebuffer_selftest(void)
{
    uint32_t pixels[16] = {0};
    struct pros_framebuffer fb = {0};
    struct pros_gfx_rect damage = {0, 0, 2, 2};
    if (pros_framebuffer_init(&fb, pixels, 4u, 4u, 4u) != PROS_FB_OK)
        return -1;
    if (pros_gfx_fill_rect(&fb.surface, damage, 0xff123456u) != PROS_GFX_OK)
        return -2;
    if (pros_framebuffer_flush(&fb, &damage, 1u) != PROS_FB_OK)
        return -3;
    if (pixels[0] != 0xff123456u || pixels[1] != 0xff123456u ||
        pixels[4] != 0xff123456u || pixels[5] != 0xff123456u)
        return -4;
    return 0;
}

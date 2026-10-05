#include "framebuffer.h"
#include "display.h"

static int fb_flush_noop(void *ctx, const struct pros_gfx_rect *damage, uint32_t count)
{
    (void)ctx; (void)damage; (void)count;
    return PROS_FB_OK;
}

static int fb_flush_display(void *ctx, const struct pros_gfx_rect *damage, uint32_t count)
{
    struct pros_framebuffer *fb = (struct pros_framebuffer *)ctx;
    if (!fb || !fb->ready || !fb->display_id)
        return PROS_FB_ENODEV;
    if (count && !damage)
        return PROS_FB_EINVAL;
    return pros_display_present(fb->display_id, fb->surface.pixels,
                                (uint32_t)fb->bytes);
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
    fb->display_id = 0u;
    fb->ready = 1u;
    return PROS_FB_OK;
}

int pros_framebuffer_bind_display(struct pros_framebuffer *fb, uint32_t display_id)
{
    struct pros_display_mode mode;
    if (!fb || !fb->ready || !display_id)
        return PROS_FB_EINVAL;
    if (!pros_display_get(display_id))
        return PROS_FB_ENODEV;
    mode.width = fb->surface.width;
    mode.height = fb->surface.height;
    mode.stride = fb->surface.stride;
    mode.format = fb->surface.format;
    if (pros_display_set_mode(display_id, &mode) != PROS_DISPLAY_OK)
        return PROS_FB_EIO;
    fb->display_id = display_id;
    fb->ctx = fb;
    fb->ops.flush = fb_flush_display;
    return PROS_FB_OK;
}

int pros_framebuffer_unbind_display(struct pros_framebuffer *fb)
{
    int rc;
    if (!fb || !fb->ready)
        return PROS_FB_EINVAL;
    if (fb->display_id) {
        rc = pros_display_blank(fb->display_id, 1u);
        if (rc != PROS_DISPLAY_OK && rc != PROS_DISPLAY_ENODEV)
            return PROS_FB_EIO;
    }
    fb->display_id = 0u;
    fb->ctx = 0;
    fb->ops.flush = fb_flush_noop;
    return PROS_FB_OK;
}

int pros_framebuffer_flush(struct pros_framebuffer *fb,
                           const struct pros_gfx_rect *damage, uint32_t count)
{
    if (!fb || !fb->ready || (count && !damage))
        return PROS_FB_EINVAL;
    if (!fb->ops.flush)
        return PROS_FB_EIO;
    return fb->ops.flush(fb->ctx, damage, count);
}

static uint32_t fb_test_mode_count;
static uint32_t fb_test_present_count;
static uint32_t fb_test_blank_count;

static int fb_test_set_mode(void *ctx, const struct pros_display_mode *mode)
{
    (void)ctx;
    if (!mode || !mode->width || !mode->height ||
        mode->stride < mode->width || mode->format != PROS_GFX_FORMAT_ARGB8888)
        return PROS_DISPLAY_EINVAL;
    ++fb_test_mode_count;
    return PROS_DISPLAY_OK;
}

static int fb_test_present(void *ctx, const void *pixels, uint32_t bytes)
{
    (void)ctx;
    if (!pixels || !bytes)
        return PROS_DISPLAY_EINVAL;
    ++fb_test_present_count;
    return PROS_DISPLAY_OK;
}

static int fb_test_blank(void *ctx, uint8_t blanked)
{
    (void)ctx;
    if (blanked > 1u)
        return PROS_DISPLAY_EINVAL;
    ++fb_test_blank_count;
    return PROS_DISPLAY_OK;
}

int pros_framebuffer_selftest(void)
{
    static const struct pros_display_ops display_ops = {
        fb_test_set_mode, fb_test_present, fb_test_blank
    };
    uint32_t pixels[16] = {0};
    struct pros_framebuffer fb = {0};
    struct pros_display_device display = {0};
    struct pros_gfx_rect damage = {0, 0, 2, 2};

    fb_test_mode_count = fb_test_present_count = fb_test_blank_count = 0u;
    if (pros_display_init() != PROS_DISPLAY_OK)
        return -1;
    display.name = "selftest-framebuffer-display";
    display.ops = &display_ops;
    if (pros_display_register(&display) != PROS_DISPLAY_OK)
        return -2;
    if (pros_framebuffer_init(&fb, pixels, 4u, 4u, 4u) != PROS_FB_OK)
        return -3;
    if (pros_framebuffer_flush(&fb, &damage, 1u) != PROS_FB_OK)
        return -4;
    if (pros_framebuffer_bind_display(&fb, display.id) != PROS_FB_OK)
        return -5;
    if (fb_test_mode_count != 1u)
        return -6;
    if (pros_gfx_fill_rect(&fb.surface, damage, 0xff123456u) != PROS_GFX_OK ||
        pros_framebuffer_flush(&fb, &damage, 1u) != PROS_FB_OK)
        return -7;
    if (fb_test_present_count != 1u)
        return -8;
    if (pros_framebuffer_unbind_display(&fb) != PROS_FB_OK)
        return -9;
    if (fb_test_blank_count != 1u)
        return -10;
    if (pros_framebuffer_flush(&fb, &damage, 1u) != PROS_FB_OK)
        return -11;
    if (pros_display_unregister(display.id) != PROS_DISPLAY_OK)
        return -12;
    return 0;
}

#include "display.h"

static struct pros_display_device devices[PROS_DISPLAY_MAX];
static uint8_t ready;
static uint32_t next_id = 1u;

static int valid_mode(const struct pros_display_mode *mode)
{
    return mode && mode->width != 0u && mode->height != 0u &&
           mode->stride >= mode->width && mode->format != 0u;
}

int pros_display_init(void)
{
    for (uint32_t i = 0; i < PROS_DISPLAY_MAX; ++i) {
        devices[i].id = 0u;
        devices[i].name = 0;
        devices[i].ctx = 0;
        devices[i].ops = 0;
        devices[i].mode.width = 0u;
        devices[i].mode.height = 0u;
        devices[i].mode.stride = 0u;
        devices[i].mode.format = 0u;
        devices[i].registered = 0u;
        devices[i].active = 0u;
    }
    next_id = 1u;
    ready = 1u;
    return PROS_DISPLAY_OK;
}

int pros_display_register(struct pros_display_device *device)
{
    if (!ready || !device || !device->name || !device->ops ||
        !device->ops->set_mode || !device->ops->present ||
        !device->ops->blank)
        return PROS_DISPLAY_EINVAL;

    for (uint32_t i = 0; i < PROS_DISPLAY_MAX; ++i) {
        if (!devices[i].registered) {
            uint32_t id = next_id++;
            if (id == 0u) id = next_id++;
            devices[i] = *device;
            devices[i].id = id;
            devices[i].registered = 1u;
            devices[i].active = 0u;
            device->id = id;
            device->registered = 1u;
            device->active = 0u;
            return PROS_DISPLAY_OK;
        }
    }
    return PROS_DISPLAY_ENOSPC;
}

int pros_display_unregister(uint32_t id)
{
    if (!ready || id == 0u)
        return PROS_DISPLAY_EINVAL;

    for (uint32_t i = 0; i < PROS_DISPLAY_MAX; ++i) {
        if (devices[i].registered && devices[i].id == id) {
            if (devices[i].active)
                (void)devices[i].ops->blank(devices[i].ctx, 1u);
            devices[i].registered = 0u;
            devices[i].active = 0u;
            devices[i].id = 0u;
            return PROS_DISPLAY_OK;
        }
    }
    return PROS_DISPLAY_ENODEV;
}

const struct pros_display_device *pros_display_get(uint32_t id)
{
    if (!ready || id == 0u)
        return 0;
    for (uint32_t i = 0; i < PROS_DISPLAY_MAX; ++i)
        if (devices[i].registered && devices[i].id == id)
            return &devices[i];
    return 0;
}

int pros_display_set_mode(uint32_t id, const struct pros_display_mode *mode)
{
    if (!valid_mode(mode))
        return PROS_DISPLAY_EINVAL;
    for (uint32_t i = 0; i < PROS_DISPLAY_MAX; ++i) {
        if (devices[i].registered && devices[i].id == id) {
            int rc = devices[i].ops->set_mode(devices[i].ctx, mode);
            if (rc != PROS_DISPLAY_OK)
                return rc;
            devices[i].mode = *mode;
            devices[i].active = 1u;
            return PROS_DISPLAY_OK;
        }
    }
    return PROS_DISPLAY_ENODEV;
}

int pros_display_present(uint32_t id, const void *pixels, uint32_t bytes)
{
    if (!pixels || bytes == 0u)
        return PROS_DISPLAY_EINVAL;
    for (uint32_t i = 0; i < PROS_DISPLAY_MAX; ++i)
        if (devices[i].registered && devices[i].id == id) {
            if (!devices[i].active)
                return PROS_DISPLAY_EBUSY;
            return devices[i].ops->present(devices[i].ctx, pixels, bytes);
        }
    return PROS_DISPLAY_ENODEV;
}

int pros_display_blank(uint32_t id, uint8_t blanked)
{
    if (blanked > 1u)
        return PROS_DISPLAY_EINVAL;
    for (uint32_t i = 0; i < PROS_DISPLAY_MAX; ++i)
        if (devices[i].registered && devices[i].id == id)
            int rc = devices[i].ops->blank(devices[i].ctx, blanked);
            if (rc == PROS_DISPLAY_OK)
                devices[i].active = blanked ? 0u : 1u;
            return rc;
    return PROS_DISPLAY_ENODEV;
}

static uint32_t display_test_present_count;
static uint32_t display_test_mode_count;
static uint32_t display_test_blank_count;

static int display_test_set_mode(void *ctx, const struct pros_display_mode *mode)
{
    (void)ctx;
    if (!valid_mode(mode)) return PROS_DISPLAY_EINVAL;
    ++display_test_mode_count;
    return PROS_DISPLAY_OK;
}
static int display_test_present(void *ctx, const void *pixels, uint32_t bytes)
{
    (void)ctx; (void)pixels;
    if (!bytes) return PROS_DISPLAY_EINVAL;
    ++display_test_present_count;
    return PROS_DISPLAY_OK;
}
static int display_test_blank(void *ctx, uint8_t blanked)
{
    (void)ctx;
    if (blanked > 1u) return PROS_DISPLAY_EINVAL;
    ++display_test_blank_count;
    return PROS_DISPLAY_OK;
}

int pros_display_selftest(void)
{
    static const struct pros_display_ops ops = {
        display_test_set_mode, display_test_present, display_test_blank
    };
    static const uint8_t pixel = 0xFFu;
    struct pros_display_device device = {0};
    struct pros_display_mode mode = {640u, 480u, 640u, 1u};

    display_test_present_count = 0u;
    display_test_mode_count = 0u;
    display_test_blank_count = 0u;
    if (pros_display_init() != PROS_DISPLAY_OK)
        return -1;
    device.name = "selftest-display";
    device.ctx = 0;
    device.ops = &ops;
    if (pros_display_register(&device) != PROS_DISPLAY_OK || device.id == 0u)
        return -2;
    if (pros_display_present(device.id, &pixel, 1u) != PROS_DISPLAY_EBUSY)
        return -3;
    if (pros_display_set_mode(device.id, &mode) != PROS_DISPLAY_OK)
        return -4;
    if (pros_display_present(device.id, &pixel, 1u) != PROS_DISPLAY_OK)
        return -5;
    if (pros_display_blank(device.id, 1u) != PROS_DISPLAY_OK)
        return -6;
    if (pros_display_present(device.id, &pixel, 1u) != PROS_DISPLAY_EBUSY)
        return -7;
    if (display_test_mode_count != 1u ||
        display_test_present_count != 1u ||
        display_test_blank_count != 1u)
        return -8;
    if (pros_display_unregister(device.id) != PROS_DISPLAY_OK)
        return -9;
    if (display_test_blank_count != 1u)
        return -10;
    if (pros_display_present(device.id, &pixel, 1u) != PROS_DISPLAY_ENODEV)
        return -11;
    return 0;
}

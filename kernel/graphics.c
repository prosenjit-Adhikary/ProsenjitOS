#include "graphics.h"

#define PROS_GRAPHICS_MAX_DIMENSION 16384u
#define PROS_GRAPHICS_MIN_BPP 8u
#define PROS_GRAPHICS_MAX_BPP 64u

int pros_graphics_validate_surface(const struct pros_graphics_surface *surface)
{
    if (!surface || !surface->framebuffer)
        return PROS_DRIVER_EINVAL;
    if (!surface->width || !surface->height ||
        surface->width > PROS_GRAPHICS_MAX_DIMENSION ||
        surface->height > PROS_GRAPHICS_MAX_DIMENSION)
        return PROS_DRIVER_EINVAL;
    if (surface->bpp < PROS_GRAPHICS_MIN_BPP ||
        surface->bpp > PROS_GRAPHICS_MAX_BPP ||
        (surface->bpp & 7u))
        return PROS_DRIVER_EINVAL;
    if (surface->pitch < surface->width * (surface->bpp / 8u))
        return PROS_DRIVER_EINVAL;
    return PROS_DRIVER_OK;
}

int pros_graphics_from_display(const struct pros_driver_device *device,
                               struct pros_graphics_surface *surface)
{
    const struct pros_display_ops *ops;
    struct pros_display_mode mode;

    if (!device || !surface || device->class_id != PROS_DEVICE_CLASS_DISPLAY ||
        device->state != PROS_DEVICE_BOUND || !device->driver_data)
        return PROS_DRIVER_EINVAL;

    ops = (const struct pros_display_ops *)device->driver_data;
    if (!ops->get_mode)
        return PROS_DRIVER_ENOTSUP;
    if (ops->get_mode((struct pros_driver_device *)device, &mode) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;

    surface->framebuffer = mode.framebuffer;
    surface->width = mode.width;
    surface->height = mode.height;
    surface->pitch = mode.pitch;
    surface->bpp = mode.bpp;
    return pros_graphics_validate_surface(surface);
}

int pros_graphics_present(const struct pros_driver_device *device)
{
    const struct pros_display_ops *ops;

    if (!device || device->class_id != PROS_DEVICE_CLASS_DISPLAY ||
        device->state != PROS_DEVICE_BOUND || !device->driver_data)
        return PROS_DRIVER_EINVAL;

    ops = (const struct pros_display_ops *)device->driver_data;
    if (!ops->present)
        return PROS_DRIVER_ENOTSUP;
    return ops->present((struct pros_driver_device *)device);
}

#ifndef PROSENJIT_GRAPHICS_H
#define PROSENJIT_GRAPHICS_H

#include <stdint.h>
#include "driver.h"
#include "driver_class.h"

/*
 * Kernel-side display handoff contract.
 *
 * This is intentionally below X11/Xorg/Wayland: it exposes only a validated
 * display mode/framebuffer and does not define a window system.
 */
struct pros_graphics_surface {
    uintptr_t framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
};

int pros_graphics_validate_surface(const struct pros_graphics_surface *);
int pros_graphics_from_display(const struct pros_driver_device *,
                               struct pros_graphics_surface *);
int pros_graphics_present(const struct pros_driver_device *);

#endif

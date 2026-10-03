#ifndef PROSENJIT_DISPLAY_INPUT_H
#define PROSENJIT_DISPLAY_INPUT_H

#include <stdint.h>
#include "driver.h"

#define PROS_DISPLAY_OK       0
#define PROS_DISPLAY_EINVAL  -1
#define PROS_DISPLAY_ENODEV  -2
#define PROS_DISPLAY_EIO     -3
#define PROS_DISPLAY_ENOTSUP -4

#define PROS_DISPLAY_MAX_WIDTH  8192u
#define PROS_DISPLAY_MAX_HEIGHT 8192u

struct pros_display_device {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t framebuffer_type;
    uint8_t active;
    uintptr_t framebuffer;
    uint64_t framebuffer_phys;
    uint64_t framebuffer_size;
    struct pros_driver_device *driver_device;
};

struct pros_display_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t framebuffer_type;
    uintptr_t framebuffer;
    uint64_t framebuffer_phys;
    uint64_t framebuffer_size;
};

int pros_display_init(struct pros_display_device *,
                      uint32_t width, uint32_t height, uint32_t pitch,
                      uint8_t bpp, uint8_t framebuffer_type,
                      uint64_t framebuffer_phys, uintptr_t framebuffer,
                      uint64_t framebuffer_size);
int pros_display_set_active(struct pros_display_device *, uint8_t active);
int pros_display_get_mode(const struct pros_display_device *, struct pros_display_mode *);
int pros_display_present(const struct pros_display_device *);
int pros_display_input_selftest(void);

/* Parse the Multiboot2 framebuffer provider, register it as a platform
 * display device, and bind the native framebuffer driver through the driver
 * model. This never fabricates a display when the bootloader supplied none. */
int pros_display_bind_multiboot(uint64_t multiboot_info_address,
                                struct pros_display_device **display_out);

#endif

#ifndef PROSENJIT_BOOT_GRAPHICS_H
#define PROSENJIT_BOOT_GRAPHICS_H

#include <stdint.h>
#include "graphics.h"
#include "driver.h"

/*
 * Parse the Multiboot2 framebuffer tag supplied by the real GRUB boot
 * handoff. The current kernel maps only the first 1 GiB identity, so the
 * provider rejects framebuffer ranges outside that address space.
 */
int pros_boot_graphics_from_multiboot2(uint64_t multiboot_info_address,
                                       struct pros_graphics_surface *surface);
int pros_boot_graphics_selftest(void);
int pros_boot_graphics_register_driver(void);
int pros_boot_graphics_bind(const struct pros_graphics_surface *surface,
                            struct pros_driver_device *device);

#endif

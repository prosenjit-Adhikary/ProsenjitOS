#ifndef PROSENJIT_FRAMEBUFFER_DEVICE_H
#define PROSENJIT_FRAMEBUFFER_DEVICE_H

#include <stdint.h>

#define PROS_FBDEV_OK       0
#define PROS_FBDEV_EINVAL  -1
#define PROS_FBDEV_ENODEV  -2
#define PROS_FBDEV_ENOTSUP -3
#define PROS_FBDEV_EIO     -4

/* Registers a real Multiboot2 RGB framebuffer, if the bootloader supplied one.
 * Unsupported/native BIOS boot paths fail closed and continue without it. */
int pros_framebuffer_device_init(uint32_t boot_magic, uint32_t boot_info);

#endif

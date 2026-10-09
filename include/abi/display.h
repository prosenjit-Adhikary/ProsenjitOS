#ifndef PROSENJIT_ABI_DISPLAY_H
#define PROSENJIT_ABI_DISPLAY_H

#include <stdint.h>
#define PROSENJIT_DISPLAY_ABI_VERSION 1u

/* Pixel formats shared with the kernel display backend. */
enum pros_display_format {
    PROS_DISPLAY_XRGB8888 = 1 /* 32-bit XRGB8888 pixels; pitch is in bytes. */
};

/*
 * Versioned userspace-facing metadata contract.
 * - version must equal PROSENJIT_DISPLAY_ABI_VERSION.
 * - width/height are non-zero pixel dimensions.
 * - pitch is bytes per row and must be >= width * 4 for XRGB8888.
 * - framebuffer_bytes must cover pitch * height.
 *
 * Security boundary: framebuffer is not automatically a userspace pointer.
 * A kernel implementation must not expose or dereference this address from
 * an untrusted process until a separate, permission-checked mapping/handle
 * contract exists. Treat it as metadata only at this ABI stage.
 */
struct pros_display_info {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint64_t framebuffer;
    uint64_t framebuffer_bytes;
};
struct pros_display_event { uint32_t type,flags; uint64_t timestamp,value0,value1; };
#define PROS_DISPLAY_EVENT_NONE 0u
#define PROS_DISPLAY_EVENT_RESIZE 1u
#define PROS_DISPLAY_EVENT_HOTPLUG 2u
#define PROS_DISPLAY_EVENT_VSYNC 3u
#define PROS_DISPLAY_EVENT_DEVICE_LOST 4u
#endif

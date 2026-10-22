#ifndef PROSENJIT_FRAMEBUFFER_H
#define PROSENJIT_FRAMEBUFFER_H

#include <stdint.h>
#include "hw_stack.h"

#define PROS_FRAMEBUFFER_ENDPOINT_ID 0xF0000001u
#define PROS_FRAMEBUFFER_ABI_VERSION 1u

struct pros_framebuffer_info {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t endpoint_id;
    uint64_t size;
};

/* Optional Multiboot2 RGB framebuffer backend; no GPU modesetting is performed. */
int pros_framebuffer_init(uint32_t boot_magic, uint64_t boot_info_address);
int pros_framebuffer_get_info(struct pros_framebuffer_info *out);
int pros_framebuffer_selftest(void);

#endif

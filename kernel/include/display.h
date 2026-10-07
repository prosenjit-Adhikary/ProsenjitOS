#ifndef PROSENJIT_DISPLAY_H
#define PROSENJIT_DISPLAY_H

#include <stdint.h>
#include "driver.h"

#define PROS_DISPLAY_ABI_VERSION 1u
#define PROS_DISPLAY_BACKEND_MAGIC 0x50534442u /* PSDB */

#define PROS_DISPLAY_MAX_WIDTH 16384u
#define PROS_DISPLAY_MAX_HEIGHT 16384u

struct pros_display_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint64_t framebuffer;
    uint64_t framebuffer_size;
};

struct pros_display_backend {
    uint32_t magic;
    uint32_t abi_version;
    int (*get_info)(void *ctx, struct pros_display_info *info);
    int (*present)(void *ctx, const void *pixels, uint64_t size);
    void *ctx;
};

int pros_display_init(void);
int pros_display_bind(uint32_t device_id);
int pros_display_unbind(uint32_t device_id);
int pros_display_get_info(uint32_t device_id, struct pros_display_info *info);
int pros_display_present(uint32_t device_id, const void *pixels, uint64_t size);
int pros_display_selftest(void);

#endif

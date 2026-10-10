#include "framebuffer_device.h"
#include "display.h"
#include "paging.h"
#include "memory.h"
#include "driver.h"
#include <stddef.h>

#define MB2_BOOT_MAGIC 0x36D76289u
#define MB2_TAG_END 0u
#define MB2_TAG_FRAMEBUFFER 8u
#define MB2_FRAMEBUFFER_RGB 1u
#define FB_MAX_WIDTH 4096u
#define FB_MAX_HEIGHT 2160u
#define FB_MAX_BYTES (64ULL * 1024ULL * 1024ULL)
#define FB_PHYS_LIMIT (1ULL << 52)
#define FB_VIRTUAL_BASE 0xFFFF900000000000ULL
#define FB_PAGE_SIZE 4096ULL

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_framebuffer_tag {
    uint32_t type;
    uint32_t size;
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    uint8_t red_position;
    uint8_t red_mask;
    uint8_t green_position;
    uint8_t green_mask;
    uint8_t blue_position;
    uint8_t blue_mask;
} __attribute__((packed));

struct framebuffer_state {
    struct pros_kernel_display_info info;
    uint64_t mapped_base;
    uint64_t mapped_bytes;
    uint64_t mapped_page_count;
    uint8_t active;
};

static struct framebuffer_state fb_state;
static struct pros_display_backend fb_backend;

static int fb_info(void *ctx, struct pros_kernel_display_info *out)
{
    struct framebuffer_state *state = (struct framebuffer_state *)ctx;
    if (!state || !out || !state->active)
        return PROS_DRIVER_ENODEV;
    *out = state->info;
    return PROS_DRIVER_OK;
}

static int fb_present(void *ctx, const void *pixels, uint64_t size)
{
    struct framebuffer_state *state = (struct framebuffer_state *)ctx;
    const uint8_t *src = (const uint8_t *)pixels;
    volatile uint8_t *dst;
    if (!state || !state->active || !src || size == 0u ||
        size > state->info.framebuffer_size)
        return PROS_DRIVER_EINVAL;
    dst = (volatile uint8_t *)(uintptr_t)
          (state->mapped_base + (state->info.framebuffer & (FB_PAGE_SIZE - 1u)));
    for (uint64_t i = 0; i < size; ++i)
        dst[i] = src[i];
    return PROS_DRIVER_OK;
}

static int fb_probe(struct pros_driver_device *device)
{
    if (!device || !fb_state.active)
        return PROS_DRIVER_ENODEV;
    device->driver_data = &fb_backend;
    return PROS_DRIVER_OK;
}

static int fb_remove(struct pros_driver_device *device)
{
    if (!device)
        return PROS_DRIVER_EINVAL;
    device->driver_data = 0;
    return PROS_DRIVER_OK;
}

static const struct pros_driver_id fb_ids[] = {{
    PROS_DRIVER_BUS_PLATFORM, 0xFFFFu, 0xFFFFu, 0xFFu, 0xFFu, 0xFFu
}};
static const struct pros_driver_ops fb_ops = {fb_probe, fb_remove, 0, 0};
static const struct pros_driver fb_driver = {
    "multiboot2-framebuffer", "1.0.0", PROS_DRIVER_ABI_VERSION,
    PROS_DRIVER_FLAG_NATIVE, PROS_DRIVER_ORIGIN_NATIVE,
    fb_ids, 1u, &fb_ops, 0u, {0}
};
static struct pros_driver_device fb_device = {
    0u, PROS_DRIVER_BUS_PLATFORM, PROS_DEVICE_CLASS_DISPLAY,
    PROS_DEVICE_DISCOVERED,
    {PROS_DRIVER_BUS_PLATFORM, 0xFFFFu, 0xFFFFu, 0xFFu, 0xFFu, 0xFFu},
    "multiboot2-framebuffer", 0, 0, 0
};

static int framebuffer_tag_valid(const struct mb2_framebuffer_tag *tag,
                                 uint64_t *bytes_out)
{
    uint64_t bytes;
    if (!tag || !bytes_out ||
        tag->size < offsetof(struct mb2_framebuffer_tag, blue_mask) + 1u)
        return PROS_FBDEV_EINVAL;
    if (tag->framebuffer_type != MB2_FRAMEBUFFER_RGB)
        return PROS_FBDEV_ENOTSUP;
    if (tag->bpp != 32u || tag->width == 0u || tag->height == 0u ||
        tag->width > FB_MAX_WIDTH || tag->height > FB_MAX_HEIGHT)
        return PROS_FBDEV_ENOTSUP;
    if (tag->red_position != 16u || tag->red_mask != 8u ||
        tag->green_position != 8u || tag->green_mask != 8u ||
        tag->blue_position != 0u || tag->blue_mask != 8u)
        return PROS_FBDEV_ENOTSUP;
    if (tag->pitch < (uint64_t)tag->width * 4u)
        return PROS_FBDEV_EINVAL;
    bytes = (uint64_t)tag->pitch * tag->height;
    if (bytes == 0u || bytes > FB_MAX_BYTES ||
        tag->address < PROSENJIT_PHYS_LIMIT ||
        tag->address >= FB_PHYS_LIMIT || tag->address > FB_PHYS_LIMIT - bytes)
        return PROS_FBDEV_EINVAL;
    *bytes_out = bytes;
    return PROS_FBDEV_OK;
}

static int find_framebuffer_tag(uint32_t boot_info,
                                const struct mb2_framebuffer_tag **out,
                                uint64_t *bytes_out)
{
    uint32_t total_size;
    uint64_t current, end;
    if (!out || !bytes_out || boot_info == 0u ||
        (uint64_t)boot_info >= PROSENJIT_PHYS_LIMIT - 16u)
        return PROS_FBDEV_EINVAL;
    total_size = *(const uint32_t *)(uintptr_t)boot_info;
    if (total_size < 16u || total_size > (16u * 1024u * 1024u) ||
        (uint64_t)boot_info + total_size > PROSENJIT_PHYS_LIMIT)
        return PROS_FBDEV_EINVAL;
    current = (uint64_t)boot_info + 8u;
    end = (uint64_t)boot_info + total_size;
    while (current <= end && end - current >= sizeof(struct mb2_tag)) {
        const struct mb2_tag *tag = (const struct mb2_tag *)(uintptr_t)current;
        if (tag->type == MB2_TAG_END)
            return PROS_FBDEV_ENODEV;
        if (tag->size < sizeof(struct mb2_tag) || tag->size > end - current)
            return PROS_FBDEV_EINVAL;
        if (tag->type == MB2_TAG_FRAMEBUFFER) {
            const struct mb2_framebuffer_tag *fb =
                (const struct mb2_framebuffer_tag *)tag;
            int rc = framebuffer_tag_valid(fb, bytes_out);
            if (rc != PROS_FBDEV_OK)
                return rc;
            *out = fb;
            return PROS_FBDEV_OK;
        }
        current = (current + (uint64_t)tag->size + 7u) & ~7ULL;
    }
    return PROS_FBDEV_ENODEV;
}

static int map_framebuffer(uint64_t physical, uint64_t bytes)
{
    uint64_t page_start = physical & ~(FB_PAGE_SIZE - 1u);
    uint64_t offset = physical - page_start;
    uint64_t map_bytes = offset + bytes;
    uint64_t pages;
    if (map_bytes < bytes || map_bytes > FB_MAX_BYTES + FB_PAGE_SIZE)
        return PROS_FBDEV_EINVAL;
    pages = (map_bytes + FB_PAGE_SIZE - 1u) / FB_PAGE_SIZE;
    if (pages == 0u || FB_VIRTUAL_BASE > UINT64_MAX - pages * FB_PAGE_SIZE)
        return PROS_FBDEV_EINVAL;

    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t va = FB_VIRTUAL_BASE + i * FB_PAGE_SIZE;
        uint64_t pa = page_start + i * FB_PAGE_SIZE;
        if (paging_map_device_page(va, pa,
                            PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
            for (uint64_t j = 0; j < i; ++j)
                paging_unmap_page(FB_VIRTUAL_BASE + j * FB_PAGE_SIZE);
            return PROS_FBDEV_EIO;
        }
    }
    fb_state.mapped_base = FB_VIRTUAL_BASE;
    fb_state.mapped_bytes = map_bytes;
    fb_state.mapped_page_count = pages;
    return PROS_FBDEV_OK;
}

int pros_framebuffer_device_init(uint32_t boot_magic, uint32_t boot_info)
{
    const struct mb2_framebuffer_tag *tag = 0;
    uint64_t bytes = 0u;
    int rc;
    if (boot_magic != MB2_BOOT_MAGIC)
        return PROS_FBDEV_ENODEV;
    if (fb_state.active)
        return PROS_FBDEV_EIO;
    rc = find_framebuffer_tag(boot_info, &tag, &bytes);
    if (rc != PROS_FBDEV_OK)
        return rc;
    rc = map_framebuffer(tag->address, bytes);
    if (rc != PROS_FBDEV_OK)
        return rc;

    fb_state.info.width = tag->width;
    fb_state.info.height = tag->height;
    fb_state.info.pitch = tag->pitch;
    fb_state.info.format = PROS_DISPLAY_FORMAT_XRGB8888;
    fb_state.info.framebuffer = tag->address;
    fb_state.info.framebuffer_size = bytes;
    fb_state.active = 1u;
    fb_backend.magic = PROS_DISPLAY_BACKEND_MAGIC;
    fb_backend.abi_version = PROS_DISPLAY_ABI_VERSION;
    fb_backend.get_info = fb_info;
    fb_backend.present = fb_present;
    fb_backend.ctx = &fb_state;

    rc = pros_driver_register(&fb_driver);
    if (rc != PROS_DRIVER_OK)
        goto fail_mapping;
    rc = pros_device_register(&fb_device);
    if (rc != PROS_DRIVER_OK) {
        (void)pros_driver_unregister(&fb_driver);
        goto fail_mapping;
    }
    rc = pros_driver_bind(fb_device.id);
    if (rc != PROS_DRIVER_OK) {
        (void)pros_device_unregister(fb_device.id);
        (void)pros_driver_unregister(&fb_driver);
        goto fail_mapping;
    }
    rc = pros_display_bind(fb_device.id);
    if (rc != PROS_DRIVER_OK) {
        (void)pros_driver_unbind(fb_device.id);
        (void)pros_device_unregister(fb_device.id);
        (void)pros_driver_unregister(&fb_driver);
        goto fail_mapping;
    }
    return PROS_FBDEV_OK;

fail_mapping:
    fb_state.active = 0u;
    for (uint64_t i = 0; i < fb_state.mapped_page_count; ++i)
        paging_unmap_page(FB_VIRTUAL_BASE + i * FB_PAGE_SIZE);
    fb_state.mapped_page_count = 0u;
    fb_state.mapped_bytes = 0u;
    return PROS_FBDEV_EIO;
}

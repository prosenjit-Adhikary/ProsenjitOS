#include "boot_graphics.h"
#include "memory.h"

#define MULTIBOOT2_TAG_END         0u
#define MULTIBOOT2_TAG_FRAMEBUFFER 8u
#define MULTIBOOT2_FRAMEBUFFER_RGB 2u
#define PROS_GRAPHICS_PHYS_LIMIT   PROSENJIT_PHYS_LIMIT

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
};

static int validate_boot_range(const struct multiboot_tag_framebuffer *tag)
{
    uint64_t bytes;

    if (!tag || tag->size < sizeof(*tag))
        return PROS_DRIVER_EINVAL;
    if (tag->framebuffer_addr == 0 ||
        tag->framebuffer_addr >= PROS_GRAPHICS_PHYS_LIMIT)
        return PROS_DRIVER_EINVAL;
    if (tag->framebuffer_pitch == 0 || tag->framebuffer_width == 0 ||
        tag->framebuffer_height == 0)
        return PROS_DRIVER_EINVAL;
    if (tag->framebuffer_type != MULTIBOOT2_FRAMEBUFFER_RGB)
        return PROS_DRIVER_ENOTSUP;

    bytes = (uint64_t)tag->framebuffer_pitch * tag->framebuffer_height;
    if (bytes == 0 || bytes > PROS_GRAPHICS_PHYS_LIMIT ||
        tag->framebuffer_addr > PROS_GRAPHICS_PHYS_LIMIT - bytes)
        return PROS_DRIVER_EINVAL;

    return PROS_DRIVER_OK;
}

int pros_boot_graphics_from_multiboot2(uint64_t multiboot_info_address,
                                       struct pros_graphics_surface *surface)
{
    uint32_t total_size;
    uint64_t current;
    uint64_t end;

    if (!surface || multiboot_info_address == 0)
        return PROS_DRIVER_EINVAL;

    total_size = *(const uint32_t *)(uintptr_t)multiboot_info_address;
    if (total_size < 16u || total_size > (16u * 1024u * 1024u))
        return PROS_DRIVER_EINVAL;

    current = multiboot_info_address + 8ULL;
    end = multiboot_info_address + (uint64_t)total_size;
    while (current + 8ULL <= end) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(uintptr_t)current;

        if (tag->type == MULTIBOOT2_TAG_END)
            break;
        if (tag->size < 8u || current + tag->size > end)
            return PROS_DRIVER_EINVAL;

        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
            const struct multiboot_tag_framebuffer *fb =
                (const struct multiboot_tag_framebuffer *)tag;
            if (validate_boot_range(fb) != PROS_DRIVER_OK)
                return validate_boot_range(fb);

            surface->framebuffer = (uintptr_t)fb->framebuffer_addr;
            surface->width = fb->framebuffer_width;
            surface->height = fb->framebuffer_height;
            surface->pitch = fb->framebuffer_pitch;
            surface->bpp = fb->framebuffer_bpp;
            return pros_graphics_validate_surface(surface);
        }

        current = (current + (uint64_t)tag->size + 7ULL) & ~7ULL;
    }

    return PROS_DRIVER_ENODEV;
}

int pros_boot_graphics_selftest(void)
{
    uint8_t buffer[128] = {0};
    struct multiboot_tag_framebuffer *fb =
        (struct multiboot_tag_framebuffer *)(void *)(buffer + 8);
    struct pros_graphics_surface surface;

    *(uint32_t *)(void *)buffer = 128u;
    fb->type = MULTIBOOT2_TAG_FRAMEBUFFER;
    fb->size = sizeof(*fb);
    fb->framebuffer_addr = 0x200000u;
    fb->framebuffer_pitch = 640u * 4u;
    fb->framebuffer_width = 640u;
    fb->framebuffer_height = 1u;
    fb->framebuffer_bpp = 32u;
    fb->framebuffer_type = MULTIBOOT2_FRAMEBUFFER_RGB;

    *(uint32_t *)(void *)(buffer + 8u + sizeof(*fb)) = MULTIBOOT2_TAG_END;
    *(uint32_t *)(void *)(buffer + 12u + sizeof(*fb)) = 8u;

    if (pros_boot_graphics_from_multiboot2((uint64_t)(uintptr_t)buffer,
                                            &surface) != PROS_DRIVER_OK)
        return -1;
    if (surface.framebuffer != 0x200000u || surface.width != 640u ||
        surface.height != 1u || surface.pitch != 2560u ||
        surface.bpp != 32u)
        return -2;

    return PROS_DRIVER_OK;
}

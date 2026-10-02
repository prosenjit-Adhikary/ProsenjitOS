#include "boot_graphics.h"
#include "memory.h"

#define MULTIBOOT2_TAG_END         0u
#define MULTIBOOT2_TAG_FRAMEBUFFER 8u
#define MULTIBOOT2_FRAMEBUFFER_RGB 1u
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

static struct pros_graphics_surface bound_surface;
static const struct pros_display_ops boot_display_ops;

static int boot_display_probe(struct pros_driver_device *device)
{
    if (!device || !device->bus_data)
        return PROS_DRIVER_EINVAL;
    bound_surface = *(const struct pros_graphics_surface *)device->bus_data;
    if (pros_graphics_validate_surface(&bound_surface) != PROS_DRIVER_OK)
        return PROS_DRIVER_EINVAL;
    device->driver_data = (void *)&boot_display_ops;
    return PROS_DRIVER_OK;
}

static int boot_display_remove(struct pros_driver_device *device)
{
    if (!device)
        return PROS_DRIVER_EINVAL;
    device->driver_data = 0;
    return PROS_DRIVER_OK;
}

static const struct pros_driver_id boot_display_ids[] = {
    { PROS_DRIVER_BUS_PLATFORM, 0u, 0u, 0x03u, 0u, 0u }
};

static const struct pros_driver_ops boot_display_driver_ops = {
    .probe = boot_display_probe,
    .remove = boot_display_remove,
    .suspend = 0,
    .resume = 0
};

static const struct pros_driver boot_display_driver = {
    .name = "multiboot2-framebuffer",
    .version = "1.0",
    .abi_version = PROS_DRIVER_ABI_VERSION,
    .flags = PROS_DRIVER_FLAG_NATIVE,
    .origin = PROS_DRIVER_ORIGIN_NATIVE,
    .ids = boot_display_ids,
    .id_count = 1u,
    .ops = &boot_display_driver_ops
};

static int boot_display_get_mode(struct pros_driver_device *device,
                                 struct pros_display_mode *mode)
{
    if (!device || !mode || device->driver_data == 0)
        return PROS_DRIVER_EINVAL;
    *mode = (struct pros_display_mode){
        .width = bound_surface.width,
        .height = bound_surface.height,
        .pitch = bound_surface.pitch,
        .bpp = bound_surface.bpp,
        .framebuffer = bound_surface.framebuffer
    };
    return PROS_DRIVER_OK;
}

static int boot_display_present(struct pros_driver_device *device)
{
    (void)device;
    /* The boot framebuffer is a memory surface; no hardware flip is defined. */
    return PROS_DRIVER_ENOTSUP;
}

static const struct pros_display_ops boot_display_ops = {
    .get_mode = boot_display_get_mode,
    .present = boot_display_present
};

int pros_boot_graphics_register_driver(void)
{
    return pros_driver_register(&boot_display_driver);
}

int pros_boot_graphics_bind(const struct pros_graphics_surface *surface,
                            struct pros_driver_device *device)
{
    if (!surface || !device ||
        pros_graphics_validate_surface(surface) != PROS_DRIVER_OK)
        return PROS_DRIVER_EINVAL;

    bound_surface = *surface;
    *device = (struct pros_driver_device){
        .id = 0u,
        .bus = PROS_DRIVER_BUS_PLATFORM,
        .class_id = PROS_DEVICE_CLASS_DISPLAY,
        .state = PROS_DEVICE_DISCOVERED,
        .hwid = { .bus = PROS_DRIVER_BUS_PLATFORM,
                  .class_code = 0x03u },
        .name = "multiboot2-framebuffer",
        .driver = 0,
        .driver_data = 0,
        .bus_data = &bound_surface
    };
    return PROS_DRIVER_OK;
}

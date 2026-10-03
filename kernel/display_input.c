#include "display_input.h"
#include "paging.h"

#define MULTIBOOT2_TAG_END 0u
#define MULTIBOOT2_TAG_FRAMEBUFFER 8u
#define MULTIBOOT2_FRAMEBUFFER_RGB 1u

#define PROS_DISPLAY_FB_VA 0xFFFF900000000000ULL

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

struct display_provider {
    struct pros_display_device display;
    struct pros_driver_device device;
    uint8_t registered;
};

static struct display_provider provider;

static int checked_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (!out || UINT64_MAX - a < b)
        return -1;
    *out = a + b;
    return 0;
}

static int framebuffer_size(const struct multiboot_tag_framebuffer *tag,
                            uint64_t *size_out)
{
    uint64_t size;

    if (!tag || !size_out || tag->framebuffer_pitch == 0u ||
        tag->framebuffer_height == 0u)
        return -1;

    if ((uint64_t)tag->framebuffer_pitch >
        UINT64_MAX / (uint64_t)tag->framebuffer_height)
        return -1;

    size = (uint64_t)tag->framebuffer_pitch *
           (uint64_t)tag->framebuffer_height;
    if (size == 0u)
        return -1;

    *size_out = size;
    return 0;
}

static int framebuffer_map(uint64_t physical, uint64_t size,
                           uintptr_t *mapped_out)
{
    uint64_t first;
    uint64_t last;
    uint64_t offset;
    uint64_t page_count;

    if (!mapped_out || size == 0u)
        return PROS_DISPLAY_EINVAL;

    first = physical & ~0xFFFULL;
    if (checked_add_u64(physical, size, &last) != 0)
        return PROS_DISPLAY_EIO;
    if (last == 0u)
        return PROS_DISPLAY_EIO;
    last = (last + 0xFFFULL) & ~0xFFFULL;
    if (last < first)
        return PROS_DISPLAY_EIO;

    page_count = (last - first) / 0x1000ULL;
    if (page_count == 0u || page_count > (UINT64_MAX / 0x1000ULL))
        return PROS_DISPLAY_EIO;

    for (uint64_t i = 0u; i < page_count; ++i) {
        uint64_t pa = first + i * 0x1000ULL;
        uint64_t va = PROS_DISPLAY_FB_VA + i * 0x1000ULL;
        if (va < PROS_DISPLAY_FB_VA ||
            paging_map_page(va, pa, PROSENJIT_PAGE_WRITABLE |
                                   PROSENJIT_PAGE_NX) != 0)
            return PROS_DISPLAY_EIO;
    }

    offset = physical - first;
    *mapped_out = (uintptr_t)(PROS_DISPLAY_FB_VA + offset);
    return PROS_DISPLAY_OK;
}

static int display_probe(struct pros_driver_device *device)
{
    struct display_provider *p;

    if (!device || !device->bus_data ||
        device->class_id != PROS_DEVICE_CLASS_DISPLAY)
        return PROS_DRIVER_EINVAL;

    p = (struct display_provider *)device->bus_data;
    if (p->display.framebuffer == 0u || p->display.width == 0u ||
        p->display.height == 0u || p->display.pitch == 0u)
        return PROS_DRIVER_EINVAL;

    p->display.driver_device = device;
    p->display.active = 1u;
    device->driver_data = &p->display;
    return PROS_DRIVER_OK;
}

static int display_remove(struct pros_driver_device *device)
{
    struct display_provider *p;

    if (!device || !device->driver_data)
        return PROS_DRIVER_ENODEV;

    p = (struct display_provider *)device->bus_data;
    if (p)
        p->display.active = 0u;
    device->driver_data = 0;
    return PROS_DRIVER_OK;
}

static const struct pros_driver_id display_ids[] = {
    { PROS_DRIVER_BUS_PLATFORM, 0u, 0u, 0x03u, 0u, 0u }
};

static const struct pros_driver_ops display_ops = {
    display_probe, display_remove, 0, 0
};

static const struct pros_driver display_driver = {
    "multiboot2-framebuffer",
    "1.0",
    PROS_DRIVER_ABI_VERSION,
    PROS_DRIVER_FLAG_NATIVE,
    PROS_DRIVER_ORIGIN_NATIVE,
    display_ids,
    1u,
    &display_ops
};

int pros_display_init(struct pros_display_device *d,
                       uint32_t width, uint32_t height, uint32_t pitch,
                       uint8_t bpp, uint8_t framebuffer_type,
                       uint64_t framebuffer_phys, uintptr_t framebuffer,
                       uint64_t framebuffer_size)
{
    uint64_t minimum_pitch;

    if (!d || width == 0u || height == 0u ||
        width > PROS_DISPLAY_MAX_WIDTH || height > PROS_DISPLAY_MAX_HEIGHT ||
        pitch == 0u || framebuffer_phys == 0u || framebuffer == 0u ||
        framebuffer_size == 0u)
        return PROS_DISPLAY_EINVAL;

    if (bpp != 32u || framebuffer_type != MULTIBOOT2_FRAMEBUFFER_RGB)
        return PROS_DISPLAY_ENOTSUP;

    minimum_pitch = (uint64_t)width * 4ULL;
    if (minimum_pitch > UINT32_MAX || (uint64_t)pitch < minimum_pitch)
        return PROS_DISPLAY_EINVAL;

    if ((uint64_t)pitch * (uint64_t)height > framebuffer_size)
        return PROS_DISPLAY_EINVAL;

    d->width = width;
    d->height = height;
    d->pitch = pitch;
    d->bpp = bpp;
    d->framebuffer_type = framebuffer_type;
    d->active = 0u;
    d->framebuffer = framebuffer;
    d->framebuffer_phys = framebuffer_phys;
    d->framebuffer_size = framebuffer_size;
    d->driver_device = 0;
    return PROS_DISPLAY_OK;
}

int pros_display_set_active(struct pros_display_device *d, uint8_t active)
{
    if (!d)
        return PROS_DISPLAY_EINVAL;
    d->active = active ? 1u : 0u;
    return PROS_DISPLAY_OK;
}

int pros_display_get_mode(const struct pros_display_device *d,
                           struct pros_display_mode *mode)
{
    if (!d || !mode || !d->active)
        return PROS_DISPLAY_EINVAL;

    mode->width = d->width;
    mode->height = d->height;
    mode->pitch = d->pitch;
    mode->bpp = d->bpp;
    mode->framebuffer_type = d->framebuffer_type;
    mode->framebuffer = d->framebuffer;
    mode->framebuffer_phys = d->framebuffer_phys;
    mode->framebuffer_size = d->framebuffer_size;
    return PROS_DISPLAY_OK;
}

int pros_display_present(const struct pros_display_device *d)
{
    if (!d || !d->active)
        return PROS_DISPLAY_EINVAL;

    /*
     * Multiboot2 handed us the scanout framebuffer itself. There is no
     * separate page-flip/commit operation to invoke, so claiming a hardware
     * flip here would be false. Callers render directly into the validated
     * mapped surface.
     */
    return PROS_DISPLAY_ENOTSUP;
}

int pros_display_input_selftest(void)
{
    struct pros_display_device d;
    struct pros_display_mode mode;

    if (pros_display_init(&d, 2u, 2u, 8u, 32u,
                           MULTIBOOT2_FRAMEBUFFER_RGB,
                           0x200000ULL, (uintptr_t)0xFFFF900000002000ULL,
                           16u) != PROS_DISPLAY_OK)
        return PROS_DISPLAY_EINVAL;

    if (pros_display_set_active(&d, 1u) != PROS_DISPLAY_OK ||
        pros_display_get_mode(&d, &mode) != PROS_DISPLAY_OK ||
        mode.width != 2u || mode.height != 2u || mode.pitch != 8u ||
        mode.bpp != 32u || mode.framebuffer != d.framebuffer ||
        pros_display_present(&d) != PROS_DISPLAY_ENOTSUP)
        return PROS_DISPLAY_EINVAL;

    return PROS_DISPLAY_OK;
}

int pros_display_bind_multiboot(uint64_t multiboot_info_address,
                                struct pros_display_device **display_out)
{
    uint32_t total_size;
    uint64_t current;
    uint64_t end;
    const struct multiboot_tag_framebuffer *found = 0;
    uint64_t fb_size;
    uintptr_t mapped;
    int rc;

    if (display_out)
        *display_out = 0;
    if (multiboot_info_address == 0u)
        return PROS_DISPLAY_ENODEV;

    total_size = *(const uint32_t *)(uintptr_t)multiboot_info_address;
    if (total_size < 16u || total_size > (16u * 1024u * 1024u))
        return PROS_DISPLAY_EINVAL;

    current = multiboot_info_address + 8ULL;
    end = multiboot_info_address + (uint64_t)total_size;
    while (current + 8ULL <= end) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(uintptr_t)current;

        if (tag->type == MULTIBOOT2_TAG_END)
            break;
        if (tag->size < 8u || current + (uint64_t)tag->size > end)
            return PROS_DISPLAY_EINVAL;

        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
            if (tag->size < sizeof(struct multiboot_tag_framebuffer))
                return PROS_DISPLAY_EINVAL;
            found = (const struct multiboot_tag_framebuffer *)tag;
            break;
        }

        current = (current + (uint64_t)tag->size + 7ULL) & ~7ULL;
    }

    if (!found)
        return PROS_DISPLAY_ENODEV;
    if (found->framebuffer_type != MULTIBOOT2_FRAMEBUFFER_RGB ||
        found->framebuffer_bpp != 32u ||
        found->framebuffer_addr == 0u)
        return PROS_DISPLAY_ENOTSUP;
    if (framebuffer_size(found, &fb_size) != 0)
        return PROS_DISPLAY_EINVAL;

    if (checked_add_u64(found->framebuffer_addr, fb_size, &current) != 0)
        return PROS_DISPLAY_EINVAL;

    mapped = 0u;
    rc = framebuffer_map(found->framebuffer_addr, fb_size, &mapped);
    if (rc != PROS_DISPLAY_OK)
        return rc;

    provider.display = (struct pros_display_device){0};
    rc = pros_display_init(&provider.display,
                           found->framebuffer_width,
                           found->framebuffer_height,
                           found->framebuffer_pitch,
                           found->framebuffer_bpp,
                           found->framebuffer_type,
                           found->framebuffer_addr,
                           mapped,
                           fb_size);
    if (rc != PROS_DISPLAY_OK)
        return rc;

    provider.device = (struct pros_driver_device){
        0u,
        PROS_DRIVER_BUS_PLATFORM,
        PROS_DEVICE_CLASS_DISPLAY,
        PROS_DEVICE_DISCOVERED,
        { PROS_DRIVER_BUS_PLATFORM, 0u, 0u, 0x03u, 0u, 0u },
        "multiboot2-framebuffer",
        0,
        0,
        &provider
    };

    if (!provider.registered) {
        rc = pros_driver_register(&display_driver);
        if (rc != PROS_DRIVER_OK && rc != PROS_DRIVER_EBUSY)
            return rc;
        rc = pros_device_register(&provider.device);
        if (rc != PROS_DRIVER_OK)
            return rc;
        provider.registered = 1u;
    }

    rc = pros_driver_bind(provider.device.id);
    if (rc != PROS_DRIVER_OK && rc != PROS_DRIVER_EBUSY)
        return rc;

    if (!provider.display.active ||
        provider.device.driver != &display_driver ||
        provider.device.state != PROS_DEVICE_BOUND)
        return PROS_DISPLAY_EIO;

    if (display_out)
        *display_out = &provider.display;
    return PROS_DISPLAY_OK;
}

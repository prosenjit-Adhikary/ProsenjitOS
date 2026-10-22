#include "framebuffer.h"
#include "paging.h"

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define MULTIBOOT2_TAG_END 0u
#define MULTIBOOT2_TAG_FRAMEBUFFER 8u
#define FRAMEBUFFER_VIRTUAL_BASE 0xFFFFA00000000000ULL
#define MULTIBOOT2_INFO_MAX (16u * 1024u * 1024u)
#define MULTIBOOT2_FRAMEBUFFER_RGB 1u

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};
struct multiboot_framebuffer_tag {
    uint32_t type, size;
    uint64_t address;
    uint32_t pitch, width, height;
    uint8_t bpp, framebuffer_type;
    uint16_t reserved;
    uint8_t red_position, red_mask_size;
    uint8_t green_position, green_mask_size;
    uint8_t blue_position, blue_mask_size;
} __attribute__((packed));

struct framebuffer_state {
    uint64_t virtual_base;
    uint64_t physical_page_base;
    uint64_t mapping_length;
    uint64_t byte_offset;
    uint64_t framebuffer_size;
    uint32_t width, height, pitch;
    uint8_t active;
};
static struct framebuffer_state framebuffer;

static int framebuffer_backend_init(void *context)
{
    const struct framebuffer_state *s = (const struct framebuffer_state *)context;
    return (s == &framebuffer && s->active) ? PROS_DRIVER_OK : PROS_DRIVER_ENODEV;
}

static int framebuffer_backend_transfer(void *context, const void *buffer, uint32_t length)
{
    const struct framebuffer_state *s = (const struct framebuffer_state *)context;
    if (s != &framebuffer || !s->active || !buffer ||
        length != (uint32_t)sizeof(struct pros_hw_display_frame))
        return PROS_DRIVER_EINVAL;

    const struct pros_hw_display_frame *frame =
        (const struct pros_hw_display_frame *)buffer;
    if (frame->version != PROS_HW_STACK_ABI_VERSION ||
        frame->width != s->width || frame->height != s->height ||
        frame->stride != s->pitch || frame->format != PROS_HW_DISPLAY_XRGB8888 ||
        frame->length != s->framebuffer_size || !frame->pixels)
        return PROS_DRIVER_EINVAL;

    volatile uint8_t *dst =
        (volatile uint8_t *)(uintptr_t)(s->virtual_base + s->byte_offset);
    const uint8_t *src = (const uint8_t *)frame->pixels;
    for (uint64_t i = 0u; i < s->framebuffer_size; ++i)
        dst[i] = src[i];
    return PROS_DRIVER_OK;
}

static int framebuffer_backend_shutdown(void *context)
{
    struct framebuffer_state *s = (struct framebuffer_state *)context;
    if (s != &framebuffer) return PROS_DRIVER_EINVAL;
    if (s->active) paging_unmap_mmio_range(s->virtual_base, s->mapping_length);
    s->active = 0u;
    s->virtual_base = 0u;
    s->physical_page_base = 0u;
    s->mapping_length = 0u;
    s->byte_offset = 0u;
    s->framebuffer_size = 0u;
    s->width = s->height = s->pitch = 0u;
    return PROS_DRIVER_OK;
}

static int parse_framebuffer(uint64_t address, struct multiboot_framebuffer_tag *out)
{
    if (address == 0u || !out) return PROS_DRIVER_EINVAL;
    const uint32_t total_size = *(const uint32_t *)(uintptr_t)address;
    if (total_size < 16u || total_size > MULTIBOOT2_INFO_MAX ||
        address > UINT64_MAX - (uint64_t)total_size)
        return PROS_DRIVER_EINVAL;
    const uint64_t end = address + total_size;
    uint64_t current = address + 8u;
    while (current <= end && end - current >= sizeof(struct multiboot_tag)) {
        const struct multiboot_tag *tag = (const struct multiboot_tag *)(uintptr_t)current;
        if (tag->type == MULTIBOOT2_TAG_END) break;
        if (tag->size < sizeof(struct multiboot_tag) ||
            (uint64_t)tag->size > end - current)
            return PROS_DRIVER_EINVAL;
        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
            if (tag->size < sizeof(struct multiboot_framebuffer_tag))
                return PROS_DRIVER_EINVAL;
            const struct multiboot_framebuffer_tag *fb =
                (const struct multiboot_framebuffer_tag *)tag;
            if (fb->framebuffer_type != MULTIBOOT2_FRAMEBUFFER_RGB ||
                fb->bpp != 32u || fb->width == 0u || fb->height == 0u ||
                fb->width > PROS_HW_FRAME_MAX_WIDTH ||
                fb->height > PROS_HW_FRAME_MAX_HEIGHT ||
                fb->red_position != 16u || fb->red_mask_size != 8u ||
                fb->green_position != 8u || fb->green_mask_size != 8u ||
                fb->blue_position != 0u || fb->blue_mask_size != 8u ||
                (uint64_t)fb->pitch < (uint64_t)fb->width * 4u)
                return PROS_DRIVER_ENOTSUP;
            uint64_t size = (uint64_t)fb->pitch * (uint64_t)fb->height;
            if (size == 0u || size > PROS_HW_TRANSFER_MAX ||
                fb->address > 0x000FFFFFFFFFFFFFULL ||
                size - 1u > 0x000FFFFFFFFFFFFFULL - fb->address)
                return PROS_DRIVER_EINVAL;
            *out = *fb;
            return PROS_DRIVER_OK;
        }
        uint64_t next = current + (uint64_t)tag->size;
        if (next > UINT64_MAX - 7u) return PROS_DRIVER_EINVAL;
        current = (next + 7u) & ~7ULL;
    }
    return PROS_DRIVER_ENOTSUP;
}

int pros_framebuffer_init(uint32_t boot_magic, uint64_t boot_info_address)
{
    if (boot_magic != MULTIBOOT2_BOOT_MAGIC) return PROS_DRIVER_ENOTSUP;
    if (framebuffer.active) return PROS_DRIVER_EBUSY;

    struct multiboot_framebuffer_tag fb;
    int rc = parse_framebuffer(boot_info_address, &fb);
    if (rc != PROS_DRIVER_OK) return rc;

    uint64_t page_base = fb.address & ~0xFFFULL;
    uint64_t offset = fb.address - page_base;
    uint64_t total = offset + (uint64_t)fb.pitch * fb.height;
    if (total < offset || total > UINT64_MAX - 0xFFFULL)
        return PROS_DRIVER_EINVAL;
    uint64_t map_length = (total + 0xFFFULL) & ~0xFFFULL;
    if (map_length == 0u || map_length > (256ULL * 1024ULL * 1024ULL))
        return PROS_DRIVER_EINVAL;

    rc = paging_map_mmio_range(FRAMEBUFFER_VIRTUAL_BASE, page_base, map_length);
    if (rc != 0) return PROS_DRIVER_EIO;

    framebuffer.virtual_base = FRAMEBUFFER_VIRTUAL_BASE;
    framebuffer.physical_page_base = page_base;
    framebuffer.mapping_length = map_length;
    framebuffer.byte_offset = offset;
    framebuffer.framebuffer_size = (uint64_t)fb.pitch * fb.height;
    framebuffer.width = fb.width;
    framebuffer.height = fb.height;
    framebuffer.pitch = fb.pitch;
    framebuffer.active = 1u;

    static const struct pros_hw_endpoint_ops ops = {
        framebuffer_backend_init, framebuffer_backend_transfer,
        framebuffer_backend_shutdown
    };
    rc = pros_hw_endpoint_register(PROS_FRAMEBUFFER_ENDPOINT_ID,
                                   PROS_HW_ENDPOINT_DISPLAY, &ops, &framebuffer);
    if (rc != PROS_DRIVER_OK) {
        if (framebuffer.active) (void)framebuffer_backend_shutdown(&framebuffer);
        return rc;
    }
    return PROS_DRIVER_OK;
}

int pros_framebuffer_get_info(struct pros_framebuffer_info *out)
{
    if (!out) return PROS_DRIVER_EINVAL;
    if (!framebuffer.active) return PROS_DRIVER_ENODEV;
    out->version = PROS_FRAMEBUFFER_ABI_VERSION;
    out->width = framebuffer.width;
    out->height = framebuffer.height;
    out->pitch = framebuffer.pitch;
    out->format = PROS_HW_DISPLAY_XRGB8888;
    out->endpoint_id = PROS_FRAMEBUFFER_ENDPOINT_ID;
    out->size = framebuffer.framebuffer_size;
    return PROS_DRIVER_OK;
}

/*
 * Exercise the complete boot-framebuffer -> endpoint -> backend presentation
 * path without allocating a second multi-megabyte frame. The current mapped
 * image is used as the source, so the byte-for-byte copy is visually neutral.
 * This validates dispatch and bounds, not visible pixels or GPU modesetting.
 */
int pros_framebuffer_selftest(void)
{
    struct pros_hw_endpoint_status status;
    if (!framebuffer.active) return PROS_DRIVER_ENODEV;
    if (framebuffer.framebuffer_size == 0u ||
        framebuffer.framebuffer_size > UINT32_MAX)
        return PROS_DRIVER_EINVAL;
    if (pros_hw_endpoint_status(PROS_FRAMEBUFFER_ENDPOINT_ID,
                                PROS_HW_ENDPOINT_DISPLAY, &status) != PROS_DRIVER_OK ||
        status.initialized == 0u)
        return PROS_DRIVER_ENODEV;

    const void *mapped_pixels = (const void *)(uintptr_t)
        (framebuffer.virtual_base + framebuffer.byte_offset);
    int rc = pros_hw_display_present(PROS_FRAMEBUFFER_ENDPOINT_ID,
                                     framebuffer.width, framebuffer.height,
                                     framebuffer.pitch, PROS_HW_DISPLAY_XRGB8888,
                                     mapped_pixels,
                                     (uint32_t)framebuffer.framebuffer_size);
    if (rc != PROS_DRIVER_OK) return rc;

    if (pros_hw_endpoint_status(PROS_FRAMEBUFFER_ENDPOINT_ID,
                                PROS_HW_ENDPOINT_DISPLAY, &status) != PROS_DRIVER_OK ||
        status.initialized == 0u || status.last_error != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    return PROS_DRIVER_OK;
}

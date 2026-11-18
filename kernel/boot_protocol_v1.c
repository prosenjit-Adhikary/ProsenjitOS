#include "pros_boot_protocol_v1_parser.h"

#include <string.h>

#define PROS_BOOT_V1_MAX_PHYS UINT64_C(0x000FFFFFFFFFFFFF)

static int add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}

static int range_inside(uint32_t offset, uint32_t count, uint32_t stride,
                        uint32_t total, uint32_t alignment)
{
    uint64_t bytes = (uint64_t)count * stride;
    uint64_t end = (uint64_t)offset + bytes;
    if (alignment != 0u && (offset % alignment) != 0u) return 0;
    return offset >= PROS_BOOT_V1_HEADER_SIZE && end <= total &&
           end >= offset;
}

static int overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    uint64_t ae, be;
    if (!add_u64(a, alen, &ae) || !add_u64(b, blen, &be)) return 1;
    return a < be && b < ae;
}

/* Require complete contiguous coverage by non-usable records. */
static int reserved_coverage(const struct pros_boot_v1_mmap_entry *map,
                             uint32_t count, uint64_t base, uint64_t length)
{
    uint64_t end;
    if (length == 0u || !add_u64(base, length, &end)) return 0;
    uint64_t cursor = base;

    for (uint32_t i = 0; i < count && cursor < end; ++i) {
        uint64_t entry_end;
        uint64_t entry_base = map[i].base;
        if (!add_u64(entry_base, map[i].length, &entry_end)) return 0;
        if (entry_end <= cursor) continue;
        if (entry_base > cursor || map[i].type == PROS_BOOT_V1_MEMORY_USABLE)
            return 0;
        if (entry_end > cursor) cursor = entry_end;
    }
    return cursor >= end;
}

static int region_overlap(uint32_t a, uint64_t alen,
                          uint32_t b, uint64_t blen)
{
    return overlaps(a, alen, b, blen);
}

int pros_boot_v1_validate(const void *allocation,
                          size_t accessible_size,
                          uint64_t allocation_phys,
                          uint64_t kernel_start,
                          uint64_t kernel_end,
                          const void *acpi_rsdp_mapping,
                          struct pros_boot_v1_view *view)
{
    if (allocation == NULL || view == NULL ||
        accessible_size < PROS_BOOT_V1_HEADER_SIZE ||
        allocation_phys >= PROS_BOOT_V1_EARLY_PHYS_LIMIT ||
        (allocation_phys & 7u) != 0u || kernel_end < kernel_start)
        return -1;

    memset(view, 0, sizeof(*view));

    const uint8_t *bytes = (const uint8_t *)allocation;
    const struct pros_boot_v1_header *h =
        (const struct pros_boot_v1_header *)allocation;

    if (h->magic != PROS_BOOT_V1_MAGIC ||
        h->version != PROS_BOOT_V1_VERSION ||
        h->header_size != PROS_BOOT_V1_HEADER_SIZE ||
        h->flags != 0u ||
        h->total_size < PROS_BOOT_V1_HEADER_SIZE ||
        h->total_size > PROS_BOOT_V1_MAX_TOTAL_SIZE ||
        h->total_size > accessible_size ||
        h->self_phys != allocation_phys)
        return -2;

    for (size_t i = 0; i < sizeof(h->reserved); ++i)
        if (h->reserved[i] != 0u) return -3;

    uint64_t allocation_end;
    if (!add_u64(allocation_phys, h->total_size, &allocation_end) ||
        allocation_end > PROS_BOOT_V1_EARLY_PHYS_LIMIT ||
        allocation_end > PROS_BOOT_V1_MAX_PHYS ||
        (kernel_end > kernel_start &&
         overlaps(allocation_phys, h->total_size,
                  kernel_start, kernel_end - kernel_start)))
        return -3;

    if (h->mmap_count == 0u ||
        h->mmap_entry_size != PROS_BOOT_V1_MMAP_ENTRY_SIZE ||
        !range_inside(h->mmap_offset, h->mmap_count,
                      h->mmap_entry_size, h->total_size, 8u))
        return -4;

    if (h->modules_count == 0u) {
        if (h->modules_offset != 0u || h->module_entry_size != 0u)
            return -5;
    } else if (h->module_entry_size != PROS_BOOT_V1_MODULE_ENTRY_SIZE ||
               !range_inside(h->modules_offset, h->modules_count,
                             h->module_entry_size, h->total_size, 8u)) {
        return -5;
    }

    if (h->boot_device_size == 0u) {
        if (h->boot_device_offset != 0u ||
            h->boot_device_kind != PROS_BOOT_V1_DEVICE_NONE)
            return -6;
    } else if (h->boot_device_kind != PROS_BOOT_V1_DEVICE_OPAQUE_V1 ||
               !range_inside(h->boot_device_offset, h->boot_device_size,
                             1u, h->total_size, 8u)) {
        return -6;
    }

    uint64_t mmap_bytes = (uint64_t)h->mmap_count * h->mmap_entry_size;
    uint64_t module_bytes = (uint64_t)h->modules_count * h->module_entry_size;
    if (region_overlap(0u, h->header_size, h->mmap_offset, mmap_bytes) ||
        (module_bytes != 0u &&
         (region_overlap(0u, h->header_size, h->modules_offset, module_bytes) ||
          region_overlap(h->mmap_offset, mmap_bytes,
                         h->modules_offset, module_bytes))) ||
        (h->boot_device_size != 0u &&
         (region_overlap(0u, h->header_size,
                         h->boot_device_offset, h->boot_device_size) ||
          region_overlap(h->mmap_offset, mmap_bytes,
                         h->boot_device_offset, h->boot_device_size) ||
          (module_bytes != 0u &&
           region_overlap(h->modules_offset, module_bytes,
                          h->boot_device_offset, h->boot_device_size)))))
        return -7;

    const struct pros_boot_v1_mmap_entry *map =
        (const struct pros_boot_v1_mmap_entry *)(bytes + h->mmap_offset);
    int usable_found = 0;
    uint64_t previous_end = 0u;
    for (uint32_t i = 0; i < h->mmap_count; ++i) {
        uint64_t end;
        if (map[i].length == 0u || map[i].attributes != 0u ||
            !add_u64(map[i].base, map[i].length, &end) ||
            end > PROS_BOOT_V1_MAX_PHYS ||
            (i != 0u && map[i].base < previous_end))
            return -8;
        previous_end = end;
        if (map[i].type == PROS_BOOT_V1_MEMORY_USABLE) usable_found = 1;
    }
    if (!usable_found ||
        !reserved_coverage(map, h->mmap_count,
                           allocation_phys, h->total_size))
        return -9;

    if (h->modules_count != 0u) {
        const struct pros_boot_v1_module_entry *mods =
            (const struct pros_boot_v1_module_entry *)(bytes + h->modules_offset);
        for (uint32_t i = 0; i < h->modules_count; ++i) {
            uint64_t end;
            if (mods[i].length == 0u ||
                !add_u64(mods[i].start, mods[i].length, &end) ||
                end > PROS_BOOT_V1_EARLY_PHYS_LIMIT ||
                (mods[i].flags & ~PROS_BOOT_V1_MODULE_OPTIONAL) != 0u ||
                (mods[i].type != PROS_BOOT_V1_MODULE_INITRAMFS &&
                 mods[i].type != PROS_BOOT_V1_MODULE_DRIVER_BUNDLE &&
                 (mods[i].flags & PROS_BOOT_V1_MODULE_OPTIONAL) == 0u) ||
                !reserved_coverage(map, h->mmap_count,
                                   mods[i].start, mods[i].length) ||
                overlaps(mods[i].start, mods[i].length,
                         allocation_phys, h->total_size) ||
                (kernel_end > kernel_start &&
                 overlaps(mods[i].start, mods[i].length,
                          kernel_start, kernel_end - kernel_start)))
                return -10;
            for (uint32_t j = 0; j < i; ++j)
                if (overlaps(mods[i].start, mods[i].length,
                             mods[j].start, mods[j].length))
                    return -10;
        }
    }

    /* Framebuffer is MMIO: validate arithmetic, never dereference it here. */
    if (h->framebuffer_phys == 0u && h->framebuffer_size == 0u &&
        h->framebuffer_width == 0u && h->framebuffer_height == 0u &&
        h->framebuffer_pitch == 0u && h->framebuffer_format == 0u) {
        /* absent */
    } else {
        uint64_t fb_end, minimum_pitch, minimum_size;
        if (h->framebuffer_phys == 0u || h->framebuffer_size == 0u ||
            h->framebuffer_width == 0u || h->framebuffer_height == 0u ||
            (h->framebuffer_format != PROS_BOOT_V1_FRAMEBUFFER_RGBX8888 &&
             h->framebuffer_format != PROS_BOOT_V1_FRAMEBUFFER_BGRX8888) ||
            !add_u64(0u, (uint64_t)h->framebuffer_width * 4u,
                     &minimum_pitch) ||
            h->framebuffer_pitch < minimum_pitch ||
            !add_u64(0u, (uint64_t)h->framebuffer_pitch *
                     h->framebuffer_height, &minimum_size) ||
            h->framebuffer_size < minimum_size ||
            !add_u64(h->framebuffer_phys, h->framebuffer_size, &fb_end) ||
            fb_end > PROS_BOOT_V1_MAX_PHYS)
            return -11;
    }

    /* The caller supplies a bounded mapping of the validated physical RSDP. */
    if (h->acpi_rsdp_phys == 0u && h->acpi_rsdp_size == 0u) {
        if (acpi_rsdp_mapping != NULL) return -12;
    } else {
        if (acpi_rsdp_mapping == NULL || h->acpi_rsdp_phys == 0u ||
            h->acpi_rsdp_size < 20u ||
            h->acpi_rsdp_size > PROS_BOOT_V1_EARLY_PHYS_LIMIT ||
            h->acpi_rsdp_phys >= PROS_BOOT_V1_EARLY_PHYS_LIMIT ||
            h->acpi_rsdp_size > PROS_BOOT_V1_EARLY_PHYS_LIMIT - h->acpi_rsdp_phys ||
            !reserved_coverage(map, h->mmap_count,
                               h->acpi_rsdp_phys, h->acpi_rsdp_size) ||
            overlaps(h->acpi_rsdp_phys, h->acpi_rsdp_size,
                     allocation_phys, h->total_size) ||
            (kernel_end > kernel_start &&
             overlaps(h->acpi_rsdp_phys, h->acpi_rsdp_size,
                      kernel_start, kernel_end - kernel_start)))
            return -12;

        if (h->modules_count != 0u) {
            const struct pros_boot_v1_module_entry *mods =
                (const struct pros_boot_v1_module_entry *)(bytes + h->modules_offset);
            for (uint32_t i = 0; i < h->modules_count; ++i)
                if (overlaps(h->acpi_rsdp_phys, h->acpi_rsdp_size,
                             mods[i].start, mods[i].length))
                    return -12;
        }

        const uint8_t *rsdp = (const uint8_t *)acpi_rsdp_mapping;
        static const uint8_t signature[8] = {
            'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '
        };
        if (memcmp(rsdp, signature, sizeof(signature)) != 0)
            return -12;
        uint8_t checksum = 0u;
        for (uint32_t i = 0; i < 20u; ++i) checksum = (uint8_t)(checksum + rsdp[i]);
        if (checksum != 0u) return -12;

        if (rsdp[15] >= 2u) {
            if (h->acpi_rsdp_size < 36u) return -12;
            uint32_t declared_length;
            memcpy(&declared_length, rsdp + 20u, sizeof(declared_length));
            if (declared_length < 36u || declared_length > h->acpi_rsdp_size)
                return -12;
            checksum = 0u;
            for (uint32_t i = 0; i < declared_length; ++i)
                checksum = (uint8_t)(checksum + rsdp[i]);
            if (checksum != 0u) return -12;
        }
    }

    view->header = h;
    view->memory_map = map;
    view->modules = h->modules_count != 0u
        ? (const struct pros_boot_v1_module_entry *)(bytes + h->modules_offset)
        : NULL;
    view->boot_device = h->boot_device_size != 0u
        ? bytes + h->boot_device_offset : NULL;
    return 0;
}

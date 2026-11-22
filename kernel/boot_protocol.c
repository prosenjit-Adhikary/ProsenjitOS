#include "abi/boot.h"

#include <stdint.h>

struct boot_region {
    uint32_t offset;
    uint32_t size;
};

static int add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}

static int region_valid(struct boot_region r, uint32_t total)
{
    return r.offset >= PROSENJIT_BOOT_V1_HEADER_SIZE &&
           r.size != 0u && r.offset <= total && r.size <= total - r.offset;
}

static int regions_overlap(struct boot_region a, struct boot_region b)
{
    uint64_t a_end = (uint64_t)a.offset + a.size;
    uint64_t b_end = (uint64_t)b.offset + b.size;
    return (uint64_t)a.offset < b_end && (uint64_t)b.offset < a_end;
}

static int all_zero(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        if (p[i] != 0u) return 0;
    return 1;
}

int pros_boot_v1_parse(const void *allocation, size_t available_bytes,
                       uint64_t expected_phys, struct pros_boot_v1_view *out)
{
    const uint8_t *bytes = (const uint8_t *)allocation;
    const struct pros_boot_v1_header *h;
    struct boot_region header_region = {0u, PROSENJIT_BOOT_V1_HEADER_SIZE};
    struct boot_region mmap_region, module_region, device_region;
    uint64_t allocation_end;
    uint64_t mmap_bytes;
    uint64_t module_bytes;
    int has_usable = 0;

    if (!bytes || !out || available_bytes < PROSENJIT_BOOT_V1_HEADER_SIZE)
        return -1;
    if ((expected_phys & 7u) != 0u) return -2;

    h = (const struct pros_boot_v1_header *)bytes;
    if (h->magic != PROSENJIT_BOOT_V1_MAGIC ||
        h->version != PROSENJIT_BOOT_V1_VERSION ||
        h->header_size != PROSENJIT_BOOT_V1_HEADER_SIZE ||
        h->flags != 0u || h->self_phys != expected_phys)
        return -3;
    if (h->total_size < PROSENJIT_BOOT_V1_HEADER_SIZE ||
        h->total_size > PROSENJIT_BOOT_V1_MAX_SIZE ||
        h->total_size > available_bytes)
        return -4;
    if (!add_u64(expected_phys, h->total_size, &allocation_end) ||
        expected_phys >= 0x40000000ULL || allocation_end > 0x40000000ULL)
        return -5;
    if (!all_zero(h->reserved, sizeof(h->reserved))) return -6;

    if (h->mmap_count == 0u ||
        h->mmap_entry_size != sizeof(struct pros_boot_v1_mmap_entry) ||
        h->mmap_offset == 0u || (h->mmap_offset & 7u) != 0u)
        return -7;
    mmap_bytes = (uint64_t)h->mmap_count * h->mmap_entry_size;
    if (mmap_bytes > UINT32_MAX) return -8;
    mmap_region.offset = h->mmap_offset;
    mmap_region.size = (uint32_t)mmap_bytes;
    if (!region_valid(mmap_region, h->total_size)) return -9;

    module_region.offset = h->modules_offset;
    module_region.size = 0u;
    if (h->modules_count == 0u) {
        if (h->modules_offset != 0u || h->module_entry_size != 0u)
            return -10;
    } else {
        if (h->modules_offset == 0u || (h->modules_offset & 7u) != 0u ||
            h->module_entry_size != sizeof(struct pros_boot_v1_module_entry))
            return -10;
        module_bytes = (uint64_t)h->modules_count * h->module_entry_size;
        if (module_bytes > UINT32_MAX) return -11;
        module_region.size = (uint32_t)module_bytes;
        if (!region_valid(module_region, h->total_size)) return -12;
        if (regions_overlap(header_region, module_region) ||
            regions_overlap(mmap_region, module_region))
            return -13;
    }

    device_region.offset = h->boot_device_offset;
    device_region.size = h->boot_device_size;
    if (h->boot_device_size == 0u) {
        if (h->boot_device_offset != 0u ||
            h->boot_device_kind != PROSENJIT_BOOT_DEVICE_NONE)
            return -14;
    } else {
        if (h->boot_device_offset == 0u ||
            h->boot_device_kind != PROSENJIT_BOOT_DEVICE_OPAQUE_V1 ||
            !region_valid(device_region, h->total_size))
            return -14;
        if (regions_overlap(header_region, device_region) ||
            regions_overlap(mmap_region, device_region) ||
            (module_region.size && regions_overlap(module_region, device_region)))
            return -15;
    }

    if (regions_overlap(header_region, mmap_region)) return -16;

    if (h->framebuffer_format == PROSENJIT_BOOT_FRAMEBUFFER_NONE) {
        if (h->framebuffer_phys || h->framebuffer_size ||
            h->framebuffer_width || h->framebuffer_height ||
            h->framebuffer_pitch)
            return -17;
    } else {
        uint64_t min_pitch, required_size, fb_end;
        if ((h->framebuffer_format != PROSENJIT_BOOT_FRAMEBUFFER_RGBX8888 &&
             h->framebuffer_format != PROSENJIT_BOOT_FRAMEBUFFER_BGRX8888) ||
            !h->framebuffer_phys || !h->framebuffer_size ||
            !h->framebuffer_width || !h->framebuffer_height)
            return -17;
        min_pitch = (uint64_t)h->framebuffer_width * 4u;
        required_size = (uint64_t)h->framebuffer_pitch * h->framebuffer_height;
        if (h->framebuffer_pitch < min_pitch ||
            required_size > h->framebuffer_size ||
            !add_u64(h->framebuffer_phys, h->framebuffer_size, &fb_end))
            return -18;
        (void)fb_end;
    }

    if (h->acpi_rsdp_phys == 0u) {
        if (h->acpi_rsdp_size != 0u) return -19;
    } else {
        uint64_t rsdp_end;
        if (h->acpi_rsdp_size < 20u ||
            !add_u64(h->acpi_rsdp_phys, h->acpi_rsdp_size, &rsdp_end))
            return -19;
        (void)rsdp_end;
    }

    if (h->modules_count) {
        const struct pros_boot_v1_module_entry *m =
            (const struct pros_boot_v1_module_entry *)(bytes + module_region.offset);
        for (uint32_t i = 0; i < h->modules_count; ++i) {
            uint64_t end;
            if (!m[i].start || !m[i].length ||
                !add_u64(m[i].start, m[i].length, &end) ||
                (m[i].flags & ~PROSENJIT_BOOT_MODULE_OPTIONAL) != 0u)
                return -20;
            if (m[i].type != PROSENJIT_BOOT_MODULE_INITRAMFS &&
                m[i].type != PROSENJIT_BOOT_MODULE_DRIVER_BUNDLE &&
                !(m[i].flags & PROSENJIT_BOOT_MODULE_OPTIONAL))
                return -21;
            (void)end;
        }
    }

    {
        const struct pros_boot_v1_mmap_entry *entries =
            (const struct pros_boot_v1_mmap_entry *)(bytes + mmap_region.offset);
        uint64_t previous_end = 0u;
        for (uint32_t i = 0; i < h->mmap_count; ++i) {
            uint64_t end;
            if (!entries[i].length || entries[i].attributes != 0u ||
                !add_u64(entries[i].base, entries[i].length, &end))
                return -22;
            if (i && entries[i].base < previous_end) return -23;
            if (entries[i].type == PROSENJIT_BOOT_MEMORY_USABLE)
                has_usable = 1;
            previous_end = end;
        }
    }
    if (!has_usable) return -24;

    out->header = h;
    out->mmap = (const struct pros_boot_v1_mmap_entry *)(bytes + mmap_region.offset);
    out->modules = h->modules_count
        ? (const struct pros_boot_v1_module_entry *)(bytes + module_region.offset)
        : (const struct pros_boot_v1_module_entry *)0;
    out->boot_device = h->boot_device_size ? bytes + device_region.offset : (const uint8_t *)0;
    return 0;
}

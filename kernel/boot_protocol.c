#include "abi/boot.h"

#include <stdint.h>

struct boot_region {
    uint32_t offset;
    uint32_t size;
};

#define BOOT_EARLY_PHYS_LIMIT 0x40000000ULL

static int add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}

static int phys_ranges_overlap(uint64_t a, uint64_t a_len,
                               uint64_t b, uint64_t b_len)
{
    uint64_t a_end, b_end;
    if (!a_len || !b_len ||
        !add_u64(a, a_len, &a_end) ||
        !add_u64(b, b_len, &b_end))
        return 0;
    return a < b_end && b < a_end;
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

static int range_reserved_by_map(
    const struct pros_boot_v1_mmap_entry *entries, uint32_t count,
    uint64_t base, uint64_t length)
{
    uint64_t end, cursor;
    if (!length || !add_u64(base, length, &end)) return 0;
    cursor = base;
    for (uint32_t i = 0; i < count && cursor < end; ++i) {
        uint64_t entry_end;
        if (!add_u64(entries[i].base, entries[i].length, &entry_end))
            return 0;
        if (entry_end <= cursor) continue;
        if (entries[i].base > cursor ||
            entries[i].type == PROSENJIT_BOOT_MEMORY_USABLE)
            return 0;
        cursor = entry_end < end ? entry_end : end;
    }
    return cursor == end;
}

static int all_zero(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        if (p[i] != 0u) return 0;
    return 1;
}

int pros_boot_v1_parse_with_limit(const void *allocation, size_t available_bytes,
                                  uint64_t expected_phys, uint64_t physical_limit,
                                  struct pros_boot_v1_view *out)
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
    if (physical_limit < BOOT_EARLY_PHYS_LIMIT || physical_limit > (1ULL << 52) ||
        (physical_limit & (physical_limit - 1u)) != 0u)
        return -2;
    if ((expected_phys & 7u) != 0u) return -2;

    h = (const struct pros_boot_v1_header *)bytes;
    if (h->magic != PROSENJIT_BOOT_V1_MAGIC ||
        h->version != PROSENJIT_BOOT_V1_VERSION ||
        h->header_size != PROSENJIT_BOOT_V1_HEADER_SIZE ||
        h->flags != 0u || h->self_phys != expected_phys)
        return -3;
    if (h->total_size < PROSENJIT_BOOT_V1_HEADER_SIZE ||
        h->total_size > PROSENJIT_BOOT_V1_MAX_SIZE ||
        (h->total_size & 7u) != 0u ||
        h->total_size > available_bytes)
        return -4;
    /* The handoff allocation itself is dereferenced through the bootstrap
     * identity map, so it remains inside the early mapping window. */
    if (!add_u64(expected_phys, h->total_size, &allocation_end) ||
        expected_phys >= BOOT_EARLY_PHYS_LIMIT ||
        allocation_end > BOOT_EARLY_PHYS_LIMIT ||
        allocation_end > physical_limit)
        return -5;
    if (!all_zero(h->reserved, sizeof(h->reserved))) return -6;

    if (h->mmap_count == 0u ||
        h->mmap_count > PROSENJIT_BOOT_V1_MAX_MMAP_ENTRIES ||
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
        if (h->modules_count > PROSENJIT_BOOT_V1_MAX_MODULES ||
            h->modules_offset == 0u || (h->modules_offset & 7u) != 0u ||
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
            !add_u64(h->framebuffer_phys, h->framebuffer_size, &fb_end) ||
            h->framebuffer_phys >= physical_limit ||
            fb_end > physical_limit ||
            phys_ranges_overlap(h->framebuffer_phys, h->framebuffer_size,
                                expected_phys, h->total_size))
            return -18;
        (void)fb_end;
    }

    if (h->acpi_rsdp_phys == 0u) {
        if (h->acpi_rsdp_size != 0u) return -19;
    } else {
        uint64_t rsdp_end;
        if (h->acpi_rsdp_size < 20u ||
            !add_u64(h->acpi_rsdp_phys, h->acpi_rsdp_size, &rsdp_end) ||
            h->acpi_rsdp_phys >= physical_limit ||
            rsdp_end > physical_limit ||
            phys_ranges_overlap(h->acpi_rsdp_phys, h->acpi_rsdp_size,
                                expected_phys, h->total_size) ||
            (h->framebuffer_size &&
             phys_ranges_overlap(h->acpi_rsdp_phys, h->acpi_rsdp_size,
                                 h->framebuffer_phys, h->framebuffer_size)))
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
                m[i].start >= physical_limit ||
                end > physical_limit ||
                phys_ranges_overlap(m[i].start, m[i].length,
                                    expected_phys, h->total_size) ||
                (m[i].flags & ~PROSENJIT_BOOT_MODULE_OPTIONAL) != 0u)
                return -20;
            if ((h->framebuffer_size &&
                 phys_ranges_overlap(m[i].start, m[i].length,
                                     h->framebuffer_phys, h->framebuffer_size)) ||
                (h->acpi_rsdp_size &&
                 phys_ranges_overlap(m[i].start, m[i].length,
                                     h->acpi_rsdp_phys, h->acpi_rsdp_size)))
                return -20;
            for (uint32_t j = 0; j < i; ++j) {
                if (phys_ranges_overlap(m[i].start, m[i].length,
                                        m[j].start, m[j].length))
                    return -20;
            }
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
                !add_u64(entries[i].base, entries[i].length, &end) ||
                entries[i].base >= physical_limit || end > physical_limit)
                return -22;
            if (i && entries[i].base < previous_end) return -23;
            if (entries[i].type == PROSENJIT_BOOT_MEMORY_USABLE)
                has_usable = 1;
            previous_end = end;
        }
    }
    if (!has_usable) return -24;

    /* Framebuffer MMIO is not required to be in the RAM map, but it must
     * never overlap a range advertised as allocator-usable memory. */
    if (h->framebuffer_size) {
        const struct pros_boot_v1_mmap_entry *entries =
            (const struct pros_boot_v1_mmap_entry *)(bytes + mmap_region.offset);
        for (uint32_t i = 0; i < h->mmap_count; ++i) {
            if (entries[i].type == PROSENJIT_BOOT_MEMORY_USABLE &&
                phys_ranges_overlap(h->framebuffer_phys, h->framebuffer_size,
                                    entries[i].base, entries[i].length))
                return -28;
        }
    }

    {
        const struct pros_boot_v1_mmap_entry *entries =
            (const struct pros_boot_v1_mmap_entry *)(bytes + mmap_region.offset);
        if (!range_reserved_by_map(entries, h->mmap_count,
                                   expected_phys, h->total_size))
            return -25;
        if (h->acpi_rsdp_size &&
            !range_reserved_by_map(entries, h->mmap_count,
                                   h->acpi_rsdp_phys, h->acpi_rsdp_size))
            return -26;
        if (h->modules_count) {
            const struct pros_boot_v1_module_entry *modules =
                (const struct pros_boot_v1_module_entry *)(bytes + module_region.offset);
            for (uint32_t i = 0; i < h->modules_count; ++i) {
                if (!range_reserved_by_map(entries, h->mmap_count,
                                           modules[i].start, modules[i].length))
                    return -27;
            }
        }
    }

    out->header = h;
    out->mmap = (const struct pros_boot_v1_mmap_entry *)(bytes + mmap_region.offset);
    out->modules = h->modules_count
        ? (const struct pros_boot_v1_module_entry *)(bytes + module_region.offset)
        : (const struct pros_boot_v1_module_entry *)0;
    out->boot_device = h->boot_device_size ? bytes + device_region.offset : (const uint8_t *)0;
    return 0;
}

/* Compatibility wrapper for callers that only have the legacy 1-GiB
 * bootstrap physical window. New boot producers must pass decoded MAXPHYADDR. */
int pros_boot_v1_parse(const void *allocation, size_t available_bytes,
                       uint64_t expected_phys, struct pros_boot_v1_view *out)
{
    return pros_boot_v1_parse_with_limit(allocation, available_bytes,
                                         expected_phys, BOOT_EARLY_PHYS_LIMIT,
                                         out);
}


/* Validate a copied ACPI Root System Description Pointer without
 * dereferencing its physical address. The caller must first map/copy the
 * complete range using the platform's validated physical-access policy. */
int pros_boot_v1_validate_rsdp(const void *rsdp, size_t available_bytes)
{
    static const uint8_t signature[8] = {
        'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '
    };
    const uint8_t *bytes = (const uint8_t *)rsdp;
    uint32_t declared_length;
    uint8_t sum = 0u;

    if (!bytes || available_bytes < 20u) return -1;
    for (size_t i = 0u; i < sizeof(signature); ++i)
        if (bytes[i] != signature[i]) return -2;

    /* ACPI revision 0 is the original 20-byte RSDP. Revision 1 is
     * reserved/unsupported; revision 2 and later use the extended form. */
    if (bytes[15] == 0u) {
        if (available_bytes != 20u) return -3;
        for (size_t i = 0u; i < 20u; ++i) sum = (uint8_t)(sum + bytes[i]);
        return sum == 0u ? 0 : -4;
    }
    if (bytes[15] == 1u || available_bytes < 36u) return -3;

    declared_length = (uint32_t)bytes[20] |
                      ((uint32_t)bytes[21] << 8) |
                      ((uint32_t)bytes[22] << 16) |
                      ((uint32_t)bytes[23] << 24);
    if (declared_length < 36u || declared_length != available_bytes)
        return -5;

    /* ACPI reserves bytes 33..35 in the revision-2+ RSDP; require zero. */
    if (bytes[33] != 0u || bytes[34] != 0u || bytes[35] != 0u)
        return -7;

    sum = 0u;
    for (size_t i = 0u; i < 20u; ++i) sum = (uint8_t)(sum + bytes[i]);
    if (sum != 0u) return -4;

    sum = 0u;
    for (size_t i = 0u; i < available_bytes; ++i)
        sum = (uint8_t)(sum + bytes[i]);
    return sum == 0u ? 0 : -6;
}


/* Pure decoder: the caller captures CPUID on the boot CPU before parsing
 * firmware-supplied physical ranges. Do not silently substitute 52 bits. */
int pros_boot_v1_decode_maxphyaddr(uint32_t max_extended_leaf,
                                   uint32_t extended_leaf_8_eax,
                                   uint32_t leaf_1_edx,
                                   uint8_t *physical_bits,
                                   uint64_t *physical_limit)
{
    uint32_t width;

    if (!physical_bits || !physical_limit) return -1;
    if (max_extended_leaf >= 0x80000008u) {
        width = extended_leaf_8_eax & 0xffu;
    } else {
        /* CPUID.01H:EDX.PAE is bit 6. Long mode alone is not enough
         * to infer a physical-address width when leaf 80000008 is absent. */
        if ((leaf_1_edx & (1u << 6)) == 0u) return -2;
        width = 36u;
    }

    if (width < 32u || width > 52u) return -3;
    *physical_bits = (uint8_t)width;
    *physical_limit = 1ULL << width;
    return 0;
}

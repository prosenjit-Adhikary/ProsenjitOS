#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA    0xCFCu

static inline void pci_out32(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t pci_in32(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_config_address(uint8_t bus, uint8_t slot,
                                   uint8_t function, uint8_t offset)
{
    return 0x80000000u |
           ((uint32_t)bus << 16) |
           ((uint32_t)(slot & 0x1Fu) << 11) |
           ((uint32_t)(function & 0x07u) << 8) |
           ((uint32_t)(offset & 0xFCu));
}

int pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint32_t *value)
{
    if (!value || slot >= 32u || function >= 8u ||
        (offset & 3u) != 0u)
        return PROS_PCI_EINVAL;

    pci_out32(PCI_CONFIG_ADDRESS,
              pci_config_address(bus, slot, function, offset));
    *value = pci_in32(PCI_CONFIG_DATA);
    return PROS_PCI_OK;
}

static int pci_read16(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint16_t *value)
{
    uint32_t word;
    if (!value || (offset & 1u) != 0u)
        return PROS_PCI_EINVAL;
    if (pci_config_read32(bus, slot, function, (uint8_t)(offset & 0xFCu),
                          &word) != PROS_PCI_OK)
        return PROS_PCI_EINVAL;
    *value = (uint16_t)((word >> ((offset & 2u) * 8u)) & 0xFFFFu);
    return PROS_PCI_OK;
}

int pci_scan(struct pros_pci_device *devices, uint32_t capacity,
             uint32_t *count)
{
    uint32_t found = 0u;

    if (!devices || !count || capacity == 0u)
        return PROS_PCI_EINVAL;

    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint32_t slot = 0u; slot < 32u; ++slot) {
            uint16_t vendor;
            if (pci_read16((uint8_t)bus, (uint8_t)slot, 0u, 0u, &vendor) !=
                    PROS_PCI_OK)
                return PROS_PCI_EINVAL;
            if (vendor == 0xFFFFu)
                continue;

            uint32_t header_word;
            if (pci_config_read32((uint8_t)bus, (uint8_t)slot, 0u, 0x0Cu,
                                  &header_word) != PROS_PCI_OK)
                return PROS_PCI_EINVAL;
            uint8_t header_type = (uint8_t)((header_word >> 16) & 0xFFu);
            uint32_t functions = (header_type & 0x80u) ? 8u : 1u;

            for (uint32_t function = 0u; function < functions; ++function) {
                uint32_t id;
                if (pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                      (uint8_t)function, 0u, &id) !=
                        PROS_PCI_OK)
                    return PROS_PCI_EINVAL;
                if ((uint16_t)(id & 0xFFFFu) == 0xFFFFu)
                    continue;

                uint32_t class_word;
                if (pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                      (uint8_t)function, 0x08u, &class_word) !=
                        PROS_PCI_OK)
                    return PROS_PCI_EINVAL;

                if (found >= capacity)
                    return PROS_PCI_EINVAL;

                devices[found].bus = (uint8_t)bus;
                devices[found].slot = (uint8_t)slot;
                devices[found].function = (uint8_t)function;
                devices[found].vendor_id = (uint16_t)(id & 0xFFFFu);
                devices[found].device_id = (uint16_t)(id >> 16);
                devices[found].class_code = (uint8_t)(class_word >> 24);
                devices[found].subclass = (uint8_t)(class_word >> 16);
                devices[found].prog_if = (uint8_t)(class_word >> 8);
                devices[found].header_type = header_type;
                ++found;
            }
        }
    }

    *count = found;
    return PROS_PCI_OK;
}

/*
 * Decode a BAR value without touching PCI configuration state. BAR sizing is
 * intentionally omitted: probing sizes requires writes to live device config
 * space, so callers must obtain the aperture length from a trusted platform or
 * device-specific source before mapping it.
 */
int pci_decode_bar(uint8_t index, uint8_t bar_count, uint32_t low,
                   uint32_t high, struct pros_pci_bar *out)
{
    uint64_t base;
    uint8_t type, width, prefetchable = 0u;
    if (!out || bar_count == 0u || bar_count > PROS_PCI_BAR_COUNT_MAX ||
        index >= bar_count)
        return PROS_PCI_EINVAL;
    if (low == 0u || low == UINT32_MAX)
        return PROS_PCI_ENODEV;

    if (low & 1u) {
        base = (uint64_t)(low & 0xFFFFFFFCu);
        if (base == 0u) return PROS_PCI_ENODEV;
        if (base > 0xFFFFu) return PROS_PCI_ENOTSUP;
        type = PROS_PCI_BAR_IO;
        width = 32u;
    } else {
        uint32_t memory_type = (low >> 1) & 3u;
        prefetchable = (uint8_t)((low >> 3) & 1u);
        if (memory_type == 0u) {
            base = (uint64_t)(low & 0xFFFFFFF0u);
            type = PROS_PCI_BAR_MMIO32;
            width = 32u;
        } else if (memory_type == 2u) {
            if ((uint32_t)index + 1u >= bar_count)
                return PROS_PCI_EINVAL;
            base = ((uint64_t)high << 32) | (uint64_t)(low & 0xFFFFFFF0u);
            type = PROS_PCI_BAR_MMIO64;
            width = 64u;
        } else {
            return PROS_PCI_ENOTSUP;
        }
        if (base == 0u) return PROS_PCI_ENODEV;
        if (base > 0x000FFFFFFFFFFFFFULL)
            return PROS_PCI_ENOTSUP;
    }

    out->base = base;
    out->index = index;
    out->type = type;
    out->width = width;
    out->prefetchable = prefetchable;
    return PROS_PCI_OK;
}

int pci_read_bar(const struct pros_pci_device *device, uint8_t index,
                 struct pros_pci_bar *out)
{
    uint8_t header_type, bar_count;
    uint32_t low, high = 0u;
    if (!device || !out || device->slot >= 32u || device->function >= 8u)
        return PROS_PCI_EINVAL;
    header_type = (uint8_t)(device->header_type & 0x7Fu);
    if (header_type == 0u) bar_count = 6u;
    else if (header_type == 1u) bar_count = 2u;
    else if (header_type == 2u) bar_count = 1u;
    else return PROS_PCI_ENOTSUP;
    if (index >= bar_count) return PROS_PCI_EINVAL;

    if (pci_config_read32(device->bus, device->slot, device->function,
                          (uint8_t)(0x10u + (uint32_t)index * 4u), &low) != PROS_PCI_OK)
        return PROS_PCI_EINVAL;
    if (low != 0u && low != UINT32_MAX && !(low & 1u) &&
        (((low >> 1) & 3u) == 2u)) {
        if ((uint32_t)index + 1u >= bar_count) return PROS_PCI_EINVAL;
        if (pci_config_read32(device->bus, device->slot, device->function,
                              (uint8_t)(0x14u + (uint32_t)index * 4u), &high) != PROS_PCI_OK)
            return PROS_PCI_EINVAL;
    }
    return pci_decode_bar(index, bar_count, low, high, out);
}

int pci_selftest(void)
{
    struct pros_pci_bar bar;
    if (pci_config_address(2u, 3u, 1u, 0x14u) != 0x80021914u)
        return -1;
    if (pci_config_address(255u, 31u, 7u, 0xFCu) != 0x80FFFFFCu)
        return -2;
    if (pci_decode_bar(0u, 6u, 0xFEB00008u, 0u, &bar) != PROS_PCI_OK ||
        bar.base != 0xFEB00000ULL || bar.type != PROS_PCI_BAR_MMIO32 ||
        !bar.prefetchable || bar.width != 32u)
        return -3;
    if (pci_decode_bar(0u, 6u, 0x0000C001u, 0u, &bar) != PROS_PCI_OK ||
        bar.base != 0xC000ULL || bar.type != PROS_PCI_BAR_IO)
        return -4;
    if (pci_decode_bar(2u, 6u, 0x00000004u, 0x00000012u, &bar) != PROS_PCI_OK ||
        bar.base != 0x1200000000ULL || bar.type != PROS_PCI_BAR_MMIO64 ||
        bar.index != 2u || bar.width != 64u)
        return -5;
    if (pci_decode_bar(5u, 6u, 0x00000004u, 0u, &bar) != PROS_PCI_EINVAL)
        return -6;
    if (pci_decode_bar(0u, 6u, 0u, 0u, &bar) != PROS_PCI_ENODEV)
        return -7;
    if (pci_decode_bar(0u, 6u, 0x00000002u, 0u, &bar) != PROS_PCI_ENOTSUP)
        return -8;
    if (pci_decode_bar(0u, 6u, 0x00000004u, 0xFFFFFFFFu, &bar) != PROS_PCI_ENOTSUP)
        return -9;
    return PROS_PCI_OK;
}

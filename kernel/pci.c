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

int pci_selftest(void)
{
    if (pci_config_address(2u, 3u, 1u, 0x14u) != 0x80021914u)
        return -1;
    if (pci_config_address(255u, 31u, 7u, 0xFCu) != 0x80FFFFFCu)
        return -1;
    return PROS_PCI_OK;
}

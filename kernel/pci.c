#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA    0xCFCu
#define PCI_COMMAND_IO     0x0001u
#define PCI_COMMAND_MEMORY 0x0002u
#define PCI_COMMAND_MASTER 0x0004u

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

int pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function,
                       uint8_t offset, uint32_t value)
{
    if (slot >= 32u || function >= 8u || (offset & 3u) != 0u)
        return PROS_PCI_EINVAL;

    pci_out32(PCI_CONFIG_ADDRESS,
              pci_config_address(bus, slot, function, offset));
    pci_out32(PCI_CONFIG_DATA, value);
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

/*
 * PCI command is the low halfword of offset 0x04; the high halfword is
 * write-one-to-clear status. Write a zero status halfword to avoid clearing
 * latched status bits while changing command flags.
 */
static int pci_write_command(const struct pros_pci_device *device,
                             uint16_t command)
{
    return pci_config_write32(device->bus, device->slot, device->function,
                              0x04u, (uint32_t)command);
}

static int pci_bar_decode(uint32_t original_low, uint32_t original_high,
                          uint32_t mask_low, uint32_t mask_high,
                          struct pros_pci_bar *bar)
{
    uint64_t mask;
    uint64_t size;

    if (!bar || original_low == 0u || original_low == 0xFFFFFFFFu)
        return PROS_PCI_EINVAL;

    bar->is_io = (uint8_t)(original_low & 1u);
    bar->is_64bit = 0u;
    bar->prefetchable = 0u;

    if (bar->is_io) {
        bar->base = (uint64_t)(original_low & ~3u);
        size = (uint64_t)(uint32_t)(~(mask_low & ~3u) + 1u);
    } else {
        uint32_t type = (original_low >> 1) & 3u;
        bar->prefetchable = (uint8_t)((original_low >> 3) & 1u);
        if (type == 0u) {
            bar->base = (uint64_t)(original_low & ~0xFu);
            size = (uint64_t)(uint32_t)(~(mask_low & ~0xFu) + 1u);
        } else if (type == 2u) {
            bar->is_64bit = 1u;
            bar->base = ((uint64_t)original_high << 32) |
                        (uint64_t)(original_low & ~0xFu);
            mask = ((uint64_t)mask_high << 32) |
                   (uint64_t)(mask_low & ~0xFu);
            size = ~mask + 1u;
        } else {
            return PROS_PCI_EUNSUPPORTED;
        }
    }

    if (size == 0u || (size & (size - 1u)) != 0u ||
        bar->base > UINT64_MAX - size)
        return PROS_PCI_EIO;
    bar->size = size;
    return PROS_PCI_OK;
}

int pci_get_bar(const struct pros_pci_device *device, uint8_t bar_index,
                struct pros_pci_bar *bar)
{
    uint32_t header_word, original_low, original_high = 0u;
    uint32_t mask_low, mask_high = 0u;
    uint16_t command;
    uint8_t offset;
    uint8_t is_64bit = 0u;
    int rc;

    if (!device || !bar || bar_index >= 6u ||
        device->slot >= 32u || device->function >= 8u)
        return PROS_PCI_EINVAL;
    if (pci_config_read32(device->bus, device->slot, device->function,
                          0x0Cu, &header_word) != PROS_PCI_OK)
        return PROS_PCI_EIO;
    if (((header_word >> 16) & 0x7Fu) > 1u)
        return PROS_PCI_EUNSUPPORTED;
    if (((header_word >> 16) & 0x7Fu) == 1u && bar_index >= 2u)
        return PROS_PCI_EINVAL;

    offset = (uint8_t)(0x10u + bar_index * 4u);
    if (pci_config_read32(device->bus, device->slot, device->function,
                          offset, &original_low) != PROS_PCI_OK)
        return PROS_PCI_EIO;
    if (original_low == 0u || original_low == 0xFFFFFFFFu)
        return PROS_PCI_ENOSPC;
    if ((original_low & 1u) == 0u &&
        (((original_low >> 1) & 3u) == 2u)) {
        if (bar_index >= 5u)
            return PROS_PCI_EINVAL;
        is_64bit = 1u;
        if (pci_config_read32(device->bus, device->slot, device->function,
                              (uint8_t)(offset + 4u),
                              &original_high) != PROS_PCI_OK)
            return PROS_PCI_EIO;
    }

    if (pci_read16(device->bus, device->slot, device->function, 0x04u,
                   &command) != PROS_PCI_OK)
        return PROS_PCI_EIO;
    rc = pci_write_command(device,
                           (uint16_t)(command &
                                      ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY)));
    if (rc != PROS_PCI_OK)
        return rc;

    (void)pci_config_write32(device->bus, device->slot, device->function,
                             offset, 0xFFFFFFFFu);
    if (is_64bit)
        (void)pci_config_write32(device->bus, device->slot, device->function,
                                 (uint8_t)(offset + 4u), 0xFFFFFFFFu);
    (void)pci_config_read32(device->bus, device->slot, device->function,
                            offset, &mask_low);
    if (is_64bit)
        (void)pci_config_read32(device->bus, device->slot, device->function,
                                (uint8_t)(offset + 4u), &mask_high);
    (void)pci_config_write32(device->bus, device->slot, device->function,
                             offset, original_low);
    if (is_64bit)
        (void)pci_config_write32(device->bus, device->slot, device->function,
                                 (uint8_t)(offset + 4u), original_high);
    (void)pci_write_command(device, command);

    return pci_bar_decode(original_low, original_high, mask_low, mask_high,
                          bar);
}

int pci_enable_memory_and_bus_master(const struct pros_pci_device *device)
{
    uint16_t command;
    if (!device || device->slot >= 32u || device->function >= 8u)
        return PROS_PCI_EINVAL;
    if (pci_read16(device->bus, device->slot, device->function, 0x04u,
                   &command) != PROS_PCI_OK)
        return PROS_PCI_EIO;
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    return pci_write_command(device, command);
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
    struct pros_pci_bar bar;
    if (pci_config_address(2u, 3u, 1u, 0x14u) != 0x80021914u)
        return -1;
    if (pci_config_address(255u, 31u, 7u, 0xFCu) != 0x80FFFFFCu)
        return -1;

    if (pci_bar_decode(0xFEBF0008u, 0u, 0xFFFFF008u, 0u, &bar) !=
            PROS_PCI_OK ||
        bar.base != 0xFEBF0000u || bar.size != 0x1000u ||
        bar.is_io || bar.is_64bit || !bar.prefetchable)
        return -2;
    if (pci_bar_decode(0x0000000Cu, 0x00000012u,
                       0xFFFFE00Cu, 0xFFFFFFFFu, &bar) != PROS_PCI_OK ||
        bar.base != 0x1200000000ULL || bar.size != 0x2000u ||
        bar.is_io || !bar.is_64bit || !bar.prefetchable)
        return -3;
    if (pci_bar_decode(0x0000C001u, 0u, 0xFFFFFF01u, 0u, &bar) !=
            PROS_PCI_OK ||
        bar.base != 0xC000u || bar.size != 0x100u || !bar.is_io)
        return -4;
    if (pci_bar_decode(0x00000002u, 0u, 0xFFFFF002u, 0u, &bar) !=
            PROS_PCI_EUNSUPPORTED)
        return -5;
    return PROS_PCI_OK;
}

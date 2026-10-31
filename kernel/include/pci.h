#ifndef PROSENJIT_PCI_H
#define PROSENJIT_PCI_H

#include <stdint.h>

#define PROS_PCI_OK 0
#define PROS_PCI_EINVAL -1
#define PROS_PCI_ENOSPC -2
#define PROS_PCI_EUNSUPPORTED -3
#define PROS_PCI_EIO -4
#define PROS_PCI_MAX_DEVICES 256u

struct pros_pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
};

struct pros_pci_bar {
    uint64_t base;
    uint64_t size;
    uint8_t is_io;
    uint8_t is_64bit;
    uint8_t prefetchable;
};

int pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint32_t *value);
int pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function,
                       uint8_t offset, uint32_t value);
int pci_scan(struct pros_pci_device *devices, uint32_t capacity,
             uint32_t *count);
/* Probe and restore a type-0/type-1 header BAR; disables decode while probing. */
int pci_get_bar(const struct pros_pci_device *device, uint8_t bar_index,
                struct pros_pci_bar *bar);
/* Enable the PCI memory-space and bus-master command bits for a device. */
int pci_enable_memory_and_bus_master(const struct pros_pci_device *device);
int pci_selftest(void);

#endif

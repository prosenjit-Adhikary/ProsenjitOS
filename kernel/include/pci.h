#ifndef PROSENJIT_PCI_H
#define PROSENJIT_PCI_H

#include <stdint.h>

#define PROS_PCI_OK 0
#define PROS_PCI_EINVAL -1
#define PROS_PCI_ENODEV -2
#define PROS_PCI_ENOTSUP -3
#define PROS_PCI_MAX_DEVICES 256u
#define PROS_PCI_BAR_COUNT_MAX 6u

enum pros_pci_bar_type {
    PROS_PCI_BAR_IO = 1,
    PROS_PCI_BAR_MMIO32 = 2,
    PROS_PCI_BAR_MMIO64 = 3
};

struct pros_pci_bar {
    uint64_t base;
    uint8_t index;
    uint8_t type;
    uint8_t width;
    uint8_t prefetchable;
};

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

int pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint32_t *value);
int pci_scan(struct pros_pci_device *devices, uint32_t capacity,
             uint32_t *count);
/* Read-only BAR inspection; no sizing writes or device enablement are performed. */
int pci_decode_bar(uint8_t index, uint8_t bar_count, uint32_t low,
                   uint32_t high, struct pros_pci_bar *out);
int pci_read_bar(const struct pros_pci_device *device, uint8_t index,
                 struct pros_pci_bar *out);
int pci_selftest(void);

#endif

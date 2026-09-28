#ifndef PROSENJIT_PCI_H
#define PROSENJIT_PCI_H

#include <stdint.h>

#define PROS_PCI_OK 0
#define PROS_PCI_EINVAL -1
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

int pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint32_t *value);
int pci_scan(struct pros_pci_device *devices, uint32_t capacity,
             uint32_t *count);
int pci_selftest(void);

#endif

#ifndef PROSENJIT_PCI_H
#define PROSENJIT_PCI_H

#include <stdint.h>

#define PROS_PCI_OK 0
#define PROS_PCI_EINVAL -1
#define PROS_PCI_ENOSPC -2
#define PROS_PCI_EUNSUPPORTED -3
#define PROS_PCI_EIO -4
#define PROS_PCI_ENODEV -5
#define PROS_PCI_ENOCAP -6
#define PROS_PCI_MAX_DEVICES 256u

#define PROS_PCI_USB_UHCI 1u
#define PROS_PCI_USB_OHCI 2u
#define PROS_PCI_USB_EHCI 3u
#define PROS_PCI_USB_XHCI 4u

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

/* Read-only PCI MSI/MSI-X capability metadata; no interrupt state is changed. */
struct pros_pci_interrupt_caps {
    uint8_t msi_present;
    uint8_t msi_enabled;
    uint8_t msi_64bit;
    uint8_t msi_mask_capable;
    uint16_t msi_max_vectors;
    uint8_t msix_present;
    uint8_t msix_enabled;
    uint8_t msix_function_mask;
    uint16_t msix_table_size;
    uint8_t msix_table_bir;
    uint32_t msix_table_offset;
    uint8_t msix_pba_bir;
    uint32_t msix_pba_offset;
};

struct pros_pci_mmio_bar {
    uint64_t physical_address;
    uint64_t virtual_address;
    uint64_t size;
};

int pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint32_t *value);
int pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function,
                       uint8_t offset, uint32_t value);
int pci_scan(struct pros_pci_device *devices, uint32_t capacity,
             uint32_t *count);
/* Find a conventional PCI capability; returns its config-space offset. */
int pci_find_capability(const struct pros_pci_device *device,
                        uint8_t capability_id, uint8_t *offset);
/* Read-only MSI/MSI-X discovery; this does not enable interrupt delivery. */
int pci_get_interrupt_capabilities(
    const struct pros_pci_device *device,
    struct pros_pci_interrupt_caps *caps);
/* Probe and restore a type-0/type-1 header BAR; disables decode while probing. */
int pci_get_bar(const struct pros_pci_device *device, uint8_t bar_index,
                struct pros_pci_bar *bar);
/* Enable memory decoding without granting bus-master DMA permission. */
int pci_enable_memory_space(const struct pros_pci_device *device);
/* Map a memory BAR uncached; this does not enable bus mastering or DMA. */
int pci_map_mmio_bar(const struct pros_pci_device *device, uint8_t bar_index,
                     struct pros_pci_mmio_bar *mapped);
void pci_unmap_mmio_bar(struct pros_pci_mmio_bar *mapped);
/* Toggle bus-master DMA permission while preserving other PCI command bits. */
int pci_enable_bus_master(const struct pros_pci_device *device);
int pci_disable_bus_master(const struct pros_pci_device *device);
/* Enable the PCI memory-space and bus-master command bits for a device. */
int pci_enable_memory_and_bus_master(const struct pros_pci_device *device);
/* Classify a PCI USB host controller by class/subclass/programming interface. */
int pci_usb_controller_type(const struct pros_pci_device *device,
                            uint8_t *controller_type);
int pci_selftest(void);

#endif

#ifndef PROSENJIT_XHCI_H
#define PROSENJIT_XHCI_H

#include <stdint.h>

#define PROS_XHCI_OK 0
#define PROS_XHCI_EINVAL -1
#define PROS_XHCI_EBOUNDS -2
#define PROS_XHCI_EUNSUPPORTED -3

/*
 * Validated xHCI capability-register snapshot. This parser does not start or
 * reset a controller, enable bus mastering, or perform DMA.
 */
struct pros_xhci_capabilities {
    uint16_t interface_version;
    uint8_t capability_length;
    uint8_t max_device_slots;
    uint16_t max_interrupters;
    uint8_t max_ports;
    uint8_t context_size;
    uint8_t address_64bit;
    uint32_t doorbell_offset;
    uint32_t runtime_offset;
    uint32_t extended_capability_offset;
};

int pros_xhci_capabilities_parse(const volatile void *mmio,
                                 uint64_t mmio_size,
                                 struct pros_xhci_capabilities *out);
int pros_xhci_selftest(void);

#endif

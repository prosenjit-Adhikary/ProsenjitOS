#ifndef PROSENJIT_USB_XHCI_H
#define PROSENJIT_USB_XHCI_H

#include <stdint.h>
#include "pci.h"

#define PROS_USB_XHCI_OK 0
#define PROS_USB_XHCI_EINVAL -1
#define PROS_USB_XHCI_EUNSUPPORTED -2
#define PROS_USB_XHCI_EBOUNDS -3
#define PROS_USB_XHCI_EIO -4
#define PROS_USB_XHCI_ENODEV -5
#define PROS_USB_XHCI_ETIMEOUT -6

struct pros_xhci_capabilities {
    uint16_t version;
    uint8_t capability_length;
    uint8_t max_slots;
    uint16_t max_interrupters;
    uint8_t max_ports;
    uint32_t operational_offset;
    uint32_t doorbell_offset;
    uint32_t runtime_offset;
    uint32_t extended_capabilities_offset;
};

struct pros_xhci_controller {
    struct pros_pci_device pci;
    struct pros_pci_mmio_bar bar;
    struct pros_xhci_capabilities capabilities;
    uint8_t initialized;
    uint8_t ownership_claimed;
};

/* Parse read-only xHCI capability registers from a bounded MMIO window. */
int pros_xhci_parse_capabilities(const volatile void *mmio,
                                 uint64_t bar_size,
                                 struct pros_xhci_capabilities *capabilities);
/* Probe BAR0 and cache validated register offsets; never enables bus mastering. */
int pros_xhci_probe(const struct pros_pci_device *device,
                    struct pros_xhci_controller *controller);
/* Walk the bounded xECP chain and return the legacy-support capability offset. */
int pros_xhci_find_legacy_support(const volatile void *mmio,
                                  uint64_t bar_size,
                                  uint32_t first_offset,
                                  uint32_t *legacy_offset);
/* Request BIOS ownership release; never clear the BIOS-owned semaphore. */
int pros_xhci_legacy_handoff(volatile void *mmio, uint64_t bar_size,
                             uint32_t capability_offset, uint32_t max_polls);
/* Resolve optional xECP legacy capability and claim OS ownership, bounded. */
int pros_xhci_claim_ownership(struct pros_xhci_controller *controller,
                              uint32_t max_polls);
void pros_xhci_detach(struct pros_xhci_controller *controller);
int pros_xhci_selftest(void);

#endif

#ifndef PROSENJIT_XHCI_H
#define PROSENJIT_XHCI_H

#include <stdint.h>

/* Parsed from a read-only snapshot of an xHCI capability register BAR.
 * This is validation/inspection only: it does not map MMIO or start hardware. */
struct pros_xhci_capabilities {
    uint16_t interface_version;
    uint8_t capability_length;
    uint8_t max_slots;
    uint16_t max_interrupters;
    uint8_t max_ports;
    uint8_t context_size_64;
    uint32_t doorbell_offset;
    uint32_t runtime_offset;
    uint32_t extended_capabilities_offset;
};

int pros_xhci_parse_capabilities(const uint8_t *bar, uint32_t bar_length,
                                 struct pros_xhci_capabilities *out);

#endif

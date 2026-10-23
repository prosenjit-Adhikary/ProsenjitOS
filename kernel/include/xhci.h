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

/* Validate the complete xHCI extended-capability linked list in a BAR
 * snapshot. first_offset == 0 means no list. count_out is written only on
 * success. This routine never accesses live MMIO. */
int pros_xhci_validate_extended_capabilities(const uint8_t *bar,
                                             uint32_t bar_length,
                                             uint32_t first_offset,
                                             uint32_t *count_out);

#endif

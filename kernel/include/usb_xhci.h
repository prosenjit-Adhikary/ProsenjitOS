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
    uint16_t scratchpad_buffers;
    uint16_t page_size_mask;
    uint8_t address_64bit;
    uint8_t context_size_64;
    uint32_t operational_offset;
    uint32_t doorbell_offset;
    uint32_t runtime_offset;
    uint32_t extended_capabilities_offset;
};

struct pros_xhci_transfer_event {
    uint64_t parameter;
    uint32_t residual_length;
    uint8_t completion_code;
    uint8_t slot_id;
    uint8_t endpoint_id;
    uint8_t event_data;
};

struct pros_xhci_trb;
struct pros_xhci_command_ring;
struct pros_usb_dma_buffer;
struct pros_xhci_erst;
struct pros_xhci_event_ring;

struct pros_xhci_controller {
    struct pros_pci_device pci;
    struct pros_pci_mmio_bar bar;
    struct pros_xhci_capabilities capabilities;
    uint8_t initialized;
    uint8_t ownership_claimed;
    uint8_t legacy_smi_disabled;
    uint8_t reset_complete;
};

/* Parse read-only xHCI capability registers from a bounded MMIO window. */
int pros_xhci_parse_capabilities(const volatile void *mmio,
                                 uint64_t bar_size,
                                 struct pros_xhci_capabilities *capabilities);
/* Clear/read back PCI bus mastering, then probe BAR0 without enabling DMA. */
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
/* Disable legacy SMI sources without acknowledging RW1C status bits. */
int pros_xhci_disable_legacy_smi(struct pros_xhci_controller *controller);
/* Halt and reset only after ownership and legacy SMI shutdown; bounded polls. */
int pros_xhci_halt_reset(struct pros_xhci_controller *controller,
                         uint32_t max_polls);
void pros_xhci_detach(struct pros_xhci_controller *controller);
int pros_xhci_runtime_access_selftest(void);
/* Program a single-segment event ring while keeping IMAN.IE disabled. */
int pros_xhci_program_interrupter_disabled(
    struct pros_xhci_controller *, uint16_t,
    const struct pros_xhci_erst *, const struct pros_xhci_event_ring *,
    uint16_t);
int pros_xhci_interrupter_program_selftest(void);
int pros_xhci_decode_transfer_event(const struct pros_xhci_trb *,
                                    struct pros_xhci_transfer_event *);
int pros_xhci_transfer_event_selftest(void);
int pros_xhci_acknowledge_event_ring(
    struct pros_xhci_controller *, uint16_t, const struct pros_xhci_erst *,
    const struct pros_xhci_event_ring *);
int pros_xhci_event_ring_ack_selftest(void);
/* Program DCBAAP, CONFIG and CRCR while the controller remains halted. */
int pros_xhci_program_command_path_disabled(
    struct pros_xhci_controller *, const struct pros_usb_dma_buffer *,
    const struct pros_xhci_command_ring *);
int pros_xhci_command_path_setup_selftest(void);
int pros_xhci_validate_dma_layout(
    const struct pros_xhci_controller *, const struct pros_usb_dma_buffer *,
    const struct pros_xhci_command_ring *, const struct pros_xhci_erst *,
    const struct pros_xhci_event_ring *);
int pros_xhci_dma_layout_selftest(void);
int pros_xhci_read_mfindex(const struct pros_xhci_controller *controller,
                           uint32_t *value);
int pros_xhci_read_interrupter32(
    const struct pros_xhci_controller *controller, uint16_t interrupter,
    uint32_t register_offset, uint32_t *value);
int pros_xhci_read_interrupter64(
    const struct pros_xhci_controller *controller, uint16_t interrupter,
    uint32_t register_offset, uint64_t *value);
int pros_xhci_selftest(void);

#endif

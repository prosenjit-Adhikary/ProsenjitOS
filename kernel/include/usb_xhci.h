#ifndef PROSENJIT_USB_XHCI_H
#define PROSENJIT_USB_XHCI_H

#include <stdint.h>
#include "pci.h"
#include "usb_dma.h"
#include "usb_msc.h"
#include "usb_xhci_ring.h"

#define PROS_USB_XHCI_OK 0
#define PROS_USB_XHCI_EINVAL -1
#define PROS_USB_XHCI_EUNSUPPORTED -2
#define PROS_USB_XHCI_EBOUNDS -3
#define PROS_USB_XHCI_EIO -4
#define PROS_USB_XHCI_ENODEV -5
#define PROS_USB_XHCI_ETIMEOUT -6
#define PROS_USB_XHCI_ESTART -7
#define PROS_USB_XHCI_ECOMPLETION -8

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


/*
 * Scratchpad array and buffers are DMA-owned until the controller is known
 * quiescent. The current allocator supports a single contiguous pool, so all
 * scratchpad pages are individually page-aligned within that pool.
 */
struct pros_xhci_scratchpad_set {
    struct pros_usb_dma_buffer pointer_array;
    struct pros_usb_dma_buffer buffer_pool;
    uint16_t count;
    uint8_t initialized;
};

struct pros_xhci_controller;
struct pros_xhci_dma_resources;

/*
 * Single-device bootstrap slot. Once published through DCBAA, the output
 * context and EP0 ring remain pinned for the lifetime of the controller.
 */
struct pros_xhci_device_slot {
    struct pros_usb_dma_buffer output_context;
    struct pros_usb_dma_buffer input_context;
    struct pros_usb_dma_buffer ep0_ring_dma;
    struct pros_usb_dma_buffer control_data;
    struct pros_xhci_transfer_ring ep0_ring;
    struct pros_usb_dma_buffer bulk_in_ring_dma;
    struct pros_usb_dma_buffer bulk_out_ring_dma;
    struct pros_usb_dma_buffer bulk_bounce;
    struct pros_xhci_transfer_ring bulk_in_ring;
    struct pros_xhci_transfer_ring bulk_out_ring;
    uint8_t slot_id;
    uint8_t root_port;
    uint8_t speed;
    uint16_t ep0_max_packet_size;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t num_configurations;
    uint8_t device_descriptor_valid;
    uint8_t msc_interface_number;
    uint8_t msc_endpoint_in;
    uint8_t msc_endpoint_out;
    uint8_t msc_configuration_value;
    uint16_t msc_max_packet_in;
    uint16_t msc_max_packet_out;
    uint8_t msc_interface_valid;
    uint8_t bulk_in_dci;
    uint8_t bulk_out_dci;
    uint8_t bulk_rings_allocated;
    /* Set only when an unpublished bulk-ring rollback needs a free retry. */
    uint8_t bulk_cleanup_pending;
    uint8_t bulk_endpoints_configured;
    /* Set when a posted bulk TD cannot be proven complete; DMA stays pinned. */
    uint8_t bulk_io_poisoned;
    uint8_t bulk_in_halted;
    uint8_t bulk_out_halted;
    uint16_t interrupter;
    uint32_t completion_polls;
    struct pros_xhci_controller *controller;
    struct pros_xhci_dma_resources *resources;
    struct pros_usb_msc msc;
    uint8_t initialized;
    uint8_t addressed;
};

/*
 * Persistent software ownership for one controller's DMA-visible objects.
 * These allocations are not automatically released on controller detach.
 * Once hardware_referenced is set, release is refused until a future,
 * explicit controller-quiesce API proves that DMA can no longer occur.
 */
struct pros_xhci_dma_resources {
    struct pros_usb_dma_buffer dcbaa;
    struct pros_usb_dma_buffer command_ring_dma;
    struct pros_usb_dma_buffer event_ring_dma;
    struct pros_usb_dma_buffer erst_dma;
    struct pros_xhci_scratchpad_set scratchpads;
    struct pros_xhci_command_ring command_ring;
    struct pros_xhci_event_ring event_ring;
    struct pros_xhci_erst erst;
    uint8_t initialized;
    uint8_t hardware_referenced;
    volatile uint32_t event_lock; /* Fail-fast serialization of shared event-ring access. */
    /* ERDP acknowledgement failure makes the shared event ring unusable. */
    volatile uint32_t event_ring_poisoned;
    /* Snapshot captured before timeout recovery changes controller state. */
    uint32_t diagnostic_usbcmd;
    uint32_t diagnostic_usbsts;
    uint32_t diagnostic_iman;
    uint32_t diagnostic_doorbell;
    uint32_t diagnostic_command_control;
    uint32_t diagnostic_event_control;
    uint32_t diagnostic_event_dequeue_index;
    uint32_t diagnostic_command_enqueue_index;
    uint32_t diagnostic_command_outstanding;
    uint8_t diagnostic_event_cycle;
    uint8_t diagnostic_command_cycle;
    uint64_t diagnostic_crcr;
    uint64_t diagnostic_erdp;
    /* Deferred Port Status Change Events, indexed by one-based port ID. */
    uint32_t pending_port_changes[8];
    struct pros_xhci_device_slot device_slot;
};

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
/* Program DCBAA[0], DCBAAP, CONFIG and CRCR while halted and bus mastering off.
 * scratchpads must remain allocated while the controller can reference them;
 * an initialized zero-count set is valid. */
int pros_xhci_program_command_path_disabled(
    struct pros_xhci_controller *, const struct pros_usb_dma_buffer *,
    const struct pros_xhci_scratchpad_set *,
    const struct pros_xhci_command_ring *);
int pros_xhci_command_path_setup_selftest(void);
int pros_xhci_validate_dma_layout(
    const struct pros_xhci_controller *, const struct pros_usb_dma_buffer *,
    const struct pros_xhci_command_ring *, const struct pros_xhci_erst *,
    const struct pros_xhci_event_ring *);
int pros_xhci_dma_layout_selftest(void);
/* Allocate the complete controller DMA set while halted. If rollback fails,
 * a partial owner is preserved in resources and release may be retried. */
int pros_xhci_dma_resources_alloc(
    const struct pros_xhci_controller *, struct pros_xhci_dma_resources *);
/* Release only allocations not yet published to hardware; failure is retryable. */
int pros_xhci_dma_resources_release(struct pros_xhci_dma_resources *);
int pros_xhci_dma_resources_selftest(void);
/* Program DCBAA/CRCR and interrupter DMA registers; keep bus mastering and RUN off. */
int pros_xhci_program_dma_resources_disabled(
    struct pros_xhci_controller *, struct pros_xhci_dma_resources *, uint16_t);
int pros_xhci_program_dma_resources_selftest(void);
/* Start in polling mode and verify a hardware No-op Command completion. */
int pros_xhci_noop_command(struct pros_xhci_controller *,
                           struct pros_xhci_dma_resources *, uint16_t,
                           uint32_t, uint32_t);
/* Reset one connected USB 1.x/2.0 port after controller RUN; USB 3.x is deferred. */
int pros_xhci_port_reset(struct pros_xhci_controller *, uint8_t, uint32_t,
                         uint32_t *);
int pros_xhci_port_reset_selftest(void);
/* Issue Enable Slot and retain intervening port-change events. */
int pros_xhci_enable_slot(struct pros_xhci_controller *,
                          struct pros_xhci_dma_resources *, uint16_t,
                          uint32_t, uint8_t *);
/* Atomically consume one coalesced port-change notification. */
int pros_xhci_take_pending_port_change(
    struct pros_xhci_dma_resources *, const struct pros_xhci_controller *,
    uint8_t, uint8_t *);
/* Drain deferred port-change events between command/transfer transactions. */
int pros_xhci_poll_port_events(
    struct pros_xhci_controller *, struct pros_xhci_dma_resources *,
    uint16_t, uint32_t, uint32_t *);
int pros_xhci_port_event_poll_selftest(void);
int pros_xhci_port_event_selftest(void);
/* Build and submit Address Device for the single supported bootstrap slot. */
int pros_xhci_address_device(struct pros_xhci_controller *,
                             struct pros_xhci_dma_resources *, uint16_t,
                             uint8_t, uint8_t, uint8_t, uint32_t);
int pros_xhci_get_device_descriptor(
    struct pros_xhci_controller *, struct pros_xhci_dma_resources *,
    uint16_t, uint32_t);
int pros_xhci_configure_msc_interface(
    struct pros_xhci_controller *, struct pros_xhci_dma_resources *,
    uint16_t, uint32_t);
int pros_xhci_configure_bulk_endpoints(
    struct pros_xhci_controller *, struct pros_xhci_dma_resources *,
    uint16_t, uint32_t);
int pros_xhci_init_mass_storage(struct pros_xhci_controller *,
                                struct pros_xhci_dma_resources *,
                                uint16_t, uint32_t);
/* Allocate and validate scratchpad DMA storage; does not program MMIO. */
int pros_xhci_scratchpad_alloc(
    const struct pros_xhci_capabilities *, struct pros_xhci_scratchpad_set *);
int pros_xhci_scratchpad_validate(
    const struct pros_xhci_capabilities *,
    const struct pros_xhci_scratchpad_set *);
int pros_xhci_scratchpad_release(struct pros_xhci_scratchpad_set *);
int pros_xhci_scratchpad_selftest(void);
int pros_xhci_read_mfindex(const struct pros_xhci_controller *controller,
                           uint32_t *value);
int pros_xhci_read_interrupter32(
    const struct pros_xhci_controller *controller, uint16_t interrupter,
    uint32_t register_offset, uint32_t *value);
int pros_xhci_read_interrupter64(
    const struct pros_xhci_controller *controller, uint16_t interrupter,
    uint32_t register_offset, uint64_t *value);
int pros_xhci_pin_controller_owner(struct pros_xhci_dma_resources *,
                                   struct pros_xhci_controller *,
                                   struct pros_xhci_controller *);
int pros_xhci_controller_owner_selftest(void);
int pros_xhci_selftest(void);

#endif

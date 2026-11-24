#ifndef PROSENJIT_USB_XHCI_RING_H
#define PROSENJIT_USB_XHCI_RING_H
#include <stdint.h>
#include "usb_dma.h"
#define PROS_XHCI_RING_OK 0
#define PROS_XHCI_RING_EINVAL -1
#define PROS_XHCI_RING_EEMPTY -3
#define PROS_XHCI_RING_EFULL -4
#define PROS_XHCI_TRB_SIZE 16u
#define PROS_XHCI_TRB_TYPE_SHIFT 10u
#define PROS_XHCI_TRB_TYPE_MASK (0x3Fu << PROS_XHCI_TRB_TYPE_SHIFT)
#define PROS_XHCI_TRB_CYCLE (1u << 0)
#define PROS_XHCI_TRB_TOGGLE_CYCLE (1u << 1)
#define PROS_XHCI_TRB_TYPE_LINK 6u
struct pros_xhci_trb { uint64_t parameter; uint32_t status; uint32_t control; } __attribute__((packed, aligned(16)));
struct pros_xhci_transfer_ring { struct pros_xhci_trb *trbs; uint64_t physical_address; uint32_t entry_count; uint32_t enqueue_index; uint32_t dequeue_index; uint32_t outstanding_count; uint8_t producer_cycle; uint8_t consumer_cycle; uint8_t initialized; };
struct pros_xhci_command_ring { struct pros_xhci_trb *trbs; uint64_t physical_address; uint32_t entry_count; uint32_t enqueue_index; uint32_t dequeue_index; uint32_t outstanding_count; uint8_t producer_cycle; uint8_t initialized; };
struct pros_xhci_erst_entry { uint64_t segment_base; uint16_t segment_size; uint16_t reserved; } __attribute__((packed, aligned(16)));
struct pros_xhci_erst { struct pros_xhci_erst_entry *entries; uint64_t physical_address; uint32_t entry_count; uint8_t initialized; };
struct pros_xhci_event_ring { struct pros_xhci_trb *trbs; uint64_t physical_address; uint32_t entry_count; uint32_t dequeue_index; uint8_t consumer_cycle; uint8_t initialized; };
int pros_xhci_transfer_ring_init(struct pros_xhci_transfer_ring *, const struct pros_usb_dma_buffer *, uint32_t);
int pros_xhci_transfer_ring_enqueue_td(struct pros_xhci_transfer_ring *, const struct pros_usb_dma_buffer *, uint32_t, uint16_t, uint64_t *);
int pros_xhci_transfer_ring_complete_td(struct pros_xhci_transfer_ring *, uint64_t);
int pros_xhci_transfer_ring_dequeue_pointer(const struct pros_xhci_transfer_ring *, uint64_t *);
int pros_xhci_build_reset_endpoint_command(struct pros_xhci_trb *, uint8_t, uint8_t, uint8_t);
int pros_xhci_build_set_dequeue_command(struct pros_xhci_trb *, uint64_t, uint8_t, uint8_t);
int pros_xhci_transfer_ring_enqueue_normal(struct pros_xhci_transfer_ring *, const struct pros_usb_dma_buffer *, uint32_t, uint64_t *);
int pros_xhci_transfer_ring_enqueue_control(
    struct pros_xhci_transfer_ring *, const uint8_t[8],
    const struct pros_usb_dma_buffer *, uint32_t, uint8_t,
    uint64_t *, uint64_t *);
int pros_xhci_transfer_ring_handle_td_event(
    struct pros_xhci_transfer_ring *, const struct pros_xhci_trb *,
    uint8_t, uint8_t, uint8_t *);
int pros_xhci_transfer_ring_complete(struct pros_xhci_transfer_ring *, uint64_t);
int pros_xhci_transfer_ring_handle_event(struct pros_xhci_transfer_ring *, const struct pros_xhci_trb *, uint8_t, uint8_t, uint8_t *);
int pros_xhci_transfer_ring_selftest(void);
int pros_xhci_command_ring_init(struct pros_xhci_command_ring *, const struct pros_usb_dma_buffer *, uint32_t);
int pros_xhci_command_ring_enqueue(struct pros_xhci_command_ring *, uint64_t, uint32_t, uint32_t, uint64_t *);
int pros_xhci_command_ring_complete(struct pros_xhci_command_ring *, uint64_t);
int pros_xhci_command_ring_handle_completion(struct pros_xhci_command_ring *, const struct pros_xhci_trb *, uint8_t *);
int pros_xhci_event_ring_init(struct pros_xhci_event_ring *, const struct pros_usb_dma_buffer *, uint32_t);
int pros_xhci_event_ring_peek(const struct pros_xhci_event_ring *, struct pros_xhci_trb *, uint64_t *);
int pros_xhci_event_ring_dequeue(struct pros_xhci_event_ring *, struct pros_xhci_trb *, uint64_t *);
int pros_xhci_event_ring_erdp_value(const struct pros_xhci_event_ring *, uint64_t *);
int pros_xhci_event_type(const struct pros_xhci_trb *, uint8_t *);
int pros_xhci_event_completion_code(const struct pros_xhci_trb *, uint8_t *);
int pros_xhci_erst_init(struct pros_xhci_erst *, const struct pros_usb_dma_buffer *, const struct pros_usb_dma_buffer *, uint32_t);
int pros_xhci_erst_init_segments(struct pros_xhci_erst *, const struct pros_usb_dma_buffer *, const struct pros_usb_dma_buffer *, uint32_t);
int pros_xhci_erst_selftest(void);
int pros_xhci_ring_selftest(void);
#endif

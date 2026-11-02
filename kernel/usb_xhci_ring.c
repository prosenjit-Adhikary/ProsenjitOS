#include "usb_xhci_ring.h"

#define XHCI_LINK_TYPE (PROS_XHCI_TRB_TYPE_LINK << PROS_XHCI_TRB_TYPE_SHIFT)

static int dma_ring_valid(const struct pros_usb_dma_buffer *dma, uint32_t count)
{
    uint64_t bytes;
    if (!dma || !dma->virtual_address || !dma->physical_address ||
        !dma->allocation_id || !dma->page_count || count < 4u ||
        count > UINT32_MAX / PROS_XHCI_TRB_SIZE)
        return 0;
    bytes = (uint64_t)count * PROS_XHCI_TRB_SIZE;
    return dma->size >= bytes && dma->allocation_size >= bytes &&
           (dma->physical_address & 63u) == 0u &&
           (((uintptr_t)dma->virtual_address & 63u) == 0u) &&
           dma->physical_address <= UINT64_MAX - bytes;
}

static void clear_trb(struct pros_xhci_trb *trb)
{
    trb->parameter = 0u;
    trb->status = 0u;
    trb->control = 0u;
}

int pros_xhci_command_ring_init(struct pros_xhci_command_ring *ring,
                                const struct pros_usb_dma_buffer *dma,
                                uint32_t count)
{
    if (!ring || !dma_ring_valid(dma, count))
        return PROS_XHCI_RING_EINVAL;
    ring->initialized = 0u;
    ring->trbs = (struct pros_xhci_trb *)dma->virtual_address;
    ring->physical_address = dma->physical_address;
    ring->entry_count = count;
    ring->enqueue_index = 0u;
    ring->producer_cycle = 1u;
    for (uint32_t i = 0u; i < count; ++i)
        clear_trb(&ring->trbs[i]);
    ring->trbs[count - 1u].parameter = dma->physical_address;
    ring->trbs[count - 1u].control =
        XHCI_LINK_TYPE | PROS_XHCI_TRB_TOGGLE_CYCLE | PROS_XHCI_TRB_CYCLE;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ring->initialized = 1u;
    return PROS_XHCI_RING_OK;
}

int pros_xhci_command_ring_enqueue(struct pros_xhci_command_ring *ring,
                                   uint64_t parameter, uint32_t status,
                                   uint32_t control, uint64_t *trb_address)
{
    uint32_t index;
    struct pros_xhci_trb *trb;
    if (!ring || !ring->initialized || !ring->trbs || !trb_address ||
        ring->entry_count < 4u || ring->enqueue_index >= ring->entry_count - 1u ||
        ((control & PROS_XHCI_TRB_TYPE_MASK) == XHCI_LINK_TYPE))
        return PROS_XHCI_RING_EINVAL;

    index = ring->enqueue_index;
    trb = &ring->trbs[index];
    trb->parameter = parameter;
    trb->status = status;
    trb->control = control & ~PROS_XHCI_TRB_CYCLE;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    trb->control |= ring->producer_cycle ? PROS_XHCI_TRB_CYCLE : 0u;
    *trb_address = ring->physical_address + (uint64_t)index * PROS_XHCI_TRB_SIZE;

    ++ring->enqueue_index;
    if (ring->enqueue_index == ring->entry_count - 1u) {
        ring->enqueue_index = 0u;
        ring->producer_cycle ^= 1u;
        ring->trbs[ring->entry_count - 1u].control =
            XHCI_LINK_TYPE | PROS_XHCI_TRB_TOGGLE_CYCLE |
            (ring->producer_cycle ? PROS_XHCI_TRB_CYCLE : 0u);
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }
    return PROS_XHCI_RING_OK;
}

int pros_xhci_event_ring_init(struct pros_xhci_event_ring *ring,
                              const struct pros_usb_dma_buffer *dma,
                              uint32_t count)
{
    if (!ring || !dma_ring_valid(dma, count))
        return PROS_XHCI_RING_EINVAL;
    ring->initialized = 0u;
    ring->trbs = (struct pros_xhci_trb *)dma->virtual_address;
    ring->physical_address = dma->physical_address;
    ring->entry_count = count;
    ring->dequeue_index = 0u;
    ring->consumer_cycle = 1u;
    for (uint32_t i = 0u; i < count; ++i)
        clear_trb(&ring->trbs[i]);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ring->initialized = 1u;
    return PROS_XHCI_RING_OK;
}

int pros_xhci_event_ring_dequeue(struct pros_xhci_event_ring *ring,
                                 struct pros_xhci_trb *event,
                                 uint64_t *trb_address)
{
    volatile struct pros_xhci_trb *source;
    uint32_t control;
    if (!ring || !ring->initialized || !ring->trbs || !event || !trb_address ||
        ring->entry_count < 4u || ring->dequeue_index >= ring->entry_count)
        return PROS_XHCI_RING_EINVAL;

    source = (volatile struct pros_xhci_trb *)&ring->trbs[ring->dequeue_index];
    control = source->control;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if ((control & PROS_XHCI_TRB_CYCLE) !=
        (ring->consumer_cycle ? PROS_XHCI_TRB_CYCLE : 0u))
        return PROS_XHCI_RING_EEMPTY;

    event->parameter = source->parameter;
    event->status = source->status;
    event->control = control;
    *trb_address = ring->physical_address +
                   (uint64_t)ring->dequeue_index * PROS_XHCI_TRB_SIZE;
    ++ring->dequeue_index;
    if (ring->dequeue_index == ring->entry_count) {
        ring->dequeue_index = 0u;
        ring->consumer_cycle ^= 1u;
    }
    return PROS_XHCI_RING_OK;
}

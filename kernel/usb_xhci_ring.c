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
    ring->dequeue_index = 0u;
    ring->outstanding_count = 0u;
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
    if (ring->outstanding_count >= ring->entry_count - 1u)
        return PROS_XHCI_RING_EFULL;

    index = ring->enqueue_index;
    trb = &ring->trbs[index];
    trb->parameter = parameter;
    trb->status = status;
    trb->control = control & ~PROS_XHCI_TRB_CYCLE;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    trb->control |= ring->producer_cycle ? PROS_XHCI_TRB_CYCLE : 0u;
    *trb_address = ring->physical_address + (uint64_t)index * PROS_XHCI_TRB_SIZE;

    ++ring->outstanding_count;
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


int pros_xhci_command_ring_complete(struct pros_xhci_command_ring *ring,
                                    uint64_t completed_trb_address)
{
    uint64_t expected;
    if (!ring || !ring->initialized || !ring->trbs ||
        ring->entry_count < 4u || ring->dequeue_index >= ring->entry_count - 1u)
        return PROS_XHCI_RING_EINVAL;
    if (ring->outstanding_count == 0u)
        return PROS_XHCI_RING_EEMPTY;

    expected = ring->physical_address +
               (uint64_t)ring->dequeue_index * PROS_XHCI_TRB_SIZE;
    if (completed_trb_address != expected)
        return PROS_XHCI_RING_EINVAL;

    ring->dequeue_index++;
    if (ring->dequeue_index == ring->entry_count - 1u)
        ring->dequeue_index = 0u;
    ring->outstanding_count--;
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

int pros_xhci_ring_selftest(void)
{
    static struct pros_xhci_trb command_trbs[8] __attribute__((aligned(64)));
    static struct pros_xhci_trb event_trbs[4] __attribute__((aligned(64)));
    struct pros_usb_dma_buffer command_dma = {
        0x100000u, command_trbs, sizeof(command_trbs),
        sizeof(command_trbs), 1u, 1u
    };
    struct pros_usb_dma_buffer event_dma = {
        0x200000u, event_trbs, sizeof(event_trbs),
        sizeof(event_trbs), 1u, 2u
    };
    struct pros_xhci_command_ring command;
    struct pros_xhci_event_ring events;
    struct pros_xhci_trb event;
    uint64_t address = 0u;

    if (pros_xhci_command_ring_init(&command, &command_dma, 8u) != 0 ||
        command.trbs[7].parameter != command_dma.physical_address ||
        (command.trbs[7].control & PROS_XHCI_TRB_TYPE_MASK) != XHCI_LINK_TYPE ||
        !(command.trbs[7].control & PROS_XHCI_TRB_TOGGLE_CYCLE))
        return -1;
    for (uint32_t i = 0u; i < 7u; ++i) {
        if (pros_xhci_command_ring_enqueue(&command, i + 0x100u, i,
                1u << PROS_XHCI_TRB_TYPE_SHIFT, &address) != 0 ||
            address != command_dma.physical_address + (uint64_t)i * 16u ||
            command.trbs[i].parameter != i + 0x100u ||
            !(command.trbs[i].control & PROS_XHCI_TRB_CYCLE))
            return -2;
    }
    if (command.enqueue_index != 0u || command.producer_cycle != 0u ||
        command.outstanding_count != 7u ||
        pros_xhci_command_ring_enqueue(&command, 0u, 0u,
            1u << PROS_XHCI_TRB_TYPE_SHIFT, &address) != PROS_XHCI_RING_EFULL ||
        pros_xhci_command_ring_complete(&command, command_dma.physical_address + 16u) !=
            PROS_XHCI_RING_EINVAL ||
        pros_xhci_command_ring_complete(&command, command_dma.physical_address) != 0 ||
        command.outstanding_count != 6u ||
        pros_xhci_command_ring_enqueue(&command, 0x200u, 0u,
            1u << PROS_XHCI_TRB_TYPE_SHIFT, &address) != 0 ||
        address != command_dma.physical_address ||
        command.outstanding_count != 7u ||
        pros_xhci_command_ring_complete(&command, command_dma.physical_address) !=
            PROS_XHCI_RING_EINVAL ||
        pros_xhci_command_ring_complete(&command, command_dma.physical_address + 16u) != 0 ||
        command.outstanding_count != 6u ||
        pros_xhci_command_ring_complete(&command, command_dma.physical_address + 32u) != 0 ||
        command.outstanding_count != 5u)
        return -3;
    if (pros_xhci_event_ring_init(&events, &event_dma, 4u) != 0 ||
        pros_xhci_event_ring_dequeue(&events, &event, &address) !=
            PROS_XHCI_RING_EEMPTY)
        return -4;

    event_trbs[0].parameter = 0xABCDEFu;
    event_trbs[0].status = 0x1234u;
    event_trbs[0].control = (32u << PROS_XHCI_TRB_TYPE_SHIFT) | PROS_XHCI_TRB_CYCLE;
    if (pros_xhci_event_ring_dequeue(&events, &event, &address) != 0 ||
        event.parameter != 0xABCDEFu || event.status != 0x1234u ||
        address != event_dma.physical_address)
        return -5;
    for (uint32_t i = 1u; i < 4u; ++i)
        event_trbs[i].control = PROS_XHCI_TRB_CYCLE;
    for (uint32_t i = 1u; i < 4u; ++i)
        if (pros_xhci_event_ring_dequeue(&events, &event, &address) != 0)
            return -6;
    if (events.dequeue_index != 0u || events.consumer_cycle != 0u ||
        pros_xhci_event_ring_dequeue(&events, &event, &address) !=
            PROS_XHCI_RING_EEMPTY)
        return -7;
    command_dma.physical_address |= 1u;
    if (pros_xhci_command_ring_init(&command, &command_dma, 8u) !=
        PROS_XHCI_RING_EINVAL)
        return -8;
    return PROS_XHCI_RING_OK;
}

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

int pros_xhci_event_ring_peek(const struct pros_xhci_event_ring *ring,
                              struct pros_xhci_trb *event,
                              uint64_t *trb_address)
{
    const volatile struct pros_xhci_trb *source;
    uint32_t control;
    if (!ring || !ring->initialized || !ring->trbs || !event || !trb_address ||
        ring->entry_count < 4u || ring->dequeue_index >= ring->entry_count)
        return PROS_XHCI_RING_EINVAL;
    source = (const volatile struct pros_xhci_trb *)&ring->trbs[ring->dequeue_index];
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


static int xhci_erst_dma_valid(const struct pros_usb_dma_buffer *dma,
                               uint64_t required, uint64_t alignment) {
    return dma && dma->virtual_address && dma->physical_address &&
           dma->allocation_id && dma->page_count &&
           dma->size >= required && dma->allocation_size >= required &&
           (dma->physical_address & (alignment - 1u)) == 0u &&
           (((uintptr_t)dma->virtual_address & (alignment - 1u)) == 0u) &&
           dma->physical_address <= UINT64_MAX - required;
}

int pros_xhci_erst_init(struct pros_xhci_erst *erst,
                        const struct pros_usb_dma_buffer *table_dma,
                        const struct pros_usb_dma_buffer *event_dma,
                        uint32_t event_trb_count) {
    uint64_t event_bytes;
    struct pros_xhci_erst_entry *entry;
    if (!erst)
        return PROS_XHCI_RING_EINVAL;
    erst->initialized = 0u;
    erst->entries = 0;
    erst->physical_address = 0u;
    erst->entry_count = 0u;
    if (event_trb_count < 4u || event_trb_count > 0xFFFFu)
        return PROS_XHCI_RING_EINVAL;
    event_bytes = (uint64_t)event_trb_count * PROS_XHCI_TRB_SIZE;
    if (!xhci_erst_dma_valid(table_dma, sizeof(*entry), 64u) ||
        !xhci_erst_dma_valid(event_dma, event_bytes, 64u))
        return PROS_XHCI_RING_EINVAL;
    entry = (struct pros_xhci_erst_entry *)table_dma->virtual_address;
    entry->segment_base = event_dma->physical_address;
    entry->segment_size = (uint16_t)event_trb_count;
    entry->reserved = 0u;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    erst->entries = entry;
    erst->physical_address = table_dma->physical_address;
    erst->entry_count = 1u;
    erst->initialized = 1u;
    return PROS_XHCI_RING_OK;
}


#define PROS_XHCI_ERST_MAX_SEGMENTS 8u

/* Inputs are prevalidated non-empty physical ranges. Subtraction avoids
 * end-address addition overflow in the overlap predicate itself. */
static int xhci_dma_ranges_overlap(uint64_t a, uint64_t a_size,
                                  uint64_t b, uint64_t b_size)
{
    if (a_size == 0u || b_size == 0u)
        return 0;
    return a <= b ? (b - a) < a_size : (a - b) < b_size;
}

int pros_xhci_erst_init_segments(
    struct pros_xhci_erst *erst,
    const struct pros_usb_dma_buffer *table_dma,
    const struct pros_usb_dma_buffer *segments,
    uint32_t segment_count)
{
    uint64_t table_bytes;
    struct pros_xhci_erst_entry *entries;
    if (!erst)
        return PROS_XHCI_RING_EINVAL;
    erst->initialized = 0u;
    erst->entries = 0;
    erst->physical_address = 0u;
    erst->entry_count = 0u;
    if (!segments || segment_count == 0u ||
        segment_count > PROS_XHCI_ERST_MAX_SEGMENTS)
        return PROS_XHCI_RING_EINVAL;
    table_bytes = (uint64_t)segment_count *
                  sizeof(struct pros_xhci_erst_entry);
    if (!xhci_erst_dma_valid(table_dma, table_bytes, 64u))
        return PROS_XHCI_RING_EINVAL;
    for (uint32_t i = 0u; i < segment_count; ++i) {
        uint64_t bytes = segments[i].size;
        uint64_t trb_count;
        if (bytes < 4u * PROS_XHCI_TRB_SIZE ||
            (bytes % PROS_XHCI_TRB_SIZE) != 0u)
            return PROS_XHCI_RING_EINVAL;
        trb_count = bytes / PROS_XHCI_TRB_SIZE;
        if (trb_count > 0xFFFFu ||
            !xhci_erst_dma_valid(&segments[i], bytes, 64u))
            return PROS_XHCI_RING_EINVAL;
        if (xhci_dma_ranges_overlap(table_dma->physical_address, table_bytes,
                                    segments[i].physical_address, bytes))
            return PROS_XHCI_RING_EINVAL;
        for (uint32_t j = 0u; j < i; ++j) {
            if (xhci_dma_ranges_overlap(segments[j].physical_address,
                                        segments[j].size,
                                        segments[i].physical_address, bytes))
                return PROS_XHCI_RING_EINVAL;
        }
    }
    entries = (struct pros_xhci_erst_entry *)table_dma->virtual_address;
    for (uint32_t i = 0u; i < segment_count; ++i) {
        entries[i].segment_base = segments[i].physical_address;
        entries[i].segment_size =
            (uint16_t)(segments[i].size / PROS_XHCI_TRB_SIZE);
        entries[i].reserved = 0u;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    erst->entries = entries;
    erst->physical_address = table_dma->physical_address;
    erst->entry_count = segment_count;
    erst->initialized = 1u;
    return PROS_XHCI_RING_OK;
}

int pros_xhci_erst_selftest(void) {
    static struct pros_xhci_erst_entry table[4] __attribute__((aligned(64)));
    static struct pros_xhci_trb events[8] __attribute__((aligned(64)));
    struct pros_usb_dma_buffer table_dma = {
        0x300000u, table, sizeof(table), sizeof(table), 1u, 3u
    };
    struct pros_usb_dma_buffer event_dma = {
        0x400000u, events, sizeof(events), sizeof(events), 1u, 4u
    };
    struct pros_xhci_erst erst;
    static struct pros_xhci_erst_entry multi_table[8] __attribute__((aligned(64)));
    static struct pros_xhci_trb events2[4] __attribute__((aligned(64)));
    struct pros_usb_dma_buffer multi_table_dma = {
        0x500000u, multi_table, sizeof(multi_table), sizeof(multi_table), 1u, 5u
    };
    struct pros_usb_dma_buffer segments[2] = {
        {0x600000u, events, sizeof(events), sizeof(events), 1u, 6u},
        {0x700000u, events2, sizeof(events2), sizeof(events2), 1u, 7u}
    };
    if (pros_xhci_erst_init(&erst, &table_dma, &event_dma, 8u) !=
            PROS_XHCI_RING_OK ||
        !erst.initialized || erst.entry_count != 1u ||
        erst.physical_address != table_dma.physical_address ||
        erst.entries[0].segment_base != event_dma.physical_address ||
        erst.entries[0].segment_size != 8u || erst.entries[0].reserved != 0u)
        return -1;
    if (pros_xhci_erst_init(&erst, &table_dma, &event_dma, 3u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
        return -2;
    if (pros_xhci_erst_init(&erst, &table_dma, &event_dma, 0x10000u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
        return -3;
    {
        struct pros_usb_dma_buffer bad = event_dma;
        bad.size = 16u;
        if (pros_xhci_erst_init(&erst, &table_dma, &bad, 8u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
            return -4;
        bad = event_dma;
        bad.physical_address |= 1u;
        if (pros_xhci_erst_init(&erst, &table_dma, &bad, 8u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
            return -5;
        bad = table_dma;
        bad.allocation_id = 0u;
        if (pros_xhci_erst_init(&erst, &bad, &event_dma, 8u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
            return -6;
    }
    {
        struct pros_usb_dma_buffer overlap[2] = {segments[0], segments[1]};
        overlap[1].physical_address = overlap[0].physical_address + 64u;
        if (pros_xhci_erst_init_segments(&erst, &multi_table_dma,
                                         overlap, 2u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
            return -13;
        overlap[1] = segments[1];
        overlap[0].physical_address = multi_table_dma.physical_address;
        if (pros_xhci_erst_init_segments(&erst, &multi_table_dma,
                                         overlap, 2u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
            return -14;
        overlap[0] = segments[0];
        overlap[0].physical_address = UINT64_MAX - 63u;
        if (pros_xhci_erst_init_segments(&erst, &multi_table_dma,
                                         overlap, 2u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
            return -15;
    }
    if (pros_xhci_erst_init_segments(&erst, &multi_table_dma, segments, 2u) !=
            PROS_XHCI_RING_OK ||
        !erst.initialized || erst.entry_count != 2u ||
        erst.entries[0].segment_base != segments[0].physical_address ||
        erst.entries[0].segment_size != 8u ||
        erst.entries[1].segment_base != segments[1].physical_address ||
        erst.entries[1].segment_size != 4u ||
        erst.entries[0].reserved != 0u || erst.entries[1].reserved != 0u)
        return -7;
    if (pros_xhci_erst_init_segments(&erst, &multi_table_dma, segments, 0u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
        return -8;
    if (pros_xhci_erst_init_segments(&erst, &multi_table_dma, segments, 9u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
        return -9;
    {
        struct pros_usb_dma_buffer bad_segments[2] = {segments[0], segments[1]};
        bad_segments[1].size = 63u;
        if (pros_xhci_erst_init_segments(&erst, &multi_table_dma,
                                         bad_segments, 2u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
            return -10;
        bad_segments[1] = segments[1];
        bad_segments[1].size = 0x100000u;
        bad_segments[1].allocation_size = 0x100000u;
        if (pros_xhci_erst_init_segments(&erst, &multi_table_dma,
                                         bad_segments, 2u) !=
            PROS_XHCI_RING_EINVAL || erst.initialized)
            return -11;
        {
            struct pros_usb_dma_buffer small_table = table_dma;
            small_table.size = sizeof(struct pros_xhci_erst_entry);
            small_table.allocation_size = sizeof(struct pros_xhci_erst_entry);
            if (pros_xhci_erst_init_segments(&erst, &small_table, segments, 2u) !=
                PROS_XHCI_RING_EINVAL || erst.initialized)
                return -12;
        }
    }
    return PROS_XHCI_RING_OK;
}



/*
 * Consume a Command Completion Event only when it identifies the next
 * outstanding command TRB (xHCI command completions are retired in ring order).
 * Completion status is returned to the caller;
 * this helper does not dispatch callbacks or touch controller MMIO.
 */
int pros_xhci_command_ring_handle_completion(
    struct pros_xhci_command_ring *ring,
    const struct pros_xhci_trb *event,
    uint8_t *completion_code)
{
    uint8_t type, code;
    int rc;
    if (!ring || !event || !completion_code ||
        pros_xhci_event_type(event, &type) != PROS_XHCI_RING_OK ||
        type != 33u || (event->parameter & 0xFu) != 0u)
        return PROS_XHCI_RING_EINVAL;
    if (pros_xhci_event_completion_code(event, &code) != PROS_XHCI_RING_OK ||
        code == 0u)
        return PROS_XHCI_RING_EINVAL;
    rc = pros_xhci_command_ring_complete(ring, event->parameter);
    if (rc != PROS_XHCI_RING_OK)
        return rc;
    *completion_code = code;
    return PROS_XHCI_RING_OK;
}

/* Calculate ERDP for the next software dequeue position; no MMIO write. */
int pros_xhci_event_ring_erdp_value(const struct pros_xhci_event_ring *ring,
                                   uint64_t *value)
{
    uint64_t offset, address;
    if (!ring || !value || !ring->initialized || !ring->trbs ||
        ring->entry_count < 4u || ring->dequeue_index >= ring->entry_count ||
        (ring->physical_address & 0xFu) != 0u)
        return PROS_XHCI_RING_EINVAL;
    offset = (uint64_t)ring->dequeue_index * PROS_XHCI_TRB_SIZE;
    if (ring->physical_address > UINT64_MAX - offset)
        return PROS_XHCI_RING_EINVAL;
    address = ring->physical_address + offset;
    if ((address & 0xFu) != 0u)
        return PROS_XHCI_RING_EINVAL;
    *value = address | (1ull << 3); /* EHB write-one-to-clear bit */
    return PROS_XHCI_RING_OK;
}

int pros_xhci_event_type(const struct pros_xhci_trb *event, uint8_t *type)
{
    if (!event || !type)
        return PROS_XHCI_RING_EINVAL;
    *type = (uint8_t)((event->control & PROS_XHCI_TRB_TYPE_MASK) >>
                      PROS_XHCI_TRB_TYPE_SHIFT);
    return PROS_XHCI_RING_OK;
}

int pros_xhci_event_completion_code(const struct pros_xhci_trb *event,
                                    uint8_t *completion_code)
{
    uint8_t type;
    if (!event || !completion_code ||
        pros_xhci_event_type(event, &type) != PROS_XHCI_RING_OK ||
        (type != 32u && type != 33u))
        return PROS_XHCI_RING_EINVAL;
    *completion_code = (uint8_t)(event->status >> 24);
    return PROS_XHCI_RING_OK;
}


#define XHCI_NORMAL_TRB_TYPE 1u
#define XHCI_TRB_IOC (1u << 5)
#define XHCI_NORMAL_LENGTH_MAX 0x1FFFFu

int pros_xhci_transfer_ring_init(struct pros_xhci_transfer_ring *ring,
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
    ring->consumer_cycle = 1u;
    for (uint32_t i = 0u; i < count; ++i)
        clear_trb(&ring->trbs[i]);
    ring->trbs[count - 1u].parameter = dma->physical_address;
    ring->trbs[count - 1u].control =
        XHCI_LINK_TYPE | PROS_XHCI_TRB_TOGGLE_CYCLE | PROS_XHCI_TRB_CYCLE;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ring->initialized = 1u;
    return PROS_XHCI_RING_OK;
}

int pros_xhci_transfer_ring_enqueue_normal(
    struct pros_xhci_transfer_ring *ring,
    const struct pros_usb_dma_buffer *data_dma,
    uint32_t length, uint64_t *trb_address)
{
    struct pros_xhci_trb *trb;
    uint32_t index;
    if (!ring || !ring->initialized || !ring->trbs || !data_dma ||
        !trb_address || length == 0u || length > XHCI_NORMAL_LENGTH_MAX ||
        !data_dma->virtual_address || !data_dma->physical_address ||
        !data_dma->allocation_id || !data_dma->page_count ||
        data_dma->size < length || data_dma->allocation_size < length ||
        (data_dma->physical_address & 0xFu) != 0u ||
        (((uintptr_t)data_dma->virtual_address & 0xFu) != 0u) ||
        (uintptr_t)data_dma->virtual_address > UINTPTR_MAX - length ||
        data_dma->physical_address > UINT64_MAX - length ||
        ring->entry_count < 4u || ring->enqueue_index >= ring->entry_count - 1u)
        return PROS_XHCI_RING_EINVAL;
    if (xhci_dma_ranges_overlap(
            ring->physical_address,
            (uint64_t)ring->entry_count * PROS_XHCI_TRB_SIZE,
            data_dma->physical_address, length))
        return PROS_XHCI_RING_EINVAL;
    if (ring->outstanding_count >= ring->entry_count - 1u)
        return PROS_XHCI_RING_EFULL;

    index = ring->enqueue_index;
    trb = &ring->trbs[index];
    trb->parameter = data_dma->physical_address;
    trb->status = length;
    trb->control = (XHCI_NORMAL_TRB_TYPE << PROS_XHCI_TRB_TYPE_SHIFT) |
                   XHCI_TRB_IOC;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    trb->control |= ring->producer_cycle ? PROS_XHCI_TRB_CYCLE : 0u;
    *trb_address = ring->physical_address +
                   (uint64_t)index * PROS_XHCI_TRB_SIZE;

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

int pros_xhci_transfer_ring_complete(struct pros_xhci_transfer_ring *ring,
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
    if (ring->consumer_cycle > 1u)
        return PROS_XHCI_RING_EINVAL;
    ++ring->dequeue_index;
    if (ring->dequeue_index == ring->entry_count - 1u) {
        ring->dequeue_index = 0u;
        ring->consumer_cycle ^= 1u;
    }
    --ring->outstanding_count;
    return PROS_XHCI_RING_OK;
}


/*
 * Match a non-Event-Data Transfer Event to its endpoint ring and reclaim the
 * next in-order Normal TRB. Event Data payloads require a separate TD map
 * and are deliberately rejected until that mapping is implemented.
 * This path remains independent of controller interrupt-enable state.
 * ERDP acknowledgement is performed by the controller runtime layer.
 */

/* Non-final chunks stay 16-byte aligned for the next TRB buffer pointer. */
#define XHCI_NORMAL_TRB_LENGTH_CHUNK 0x1FFF0u
#define XHCI_NORMAL_TD_MAX_TRBS 32u

#define XHCI_SETUP_STAGE_TRB_TYPE 2u
#define XHCI_DATA_STAGE_TRB_TYPE 3u
#define XHCI_STATUS_STAGE_TRB_TYPE 4u
#define XHCI_TRB_IDT (1u << 6)
#define XHCI_TRB_CHAIN (1u << 4)
#define XHCI_TRB_DIR_IN (1u << 16)

static int xhci_transfer_ring_enqueue_raw(
    struct pros_xhci_transfer_ring *ring, uint64_t parameter,
    uint32_t status, uint32_t control, uint64_t *trb_address)
{
    struct pros_xhci_trb *trb;
    uint32_t index;
    if (!ring || !ring->initialized || !ring->trbs || !trb_address ||
        ring->entry_count < 4u ||
        ring->enqueue_index >= ring->entry_count - 1u ||
        ring->outstanding_count >= ring->entry_count - 1u ||
        ring->producer_cycle > 1u)
        return PROS_XHCI_RING_EINVAL;
    index = ring->enqueue_index;
    trb = &ring->trbs[index];
    trb->parameter = parameter;
    trb->status = status;
    trb->control = control & ~PROS_XHCI_TRB_CYCLE;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    trb->control |= ring->producer_cycle ? PROS_XHCI_TRB_CYCLE : 0u;
    *trb_address = ring->physical_address +
                   (uint64_t)index * PROS_XHCI_TRB_SIZE;
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

/*
 * Queue one complete control TD: Setup Stage, optional Data Stage, Status
 * Stage. The setup packet is encoded as an immediate 8-byte payload.
 */
int pros_xhci_transfer_ring_enqueue_control(
    struct pros_xhci_transfer_ring *ring, const uint8_t setup_packet[8],
    const struct pros_usb_dma_buffer *data_dma, uint32_t data_length,
    uint8_t data_in, uint64_t *first_trb_address,
    uint64_t *last_trb_address)
{
    uint64_t ring_bytes, setup_value = 0u, first = 0u, last = 0u;
    uint32_t stage_count, transfer_type, setup_control, status_control;
    uint8_t setup_direction;
    int rc;
    if (!ring || !ring->initialized || !ring->trbs || !setup_packet ||
        !first_trb_address || !last_trb_address ||
        ring->entry_count < 4u ||
        ring->entry_count > UINT32_MAX / PROS_XHCI_TRB_SIZE ||
        ring->enqueue_index >= ring->entry_count - 1u ||
        ring->dequeue_index >= ring->entry_count - 1u ||
        ring->producer_cycle > 1u ||
        ring->outstanding_count > ring->entry_count - 1u ||
        data_length > 0xFFFFu || data_in > 1u ||
        ring->physical_address == 0u ||
        (ring->physical_address & 63u) != 0u ||
        (((uintptr_t)ring->trbs & 63u) != 0u))
        return PROS_XHCI_RING_EINVAL;

    setup_direction = (setup_packet[0] & 0x80u) != 0u ? 1u : 0u;
    if (((uint32_t)setup_packet[6] |
         ((uint32_t)setup_packet[7] << 8)) != data_length ||
        (data_length != 0u && setup_direction != data_in))
        return PROS_XHCI_RING_EINVAL;

    stage_count = data_length == 0u ? 2u : 3u;
    if (stage_count > (ring->entry_count - 1u) - ring->outstanding_count)
        return PROS_XHCI_RING_EFULL;
    ring_bytes = (uint64_t)ring->entry_count * PROS_XHCI_TRB_SIZE;
    if (ring->physical_address > UINT64_MAX - ring_bytes ||
        (uintptr_t)ring->trbs > UINTPTR_MAX - ring_bytes)
        return PROS_XHCI_RING_EINVAL;

    if (data_length != 0u) {
        if (!data_dma || !data_dma->virtual_address ||
            !data_dma->physical_address || !data_dma->allocation_id ||
            !data_dma->page_count || data_dma->size < data_length ||
            data_dma->allocation_size < data_length ||
            (data_dma->physical_address & 0xFu) != 0u ||
            (((uintptr_t)data_dma->virtual_address & 0xFu) != 0u) ||
            (uintptr_t)data_dma->virtual_address > UINTPTR_MAX - data_length ||
            data_dma->physical_address > UINT64_MAX - data_length ||
            xhci_dma_ranges_overlap(ring->physical_address, ring_bytes,
                                    data_dma->physical_address, data_length))
            return PROS_XHCI_RING_EINVAL;
    } else if (data_dma != 0) {
        if (data_dma->size != 0u)
            return PROS_XHCI_RING_EINVAL;
    }

    for (uint32_t i = 0u; i < 8u; ++i)
        setup_value |= (uint64_t)setup_packet[i] << (i * 8u);
    transfer_type = data_length == 0u ? 0u : (data_in ? 2u : 3u);
    setup_control = (XHCI_SETUP_STAGE_TRB_TYPE <<
                     PROS_XHCI_TRB_TYPE_SHIFT) |
                    XHCI_TRB_IDT | XHCI_TRB_CHAIN |
                    (transfer_type << 16);
    rc = xhci_transfer_ring_enqueue_raw(ring, setup_value, 8u,
                                         setup_control, &first);
    if (rc != PROS_XHCI_RING_OK)
        return rc;

    if (data_length != 0u) {
        uint32_t data_control =
            (XHCI_DATA_STAGE_TRB_TYPE << PROS_XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_CHAIN | (data_in ? XHCI_TRB_DIR_IN : 0u);
        rc = xhci_transfer_ring_enqueue_raw(ring,
            data_dma->physical_address, data_length, data_control, &last);
        if (rc != PROS_XHCI_RING_OK)
            return rc;
    }

    status_control = (XHCI_STATUS_STAGE_TRB_TYPE <<
                      PROS_XHCI_TRB_TYPE_SHIFT) | (1u << 5);
    if (data_length == 0u || !data_in)
        status_control |= XHCI_TRB_DIR_IN;
    rc = xhci_transfer_ring_enqueue_raw(ring, 0u, 0u,
                                         status_control, &last);
    if (rc != PROS_XHCI_RING_OK)
        return rc;
    *first_trb_address = first;
    *last_trb_address = last;
    return PROS_XHCI_RING_OK;
}

int pros_xhci_transfer_ring_handle_td_event(
    struct pros_xhci_transfer_ring *ring,
    const struct pros_xhci_trb *event,
    uint8_t expected_slot, uint8_t expected_endpoint,
    uint8_t *completion_code)
{
    uint8_t type, code;
    int rc;
    if (!ring || !event || !completion_code ||
        expected_slot == 0u || expected_endpoint == 0u ||
        pros_xhci_event_type(event, &type) != PROS_XHCI_RING_OK ||
        type != 32u || (event->control & (1u << 2)) != 0u ||
        (uint8_t)(event->control >> 24) != expected_slot ||
        (uint8_t)((event->control >> 16) & 0x1Fu) != expected_endpoint ||
        (event->parameter & 0xFu) != 0u)
        return PROS_XHCI_RING_EINVAL;
    code = (uint8_t)(event->status >> 24);
    if (code == 0u)
        return PROS_XHCI_RING_EINVAL;
    rc = pros_xhci_transfer_ring_complete_td(ring, event->parameter);
    if (rc != PROS_XHCI_RING_OK)
        return rc;
    *completion_code = code;
    return PROS_XHCI_RING_OK;
}


int pros_xhci_transfer_ring_enqueue_td(
    struct pros_xhci_transfer_ring *ring,
    const struct pros_usb_dma_buffer *data_dma,
    uint32_t length, uint16_t max_packet_size,
    uint64_t *first_trb_address)
{
    uint32_t trb_count, remaining, offset, index;
    uint64_t ring_bytes;
    if (!ring || !ring->initialized || !ring->trbs || !data_dma ||
        !first_trb_address || length == 0u || max_packet_size == 0u ||
        max_packet_size > 1024u ||
        !data_dma->virtual_address || !data_dma->physical_address ||
        !data_dma->allocation_id || !data_dma->page_count ||
        data_dma->size < length || data_dma->allocation_size < length ||
        (data_dma->physical_address & 0xFu) != 0u ||
        (((uintptr_t)data_dma->virtual_address & 0xFu) != 0u) ||
        (uintptr_t)data_dma->virtual_address > UINTPTR_MAX - length ||
        data_dma->physical_address > UINT64_MAX - length ||
        ring->entry_count < 4u ||
        ring->enqueue_index >= ring->entry_count - 1u ||
        ring->dequeue_index >= ring->entry_count - 1u ||
        ring->producer_cycle > 1u ||
        ring->entry_count > UINT32_MAX / PROS_XHCI_TRB_SIZE)
        return PROS_XHCI_RING_EINVAL;

    trb_count = length / XHCI_NORMAL_TRB_LENGTH_CHUNK +
                ((length % XHCI_NORMAL_TRB_LENGTH_CHUNK) != 0u ? 1u : 0u);
    if (trb_count == 0u || trb_count > XHCI_NORMAL_TD_MAX_TRBS)
        return PROS_XHCI_RING_EINVAL;
    if (ring->outstanding_count > ring->entry_count - 1u ||
        trb_count > (ring->entry_count - 1u) - ring->outstanding_count)
        return PROS_XHCI_RING_EFULL;

    ring_bytes = (uint64_t)ring->entry_count * PROS_XHCI_TRB_SIZE;
    if (ring->physical_address == 0u ||
        (ring->physical_address & 63u) != 0u ||
        (((uintptr_t)ring->trbs & 63u) != 0u) ||
        ring->physical_address > UINT64_MAX - ring_bytes ||
        (uintptr_t)ring->trbs > UINTPTR_MAX - ring_bytes ||
        xhci_dma_ranges_overlap(ring->physical_address, ring_bytes,
                                data_dma->physical_address, length))
        return PROS_XHCI_RING_EINVAL;

    *first_trb_address = ring->physical_address +
                         (uint64_t)ring->enqueue_index * PROS_XHCI_TRB_SIZE;
    remaining = length;
    offset = 0u;
    for (uint32_t i = 0u; i < trb_count; ++i) {
        struct pros_xhci_trb *trb;
        uint32_t chunk = remaining > XHCI_NORMAL_TRB_LENGTH_CHUNK
                             ? XHCI_NORMAL_TRB_LENGTH_CHUNK : remaining;
        uint32_t bytes_after = remaining - chunk;
        uint32_t td_size = bytes_after / max_packet_size +
            ((bytes_after % max_packet_size) != 0u ? 1u : 0u);
        if (td_size > 31u)
            td_size = 31u;
        index = ring->enqueue_index;
        trb = &ring->trbs[index];
        trb->parameter = data_dma->physical_address + offset;
        trb->status = chunk | (td_size << 17);
        trb->control = (XHCI_NORMAL_TRB_TYPE << PROS_XHCI_TRB_TYPE_SHIFT) |
                       (i + 1u < trb_count ? (1u << 4) : XHCI_TRB_IOC);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        trb->control |= ring->producer_cycle ? PROS_XHCI_TRB_CYCLE : 0u;

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
        remaining -= chunk;
        offset += chunk;
    }
    return PROS_XHCI_RING_OK;
}

int pros_xhci_transfer_ring_complete_td(
    struct pros_xhci_transfer_ring *ring,
    uint64_t completed_trb_address)
{
    uint64_t offset, ring_bytes;
    uint32_t usable, completed_index, distance, retire_count;
    if (!ring || !ring->initialized || !ring->trbs ||
        ring->entry_count < 4u ||
        ring->dequeue_index >= ring->entry_count - 1u ||
        ring->outstanding_count == 0u ||
        ring->entry_count > UINT32_MAX / PROS_XHCI_TRB_SIZE ||
        ring->outstanding_count > ring->entry_count - 1u ||
        ring->consumer_cycle > 1u ||
        ring->physical_address == 0u ||
        (ring->physical_address & 63u) != 0u ||
        (((uintptr_t)ring->trbs & 63u) != 0u))
        return PROS_XHCI_RING_EINVAL;
    usable = ring->entry_count - 1u;
    ring_bytes = (uint64_t)ring->entry_count * PROS_XHCI_TRB_SIZE;
    if (ring->physical_address > UINT64_MAX - ring_bytes ||
        (uintptr_t)ring->trbs > UINTPTR_MAX - ring_bytes)
        return PROS_XHCI_RING_EINVAL;
    if (completed_trb_address < ring->physical_address)
        return PROS_XHCI_RING_EINVAL;
    offset = completed_trb_address - ring->physical_address;
    if ((offset % PROS_XHCI_TRB_SIZE) != 0u ||
        offset / PROS_XHCI_TRB_SIZE >= usable)
        return PROS_XHCI_RING_EINVAL;
    completed_index = (uint32_t)(offset / PROS_XHCI_TRB_SIZE);
    distance = (completed_index + usable - ring->dequeue_index) % usable;
    retire_count = distance + 1u;
    if (retire_count > ring->outstanding_count)
        return PROS_XHCI_RING_EINVAL;
    if (retire_count >= usable - ring->dequeue_index)
        ring->consumer_cycle ^= 1u;
    ring->dequeue_index = (completed_index + 1u) % usable;
    ring->outstanding_count -= retire_count;
    return PROS_XHCI_RING_OK;
}

/*
 * Return the xHCI Set TR Dequeue Pointer parameter for the next software-owned
 * TRB. Bit 0 carries the consumer Cycle State (DCS); the ring Link TRB is
 * never exposed as the dequeue target.
 */
int pros_xhci_transfer_ring_dequeue_pointer(
    const struct pros_xhci_transfer_ring *ring, uint64_t *parameter)
{
    uint64_t offset, address;
    if (!ring || !parameter || !ring->initialized || !ring->trbs ||
        ring->entry_count < 4u ||
        ring->dequeue_index >= ring->entry_count - 1u ||
        ring->consumer_cycle > 1u || ring->physical_address == 0u ||
        (ring->physical_address & 63u) != 0u ||
        ring->entry_count > UINT32_MAX / PROS_XHCI_TRB_SIZE)
        return PROS_XHCI_RING_EINVAL;
    offset = (uint64_t)ring->dequeue_index * PROS_XHCI_TRB_SIZE;
    if (ring->physical_address > UINT64_MAX - offset)
        return PROS_XHCI_RING_EINVAL;
    address = ring->physical_address + offset;
    if ((address & 0xFu) != 0u || (address & 1u) != 0u)
        return PROS_XHCI_RING_EINVAL;
    *parameter = address | (uint64_t)ring->consumer_cycle;
    return PROS_XHCI_RING_OK;
}


#define XHCI_RESET_ENDPOINT_COMMAND_TYPE 14u
#define XHCI_SET_TR_DEQUEUE_POINTER_COMMAND_TYPE 16u
#define XHCI_COMMAND_ENDPOINT_SHIFT 16u
#define XHCI_COMMAND_SLOT_SHIFT 24u

int pros_xhci_build_reset_endpoint_command(struct pros_xhci_trb *trb,
                                           uint8_t slot_id,
                                           uint8_t endpoint_id,
                                           uint8_t cycle)
{
    if (!trb || slot_id == 0u || endpoint_id == 0u ||
        endpoint_id > 31u || cycle > 1u)
        return PROS_XHCI_RING_EINVAL;
    trb->parameter = 0u;
    trb->status = 0u;
    trb->control =
        (XHCI_RESET_ENDPOINT_COMMAND_TYPE << PROS_XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)endpoint_id << XHCI_COMMAND_ENDPOINT_SHIFT) |
        ((uint32_t)slot_id << XHCI_COMMAND_SLOT_SHIFT) |
        (cycle ? PROS_XHCI_TRB_CYCLE : 0u);
    return PROS_XHCI_RING_OK;
}

int pros_xhci_build_set_dequeue_command(struct pros_xhci_trb *trb,
                                        uint64_t dequeue_parameter,
                                        uint8_t slot_id,
                                        uint8_t endpoint_id)
{
    if (!trb || slot_id == 0u || endpoint_id == 0u ||
        endpoint_id > 31u || (dequeue_parameter & 0xEu) != 0u ||
        (dequeue_parameter & ~1ull) == 0u ||
        ((dequeue_parameter & ~1ull) & 0xFu) != 0u)
        return PROS_XHCI_RING_EINVAL;
    /*
     * DCS is parameter bit 0, not control bit 16 (which belongs to
     * the Endpoint ID field). Preserve it only in the dequeue pointer.
     */
    trb->parameter = dequeue_parameter;
    trb->status = 0u;
    trb->control =
        (XHCI_SET_TR_DEQUEUE_POINTER_COMMAND_TYPE <<
         PROS_XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)endpoint_id << XHCI_COMMAND_ENDPOINT_SHIFT) |
        ((uint32_t)slot_id << XHCI_COMMAND_SLOT_SHIFT);
    return PROS_XHCI_RING_OK;
}


int pros_xhci_transfer_ring_handle_event(
    struct pros_xhci_transfer_ring *ring,
    const struct pros_xhci_trb *event,
    uint8_t expected_slot, uint8_t expected_endpoint,
    uint8_t *completion_code)
{
    uint8_t type, code;
    int rc;
    if (!ring || !event || !completion_code ||
        expected_slot == 0u || expected_endpoint == 0u ||
        pros_xhci_event_type(event, &type) != PROS_XHCI_RING_OK ||
        type != 32u || (event->control & (1u << 2)) != 0u ||
        (uint8_t)(event->control >> 24) != expected_slot ||
        (uint8_t)((event->control >> 16) & 0x1Fu) != expected_endpoint ||
        (event->parameter & 0xFu) != 0u)
        return PROS_XHCI_RING_EINVAL;
    code = (uint8_t)(event->status >> 24);
    if (code == 0u)
        return PROS_XHCI_RING_EINVAL;
    rc = pros_xhci_transfer_ring_complete(ring, event->parameter);
    if (rc != PROS_XHCI_RING_OK)
        return rc;
    *completion_code = code;
    return PROS_XHCI_RING_OK;
}

int pros_xhci_transfer_ring_selftest(void)
{
    {
        static struct pros_xhci_trb control_trbs[8] __attribute__((aligned(64)));
        static uint8_t control_data[64] __attribute__((aligned(16)));
        struct pros_usb_dma_buffer control_ring_dma = {
            0xC00000u, control_trbs, sizeof(control_trbs),
            sizeof(control_trbs), 1u, 21u
        };
        struct pros_usb_dma_buffer control_data_dma = {
            0xD00000u, control_data, sizeof(control_data),
            sizeof(control_data), 1u, 22u
        };
        struct pros_xhci_transfer_ring control_ring;
        const uint8_t get_device_descriptor[8] =
            {0x80u, 6u, 0u, 1u, 0u, 0u, 8u, 0u};
        const uint8_t set_configuration[8] =
            {0u, 9u, 1u, 0u, 0u, 0u, 0u, 0u};
        struct pros_xhci_trb transfer_event = {0};
        uint64_t first = 0u, last = 0u;
        uint8_t completion = 0u;
        if (pros_xhci_transfer_ring_init(&control_ring,
                &control_ring_dma, 8u) != PROS_XHCI_RING_OK ||
            pros_xhci_transfer_ring_enqueue_control(&control_ring,
                get_device_descriptor, &control_data_dma, 8u, 1u,
                &first, &last) != PROS_XHCI_RING_OK ||
            first != control_ring_dma.physical_address ||
            last != control_ring_dma.physical_address + 32u ||
            control_trbs[0].parameter != 0x0008000001000680ull ||
            control_trbs[0].status != 8u ||
            (control_trbs[0].control & PROS_XHCI_TRB_TYPE_MASK) !=
                (XHCI_SETUP_STAGE_TRB_TYPE << PROS_XHCI_TRB_TYPE_SHIFT) ||
            (control_trbs[0].control & (XHCI_TRB_IDT | XHCI_TRB_CHAIN)) !=
                (XHCI_TRB_IDT | XHCI_TRB_CHAIN) ||
            ((control_trbs[0].control >> 16) & 3u) != 2u ||
            control_trbs[1].parameter != control_data_dma.physical_address ||
            control_trbs[1].status != 8u ||
            (control_trbs[1].control & PROS_XHCI_TRB_TYPE_MASK) !=
                (XHCI_DATA_STAGE_TRB_TYPE << PROS_XHCI_TRB_TYPE_SHIFT) ||
            (control_trbs[1].control & XHCI_TRB_DIR_IN) == 0u ||
            (control_trbs[2].control & PROS_XHCI_TRB_TYPE_MASK) !=
                (XHCI_STATUS_STAGE_TRB_TYPE << PROS_XHCI_TRB_TYPE_SHIFT) ||
            (control_trbs[2].control & XHCI_TRB_DIR_IN) != 0u ||
            (control_trbs[2].control & (1u << 5)) == 0u ||
            control_ring.outstanding_count != 3u)
            return -30;
        transfer_event.parameter = last;
        transfer_event.status = 1u << 24;
        transfer_event.control = (32u << PROS_XHCI_TRB_TYPE_SHIFT) |
                                 (1u << 24) | (1u << 16) |
                                 PROS_XHCI_TRB_CYCLE;
        if (pros_xhci_transfer_ring_handle_td_event(&control_ring,
                &transfer_event, 1u, 1u, &completion) != PROS_XHCI_RING_OK ||
            completion != 1u || control_ring.outstanding_count != 0u)
            return -31;
        if (pros_xhci_transfer_ring_enqueue_control(&control_ring,
                set_configuration, 0, 0u, 0u, &first, &last) !=
                PROS_XHCI_RING_OK ||
            last != control_ring_dma.physical_address + 64u ||
            (control_trbs[4].control & PROS_XHCI_TRB_TYPE_MASK) !=
                (XHCI_STATUS_STAGE_TRB_TYPE << PROS_XHCI_TRB_TYPE_SHIFT) ||
            (control_trbs[4].control & XHCI_TRB_DIR_IN) == 0u ||
            control_ring.outstanding_count != 2u ||
            pros_xhci_transfer_ring_complete_td(&control_ring, last) !=
                PROS_XHCI_RING_OK || control_ring.outstanding_count != 0u)
            return -32;
        {
            uint8_t bad_setup[8] =
                {0x80u, 6u, 0u, 1u, 0u, 0u, 7u, 0u};
            if (pros_xhci_transfer_ring_enqueue_control(&control_ring,
                    bad_setup, &control_data_dma, 8u, 1u,
                    &first, &last) != PROS_XHCI_RING_EINVAL ||
                pros_xhci_transfer_ring_enqueue_control(&control_ring,
                    get_device_descriptor, &control_data_dma, 8u, 0u,
                    &first, &last) != PROS_XHCI_RING_EINVAL)
                return -33;
        }
        /* Exercise a control TD that crosses the Link TRB and toggles cycle. */
        if (pros_xhci_transfer_ring_init(&control_ring,
                &control_ring_dma, 8u) != PROS_XHCI_RING_OK)
            return -34;
        for (uint32_t i = 0u; i < 6u; ++i)
            if (pros_xhci_transfer_ring_enqueue_normal(&control_ring,
                    &control_data_dma, 8u, &first) != PROS_XHCI_RING_OK)
                return -35;
        for (uint32_t i = 0u; i < 6u; ++i)
            if (pros_xhci_transfer_ring_complete(&control_ring,
                    control_ring_dma.physical_address +
                    (uint64_t)i * PROS_XHCI_TRB_SIZE) != PROS_XHCI_RING_OK)
                return -36;
        if (pros_xhci_transfer_ring_enqueue_control(&control_ring,
                get_device_descriptor, &control_data_dma, 8u, 1u,
                &first, &last) != PROS_XHCI_RING_OK ||
            first != control_ring_dma.physical_address + 6u * 16u ||
            last != control_ring_dma.physical_address + 16u ||
            control_ring.enqueue_index != 2u ||
            control_ring.producer_cycle != 0u ||
            (control_trbs[7].control & PROS_XHCI_TRB_TYPE_MASK) !=
                XHCI_LINK_TYPE ||
            (control_trbs[0].control & PROS_XHCI_TRB_CYCLE) != 0u ||
            pros_xhci_transfer_ring_complete_td(&control_ring, last) !=
                PROS_XHCI_RING_OK ||
            control_ring.outstanding_count != 0u ||
            control_ring.consumer_cycle != 0u ||
            pros_xhci_transfer_ring_dequeue_pointer(&control_ring, &first) !=
                PROS_XHCI_RING_OK ||
            first != control_ring_dma.physical_address + 2u * 16u)
            return -37;
    }


    {
        struct pros_xhci_trb command = {0};
        if (pros_xhci_build_reset_endpoint_command(&command, 2u, 5u, 1u) !=
                PROS_XHCI_RING_OK ||
            command.parameter != 0u || command.status != 0u ||
            (command.control & PROS_XHCI_TRB_TYPE_MASK) !=
                (14u << PROS_XHCI_TRB_TYPE_SHIFT) ||
            ((command.control >> 16) & 0x1Fu) != 5u ||
            (uint8_t)(command.control >> 24) != 2u ||
            (command.control & PROS_XHCI_TRB_CYCLE) == 0u ||
            pros_xhci_build_reset_endpoint_command(&command, 0u, 5u, 1u) !=
                PROS_XHCI_RING_EINVAL ||
            pros_xhci_build_reset_endpoint_command(&command, 2u, 32u, 1u) !=
                PROS_XHCI_RING_EINVAL ||
            pros_xhci_build_reset_endpoint_command(&command, 2u, 5u, 2u) !=
                PROS_XHCI_RING_EINVAL)
            return -41;
        if (pros_xhci_build_set_dequeue_command(&command, 0x900001u, 2u, 5u) !=
                PROS_XHCI_RING_OK ||
            command.parameter != 0x900001u || command.status != 0u ||
            (command.control & PROS_XHCI_TRB_TYPE_MASK) !=
                (16u << PROS_XHCI_TRB_TYPE_SHIFT) ||
            ((command.control >> 16) & 0x1Fu) != 5u ||
            (uint8_t)(command.control >> 24) != 2u ||
            (command.control & PROS_XHCI_TRB_CYCLE) != 0u ||
            pros_xhci_build_set_dequeue_command(&command, 0x900003u, 2u, 5u) !=
                PROS_XHCI_RING_EINVAL ||
            pros_xhci_build_set_dequeue_command(&command, 0x900000u, 2u, 5u) !=
                PROS_XHCI_RING_OK ||
            command.parameter != 0x900000u ||
            ((command.control >> 16) & 0x1Fu) != 5u ||
            (command.control & PROS_XHCI_TRB_CYCLE) != 0u ||
            pros_xhci_build_set_dequeue_command(&command, 0x900001u, 2u, 5u) !=
                PROS_XHCI_RING_OK ||
            command.parameter != 0x900001u ||
            ((command.control >> 16) & 0x1Fu) != 5u ||
            pros_xhci_build_set_dequeue_command(&command, 0x900001u, 0u, 5u) !=
                PROS_XHCI_RING_EINVAL)
            return -42;
    }

    static struct pros_xhci_trb trbs[8] __attribute__((aligned(64)));
    static uint8_t data[128] __attribute__((aligned(16)));
    struct pros_usb_dma_buffer ring_dma = {
        0x900000u, trbs, sizeof(trbs), sizeof(trbs), 1u, 9u
    };
    struct pros_usb_dma_buffer data_dma = {
        0xA00000u, data, sizeof(data), sizeof(data), 1u, 10u
    };
    struct pros_xhci_transfer_ring ring;
    uint64_t address = 0u;
    if (pros_xhci_transfer_ring_init(&ring, &ring_dma, 8u) !=
            PROS_XHCI_RING_OK ||
        pros_xhci_transfer_ring_enqueue_normal(&ring, &data_dma, 64u,
                                               &address) != PROS_XHCI_RING_OK ||
        address != ring_dma.physical_address ||
        trbs[0].parameter != data_dma.physical_address ||
        trbs[0].status != 64u ||
        (trbs[0].control & PROS_XHCI_TRB_TYPE_MASK) !=
            (XHCI_NORMAL_TRB_TYPE << PROS_XHCI_TRB_TYPE_SHIFT) ||
        (trbs[0].control & XHCI_TRB_IOC) == 0u ||
        (trbs[0].control & PROS_XHCI_TRB_CYCLE) == 0u)
        return -1;
    {
        uint64_t dequeue_parameter = 0u;
        if (pros_xhci_transfer_ring_dequeue_pointer(&ring,
                &dequeue_parameter) != PROS_XHCI_RING_OK ||
            dequeue_parameter != (ring_dma.physical_address | 1u))
            return -38;
        {
            uint64_t saved_parameter = dequeue_parameter;
            ring.consumer_cycle = 2u;
            if (pros_xhci_transfer_ring_dequeue_pointer(&ring,
                    &dequeue_parameter) != PROS_XHCI_RING_EINVAL ||
                dequeue_parameter != saved_parameter)
                return -40;
            ring.consumer_cycle = 1u;
        }
        if (pros_xhci_transfer_ring_complete(&ring, address) !=
                PROS_XHCI_RING_OK ||
            pros_xhci_transfer_ring_dequeue_pointer(&ring,
                &dequeue_parameter) != PROS_XHCI_RING_OK ||
            dequeue_parameter !=
                ((ring_dma.physical_address + PROS_XHCI_TRB_SIZE) | 1u))
            return -39;
    }
    if (pros_xhci_transfer_ring_init(&ring, &ring_dma, 8u) !=
            PROS_XHCI_RING_OK ||
        pros_xhci_transfer_ring_enqueue_normal(&ring, &data_dma, 64u,
                                               &address) != PROS_XHCI_RING_OK ||
        pros_xhci_transfer_ring_complete(&ring, address + 16u) !=
            PROS_XHCI_RING_EINVAL ||
        pros_xhci_transfer_ring_complete(&ring, address) != PROS_XHCI_RING_OK ||
        ring.outstanding_count != 0u)
        return -2;
    {
        struct pros_usb_dma_buffer bad = data_dma;
        bad.size = 32u;
        if (pros_xhci_transfer_ring_enqueue_normal(&ring, &bad, 64u, &address) !=
                PROS_XHCI_RING_EINVAL)
            return -3;
        bad = data_dma;
        bad.physical_address |= 1u;
        if (pros_xhci_transfer_ring_enqueue_normal(&ring, &bad, 64u, &address) !=
                PROS_XHCI_RING_EINVAL)
            return -4;
        bad = data_dma;
        bad.physical_address = ring_dma.physical_address;
        if (pros_xhci_transfer_ring_enqueue_normal(&ring, &bad, 64u, &address) !=
                PROS_XHCI_RING_EINVAL)
            return -10;
        bad = data_dma;
        bad.virtual_address = (void *)(uintptr_t)(UINTPTR_MAX - 15u);
        if (pros_xhci_transfer_ring_enqueue_td(&ring, &bad, 64u, 64u, &address) !=
                PROS_XHCI_RING_EINVAL)
            return -26;
        if (pros_xhci_transfer_ring_enqueue_normal(&ring, &data_dma, 0u, &address) !=
                PROS_XHCI_RING_EINVAL ||
            pros_xhci_transfer_ring_enqueue_normal(&ring, &data_dma,
                XHCI_NORMAL_LENGTH_MAX + 1u, &address) != PROS_XHCI_RING_EINVAL)
            return -5;
    }
    if (pros_xhci_transfer_ring_init(&ring, &ring_dma, 8u) !=
        PROS_XHCI_RING_OK)
        return -6;
    for (uint32_t i = 0u; i < 7u; ++i)
        if (pros_xhci_transfer_ring_enqueue_normal(&ring, &data_dma, 32u,
                                                   &address) != PROS_XHCI_RING_OK)
            return -7;
    if (ring.enqueue_index != 0u || ring.producer_cycle != 0u ||
        ring.outstanding_count != 7u ||
        (trbs[7].control & PROS_XHCI_TRB_CYCLE) != 0u ||
        pros_xhci_transfer_ring_enqueue_normal(&ring, &data_dma, 32u,
                                               &address) != PROS_XHCI_RING_EFULL)
        return -8;
    if (pros_xhci_transfer_ring_complete(&ring, ring_dma.physical_address) !=
            PROS_XHCI_RING_OK ||
        pros_xhci_transfer_ring_enqueue_normal(&ring, &data_dma, 32u,
                                               &address) != PROS_XHCI_RING_OK ||
        address != ring_dma.physical_address ||
        (trbs[0].control & PROS_XHCI_TRB_CYCLE) != 0u)
        return -9;
    {
        struct pros_xhci_trb transfer_event = {0};
        uint8_t completion_code = 0u;
        if (pros_xhci_transfer_ring_init(&ring, &ring_dma, 8u) !=
                PROS_XHCI_RING_OK ||
            pros_xhci_transfer_ring_enqueue_normal(&ring, &data_dma, 48u,
                                                   &address) != PROS_XHCI_RING_OK)
            return -11;
        transfer_event.parameter = address;
        transfer_event.status = 1u << 24;
        transfer_event.control = (32u << PROS_XHCI_TRB_TYPE_SHIFT) |
                                 (1u << 24) | (2u << 16) |
                                 PROS_XHCI_TRB_CYCLE;
        if (pros_xhci_transfer_ring_handle_event(&ring, &transfer_event,
                1u, 2u, &completion_code) != PROS_XHCI_RING_OK ||
            completion_code != 1u || ring.outstanding_count != 0u)
            return -12;
        if (pros_xhci_transfer_ring_handle_event(&ring, &transfer_event,
                2u, 2u, &completion_code) != PROS_XHCI_RING_EINVAL)
            return -13;
        transfer_event.control |= (1u << 2);
        if (pros_xhci_transfer_ring_handle_event(&ring, &transfer_event,
                1u, 2u, &completion_code) != PROS_XHCI_RING_EINVAL)
            return -14;
        transfer_event.control &= ~(1u << 2);
        transfer_event.parameter |= 1u;
        if (pros_xhci_transfer_ring_handle_event(&ring, &transfer_event,
                1u, 2u, &completion_code) != PROS_XHCI_RING_EINVAL)
            return -15;
        transfer_event.parameter = address;
        transfer_event.status = 0u;
        if (pros_xhci_transfer_ring_handle_event(&ring, &transfer_event,
                1u, 2u, &completion_code) != PROS_XHCI_RING_EINVAL)
            return -16;
        transfer_event.status = 1u << 24;
        transfer_event.control = (33u << PROS_XHCI_TRB_TYPE_SHIFT) |
                                 (1u << 24) | (2u << 16);
        if (pros_xhci_transfer_ring_handle_event(&ring, &transfer_event,
                1u, 2u, &completion_code) != PROS_XHCI_RING_EINVAL)
            return -17;
        transfer_event.control = (32u << PROS_XHCI_TRB_TYPE_SHIFT) |
                                 (1u << 24) | (2u << 16);
        if (pros_xhci_transfer_ring_handle_event(&ring, &transfer_event,
                1u, 2u, &completion_code) != PROS_XHCI_RING_EEMPTY)
            return -18;
    }
    {
        static uint8_t large_data[131120] __attribute__((aligned(16)));
        struct pros_usb_dma_buffer large_dma = {
            0xB00000u, large_data, sizeof(large_data),
            sizeof(large_data), 1u, 15u
        };
        struct pros_xhci_trb *first, *second;
        uint64_t first_address = 0u, last_address;
        if (pros_xhci_transfer_ring_init(&ring, &ring_dma, 8u) !=
                PROS_XHCI_RING_OK ||
            pros_xhci_transfer_ring_enqueue_td(&ring, &large_dma,
                sizeof(large_data), 0u, &first_address) !=
                PROS_XHCI_RING_EINVAL ||
            pros_xhci_transfer_ring_enqueue_td(&ring, &large_dma,
                sizeof(large_data), 8u, &first_address) !=
                PROS_XHCI_RING_OK)
            return -19;
        first = &trbs[0];
        second = &trbs[1];
        last_address = ring_dma.physical_address + PROS_XHCI_TRB_SIZE;
        if (first_address != ring_dma.physical_address ||
            first->parameter != large_dma.physical_address ||
            first->status != (XHCI_NORMAL_TRB_LENGTH_CHUNK | (8u << 17)) ||
            (first->control & (1u << 4)) == 0u ||
            (first->control & XHCI_TRB_IOC) != 0u ||
            second->parameter != large_dma.physical_address +
                                 XHCI_NORMAL_TRB_LENGTH_CHUNK ||
            second->status != (sizeof(large_data) - XHCI_NORMAL_TRB_LENGTH_CHUNK) ||
            (second->control & XHCI_TRB_IOC) == 0u ||
            (second->control & (1u << 4)) != 0u ||
            ring.outstanding_count != 2u ||
            pros_xhci_transfer_ring_complete_td(&ring,
                last_address + 16u) != PROS_XHCI_RING_EINVAL ||
            pros_xhci_transfer_ring_complete_td(&ring, last_address) !=
                PROS_XHCI_RING_OK || ring.outstanding_count != 0u)
            return -20;

        if (pros_xhci_transfer_ring_init(&ring, &ring_dma, 8u) !=
            PROS_XHCI_RING_OK)
            return -21;
        for (uint32_t i = 0u; i < 6u; ++i)
            if (pros_xhci_transfer_ring_enqueue_normal(&ring, &data_dma, 16u,
                                                       &address) !=
                PROS_XHCI_RING_OK)
                return -22;
        for (uint32_t i = 0u; i < 6u; ++i)
            if (pros_xhci_transfer_ring_complete(&ring,
                ring_dma.physical_address + (uint64_t)i * PROS_XHCI_TRB_SIZE) !=
                PROS_XHCI_RING_OK)
                return -23;
        if (pros_xhci_transfer_ring_enqueue_td(&ring, &large_dma,
                sizeof(large_data), 8u, &first_address) != PROS_XHCI_RING_OK ||
            first_address != ring_dma.physical_address +
                             6u * PROS_XHCI_TRB_SIZE ||
            ring.enqueue_index != 1u || ring.producer_cycle != 0u ||
            (trbs[6].control & (1u << 4)) == 0u ||
            (trbs[0].control & XHCI_TRB_IOC) == 0u ||
            (trbs[0].control & PROS_XHCI_TRB_CYCLE) != 0u ||
            pros_xhci_transfer_ring_complete_td(&ring,
                ring_dma.physical_address) != PROS_XHCI_RING_OK ||
            ring.outstanding_count != 0u || ring.consumer_cycle != 0u ||
            pros_xhci_transfer_ring_dequeue_pointer(&ring, &first_address) !=
                PROS_XHCI_RING_OK ||
            first_address != ring_dma.physical_address +
                             1u * PROS_XHCI_TRB_SIZE)
            return -24;
        {
            struct pros_usb_dma_buffer overlap = large_dma;
            overlap.physical_address = ring_dma.physical_address;
            if (pros_xhci_transfer_ring_enqueue_td(&ring, &overlap, 64u, 64u,
                                                   &first_address) !=
                PROS_XHCI_RING_EINVAL)
                return -25;
        }
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
    uint64_t address = 0u, erdp = 0u;
    uint8_t event_type = 0u, completion_code = 0u;

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
    event_trbs[0].status = 0xAB000034u;
    event_trbs[0].control = (32u << PROS_XHCI_TRB_TYPE_SHIFT) | PROS_XHCI_TRB_CYCLE;
    if (pros_xhci_event_ring_erdp_value(&events, &erdp) != PROS_XHCI_RING_OK ||
        erdp != (event_dma.physical_address | (1ull << 3)) ||
        pros_xhci_event_ring_dequeue(&events, &event, &address) != 0 ||
        event.parameter != 0xABCDEFu || event.status != 0xAB000034u ||
        address != event_dma.physical_address ||
        pros_xhci_event_type(&event, &event_type) != PROS_XHCI_RING_OK ||
        event_type != 32u ||
        pros_xhci_event_completion_code(&event, &completion_code) != PROS_XHCI_RING_OK ||
        completion_code != 0xABu)
        return -5;
    if (pros_xhci_event_ring_erdp_value(&events, &erdp) != PROS_XHCI_RING_OK ||
        erdp != ((event_dma.physical_address + PROS_XHCI_TRB_SIZE) | (1ull << 3)))
        return -10;
    for (uint32_t i = 1u; i < 4u; ++i)
        event_trbs[i].control = PROS_XHCI_TRB_CYCLE;
    for (uint32_t i = 1u; i < 4u; ++i)
        if (pros_xhci_event_ring_dequeue(&events, &event, &address) != 0)
            return -6;
    if (events.dequeue_index != 0u || events.consumer_cycle != 0u ||
        pros_xhci_event_ring_erdp_value(&events, &erdp) != PROS_XHCI_RING_OK ||
        erdp != (event_dma.physical_address | (1ull << 3)) ||
        pros_xhci_event_ring_dequeue(&events, &event, &address) !=
            PROS_XHCI_RING_EEMPTY)
        return -7;
    event.control = (34u << PROS_XHCI_TRB_TYPE_SHIFT);
    if (pros_xhci_event_completion_code(&event, &completion_code) !=
            PROS_XHCI_RING_EINVAL ||
        pros_xhci_event_type(0, &event_type) != PROS_XHCI_RING_EINVAL)
        return -11;
    command_dma.physical_address |= 1u;
    if (pros_xhci_command_ring_init(&command, &command_dma, 8u) !=
        PROS_XHCI_RING_EINVAL)
        return -8;
    if (pros_xhci_erst_selftest() != PROS_XHCI_RING_OK)
        return -9;
    {
        /* Keep this fixture independent of the earlier deliberate DMA corruption test. */
        static struct pros_xhci_trb completion_trbs[8] __attribute__((aligned(64)));
        struct pros_usb_dma_buffer completion_dma = {
            0x800000u, completion_trbs, sizeof(completion_trbs),
            sizeof(completion_trbs), 1u, 8u
        };
        struct pros_xhci_command_ring completion_ring;
        struct pros_xhci_trb completion_event = {0};
        uint8_t completion_code = 0u;
        uint64_t command_address = 0u;
        if (pros_xhci_command_ring_init(&completion_ring, &completion_dma, 8u) !=
                PROS_XHCI_RING_OK ||
            pros_xhci_command_ring_enqueue(&completion_ring, 0xCAFEu, 0u,
                1u << PROS_XHCI_TRB_TYPE_SHIFT, &command_address) !=
                PROS_XHCI_RING_OK)
            return -12;
        completion_event.parameter = command_address;
        completion_event.status = 1u << 24; /* Success completion code */
        completion_event.control = (33u << PROS_XHCI_TRB_TYPE_SHIFT) |
                                   PROS_XHCI_TRB_CYCLE;
        if (pros_xhci_command_ring_handle_completion(&completion_ring,
                &completion_event, &completion_code) != PROS_XHCI_RING_OK ||
            completion_code != 1u || completion_ring.outstanding_count != 0u)
            return -13;
        if (pros_xhci_command_ring_handle_completion(&completion_ring,
                &completion_event, &completion_code) !=
                PROS_XHCI_RING_EEMPTY)
            return -14;
        completion_event.control = (32u << PROS_XHCI_TRB_TYPE_SHIFT);
        if (pros_xhci_command_ring_handle_completion(&completion_ring,
                &completion_event, &completion_code) !=
                PROS_XHCI_RING_EINVAL)
            return -15;
        completion_event.control = (33u << PROS_XHCI_TRB_TYPE_SHIFT);
        completion_event.parameter = command_address + 1u;
        if (pros_xhci_command_ring_handle_completion(&completion_ring,
                &completion_event, &completion_code) !=
                PROS_XHCI_RING_EINVAL)
            return -16;
        completion_event.parameter = command_address;
        completion_event.status = 0u;
        if (pros_xhci_command_ring_handle_completion(&completion_ring,
                &completion_event, &completion_code) !=
                PROS_XHCI_RING_EINVAL)
            return -17;
    }
    {
        int transfer_rc = pros_xhci_transfer_ring_selftest();
        if (transfer_rc != PROS_XHCI_RING_OK)
            return transfer_rc;
    }
    return PROS_XHCI_RING_OK;
}

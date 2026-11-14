#include "usb_dma.h"
#include "memory.h"

#define USB_DMA_PHYS_LIMIT PROSENJIT_PHYS_LIMIT
#define USB_DMA_MAX_ALLOCS 64u

struct usb_dma_record {
    uint64_t physical_address;
    uint64_t page_count;
    uint64_t allocation_id;
    uint8_t active;
};

static struct usb_dma_record usb_dma_records[USB_DMA_MAX_ALLOCS];
static uint64_t usb_dma_next_id = 1u;
static volatile uint32_t usb_dma_lock_word;
/* Targeted one-shot fault injection used only by the DMA allocator self-test. */
static uint64_t usb_dma_test_fail_free_id;

static void usb_dma_lock(void) {
    while (__atomic_exchange_n(&usb_dma_lock_word, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
}
static void usb_dma_unlock(void) {
    __atomic_store_n(&usb_dma_lock_word, 0u, __ATOMIC_RELEASE);
}
static int usb_dma_is_power_of_two(uint64_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}
static void usb_dma_zero(void *address, uint64_t length) {
    volatile uint8_t *bytes = (volatile uint8_t *)address;
    for (uint64_t i = 0u; i < length; ++i)
        bytes[i] = 0u;
}
static void usb_dma_clear_descriptor(struct pros_usb_dma_buffer *buffer) {
    buffer->physical_address = 0u;
    buffer->virtual_address = 0;
    buffer->size = 0u;
    buffer->allocation_size = 0u;
    buffer->page_count = 0u;
    buffer->allocation_id = 0u;
}

int pros_usb_dma_alloc(uint64_t size, uint64_t alignment,
                       struct pros_usb_dma_buffer *buffer) {
    uint64_t page_count, alignment_pages, allocation_size, physical, id;
    uint32_t slot = USB_DMA_MAX_ALLOCS;

    if (!buffer)
        return PROS_USB_DMA_EINVAL;
    usb_dma_clear_descriptor(buffer);
    if (size == 0u || alignment < PROSENJIT_PAGE_SIZE ||
        !usb_dma_is_power_of_two(alignment) ||
        (alignment & (PROSENJIT_PAGE_SIZE - 1u)) != 0u ||
        size > UINT64_MAX - (PROSENJIT_PAGE_SIZE - 1u))
        return PROS_USB_DMA_EINVAL;

    page_count = (size + PROSENJIT_PAGE_SIZE - 1u) /
                 PROSENJIT_PAGE_SIZE;
    alignment_pages = alignment / PROSENJIT_PAGE_SIZE;
    if (page_count == 0u || page_count > USB_DMA_PHYS_LIMIT /
                                           PROSENJIT_PAGE_SIZE ||
        page_count > UINT64_MAX / PROSENJIT_PAGE_SIZE)
        return PROS_USB_DMA_EINVAL;
    allocation_size = page_count * PROSENJIT_PAGE_SIZE;

    usb_dma_lock();
    for (uint32_t i = 0u; i < USB_DMA_MAX_ALLOCS; ++i) {
        if (!usb_dma_records[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == USB_DMA_MAX_ALLOCS) {
        usb_dma_unlock();
        return PROS_USB_DMA_ENOMEM;
    }

    physical = physical_page_alloc_contiguous(page_count, alignment_pages);
    if (physical == 0u) {
        usb_dma_unlock();
        return PROS_USB_DMA_ENOMEM;
    }
    if (physical >= USB_DMA_PHYS_LIMIT ||
        allocation_size > USB_DMA_PHYS_LIMIT - physical ||
        (physical & (alignment - 1u)) != 0u) {
        (void)physical_page_free_contiguous(physical, page_count);
        usb_dma_unlock();
        return PROS_USB_DMA_ESTATE;
    }

    /* The kernel keeps the allocator-managed first GiB identity mapped. */
    usb_dma_zero((void *)(uintptr_t)physical, allocation_size);
    id = usb_dma_next_id;
    if (id == 0u) {
        (void)physical_page_free_contiguous(physical, page_count);
        usb_dma_unlock();
        return PROS_USB_DMA_ESTATE;
    }
    /* Never wrap/reuse an ownership token: exhaustion permanently fails closed. */
    usb_dma_next_id = (id == UINT64_MAX) ? 0u : id + 1u;
    usb_dma_records[slot].physical_address = physical;
    usb_dma_records[slot].page_count = page_count;
    usb_dma_records[slot].allocation_id = id;
    usb_dma_records[slot].active = 1u;
    buffer->physical_address = physical;
    buffer->virtual_address = (void *)(uintptr_t)physical;
    buffer->size = size;
    buffer->allocation_size = allocation_size;
    buffer->page_count = page_count;
    buffer->allocation_id = id;
    usb_dma_unlock();
    return PROS_USB_DMA_OK;
}

int pros_usb_dma_free(struct pros_usb_dma_buffer *buffer) {
    uint32_t slot = USB_DMA_MAX_ALLOCS;
    if (!buffer || buffer->physical_address == 0u ||
        !buffer->virtual_address || buffer->size == 0u ||
        buffer->allocation_size == 0u || buffer->page_count == 0u ||
        buffer->allocation_id == 0u ||
        buffer->page_count > UINT64_MAX / PROSENJIT_PAGE_SIZE ||
        buffer->allocation_size !=
            buffer->page_count * PROSENJIT_PAGE_SIZE ||
        buffer->size > buffer->allocation_size ||
        buffer->virtual_address !=
            (void *)(uintptr_t)buffer->physical_address)
        return PROS_USB_DMA_EINVAL;

    usb_dma_lock();
    for (uint32_t i = 0u; i < USB_DMA_MAX_ALLOCS; ++i) {
        if (usb_dma_records[i].active &&
            usb_dma_records[i].physical_address == buffer->physical_address &&
            usb_dma_records[i].page_count == buffer->page_count &&
            usb_dma_records[i].allocation_id == buffer->allocation_id) {
            slot = i;
            break;
        }
    }
    if (slot == USB_DMA_MAX_ALLOCS) {
        usb_dma_unlock();
        return PROS_USB_DMA_ESTATE;
    }

    /* Deterministically exercise the retry contract without freeing the pages. */
    if (__atomic_load_n(&usb_dma_test_fail_free_id, __ATOMIC_ACQUIRE) ==
        buffer->allocation_id) {
        __atomic_store_n(&usb_dma_test_fail_free_id, 0u, __ATOMIC_RELEASE);
        usb_dma_unlock();
        return PROS_USB_DMA_ESTATE;
    }

    /* Scrub the full rounded allocation before returning pages to the allocator. */
    usb_dma_zero(buffer->virtual_address, buffer->allocation_size);
    if (physical_page_free_contiguous(buffer->physical_address,
                                      buffer->page_count) != 0) {
        usb_dma_unlock();
        return PROS_USB_DMA_ESTATE;
    }
    usb_dma_records[slot].active = 0u;
    usb_dma_records[slot].physical_address = 0u;
    usb_dma_records[slot].page_count = 0u;
    usb_dma_records[slot].allocation_id = 0u;
    usb_dma_clear_descriptor(buffer);
    usb_dma_unlock();
    return PROS_USB_DMA_OK;
}


/*
 * Arm one deterministic free failure for an active allocation. This test seam
 * validates the complete ownership token while holding the allocator lock;
 * the failure is consumed by the next matching pros_usb_dma_free() call.
 */
int pros_usb_dma_test_fail_next_free(
    const struct pros_usb_dma_buffer *buffer) {
    uint32_t slot = USB_DMA_MAX_ALLOCS;

    if (!buffer || buffer->physical_address == 0u ||
        buffer->allocation_id == 0u)
        return PROS_USB_DMA_EINVAL;

    usb_dma_lock();
    if (__atomic_load_n(&usb_dma_test_fail_free_id, __ATOMIC_ACQUIRE) != 0u) {
        usb_dma_unlock();
        return PROS_USB_DMA_ESTATE;
    }
    for (uint32_t i = 0u; i < USB_DMA_MAX_ALLOCS; ++i) {
        if (usb_dma_records[i].active &&
            usb_dma_records[i].physical_address == buffer->physical_address &&
            usb_dma_records[i].page_count == buffer->page_count &&
            usb_dma_records[i].allocation_id == buffer->allocation_id) {
            slot = i;
            break;
        }
    }
    if (slot == USB_DMA_MAX_ALLOCS) {
        usb_dma_unlock();
        return PROS_USB_DMA_ESTATE;
    }
    __atomic_store_n(&usb_dma_test_fail_free_id, buffer->allocation_id,
                     __ATOMIC_RELEASE);
    usb_dma_unlock();
    return PROS_USB_DMA_OK;
}

int pros_usb_dma_selftest(void) {
    struct pros_usb_dma_buffer buffer, stale, replacement, retry;
    volatile uint8_t *bytes;
    uint64_t free_before = physical_memory_free_pages();

    if (pros_usb_dma_alloc(6000u, 8192u, &buffer) != PROS_USB_DMA_OK)
        return -1;
    if (buffer.physical_address == 0u ||
        buffer.physical_address >= USB_DMA_PHYS_LIMIT ||
        (buffer.physical_address & 8191u) != 0u ||
        buffer.size != 6000u || buffer.allocation_size != 8192u ||
        buffer.page_count != 2u || buffer.allocation_id == 0u ||
        buffer.virtual_address != (void *)(uintptr_t)buffer.physical_address)
        return -2;

    bytes = (volatile uint8_t *)buffer.virtual_address;
    for (uint64_t i = 0u; i < buffer.allocation_size; ++i) {
        if (bytes[i] != 0u)
            return -3;
    }
    bytes[0] = 0xA5u;
    bytes[buffer.size - 1u] = 0x5Au;
    if (bytes[0] != 0xA5u || bytes[buffer.size - 1u] != 0x5Au)
        return -4;

    stale = buffer;
    if (pros_usb_dma_free(&buffer) != PROS_USB_DMA_OK ||
        buffer.physical_address != 0u || buffer.virtual_address != 0 ||
        buffer.page_count != 0u || buffer.allocation_id != 0u ||
        physical_memory_free_pages() != free_before)
        return -5;
    if (((volatile uint8_t *)(uintptr_t)stale.physical_address)[0] != 0u)
        return -6;
    if (pros_usb_dma_alloc(4096u, 4096u, &replacement) != PROS_USB_DMA_OK)
        return -7;
    if (pros_usb_dma_free(&stale) != PROS_USB_DMA_ESTATE)
        return -8;
    if (pros_usb_dma_free(&replacement) != PROS_USB_DMA_OK)
        return -9;
    if (pros_usb_dma_free(&buffer) != PROS_USB_DMA_EINVAL)
        return -10;

    if (pros_usb_dma_alloc(0u, 4096u, &buffer) != PROS_USB_DMA_EINVAL ||
        pros_usb_dma_alloc(4096u, 3000u, &buffer) != PROS_USB_DMA_EINVAL ||
        pros_usb_dma_alloc(UINT64_MAX, 4096u, &buffer) != PROS_USB_DMA_EINVAL)
        return -11;

    if (pros_usb_dma_alloc(4096u, 4096u, &retry) != PROS_USB_DMA_OK)
        return -12;
    __atomic_store_n(&usb_dma_test_fail_free_id, retry.allocation_id,
                     __ATOMIC_RELEASE);
    if (pros_usb_dma_free(&retry) != PROS_USB_DMA_ESTATE ||
        retry.physical_address == 0u || retry.allocation_id == 0u)
        return -13;
    if (pros_usb_dma_free(&retry) != PROS_USB_DMA_OK ||
        retry.physical_address != 0u || retry.allocation_id != 0u)
        return -14;

    return physical_memory_free_pages() == free_before ?
           PROS_USB_DMA_OK : -15;
}

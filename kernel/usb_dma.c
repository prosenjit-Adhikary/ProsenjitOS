#include "usb_dma.h"
#include "memory.h"

#define USB_DMA_PHYS_LIMIT PROSENJIT_PHYS_LIMIT

static int usb_dma_is_power_of_two(uint64_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static void usb_dma_zero(void *address, uint64_t length) {
    volatile uint8_t *bytes = (volatile uint8_t *)address;
    for (uint64_t i = 0u; i < length; ++i)
        bytes[i] = 0u;
}

int pros_usb_dma_alloc(uint64_t size, uint64_t alignment,
                       struct pros_usb_dma_buffer *buffer) {
    uint64_t page_count, alignment_pages, allocation_size, physical;

    if (!buffer)
        return PROS_USB_DMA_EINVAL;
    buffer->physical_address = 0u;
    buffer->virtual_address = 0;
    buffer->size = 0u;
    buffer->allocation_size = 0u;
    buffer->page_count = 0u;

    if (size == 0u || alignment < PROSENJIT_PAGE_SIZE ||
        !usb_dma_is_power_of_two(alignment) ||
        (alignment & (PROSENJIT_PAGE_SIZE - 1u)) != 0u ||
        size > UINT64_MAX - (PROSENJIT_PAGE_SIZE - 1u))
        return PROS_USB_DMA_EINVAL;

    page_count = (size + PROSENJIT_PAGE_SIZE - 1u) /
                 PROSENJIT_PAGE_SIZE;
    alignment_pages = alignment / PROSENJIT_PAGE_SIZE;
    if (page_count == 0u || page_count > USB_DMA_PHYS_LIMIT /
                                           PROSENJIT_PAGE_SIZE)
        return PROS_USB_DMA_EINVAL;
    if (page_count > UINT64_MAX / PROSENJIT_PAGE_SIZE)
        return PROS_USB_DMA_EINVAL;
    allocation_size = page_count * PROSENJIT_PAGE_SIZE;

    physical = physical_page_alloc_contiguous(page_count, alignment_pages);
    if (physical == 0u)
        return PROS_USB_DMA_ENOMEM;
    if (physical >= USB_DMA_PHYS_LIMIT ||
        allocation_size > USB_DMA_PHYS_LIMIT - physical ||
        (physical & (alignment - 1u)) != 0u) {
        (void)physical_page_free_contiguous(physical, page_count);
        return PROS_USB_DMA_ESTATE;
    }

    /* The kernel keeps the allocator-managed first GiB identity mapped. */
    usb_dma_zero((void *)(uintptr_t)physical, allocation_size);

    buffer->physical_address = physical;
    buffer->virtual_address = (void *)(uintptr_t)physical;
    buffer->size = size;
    buffer->allocation_size = allocation_size;
    buffer->page_count = page_count;
    return PROS_USB_DMA_OK;
}

int pros_usb_dma_free(struct pros_usb_dma_buffer *buffer) {
    if (!buffer || buffer->physical_address == 0u ||
        !buffer->virtual_address || buffer->size == 0u ||
        buffer->allocation_size == 0u || buffer->page_count == 0u ||
        buffer->page_count > UINT64_MAX / PROSENJIT_PAGE_SIZE ||
        buffer->allocation_size !=
            buffer->page_count * PROSENJIT_PAGE_SIZE ||
        buffer->virtual_address !=
            (void *)(uintptr_t)buffer->physical_address)
        return PROS_USB_DMA_EINVAL;

    if (physical_page_free_contiguous(buffer->physical_address,
                                      buffer->page_count) != 0)
        return PROS_USB_DMA_ESTATE;

    buffer->physical_address = 0u;
    buffer->virtual_address = 0;
    buffer->size = 0u;
    buffer->allocation_size = 0u;
    buffer->page_count = 0u;
    return PROS_USB_DMA_OK;
}

int pros_usb_dma_selftest(void) {
    struct pros_usb_dma_buffer buffer;
    volatile uint8_t *bytes;
    uint64_t free_before = physical_memory_free_pages();

    if (pros_usb_dma_alloc(6000u, 8192u, &buffer) != PROS_USB_DMA_OK)
        return -1;
    if (buffer.physical_address == 0u ||
        buffer.physical_address >= USB_DMA_PHYS_LIMIT ||
        (buffer.physical_address & 8191u) != 0u ||
        buffer.size != 6000u || buffer.allocation_size != 8192u ||
        buffer.page_count != 2u ||
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

    if (pros_usb_dma_free(&buffer) != PROS_USB_DMA_OK ||
        buffer.physical_address != 0u || buffer.virtual_address != 0 ||
        buffer.page_count != 0u ||
        physical_memory_free_pages() != free_before)
        return -5;
    if (pros_usb_dma_free(&buffer) != PROS_USB_DMA_EINVAL)
        return -6;

    if (pros_usb_dma_alloc(0u, 4096u, &buffer) != PROS_USB_DMA_EINVAL ||
        pros_usb_dma_alloc(4096u, 3000u, &buffer) != PROS_USB_DMA_EINVAL ||
        pros_usb_dma_alloc(UINT64_MAX, 4096u, &buffer) != PROS_USB_DMA_EINVAL)
        return -7;
    return PROS_USB_DMA_OK;
}

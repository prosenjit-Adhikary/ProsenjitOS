#ifndef PROSENJIT_USB_DMA_H
#define PROSENJIT_USB_DMA_H

#include <stdint.h>

#define PROS_USB_DMA_OK 0
#define PROS_USB_DMA_EINVAL -1
#define PROS_USB_DMA_ENOMEM -2
#define PROS_USB_DMA_ESTATE -3

/*
 * DMA buffers are physically contiguous, page-backed, and identity-mapped.
 * The current physical allocator limits them to the first 1 GiB, so their
 * bus addresses fit in 32 bits. This is a memory primitive only: it does not
 * enable PCI bus mastering or establish an IOMMU mapping. The caller must
 * quiesce the device before freeing a buffer; this allocator is not ISR-safe.
 */
struct pros_usb_dma_buffer {
    uint64_t physical_address;
    void *virtual_address;
    uint64_t size;
    uint64_t allocation_size;
    uint64_t page_count;
    /* Monotonic ownership token prevents stale copied descriptors freeing reuse. */
    uint64_t allocation_id;
};

int pros_usb_dma_alloc(uint64_t size, uint64_t alignment,
                       struct pros_usb_dma_buffer *buffer);
int pros_usb_dma_free(struct pros_usb_dma_buffer *buffer);
/* One-shot fault injection for deterministic caller cleanup regression tests. */
int pros_usb_dma_test_fail_next_free(const struct pros_usb_dma_buffer *buffer);
int pros_usb_dma_test_fail_alloc_after(uint32_t successful_allocations);
void pros_usb_dma_test_clear_alloc_failure(void);
int pros_usb_dma_selftest(void);

#endif

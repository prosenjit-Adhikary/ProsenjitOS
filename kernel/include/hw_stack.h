#ifndef PROSENJIT_HW_STACK_H
#define PROSENJIT_HW_STACK_H
#include <stdint.h>
#include "driver.h"
#include "pci.h"
#define PROS_HW_STACK_ABI_VERSION 1u
#define PROS_HW_ENDPOINT_MAX 16u
#define PROS_HW_TRANSFER_MAX (16u * 1024u * 1024u)
#define PROS_HW_RESOURCE_MAX (256u * 1024u * 1024u)
enum pros_hw_endpoint_type { PROS_HW_ENDPOINT_DISPLAY=1, PROS_HW_ENDPOINT_INPUT=2, PROS_HW_ENDPOINT_AUDIO=3 };
enum pros_hw_resource_type { PROS_HW_RESOURCE_MMIO=1, PROS_HW_RESOURCE_IOPORT=2, PROS_HW_RESOURCE_IRQ=3, PROS_HW_RESOURCE_DMA=4 };
struct pros_hw_resource { uint32_t version, type; uint64_t base, length; uint32_t irq, flags; };
struct pros_hw_endpoint_ops {
    int (*init)(void *context);
    int (*transfer)(void *context, const void *buffer, uint32_t length);
    int (*shutdown)(void *context);
};
struct pros_hw_endpoint_status { uint32_t version, type, device_id, initialized; int32_t last_error; };
int pros_hw_classify_pci(const struct pros_pci_device *, enum pros_device_class *);
int pros_hw_resource_validate(const struct pros_hw_resource *);
int pros_hw_stack_init(void);
int pros_hw_endpoint_register(uint32_t, enum pros_hw_endpoint_type, const struct pros_hw_endpoint_ops *, void *);
int pros_hw_endpoint_submit(uint32_t, enum pros_hw_endpoint_type, const void *, uint32_t);
int pros_hw_endpoint_status(uint32_t, enum pros_hw_endpoint_type, struct pros_hw_endpoint_status *);
int pros_hw_endpoint_unregister(uint32_t, enum pros_hw_endpoint_type);
int pros_hw_stack_selftest(void);
#endif

#ifndef PROSENJIT_USB_XHCI_RING_H
#define PROSENJIT_USB_XHCI_RING_H
#include <stdint.h>
#include "usb_dma.h"
#define PROS_XHCI_TRB_SIZE 16u
struct pros_xhci_trb { uint64_t parameter; uint32_t status; uint32_t control; } __attribute__((packed, aligned(16)));
#endif

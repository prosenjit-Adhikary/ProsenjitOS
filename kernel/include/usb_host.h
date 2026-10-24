#ifndef PROSENJIT_USB_HOST_H
#define PROSENJIT_USB_HOST_H
#include <stdint.h>

/* Classification only. These values do not imply an implemented controller. */
enum pros_usb_host_controller {
    PROS_USB_HC_UNSUPPORTED = 0,
    PROS_USB_HC_UHCI,
    PROS_USB_HC_OHCI,
    PROS_USB_HC_EHCI,
    PROS_USB_HC_XHCI,
    PROS_USB_HC_OTHER
};

/* PCI base class 0x0C, subclass 0x03, programming interface byte. */
int pros_usb_host_classify_pci(uint8_t class_code, uint8_t subclass,
                               uint8_t prog_if,
                               enum pros_usb_host_controller *controller);
const char *pros_usb_host_controller_name(enum pros_usb_host_controller controller);

#endif

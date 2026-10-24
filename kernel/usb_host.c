#include "usb_host.h"
#include "driver.h"

int pros_usb_host_classify_pci(uint8_t class_code, uint8_t subclass,
                               uint8_t prog_if,
                               enum pros_usb_host_controller *controller)
{
    if (!controller) return PROS_DRIVER_EINVAL;
    *controller = PROS_USB_HC_UNSUPPORTED;
    if (class_code != 0x0Cu || subclass != 0x03u)
        return PROS_DRIVER_ENODEV;

    switch (prog_if) {
    case 0x00u: *controller = PROS_USB_HC_UHCI; break;
    case 0x10u: *controller = PROS_USB_HC_OHCI; break;
    case 0x20u: *controller = PROS_USB_HC_EHCI; break;
    case 0x30u: *controller = PROS_USB_HC_XHCI; break;
    case 0x80u: *controller = PROS_USB_HC_OTHER; break;
    default: return PROS_DRIVER_ENOTSUP;
    }
    return PROS_DRIVER_OK;
}

const char *pros_usb_host_controller_name(enum pros_usb_host_controller controller)
{
    switch (controller) {
    case PROS_USB_HC_UHCI: return "UHCI";
    case PROS_USB_HC_OHCI: return "OHCI";
    case PROS_USB_HC_EHCI: return "EHCI";
    case PROS_USB_HC_XHCI: return "xHCI";
    case PROS_USB_HC_OTHER: return "other USB host controller";
    default: return "unsupported";
    }
}

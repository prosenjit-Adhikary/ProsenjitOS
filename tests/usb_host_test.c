#include <assert.h>
#include "usb_host.h"
#include "driver.h"

int main(void)
{
    enum pros_usb_host_controller hc = PROS_USB_HC_XHCI;
    assert(pros_usb_host_classify_pci(0x0Cu, 0x03u, 0x00u, &hc) == PROS_DRIVER_OK);
    assert(hc == PROS_USB_HC_UHCI);
    assert(pros_usb_host_classify_pci(0x0Cu, 0x03u, 0x10u, &hc) == PROS_DRIVER_OK);
    assert(hc == PROS_USB_HC_OHCI);
    assert(pros_usb_host_classify_pci(0x0Cu, 0x03u, 0x20u, &hc) == PROS_DRIVER_OK);
    assert(hc == PROS_USB_HC_EHCI);
    assert(pros_usb_host_classify_pci(0x0Cu, 0x03u, 0x30u, &hc) == PROS_DRIVER_OK);
    assert(hc == PROS_USB_HC_XHCI);
    assert(pros_usb_host_classify_pci(0x0Cu, 0x03u, 0x80u, &hc) == PROS_DRIVER_OK);
    assert(hc == PROS_USB_HC_OTHER);
    assert(pros_usb_host_classify_pci(0x02u, 0x00u, 0x30u, &hc) == PROS_DRIVER_ENODEV);
    assert(hc == PROS_USB_HC_UNSUPPORTED);
    assert(pros_usb_host_classify_pci(0x0Cu, 0x03u, 0x40u, &hc) == PROS_DRIVER_ENOTSUP);
    assert(hc == PROS_USB_HC_UNSUPPORTED);
    assert(pros_usb_host_classify_pci(0x0Cu, 0x03u, 0x30u, 0) == PROS_DRIVER_EINVAL);
    assert(pros_usb_host_controller_name(PROS_USB_HC_XHCI)[0] == 'x');
    return 0;
}

#ifndef PROSENJIT_USB_DESC_H
#define PROSENJIT_USB_DESC_H

#include <stdint.h>

#define PROS_USB_DESC_OK 0
#define PROS_USB_DESC_EINVAL -1
#define PROS_USB_DESC_EFORMAT -2
#define PROS_USB_DESC_ENOTFOUND -3
#define PROS_USB_DESC_EUNSUPPORTED -4

struct pros_usb_msc_interface {
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
    uint16_t max_packet_in;
    uint16_t max_packet_out;
    uint8_t configuration_value;
};

/*
 * Select exactly one USB MSC/SCSI-transparent/Bulk-Only interface (class
 * 08h/subclass 06h/protocol 50h), alternate setting zero, with one bulk IN
 * and one bulk OUT endpoint. The descriptor buffer is read-only.
 */
int pros_usb_msc_select_interface(const uint8_t *descriptors,
                                  uint32_t descriptor_length,
                                  struct pros_usb_msc_interface *out);
int pros_usb_desc_selftest(void);

#endif

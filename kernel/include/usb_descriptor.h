#ifndef PROSENJIT_USB_DESCRIPTOR_H
#define PROSENJIT_USB_DESCRIPTOR_H
#include <stdint.h>

#define PROS_USB_DESC_CONFIGURATION 0x02u
#define PROS_USB_DESC_INTERFACE     0x04u
#define PROS_USB_DESC_ENDPOINT      0x05u
#define PROS_USB_CLASS_HID          0x03u
#define PROS_USB_SUBCLASS_BOOT      0x01u
#define PROS_USB_PROTOCOL_KEYBOARD  0x01u
#define PROS_USB_PROTOCOL_MOUSE     0x02u
#define PROS_USB_ENDPOINT_IN        0x80u
#define PROS_USB_ENDPOINT_TRANSFER_MASK 0x03u
#define PROS_USB_ENDPOINT_INTERRUPT 0x03u

enum pros_usb_hid_kind {
    PROS_USB_HID_NONE = 0,
    PROS_USB_HID_KEYBOARD,
    PROS_USB_HID_MOUSE
};

struct pros_usb_hid_endpoint {
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_address;
    uint8_t interval;
    uint16_t max_packet_size;
    enum pros_usb_hid_kind kind;
};

/*
 * Parse a complete configuration descriptor blob already obtained from a
 * transport. Never reads beyond blob_length. Selects a boot-protocol HID
 * interface with an interrupt-IN endpoint. This does not fetch USB descriptors.
 */
int pros_usb_find_boot_hid_endpoint(const uint8_t *blob, uint32_t blob_length,
                                    struct pros_usb_hid_endpoint *out);

#endif

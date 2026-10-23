#include "usb_descriptor.h"
#include "driver.h"

int pros_usb_find_boot_hid_endpoint(const uint8_t *blob, uint32_t blob_length,
                                    struct pros_usb_hid_endpoint *out)
{
    if (!blob || !out || blob_length < 9u) return PROS_DRIVER_EINVAL;
    if (blob[0] < 9u || blob[1] != PROS_USB_DESC_CONFIGURATION)
        return PROS_DRIVER_EIO;

    uint32_t total = (uint32_t)blob[2] | ((uint32_t)blob[3] << 8);
    if (total < 9u || total > blob_length) return PROS_DRIVER_EIO;

    uint8_t current_interface = 0u, current_alt = 0u;
    enum pros_usb_hid_kind current_kind = PROS_USB_HID_NONE;
    uint8_t candidate_interface = 0u, candidate_alt = 0u;
    enum pros_usb_hid_kind candidate_kind = PROS_USB_HID_NONE;
    uint8_t found_interface = 0u;
    uint32_t offset = 0u;

    while (offset < total) {
        if (total - offset < 2u) return PROS_DRIVER_EIO;
        uint8_t length = blob[offset];
        uint8_t type = blob[offset + 1u];
        if (length < 2u || (uint32_t)length > total - offset)
            return PROS_DRIVER_EIO;

        if (offset == 0u) {
            if (type != PROS_USB_DESC_CONFIGURATION || length < 9u)
                return PROS_DRIVER_EIO;
        } else if (type == PROS_USB_DESC_INTERFACE) {
            if (length < 9u) return PROS_DRIVER_EIO;
            current_interface = blob[offset + 2u];
            current_alt = blob[offset + 3u];
            current_kind = PROS_USB_HID_NONE;
            if (blob[offset + 5u] == PROS_USB_CLASS_HID &&
                blob[offset + 6u] == PROS_USB_SUBCLASS_BOOT) {
                if (blob[offset + 7u] == PROS_USB_PROTOCOL_KEYBOARD)
                    current_kind = PROS_USB_HID_KEYBOARD;
                else if (blob[offset + 7u] == PROS_USB_PROTOCOL_MOUSE)
                    current_kind = PROS_USB_HID_MOUSE;
            }
        } else if (type == PROS_USB_DESC_ENDPOINT) {
            if (length < 7u) return PROS_DRIVER_EIO;
            uint8_t address = blob[offset + 2u];
            uint8_t attributes = blob[offset + 3u];
            uint16_t packet = (uint16_t)blob[offset + 4u] |
                              ((uint16_t)blob[offset + 5u] << 8);
            if (current_kind != PROS_USB_HID_NONE &&
                (address & PROS_USB_ENDPOINT_IN) != 0u &&
                (attributes & PROS_USB_ENDPOINT_TRANSFER_MASK) ==
                    PROS_USB_ENDPOINT_INTERRUPT) {
                uint16_t packet_size = (uint16_t)(packet & 0x07FFu);
                if (packet_size == 0u || (packet & 0xF800u) != 0u)
                    return PROS_DRIVER_EIO;
                candidate_interface = current_interface;
                candidate_alt = current_alt;
                candidate_kind = current_kind;
                out->endpoint_address = address;
                out->max_packet_size = packet_size;
                out->interval = blob[offset + 6u];
                found_interface = 1u;
            }
        }
        offset += length;
    }

    if (offset != total) return PROS_DRIVER_EIO;
    if (!found_interface) return PROS_DRIVER_ENODEV;
    out->interface_number = candidate_interface;
    out->alternate_setting = candidate_alt;
    out->kind = candidate_kind;
    return PROS_DRIVER_OK;
}

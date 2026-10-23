#include "usb_descriptor.h"
#include "driver.h"

int pros_usb_find_boot_hid_endpoint(const uint8_t *blob, uint32_t blob_length,
                                    struct pros_usb_hid_endpoint *out)
{
    if (!blob || !out || blob_length < 9u) return PROS_DRIVER_EINVAL;
    /* USB 2.0 configuration descriptors have a fixed nine-byte header. */
    if (blob[0] != 9u || blob[1] != PROS_USB_DESC_CONFIGURATION ||
        blob[4] == 0u || blob[5] == 0u ||
        (blob[7] & 0x80u) == 0u || (blob[7] & 0x1Fu) != 0u)
        return PROS_DRIVER_EIO;

    uint32_t total = (uint32_t)blob[2] | ((uint32_t)blob[3] << 8);
    if (total < 9u || total > blob_length) return PROS_DRIVER_EIO;

    uint8_t seen_interfaces[256] = {0};
    uint16_t distinct_interfaces = 0u;
    uint8_t current_interface = 0u, current_alt = 0u;
    uint8_t expected_endpoints = 0u, observed_endpoints = 0u;
    uint32_t seen_endpoint_addresses = 0u;
    uint8_t in_interface = 0u;
    enum pros_usb_hid_kind current_kind = PROS_USB_HID_NONE;
    struct pros_usb_hid_endpoint candidate = {0};
    uint8_t found = 0u;
    uint32_t offset = 0u;

    while (offset < total) {
        if (total - offset < 2u) return PROS_DRIVER_EIO;
        uint8_t length = blob[offset];
        uint8_t type = blob[offset + 1u];
        if (length < 2u || (uint32_t)length > total - offset)
            return PROS_DRIVER_EIO;

        if (offset == 0u) {
            if (type != PROS_USB_DESC_CONFIGURATION || length != 9u)
                return PROS_DRIVER_EIO;
        } else if (type == PROS_USB_DESC_INTERFACE) {
            if (length != 9u) return PROS_DRIVER_EIO;
            if (in_interface && observed_endpoints != expected_endpoints)
                return PROS_DRIVER_EIO;
            current_interface = blob[offset + 2u];
            current_alt = blob[offset + 3u];
            /* Interface number + alternate setting must be unique. Scan only
             * the already-validated prefix to avoid a large kernel stack
             * bitmap and avoid global mutable parser state. */
            for (uint32_t prior = 9u; prior < offset;) {
                uint8_t prior_length = blob[prior];
                uint8_t prior_type = blob[prior + 1u];
                if (prior_type == PROS_USB_DESC_INTERFACE &&
                    blob[prior + 2u] == current_interface &&
                    blob[prior + 3u] == current_alt)
                    return PROS_DRIVER_EIO;
                prior += prior_length;
            }
            if (!seen_interfaces[current_interface]) {
                seen_interfaces[current_interface] = 1u;
                ++distinct_interfaces;
            }
            expected_endpoints = blob[offset + 4u];
            observed_endpoints = 0u;
            seen_endpoint_addresses = 0u;
            in_interface = 1u;
            current_kind = PROS_USB_HID_NONE;
            if (blob[offset + 5u] == PROS_USB_CLASS_HID &&
                blob[offset + 6u] == PROS_USB_SUBCLASS_BOOT) {
                if (blob[offset + 7u] == PROS_USB_PROTOCOL_KEYBOARD)
                    current_kind = PROS_USB_HID_KEYBOARD;
                else if (blob[offset + 7u] == PROS_USB_PROTOCOL_MOUSE)
                    current_kind = PROS_USB_HID_MOUSE;
            }
        } else if (type == PROS_USB_DESC_ENDPOINT) {
            if (length != 7u || !in_interface) return PROS_DRIVER_EIO;
            if (observed_endpoints == UINT8_MAX) return PROS_DRIVER_EIO;
            ++observed_endpoints;

            uint8_t address = blob[offset + 2u];
            uint8_t attributes = blob[offset + 3u];
            uint16_t packet = (uint16_t)blob[offset + 4u] |
                              ((uint16_t)blob[offset + 5u] << 8);
            uint16_t packet_size = (uint16_t)(packet & 0x07FFu);
            uint8_t transfer_type =
                (uint8_t)(attributes & PROS_USB_ENDPOINT_TRANSFER_MASK);

            /* Validate every endpoint, including endpoints not selected for HID. */
            if ((address & 0x70u) != 0u || (address & 0x0Fu) == 0u ||
                (attributes & 0xC0u) != 0u || packet_size == 0u)
                return PROS_DRIVER_EIO;
            uint32_t endpoint_index = (uint32_t)(address & 0x0Fu);
            if ((address & PROS_USB_ENDPOINT_IN) != 0u) endpoint_index += 16u;
            uint32_t endpoint_bit = 1u << endpoint_index;
            if ((seen_endpoint_addresses & endpoint_bit) != 0u)
                return PROS_DRIVER_EIO;
            seen_endpoint_addresses |= endpoint_bit;

            if (transfer_type == PROS_USB_ENDPOINT_INTERRUPT) {
                if (blob[offset + 6u] == 0u || (packet & 0xF800u) != 0u)
                    return PROS_DRIVER_EIO;
            }

            if (current_kind != PROS_USB_HID_NONE &&
                (address & PROS_USB_ENDPOINT_IN) != 0u &&
                transfer_type == PROS_USB_ENDPOINT_INTERRUPT) {
                /* Boot reports are fixed-size: keyboard 8 bytes, mouse 3. */
                uint16_t minimum_report_size =
                    current_kind == PROS_USB_HID_KEYBOARD ? 8u : 3u;
                if (packet_size < minimum_report_size)
                    return PROS_DRIVER_EIO;
                candidate.interface_number = current_interface;
                candidate.alternate_setting = current_alt;
                candidate.endpoint_address = address;
                candidate.max_packet_size = packet_size;
                candidate.interval = blob[offset + 6u];
                candidate.kind = current_kind;
                found = 1u;
            }
        }
        offset += length;
    }

    if (offset != total) return PROS_DRIVER_EIO;
    if (in_interface && observed_endpoints != expected_endpoints)
        return PROS_DRIVER_EIO;
    if (distinct_interfaces != blob[4]) return PROS_DRIVER_EIO;
    if (!found) return PROS_DRIVER_ENODEV;
    *out = candidate;
    return PROS_DRIVER_OK;
}

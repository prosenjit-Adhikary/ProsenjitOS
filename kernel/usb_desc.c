#include "usb_desc.h"

#define USB_DESC_CONFIGURATION 2u
#define USB_DESC_INTERFACE 4u
#define USB_DESC_ENDPOINT 5u
#define USB_CLASS_MASS_STORAGE 0x08u
#define USB_SUBCLASS_SCSI 0x06u
#define USB_PROTOCOL_BULK_ONLY 0x50u
#define USB_ENDPOINT_DIRECTION_IN 0x80u
#define USB_ENDPOINT_NUMBER_MASK 0x0Fu
#define USB_ENDPOINT_TRANSFER_MASK 0x03u
#define USB_ENDPOINT_TRANSFER_BULK 0x02u

struct usb_desc_interface_state {
    uint8_t number;
    uint8_t alternate;
    uint8_t is_msc;
    uint8_t expected_endpoints;
    uint8_t actual_endpoints;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
    uint16_t packet_in;
    uint16_t packet_out;
};

static int usb_desc_bulk_packet_valid(uint16_t packet) {
    return packet == 8u || packet == 16u || packet == 32u ||
           packet == 64u || packet == 512u || packet == 1024u;
}

static uint16_t usb_desc_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void usb_desc_clear(struct pros_usb_msc_interface *out) {
    out->interface_number = 0u;
    out->alternate_setting = 0u;
    out->endpoint_in = 0u;
    out->endpoint_out = 0u;
    out->max_packet_in = 0u;
    out->max_packet_out = 0u;
    out->configuration_value = 0u;
}

static int usb_desc_finish_interface(
    const struct usb_desc_interface_state *state, uint8_t config_value,
    struct pros_usb_msc_interface *selected, uint32_t *matches) {
    if (state->actual_endpoints != state->expected_endpoints)
        return PROS_USB_DESC_EFORMAT;

    if (state->is_msc && state->alternate == 0u) {
        if (state->endpoint_in == 0u || state->endpoint_out == 0u)
            return PROS_USB_DESC_EFORMAT;
        ++*matches;
        if (*matches == 1u) {
            selected->interface_number = state->number;
            selected->alternate_setting = state->alternate;
            selected->endpoint_in = state->endpoint_in;
            selected->endpoint_out = state->endpoint_out;
            selected->max_packet_in = state->packet_in;
            selected->max_packet_out = state->packet_out;
            selected->configuration_value = config_value;
        }
    }
    return PROS_USB_DESC_OK;
}

int pros_usb_msc_select_interface(const uint8_t *descriptors,
                                  uint32_t descriptor_length,
                                  struct pros_usb_msc_interface *out) {
    uint8_t seen_interfaces[256];
    uint8_t config_value, declared_interfaces;
    uint16_t total_length;
    uint32_t offset, interface_count = 0u, matches = 0u;
    uint8_t have_interface = 0u;
    struct usb_desc_interface_state state = {0};
    struct pros_usb_msc_interface selected = {0};

    if (!descriptors || !out)
        return PROS_USB_DESC_EINVAL;
    usb_desc_clear(out);
    usb_desc_clear(&selected);
    for (uint32_t i = 0u; i < sizeof(seen_interfaces); ++i)
        seen_interfaces[i] = 0u;

    if (descriptor_length < 9u || descriptors[0] != 9u ||
        descriptors[1] != USB_DESC_CONFIGURATION ||
        (descriptors[7] & 0x80u) == 0u ||
        (descriptors[7] & 0x1Fu) != 0u)
        return PROS_USB_DESC_EFORMAT;

    total_length = usb_desc_le16(descriptors + 2u);
    declared_interfaces = descriptors[4];
    config_value = descriptors[5];
    if (total_length < 9u || total_length > descriptor_length ||
        declared_interfaces == 0u || config_value == 0u)
        return PROS_USB_DESC_EFORMAT;

    offset = 9u;
    while (offset < total_length) {
        uint8_t length, type;
        const uint8_t *d;

        if (total_length - offset < 2u)
            return PROS_USB_DESC_EFORMAT;
        d = descriptors + offset;
        length = d[0];
        type = d[1];
        if (length < 2u || length > total_length - offset)
            return PROS_USB_DESC_EFORMAT;

        if (type == USB_DESC_INTERFACE) {
            int rc;
            if (length != 9u)
                return PROS_USB_DESC_EFORMAT;
            if (have_interface) {
                rc = usb_desc_finish_interface(&state, config_value,
                                               &selected, &matches);
                if (rc != PROS_USB_DESC_OK)
                    return rc;
            }

            state.number = d[2];
            state.alternate = d[3];
            state.expected_endpoints = d[4];
            state.actual_endpoints = 0u;
            state.is_msc =
                (d[5] == USB_CLASS_MASS_STORAGE &&
                 d[6] == USB_SUBCLASS_SCSI &&
                 d[7] == USB_PROTOCOL_BULK_ONLY) ? 1u : 0u;
            state.endpoint_in = 0u;
            state.endpoint_out = 0u;
            state.packet_in = 0u;
            state.packet_out = 0u;
            have_interface = 1u;

            if (seen_interfaces[state.number] == 0u) {
                seen_interfaces[state.number] = 1u;
                ++interface_count;
            }
        } else if (type == USB_DESC_ENDPOINT) {
            uint8_t address, attributes, number;
            uint16_t raw_packet, max_packet;

            if (!have_interface || length != 7u ||
                state.actual_endpoints == 0xFFu)
                return PROS_USB_DESC_EFORMAT;
            ++state.actual_endpoints;

            address = d[2];
            attributes = d[3];
            number = (uint8_t)(address & USB_ENDPOINT_NUMBER_MASK);
            raw_packet = usb_desc_le16(d + 4u);
            max_packet = (uint16_t)(raw_packet & 0x07FFu);
            if (number == 0u || (address & 0x70u) != 0u)
                return PROS_USB_DESC_EFORMAT;

            if (state.is_msc && state.alternate == 0u &&
                (attributes & USB_ENDPOINT_TRANSFER_MASK) ==
                    USB_ENDPOINT_TRANSFER_BULK) {
                if ((attributes & 0xFCu) != 0u ||
                    (raw_packet & 0xF800u) != 0u ||
                    !usb_desc_bulk_packet_valid(max_packet))
                    return PROS_USB_DESC_EFORMAT;

                if ((address & USB_ENDPOINT_DIRECTION_IN) != 0u) {
                    if (state.endpoint_in != 0u)
                        return PROS_USB_DESC_EFORMAT;
                    state.endpoint_in = address;
                    state.packet_in = max_packet;
                } else {
                    if (state.endpoint_out != 0u)
                        return PROS_USB_DESC_EFORMAT;
                    state.endpoint_out = address;
                    state.packet_out = max_packet;
                }
            }
        }

        offset += length;
    }

    if (have_interface) {
        int rc = usb_desc_finish_interface(&state, config_value,
                                           &selected, &matches);
        if (rc != PROS_USB_DESC_OK)
            return rc;
    }

    if (offset != total_length || interface_count != declared_interfaces)
        return PROS_USB_DESC_EFORMAT;
    if (matches == 0u)
        return PROS_USB_DESC_ENOTFOUND;
    if (matches != 1u)
        return PROS_USB_DESC_EUNSUPPORTED;

    *out = selected;
    return PROS_USB_DESC_OK;
}

int pros_usb_desc_selftest(void) {
    uint8_t valid[] = {
        9u, 2u, 32u, 0u, 1u, 1u, 0u, 0x80u, 50u,
        9u, 4u, 3u, 0u, 2u, 0x08u, 0x06u, 0x50u, 0u,
        7u, 5u, 0x81u, 0x02u, 64u, 0u, 0u,
        7u, 5u, 0x02u, 0x02u, 64u, 0u, 0u
    };
    struct pros_usb_msc_interface selected;
    uint8_t malformed[sizeof(valid)];

    if (pros_usb_msc_select_interface(valid, sizeof(valid), &selected) !=
        PROS_USB_DESC_OK ||
        selected.interface_number != 3u ||
        selected.alternate_setting != 0u ||
        selected.endpoint_in != 0x81u ||
        selected.endpoint_out != 0x02u ||
        selected.max_packet_in != 64u ||
        selected.max_packet_out != 64u ||
        selected.configuration_value != 1u)
        return -1;

    if (pros_usb_msc_select_interface(valid, 8u, &selected) !=
        PROS_USB_DESC_EFORMAT)
        return -2;

    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[22] = 65u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_EFORMAT)
        return -9;

    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[22] = 0u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_EFORMAT)
        return -10;

    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[22] = 0u;
    malformed[23] = 2u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_OK ||
        selected.max_packet_in != 512u)
        return -11;

    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[22] = 0u;
    malformed[23] = 4u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_OK ||
        selected.max_packet_in != 1024u)
        return -12;

    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[20] = 0x01u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_EFORMAT)
        return -3;

    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[27] = 0x81u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_EFORMAT)
        return -4;

    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[0] = 8u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_EFORMAT)
        return -5;

    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[14] = 0x03u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_ENOTFOUND)
        return -6;

    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[4] = 2u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_EFORMAT)
        return -7;

    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[7] = 0x00u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_EFORMAT)
        return -13;
    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[7] = 0xA1u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_EFORMAT)
        return -14;
    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[9] = 8u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_EFORMAT)
        return -15;
    for (uint32_t i = 0u; i < sizeof(valid); ++i)
        malformed[i] = valid[i];
    malformed[18] = 8u;
    if (pros_usb_msc_select_interface(malformed, sizeof(malformed),
                                      &selected) != PROS_USB_DESC_EFORMAT)
        return -16;

    if (pros_usb_msc_select_interface(0, sizeof(valid), &selected) !=
        PROS_USB_DESC_EINVAL ||
        pros_usb_msc_select_interface(valid, sizeof(valid), 0) !=
        PROS_USB_DESC_EINVAL)
        return -8;

    return PROS_USB_DESC_OK;
}

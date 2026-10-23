#include "xhci.h"
#include "driver.h"

#define XHCI_CAP_HEADER_MIN 0x20u
#define XHCI_RUNTIME_BASE_SIZE 0x20u
#define XHCI_RUNTIME_INTERRUPTER_SIZE 0x20u

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int range_valid(uint32_t offset, uint32_t length, uint32_t limit)
{
    return offset <= limit && length <= limit - offset;
}

int pros_xhci_parse_capabilities(const uint8_t *bar, uint32_t bar_length,
                                 struct pros_xhci_capabilities *out)
{
    if (!bar || !out || bar_length < XHCI_CAP_HEADER_MIN)
        return PROS_DRIVER_EINVAL;

    struct pros_xhci_capabilities parsed = {0};
    parsed.capability_length = bar[0];
    parsed.interface_version = read_le16(&bar[2]);

    uint32_t structural = read_le32(&bar[4]);
    uint32_t capability = read_le32(&bar[0x10]);
    uint32_t doorbell = read_le32(&bar[0x14]);
    uint32_t runtime = read_le32(&bar[0x18]);

    if (parsed.capability_length < XHCI_CAP_HEADER_MIN ||
        (parsed.capability_length & 3u) != 0u ||
        parsed.capability_length > bar_length ||
        parsed.interface_version < 0x0100u)
        return PROS_DRIVER_EIO;

    parsed.max_slots = (uint8_t)(structural & 0xFFu);
    parsed.max_interrupters = (uint16_t)((structural >> 8) & 0x7FFu);
    parsed.max_ports = (uint8_t)(structural >> 24);
    parsed.context_size_64 = (uint8_t)((capability >> 2) & 1u);
    parsed.doorbell_offset = doorbell & ~3u;
    parsed.runtime_offset = runtime & ~0x1Fu;
    parsed.extended_capabilities_offset = (capability >> 16) * 4u;

    uint32_t doorbell_length = ((uint32_t)parsed.max_slots + 1u) * 4u;
    uint32_t runtime_length = XHCI_RUNTIME_BASE_SIZE +
                              (uint32_t)parsed.max_interrupters *
                                  XHCI_RUNTIME_INTERRUPTER_SIZE;
    if (parsed.max_slots == 0u || parsed.max_interrupters == 0u ||
        parsed.max_ports == 0u ||
        (doorbell & 3u) != 0u || (runtime & 0x1Fu) != 0u ||
        /* Operational registers occupy CAPLENGTH..CAPLENGTH+0x3F.
         * Doorbell/runtime windows must not alias that register block. */
        parsed.doorbell_offset < (uint32_t)parsed.capability_length + 0x40u ||
        parsed.runtime_offset < (uint32_t)parsed.capability_length + 0x40u ||
        /* Operational registers include DCBAAP and CONFIG, beyond the first
         * 0x20 bytes. Validate the whole minimum 0x40-byte register block. */
        !range_valid(parsed.capability_length, 0x40u, bar_length) ||
        !range_valid(parsed.doorbell_offset, doorbell_length, bar_length) ||
        !range_valid(parsed.runtime_offset, runtime_length, bar_length))
        return PROS_DRIVER_EIO;

    /* Doorbell and runtime register windows are separate xHCI regions. Reject
     * a malformed snapshot that aliases them, before any future MMIO access. */
    uint32_t doorbell_end = parsed.doorbell_offset + doorbell_length;
    uint32_t runtime_end = parsed.runtime_offset + runtime_length;
    if (parsed.doorbell_offset < runtime_end &&
        parsed.runtime_offset < doorbell_end)
        return PROS_DRIVER_EIO;

    /* xECP is a BAR-relative pointer and may lie beyond CAPLENGTH. */
    if (parsed.extended_capabilities_offset != 0u &&
        (parsed.extended_capabilities_offset < parsed.capability_length ||
         !range_valid(parsed.extended_capabilities_offset, 4u, bar_length)))
        return PROS_DRIVER_EIO;

    *out = parsed;
    return PROS_DRIVER_OK;
}

int pros_xhci_validate_extended_capabilities(const uint8_t *bar,
                                             uint32_t bar_length,
                                             uint32_t first_offset,
                                             uint32_t *count_out)
{
    if (!bar || !count_out)
        return PROS_DRIVER_EINVAL;
    if (bar_length < XHCI_CAP_HEADER_MIN)
        return PROS_DRIVER_EINVAL;
    if (bar[0] < XHCI_CAP_HEADER_MIN || (bar[0] & 3u) != 0u ||
        bar[0] > bar_length)
        return PROS_DRIVER_EIO;
    if (first_offset == 0u) {
        *count_out = 0u;
        return PROS_DRIVER_OK;
    }
    if ((first_offset & 3u) != 0u ||
        first_offset < bar[0] ||
        !range_valid(first_offset, 4u, bar_length))
        return PROS_DRIVER_EIO;

    uint32_t offset = first_offset;
    uint32_t count = 0u;
    uint32_t max_headers = bar_length / 4u;
    while (offset != 0u) {
        if (count >= max_headers || !range_valid(offset, 4u, bar_length))
            return PROS_DRIVER_EIO;
        uint32_t header = read_le32(&bar[offset]);
        uint32_t capability_id = header & 0xFFu;
        uint32_t next_dwords = (header >> 8) & 0xFFu;
        if (capability_id == 0u)
            return PROS_DRIVER_EIO;
        ++count;
        if (next_dwords == 0u)
            break;
        uint32_t delta = next_dwords * 4u;
        if (delta > UINT32_MAX - offset)
            return PROS_DRIVER_EIO;
        uint32_t next = offset + delta;
        if (next <= offset || !range_valid(next, 4u, bar_length))
            return PROS_DRIVER_EIO;
        offset = next;
    }
    *count_out = count;
    return PROS_DRIVER_OK;
}

/* Validate Supported Protocol capabilities (ID 2) and their one-based port
 * ranges against MaxPorts. Unknown capability types are skipped safely. */
int pros_xhci_validate_supported_protocols(const uint8_t *bar,
                                           uint32_t bar_length,
                                           uint32_t first_offset,
                                           uint8_t max_ports,
                                           uint32_t *count_out)
{
    if (!bar || !count_out || max_ports == 0u)
        return PROS_DRIVER_EINVAL;
    if (bar_length < XHCI_CAP_HEADER_MIN ||
        bar[0] < XHCI_CAP_HEADER_MIN || (bar[0] & 3u) != 0u ||
        bar[0] > bar_length)
        return PROS_DRIVER_EIO;
    if (first_offset == 0u) {
        *count_out = 0u;
        return PROS_DRIVER_OK;
    }
    if ((first_offset & 3u) != 0u ||
        first_offset < bar[0] ||
        !range_valid(first_offset, 4u, bar_length))
        return PROS_DRIVER_EIO;

    uint32_t offset = first_offset;
    uint32_t visited = 0u;
    uint32_t supported = 0u;
    /* A physical xHCI port belongs to one Supported Protocol range only.
     * Track port numbers in a compact bitmap so malformed overlapping ranges
     * fail closed without allocating memory or publishing a partial count. */
    uint8_t claimed_ports[32] = {0};
    uint32_t max_headers = bar_length / 4u;
    while (offset != 0u) {
        if (visited >= max_headers || !range_valid(offset, 4u, bar_length))
            return PROS_DRIVER_EIO;
        uint32_t header = read_le32(&bar[offset]);
        uint32_t id = header & 0xFFu;
        uint32_t next_dwords = (header >> 8) & 0xFFu;
        uint32_t minimum_next_offset = offset + 4u;
        if (id == 0u)
            return PROS_DRIVER_EIO;
        if (id == 2u) {
            if (!range_valid(offset, 16u, bar_length))
                return PROS_DRIVER_EIO;
            uint32_t ports = read_le32(&bar[offset + 8u]);
            uint32_t first_port = ports & 0xFFu;
            uint32_t port_count = (ports >> 8) & 0xFFu;
            uint32_t protocol_info = read_le32(&bar[offset + 12u]);
            uint32_t psi_count = (protocol_info >> 28) & 0x0Fu;
            /* Each PSI entry is one additional dword after the fixed 16-byte
             * Supported Protocol structure. Validate the entire structure,
             * not merely the fixed header, before a caller consumes it. */
            uint32_t structure_length = 16u + psi_count * 4u;
            if (!range_valid(offset, structure_length, bar_length) ||
                first_port == 0u || port_count == 0u ||
                first_port > max_ports ||
                port_count > (uint32_t)max_ports - first_port + 1u)
                return PROS_DRIVER_EIO;
            for (uint32_t port = first_port;
                 port < first_port + port_count; ++port) {
                uint32_t bit = port - 1u;
                uint8_t mask = (uint8_t)(1u << (bit & 7u));
                if ((claimed_ports[bit >> 3] & mask) != 0u)
                    return PROS_DRIVER_EIO;
                claimed_ports[bit >> 3] |= mask;
            }
            minimum_next_offset = offset + structure_length;
            ++supported;
        }
        ++visited;
        if (next_dwords == 0u)
            break;
        uint32_t delta = next_dwords * 4u;
        if (delta > UINT32_MAX - offset)
            return PROS_DRIVER_EIO;
        uint32_t next = offset + delta;
        if (next <= offset || next < minimum_next_offset ||
            !range_valid(next, 4u, bar_length))
            return PROS_DRIVER_EIO;
        offset = next;
    }
    *count_out = supported;
    return PROS_DRIVER_OK;
}

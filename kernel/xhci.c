#include "xhci.h"
#include "driver.h"

#define XHCI_CAP_HEADER_MIN 0x20u
#define XHCI_RUNTIME_MIN_SIZE 0x20u

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

    /* Do not publish partial data on malformed or truncated BAR snapshots. */
    struct pros_xhci_capabilities parsed = {0};
    parsed.capability_length = bar[0];
    parsed.interface_version = read_le16(&bar[2]);

    uint32_t structural = read_le32(&bar[4]);    /* HCSPARAMS1 */
    uint32_t capability = read_le32(&bar[0x10]); /* HCCPARAMS1 */
    uint32_t doorbell = read_le32(&bar[0x14]);   /* DBOFF */
    uint32_t runtime = read_le32(&bar[0x18]);    /* RTSOFF */

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
    uint32_t xecp_dwords = capability >> 16;
    parsed.extended_capabilities_offset = xecp_dwords * 4u;

    if (parsed.max_slots == 0u || parsed.max_interrupters == 0u ||
        parsed.max_ports == 0u ||
        (doorbell & 3u) != 0u || (runtime & 0x1Fu) != 0u ||
        parsed.doorbell_offset < parsed.capability_length ||
        parsed.runtime_offset < parsed.capability_length ||
        !range_valid(parsed.doorbell_offset,
                     ((uint32_t)parsed.max_slots + 1u) * 4u, bar_length) ||
        !range_valid(parsed.runtime_offset, XHCI_RUNTIME_MIN_SIZE, bar_length))
        return PROS_DRIVER_EIO;

    if (parsed.extended_capabilities_offset != 0u &&
        ((parsed.extended_capabilities_offset & 3u) != 0u ||
         parsed.extended_capabilities_offset < parsed.capability_length ||
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
    if (first_offset == 0u) {
        *count_out = 0u;
        return PROS_DRIVER_OK;
    }
    if ((first_offset & 3u) != 0u || first_offset < XHCI_CAP_HEADER_MIN ||
        !range_valid(first_offset, 4u, bar_length))
        return PROS_DRIVER_EIO;

    uint32_t offset = first_offset;
    uint32_t count = 0u;
    /* Each nonzero Next pointer is a positive DWORD delta, so valid traversal
     * advances monotonically. Bound by the BAR's maximum possible header count
     * as a defensive guard against malformed snapshots. */
    uint32_t max_headers = bar_length / 4u;
    while (offset != 0u) {
        if (count >= max_headers || !range_valid(offset, 4u, bar_length))
            return PROS_DRIVER_EIO;
        uint32_t header = read_le32(&bar[offset]);
        uint32_t capability_id = header & 0xFFu;
        uint32_t next_dwords = (header >> 8) & 0xFFu;
        /* Capability ID zero is reserved; do not treat zero-filled BAR space
         * as a valid extended-capability header. */
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

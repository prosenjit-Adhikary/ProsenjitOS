#include <assert.h>
#include <stdint.h>
#include "driver.h"
#include "xhci.h"

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void make_bar(uint8_t *bar, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) bar[i] = 0u;
    bar[0] = 0x40u; /* operational registers begin at BAR + 0x40 */
    bar[2] = 0x00u; bar[3] = 0x01u;
    put32(&bar[4], 8u | (4u << 8) | (6u << 24));
    put32(&bar[0x10], (0x30u << 16) | (1u << 2)); /* xECP at 0xC0 */
    put32(&bar[0x14], 0x100u);
    put32(&bar[0x18], 0x140u);
}

static int caps_equal(const struct pros_xhci_capabilities *a,
                      const struct pros_xhci_capabilities *b)
{
    return a->interface_version == b->interface_version &&
           a->capability_length == b->capability_length &&
           a->max_slots == b->max_slots &&
           a->max_interrupters == b->max_interrupters &&
           a->max_ports == b->max_ports &&
           a->context_size_64 == b->context_size_64 &&
           a->doorbell_offset == b->doorbell_offset &&
           a->runtime_offset == b->runtime_offset &&
           a->extended_capabilities_offset == b->extended_capabilities_offset;
}

int main(void)
{
    uint8_t bar[512];
    struct pros_xhci_capabilities caps = {0};
    make_bar(bar, sizeof(bar));
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_OK);
    assert(caps.capability_length == 0x40u);
    assert(caps.interface_version == 0x0100u);
    assert(caps.max_slots == 8u && caps.max_interrupters == 4u && caps.max_ports == 6u);
    assert(caps.context_size_64 == 1u);
    assert(caps.doorbell_offset == 0x100u && caps.runtime_offset == 0x140u);
    assert(caps.extended_capabilities_offset == 0xC0u);

    uint32_t ext_count = 99u;
    uint32_t protocol_count = 99u;
    put32(&bar[0xC0], 0x00000201u);
    put32(&bar[0xC8], 0x00000002u);
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), caps.extended_capabilities_offset,
               &ext_count) == PROS_DRIVER_OK);
    assert(ext_count == 2u);
    assert(pros_xhci_validate_extended_capabilities(bar, sizeof(bar), 0u,
                                                     &ext_count) == PROS_DRIVER_OK);
    assert(ext_count == 0u);
    /* Extended capabilities may not begin inside the CAPLENGTH register bank. */
    ext_count = 76u;
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), 0x20u, &ext_count) == PROS_DRIVER_EIO);
    assert(ext_count == 76u);
    ext_count = 77u;
    put32(&bar[0xC0], 0x0000FF01u);
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), 0xC0u, &ext_count) == PROS_DRIVER_EIO);
    assert(ext_count == 77u);
    put32(&bar[0xC0], 0x00000000u);
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), 0xC0u, &ext_count) == PROS_DRIVER_EIO);
    assert(ext_count == 77u);
    put32(&bar[0xC0], 0x00000201u);
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), 0xC1u, &ext_count) == PROS_DRIVER_EIO);
    assert(pros_xhci_validate_extended_capabilities(
               0, sizeof(bar), 0u, &ext_count) == PROS_DRIVER_EINVAL);


    /* Linked-list walkers must not parse doorbell/runtime registers as
       extended-capability headers, even if the BAR snapshot is in bounds. */
    make_bar(bar, sizeof(bar));
    ext_count = 63u;
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), 0x100u, &ext_count) == PROS_DRIVER_EIO);
    assert(ext_count == 63u);
    protocol_count = 64u;
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0x140u, 6u, &protocol_count) == PROS_DRIVER_EIO);
    assert(protocol_count == 64u);

    /* Supported Protocol capability: USB 2 protocol, ports 1..4. */
    make_bar(bar, sizeof(bar));
    put32(&bar[0xC0], 0x00000002u);
    put32(&bar[0xC8], 1u | (4u << 8));
    protocol_count = 75u;
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0x20u, 6u, &protocol_count) == PROS_DRIVER_EIO);
    assert(protocol_count == 75u);
    protocol_count = 99u;
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0xC0u, 6u, &protocol_count) == PROS_DRIVER_OK);
    assert(protocol_count == 1u);
    protocol_count = 99u;
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0xC0u, 3u, &protocol_count) == PROS_DRIVER_EIO);
    assert(protocol_count == 99u);
    put32(&bar[0xC0], 0x00000000u);
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0xC0u, 6u, &protocol_count) == PROS_DRIVER_EIO);
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0u, 6u, &protocol_count) == PROS_DRIVER_OK);
    assert(protocol_count == 0u);

    /* A Supported Protocol header at the end of the snapshot must not read
       its remaining three dwords beyond the supplied BAR bounds. */
    put32(&bar[508], 0x00000002u);
    protocol_count = 55u;
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 508u, 6u, &protocol_count) == PROS_DRIVER_EIO);
    assert(protocol_count == 55u);

    /* Unknown capability IDs are skipped via the linked-list next pointer. */
    make_bar(bar, sizeof(bar));
    put32(&bar[0xC0], 0x00000403u); /* ID 3, next capability at +16 */
    put32(&bar[0xD0], 0x00000002u); /* Supported Protocol */
    put32(&bar[0xD8], 2u | (2u << 8)); /* ports 2..3 */
    protocol_count = 0u;
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0xC0u, 6u, &protocol_count) == PROS_DRIVER_OK);
    assert(protocol_count == 1u);

    /* Distinct Supported Protocol records may not claim the same port.
       The failure must not publish a partial protocol count. */
    make_bar(bar, sizeof(bar));
    put32(&bar[0xC0], 0x00000802u); /* next record at +32 bytes */
    put32(&bar[0xC8], 1u | (2u << 8)); /* ports 1..2 */
    put32(&bar[0xE0], 0x00000002u);
    put32(&bar[0xE8], 2u | (2u << 8)); /* ports 2..3 overlaps port 2 */
    protocol_count = 91u;
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0xC0u, 6u, &protocol_count) == PROS_DRIVER_EIO);
    assert(protocol_count == 91u);

    /* Adjacent, non-overlapping ranges remain valid. */
    put32(&bar[0xE8], 3u | (2u << 8)); /* ports 3..4 */
    protocol_count = 0u;
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0xC0u, 6u, &protocol_count) == PROS_DRIVER_OK);
    assert(protocol_count == 2u);

    /* PSI count expands the capability beyond its fixed 16-byte structure. */
    make_bar(bar, sizeof(bar));
    put32(&bar[0xC0], 0x00000002u);
    put32(&bar[0xC8], 1u | (2u << 8));
    put32(&bar[0xCC], 3u << 28); /* three PSI dwords */
    protocol_count = 44u;
    assert(pros_xhci_validate_supported_protocols(
               bar, 0xD0u, 0xC0u, 6u, &protocol_count) == PROS_DRIVER_EIO);
    assert(protocol_count == 44u);
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0xC0u, 6u, &protocol_count) == PROS_DRIVER_OK);
    assert(protocol_count == 1u);

    /* A next pointer into the fixed 16-byte body is invalid even without PSI. */
    make_bar(bar, sizeof(bar));
    put32(&bar[0xC0], 0x00000302u); /* next capability at +12 bytes */
    protocol_count = 87u;
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0xC0u, 6u, &protocol_count) == PROS_DRIVER_EIO);
    assert(protocol_count == 87u);

    /* Maximum PSIC is rejected if the snapshot truncates its PSI array. */
    make_bar(bar, sizeof(bar));
    put32(&bar[0xC0], 0x00000002u);
    put32(&bar[0xC8], 1u | (2u << 8));
    put32(&bar[0xCC], 15u << 28);
    protocol_count = 86u;
    assert(pros_xhci_validate_supported_protocols(
               bar, 0x100u, 0xC0u, 6u, &protocol_count) == PROS_DRIVER_EIO);
    assert(protocol_count == 86u);

    /* A next pointer may not land inside the current capability's PSI array. */
    make_bar(bar, sizeof(bar));
    put32(&bar[0xC0], 0x00000102u); /* next pointer at +4 bytes */
    put32(&bar[0xC8], 1u | (2u << 8));
    put32(&bar[0xCC], 2u << 28);
    protocol_count = 88u;
    assert(pros_xhci_validate_supported_protocols(
               bar, sizeof(bar), 0xC0u, 6u, &protocol_count) == PROS_DRIVER_EIO);
    assert(protocol_count == 88u);

    assert(pros_xhci_parse_capabilities(0, sizeof(bar), &caps) == PROS_DRIVER_EINVAL);
    assert(pros_xhci_parse_capabilities(bar, 0x1Fu, &caps) == PROS_DRIVER_EINVAL);
    struct pros_xhci_capabilities saved = caps;
    bar[0] = 0x1Cu;
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    assert(caps_equal(&caps, &saved));
    make_bar(bar, sizeof(bar));
    bar[0] = 0x41u;
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    bar[2] = 0u; bar[3] = 0u;
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[4], 0u);
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x14], 0x140u); /* aliases the runtime-register window */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x14], 0x1FCu);
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x18], 0x1E0u);
    assert(pros_xhci_parse_capabilities(bar, 0x1F0u, &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x18], 0x180u); /* base runtime area fits, interrupter sets do not */
    assert(pros_xhci_parse_capabilities(bar, 0x200u, &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x10], (0x0Fu << 16) | (1u << 2)); /* xECP overlaps capability registers */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x10], (0x7Fu << 16) | (1u << 2));
    assert(pros_xhci_parse_capabilities(bar, 0x1F0u, &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x14], 0x102u);
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);

    /* The first xECP header may not alias the doorbell or runtime windows. */
    make_bar(bar, sizeof(bar));
    put32(&bar[0x10], (0x40u << 16) | (1u << 2)); /* xECP at 0x100 */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x10], (0x50u << 16) | (1u << 2)); /* xECP at 0x140 */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);

    /* Neither doorbell nor runtime windows may alias operational registers. */
    make_bar(bar, sizeof(bar));
    put32(&bar[0x14], 0x60u);
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x18], 0x60u);
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);

    /* The operational register block must include DCBAAP and CONFIG. Here
       doorbell/runtime windows fit, but CAPLENGTH + 0x40 exceeds the BAR. */
    make_bar(bar, sizeof(bar));
    bar[0] = 0xC0u;
    put32(&bar[4], 8u | (1u << 8) | (6u << 24));
    put32(&bar[0x14], 0xD0u);
    put32(&bar[0x18], 0x80u);
    assert(pros_xhci_parse_capabilities(bar, 0xFFu, &caps) == PROS_DRIVER_EIO);
    return 0;
}

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
    assert(caps.extended_capabilities_offset == 0xC0u); /* may exceed CAPLENGTH */

    uint32_t ext_count = 99u;
    put32(&bar[0xC0], 0x00000201u); /* capability ID 1; next is +2 DWORDs */
    put32(&bar[0xC8], 0x00000002u); /* capability ID 2; end of chain */
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), caps.extended_capabilities_offset,
               &ext_count) == PROS_DRIVER_OK);
    assert(ext_count == 2u);
    assert(pros_xhci_validate_extended_capabilities(bar, sizeof(bar), 0u,
                                                     &ext_count) == PROS_DRIVER_OK);
    assert(ext_count == 0u);
    ext_count = 77u;
    put32(&bar[0xC0], 0x0000FF01u); /* next pointer escapes BAR */
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), 0xC0u, &ext_count) == PROS_DRIVER_EIO);
    assert(ext_count == 77u);
    put32(&bar[0xC0], 0x00000000u); /* reserved capability ID zero */
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), 0xC0u, &ext_count) == PROS_DRIVER_EIO);
    assert(ext_count == 77u);
    /* A valid chain may continue beyond CAPLENGTH but must stay inside BAR. */
    put32(&bar[0xC0], 0x00000301u);
    put32(&bar[0xCC], 0x00000102u);
    put32(&bar[0xD0], 0x00000002u);
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), 0xC0u, &ext_count) == PROS_DRIVER_OK);
    assert(ext_count == 3u);
    put32(&bar[0xC0], 0x00000201u);
    assert(pros_xhci_validate_extended_capabilities(
               bar, sizeof(bar), 0xC1u, &ext_count) == PROS_DRIVER_EIO);
    assert(pros_xhci_validate_extended_capabilities(
               0, sizeof(bar), 0u, &ext_count) == PROS_DRIVER_EINVAL);

    assert(pros_xhci_parse_capabilities(0, sizeof(bar), &caps) == PROS_DRIVER_EINVAL);
    assert(pros_xhci_parse_capabilities(bar, 0x1Fu, &caps) == PROS_DRIVER_EINVAL);
    struct pros_xhci_capabilities saved = caps;
    bar[0] = 0x1Cu;
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    assert(caps_equal(&caps, &saved));
    make_bar(bar, sizeof(bar));
    bar[0] = 0x21u;
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    bar[2] = 0u; bar[3] = 0u;
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[4], 0u);
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x14], 0x1FCu);
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x18], 0x1E0u);
    assert(pros_xhci_parse_capabilities(bar, 0x1F0u, &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x10], (0x80u << 16) | (1u << 2)); /* xECP outside BAR */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x14], 0x102u);
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    return 0;
}

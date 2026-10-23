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
    bar[0] = 0x20u; /* CAPLENGTH */
    bar[2] = 0x00u; bar[3] = 0x01u; /* xHCI 1.0 */
    put32(&bar[4], 8u | (4u << 8) | (6u << 24)); /* slots, interrupters, ports */
    put32(&bar[0x10], (0x30u << 16) | (1u << 2)); /* xECP at 0xC0, 64-bit context */
    put32(&bar[0x14], 0x40u); /* doorbell array */
    put32(&bar[0x18], 0x80u); /* runtime registers */
}

int main(void)
{
    uint8_t bar[256];
    struct pros_xhci_capabilities caps = {0};
    make_bar(bar, sizeof(bar));
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_OK);
    assert(caps.capability_length == 0x20u);
    assert(caps.interface_version == 0x0100u);
    assert(caps.max_slots == 8u && caps.max_interrupters == 4u && caps.max_ports == 6u);
    assert(caps.context_size_64 == 1u);
    assert(caps.doorbell_offset == 0x40u && caps.runtime_offset == 0x80u);
    assert(caps.extended_capabilities_offset == 0xC0u);

    assert(pros_xhci_parse_capabilities(0, sizeof(bar), &caps) == PROS_DRIVER_EINVAL);
    assert(pros_xhci_parse_capabilities(bar, 0x1Fu, &caps) == PROS_DRIVER_EINVAL);
    struct pros_xhci_capabilities saved = caps;
    bar[0] = 0x1Cu; /* capability header too short */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    assert(caps.capability_length == saved.capability_length); /* no partial output */
    make_bar(bar, sizeof(bar));
    bar[0] = 0x21u; /* not dword aligned */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    bar[2] = 0u; bar[3] = 0u; /* unsupported interface version */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[4], 0u); /* zero slots/interrupters/ports */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x14], 0xFCu); /* doorbell array extends beyond 256-byte BAR */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x18], 0xE0u); /* runtime minimum range does not fit */
    assert(pros_xhci_parse_capabilities(bar, 0xF0u, &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x10], (0x7Fu << 16) | (1u << 2)); /* xECP at 0x1FC, truncated */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    make_bar(bar, sizeof(bar));
    put32(&bar[0x14], 0x42u); /* reserved low offset bits */
    assert(pros_xhci_parse_capabilities(bar, sizeof(bar), &caps) == PROS_DRIVER_EIO);
    return 0;
}

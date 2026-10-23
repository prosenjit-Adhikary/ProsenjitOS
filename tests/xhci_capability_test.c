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
           a.max_interrupters == b->max_interrupters;
}

#include "xhci.h"

#define XHCI_CAP_LENGTH 0x00u
#define XHCI_HCSPARAMS1 0x04u
#define XHCI_HCCPARAMS1 0x10u
#define XHCI_DBOFF 0x14u
#define XHCI_RTSOFF 0x18u
#define XHCI_MIN_CAP_SIZE 0x20u
#define XHCI_MIN_RUNTIME_SIZE 0x20u

static uint32_t xhci_read32(const volatile uint8_t *base, uint32_t offset) {
    const volatile uint32_t *reg =
        (const volatile uint32_t *)(const volatile void *)(base + offset);
    return *reg;
}

static int xhci_range_valid(uint64_t offset, uint64_t length,
                            uint64_t total) {
    return offset <= total && length <= total - offset;
}

int pros_xhci_capabilities_parse(const volatile void *mmio,
                                 uint64_t mmio_size,
                                 struct pros_xhci_capabilities *out) {
    const volatile uint8_t *base = (const volatile uint8_t *)mmio;
    struct pros_xhci_capabilities value;
    uint32_t hcs1, hcc1, dboff, rtsoff;
    uint32_t cap_length;

    if (!mmio || !out || (((uintptr_t)mmio) & 3u) != 0u)
        return PROS_XHCI_EINVAL;

    out->interface_version = 0u;
    out->capability_length = 0u;
    out->max_device_slots = 0u;
    out->max_interrupters = 0u;
    out->max_ports = 0u;
    out->context_size = 0u;
    out->address_64bit = 0u;
    out->doorbell_offset = 0u;
    out->runtime_offset = 0u;
    out->extended_capability_offset = 0u;

    if (mmio_size < XHCI_MIN_CAP_SIZE)
        return PROS_XHCI_EBOUNDS;

    cap_length = base[XHCI_CAP_LENGTH];
    if (cap_length < XHCI_MIN_CAP_SIZE || (cap_length & 3u) != 0u ||
        !xhci_range_valid(0u, cap_length, mmio_size))
        return PROS_XHCI_EBOUNDS;

    hcs1 = xhci_read32(base, XHCI_HCSPARAMS1);
    hcc1 = xhci_read32(base, XHCI_HCCPARAMS1);
    dboff = xhci_read32(base, XHCI_DBOFF) & ~3u;
    rtsoff = xhci_read32(base, XHCI_RTSOFF) & ~0x1Fu;

    if (dboff < cap_length || rtsoff < cap_length ||
        !xhci_range_valid(dboff, sizeof(uint32_t), mmio_size) ||
        !xhci_range_valid(rtsoff, XHCI_MIN_RUNTIME_SIZE, mmio_size))
        return PROS_XHCI_EBOUNDS;

    value.interface_version =
        (uint16_t)(((uint16_t)base[3] << 8) | base[2]);
    value.capability_length = (uint8_t)cap_length;
    value.max_device_slots = (uint8_t)(hcs1 & 0xFFu);
    value.max_interrupters = (uint16_t)((hcs1 >> 8) & 0x7FFu);
    value.max_ports = (uint8_t)((hcs1 >> 24) & 0xFFu);
    value.context_size = (hcc1 & (1u << 2)) ? 64u : 32u;
    value.address_64bit = (hcc1 & 1u) ? 1u : 0u;
    value.doorbell_offset = dboff;
    value.runtime_offset = rtsoff;
    value.extended_capability_offset =
        ((hcc1 >> 16) & 0xFFFFu) * 4u;
    if (value.extended_capability_offset != 0u &&
        (value.extended_capability_offset < cap_length ||
         !xhci_range_valid(value.extended_capability_offset,
                           sizeof(uint32_t), mmio_size)))
        return PROS_XHCI_EBOUNDS;

    if (value.interface_version == 0u ||
        value.max_device_slots == 0u ||
        value.max_interrupters == 0u ||
        value.max_ports == 0u)
        return PROS_XHCI_EUNSUPPORTED;

    *out = value;
    return PROS_XHCI_OK;
}

static void xhci_test_put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

int pros_xhci_selftest(void) {
    uint8_t registers[0x200];
    struct pros_xhci_capabilities caps;

    for (uint32_t i = 0u; i < sizeof(registers); ++i)
        registers[i] = 0u;

    registers[0] = 0x40u;
    registers[2] = 0x10u;
    registers[3] = 0x01u;
    xhci_test_put32(registers + XHCI_HCSPARAMS1,
                    8u | (4u << 8) | (6u << 24));
    xhci_test_put32(registers + XHCI_HCCPARAMS1,
                    1u | (1u << 2) | (0x20u << 16));
    xhci_test_put32(registers + XHCI_DBOFF, 0x100u);
    xhci_test_put32(registers + XHCI_RTSOFF, 0x120u);

    if (pros_xhci_capabilities_parse(registers, sizeof(registers),
                                     &caps) != PROS_XHCI_OK ||
        caps.interface_version != 0x0110u ||
        caps.capability_length != 0x40u ||
        caps.max_device_slots != 8u ||
        caps.max_interrupters != 4u || caps.max_ports != 6u ||
        caps.context_size != 64u || !caps.address_64bit ||
        caps.doorbell_offset != 0x100u ||
        caps.runtime_offset != 0x120u ||
        caps.extended_capability_offset != 0x80u)
        return -1;

    if (pros_xhci_capabilities_parse(registers, 0x1Fu, &caps) !=
        PROS_XHCI_EBOUNDS)
        return -2;

    registers[0] = 0x21u;
    if (pros_xhci_capabilities_parse(registers, sizeof(registers), &caps) !=
        PROS_XHCI_EBOUNDS)
        return -3;

    registers[0] = 0x40u;
    xhci_test_put32(registers + XHCI_DBOFF, 0x200u);
    if (pros_xhci_capabilities_parse(registers, sizeof(registers), &caps) !=
        PROS_XHCI_EBOUNDS)
        return -4;

    xhci_test_put32(registers + XHCI_DBOFF, 0x100u);
    xhci_test_put32(registers + XHCI_RTSOFF, 0x200u);
    if (pros_xhci_capabilities_parse(registers, sizeof(registers), &caps) !=
        PROS_XHCI_EBOUNDS)
        return -5;

    xhci_test_put32(registers + XHCI_RTSOFF, 0x120u);
    xhci_test_put32(registers + XHCI_HCSPARAMS1, 0u);
    if (pros_xhci_capabilities_parse(registers, sizeof(registers), &caps) !=
        PROS_XHCI_EUNSUPPORTED)
        return -6;

    if (pros_xhci_capabilities_parse(0, sizeof(registers), &caps) !=
        PROS_XHCI_EINVAL ||
        pros_xhci_capabilities_parse(registers, sizeof(registers), 0) !=
        PROS_XHCI_EINVAL)
        return -7;

    return PROS_XHCI_OK;
}

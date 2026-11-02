#include "usb_xhci.h"

#define XHCI_CAPABILITY_MIN_LENGTH 0x20u
#define XHCI_OPERATIONAL_MIN_LENGTH 0x40u
#define XHCI_MAX_SUPPORTED_VERSION 0x0120u

static uint32_t xhci_read32(const volatile uint8_t *base, uint32_t offset) {
    const volatile uint32_t *reg =
        (const volatile uint32_t *)(const volatile void *)(base + offset);
    return *reg;
}

static int xhci_range_valid(uint64_t offset, uint64_t length,
                            uint64_t total) {
    return offset <= total && length <= total - offset;
}

int pros_xhci_parse_capabilities(const volatile void *mmio,
                                 uint64_t bar_size,
                                 struct pros_xhci_capabilities *capabilities) {
    const volatile uint8_t *base = (const volatile uint8_t *)mmio;
    uint32_t cap0, hcs1, hcc1, dboff, rtsoff;
    uint32_t cap_length, version, operational, doorbell, runtime, ext_caps;
    uint32_t slots, interrupters, ports;

    if (!mmio || !capabilities ||
        (((uintptr_t)mmio) & (sizeof(uint32_t) - 1u)) != 0u)
        return PROS_USB_XHCI_EINVAL;
    capabilities->version = 0u;
    capabilities->capability_length = 0u;
    capabilities->max_slots = 0u;
    capabilities->max_interrupters = 0u;
    capabilities->max_ports = 0u;
    capabilities->operational_offset = 0u;
    capabilities->doorbell_offset = 0u;
    capabilities->runtime_offset = 0u;
    capabilities->extended_capabilities_offset = 0u;

    if (bar_size < XHCI_CAPABILITY_MIN_LENGTH)
        return PROS_USB_XHCI_EBOUNDS;

    cap0 = xhci_read32(base, 0u);
    cap_length = cap0 & 0xFFu;
    version = cap0 >> 16;
    if (cap_length < XHCI_CAPABILITY_MIN_LENGTH ||
        (cap_length & 3u) != 0u ||
        !xhci_range_valid(cap_length, XHCI_OPERATIONAL_MIN_LENGTH, bar_size))
        return PROS_USB_XHCI_EBOUNDS;
    if (version < 0x0090u || version > XHCI_MAX_SUPPORTED_VERSION)
        return PROS_USB_XHCI_EUNSUPPORTED;

    hcs1 = xhci_read32(base, 4u);
    hcc1 = xhci_read32(base, 0x10u);
    dboff = xhci_read32(base, 0x14u);
    rtsoff = xhci_read32(base, 0x18u);

    slots = hcs1 & 0xFFu;
    interrupters = (hcs1 >> 8) & 0x7FFu;
    ports = (hcs1 >> 24) & 0xFFu;
    operational = cap_length;
    doorbell = dboff & ~0x3u;
    runtime = rtsoff & ~0x1Fu;
    ext_caps = ((hcc1 >> 16) & 0xFFFFu) * 4u;

    if (slots == 0u || interrupters == 0u || ports == 0u)
        return PROS_USB_XHCI_EUNSUPPORTED;
    if (!xhci_range_valid(operational, XHCI_OPERATIONAL_MIN_LENGTH, bar_size) ||
        !xhci_range_valid(doorbell, 4u, bar_size) ||
        !xhci_range_valid(runtime, 0x20u, bar_size))
        return PROS_USB_XHCI_EBOUNDS;
    if (ext_caps != 0u &&
        (ext_caps < XHCI_CAPABILITY_MIN_LENGTH ||
         (ext_caps & 3u) != 0u ||
         !xhci_range_valid(ext_caps, 4u, bar_size)))
        return PROS_USB_XHCI_EBOUNDS;

    capabilities->version = (uint16_t)version;
    capabilities->capability_length = (uint8_t)cap_length;
    capabilities->max_slots = (uint8_t)slots;
    capabilities->max_interrupters = (uint16_t)interrupters;
    capabilities->max_ports = (uint8_t)ports;
    capabilities->operational_offset = operational;
    capabilities->doorbell_offset = doorbell;
    capabilities->runtime_offset = runtime;
    capabilities->extended_capabilities_offset = ext_caps;
    return PROS_USB_XHCI_OK;
}

int pros_xhci_probe(const struct pros_pci_device *device,
                    struct pros_xhci_controller *controller) {
    uint8_t controller_type = 0u;
    struct pros_pci_bar bar;
    int rc;

    if (!device || !controller)
        return PROS_USB_XHCI_EINVAL;
    controller->initialized = 0u;
    controller->bar.physical_address = 0u;
    controller->bar.virtual_address = 0u;
    controller->bar.size = 0u;
    rc = pci_usb_controller_type(device, &controller_type);
    if (rc != PROS_PCI_OK || controller_type != PROS_PCI_USB_XHCI)
        return PROS_USB_XHCI_EUNSUPPORTED;

    rc = pci_get_bar(device, 0u, &bar);
    if (rc != PROS_PCI_OK || bar.is_io || bar.base == 0u ||
        bar.size < 0x20u || bar.size > (16u * 1024u * 1024u) ||
        bar.base > UINT64_MAX - bar.size)
        return PROS_USB_XHCI_EBOUNDS;

    rc = pci_map_mmio_bar(device, 0u, &controller->bar);
    if (rc != PROS_PCI_OK)
        return PROS_USB_XHCI_EIO;
    rc = pros_xhci_parse_capabilities(
        (const volatile void *)(uintptr_t)controller->bar.virtual_address,
        controller->bar.size, &controller->capabilities);
    if (rc != PROS_USB_XHCI_OK) {
        pci_unmap_mmio_bar(&controller->bar);
        return rc;
    }

    controller->pci = *device;
    controller->initialized = 1u;
    /* Deliberately do not set PCI bus-master, USBCMD.RUN, or DMA ring pointers. */
    return PROS_USB_XHCI_OK;
}

/*
 * xHCI extended capability headers encode the next pointer as a positive
 * dword offset from the current capability. The walk is bounded by BAR size.
 */
int pros_xhci_find_legacy_support(const volatile void *mmio,
                                  uint64_t bar_size,
                                  uint32_t first_offset,
                                  uint32_t *legacy_offset) {
    const volatile uint8_t *base = (const volatile uint8_t *)mmio;
    uint64_t offset = first_offset;
    uint64_t max_steps;
    if (!mmio || !legacy_offset ||
        (((uintptr_t)mmio) & (sizeof(uint32_t) - 1u)) != 0u)
        return PROS_USB_XHCI_EINVAL;
    *legacy_offset = 0u;
    if (first_offset == 0u)
        return PROS_USB_XHCI_ENODEV;
    if (first_offset < XHCI_CAPABILITY_MIN_LENGTH ||
        (first_offset & 3u) != 0u ||
        !xhci_range_valid(first_offset, sizeof(uint32_t), bar_size))
        return PROS_USB_XHCI_EBOUNDS;

    max_steps = bar_size / sizeof(uint32_t);
    for (uint64_t step = 0u; step < max_steps; ++step) {
        uint32_t header = xhci_read32(base, (uint32_t)offset);
        uint32_t capability_id = header & 0xFFu;
        uint32_t next_dwords = (header >> 8) & 0xFFu;
        if (capability_id == 1u) {
            *legacy_offset = (uint32_t)offset;
            return PROS_USB_XHCI_OK;
        }
        if (next_dwords == 0u)
            return PROS_USB_XHCI_ENODEV;
        offset += (uint64_t)next_dwords * sizeof(uint32_t);
        if (offset > UINT32_MAX ||
            !xhci_range_valid(offset, sizeof(uint32_t), bar_size))
            return PROS_USB_XHCI_EBOUNDS;
    }
    return PROS_USB_XHCI_EBOUNDS;
}

/*
 * Request the BIOS/OS ownership handoff through the xHCI Legacy Support
 * capability. max_polls is an explicit finite polling budget; this routine
 * never clears the BIOS-owned bit and never waits without a hard bound.
 */
int pros_xhci_legacy_handoff(volatile void *mmio, uint64_t bar_size,
                             uint32_t capability_offset, uint32_t max_polls) {
    volatile uint8_t *base = (volatile uint8_t *)mmio;
    volatile uint8_t *bios_owned;
    volatile uint8_t *os_owned;
    uint32_t header;

    if (!mmio || max_polls == 0u ||
        (((uintptr_t)mmio) & (sizeof(uint32_t) - 1u)) != 0u ||
        (capability_offset & 3u) != 0u)
        return PROS_USB_XHCI_EINVAL;
    if (capability_offset < XHCI_CAPABILITY_MIN_LENGTH ||
        !xhci_range_valid(capability_offset, 8u, bar_size))
        return PROS_USB_XHCI_EBOUNDS;

    header = xhci_read32(base, capability_offset);
    if ((header & 0xFFu) != 1u)
        return PROS_USB_XHCI_EUNSUPPORTED;

    bios_owned = &base[capability_offset + 2u];
    os_owned = &base[capability_offset + 3u];
    *os_owned = (uint8_t)(*os_owned | 1u);

    for (uint32_t poll = 0u; poll < max_polls; ++poll) {
        if ((*bios_owned & 1u) == 0u)
            return PROS_USB_XHCI_OK;
        __asm__ volatile ("pause" ::: "memory");
    }
    return PROS_USB_XHCI_ETIMEOUT;
}

void pros_xhci_detach(struct pros_xhci_controller *controller) {
    if (!controller)
        return;
    if (controller->bar.virtual_address != 0u)
        pci_unmap_mmio_bar(&controller->bar);
    controller->initialized = 0u;
    controller->capabilities.version = 0u;
    controller->capabilities.capability_length = 0u;
    controller->capabilities.max_slots = 0u;
    controller->capabilities.max_interrupters = 0u;
    controller->capabilities.max_ports = 0u;
    controller->capabilities.operational_offset = 0u;
    controller->capabilities.doorbell_offset = 0u;
    controller->capabilities.runtime_offset = 0u;
    controller->capabilities.extended_capabilities_offset = 0u;
    controller->pci.bus = 0u;
    controller->pci.slot = 0u;
    controller->pci.function = 0u;
}

int pros_xhci_selftest(void) {
    uint32_t registers[256] = {0};
    struct pros_xhci_capabilities caps;
    uint32_t legacy_offset = 0u;
    volatile uint8_t *bytes = (volatile uint8_t *)registers;

    registers[0] = (0x0100u << 16) | 0x40u;
    registers[1] = 32u | (8u << 8) | (4u << 24);
    registers[4] = 0u;
    registers[5] = 0x100u;
    registers[6] = 0x200u;
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
            PROS_USB_XHCI_OK ||
        caps.version != 0x0100u || caps.capability_length != 0x40u ||
        caps.max_slots != 32u || caps.max_interrupters != 8u ||
        caps.max_ports != 4u || caps.operational_offset != 0x40u ||
        caps.doorbell_offset != 0x100u || caps.runtime_offset != 0x200u ||
        caps.extended_capabilities_offset != 0u)
        return -1;

    registers[0] = (0x0100u << 16) | 0x10u;
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
        PROS_USB_XHCI_EBOUNDS)
        return -2;
    registers[0] = (0x0100u << 16) | 0x40u;
    registers[5] = 0x1000u;
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
        PROS_USB_XHCI_EBOUNDS)
        return -3;
    registers[5] = 0x100u;
    registers[6] = 0x400u;
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
        PROS_USB_XHCI_EBOUNDS)
        return -4;
    registers[6] = 0x200u;
    registers[4] = (0x0400u << 16);
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
        PROS_USB_XHCI_EBOUNDS)
        return -5;
    /* Walk an unknown extended capability to the legacy-support entry. */
    registers[4] = (0x10u << 16); /* xECP = 0x40 */
    registers[16] = 2u | (4u << 8);
    registers[20] = 1u;
    if (pros_xhci_find_legacy_support(bytes, sizeof(registers), 0x40u,
                                      &legacy_offset) != PROS_USB_XHCI_OK ||
        legacy_offset != 0x50u)
        return -8;
    registers[16] = 2u;
    if (pros_xhci_find_legacy_support(bytes, sizeof(registers), 0x40u,
                                      &legacy_offset) != PROS_USB_XHCI_ENODEV)
        return -9;
    registers[16] = 2u | (0xFFu << 8);
    if (pros_xhci_find_legacy_support(bytes, sizeof(registers), 0x40u,
                                      &legacy_offset) != PROS_USB_XHCI_EBOUNDS)
        return -10;
    if (pros_xhci_find_legacy_support(bytes, sizeof(registers), 0u,
                                      &legacy_offset) != PROS_USB_XHCI_ENODEV ||
        pros_xhci_find_legacy_support(bytes, sizeof(registers), 0x1000u,
                                      &legacy_offset) != PROS_USB_XHCI_EBOUNDS)
        return -11;
    /* BIOS-owned clear: request OS ownership without touching BIOS state. */
    registers[16] = 1u;
    bytes[0x42] = 0u;
    bytes[0x43] = 0u;
    if (pros_xhci_legacy_handoff(bytes, sizeof(registers), 0x40u, 1u) !=
            PROS_USB_XHCI_OK || bytes[0x43] != 1u || bytes[0x42] != 0u)
        return -12;
    /* BIOS-owned set: OS request is retained and the finite budget expires. */
    bytes[0x42] = 1u;
    bytes[0x43] = 0u;
    if (pros_xhci_legacy_handoff(bytes, sizeof(registers), 0x40u, 1u) !=
            PROS_USB_XHCI_ETIMEOUT || bytes[0x43] != 1u || bytes[0x42] != 1u)
        return -13;
    if (pros_xhci_legacy_handoff(bytes, sizeof(registers), 0x40u, 0u) !=
            PROS_USB_XHCI_EINVAL ||
        pros_xhci_legacy_handoff(bytes, sizeof(registers), 0x1000u, 1u) !=
            PROS_USB_XHCI_EBOUNDS)
        return -14;
    registers[16] = 2u;
    if (pros_xhci_legacy_handoff(bytes, sizeof(registers), 0x40u, 1u) !=
            PROS_USB_XHCI_EUNSUPPORTED)
        return -15;
    registers[4] = 0u;
    registers[1] = 0u;
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
        PROS_USB_XHCI_EUNSUPPORTED)
        return -6;
    if (pros_xhci_parse_capabilities(0, sizeof(registers), &caps) !=
            PROS_USB_XHCI_EINVAL ||
        pros_xhci_parse_capabilities(bytes, 8u, &caps) !=
            PROS_USB_XHCI_EBOUNDS ||
        pros_xhci_parse_capabilities(bytes, sizeof(registers), 0) !=
            PROS_USB_XHCI_EINVAL)
        return -7;
    return PROS_USB_XHCI_OK;
}

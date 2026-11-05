#include "usb_xhci.h"
#include "usb_xhci_ring.h"
#include "memory.h"
#include "usb_desc.h"

#define XHCI_CAPABILITY_MIN_LENGTH 0x20u
#define XHCI_OPERATIONAL_MIN_LENGTH 0x40u
#define XHCI_MAX_SUPPORTED_VERSION 0x0120u
#define XHCI_LEGACY_CTLSTS_OFFSET 4u
#define XHCI_LEGACY_SMI_ENABLE_MASK 0x0000E011u
#define XHCI_USBCMD_OFFSET 0u
#define XHCI_USBSTS_OFFSET 4u
#define XHCI_USBCMD_RUN (1u << 0)
#define XHCI_USBCMD_HCRST (1u << 1)
#define XHCI_USBCMD_LHCRST (1u << 4)
#define XHCI_USBCMD_CSS (1u << 8)
#define XHCI_USBCMD_CRS (1u << 9)
#define XHCI_USBCMD_ACTION_MASK \
    (XHCI_USBCMD_HCRST | XHCI_USBCMD_LHCRST | \
     XHCI_USBCMD_CSS | XHCI_USBCMD_CRS)
#define XHCI_USBSTS_HCHALTED (1u << 0)
#define XHCI_USBSTS_CNR (1u << 11)
#define XHCI_USBSTS_HCE (1u << 12)

static int xhci_read_pci_command(const struct pros_pci_device *device,
                                 uint32_t *command);

static uint32_t xhci_read32(const volatile uint8_t *base, uint32_t offset) {
    const volatile uint32_t *reg =
        (const volatile uint32_t *)(const volatile void *)(base + offset);
    return *reg;
}

static void xhci_write32(volatile uint8_t *base, uint32_t offset,
                         uint32_t value) {
    volatile uint32_t *reg =
        (volatile uint32_t *)(volatile void *)(base + offset);
    *reg = value;
}

static int xhci_range_valid(uint64_t offset, uint64_t length,
                            uint64_t total) {
    return offset <= total && length <= total - offset;
}

static int xhci_dma_spans_overlap(uint64_t a, uint64_t a_size,
                                  uint64_t b, uint64_t b_size)
{
    if (a_size == 0u || b_size == 0u)
        return 0;
    return a <= b ? (b - a) < a_size : (a - b) < b_size;
}

typedef uint32_t (*xhci_register_read_fn)(void *context, uint32_t offset);
typedef void (*xhci_register_write_fn)(void *context, uint32_t offset,
                                       uint32_t value);

static uint32_t xhci_mmio_context_read(void *context, uint32_t offset) {
    return xhci_read32((volatile uint8_t *)context, offset);
}

static void xhci_mmio_context_write(void *context, uint32_t offset,
                                    uint32_t value) {
    xhci_write32((volatile uint8_t *)context, offset, value);
}

/*
 * Stop the controller, wait for HCHalted, issue HCRST, then wait for both
 * HCRST self-clear and CNR clear. Each phase has its own finite poll budget.
 */
static int xhci_halt_reset_sequence(void *context,
                                   xhci_register_read_fn read_register,
                                   xhci_register_write_fn write_register,
                                   uint32_t max_polls) {
    uint32_t command;
    uint32_t status;
    uint32_t poll;
    int halted = 0;
    int reset_cleared = 0;
    int controller_ready = 0;

    if (!context || !read_register || !write_register || max_polls == 0u)
        return PROS_USB_XHCI_EINVAL;

    command = read_register(context, XHCI_USBCMD_OFFSET);
    /* Do not replay an in-flight one-shot command through a read/modify/write. */
    if ((command & XHCI_USBCMD_ACTION_MASK) != 0u)
        return PROS_USB_XHCI_EIO;

    if ((command & XHCI_USBCMD_RUN) != 0u) {
        command &= ~XHCI_USBCMD_RUN;
        write_register(context, XHCI_USBCMD_OFFSET, command);
    }

    for (poll = 0u; poll < max_polls; ++poll) {
        command = read_register(context, XHCI_USBCMD_OFFSET);
        status = read_register(context, XHCI_USBSTS_OFFSET);
        if ((command & XHCI_USBCMD_RUN) == 0u &&
            (status & XHCI_USBSTS_HCHALTED) != 0u) {
            halted = 1;
            break;
        }
        __asm__ volatile ("pause" ::: "memory");
    }
    if (!halted)
        return PROS_USB_XHCI_ETIMEOUT;

    command = read_register(context, XHCI_USBCMD_OFFSET);
    if ((command & (XHCI_USBCMD_RUN | XHCI_USBCMD_ACTION_MASK)) != 0u)
        return PROS_USB_XHCI_EIO;
    write_register(context, XHCI_USBCMD_OFFSET,
                   command | XHCI_USBCMD_HCRST);

    for (poll = 0u; poll < max_polls; ++poll) {
        if ((read_register(context, XHCI_USBCMD_OFFSET) &
             XHCI_USBCMD_HCRST) == 0u) {
            reset_cleared = 1;
            break;
        }
        __asm__ volatile ("pause" ::: "memory");
    }
    if (!reset_cleared)
        return PROS_USB_XHCI_ETIMEOUT;

    for (poll = 0u; poll < max_polls; ++poll) {
        if ((read_register(context, XHCI_USBSTS_OFFSET) &
             XHCI_USBSTS_CNR) == 0u) {
            controller_ready = 1;
            break;
        }
        __asm__ volatile ("pause" ::: "memory");
    }
    if (!controller_ready)
        return PROS_USB_XHCI_ETIMEOUT;

    return PROS_USB_XHCI_OK;
}

/* Minimal register model used to test the sequencing without real hardware. */
struct xhci_test_register_model {
    uint32_t command;
    uint32_t status;
    uint8_t auto_halt;
    uint8_t auto_reset;
    uint8_t auto_ready;
};

static uint32_t xhci_test_model_read(void *context, uint32_t offset) {
    struct xhci_test_register_model *model =
        (struct xhci_test_register_model *)context;
    if (offset == XHCI_USBCMD_OFFSET)
        return model->command;
    if (offset == XHCI_USBSTS_OFFSET)
        return model->status;
    return 0u;
}

static void xhci_test_model_write(void *context, uint32_t offset,
                                  uint32_t value) {
    struct xhci_test_register_model *model =
        (struct xhci_test_register_model *)context;
    if (offset != XHCI_USBCMD_OFFSET)
        return;
    model->command = value;
    if ((value & XHCI_USBCMD_RUN) == 0u && model->auto_halt)
        model->status |= XHCI_USBSTS_HCHALTED;
    if ((value & XHCI_USBCMD_HCRST) != 0u && model->auto_reset) {
        model->command &= ~XHCI_USBCMD_HCRST;
        if (model->auto_ready)
            model->status &= ~XHCI_USBSTS_CNR;
        model->status |= XHCI_USBSTS_HCHALTED;
    }
}

int pros_xhci_parse_capabilities(const volatile void *mmio,
                                 uint64_t bar_size,
                                 struct pros_xhci_capabilities *capabilities) {
    const volatile uint8_t *base = (const volatile uint8_t *)mmio;
    uint32_t cap0, hcs1, hcs2, hcc1, dboff, rtsoff, page_sizes;
    uint32_t cap_length, version, operational, doorbell, runtime, ext_caps;
    uint32_t slots, interrupters, ports, scratchpads;

    if (!mmio || !capabilities ||
        (((uintptr_t)mmio) & (sizeof(uint32_t) - 1u)) != 0u)
        return PROS_USB_XHCI_EINVAL;
    capabilities->version = 0u;
    capabilities->capability_length = 0u;
    capabilities->max_slots = 0u;
    capabilities->max_interrupters = 0u;
    capabilities->max_ports = 0u;
    capabilities->scratchpad_buffers = 0u;
    capabilities->page_size_mask = 0u;
    capabilities->address_64bit = 0u;
    capabilities->context_size_64 = 0u;
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
    hcs2 = xhci_read32(base, 8u);
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
    scratchpads = ((hcs2 >> 21) & 0x1Fu) |
                  (((hcs2 >> 27) & 0x1Fu) << 5);

    if (slots == 0u || interrupters == 0u || ports == 0u)
        return PROS_USB_XHCI_EUNSUPPORTED;
    /*
     * Doorbell array contains one entry for the host plus one per slot.
     * Runtime space has MFINDEX (0x20 bytes) followed by one 0x20-byte
     * register set per advertised interrupter. Validate the complete spans,
     * not merely the first register in each region.
     */
    {
        uint64_t doorbell_bytes = ((uint64_t)slots + 1u) * 4u;
        uint64_t runtime_bytes = 0x20u + (uint64_t)interrupters * 0x20u;
        if (!xhci_range_valid(operational, XHCI_OPERATIONAL_MIN_LENGTH,
                              bar_size) ||
            !xhci_range_valid(doorbell, doorbell_bytes, bar_size) ||
            !xhci_range_valid(runtime, runtime_bytes, bar_size))
            return PROS_USB_XHCI_EBOUNDS;
    }
    /*
     * The current DMA allocator supplies 4 KiB pages and identity-mapped
     * addresses below 1 GiB. Reject controllers that do not advertise 4 KiB
     * pages; larger-page-only controllers cannot safely consume these rings.
     */
    page_sizes = xhci_read32(base, operational + 0x08u);
    if ((page_sizes & 1u) == 0u)
        return PROS_USB_XHCI_EUNSUPPORTED;

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
    capabilities->scratchpad_buffers = (uint16_t)scratchpads;
    capabilities->page_size_mask = (uint16_t)(page_sizes & 0xFFFFu);
    capabilities->address_64bit = (uint8_t)(hcc1 & 1u);
    capabilities->context_size_64 = (uint8_t)((hcc1 >> 2) & 1u);
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
    controller->ownership_claimed = 0u;
    controller->legacy_smi_disabled = 0u;
    controller->reset_complete = 0u;
    controller->bar.physical_address = 0u;
    controller->bar.virtual_address = 0u;
    controller->bar.size = 0u;
    rc = pci_usb_controller_type(device, &controller_type);
    if (rc != PROS_PCI_OK || controller_type != PROS_PCI_USB_XHCI)
        return PROS_USB_XHCI_EUNSUPPORTED;

    /*
     * Firmware may leave PCI bus mastering enabled. Clear and read back the
     * command bit before mapping or touching the controller; "not enabled by
     * this driver" is not a sufficient DMA-safety guarantee.
     */
    rc = pci_disable_bus_master(device);
    if (rc != PROS_PCI_OK)
        return PROS_USB_XHCI_EIO;

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

/*
 * Claim OS ownership after a successful read-only probe. A controller with
 * no extended capabilities or no Legacy Support entry needs no semaphore
 * handoff. This routine does not touch SMI controls, reset, DMA, or RUN.
 */
int pros_xhci_claim_ownership(struct pros_xhci_controller *controller,
                              uint32_t max_polls) {
    uint32_t legacy_offset = 0u;
    uint32_t first_offset;
    int rc;

    if (!controller || !controller->initialized ||
        controller->bar.virtual_address == 0u || controller->bar.size == 0u)
        return PROS_USB_XHCI_EINVAL;
    if (controller->ownership_claimed)
        return PROS_USB_XHCI_OK;

    first_offset = controller->capabilities.extended_capabilities_offset;
    if (first_offset == 0u) {
        controller->ownership_claimed = 1u;
        return PROS_USB_XHCI_OK;
    }

    rc = pros_xhci_find_legacy_support(
        (const volatile void *)(uintptr_t)controller->bar.virtual_address,
        controller->bar.size, first_offset, &legacy_offset);
    if (rc == PROS_USB_XHCI_ENODEV) {
        controller->ownership_claimed = 1u;
        return PROS_USB_XHCI_OK;
    }
    if (rc != PROS_USB_XHCI_OK)
        return rc;

    rc = pros_xhci_legacy_handoff(
        (volatile void *)(uintptr_t)controller->bar.virtual_address,
        controller->bar.size, legacy_offset, max_polls);
    if (rc != PROS_USB_XHCI_OK)
        return rc;

    controller->ownership_claimed = 1u;
    return PROS_USB_XHCI_OK;
}

/*
 * Disable all legacy SMI enables after OS ownership is established.
 * USBLEGCTLSTS mixes RW enables, RO shadow status, RW1C status and reserved
 * fields. Write zero to the complete register: this disables all SMI
 * enables, writes zero to RW1C status bits (no acknowledgement), and obeys
 * the required-zero behavior for reserved fields.
 */
int pros_xhci_disable_legacy_smi(struct pros_xhci_controller *controller) {
    volatile uint8_t *base;
    uint32_t first_offset, legacy_offset = 0u, register_offset;
    uint32_t verify;
    int rc;

    if (!controller || !controller->initialized ||
        !controller->ownership_claimed ||
        controller->bar.virtual_address == 0u || controller->bar.size == 0u)
        return PROS_USB_XHCI_EINVAL;
    if (controller->legacy_smi_disabled)
        return PROS_USB_XHCI_OK;

    first_offset = controller->capabilities.extended_capabilities_offset;
    if (first_offset == 0u) {
        controller->legacy_smi_disabled = 1u;
        return PROS_USB_XHCI_OK;
    }

    base = (volatile uint8_t *)(uintptr_t)controller->bar.virtual_address;
    rc = pros_xhci_find_legacy_support(base, controller->bar.size,
                                       first_offset, &legacy_offset);
    if (rc == PROS_USB_XHCI_ENODEV) {
        controller->legacy_smi_disabled = 1u;
        return PROS_USB_XHCI_OK;
    }
    if (rc != PROS_USB_XHCI_OK)
        return rc;

    register_offset = legacy_offset + XHCI_LEGACY_CTLSTS_OFFSET;
    if (!xhci_range_valid(register_offset, sizeof(uint32_t),
                          controller->bar.size))
        return PROS_USB_XHCI_EBOUNDS;

    xhci_write32(base, register_offset, 0u);

    verify = xhci_read32(base, register_offset);
    if ((verify & XHCI_LEGACY_SMI_ENABLE_MASK) != 0u)
        return PROS_USB_XHCI_EIO;

    controller->legacy_smi_disabled = 1u;
    return PROS_USB_XHCI_OK;
}

/*
 * Halt/reset is intentionally gated behind both firmware ownership and
 * legacy SMI shutdown. No PCI bus mastering, DMA, or controller RUN follows.
 */
int pros_xhci_halt_reset(struct pros_xhci_controller *controller,
                         uint32_t max_polls) {
    volatile uint8_t *base;
    uint32_t operational_offset;
    int rc;

    if (!controller || !controller->initialized ||
        !controller->ownership_claimed ||
        !controller->legacy_smi_disabled ||
        controller->bar.virtual_address == 0u || controller->bar.size == 0u)
        return PROS_USB_XHCI_EINVAL;
    if (controller->reset_complete)
        return PROS_USB_XHCI_OK;
    if (max_polls == 0u)
        return PROS_USB_XHCI_EINVAL;

    operational_offset = controller->capabilities.operational_offset;
    if ((operational_offset & 3u) != 0u ||
        !xhci_range_valid(operational_offset, 8u, controller->bar.size))
        return PROS_USB_XHCI_EBOUNDS;

    base = (volatile uint8_t *)(uintptr_t)controller->bar.virtual_address;
    rc = xhci_halt_reset_sequence(
        (void *)(base + operational_offset), xhci_mmio_context_read,
        xhci_mmio_context_write, max_polls);
    if (rc != PROS_USB_XHCI_OK)
        return rc;

    controller->reset_complete = 1u;
    return PROS_USB_XHCI_OK;
}

void pros_xhci_detach(struct pros_xhci_controller *controller) {
    if (!controller)
        return;
    if (controller->bar.virtual_address != 0u)
        pci_unmap_mmio_bar(&controller->bar);
    controller->initialized = 0u;
    controller->ownership_claimed = 0u;
    controller->legacy_smi_disabled = 0u;
    controller->reset_complete = 0u;
    controller->capabilities.version = 0u;
    controller->capabilities.capability_length = 0u;
    controller->capabilities.max_slots = 0u;
    controller->capabilities.max_interrupters = 0u;
    controller->capabilities.max_ports = 0u;
    controller->capabilities.scratchpad_buffers = 0u;
    controller->capabilities.page_size_mask = 0u;
    controller->capabilities.address_64bit = 0u;
    controller->capabilities.context_size_64 = 0u;
    controller->capabilities.operational_offset = 0u;
    controller->capabilities.doorbell_offset = 0u;
    controller->capabilities.runtime_offset = 0u;
    controller->capabilities.extended_capabilities_offset = 0u;
    controller->pci.bus = 0u;
    controller->pci.slot = 0u;
    controller->pci.function = 0u;
}


/*
 * Runtime register access is read-only in this stage. It is gated until the
 * controller has completed the bounded halt/reset sequence, and validates
 * the complete advertised runtime span before calculating any register.
 */
static int xhci_runtime_window(const struct pros_xhci_controller *controller,
                               uint64_t *runtime_base) {
    uint64_t span;
    if (!controller || !runtime_base || !controller->initialized ||
        !controller->reset_complete || controller->bar.virtual_address == 0u ||
        controller->bar.size == 0u)
        return PROS_USB_XHCI_EINVAL;
    if ((controller->capabilities.runtime_offset & 0x1Fu) != 0u ||
        controller->capabilities.max_interrupters == 0u)
        return PROS_USB_XHCI_EBOUNDS;
    span = 0x20u + (uint64_t)controller->capabilities.max_interrupters * 0x20u;
    if (!xhci_range_valid(controller->capabilities.runtime_offset, span,
                          controller->bar.size))
        return PROS_USB_XHCI_EBOUNDS;
    *runtime_base = controller->capabilities.runtime_offset;
    return PROS_USB_XHCI_OK;
}

int pros_xhci_read_mfindex(const struct pros_xhci_controller *controller,
                           uint32_t *value) {
    uint64_t runtime_base;
    int rc;
    if (!value)
        return PROS_USB_XHCI_EINVAL;
    rc = xhci_runtime_window(controller, &runtime_base);
    if (rc != PROS_USB_XHCI_OK)
        return rc;
    if ((controller->bar.virtual_address & 3u) != 0u)
        return PROS_USB_XHCI_EBOUNDS;
    *value = xhci_read32(
        (const volatile uint8_t *)(uintptr_t)controller->bar.virtual_address,
        (uint32_t)runtime_base);
    return PROS_USB_XHCI_OK;
}

int pros_xhci_read_interrupter32(
    const struct pros_xhci_controller *controller, uint16_t interrupter,
    uint32_t register_offset, uint32_t *value) {
    uint64_t runtime_base, offset;
    int rc;
    if (!value || (register_offset != 0x00u && register_offset != 0x04u &&
                   register_offset != 0x08u))
        return PROS_USB_XHCI_EINVAL;
    rc = xhci_runtime_window(controller, &runtime_base);
    if (rc != PROS_USB_XHCI_OK)
        return rc;
    if (interrupter >= controller->capabilities.max_interrupters)
        return PROS_USB_XHCI_EBOUNDS;
    offset = runtime_base + 0x20u + (uint64_t)interrupter * 0x20u +
             register_offset;
    if ((offset & 3u) != 0u ||
        !xhci_range_valid(offset, sizeof(uint32_t), controller->bar.size) ||
        (controller->bar.virtual_address & 3u) != 0u)
        return PROS_USB_XHCI_EBOUNDS;
    *value = xhci_read32(
        (const volatile uint8_t *)(uintptr_t)controller->bar.virtual_address,
        (uint32_t)offset);
    return PROS_USB_XHCI_OK;
}

int pros_xhci_read_interrupter64(
    const struct pros_xhci_controller *controller, uint16_t interrupter,
    uint32_t register_offset, uint64_t *value) {
    uint64_t runtime_base, offset;
    const volatile uint64_t *reg;
    int rc;
    if (!value || (register_offset != 0x10u && register_offset != 0x18u))
        return PROS_USB_XHCI_EINVAL;
    rc = xhci_runtime_window(controller, &runtime_base);
    if (rc != PROS_USB_XHCI_OK)
        return rc;
    if (interrupter >= controller->capabilities.max_interrupters)
        return PROS_USB_XHCI_EBOUNDS;
    offset = runtime_base + 0x20u + (uint64_t)interrupter * 0x20u +
             register_offset;
    if ((offset & 7u) != 0u ||
        !xhci_range_valid(offset, sizeof(uint64_t), controller->bar.size) ||
        (controller->bar.virtual_address & 7u) != 0u)
        return PROS_USB_XHCI_EBOUNDS;
    reg = (const volatile uint64_t *)(uintptr_t)
          (controller->bar.virtual_address + offset);
    *value = *reg;
    return PROS_USB_XHCI_OK;
}


/*
 * Program one contiguous event-ring segment after validated ERST setup,
 * while leaving IMAN.IE disabled.
 * Writing zero to IMAN.IP does not acknowledge its RW1C pending status.
 * The caller must separately enable interrupts after the full controller
 * interrupt path is ready.
 */
int pros_xhci_program_interrupter_disabled(
    struct pros_xhci_controller *controller, uint16_t interrupter,
    const struct pros_xhci_erst *erst,
    const struct pros_xhci_event_ring *event_ring,
    uint16_t moderation_interval)
{
    uint64_t runtime_base, iman_offset, imod_offset, erstsz_offset;
    uint64_t erstba_offset, erdp_offset, bar_base;
    volatile uint8_t *bar;
    volatile uint64_t *erstba_reg, *erdp_reg;
    uint64_t event_bytes, erdp_value;
    uint32_t iman_value;
    int rc;

    if (!erst || !event_ring || !erst->initialized || !erst->entries ||
        erst->entry_count != 1u || erst->physical_address == 0u ||
        (erst->physical_address & 63u) != 0u ||
        (((uintptr_t)erst->entries & 63u) != 0u) ||
        !event_ring->initialized || !event_ring->trbs ||
        event_ring->entry_count < 4u ||
        event_ring->dequeue_index >= event_ring->entry_count ||
        (event_ring->physical_address & 0xFu) != 0u)
        return PROS_USB_XHCI_EINVAL;

    event_bytes = (uint64_t)event_ring->entry_count * PROS_XHCI_TRB_SIZE;
    if (event_ring->physical_address > UINT64_MAX - event_bytes ||
        erst->entries[0].reserved != 0u ||
        erst->entries[0].segment_base != event_ring->physical_address ||
        erst->entries[0].segment_size != event_ring->entry_count ||
        erst->physical_address > UINT64_MAX -
            sizeof(struct pros_xhci_erst_entry))
        return PROS_USB_XHCI_EINVAL;

    rc = xhci_runtime_window(controller, &runtime_base);
    if (rc != PROS_USB_XHCI_OK)
        return rc;
    if (interrupter >= controller->capabilities.max_interrupters)
        return PROS_USB_XHCI_EBOUNDS;

    iman_offset = runtime_base + 0x20u + (uint64_t)interrupter * 0x20u;
    imod_offset = iman_offset + 0x04u;
    erstsz_offset = iman_offset + 0x08u;
    erstba_offset = iman_offset + 0x10u;
    erdp_offset = iman_offset + 0x18u;
    if (!xhci_range_valid(iman_offset, 4u, controller->bar.size) ||
        !xhci_range_valid(imod_offset, 4u, controller->bar.size) ||
        !xhci_range_valid(erstsz_offset, 4u, controller->bar.size) ||
        !xhci_range_valid(erstba_offset, 8u, controller->bar.size) ||
        !xhci_range_valid(erdp_offset, 8u, controller->bar.size) ||
        (iman_offset & 3u) != 0u || (imod_offset & 3u) != 0u ||
        (erstsz_offset & 3u) != 0u || (erstba_offset & 7u) != 0u ||
        (erdp_offset & 7u) != 0u ||
        (controller->bar.virtual_address & 7u) != 0u)
        return PROS_USB_XHCI_EBOUNDS;
    bar_base = controller->bar.virtual_address;
    if (bar_base > UINT64_MAX - erdp_offset - sizeof(uint64_t))
        return PROS_USB_XHCI_EBOUNDS;
    bar = (volatile uint8_t *)(uintptr_t)bar_base;
    erstba_reg = (volatile uint64_t *)(volatile void *)(bar + erstba_offset);
    erdp_reg = (volatile uint64_t *)(volatile void *)(bar + erdp_offset);
    erdp_value = event_ring->physical_address +
                 (uint64_t)event_ring->dequeue_index * PROS_XHCI_TRB_SIZE;
    if ((erdp_value & 0xFu) != 0u)
        return PROS_USB_XHCI_EINVAL;

    /* Disable IE without writing one to IMAN.IP (RW1C). */
    xhci_write32(bar, (uint32_t)iman_offset, 0u);
    xhci_write32(bar, (uint32_t)imod_offset, moderation_interval);
    xhci_write32(bar, (uint32_t)erstsz_offset, erst->entry_count);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *erstba_reg = erst->physical_address;
    *erdp_reg = erdp_value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    iman_value = xhci_read32(bar, (uint32_t)iman_offset);
    if ((iman_value & (1u << 1)) != 0u ||
        (xhci_read32(bar, (uint32_t)imod_offset) & 0xFFFFu) !=
            moderation_interval ||
        xhci_read32(bar, (uint32_t)erstsz_offset) != erst->entry_count ||
        *erstba_reg != erst->physical_address ||
        (*erdp_reg & ~0xFull) != erdp_value ||
        (*erdp_reg & 0xFu) != 0u)
        return PROS_USB_XHCI_EIO;
    return PROS_USB_XHCI_OK;
}


/* Decode the Transfer Event produced by the endpoint transfer-ring path. */
int pros_xhci_decode_transfer_event(
    const struct pros_xhci_trb *event,
    struct pros_xhci_transfer_event *decoded)
{
    struct pros_xhci_transfer_event result;
    uint8_t type;
    if (!event || !decoded ||
        pros_xhci_event_type(event, &type) != PROS_XHCI_RING_OK ||
        type != 32u)
        return PROS_USB_XHCI_EINVAL;
    /* Event Data transfers carry opaque payload in parameter, not a TRB pointer. */
    result.event_data = (uint8_t)((event->control >> 2) & 1u);
    result.slot_id = (uint8_t)(event->control >> 24);
    result.endpoint_id = (uint8_t)((event->control >> 16) & 0x1Fu);
    result.completion_code = (uint8_t)(event->status >> 24);
    result.residual_length = event->status & 0x00FFFFFFu;
    result.parameter = event->parameter;
    if (result.slot_id == 0u || result.endpoint_id == 0u ||
        result.completion_code == 0u ||
        (!result.event_data && (result.parameter & 0xFu) != 0u))
        return PROS_USB_XHCI_EINVAL;
    *decoded = result;
    return PROS_USB_XHCI_OK;
}

int pros_xhci_transfer_event_selftest(void)
{
    struct pros_xhci_trb event = {0};
    struct pros_xhci_transfer_event decoded = {0};
    event.parameter = 0x12345000u;
    event.status = (1u << 24) | 0x1234u;
    event.control = (32u << PROS_XHCI_TRB_TYPE_SHIFT) |
                    (3u << 24) | (4u << 16) | PROS_XHCI_TRB_CYCLE;
    if (pros_xhci_decode_transfer_event(&event, &decoded) !=
            PROS_USB_XHCI_OK ||
        decoded.parameter != 0x12345000u ||
        decoded.residual_length != 0x1234u ||
        decoded.completion_code != 1u || decoded.slot_id != 3u ||
        decoded.endpoint_id != 4u || decoded.event_data != 0u)
        return -1;

    event.parameter = 0x123456789ABCDEF1ull;
    event.control |= (1u << 2); /* Event Data: parameter is not a TRB pointer. */
    if (pros_xhci_decode_transfer_event(&event, &decoded) !=
            PROS_USB_XHCI_OK ||
        decoded.parameter != 0x123456789ABCDEF1ull ||
        decoded.event_data != 1u)
        return -2;

    event.control &= ~(1u << 2);
    if (pros_xhci_decode_transfer_event(&event, &decoded) !=
        PROS_USB_XHCI_EINVAL)
        return -3;
    event.parameter = 0x12345000u;
    event.control &= ~(0xFFu << 24);
    if (pros_xhci_decode_transfer_event(&event, &decoded) !=
        PROS_USB_XHCI_EINVAL)
        return -4;
    event.control = (32u << PROS_XHCI_TRB_TYPE_SHIFT) |
                    (3u << 24) | PROS_USB_XHCI_OK;
    if (pros_xhci_decode_transfer_event(&event, &decoded) !=
        PROS_USB_XHCI_EINVAL)
        return -5;
    event.control = (32u << PROS_XHCI_TRB_TYPE_SHIFT) |
                    (3u << 24) | (4u << 16);
    event.status = 0u; /* Completion code zero is reserved/invalid. */
    if (pros_xhci_decode_transfer_event(&event, &decoded) !=
        PROS_USB_XHCI_EINVAL)
        return -6;
    event.status = 1u << 24;
    event.control = (33u << PROS_XHCI_TRB_TYPE_SHIFT) |
                    (3u << 24) | (4u << 16);
    if (pros_xhci_decode_transfer_event(&event, &decoded) !=
        PROS_USB_XHCI_EINVAL ||
        pros_xhci_decode_transfer_event(0, &decoded) != PROS_USB_XHCI_EINVAL ||
        pros_xhci_decode_transfer_event(&event, 0) != PROS_USB_XHCI_EINVAL)
        return -7;
    return PROS_USB_XHCI_OK;
}


/*
 * Publish the software event-ring dequeue position to ERDP and clear EHB.
 * The ERST/ring pair must match the configuration already programmed for
 * this interrupter. This does not enable IMAN.IE or dispatch interrupts.
 */
int pros_xhci_acknowledge_event_ring(
    struct pros_xhci_controller *controller, uint16_t interrupter,
    const struct pros_xhci_erst *erst,
    const struct pros_xhci_event_ring *event_ring)
{
    uint64_t runtime_base, iman_offset, erstsz_offset, erstba_offset;
    uint64_t erdp_offset, bar_base, erdp_value, event_bytes;
    volatile uint8_t *bar;
    volatile uint64_t *erstba_reg, *erdp_reg;
    int rc;

    if (!erst || !event_ring || !erst->initialized || !erst->entries ||
        erst->entry_count != 1u || erst->physical_address == 0u ||
        (erst->physical_address & 63u) != 0u ||
        !event_ring->initialized || !event_ring->trbs ||
        event_ring->entry_count < 4u ||
        event_ring->dequeue_index >= event_ring->entry_count ||
        (event_ring->physical_address & 0xFu) != 0u ||
        erst->entries[0].reserved != 0u ||
        erst->entries[0].segment_base != event_ring->physical_address ||
        erst->entries[0].segment_size != event_ring->entry_count ||
        erst->physical_address > UINT64_MAX -
            sizeof(struct pros_xhci_erst_entry))
        return PROS_USB_XHCI_EINVAL;
    event_bytes = (uint64_t)event_ring->entry_count * PROS_XHCI_TRB_SIZE;
    if (event_ring->physical_address > UINT64_MAX - event_bytes)
        return PROS_USB_XHCI_EINVAL;

    rc = xhci_runtime_window(controller, &runtime_base);
    if (rc != PROS_USB_XHCI_OK)
        return rc;
    if (interrupter >= controller->capabilities.max_interrupters)
        return PROS_USB_XHCI_EBOUNDS;

    iman_offset = runtime_base + 0x20u + (uint64_t)interrupter * 0x20u;
    erstsz_offset = iman_offset + 0x08u;
    erstba_offset = iman_offset + 0x10u;
    erdp_offset = iman_offset + 0x18u;
    if (!xhci_range_valid(iman_offset, 4u, controller->bar.size) ||
        !xhci_range_valid(erstsz_offset, 4u, controller->bar.size) ||
        !xhci_range_valid(erstba_offset, 8u, controller->bar.size) ||
        !xhci_range_valid(erdp_offset, 8u, controller->bar.size) ||
        (iman_offset & 3u) != 0u || (erstsz_offset & 3u) != 0u ||
        (erstba_offset & 7u) != 0u || (erdp_offset & 7u) != 0u ||
        (controller->bar.virtual_address & 7u) != 0u)
        return PROS_USB_XHCI_EBOUNDS;
    bar_base = controller->bar.virtual_address;
    if (bar_base > UINT64_MAX - erdp_offset - sizeof(uint64_t))
        return PROS_USB_XHCI_EBOUNDS;
    bar = (volatile uint8_t *)(uintptr_t)bar_base;
    erstba_reg = (volatile uint64_t *)(volatile void *)(bar + erstba_offset);
    erdp_reg = (volatile uint64_t *)(volatile void *)(bar + erdp_offset);
    if (xhci_read32(bar, (uint32_t)erstsz_offset) != erst->entry_count ||
        *erstba_reg != erst->physical_address)
        return PROS_USB_XHCI_EINVAL;
    rc = pros_xhci_event_ring_erdp_value(event_ring, &erdp_value);
    if (rc != PROS_XHCI_RING_OK)
        return PROS_USB_XHCI_EINVAL;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    *erdp_reg = erdp_value; /* EHB=1 is RW1C; DESI remains zero. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if ((*erdp_reg & ~0xFull) != (erdp_value & ~0xFull) ||
        (*erdp_reg & 0x7u) != 0u)
        return PROS_USB_XHCI_EIO;
    return PROS_USB_XHCI_OK;
}

int pros_xhci_event_ring_ack_selftest(void)
{
    static uint64_t mmio[128] __attribute__((aligned(64)));
    static struct pros_xhci_erst_entry table[1] __attribute__((aligned(64)));
    static struct pros_xhci_trb event_trbs[8] __attribute__((aligned(64)));
    struct pros_usb_dma_buffer table_dma = {
        0xB00000u, table, sizeof(table), sizeof(table), 1u, 11u
    };
    struct pros_usb_dma_buffer event_dma = {
        0xC00000u, event_trbs, sizeof(event_trbs), sizeof(event_trbs), 1u, 12u
    };
    struct pros_xhci_erst erst;
    struct pros_xhci_event_ring events;
    struct pros_xhci_controller controller = {0};
    volatile uint8_t *bytes = (volatile uint8_t *)mmio;
    volatile uint64_t *erdp = (volatile uint64_t *)(volatile void *)(bytes + 0x158u);

    controller.initialized = 1u;
    controller.reset_complete = 1u;
    controller.bar.virtual_address = (uint64_t)(uintptr_t)bytes;
    controller.bar.size = sizeof(mmio);
    controller.capabilities.runtime_offset = 0x100u;
    controller.capabilities.max_interrupters = 2u;
    if (pros_xhci_event_ring_init(&events, &event_dma, 8u) !=
            PROS_XHCI_RING_OK ||
        pros_xhci_erst_init(&erst, &table_dma, &event_dma, 8u) !=
            PROS_XHCI_RING_OK ||
        pros_xhci_program_interrupter_disabled(&controller, 1u, &erst,
                                                &events, 0u) !=
            PROS_USB_XHCI_OK)
        return -1;
    events.dequeue_index = 2u;
    if (pros_xhci_acknowledge_event_ring(&controller, 1u, &erst, &events) !=
            PROS_USB_XHCI_OK ||
        *erdp != ((event_dma.physical_address + 2u * PROS_XHCI_TRB_SIZE) |
                  (1ull << 3)))
        return -2;
    if (pros_xhci_acknowledge_event_ring(&controller, 2u, &erst, &events) !=
        PROS_USB_XHCI_EBOUNDS)
        return -3;
    {
        struct pros_xhci_erst bad = erst;
        bad.entry_count = 2u;
        if (pros_xhci_acknowledge_event_ring(&controller, 1u, &bad, &events) !=
            PROS_USB_XHCI_EINVAL)
            return -4;
    }
    {
        struct pros_xhci_event_ring bad = events;
        bad.physical_address = UINT64_MAX - 15u;
        if (pros_xhci_acknowledge_event_ring(&controller, 1u, &erst, &bad) !=
            PROS_USB_XHCI_EINVAL)
            return -5;
    }
    controller.reset_complete = 0u;
    if (pros_xhci_acknowledge_event_ring(&controller, 1u, &erst, &events) !=
        PROS_USB_XHCI_EINVAL)
        return -6;
    return PROS_USB_XHCI_OK;
}


/*
 * Prepare DCBAA, CONFIG and CRCR while the controller remains halted and
 * PCI bus mastering is still disabled. The command ring must be fresh and
 * empty; controller start is a separate, later stage.
 */
int pros_xhci_program_command_path_disabled(
    struct pros_xhci_controller *controller,
    const struct pros_usb_dma_buffer *dcbaa_dma,
    const struct pros_xhci_scratchpad_set *scratchpads,
    const struct pros_xhci_command_ring *command_ring)
{
    uint64_t dcbaa_bytes, ring_bytes;
    uint64_t op, usbcmd_offset, usbsts_offset, crcr_offset;
    uint64_t dcbaap_offset, config_offset, bar_base;
    volatile uint8_t *bar;
    volatile uint64_t *crcr_reg, *dcbaap_reg;
    uint64_t *dcbaa;
    uint32_t command, status, max_slots;
    if (!controller || !dcbaa_dma || !scratchpads || !command_ring ||
        !controller->initialized || !controller->ownership_claimed ||
        !controller->legacy_smi_disabled || !controller->reset_complete ||
        !controller->bar.virtual_address || !controller->bar.size ||
        controller->capabilities.max_slots == 0u ||
        !dcbaa_dma->virtual_address || !dcbaa_dma->physical_address ||
        !dcbaa_dma->allocation_id || !dcbaa_dma->page_count ||
        !command_ring->initialized || !command_ring->trbs ||
        command_ring->entry_count < 4u ||
        command_ring->physical_address == 0u ||
        command_ring->enqueue_index != 0u ||
        command_ring->dequeue_index != 0u ||
        command_ring->outstanding_count != 0u ||
        command_ring->producer_cycle != 1u ||
        command_ring->physical_address == 0u ||
        (command_ring->physical_address & 63u) != 0u ||
        (((uintptr_t)command_ring->trbs & 63u) != 0u))
        return PROS_USB_XHCI_EINVAL;

    max_slots = controller->capabilities.max_slots;
    dcbaa_bytes = ((uint64_t)max_slots + 1u) * sizeof(uint64_t);
    ring_bytes = (uint64_t)command_ring->entry_count * PROS_XHCI_TRB_SIZE;
    if (pros_xhci_scratchpad_validate(&controller->capabilities,
                                      scratchpads) != PROS_USB_XHCI_OK ||
        dcbaa_dma->size < dcbaa_bytes ||
        dcbaa_dma->allocation_size < dcbaa_bytes ||
        (dcbaa_dma->physical_address & 63u) != 0u ||
        (((uintptr_t)dcbaa_dma->virtual_address & 63u) != 0u) ||
        dcbaa_dma->physical_address > UINT64_MAX - dcbaa_bytes ||
        (uintptr_t)dcbaa_dma->virtual_address > UINTPTR_MAX - dcbaa_bytes ||
        command_ring->entry_count > UINT32_MAX / PROS_XHCI_TRB_SIZE ||
        command_ring->physical_address > UINT64_MAX - ring_bytes ||
        (uintptr_t)command_ring->trbs > UINTPTR_MAX - ring_bytes ||
        xhci_dma_spans_overlap(dcbaa_dma->physical_address, dcbaa_bytes,
                               command_ring->physical_address, ring_bytes) ||
        (scratchpads->count != 0u &&
         (xhci_dma_spans_overlap(dcbaa_dma->physical_address, dcbaa_bytes,
             scratchpads->pointer_array.physical_address,
             scratchpads->pointer_array.allocation_size) ||
          xhci_dma_spans_overlap(dcbaa_dma->physical_address, dcbaa_bytes,
             scratchpads->buffer_pool.physical_address,
             scratchpads->buffer_pool.allocation_size) ||
          xhci_dma_spans_overlap(command_ring->physical_address, ring_bytes,
             scratchpads->pointer_array.physical_address,
             scratchpads->pointer_array.allocation_size) ||
          xhci_dma_spans_overlap(command_ring->physical_address, ring_bytes,
             scratchpads->buffer_pool.physical_address,
             scratchpads->buffer_pool.allocation_size))))
        return PROS_USB_XHCI_EINVAL;

    op = controller->capabilities.operational_offset;
    usbcmd_offset = op;
    usbsts_offset = op + 0x04u;
    crcr_offset = op + 0x18u;
    dcbaap_offset = op + 0x30u;
    config_offset = op + 0x38u;
    if (!xhci_range_valid(usbcmd_offset, 4u, controller->bar.size) ||
        !xhci_range_valid(usbsts_offset, 4u, controller->bar.size) ||
        !xhci_range_valid(crcr_offset, 8u, controller->bar.size) ||
        !xhci_range_valid(dcbaap_offset, 8u, controller->bar.size) ||
        !xhci_range_valid(config_offset, 4u, controller->bar.size) ||
        (usbcmd_offset & 3u) != 0u || (usbsts_offset & 3u) != 0u ||
        (crcr_offset & 7u) != 0u || (dcbaap_offset & 7u) != 0u ||
        (config_offset & 3u) != 0u ||
        (controller->bar.virtual_address & 7u) != 0u)
        return PROS_USB_XHCI_EBOUNDS;
    bar_base = controller->bar.virtual_address;
    if (bar_base > UINT64_MAX - config_offset - sizeof(uint32_t))
        return PROS_USB_XHCI_EBOUNDS;
    bar = (volatile uint8_t *)(uintptr_t)bar_base;
    command = xhci_read32(bar, (uint32_t)usbcmd_offset);
    status = xhci_read32(bar, (uint32_t)usbsts_offset);
    if ((command & XHCI_USBCMD_RUN) != 0u ||
        (status & XHCI_USBSTS_HCHALTED) == 0u)
        return PROS_USB_XHCI_EIO;

    dcbaa = (uint64_t *)dcbaa_dma->virtual_address;
    for (uint32_t i = 0u; i <= max_slots; ++i)
        dcbaa[i] = 0u;
    /* xHCI reserves DCBAA[0] for the Scratchpad Buffer Array pointer. */
    if (scratchpads->count != 0u)
        dcbaa[0] = scratchpads->pointer_array.physical_address;
    __atomic_thread_fence(__ATOMIC_RELEASE);

    dcbaap_reg = (volatile uint64_t *)(volatile void *)(bar + dcbaap_offset);
    crcr_reg = (volatile uint64_t *)(volatile void *)(bar + crcr_offset);
    *dcbaap_reg = dcbaa_dma->physical_address;
    xhci_write32(bar, (uint32_t)config_offset, max_slots);
    *crcr_reg = command_ring->physical_address | 1ull; /* RCS=1 */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (*dcbaap_reg != dcbaa_dma->physical_address ||
        (xhci_read32(bar, (uint32_t)config_offset) & 0xFFu) != max_slots ||
        (*crcr_reg & ~0x3Full) != command_ring->physical_address ||
        (*crcr_reg & 1u) == 0u ||
        (xhci_read32(bar, (uint32_t)usbcmd_offset) & XHCI_USBCMD_RUN) != 0u)
        return PROS_USB_XHCI_EIO;
    return PROS_USB_XHCI_OK;
}


/*
 * Start a fully-programmed controller and prove the command/event DMA path
 * with one No-op Command. Polling only: USBCMD.INTE and IMAN.IE stay clear.
 * DMA ownership is pinned before bus mastering is granted and is never
 * released by this routine, including timeout/error recovery.
 */
int pros_xhci_noop_command(struct pros_xhci_controller *controller,
                           struct pros_xhci_dma_resources *resources,
                           uint16_t interrupter, uint32_t run_polls,
                           uint32_t completion_polls)
{
    uint64_t op, iman, doorbell;
    uint64_t expected_command;
    volatile uint8_t *bar;
    uint32_t command, status, poll;
    struct pros_xhci_trb event;
    uint64_t event_address;
    uint8_t event_type, completion_code;
    int rc, halted = 0;

    if (!controller || !resources || !controller->initialized ||
        !controller->ownership_claimed || !controller->legacy_smi_disabled ||
        !controller->reset_complete || !controller->bar.virtual_address ||
        !resources->initialized || !resources->hardware_referenced ||
        !resources->command_ring.initialized ||
        !resources->event_ring.initialized || !resources->erst.initialized ||
        interrupter >= controller->capabilities.max_interrupters ||
        run_polls == 0u || completion_polls == 0u)
        return PROS_USB_XHCI_EINVAL;

    rc = pros_xhci_validate_dma_layout(controller, &resources->dcbaa,
            &resources->command_ring, &resources->erst,
            &resources->event_ring);
    if (rc != PROS_USB_XHCI_OK)
        return rc;
    if (resources->command_ring.outstanding_count != 0u)
        return PROS_USB_XHCI_EIO;

    op = controller->capabilities.operational_offset;
    iman = (uint64_t)controller->capabilities.runtime_offset + 0x20u +
           (uint64_t)interrupter * 0x20u;
    doorbell = controller->capabilities.doorbell_offset;
    if (!xhci_range_valid(op, 0x40u, controller->bar.size) ||
        !xhci_range_valid(iman, 4u, controller->bar.size) ||
        !xhci_range_valid(doorbell, 4u, controller->bar.size) ||
        (op & 3u) != 0u || (iman & 3u) != 0u ||
        (doorbell & 3u) != 0u ||
        (controller->bar.virtual_address & 7u) != 0u)
        return PROS_USB_XHCI_EBOUNDS;

    bar = (volatile uint8_t *)(uintptr_t)controller->bar.virtual_address;
    command = xhci_read32(bar, (uint32_t)op);
    status = xhci_read32(bar, (uint32_t)(op + 4u));
    if ((command & (XHCI_USBCMD_RUN | (1u << 2))) != 0u ||
        (status & XHCI_USBSTS_HCHALTED) == 0u ||
        (xhci_read32(bar, (uint32_t)iman) & 2u) != 0u)
        return PROS_USB_XHCI_EIO;

    /* Verify the DMA pointers still match the persistent software owners. */
    {
        volatile uint64_t *crcr =
            (volatile uint64_t *)(volatile void *)(bar + op + 0x18u);
        volatile uint64_t *dcbaap =
            (volatile uint64_t *)(volatile void *)(bar + op + 0x30u);
        volatile uint64_t *erstba =
            (volatile uint64_t *)(volatile void *)(bar + iman + 0x10u);
        if ((*crcr & ~0x3Full) != resources->command_ring.physical_address ||
            (*dcbaap & ~0x3Full) != resources->dcbaa.physical_address ||
            (*erstba & ~0x3Full) != resources->erst.physical_address ||
            xhci_read32(bar, (uint32_t)(iman + 0x08u)) !=
                resources->erst.entry_count)
            return PROS_USB_XHCI_EIO;
    }

    /* From this point onward, retain every DMA allocation permanently. */
    resources->hardware_referenced = 1u;
    rc = xhci_read_pci_command(&controller->pci, &status);
    if (rc != PROS_PCI_OK || (status & (1u << 2)) == 0u)
        return PROS_USB_XHCI_EIO;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    xhci_write32(bar, (uint32_t)op, command | XHCI_USBCMD_RUN);
    for (poll = 0u; poll < run_polls; ++poll) {
        status = xhci_read32(bar, (uint32_t)(op + 4u));
        if ((status & XHCI_USBSTS_HCE) != 0u) {
            rc = PROS_USB_XHCI_ESTART;
            goto capture_failure;
        }
        if ((status & XHCI_USBSTS_HCHALTED) == 0u &&
            (status & XHCI_USBSTS_CNR) == 0u)
            break;
        __asm__ volatile ("pause" ::: "memory");
    }
    if (poll == run_polls) {
        rc = PROS_USB_XHCI_ESTART;
        goto capture_failure;
    }

    rc = pros_xhci_command_ring_enqueue(&resources->command_ring, 0u, 0u,
            23u << PROS_XHCI_TRB_TYPE_SHIFT, &expected_command);
    if (rc != PROS_XHCI_RING_OK)
        goto recover;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    xhci_write32(bar, (uint32_t)doorbell, 0u);

    for (poll = 0u; poll < completion_polls; ++poll) {
        rc = pros_xhci_event_ring_dequeue(&resources->event_ring, &event,
                                          &event_address);
        if (rc == PROS_XHCI_RING_EEMPTY) {
            __asm__ volatile ("pause" ::: "memory");
            continue;
        }
        if (rc != PROS_XHCI_RING_OK)
            goto recover;
        rc = pros_xhci_event_type(&event, &event_type);
        if (rc != PROS_XHCI_RING_OK || event_type != 33u) {
            (void)pros_xhci_acknowledge_event_ring(controller, interrupter,
                    &resources->erst, &resources->event_ring);
            goto recover;
        }
        rc = pros_xhci_command_ring_handle_completion(
                &resources->command_ring, &event, &completion_code);
        if (pros_xhci_acknowledge_event_ring(controller, interrupter,
                &resources->erst, &resources->event_ring) != PROS_USB_XHCI_OK)
            goto recover;
        if (rc != PROS_XHCI_RING_OK || event.parameter != expected_command ||
            completion_code != 1u)
            goto recover;
        return PROS_USB_XHCI_OK;
    }
    rc = PROS_USB_XHCI_ECOMPLETION;
capture_failure:
    /*
     * Preserve live hardware state for both controller HCE and startup/
     * completion timeouts before recovery clears RUN and halts the controller.
     */
    resources->diagnostic_usbcmd = xhci_read32(bar, (uint32_t)op);
    resources->diagnostic_usbsts = xhci_read32(bar, (uint32_t)(op + 4u));
    resources->diagnostic_iman = xhci_read32(bar, (uint32_t)iman);
    resources->diagnostic_doorbell = xhci_read32(bar, (uint32_t)doorbell);
    resources->diagnostic_crcr =
        *(volatile uint64_t *)(volatile void *)(bar + op + 0x18u);
    resources->diagnostic_erdp =
        *(volatile uint64_t *)(volatile void *)(bar + iman + 0x18u);
    resources->diagnostic_command_control =
        resources->command_ring.trbs[0].control;
    resources->diagnostic_event_control =
        resources->event_ring.trbs[0].control;
    resources->diagnostic_event_dequeue_index =
        resources->event_ring.dequeue_index;
    resources->diagnostic_event_cycle =
        resources->event_ring.consumer_cycle;
    resources->diagnostic_command_enqueue_index =
        resources->command_ring.enqueue_index;
    resources->diagnostic_command_outstanding =
        resources->command_ring.outstanding_count;
    resources->diagnostic_command_cycle =
        resources->command_ring.producer_cycle;

recover:
    /* Do not revoke bus mastering unless hardware confirms it is halted. */
    command = xhci_read32(bar, (uint32_t)op);
    if ((command & XHCI_USBCMD_RUN) != 0u)
        xhci_write32(bar, (uint32_t)op, command & ~XHCI_USBCMD_RUN);
    for (poll = 0u; poll < run_polls; ++poll) {
        status = xhci_read32(bar, (uint32_t)(op + 4u));
        if ((status & XHCI_USBSTS_HCHALTED) != 0u) {
            halted = 1;
            break;
        }
        __asm__ volatile ("pause" ::: "memory");
    }
    if (halted)
        (void)pci_disable_bus_master(&controller->pci);
    /* A failed halt deliberately leaves bus mastering and DMA owners pinned. */
    return rc != PROS_USB_XHCI_OK ? rc : PROS_USB_XHCI_EIO;
}


/*
 * Validate the complete DMA memory layout before PCI bus mastering is ever
 * granted. Physical and virtual ranges must be disjoint across all four
 * controller-visible objects.
 */
int pros_xhci_validate_dma_layout(
    const struct pros_xhci_controller *controller,
    const struct pros_usb_dma_buffer *dcbaa_dma,
    const struct pros_xhci_command_ring *command_ring,
    const struct pros_xhci_erst *erst,
    const struct pros_xhci_event_ring *event_ring)
{
    uint64_t dcbaa_bytes, command_bytes, erst_bytes, event_bytes;
    uint64_t p[4], n[4], v[4];
    if (!controller || !dcbaa_dma || !command_ring || !erst || !event_ring ||
        !controller->initialized || controller->capabilities.max_slots == 0u ||
        !dcbaa_dma->virtual_address || !dcbaa_dma->physical_address ||
        !dcbaa_dma->allocation_id || !dcbaa_dma->page_count ||
        !command_ring->initialized || !command_ring->trbs ||
        command_ring->entry_count < 4u ||
        command_ring->enqueue_index >= command_ring->entry_count - 1u ||
        command_ring->dequeue_index >= command_ring->entry_count - 1u ||
        command_ring->outstanding_count > command_ring->entry_count - 1u ||
        command_ring->producer_cycle > 1u || !erst->initialized ||
        !erst->entries || erst->entry_count != 1u ||
        !event_ring->initialized || !event_ring->trbs ||
        event_ring->entry_count < 4u ||
        event_ring->dequeue_index >= event_ring->entry_count ||
        event_ring->consumer_cycle > 1u)
        return PROS_USB_XHCI_EINVAL;

    dcbaa_bytes = ((uint64_t)controller->capabilities.max_slots + 1u) *
                  sizeof(uint64_t);
    if (command_ring->entry_count > UINT32_MAX / PROS_XHCI_TRB_SIZE ||
        event_ring->entry_count > UINT32_MAX / PROS_XHCI_TRB_SIZE)
        return PROS_USB_XHCI_EBOUNDS;
    command_bytes = (uint64_t)command_ring->entry_count *
                    PROS_XHCI_TRB_SIZE;
    erst_bytes = (uint64_t)erst->entry_count *
                 sizeof(struct pros_xhci_erst_entry);
    event_bytes = (uint64_t)event_ring->entry_count *
                  PROS_XHCI_TRB_SIZE;

    if (dcbaa_dma->size < dcbaa_bytes ||
        dcbaa_dma->allocation_size < dcbaa_bytes ||
        (dcbaa_dma->physical_address & 63u) != 0u ||
        (((uintptr_t)dcbaa_dma->virtual_address & 63u) != 0u) ||
        dcbaa_dma->physical_address > UINT64_MAX - dcbaa_bytes ||
        (uintptr_t)dcbaa_dma->virtual_address > UINTPTR_MAX - dcbaa_bytes ||
        command_ring->physical_address == 0u ||
        (command_ring->physical_address & 63u) != 0u ||
        (((uintptr_t)command_ring->trbs & 63u) != 0u) ||
        command_ring->physical_address > UINT64_MAX - command_bytes ||
        (uintptr_t)command_ring->trbs > UINTPTR_MAX - command_bytes ||
        erst->physical_address == 0u ||
        (erst->physical_address & 63u) != 0u ||
        erst->physical_address > UINT64_MAX - erst_bytes ||
        (uintptr_t)erst->entries > UINTPTR_MAX - erst_bytes ||
        erst->entries[0].reserved != 0u ||
        event_ring->physical_address == 0u ||
        (event_ring->physical_address & 15u) != 0u ||
        (((uintptr_t)event_ring->trbs & 15u) != 0u) ||
        event_ring->physical_address > UINT64_MAX - event_bytes ||
        (uintptr_t)event_ring->trbs > UINTPTR_MAX - event_bytes ||
        erst->entries[0].segment_base != event_ring->physical_address ||
        erst->entries[0].segment_size != event_ring->entry_count)
        return PROS_USB_XHCI_EBOUNDS;

    p[0] = dcbaa_dma->physical_address;
    p[1] = command_ring->physical_address;
    p[2] = erst->physical_address;
    p[3] = event_ring->physical_address;
    n[0] = dcbaa_bytes;
    n[1] = command_bytes;
    n[2] = erst_bytes;
    n[3] = event_bytes;
    v[0] = (uint64_t)(uintptr_t)dcbaa_dma->virtual_address;
    v[1] = (uint64_t)(uintptr_t)command_ring->trbs;
    v[2] = (uint64_t)(uintptr_t)erst->entries;
    v[3] = (uint64_t)(uintptr_t)event_ring->trbs;
    for (uint32_t i = 0u; i < 4u; ++i) {
        for (uint32_t j = i + 1u; j < 4u; ++j) {
            if (xhci_dma_spans_overlap(p[i], n[i], p[j], n[j]) ||
                xhci_dma_spans_overlap(v[i], n[i], v[j], n[j]))
                return PROS_USB_XHCI_EINVAL;
        }
    }
    return PROS_USB_XHCI_OK;
}

int pros_xhci_dma_layout_selftest(void)
{
    static uint64_t dcbaa[8] __attribute__((aligned(64)));
    static struct pros_xhci_trb command_trbs[8] __attribute__((aligned(64)));
    static struct pros_xhci_erst_entry erst_entries[1] __attribute__((aligned(64)));
    static struct pros_xhci_trb event_trbs[8] __attribute__((aligned(64)));
    struct pros_usb_dma_buffer dcbaa_dma = {
        0x1000000u, dcbaa, sizeof(dcbaa), sizeof(dcbaa), 1u, 21u
    };
    struct pros_usb_dma_buffer command_dma = {
        0x1100000u, command_trbs, sizeof(command_trbs),
        sizeof(command_trbs), 1u, 22u
    };
    struct pros_usb_dma_buffer event_dma = {
        0x1300000u, event_trbs, sizeof(event_trbs),
        sizeof(event_trbs), 1u, 23u
    };
    struct pros_xhci_command_ring command;
    struct pros_xhci_erst erst;
    struct pros_xhci_event_ring events;
    struct pros_xhci_controller controller = {0};

    controller.initialized = 1u;
    controller.capabilities.max_slots = 7u;
    if (pros_xhci_command_ring_init(&command, &command_dma, 8u) !=
            PROS_XHCI_RING_OK ||
        pros_xhci_event_ring_init(&events, &event_dma, 8u) !=
            PROS_XHCI_RING_OK ||
        pros_xhci_erst_init(&erst,
            &(struct pros_usb_dma_buffer){
                0x1200000u, erst_entries, sizeof(erst_entries),
                sizeof(erst_entries), 1u, 24u
            }, &event_dma, 8u) != PROS_XHCI_RING_OK ||
        pros_xhci_validate_dma_layout(&controller, &dcbaa_dma, &command,
                                      &erst, &events) != PROS_USB_XHCI_OK)
        return -1;
    {
        struct pros_usb_dma_buffer bad = dcbaa_dma;
        bad.physical_address = command.physical_address;
        if (pros_xhci_validate_dma_layout(&controller, &bad, &command,
                                          &erst, &events) !=
            PROS_USB_XHCI_EINVAL)
            return -2;
    }
    {
        struct pros_xhci_event_ring bad = events;
        struct pros_xhci_erst bad_erst = erst;
        bad.physical_address = command.physical_address;
        bad_erst.entries[0].segment_base = bad.physical_address;
        if (pros_xhci_validate_dma_layout(&controller, &dcbaa_dma, &command,
                                          &bad_erst, &bad) !=
            PROS_USB_XHCI_EINVAL)
            return -3;
    }
    {
        struct pros_xhci_erst bad = erst;
        bad.entries[0].segment_size = 4u;
        if (pros_xhci_validate_dma_layout(&controller, &dcbaa_dma, &command,
                                          &bad, &events) !=
            PROS_USB_XHCI_EBOUNDS)
            return -4;
    }
    {
        struct pros_xhci_controller bad = controller;
        bad.capabilities.max_slots = 0u;
        if (pros_xhci_validate_dma_layout(&bad, &dcbaa_dma, &command,
                                          &erst, &events) !=
            PROS_USB_XHCI_EINVAL)
            return -5;
    }
    {
        struct pros_xhci_command_ring bad = command;
        bad.dequeue_index = bad.entry_count;
        if (pros_xhci_validate_dma_layout(&controller, &dcbaa_dma, &bad,
                                          &erst, &events) !=
            PROS_USB_XHCI_EINVAL)
            return -6;
    }
    return PROS_USB_XHCI_OK;
}

int pros_xhci_command_path_setup_selftest(void)
{
    static uint64_t mmio[128] __attribute__((aligned(64)));
    static uint64_t dcbaa[32] __attribute__((aligned(64)));
    static struct pros_xhci_trb command_trbs[8] __attribute__((aligned(64)));
    struct pros_usb_dma_buffer dcbaa_dma = {
        0xD00000u, dcbaa, sizeof(dcbaa), sizeof(dcbaa), 1u, 13u
    };
    struct pros_usb_dma_buffer command_dma = {
        0xE00000u, command_trbs, sizeof(command_trbs),
        sizeof(command_trbs), 1u, 14u
    };
    struct pros_xhci_command_ring command_ring;
    struct pros_xhci_scratchpad_set scratchpads = {0};
    struct pros_xhci_controller controller = {0};
    volatile uint8_t *bytes = (volatile uint8_t *)mmio;
    volatile uint32_t *usbcmd = (volatile uint32_t *)(volatile void *)(bytes + 0x40u);
    volatile uint32_t *usbsts = (volatile uint32_t *)(volatile void *)(bytes + 0x44u);
    volatile uint32_t *config = (volatile uint32_t *)(volatile void *)(bytes + 0x78u);
    volatile uint64_t *crcr = (volatile uint64_t *)(volatile void *)(bytes + 0x58u);
    volatile uint64_t *dcbaap = (volatile uint64_t *)(volatile void *)(bytes + 0x70u);

    controller.initialized = 1u;
    controller.ownership_claimed = 1u;
    controller.legacy_smi_disabled = 1u;
    controller.reset_complete = 1u;
    controller.bar.virtual_address = (uint64_t)(uintptr_t)bytes;
    controller.bar.size = sizeof(mmio);
    controller.capabilities.operational_offset = 0x40u;
    controller.capabilities.max_slots = 16u;
    controller.capabilities.scratchpad_buffers = 2u;
    controller.capabilities.page_size_mask = 1u;
    *usbcmd = 0u;
    *usbsts = XHCI_USBSTS_HCHALTED;
    for (uint32_t i = 0u; i < 32u; ++i)
        dcbaa[i] = UINT64_MAX;
    if (pros_xhci_scratchpad_alloc(&controller.capabilities, &scratchpads) !=
            PROS_USB_XHCI_OK ||
        pros_xhci_command_ring_init(&command_ring, &command_dma, 8u) !=
            PROS_XHCI_RING_OK ||
        pros_xhci_program_command_path_disabled(&controller, &dcbaa_dma, &scratchpads, &command_ring) !=
            PROS_USB_XHCI_OK ||
        *config != 16u || *dcbaap != dcbaa_dma.physical_address ||
        dcbaa[0] != scratchpads.pointer_array.physical_address ||
        *crcr != (command_dma.physical_address | 1ull) ||
        *usbcmd != 0u)
        return -1;
    for (uint32_t i = 1u; i <= 16u; ++i)
        if (dcbaa[i] != 0u)
            return -2;
    {
        struct pros_xhci_scratchpad_set bad = scratchpads;
        bad.count = 1u;
        if (pros_xhci_program_command_path_disabled(&controller, &dcbaa_dma,
                &bad, &command_ring) != PROS_USB_XHCI_EINVAL)
            return -8;
    }
    {
        struct pros_usb_dma_buffer bad = dcbaa_dma;
        bad.size = 8u;
        if (pros_xhci_program_command_path_disabled(&controller, &bad, &scratchpads, &command_ring) !=
            PROS_USB_XHCI_EINVAL)
            return -3;
    }
    {
        struct pros_xhci_controller bad = controller;
        bad.legacy_smi_disabled = 0u;
        if (pros_xhci_program_command_path_disabled(&bad, &dcbaa_dma, &scratchpads, &command_ring) !=
            PROS_USB_XHCI_EINVAL)
            return -4;
    }
    {
        struct pros_xhci_command_ring bad = command_ring;
        bad.physical_address = dcbaa_dma.physical_address;
        if (pros_xhci_program_command_path_disabled(&controller, &dcbaa_dma, &scratchpads, &bad) !=
            PROS_USB_XHCI_EINVAL)
            return -5;
        bad = command_ring;
        bad.outstanding_count = 1u;
        if (pros_xhci_program_command_path_disabled(&controller, &dcbaa_dma, &scratchpads, &bad) !=
            PROS_USB_XHCI_EINVAL)
            return -6;
    }
    {
        struct pros_xhci_controller bad = controller;
        *usbsts = 0u;
        if (pros_xhci_program_command_path_disabled(&bad, &dcbaa_dma, &scratchpads, &command_ring) !=
            PROS_USB_XHCI_EIO)
            return -7;
        *usbsts = XHCI_USBSTS_HCHALTED;
    }
    if (pros_xhci_scratchpad_release(&scratchpads) != PROS_USB_XHCI_OK)
        return -9;
    return PROS_USB_XHCI_OK;
}

int pros_xhci_interrupter_program_selftest(void)
{
    static uint64_t mmio[128] __attribute__((aligned(64)));
    static struct pros_xhci_erst_entry table[1] __attribute__((aligned(64)));
    static struct pros_xhci_trb event_trbs[8] __attribute__((aligned(64)));
    struct pros_usb_dma_buffer table_dma = {
        0x500000u, table, sizeof(table), sizeof(table), 1u, 5u
    };
    struct pros_usb_dma_buffer event_dma = {
        0x600000u, event_trbs, sizeof(event_trbs), sizeof(event_trbs), 1u, 6u
    };
    struct pros_xhci_erst erst;
    struct pros_xhci_event_ring events;
    struct pros_xhci_controller controller = {0};
    volatile uint8_t *bytes = (volatile uint8_t *)mmio;
    volatile uint32_t *iman = (volatile uint32_t *)(volatile void *)(bytes + 0x140u);
    volatile uint32_t *imod = (volatile uint32_t *)(volatile void *)(bytes + 0x144u);
    volatile uint32_t *erstsz = (volatile uint32_t *)(volatile void *)(bytes + 0x148u);
    volatile uint64_t *erstba = (volatile uint64_t *)(volatile void *)(bytes + 0x150u);
    volatile uint64_t *erdp = (volatile uint64_t *)(volatile void *)(bytes + 0x158u);

    controller.initialized = 1u;
    controller.reset_complete = 1u;
    controller.bar.virtual_address = (uint64_t)(uintptr_t)bytes;
    controller.bar.size = sizeof(mmio);
    controller.capabilities.runtime_offset = 0x100u;
    controller.capabilities.max_interrupters = 2u;
    if (pros_xhci_event_ring_init(&events, &event_dma, 8u) !=
            PROS_XHCI_RING_OK ||
        pros_xhci_erst_init(&erst, &table_dma, &event_dma, 8u) !=
            PROS_XHCI_RING_OK)
        return -1;
    *iman = 0x2u; /* IE set before setup */
    if (pros_xhci_program_interrupter_disabled(&controller, 1u, &erst,
                                                &events, 0x1234u) !=
            PROS_USB_XHCI_OK ||
        (*iman & 0x2u) != 0u || *imod != 0x1234u ||
        *erstsz != 1u || *erstba != table_dma.physical_address ||
        *erdp != event_dma.physical_address)
        return -2;
    if (pros_xhci_program_interrupter_disabled(&controller, 2u, &erst,
                                                &events, 0u) !=
            PROS_USB_XHCI_EBOUNDS)
        return -3;
    controller.reset_complete = 0u;
    if (pros_xhci_program_interrupter_disabled(&controller, 0u, &erst,
                                                &events, 0u) !=
            PROS_USB_XHCI_EINVAL)
        return -4;
    controller.reset_complete = 1u;
    {
        struct pros_xhci_erst bad = erst;
        bad.entries = 0;
        if (pros_xhci_program_interrupter_disabled(&controller, 0u, &bad,
                                                    &events, 0u) !=
            PROS_USB_XHCI_EINVAL)
            return -5;
    }
    {
        struct pros_xhci_event_ring bad = events;
        bad.physical_address += 16u;
        if (pros_xhci_program_interrupter_disabled(&controller, 0u, &erst,
                                                    &bad, 0u) !=
            PROS_USB_XHCI_EINVAL)
            return -6;
    }
    {
        struct pros_xhci_controller bad = controller;
        bad.bar.virtual_address = UINT64_MAX - 0x100u;
        if (pros_xhci_program_interrupter_disabled(&bad, 0u, &erst,
                                                    &events, 0u) !=
            PROS_USB_XHCI_EBOUNDS)
            return -7;
    }
    return PROS_USB_XHCI_OK;
}

int pros_xhci_runtime_access_selftest(void) {
    static uint64_t mmio[128] __attribute__((aligned(8)));
    struct pros_xhci_controller controller = {0};
    uint32_t value32 = 0u;
    uint64_t value64 = 0u;
    volatile uint8_t *bytes = (volatile uint8_t *)mmio;

    controller.initialized = 1u;
    controller.reset_complete = 1u;
    controller.bar.virtual_address = (uint64_t)(uintptr_t)bytes;
    controller.bar.size = sizeof(mmio);
    controller.capabilities.runtime_offset = 0x100u;
    controller.capabilities.max_interrupters = 2u;

    xhci_write32(bytes, 0x100u, 0x12345678u); /* MFINDEX */
    xhci_write32(bytes, 0x120u, 0xA5A50001u); /* IMAN, interrupter 0 */
    xhci_write32(bytes, 0x124u, 0x00001234u); /* IMOD, interrupter 0 */
    xhci_write32(bytes, 0x128u, 0x00000002u); /* ERSTSZ, interrupter 0 */
    *(volatile uint64_t *)(volatile void *)(bytes + 0x130u) =
        0x1122334455667788ull; /* ERSTBA */
    *(volatile uint64_t *)(volatile void *)(bytes + 0x138u) =
        0x8877665544332210ull; /* ERDP */
    xhci_write32(bytes, 0x144u, 0xCAFEBABEu); /* IMOD, interrupter 1 */

    if (pros_xhci_read_mfindex(&controller, &value32) != PROS_USB_XHCI_OK ||
        value32 != 0x12345678u)
        return -1;
    if (pros_xhci_read_interrupter32(&controller, 0u, 0x00u, &value32) !=
            PROS_USB_XHCI_OK || value32 != 0xA5A50001u ||
        pros_xhci_read_interrupter32(&controller, 0u, 0x04u, &value32) !=
            PROS_USB_XHCI_OK || value32 != 0x1234u ||
        pros_xhci_read_interrupter32(&controller, 0u, 0x08u, &value32) !=
            PROS_USB_XHCI_OK || value32 != 2u ||
        pros_xhci_read_interrupter32(&controller, 1u, 0x04u, &value32) !=
            PROS_USB_XHCI_OK || value32 != 0xCAFEBABEu)
        return -2;
    if (pros_xhci_read_interrupter64(&controller, 0u, 0x10u, &value64) !=
            PROS_USB_XHCI_OK || value64 != 0x1122334455667788ull ||
        pros_xhci_read_interrupter64(&controller, 0u, 0x18u, &value64) !=
            PROS_USB_XHCI_OK || value64 != 0x8877665544332210ull)
        return -3;
    if (pros_xhci_read_interrupter32(&controller, 2u, 0x00u, &value32) !=
            PROS_USB_XHCI_EBOUNDS ||
        pros_xhci_read_interrupter32(&controller, 0u, 0x10u, &value32) !=
            PROS_USB_XHCI_EINVAL ||
        pros_xhci_read_interrupter64(&controller, 0u, 0x08u, &value64) !=
            PROS_USB_XHCI_EINVAL ||
        pros_xhci_read_mfindex(&controller, 0) != PROS_USB_XHCI_EINVAL)
        return -4;
    controller.reset_complete = 0u;
    if (pros_xhci_read_mfindex(&controller, &value32) != PROS_USB_XHCI_EINVAL)
        return -5;
    controller.reset_complete = 1u;
    controller.capabilities.runtime_offset = 0x3E0u;
    if (pros_xhci_read_mfindex(&controller, &value32) !=
        PROS_USB_XHCI_EBOUNDS)
        return -6;
    return PROS_USB_XHCI_OK;
}


#define XHCI_PORTSC_BASE 0x400u
#define XHCI_PORT_REGISTER_STRIDE 0x10u
#define XHCI_PORTSC_CCS (1u << 0)
#define XHCI_PORTSC_PR (1u << 4)
#define XHCI_PORTSC_WPR (1u << 31)
#define XHCI_PORTSC_SPEED_MASK (0xFu << 10)
#define XHCI_PORTSC_PRC (1u << 21)
#define XHCI_PORTSC_WRC (1u << 19)
#define XHCI_PORTSC_PP (1u << 9)
#define XHCI_PORTSC_PIC_MASK (3u << 14)
#define XHCI_PORTSC_WAKE_MASK (7u << 25)
#define XHCI_PORTSC_PLS_MASK (0xFu << 5)
#define XHCI_PORTSC_CHANGE_MASK (0x7Fu << 17)

static int xhci_port_reset_sequence(
    void *context, xhci_register_read_fn read_register,
    xhci_register_write_fn write_register, uint32_t portsc_offset,
    uint32_t max_polls, uint32_t *final_status)
{
    uint32_t value, speed, write_value, poll, reset_bit, change_bit;
    if (!context || !read_register || !write_register ||
        max_polls == 0u || (portsc_offset & 3u) != 0u)
        return PROS_USB_XHCI_EINVAL;

    value = read_register(context, portsc_offset);
    if ((value & XHCI_PORTSC_CCS) == 0u)
        return PROS_USB_XHCI_ENODEV;
    speed = (value & XHCI_PORTSC_SPEED_MASK) >> 10;
    if (speed == 0u)
        return PROS_USB_XHCI_EIO;
    /* USB 3.x requires Warm Port Reset (WPR); USB 2.x uses PR. */
    reset_bit = speed >= 4u ? XHCI_PORTSC_WPR : XHCI_PORTSC_PR;
    change_bit = speed >= 4u ? XHCI_PORTSC_WRC : XHCI_PORTSC_PRC;
    if ((value & (XHCI_PORTSC_PR | XHCI_PORTSC_WPR)) != 0u)
        return PROS_USB_XHCI_EIO;

    /* Preserve only defined RW controls; never replay RW1C change bits. */
    write_value = value & (XHCI_PORTSC_PP | XHCI_PORTSC_PLS_MASK |
                           XHCI_PORTSC_PIC_MASK | XHCI_PORTSC_WAKE_MASK);
    write_register(context, portsc_offset, write_value | reset_bit);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    for (poll = 0u; poll < max_polls; ++poll) {
        value = read_register(context, portsc_offset);
        if ((value & reset_bit) == 0u &&
            (value & change_bit) != 0u) {
            /* Acknowledge only this reset-change bit; preserve other RW1C status. */
            write_value = value & (XHCI_PORTSC_PP | XHCI_PORTSC_PLS_MASK |
                                   XHCI_PORTSC_PIC_MASK | XHCI_PORTSC_WAKE_MASK);
            write_register(context, portsc_offset,
                           write_value | change_bit);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            value = read_register(context, portsc_offset);
            if ((value & reset_bit) != 0u ||
                (value & change_bit) != 0u)
                return PROS_USB_XHCI_EIO;
            if (final_status)
                *final_status = value;
            return PROS_USB_XHCI_OK;
        }
        __asm__ volatile ("pause" ::: "memory");
    }
    return PROS_USB_XHCI_ETIMEOUT;
}

int pros_xhci_port_reset(struct pros_xhci_controller *controller,
                         uint8_t port, uint32_t max_polls,
                         uint32_t *final_status)
{
    uint64_t offset;
    uint32_t pci_command, command, status;
    volatile uint8_t *bar;
    int rc;
    if (!controller || !controller->initialized ||
        !controller->reset_complete || !controller->bar.virtual_address ||
        port == 0u || port > controller->capabilities.max_ports ||
        max_polls == 0u)
        return PROS_USB_XHCI_EINVAL;
    if ((controller->bar.virtual_address & 3u) != 0u ||
        (controller->capabilities.operational_offset & 3u) != 0u)
        return PROS_USB_XHCI_EBOUNDS;

    rc = xhci_read_pci_command(&controller->pci, &pci_command);
    if (rc != PROS_PCI_OK || (pci_command & (1u << 2)) == 0u)
        return PROS_USB_XHCI_EIO;
    offset = (uint64_t)controller->capabilities.operational_offset +
             XHCI_PORTSC_BASE +
             (uint64_t)(port - 1u) * XHCI_PORT_REGISTER_STRIDE;
    if (!xhci_range_valid(offset, sizeof(uint32_t), controller->bar.size))
        return PROS_USB_XHCI_EBOUNDS;
    bar = (volatile uint8_t *)(uintptr_t)controller->bar.virtual_address;
    command = xhci_read32(bar, controller->capabilities.operational_offset);
    status = xhci_read32(bar,
                         controller->capabilities.operational_offset + 4u);
    if ((command & XHCI_USBCMD_RUN) == 0u ||
        (status & XHCI_USBSTS_HCHALTED) != 0u ||
        (status & XHCI_USBSTS_HCE) != 0u)
        return PROS_USB_XHCI_EIO;
    return xhci_port_reset_sequence((void *)bar, xhci_mmio_context_read,
        xhci_mmio_context_write, (uint32_t)offset, max_polls, final_status);
}

struct xhci_port_reset_model {
    uint32_t status;
    uint32_t writes;
    uint8_t auto_complete;
};
static uint32_t xhci_port_reset_model_read(void *context, uint32_t offset)
{
    struct xhci_port_reset_model *model =
        (struct xhci_port_reset_model *)context;
    return offset == 0x400u ? model->status : 0u;
}
static void xhci_port_reset_model_write(void *context, uint32_t offset,
                                        uint32_t value)
{
    struct xhci_port_reset_model *model =
        (struct xhci_port_reset_model *)context;
    if (offset != 0x400u)
        return;
    ++model->writes;
    /* Emulate RW1C semantics for port-change bits. */
    model->status &= ~(value & XHCI_PORTSC_CHANGE_MASK);
    if ((value & XHCI_PORTSC_PR) != 0u && model->auto_complete) {
        model->status &= ~XHCI_PORTSC_PR;
        model->status |= XHCI_PORTSC_PRC;
    }
    if ((value & XHCI_PORTSC_WPR) != 0u && model->auto_complete) {
        model->status &= ~XHCI_PORTSC_WPR;
        model->status |= XHCI_PORTSC_WRC;
    }
}
int pros_xhci_port_reset_selftest(void)
{
    struct xhci_port_reset_model model = {
        XHCI_PORTSC_CCS | (3u << 10) | XHCI_PORTSC_PP |
        (1u << 17) | (1u << 18), 0u, 1u
    };
    uint32_t final_status = 0u;
    if (xhci_port_reset_sequence(&model, xhci_port_reset_model_read,
            xhci_port_reset_model_write, 0x400u, 4u,
            &final_status) != PROS_USB_XHCI_OK ||
        model.writes != 2u ||
        (final_status & (XHCI_PORTSC_PR | XHCI_PORTSC_PRC)) != 0u ||
        (final_status & (1u << 17)) == 0u ||
        (final_status & (1u << 18)) == 0u)
        return -1;
    model.status = XHCI_PORTSC_CCS | (4u << 10) | (1u << 17) | (1u << 18);
    model.writes = 0u;
    if (xhci_port_reset_sequence(&model, xhci_port_reset_model_read,
            xhci_port_reset_model_write, 0x400u, 4u,
            &final_status) != PROS_USB_XHCI_OK ||
        model.writes != 2u ||
        (final_status & (XHCI_PORTSC_WPR | XHCI_PORTSC_WRC)) != 0u ||
        (final_status & (1u << 17)) == 0u ||
        (final_status & (1u << 18)) == 0u)
        return -2;
    model.status = (3u << 10);
    model.writes = 0u;
    if (xhci_port_reset_sequence(&model, xhci_port_reset_model_read,
            xhci_port_reset_model_write, 0x400u, 4u, 0) !=
            PROS_USB_XHCI_ENODEV || model.writes != 0u)
        return -3;
    model.status = XHCI_PORTSC_CCS | (2u << 10);
    model.auto_complete = 0u;
    if (xhci_port_reset_sequence(&model, xhci_port_reset_model_read,
            xhci_port_reset_model_write, 0x400u, 2u, 0) !=
            PROS_USB_XHCI_ETIMEOUT)
        return -4;
    if (xhci_port_reset_sequence(&model, xhci_port_reset_model_read,
            xhci_port_reset_model_write, 0x400u, 0u, 0) !=
            PROS_USB_XHCI_EINVAL)
        return -5;
    return PROS_USB_XHCI_OK;
}


#define XHCI_TRB_TYPE_ENABLE_SLOT 9u
#define XHCI_TRB_TYPE_PORT_STATUS_CHANGE 34u
#define XHCI_TRB_SLOT_ID_SHIFT 24u
#define XHCI_TRB_PORT_ID_SHIFT 24u

static int xhci_record_port_status_change(
    struct pros_xhci_dma_resources *resources,
    const struct pros_xhci_controller *controller,
    const struct pros_xhci_trb *event)
{
    uint8_t type, port_id;
    if (!resources || !controller || !event ||
        pros_xhci_event_type(event, &type) != PROS_XHCI_RING_OK ||
        type != XHCI_TRB_TYPE_PORT_STATUS_CHANGE)
        return PROS_USB_XHCI_EINVAL;
    port_id = (uint8_t)(event->parameter >> XHCI_TRB_PORT_ID_SHIFT);
    if (port_id == 0u || port_id > controller->capabilities.max_ports)
        return PROS_USB_XHCI_EBOUNDS;
    resources->pending_port_changes[port_id / 32u] |=
        1u << (port_id % 32u);
    return PROS_USB_XHCI_OK;
}


#define XHCI_TRB_TYPE_ADDRESS_DEVICE 11u

static uint16_t xhci_initial_ep0_packet_size(uint8_t speed)
{
    if (speed == 1u || speed == 2u)
        return 8u;
    if (speed == 3u)
        return 64u;
    if (speed == 4u || speed == 5u)
        return 512u;
    return 0u;
}

static int xhci_build_address_device_contexts(
    void *input_context, uint64_t input_size,
    void *output_context, uint64_t output_size,
    uint32_t context_size, uint8_t root_port, uint8_t speed,
    uint16_t ep0_packet_size, uint64_t ep0_ring_address)
{
    uint64_t required_input, required_output;
    uint8_t *input = (uint8_t *)input_context;
    uint8_t *output = (uint8_t *)output_context;
    uint32_t *control, *slot, *ep0;
    if (!input || !output || (context_size != 32u && context_size != 64u) ||
        root_port == 0u || speed == 0u ||
        xhci_initial_ep0_packet_size(speed) == 0u ||
        xhci_initial_ep0_packet_size(speed) != ep0_packet_size ||
        ep0_ring_address == 0u || (ep0_ring_address & 63u) != 0u ||
        ((uintptr_t)input & 63u) != 0u ||
        ((uintptr_t)output & 63u) != 0u)
        return PROS_USB_XHCI_EINVAL;
    required_input = (uint64_t)context_size * 33u;
    required_output = (uint64_t)context_size * 32u;
    if (input_size < required_input || output_size < required_output)
        return PROS_USB_XHCI_EBOUNDS;

    for (uint64_t i = 0u; i < required_input; ++i)
        input[i] = 0u;
    for (uint64_t i = 0u; i < required_output; ++i)
        output[i] = 0u;

    control = (uint32_t *)(void *)input;
    slot = (uint32_t *)(void *)(input + context_size);
    ep0 = (uint32_t *)(void *)(input + (uint64_t)context_size * 2u);

    /* Add Slot Context (bit 0) and EP0 Context (bit 1). */
    control[1] = 3u;
    slot[0] = ((uint32_t)speed << 20) | (1u << 27);
    slot[1] = (uint32_t)root_port << 16;
    ep0[1] = (3u << 1) | (4u << 3) |
             ((uint32_t)ep0_packet_size << 16);
    ep0[2] = (uint32_t)ep0_ring_address | 1u; /* DCS = 1 */
    ep0[3] = (uint32_t)(ep0_ring_address >> 32);
    ep0[4] = 8u; /* Average TRB length for the default control endpoint. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return PROS_USB_XHCI_OK;
}

int pros_xhci_address_context_selftest(void)
{
    static uint8_t input[33u * 64u] __attribute__((aligned(64)));
    static uint8_t output[32u * 64u] __attribute__((aligned(64)));
    uint32_t *control, *slot, *ep0;
    if (xhci_build_address_device_contexts(input, sizeof(input),
            output, sizeof(output), 64u, 7u, 5u, 512u,
            0x12345000u) != PROS_USB_XHCI_OK)
        return -1;
    control = (uint32_t *)(void *)input;
    slot = (uint32_t *)(void *)(input + 64u);
    ep0 = (uint32_t *)(void *)(input + 128u);
    if (control[1] != 3u ||
        slot[0] != ((5u << 20) | (1u << 27)) ||
        slot[1] != (7u << 16) ||
        ep0[1] != ((3u << 1) | (4u << 3) | (512u << 16)) ||
        ep0[2] != 0x12345001u || ep0[3] != 0u || ep0[4] != 8u)
        return -2;
    if (xhci_build_address_device_contexts(input, sizeof(input),
            output, sizeof(output), 32u, 2u, 3u, 64u,
            0x200000u) != PROS_USB_XHCI_OK)
        return -3;
    control = (uint32_t *)(void *)input;
    slot = (uint32_t *)(void *)(input + 32u);
    ep0 = (uint32_t *)(void *)(input + 64u);
    if (control[1] != 3u ||
        slot[0] != ((3u << 20) | (1u << 27)) ||
        slot[1] != (2u << 16) ||
        ep0[1] != ((3u << 1) | (4u << 3) | (64u << 16)) ||
        ep0[2] != 0x200001u)
        return -4;
    if (xhci_build_address_device_contexts(input, sizeof(input),
            output, sizeof(output), 48u, 2u, 3u, 64u,
            0x200000u) != PROS_USB_XHCI_EINVAL ||
        xhci_build_address_device_contexts(input, 64u,
            output, sizeof(output), 64u, 2u, 3u, 64u,
            0x200000u) != PROS_USB_XHCI_EBOUNDS ||
        xhci_build_address_device_contexts(input, sizeof(input),
            output, sizeof(output), 64u, 2u, 3u, 512u,
            0x200000u) != PROS_USB_XHCI_EINVAL)
        return -5;
    return PROS_USB_XHCI_OK;
}

/*
 * Address the enabled slot and install its output context and default control
 * endpoint ring. The DMA objects remain pinned once DCBAA is published.
 */
int pros_xhci_address_device(struct pros_xhci_controller *controller,
                             struct pros_xhci_dma_resources *resources,
                             uint16_t interrupter, uint8_t slot_id,
                             uint8_t root_port, uint8_t speed,
                             uint32_t completion_polls)
{
    struct pros_xhci_device_slot *slot;
    uint64_t op, doorbell, iman, expected_command, event_address;
    uint64_t context_size, input_bytes, output_bytes, dcbaa_bytes;
    volatile uint8_t *bar;
    volatile uint64_t *dcbaa;
    uint32_t command, status, poll;
    uint8_t event_type, completion_code, returned_slot;
    uint16_t ep0_packet_size;
    struct pros_xhci_trb event;
    int rc;

    if (!controller || !resources || !controller->initialized ||
        !controller->reset_complete || !controller->bar.virtual_address ||
        !resources->initialized || !resources->hardware_referenced ||
        !resources->command_ring.initialized ||
        !resources->event_ring.initialized || !resources->erst.initialized ||
        resources->command_ring.outstanding_count != 0u ||
        interrupter >= controller->capabilities.max_interrupters ||
        completion_polls == 0u || slot_id == 0u ||
        slot_id > controller->capabilities.max_slots ||
        root_port == 0u || root_port > controller->capabilities.max_ports ||
        resources->device_slot.initialized)
        return PROS_USB_XHCI_EINVAL;

    ep0_packet_size = xhci_initial_ep0_packet_size(speed);
    if (ep0_packet_size == 0u)
        return PROS_USB_XHCI_EUNSUPPORTED;
    context_size = controller->capabilities.context_size_64 ? 64u : 32u;
    input_bytes = context_size * 33u;
    output_bytes = context_size * 32u;
    dcbaa_bytes = ((uint64_t)controller->capabilities.max_slots + 1u) *
                  sizeof(uint64_t);
    if (!resources->dcbaa.virtual_address ||
        resources->dcbaa.size < dcbaa_bytes ||
        resources->dcbaa.allocation_size < dcbaa_bytes)
        return PROS_USB_XHCI_EBOUNDS;

    slot = &resources->device_slot;
    rc = pros_usb_dma_alloc(output_bytes, PROSENJIT_PAGE_SIZE,
                            &slot->output_context);
    if (rc != PROS_USB_DMA_OK)
        goto alloc_failed;
    rc = pros_usb_dma_alloc(input_bytes, PROSENJIT_PAGE_SIZE,
                            &slot->input_context);
    if (rc != PROS_USB_DMA_OK)
        goto alloc_failed;
    rc = pros_usb_dma_alloc(256u * PROS_XHCI_TRB_SIZE,
                            PROSENJIT_PAGE_SIZE,
                            &slot->ep0_ring_dma);
    if (rc != PROS_USB_DMA_OK)
        goto alloc_failed;
    rc = pros_usb_dma_alloc(4096u, PROSENJIT_PAGE_SIZE,
                            &slot->control_data);
    if (rc != PROS_USB_DMA_OK)
        goto alloc_failed;
    rc = pros_xhci_transfer_ring_init(&slot->ep0_ring,
                                      &slot->ep0_ring_dma, 256u);
    if (rc != PROS_XHCI_RING_OK)
        goto alloc_failed;
    rc = xhci_build_address_device_contexts(
        slot->input_context.virtual_address, slot->input_context.size,
        slot->output_context.virtual_address, slot->output_context.size,
        (uint32_t)context_size, root_port, speed, ep0_packet_size,
        slot->ep0_ring.physical_address);
    if (rc != PROS_USB_XHCI_OK)
        goto alloc_failed;

    op = controller->capabilities.operational_offset;
    doorbell = controller->capabilities.doorbell_offset;
    iman = (uint64_t)controller->capabilities.runtime_offset + 0x20u +
           (uint64_t)interrupter * 0x20u;
    if (!xhci_range_valid(op, 0x40u, controller->bar.size) ||
        !xhci_range_valid(doorbell, 4u, controller->bar.size) ||
        !xhci_range_valid(iman + 0x18u, 8u, controller->bar.size) ||
        (op & 3u) != 0u || (doorbell & 3u) != 0u ||
        (iman & 7u) != 0u || (controller->bar.virtual_address & 7u) != 0u) {
        rc = PROS_USB_XHCI_EBOUNDS;
        goto alloc_failed;
    }
    bar = (volatile uint8_t *)(uintptr_t)controller->bar.virtual_address;
    command = xhci_read32(bar, (uint32_t)op);
    status = xhci_read32(bar, (uint32_t)(op + 4u));
    if ((command & XHCI_USBCMD_RUN) == 0u ||
        (status & (XHCI_USBSTS_HCHALTED | XHCI_USBSTS_HCE)) != 0u ||
        xhci_read32(bar, (uint32_t)(iman + 0x08u)) !=
            resources->erst.entry_count) {
        rc = PROS_USB_XHCI_EIO;
        goto alloc_failed;
    }
    rc = xhci_read_pci_command(&controller->pci, &status);
    if (rc != PROS_PCI_OK || (status & (1u << 2)) == 0u) {
        rc = PROS_USB_XHCI_EIO;
        goto alloc_failed;
    }

    /* Publish the output context before Address Device can reference it. */
    dcbaa = (volatile uint64_t *)(volatile void *)
        resources->dcbaa.virtual_address;
    if (dcbaa[slot_id] != 0u) {
        rc = PROS_USB_XHCI_EIO;
        goto alloc_failed;
    }
    dcbaa[slot_id] = slot->output_context.physical_address;
    __atomic_thread_fence(__ATOMIC_RELEASE);

    slot->slot_id = slot_id;
    slot->root_port = root_port;
    slot->speed = speed;
    slot->ep0_max_packet_size = ep0_packet_size;
    slot->initialized = 1u;

    rc = pros_xhci_command_ring_enqueue(&resources->command_ring,
        slot->input_context.physical_address, 0u,
        (XHCI_TRB_TYPE_ADDRESS_DEVICE << PROS_XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)slot_id << XHCI_TRB_SLOT_ID_SHIFT),
        &expected_command);
    if (rc != PROS_XHCI_RING_OK) {
        dcbaa[slot_id] = 0u;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        slot->initialized = 0u;
        rc = PROS_USB_XHCI_EIO;
        goto alloc_failed;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    xhci_write32(bar, (uint32_t)doorbell, 0u);

    for (poll = 0u; poll < completion_polls; ++poll) {
        rc = pros_xhci_event_ring_dequeue(&resources->event_ring, &event,
                                          &event_address);
        if (rc == PROS_XHCI_RING_EEMPTY) {
            __asm__ volatile ("pause" ::: "memory");
            continue;
        }
        if (rc != PROS_XHCI_RING_OK)
            return PROS_USB_XHCI_EIO;
        if (pros_xhci_event_type(&event, &event_type) != PROS_XHCI_RING_OK)
            return PROS_USB_XHCI_EIO;
        if (event_type == XHCI_TRB_TYPE_PORT_STATUS_CHANGE) {
            rc = xhci_record_port_status_change(resources, controller, &event);
            if (pros_xhci_acknowledge_event_ring(controller, interrupter,
                    &resources->erst, &resources->event_ring) !=
                    PROS_USB_XHCI_OK)
                return PROS_USB_XHCI_EIO;
            if (rc != PROS_USB_XHCI_OK)
                return rc;
            continue;
        }
        if (event_type != 33u) {
            (void)pros_xhci_acknowledge_event_ring(controller, interrupter,
                    &resources->erst, &resources->event_ring);
            return PROS_USB_XHCI_EUNSUPPORTED;
        }
        rc = pros_xhci_command_ring_handle_completion(
            &resources->command_ring, &event, &completion_code);
        if (pros_xhci_acknowledge_event_ring(controller, interrupter,
                &resources->erst, &resources->event_ring) != PROS_USB_XHCI_OK)
            return PROS_USB_XHCI_EIO;
        if (rc != PROS_XHCI_RING_OK || event.parameter != expected_command)
            return PROS_USB_XHCI_EIO;
        returned_slot = (uint8_t)(event.control >> XHCI_TRB_SLOT_ID_SHIFT);
        if (returned_slot != slot_id)
            return PROS_USB_XHCI_EIO;
        if (completion_code != 1u)
            return PROS_USB_XHCI_EIO;
        slot->addressed = 1u;
        return PROS_USB_XHCI_OK;
    }
    /* The command may still be consuming these DMA contexts; keep them pinned. */
    return PROS_USB_XHCI_ECOMPLETION;

alloc_failed:
    if (!slot->initialized) {
        if (slot->control_data.physical_address)
            (void)pros_usb_dma_free(&slot->control_data);
        if (slot->ep0_ring_dma.physical_address)
            (void)pros_usb_dma_free(&slot->ep0_ring_dma);
        if (slot->input_context.physical_address)
            (void)pros_usb_dma_free(&slot->input_context);
        if (slot->output_context.physical_address)
            (void)pros_usb_dma_free(&slot->output_context);
        slot->ep0_ring.initialized = 0u;
        slot->slot_id = 0u;
        slot->root_port = 0u;
        slot->speed = 0u;
        slot->ep0_max_packet_size = 0u;
    }
    return rc;
}


/*
 * Fetch the first eight bytes of the USB device descriptor through EP0. This
 * proves Setup/Data/Status TD execution after Address Device, without yet
 * enabling bulk endpoints or binding MSC.
 */

static int xhci_ep0_control_transfer(
    struct pros_xhci_controller *controller,
    struct pros_xhci_dma_resources *resources,
    uint16_t interrupter, const uint8_t setup[8],
    uint32_t data_length, uint8_t data_in, uint32_t completion_polls)
{
    struct pros_xhci_device_slot *slot;
    uint64_t doorbell_offset, first_trb, last_trb;
    volatile uint8_t *bar;
    uint32_t command, status, poll;
    uint8_t event_type, completion_code;
    struct pros_xhci_trb event;
    int rc;

    if (!controller || !resources || !setup ||
        !controller->initialized || !controller->reset_complete ||
        !controller->bar.virtual_address || !resources->initialized ||
        !resources->hardware_referenced ||
        !resources->device_slot.initialized ||
        !resources->device_slot.addressed ||
        !resources->device_slot.control_data.virtual_address ||
        !resources->device_slot.ep0_ring.initialized ||
        resources->device_slot.ep0_ring.outstanding_count != 0u ||
        !resources->event_ring.initialized || !resources->erst.initialized ||
        interrupter >= controller->capabilities.max_interrupters ||
        completion_polls == 0u || data_in > 1u ||
        data_length > resources->device_slot.control_data.size)
        return PROS_USB_XHCI_EINVAL;
    slot = &resources->device_slot;
    doorbell_offset = (uint64_t)controller->capabilities.doorbell_offset +
                      (uint64_t)slot->slot_id * sizeof(uint32_t);
    if (!xhci_range_valid(doorbell_offset, sizeof(uint32_t),
                          controller->bar.size) ||
        (doorbell_offset & 3u) != 0u ||
        (controller->bar.virtual_address & 7u) != 0u)
        return PROS_USB_XHCI_EBOUNDS;
    bar = (volatile uint8_t *)(uintptr_t)controller->bar.virtual_address;
    command = xhci_read32(bar, controller->capabilities.operational_offset);
    status = xhci_read32(bar,
                         controller->capabilities.operational_offset + 4u);
    if ((command & XHCI_USBCMD_RUN) == 0u ||
        (status & (XHCI_USBSTS_HCHALTED | XHCI_USBSTS_HCE)) != 0u)
        return PROS_USB_XHCI_EIO;
    rc = xhci_read_pci_command(&controller->pci, &status);
    if (rc != PROS_PCI_OK || (status & (1u << 2)) == 0u)
        return PROS_USB_XHCI_EIO;

    rc = pros_xhci_transfer_ring_enqueue_control(&slot->ep0_ring, setup,
        data_length ? &slot->control_data : 0, data_length, data_in,
        &first_trb, &last_trb);
    if (rc != PROS_XHCI_RING_OK)
        return PROS_USB_XHCI_EIO;
    (void)first_trb;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    /* Doorbell index is Slot ID; target endpoint DCI 1 is EP0. */
    xhci_write32(bar, (uint32_t)doorbell_offset, 1u);

    for (poll = 0u; poll < completion_polls; ++poll) {
        uint64_t event_address;
        rc = pros_xhci_event_ring_dequeue(&resources->event_ring, &event,
                                          &event_address);
        if (rc == PROS_XHCI_RING_EEMPTY) {
            __asm__ volatile ("pause" ::: "memory");
            continue;
        }
        if (rc != PROS_XHCI_RING_OK)
            return PROS_USB_XHCI_EIO;
        if (pros_xhci_event_type(&event, &event_type) != PROS_XHCI_RING_OK)
            return PROS_USB_XHCI_EIO;
        if (event_type == XHCI_TRB_TYPE_PORT_STATUS_CHANGE) {
            rc = xhci_record_port_status_change(resources, controller, &event);
            if (pros_xhci_acknowledge_event_ring(controller, interrupter,
                    &resources->erst, &resources->event_ring) !=
                    PROS_USB_XHCI_OK)
                return PROS_USB_XHCI_EIO;
            if (rc != PROS_USB_XHCI_OK)
                return rc;
            continue;
        }
        if (event_type != 32u) {
            (void)pros_xhci_acknowledge_event_ring(controller, interrupter,
                    &resources->erst, &resources->event_ring);
            return PROS_USB_XHCI_EUNSUPPORTED;
        }
        rc = pros_xhci_transfer_ring_handle_td_event(&slot->ep0_ring,
            &event, slot->slot_id, 1u, &completion_code);
        if (pros_xhci_acknowledge_event_ring(controller, interrupter,
                &resources->erst, &resources->event_ring) != PROS_USB_XHCI_OK)
            return PROS_USB_XHCI_EIO;
        if (rc != PROS_XHCI_RING_OK || event.parameter != last_trb)
            return PROS_USB_XHCI_EIO;
        if (completion_code != 1u || (event.status & 0x00FFFFFFu) != 0u)
            return PROS_USB_XHCI_EIO;
        return PROS_USB_XHCI_OK;
    }
    return PROS_USB_XHCI_ECOMPLETION;
}

/* Fetch and validate both the descriptor prefix and the complete 18-byte device descriptor. */
int pros_xhci_get_device_descriptor(
    struct pros_xhci_controller *controller,
    struct pros_xhci_dma_resources *resources,
    uint16_t interrupter, uint32_t completion_polls)
{
    static const uint8_t get_descriptor_prefix[8] =
        {0x80u, 6u, 0u, 1u, 0u, 0u, 8u, 0u};
    static const uint8_t get_full_device_descriptor[8] =
        {0x80u, 6u, 0u, 1u, 0u, 0u, 18u, 0u};
    struct pros_xhci_device_slot *slot;
    const uint8_t *descriptor;
    uint8_t packet;
    int rc;
    if (!resources || !resources->device_slot.initialized ||
        !resources->device_slot.addressed)
        return PROS_USB_XHCI_EINVAL;
    slot = &resources->device_slot;
    slot->device_descriptor_valid = 0u;

    rc = xhci_ep0_control_transfer(controller, resources, interrupter,
        get_descriptor_prefix, 8u, 1u, completion_polls);
    if (rc != PROS_USB_XHCI_OK)
        return rc;
    descriptor = (const uint8_t *)slot->control_data.virtual_address;
    if (descriptor[0] < 18u || descriptor[1] != 1u)
        return PROS_USB_XHCI_EIO;
    packet = descriptor[7];
    if ((slot->speed >= 4u && packet != 9u) ||
        (slot->speed == 3u && packet != 64u) ||
        (slot->speed == 2u && packet != 8u) ||
        (slot->speed == 1u && packet != slot->ep0_max_packet_size))
        return PROS_USB_XHCI_EUNSUPPORTED;

    rc = xhci_ep0_control_transfer(controller, resources, interrupter,
        get_full_device_descriptor, 18u, 1u, completion_polls);
    if (rc != PROS_USB_XHCI_OK)
        return rc;
    descriptor = (const uint8_t *)slot->control_data.virtual_address;
    if (descriptor[0] < 18u || descriptor[1] != 1u ||
        descriptor[7] != packet || descriptor[17] == 0u)
        return PROS_USB_XHCI_EIO;

    slot->vendor_id = (uint16_t)descriptor[8] |
                      ((uint16_t)descriptor[9] << 8);
    slot->product_id = (uint16_t)descriptor[10] |
                       ((uint16_t)descriptor[11] << 8);
    slot->num_configurations = descriptor[17];
    slot->device_descriptor_valid = 1u;
    return PROS_USB_XHCI_OK;
}


/*
 * Read the active configuration descriptor, select one unambiguous
 * SCSI-transparent BOT interface, and issue SET_CONFIGURATION.
 */
int pros_xhci_configure_msc_interface(
    struct pros_xhci_controller *controller,
    struct pros_xhci_dma_resources *resources,
    uint16_t interrupter, uint32_t completion_polls)
{
    static const uint8_t get_config_header[8] =
        {0x80u, 6u, 0u, 2u, 0u, 0u, 9u, 0u};
    struct pros_xhci_device_slot *slot;
    struct pros_usb_msc_interface selected;
    const uint8_t *descriptor;
    uint8_t get_full_config[8] =
        {0x80u, 6u, 0u, 2u, 0u, 0u, 0u, 0u};
    uint8_t set_configuration[8] =
        {0x00u, 9u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t total_length;
    int rc;
    if (!resources || !resources->device_slot.initialized ||
        !resources->device_slot.addressed ||
        !resources->device_slot.device_descriptor_valid ||
        resources->device_slot.msc_interface_valid)
        return PROS_USB_XHCI_EINVAL;
    slot = &resources->device_slot;

    rc = xhci_ep0_control_transfer(controller, resources, interrupter,
        get_config_header, 9u, 1u, completion_polls);
    if (rc != PROS_USB_XHCI_OK)
        return rc;
    descriptor = (const uint8_t *)slot->control_data.virtual_address;
    if (descriptor[0] != 9u || descriptor[1] != 2u)
        return PROS_USB_XHCI_EIO;
    total_length = (uint32_t)descriptor[2] |
                   ((uint32_t)descriptor[3] << 8);
    if (total_length < 9u || total_length > slot->control_data.size)
        return PROS_USB_XHCI_EBOUNDS;

    get_full_config[6] = (uint8_t)total_length;
    get_full_config[7] = (uint8_t)(total_length >> 8);
    rc = xhci_ep0_control_transfer(controller, resources, interrupter,
        get_full_config, total_length, 1u, completion_polls);
    if (rc != PROS_USB_XHCI_OK)
        return rc;
    descriptor = (const uint8_t *)slot->control_data.virtual_address;
    rc = pros_usb_msc_select_interface(descriptor, total_length, &selected);
    if (rc != PROS_USB_DESC_OK)
        return rc == PROS_USB_DESC_ENOTFOUND ||
               rc == PROS_USB_DESC_EUNSUPPORTED ?
               PROS_USB_XHCI_EUNSUPPORTED : PROS_USB_XHCI_EIO;

    set_configuration[2] = selected.configuration_value;
    rc = xhci_ep0_control_transfer(controller, resources, interrupter,
        set_configuration, 0u, 0u, completion_polls);
    if (rc != PROS_USB_XHCI_OK)
        return rc;

    slot->msc_interface_number = selected.interface_number;
    slot->msc_endpoint_in = selected.endpoint_in;
    slot->msc_endpoint_out = selected.endpoint_out;
    slot->msc_configuration_value = selected.configuration_value;
    slot->msc_max_packet_in = selected.max_packet_in;
    slot->msc_max_packet_out = selected.max_packet_out;
    slot->msc_interface_valid = 1u;
    return PROS_USB_XHCI_OK;
}

/*
 * Enable Slot is the first command in device enumeration. Port Status Change
 * Events may be queued ahead of its completion after port reset; preserve those
 * events in a bounded bitmap rather than silently dropping them.
 */
int pros_xhci_enable_slot(struct pros_xhci_controller *controller,
                          struct pros_xhci_dma_resources *resources,
                          uint16_t interrupter, uint32_t completion_polls,
                          uint8_t *slot_id)
{
    uint64_t op, doorbell, iman, expected_command, event_address;
    volatile uint8_t *bar;
    uint32_t command, status, poll;
    uint8_t event_type, completion_code, returned_slot;
    struct pros_xhci_trb event;
    int rc;

    if (!controller || !resources || !slot_id ||
        !controller->initialized || !controller->reset_complete ||
        !controller->bar.virtual_address || !resources->initialized ||
        !resources->hardware_referenced ||
        !resources->command_ring.initialized ||
        !resources->event_ring.initialized || !resources->erst.initialized ||
        resources->command_ring.outstanding_count != 0u ||
        interrupter >= controller->capabilities.max_interrupters ||
        completion_polls == 0u || controller->capabilities.max_slots == 0u)
        return PROS_USB_XHCI_EINVAL;
    *slot_id = 0u;

    op = controller->capabilities.operational_offset;
    doorbell = controller->capabilities.doorbell_offset;
    iman = (uint64_t)controller->capabilities.runtime_offset + 0x20u +
           (uint64_t)interrupter * 0x20u;
    if (!xhci_range_valid(op, 0x40u, controller->bar.size) ||
        !xhci_range_valid(doorbell, 4u, controller->bar.size) ||
        !xhci_range_valid(iman + 0x18u, 8u, controller->bar.size) ||
        (op & 3u) != 0u || (doorbell & 3u) != 0u ||
        (iman & 7u) != 0u || (controller->bar.virtual_address & 7u) != 0u)
        return PROS_USB_XHCI_EBOUNDS;

    bar = (volatile uint8_t *)(uintptr_t)controller->bar.virtual_address;
    command = xhci_read32(bar, (uint32_t)op);
    status = xhci_read32(bar, (uint32_t)(op + 4u));
    if ((command & XHCI_USBCMD_RUN) == 0u ||
        (status & (XHCI_USBSTS_HCHALTED | XHCI_USBSTS_HCE)) != 0u ||
        xhci_read32(bar, (uint32_t)(iman + 0x08u)) !=
            resources->erst.entry_count)
        return PROS_USB_XHCI_EIO;
    rc = xhci_read_pci_command(&controller->pci, &status);
    if (rc != PROS_PCI_OK || (status & (1u << 2)) == 0u)
        return PROS_USB_XHCI_EIO;

    rc = pros_xhci_command_ring_enqueue(&resources->command_ring, 0u, 0u,
        XHCI_TRB_TYPE_ENABLE_SLOT << PROS_XHCI_TRB_TYPE_SHIFT,
        &expected_command);
    if (rc != PROS_XHCI_RING_OK)
        return PROS_USB_XHCI_EIO;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    xhci_write32(bar, (uint32_t)doorbell, 0u);

    for (poll = 0u; poll < completion_polls; ++poll) {
        rc = pros_xhci_event_ring_dequeue(&resources->event_ring, &event,
                                          &event_address);
        if (rc == PROS_XHCI_RING_EEMPTY) {
            __asm__ volatile ("pause" ::: "memory");
            continue;
        }
        if (rc != PROS_XHCI_RING_OK)
            return PROS_USB_XHCI_EIO;
        if (pros_xhci_event_type(&event, &event_type) != PROS_XHCI_RING_OK)
            return PROS_USB_XHCI_EIO;

        if (event_type == XHCI_TRB_TYPE_PORT_STATUS_CHANGE) {
            rc = xhci_record_port_status_change(resources, controller, &event);
            if (pros_xhci_acknowledge_event_ring(controller, interrupter,
                    &resources->erst, &resources->event_ring) !=
                    PROS_USB_XHCI_OK)
                return PROS_USB_XHCI_EIO;
            if (rc != PROS_USB_XHCI_OK)
                return rc;
            continue;
        }
        if (event_type != 33u) {
            (void)pros_xhci_acknowledge_event_ring(controller, interrupter,
                    &resources->erst, &resources->event_ring);
            return PROS_USB_XHCI_EUNSUPPORTED;
        }

        rc = pros_xhci_command_ring_handle_completion(
            &resources->command_ring, &event, &completion_code);
        if (pros_xhci_acknowledge_event_ring(controller, interrupter,
                &resources->erst, &resources->event_ring) != PROS_USB_XHCI_OK)
            return PROS_USB_XHCI_EIO;
        if (rc != PROS_XHCI_RING_OK || event.parameter != expected_command)
            return PROS_USB_XHCI_EIO;
        if (completion_code != 1u)
            return PROS_USB_XHCI_EIO;
        returned_slot = (uint8_t)(event.control >> XHCI_TRB_SLOT_ID_SHIFT);
        if (returned_slot == 0u ||
            returned_slot > controller->capabilities.max_slots)
            return PROS_USB_XHCI_EIO;
        *slot_id = returned_slot;
        return PROS_USB_XHCI_OK;
    }
    /* Command remains outstanding; caller must not submit another command. */
    return PROS_USB_XHCI_ECOMPLETION;
}


int pros_xhci_port_event_selftest(void)
{
    struct pros_xhci_dma_resources resources = {0};
    struct pros_xhci_controller controller = {0};
    struct pros_xhci_trb event = {0};
    controller.capabilities.max_ports = 255u;
    event.parameter = (uint64_t)33u << XHCI_TRB_PORT_ID_SHIFT;
    event.control = XHCI_TRB_TYPE_PORT_STATUS_CHANGE <<
                    PROS_XHCI_TRB_TYPE_SHIFT;
    if (xhci_record_port_status_change(&resources, &controller, &event) !=
            PROS_USB_XHCI_OK ||
        (resources.pending_port_changes[1] & (1u << 1)) == 0u)
        return -1;
    event.parameter = (uint64_t)255u << XHCI_TRB_PORT_ID_SHIFT;
    if (xhci_record_port_status_change(&resources, &controller, &event) !=
            PROS_USB_XHCI_OK ||
        (resources.pending_port_changes[7] & (1u << 31)) == 0u)
        return -2;
    event.parameter = 0u;
    if (xhci_record_port_status_change(&resources, &controller, &event) !=
            PROS_USB_XHCI_EBOUNDS)
        return -3;
    event.parameter = (uint64_t)2u << XHCI_TRB_PORT_ID_SHIFT;
    controller.capabilities.max_ports = 1u;
    if (xhci_record_port_status_change(&resources, &controller, &event) !=
            PROS_USB_XHCI_EBOUNDS)
        return -4;
    return PROS_USB_XHCI_OK;
}

int pros_xhci_selftest(void) {    if (pros_xhci_address_context_selftest() != PROS_USB_XHCI_OK)
        return -102;

    if (pros_xhci_port_reset_selftest() != PROS_USB_XHCI_OK)
        return -100;
    if (pros_xhci_port_event_selftest() != PROS_USB_XHCI_OK)
        return -101;
    uint32_t registers[256] = {0};
    struct pros_xhci_capabilities caps;
    struct pros_xhci_controller controller = {0};
    uint32_t legacy_offset = 0u;
    volatile uint8_t *bytes = (volatile uint8_t *)registers;

    registers[0] = (0x0100u << 16) | 0x40u;
    registers[1] = 32u | (8u << 8) | (4u << 24);
    registers[2] = (2u << 21); /* Two scratchpad buffers. */
    registers[4] = (1u << 0) | (1u << 2); /* AC64 and 64-byte contexts. */
    registers[5] = 0x100u;
    registers[6] = 0x200u;
    registers[18] = 1u; /* 4 KiB page size supported. */
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
            PROS_USB_XHCI_OK ||
        caps.version != 0x0100u || caps.capability_length != 0x40u ||
        caps.max_slots != 32u || caps.max_interrupters != 8u ||
        caps.max_ports != 4u || caps.operational_offset != 0x40u ||
        caps.doorbell_offset != 0x100u || caps.runtime_offset != 0x200u ||
        caps.extended_capabilities_offset != 0u ||
        caps.scratchpad_buffers != 2u || caps.page_size_mask != 1u ||
        caps.address_64bit != 1u || caps.context_size_64 != 1u)
        return -1;

    registers[18] = 0u;
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
        PROS_USB_XHCI_EUNSUPPORTED)
        return -36;
    registers[18] = 1u;
    registers[0] = (0x0100u << 16) | 0x10u;
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
        PROS_USB_XHCI_EBOUNDS)
        return -2;
    registers[0] = (0x0100u << 16) | 0x40u;
    registers[5] = 0x1000u;
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
        PROS_USB_XHCI_EBOUNDS)
        return -3;
    /* First doorbell fits, but the full advertised array does not. */
    registers[6] = 0x200u;
    registers[5] = 0x3F0u;
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
        PROS_USB_XHCI_EBOUNDS)
        return -34;
    registers[5] = 0x100u;
    /* Runtime base fits, but all eight interrupter sets do not. */
    registers[6] = 0x300u;
    if (pros_xhci_parse_capabilities(bytes, sizeof(registers), &caps) !=
        PROS_USB_XHCI_EBOUNDS)
        return -35;
    registers[6] = 0x200u;
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
    /* Controller-level claim integrates xECP lookup with the handoff. */
    registers[16] = 1u;
    bytes[0x42] = 0u;
    bytes[0x43] = 0u;
    controller.initialized = 1u;
    controller.bar.virtual_address = (uint64_t)(uintptr_t)bytes;
    controller.bar.size = sizeof(registers);
    controller.capabilities.extended_capabilities_offset = 0x40u;
    if (pros_xhci_claim_ownership(&controller, 1u) != PROS_USB_XHCI_OK ||
        !controller.ownership_claimed || bytes[0x43] != 1u || bytes[0x42] != 0u)
        return -16;
    /* Repeated claim is idempotent and does not rewrite ownership state. */
    bytes[0x43] = 0u;
    if (pros_xhci_claim_ownership(&controller, 1u) != PROS_USB_XHCI_OK ||
        bytes[0x43] != 0u)
        return -17;
    controller.ownership_claimed = 0u;
    bytes[0x42] = 1u;
    bytes[0x43] = 0u;
    if (pros_xhci_claim_ownership(&controller, 1u) != PROS_USB_XHCI_ETIMEOUT ||
        controller.ownership_claimed || bytes[0x42] != 1u)
        return -18;
    controller.capabilities.extended_capabilities_offset = 0u;
    if (pros_xhci_claim_ownership(&controller, 1u) != PROS_USB_XHCI_OK ||
        !controller.ownership_claimed)
        return -19;
    controller.initialized = 0u;
    controller.ownership_claimed = 0u;
    if (pros_xhci_claim_ownership(&controller, 1u) != PROS_USB_XHCI_EINVAL)
        return -20;
    /* SMI shutdown clears enables and writes zero to every RW1C status bit. */
    controller.initialized = 1u;
    controller.ownership_claimed = 1u;
    controller.legacy_smi_disabled = 0u;
    controller.capabilities.extended_capabilities_offset = 0x40u;
    registers[16] = 1u;
    registers[17] = 0xE000E011u | 0x000E1FEEu | 0x00110000u;
    if (pros_xhci_disable_legacy_smi(&controller) != PROS_USB_XHCI_OK ||
        !controller.legacy_smi_disabled ||
        (registers[17] & XHCI_LEGACY_SMI_ENABLE_MASK) != 0u ||
        registers[17] != 0u)
        return -21;
    /* SMI shutdown is gated until ownership has been claimed. */
    controller.legacy_smi_disabled = 0u;
    controller.ownership_claimed = 0u;
    if (pros_xhci_disable_legacy_smi(&controller) != PROS_USB_XHCI_EINVAL)
        return -22;
    /* The reset sequence must stop, halt, reset, and wait for CNR. */
    {
        struct xhci_test_register_model model = {
            XHCI_USBCMD_RUN, 0u, 1u, 1u, 1u
        };
        if (xhci_halt_reset_sequence(&model, xhci_test_model_read,
                                     xhci_test_model_write, 4u) !=
                PROS_USB_XHCI_OK ||
            (model.command & (XHCI_USBCMD_RUN | XHCI_USBCMD_HCRST)) != 0u ||
            (model.status & XHCI_USBSTS_HCHALTED) == 0u ||
            (model.status & XHCI_USBSTS_CNR) != 0u)
            return -23;
    }
    /* A controller that never reports HCHalted must time out safely. */
    {
        struct xhci_test_register_model model = {
            XHCI_USBCMD_RUN, 0u, 0u, 0u, 0u
        };
        if (xhci_halt_reset_sequence(&model, xhci_test_model_read,
                                     xhci_test_model_write, 2u) !=
                PROS_USB_XHCI_ETIMEOUT ||
            (model.command & XHCI_USBCMD_RUN) != 0u ||
            (model.command & XHCI_USBCMD_HCRST) != 0u)
            return -24;
    }
    /* HCRST that does not self-clear must not be reported as success. */
    {
        struct xhci_test_register_model model = {
            0u, XHCI_USBSTS_HCHALTED, 1u, 0u, 0u
        };
        if (xhci_halt_reset_sequence(&model, xhci_test_model_read,
                                     xhci_test_model_write, 2u) !=
                PROS_USB_XHCI_ETIMEOUT ||
            (model.command & XHCI_USBCMD_HCRST) == 0u)
            return -25;
    }
    /* A reset that never clears CNR must also time out. */
    {
        struct xhci_test_register_model model = {
            0u, XHCI_USBSTS_HCHALTED | XHCI_USBSTS_CNR, 1u, 1u, 0u
        };
        if (xhci_halt_reset_sequence(&model, xhci_test_model_read,
                                     xhci_test_model_write, 2u) !=
                PROS_USB_XHCI_ETIMEOUT ||
            (model.command & XHCI_USBCMD_HCRST) != 0u ||
            (model.status & XHCI_USBSTS_CNR) == 0u)
            return -32;
    }
    /* Never replay an in-flight one-shot USBCMD action. */
    {
        struct xhci_test_register_model model = {
            XHCI_USBCMD_CSS, XHCI_USBSTS_HCHALTED, 1u, 1u, 1u
        };
        if (xhci_halt_reset_sequence(&model, xhci_test_model_read,
                                     xhci_test_model_write, 2u) !=
                PROS_USB_XHCI_EIO ||
            model.command != XHCI_USBCMD_CSS)
            return -33;
    }
    /* Public entry point enforces the ownership and SMI safety gates. */
    controller.initialized = 1u;
    controller.ownership_claimed = 0u;
    controller.legacy_smi_disabled = 0u;
    controller.reset_complete = 0u;
    controller.capabilities.operational_offset = 0x40u;
    if (pros_xhci_halt_reset(&controller, 2u) != PROS_USB_XHCI_EINVAL)
        return -26;
    controller.ownership_claimed = 1u;
    if (pros_xhci_halt_reset(&controller, 2u) != PROS_USB_XHCI_EINVAL)
        return -27;
    controller.legacy_smi_disabled = 1u;
    controller.bar.size = 0x44u;
    if (pros_xhci_halt_reset(&controller, 2u) != PROS_USB_XHCI_EBOUNDS)
        return -28;
    controller.bar.size = sizeof(registers);
    if (pros_xhci_halt_reset(&controller, 0u) != PROS_USB_XHCI_EINVAL)
        return -29;
    /* Exercise the production MMIO wrapper's bounded halt timeout. */
    registers[16] = XHCI_USBCMD_RUN;
    registers[17] = 0u;
    if (pros_xhci_halt_reset(&controller, 2u) != PROS_USB_XHCI_ETIMEOUT ||
        (registers[16] & XHCI_USBCMD_RUN) != 0u ||
        (registers[16] & XHCI_USBCMD_HCRST) != 0u ||
        controller.reset_complete)
        return -30;
    /* Exercise the production MMIO wrapper's bounded HCRST timeout. */
    registers[16] = 0u;
    registers[17] = XHCI_USBSTS_HCHALTED;
    if (pros_xhci_halt_reset(&controller, 2u) != PROS_USB_XHCI_ETIMEOUT ||
        (registers[16] & XHCI_USBCMD_HCRST) == 0u ||
        controller.reset_complete)
        return -31;
    controller.initialized = 0u;
    controller.ownership_claimed = 0u;
    controller.legacy_smi_disabled = 0u;
    controller.reset_complete = 0u;

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
    if (pros_xhci_runtime_access_selftest() != PROS_USB_XHCI_OK)
        return -36;
    if (pros_xhci_interrupter_program_selftest() != PROS_USB_XHCI_OK)
        return -37;
    if (pros_xhci_transfer_event_selftest() != PROS_USB_XHCI_OK)
        return -38;
    if (pros_xhci_event_ring_ack_selftest() != PROS_USB_XHCI_OK)
        return -39;
    if (pros_xhci_command_path_setup_selftest() != PROS_USB_XHCI_OK)
        return -40;
    if (pros_xhci_dma_layout_selftest() != PROS_USB_XHCI_OK)
        return -41;
    if (pros_xhci_scratchpad_selftest() != PROS_USB_XHCI_OK)
        return -42;
    return PROS_USB_XHCI_OK;
}


#define XHCI_SCRATCHPAD_PAGE_SIZE 4096u
#define XHCI_SCRATCHPAD_MAX_COUNT 1023u
#define XHCI_DMA_PHYS_LIMIT (1ull << 30)

/*
 * Allocate the xHCI Scratchpad Buffer Array and a contiguous pool containing
 * one 4 KiB page per advertised scratchpad buffer. No controller register is
 * touched here. The caller must keep PCI bus mastering disabled until all
 * DMA objects are validated and must quiesce the controller before release.
 */
int pros_xhci_scratchpad_alloc(
    const struct pros_xhci_capabilities *capabilities,
    struct pros_xhci_scratchpad_set *set)
{
    uint64_t pointer_bytes, pool_bytes;
    int rc;

    if (!capabilities || !set)
        return PROS_USB_XHCI_EINVAL;
    if (set->initialized || set->pointer_array.physical_address != 0u ||
        set->buffer_pool.physical_address != 0u)
        return PROS_USB_XHCI_EINVAL;

    set->count = 0u;
    set->initialized = 0u;
    if (capabilities->scratchpad_buffers > XHCI_SCRATCHPAD_MAX_COUNT)
        return PROS_USB_XHCI_EUNSUPPORTED;
    if ((capabilities->page_size_mask & 1u) == 0u)
        return PROS_USB_XHCI_EUNSUPPORTED;

    if (capabilities->scratchpad_buffers == 0u) {
        set->initialized = 1u;
        return PROS_USB_XHCI_OK;
    }

    pointer_bytes = (uint64_t)capabilities->scratchpad_buffers *
                    sizeof(uint64_t);
    pool_bytes = (uint64_t)capabilities->scratchpad_buffers *
                 XHCI_SCRATCHPAD_PAGE_SIZE;
    if (pointer_bytes == 0u || pool_bytes == 0u ||
        pointer_bytes > UINT64_MAX - (XHCI_SCRATCHPAD_PAGE_SIZE - 1u) ||
        pool_bytes > UINT64_MAX - (XHCI_SCRATCHPAD_PAGE_SIZE - 1u))
        return PROS_USB_XHCI_EBOUNDS;

    rc = pros_usb_dma_alloc(pointer_bytes, XHCI_SCRATCHPAD_PAGE_SIZE,
                            &set->pointer_array);
    if (rc != PROS_USB_DMA_OK)
        return PROS_USB_XHCI_EIO;

    rc = pros_usb_dma_alloc(pool_bytes, XHCI_SCRATCHPAD_PAGE_SIZE,
                            &set->buffer_pool);
    if (rc != PROS_USB_DMA_OK) {
        if (pros_usb_dma_free(&set->pointer_array) != PROS_USB_DMA_OK)
            set->initialized = 1u; /* Preserve a retryable partial owner. */
        return PROS_USB_XHCI_EIO;
    }

    if (!set->pointer_array.virtual_address ||
        !set->buffer_pool.virtual_address ||
        set->pointer_array.size < pointer_bytes ||
        set->pointer_array.allocation_size < pointer_bytes ||
        set->buffer_pool.size < pool_bytes ||
        set->buffer_pool.allocation_size < pool_bytes ||
        set->pointer_array.physical_address >= XHCI_DMA_PHYS_LIMIT ||
        set->buffer_pool.physical_address >= XHCI_DMA_PHYS_LIMIT ||
        set->pointer_array.physical_address >
            XHCI_DMA_PHYS_LIMIT - set->pointer_array.allocation_size ||
        set->buffer_pool.physical_address >
            XHCI_DMA_PHYS_LIMIT - set->buffer_pool.allocation_size ||
        (set->pointer_array.physical_address &
            (XHCI_SCRATCHPAD_PAGE_SIZE - 1u)) != 0u ||
        (set->buffer_pool.physical_address &
            (XHCI_SCRATCHPAD_PAGE_SIZE - 1u)) != 0u ||
        xhci_dma_spans_overlap(set->pointer_array.physical_address,
            set->pointer_array.allocation_size,
            set->buffer_pool.physical_address,
            set->buffer_pool.allocation_size)) {
        {
            int pool_rc = pros_usb_dma_free(&set->buffer_pool);
            int array_rc = pros_usb_dma_free(&set->pointer_array);
            if (pool_rc != PROS_USB_DMA_OK || array_rc != PROS_USB_DMA_OK)
                set->initialized = 1u; /* Allow caller to retry cleanup. */
        }
        return PROS_USB_XHCI_EBOUNDS;
    }

    {
        uint64_t *entries = (uint64_t *)set->pointer_array.virtual_address;
        for (uint32_t i = 0u; i < capabilities->scratchpad_buffers; ++i) {
            uint64_t address = set->buffer_pool.physical_address +
                (uint64_t)i * XHCI_SCRATCHPAD_PAGE_SIZE;
            if ((address & (XHCI_SCRATCHPAD_PAGE_SIZE - 1u)) != 0u ||
                address >= XHCI_DMA_PHYS_LIMIT ||
                XHCI_SCRATCHPAD_PAGE_SIZE > XHCI_DMA_PHYS_LIMIT - address) {
                {
                    int pool_rc = pros_usb_dma_free(&set->buffer_pool);
                    int array_rc = pros_usb_dma_free(&set->pointer_array);
                    if (pool_rc != PROS_USB_DMA_OK ||
                        array_rc != PROS_USB_DMA_OK)
                        set->initialized = 1u;
                }
                return PROS_USB_XHCI_EBOUNDS;
            }
            entries[i] = address;
        }
    }

    __atomic_thread_fence(__ATOMIC_RELEASE);
    set->count = capabilities->scratchpad_buffers;
    set->initialized = 1u;
    rc = pros_xhci_scratchpad_validate(capabilities, set);
    if (rc != PROS_USB_XHCI_OK) {
        (void)pros_xhci_scratchpad_release(set);
        return rc;
    }
    return PROS_USB_XHCI_OK;
}

/* Validate the exact pointer-array contents and the two DMA allocations. */
int pros_xhci_scratchpad_validate(
    const struct pros_xhci_capabilities *capabilities,
    const struct pros_xhci_scratchpad_set *set)
{
    uint64_t pointer_bytes, pool_bytes;
    const uint64_t *entries;

    if (!capabilities || !set || !set->initialized ||
        set->count != capabilities->scratchpad_buffers ||
        set->count > XHCI_SCRATCHPAD_MAX_COUNT ||
        (capabilities->page_size_mask & 1u) == 0u)
        return PROS_USB_XHCI_EINVAL;

    if (set->count == 0u) {
        if (set->pointer_array.physical_address != 0u ||
            set->pointer_array.virtual_address ||
            set->buffer_pool.physical_address != 0u ||
            set->buffer_pool.virtual_address)
            return PROS_USB_XHCI_EINVAL;
        return PROS_USB_XHCI_OK;
    }

    pointer_bytes = (uint64_t)set->count * sizeof(uint64_t);
    pool_bytes = (uint64_t)set->count * XHCI_SCRATCHPAD_PAGE_SIZE;
    if (!set->pointer_array.virtual_address ||
        !set->buffer_pool.virtual_address ||
        !set->pointer_array.physical_address ||
        !set->buffer_pool.physical_address ||
        !set->pointer_array.allocation_id ||
        !set->buffer_pool.allocation_id ||
        set->pointer_array.size < pointer_bytes ||
        set->pointer_array.allocation_size < pointer_bytes ||
        set->buffer_pool.size < pool_bytes ||
        set->buffer_pool.allocation_size < pool_bytes ||
        set->pointer_array.virtual_address !=
            (void *)(uintptr_t)set->pointer_array.physical_address ||
        set->buffer_pool.virtual_address !=
            (void *)(uintptr_t)set->buffer_pool.physical_address ||
        (set->pointer_array.physical_address & 63u) != 0u ||
        (set->buffer_pool.physical_address &
            (XHCI_SCRATCHPAD_PAGE_SIZE - 1u)) != 0u ||
        set->pointer_array.physical_address >= XHCI_DMA_PHYS_LIMIT ||
        set->buffer_pool.physical_address >= XHCI_DMA_PHYS_LIMIT ||
        set->pointer_array.allocation_size >
            XHCI_DMA_PHYS_LIMIT - set->pointer_array.physical_address ||
        set->buffer_pool.allocation_size >
            XHCI_DMA_PHYS_LIMIT - set->buffer_pool.physical_address ||
        xhci_dma_spans_overlap(set->pointer_array.physical_address,
            set->pointer_array.allocation_size,
            set->buffer_pool.physical_address,
            set->buffer_pool.allocation_size))
        return PROS_USB_XHCI_EBOUNDS;

    entries = (const uint64_t *)set->pointer_array.virtual_address;
    for (uint32_t i = 0u; i < set->count; ++i) {
        uint64_t expected = set->buffer_pool.physical_address +
            (uint64_t)i * XHCI_SCRATCHPAD_PAGE_SIZE;
        if (entries[i] != expected ||
            (entries[i] & (XHCI_SCRATCHPAD_PAGE_SIZE - 1u)) != 0u ||
            entries[i] >= XHCI_DMA_PHYS_LIMIT ||
            XHCI_SCRATCHPAD_PAGE_SIZE > XHCI_DMA_PHYS_LIMIT - entries[i])
            return PROS_USB_XHCI_EINVAL;
    }
    return PROS_USB_XHCI_OK;
}

/*
 * Release only after the controller has been halted/reset and can no longer
 * DMA to these pages. The allocator validates ownership tokens and scrubs
 * each full allocation before returning it to physical memory.
 */
int pros_xhci_scratchpad_release(struct pros_xhci_scratchpad_set *set)
{
    int rc;
    if (!set || !set->initialized)
        return PROS_USB_XHCI_EINVAL;

    if (set->buffer_pool.physical_address != 0u) {
        rc = pros_usb_dma_free(&set->buffer_pool);
        if (rc != PROS_USB_DMA_OK)
            return PROS_USB_XHCI_EIO;
    }
    if (set->pointer_array.physical_address != 0u) {
        rc = pros_usb_dma_free(&set->pointer_array);
        if (rc != PROS_USB_DMA_OK)
            return PROS_USB_XHCI_EIO;
    }
    set->count = 0u;
    set->initialized = 0u;
    return PROS_USB_XHCI_OK;
}

int pros_xhci_scratchpad_selftest(void)
{
    struct pros_xhci_capabilities capabilities = {0};
    struct pros_xhci_scratchpad_set set = {0};
    uint64_t free_before = physical_memory_free_pages();

    capabilities.scratchpad_buffers = 3u;
    capabilities.page_size_mask = 1u;
    capabilities.address_64bit = 0u;
    if (pros_xhci_scratchpad_alloc(&capabilities, &set) !=
            PROS_USB_XHCI_OK ||
        pros_xhci_scratchpad_validate(&capabilities, &set) !=
            PROS_USB_XHCI_OK ||
        set.count != 3u ||
        set.pointer_array.size != 3u * sizeof(uint64_t) ||
        set.buffer_pool.size != 3u * XHCI_SCRATCHPAD_PAGE_SIZE)
        return -1;

    {
        uint64_t *entries = (uint64_t *)set.pointer_array.virtual_address;
        uint64_t saved = entries[1];
        entries[1] ^= XHCI_SCRATCHPAD_PAGE_SIZE;
        if (pros_xhci_scratchpad_validate(&capabilities, &set) !=
            PROS_USB_XHCI_EINVAL)
            return -2;
        entries[1] = saved;
    }
    {
        struct pros_xhci_capabilities bad = capabilities;
        bad.page_size_mask = 0u;
        if (pros_xhci_scratchpad_validate(&bad, &set) !=
            PROS_USB_XHCI_EINVAL)
            return -3;
        bad = capabilities;
        bad.scratchpad_buffers = 4u;
        if (pros_xhci_scratchpad_validate(&bad, &set) !=
            PROS_USB_XHCI_EINVAL)
            return -4;
        bad.scratchpad_buffers = XHCI_SCRATCHPAD_MAX_COUNT + 1u;
        if (pros_xhci_scratchpad_alloc(&bad, &(struct pros_xhci_scratchpad_set){0}) !=
            PROS_USB_XHCI_EUNSUPPORTED)
            return -5;
    }
    if (pros_xhci_scratchpad_release(&set) != PROS_USB_XHCI_OK ||
        set.initialized || set.count != 0u ||
        set.pointer_array.physical_address != 0u ||
        set.buffer_pool.physical_address != 0u ||
        physical_memory_free_pages() != free_before)
        return -6;

    capabilities.scratchpad_buffers = 0u;
    set = (struct pros_xhci_scratchpad_set){0};
    if (pros_xhci_scratchpad_alloc(&capabilities, &set) !=
            PROS_USB_XHCI_OK ||
        pros_xhci_scratchpad_validate(&capabilities, &set) !=
            PROS_USB_XHCI_OK ||
        pros_xhci_scratchpad_release(&set) != PROS_USB_XHCI_OK)
        return -7;

    capabilities.scratchpad_buffers = 1u;
    capabilities.page_size_mask = 0u;
    if (pros_xhci_scratchpad_alloc(&capabilities,
            &(struct pros_xhci_scratchpad_set){0}) !=
        PROS_USB_XHCI_EUNSUPPORTED)
        return -8;
    return physical_memory_free_pages() == free_before ?
        PROS_USB_XHCI_OK : -9;
}


#define XHCI_RESOURCE_COMMAND_TRBS 256u
#define XHCI_RESOURCE_EVENT_TRBS 256u

static int xhci_resource_scratchpads_disjoint(
    const struct pros_xhci_dma_resources *resources)
{
    const struct pros_xhci_scratchpad_set *sp = &resources->scratchpads;
    const struct pros_usb_dma_buffer *objects[4] = {
        &resources->dcbaa, &resources->command_ring_dma,
        &resources->event_ring_dma, &resources->erst_dma
    };
    if (sp->count == 0u)
        return 1;
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (xhci_dma_spans_overlap(objects[i]->physical_address,
                objects[i]->allocation_size,
                sp->pointer_array.physical_address,
                sp->pointer_array.allocation_size) ||
            xhci_dma_spans_overlap(objects[i]->physical_address,
                objects[i]->allocation_size,
                sp->buffer_pool.physical_address,
                sp->buffer_pool.allocation_size))
            return 0;
    }
    return 1;
}

/* Refuse release after any DMA address has been published to hardware. */
int pros_xhci_dma_resources_release(struct pros_xhci_dma_resources *resources)
{
    if (!resources)
        return PROS_USB_XHCI_EINVAL;
    if (resources->hardware_referenced)
        return PROS_USB_XHCI_EIO;
    if (resources->scratchpads.initialized &&
        pros_xhci_scratchpad_release(&resources->scratchpads) !=
            PROS_USB_XHCI_OK)
        return PROS_USB_XHCI_EIO;
    if (resources->erst_dma.physical_address != 0u &&
        pros_usb_dma_free(&resources->erst_dma) != PROS_USB_DMA_OK)
        return PROS_USB_XHCI_EIO;
    if (resources->event_ring_dma.physical_address != 0u &&
        pros_usb_dma_free(&resources->event_ring_dma) != PROS_USB_DMA_OK)
        return PROS_USB_XHCI_EIO;
    if (resources->command_ring_dma.physical_address != 0u &&
        pros_usb_dma_free(&resources->command_ring_dma) != PROS_USB_DMA_OK)
        return PROS_USB_XHCI_EIO;
    if (resources->dcbaa.physical_address != 0u &&
        pros_usb_dma_free(&resources->dcbaa) != PROS_USB_DMA_OK)
        return PROS_USB_XHCI_EIO;
    resources->command_ring = (struct pros_xhci_command_ring){0};
    resources->event_ring = (struct pros_xhci_event_ring){0};
    resources->erst = (struct pros_xhci_erst){0};
    resources->initialized = 0u;
    return PROS_USB_XHCI_OK;
}

int pros_xhci_dma_resources_alloc(
    const struct pros_xhci_controller *controller,
    struct pros_xhci_dma_resources *resources)
{
    uint64_t dcbaa_size;
    uint32_t op;
    const volatile uint8_t *bar;
    int rc;

    if (!controller || !resources || !controller->initialized ||
        !controller->reset_complete || controller->bar.virtual_address == 0u ||
        controller->capabilities.max_slots == 0u)
        return PROS_USB_XHCI_EINVAL;
    if (resources->initialized || resources->hardware_referenced ||
        resources->dcbaa.physical_address != 0u ||
        resources->command_ring_dma.physical_address != 0u ||
        resources->event_ring_dma.physical_address != 0u ||
        resources->erst_dma.physical_address != 0u ||
        resources->scratchpads.initialized)
        return PROS_USB_XHCI_EINVAL;

    op = controller->capabilities.operational_offset;
    if ((op & 3u) != 0u || !xhci_range_valid(op, 8u, controller->bar.size))
        return PROS_USB_XHCI_EBOUNDS;
    bar = (const volatile uint8_t *)(uintptr_t)controller->bar.virtual_address;
    if ((xhci_read32(bar, op) & XHCI_USBCMD_RUN) != 0u ||
        (xhci_read32(bar, op + 4u) & XHCI_USBSTS_HCHALTED) == 0u)
        return PROS_USB_XHCI_EIO;

    *resources = (struct pros_xhci_dma_resources){0};
    dcbaa_size = ((uint64_t)controller->capabilities.max_slots + 1u) *
                 sizeof(uint64_t);
    rc = pros_usb_dma_alloc(dcbaa_size, PROSENJIT_PAGE_SIZE, &resources->dcbaa);
    if (rc != PROS_USB_DMA_OK)
        goto fail;
    rc = pros_usb_dma_alloc(256u * PROS_XHCI_TRB_SIZE, PROSENJIT_PAGE_SIZE,
                            &resources->command_ring_dma);
    if (rc != PROS_USB_DMA_OK)
        goto fail;
    rc = pros_usb_dma_alloc(256u * PROS_XHCI_TRB_SIZE, PROSENJIT_PAGE_SIZE,
                            &resources->event_ring_dma);
    if (rc != PROS_USB_DMA_OK)
        goto fail;
    rc = pros_usb_dma_alloc(sizeof(struct pros_xhci_erst_entry), PROSENJIT_PAGE_SIZE,
                            &resources->erst_dma);
    if (rc != PROS_USB_DMA_OK)
        goto fail;
    rc = pros_xhci_scratchpad_alloc(&controller->capabilities,
                                    &resources->scratchpads);
    if (rc != PROS_USB_XHCI_OK)
        goto fail;
    rc = pros_xhci_command_ring_init(&resources->command_ring,
        &resources->command_ring_dma, 256u);
    if (rc != PROS_XHCI_RING_OK)
        goto fail;
    rc = pros_xhci_event_ring_init(&resources->event_ring,
        &resources->event_ring_dma, 256u);
    if (rc != PROS_XHCI_RING_OK)
        goto fail;
    rc = pros_xhci_erst_init(&resources->erst, &resources->erst_dma,
        &resources->event_ring_dma, 256u);
    if (rc != PROS_XHCI_RING_OK)
        goto fail;
    rc = pros_xhci_validate_dma_layout(controller, &resources->dcbaa,
        &resources->command_ring, &resources->erst, &resources->event_ring);
    if (rc != PROS_USB_XHCI_OK || !xhci_resource_scratchpads_disjoint(resources))
        goto fail;
    resources->initialized = 1u;
    return PROS_USB_XHCI_OK;

fail:
    {
        int release_rc = pros_xhci_dma_resources_release(resources);
        if (release_rc != PROS_USB_XHCI_OK)
            return PROS_USB_XHCI_EIO; /* Partial owner remains retryable. */
    }
    return (rc == PROS_USB_DMA_ENOMEM) ? PROS_USB_XHCI_EIO :
           (rc == PROS_USB_XHCI_OK ? PROS_USB_XHCI_EINVAL : rc);
}


typedef int (*xhci_pci_command_read_fn)(const struct pros_pci_device *,
                                        uint32_t *);
typedef int (*xhci_pci_command_enable_fn)(const struct pros_pci_device *);

static int xhci_read_pci_command(const struct pros_pci_device *device,
                                 uint32_t *command)
{
    return pci_config_read32(device->bus, device->slot, device->function,
                             0x04u, command);
}

/*
 * Publish the controller DMA set while it remains halted. Validate and
 * pin every DMA allocation before the first MMIO write; program DCBAA/CRCR
 * with bus mastering disabled, then enable bus mastering immediately before
 * ERSTBA publication because that write may trigger an immediate DMA fetch.
 * A partial register update may already expose DMA addresses to hardware.
 */
static int xhci_program_dma_resources_disabled_with_reader(
    struct pros_xhci_controller *controller,
    struct pros_xhci_dma_resources *resources, uint16_t interrupter,
    xhci_pci_command_read_fn read_pci_command,
    xhci_pci_command_enable_fn enable_bus_master)
{
    uint64_t op;
    uint32_t pci_command, usbcmd, usbsts;
    int rc;

    if (!controller || !resources || !read_pci_command ||
        !enable_bus_master || !controller->initialized || !controller->ownership_claimed ||
        !controller->legacy_smi_disabled || !controller->reset_complete ||
        !resources->initialized || resources->hardware_referenced ||
        !controller->bar.virtual_address || !controller->bar.size ||
        interrupter >= controller->capabilities.max_interrupters)
        return PROS_USB_XHCI_EINVAL;

    rc = read_pci_command(&controller->pci, &pci_command);
    if (rc != PROS_PCI_OK)
        return PROS_USB_XHCI_EIO;
    if ((pci_command & (1u << 2)) != 0u)
        return PROS_USB_XHCI_EIO;

    op = controller->capabilities.operational_offset;
    if ((op & 3u) != 0u ||
        !xhci_range_valid(op, 8u, controller->bar.size))
        return PROS_USB_XHCI_EBOUNDS;
    {
        const volatile uint8_t *bar =
            (const volatile uint8_t *)(uintptr_t)
                controller->bar.virtual_address;
        usbcmd = xhci_read32(bar, (uint32_t)op);
        usbsts = xhci_read32(bar, (uint32_t)(op + 4u));
    }
    if ((usbcmd & XHCI_USBCMD_RUN) != 0u ||
        (usbsts & XHCI_USBSTS_HCHALTED) == 0u)
        return PROS_USB_XHCI_EIO;

    rc = pros_xhci_validate_dma_layout(controller, &resources->dcbaa,
        &resources->command_ring, &resources->erst, &resources->event_ring);
    if (rc != PROS_USB_XHCI_OK ||
        pros_xhci_scratchpad_validate(&controller->capabilities,
                                     &resources->scratchpads) !=
            PROS_USB_XHCI_OK ||
        !xhci_resource_scratchpads_disjoint(resources))
        return PROS_USB_XHCI_EINVAL;

    /*
     * Fail closed from this point onward. Even if a later register readback
     * fails, never let the caller free memory that may have been published.
     */
    resources->hardware_referenced = 1u;
    rc = pros_xhci_program_command_path_disabled(controller,
        &resources->dcbaa, &resources->scratchpads,
        &resources->command_ring);
    if (rc != PROS_USB_XHCI_OK)
        return rc;

    /*
     * ERSTBA publication makes the controller fetch the ERST immediately on
     * some implementations (including QEMU). Enable PCI DMA only after every
     * buffer is allocated, validated and pinned, but before publishing ERSTBA.
     */
    rc = enable_bus_master(&controller->pci);
    if (rc != PROS_PCI_OK)
        return PROS_USB_XHCI_EIO;
    rc = read_pci_command(&controller->pci, &pci_command);
    if (rc != PROS_PCI_OK || (pci_command & (1u << 2)) == 0u)
        return PROS_USB_XHCI_EIO;

    rc = pros_xhci_program_interrupter_disabled(controller, interrupter,
        &resources->erst, &resources->event_ring, 0u);
    if (rc != PROS_USB_XHCI_OK)
        return rc;

    {
        const volatile uint8_t *bar =
            (const volatile uint8_t *)(uintptr_t)
                controller->bar.virtual_address;
        if ((xhci_read32(bar, (uint32_t)op) & XHCI_USBCMD_RUN) != 0u ||
            (xhci_read32(bar, (uint32_t)(op + 4u)) &
             XHCI_USBSTS_HCHALTED) == 0u)
            return PROS_USB_XHCI_EIO;
    }
    return PROS_USB_XHCI_OK;
}

int pros_xhci_program_dma_resources_disabled(
    struct pros_xhci_controller *controller,
    struct pros_xhci_dma_resources *resources, uint16_t interrupter)
{
    return xhci_program_dma_resources_disabled_with_reader(controller,
        resources, interrupter, xhci_read_pci_command,
        pci_enable_bus_master);
}

struct xhci_test_pci_command {
    uint32_t command;
    uint32_t reads;
    uint32_t enables;
};

static struct xhci_test_pci_command *xhci_test_pci_command_model;

static int xhci_test_read_pci_command(const struct pros_pci_device *device,
                                      uint32_t *command)
{
    (void)device;
    if (!xhci_test_pci_command_model || !command)
        return PROS_PCI_EINVAL;
    ++xhci_test_pci_command_model->reads;
    *command = xhci_test_pci_command_model->command;
    return PROS_PCI_OK;
}

static int xhci_test_enable_bus_master(const struct pros_pci_device *device)
{
    (void)device;
    if (!xhci_test_pci_command_model)
        return PROS_PCI_EINVAL;
    ++xhci_test_pci_command_model->enables;
    xhci_test_pci_command_model->command |= (1u << 2);
    return PROS_PCI_OK;
}

int pros_xhci_program_dma_resources_selftest(void)
{
    static uint64_t mmio[128] __attribute__((aligned(64)));
    struct pros_xhci_controller controller = {0};
    struct pros_xhci_dma_resources resources = {0};
    struct xhci_test_pci_command pci_model = {0u, 0u, 0u};
    volatile uint8_t *bar = (volatile uint8_t *)mmio;
    volatile uint64_t *dcbaap =
        (volatile uint64_t *)(volatile void *)(bar + 0x70u);
    volatile uint64_t *crcr =
        (volatile uint64_t *)(volatile void *)(bar + 0x58u);
    volatile uint64_t *erstba =
        (volatile uint64_t *)(volatile void *)(bar + 0x130u);
    volatile uint32_t *erstsz =
        (volatile uint32_t *)(volatile void *)(bar + 0x128u);
    volatile uint32_t *iman =
        (volatile uint32_t *)(volatile void *)(bar + 0x120u);
    uint64_t free_before = physical_memory_free_pages();

    controller.initialized = 1u;
    controller.ownership_claimed = 1u;
    controller.legacy_smi_disabled = 1u;
    controller.reset_complete = 1u;
    controller.bar.virtual_address = (uint64_t)(uintptr_t)bar;
    controller.bar.size = sizeof(mmio);
    controller.capabilities.operational_offset = 0x40u;
    controller.capabilities.runtime_offset = 0x100u;
    controller.capabilities.max_slots = 8u;
    controller.capabilities.max_interrupters = 1u;
    controller.capabilities.scratchpad_buffers = 2u;
    controller.capabilities.page_size_mask = 1u;
    xhci_test_pci_command_model = &pci_model;
    xhci_write32(bar, 0x40u, 0u);
    xhci_write32(bar, 0x44u, XHCI_USBSTS_HCHALTED);

    if (pros_xhci_dma_resources_alloc(&controller, &resources) !=
        PROS_USB_XHCI_OK)
        return -1;
    pci_model.command = 1u << 2; /* Must reject while bus mastering is on. */
    if (xhci_program_dma_resources_disabled_with_reader(&controller,
            &resources, 0u, xhci_test_read_pci_command,
            xhci_test_enable_bus_master) !=
            PROS_USB_XHCI_EIO ||
        resources.hardware_referenced || pci_model.reads != 1u)
        return -2;

    pci_model.command = 0u;
    if (xhci_program_dma_resources_disabled_with_reader(&controller,
            &resources, 0u, xhci_test_read_pci_command,
            xhci_test_enable_bus_master) != PROS_USB_XHCI_OK ||
        !resources.hardware_referenced || pci_model.reads != 3u ||
        pci_model.enables != 1u || (pci_model.command & (1u << 2)) == 0u)
        return -3;
    if (*dcbaap != resources.dcbaa.physical_address ||
        (*crcr & ~0x3Full) != resources.command_ring.physical_address ||
        (*crcr & 1u) == 0u ||
        *erstba != resources.erst.physical_address ||
        *erstsz != 1u || (*iman & (1u << 1)) != 0u ||
        (xhci_read32(bar, 0x40u) & XHCI_USBCMD_RUN) != 0u ||
        (xhci_read32(bar, 0x44u) & XHCI_USBSTS_HCHALTED) == 0u)
        return -4;
    if (pros_xhci_dma_resources_release(&resources) != PROS_USB_XHCI_EIO ||
        !resources.initialized)
        return -5;

    /* This test MMIO model cannot DMA; clear the synthetic pin for cleanup. */
    resources.hardware_referenced = 0u;
    if (pros_xhci_dma_resources_release(&resources) != PROS_USB_XHCI_OK ||
        physical_memory_free_pages() != free_before) {
        xhci_test_pci_command_model = 0;
        return -6;
    }
    xhci_test_pci_command_model = 0;
    return PROS_USB_XHCI_OK;
}

int pros_xhci_dma_resources_selftest(void)
{
    static uint64_t mmio[64] __attribute__((aligned(64)));
    struct pros_xhci_controller controller = {0};
    struct pros_xhci_dma_resources resources = {0};
    volatile uint8_t *bar = (volatile uint8_t *)mmio;
    uint64_t free_before = physical_memory_free_pages();

    controller.initialized = 1u;
    controller.reset_complete = 1u;
    controller.bar.virtual_address = (uint64_t)(uintptr_t)bar;
    controller.bar.size = sizeof(mmio);
    controller.capabilities.operational_offset = 0x40u;
    controller.capabilities.max_slots = 8u;
    controller.capabilities.scratchpad_buffers = 2u;
    controller.capabilities.page_size_mask = 1u;
    xhci_write32(bar, 0x40u, 0u);
    xhci_write32(bar, 0x44u, XHCI_USBSTS_HCHALTED);

    {
        int alloc_rc = pros_xhci_dma_resources_alloc(&controller, &resources);
        if (alloc_rc != PROS_USB_XHCI_OK)
            return -10 + alloc_rc;
    }
    if (!resources.initialized || resources.hardware_referenced)
        return -20;
    if (resources.scratchpads.count != 2u)
        return -21;
    if (resources.command_ring.entry_count != 256u)
        return -22;
    if (resources.event_ring.entry_count != 256u)
        return -23;
    if (resources.erst.entry_count != 1u)
        return -24;
    if (pros_xhci_validate_dma_layout(&controller, &resources.dcbaa,
            &resources.command_ring, &resources.erst,
            &resources.event_ring) != PROS_USB_XHCI_OK)
        return -25;
    if (!xhci_resource_scratchpads_disjoint(&resources))
        return -26;

    resources.hardware_referenced = 1u;
    if (pros_xhci_dma_resources_release(&resources) != PROS_USB_XHCI_EIO ||
        !resources.initialized || resources.dcbaa.physical_address == 0u)
        return -2;
    resources.hardware_referenced = 0u;
    if (pros_xhci_dma_resources_release(&resources) != PROS_USB_XHCI_OK ||
        resources.initialized || resources.dcbaa.physical_address != 0u ||
        resources.command_ring_dma.physical_address != 0u ||
        resources.event_ring_dma.physical_address != 0u ||
        resources.erst_dma.physical_address != 0u ||
        resources.scratchpads.initialized ||
        physical_memory_free_pages() != free_before)
        return -3;

    xhci_write32(bar, 0x44u, 0u);
    if (pros_xhci_dma_resources_alloc(&controller, &resources) !=
        PROS_USB_XHCI_EIO)
        return -4;
    return physical_memory_free_pages() == free_before ?
        PROS_USB_XHCI_OK : -5;
}

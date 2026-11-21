#include "pci.h"
#include "paging.h"

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA    0xCFCu
#define PCI_COMMAND_IO     0x0001u
#define PCI_COMMAND_MEMORY 0x0002u
#define PCI_COMMAND_MASTER 0x0004u

static inline void pci_out32(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t pci_in32(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_config_address(uint8_t bus, uint8_t slot,
                                   uint8_t function, uint8_t offset)
{
    return 0x80000000u |
           ((uint32_t)bus << 16) |
           ((uint32_t)(slot & 0x1Fu) << 11) |
           ((uint32_t)(function & 0x07u) << 8) |
           ((uint32_t)(offset & 0xFCu));
}

/*
 * Mechanism #1 shares one address/data register pair. Serialize each pair
 * against local IRQ handlers and other CPUs. Multi-register transactions hold
 * this lock and use the private raw accessors.
 */
static volatile uint8_t pci_config_lock_state;
static uint64_t pci_config_lock(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory", "cc");
    while (__atomic_test_and_set(&pci_config_lock_state, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
    return flags;
}
static void pci_config_unlock(uint64_t flags)
{
    __atomic_clear(&pci_config_lock_state, __ATOMIC_RELEASE);
    if (flags & (1ull << 9))
        __asm__ volatile ("sti" ::: "memory");
}
static int pci_config_read32_raw(uint8_t bus, uint8_t slot, uint8_t function,
                                 uint8_t offset, uint32_t *value)
{
    if (!value || slot >= 32u || function >= 8u || (offset & 3u) != 0u)
        return PROS_PCI_EINVAL;
    pci_out32(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, function, offset));
    *value = pci_in32(PCI_CONFIG_DATA);
    return PROS_PCI_OK;
}
static int pci_config_write32_raw(uint8_t bus, uint8_t slot, uint8_t function,
                                  uint8_t offset, uint32_t value)
{
    if (slot >= 32u || function >= 8u || (offset & 3u) != 0u)
        return PROS_PCI_EINVAL;
    pci_out32(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, function, offset));
    pci_out32(PCI_CONFIG_DATA, value);
    return PROS_PCI_OK;
}
int pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint32_t *value)
{
    uint64_t flags;
    int rc;
    if (!value || slot >= 32u || function >= 8u || (offset & 3u) != 0u)
        return PROS_PCI_EINVAL;
    flags = pci_config_lock();
    rc = pci_config_read32_raw(bus, slot, function, offset, value);
    pci_config_unlock(flags);
    return rc;
}
int pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function,
                       uint8_t offset, uint32_t value)
{
    uint64_t flags;
    int rc;
    if (slot >= 32u || function >= 8u || (offset & 3u) != 0u)
        return PROS_PCI_EINVAL;
    flags = pci_config_lock();
    rc = pci_config_write32_raw(bus, slot, function, offset, value);
    pci_config_unlock(flags);
    return rc;
}

static int pci_read16(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint16_t *value)
{
    uint32_t word;
    if (!value || (offset & 1u) != 0u)
        return PROS_PCI_EINVAL;
    if (pci_config_read32(bus, slot, function, (uint8_t)(offset & 0xFCu),
                          &word) != PROS_PCI_OK)
        return PROS_PCI_EINVAL;
    *value = (uint16_t)((word >> ((offset & 2u) * 8u)) & 0xFFFFu);
    return PROS_PCI_OK;
}

static int pci_validate_device(const struct pros_pci_device *device);

/*
 * Conventional capabilities occupy aligned offsets 0x40..0xFC. Keep a
 * visited bitmap so malformed firmware/device chains cannot loop forever.
 */
static int pci_capability_advance(uint8_t current, uint8_t next,
                                  uint64_t *visited, uint8_t *next_offset)
{
    uint64_t current_bit;
    if (!visited || !next_offset)
        return PROS_PCI_EINVAL;
    if (current < 0x40u || current > 0xFCu || (current & 3u) != 0u)
        return PROS_PCI_EIO;
    current_bit = 1ULL << (current >> 2);
    if (*visited & current_bit)
        return PROS_PCI_EIO;
    *visited |= current_bit;

    if (next == 0u) {
        *next_offset = 0u;
        return PROS_PCI_OK;
    }
    if (next < 0x40u || next > 0xFCu || (next & 3u) != 0u)
        return PROS_PCI_EIO;
    if (*visited & (1ULL << (next >> 2)))
        return PROS_PCI_EIO;
    *next_offset = next;
    return PROS_PCI_OK;
}

int pci_find_capability(const struct pros_pci_device *device,
                        uint8_t capability_id, uint8_t *offset)
{
    uint16_t status;
    uint32_t pointer_word;
    uint8_t current;
    uint64_t visited = 0u;
    int rc;

    if (!offset || capability_id == 0u)
        return PROS_PCI_EINVAL;
    *offset = 0u;
    rc = pci_validate_device(device);
    if (rc != PROS_PCI_OK)
        return rc;
    if ((device->header_type & 0x7Fu) > 1u)
        return PROS_PCI_EUNSUPPORTED;
    if (pci_read16(device->bus, device->slot, device->function, 0x06u,
                   &status) != PROS_PCI_OK)
        return PROS_PCI_EIO;
    if ((status & 0x0010u) == 0u)
        return PROS_PCI_ENOCAP;
    if (pci_config_read32(device->bus, device->slot, device->function,
                          0x34u, &pointer_word) != PROS_PCI_OK)
        return PROS_PCI_EIO;
    current = (uint8_t)(pointer_word & 0xFFu);

    while (current != 0u) {
        uint32_t capability_word;
        uint8_t next;
        /* Reject reserved/misaligned offsets before touching config space. */
        if (current < 0x40u || current > 0xFCu || (current & 3u) != 0u ||
            (visited & (1ULL << (current >> 2))) != 0u)
            return PROS_PCI_EIO;
        rc = pci_config_read32(device->bus, device->slot, device->function,
                               current, &capability_word);
        if (rc != PROS_PCI_OK)
            return PROS_PCI_EIO;
        if ((uint8_t)capability_word == capability_id) {
            uint8_t ignored;
            rc = pci_capability_advance(current,
                    (uint8_t)(capability_word >> 8), &visited, &ignored);
            if (rc != PROS_PCI_OK)
                return rc;
            *offset = current;
            return PROS_PCI_OK;
        }
        next = (uint8_t)(capability_word >> 8);
        rc = pci_capability_advance(current, next, &visited, &current);
        if (rc != PROS_PCI_OK)
            return rc;
    }
    return PROS_PCI_ENOCAP;
}


static void pci_decode_msi_control(uint16_t control,
                                     struct pros_pci_interrupt_caps *caps)
{
    caps->msi_present = 1u;
    caps->msi_enabled = (uint8_t)(control & 1u);
    caps->msi_64bit = (uint8_t)((control >> 7) & 1u);
    caps->msi_mask_capable = (uint8_t)((control >> 8) & 1u);
    caps->msi_max_vectors = (uint16_t)(1u << ((control >> 1) & 7u));
}

static int pci_decode_msix_registers(uint16_t control, uint32_t table,
                                     uint32_t pba,
                                     struct pros_pci_interrupt_caps *caps)
{
    uint8_t table_bir = (uint8_t)(table & 7u);
    uint8_t pba_bir = (uint8_t)(pba & 7u);
    if (!caps || table_bir >= 6u || pba_bir >= 6u)
        return PROS_PCI_EIO;
    caps->msix_present = 1u;
    caps->msix_enabled = (uint8_t)((control >> 15) & 1u);
    caps->msix_function_mask = (uint8_t)((control >> 14) & 1u);
    caps->msix_table_size = (uint16_t)((control & 0x07FFu) + 1u);
    caps->msix_table_bir = table_bir;
    caps->msix_table_offset = table & ~7u;
    caps->msix_pba_bir = pba_bir;
    caps->msix_pba_offset = pba & ~7u;
    return PROS_PCI_OK;
}

/*
 * Read-only discovery only. This routine never enables MSI/MSI-X, writes
 * message address/data, unmasks vectors, or changes PCI command bits.
 */
int pci_get_interrupt_capabilities(
    const struct pros_pci_device *device,
    struct pros_pci_interrupt_caps *caps)
{
    uint8_t offset;
    uint32_t word, table, pba;
    int found = 0;
    int rc;

    if (!caps)
        return PROS_PCI_EINVAL;
    caps->msi_present = 0u;
    caps->msi_enabled = 0u;
    caps->msi_64bit = 0u;
    caps->msi_mask_capable = 0u;
    caps->msi_max_vectors = 0u;
    caps->msix_present = 0u;
    caps->msix_enabled = 0u;
    caps->msix_function_mask = 0u;
    caps->msix_table_size = 0u;
    caps->msix_table_bir = 0u;
    caps->msix_table_offset = 0u;
    caps->msix_pba_bir = 0u;
    caps->msix_pba_offset = 0u;

    rc = pci_find_capability(device, 0x05u, &offset);
    if (rc == PROS_PCI_OK) {
        if (pci_config_read32(device->bus, device->slot, device->function,
                              offset, &word) != PROS_PCI_OK)
            return PROS_PCI_EIO;
        pci_decode_msi_control((uint16_t)(word >> 16), caps);
        found = 1;
    } else if (rc != PROS_PCI_ENOCAP) {
        return rc;
    }

    rc = pci_find_capability(device, 0x11u, &offset);
    if (rc == PROS_PCI_OK) {
        if (offset > 0xF4u ||
            pci_config_read32(device->bus, device->slot, device->function,
                              (uint8_t)(offset + 4u), &table) != PROS_PCI_OK ||
            pci_config_read32(device->bus, device->slot, device->function,
                              (uint8_t)(offset + 8u), &pba) != PROS_PCI_OK)
            return PROS_PCI_EIO;
        if (pci_config_read32(device->bus, device->slot, device->function,
                              offset, &word) != PROS_PCI_OK)
            return PROS_PCI_EIO;
        rc = pci_decode_msix_registers((uint16_t)(word >> 16), table, pba,
                                       caps);
        if (rc != PROS_PCI_OK)
            return rc;
        found = 1;
    } else if (rc != PROS_PCI_ENOCAP) {
        return rc;
    }

    return found ? PROS_PCI_OK : PROS_PCI_ENOCAP;
}

static uint16_t pci_command_update_bits(uint16_t command,
                                        uint16_t set_bits,
                                        uint16_t clear_bits)
{
    return (uint16_t)((command | set_bits) & (uint16_t)~clear_bits);
}

static int pci_update_command_bits(const struct pros_pci_device *device,
                                   uint16_t set_bits, uint16_t clear_bits)
{
    uint32_t command_word = 0u, verify_word = 0u;
    uint16_t command, updated;
    uint64_t flags;
    int rc = pci_validate_device(device);
    if (rc != PROS_PCI_OK)
        return rc;

    /*
     * Command-register read/modify/write must be atomic as a whole. Writing
     * only the low halfword also writes zero to the W1C status halfword.
     */
    flags = pci_config_lock();
    rc = pci_config_read32_raw(device->bus, device->slot, device->function,
                               0x04u, &command_word);
    if (rc != PROS_PCI_OK) {
        rc = PROS_PCI_EIO;
        goto out;
    }
    command = (uint16_t)command_word;
    updated = pci_command_update_bits(command, set_bits, clear_bits);
    rc = pci_config_write32_raw(device->bus, device->slot, device->function,
                                0x04u, (uint32_t)updated);
    if (rc != PROS_PCI_OK) {
        rc = PROS_PCI_EIO;
        goto out;
    }
    rc = pci_config_read32_raw(device->bus, device->slot, device->function,
                               0x04u, &verify_word);
    if (rc != PROS_PCI_OK || (uint16_t)verify_word != updated)
        rc = PROS_PCI_EIO;
    else
        rc = PROS_PCI_OK;
out:
    pci_config_unlock(flags);
    return rc;
}

static int pci_validate_device(const struct pros_pci_device *device)
{
    uint32_t id;
    if (!device || device->slot >= 32u || device->function >= 8u)
        return PROS_PCI_EINVAL;
    if (pci_config_read32(device->bus, device->slot, device->function,
                          0x00u, &id) != PROS_PCI_OK)
        return PROS_PCI_EIO;
    if ((uint16_t)id == 0xFFFFu ||
        (uint16_t)id != device->vendor_id ||
        (uint16_t)(id >> 16) != device->device_id)
        return PROS_PCI_ENODEV;
    return PROS_PCI_OK;
}

static int pci_bar_decode(uint32_t original_low, uint32_t original_high,
                          uint32_t mask_low, uint32_t mask_high,
                          struct pros_pci_bar *bar)
{
    uint64_t mask;
    uint64_t size;

    if (!bar || original_low == 0u || original_low == 0xFFFFFFFFu)
        return PROS_PCI_EINVAL;

    bar->is_io = (uint8_t)(original_low & 1u);
    bar->is_64bit = 0u;
    bar->prefetchable = 0u;

    if (bar->is_io) {
        bar->base = (uint64_t)(original_low & ~3u);
        size = (uint64_t)(uint32_t)(~(mask_low & ~3u) + 1u);
    } else {
        uint32_t type = (original_low >> 1) & 3u;
        bar->prefetchable = (uint8_t)((original_low >> 3) & 1u);
        if (type == 0u) {
            bar->base = (uint64_t)(original_low & ~0xFu);
            size = (uint64_t)(uint32_t)(~(mask_low & ~0xFu) + 1u);
        } else if (type == 2u) {
            bar->is_64bit = 1u;
            bar->base = ((uint64_t)original_high << 32) |
                        (uint64_t)(original_low & ~0xFu);
            mask = ((uint64_t)mask_high << 32) |
                   (uint64_t)(mask_low & ~0xFu);
            size = ~mask + 1u;
        } else {
            return PROS_PCI_EUNSUPPORTED;
        }
    }

    if (size == 0u || (size & (size - 1u)) != 0u ||
        bar->base > UINT64_MAX - size)
        return PROS_PCI_EIO;
    bar->size = size;
    return PROS_PCI_OK;
}

int pci_get_bar(const struct pros_pci_device *device, uint8_t bar_index,
                struct pros_pci_bar *bar)
{
    uint32_t header_word = 0u, original_low = 0u, original_high = 0u;
    uint32_t mask_low = 0u, mask_high = 0u, command_word = 0u;
    uint32_t verify_word = 0u;
    uint16_t command = 0u;
    uint8_t offset, is_64bit = 0u, command_disabled = 0u;
    uint8_t low_written = 0u, high_written = 0u;
    uint8_t low_saved = 0u, high_saved = 0u;
    uint64_t flags;
    int rc, restore_rc = PROS_PCI_OK;

    if (!bar || bar_index >= 6u)
        return PROS_PCI_EINVAL;
    rc = pci_validate_device(device);
    if (rc != PROS_PCI_OK)
        return rc;

    /* BAR sizing is one indivisible config-space transaction. */
    flags = pci_config_lock();
    if (pci_config_read32_raw(device->bus, device->slot, device->function,
                              0x0Cu, &header_word) != PROS_PCI_OK) {
        rc = PROS_PCI_EIO;
        goto out;
    }
    if (((header_word >> 16) & 0x7Fu) > 1u) {
        rc = PROS_PCI_EUNSUPPORTED;
        goto out;
    }
    if (((header_word >> 16) & 0x7Fu) == 1u && bar_index >= 2u) {
        rc = PROS_PCI_EINVAL;
        goto out;
    }
    if (bar_index != 0u) {
        uint32_t previous_low;
        uint8_t previous_offset = (uint8_t)(0x10u + (bar_index - 1u) * 4u);
        if (pci_config_read32_raw(device->bus, device->slot, device->function,
                previous_offset, &previous_low) != PROS_PCI_OK) {
            rc = PROS_PCI_EIO;
            goto out;
        }
        if ((previous_low & 1u) == 0u && ((previous_low >> 1) & 3u) == 2u) {
            rc = PROS_PCI_EINVAL;
            goto out;
        }
    }

    offset = (uint8_t)(0x10u + bar_index * 4u);
    if (pci_config_read32_raw(device->bus, device->slot, device->function,
                              offset, &original_low) != PROS_PCI_OK) {
        rc = PROS_PCI_EIO;
        goto out;
    }
    if (original_low == 0u || original_low == 0xFFFFFFFFu) {
        rc = PROS_PCI_ENOSPC;
        goto out;
    }
    if ((original_low & 1u) == 0u && ((original_low >> 1) & 3u) == 2u) {
        if (bar_index >= 5u) {
            rc = PROS_PCI_EINVAL;
            goto out;
        }
        is_64bit = 1u;
        if (pci_config_read32_raw(device->bus, device->slot, device->function,
                (uint8_t)(offset + 4u), &original_high) != PROS_PCI_OK) {
            rc = PROS_PCI_EIO;
            goto out;
        }
    }
    if (pci_config_read32_raw(device->bus, device->slot, device->function,
                              0x04u, &command_word) != PROS_PCI_OK) {
        rc = PROS_PCI_EIO;
        goto out;
    }
    command = (uint16_t)command_word;
    if (pci_config_write32_raw(device->bus, device->slot, device->function,
            0x04u, (uint32_t)(command & (uint16_t)~(PCI_COMMAND_IO |
                         PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))) != PROS_PCI_OK) {
        rc = PROS_PCI_EIO;
        goto out;
    }
    command_disabled = 1u;
    if (pci_config_read32_raw(device->bus, device->slot, device->function,
                              0x04u, &verify_word) != PROS_PCI_OK ||
        ((uint16_t)verify_word & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                                  PCI_COMMAND_MASTER)) != 0u) {
        rc = PROS_PCI_EIO;
        goto restore;
    }

    if (pci_config_write32_raw(device->bus, device->slot, device->function,
                               offset, 0xFFFFFFFFu) != PROS_PCI_OK) {
        rc = PROS_PCI_EIO;
        goto restore;
    }
    low_written = 1u;
    if (is_64bit) {
        if (pci_config_write32_raw(device->bus, device->slot, device->function,
                (uint8_t)(offset + 4u), 0xFFFFFFFFu) != PROS_PCI_OK) {
            rc = PROS_PCI_EIO;
            goto restore;
        }
        high_written = 1u;
    }
    if (pci_config_read32_raw(device->bus, device->slot, device->function,
                              offset, &mask_low) != PROS_PCI_OK) {
        rc = PROS_PCI_EIO;
        goto restore;
    }
    if (is_64bit && pci_config_read32_raw(device->bus, device->slot,
            device->function, (uint8_t)(offset + 4u), &mask_high) != PROS_PCI_OK) {
        rc = PROS_PCI_EIO;
        goto restore;
    }
    rc = PROS_PCI_OK;

restore:
    /* Restore upper before lower for a 64-bit BAR, then restore command. */
    if (high_written) {
        if (pci_config_write32_raw(device->bus, device->slot, device->function,
                (uint8_t)(offset + 4u), original_high) != PROS_PCI_OK)
            restore_rc = PROS_PCI_EIO;
        else
            high_saved = 1u;
    }
    if (low_written) {
        if (pci_config_write32_raw(device->bus, device->slot, device->function,
                offset, original_low) != PROS_PCI_OK)
            restore_rc = PROS_PCI_EIO;
        else
            low_saved = 1u;
    }
    if (command_disabled) {
        if (pci_config_write32_raw(device->bus, device->slot, device->function,
                0x04u, (uint32_t)command) != PROS_PCI_OK) {
            restore_rc = PROS_PCI_EIO;
        } else if (pci_config_read32_raw(device->bus, device->slot,
                       device->function, 0x04u, &verify_word) != PROS_PCI_OK ||
                   (uint16_t)verify_word != command) {
            restore_rc = PROS_PCI_EIO;
        }
    }
    if (low_written &&
        (pci_config_read32_raw(device->bus, device->slot, device->function,
                               offset, &verify_word) != PROS_PCI_OK ||
         verify_word != original_low))
        restore_rc = PROS_PCI_EIO;
    if (high_written &&
        (pci_config_read32_raw(device->bus, device->slot, device->function,
                (uint8_t)(offset + 4u), &verify_word) != PROS_PCI_OK ||
         verify_word != original_high))
        restore_rc = PROS_PCI_EIO;
    if (restore_rc != PROS_PCI_OK)
        rc = PROS_PCI_EIO;
    if (rc == PROS_PCI_OK && (!low_saved || (is_64bit && !high_saved)))
        rc = PROS_PCI_EIO;
    if (rc == PROS_PCI_OK)
        rc = pci_bar_decode(original_low, original_high, mask_low, mask_high, bar);
out:
    pci_config_unlock(flags);
    return rc;
}

int pci_enable_memory_space(const struct pros_pci_device *device)
{
    return pci_update_command_bits(device, PCI_COMMAND_MEMORY, 0u);
}

int pci_enable_bus_master(const struct pros_pci_device *device)
{
    return pci_update_command_bits(device, PCI_COMMAND_MASTER, 0u);
}

int pci_disable_bus_master(const struct pros_pci_device *device)
{
    uint16_t command;
    int rc = pci_update_command_bits(device, 0u, PCI_COMMAND_MASTER);
    if (rc != PROS_PCI_OK)
        return rc;
    /* Do not report DMA isolation until the hardware bit reads back clear. */
    if (pci_read16(device->bus, device->slot, device->function, 0x04u,
                   &command) != PROS_PCI_OK)
        return PROS_PCI_EIO;
    return (command & PCI_COMMAND_MASTER) == 0u ?
           PROS_PCI_OK : PROS_PCI_EIO;
}

int pci_map_mmio_bar(const struct pros_pci_device *device, uint8_t bar_index,
                     struct pros_pci_mmio_bar *mapped)
{
    struct pros_pci_bar bar;
    uint64_t virtual_address = 0u;
    int rc;

    if (!mapped)
        return PROS_PCI_EINVAL;
    mapped->physical_address = 0u;
    mapped->virtual_address = 0u;
    mapped->size = 0u;
    if (!device || bar_index >= 6u)
        return PROS_PCI_EINVAL;

    rc = pci_get_bar(device, bar_index, &bar);
    if (rc != PROS_PCI_OK)
        return rc;
    if (bar.is_io || bar.base == 0u || bar.size == 0u)
        return PROS_PCI_EUNSUPPORTED;

    /* Map the complete BAR uncached before enabling device memory decode. */
    if (paging_map_mmio_region(bar.base, bar.size, &virtual_address) != 0)
        return PROS_PCI_EIO;
    rc = pci_enable_memory_space(device);
    if (rc != PROS_PCI_OK) {
        paging_unmap_mmio_region(virtual_address, bar.size);
        return rc;
    }

    mapped->physical_address = bar.base;
    mapped->virtual_address = virtual_address;
    mapped->size = bar.size;
    return PROS_PCI_OK;
}

void pci_unmap_mmio_bar(struct pros_pci_mmio_bar *mapped)
{
    if (!mapped || mapped->virtual_address == 0u || mapped->size == 0u)
        return;
    paging_unmap_mmio_region(mapped->virtual_address, mapped->size);
    mapped->physical_address = 0u;
    mapped->virtual_address = 0u;
    mapped->size = 0u;
}

int pci_enable_memory_and_bus_master(const struct pros_pci_device *device)
{
    return pci_update_command_bits(device,
                                   PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER,
                                   0u);
}

int pci_scan(struct pros_pci_device *devices, uint32_t capacity,
             uint32_t *count)
{
    uint32_t found = 0u;

    if (!devices || !count || capacity == 0u)
        return PROS_PCI_EINVAL;

    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint32_t slot = 0u; slot < 32u; ++slot) {
            uint16_t vendor;
            if (pci_read16((uint8_t)bus, (uint8_t)slot, 0u, 0u, &vendor) !=
                    PROS_PCI_OK)
                return PROS_PCI_EINVAL;
            if (vendor == 0xFFFFu)
                continue;

            uint32_t header_word;
            if (pci_config_read32((uint8_t)bus, (uint8_t)slot, 0u, 0x0Cu,
                                  &header_word) != PROS_PCI_OK)
                return PROS_PCI_EINVAL;
            uint8_t header_type = (uint8_t)((header_word >> 16) & 0xFFu);
            uint32_t functions = (header_type & 0x80u) ? 8u : 1u;

            for (uint32_t function = 0u; function < functions; ++function) {
                uint32_t id;
                if (pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                      (uint8_t)function, 0u, &id) !=
                        PROS_PCI_OK)
                    return PROS_PCI_EINVAL;
                if ((uint16_t)(id & 0xFFFFu) == 0xFFFFu)
                    continue;

                uint32_t class_word;
                if (pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                      (uint8_t)function, 0x08u, &class_word) !=
                        PROS_PCI_OK)
                    return PROS_PCI_EINVAL;

                if (found >= capacity)
                    return PROS_PCI_EINVAL;

                devices[found].bus = (uint8_t)bus;
                devices[found].slot = (uint8_t)slot;
                devices[found].function = (uint8_t)function;
                devices[found].vendor_id = (uint16_t)(id & 0xFFFFu);
                devices[found].device_id = (uint16_t)(id >> 16);
                devices[found].class_code = (uint8_t)(class_word >> 24);
                devices[found].subclass = (uint8_t)(class_word >> 16);
                devices[found].prog_if = (uint8_t)(class_word >> 8);
                devices[found].header_type = header_type;
                ++found;
            }
        }
    }

    *count = found;
    return PROS_PCI_OK;
}

int pci_usb_controller_type(const struct pros_pci_device *device,
                            uint8_t *controller_type)
{
    if (!device || !controller_type)
        return PROS_PCI_EINVAL;
    if (device->class_code != 0x0Cu || device->subclass != 0x03u)
        return PROS_PCI_EUNSUPPORTED;

    switch (device->prog_if) {
    case 0x00u: *controller_type = PROS_PCI_USB_UHCI; return PROS_PCI_OK;
    case 0x10u: *controller_type = PROS_PCI_USB_OHCI; return PROS_PCI_OK;
    case 0x20u: *controller_type = PROS_PCI_USB_EHCI; return PROS_PCI_OK;
    case 0x30u: *controller_type = PROS_PCI_USB_XHCI; return PROS_PCI_OK;
    default: return PROS_PCI_EUNSUPPORTED;
    }
}

int pci_selftest(void)
{
    struct pros_pci_bar bar;
    struct pros_pci_device usb;
    struct pros_pci_mmio_bar mapped;
    uint8_t controller_type = 0u;

    usb.class_code = 0x0Cu;
    usb.subclass = 0x03u;
    usb.prog_if = 0x30u;
    if (pci_usb_controller_type(&usb, &controller_type) != PROS_PCI_OK ||
        controller_type != PROS_PCI_USB_XHCI)
        return -6;
    usb.prog_if = 0x20u;
    if (pci_usb_controller_type(&usb, &controller_type) != PROS_PCI_OK ||
        controller_type != PROS_PCI_USB_EHCI)
        return -7;
    usb.prog_if = 0x10u;
    if (pci_usb_controller_type(&usb, &controller_type) != PROS_PCI_OK ||
        controller_type != PROS_PCI_USB_OHCI)
        return -8;
    usb.prog_if = 0x00u;
    if (pci_usb_controller_type(&usb, &controller_type) != PROS_PCI_OK ||
        controller_type != PROS_PCI_USB_UHCI)
        return -9;
    usb.prog_if = 0x40u;
    if (pci_usb_controller_type(&usb, &controller_type) !=
            PROS_PCI_EUNSUPPORTED)
        return -10;
    usb.class_code = 0x01u;
    if (pci_usb_controller_type(&usb, &controller_type) !=
            PROS_PCI_EUNSUPPORTED)
        return -11;
    if (pci_usb_controller_type(0, &controller_type) != PROS_PCI_EINVAL ||
        pci_usb_controller_type(&usb, 0) != PROS_PCI_EINVAL)
        return -12;
    if (pci_map_mmio_bar(0, 0u, &mapped) != PROS_PCI_EINVAL ||
        pci_map_mmio_bar(&usb, 6u, &mapped) != PROS_PCI_EINVAL)
        return -13;
    pci_unmap_mmio_bar(&mapped);
    if (mapped.physical_address != 0u || mapped.virtual_address != 0u ||
        mapped.size != 0u)
        return -14;
    if (pci_command_update_bits(PCI_COMMAND_IO | PCI_COMMAND_MEMORY,
            PCI_COMMAND_MASTER, 0u) !=
            (PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER) ||
        pci_command_update_bits(PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
            PCI_COMMAND_MASTER, 0u, PCI_COMMAND_MASTER) !=
            (PCI_COMMAND_IO | PCI_COMMAND_MEMORY) ||
        pci_command_update_bits(0xA5A1u, 0u, PCI_COMMAND_MASTER) !=
            (uint16_t)(0xA5A1u & (uint16_t)~PCI_COMMAND_MASTER))
        return -15;
    {
        uint64_t visited = 0u;
        uint8_t next = 0u;
        if (pci_capability_advance(0x40u, 0x48u, &visited, &next) !=
                PROS_PCI_OK || next != 0x48u ||
            pci_capability_advance(0x48u, 0u, &visited, &next) !=
                PROS_PCI_OK || next != 0u ||
            pci_capability_advance(0x40u, 0u, &visited, &next) !=
                PROS_PCI_EIO)
            return -16;
        visited = 0u;
        if (pci_capability_advance(0x40u, 0x48u, &visited, &next) !=
                PROS_PCI_OK || next != 0x48u ||
            pci_capability_advance(0x48u, 0x40u, &visited, &next) !=
                PROS_PCI_EIO)
            return -17;
        visited = 0u;
        if (pci_capability_advance(0x41u, 0u, &visited, &next) !=
                PROS_PCI_EIO ||
            pci_capability_advance(0x40u, 0x3Cu, &visited, &next) !=
                PROS_PCI_EIO ||
            pci_capability_advance(0x40u, 0xFDu, &visited, &next) !=
                PROS_PCI_EIO)
            return -18;
    }
    {
        struct pros_pci_interrupt_caps caps;
        pci_decode_msi_control((uint16_t)(1u | (3u << 1) | (1u << 7) |
                                          (1u << 8)), &caps);
        if (!caps.msi_present || !caps.msi_enabled || !caps.msi_64bit ||
            !caps.msi_mask_capable || caps.msi_max_vectors != 8u)
            return -19;
        if (pci_decode_msix_registers((uint16_t)(1u << 15) |
                                      (1u << 14) | 0x0003u,
                                      0x00001202u, 0x00003404u,
                                      &caps) != PROS_PCI_OK ||
            !caps.msix_present || !caps.msix_enabled ||
            !caps.msix_function_mask || caps.msix_table_size != 4u ||
            caps.msix_table_bir != 2u ||
            caps.msix_table_offset != 0x1200u ||
            caps.msix_pba_bir != 4u ||
            caps.msix_pba_offset != 0x3400u)
            return -20;
        if (pci_decode_msix_registers(0u, 0x1006u, 0x2000u, &caps) !=
                PROS_PCI_EIO ||
            pci_decode_msix_registers(0u, 0x1000u, 0x2007u, &caps) !=
                PROS_PCI_EIO)
            return -21;
    }
    if (pci_config_address(2u, 3u, 1u, 0x14u) != 0x80021914u)
        return -1;
    if (pci_config_address(255u, 31u, 7u, 0xFCu) != 0x80FFFFFCu)
        return -1;

    if (pci_bar_decode(0xFEBF0008u, 0u, 0xFFFFF008u, 0u, &bar) !=
            PROS_PCI_OK ||
        bar.base != 0xFEBF0000u || bar.size != 0x1000u ||
        bar.is_io || bar.is_64bit || !bar.prefetchable)
        return -2;
    if (pci_bar_decode(0x0000000Cu, 0x00000012u,
                       0xFFFFE00Cu, 0xFFFFFFFFu, &bar) != PROS_PCI_OK ||
        bar.base != 0x1200000000ULL || bar.size != 0x2000u ||
        bar.is_io || !bar.is_64bit || !bar.prefetchable)
        return -3;
    if (pci_bar_decode(0x0000C001u, 0u, 0xFFFFFF01u, 0u, &bar) !=
            PROS_PCI_OK ||
        bar.base != 0xC000u || bar.size != 0x100u || !bar.is_io)
        return -4;
    if (pci_bar_decode(0x00000002u, 0u, 0xFFFFF002u, 0u, &bar) !=
            PROS_PCI_EUNSUPPORTED)
        return -5;
    return PROS_PCI_OK;
}

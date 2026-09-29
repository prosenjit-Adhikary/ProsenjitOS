#include "ata_pio.h"

#define ATA_PRIMARY_IO       0x1F0u
#define ATA_PRIMARY_CONTROL  0x3F6u
#define ATA_SECONDARY_IO     0x170u
#define ATA_SECONDARY_CONTROL 0x376u

#define ATA_REG_DATA         0u
#define ATA_REG_ERROR        1u
#define ATA_REG_FEATURES     1u
#define ATA_REG_SECTOR_COUNT 2u
#define ATA_REG_LBA_LOW      3u
#define ATA_REG_LBA_MID      4u
#define ATA_REG_LBA_HIGH     5u
#define ATA_REG_DRIVE        6u
#define ATA_REG_STATUS       7u
#define ATA_REG_COMMAND      7u

#define ATA_CMD_READ_PIO     0x20u
#define ATA_CMD_WRITE_PIO    0x30u
#define ATA_CMD_CACHE_FLUSH  0xE7u
#define ATA_CMD_IDENTIFY     0xECu

#define ATA_STATUS_ERR       0x01u
#define ATA_STATUS_DRQ       0x08u
#define ATA_STATUS_DF        0x20u
#define ATA_STATUS_BSY       0x80u

#define ATA_TIMEOUT 1000000u
#define ATA_MAX_LBA 0x0FFFFFFFULL

static inline void ata_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t ata_in8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void ata_out16(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t ata_in16(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void ata_delay_400ns(uint16_t control)
{
    (void)ata_in8(control);
    (void)ata_in8(control);
    (void)ata_in8(control);
    (void)ata_in8(control);
}

static int ata_wait_not_busy(struct pros_ata_pio *ata)
{
    uint8_t status = 0u;
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        status = ata_in8(ata->io_base + ATA_REG_STATUS);
        if ((status & ATA_STATUS_BSY) == 0u)
            return (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) ? PROS_ATA_EIO
                                                                : PROS_ATA_OK;
    }
    return PROS_ATA_EIO;
}

static int ata_wait_drq(struct pros_ata_pio *ata)
{
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        uint8_t status = ata_in8(ata->io_base + ATA_REG_STATUS);
        if ((status & ATA_STATUS_BSY) != 0u)
            continue;
        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0u)
            return PROS_ATA_EIO;
        if ((status & ATA_STATUS_DRQ) != 0u)
            return PROS_ATA_OK;
    }
    return PROS_ATA_EIO;
}

static int ata_select(struct pros_ata_pio *ata, uint32_t lba)
{
    uint8_t drive = (uint8_t)(0xE0u | ((ata->drive & 1u) << 4) |
                              ((lba >> 24) & 0x0Fu));
    ata_out8(ata->io_base + ATA_REG_DRIVE, drive);
    ata_delay_400ns(ata->control_base);
    return ata_wait_not_busy(ata);
}

static int ata_pio_read(struct pros_block_device *device,
                        uint64_t lba, uint32_t count, void *buffer)
{
    struct pros_ata_pio *ata;
    uint8_t *dst = (uint8_t *)buffer;

    if (!device || !buffer || count == 0u || device->private_data == 0)
        return PROS_BLOCK_EINVAL;
    ata = (struct pros_ata_pio *)device->private_data;
    if (lba > ATA_MAX_LBA || count > ATA_MAX_LBA + 1u - lba)
        return PROS_BLOCK_EINVAL;

    for (uint32_t sector = 0u; sector < count; ++sector) {
        uint32_t current = (uint32_t)(lba + sector);
        if (ata_select(ata, current) != PROS_ATA_OK)
            return PROS_BLOCK_EIO;
        ata_out8(ata->io_base + ATA_REG_FEATURES, 0u);
        ata_out8(ata->io_base + ATA_REG_SECTOR_COUNT, 1u);
        ata_out8(ata->io_base + ATA_REG_LBA_LOW, (uint8_t)current);
        ata_out8(ata->io_base + ATA_REG_LBA_MID, (uint8_t)(current >> 8));
        ata_out8(ata->io_base + ATA_REG_LBA_HIGH, (uint8_t)(current >> 16));
        ata_out8(ata->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
        if (ata_wait_drq(ata) != PROS_ATA_OK)
            return PROS_BLOCK_EIO;
        for (uint32_t word = 0u; word < 256u; ++word)
            ((uint16_t *)dst)[word] = ata_in16(ata->io_base + ATA_REG_DATA);
        dst += 512u;
    }
    return PROS_BLOCK_OK;
}

static int ata_pio_write(struct pros_block_device *device,
                         uint64_t lba, uint32_t count, const void *buffer)
{
    struct pros_ata_pio *ata;
    const uint8_t *src = (const uint8_t *)buffer;

    if (!device || !buffer || count == 0u || device->private_data == 0)
        return PROS_BLOCK_EINVAL;
    ata = (struct pros_ata_pio *)device->private_data;
    if (lba > ATA_MAX_LBA || count > ATA_MAX_LBA + 1u - lba)
        return PROS_BLOCK_EINVAL;

    for (uint32_t sector = 0u; sector < count; ++sector) {
        uint32_t current = (uint32_t)(lba + sector);
        if (ata_select(ata, current) != PROS_ATA_OK)
            return PROS_BLOCK_EIO;
        ata_out8(ata->io_base + ATA_REG_FEATURES, 0u);
        ata_out8(ata->io_base + ATA_REG_SECTOR_COUNT, 1u);
        ata_out8(ata->io_base + ATA_REG_LBA_LOW, (uint8_t)current);
        ata_out8(ata->io_base + ATA_REG_LBA_MID, (uint8_t)(current >> 8));
        ata_out8(ata->io_base + ATA_REG_LBA_HIGH, (uint8_t)(current >> 16));
        ata_out8(ata->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
        if (ata_wait_drq(ata) != PROS_ATA_OK)
            return PROS_BLOCK_EIO;
        for (uint32_t word = 0u; word < 256u; ++word)
            ata_out16(ata->io_base + ATA_REG_DATA, ((const uint16_t *)src)[word]);
        if (ata_wait_not_busy(ata) != PROS_ATA_OK)
            return PROS_BLOCK_EIO;
        src += 512u;
    }
    return PROS_BLOCK_OK;
}

static int ata_pio_flush(struct pros_block_device *device)
{
    struct pros_ata_pio *ata;
    if (!device || !device->private_data)
        return PROS_BLOCK_EINVAL;
    ata = (struct pros_ata_pio *)device->private_data;
    if (ata_wait_not_busy(ata) != PROS_ATA_OK)
        return PROS_BLOCK_EIO;
    ata_out8(ata->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy(ata);
}

static int ata_identify_sector_count(struct pros_ata_pio *ata, uint64_t *sector_count)
{
    uint16_t identify[256];
    if (!ata || !sector_count)
        return PROS_ATA_EINVAL;
    if (ata_select(ata, 0u) != PROS_ATA_OK)
        return PROS_ATA_EIO;
    ata_out8(ata->io_base + ATA_REG_FEATURES, 0u);
    ata_out8(ata->io_base + ATA_REG_SECTOR_COUNT, 0u);
    ata_out8(ata->io_base + ATA_REG_LBA_LOW, 0u);
    ata_out8(ata->io_base + ATA_REG_LBA_MID, 0u);
    ata_out8(ata->io_base + ATA_REG_LBA_HIGH, 0u);
    ata_out8(ata->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    if (ata_wait_drq(ata) != PROS_ATA_OK)
        return PROS_ATA_EIO;
    for (uint32_t i = 0u; i < 256u; ++i)
        identify[i] = ata_in16(ata->io_base + ATA_REG_DATA);
    *sector_count = (uint64_t)identify[60] |
                    ((uint64_t)identify[61] << 16);
    return *sector_count == 0u ? PROS_ATA_EIO : PROS_ATA_OK;
}

int ata_pio_init(struct pros_ata_pio *ata, uint8_t channel, uint8_t drive)
{
    uint8_t status;
    uint16_t io_base;
    uint16_t control_base;
    uint64_t sector_count;

    if (!ata || channel > 1u || drive > 1u)
        return PROS_ATA_EINVAL;

    io_base = channel == 0u ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
    control_base = channel == 0u ? ATA_PRIMARY_CONTROL : ATA_SECONDARY_CONTROL;

    ata->io_base = io_base;
    ata->control_base = control_base;
    ata->drive = drive;

    ata_out8(io_base + ATA_REG_DRIVE, (uint8_t)(0xE0u | (drive << 4)));
    ata_delay_400ns(control_base);
    status = ata_in8(io_base + ATA_REG_STATUS);
    if (status == 0xFFu || status == 0x00u)
        return PROS_ATA_ENODEV;
    if (ata_wait_not_busy(ata) != PROS_ATA_OK)
        return PROS_ATA_EIO;
    if (ata_identify_sector_count(ata, &sector_count) != PROS_ATA_OK ||
        sector_count > (uint64_t)ATA_MAX_LBA + 1u)
        return PROS_ATA_EIO;

    if (block_device_init(&ata->device, 512u, sector_count,
                          ata_pio_read, ata_pio_write, ata) != PROS_BLOCK_OK)
        return PROS_ATA_EINVAL;
    if (block_device_set_flush(&ata->device, ata_pio_flush) != PROS_BLOCK_OK)
        return PROS_ATA_EINVAL;
    return PROS_ATA_OK;
}

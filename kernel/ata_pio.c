#include "ata_pio.h"

#define ATA_PRIMARY_BASE 0x1F0u
#define ATA_PRIMARY_CTRL 0x3F6u

#define ATA_REG_DATA      0u
#define ATA_REG_ERROR     1u
#define ATA_REG_FEATURES  1u
#define ATA_REG_SECCOUNT0 2u
#define ATA_REG_LBA0      3u
#define ATA_REG_LBA1      4u
#define ATA_REG_LBA2      5u
#define ATA_REG_HDDEVSEL  6u
#define ATA_REG_STATUS    7u
#define ATA_REG_COMMAND   7u

#define ATA_CMD_IDENTIFY       0xECu
#define ATA_CMD_READ_PIO       0x20u
#define ATA_CMD_READ_PIO_EXT   0x24u
#define ATA_CMD_WRITE_PIO      0x30u
#define ATA_CMD_WRITE_PIO_EXT  0x34u
#define ATA_CMD_CACHE_FLUSH    0xE7u
#define ATA_CMD_CACHE_FLUSH_EXT 0xEAu

#define ATA_SR_ERR  0x01u
#define ATA_SR_DRQ  0x08u
#define ATA_SR_SRV  0x10u
#define ATA_SR_DF   0x20u
#define ATA_SR_RDY  0x40u
#define ATA_SR_BSY  0x80u

#define ATA_TIMEOUT 1000000u

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

static inline uint16_t ata_in16(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void ata_out16(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void ata_delay400ns(uint16_t ctrl)
{
    (void)ata_in8(ctrl);
    (void)ata_in8(ctrl);
    (void)ata_in8(ctrl);
    (void)ata_in8(ctrl);
}

static int ata_wait_not_busy(const struct pros_ata_pio *ata)
{
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i)
        if ((ata_in8(ata->io_base + ATA_REG_STATUS) & ATA_SR_BSY) == 0u)
            return PROS_ATA_OK;
    return PROS_ATA_ETIMEOUT;
}

static int ata_wait_drq(const struct pros_ata_pio *ata)
{
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        uint8_t status = ata_in8(ata->io_base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR)
            return PROS_ATA_EIO;
        if (status & ATA_SR_DF)
            return PROS_ATA_EIO;
        if (status & ATA_SR_DRQ)
            return PROS_ATA_OK;
        if (status & ATA_SR_BSY)
            continue;
        if ((status & ATA_SR_RDY) == 0u)
            continue;
    }
    return PROS_ATA_ETIMEOUT;
}

static void ata_select(const struct pros_ata_pio *ata, uint32_t lba)
{
    ata_out8(ata->io_base + ATA_REG_HDDEVSEL,
             (uint8_t)(0xE0u | (ata->drive ? 0x10u : 0u) |
                       ((lba >> 24) & 0x0Fu)));
    ata_delay400ns(ata->ctrl_base);
}

static int ata_identify(struct pros_ata_pio *ata)
{
    uint16_t identify[256];
    uint8_t status;
    uint32_t lba28_sectors;
    uint64_t lba48_sectors;

    ata_out8(ata->io_base + ATA_REG_HDDEVSEL,
             (uint8_t)(0xA0u | (ata->drive ? 0x10u : 0u)));
    ata_delay400ns(ata->ctrl_base);
    ata_out8(ata->io_base + ATA_REG_SECCOUNT0, 0u);
    ata_out8(ata->io_base + ATA_REG_LBA0, 0u);
    ata_out8(ata->io_base + ATA_REG_LBA1, 0u);
    ata_out8(ata->io_base + ATA_REG_LBA2, 0u);
    ata_out8(ata->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    status = ata_in8(ata->io_base + ATA_REG_STATUS);
    if (status == 0u)
        return PROS_ATA_ENODEV;
    if (status & ATA_SR_ERR)
        return PROS_ATA_ENOTSUP;
    if (ata_wait_drq(ata) != PROS_ATA_OK)
        return PROS_ATA_EIO;

    for (uint32_t i = 0u; i < 256u; ++i)
        identify[i] = ata_in16(ata->io_base + ATA_REG_DATA);

    /* Word 0 bit 15 identifies non-ATA packet devices. */
    if (identify[0] & 0x8000u)
        return PROS_ATA_ENOTSUP;

    lba28_sectors = (uint32_t)identify[60] |
                    ((uint32_t)identify[61] << 16);
    lba48_sectors = (uint64_t)identify[100] |
                    ((uint64_t)identify[101] << 16) |
                    ((uint64_t)identify[102] << 32) |
                    ((uint64_t)identify[103] << 48);

    if (lba48_sectors != 0u && (identify[83] & (1u << 10)) != 0u) {
        ata->lba48 = 1u;
        ata->block.sector_count = lba48_sectors;
    } else if (lba28_sectors != 0u) {
        ata->lba48 = 0u;
        ata->block.sector_count = lba28_sectors;
    } else {
        return PROS_ATA_ENOTSUP;
    }

    ata->block.sector_size = 512u;
    return PROS_ATA_OK;
}

static void ata_trace_record(struct pros_ata_pio *ata, uint64_t lba,
                              uint32_t count, uint8_t write)
{
    struct pros_ata_trace_event *event;
    if (!ata)
        return;
    event = &ata->trace[ata->trace_head];
    event->lba = lba;
    event->count = count;
    event->write = write;
    ata->trace_head = (ata->trace_head + 1u) % PROS_ATA_TRACE_DEPTH;
    if (ata->trace_count < PROS_ATA_TRACE_DEPTH)
        ++ata->trace_count;
}

void pros_ata_pio_trace_reset(struct pros_ata_pio *ata)
{
    if (!ata)
        return;
    ata->trace_count = 0u;
    ata->trace_head = 0u;
}

int pros_ata_pio_trace_find(const struct pros_ata_pio *ata, uint64_t lba,
                            uint8_t write, uint64_t *trace_lba,
                            uint32_t *trace_count)
{
    if (!ata || !trace_lba || !trace_count)
        return PROS_ATA_EINVAL;
    for (uint32_t n = 0u; n < ata->trace_count; ++n) {
        uint32_t index = (ata->trace_head + PROS_ATA_TRACE_DEPTH - 1u - n) %
                         PROS_ATA_TRACE_DEPTH;
        const struct pros_ata_trace_event *event = &ata->trace[index];
        if (event->write == write && lba >= event->lba &&
            lba - event->lba < event->count) {
            *trace_lba = event->lba;
            *trace_count = event->count;
            return PROS_ATA_OK;
        }
    }
    return PROS_ATA_ENOTSUP;
}

static int ata_prepare(struct pros_ata_pio *ata, uint64_t lba,
                       uint32_t count, uint8_t command)
{
    if (!ata || count == 0u || count > 256u ||
        !block_device_lba_range_valid(&ata->block, lba, count))
        return PROS_ATA_EINVAL;

    if (!ata->lba48 && lba > 0x0FFFFFFFu)
        return PROS_ATA_ENOTSUP;

    if (ata->lba48) {
        ata_out8(ata->io_base + ATA_REG_HDDEVSEL,
                 (uint8_t)(0x40u | (ata->drive ? 0x10u : 0u)));
        ata_delay400ns(ata->ctrl_base);

        ata_out8(ata->io_base + ATA_REG_SECCOUNT0, (uint8_t)(count >> 8));
        ata_out8(ata->io_base + ATA_REG_LBA0, (uint8_t)(lba >> 24));
        ata_out8(ata->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 32));
        ata_out8(ata->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 40));

        ata_out8(ata->io_base + ATA_REG_SECCOUNT0, (uint8_t)(count & 0xFFu));
        ata_out8(ata->io_base + ATA_REG_LBA0, (uint8_t)lba);
        ata_out8(ata->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        ata_out8(ata->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    } else {
        ata_select(ata, (uint32_t)lba);
        ata_out8(ata->io_base + ATA_REG_SECCOUNT0,
                 (uint8_t)(count == 256u ? 0u : count));
        ata_out8(ata->io_base + ATA_REG_LBA0, (uint8_t)lba);
        ata_out8(ata->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        ata_out8(ata->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    }

    ata_out8(ata->io_base + ATA_REG_COMMAND, command);
    return PROS_ATA_OK;
}

static int ata_read(struct pros_block_device *device, uint64_t lba,
                     uint32_t count, void *buffer)
{
    struct pros_ata_pio *ata = (struct pros_ata_pio *)device->private_data;
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t remaining = count;

    if (!ata || !buffer || device->sector_size != 512u)
        return PROS_BLOCK_EINVAL;

    while (remaining != 0u) {
        uint32_t chunk = remaining > 256u ? 256u : remaining;
        ata_trace_record(ata, lba, chunk, 0u);
        int rc = ata_prepare(ata, lba, chunk,
                             ata->lba48 ? ATA_CMD_READ_PIO_EXT :
                                          ATA_CMD_READ_PIO);
        if (rc != PROS_ATA_OK)
            return PROS_BLOCK_EIO;

        for (uint32_t s = 0u; s < chunk; ++s) {
            rc = ata_wait_drq(ata);
            if (rc != PROS_ATA_OK)
                return PROS_BLOCK_EIO;
            for (uint32_t w = 0u; w < 256u; ++w)
                ((uint16_t *)(void *)dst)[w] =
                    ata_in16(ata->io_base + ATA_REG_DATA);
            dst += 512u;
        }
        remaining -= chunk;
        /*
         * Do not compute a one-past-the-end LBA after the final chunk.
         * block_device_lba_add() deliberately rejects results outside the
         * device, so advancing after a valid final sector would turn a
         * successful read/write ending at the last sector into an I/O error.
         */
        if (remaining != 0u) {
            uint64_t next_lba;
            if (block_device_lba_add(device, lba, chunk, &next_lba) !=
                    PROS_BLOCK_OK)
                return PROS_BLOCK_EIO;
            lba = next_lba;
        }
    }
    return PROS_BLOCK_OK;
}

static int ata_write(struct pros_block_device *device, uint64_t lba,
                      uint32_t count, const void *buffer)
{
    struct pros_ata_pio *ata = (struct pros_ata_pio *)device->private_data;
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t remaining = count;

    if (!ata || !buffer || device->sector_size != 512u)
        return PROS_BLOCK_EINVAL;

    while (remaining != 0u) {
        uint32_t chunk = remaining > 256u ? 256u : remaining;
        ata_trace_record(ata, lba, chunk, 1u);
        int rc = ata_prepare(ata, lba, chunk,
                             ata->lba48 ? ATA_CMD_WRITE_PIO_EXT :
                                          ATA_CMD_WRITE_PIO);
        if (rc != PROS_ATA_OK)
            return PROS_BLOCK_EIO;

        for (uint32_t s = 0u; s < chunk; ++s) {
            rc = ata_wait_drq(ata);
            if (rc != PROS_ATA_OK)
                return PROS_BLOCK_EIO;
            for (uint32_t w = 0u; w < 256u; ++w)
                ata_out16(ata->io_base + ATA_REG_DATA,
                          ((const uint16_t *)(const void *)src)[w]);
            src += 512u;
        }

        /* Wait for the device to finish the PIO write before issuing the
         * next command. The final data word only completes the host transfer;
         * the device may still have BSY asserted while committing the sector. */
        rc = ata_wait_not_busy(ata);
        if (rc != PROS_ATA_OK)
            return PROS_BLOCK_EIO;
        {
            uint8_t status = ata_in8(ata->io_base + ATA_REG_STATUS);
            if (status & (ATA_SR_ERR | ATA_SR_DF))
                return PROS_BLOCK_EIO;
        }

        remaining -= chunk;
        /*
         * Do not compute a one-past-the-end LBA after the final chunk.
         * block_device_lba_add() deliberately rejects results outside the
         * device, so advancing after a valid final sector would turn a
         * successful read/write ending at the last sector into an I/O error.
         */
        if (remaining != 0u) {
            uint64_t next_lba;
            if (block_device_lba_add(device, lba, chunk, &next_lba) !=
                    PROS_BLOCK_OK)
                return PROS_BLOCK_EIO;
            lba = next_lba;
        }
    }

    return PROS_BLOCK_OK;
}

static int ata_flush(struct pros_block_device *device)
{
    struct pros_ata_pio *ata = (struct pros_ata_pio *)device->private_data;
    if (!ata)
        return PROS_BLOCK_EINVAL;
    ata_out8(ata->io_base + ATA_REG_COMMAND,
             ata->lba48 ? ATA_CMD_CACHE_FLUSH_EXT : ATA_CMD_CACHE_FLUSH);
    if (ata_wait_not_busy(ata) != PROS_ATA_OK)
        return PROS_BLOCK_EIO;
    /* A flush can complete with BSY clear but still report device failure. */
    {
        uint8_t status = ata_in8(ata->io_base + ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF))
            return PROS_BLOCK_EIO;
    }
    return PROS_BLOCK_OK;
}

int pros_ata_pio_probe(struct pros_ata_pio *ata)
{
    int rc;
    if (!ata)
        return PROS_ATA_EINVAL;

    ata->io_base = ATA_PRIMARY_BASE;
    ata->ctrl_base = ATA_PRIMARY_CTRL;
    ata->drive = 0u;
    ata->lba48 = 0u;
    pros_ata_pio_trace_reset(ata);

    rc = block_device_init(&ata->block, 512u, 1u,
                           ata_read, ata_write, ata);
    if (rc != PROS_BLOCK_OK)
        return PROS_ATA_EIO;

    rc = ata_identify(ata);
    if (rc != PROS_ATA_OK)
        return rc;

    return block_device_set_flush(&ata->block, ata_flush) == PROS_BLOCK_OK
        ? PROS_ATA_OK : PROS_ATA_EIO;
}

int pros_ata_pio_selftest(void)
{
    struct pros_ata_pio ata;
    uint8_t sector[512];

    int rc = pros_ata_pio_probe(&ata);
    if (rc != PROS_ATA_OK)
        return rc;

    if (block_device_read(&ata.block, 0u, 1u, sector) != PROS_BLOCK_OK)
        return PROS_ATA_EIO;

    /*
     * Regression for a valid request that ends exactly at the device boundary.
     * The last sector must be readable; advancing to the one-past-end LBA after
     * the final chunk is invalid and must not turn this successful read into EIO.
     */
    if (ata.block.sector_count == 0u ||
        block_device_read(&ata.block, ata.block.sector_count - 1u, 1u,
                          sector) != PROS_BLOCK_OK)
        return PROS_ATA_EIO;

    /* A real disk read must provide the conventional MBR signature or a
     * non-partitioned volume may be present. Do not guess a filesystem. */
    if ((sector[510] != 0x55u || sector[511] != 0xAAu) &&
        !(sector[0] == 0u && sector[1] == 0u))
        return PROS_ATA_EIO;

    return PROS_ATA_OK;
}

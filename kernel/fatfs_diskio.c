#include <stdint.h>
#include "fatfs_diskio.h"
#include "ff.h"
#include "diskio.h"

struct fatfs_disk_slot {
    struct pros_block_device *device;
    uint8_t read_only;
    uint8_t state; /* 0=free, 1=published, 2=writer owns slot */
};

static struct fatfs_disk_slot disk_slots[PROS_FATFS_DISK_MAX];

static struct pros_block_device *disk_slot_device(BYTE pdrv)
{
    if (pdrv >= PROS_FATFS_DISK_MAX ||
        __atomic_load_n(&disk_slots[pdrv].state, __ATOMIC_ACQUIRE) != 1u)
        return 0;
    return disk_slots[pdrv].device;
}

int pros_fatfs_disk_bind(uint8_t pdrv, struct pros_block_device *device,
                         uint8_t read_only)
{
    uint8_t expected = 0u;
    if (pdrv >= PROS_FATFS_DISK_MAX || !device || !device->read ||
        device->sector_count == 0u ||
        (device->sector_size != 512u && device->sector_size != 1024u &&
         device->sector_size != 2048u && device->sector_size != 4096u) ||
        (!read_only && !device->write))
        return PROS_BLOCK_EINVAL;
    if (!__atomic_compare_exchange_n(&disk_slots[pdrv].state, &expected, 2u,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return PROS_BLOCK_EINVAL;
    disk_slots[pdrv].device = device;
    disk_slots[pdrv].read_only = read_only ? 1u : 0u;
    __atomic_store_n(&disk_slots[pdrv].state, 1u, __ATOMIC_RELEASE);
    return PROS_BLOCK_OK;
}

int pros_fatfs_disk_unbind(uint8_t pdrv)
{
    uint8_t expected = 1u;
    if (pdrv >= PROS_FATFS_DISK_MAX ||
        !__atomic_compare_exchange_n(&disk_slots[pdrv].state, &expected, 2u,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return PROS_BLOCK_EINVAL;
    disk_slots[pdrv].device = 0;
    disk_slots[pdrv].read_only = 0u;
    __atomic_store_n(&disk_slots[pdrv].state, 0u, __ATOMIC_RELEASE);
    return PROS_BLOCK_OK;
}

DSTATUS disk_status(BYTE pdrv)
{
    struct pros_block_device *device = disk_slot_device(pdrv);
    if (!device)
        return STA_NOINIT;
    return disk_slots[pdrv].read_only ? STA_PROTECT : 0u;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buffer, LBA_t sector, UINT count)
{
    struct pros_block_device *device = disk_slot_device(pdrv);
    if (!device)
        return RES_NOTRDY;
    if (!buffer || count == 0u)
        return RES_PARERR;
    if (sector > UINT64_MAX || (uint64_t)sector >= device->sector_count ||
        (uint64_t)count > device->sector_count - (uint64_t)sector)
        return RES_PARERR;
    return block_device_read(device, (uint64_t)sector, (uint32_t)count,
                             buffer) == PROS_BLOCK_OK ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buffer, LBA_t sector, UINT count)
{
    struct pros_block_device *device = disk_slot_device(pdrv);
    if (!device)
        return RES_NOTRDY;
    if (!buffer || count == 0u)
        return RES_PARERR;
    if (disk_slots[pdrv].read_only)
        return RES_WRPRT;
    if (!device->write || sector > UINT64_MAX ||
        (uint64_t)sector >= device->sector_count ||
        (uint64_t)count > device->sector_count - (uint64_t)sector)
        return RES_PARERR;
    return block_device_write(device, (uint64_t)sector, (uint32_t)count,
                              buffer) == PROS_BLOCK_OK ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE command, void *buffer)
{
    struct pros_block_device *device = disk_slot_device(pdrv);
    if (!device)
        return RES_NOTRDY;

    switch (command) {
    case CTRL_SYNC:
        return block_device_flush(device) == PROS_BLOCK_OK ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        if (!buffer)
            return RES_PARERR;
        *(LBA_t *)buffer = (LBA_t)device->sector_count;
        return (uint64_t)*(LBA_t *)buffer == device->sector_count ?
                   RES_OK : RES_PARERR;
    case GET_SECTOR_SIZE:
        if (!buffer)
            return RES_PARERR;
        *(WORD *)buffer = (WORD)device->sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (!buffer)
            return RES_PARERR;
        *(DWORD *)buffer = 1u;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/* Synthetic in-memory disk used to verify the FatFs media-access boundary. */
static uint8_t diskio_test_media[4u * 512u];

static int diskio_test_read(struct pros_block_device *device, uint64_t lba,
                            uint32_t count, void *buffer)
{
    (void)device;
    for (uint32_t i = 0u; i < count * 512u; ++i)
        ((uint8_t *)buffer)[i] = diskio_test_media[lba * 512u + i];
    return PROS_BLOCK_OK;
}

static int diskio_test_write(struct pros_block_device *device, uint64_t lba,
                             uint32_t count, const void *buffer)
{
    (void)device;
    for (uint32_t i = 0u; i < count * 512u; ++i)
        diskio_test_media[lba * 512u + i] = ((const uint8_t *)buffer)[i];
    return PROS_BLOCK_OK;
}

static int diskio_test_flush(struct pros_block_device *device)
{
    return device ? PROS_BLOCK_OK : PROS_BLOCK_EINVAL;
}

int pros_fatfs_diskio_selftest(void)
{
    struct pros_block_device device;
    uint8_t input[512], output[512];
    WORD sector_size = 0u;
    LBA_t sector_count = 0u;
    DWORD block_size = 0u;

    for (uint32_t i = 0u; i < sizeof(diskio_test_media); ++i)
        diskio_test_media[i] = 0u;
    for (uint32_t i = 0u; i < sizeof(input); ++i)
        input[i] = (uint8_t)(i ^ 0xA5u);

    if (block_device_init(&device, 512u, 4u, diskio_test_read,
                          diskio_test_write, 0) != PROS_BLOCK_OK ||
        block_device_set_flush(&device, diskio_test_flush) != PROS_BLOCK_OK ||
        pros_fatfs_disk_bind(0u, &device, 0u) != PROS_BLOCK_OK)
        return -1;

    if (disk_initialize(0u) != 0u ||
        disk_write(0u, input, 1u, 1u) != RES_OK ||
        disk_read(0u, output, 1u, 1u) != RES_OK)
        return -2;
    for (uint32_t i = 0u; i < sizeof(input); ++i)
        if (input[i] != output[i])
            return -3;
    if (disk_ioctl(0u, CTRL_SYNC, 0) != RES_OK ||
        disk_ioctl(0u, GET_SECTOR_COUNT, &sector_count) != RES_OK ||
        disk_ioctl(0u, GET_SECTOR_SIZE, &sector_size) != RES_OK ||
        disk_ioctl(0u, GET_BLOCK_SIZE, &block_size) != RES_OK ||
        sector_count != 4u || sector_size != 512u || block_size != 1u ||
        disk_read(0u, output, 4u, 1u) != RES_PARERR ||
        pros_fatfs_disk_unbind(0u) != PROS_BLOCK_OK)
        return -4;

    if (pros_fatfs_disk_bind(0u, &device, 1u) != PROS_BLOCK_OK ||
        (disk_status(0u) & STA_PROTECT) == 0u ||
        disk_write(0u, input, 0u, 1u) != RES_WRPRT ||
        pros_fatfs_disk_unbind(0u) != PROS_BLOCK_OK)
        return -5;
    return PROS_BLOCK_OK;
}

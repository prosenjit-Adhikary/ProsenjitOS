#include "fatfs_disk.h"
#include "ff.h"
#include "diskio.h"

struct pros_fatfs_disk_slot {
    struct pros_block_device *device;
    uint32_t in_flight;
    uint8_t read_only;
    uint8_t unbinding;
};

static struct pros_fatfs_disk_slot fatfs_disks[FF_VOLUMES];

static int fatfs_disk_enter(BYTE pdrv, uint8_t write_access,
                            struct pros_block_device **device)
{
    struct pros_fatfs_disk_slot *slot;
    struct pros_block_device *bound;

    if (!device || pdrv >= FF_VOLUMES)
        return PROS_FATFS_DISK_EINVAL;
    slot = &fatfs_disks[pdrv];

    if (__atomic_load_n(&slot->unbinding, __ATOMIC_ACQUIRE))
        return PROS_FATFS_DISK_ENODEV;
    __atomic_add_fetch(&slot->in_flight, 1u, __ATOMIC_ACQ_REL);

    bound = __atomic_load_n(&slot->device, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&slot->unbinding, __ATOMIC_ACQUIRE) || !bound) {
        __atomic_sub_fetch(&slot->in_flight, 1u, __ATOMIC_RELEASE);
        return PROS_FATFS_DISK_ENODEV;
    }
    if (write_access &&
        __atomic_load_n(&slot->read_only, __ATOMIC_ACQUIRE)) {
        __atomic_sub_fetch(&slot->in_flight, 1u, __ATOMIC_RELEASE);
        return PROS_FATFS_DISK_EBUSY;
    }

    *device = bound;
    return PROS_FATFS_DISK_OK;
}

static void fatfs_disk_leave(BYTE pdrv)
{
    __atomic_sub_fetch(&fatfs_disks[pdrv].in_flight, 1u, __ATOMIC_RELEASE);
}

int pros_fatfs_disk_bind(uint8_t pdrv, struct pros_block_device *device,
                         uint8_t read_only)
{
    struct pros_fatfs_disk_slot *slot;
    uint8_t expected = 0u;

    if (pdrv >= FF_VOLUMES || !device || !device->read ||
        device->sector_count == 0u ||
        device->sector_size < FF_MIN_SS || device->sector_size > FF_MAX_SS ||
        (device->sector_size & (device->sector_size - 1u)) != 0u ||
        (!read_only && !device->write))
        return PROS_FATFS_DISK_EINVAL;

    slot = &fatfs_disks[pdrv];
    if (!__atomic_compare_exchange_n(&slot->unbinding, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return PROS_FATFS_DISK_EBUSY;

    if (__atomic_load_n(&slot->device, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&slot->in_flight, __ATOMIC_ACQUIRE) != 0u) {
        __atomic_store_n(&slot->unbinding, 0u, __ATOMIC_RELEASE);
        return PROS_FATFS_DISK_EBUSY;
    }

    __atomic_store_n(&slot->read_only, read_only ? 1u : 0u,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&slot->device, device, __ATOMIC_RELEASE);
    __atomic_store_n(&slot->unbinding, 0u, __ATOMIC_RELEASE);
    return PROS_FATFS_DISK_OK;
}

int pros_fatfs_disk_unbind(uint8_t pdrv)
{
    struct pros_fatfs_disk_slot *slot;
    uint8_t expected = 0u;

    if (pdrv >= FF_VOLUMES)
        return PROS_FATFS_DISK_EINVAL;
    slot = &fatfs_disks[pdrv];

    if (!__atomic_compare_exchange_n(&slot->unbinding, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return PROS_FATFS_DISK_EBUSY;
    if (__atomic_load_n(&slot->device, __ATOMIC_ACQUIRE) == 0) {
        __atomic_store_n(&slot->unbinding, 0u, __ATOMIC_RELEASE);
        return PROS_FATFS_DISK_ENODEV;
    }
    if (__atomic_load_n(&slot->in_flight, __ATOMIC_ACQUIRE) != 0u) {
        __atomic_store_n(&slot->unbinding, 0u, __ATOMIC_RELEASE);
        return PROS_FATFS_DISK_EBUSY;
    }

    __atomic_store_n(&slot->device, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&slot->read_only, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&slot->unbinding, 0u, __ATOMIC_RELEASE);
    return PROS_FATFS_DISK_OK;
}

int pros_fatfs_disk_is_bound(uint8_t pdrv)
{
    if (pdrv >= FF_VOLUMES ||
        __atomic_load_n(&fatfs_disks[pdrv].unbinding, __ATOMIC_ACQUIRE))
        return 0;
    return __atomic_load_n(&fatfs_disks[pdrv].device, __ATOMIC_ACQUIRE) != 0;
}

DSTATUS disk_status(BYTE pdrv)
{
    struct pros_block_device *device;
    uint8_t status = 0u;

    if (fatfs_disk_enter(pdrv, 0u, &device) != PROS_FATFS_DISK_OK)
        return STA_NOINIT;
    (void)device;
    if (__atomic_load_n(&fatfs_disks[pdrv].read_only, __ATOMIC_ACQUIRE))
        status |= STA_PROTECT;
    fatfs_disk_leave(pdrv);
    return status;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buffer, LBA_t sector, UINT count)
{
    struct pros_block_device *device;
    int rc;

    if (!buffer || count == 0u)
        return RES_PARERR;
    rc = fatfs_disk_enter(pdrv, 0u, &device);
    if (rc != PROS_FATFS_DISK_OK)
        return RES_NOTRDY;
    if (!block_device_lba_range_valid(device, (uint64_t)sector,
                                      (uint32_t)count)) {
        fatfs_disk_leave(pdrv);
        return RES_PARERR;
    }
    rc = block_device_read(device, (uint64_t)sector, (uint32_t)count, buffer);
    fatfs_disk_leave(pdrv);
    return rc == PROS_BLOCK_OK ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buffer, LBA_t sector, UINT count)
{
    struct pros_block_device *device;
    int rc;

    if (!buffer || count == 0u)
        return RES_PARERR;
    rc = fatfs_disk_enter(pdrv, 1u, &device);
    if (rc == PROS_FATFS_DISK_EBUSY)
        return RES_WRPRT;
    if (rc != PROS_FATFS_DISK_OK)
        return RES_NOTRDY;
    if (!block_device_lba_range_valid(device, (uint64_t)sector,
                                      (uint32_t)count)) {
        fatfs_disk_leave(pdrv);
        return RES_PARERR;
    }
    rc = block_device_write(device, (uint64_t)sector, (uint32_t)count, buffer);
    fatfs_disk_leave(pdrv);
    return rc == PROS_BLOCK_OK ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE command, void *buffer)
{
    struct pros_block_device *device;
    int rc = fatfs_disk_enter(pdrv, 0u, &device);

    if (rc != PROS_FATFS_DISK_OK)
        return RES_NOTRDY;

    switch (command) {
    case CTRL_SYNC:
        rc = block_device_flush(device);
        break;
    case GET_SECTOR_COUNT:
        if (!buffer) {
            rc = PROS_BLOCK_EINVAL;
            break;
        }
        *(LBA_t *)buffer = (LBA_t)device->sector_count;
        rc = PROS_BLOCK_OK;
        break;
    case GET_SECTOR_SIZE:
        if (!buffer || device->sector_size > UINT16_MAX) {
            rc = PROS_BLOCK_EINVAL;
            break;
        }
        *(WORD *)buffer = (WORD)device->sector_size;
        rc = PROS_BLOCK_OK;
        break;
    case GET_BLOCK_SIZE:
        /* Formatting is disabled; erase geometry is not advertised. */
        rc = PROS_BLOCK_EINVAL;
        break;
    default:
        rc = PROS_BLOCK_EINVAL;
        break;
    }

    fatfs_disk_leave(pdrv);
    if (rc == PROS_BLOCK_OK)
        return RES_OK;
    return command == CTRL_SYNC ? RES_ERROR : RES_PARERR;
}

struct fatfs_disk_test_media {
    uint8_t data[4u * 512u];
    uint32_t flushes;
    uint8_t fail_reads;
    uint8_t fail_writes;
    uint8_t fail_flush;
    uint32_t fail_after_sectors;
};

static int fatfs_disk_test_read(struct pros_block_device *device,
                                uint64_t lba, uint32_t count, void *buffer)
{
    struct fatfs_disk_test_media *media =
        (struct fatfs_disk_test_media *)device->private_data;
    if (!media || !buffer || lba >= 4u || count > 4u - lba)
        return PROS_BLOCK_EINVAL;
    if (media->fail_reads)
        return PROS_BLOCK_EIO;
    for (uint32_t i = 0u; i < count * 512u; ++i)
        ((uint8_t *)buffer)[i] = media->data[lba * 512u + i];
    return PROS_BLOCK_OK;
}

static int fatfs_disk_test_write(struct pros_block_device *device,
                                 uint64_t lba, uint32_t count,
                                 const void *buffer)
{
    struct fatfs_disk_test_media *media =
        (struct fatfs_disk_test_media *)device->private_data;
    if (!media || !buffer || lba >= 4u || count > 4u - lba)
        return PROS_BLOCK_EINVAL;
    if (media->fail_writes)
        return PROS_BLOCK_EIO;
    for (uint32_t sector = 0u; sector < count; ++sector) {
        if (media->fail_after_sectors != 0u &&
            sector == media->fail_after_sectors)
            return PROS_BLOCK_EIO;
        for (uint32_t i = 0u; i < 512u; ++i)
            media->data[(lba + sector) * 512u + i] =
                ((const uint8_t *)buffer)[sector * 512u + i];
    }
    return PROS_BLOCK_OK;
}

static int fatfs_disk_test_flush(struct pros_block_device *device)
{
    struct fatfs_disk_test_media *media =
        (struct fatfs_disk_test_media *)device->private_data;
    if (!media)
        return PROS_BLOCK_EINVAL;
    ++media->flushes;
    return media->fail_flush ? PROS_BLOCK_EIO : PROS_BLOCK_OK;
}

int pros_fatfs_disk_selftest(void)
{
    static struct fatfs_disk_test_media media;
    struct pros_block_device device;
    uint8_t input[512];
    uint8_t output[512];
    uint8_t rejected_write[512];
    static uint8_t multi_input[1024];
    static uint8_t multi_output[1024];
    uint8_t multi_input[2u * 512u];
    uint8_t multi_output[2u * 512u];
    const BYTE drive = (BYTE)(FF_VOLUMES - 1u);
    uint8_t bound = 0u;
    int result = 0;

    if (pros_fatfs_disk_is_bound(drive))
        return -1;
    for (uint32_t i = 0u; i < sizeof(input); ++i)
        input[i] = (uint8_t)(i ^ 0xA5u);
    for (uint32_t i = 0u; i < sizeof(media.data); ++i)
        media.data[i] = 0u;
    media.flushes = 0u;
    media.fail_reads = 0u;
    media.fail_writes = 0u;
    media.fail_flush = 0u;
    media.fail_after_sectors = 0u;

    if (block_device_init(&device, 512u, 4u, fatfs_disk_test_read,
                          fatfs_disk_test_write, &media) != PROS_BLOCK_OK ||
        block_device_set_flush(&device, fatfs_disk_test_flush) != PROS_BLOCK_OK)
        return -2;

    if (pros_fatfs_disk_bind(drive, &device, 0u) != PROS_FATFS_DISK_OK)
        return -3;
    bound = 1u;
    if (pros_fatfs_disk_bind(drive, &device, 0u) !=
        PROS_FATFS_DISK_EBUSY) {
        result = -4;
        goto cleanup;
    }
    /* Simulate an in-flight disk operation: unbind must not free its device. */
    __atomic_add_fetch(&fatfs_disks[drive].in_flight, 1u, __ATOMIC_ACQ_REL);
    if (pros_fatfs_disk_unbind(drive) != PROS_FATFS_DISK_EBUSY) {
        __atomic_sub_fetch(&fatfs_disks[drive].in_flight, 1u,
                           __ATOMIC_RELEASE);
        result = -5;
        goto cleanup;
    }
    __atomic_sub_fetch(&fatfs_disks[drive].in_flight, 1u, __ATOMIC_RELEASE);

    if (disk_initialize(drive) != 0u ||
        disk_write(drive, input, 1u, 1u) != RES_OK ||
        disk_read(drive, output, 1u, 1u) != RES_OK) {
        result = -4;
        goto cleanup;
    }
    for (uint32_t i = 0u; i < sizeof(input); ++i) {
        if (input[i] != output[i]) {
            result = -5;
            goto cleanup;
        }
    }

    /* Exercise a two-sector transfer ending exactly at the device boundary. */
    for (uint32_t i = 0u; i < sizeof(multi_input); ++i)
        multi_input[i] = (uint8_t)((i * 13u) ^ (i >> 3) ^ 0x6Du);
    if (disk_write(drive, multi_input, 2u, 2u) != RES_OK ||
        disk_read(drive, multi_output, 2u, 2u) != RES_OK ||
        memcmp(multi_input, multi_output, sizeof(multi_input)) != 0 ||
        disk_read(drive, multi_output, 3u, 2u) != RES_PARERR) {
        result = -20;
        goto cleanup;
    }

    if (disk_ioctl(drive, CTRL_SYNC, 0) != RES_OK || media.flushes != 1u ||
        disk_read(drive, output, 4u, 1u) != RES_PARERR) {
        result = -6;
        goto cleanup;
    }

    /* Block read errors must cross the FatFs disk-I/O boundary. */
    media.fail_reads = 1u;
    if (disk_read(drive, output, 1u, 1u) != RES_ERROR) {
        media.fail_reads = 0u;
        result = -12;
        goto cleanup;
    }
    media.fail_reads = 0u;
    if (disk_read(drive, output, 1u, 1u) != RES_OK) {
        result = -13;
        goto cleanup;
    }
    for (uint32_t i = 0u; i < sizeof(input); ++i) {
        if (input[i] != output[i]) {
            result = -14;
            goto cleanup;
        }
        rejected_write[i] = (uint8_t)~input[i];
    }

    /* A failed write must not mutate the mock medium or report success. */
    media.fail_writes = 1u;
    if (disk_write(drive, rejected_write, 1u, 1u) != RES_ERROR) {
        media.fail_writes = 0u;
        result = -15;
        goto cleanup;
    }
    media.fail_writes = 0u;
    if (disk_read(drive, output, 1u, 1u) != RES_OK) {
        result = -16;
        goto cleanup;
    }
    for (uint32_t i = 0u; i < sizeof(input); ++i) {
        if (input[i] != output[i]) {
            result = -17;
            goto cleanup;
        }
    }

    /*
     * A multi-sector request can partially modify media before returning an
     * I/O error. Verify the exact partial state, then retry the entire range.
     */
    for (uint32_t i = 0u; i < sizeof(multi_input); ++i)
        multi_input[i] = (uint8_t)(0x3Du + (i * 17u));
    media.fail_after_sectors = 1u;
    if (disk_write(drive, multi_input, 1u, 2u) != RES_ERROR) {
        media.fail_after_sectors = 0u;
        result = -20;
        goto cleanup;
    }
    media.fail_after_sectors = 0u;
    if (disk_read(drive, multi_output, 1u, 2u) != RES_OK) {
        result = -21;
        goto cleanup;
    }
    for (uint32_t i = 0u; i < 512u; ++i) {
        if (multi_output[i] != multi_input[i] ||
            multi_output[512u + i] != 0u) {
            result = -22;
            goto cleanup;
        }
    }
    if (disk_write(drive, multi_input, 1u, 2u) != RES_OK ||
        disk_read(drive, multi_output, 1u, 2u) != RES_OK) {
        result = -23;
        goto cleanup;
    }
    for (uint32_t i = 0u; i < sizeof(multi_input); ++i) {
        if (multi_output[i] != multi_input[i]) {
            result = -24;
            goto cleanup;
        }
    }

    /* CTRL_SYNC must report a failed flush and allow a later retry. */
    media.fail_flush = 1u;
    if (disk_ioctl(drive, CTRL_SYNC, 0) != RES_ERROR) {
        media.fail_flush = 0u;
        result = -18;
        goto cleanup;
    }
    media.fail_flush = 0u;
    if (disk_ioctl(drive, CTRL_SYNC, 0) != RES_OK || media.flushes != 3u) {
        result = -19;
        goto cleanup;
    }
    if (pros_fatfs_disk_unbind(drive) != PROS_FATFS_DISK_OK) {
        result = -7;
        goto cleanup;
    }
    bound = 0u;
    if (pros_fatfs_disk_is_bound(drive)) {
        result = -8;
        goto cleanup;
    }

    if (pros_fatfs_disk_bind(drive, &device, 1u) != PROS_FATFS_DISK_OK) {
        result = -9;
        goto cleanup;
    }
    bound = 1u;
    if ((disk_status(drive) & STA_PROTECT) == 0u ||
        disk_write(drive, input, 0u, 1u) != RES_WRPRT)
        result = -10;

cleanup:
    if (bound && pros_fatfs_disk_unbind(drive) != PROS_FATFS_DISK_OK &&
        result == 0)
        result = -11;
    return result;
}

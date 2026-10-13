#include "block.h"
#include "health.h"

static uint64_t read_count;
static uint64_t write_count;
static uint64_t error_count;
static uint64_t flush_count;
static uint64_t flush_error_count;
static volatile uint32_t block_init_diag_stage;
uint32_t block_device_init_diag_stage_get(void) { return block_init_diag_stage; }

int block_device_init(struct pros_block_device *device,
                      uint32_t sector_size,
                      uint64_t sector_count,
                      pros_block_read_fn read,
                      pros_block_write_fn write,
                      void *private_data)
{
    block_init_diag_stage = 1u;
    if (!device || sector_size == 0 || sector_count == 0 || !read)
        return PROS_BLOCK_EINVAL;
    block_init_diag_stage = 2u;
    device->sector_size = sector_size;
    block_init_diag_stage = 3u;
    device->sector_count = sector_count;
    block_init_diag_stage = 4u;
    device->read = read;
    block_init_diag_stage = 5u;
    device->write = write;
    block_init_diag_stage = 6u;
    device->flush = 0;
    block_init_diag_stage = 7u;
    device->private_data = private_data;
    block_init_diag_stage = 8u;
    return PROS_BLOCK_OK;
}

int block_device_read(struct pros_block_device *device,
                      uint64_t lba, uint32_t count, void *buffer)
{
    if (!device || !device->read || !buffer || count == 0) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE, 0x424C4B52u,
                                 PROS_BLOCK_HEALTH_INVALID_REQUEST, lba);
        return PROS_BLOCK_EINVAL;
    }
    if (lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE, 0x424C4B52u,
                                 PROS_BLOCK_HEALTH_INVALID_REQUEST, lba);
        return PROS_BLOCK_EINVAL;
    }
    ++read_count;
    int rc = device->read(device, lba, count, buffer);
    if (rc != PROS_BLOCK_OK) {
        ++error_count;
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE,
                                 0x424C4B52u, PROS_BLOCK_HEALTH_READ_FAILURE, lba);
    }
    return rc;
}

int block_device_write(struct pros_block_device *device,
                       uint64_t lba, uint32_t count, const void *buffer)
{
    if (!device || !device->write || !buffer || count == 0) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE, 0x424C4B57u,
                                 PROS_BLOCK_HEALTH_INVALID_REQUEST, lba);
        return PROS_BLOCK_EINVAL;
    }
    if (lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE, 0x424C4B57u,
                                 PROS_BLOCK_HEALTH_INVALID_REQUEST, lba);
        return PROS_BLOCK_EINVAL;
    }
    ++write_count;
    int rc = device->write(device, lba, count, buffer);
    if (rc != PROS_BLOCK_OK) {
        ++error_count;
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE,
                                 0x424C4B57u, PROS_BLOCK_HEALTH_WRITE_FAILURE, lba);
    }
    return rc;
}

int block_device_set_flush(struct pros_block_device *device,
                           pros_block_flush_fn flush)
{
    if (!device) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE, 0x424C4B46u,
                                 PROS_BLOCK_HEALTH_INVALID_REQUEST, 0u);
        return PROS_BLOCK_EINVAL;
    }
    device->flush = flush;
    return PROS_BLOCK_OK;
}

static int block_lba_range_valid(const struct pros_block_device *device,
                                 uint64_t lba, uint32_t count)
{
    if (!device || count == 0u || lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba)
        return 0;
    return 1;
}

int block_device_lba_range_valid(const struct pros_block_device *device,
                                 uint64_t lba, uint32_t count)
{
    return block_lba_range_valid(device, lba, count);
}

int block_device_lba_add(const struct pros_block_device *device,
                         uint64_t lba, uint64_t offset, uint64_t *result)
{
    if (!device || !result || lba >= device->sector_count ||
        offset >= device->sector_count - lba)
        return PROS_BLOCK_EINVAL;
    *result = lba + offset;
    return PROS_BLOCK_OK;
}

int block_device_flush(struct pros_block_device *device)
{
    if (!device)
        return PROS_BLOCK_EINVAL;
    if (!device->flush)
        return PROS_BLOCK_OK;
    ++flush_count;
    int rc = device->flush(device);
    if (rc != PROS_BLOCK_OK) {
        ++flush_error_count;
        ++error_count;
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE,
                                 0x424C4B46u, PROS_BLOCK_HEALTH_FLUSH_FAILURE, 0u);
    }
    return rc;
}

uint64_t block_read_count(void) { return read_count; }
uint64_t block_write_count(void) { return write_count; }
uint64_t block_error_count(void) { return error_count; }
uint64_t block_flush_count(void) { return flush_count; }
uint64_t block_flush_error_count(void) { return flush_error_count; }


static int block_selftest_read_fail(struct pros_block_device *device,
                                    uint64_t lba, uint32_t count, void *buffer)
{
    (void)device; (void)lba; (void)count; (void)buffer;
    return PROS_BLOCK_EIO;
}

static int block_selftest_write_fail(struct pros_block_device *device,
                                     uint64_t lba, uint32_t count,
                                     const void *buffer)
{
    (void)device; (void)lba; (void)count; (void)buffer;
    return PROS_BLOCK_EIO;
}

static int block_selftest_flush_fail(struct pros_block_device *device)
{
    (void)device;
    return PROS_BLOCK_EIO;
}

int block_device_selftest(void)
{
    struct pros_block_device device;
    uint8_t sector[512];
    uint64_t errors_before = error_count;
    uint64_t flush_errors_before = flush_error_count;
    uint64_t lba_result = 0u;

    if (block_device_init(&device, 512u, 4u,
                          block_selftest_read_fail,
                          block_selftest_write_fail,
                          0) != PROS_BLOCK_OK ||
        block_device_set_flush(&device, block_selftest_flush_fail) !=
            PROS_BLOCK_OK)
        return PROS_BLOCK_EINVAL;

    if (!block_device_lba_range_valid(&device, 0u, 1u) ||
        !block_device_lba_range_valid(&device, 3u, 1u) ||
        block_device_lba_range_valid(&device, 4u, 1u) ||
        block_device_lba_range_valid(&device, 3u, 2u) ||
        block_device_lba_add(&device, 3u, 0u, &lba_result) != PROS_BLOCK_OK ||
        lba_result != 3u ||
        block_device_lba_add(&device, 3u, 1u, &lba_result) != PROS_BLOCK_EINVAL ||
        block_device_lba_add(&device, 4u, 0u, &lba_result) != PROS_BLOCK_EINVAL)
        return PROS_BLOCK_EINVAL;

    /* UINT64_MAX must never wrap through the checked addition path. */
    {
        struct pros_block_device wide = device;
        wide.sector_count = UINT64_MAX;
        if (block_device_lba_range_valid(&wide, UINT64_MAX, 1u) ||
            block_device_lba_add(&wide, UINT64_MAX, 1u, &lba_result) !=
                PROS_BLOCK_EINVAL ||
            block_device_lba_add(&wide, UINT64_MAX - 1u, 1u, &lba_result) !=
                PROS_BLOCK_EINVAL ||
            block_device_lba_range_valid(&wide, UINT64_MAX - 1u, 2u))
            return PROS_BLOCK_EINVAL;
    }

    if (block_device_read(&device, 4u, 1u, sector) != PROS_BLOCK_EINVAL ||
        block_device_write(&device, 4u, 1u, sector) != PROS_BLOCK_EINVAL ||
        block_device_read(&device, 0u, 1u, sector) != PROS_BLOCK_EIO ||
        block_device_write(&device, 0u, 1u, sector) != PROS_BLOCK_EIO ||
        block_device_flush(&device) != PROS_BLOCK_EIO ||
        error_count < errors_before + 3u ||
        flush_error_count < flush_errors_before + 1u)
        return PROS_BLOCK_EIO;

    return PROS_BLOCK_OK;
}

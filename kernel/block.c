#include "block.h"
#include "health.h"

static uint64_t read_count;
static uint64_t write_count;
static uint64_t error_count;
static uint64_t flush_count;
static uint64_t flush_error_count;

int block_device_init(struct pros_block_device *device,
                      uint32_t sector_size,
                      uint64_t sector_count,
                      pros_block_read_fn read,
                      pros_block_write_fn write,
                      void *private_data)
{
    if (!device || sector_size == 0 || sector_count == 0 || !read)
        return PROS_BLOCK_EINVAL;
    device->sector_size = sector_size;
    device->sector_count = sector_count;
    device->read = read;
    device->write = write;
    device->flush = 0;
    device->private_data = private_data;
    return PROS_BLOCK_OK;
}

int block_device_read(struct pros_block_device *device,
                      uint64_t lba, uint32_t count, void *buffer)
{
    if (!device || !device->read || !buffer || count == 0)
        return PROS_BLOCK_EINVAL;
    if (lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba)
        return PROS_BLOCK_EINVAL;
    ++read_count;
    int rc = device->read(device, lba, count, buffer);
    if (rc != PROS_BLOCK_OK) {
        ++error_count;
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE,
                                 0x424C4B52u, (uint32_t)(-rc), lba);
    }
    return rc;
}

int block_device_write(struct pros_block_device *device,
                       uint64_t lba, uint32_t count, const void *buffer)
{
    if (!device || !device->write || !buffer || count == 0)
        return PROS_BLOCK_EINVAL;
    if (lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba)
        return PROS_BLOCK_EINVAL;
    ++write_count;
    int rc = device->write(device, lba, count, buffer);
    if (rc != PROS_BLOCK_OK) {
        ++error_count;
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE,
                                 0x424C4B57u, (uint32_t)(-rc), lba);
    }
    return rc;
}

int block_device_set_flush(struct pros_block_device *device,
                           pros_block_flush_fn flush)
{
    if (!device)
        return PROS_BLOCK_EINVAL;
    device->flush = flush;
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
                                 0x424C4B46u, (uint32_t)(-rc), 0u);
    }
    return rc;
}

uint64_t block_read_count(void) { return read_count; }
uint64_t block_write_count(void) { return write_count; }
uint64_t block_error_count(void) { return error_count; }
uint64_t block_flush_count(void) { return flush_count; }
uint64_t block_flush_error_count(void) { return flush_error_count; }

#include "block.h"

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
    return device->read(device, lba, count, buffer);
}

int block_device_write(struct pros_block_device *device,
                       uint64_t lba, uint32_t count, const void *buffer)
{
    if (!device || !device->write || !buffer || count == 0)
        return PROS_BLOCK_EINVAL;
    if (lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba)
        return PROS_BLOCK_EINVAL;
    return device->write(device, lba, count, buffer);
}

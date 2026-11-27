#include "block.h"

#include "health.h"

static uint64_t read_count;
static uint64_t write_count;
static uint64_t error_count;
static uint64_t flush_count;
static uint64_t flush_error_count;
/* Nonzero across the complete public block operation, including health hooks. */
static volatile uint32_t block_io_active;

uint32_t block_device_io_active(void)
{
    return __atomic_load_n(&block_io_active, __ATOMIC_ACQUIRE);
}

static void block_io_enter(void)
{
    (void)__atomic_add_fetch(&block_io_active, 1u, __ATOMIC_ACQ_REL);
}

static void block_io_leave(void)
{
    (void)__atomic_sub_fetch(&block_io_active, 1u, __ATOMIC_RELEASE);
}

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
    int rc;
    block_io_enter();
    if (!device || !device->read || !buffer || count == 0) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE, 0x424C4B52u,
                                 PROS_BLOCK_HEALTH_INVALID_REQUEST, lba);
        block_io_leave();
        return PROS_BLOCK_EINVAL;
    }
    if (lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE, 0x424C4B52u,
                                 PROS_BLOCK_HEALTH_INVALID_REQUEST, lba);
        block_io_leave();
        return PROS_BLOCK_EINVAL;
    }
    ++read_count;
    rc = device->read(device, lba, count, buffer);
    if (rc != PROS_BLOCK_OK) {
        ++error_count;
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE,
                                 0x424C4B52u, PROS_BLOCK_HEALTH_READ_FAILURE, lba);
    }
    block_io_leave();
    return rc;
}

int block_device_write(struct pros_block_device *device,
                       uint64_t lba, uint32_t count, const void *buffer)
{
    int rc;
    block_io_enter();
    if (!device || !device->write || !buffer || count == 0) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE, 0x424C4B57u,
                                 PROS_BLOCK_HEALTH_INVALID_REQUEST, lba);
        block_io_leave();
        return PROS_BLOCK_EINVAL;
    }
    if (lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE, 0x424C4B57u,
                                 PROS_BLOCK_HEALTH_INVALID_REQUEST, lba);
        block_io_leave();
        return PROS_BLOCK_EINVAL;
    }
    ++write_count;
    rc = device->write(device, lba, count, buffer);
    if (rc != PROS_BLOCK_OK) {
        ++error_count;
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE,
                                 0x424C4B57u, PROS_BLOCK_HEALTH_WRITE_FAILURE, lba);
    }
    block_io_leave();
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
    int rc;
    if (!device)
        return PROS_BLOCK_EINVAL;
    block_io_enter();
    /* Never report a durability barrier when the device cannot provide one. */
    if (!device->flush) {
        block_io_leave();
        return PROS_BLOCK_ENOTSUP;
    }
    ++flush_count;
    rc = device->flush(device);
    if (rc != PROS_BLOCK_OK) {
        ++flush_error_count;
        ++error_count;
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_STORAGE,
                                 0x424C4B46u, PROS_BLOCK_HEALTH_FLUSH_FAILURE, 0u);
    }
    block_io_leave();
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

static int block_selftest_read_active(struct pros_block_device *device,
                                       uint64_t lba, uint32_t count,
                                       void *buffer)
{
    (void)device; (void)lba; (void)count; (void)buffer;
    return block_device_io_active() != 0u ? PROS_BLOCK_OK : PROS_BLOCK_EIO;
}

static int block_selftest_write_active(struct pros_block_device *device,
                                        uint64_t lba, uint32_t count,
                                        const void *buffer)
{
    (void)device; (void)lba; (void)count; (void)buffer;
    return block_device_io_active() != 0u ? PROS_BLOCK_OK : PROS_BLOCK_EIO;
}

static int block_selftest_flush_active(struct pros_block_device *device)
{
    (void)device;
    return block_device_io_active() != 0u ? PROS_BLOCK_OK : PROS_BLOCK_EIO;
}

int block_device_selftest(void)
{
    struct pros_block_device device;
    struct pros_block_device no_flush_device;
    struct pros_block_device active_device;
    uint8_t sector[512];
    uint64_t errors_before = error_count;
    uint64_t flush_errors_before = flush_error_count;
    uint64_t flushes_before = flush_count;
    uint64_t lba_result = 0u;

    if (block_device_init(&device, 512u, 4u,
                          block_selftest_read_fail,
                          block_selftest_write_fail,
                          0) != PROS_BLOCK_OK ||
        block_device_set_flush(&device, block_selftest_flush_fail) !=
            PROS_BLOCK_OK)
        return PROS_BLOCK_EINVAL;

    if (block_device_io_active() != 0u ||
        block_device_init(&active_device, 512u, 4u,
                          block_selftest_read_active,
                          block_selftest_write_active, 0) != PROS_BLOCK_OK ||
        block_device_set_flush(&active_device,
                               block_selftest_flush_active) != PROS_BLOCK_OK ||
        block_device_read(&active_device, 0u, 1u, sector) != PROS_BLOCK_OK ||
        block_device_io_active() != 0u ||
        block_device_write(&active_device, 0u, 1u, sector) != PROS_BLOCK_OK ||
        block_device_io_active() != 0u ||
        block_device_flush(&active_device) != PROS_BLOCK_OK ||
        block_device_io_active() != 0u)
        return PROS_BLOCK_EIO;

    if (block_device_init(&no_flush_device, 512u, 4u,
                          block_selftest_read_fail,
                          block_selftest_write_fail, 0) != PROS_BLOCK_OK ||
        block_device_flush(&no_flush_device) != PROS_BLOCK_ENOTSUP ||
        flush_count != flushes_before)
        return PROS_BLOCK_EIO;

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
        block_device_io_active() != 0u ||
        block_device_write(&device, 4u, 1u, sector) != PROS_BLOCK_EINVAL ||
        block_device_io_active() != 0u ||
        block_device_read(&device, 0u, 1u, sector) != PROS_BLOCK_EIO ||
        block_device_io_active() != 0u ||
        block_device_write(&device, 0u, 1u, sector) != PROS_BLOCK_EIO ||
        block_device_io_active() != 0u ||
        block_device_flush(&device) != PROS_BLOCK_EIO ||
        block_device_io_active() != 0u ||
        error_count < errors_before + 3u ||
        flush_error_count < flush_errors_before + 1u)
        return PROS_BLOCK_EIO;

    return PROS_BLOCK_OK;
}


/* Stable, bounded registry for block devices published to the kernel. */
static struct pros_block_device *block_registry[PROS_BLOCK_REGISTRY_CAPACITY];
static volatile uint32_t block_registry_lock;
static void block_registry_acquire(void) {
    while (__atomic_exchange_n(&block_registry_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause" ::: "memory");
}
static void block_registry_release(void) {
    __atomic_store_n(&block_registry_lock, 0u, __ATOMIC_RELEASE);
}
int block_device_register(struct pros_block_device *device) {
    int free_slot = -1;
    if (!device || !device->read || device->sector_size == 0u ||
        device->sector_count == 0u) return PROS_BLOCK_EINVAL;
    block_registry_acquire();
    for (uint32_t i = 0u; i < PROS_BLOCK_REGISTRY_CAPACITY; ++i) {
        if (block_registry[i] == device) {
            block_registry_release(); return PROS_BLOCK_EINVAL;
        }
        if (!block_registry[i] && free_slot < 0) free_slot = (int)i;
    }
    if (free_slot < 0) {
        block_registry_release(); return PROS_BLOCK_ENOSPC;
    }
    block_registry[free_slot] = device;
    block_registry_release();
    return PROS_BLOCK_OK;
}
int block_device_unregister(struct pros_block_device *device) {
    if (!device) return PROS_BLOCK_EINVAL;
    block_registry_acquire();
    for (uint32_t i = 0u; i < PROS_BLOCK_REGISTRY_CAPACITY; ++i) {
        if (block_registry[i] == device) {
            block_registry[i] = 0;
            block_registry_release(); return PROS_BLOCK_OK;
        }
    }
    block_registry_release();
    return PROS_BLOCK_ENOENT;
}
struct pros_block_device *block_device_registry_get(uint32_t index) {
    struct pros_block_device *device = 0;
    uint32_t seen = 0u;
    block_registry_acquire();
    for (uint32_t i = 0u; i < PROS_BLOCK_REGISTRY_CAPACITY; ++i) {
        if (block_registry[i] && seen++ == index) {
            device = block_registry[i]; break;
        }
    }
    block_registry_release();
    return device;
}
uint32_t block_device_registry_count(void) {
    uint32_t count = 0u;
    block_registry_acquire();
    for (uint32_t i = 0u; i < PROS_BLOCK_REGISTRY_CAPACITY; ++i)
        if (block_registry[i]) ++count;
    block_registry_release();
    return count;
}
int block_device_registry_selftest(void) {
    struct pros_block_device first, second;
    struct pros_block_device fillers[PROS_BLOCK_REGISTRY_CAPACITY];
    struct pros_block_device overflow;
    uint32_t before = block_device_registry_count();
    uint32_t filler_count = 0u;
    int result = PROS_BLOCK_EIO;

    if (before > PROS_BLOCK_REGISTRY_CAPACITY - 2u)
        return PROS_BLOCK_ENOSPC;
    if (block_device_init(&first, 512u, 8u, block_selftest_read_fail,
                          block_selftest_write_fail, 0) != PROS_BLOCK_OK ||
        block_device_init(&second, 512u, 16u, block_selftest_read_fail,
                          block_selftest_write_fail, 0) != PROS_BLOCK_OK ||
        block_device_init(&overflow, 512u, 32u, block_selftest_read_fail,
                          block_selftest_write_fail, 0) != PROS_BLOCK_OK)
        return PROS_BLOCK_EINVAL;

    if (block_device_register(&first) != PROS_BLOCK_OK ||
        block_device_register(&first) != PROS_BLOCK_EINVAL ||
        block_device_register(&second) != PROS_BLOCK_OK ||
        block_device_registry_count() != before + 2u ||
        block_device_registry_get(before) != &first ||
        block_device_registry_get(before + 1u) != &second ||
        block_device_registry_get(before + 2u) != 0 ||
        block_device_unregister(&first) != PROS_BLOCK_OK ||
        block_device_unregister(&first) != PROS_BLOCK_ENOENT ||
        block_device_registry_count() != before + 1u ||
        block_device_registry_get(before) != &second ||
        block_device_unregister(&second) != PROS_BLOCK_OK ||
        block_device_registry_count() != before)
        return PROS_BLOCK_EIO;

    for (uint32_t i = 0u; i < PROS_BLOCK_REGISTRY_CAPACITY; ++i) {
        if (block_device_init(&fillers[i], 512u, (uint64_t)i + 1u,
                block_selftest_read_fail, block_selftest_write_fail, 0) !=
                PROS_BLOCK_OK)
            goto cleanup;
    }
    while (block_device_registry_count() <
           PROS_BLOCK_REGISTRY_CAPACITY) {
        if (filler_count >= PROS_BLOCK_REGISTRY_CAPACITY ||
            block_device_register(&fillers[filler_count]) != PROS_BLOCK_OK)
            goto cleanup;
        ++filler_count;
    }
    if (block_device_registry_count() != PROS_BLOCK_REGISTRY_CAPACITY ||
        block_device_register(&overflow) != PROS_BLOCK_ENOSPC)
        goto cleanup;

    /* A freed middle slot must be reusable even when the registry was full. */
    if (filler_count < 2u ||
        block_device_unregister(&fillers[0]) != PROS_BLOCK_OK ||
        block_device_register(&overflow) != PROS_BLOCK_OK ||
        block_device_registry_count() != PROS_BLOCK_REGISTRY_CAPACITY ||
        block_device_unregister(&overflow) != PROS_BLOCK_OK)
        goto cleanup;
    result = PROS_BLOCK_OK;

cleanup:
    (void)block_device_unregister(&overflow);
    for (uint32_t i = 0u; i < filler_count; ++i)
        (void)block_device_unregister(&fillers[i]);
    if (block_device_registry_count() != before)
        result = PROS_BLOCK_EIO;
    return result;
}

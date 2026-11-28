#ifndef PROSENJIT_BLOCK_H
#define PROSENJIT_BLOCK_H

#include <stdint.h>

#define PROS_BLOCK_OK       0
#define PROS_BLOCK_EINVAL  -1
#define PROS_BLOCK_EIO     -2
#define PROS_BLOCK_ENOTSUP -5

/* Stable block-health event codes (PROS_HEALTH_STORAGE). */
enum pros_block_health_code {
    PROS_BLOCK_HEALTH_INVALID_REQUEST = 1u,
    PROS_BLOCK_HEALTH_READ_FAILURE = 2u,
    PROS_BLOCK_HEALTH_WRITE_FAILURE = 3u,
    PROS_BLOCK_HEALTH_FLUSH_FAILURE = 4u
};

struct pros_block_device;
typedef int (*pros_block_read_fn)(struct pros_block_device *, uint64_t, uint32_t, void *);
typedef int (*pros_block_write_fn)(struct pros_block_device *, uint64_t, uint32_t, const void *);
typedef int (*pros_block_flush_fn)(struct pros_block_device *);

struct pros_block_device {
    uint32_t sector_size;
    uint64_t sector_count;
    pros_block_read_fn read;
    pros_block_write_fn write;
    pros_block_flush_fn flush;
    void *private_data;
};

#define PROS_BLOCK_REGISTRY_CAPACITY 16u
#define PROS_BLOCK_ENOSPC -3
#define PROS_BLOCK_ENOENT -4

int block_device_register(struct pros_block_device *);
int block_device_unregister(struct pros_block_device *);
struct pros_block_device *block_device_registry_get(uint32_t index);
uint32_t block_device_registry_count(void);
int block_device_registry_selftest(void);

int block_device_init(struct pros_block_device *, uint32_t, uint64_t,
                      pros_block_read_fn, pros_block_write_fn, void *);
int block_device_read(struct pros_block_device *, uint64_t, uint32_t, void *);
int block_device_write(struct pros_block_device *, uint64_t, uint32_t, const void *);
int block_device_set_flush(struct pros_block_device *, pros_block_flush_fn);
int block_device_flush(struct pros_block_device *);
/* Nonzero while a public block read/write/flush or its health hook is active. */
uint32_t block_device_io_active(void);
/* LBA values are always sector indices in the device-local address space. */
int block_device_lba_range_valid(const struct pros_block_device *, uint64_t,
                                 uint32_t);
int block_device_lba_add(const struct pros_block_device *, uint64_t, uint64_t,
                         uint64_t *);

uint64_t block_read_count(void);
uint64_t block_write_count(void);
uint64_t block_error_count(void);
uint64_t block_flush_count(void);
uint64_t block_flush_error_count(void);
int block_device_selftest(void);

#endif

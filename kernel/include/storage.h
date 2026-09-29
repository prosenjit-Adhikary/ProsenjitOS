#ifndef PROSENJIT_STORAGE_H
#define PROSENJIT_STORAGE_H

#include <stdint.h>
#include "block.h"
#include "partition.h"
#include "vfs.h"

#define PROS_STORAGE_OK        0
#define PROS_STORAGE_EINVAL   -1
#define PROS_STORAGE_EIO      -2
#define PROS_STORAGE_EOVERFLOW -3

#define PROS_STORAGE_MAX_VOLUMES 8u

struct pros_storage_volume {
    struct pros_partition partition;
    struct pros_partition_device device;
    enum pros_filesystem_type filesystem;
};

int pros_storage_discover(struct pros_block_device *device,
                          struct pros_storage_volume *volumes,
                          uint32_t capacity,
                          uint32_t *count);

int pros_storage_selftest(void);

#endif

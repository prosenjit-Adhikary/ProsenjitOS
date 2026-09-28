#ifndef PROSENJIT_PARTITION_H
#define PROSENJIT_PARTITION_H

#include <stdint.h>
#include "block.h"

#define PROS_PARTITION_OK       0
#define PROS_PARTITION_EINVAL  -1
#define PROS_PARTITION_ENOTSUP -2
#define PROS_PARTITION_EIO     -3

enum pros_partition_type {
    PROS_PARTITION_NONE = 0,
    PROS_PARTITION_MBR,
    PROS_PARTITION_GPT
};

struct pros_partition {
    uint64_t first_lba;
    uint64_t sector_count;
    uint8_t type_guid[16];
    uint8_t mbr_type;
    enum pros_partition_type table_type;
};

int partition_probe(struct pros_block_device *device,
                    enum pros_partition_type *table_type);
int partition_read_mbr(struct pros_block_device *device,
                       struct pros_partition *partitions,
                       uint32_t capacity, uint32_t *count);
int partition_read_gpt(struct pros_block_device *device,
                       struct pros_partition *partitions,
                       uint32_t capacity, uint32_t *count);

#endif

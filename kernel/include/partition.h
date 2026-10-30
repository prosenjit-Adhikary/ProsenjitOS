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

#define PROS_PARTITION_TRACE_DEPTH 64u

struct pros_partition_trace_event {
    uint64_t local_lba;
    uint64_t parent_lba;
    uint32_t count;
    uint8_t write;
};

struct pros_partition_device {
    struct pros_block_device device;
    struct pros_block_device *parent;
    uint64_t first_lba;
    struct pros_partition_trace_event trace[PROS_PARTITION_TRACE_DEPTH];
    uint32_t trace_count;
    uint32_t trace_head;
};

struct pros_partition {
    uint64_t first_lba;
    uint64_t sector_count;
    uint8_t type_guid[16];
    uint8_t mbr_type;
    enum pros_partition_type table_type;
};

/*
 * Probe/read APIs publish output values only on success. partition_probe()
 * leaves *table_type unchanged on error. On a valid read_mbr/read_gpt call,
 * *count is set to zero and the full caller-provided partition array is
 * cleared before I/O; entries become valid only when success is returned
 * and included in the resulting count.
 */
int partition_probe(struct pros_block_device *device,
                    enum pros_partition_type *table_type);
int partition_read_mbr(struct pros_block_device *device,
                       struct pros_partition *partitions,
                       uint32_t capacity, uint32_t *count);
int partition_read_gpt(struct pros_block_device *device,
                       struct pros_partition *partitions,
                       uint32_t capacity, uint32_t *count);
void partition_trace_reset(struct pros_partition_device *);
int partition_trace_find(const struct pros_partition_device *, uint64_t lba,
                         uint8_t write, uint64_t *parent_lba,
                         uint32_t *trace_count);

int partition_device_init(struct pros_partition_device *,
                          struct pros_block_device *,
                          const struct pros_partition *);
int partition_selftest(void);

#endif

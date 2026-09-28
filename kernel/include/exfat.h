#ifndef PROSENJIT_EXFAT_H
#define PROSENJIT_EXFAT_H

#include <stdint.h>
#include "vfs.h"

#define PROS_EXFAT_OK        0
#define PROS_EXFAT_EINVAL   -1
#define PROS_EXFAT_EIO      -2
#define PROS_EXFAT_ENOTSUP  -3
#define PROS_EXFAT_ECORRUPT -4

struct pros_exfat {
    struct pros_vfs_mount mount;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint64_t volume_length;
};

int exfat_mount(struct pros_exfat *, struct pros_block_device *);
int exfat_cluster_lba(const struct pros_exfat *, uint32_t, uint64_t *);
int exfat_read_cluster(const struct pros_exfat *, uint32_t, void *);
int exfat_selftest(void);

#endif

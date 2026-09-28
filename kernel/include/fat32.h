#ifndef PROSENJIT_FAT32_H
#define PROSENJIT_FAT32_H

#include <stdint.h>
#include "vfs.h"

#define PROS_FAT32_OK       0
#define PROS_FAT32_EINVAL  -1
#define PROS_FAT32_EIO     -2
#define PROS_FAT32_ENOTSUP -3
#define PROS_FAT32_ECORRUPT -4

struct pros_fat32 {
    struct pros_vfs_mount mount;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint64_t fat_lba;
    uint64_t data_lba;
    uint32_t cluster_count;
};

int fat32_mount(struct pros_fat32 *, struct pros_block_device *);
int fat32_cluster_lba(const struct pros_fat32 *, uint32_t, uint64_t *);
int fat32_read_cluster(const struct pros_fat32 *, uint32_t, void *);
int fat32_selftest(void);

#endif

#ifndef PROSENJIT_NTFS_H
#define PROSENJIT_NTFS_H

#include <stdint.h>
#include "vfs.h"

#define PROS_NTFS_OK        0
#define PROS_NTFS_EINVAL   -1
#define PROS_NTFS_EIO      -2
#define PROS_NTFS_ENOTSUP  -3
#define PROS_NTFS_ECORRUPT -4

struct pros_ntfs {
    struct pros_vfs_mount mount;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mftmirr_lcn;
    uint32_t clusters_per_file_record;
    uint32_t bytes_per_file_record;
};

int ntfs_mount(struct pros_ntfs *, struct pros_block_device *);
int ntfs_cluster_lba(const struct pros_ntfs *, uint64_t, uint64_t *);
int ntfs_selftest(void);

#endif

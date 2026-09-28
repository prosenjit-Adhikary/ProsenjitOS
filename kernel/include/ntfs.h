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
int ntfs_read_file_record(const struct pros_ntfs *, uint64_t record_number, void *buffer, uint32_t buffer_size);
struct pros_ntfs_run { uint64_t lcn; uint64_t clusters; uint8_t sparse; };
int ntfs_decode_runlist(const uint8_t *runlist, uint32_t length, struct pros_ntfs_run *runs, uint32_t capacity, uint32_t *run_count);
int ntfs_find_attribute(const void *record, uint32_t record_size, uint32_t type, uint32_t *offset);
int ntfs_attribute_data(const void *record, uint32_t record_size, uint32_t attr_offset, const uint8_t **data, uint32_t *data_size);
int ntfs_file_record_fixup(void *record, uint32_t record_size, uint32_t bytes_per_sector);
int ntfs_selftest(void);

#endif

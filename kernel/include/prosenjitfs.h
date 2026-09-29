#ifndef PROSENJITFS_H
#define PROSENJITFS_H

#include <stdint.h>
#include "vfs.h"
#include "prosenjitfs_format.h"

#define PROSENJITFS_OK        0
#define PROSENJITFS_EINVAL   -1
#define PROSENJITFS_EIO      -2
#define PROSENJITFS_ENOTSUP  -3
#define PROSENJITFS_ECORRUPT -4
#define PROSENJITFS_ENOSPC   -5
#define PROSENJITFS_ENOENT   -6
#define PROSENJITFS_EEXIST   -7
#define PROSENJITFS_EOVERFLOW -8
#define PROSENJITFS_ENOTDIR  -9
#define PROSENJITFS_ENOTEMPTY -10

struct pros_prosenjitfs_tx {
    uint32_t count;
    uint64_t targets[PROSENJITFS_MAX_TX_BLOCKS];
    uint8_t *images;
};

struct pros_prosenjitfs {
    struct pros_vfs_mount mount;
    struct pros_prosenjitfs_superblock sb;
    uint8_t read_only;
    uint8_t degraded_superblock;
    uint8_t *scratch;
    struct pros_prosenjitfs_tx tx;
};

uint32_t prosenjitfs_crc32c(const void *, uint32_t);
int prosenjitfs_format(struct pros_block_device *);
int prosenjitfs_mount(struct pros_prosenjitfs *, struct pros_block_device *,
                      uint8_t read_only);
int prosenjitfs_unmount(struct pros_prosenjitfs *);
int prosenjitfs_alloc_blocks(struct pros_prosenjitfs *, uint32_t, uint64_t *);
int prosenjitfs_free_blocks(struct pros_prosenjitfs *, uint64_t, uint32_t);
int prosenjitfs_read_inode(const struct pros_prosenjitfs *,
                           uint64_t, struct pros_prosenjitfs_inode *);
int prosenjitfs_write_inode(struct pros_prosenjitfs *, uint64_t,
                            const struct pros_prosenjitfs_inode *);
int prosenjitfs_selftest(void);

#endif

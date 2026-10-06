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
#define PROSENJITFS_EPERM     -11

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
    /* Populated only after a v2 COW volume passes full geometry and checksum
     * validation. v1 mounts leave these fields zero and retain their exact
     * legacy write semantics. */
    uint8_t cow_enabled;
    uint8_t cow_degraded;
    uint16_t cow_reserved;
    uint64_t cow_extension_block;
    uint64_t snapshot_table_start;
    uint32_t snapshot_table_blocks;
    uint64_t refcount_start;
    uint32_t refcount_blocks;
    uint64_t snapshot_generation;
};

uint32_t prosenjitfs_crc32c(const void *, uint32_t);
int prosenjitfs_format(struct pros_block_device *);
int prosenjitfs_probe(struct pros_block_device *);
int prosenjitfs_mount(struct pros_prosenjitfs *, struct pros_block_device *,
                      uint8_t read_only);
int prosenjitfs_unmount(struct pros_prosenjitfs *);
int prosenjitfs_alloc_blocks(struct pros_prosenjitfs *, uint32_t, uint64_t *);
int prosenjitfs_alloc_inode(struct pros_prosenjitfs *, uint64_t *);
int prosenjitfs_free_blocks(struct pros_prosenjitfs *, uint64_t, uint32_t);
int prosenjitfs_read_inode(const struct pros_prosenjitfs *,
                           uint64_t, struct pros_prosenjitfs_inode *);
int prosenjitfs_write_inode(struct pros_prosenjitfs *, uint64_t,
                            const struct pros_prosenjitfs_inode *);
void prosenjitfs_write_diag_reset(void);
void prosenjitfs_write_diag_begin_read(void);
int prosenjitfs_write_diag_get(uint64_t *physical_lba, uint8_t *tx_byte,
                               uint8_t *post_commit_byte,
                               uint8_t *direct_byte, uint8_t *vfs_byte);
int prosenjitfs_write_diag_get_read_lba(uint64_t *physical_lba);
int prosenjitfs_write_diag_get_read_byte(uint8_t *read_block_byte);
int prosenjitfs_mkdir_diag_get(uint32_t *mkdir_stage, int *mkdir_rc,
                               uint32_t *tx_stage, uint32_t *tx_count);
int prosenjitfs_selftest(void);
int prosenjitfs_cow_geometry_validate(const struct pros_prosenjitfs *, uint64_t, uint64_t, uint32_t, uint64_t, uint32_t);
int prosenjitfs_cow_refcount_initialize(struct pros_prosenjitfs *, uint64_t, uint32_t, uint64_t);
int prosenjitfs_cow_extension_validate(const struct pros_prosenjitfs *, const struct pros_prosenjitfs_cow_extension *);
int prosenjitfs_cow_refcount_blocks(uint64_t total_blocks, uint32_t block_size, uint32_t *blocks);
int prosenjitfs_snapshot_create(struct pros_prosenjitfs *, uint64_t *snapshot_id);
int prosenjitfs_snapshot_delete(struct pros_prosenjitfs *, uint64_t snapshot_id);
int prosenjitfs_snapshot_read(struct pros_prosenjitfs *, uint64_t snapshot_id,
                              const char *, uint64_t, uint32_t, void *);

#endif

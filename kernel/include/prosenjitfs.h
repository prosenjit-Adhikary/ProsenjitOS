#ifndef PROSENJITFS_KERNEL_H
#define PROSENJITFS_KERNEL_H

#include <stdint.h>
#include "vfs.h"
#include "../../include/prosenjitfs.h"

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
    /* Serializes filesystem operations that must observe and publish one
     * authoritative inode state, notably O_APPEND. */
    volatile uint32_t io_lock;
};

/* Kernel-only diagnostics and self-test entry points. */
void prosenjitfs_write_diag_reset(void);
void prosenjitfs_write_diag_begin_read(void);
int prosenjitfs_write_diag_get(uint64_t *physical_lba, uint8_t *tx_byte,
                               uint8_t *post_commit_byte,
                               uint8_t *direct_byte, uint8_t *vfs_byte);
int prosenjitfs_write_diag_get_read_lba(uint64_t *physical_lba);
int prosenjitfs_write_diag_get_read_byte(uint8_t *read_block_byte);
int prosenjitfs_mkdir_diag_get(uint32_t *mkdir_stage, int *mkdir_rc,
                               uint32_t *tx_stage, uint32_t *tx_count);
int prosenjitfs_tx_diag_get(uint32_t *stage, uint32_t *count,
                            uint32_t *count_before_clear,
                            uint32_t *count_after_clear, int *clear_rc);
void prosenjitfs_selftest_diag_get(uint32_t *stage, int *rc);

/* Validate the mounted native filesystem without mutating it. */
int prosenjitfs_consistency_check(const struct pros_prosenjitfs *fs);
int prosenjitfs_selftest(void);

#endif

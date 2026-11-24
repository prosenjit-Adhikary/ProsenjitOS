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
    /*
     * Volatile, current-mount proof that an allocated FREE inode was reserved
     * by alloc_inode() and has not yet been published or released. V1 caps
     * inode_count at 1024; this registry is exactly 1024 bits and is never
     * persisted. A crash invalidates all in-flight reservations.
     */
    uint8_t inode_reservation_registry[128u];
    /* Exact volatile proof for low-level allocator block reservations. */
    uint8_t *block_reservation_registry;
    uint64_t block_reservation_bytes;
};

/*
 * Bounded summary from a read-only integrity scan. first_error_location is
 * the source location identifier captured by the checker for the first
 * detected consistency failure; it is zero when no such failure was found.
 * Counts describe work completed before the scan returned.
 */
struct pros_prosenjitfs_integrity_report {
    int status;
    uint32_t first_error_location;
    uint64_t inodes_checked;
    uint64_t block_bitmap_bits_checked;
    uint64_t orphaned_blocks_reported;
    uint64_t first_orphaned_block;
    uint64_t orphaned_inodes_reported;
    uint64_t first_orphaned_inode;
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
/* Read-only integrity scan for an active mount; returns EBUSY on provider-I/O contention. */
int prosenjitfs_check_integrity(const struct pros_prosenjitfs *fs);
int prosenjitfs_check_integrity_report(
    const struct pros_prosenjitfs *fs,
    struct pros_prosenjitfs_integrity_report *report);
int prosenjitfs_selftest(void);

#endif

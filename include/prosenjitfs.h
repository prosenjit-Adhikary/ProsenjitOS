#ifndef PROSENJITFS_H
#define PROSENJITFS_H

#include <stdint.h>
#include "prosenjitfs_format.h"

/*
 * Public Native ProsenjitFS API contract.
 *
 * Kernel-private state and diagnostic helpers remain in
 * kernel/include/prosenjitfs.h. This header intentionally exposes only
 * the stable filesystem interface needed across the kernel/VFS boundary.
 */

#define PROSENJITFS_OK          0
#define PROSENJITFS_EINVAL     -1
#define PROSENJITFS_EIO        -2
#define PROSENJITFS_ENOTSUP    -3
#define PROSENJITFS_ECORRUPT   -4
#define PROSENJITFS_ENOSPC     -5
#define PROSENJITFS_ENOENT     -6
#define PROSENJITFS_EEXIST     -7
#define PROSENJITFS_EOVERFLOW  -8
#define PROSENJITFS_ENOTDIR    -9
#define PROSENJITFS_ENOTEMPTY -10
#define PROSENJITFS_EPERM     -11
#define PROSENJITFS_EBUSY     -12
#define PROSENJITFS_ESTALE    -13

struct pros_block_device;
struct pros_prosenjitfs;
struct pros_prosenjitfs_inode;

uint32_t prosenjitfs_crc32c(const void *, uint32_t);
int prosenjitfs_format(struct pros_block_device *);
int prosenjitfs_probe(struct pros_block_device *);
int prosenjitfs_mount(struct pros_prosenjitfs *, struct pros_block_device *,
                      uint8_t read_only);
int prosenjitfs_unmount(struct pros_prosenjitfs *);

/*
 * Reserve a contiguous data-block range. On EIO with the mount forced
 * read-only, *first_block identifies a candidate whose commit outcome is
 * indeterminate; do not use it until remount/recovery. If recovery confirms
 * it is allocated and it was not attached to an inode, it may be reclaimed.
 */
int prosenjitfs_alloc_blocks(struct pros_prosenjitfs *, uint32_t, uint64_t *);
/* Keep-size contiguous preallocation; v1 bounds one request to eight blocks. */
int prosenjitfs_preallocate_blocks(struct pros_prosenjitfs *, uint64_t,
                                   uint64_t, uint32_t);
int prosenjitfs_alloc_inode(struct pros_prosenjitfs *, uint64_t *);
/*
 * Release only an allocated FREE inode reservation. This also provides the
 * explicit recovery path for an indeterminate alloc_inode() candidate after
 * remount; published inodes and inode zero/root are never releasable here.
 */
int prosenjitfs_free_inode(struct pros_prosenjitfs *, uint64_t);
int prosenjitfs_free_blocks(struct pros_prosenjitfs *, uint64_t, uint32_t);

/* Read an inode from an active mount; returns EBUSY on I/O-gate contention. */
int prosenjitfs_read_inode(const struct pros_prosenjitfs *,
                           uint64_t, struct pros_prosenjitfs_inode *);
/*
 * Metadata-only update for an already-published inode. The record's type,
 * generation, parent, links, size, allocation, extent and directory-index
 * fields must remain unchanged. This API cannot publish new inodes or alter
 * namespace/extent ownership; those changes require an atomic VFS operation.
 */
int prosenjitfs_write_inode(struct pros_prosenjitfs *,
                            uint64_t,
                            const struct pros_prosenjitfs_inode *);

#endif

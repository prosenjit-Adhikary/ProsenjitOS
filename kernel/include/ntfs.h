#ifndef PROSENJIT_NTFS_H
#define PROSENJIT_NTFS_H

#include <stdint.h>
#include "vfs.h"

#define PROS_NTFS_OK          0
#define PROS_NTFS_EINVAL     -1
#define PROS_NTFS_EIO        -2
#define PROS_NTFS_ENOTSUP    -3
#define PROS_NTFS_ECORRUPT   -4
#define PROS_NTFS_EOVERFLOW  -5
#define PROS_NTFS_MFT_RUN_MAX 16u
#define PROS_NTFS_VFS_NODE_MAX 32u

struct pros_ntfs_vfs_node {
    struct pros_vfs_vnode vnode;
    uint64_t record_number;
    uint64_t data_size;
    uint8_t used;
};

struct pros_ntfs_run {
    uint64_t lcn;
    uint64_t clusters;
    uint8_t sparse;
};

struct pros_ntfs {
    struct pros_vfs_mount mount;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mftmirr_lcn;
    uint32_t clusters_per_file_record;
    uint32_t bytes_per_file_record;
    struct pros_ntfs_run mft_runs[PROS_NTFS_MFT_RUN_MAX];
    uint32_t mft_run_count;
    struct pros_ntfs_vfs_node vfs_nodes[PROS_NTFS_VFS_NODE_MAX];
};

int ntfs_mount(struct pros_ntfs *, struct pros_block_device *);
int ntfs_cluster_lba(const struct pros_ntfs *, uint64_t, uint64_t *);
int ntfs_read_file_record(const struct pros_ntfs *, uint64_t record_number, void *buffer, uint32_t buffer_size);
int ntfs_decode_runlist(const uint8_t *runlist, uint32_t length, struct pros_ntfs_run *runs, uint32_t capacity, uint32_t *run_count);
int ntfs_find_attribute(const void *record, uint32_t record_size, uint32_t type, uint32_t *offset);
int ntfs_attribute_data(const void *record, uint32_t record_size, uint32_t attr_offset, const uint8_t **data, uint32_t *data_size);
int ntfs_attribute_runlist(const void *record, uint32_t record_size, uint32_t attr_offset, const uint8_t **runlist, uint32_t *runlist_size);
int ntfs_file_read(const struct pros_ntfs *fs, uint64_t record_number,
                   uint64_t offset, void *buffer, uint32_t size,
                   uint32_t *bytes_read);
int ntfs_file_write(struct pros_ntfs *fs, uint64_t record_number,
                    uint64_t offset, const void *buffer, uint32_t size,
                    uint32_t *bytes_written);
int ntfs_bitmap_allocate_clusters(struct pros_ntfs *fs,
                                   uint64_t cluster_count,
                                   uint64_t *first_lcn);
int ntfs_mft_allocate_record(struct pros_ntfs *fs,
                              uint64_t *record_number);
int ntfs_bootstrap_mft(struct pros_ntfs *fs);
int ntfs_file_record_fixup(void *record, uint32_t record_size, uint32_t bytes_per_sector);

struct pros_ntfs_file_name {
    uint64_t parent_reference;
    uint64_t allocated_size;
    uint64_t data_size;
    uint32_t flags;
    uint32_t reparse_tag;
    uint8_t name_length;
    uint8_t namespace_id;
    const uint8_t *name_utf16le;
};

struct pros_ntfs_index_root {
    uint32_t bytes_per_index_record;
    uint8_t clusters_per_index_record;
    uint32_t first_entry_offset;
    uint32_t entries_size;
    uint32_t allocated_size;
    uint8_t flags;
};

int ntfs_file_name_attribute(const void *record, uint32_t record_size,
                             uint32_t attr_offset,
                             struct pros_ntfs_file_name *info);
int ntfs_index_root_header(const void *record, uint32_t record_size,
                           uint32_t attr_offset,
                           struct pros_ntfs_index_root *info);
int ntfs_index_root_entry(const void *record, uint32_t record_size,
                          uint32_t attr_offset, uint32_t entry_offset,
                          uint64_t *file_reference,
                          const uint8_t **key, uint16_t *key_size,
                          uint8_t *entry_flags, uint32_t *next_entry_offset);

/*
 * Read-only directory lookup over a resident $INDEX_ROOT. This low-level
 * helper intentionally retains exact UTF-16LE semantics; filesystem-level
 * directory/path lookup applies the NTFS $UpCase table.
 */
int ntfs_index_root_lookup(const void *record, uint32_t record_size,
                           uint32_t attr_offset,
                           const uint8_t *name_utf16le, uint8_t name_length,
                           uint64_t *file_reference);

struct pros_ntfs_index_record {
    uint64_t vcn;
    uint32_t first_entry_offset;
    uint32_t entries_size;
    uint32_t allocated_size;
    uint8_t flags;
};

int ntfs_index_record_fixup(void *record, uint32_t record_size,
                            uint32_t bytes_per_sector);
int ntfs_index_record_header(const void *record, uint32_t record_size,
                             uint32_t bytes_per_sector, uint64_t expected_vcn,
                             struct pros_ntfs_index_record *info);
int ntfs_index_allocation_lookup(const struct pros_ntfs *fs,
                                 const void *record, uint32_t record_size,
                                 uint32_t index_root_attr_offset,
                                 uint32_t index_allocation_attr_offset,
                                 const uint8_t *name_utf16le,
                                 uint8_t name_length,
                                 uint64_t *file_reference);

/*
 * Read-only directory and path lookup. Path components use NTFS $UpCase
 * case folding. NTFS directory record 5 ($Root) is used as the starting point.
 */
int ntfs_directory_lookup(const struct pros_ntfs *fs,
                          uint64_t directory_record_number,
                          const uint8_t *name_utf16le, uint8_t name_length,
                          uint64_t *file_reference);
int ntfs_path_lookup(const struct pros_ntfs *fs,
                     const uint8_t *const *components,
                     const uint8_t *component_lengths,
                     uint32_t component_count,
                     uint64_t *file_reference);

int ntfs_selftest(void);

#endif

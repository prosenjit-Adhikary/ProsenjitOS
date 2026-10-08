#ifndef PROSENJITFS_FORMAT_H
#define PROSENJITFS_FORMAT_H

#include <stdint.h>

#define PROSENJITFS_MAGIC "PJSFS01"
#define PROSENJITFS_MAGIC_LEN 8u
#define PROSENJITFS_VERSION 1u
#define PROSENJITFS_MAX_NAME 255u
#define PROSENJITFS_MAX_TX_BLOCKS 16u
#define PROSENJITFS_DIR_BUCKETS 32u
#define PROSENJITFS_INODE_SIZE 256u

#define PROSENJITFS_FEATURE_CRC32C   (1u << 0)
#define PROSENJITFS_FEATURE_JOURNAL  (1u << 1)
#define PROSENJITFS_FEATURE_EXTENTS  (1u << 2)
#define PROSENJITFS_FEATURE_UTF8     (1u << 3)

#define PROSENJITFS_INODE_FREE  0u
#define PROSENJITFS_INODE_FILE  1u
#define PROSENJITFS_INODE_DIR   2u
#define PROSENJITFS_INODE_SYMLINK 3u

#define PROSENJITFS_INODE_FLAG_SPARSE (1u << 0)

#define PROSENJITFS_JOURNAL_MAGIC 0x50534A4Au
#define PROSENJITFS_JOURNAL_COMMIT 0x50534A43u
#define PROSENJITFS_DIR_MAGIC 0x50534449u
#define PROSENJITFS_ENTRY_MAGIC 0x5053454Eu
#define PROSENJITFS_EXTENT_MAGIC 0x50534558u

struct __attribute__((packed)) pros_prosenjitfs_superblock {
    uint8_t magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t features;
    uint8_t uuid[16];
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t journal_start;
    uint32_t journal_blocks;
    uint64_t block_bitmap_start;
    uint32_t block_bitmap_blocks;
    uint64_t inode_bitmap_start;
    uint32_t inode_bitmap_blocks;
    uint64_t inode_table_start;
    uint32_t inode_table_blocks;
    uint64_t data_start;
    uint16_t inode_size;
    uint64_t root_inode;
    uint64_t inode_count;
    uint16_t max_name;
    uint64_t transaction_sequence;
    uint32_t checksum;
};

struct __attribute__((packed)) pros_prosenjitfs_inode {
    uint8_t type;
    uint8_t flags;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t links;
    uint64_t generation;
    uint64_t size;
    uint64_t allocated_bytes;
    uint64_t parent_inode;
    uint64_t extent_head;
    uint64_t dir_index;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t checksum;
    uint8_t reserved[164];
};

struct __attribute__((packed)) pros_prosenjitfs_extent {
    uint32_t magic;
    uint32_t reserved;
    uint64_t logical_block;
    uint64_t physical_block;
    uint64_t block_count;
    uint64_t next;
    uint32_t checksum;
};

struct __attribute__((packed)) pros_prosenjitfs_dir_index {
    uint32_t magic;
    uint16_t version;
    uint16_t bucket_count;
    uint64_t inode;
    uint64_t generation;
    uint64_t heads[PROSENJITFS_DIR_BUCKETS];
    uint32_t checksum;
};

struct __attribute__((packed)) pros_prosenjitfs_dir_entry {
    uint32_t magic;
    uint64_t next;
    uint64_t inode;
    uint32_t name_hash;
    uint8_t type;
    uint8_t flags;
    uint16_t name_length;
    uint32_t checksum;
    uint8_t name[PROSENJITFS_MAX_NAME + 1u];
};

struct __attribute__((packed)) pros_prosenjitfs_journal_header {
    uint32_t magic;
    uint32_t record_count;
    uint64_t transaction_id;
    uint64_t targets[PROSENJITFS_MAX_TX_BLOCKS];
    uint32_t checksum;
};

struct __attribute__((packed)) pros_prosenjitfs_journal_commit {
    uint32_t magic;
    uint32_t record_count;
    uint64_t transaction_id;
    uint32_t checksum;
};

/*
 * The filesystem format is serialized, not compiler-native. These assertions
 * make accidental field/padding changes a build-time format break instead of
 * silently changing the on-disk ABI.
 */
_Static_assert(sizeof(struct pros_prosenjitfs_superblock) == 132u,
               "ProsenjitFS superblock layout changed");
_Static_assert(sizeof(struct pros_prosenjitfs_inode) == PROSENJITFS_INODE_SIZE,
               "ProsenjitFS inode layout changed");
_Static_assert(sizeof(struct pros_prosenjitfs_extent) == 44u,
               "ProsenjitFS extent layout changed");
_Static_assert(sizeof(struct pros_prosenjitfs_dir_index) == 284u,
               "ProsenjitFS directory-index layout changed");
_Static_assert(sizeof(struct pros_prosenjitfs_dir_entry) == 288u,
               "ProsenjitFS directory-entry layout changed");
_Static_assert(sizeof(struct pros_prosenjitfs_journal_header) == 148u,
               "ProsenjitFS journal-header layout changed");
_Static_assert(sizeof(struct pros_prosenjitfs_journal_commit) == 20u,
               "ProsenjitFS journal-commit layout changed");

#endif

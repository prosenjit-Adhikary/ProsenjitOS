}

static int pfs_read_dir_index(const struct pros_prosenjitfs *fs,
                              const struct pros_prosenjitfs_inode *inode,
                              struct pros_prosenjitfs_dir_index *index)
{
    uint8_t block[4096];
    uint32_t checksum;
    /*
     * The root directory index is the one intentional metadata exception:
     * format() places it at the last inode-table block (data_start - 1).
     * All other directory indexes live in the data region. Keep that legacy
     * on-disk layout valid while still rejecting pointers into earlier
     * metadata regions.
     */
    if (!fs || !inode || !index || inode->type != PROSENJITFS_INODE_DIR ||
        inode->dir_index < 2u || inode->dir_index >= fs->sb.total_blocks ||
        fs->sb.block_size > sizeof(block) ||
        (inode->dir_index < fs->sb.data_start &&
         inode->dir_index != fs->sb.data_start - 1u))
        return PROSENJITFS_ECORRUPT;
    {
        int rc = pfs_read_block(fs->mount.device, inode->dir_index, block);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    pfs_copy(index, block, sizeof(*index));
    if (index->magic != PROSENJITFS_DIR_MAGIC ||
        index->version != PROSENJITFS_VERSION ||
        index->bucket_count != PROSENJITFS_DIR_BUCKETS ||
        index->inode >= fs->sb.inode_count ||
        index->generation != inode->generation)
        return PROSENJITFS_ECORRUPT;
    if (inode->dir_index >= fs->sb.data_start) {
        int alloc_rc = pfs_data_range_allocated(fs, inode->dir_index, 1u);
        if (alloc_rc != PROSENJITFS_OK)
            return alloc_rc;
    }
    checksum = pfs_crc_with_zero32(block, sizeof(*index),
                                   (uint32_t)offsetof(
                                       struct pros_prosenjitfs_dir_index,
                                       checksum));
    if (checksum != index->checksum)
        return PROSENJITFS_ECORRUPT;
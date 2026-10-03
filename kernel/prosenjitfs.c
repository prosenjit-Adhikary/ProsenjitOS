    return (int)done;
}

static int pfs_file_write(struct pros_vfs_file *file, uint32_t offset,
                          uint32_t length, const void *buffer)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;

    if (!file || !file->vnode || !buffer)
        return PROS_VFS_EINVAL;
    if (file->mount->read_only || !file->mount->active)
        return PROS_VFS_EPERM;
    if (length == 0u)
        return 0;
    if ((uint64_t)offset + length > UINT64_MAX)
        return PROS_VFS_EOVERFLOW;

    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data || !data->fs)
        return PROS_VFS_EIO;
    if (prosenjitfs_read_inode(data->fs, data->inode_number, &inode) !=
        PROSENJITFS_OK)
        return PROS_VFS_EIO;

    uint32_t done = 0u;
    while (done < length) {
        uint64_t position = (uint64_t)offset + done;
        uint64_t logical_block = position / data->fs->sb.block_size;
        uint32_t in_block = (uint32_t)(position % data->fs->sb.block_size);
        uint32_t want = length - done;
        uint8_t data_image[4096];
        uint8_t extent_image[4096];
        uint8_t tail_image[4096];
        uint8_t inode_image[4096];
        struct pros_prosenjitfs_extent extent;
        uint64_t extent_lba = 0u, tail_lba = 0u;
        uint64_t physical = 0u, new_extent_lba = 0u;
        uint64_t inode_lba;
        uint32_t inode_offset;
        int rc;

        if (want > data->fs->sb.block_size - in_block)
            want = data->fs->sb.block_size - in_block;

        rc = pfs_find_extent(data->fs, &inode, logical_block, &extent,
                             &extent_lba, &tail_lba);
        if (rc == PROSENJITFS_ENOENT) {
            rc = pfs_find_free_data_block_except(data->fs, 0u, &physical);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_find_free_data_block_except(data->fs, physical,
                                                 &new_extent_lba);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            pfs_zero(data_image, data->fs->sb.block_size);
            pfs_zero(extent_image, data->fs->sb.block_size);
            pfs_zero(tail_image, data->fs->sb.block_size);
            pfs_zero(inode_image, data->fs->sb.block_size);

            if (in_block != 0u) {
                /*
                 * A new block is initially a hole. Preserve the zero-filled
                 * prefix and only replace the bytes being written.
                 */
            }
            for (uint32_t i = 0u; i < want; ++i)
                data_image[in_block + i] =
                    ((const uint8_t *)buffer)[done + i];

            struct pros_prosenjitfs_extent new_extent;
            pfs_zero(&new_extent, sizeof(new_extent));
            new_extent.magic = PROSENJITFS_EXTENT_MAGIC;
            new_extent.logical_block = logical_block;
            new_extent.physical_block = physical;
            new_extent.block_count = 1u;
            new_extent.next = 0u;
            pfs_copy(extent_image, &new_extent, sizeof(new_extent));
            pfs_put32(extent_image +
                      offsetof(struct pros_prosenjitfs_extent, checksum),
                      pfs_crc_with_zero32(extent_image, sizeof(new_extent),
                                          (uint32_t)offsetof(
                                              struct pros_prosenjitfs_extent,
                                              checksum)));

            rc = pfs_inode_block_location(data->fs, data->inode_number,
                                          &inode_lba, &inode_offset);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_read_block(data->fs->mount.device, inode_lba, inode_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, PROSENJITFS_EIO);

            if (tail_lba != 0u) {
                rc = pfs_read_block(data->fs->mount.device, tail_lba, tail_image);
                if (rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, PROSENJITFS_EIO);
                struct pros_prosenjitfs_extent tail;
                pfs_copy(&tail, tail_image, sizeof(tail));
                tail.next = new_extent_lba;
                pfs_copy(tail_image, &tail, sizeof(tail));
                pfs_put32(tail_image +
                          offsetof(struct pros_prosenjitfs_extent, checksum),
                          pfs_crc_with_zero32(tail_image, sizeof(tail),
                                              (uint32_t)offsetof(
                                                  struct pros_prosenjitfs_extent,
                                                  checksum)));
            }

            pfs_copy(&inode, inode_image + inode_offset, sizeof(inode));
            if (inode.extent_head == 0u)
                inode.extent_head = new_extent_lba;
            inode.size = ((uint64_t)offset + done + want > inode.size)
                       ? (uint64_t)offset + done + want : inode.size;
            if (inode.allocated_bytes >
                UINT64_MAX - data->fs->sb.block_size)
                return pfs_file_write_error(done, PROSENJITFS_EOVERFLOW);
            inode.allocated_bytes += data->fs->sb.block_size;
            pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
            pfs_put32(inode_image + inode_offset +
                      offsetof(struct pros_prosenjitfs_inode, checksum),
                      pfs_crc_with_zero32(inode_image + inode_offset,
                                          sizeof(inode),
                                          (uint32_t)offsetof(
                                              struct pros_prosenjitfs_inode,
                                              checksum)));

            rc = pfs_tx_reset(data->fs);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_allocator_stage(data->fs, physical, 1u, 1u);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_allocator_stage(data->fs, new_extent_lba, 1u, 1u);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_tx_add(data->fs, physical, data_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_tx_add(data->fs, new_extent_lba, extent_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            if (tail_lba != 0u) {
                rc = pfs_tx_add(data->fs, tail_lba, tail_image);
                if (rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, rc);
            }
            rc = pfs_tx_add(data->fs, inode_lba, inode_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_tx_commit(data->fs);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
        } else if (rc == PROSENJITFS_OK) {
            uint64_t delta = logical_block - extent.logical_block;
            uint8_t inode_changed = 0u;
            if (pfs_lba_add(data->fs->mount.device, extent.physical_block,
                            delta, &physical) != PROSENJITFS_OK)
                return pfs_file_write_error(done, PROSENJITFS_EOVERFLOW);
            if (physical < data->fs->sb.data_start ||
                physical >= data->fs->sb.total_blocks)
                return pfs_file_write_error(done, PROSENJITFS_ECORRUPT);
            rc = pfs_read_block(data->fs->mount.device, physical, data_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, PROSENJITFS_EIO);
            for (uint32_t i = 0u; i < want; ++i)
                data_image[in_block + i] =
                    ((const uint8_t *)buffer)[done + i];

            if ((uint64_t)offset + done + want > inode.size) {
                inode.size = (uint64_t)offset + done + want;
                inode_changed = 1u;
                rc = pfs_inode_block_location(data->fs, data->inode_number,
                                               &inode_lba, &inode_offset);
                if (rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, rc);
                rc = pfs_read_block(data->fs->mount.device, inode_lba,
                                    inode_image);
                if (rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, PROSENJITFS_EIO);
                pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
                pfs_put32(inode_image + inode_offset +
                          offsetof(struct pros_prosenjitfs_inode, checksum),
                          pfs_crc_with_zero32(inode_image + inode_offset,
                                              sizeof(inode),
                                              (uint32_t)offsetof(
                                                  struct pros_prosenjitfs_inode,
                                                  checksum)));
            }

            if (pfs_tx_reset(data->fs) != PROSENJITFS_OK ||
                pfs_tx_add(data->fs, physical, data_image) != PROSENJITFS_OK ||
                (inode_changed &&
                 pfs_tx_add(data->fs, inode_lba, inode_image) != PROSENJITFS_OK) ||
                pfs_tx_commit(data->fs) != PROSENJITFS_OK)
                return pfs_file_write_error(done, PROSENJITFS_EIO);
        } else {
            return pfs_file_write_error(done, rc);
        }

        done += want;
        file->vnode->size = inode.size;
    }
    return (int)done;
}

static int pfs_file_truncate(struct pros_vfs_file *file, uint64_t size)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    uint64_t old_size, keep_blocks;
    uint64_t current, previous = 0u;
    uint64_t steps = 0u;

    if (!file || !file->vnode || !file->mount ||
        size > UINT32_MAX)
        return PROS_VFS_EINVAL;
    if (file->mount->read_only || !file->mount->active)
        return PROS_VFS_EPERM;
    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data || !data->fs)
        return PROS_VFS_EIO;
    if (prosenjitfs_read_inode(data->fs, data->inode_number, &inode) !=
        PROSENJITFS_OK)
        return PROS_VFS_EIO;

    old_size = inode.size;
    if (size == old_size)
        return PROS_VFS_OK;

    /*
     * Growing a file is deliberately implemented as zero-filled writes.
     * This preserves the existing extent allocator and guarantees that
     * bytes in the newly visible range read back as zero after remount.
     */
    if (size > old_size) {
        static const uint8_t zeros[512] = { 0 };
        uint64_t left = size - old_size;
        uint64_t position = old_size;
        while (left != 0u) {
            uint32_t chunk = left > sizeof(zeros) ? sizeof(zeros) : (uint32_t)left;
            int rc = pfs_file_write(file, (uint32_t)position, chunk, zeros);
            if (rc != (int)chunk)
                return rc < 0 ? rc : PROS_VFS_EIO;
            position += chunk;
            left -= chunk;
        }
        return PROS_VFS_OK;
    }
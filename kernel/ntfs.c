
static int ntfs_test_read(struct pros_block_device *device, uint64_t lba,
                          uint32_t count, void *buffer)
{
    struct ntfs_test_disk *disk =
        (struct ntfs_test_disk *)device->private_data;
    if (!disk || !buffer || count == 0u)
        return PROS_BLOCK_EIO;
    for (uint32_t s = 0; s < count; ++s) {
        uint8_t *dst = (uint8_t *)buffer + s * 512u;
        for (uint32_t i = 0; i < 512u; ++i) dst[i] = 0;
        if (lba + s == 0u)
            for (uint32_t i = 0; i < 512u; ++i) dst[i] = disk->boot[i];
        else if (lba + s == 32u || lba + s == 33u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->mft_record[(lba + s - 32u) * 512u + i];
        else if (lba + s == 54u || lba + s == 55u)
            for (uint32_t i = 0u; i < 512u; ++i)
                dst[i] = disk->file_record[(lba + s - 54u) * 512u + i];
        else if (lba + s == 56u || lba + s == 57u)
            for (uint32_t i = 0u; i < 512u; ++i)
                dst[i] = disk->nonresident_record[(lba + s - 56u) * 512u + i];
        else if (lba + s == 42u || lba + s == 43u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->root_record[(lba + s - 42u) * 512u + i];
        else if (lba + s == 44u || lba + s == 45u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->bitmap_record[(lba + s - 44u) * 512u + i];
        else if (lba + s >= 336u && lba + s < 368u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->nonresident_data[(lba + s - 336u) * 512u + i];
        else if (lba + s >= 368u && lba + s < 369u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->bitmap_data[(lba + s - 368u) * 512u + i];
        else if (lba + s == 52u || lba + s == 53u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->upcase_record[(lba + s - 52u) * 512u + i];
        else if (lba + s >= 80u && lba + s < 336u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->upcase_data[(lba + s - 80u) * 512u + i];
        else if (lba + s >= 64u && lba + s < 72u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->index_record[(lba + s - 64u) * 512u + i];
    }
    return PROS_BLOCK_OK;
}



static int ntfs_test_write(struct pros_block_device *device, uint64_t lba,
                           uint32_t count, const void *buffer)
{
    struct ntfs_test_disk *disk =
        (struct ntfs_test_disk *)device->private_data;
    if (!disk || !buffer || count == 0u)
        return PROS_BLOCK_EIO;

    for (uint32_t s = 0u; s < count; ++s) {
        const uint8_t *src = (const uint8_t *)buffer + s * 512u;
        uint64_t sector = lba + s;
        if (sector == 0u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->boot[i] = src[i];
        } else if (sector == 32u || sector == 33u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->mft_record[(sector - 32u) * 512u + i] = src[i];
        } else if (sector == 54u || sector == 55u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->file_record[(sector - 54u) * 512u + i] = src[i];
        } else if (sector == 56u || sector == 57u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->nonresident_record[(sector - 56u) * 512u + i] = src[i];
        } else if (sector >= 336u && sector < 368u) {
            for (uint32_t i = 0u; i < 512u; ++i)
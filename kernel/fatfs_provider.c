#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "fatfs_provider.h"
#include "fatfs_disk.h"
#include "heap.h"
#include "vfs.h"
#include "vfs_provider.h"
#include "ff.h"
#include "diskio.h"

#define FATFS_VOLUME_MAX 8u

struct fatfs_volume {
    FATFS fs;
    struct pros_vfs_mount *mount;
    struct pros_block_device *device;
    uint8_t pdrv;
    uint8_t allocated;
    uint8_t read_only;
    char drive[3];
};

struct fatfs_node_data {
    struct fatfs_volume *volume;
    uint32_t path_length;
    char path[];
};

struct fatfs_open_file {
    FIL file;
    struct fatfs_node_data *node;
};

static struct fatfs_volume fatfs_volumes[FATFS_VOLUME_MAX];
static uint8_t fatfs_api_busy;
static uint8_t fatfs_provider_state;

static const struct pros_vfs_vnode_ops fatfs_vnode_ops;
static const struct pros_vfs_file_ops fatfs_file_ops;

static int fatfs_enter(void)
{
    return __atomic_test_and_set(&fatfs_api_busy, __ATOMIC_ACQUIRE) ?
               PROS_VFS_EBUSY : PROS_VFS_OK;
}

static void fatfs_leave(void)
{
    __atomic_clear(&fatfs_api_busy, __ATOMIC_RELEASE);
}

static uint16_t fatfs_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t fatfs_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t fatfs_le64(const uint8_t *p)
{
    return (uint64_t)fatfs_le32(p) |
           ((uint64_t)fatfs_le32(p + 4u) << 32);
}

static int fatfs_power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static int fatfs_probe(struct pros_block_device *device)
{
    uint8_t sector[4096];
    uint32_t bytes_per_sector, sectors_per_cluster;
    uint64_t total_sectors, fat_sectors, data_sectors, cluster_count;
    uint32_t reserved, fats;
    int rc;

    if (!device || !device->read || device->sector_count < 64u ||
        device->sector_size < 512u || device->sector_size > sizeof(sector) ||
        !fatfs_power_of_two(device->sector_size))
        return PROS_VFS_PROVIDER_NO_MATCH;

    rc = block_device_read(device, 0u, 1u, sector);
    if (rc != PROS_BLOCK_OK)
        return PROS_VFS_EIO;
    if (sector[510] != 0x55u || sector[511] != 0xAAu)
        return PROS_VFS_PROVIDER_NO_MATCH;

    if (memcmp(&sector[3], "EXFAT   ", 8u) == 0) {
        uint8_t sector_shift = sector[108];
        uint8_t cluster_shift = sector[109];
        uint8_t number_of_fats = sector[110];
        uint64_t volume_length = fatfs_le64(&sector[72]);
        uint32_t fat_offset = fatfs_le32(&sector[80]);
        uint32_t fat_length = fatfs_le32(&sector[84]);
        uint32_t heap_offset = fatfs_le32(&sector[88]);
        uint32_t cluster_count = fatfs_le32(&sector[92]);
        uint32_t root_cluster = fatfs_le32(&sector[96]);
        uint64_t fat_end, heap_sectors;

        if (sector_shift < 9u || sector_shift > 12u ||
            (1u << sector_shift) != device->sector_size ||
            cluster_shift > 25u - sector_shift ||
            (number_of_fats != 1u && number_of_fats != 2u) ||
            volume_length == 0u || volume_length > device->sector_count ||
            fat_offset < 24u || fat_length == 0u || cluster_count == 0u ||
            cluster_count > 0xFFFFFFF5u ||
            root_cluster < 2u || root_cluster > cluster_count + 1u)
            return PROS_VFS_PROVIDER_NO_MATCH;

        if ((uint64_t)fat_length >
            (UINT64_MAX - fat_offset) / number_of_fats)
            return PROS_VFS_PROVIDER_NO_MATCH;
        fat_end = (uint64_t)fat_offset +
                  (uint64_t)fat_length * number_of_fats;
        if (fat_end > heap_offset || heap_offset >= volume_length)
            return PROS_VFS_PROVIDER_NO_MATCH;

        heap_sectors = (uint64_t)cluster_count << cluster_shift;
        if (heap_sectors > volume_length - heap_offset ||
            (uint64_t)fat_length * device->sector_size / 4u <
                (uint64_t)cluster_count + 2u)
            return PROS_VFS_PROVIDER_NO_MATCH;
        return PROS_VFS_OK;
    }

    bytes_per_sector = fatfs_le16(&sector[11]);
    sectors_per_cluster = sector[13];
    reserved = fatfs_le16(&sector[14]);
    fats = sector[16];
    total_sectors = fatfs_le16(&sector[19]);
    if (total_sectors == 0u)
        total_sectors = fatfs_le32(&sector[32]);
    fat_sectors = fatfs_le32(&sector[36]);

    /* FAT32 classification is based on BPB geometry, not the text label. */
    if (bytes_per_sector != device->sector_size ||
        !fatfs_power_of_two(sectors_per_cluster) ||
        sectors_per_cluster > 128u || reserved == 0u ||
        (fats != 1u && fats != 2u) || fat_sectors == 0u ||
        fatfs_le16(&sector[17]) != 0u ||
        fatfs_le16(&sector[22]) != 0u ||
        fatfs_le32(&sector[44]) < 2u ||
        total_sectors == 0u || total_sectors > device->sector_count)
        return PROS_VFS_PROVIDER_NO_MATCH;

    if (fat_sectors > (UINT64_MAX - reserved) / fats)
        return PROS_VFS_PROVIDER_NO_MATCH;
    {
        uint64_t overhead = (uint64_t)reserved +
                            (uint64_t)fats * fat_sectors;
        if (overhead >= total_sectors)
            return PROS_VFS_PROVIDER_NO_MATCH;
        data_sectors = total_sectors - overhead;
    }
    cluster_count = data_sectors / sectors_per_cluster;
    if (cluster_count < 65525u || cluster_count > 0x0FFFFFF5u)
        return PROS_VFS_PROVIDER_NO_MATCH;
    return PROS_VFS_OK;
}

static int fatfs_result_to_vfs(FRESULT result)
{
    switch (result) {
    case FR_OK: return PROS_VFS_OK;
    case FR_NO_FILE:
    case FR_NO_PATH: return PROS_VFS_ENOENT;
    case FR_EXIST: return PROS_VFS_EEXIST;
    case FR_DENIED: return PROS_VFS_ENOSPC;
    case FR_WRITE_PROTECTED: return PROS_VFS_EPERM;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER: return PROS_VFS_EINVAL;
    case FR_INVALID_OBJECT: return PROS_VFS_ESTALE;
    case FR_NO_FILESYSTEM:
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED: return PROS_VFS_ENOTSUP;
    case FR_NOT_ENOUGH_CORE: return PROS_VFS_ENOSPC;
    case FR_TOO_MANY_OPEN_FILES:
    case FR_LOCKED:
    case FR_TIMEOUT: return PROS_VFS_EBUSY;
    case FR_INT_ERR: return PROS_VFS_ECORRUPT;
    case FR_DISK_ERR:
    case FR_NOT_READY: return PROS_VFS_EIO;
    default: return PROS_VFS_EIO;
    }
}

static int fatfs_api_path(struct fatfs_volume *volume, const char *path,
                          char *output, uint32_t capacity)
{
    uint32_t i = 0u, j = 0u;
    if (!volume || !path || !output || capacity < 4u)
        return PROS_VFS_EINVAL;
    output[i++] = volume->drive[0];
    output[i++] = ':';
    if (path[0] != '/')
        output[i++] = '/';
    while (path[j] != '\0') {
        if (i + 1u >= capacity)
            return PROS_VFS_EOVERFLOW;
        output[i++] = path[j++];
    }
    output[i] = '\0';
    return PROS_VFS_OK;
}

static int fatfs_valid_name(const char *name)
{
    uint32_t i;
    if (!name || name[0] == '\0' ||
        (name[0] == '.' && (name[1] == '\0' ||
                            (name[1] == '.' && name[2] == '\0'))))
        return 0;
    for (i = 0u; name[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (i >= PROS_VFS_NAME_MAX || ch < 0x20u ||
            ch == '/' || ch == '\\' || ch == ':')
            return 0;
    }
    return 1;
}

static int fatfs_join_path(const char *parent, const char *name,
                           char *output, uint32_t capacity)
{
    uint32_t parent_len = 0u, name_len = 0u, pos = 0u;
    if (!parent || !fatfs_valid_name(name) || !output)
        return PROS_VFS_EINVAL;
    while (parent[parent_len] != '\0') {
        if (parent_len >= PROS_VFS_PATH_MAX)
            return PROS_VFS_EOVERFLOW;
        ++parent_len;
    }
    while (name[name_len] != '\0')
        ++name_len;
    if (parent_len + name_len + 2u > capacity ||
        parent_len + name_len + 2u > PROS_VFS_PATH_MAX)
        return PROS_VFS_EOVERFLOW;
    for (uint32_t i = 0u; i < parent_len; ++i)
        output[pos++] = parent[i];
    if (pos == 0u || output[pos - 1u] != '/')
        output[pos++] = '/';
    for (uint32_t i = 0u; i < name_len; ++i)
        output[pos++] = name[i];
    output[pos] = '\0';
    return PROS_VFS_OK;
}

static uint16_t fatfs_mode(const FILINFO *info)
{
    uint8_t is_directory = (info->fattrib & AM_DIR) != 0u;
    uint8_t read_only = (info->fattrib & AM_RDO) != 0u;
    if (is_directory)
        return read_only ? 0555u : 0777u;
    return read_only ? 0444u : 0666u;
}

static int fatfs_node_create(struct fatfs_volume *volume, const char *path,
                             struct pros_vfs_vnode *parent,
                             enum pros_vfs_node_type type, uint64_t size,
                             struct pros_vfs_vnode **result)
{
    struct fatfs_node_data *data;
    struct pros_vfs_vnode *vnode;
    uint32_t length = 0u;
    if (!volume || !path || !result)
        return PROS_VFS_EINVAL;
    *result = 0;
    while (path[length] != '\0') {
        if (length >= PROS_VFS_PATH_MAX)
            return PROS_VFS_EOVERFLOW;
        ++length;
    }
    data = (struct fatfs_node_data *)kmalloc(sizeof(*data) + length + 1u);
    if (!data)
        return PROS_VFS_ENOSPC;
    vnode = (struct pros_vfs_vnode *)kmalloc(sizeof(*vnode));
    if (!vnode) {
        kfree(data);
        return PROS_VFS_ENOSPC;
    }
    if (parent && vfs_vnode_get(parent) != PROS_VFS_OK) {
        kfree(vnode);
        kfree(data);
        return PROS_VFS_ESTALE;
    }
    data->volume = volume;
    data->path_length = length;
    for (uint32_t i = 0u; i <= length; ++i)
        data->path[i] = path[i];

    if (vfs_vnode_init(vnode, volume->mount, parent, type,
                       &fatfs_vnode_ops,
                       type == PROS_VFS_NODE_FILE ? &fatfs_file_ops : 0,
                       data, size) != PROS_VFS_OK) {
        if (parent)
            (void)vfs_vnode_put(parent);
        kfree(vnode);
        kfree(data);
        return PROS_VFS_EIO;
    }
    if (vfs_vnode_set_metadata(vnode, type == PROS_VFS_NODE_DIRECTORY ?
                               0777u : 0666u, 0u, 0u) != PROS_VFS_OK) {
        if (parent)
            (void)vfs_vnode_put(parent);
        kfree(vnode);
        kfree(data);
        return PROS_VFS_EIO;
    }
    *result = vnode;
    return PROS_VFS_OK;
}

static void fatfs_vnode_release(struct pros_vfs_vnode *vnode)
{
    struct fatfs_node_data *data;
    if (!vnode)
        return;
    data = (struct fatfs_node_data *)vnode->private_data;
    if (vnode->parent)
        (void)vfs_vnode_put(vnode->parent);
    vnode->initialized = 0u;
    if (data)
        kfree(data);
    kfree(vnode);
}

static int fatfs_get_metadata(struct pros_vfs_vnode *vnode,
                              struct pros_vfs_metadata *metadata)
{
    struct fatfs_node_data *node;
    char path[PROS_VFS_PATH_MAX + 4u];
    FILINFO info;
    FRESULT fr;
    int rc;
    if (!vnode || !metadata || !vnode->private_data)
        return PROS_VFS_EINVAL;
    node = (struct fatfs_node_data *)vnode->private_data;
    rc = fatfs_api_path(node->volume, node->path, path, sizeof(path));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_stat(path, &info);
    fatfs_leave();
    if (fr != FR_OK)
        return fatfs_result_to_vfs(fr);
    metadata->uid = 0u;
    metadata->gid = 0u;
    metadata->mode = fatfs_mode(&info);
    metadata->reserved[0] = 0u;
    metadata->reserved[1] = 0u;
    return PROS_VFS_OK;
}

static int fatfs_lookup(struct pros_vfs_vnode *directory, const char *name,
                        struct pros_vfs_vnode **result)
{
    struct fatfs_node_data *parent;
    struct pros_vfs_vnode *child;
    char relative[PROS_VFS_PATH_MAX + 1u];
    char path[PROS_VFS_PATH_MAX + 4u];
    FILINFO info;
    enum pros_vfs_node_type type;
    FRESULT fr;
    int rc;
    if (result)
        *result = 0;
    if (!directory || !result || !directory->private_data ||
        directory->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    parent = (struct fatfs_node_data *)directory->private_data;
    rc = fatfs_join_path(parent->path, name, relative, sizeof(relative));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_api_path(parent->volume, relative, path, sizeof(path));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_stat(path, &info);
    fatfs_leave();
    if (fr != FR_OK)
        return fatfs_result_to_vfs(fr);
    type = (info.fattrib & AM_DIR) != 0u ?
               PROS_VFS_NODE_DIRECTORY : PROS_VFS_NODE_FILE;
    rc = fatfs_node_create(parent->volume, relative, directory, type,
                           (uint64_t)info.fsize, &child);
    if (rc != PROS_VFS_OK)
        return rc;
    if (vfs_vnode_set_metadata(child, fatfs_mode(&info), 0u, 0u) !=
        PROS_VFS_OK) {
        (void)vfs_vnode_put(child);
        return PROS_VFS_EIO;
    }
    *result = child;
    return PROS_VFS_OK;
}

static int fatfs_readdir(struct pros_vfs_vnode *directory, uint64_t *cookie,
                         struct pros_vfs_dirent *entry)
{
    struct fatfs_node_data *node;
    char path[PROS_VFS_PATH_MAX + 4u];
    DIR dir;
    FILINFO info;
    uint64_t index = 0u;
    uint8_t found = 0u;
    FRESULT fr, close_fr;
    int rc;
    if (!directory || !cookie || !entry || !directory->private_data ||
        directory->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    node = (struct fatfs_node_data *)directory->private_data;
    rc = fatfs_api_path(node->volume, node->path, path, sizeof(path));
    if (rc != PROS_VFS_OK)
        return rc;
    if (*cookie == UINT64_MAX)
        return PROS_VFS_EOVERFLOW;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        fatfs_leave();
        return fatfs_result_to_vfs(fr);
    }
    for (;;) {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == '\0')
            break;
        if ((info.fname[0] == '.' && info.fname[1] == '\0') ||
            (info.fname[0] == '.' && info.fname[1] == '.' &&
             info.fname[2] == '\0'))
            continue;
        if (index++ < *cookie)
            continue;
        {
            uint32_t length = 0u;
            while (info.fname[length] != '\0') {
                if (length >= PROS_VFS_NAME_MAX) {
                    fr = FR_INVALID_NAME;
                    break;
                }
                ++length;
            }
            if (fr != FR_OK)
                break;
            for (uint32_t i = 0u; i <= length; ++i)
                entry->name[i] = info.fname[i];
            entry->type = (info.fattrib & AM_DIR) != 0u ?
                              PROS_VFS_NODE_DIRECTORY : PROS_VFS_NODE_FILE;
            entry->size = (uint64_t)info.fsize;
            ++*cookie;
            found = 1u;
        }
        break;
    }
    close_fr = f_closedir(&dir);
    fatfs_leave();
    if (fr != FR_OK)
        return fr == FR_INVALID_NAME ? PROS_VFS_EOVERFLOW :
               fatfs_result_to_vfs(fr);
    if (close_fr != FR_OK)
        return fatfs_result_to_vfs(close_fr);
    return found ? PROS_VFS_OK : PROS_VFS_ENOENT;
}

static int fatfs_create(struct pros_vfs_vnode *directory, const char *name,
                        struct pros_vfs_vnode **result,
                        const struct pros_vfs_credentials *auth)
{
    struct fatfs_node_data *parent;
    struct pros_vfs_vnode *created;
    char relative[PROS_VFS_PATH_MAX + 1u];
    char path[PROS_VFS_PATH_MAX + 4u];
    FIL file;
    FRESULT fr, close_fr;
    int rc;
    if (result)
        *result = 0;
    if (!directory || !result || !auth || !directory->private_data)
        return PROS_VFS_EINVAL;
    parent = (struct fatfs_node_data *)directory->private_data;
    rc = fatfs_join_path(parent->path, name, relative, sizeof(relative));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_api_path(parent->volume, relative, path, sizeof(path));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_open(&file, path, FA_CREATE_NEW | FA_WRITE);
    if (fr == FR_OK) {
        close_fr = f_close(&file);
        if (close_fr != FR_OK)
            fr = close_fr;
    }
    fatfs_leave();
    if (fr != FR_OK)
        return fatfs_result_to_vfs(fr);
    rc = fatfs_node_create(parent->volume, relative, directory,
                           PROS_VFS_NODE_FILE, 0u, &created);
    if (rc != PROS_VFS_OK) {
        if (fatfs_enter() == PROS_VFS_OK) {
            (void)f_unlink(path);
            fatfs_leave();
        }
        return rc;
    }
    *result = created;
    return PROS_VFS_OK;
}

static int fatfs_mkdir(struct pros_vfs_vnode *directory, const char *name,
                       struct pros_vfs_vnode **result,
                       const struct pros_vfs_credentials *auth)
{
    struct fatfs_node_data *parent;
    struct pros_vfs_vnode *created;
    char relative[PROS_VFS_PATH_MAX + 1u];
    char path[PROS_VFS_PATH_MAX + 4u];
    FRESULT fr;
    int rc;
    if (result)
        *result = 0;
    if (!directory || !result || !auth || !directory->private_data)
        return PROS_VFS_EINVAL;
    parent = (struct fatfs_node_data *)directory->private_data;
    rc = fatfs_join_path(parent->path, name, relative, sizeof(relative));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_api_path(parent->volume, relative, path, sizeof(path));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_mkdir(path);
    fatfs_leave();
    if (fr != FR_OK)
        return fatfs_result_to_vfs(fr);
    rc = fatfs_node_create(parent->volume, relative, directory,
                           PROS_VFS_NODE_DIRECTORY, 0u, &created);
    if (rc != PROS_VFS_OK) {
        if (fatfs_enter() == PROS_VFS_OK) {
            (void)f_unlink(path);
            fatfs_leave();
        }
        return rc;
    }
    *result = created;
    return PROS_VFS_OK;
}

static int fatfs_remove(struct pros_vfs_vnode *directory, const char *name,
                        uint8_t require_directory)
{
    struct fatfs_node_data *parent;
    char relative[PROS_VFS_PATH_MAX + 1u];
    char path[PROS_VFS_PATH_MAX + 4u];
    FILINFO info;
    FRESULT fr;
    uint8_t type_mismatch = 0u;
    int rc;
    if (!directory || !directory->private_data)
        return PROS_VFS_EINVAL;
    parent = (struct fatfs_node_data *)directory->private_data;
    rc = fatfs_join_path(parent->path, name, relative, sizeof(relative));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_api_path(parent->volume, relative, path, sizeof(path));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_stat(path, &info);
    if (fr == FR_OK &&
        (((info.fattrib & AM_DIR) != 0u) != (require_directory != 0u)))
        type_mismatch = 1u;
    else if (fr == FR_OK)
        fr = f_unlink(path);
    fatfs_leave();
    if (type_mismatch)
        return require_directory ? PROS_VFS_ENOTDIR : PROS_VFS_EISDIR;
    return fatfs_result_to_vfs(fr);
}

static int fatfs_unlink(struct pros_vfs_vnode *directory, const char *name,
                        const struct pros_vfs_credentials *auth)
{
    if (!auth)
        return PROS_VFS_EINVAL;
    return fatfs_remove(directory, name, 0u);
}

static int fatfs_rmdir(struct pros_vfs_vnode *directory, const char *name,
                       const struct pros_vfs_credentials *auth)
{
    if (!auth)
        return PROS_VFS_EINVAL;
    return fatfs_remove(directory, name, 1u);
}

static int fatfs_rename(struct pros_vfs_vnode *old_directory,
                        const char *old_name,
                        struct pros_vfs_vnode *new_directory,
                        const char *new_name,
                        const struct pros_vfs_credentials *auth)
{
    struct fatfs_node_data *old_parent, *new_parent;
    char old_relative[PROS_VFS_PATH_MAX + 1u];
    char new_relative[PROS_VFS_PATH_MAX + 1u];
    char old_path[PROS_VFS_PATH_MAX + 4u];
    char new_path[PROS_VFS_PATH_MAX + 4u];
    FRESULT fr;
    int rc;
    if (!old_directory || !new_directory || !auth ||
        !old_directory->private_data || !new_directory->private_data)
        return PROS_VFS_EINVAL;
    old_parent = (struct fatfs_node_data *)old_directory->private_data;
    new_parent = (struct fatfs_node_data *)new_directory->private_data;
    if (old_parent->volume != new_parent->volume)
        return PROS_VFS_ENOTSUP;
    rc = fatfs_join_path(old_parent->path, old_name, old_relative,
                         sizeof(old_relative));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_join_path(new_parent->path, new_name, new_relative,
                         sizeof(new_relative));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_api_path(old_parent->volume, old_relative, old_path,
                        sizeof(old_path));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_api_path(new_parent->volume, new_relative, new_path,
                        sizeof(new_path));
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_rename(old_path, new_path);
    fatfs_leave();
    return fatfs_result_to_vfs(fr);
}

static const struct pros_vfs_vnode_ops fatfs_vnode_ops = {
    .lookup = fatfs_lookup,
    .readdir = fatfs_readdir,
    .create = fatfs_create,
    .mkdir = fatfs_mkdir,
    .rmdir = fatfs_rmdir,
    .unlink = fatfs_unlink,
    .rename = fatfs_rename,
    .release = fatfs_vnode_release,
    .get_metadata = fatfs_get_metadata
};

static int fatfs_file_open(struct pros_vfs_file *file)
{
    struct fatfs_node_data *node;
    struct fatfs_open_file *opened;
    char path[PROS_VFS_PATH_MAX + 4u];
    BYTE flags = 0u;
    FRESULT fr;
    int rc;
    if (!file || !file->vnode || !file->private_data)
        return PROS_VFS_EINVAL;
    node = (struct fatfs_node_data *)file->vnode->private_data;
    if (!node || !node->volume)
        return PROS_VFS_ESTALE;
    if (node->volume->read_only && (file->flags & PROS_VFS_O_WRITE) != 0u)
        return PROS_VFS_EPERM;
    rc = fatfs_api_path(node->volume, node->path, path, sizeof(path));
    if (rc != PROS_VFS_OK)
        return rc;
    if ((file->flags & PROS_VFS_O_READ) != 0u)
        flags |= FA_READ;
    if ((file->flags & PROS_VFS_O_WRITE) != 0u)
        flags |= FA_WRITE;
    opened = (struct fatfs_open_file *)kmalloc(sizeof(*opened));
    if (!opened)
        return PROS_VFS_ENOSPC;
    opened->node = node;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK) {
        kfree(opened);
        return rc;
    }
    fr = f_open(&opened->file, path, flags);
    fatfs_leave();
    if (fr != FR_OK) {
        kfree(opened);
        return fatfs_result_to_vfs(fr);
    }
    file->private_data = opened;
    return PROS_VFS_OK;
}

static int fatfs_file_read64(struct pros_vfs_file *file, uint64_t offset,
                             uint32_t length, void *buffer)
{
    struct fatfs_open_file *opened;
    UINT transferred = 0u;
    FRESULT fr;
    int rc;
    if (!file || !buffer || !file->private_data || length > INT_MAX)
        return PROS_VFS_EOVERFLOW;
    opened = (struct fatfs_open_file *)file->private_data;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_lseek(&opened->file, (FSIZE_t)offset);
    if (fr == FR_OK)
        fr = f_read(&opened->file, buffer, (UINT)length, &transferred);
    if (fr == FR_OK)
        file->vnode->size = (uint64_t)f_size(&opened->file);
    fatfs_leave();
    if (fr != FR_OK)
        return fatfs_result_to_vfs(fr);
    return (int)transferred;
}

static int fatfs_file_write64(struct pros_vfs_file *file, uint64_t offset,
                              uint32_t length, const void *buffer)
{
    struct fatfs_open_file *opened;
    UINT transferred = 0u;
    FRESULT fr;
    int rc;
    if (!file || !file->private_data || length > INT_MAX ||
        (length != 0u && !buffer))
        return PROS_VFS_EINVAL;
    if (length == 0u)
        return 0;
    opened = (struct fatfs_open_file *)file->private_data;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    if ((file->flags & PROS_VFS_O_APPEND) != 0u)
        fr = f_lseek(&opened->file, f_size(&opened->file));
    else
        fr = f_lseek(&opened->file, (FSIZE_t)offset);
    if (fr == FR_OK)
        fr = f_write(&opened->file, buffer, (UINT)length, &transferred);
    if (fr == FR_OK)
        file->vnode->size = (uint64_t)f_size(&opened->file);
    fatfs_leave();
    if (fr != FR_OK)
        return fatfs_result_to_vfs(fr);
    return (int)transferred;
}

static int fatfs_file_truncate(struct pros_vfs_file *file, uint64_t size)
{
    struct fatfs_open_file *opened;
    FRESULT fr;
    int rc;
    if (!file || !file->private_data)
        return PROS_VFS_EINVAL;
    opened = (struct fatfs_open_file *)file->private_data;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_lseek(&opened->file, (FSIZE_t)size);
    if (fr == FR_OK)
        fr = f_truncate(&opened->file);
    if (fr == FR_OK)
        file->vnode->size = (uint64_t)f_size(&opened->file);
    fatfs_leave();
    return fatfs_result_to_vfs(fr);
}

static int fatfs_file_close(struct pros_vfs_file *file)
{
    struct fatfs_open_file *opened;
    FRESULT fr;
    int rc;
    if (!file || !file->private_data)
        return PROS_VFS_EINVAL;
    opened = (struct fatfs_open_file *)file->private_data;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_close(&opened->file);
    fatfs_leave();
    if (fr != FR_OK)
        return fatfs_result_to_vfs(fr);
    kfree(opened);
    file->private_data = 0;
    return PROS_VFS_OK;
}

static const struct pros_vfs_file_ops fatfs_file_ops = {
    .close = fatfs_file_close,
    .open = fatfs_file_open,
    .read64 = fatfs_file_read64,
    .write64 = fatfs_file_write64,
    .truncate = fatfs_file_truncate
};

static struct fatfs_volume *fatfs_volume_allocate(void)
{
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        uint8_t expected = 0u;
        if (__atomic_compare_exchange_n(&fatfs_volumes[i].allocated,
                                        &expected, 1u, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            fatfs_volumes[i].pdrv = (uint8_t)i;
            fatfs_volumes[i].drive[0] = (char)('0' + i);
            fatfs_volumes[i].drive[1] = ':';
            fatfs_volumes[i].drive[2] = '\0';
            fatfs_volumes[i].mount = 0;
            fatfs_volumes[i].device = 0;
            fatfs_volumes[i].read_only = 0u;
            return &fatfs_volumes[i];
        }
    }
    return 0;
}

static void fatfs_volume_release(struct fatfs_volume *volume)
{
    if (!volume)
        return;
    volume->mount = 0;
    volume->device = 0;
    volume->read_only = 0u;
    __atomic_store_n(&volume->allocated, 0u, __ATOMIC_RELEASE);
}

static int fatfs_mount_cleanup_locked(struct fatfs_volume *volume)
{
    FRESULT fr;
    if (!volume)
        return PROS_VFS_EINVAL;
    fr = f_mount(0, volume->drive, 0u);
    if (fr != FR_OK)
        return fatfs_result_to_vfs(fr);
    if (pros_fatfs_disk_unbind(volume->pdrv) != PROS_FATFS_DISK_OK)
        return PROS_VFS_EBUSY;
    fatfs_volume_release(volume);
    return PROS_VFS_OK;
}

static int fatfs_mount_callback(struct pros_vfs_provider *provider,
                                struct pros_vfs_mount *mount,
                                struct pros_block_device *device,
                                struct pros_partition_device *view,
                                const struct pros_partition *partition,
                                uint8_t read_only)
{
    struct fatfs_volume *volume;
    struct pros_vfs_vnode *root = 0;
    struct pros_block_device *partition_device;
    FRESULT fr;
    int rc, cleanup_rc;
    uint8_t mount_initialized = 0u;

    if (!provider || !mount || !device || !view || !partition ||
        view->parent != device || view->first_lba != partition->first_lba ||
        view->device.sector_count != partition->sector_count)
        return PROS_VFS_EINVAL;
    partition_device = &view->device;
    volume = fatfs_volume_allocate();
    if (!volume)
        return PROS_VFS_ENOSPC;
    volume->device = partition_device;
    volume->read_only = read_only ? 1u : 0u;
    if (pros_fatfs_disk_bind(volume->pdrv, partition_device,
                             volume->read_only) != PROS_FATFS_DISK_OK) {
        fatfs_volume_release(volume);
        return PROS_VFS_EBUSY;
    }

    /*
     * Keep the global FatFs gate for the complete mount transaction. In
     * particular, a failed f_mount must be detached before its drive slot or
     * FATFS work area can be reused by another volume.
     */
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK) {
        (void)pros_fatfs_disk_unbind(volume->pdrv);
        fatfs_volume_release(volume);
        return rc;
    }

    fr = f_mount(&volume->fs, volume->drive, 1u);
    if (fr != FR_OK ||
        (volume->fs.fs_type != FS_FAT32 && volume->fs.fs_type != FS_EXFAT)) {
        rc = fr == FR_OK ? PROS_VFS_ENOTSUP : fatfs_result_to_vfs(fr);
        cleanup_rc = fatfs_mount_cleanup_locked(volume);
        fatfs_leave();
        return cleanup_rc == PROS_VFS_OK ? rc : cleanup_rc;
    }

    rc = vfs_mount_init(mount, PROS_FS_PROVIDER, partition_device,
                        volume, volume->read_only);
    if (rc != PROS_VFS_OK) {
        cleanup_rc = fatfs_mount_cleanup_locked(volume);
        fatfs_leave();
        return cleanup_rc == PROS_VFS_OK ? rc : cleanup_rc;
    }
    mount_initialized = 1u;
    volume->mount = mount;

    rc = fatfs_node_create(volume, "/", 0, PROS_VFS_NODE_DIRECTORY,
                           0u, &root);
    if (rc == PROS_VFS_OK)
        rc = vfs_mount_set_root(mount, root);
    if (root)
        (void)vfs_vnode_put(root);
    if (rc != PROS_VFS_OK) {
        if (mount_initialized) {
            mount->device = 0;
            mount->private_data = 0;
            mount->type = PROS_FS_UNKNOWN;
            mount->read_only = 0u;
        }
        cleanup_rc = fatfs_mount_cleanup_locked(volume);
        fatfs_leave();
        return cleanup_rc == PROS_VFS_OK ? rc : cleanup_rc;
    }

    fatfs_leave();
    return PROS_VFS_OK;
}

static int fatfs_sync_callback(struct pros_vfs_provider *provider,
                               struct pros_vfs_mount *mount)
{
    struct fatfs_volume *volume;
    int rc;
    if (!provider || !mount || !mount->private_data)
        return PROS_VFS_EINVAL;
    volume = (struct fatfs_volume *)mount->private_data;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    rc = disk_ioctl(volume->pdrv, CTRL_SYNC, 0) == RES_OK ?
             PROS_VFS_OK : PROS_VFS_EIO;
    fatfs_leave();
    return rc;
}

static int fatfs_unmount_callback(struct pros_vfs_provider *provider,
                                  struct pros_vfs_mount *mount)
{
    struct fatfs_volume *volume;
    FRESULT fr;
    int rc;
    if (!provider || !mount || !mount->private_data)
        return PROS_VFS_EINVAL;
    volume = (struct fatfs_volume *)mount->private_data;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fr = f_mount(0, volume->drive, 0u);
    if (fr == FR_OK &&
        pros_fatfs_disk_unbind(volume->pdrv) != PROS_FATFS_DISK_OK)
        fr = FR_DISK_ERR;
    fatfs_leave();
    if (fr != FR_OK)
        return fatfs_result_to_vfs(fr);
    fatfs_volume_release(volume);
    return PROS_VFS_OK;
}

static const struct pros_vfs_provider_ops fatfs_provider_ops = {
    .probe = fatfs_probe,
    .mount = fatfs_mount_callback,
    .sync = fatfs_sync_callback,
    .unmount = fatfs_unmount_callback
};

static struct pros_vfs_provider fatfs_provider = {
    .abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION,
    .flags = PROS_VFS_PROVIDER_PARTITION_AWARE,
    .name = "fat32-exfat",
    .ops = &fatfs_provider_ops,
    .private_data = 0,
    .active_mounts = 0u
};

int pros_fatfs_provider_init(void)
{
    uint8_t state = __atomic_load_n(&fatfs_provider_state, __ATOMIC_ACQUIRE);
    if (state == 1u)
        return PROS_VFS_OK;
    if (state == 2u)
        return PROS_VFS_EBUSY;
    {
        uint8_t expected = 0u;
        if (!__atomic_compare_exchange_n(&fatfs_provider_state, &expected,
                                          2u, 0, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE))
            return PROS_VFS_EBUSY;
    }
    if (pros_vfs_provider_register(&fatfs_provider) != PROS_VFS_OK) {
        __atomic_store_n(&fatfs_provider_state, 0u, __ATOMIC_RELEASE);
        return PROS_VFS_EINVAL;
    }
    __atomic_store_n(&fatfs_provider_state, 1u, __ATOMIC_RELEASE);
    return PROS_VFS_OK;
}

struct fatfs_probe_test_media {
    uint8_t sector[4096];
    uint8_t fail_reads;
};

static int fatfs_probe_test_read(struct pros_block_device *device,
                                 uint64_t lba, uint32_t count, void *buffer)
{
    struct fatfs_probe_test_media *media =
        (struct fatfs_probe_test_media *)device->private_data;
    if (!media || !buffer || media->fail_reads || lba != 0u || count != 1u)
        return PROS_BLOCK_EIO;
    for (uint32_t i = 0u; i < device->sector_size; ++i)
        ((uint8_t *)buffer)[i] = media->sector[i];
    return PROS_BLOCK_OK;
}

static void fatfs_probe_test_signature(uint8_t *sector)
{
    sector[510] = 0x55u;
    sector[511] = 0xAAu;
}

static int fatfs_probe_selftest(void)
{
    static struct fatfs_probe_test_media media;
    struct pros_block_device device;

    for (uint32_t i = 0u; i < sizeof(media); ++i)
        ((uint8_t *)&media)[i] = 0u;
    if (block_device_init(&device, 512u, 100000u,
                          fatfs_probe_test_read, 0, &media) != PROS_BLOCK_OK)
        return -1;

    /* A valid FAT32 BPB geometry must match. */
    fatfs_probe_test_signature(media.sector);
    media.sector[11] = 0x00u;
    media.sector[12] = 0x02u;
    media.sector[13] = 1u;
    media.sector[14] = 32u;
    media.sector[16] = 2u;
    media.sector[32] = 0x70u;
    media.sector[33] = 0x11u;
    media.sector[34] = 0x01u;
    media.sector[35] = 0x00u; /* 70000 sectors */
    media.sector[36] = 0x58u;
    media.sector[37] = 0x02u; /* 600 sectors per FAT */
    media.sector[44] = 2u;
    if (fatfs_probe(&device) != PROS_VFS_OK)
        return -2;

    /* FAT16 must remain unsupported even though FatFs itself can parse it. */
    for (uint32_t i = 0u; i < sizeof(media.sector); ++i)
        media.sector[i] = 0u;
    fatfs_probe_test_signature(media.sector);
    media.sector[11] = 0x00u;
    media.sector[12] = 0x02u;
    media.sector[13] = 1u;
    media.sector[14] = 1u;
    media.sector[16] = 2u;
    media.sector[17] = 0x00u;
    media.sector[18] = 0x02u; /* 512 root entries */
    media.sector[19] = 0x40u;
    media.sector[20] = 0x9Cu; /* 40000 sectors */
    media.sector[22] = 150u;
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -3;

    /* A bounded exFAT geometry must match. */
    for (uint32_t i = 0u; i < sizeof(media.sector); ++i)
        media.sector[i] = 0u;
    fatfs_probe_test_signature(media.sector);
    for (uint32_t i = 0u; i < 8u; ++i)
        media.sector[3u + i] = (uint8_t)"EXFAT   "[i];
    media.sector[72] = 0xA0u; /* 100000 sectors */
    media.sector[73] = 0x86u;
    media.sector[74] = 0x01u;
    media.sector[80] = 24u;
    media.sector[84] = 0x20u; /* 800 sectors per FAT */
    media.sector[85] = 0x03u;
    media.sector[88] = 0x58u; /* cluster heap starts at 1624 */
    media.sector[89] = 0x06u;
    media.sector[92] = 0x30u; /* 98000 clusters */
    media.sector[93] = 0x7Fu;
    media.sector[94] = 0x01u;
    media.sector[96] = 2u;
    media.sector[108] = 9u;
    media.sector[109] = 0u;
    media.sector[110] = 2u;
    if (fatfs_probe(&device) != PROS_VFS_OK)
        return -4;

    /* A forged exFAT signature with an out-of-range volume is rejected. */
    media.sector[72] = 0xFFu;
    media.sector[73] = 0xFFu;
    media.sector[74] = 0xFFu;
    media.sector[75] = 0xFFu;
    media.sector[76] = 0xFFu;
    media.sector[77] = 0xFFu;
    media.sector[78] = 0xFFu;
    media.sector[79] = 0xFFu;
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -5;

    media.fail_reads = 1u;
    if (fatfs_probe(&device) != PROS_VFS_EIO)
        return -6;
    return PROS_VFS_OK;
}


#define FATFS_MOUNT_TEST_SECTORS 66581u
#define FATFS_MOUNT_TEST_CACHE 64u

struct fatfs_mount_test_sector {
    uint64_t lba;
    uint8_t valid;
    uint8_t data[512];
};

struct fatfs_mount_test_media {
    uint8_t boot[512];
    uint8_t fsinfo[512];
    uint8_t fat[512];
    struct fatfs_mount_test_sector cache[FATFS_MOUNT_TEST_CACHE];
    uint32_t flushes;
};

static struct fatfs_mount_test_media fatfs_mount_test_media;

static void fatfs_mount_test_put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void fatfs_mount_test_put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void fatfs_mount_test_init_media(void)
{
    memset(&fatfs_mount_test_media, 0, sizeof(fatfs_mount_test_media));
    fatfs_mount_test_media.boot[0] = 0xEBu;
    fatfs_mount_test_media.boot[1] = 0x58u;
    fatfs_mount_test_media.boot[2] = 0x90u;
    memcpy(&fatfs_mount_test_media.boot[3], "MSWIN4.1", 8u);
    fatfs_mount_test_put16(&fatfs_mount_test_media.boot[11], 512u);
    fatfs_mount_test_media.boot[13] = 1u;
    fatfs_mount_test_put16(&fatfs_mount_test_media.boot[14], 32u);
    fatfs_mount_test_media.boot[16] = 2u;
    fatfs_mount_test_media.boot[21] = 0xF8u;
    fatfs_mount_test_put16(&fatfs_mount_test_media.boot[24], 63u);
    fatfs_mount_test_put16(&fatfs_mount_test_media.boot[26], 255u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[32],
                           FATFS_MOUNT_TEST_SECTORS);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[36], 512u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[44], 2u);
    fatfs_mount_test_put16(&fatfs_mount_test_media.boot[48], 1u);
    fatfs_mount_test_put16(&fatfs_mount_test_media.boot[50], 6u);
    fatfs_mount_test_media.boot[64] = 0x80u;
    fatfs_mount_test_media.boot[66] = 0x29u;
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[67], 0x50524F53u);
    memcpy(&fatfs_mount_test_media.boot[71], "PROSENJITOS", 11u);
    memcpy(&fatfs_mount_test_media.boot[82], "FAT32   ", 8u);
    fatfs_mount_test_media.boot[510] = 0x55u;
    fatfs_mount_test_media.boot[511] = 0xAAu;

    fatfs_mount_test_put32(&fatfs_mount_test_media.fsinfo[0], 0x41615252u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.fsinfo[484], 0x61417272u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.fsinfo[488], UINT32_MAX);
    fatfs_mount_test_put32(&fatfs_mount_test_media.fsinfo[492], UINT32_MAX);
    fatfs_mount_test_put32(&fatfs_mount_test_media.fsinfo[508], 0xAA550000u);

    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[0], 0x0FFFFFF8u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[4], 0xFFFFFFFFu);
    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[8], 0x0FFFFFFFu);
}

static int fatfs_mount_test_read(struct pros_block_device *device,
                                 uint64_t lba, uint32_t count, void *buffer)
{
    struct fatfs_mount_test_media *media =
        (struct fatfs_mount_test_media *)device->private_data;
    if (!media || !buffer || lba >= FATFS_MOUNT_TEST_SECTORS ||
        count > FATFS_MOUNT_TEST_SECTORS - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t n = 0u; n < count; ++n) {
        uint64_t current = lba + n;
        uint8_t *out = (uint8_t *)buffer + n * 512u;
        uint8_t cached = 0u;
        for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
            if (media->cache[i].valid && media->cache[i].lba == current) {
                memcpy(out, media->cache[i].data, 512u);
                cached = 1u;
                break;
            }
        }
        if (cached)
            continue;
        if (current == 0u || current == 6u)
            memcpy(out, media->boot, 512u);
        else if (current == 1u || current == 7u)
            memcpy(out, media->fsinfo, 512u);
        else if (current == 32u || current == 544u)
            memcpy(out, media->fat, 512u);
        else
            memset(out, 0, 512u);
    }
    return PROS_BLOCK_OK;
}

static int fatfs_mount_test_write(struct pros_block_device *device,
                                  uint64_t lba, uint32_t count,
                                  const void *buffer)
{
    struct fatfs_mount_test_media *media =
        (struct fatfs_mount_test_media *)device->private_data;
    if (!media || !buffer || lba >= FATFS_MOUNT_TEST_SECTORS ||
        count > FATFS_MOUNT_TEST_SECTORS - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t n = 0u; n < count; ++n) {
        uint64_t current = lba + n;
        const uint8_t *input = (const uint8_t *)buffer + n * 512u;
        struct fatfs_mount_test_sector *slot = 0;
        for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
            if (media->cache[i].valid && media->cache[i].lba == current) {
                slot = &media->cache[i];
                break;
            }
            if (!media->cache[i].valid && !slot)
                slot = &media->cache[i];
        }
        if (!slot)
            return PROS_BLOCK_EIO;
        slot->lba = current;
        slot->valid = 1u;
        memcpy(slot->data, input, 512u);
    }
    return PROS_BLOCK_OK;
}

static int fatfs_mount_test_flush(struct pros_block_device *device)
{
    struct fatfs_mount_test_media *media =
        (struct fatfs_mount_test_media *)device->private_data;
    if (!media)
        return PROS_BLOCK_EINVAL;
    ++media->flushes;
    return PROS_BLOCK_OK;
}

static int fatfs_mount_selftest(struct pros_vfs_provider *provider)
{
    static const uint8_t payload[] = {'F','A','T','3','2','-','O','K','!'};
    struct pros_block_device device;
    struct pros_partition partition;
    struct pros_partition_device view;
    struct pros_vfs_mount mount = {0};
    struct pros_vfs_provider *detected = 0;
    struct pros_vfs_file file;
    struct pros_vfs_dir dir;
    struct pros_vfs_dirent entry;
    uint8_t readback[sizeof(payload)];
    uint32_t transferred = 0u;
    int rc;
    uint8_t file_open = 0u, dir_open = 0u, mounted = 0u;

    fatfs_mount_test_init_media();
    if (block_device_init(&device, 512u, FATFS_MOUNT_TEST_SECTORS,
                          fatfs_mount_test_read, fatfs_mount_test_write,
                          &fatfs_mount_test_media) != PROS_BLOCK_OK ||
        block_device_set_flush(&device, fatfs_mount_test_flush) !=
            PROS_BLOCK_OK)
        return -3;

    memset(&partition, 0, sizeof(partition));
    partition.first_lba = 0u;
    partition.sector_count = FATFS_MOUNT_TEST_SECTORS;
    partition.mbr_type = 0x0Cu;
    partition.table_type = PROS_PARTITION_MBR;
    if (partition_device_init(&view, &device, &partition) !=
        PROS_PARTITION_OK)
        return -4;

    rc = pros_vfs_provider_probe_device(&view.device,
                                        PROS_VFS_PROVIDER_PARTITION_AWARE,
                                        &detected);
    if (rc != PROS_VFS_OK || detected != provider)
        return -5;
    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 0u);
    if (rc != PROS_VFS_OK)
        return -6;
    mounted = 1u;

    rc = vfs_kernel_file_open(&mount, "/FATTEST.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    file_open = 1u;
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, sizeof(readback), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_truncate(&file, 4u) != PROS_VFS_OK ||
        vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, 4u, &transferred) != PROS_VFS_OK ||
        transferred != 4u || memcmp(payload, readback, 4u) != 0)
        goto cleanup;
    if (vfs_file_close(&file) != PROS_VFS_OK)
        goto cleanup;
    file_open = 0u;

    rc = vfs_kernel_dir_open(&mount, "/", &dir);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    dir_open = 1u;
    if (vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
        strcmp(entry.name, "FATTEST.TXT") != 0 ||
        vfs_dir_read(&dir, &entry) != PROS_VFS_ENOENT)
        goto cleanup;
    if (vfs_dir_close(&dir) != PROS_VFS_OK)
        goto cleanup;
    dir_open = 0u;

    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK)
        goto cleanup;
    mounted = 0u;

    /* Remount read-only and prove that file data persisted. */
    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 1u);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    mounted = 1u;
    rc = vfs_kernel_file_open(&mount, "/FATTEST.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    file_open = 1u;
    if (vfs_file_read(&file, readback, 4u, &transferred) != PROS_VFS_OK ||
        transferred != 4u || memcmp(payload, readback, 4u) != 0)
        goto cleanup;
    if (vfs_file_close(&file) != PROS_VFS_OK)
        goto cleanup;
    file_open = 0u;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK)
        goto cleanup;
    mounted = 0u;
    if (fatfs_mount_test_media.flushes == 0u)
        return -7;
    return PROS_VFS_OK;

cleanup:
    if (file_open)
        (void)vfs_file_close(&file);
    if (dir_open)
        (void)vfs_dir_close(&dir);
    if (mounted)
        (void)pros_vfs_provider_unmount_device(&mount);
    return rc < 0 ? rc : PROS_VFS_EIO;
}

int pros_fatfs_provider_selftest(void)
{
    if (pros_fatfs_provider_init() != PROS_VFS_OK)
        return -1;
    if (pros_vfs_provider_find("fat32-exfat") != &fatfs_provider)
        return -2;
    rc = fatfs_probe_selftest();
    if (rc != PROS_VFS_OK)
        return rc;
    return fatfs_mount_selftest(&fatfs_provider);
}

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
        if (sector_shift < 9u || sector_shift > 12u ||
            (1u << sector_shift) != device->sector_size)
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
    return (info->fattrib & AM_RDO) != 0u ? 0555u : 0777u;
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
    if (vfs_vnode_set_metadata(vnode, 0777u, 0u, 0u) != PROS_VFS_OK) {
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
    int rc;
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
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK) {
        (void)pros_fatfs_disk_unbind(volume->pdrv);
        fatfs_volume_release(volume);
        return rc;
    }
    fr = f_mount(&volume->fs, volume->drive, 1u);
    fatfs_leave();
    if (fr != FR_OK ||
        (volume->fs.fs_type != FS_FAT32 && volume->fs.fs_type != FS_EXFAT)) {
        if (fatfs_enter() == PROS_VFS_OK) {
            (void)f_mount(0, volume->drive, 0u);
            fatfs_leave();
        }
        (void)pros_fatfs_disk_unbind(volume->pdrv);
        fatfs_volume_release(volume);
        return fr == FR_OK ? PROS_VFS_ENOTSUP : fatfs_result_to_vfs(fr);
    }

    rc = vfs_mount_init(mount, PROS_FS_PROVIDER, partition_device,
                        volume, volume->read_only);
    if (rc != PROS_VFS_OK) {
        if (fatfs_enter() == PROS_VFS_OK) {
            (void)f_mount(0, volume->drive, 0u);
            fatfs_leave();
        }
        (void)pros_fatfs_disk_unbind(volume->pdrv);
        fatfs_volume_release(volume);
        return rc;
    }
    volume->mount = mount;
    rc = fatfs_node_create(volume, "/", 0, PROS_VFS_NODE_DIRECTORY,
                           0u, &root);
    if (rc == PROS_VFS_OK)
        rc = vfs_mount_set_root(mount, root);
    if (root)
        (void)vfs_vnode_put(root);
    if (rc != PROS_VFS_OK) {
        mount->device = 0;
        mount->private_data = 0;
        mount->type = PROS_FS_UNKNOWN;
        mount->read_only = 0u;
        if (fatfs_enter() == PROS_VFS_OK) {
            (void)f_mount(0, volume->drive, 0u);
            fatfs_leave();
        }
        (void)pros_fatfs_disk_unbind(volume->pdrv);
        fatfs_volume_release(volume);
        return rc;
    }
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

int pros_fatfs_provider_selftest(void)
{
    if (pros_fatfs_provider_init() != PROS_VFS_OK)
        return -1;
    return pros_vfs_provider_find("fat32-exfat") == &fatfs_provider ?
               PROS_VFS_OK : -2;
}

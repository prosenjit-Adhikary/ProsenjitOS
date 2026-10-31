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

#define FATFS_VOLUME_MAX FF_VOLUMES
#if FF_VOLUMES > 10
#error FatFs numeric drive strings support at most ten volumes
#endif

struct fatfs_open_file;

struct fatfs_volume {
    FATFS fs;
    struct fatfs_open_file *open_files;
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
    struct fatfs_volume *volume;
    struct fatfs_open_file *next;
    uint8_t orphaned;
};

static struct fatfs_volume fatfs_volumes[FATFS_VOLUME_MAX];
static uint8_t fatfs_api_busy;
static uint8_t fatfs_provider_state;
static char fatfs_path_scratch[PROS_VFS_PATH_MAX + 4u];
static char fatfs_path_scratch2[PROS_VFS_PATH_MAX + 4u];

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

static struct fatfs_volume *fatfs_volume_allocate(void);
static int fatfs_result_to_vfs(FRESULT result);
static void fatfs_volume_release(struct fatfs_volume *volume);

static int fatfs_probe(struct pros_block_device *device)
{
    struct fatfs_volume *volume;
    FRESULT fr, detach_result;
    int rc, result = PROS_VFS_PROVIDER_NO_MATCH;

    /*
     * Do not duplicate FAT12/16/32 or exFAT on-disk parsing here. Let the
     * pinned, patched upstream FatFs implementation validate the volume and
     * classify it. Probe is strictly read-only and never formats media.
     */
    if (!device || !device->read || device->sector_count < 64u ||
        device->sector_size < FF_MIN_SS ||
        device->sector_size > FF_MAX_SS ||
        (device->sector_size & (device->sector_size - 1u)) != 0u)
        return PROS_VFS_PROVIDER_NO_MATCH;

    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;

    volume = fatfs_volume_allocate();
    if (!volume) {
        fatfs_leave();
        return PROS_VFS_ENOSPC;
    }
    volume->device = device;
    volume->read_only = 1u;

    rc = pros_fatfs_disk_bind(volume->pdrv, device, 1u);
    if (rc != PROS_FATFS_DISK_OK) {
        fatfs_volume_release(volume);
        fatfs_leave();
        return PROS_VFS_EBUSY;
    }

    fr = f_mount(&volume->fs, volume->drive, 1u);
    if (fr == FR_OK) {
        if (volume->fs.fs_type == FS_FAT32 ||
            volume->fs.fs_type == FS_EXFAT)
            result = PROS_VFS_OK;
    } else if (fr == FR_DISK_ERR || fr == FR_NOT_READY) {
        result = PROS_VFS_EIO;
    }

    /* Always detach FatFs before releasing or reusing its work area. */
    detach_result = f_mount(0, volume->drive, 0u);
    if (detach_result != FR_OK) {
        fatfs_leave();
        return PROS_VFS_EIO; /* retain the slot and disk binding safely */
    }
    if (pros_fatfs_disk_unbind(volume->pdrv) != PROS_FATFS_DISK_OK) {
        fatfs_leave();
        return PROS_VFS_EBUSY; /* retain allocated state; never reuse live slot */
    }
    fatfs_volume_release(volume);
    fatfs_leave();
    return result;
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

static int fatfs_join_api_path(struct fatfs_volume *volume,
                               const char *parent, const char *name,
                               char *output)
{
    int rc;
    if (!volume || !output)
        return PROS_VFS_EINVAL;
    output[0] = volume->drive[0];
    output[1] = ':';
    rc = fatfs_join_path(parent, name, output + 2u,
                         PROS_VFS_PATH_MAX + 2u);
    return rc;
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
    FILINFO info;
    FRESULT fr;
    int rc;
    if (!vnode || !metadata || !vnode->private_data)
        return PROS_VFS_EINVAL;
    node = (struct fatfs_node_data *)vnode->private_data;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_api_path(node->volume, node->path, fatfs_path_scratch,
                        sizeof(fatfs_path_scratch));
    if (rc != PROS_VFS_OK) {
        fatfs_leave();
        return rc;
    }
    fr = f_stat(fatfs_path_scratch, &info);
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
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_join_api_path(parent->volume, parent->path, name,
                             fatfs_path_scratch);
    if (rc != PROS_VFS_OK) {
        fatfs_leave();
        return rc;
    }
    fr = f_stat(fatfs_path_scratch, &info);
    if (fr != FR_OK) {
        fatfs_leave();
        return fatfs_result_to_vfs(fr);
    }
    type = (info.fattrib & AM_DIR) != 0u ?
               PROS_VFS_NODE_DIRECTORY : PROS_VFS_NODE_FILE;
    rc = fatfs_node_create(parent->volume, fatfs_path_scratch + 2u,
                           directory, type, (uint64_t)info.fsize, &child);
    if (rc == PROS_VFS_OK &&
        vfs_vnode_set_metadata(child, fatfs_mode(&info), 0u, 0u) !=
            PROS_VFS_OK) {
        (void)vfs_vnode_put(child);
        rc = PROS_VFS_EIO;
    }
    fatfs_leave();
    if (rc == PROS_VFS_OK)
        *result = child;
    return rc;
}

static int fatfs_readdir(struct pros_vfs_vnode *directory, uint64_t *cookie,
                         struct pros_vfs_dirent *entry)
{
    struct fatfs_node_data *node;
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
    if (*cookie == UINT64_MAX)
        return PROS_VFS_EOVERFLOW;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_api_path(node->volume, node->path, fatfs_path_scratch,
                        sizeof(fatfs_path_scratch));
    if (rc != PROS_VFS_OK) {
        fatfs_leave();
        return rc;
    }
    fr = f_opendir(&dir, fatfs_path_scratch);
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
    struct fatfs_open_file *created_file;
    struct fatfs_open_file **cursor;
    FRESULT fr, close_fr;
    int rc;
    if (result)
        *result = 0;
    if (!directory || !result || !auth || !directory->private_data)
        return PROS_VFS_EINVAL;
    parent = (struct fatfs_node_data *)directory->private_data;
    created_file = (struct fatfs_open_file *)kmalloc(sizeof(*created_file));
    if (!created_file)
        return PROS_VFS_ENOSPC;
    created_file->node = 0;
    created_file->volume = parent->volume;
    created_file->next = 0;
    created_file->orphaned = 0u;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK) {
        kfree(created_file);
        return rc;
    }
    rc = fatfs_join_api_path(parent->volume, parent->path, name,
                             fatfs_path_scratch);
    if (rc != PROS_VFS_OK) {
        fatfs_leave();
        kfree(created_file);
        return rc;
    }
    fr = f_open(&created_file->file, fatfs_path_scratch,
                FA_CREATE_NEW | FA_WRITE);
    if (fr == FR_OK) {
        created_file->next = parent->volume->open_files;
        parent->volume->open_files = created_file;
        close_fr = f_close(&created_file->file);
        if (close_fr == FR_OK) {
            cursor = &parent->volume->open_files;
            while (*cursor && *cursor != created_file)
                cursor = &(*cursor)->next;
            if (*cursor == created_file)
                *cursor = created_file->next;
            kfree(created_file);
            created_file = 0;
        } else {
            /*
             * f_close leaves the FIL valid when its sync fails. Keep it
             * registered so a later provider sync can retry and close it.
             */
            created_file->orphaned = 1u;
            fr = close_fr;
        }
    } else {
        kfree(created_file);
        created_file = 0;
    }
    if (fr != FR_OK) {
        fatfs_leave();
        return fatfs_result_to_vfs(fr);
    }
    rc = fatfs_node_create(parent->volume, fatfs_path_scratch + 2u,
                           directory, PROS_VFS_NODE_FILE, 0u, &created);
    if (rc != PROS_VFS_OK) {
        FRESULT rollback_fr = f_unlink(fatfs_path_scratch);
        if (rollback_fr != FR_OK && rollback_fr != FR_NO_FILE)
            rc = fatfs_result_to_vfs(rollback_fr);
    }
    fatfs_leave();
    if (rc == PROS_VFS_OK)
        *result = created;
    return rc;
}

static int fatfs_mkdir(struct pros_vfs_vnode *directory, const char *name,
                       struct pros_vfs_vnode **result,
                       const struct pros_vfs_credentials *auth)
{
    struct fatfs_node_data *parent;
    struct pros_vfs_vnode *created;
    FRESULT fr;
    int rc;
    if (result)
        *result = 0;
    if (!directory || !result || !auth || !directory->private_data)
        return PROS_VFS_EINVAL;
    parent = (struct fatfs_node_data *)directory->private_data;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_join_api_path(parent->volume, parent->path, name,
                             fatfs_path_scratch);
    if (rc != PROS_VFS_OK) {
        fatfs_leave();
        return rc;
    }
    fr = f_mkdir(fatfs_path_scratch);
    if (fr != FR_OK) {
        fatfs_leave();
        return fatfs_result_to_vfs(fr);
    }
    rc = fatfs_node_create(parent->volume, fatfs_path_scratch + 2u,
                           directory, PROS_VFS_NODE_DIRECTORY, 0u, &created);
    if (rc != PROS_VFS_OK) {
        FRESULT rollback_fr = f_unlink(fatfs_path_scratch);
        if (rollback_fr != FR_OK && rollback_fr != FR_NO_FILE)
            rc = fatfs_result_to_vfs(rollback_fr);
    }
    fatfs_leave();
    if (rc == PROS_VFS_OK)
        *result = created;
    return rc;
}

static int fatfs_remove(struct pros_vfs_vnode *directory, const char *name,
                        uint8_t require_directory)
{
    struct fatfs_node_data *parent;
    FILINFO info;
    FRESULT fr;
    uint8_t type_mismatch = 0u;
    uint8_t attribute_read_only = 0u;
    int rc;
    if (!directory || !directory->private_data)
        return PROS_VFS_EINVAL;
    parent = (struct fatfs_node_data *)directory->private_data;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_join_api_path(parent->volume, parent->path, name,
                             fatfs_path_scratch);
    if (rc != PROS_VFS_OK) {
        fatfs_leave();
        return rc;
    }
    fr = f_stat(fatfs_path_scratch, &info);
    if (fr == FR_OK &&
        (((info.fattrib & AM_DIR) != 0u) != (require_directory != 0u)))
        type_mismatch = 1u;
    else if (fr == FR_OK && (info.fattrib & AM_RDO) != 0u)
        attribute_read_only = 1u;
    else if (fr == FR_OK)
        fr = f_unlink(fatfs_path_scratch);
    fatfs_leave();
    if (type_mismatch)
        return require_directory ? PROS_VFS_ENOTDIR : PROS_VFS_EISDIR;
    if (attribute_read_only)
        return PROS_VFS_EPERM;
    if (require_directory && fr == FR_DENIED)
        return PROS_VFS_ENOTEMPTY;
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
    FILINFO old_info;
    FRESULT fr;
    int rc;
    if (!old_directory || !new_directory || !auth ||
        !old_directory->private_data || !new_directory->private_data)
        return PROS_VFS_EINVAL;
    old_parent = (struct fatfs_node_data *)old_directory->private_data;
    new_parent = (struct fatfs_node_data *)new_directory->private_data;
    if (old_parent->volume != new_parent->volume)
        return PROS_VFS_ENOTSUP;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_join_api_path(old_parent->volume, old_parent->path, old_name,
                             fatfs_path_scratch);
    if (rc == PROS_VFS_OK) {
        fr = f_stat(fatfs_path_scratch, &old_info);
        if (fr != FR_OK)
            rc = fatfs_result_to_vfs(fr);
        else if ((old_info.fattrib & AM_RDO) != 0u)
            rc = PROS_VFS_EPERM;
        else if ((old_info.fattrib & AM_DIR) != 0u)
            /* Cached directory vnodes retain absolute paths after rename. */
            rc = PROS_VFS_ENOTSUP;
    }
    if (rc == PROS_VFS_OK)
        rc = fatfs_join_api_path(new_parent->volume, new_parent->path,
                                 new_name, fatfs_path_scratch2);
    if (rc != PROS_VFS_OK) {
        fatfs_leave();
        return rc;
    }
    fr = f_rename(fatfs_path_scratch, fatfs_path_scratch2);
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
    FILINFO info;
    BYTE flags = 0u;
    FRESULT fr;
    int rc;
    if (!file || !file->vnode || !file->private_data)
        return PROS_VFS_EINVAL;
    if ((file->flags & (PROS_VFS_O_READ | PROS_VFS_O_WRITE)) == 0u ||
        (file->flags & ~(PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                         PROS_VFS_O_DIRECTORY | PROS_VFS_O_CREATE |
                         PROS_VFS_O_APPEND)) != 0u)
        return PROS_VFS_EINVAL;
    node = (struct fatfs_node_data *)file->vnode->private_data;
    if (!node || !node->volume)
        return PROS_VFS_ESTALE;
    if (node->volume->read_only && (file->flags & PROS_VFS_O_WRITE) != 0u)
        return PROS_VFS_EPERM;
    if ((file->flags & PROS_VFS_O_READ) != 0u)
        flags |= FA_READ;
    if ((file->flags & PROS_VFS_O_WRITE) != 0u)
        flags |= FA_WRITE;
    opened = (struct fatfs_open_file *)kmalloc(sizeof(*opened));
    if (!opened)
        return PROS_VFS_ENOSPC;
    opened->node = node;
    opened->volume = node->volume;
    opened->orphaned = 0u;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK) {
        kfree(opened);
        return rc;
    }
    rc = fatfs_api_path(node->volume, node->path, fatfs_path_scratch,
                        sizeof(fatfs_path_scratch));
    if (rc == PROS_VFS_OK && (file->flags & PROS_VFS_O_WRITE) != 0u) {
        fr = f_stat(fatfs_path_scratch, &info);
        if (fr != FR_OK)
            rc = fatfs_result_to_vfs(fr);
        else if ((info.fattrib & AM_RDO) != 0u)
            rc = PROS_VFS_EPERM;
    }
    if (rc == PROS_VFS_OK) {
        fr = f_open(&opened->file, fatfs_path_scratch, flags);
        if (fr != FR_OK) {
            rc = fatfs_result_to_vfs(fr);
        } else {
            /* Publish the live FIL under the same gate used by sync/close. */
            opened->next = node->volume->open_files;
            node->volume->open_files = opened;
        }
    }
    fatfs_leave();
    if (rc != PROS_VFS_OK) {
        kfree(opened);
        return rc;
    }
    file->private_data = opened;
    return PROS_VFS_OK;
}

static int fatfs_file_read64(struct pros_vfs_file *file, uint64_t offset,
                             uint32_t length, void *buffer)
{
    struct fatfs_open_file *opened;
    UINT transferred = 0u;
    FSIZE_t current_size;
    FRESULT fr;
    int rc;
    if (!file || !file->vnode || !file->private_data ||
        (length != 0u && !buffer))
        return PROS_VFS_EINVAL;
    if (length > INT_MAX || offset > UINT64_MAX - (uint64_t)length)
        return PROS_VFS_EOVERFLOW;
    if ((file->flags & PROS_VFS_O_READ) == 0u)
        return PROS_VFS_EPERM;
    if (length == 0u)
        return 0;
    opened = (struct fatfs_open_file *)file->private_data;
    if (opened->node && opened->node->volume &&
        opened->node->volume->fs.fs_type != FS_EXFAT &&
        offset > UINT32_MAX)
        return PROS_VFS_EOVERFLOW;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    /*
     * FatFs f_lseek() may extend a file when the handle is writable.
     * A read beyond EOF must remain a no-op, never allocate clusters.
     */
    current_size = f_size(&opened->file);
    file->vnode->size = (uint64_t)current_size;
    if (offset >= (uint64_t)current_size) {
        fatfs_leave();
        return 0;
    }
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
    if (!file || !file->vnode || !file->private_data ||
        (length != 0u && !buffer))
        return PROS_VFS_EINVAL;
    if (length > INT_MAX || offset > UINT64_MAX - (uint64_t)length)
        return PROS_VFS_EOVERFLOW;
    if ((file->flags & PROS_VFS_O_WRITE) == 0u)
        return PROS_VFS_EPERM;
    if (length == 0u)
        return 0;
    opened = (struct fatfs_open_file *)file->private_data;
    if ((file->flags & PROS_VFS_O_APPEND) == 0u &&
        opened->node && opened->node->volume &&
        opened->node->volume->fs.fs_type != FS_EXFAT &&
        offset > (uint64_t)UINT32_MAX - (uint64_t)length)
        return PROS_VFS_EOVERFLOW;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    if ((file->flags & PROS_VFS_O_APPEND) != 0u) {
        FSIZE_t end = f_size(&opened->file);
        if ((uint64_t)end > UINT64_MAX - (uint64_t)length ||
            (opened->node && opened->node->volume &&
             opened->node->volume->fs.fs_type != FS_EXFAT &&
             (uint64_t)end > (uint64_t)UINT32_MAX - (uint64_t)length)) {
            fatfs_leave();
            return PROS_VFS_EOVERFLOW;
        }
        fr = f_lseek(&opened->file, end);
    } else {
        fr = f_lseek(&opened->file, (FSIZE_t)offset);
    }
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
    if (!file || !file->vnode || !file->private_data)
        return PROS_VFS_EINVAL;
    if ((file->flags & PROS_VFS_O_WRITE) == 0u)
        return PROS_VFS_EPERM;
    opened = (struct fatfs_open_file *)file->private_data;
    if (opened->node && opened->node->volume &&
        opened->node->volume->fs.fs_type != FS_EXFAT &&
        size > UINT32_MAX)
        return PROS_VFS_EOVERFLOW;
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
    if (fr == FR_OK) {
        struct fatfs_open_file **cursor =
            &opened->volume->open_files;
        while (*cursor && *cursor != opened)
            cursor = &(*cursor)->next;
        if (*cursor == opened)
            *cursor = opened->next;
        /*
         * A successful f_close invalidates the FIL even if our bookkeeping
         * invariant is damaged. Do not retain an invalid FIL or leave a VFS
         * handle that can never be closed again.
         */
    }
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
            fatfs_volumes[i].open_files = 0;
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
    volume->open_files = 0;
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
        /*
         * Do not recycle the volume slot if a concurrent disk operation
         * prevents unbind. A leaked slot is safer than reusing a live pdrv.
         */
        if (pros_fatfs_disk_unbind(volume->pdrv) == PROS_FATFS_DISK_OK)
            fatfs_volume_release(volume);
        else
            rc = PROS_VFS_EBUSY;
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
    if (volume->mount != mount || !volume->allocated)
        return PROS_VFS_ESTALE;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    /*
     * Flush every live FIL before the device barrier. A create operation whose
     * f_close failed is marked orphaned and is reaped here after sync succeeds.
     */
    {
        struct fatfs_open_file **cursor = &volume->open_files;
        while (*cursor) {
            struct fatfs_open_file *opened = *cursor;
            FRESULT fr = f_sync(&opened->file);
            if (fr == FR_OK && opened->orphaned)
                fr = f_close(&opened->file);
            if (fr != FR_OK) {
                rc = fatfs_result_to_vfs(fr);
                fatfs_leave();
                return rc;
            }
            if (opened->orphaned) {
                *cursor = opened->next;
                kfree(opened);
                continue;
            }
            cursor = &opened->next;
        }
    }
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
    if (volume->mount != mount || !volume->allocated ||
        volume->open_files != 0) {
        fatfs_leave();
        return PROS_VFS_EBUSY;
    }
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
    {
        int register_rc = pros_vfs_provider_register(&fatfs_provider);
        if (register_rc != PROS_VFS_OK) {
            __atomic_store_n(&fatfs_provider_state, 0u, __ATOMIC_RELEASE);
            return register_rc;
        }
    }
    __atomic_store_n(&fatfs_provider_state, 1u, __ATOMIC_RELEASE);
    return PROS_VFS_OK;
}

#define FATFS_MOUNT_TEST_SECTORS 66582u
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
    uint8_t fail_reads;
    uint8_t fail_flush;
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
    if (media->fail_reads)
        return PROS_BLOCK_EIO;
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
        else if (current == 32u || current == 233u || current == 544u)
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
    return media->fail_flush ? PROS_BLOCK_EIO : PROS_BLOCK_OK;
}

static int fatfs_probe_selftest(void)
{
    struct pros_block_device device;

    fatfs_mount_test_init_media();
    if (block_device_init(&device, 512u, FATFS_MOUNT_TEST_SECTORS,
                          fatfs_mount_test_read, fatfs_mount_test_write,
                          &fatfs_mount_test_media) != PROS_BLOCK_OK)
        return -1;

    /* The real FatFs mount path must recognize the synthetic FAT32 fixture. */
    if (fatfs_probe(&device) != PROS_VFS_OK)
        return -2;

    /* Invalid media is rejected by upstream FatFs, not a local BPB parser. */
    fatfs_mount_test_init_media();
    memset(fatfs_mount_test_media.boot, 0,
           sizeof(fatfs_mount_test_media.boot));
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -3;

    /*
     * Present a structurally plausible FAT16 geometry. The provider must
     * reject it even though upstream FatFs can mount/classify FAT16 volumes.
     */
    fatfs_mount_test_init_media();
    fatfs_mount_test_put16(&fatfs_mount_test_media.boot[17], 512u);
    fatfs_mount_test_put16(&fatfs_mount_test_media.boot[22], 201u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[32], 51732u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[36], 0u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[44], 0u);
    fatfs_mount_test_put16(&fatfs_mount_test_media.fat[0], 0xFFF8u);
    fatfs_mount_test_put16(&fatfs_mount_test_media.fat[2], 0xFFFFu);
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -4;

    /* I/O failures remain distinct from an ordinary format mismatch. */
    fatfs_mount_test_init_media();
    fatfs_mount_test_media.fail_reads = 1u;
    if (fatfs_probe(&device) != PROS_VFS_EIO)
        return -5;

    return PROS_VFS_OK;
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
    uint32_t flushes_before_open_sync = 0u;
    int rc;
    int failure_stage = 0;
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
    if (rc != PROS_VFS_OK) {
        /* Preserve the provider/VFS error for the boot log while diagnosing
         * the first create/open path; the call site is already known. */
        failure_stage = -rc;
        goto cleanup;
    }
    file_open = 1u;
    /* A read past EOF on a writable handle must not extend the file. */
    if (vfs_file_seek(&file, 4096u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, 1u, &transferred) != PROS_VFS_OK ||
        transferred != 0u || file.vnode->size != 0u ||
        vfs_file_seek(&file, 0u) != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    /*
     * FatFs clips FAT32 seeks at 4 GiB - 1. The provider must reject a
     * wider offset/size rather than silently redirecting I/O or truncation.
     */
    if (vfs_file_seek(&file, UINT64_C(0x100000000)) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, 1u, &transferred) !=
            PROS_VFS_EOVERFLOW ||
        vfs_file_seek(&file, UINT64_C(0xFFFFFFFF)) != PROS_VFS_OK ||
        vfs_file_write(&file, payload, 1u, &transferred) !=
            PROS_VFS_EOVERFLOW ||
        vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
        vfs_file_truncate(&file, UINT64_C(0x100000000)) !=
            PROS_VFS_EOVERFLOW) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, sizeof(readback), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_seek(&file, sizeof(payload) + 64u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, 1u, &transferred) != PROS_VFS_OK ||
        transferred != 0u ||
        vfs_file_seek(&file, (uint64_t)UINT32_MAX + 1u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, 1u, &transferred) !=
            PROS_VFS_EOVERFLOW ||
        vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, sizeof(readback), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_truncate(&file, 4u) != PROS_VFS_OK ||
        vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, 4u, &transferred) != PROS_VFS_OK ||
        transferred != 4u || memcmp(payload, readback, 4u) != 0) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    /*
     * Sync must flush an open FIL (including its FatFs buffer), then refuse
     * teardown while the VFS handle still pins the root vnode.
     */
    flushes_before_open_sync = fatfs_mount_test_media.flushes;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_EBUSY ||
        fatfs_mount_test_media.flushes <= flushes_before_open_sync) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;

    /* O_APPEND must use the current on-disk EOF, not the caller's seek. */
    rc = vfs_kernel_file_open(&mount, "/FATTEST.TXT",
                              PROS_VFS_O_WRITE | PROS_VFS_O_APPEND, &file);
    if (rc != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
        vfs_file_write(&file, payload + 4u, sizeof(payload) - 4u,
                       &transferred) != PROS_VFS_OK ||
        transferred != sizeof(payload) - 4u ||
        file.vnode->size != sizeof(payload)) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    if (vfs_kernel_rename(&mount, "/FATTEST.TXT", "/RENAMED.TXT") !=
        PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }

    /* Exercise directory creation and the non-empty rmdir error mapping. */
    if (vfs_kernel_mkdir(&mount, "/TESTDIR") != PROS_VFS_OK ||
        vfs_kernel_rename(&mount, "/TESTDIR", "/MOVEDDIR") !=
            PROS_VFS_ENOTSUP) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    rc = vfs_kernel_file_open(&mount, "/TESTDIR/CHILD.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_write(&file, payload, 1u, &transferred) != PROS_VFS_OK ||
        transferred != 1u || vfs_file_close(&file) != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    if (vfs_kernel_rmdir(&mount, "/TESTDIR") != PROS_VFS_ENOTEMPTY ||
        vfs_kernel_unlink(&mount, "/TESTDIR/CHILD.TXT") != PROS_VFS_OK ||
        vfs_kernel_rmdir(&mount, "/TESTDIR") != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }

    rc = vfs_kernel_dir_open(&mount, "/", &dir);
    if (rc != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    dir_open = 1u;
    if (vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
        memcmp(entry.name, "RENAMED.TXT", sizeof("RENAMED.TXT")) != 0 ||
        vfs_dir_read(&dir, &entry) != PROS_VFS_ENOENT) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    if (vfs_dir_close(&dir) != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    dir_open = 0u;

    /*
     * A failed durable flush must abort unmount before VFS teardown. The
     * mount/provider pin must remain live so the caller can retry safely.
     */
    fatfs_mount_test_media.fail_flush = 1u;
    rc = pros_vfs_provider_unmount_device(&mount);
    fatfs_mount_test_media.fail_flush = 0u;
    if (rc != PROS_VFS_EIO || !mount.active || mount.provider != provider) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    mounted = 0u;

    /* Remount read-only and prove that file data persisted. */
    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 1u);
    if (rc != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    mounted = 1u;
    /* A read-only remount must reject write admission before provider I/O. */
    rc = vfs_kernel_file_open(&mount, "/RENAMED.TXT", PROS_VFS_O_WRITE,
                              &file);
    if (rc != PROS_VFS_EPERM) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    rc = vfs_kernel_file_open(&mount, "/RENAMED.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK ||
        transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {

        failure_stage = __LINE__;
        goto cleanup;
    }
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
    return failure_stage ? -failure_stage : (rc < 0 ? rc : PROS_VFS_EIO);
}

int pros_fatfs_provider_selftest(void)
{
    int rc;
    rc = pros_fatfs_provider_init();
    if (rc != PROS_VFS_OK)
        return -1000 + rc;
    if (pros_vfs_provider_find("fat32-exfat") != &fatfs_provider)
        return -1100;
    rc = fatfs_probe_selftest();
    if (rc != PROS_VFS_OK)
        return -2000 + rc;
    rc = fatfs_mount_selftest(&fatfs_provider);
    if (rc != PROS_VFS_OK)
        return -3000 + rc;
    return PROS_VFS_OK;
}

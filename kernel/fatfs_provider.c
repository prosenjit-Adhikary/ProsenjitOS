#include <stdint.h>
#include <limits.h>
#include <string.h>

/* Shared diagnostics are declared before the provider close callback. */
static uint8_t fatfs_mount_test_last_close_stage;
static uint8_t fatfs_mount_test_last_close_result;
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
/* Temporary stage marker for the FAT32 first-create QEMU regression. */
static uint8_t fatfs_mount_test_error_site;
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

/*
 * FatFs FR_INT_ERR means its in-memory/on-disk invariants failed. There is no
 * safe in-kernel exFAT repair routine yet, so quarantine every mounted exFAT
 * volume read-only before returning the corruption error. This preserves
 * readable data and prevents subsequent metadata writes from worsening it.
 */
static void fatfs_quarantine_exfat_volumes(void)
{
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        struct fatfs_volume *volume = &fatfs_volumes[i];
        if (!volume->allocated || volume->fs.fs_type != FS_EXFAT)
            continue;
        volume->read_only = 1u;
        (void)pros_fatfs_disk_set_read_only(volume->pdrv, 1u);
    }
}

static int fatfs_result_to_vfs(FRESULT result)
{
    if (result == FR_INT_ERR)
        fatfs_quarantine_exfat_volumes();
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
    /*
     * FAT has no directory entry for the volume root. Some FatFs builds
     * reject f_stat("0:/") even though the mounted root is valid. The root
     * vnode's metadata is synthetic and was initialized by fatfs_node_create;
     * use that authoritative cached metadata instead of asking FatFs to stat
     * a non-entry. Child nodes continue to use on-disk attributes.
     */
    if (node->path_length == 1u && node->path[0] == '/' &&
        vnode->metadata_valid) {
        metadata->uid = vnode->uid;
        metadata->gid = vnode->gid;
        metadata->mode = vnode->mode;
        metadata->reserved[0] = 0u;
        metadata->reserved[1] = 0u;
        return PROS_VFS_OK;
    }
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
    fatfs_mount_test_error_site = 1u;
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
    fatfs_mount_test_error_site = 2u;
    fr = f_open(&created_file->file, fatfs_path_scratch,
                FA_CREATE_NEW | FA_WRITE);
    if (fr == FR_OK) {
        created_file->next = parent->volume->open_files;
        parent->volume->open_files = created_file;
        fatfs_mount_test_error_site = 3u;
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
        fatfs_mount_test_error_site = 4u;
        fr = f_stat(fatfs_path_scratch, &info);
        if (fr != FR_OK)
            rc = fatfs_result_to_vfs(fr);
        else if ((info.fattrib & AM_RDO) != 0u)
            rc = PROS_VFS_EPERM;
    }
    if (rc == PROS_VFS_OK) {
        fatfs_mount_test_error_site = 5u;
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
    fatfs_mount_test_last_close_stage = 1u;
    fatfs_mount_test_last_close_result = 0xFFu;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    fatfs_mount_test_last_close_stage = 2u;
    fr = f_close(&opened->file);
    fatfs_mount_test_last_close_result = (uint8_t)fr;
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
#define FATFS_MOUNT_TEST_CACHE 256u

struct fatfs_mount_test_sector {
    uint64_t lba;
    uint8_t valid;
    uint8_t data[512];
};

struct fatfs_mount_test_media {
    uint8_t boot[512];
    uint8_t fsinfo[512];
    uint8_t fat[512];
    uint8_t exfat_root[512];
    uint8_t exfat_bitmap[4u * 512u];
    uint8_t exfat_mode;
    struct fatfs_mount_test_sector cache[FATFS_MOUNT_TEST_CACHE];
    uint32_t flushes;
    uint8_t fail_reads;
    uint8_t fail_read_lba_enabled;
    uint64_t fail_read_lba;
    uint8_t fail_writes;
    uint8_t fail_flush;
    uint32_t fail_read_after_sectors;
    uint32_t fail_write_after_sectors;
    uint64_t partial_write_lba;
    uint32_t partial_write_sectors;
    uint8_t fail_write_lba_enabled;
    uint64_t fail_write_lba;
};

static struct fatfs_mount_test_media fatfs_mount_test_media;
/* Diagnostic source for the exFAT close-retry self-test: 1=read, 2=write, 3=flush. */
static uint8_t fatfs_mount_test_last_io_error;

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

static void fatfs_mount_test_put64(uint8_t *p, uint64_t value)
{
    fatfs_mount_test_put32(p, (uint32_t)value);
    fatfs_mount_test_put32(p + 4u, (uint32_t)(value >> 32));
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
    if (media->fail_reads ||
        (media->fail_read_lba_enabled && media->fail_read_lba >= lba &&
         media->fail_read_lba - lba < count)) {
        fatfs_mount_test_last_io_error = 1u;
        return PROS_BLOCK_EIO;
    }
    for (uint32_t n = 0u; n < count; ++n) {
        if (media->fail_read_after_sectors != 0u &&
            n == media->fail_read_after_sectors)
            return PROS_BLOCK_EIO;
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
        else if (media->exfat_mode && current == 64u)
            memcpy(out, media->exfat_root, 512u);
        else if (media->exfat_mode && current >= 68u && current < 72u)
            memcpy(out, media->exfat_bitmap + (current - 68u) * 512u, 512u);
        else if (media->exfat_mode && current == 24u)
            memcpy(out, media->fat, 512u);
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
    if (media->fail_writes) {
        fatfs_mount_test_last_io_error = 2u;
        return PROS_BLOCK_EIO;
    }
    if (media->fail_write_lba_enabled &&
        media->fail_write_lba >= lba &&
        media->fail_write_lba - lba < count) {
        fatfs_mount_test_last_io_error = 2u;
        return PROS_BLOCK_EIO;
    }
    for (uint32_t n = 0u; n < count; ++n) {
        if (media->fail_write_after_sectors != 0u &&
            n == media->fail_write_after_sectors) {
            media->partial_write_lba = lba;
            media->partial_write_sectors = n;
            return PROS_BLOCK_EIO;
        }
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
        if (!slot) {
            /* The bounded test medium must report cache exhaustion distinctly. */
            fatfs_mount_test_last_io_error = 4u;
            return PROS_BLOCK_EIO;
        }
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
    if (media->fail_flush) {
        fatfs_mount_test_last_io_error = 3u;
        return PROS_BLOCK_EIO;
    }
    return PROS_BLOCK_OK;
}

static void fatfs_mount_test_init_exfat_media(void)
{
    fatfs_mount_test_init_media();
    fatfs_mount_test_media.exfat_mode = 1u;

    /* Minimal exFAT v1.0 volume: 5000 four-sector clusters, one FAT. */
    memset(fatfs_mount_test_media.boot, 0,
           sizeof(fatfs_mount_test_media.boot));
    fatfs_mount_test_media.boot[0] = 0xEBu;
    fatfs_mount_test_media.boot[1] = 0x76u;
    fatfs_mount_test_media.boot[2] = 0x90u;
    memcpy(&fatfs_mount_test_media.boot[3], "EXFAT   ", 8u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[72], 20064u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[80], 24u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[84], 40u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[88], 64u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[92], 5000u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[96], 2u);
    fatfs_mount_test_put16(&fatfs_mount_test_media.boot[104], 0x0100u);
    fatfs_mount_test_media.boot[108] = 9u;
    fatfs_mount_test_media.boot[109] = 2u;
    fatfs_mount_test_media.boot[110] = 1u;
    fatfs_mount_test_media.boot[510] = 0x55u;
    fatfs_mount_test_media.boot[511] = 0xAAu;

    /* Root-directory allocation bitmap entry: bitmap starts at cluster 3. */
    fatfs_mount_test_media.exfat_root[0] = 0x81u;
    fatfs_mount_test_media.exfat_root[1] = 0u;
    fatfs_mount_test_put32(&fatfs_mount_test_media.exfat_root[20], 3u);
    fatfs_mount_test_put64(&fatfs_mount_test_media.exfat_root[24], 625u);

    /*
     * The root directory (cluster 2) and allocation bitmap (cluster 3) are
     * both end-of-chain allocations; the first two bitmap bits mark them in
     * use, and the bitmap itself occupies one contiguous cluster.
     */
    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[8], 0xFFFFFFFFu);
    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[12], 0xFFFFFFFFu);
    fatfs_mount_test_media.exfat_bitmap[0] = 0x03u;
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

    /*
     * FAT32 FSInfo is an advisory free-space hint, not authoritative volume
     * geometry. Damaged signatures must make FatFs discard the hint while
     * preserving access to an otherwise valid FAT32 volume.
     */
    fatfs_mount_test_init_media();
    memset(fatfs_mount_test_media.fsinfo, 0,
           sizeof(fatfs_mount_test_media.fsinfo));
    if (fatfs_probe(&device) != PROS_VFS_OK)
        return -19;

    /* Invalid media is rejected by upstream FatFs, not a local BPB parser. */
    fatfs_mount_test_init_media();
    memset(fatfs_mount_test_media.boot, 0,
           sizeof(fatfs_mount_test_media.boot));
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -3;

    /*
     * An exFAT OEM identifier alone must never make a damaged volume mount.
     * Supply plausible geometry but leave the main/backup boot-region
     * extended sectors and checksum sector invalid. FatFs must reject it.
     */
    fatfs_mount_test_init_media();
    memset(fatfs_mount_test_media.boot, 0,
           sizeof(fatfs_mount_test_media.boot));
    fatfs_mount_test_media.boot[0] = 0xEBu;
    fatfs_mount_test_media.boot[1] = 0x76u;
    fatfs_mount_test_media.boot[2] = 0x90u;
    memcpy(&fatfs_mount_test_media.boot[3], "EXFAT   ", 8u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[80], 24u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[84], 512u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[88], 536u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[92], 65000u);
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[96], 2u);
    fatfs_mount_test_put16(&fatfs_mount_test_media.boot[104], 0x0100u);
    fatfs_mount_test_media.boot[108] = 9u;
    fatfs_mount_test_media.boot[109] = 0u;
    fatfs_mount_test_media.boot[110] = 1u;
    fatfs_mount_test_media.boot[112] = 0xFFu;
    fatfs_mount_test_media.boot[510] = 0x55u;
    fatfs_mount_test_media.boot[511] = 0xAAu;
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -6;

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

    /* A structurally valid exFAT v1.0 fixture must be recognized by FatFs. */
    fatfs_mount_test_init_exfat_media();
    if (fatfs_probe(&device) != PROS_VFS_OK)
        return -7;

    /*
     * The FAT region must end before the cluster heap begins. A one-sector
     * overlap would let FAT metadata alias user file data and must not mount.
     */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[84], 41u);
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -18;

    /*
     * The exFAT BPB root-directory cluster must be in the valid cluster
     * range. Cluster zero is reserved and must be rejected during mount.
     */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_put32(&fatfs_mount_test_media.boot[96], 0u);
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -10;

    /*
     * The allocation bitmap is mandatory. Corrupt its root-directory entry
     * type so the volume no longer advertises a valid bitmap entry.
     */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_media.exfat_root[0] = 0x05u;
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -11;

    /*
     * FatFs requires the exFAT allocation bitmap to occupy a contiguous FAT
     * chain. A fragmented chain must be rejected as an unsupported volume.
     */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[12], 5u);
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -8;

    /* Reject an exFAT sector-size shift that would be undefined in C. */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_media.boot[108] = 32u;
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -16;

    /* Reject an exFAT cluster-size shift outside FatFs' supported range. */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_media.boot[109] = 32u;
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -17;

    /* Root-directory cluster must be marked allocated in the bitmap. */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_media.exfat_bitmap[0] = 0x02u;
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -14;

    /* The bitmap file's own cluster must also be marked allocated. */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_media.exfat_bitmap[0] = 0x01u;
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -15;

    /* Reject an allocation bitmap whose first cluster is outside the heap. */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_put32(&fatfs_mount_test_media.exfat_root[20],
                           UINT32_MAX);
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -9;

    /* The bitmap must cover every allocation cluster, not just one byte. */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_put64(&fatfs_mount_test_media.exfat_root[24], 1u);
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -10;

    /* The bitmap's declared length must fit its actual contiguous FAT chain. */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_put64(&fatfs_mount_test_media.exfat_root[24], 2049u);
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -12;

    /* Reject a bitmap length larger than the entire cluster heap. */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_put64(&fatfs_mount_test_media.exfat_root[24],
                           UINT64_MAX);
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -13;

    /* A valid exFAT volume must contain an allocation bitmap entry. */
    fatfs_mount_test_init_exfat_media();
    fatfs_mount_test_media.exfat_root[0] = 0u;
    if (fatfs_probe(&device) != PROS_VFS_PROVIDER_NO_MATCH)
        return -11;

    /* I/O failures remain distinct from an ordinary format mismatch. */
    fatfs_mount_test_init_media();
    fatfs_mount_test_media.fail_reads = 1u;
    if (fatfs_probe(&device) != PROS_VFS_EIO)
        return -5;

    return PROS_VFS_OK;
}

static int fatfs_mount_test_expect_exfat_entry_error(
    struct pros_vfs_mount *mount, uint32_t entry_offset, uint8_t xor_mask)
{
    struct fatfs_volume *volume = 0;
    struct fatfs_mount_test_sector *root_sector = 0;
    FIL probe_file;
    FRESULT fr;
    uint8_t saved_value;
    int rc, result;

    if (!mount || entry_offset >= 512u)
        return PROS_VFS_EINVAL;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume)
        return PROS_VFS_EIO;
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 64u) {
            root_sector = &fatfs_mount_test_media.cache[i];
            break;
        }
    }
    if (!root_sector)
        return PROS_VFS_EIO;

    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    saved_value = root_sector->data[entry_offset];
    root_sector->data[entry_offset] ^= xor_mask;
    if (volume->fs.winsect == 64u)
        volume->fs.win[entry_offset] ^= xor_mask;

    rc = fatfs_api_path(volume, "/EXFAT.TXT", fatfs_path_scratch,
                        sizeof(fatfs_path_scratch));
    if (rc == PROS_VFS_OK) {
        fr = f_open(&probe_file, fatfs_path_scratch, FA_READ);
        if (fr == FR_OK)
            fr = f_close(&probe_file);
        result = fatfs_result_to_vfs(fr);
    } else {
        result = rc;
    }

    root_sector->data[entry_offset] = saved_value;
    if (volume->fs.winsect == 64u)
        volume->fs.win[entry_offset] = saved_value;
    fatfs_leave();
    return result;
}

/*
 * Corrupt the exFAT stream-extension lengths so a file claims more data than
 * its single allocated cluster can hold. Reads must stop at the first cluster
 * whose allocation bit is clear; restoring both length fields must make the
 * original file readable again.
 */
static int fatfs_mount_test_exfat_file_size_chain_mismatch(
    struct pros_vfs_mount *mount)
{
    static uint8_t readback[8192];
    struct fatfs_volume *volume = 0;
    struct fatfs_mount_test_sector *root_sector = 0;
    FIL file;
    UINT bytes_read = 0u;
    FRESULT fr, close_fr;
    uint32_t entry_offset = 0u;
    uint8_t found = 0u;
    uint8_t saved_valid_length[8], saved_data_length[8];
    int rc = PROS_VFS_EIO;

    if (!mount)
        return PROS_VFS_EINVAL;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 64u) {
            root_sector = &fatfs_mount_test_media.cache[i];
            break;
        }
    }
    if (!volume || !root_sector)
        return PROS_VFS_EIO;

    for (entry_offset = 0u; entry_offset + 64u <= 512u;
         entry_offset += 32u) {
        if (root_sector->data[entry_offset] == 0x85u &&
            root_sector->data[entry_offset + 32u] == 0xC0u) {
            found = 1u;
            break;
        }
    }
    if (!found)
        return PROS_VFS_ECORRUPT;

    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    memcpy(saved_valid_length, &root_sector->data[entry_offset + 40u], 8u);
    memcpy(saved_data_length, &root_sector->data[entry_offset + 56u], 8u);
    fatfs_mount_test_put64(&root_sector->data[entry_offset + 40u], 8192u);
    fatfs_mount_test_put64(&root_sector->data[entry_offset + 56u], 8192u);
    if (volume->fs.winsect == 64u) {
        fatfs_mount_test_put64(&volume->fs.win[entry_offset + 40u], 8192u);
        fatfs_mount_test_put64(&volume->fs.win[entry_offset + 56u], 8192u);
    }

    rc = fatfs_api_path(volume, "/EXFAT.TXT", fatfs_path_scratch,
                        sizeof(fatfs_path_scratch));
    if (rc == PROS_VFS_OK) {
        fr = f_open(&file, fatfs_path_scratch, FA_READ);
        if (fr == FR_INT_ERR) {
            rc = PROS_VFS_OK;
        } else if (fr == FR_OK) {
            bytes_read = 0u;
            fr = f_read(&file, readback, sizeof(readback), &bytes_read);
            close_fr = f_close(&file);
            if (fr == FR_INT_ERR && close_fr == FR_OK &&
                bytes_read < sizeof(readback))
                rc = PROS_VFS_OK;
            else
                rc = PROS_VFS_EIO;
        } else {
            rc = fatfs_result_to_vfs(fr);
        }
    }

    /* Restore the directory entry in both the backing fixture and FatFs. */
    memcpy(&root_sector->data[entry_offset + 40u], saved_valid_length, 8u);
    memcpy(&root_sector->data[entry_offset + 56u], saved_data_length, 8u);
    if (volume->fs.winsect == 64u) {
        memcpy(&volume->fs.win[entry_offset + 40u], saved_valid_length, 8u);
        memcpy(&volume->fs.win[entry_offset + 56u], saved_data_length, 8u);
    }

    if (rc == PROS_VFS_OK) {
        fr = f_open(&file, fatfs_path_scratch, FA_READ);
        if (fr == FR_OK) {
            bytes_read = 0u;
            fr = f_read(&file, readback, 1u, &bytes_read);
            close_fr = f_close(&file);
            if (fr != FR_OK || close_fr != FR_OK || bytes_read != 1u ||
                readback[0] != (uint8_t)'E')
                rc = PROS_VFS_EIO;
        } else {
            rc = fatfs_result_to_vfs(fr);
        }
    }
    fatfs_leave();
    return rc;
}

/*
 * Clear the allocation bit for the first file cluster in the synthetic
 * exFAT root directory. A read must fail as corruption even for a contiguous
 * file whose FAT chain is synthesized by FatFs.
 */
/* Change SecondaryCount while keeping the entry-set checksum valid.
 * This isolates the count/bounds validation from the independent checksum
 * validation exercised by fatfs_mount_test_expect_exfat_entry_error(). */
static int fatfs_mount_test_expect_exfat_secondary_count_error(
    struct pros_vfs_mount *mount)
{
    struct fatfs_volume *volume = 0;
    struct fatfs_mount_test_sector *root_sector = 0;
    FIL probe_file;
    uint8_t saved_count, saved_sum_lo, saved_sum_hi;
    uint16_t sum = 0u;
    FRESULT fr;
    int rc, result;

    if (!mount)
        return PROS_VFS_EINVAL;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume)
        return PROS_VFS_EIO;
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 64u) {
            root_sector = &fatfs_mount_test_media.cache[i];
            break;
        }
    }
    if (!root_sector)
        return PROS_VFS_EIO;

    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;

    /* EXFAT.TXT primary entry begins at byte 32 in this fixture. */
    saved_count = root_sector->data[33u];
    saved_sum_lo = root_sector->data[34u];
    saved_sum_hi = root_sector->data[35u];
    root_sector->data[33u] = (uint8_t)(saved_count + 1u);
    if (volume->fs.winsect == 64u)
        volume->fs.win[33u] = root_sector->data[33u];

    /* Recompute checksum over the now-declared four-entry set. */
    for (uint32_t i = 0u; i < 128u; ++i) {
        if (i == 2u) {
            ++i;
        } else {
            uint8_t value = root_sector->data[32u + i];
            sum = (uint16_t)(((sum & 1u) ? 0x8000u : 0u) +
                             (sum >> 1) + value);
        }
    }
    root_sector->data[34u] = (uint8_t)sum;
    root_sector->data[35u] = (uint8_t)(sum >> 8);
    if (volume->fs.winsect == 64u) {
        volume->fs.win[34u] = root_sector->data[34u];
        volume->fs.win[35u] = root_sector->data[35u];
    }

    rc = fatfs_api_path(volume, "/EXFAT.TXT", fatfs_path_scratch,
                        sizeof(fatfs_path_scratch));
    if (rc == PROS_VFS_OK) {
        fr = f_open(&probe_file, fatfs_path_scratch, FA_READ);
        if (fr == FR_OK)
            fr = f_close(&probe_file);
        result = fatfs_result_to_vfs(fr);
    } else {
        result = rc;
    }

    root_sector->data[33u] = saved_count;
    root_sector->data[34u] = saved_sum_lo;
    root_sector->data[35u] = saved_sum_hi;
    if (volume->fs.winsect == 64u) {
        volume->fs.win[33u] = saved_count;
        volume->fs.win[34u] = saved_sum_lo;
        volume->fs.win[35u] = saved_sum_hi;
    }
    fatfs_leave();
    return result;
}


static int fatfs_mount_test_exfat_bitmap_error(struct pros_vfs_mount *mount)
{
    struct fatfs_volume *volume = 0;
    struct fatfs_mount_test_sector *root_sector = 0;
    struct fatfs_mount_test_sector *bitmap_sector = 0;
    FIL file;
    FIL write_file;
    UINT bytes_read = 0u;
    UINT bytes_written = 0u;
    BYTE byte_read = 0u;
    BYTE byte_to_write = 0x5Au;
    BYTE saved_bitmap_byte;
    DWORD first_cluster = 0u;
    uint32_t entry_offset;
    int rc = PROS_VFS_EIO;
    FRESULT fr, close_fr, write_close_fr;

    if (!mount)
        return PROS_VFS_EINVAL;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume)
        return PROS_VFS_EIO;
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 64u)
            root_sector = &fatfs_mount_test_media.cache[i];
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 68u)
            bitmap_sector = &fatfs_mount_test_media.cache[i];
    }
    if (!root_sector || !bitmap_sector)
        return PROS_VFS_EIO;

    /* The fixture has one file here; its stream extension follows the
       primary file entry and stores FirstCluster at byte offset 20. */
    for (entry_offset = 0u; entry_offset + 64u <= 512u;
         entry_offset += 32u) {
        if (root_sector->data[entry_offset] == 0x85u &&
            root_sector->data[entry_offset + 32u] == 0xC0u) {
            first_cluster =
                (DWORD)root_sector->data[entry_offset + 52u] |
                ((DWORD)root_sector->data[entry_offset + 53u] << 8) |
                ((DWORD)root_sector->data[entry_offset + 54u] << 16) |
                ((DWORD)root_sector->data[entry_offset + 55u] << 24);
            break;
        }
    }
    if (first_cluster < 2u || first_cluster >= volume->fs.n_fatent)
        return PROS_VFS_ECORRUPT;
    {
        DWORD bit_index = first_cluster - 2u;
        uint32_t bitmap_byte = bit_index / 8u;
        BYTE bitmap_mask = (BYTE)(1u << (bit_index % 8u));
        if (bitmap_byte >= 512u)
            return PROS_VFS_EOVERFLOW;
        saved_bitmap_byte = bitmap_sector->data[bitmap_byte];
        bitmap_sector->data[bitmap_byte] &= (BYTE)~bitmap_mask;
        if (volume->fs.winsect == 68u)
            volume->fs.win[bitmap_byte] &= (BYTE)~bitmap_mask;

        rc = fatfs_enter();
        if (rc != PROS_VFS_OK) {
            bitmap_sector->data[bitmap_byte] = saved_bitmap_byte;
            if (volume->fs.winsect == 68u)
                volume->fs.win[bitmap_byte] = saved_bitmap_byte;
            return rc;
        }
        rc = fatfs_api_path(volume, "/EXFAT.TXT", fatfs_path_scratch,
                            sizeof(fatfs_path_scratch));
        if (rc == PROS_VFS_OK) {
            fr = f_open(&file, fatfs_path_scratch, FA_READ);
            if (fr == FR_OK) {
                fr = f_read(&file, &byte_read, 1u, &bytes_read);
                close_fr = f_close(&file);
                if (fr == FR_INT_ERR && close_fr == FR_OK && bytes_read == 0u) {
                    rc = PROS_VFS_OK;
                    /*
                     * Writes at offset zero select sclust directly and do
                     * not call get_fat(). A free first-cluster bitmap bit
                     * must reject the write before data reaches the medium.
                     */
                    fr = f_open(&write_file, fatfs_path_scratch, FA_WRITE);
                    if (fr == FR_OK) {
                        fr = f_write(&write_file, &byte_to_write, 1u,
                                     &bytes_written);
                        write_close_fr = f_close(&write_file);
                        if (fr != FR_INT_ERR || bytes_written != 0u ||
                            write_close_fr != FR_OK)
                            rc = PROS_VFS_EIO;
                    } else {
                        rc = fatfs_result_to_vfs(fr);
                    }
                } else {
                    rc = PROS_VFS_EIO;
                }
            } else {
                rc = fatfs_result_to_vfs(fr);
            }
        }
        bitmap_sector->data[bitmap_byte] = saved_bitmap_byte;
        if (volume->fs.winsect == 68u)
            volume->fs.win[bitmap_byte] = saved_bitmap_byte;

        /*
         * Restoring the allocation bit must make the original file readable
         * again; this also proves the rejected write did not damage its data.
         */
        if (rc == PROS_VFS_OK) {
            bytes_read = 0u;
            fr = f_open(&file, fatfs_path_scratch, FA_READ);
            if (fr == FR_OK) {
                fr = f_read(&file, &byte_read, 1u, &bytes_read);
                close_fr = f_close(&file);
                if (fr != FR_OK || close_fr != FR_OK || bytes_read != 1u ||
                    byte_read != (BYTE)'E')
                    rc = PROS_VFS_EIO;
            } else {
                rc = fatfs_result_to_vfs(fr);
            }
        }
        fatfs_leave();
    }
    return rc;
}

/*
 * Exercise allocation bit 4096 in the second bitmap sector of the valid
 * 5000-cluster exFAT fixture using its mounted geometry.
 */
static int fatfs_mount_test_exfat_bitmap_sector_boundary(
    struct pros_vfs_mount *mount)
{
    static BYTE readback[2049];
    struct fatfs_volume *volume = 0;
    struct fatfs_mount_test_sector *bitmap_sector0 = 0;
    struct fatfs_mount_test_sector *bitmap_sector1 = 0;
    FIL file;
    FRESULT fr, close_fr;
    BYTE saved_bitmap_byte0, saved_bitmap_byte1;
    UINT bytes_read = 0u;
    int rc = PROS_VFS_EIO;

    if (!mount)
        return PROS_VFS_EINVAL;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume)
        return PROS_VFS_EIO;

    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_api_path(volume, "/EXFAT.TXT", fatfs_path_scratch,
                        sizeof(fatfs_path_scratch));
    if (rc != PROS_VFS_OK)
        goto done;

    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (!fatfs_mount_test_media.cache[i].valid)
            continue;
        if (fatfs_mount_test_media.cache[i].lba == 68u)
            bitmap_sector0 = &fatfs_mount_test_media.cache[i];
        if (fatfs_mount_test_media.cache[i].lba == 69u)
            bitmap_sector1 = &fatfs_mount_test_media.cache[i];
    }

    /* Cluster 4097 is bit 4095 (last bit of sector 1); cluster 4098
     * is bit 4096 (first bit of sector 2). Both must be allocated for
     * the initial 2049-byte contiguous read to cross the boundary. */
    saved_bitmap_byte0 = fatfs_mount_test_media.exfat_bitmap[511u];
    saved_bitmap_byte1 = fatfs_mount_test_media.exfat_bitmap[512u];
    fatfs_mount_test_media.exfat_bitmap[511u] |= 0x80u;
    fatfs_mount_test_media.exfat_bitmap[512u] |= 0x01u;
    if (bitmap_sector0)
        bitmap_sector0->data[511u] =
            fatfs_mount_test_media.exfat_bitmap[511u];
    if (bitmap_sector1)
        bitmap_sector1->data[0] =
            fatfs_mount_test_media.exfat_bitmap[512u];
    if (volume->fs.winsect == 68u)
        volume->fs.win[511u] = fatfs_mount_test_media.exfat_bitmap[511u];
    if (volume->fs.winsect == 69u)
        volume->fs.win[0] = fatfs_mount_test_media.exfat_bitmap[512u];

    fr = f_open(&file, fatfs_path_scratch, FA_READ);
    if (fr != FR_OK) {
        rc = fatfs_result_to_vfs(fr);
        goto restore;
    }
    file.obj.sclust = 4097u;
    file.obj.objsize = sizeof(readback);
    file.obj.stat = 2u;
    bytes_read = 0u;
    fr = f_read(&file, readback, sizeof(readback), &bytes_read);
    close_fr = f_close(&file);
    if (fr != FR_OK || close_fr != FR_OK ||
        bytes_read != sizeof(readback)) {
        rc = PROS_VFS_EIO;
        goto restore;
    }

    /* Free the destination cluster only. The second-sector bitmap check
     * must stop the read after the first 2048-byte cluster. */
    fatfs_mount_test_media.exfat_bitmap[512u] &= (BYTE)~0x01u;
    if (bitmap_sector1)
        bitmap_sector1->data[0] =
            fatfs_mount_test_media.exfat_bitmap[512u];
    if (volume->fs.winsect == 69u)
        volume->fs.win[0] = fatfs_mount_test_media.exfat_bitmap[512u];

    fr = f_open(&file, fatfs_path_scratch, FA_READ);
    if (fr != FR_OK) {
        rc = fatfs_result_to_vfs(fr);
        goto restore;
    }
    file.obj.sclust = 4097u;
    file.obj.objsize = sizeof(readback);
    file.obj.stat = 2u;
    bytes_read = 0u;
    fr = f_read(&file, readback, sizeof(readback), &bytes_read);
    close_fr = f_close(&file);
    rc = fr == FR_INT_ERR && close_fr == FR_OK && bytes_read == 2048u ?
             PROS_VFS_OK : PROS_VFS_EIO;

restore:
    fatfs_mount_test_media.exfat_bitmap[511u] = saved_bitmap_byte0;
    fatfs_mount_test_media.exfat_bitmap[512u] = saved_bitmap_byte1;
    if (bitmap_sector0)
        bitmap_sector0->data[511u] = saved_bitmap_byte0;
    if (bitmap_sector1)
        bitmap_sector1->data[0] = saved_bitmap_byte1;
    if (volume->fs.winsect == 68u)
        volume->fs.win[511u] = saved_bitmap_byte0;
    if (volume->fs.winsect == 69u)
        volume->fs.win[0] = saved_bitmap_byte1;
done:
    fatfs_leave();
    return rc;
}

/*
 * A corrupt exFAT FAT-chain link must be rejected before it is used to
 * calculate an allocation-bitmap sector. Use a non-contiguous file object
 * with a synthetic two-cluster length and poison its first FAT link.
 */
static int fatfs_mount_test_exfat_invalid_next_cluster(
    struct pros_vfs_mount *mount)
{
    static BYTE readback[4096];
    struct fatfs_volume *volume = 0;
    struct fatfs_mount_test_sector *fat_sector = 0;
    FIL file;
    FRESULT fr, close_fr;
    DWORD first_cluster;
    DWORD saved_fat_value;
    UINT bytes_read = 0u;
    uint32_t fat_offset;
    int rc = PROS_VFS_EIO;

    if (!mount)
        return PROS_VFS_EINVAL;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume)
        return PROS_VFS_EIO;

    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    rc = fatfs_api_path(volume, "/EXFAT.TXT", fatfs_path_scratch,
                        sizeof(fatfs_path_scratch));
    if (rc != PROS_VFS_OK) {
        fatfs_leave();
        return rc;
    }
    fr = f_open(&file, fatfs_path_scratch, FA_READ);
    if (fr != FR_OK) {
        fatfs_leave();
        return fatfs_result_to_vfs(fr);
    }

    first_cluster = file.obj.sclust;
    fat_offset = first_cluster * 4u;
    if (first_cluster < 2u || first_cluster >= volume->fs.n_fatent ||
        fat_offset > 512u - 4u) {
        (void)f_close(&file);
        fatfs_leave();
        return PROS_VFS_ECORRUPT;
    }

    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 24u) {
            fat_sector = &fatfs_mount_test_media.cache[i];
            break;
        }
    }

    saved_fat_value =
        (DWORD)fatfs_mount_test_media.fat[fat_offset] |
        ((DWORD)fatfs_mount_test_media.fat[fat_offset + 1u] << 8) |
        ((DWORD)fatfs_mount_test_media.fat[fat_offset + 2u] << 16) |
        ((DWORD)fatfs_mount_test_media.fat[fat_offset + 3u] << 24);
    if (fat_sector) {
        saved_fat_value =
            (DWORD)fat_sector->data[fat_offset] |
            ((DWORD)fat_sector->data[fat_offset + 1u] << 8) |
            ((DWORD)fat_sector->data[fat_offset + 2u] << 16) |
            ((DWORD)fat_sector->data[fat_offset + 3u] << 24);
    }

    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[fat_offset],
                           0xFFFFFFFEu);
    if (fat_sector)
        fatfs_mount_test_put32(&fat_sector->data[fat_offset], 0xFFFFFFFEu);
    if (volume->fs.winsect == 24u)
        fatfs_mount_test_put32(&volume->fs.win[fat_offset], 0xFFFFFFFEu);

    /*
     * Force the file through the FAT chain (rather than its normal contiguous
     * exFAT stream) and make the next-cluster lookup occur at byte 2048.
     */
    file.obj.stat = 0u;
    file.obj.objsize = sizeof(readback);
    fr = f_read(&file, readback, sizeof(readback), &bytes_read);

    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[fat_offset],
                           saved_fat_value);
    if (fat_sector)
        fatfs_mount_test_put32(&fat_sector->data[fat_offset], saved_fat_value);
    if (volume->fs.winsect == 24u)
        fatfs_mount_test_put32(&volume->fs.win[fat_offset], saved_fat_value);

    close_fr = f_close(&file);
    if (fr == FR_INT_ERR && bytes_read == 2048u && close_fr == FR_OK)
        rc = PROS_VFS_OK;
    fatfs_leave();
    return rc;
}

/*
 * A FAT-chain link may be in range while its target cluster is free in the
 * authoritative exFAT allocation bitmap. The read path must reject that
 * target before issuing data I/O for the second cluster.
 */
static int fatfs_mount_test_exfat_chain_bitmap_mismatch(
    struct pros_vfs_mount *mount)
{
    static BYTE readback[4096];
    struct fatfs_volume *volume = 0;
    struct fatfs_mount_test_sector *fat_sector = 0;
    struct fatfs_mount_test_sector *bitmap_sector = 0;
    FIL file;
    FRESULT fr, close_fr;
    DWORD first_cluster, next_cluster, saved_fat_value;
    UINT bytes_read = 0u;
    uint32_t fat_offset, bitmap_index, bitmap_byte;
    BYTE saved_bitmap_byte, bitmap_mask;
    int rc = PROS_VFS_EIO;

    if (!mount) return PROS_VFS_EINVAL;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume) return PROS_VFS_EIO;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK) return rc;

    rc = fatfs_api_path(volume, "/EXFAT.TXT", fatfs_path_scratch,
                        sizeof(fatfs_path_scratch));
    if (rc != PROS_VFS_OK) goto done;
    fr = f_open(&file, fatfs_path_scratch, FA_READ);
    if (fr != FR_OK) {
        rc = fatfs_result_to_vfs(fr);
        goto done;
    }

    first_cluster = file.obj.sclust;
    fat_offset = first_cluster * 4u;
    if (first_cluster < 2u || first_cluster >= volume->fs.n_fatent ||
        fat_offset > 512u - 4u) {
        (void)f_close(&file);
        rc = PROS_VFS_ECORRUPT;
        goto done;
    }

    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 24u)
            fat_sector = &fatfs_mount_test_media.cache[i];
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 68u)
            bitmap_sector = &fatfs_mount_test_media.cache[i];
    }
    /*
     * Earlier exFAT self-tests may have allocated clusters immediately after
     * EXFAT.TXT. Select a genuinely free target from the authoritative bitmap
     * instead of assuming first_cluster + 1 is still free.
     */
    {
        uint8_t found_free = 0u;
        for (bitmap_index = 0u; bitmap_index < 512u * 8u; ++bitmap_index) {
            next_cluster = bitmap_index + 2u;
            if (next_cluster == first_cluster ||
                next_cluster >= volume->fs.n_fatent)
                continue;
            bitmap_byte = bitmap_index / 8u;
            bitmap_mask = (BYTE)(1u << (bitmap_index % 8u));
            saved_bitmap_byte = bitmap_sector ?
                bitmap_sector->data[bitmap_byte] :
                fatfs_mount_test_media.exfat_bitmap[bitmap_byte];
            if ((saved_bitmap_byte & bitmap_mask) == 0u) {
                found_free = 1u;
                break;
            }
        }
        if (!found_free) {
            (void)f_close(&file);
            rc = PROS_VFS_ENOSPC;
            goto done;
        }
    }

    /*
     * The synthetic block device serves its backing arrays when a sector has
     * not previously been written into the cache. FAT and bitmap cache slots
     * are therefore optional; mutate both representations when present.
     */
    if (fat_sector) {
        saved_fat_value =
            (DWORD)fat_sector->data[fat_offset] |
            ((DWORD)fat_sector->data[fat_offset + 1u] << 8) |
            ((DWORD)fat_sector->data[fat_offset + 2u] << 16) |
            ((DWORD)fat_sector->data[fat_offset + 3u] << 24);
    } else {
        saved_fat_value =
            (DWORD)fatfs_mount_test_media.fat[fat_offset] |
            ((DWORD)fatfs_mount_test_media.fat[fat_offset + 1u] << 8) |
            ((DWORD)fatfs_mount_test_media.fat[fat_offset + 2u] << 16) |
            ((DWORD)fatfs_mount_test_media.fat[fat_offset + 3u] << 24);
    }

    if (fat_sector)
        fatfs_mount_test_put32(&fat_sector->data[fat_offset], next_cluster);
    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[fat_offset],
                           next_cluster);
    if (volume->fs.winsect == 24u)
        fatfs_mount_test_put32(&volume->fs.win[fat_offset], next_cluster);
    if (bitmap_sector)
        bitmap_sector->data[bitmap_byte] &= (BYTE)~bitmap_mask;
    fatfs_mount_test_media.exfat_bitmap[bitmap_byte] &=
        (BYTE)~bitmap_mask;
    if (volume->fs.winsect == 68u)
        volume->fs.win[bitmap_byte] &= (BYTE)~bitmap_mask;

    /* Force traversal through the injected FAT link over two clusters. */
    file.obj.stat = 0u;
    file.obj.objsize = sizeof(readback);
    bytes_read = 0u;
    fr = f_read(&file, readback, sizeof(readback), &bytes_read);

    if (fat_sector)
        fatfs_mount_test_put32(&fat_sector->data[fat_offset], saved_fat_value);
    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[fat_offset],
                           saved_fat_value);
    if (volume->fs.winsect == 24u)
        fatfs_mount_test_put32(&volume->fs.win[fat_offset], saved_fat_value);
    if (bitmap_sector)
        bitmap_sector->data[bitmap_byte] = saved_bitmap_byte;
    fatfs_mount_test_media.exfat_bitmap[bitmap_byte] = saved_bitmap_byte;
    if (volume->fs.winsect == 68u)
        volume->fs.win[bitmap_byte] = saved_bitmap_byte;

    /* The free-destination probe must fail only after the first cluster. */
    if (fr != FR_INT_ERR || bytes_read != 2048u) {
        (void)f_close(&file);
        rc = PROS_VFS_EIO;
        goto done;
    }
    close_fr = f_close(&file);
    if (close_fr != FR_OK) {
        rc = fatfs_result_to_vfs(close_fr);
        goto done;
    }

    /* f_read latches FR_INT_ERR in FIL.err; use a fresh handle for the
     * independent first-cluster bitmap corruption probe. */
    fr = f_open(&file, fatfs_path_scratch, FA_READ);
    if (fr != FR_OK) {
        rc = fatfs_result_to_vfs(fr);
        goto done;
    }

    {
        uint32_t first_bitmap_index = first_cluster - 2u;
        uint32_t first_bitmap_byte = first_bitmap_index / 8u;
        BYTE first_bitmap_mask =
            (BYTE)(1u << (first_bitmap_index % 8u));
        BYTE first_saved_bitmap = bitmap_sector ?
            bitmap_sector->data[first_bitmap_byte] :
            fatfs_mount_test_media.exfat_bitmap[first_bitmap_byte];

        if ((first_saved_bitmap & first_bitmap_mask) == 0u) {
            (void)f_close(&file);
            rc = PROS_VFS_EIO;
            goto done;
        }
        if (bitmap_sector)
            bitmap_sector->data[first_bitmap_byte] &=
                (BYTE)~first_bitmap_mask;
        fatfs_mount_test_media.exfat_bitmap[first_bitmap_byte] &=
            (BYTE)~first_bitmap_mask;
        if (volume->fs.winsect == 68u)
            volume->fs.win[first_bitmap_byte] &= (BYTE)~first_bitmap_mask;

        file.obj.stat = 0u;
        file.obj.objsize = sizeof(readback);
        bytes_read = 0u;
        fr = f_read(&file, readback, sizeof(readback), &bytes_read);

        if (bitmap_sector)
            bitmap_sector->data[first_bitmap_byte] = first_saved_bitmap;
        fatfs_mount_test_media.exfat_bitmap[first_bitmap_byte] =
            first_saved_bitmap;
        if (volume->fs.winsect == 68u)
            volume->fs.win[first_bitmap_byte] = first_saved_bitmap;

        if (fr != FR_INT_ERR || bytes_read != 0u) {
            (void)f_close(&file);
            rc = PROS_VFS_EIO;
            goto done;
        }
    }

    close_fr = f_close(&file);
    rc = fr == FR_INT_ERR && bytes_read == 0u && close_fr == FR_OK ?
             PROS_VFS_OK : PROS_VFS_EIO;
done:
    fatfs_leave();
    return rc;
}

static int fatfs_exfat_mount_selftest(struct pros_vfs_provider *provider)
{
    static const uint8_t payload[] = {'E','X','F','A','T','-','I','O','!'};
    struct pros_block_device device;
    struct pros_partition partition;
    struct pros_partition_device view;
    struct pros_vfs_mount mount = {0};
    struct pros_vfs_file file;
    struct pros_vfs_dir dir;
    struct pros_vfs_dirent entry;
    static const char unicode_dir[] = "/資料";
    static const char unicode_file[] = "/資料/café-猫.txt";
    static const char unicode_name[] = "café-猫.txt";
    static const char unicode_file_renamed[] = "/資料/café-猫-renamed.txt";
    static const char unicode_name_renamed[] = "café-猫-renamed.txt";
    static uint8_t large_payload[8192];
    static uint8_t large_readback[8192];
    uint8_t readback[sizeof(payload)];
    uint32_t transferred = 0u;
    uint32_t flushes_before_close = 0u;
    struct fatfs_volume *volume = 0;
    LBA_t original_winsect;
    BYTE original_wflag;
    int rc;

    fatfs_mount_test_init_exfat_media();
    if (block_device_init(&device, 512u, FATFS_MOUNT_TEST_SECTORS,
                          fatfs_mount_test_read, fatfs_mount_test_write,
                          &fatfs_mount_test_media) != PROS_BLOCK_OK ||
        block_device_set_flush(&device, fatfs_mount_test_flush) !=
            PROS_BLOCK_OK)
        return -1;

    memset(&partition, 0, sizeof(partition));
    partition.first_lba = 0u;
    partition.sector_count = FATFS_MOUNT_TEST_SECTORS;
    partition.mbr_type = 0x07u;
    partition.table_type = PROS_PARTITION_MBR;
    if (partition_device_init(&view, &device, &partition) !=
        PROS_PARTITION_OK)
        return -2;

    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 0u);
    if (rc != PROS_VFS_OK)
        return -3;

    /* Exercise actual exFAT allocation, data I/O, and persisted readback. */
    rc = vfs_kernel_file_open(&mount, "/EXFAT.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -4;
    }
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, sizeof(readback), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -5;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -6;
    }

    /* Reopen after close to verify the provider can read the exFAT file. */
    rc = vfs_kernel_file_open(&mount, "/EXFAT.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -7;
    }
    if (vfs_file_read(&file, readback, sizeof(readback), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_seek(&file, UINT64_C(0x100000000)) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, 1u, &transferred) != PROS_VFS_OK ||
        transferred != 0u) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -8;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -9;
    }

    /*
     * Verify exFAT truncate semantics through the VFS: the new size and
     * prefix survive close/reopen, and reading at EOF returns no bytes.
     */
    rc = vfs_kernel_file_open(&mount, "/EXFAT-TRUNC.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -67;
    }
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        vfs_file_truncate(&file, 4u) != PROS_VFS_OK ||
        vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != 4u ||
        memcmp(payload, readback, 4u) != 0) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-TRUNC.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -68;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -69;
    }
    rc = vfs_kernel_file_open(&mount, "/EXFAT-TRUNC.TXT", PROS_VFS_O_READ,
                              &file);
    if (rc != PROS_VFS_OK) {
        (void)vfs_kernel_unlink(&mount, "/EXFAT-TRUNC.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -70;
    }
    if (vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != 4u ||
        memcmp(payload, readback, 4u) != 0 ||
        vfs_file_read(&file, readback, 1u, &transferred) != PROS_VFS_OK ||
        transferred != 0u) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-TRUNC.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -71;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK ||
        vfs_kernel_unlink(&mount, "/EXFAT-TRUNC.TXT") != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -72;
    }

    /*
     * File rename must update the exFAT directory entry set consistently
     * through the VFS operation. Verify the old name disappears and the
     * new name resolves to the same persisted contents, then restore the
     * canonical name used by the later fault-injection cases.
     */
    rc = vfs_kernel_rename(&mount, "/EXFAT.TXT", "/EXFAT-RENAMED.TXT");
    if (rc != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -47;
    }
    rc = vfs_kernel_file_open(&mount, "/EXFAT.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_ENOENT) {
        if (rc == PROS_VFS_OK)
            (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -48;
    }
    rc = vfs_kernel_file_open(&mount, "/EXFAT-RENAMED.TXT",
                              PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -49;
    }
    if (vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -50;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        /* A failed close is retryable; do not leak the mount's file pin. */
        if (vfs_file_close(&file) != PROS_VFS_OK) {
            (void)pros_vfs_provider_unmount_device(&mount);
            return -51;
        }
    }
    if (vfs_kernel_rename(&mount, "/EXFAT-RENAMED.TXT", "/EXFAT.TXT") !=
        PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -52;
    }

    /* A file cluster marked free in the allocation bitmap is corruption. */
    if (fatfs_mount_test_exfat_bitmap_error(&mount) != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -40;
    }

    /* Bitmap bit 4096 must be read from the second bitmap sector. */
    if (fatfs_mount_test_exfat_bitmap_sector_boundary(&mount) != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -42;
    }

    /* Invalid next-cluster links must not trigger an out-of-range bitmap read. */
    if (fatfs_mount_test_exfat_invalid_next_cluster(&mount) != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -41;
    }

    /* A valid FAT link to a free exFAT bitmap cluster is corruption. */
    if (fatfs_mount_test_exfat_chain_bitmap_mismatch(&mount) != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -43;
    }

    /* A stream length may not exceed the clusters allocated to the file. */
    if (fatfs_mount_test_exfat_file_size_chain_mismatch(&mount) !=
        PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -66;
    }

    /*
     * The primary file entry's SecondaryCount must describe the complete
     * entry set. Increasing it without adding a secondary entry must be
     * rejected as corrupt metadata rather than reading into the next set.
     */
    if (fatfs_mount_test_expect_exfat_secondary_count_error(&mount) !=
        PROS_VFS_ECORRUPT) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -74;
    }

    /*
     * The primary file entry stores a 16-bit checksum at bytes 2..3.
     * Corrupt each byte independently, then verify that FatFs rejects the
     * complete entry set. Also reject a stream-extension type in the wrong
     * secondary-entry position and an unsupported secondary-entry type.
     */
    if (fatfs_mount_test_expect_exfat_entry_error(&mount, 34u, 0x01u) !=
            PROS_VFS_ECORRUPT ||
        fatfs_mount_test_expect_exfat_entry_error(&mount, 35u, 0x80u) !=
            PROS_VFS_ECORRUPT ||
        fatfs_mount_test_expect_exfat_entry_error(&mount, 64u, 0x03u) !=
            PROS_VFS_ECORRUPT) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -39;
    }

    /*
     * The corruption probes above deliberately inject transient faults and
     * restore their fixture bytes before returning. Drop the quarantine only
     * for this isolated self-test mount so later write-failure/retry tests can
     * continue against the repaired synthetic medium. Runtime mounts remain
     * quarantined until they are unmounted.
     */
    {
        struct fatfs_volume *test_volume = 0;
        for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
            if (fatfs_volumes[i].allocated &&
                fatfs_volumes[i].mount == &mount) {
                test_volume = &fatfs_volumes[i];
                break;
            }
        }
        if (!test_volume ||
            pros_fatfs_disk_set_read_only(test_volume->pdrv, 0u) !=
                PROS_FATFS_DISK_OK) {
            (void)pros_vfs_provider_unmount_device(&mount);
            return -73;
        }
        test_volume->read_only = 0u;
    }

    /*
     * Fail the directory-entry sector write after the file data sector has
     * succeeded. Close must return EIO and retain modified state; retry must
     * rewrite the directory entry and perform the durable flush.
     */
    rc = vfs_kernel_file_open(&mount, "/EXFAT-META.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK ||
        vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload)) {
        if (rc == PROS_VFS_OK)
            (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-META.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -35;
    }
    flushes_before_close = fatfs_mount_test_media.flushes;
    fatfs_mount_test_media.fail_write_lba = 64u;
    fatfs_mount_test_media.fail_write_lba_enabled = 1u;
    rc = vfs_file_close(&file);
    fatfs_mount_test_media.fail_write_lba_enabled = 0u;
    if (rc != PROS_VFS_EIO ||
        fatfs_mount_test_media.flushes != flushes_before_close) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-META.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -36;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK ||
        fatfs_mount_test_media.flushes != flushes_before_close + 1u) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -37;
    }
    rc = vfs_kernel_file_open(&mount, "/EXFAT-META.TXT", PROS_VFS_O_READ,
                              &file);
    if (rc != PROS_VFS_OK ||
        vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -38;
    }

    /*
     * Recovery must survive provider teardown, not merely a retry on the
     * original mount. Remount the same medium and verify the committed data.
     */
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK)
        return -44;
    memset(&mount, 0, sizeof(mount));
    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 0u);
    if (rc != PROS_VFS_OK)
        return -45;
    rc = vfs_kernel_file_open(&mount, "/EXFAT-META.TXT", PROS_VFS_O_READ,
                              &file);
    if (rc != PROS_VFS_OK ||
        vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK ||
        vfs_kernel_unlink(&mount, "/EXFAT-META.TXT") != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -46;
    }

    /* A block read failure must propagate through the exFAT VFS provider. */
    rc = vfs_kernel_file_open(&mount, "/EXFAT.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -19;
    }
    fatfs_mount_test_media.fail_reads = 1u;
    rc = vfs_file_read(&file, readback, sizeof(payload), &transferred);
    fatfs_mount_test_media.fail_reads = 0u;
    if (rc != PROS_VFS_EIO) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -20;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -21;
    }
    /* A fresh handle must recover once the medium is responding again. */
    rc = vfs_kernel_file_open(&mount, "/EXFAT.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK ||
        vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -22;
    }

    /*
     * FF_LFN_UNICODE=2 exposes UTF-8 TCHAR paths and FILINFO names. Exercise
     * non-ASCII path conversion through the real VFS/provider boundary, not
     * only the upstream FatFs API.
     */
    /*
     * Fail exFAT's root-directory entry-set write during f_mkdir final sync.
     * The failed operation must remain discoverable for cleanup, after which
     * a fresh create/remove cycle must succeed on the same mounted volume.
     */
    fatfs_mount_test_media.fail_write_lba = 64u; /* exFAT root directory */
    fatfs_mount_test_media.fail_write_lba_enabled = 1u;
    rc = vfs_kernel_mkdir(&mount, "/EXFAT-MKDIRERR");
    fatfs_mount_test_media.fail_write_lba_enabled = 0u;
    if (rc != PROS_VFS_EIO ||
        vfs_kernel_rmdir(&mount, "/EXFAT-MKDIRERR") != PROS_VFS_OK ||
        vfs_kernel_mkdir(&mount, "/EXFAT-MKDIRERR") != PROS_VFS_OK ||
        vfs_kernel_rmdir(&mount, "/EXFAT-MKDIRERR") != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -86;
    }

    if (vfs_kernel_mkdir(&mount, unicode_dir) != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -12;
    }
    rc = vfs_kernel_file_open(&mount, unicode_file,
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -13;
    }
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, unicode_file);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -14;
    }

    rc = vfs_kernel_dir_open(&mount, unicode_dir, &dir);
    if (rc != PROS_VFS_OK) {
        (void)vfs_kernel_unlink(&mount, unicode_file);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -15;
    }
    if (vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
        memcmp(entry.name, unicode_name, sizeof(unicode_name)) != 0 ||
        entry.type != PROS_VFS_NODE_FILE ||
        vfs_dir_read(&dir, &entry) != PROS_VFS_ENOENT ||
        vfs_dir_close(&dir) != PROS_VFS_OK) {
        (void)vfs_dir_close(&dir);
        (void)vfs_kernel_unlink(&mount, unicode_file);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -16;
    }
    rc = vfs_kernel_file_open(&mount, unicode_file, PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        (void)vfs_kernel_unlink(&mount, unicode_file);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -17;
    }
    if (vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, unicode_file);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -18;
    }
    /*
     * Rename a Unicode LFN through the VFS bridge. Check old-path invalidation,
     * new-path readback, and the exact UTF-8 name returned by readdir, then
     * restore the original path for the remainder of the fixture.
     */
    if (vfs_kernel_rename(&mount, unicode_file, unicode_file_renamed) !=
        PROS_VFS_OK) {
        (void)vfs_kernel_unlink(&mount, unicode_file);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -52;
    }
    rc = vfs_kernel_file_open(&mount, unicode_file, PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_ENOENT) {
        if (rc == PROS_VFS_OK)
            (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, unicode_file_renamed);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -53;
    }
    rc = vfs_kernel_file_open(&mount, unicode_file_renamed,
                              PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        (void)vfs_kernel_unlink(&mount, unicode_file_renamed);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -54;
    }
    if (vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, unicode_file_renamed);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -55;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -56;
    }
    rc = vfs_kernel_dir_open(&mount, unicode_dir, &dir);
    if (rc != PROS_VFS_OK) {
        (void)vfs_kernel_unlink(&mount, unicode_file_renamed);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -57;
    }
    if (vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
        memcmp(entry.name, unicode_name_renamed,
               sizeof(unicode_name_renamed)) != 0 ||
        entry.type != PROS_VFS_NODE_FILE ||
        vfs_dir_read(&dir, &entry) != PROS_VFS_ENOENT) {
        (void)vfs_dir_close(&dir);
        (void)vfs_kernel_unlink(&mount, unicode_file_renamed);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -57;
    }
    if (vfs_dir_close(&dir) != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -59;
    }
    if (vfs_kernel_rename(&mount, unicode_file_renamed, unicode_file) !=
        PROS_VFS_OK) {
        (void)vfs_kernel_unlink(&mount, unicode_file_renamed);
        (void)vfs_kernel_rmdir(&mount, unicode_dir);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -58;
    }

    /*
     * An 8-KiB file spans four 2-KiB clusters in this fixture. Verify
     * full-file persistence and reads at offsets 512 and 4096, exercising
     * multi-sector cluster I/O, allocation, and large-offset traversal.
     */
    for (uint32_t i = 0u; i < sizeof(large_payload); ++i)
        large_payload[i] = (uint8_t)((i * 29u) ^ (i >> 2) ^ 0x5Au);
    rc = vfs_kernel_file_open(&mount, "/LARGE.BIN",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -20;
    }
    if (vfs_file_write(&file, large_payload, sizeof(large_payload),
                       &transferred) != PROS_VFS_OK ||
        transferred != sizeof(large_payload) ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/LARGE.BIN");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -21;
    }
    rc = vfs_kernel_file_open(&mount, "/LARGE.BIN", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        (void)vfs_kernel_unlink(&mount, "/LARGE.BIN");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -22;
    }
    /*
     * The exFAT fixture uses four sectors per cluster, so this read reaches
     * the block callback as one four-sector request. Fail after two sectors
     * and verify the unread tail of the caller buffer remains untouched.
     */
    memset(large_readback, 0xA5, sizeof(large_readback));
    fatfs_mount_test_media.fail_read_after_sectors = 2u;
    rc = vfs_file_read(&file, large_readback, sizeof(large_readback),
                       &transferred);
    fatfs_mount_test_media.fail_read_after_sectors = 0u;
    if (rc != PROS_VFS_EIO) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/LARGE.BIN");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -23;
    }
    for (uint32_t i = 0u; i < 1024u; ++i) {
        if (large_readback[i] != large_payload[i]) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/LARGE.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -24;
        }
    }
    for (uint32_t i = 1024u; i < sizeof(large_readback); ++i) {
        if (large_readback[i] != 0xA5u) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/LARGE.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -25;
        }
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_kernel_unlink(&mount, "/LARGE.BIN");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -26;
    }
    rc = vfs_kernel_file_open(&mount, "/LARGE.BIN", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        (void)vfs_kernel_unlink(&mount, "/LARGE.BIN");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -27;
    }
    if (vfs_file_read(&file, large_readback, sizeof(large_readback),
                      &transferred) != PROS_VFS_OK ||
        transferred != sizeof(large_payload) ||
        memcmp(large_payload, large_readback, sizeof(large_payload)) != 0 ||
        vfs_file_seek(&file, 512u) != PROS_VFS_OK ||
        vfs_file_read(&file, large_readback, 512u, &transferred) !=
            PROS_VFS_OK || transferred != 512u ||
        memcmp(large_payload + 512u, large_readback, 512u) != 0 ||
        vfs_file_seek(&file, 4096u) != PROS_VFS_OK ||
        vfs_file_read(&file, large_readback, 512u, &transferred) !=
            PROS_VFS_OK || transferred != 512u ||
        memcmp(large_payload + 4096u, large_readback, 512u) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/LARGE.BIN");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -23;
    }
    if (vfs_kernel_unlink(&mount, "/LARGE.BIN") != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -28;
    }

    /*
     * Extend an existing exFAT file, then fail the first data write in its
     * newly allocated tail cluster. The old allocation bitmap and file bytes
     * must remain unchanged after rollback.
     */
    {
        uint8_t bitmap_before[sizeof(fatfs_mount_test_media.exfat_bitmap)];
        uint8_t bitmap_after[sizeof(fatfs_mount_test_media.exfat_bitmap)];

        rc = vfs_kernel_file_open(&mount, "/EXTEND.BIN",
                                  PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                                  PROS_VFS_O_CREATE, &file);
        if (rc != PROS_VFS_OK ||
            vfs_file_write(&file, large_payload, sizeof(large_payload),
                           &transferred) != PROS_VFS_OK ||
            transferred != sizeof(large_payload) ||
            vfs_file_close(&file) != PROS_VFS_OK) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/EXTEND.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -89;
        }

        uint8_t bitmap_sector_68_before = 0u;
        memcpy(bitmap_before, fatfs_mount_test_media.exfat_bitmap,
               sizeof(bitmap_before));
        for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
            if (fatfs_mount_test_media.cache[i].valid &&
                fatfs_mount_test_media.cache[i].lba >= 68u &&
                fatfs_mount_test_media.cache[i].lba < 72u) {
                size_t offset = (size_t)(fatfs_mount_test_media.cache[i].lba -
                                         68u) * 512u;
                memcpy(bitmap_before + offset,
                       fatfs_mount_test_media.cache[i].data, 512u);
                if (fatfs_mount_test_media.cache[i].lba == 68u)
                    bitmap_sector_68_before = 1u;
            }
        }
        /* Cluster 4 is in bitmap sector 68; require its persisted baseline. */
        if (!bitmap_sector_68_before) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/EXTEND.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -95;
        }

        rc = vfs_kernel_file_open(&mount, "/EXTEND.BIN",
                                  PROS_VFS_O_READ | PROS_VFS_O_WRITE, &file);
        if (rc != PROS_VFS_OK ||
            vfs_file_seek(&file, sizeof(large_payload)) != PROS_VFS_OK) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/EXTEND.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -90;
        }
        fatfs_mount_test_media.fail_write_after_sectors = 2u;
        rc = vfs_file_write(&file, large_payload, sizeof(large_payload),
                            &transferred);
        fatfs_mount_test_media.fail_write_after_sectors = 0u;
        if (rc != PROS_VFS_EIO || transferred != 0u) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/EXTEND.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -91;
        }

        uint8_t bitmap_sector_68_after = 0u;
        memcpy(bitmap_after, fatfs_mount_test_media.exfat_bitmap,
               sizeof(bitmap_after));
        for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
            if (fatfs_mount_test_media.cache[i].valid &&
                fatfs_mount_test_media.cache[i].lba >= 68u &&
                fatfs_mount_test_media.cache[i].lba < 72u) {
                size_t offset = (size_t)(fatfs_mount_test_media.cache[i].lba -
                                         68u) * 512u;
                memcpy(bitmap_after + offset,
                       fatfs_mount_test_media.cache[i].data, 512u);
                if (fatfs_mount_test_media.cache[i].lba == 68u)
                    bitmap_sector_68_after = 1u;
            }
        }
        if (!bitmap_sector_68_after) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/EXTEND.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -96;
        }
        if (memcmp(bitmap_before, bitmap_after, sizeof(bitmap_before)) != 0) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/EXTEND.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -92;
        }
        if (vfs_file_close(&file) != PROS_VFS_OK) {
            (void)vfs_kernel_unlink(&mount, "/EXTEND.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -93;
        }
        rc = vfs_kernel_file_open(&mount, "/EXTEND.BIN", PROS_VFS_O_READ,
                                  &file);
        if (rc != PROS_VFS_OK ||
            vfs_file_read(&file, large_readback, sizeof(large_readback),
                          &transferred) != PROS_VFS_OK ||
            transferred != sizeof(large_payload) ||
            memcmp(large_payload, large_readback, sizeof(large_payload)) != 0 ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_kernel_unlink(&mount, "/EXTEND.BIN") != PROS_VFS_OK) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/EXTEND.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -94;
        }
    }

    /*
     * Fail a four-sector exFAT data write after two sectors have reached the
     * synthetic medium. The VFS must report EIO; after clearing the fault,
     * close and unlink must still complete without leaking an open handle.
     */
    rc = vfs_kernel_file_open(&mount, "/PARTIAL.BIN",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -29;
    }
    fatfs_mount_test_media.fail_write_after_sectors = 2u;
    rc = vfs_file_write(&file, large_payload, sizeof(large_payload),
                        &transferred);
    fatfs_mount_test_media.fail_write_after_sectors = 0u;
    if (rc != PROS_VFS_EIO || transferred != 0u) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/PARTIAL.BIN");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -30;
    }
    /*
     * The write callback records the actual request LBA when it accepts two
     * sectors and rejects the third. Do not assume a particular cluster is
     * allocated: earlier create/delete operations may advance last_clst.
     * Confirm the two accepted sectors contain the caller's data.
     */
    {
        uint8_t partial_sector_mask = 0u;
        uint64_t partial_lba = fatfs_mount_test_media.partial_write_lba;
        if (fatfs_mount_test_media.partial_write_sectors != 2u) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/PARTIAL.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -87;
        }
        for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
            if (!fatfs_mount_test_media.cache[i].valid)
                continue;
            if (fatfs_mount_test_media.cache[i].lba == partial_lba &&
                memcmp(fatfs_mount_test_media.cache[i].data,
                       large_payload, 512u) == 0)
                partial_sector_mask |= 1u;
            if (fatfs_mount_test_media.cache[i].lba == partial_lba + 1u &&
                memcmp(fatfs_mount_test_media.cache[i].data,
                       large_payload + 512u, 512u) == 0)
                partial_sector_mask |= 2u;
        }
        if (partial_sector_mask != 3u) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/PARTIAL.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -87;
        }
    }
    /*
     * Rollback must clear the bitmap bit for the cluster containing the
     * partial data write. Earlier create/delete operations can move the
     * allocator, so derive the cluster and bitmap sector from the callback's
     * recorded LBA instead of assuming cluster 4 / bitmap sector 68.
     */
    {
        uint64_t partial_lba = fatfs_mount_test_media.partial_write_lba;
        uint64_t cluster_number;
        uint32_t bitmap_byte_index;
        uint32_t bitmap_sector_lba;
        uint32_t bitmap_sector_offset;
        uint8_t bitmap_byte;
        if (partial_lba < 72u || ((partial_lba - 72u) % 4u) != 0u) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/PARTIAL.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -88;
        }
        cluster_number = 4u + ((partial_lba - 72u) / 4u);
        if (cluster_number < 2u || cluster_number >= 5002u) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/PARTIAL.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -88;
        }
        bitmap_byte_index = (uint32_t)((cluster_number - 2u) / 8u);
        bitmap_sector_lba = 68u + bitmap_byte_index / 512u;
        bitmap_sector_offset = bitmap_byte_index % 512u;
        bitmap_byte = fatfs_mount_test_media.exfat_bitmap[bitmap_byte_index];
        for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
            if (fatfs_mount_test_media.cache[i].valid &&
                fatfs_mount_test_media.cache[i].lba == bitmap_sector_lba) {
                bitmap_byte =
                    fatfs_mount_test_media.cache[i].data[bitmap_sector_offset];
                break;
            }
        }
        if ((bitmap_byte & (uint8_t)(1u << ((cluster_number - 2u) % 8u))) != 0u) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/PARTIAL.BIN");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -88;
        }
    }
    if (vfs_file_close(&file) != PROS_VFS_OK ||
        vfs_kernel_unlink(&mount, "/PARTIAL.BIN") != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -31;
    }

    /* A buffered exFAT write failure must be visible at the close boundary. */
    rc = vfs_kernel_file_open(&mount, "/EXFAT-FAULT.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -26;
    }
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload)) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-FAULT.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -27;
    }
    fatfs_mount_test_media.fail_writes = 1u;
    rc = vfs_file_close(&file);
    fatfs_mount_test_media.fail_writes = 0u;
    if (rc != PROS_VFS_EIO) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-FAULT.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -28;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK ||
        vfs_kernel_unlink(&mount, "/EXFAT-FAULT.TXT") != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -29;
    }

    /*
     * A failed durable flush at file-close must retain the open handle and
     * dirty metadata so close can be retried after the device recovers.
     */
    rc = vfs_kernel_file_open(&mount, "/EXFAT-FLUSH.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK ||
        vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload)) {
        if (rc == PROS_VFS_OK)
            (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-FLUSH.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -32;
    }
    flushes_before_close = fatfs_mount_test_media.flushes;
    fatfs_mount_test_media.fail_flush = 1u;
    rc = vfs_file_close(&file);
    fatfs_mount_test_media.fail_flush = 0u;
    if (rc != PROS_VFS_EIO ||
        fatfs_mount_test_media.flushes != flushes_before_close + 1u) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-FLUSH.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -33;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK ||
        fatfs_mount_test_media.flushes != flushes_before_close + 2u ||
        vfs_kernel_unlink(&mount, "/EXFAT-FLUSH.TXT") != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -34;
    }

    /*
     * Force the exFAT PercentInUse field to change. A failed VBR write must
     * be surfaced and retried; otherwise fsi_flag is lost and the BPB stays
     * stale even though file data and directory metadata were committed.
     */
    fatfs_mount_test_media.boot[112] = 100u;
    /* Earlier exFAT syncs may already have cached the VBR sector. */
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 0u)
            fatfs_mount_test_media.cache[i].data[112] = 100u;
    }
    rc = vfs_kernel_file_open(&mount, "/EXFAT-BPBERR.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK ||
        vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload)) {
        if (rc == PROS_VFS_OK)
            (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -80;
    }
    /*
     * Force the next VBR access through disk_read. FatFs has its own
     * window cache in addition to the synthetic block-media cache; merely
     * changing the backing cache does not exercise the injected read error.
     */
    {
        for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
            if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == &mount) {
                volume = &fatfs_volumes[i];
                break;
            }
        }
        if (!volume) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -811;
        }
        /*
         * sync_fs() flushes the shared window, then reads the exFAT VBR
         * directly from volbase. Keep the real window identity and dirty
         * flag intact: invalidating winsect here while wflag is set makes
         * sync_window() write to 0xFFFFFFFF before the VBR read is reached.
         */
        original_winsect = volume->fs.winsect;
        original_wflag = volume->fs.wflag;
        if (original_winsect >= FATFS_MOUNT_TEST_SECTORS) {
            (void)vfs_file_close(&file);
            (void)vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT");
            (void)pros_vfs_provider_unmount_device(&mount);
            return -812;
        }
    }
    if (volume->fs.winsect != original_winsect ||
        volume->fs.wflag != original_wflag) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -813;
    }
    flushes_before_close = fatfs_mount_test_media.flushes;
    fatfs_mount_test_last_io_error = 0u;
    fatfs_mount_test_last_close_stage = 0u;
    fatfs_mount_test_last_close_result = 0xFFu;
    fatfs_mount_test_media.fail_read_lba = 0u; /* exFAT VBR */
    fatfs_mount_test_media.fail_read_lba_enabled = 1u;
    rc = vfs_file_close(&file);
    fatfs_mount_test_media.fail_read_lba_enabled = 0u;
    /*
     * Keep each assertion independently diagnosable. The provider self-test
     * wrapper adds -2500 to this return code, so these values remain distinct
     * in the QEMU boot log. On a close error, stage 0 means VFS rejected the
     * handle before reaching the provider, stage 1 means provider gate busy,
     * and stage 2 means FatFs f_close ran (result is the raw FRESULT).
     */
    if (rc != PROS_VFS_EIO) {
        int diagnostic = fatfs_mount_test_last_close_stage == 0u ? -8150 :
                         fatfs_mount_test_last_close_stage == 1u ? -8151 :
                         -8160 - (int)fatfs_mount_test_last_close_result;
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return diagnostic;
    }
    if (fatfs_mount_test_media.flushes != flushes_before_close) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -8142;
    }
    if (fatfs_mount_test_last_io_error != 1u) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -8143;
    }
    if (volume->fs.wflag != 0u) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -8144;
    }
    if (volume->fs.winsect != volume->fs.volbase) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -8145;
    }
    /*
     * A failed media read must not be mistaken for an internal FatFs
     * invariant failure: FR_INT_ERR quarantines the volume read-only.
     */
    {
        struct fatfs_volume *volume = 0;
        for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
            if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == &mount) {
                volume = &fatfs_volumes[i];
                break;
            }
        }
        if (!volume || volume->read_only ||
            fatfs_mount_test_last_close_result != FR_DISK_ERR) {
            (void)pros_vfs_provider_unmount_device(&mount);
            return -870 - (int)fatfs_mount_test_last_close_result -
                   (volume && volume->read_only ? 20 : 0);
        }
    }
    /* A VBR write failure must also preserve the pending metadata retry. */
    fatfs_mount_test_last_io_error = 0u;
    fatfs_mount_test_media.fail_write_lba = 0u;
    fatfs_mount_test_media.fail_write_lba_enabled = 1u;
    rc = vfs_file_close(&file);
    fatfs_mount_test_media.fail_write_lba_enabled = 0u;
    if (rc != PROS_VFS_EIO ||
        fatfs_mount_test_media.flushes != flushes_before_close ||
        fatfs_mount_test_last_io_error != 2u) {
        (void)vfs_file_close(&file);
        (void)vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT");
        (void)pros_vfs_provider_unmount_device(&mount);
        return -82;
    }
    {
        struct fatfs_volume *volume = 0;
        for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
            if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == &mount) {
                volume = &fatfs_volumes[i];
                break;
            }
        }
        if (!volume || volume->read_only ||
            fatfs_mount_test_last_close_result != FR_DISK_ERR) {
            (void)pros_vfs_provider_unmount_device(&mount);
            return -880 - (int)fatfs_mount_test_last_close_result -
                   (volume && volume->read_only ? 20 : 0);
        }
    }
    fatfs_mount_test_last_io_error = 0u;
    fatfs_mount_test_last_close_stage = 0u;
    fatfs_mount_test_last_close_result = 0xFFu;
    pros_fatfs_disk_test_reset_status();
    rc = vfs_file_close(&file);
    if (rc != PROS_VFS_OK) {
        uint8_t io_error = fatfs_mount_test_last_io_error;
        uint8_t close_stage = fatfs_mount_test_last_close_stage;
        uint8_t close_result = fatfs_mount_test_last_close_result;
        uint32_t disk_status = pros_fatfs_disk_test_last_status();
        (void)pros_vfs_provider_unmount_device(&mount);
        /* Preserve raw adapter status; additive fields can collide. */
        return -100000000 - (int)disk_status -
               ((int)close_stage * 10000) -
               ((int)close_result * 100) - (int)io_error;
    }
    if (fatfs_mount_test_media.flushes != flushes_before_close + 1u) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -831;
    }
    {
        struct fatfs_mount_test_sector *vbr_sector = 0;
        for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
            if (fatfs_mount_test_media.cache[i].valid &&
                fatfs_mount_test_media.cache[i].lba == 0u) {
                vbr_sector = &fatfs_mount_test_media.cache[i];
                break;
            }
        }
        if (!vbr_sector || vbr_sector->data[112] != 0xFFu) {
            (void)pros_vfs_provider_unmount_device(&mount);
            return -84;
        }
    }
    rc = vfs_kernel_file_open(&mount, "/EXFAT-BPBERR.TXT", PROS_VFS_O_READ,
                              &file);
    if (rc != PROS_VFS_OK ||
        vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK ||
        vfs_kernel_unlink(&mount, "/EXFAT-BPBERR.TXT") != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -85;
    }

    if (vfs_kernel_unlink(&mount, unicode_file) != PROS_VFS_OK ||
        vfs_kernel_rmdir(&mount, unicode_dir) != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -25;
    }

    fatfs_mount_test_media.fail_flush = 1u;
    rc = pros_vfs_provider_unmount_device(&mount);
    fatfs_mount_test_media.fail_flush = 0u;
    if (rc != PROS_VFS_EIO || !mount.active || mount.provider != provider) {
        if (mount.active)
            (void)pros_vfs_provider_unmount_device(&mount);
        return -30;
    }
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK)
        return -31;

    /*
     * Remount the same exFAT medium read-only. Write admission must be denied
     * before FatFs mutation, while the previously committed file remains
     * readable after the provider teardown/remount boundary.
     */
    memset(&mount, 0, sizeof(mount));
    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 1u);
    if (rc != PROS_VFS_OK)
        return -60;
    rc = vfs_kernel_file_open(&mount, "/EXFAT.TXT", PROS_VFS_O_WRITE, &file);
    if (rc != PROS_VFS_EPERM) {
        if (rc == PROS_VFS_OK)
            (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -61;
    }
    rc = vfs_kernel_file_open(&mount, "/EXFAT.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        (void)pros_vfs_provider_unmount_device(&mount);
        return -62;
    }
    if (vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -63;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        (void)pros_vfs_provider_unmount_device(&mount);
        return -64;
    }
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK)
        return -65;
    return PROS_VFS_OK;
}

static int fatfs_mount_test_expect_fat32_bad_chain(
    struct pros_vfs_mount *mount, const uint8_t *expected, uint32_t length)
{
    static const uint8_t short_name[11] = {
        'B','A','D','C','H','A','I','N','T','X','T'
    };
    struct fatfs_volume *volume = 0;
    struct fatfs_mount_test_sector *root_sector = 0;
    struct pros_vfs_file file;
    uint8_t readback[32];
    uint8_t saved_cluster[4];
    uint32_t entry_offset = 0u, transferred = 0u;
    uint8_t found = 0u, file_open = 0u;
    int rc;

    if (!mount || !expected || length == 0u || length > sizeof(readback))
        return PROS_VFS_EINVAL;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume)
        return PROS_VFS_EIO;
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 1056u) {
            root_sector = &fatfs_mount_test_media.cache[i];
            break;
        }
    }
    if (!root_sector)
        return PROS_VFS_EIO;
    for (uint32_t offset = 0u; offset + 32u <= 512u; offset += 32u) {
        if (memcmp(root_sector->data + offset, short_name,
                   sizeof(short_name)) == 0) {
            entry_offset = offset;
            found = 1u;
            break;
        }
    }
    if (!found)
        return PROS_VFS_EIO;

    saved_cluster[0] = root_sector->data[entry_offset + 20u];
    saved_cluster[1] = root_sector->data[entry_offset + 21u];
    saved_cluster[2] = root_sector->data[entry_offset + 26u];
    saved_cluster[3] = root_sector->data[entry_offset + 27u];

    /*
     * A non-empty FAT32 file cannot have cluster zero. Check this separately
     * from the out-of-range cluster case below, and restore the fixture before
     * continuing so the second corruption case remains independent.
     */
    root_sector->data[entry_offset + 20u] = 0u;
    root_sector->data[entry_offset + 21u] = 0u;
    root_sector->data[entry_offset + 26u] = 0u;
    root_sector->data[entry_offset + 27u] = 0u;
    if (volume->fs.winsect == 1056u) {
        memset(volume->fs.win + entry_offset + 20u, 0, 2u);
        memset(volume->fs.win + entry_offset + 26u, 0, 2u);
    }
    rc = vfs_kernel_file_open(mount, "/BADCHAIN.TXT", PROS_VFS_O_READ,
                              &file);
    if (rc == PROS_VFS_OK) {
        file_open = 1u;
        transferred = 0u;
        rc = vfs_file_read(&file, readback, 1u, &transferred);
    }
    memcpy(root_sector->data + entry_offset + 20u, saved_cluster, 2u);
    memcpy(root_sector->data + entry_offset + 26u, saved_cluster + 2u, 2u);
    if (volume->fs.winsect == 1056u) {
        memcpy(volume->fs.win + entry_offset + 20u, saved_cluster, 2u);
        memcpy(volume->fs.win + entry_offset + 26u, saved_cluster + 2u, 2u);
    }
    if (file_open && vfs_file_close(&file) != PROS_VFS_OK)
        return PROS_VFS_EIO;
    file_open = 0u;
    if (rc != PROS_VFS_ECORRUPT || transferred != 0u)
        return PROS_VFS_EIO;

    root_sector->data[entry_offset + 20u] = 0xFFu;
    root_sector->data[entry_offset + 21u] = 0x0Fu;
    root_sector->data[entry_offset + 26u] = 0xF7u;
    root_sector->data[entry_offset + 27u] = 0xFFu;
    if (volume->fs.winsect == 1056u) {
        volume->fs.win[entry_offset + 20u] = 0xFFu;
        volume->fs.win[entry_offset + 21u] = 0x0Fu;
        volume->fs.win[entry_offset + 26u] = 0xF7u;
        volume->fs.win[entry_offset + 27u] = 0xFFu;
    }

    rc = vfs_kernel_file_open(mount, "/BADCHAIN.TXT", PROS_VFS_O_READ,
                              &file);
    if (rc == PROS_VFS_OK) {
        file_open = 1u;
        rc = vfs_file_read(&file, readback, 1u, &transferred);
    }

    root_sector->data[entry_offset + 20u] = saved_cluster[0];
    root_sector->data[entry_offset + 21u] = saved_cluster[1];
    root_sector->data[entry_offset + 26u] = saved_cluster[2];
    root_sector->data[entry_offset + 27u] = saved_cluster[3];
    if (volume->fs.winsect == 1056u) {
        volume->fs.win[entry_offset + 20u] = saved_cluster[0];
        volume->fs.win[entry_offset + 21u] = saved_cluster[1];
        volume->fs.win[entry_offset + 26u] = saved_cluster[2];
        volume->fs.win[entry_offset + 27u] = saved_cluster[3];
    }
    if (file_open && vfs_file_close(&file) != PROS_VFS_OK)
        return PROS_VFS_EIO;
    if (rc != PROS_VFS_ECORRUPT)
        return PROS_VFS_EIO;

    rc = vfs_kernel_file_open(mount, "/BADCHAIN.TXT", PROS_VFS_O_READ,
                              &file);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_file_read(&file, readback, length, &transferred);
    if (rc == PROS_VFS_OK &&
        (transferred != length || memcmp(expected, readback, length) != 0))
        rc = PROS_VFS_EIO;
    if (vfs_file_close(&file) != PROS_VFS_OK)
        return PROS_VFS_EIO;
    return rc;
}

static int fatfs_mount_test_expect_fat32_bad_link(
    struct pros_vfs_mount *mount, const uint8_t *expected, uint32_t length)
{
    static const uint8_t short_name[11] = {
        'C','H','A','I','N','B','A','D','B','I','N'
    };
    struct fatfs_volume *volume = 0;
    struct fatfs_mount_test_sector *root_sector = 0;
    struct fatfs_mount_test_sector *fat_sector = 0;
    struct pros_vfs_file file;
    static uint8_t readback[1024];
    uint8_t saved_fat[4];
    uint32_t entry_offset = 0u, start_cluster, fat_offset;
    uint32_t transferred = 0u;
    uint8_t found = 0u, file_open = 0u;
    int rc;

    if (!mount || !expected || length != sizeof(readback))
        return PROS_VFS_EINVAL;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume)
        return PROS_VFS_EIO;
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (!fatfs_mount_test_media.cache[i].valid)
            continue;
        if (fatfs_mount_test_media.cache[i].lba == 1056u)
            root_sector = &fatfs_mount_test_media.cache[i];
        else if (fatfs_mount_test_media.cache[i].lba == 32u)
            fat_sector = &fatfs_mount_test_media.cache[i];
    }
    if (!root_sector || !fat_sector)
        return PROS_VFS_EIO;
    for (uint32_t offset = 0u; offset + 32u <= 512u; offset += 32u) {
        if (memcmp(root_sector->data + offset, short_name,
                   sizeof(short_name)) == 0) {
            entry_offset = offset;
            found = 1u;
            break;
        }
    }
    if (!found)
        return PROS_VFS_EIO;

    start_cluster =
        ((((uint32_t)root_sector->data[entry_offset + 21u] << 8) |
          root_sector->data[entry_offset + 20u]) & 0x0FFFu) << 16;
    start_cluster |= (uint32_t)root_sector->data[entry_offset + 26u] |
                     ((uint32_t)root_sector->data[entry_offset + 27u] << 8);
    if (start_cluster < 2u || start_cluster >= 128u)
        return PROS_VFS_EIO;
    fat_offset = start_cluster * 4u;
    memcpy(saved_fat, fat_sector->data + fat_offset, sizeof(saved_fat));
    fatfs_mount_test_put32(fat_sector->data + fat_offset, 0x0FFFFFF7u);
    if (volume->fs.winsect == 32u)
        fatfs_mount_test_put32(volume->fs.win + fat_offset, 0x0FFFFFF7u);

    memset(readback, 0xA5, sizeof(readback));
    rc = vfs_kernel_file_open(mount, "/CHAINBAD.BIN", PROS_VFS_O_READ,
                              &file);
    if (rc == PROS_VFS_OK) {
        file_open = 1u;
        rc = vfs_file_read(&file, readback, length, &transferred);
    }
    if (file_open) {
        if (vfs_file_close(&file) != PROS_VFS_OK) {
            memcpy(fat_sector->data + fat_offset, saved_fat,
                   sizeof(saved_fat));
            if (volume->fs.winsect == 32u)
                memcpy(volume->fs.win + fat_offset, saved_fat,
                       sizeof(saved_fat));
            return PROS_VFS_EIO;
        }
        file_open = 0u;
    }
    if (rc != PROS_VFS_ECORRUPT || transferred != 0u) {
        memcpy(fat_sector->data + fat_offset, saved_fat, sizeof(saved_fat));
        if (volume->fs.winsect == 32u)
            memcpy(volume->fs.win + fat_offset, saved_fat, sizeof(saved_fat));
        return PROS_VFS_EIO;
    }

    /*
     * VFS clears bytes_read when a provider reports an error, even if the
     * provider filled the first cluster before detecting the corrupt link.
     * The buffer contents below verify that partial data; transferred stays
     * zero by the VFS error contract.
     *
     * Restore the FAT entry after the read-path corruption check. A failed
     * FatFs write marks its FIL as erroneous; f_close() then deliberately
     * retains the handle when synchronization fails. Write-chain fault
     * injection therefore belongs in a dedicated recovery/remount test,
     * not in this read-only handle regression.
     */
    memcpy(fat_sector->data + fat_offset, saved_fat, sizeof(saved_fat));
    if (volume->fs.winsect == 32u)
        memcpy(volume->fs.win + fat_offset, saved_fat, sizeof(saved_fat));
    for (uint32_t i = 0u; i < 512u; ++i) {
        if (readback[i] != expected[i])
            return PROS_VFS_EIO;
    }
    for (uint32_t i = 512u; i < sizeof(readback); ++i) {
        if (readback[i] != 0xA5u)
            return PROS_VFS_EIO;
    }

    rc = vfs_kernel_file_open(mount, "/CHAINBAD.BIN", PROS_VFS_O_READ,
                              &file);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_file_read(&file, readback, length, &transferred);
    if (rc == PROS_VFS_OK &&
        (transferred != length || memcmp(expected, readback, length) != 0))
        rc = PROS_VFS_EIO;
    if (vfs_file_close(&file) != PROS_VFS_OK)
        return PROS_VFS_EIO;
    return rc;
}


/*
 * Fail the second FAT32 mirror write after the primary FAT sector succeeds.
 * FatFs must report the failed mirror write and retain its dirty window; the
 * next sync, after the fault is cleared, must retry and synchronize both
 * FAT copies before proceeding.
 */
static int fatfs_mount_test_fat_mirror_retry(struct pros_vfs_mount *mount)
{
    struct fatfs_volume *volume = 0;
    struct fatfs_mount_test_sector *primary = 0;
    struct fatfs_mount_test_sector *mirror = 0;
    struct fatfs_mount_test_sector *free_slots[2] = {0, 0};
    uint32_t free_count = 0u;
    uint32_t allocated_cluster = 0u;
    uint64_t allocated_sector = 0u;
    struct fatfs_mount_test_sector *data_sector = 0;
    FIL file, probe;
    UINT bytes_written = 0u, bytes_read = 0u;
    BYTE payload = 0xA5u, readback = 0u;
    FRESULT fr;
    uint8_t file_open = 0u, probe_open = 0u;
    int rc = PROS_VFS_EIO;
    int stage = 0;

    if (!mount)
        return PROS_VFS_EINVAL;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume)
        return PROS_VFS_EIO;

    stage = 1;
    rc = fatfs_enter();
    if (rc != PROS_VFS_OK)
        return rc;
    stage = 2;
    rc = fatfs_api_path(volume, "/MIRROR.TXT", fatfs_path_scratch,
                        sizeof(fatfs_path_scratch));
    if (rc != PROS_VFS_OK)
        goto done;

    stage = 3;
    fr = f_open(&file, fatfs_path_scratch,
                FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        rc = fatfs_result_to_vfs(fr);
        goto done;
    }
    file_open = 1u;

    /*
     * Ensure both synthetic FAT sectors have a baseline image before the
     * injected write. The block fixture stores writes in its sector cache.
     */
    stage = 4;
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        struct fatfs_mount_test_sector *slot =
            &fatfs_mount_test_media.cache[i];
        if (!slot->valid) {
            if (free_count < 2u)
                free_slots[free_count++] = slot;
        } else if (slot->lba == 32u) {
            primary = slot;
        } else if (slot->lba == 544u) {
            mirror = slot;
        }
    }
    if (!primary) {
        if (!free_slots[0])
            goto done;
        primary = free_slots[0];
        primary->lba = 32u;
        primary->valid = 1u;
        memcpy(primary->data,
               mirror ? mirror->data : fatfs_mount_test_media.fat,
               512u);
    }
    if (!mirror) {
        struct fatfs_mount_test_sector *slot =
            free_slots[0] != primary ? free_slots[0] : free_slots[1];
        if (!slot)
            goto done;
        mirror = slot;
        mirror->lba = 544u;
        mirror->valid = 1u;
        memcpy(mirror->data, primary->data, 512u);
    }
    stage = 5;
    if (memcmp(primary->data, mirror->data, 512u) != 0)
        goto done;

    /*
     * Complete the file write first. Cluster allocation can flush the FAT
     * window during f_write(), so it is not reliable to inject the mirror
     * failure only after f_write() and assume a dirty FAT window still exists.
     */
    stage = 6;
    fr = f_write(&file, &payload, 1u, &bytes_written);
    if (fr != FR_OK || bytes_written != 1u) {
        rc = PROS_VFS_EIO;
        goto done;
    }
    allocated_cluster = file.obj.sclust;
    if (allocated_cluster < 2u || allocated_cluster >= volume->fs.n_fatent) {
        stage = 15;
        goto done;
    }
    allocated_sector = volume->fs.database +
        (uint64_t)(allocated_cluster - 2u) * volume->fs.csize;
    if (file.sect != allocated_sector) {
        stage = 16;
        goto done;
    }

    /*
     * In FF_FS_TINY mode the file data and FAT metadata share fs->win.
     * f_write() leaves the one-byte file data dirty in that window. Persist
     * that sector before reusing the shared window for the injected FAT
     * update; otherwise the test itself overwrites the pending file data.
     */
    stage = 7;
#if FF_FS_TINY
    if (volume->fs.winsect != allocated_sector || !volume->fs.wflag ||
        volume->fs.win[0] != payload) {
        stage = 20;
        goto done;
    }
    if (disk_write(volume->fs.pdrv, volume->fs.win,
                   volume->fs.winsect, 1u) != RES_OK) {
        stage = 21;
        goto done;
    }
    volume->fs.wflag = 0u;
#endif
    /*
     * Stage a controlled change to an unused FAT entry. This guarantees that
     * f_sync() must flush a dirty FAT32 sector after the fault is enabled.
     * Cluster 100 is unused in this minimal fixture (only clusters 0..2 are
     * reserved before the test files are created).
     */
    if (primary->data[400u] != 0u || primary->data[401u] != 0u ||
        primary->data[402u] != 0u || primary->data[403u] != 0u)
        goto done;
    volume->fs.winsect = 32u;
    memcpy(volume->fs.win, primary->data, 512u);
    fatfs_mount_test_put32(&volume->fs.win[400u], 0x0FFFFFFFu);
    volume->fs.wflag = 1u;

    stage = 8;
    fatfs_mount_test_media.fail_write_lba = 544u;
    fatfs_mount_test_media.fail_write_lba_enabled = 1u;
    fr = f_sync(&file);
    fatfs_mount_test_media.fail_write_lba_enabled = 0u;
    if (fr != FR_DISK_ERR) {
        rc = PROS_VFS_EIO;
        goto done;
    }
    /* f_sync flushes the FIL data sector before touching the directory. */
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == allocated_sector) {
            data_sector = &fatfs_mount_test_media.cache[i];
            break;
        }
    }
    if (!data_sector || data_sector->data[0] != payload) {
        stage = 17;
        goto done;
    }

    /* Primary committed, mirror failed: the window must remain dirty. */
    stage = 9;
    if (memcmp(primary->data, mirror->data, 512u) == 0) {
        rc = PROS_VFS_EIO;
        goto done;
    }

    /* A second sync retries the retained dirty window and updates both FATs. */
    stage = 10;
    if (f_sync(&file) != FR_OK ||
        memcmp(primary->data, mirror->data, 512u) != 0) {
        rc = PROS_VFS_EIO;
        goto done;
    }
    stage = 11;
    if (f_close(&file) != FR_OK) {
        rc = PROS_VFS_EIO;
        goto done;
    }
    file_open = 0u;

    /* Confirm file data and metadata remain readable after mirror recovery. */
    stage = 12;
    fr = f_open(&probe, fatfs_path_scratch, FA_READ);
    if (fr != FR_OK) {
        rc = fatfs_result_to_vfs(fr);
        goto done;
    }
    probe_open = 1u;
    if (probe.obj.sclust != allocated_cluster) {
        stage = 18;
        goto done;
    }
    if (probe.obj.objsize != 1u) {
        stage = 19;
        goto done;
    }
    stage = 130;
    fr = f_read(&probe, &readback, 1u, &bytes_read);
    if (fr != FR_OK) {
        rc = -2000 - (int)fr;
        goto done;
    }
    stage = 131;
    if (bytes_read != 1u) {
        rc = PROS_VFS_EIO;
        goto done;
    }
    stage = 132;
    if (readback != payload) {
        /*
         * Preserve the observed byte in the boot diagnostic. The outer
         * mount self-test adds -3000 and this helper adds -1000-stage.
         */
        stage = 200 + (int)readback;
        rc = PROS_VFS_EIO;
        goto done;
    }
    stage = 14;
    if (f_close(&probe) != FR_OK) {
        probe_open = 0u;
        rc = PROS_VFS_EIO;
        goto done;
    }
    probe_open = 0u;
    rc = PROS_VFS_OK;

done:
    fatfs_mount_test_media.fail_write_lba_enabled = 0u;
    if (probe_open)
        (void)f_close(&probe);
    if (file_open)
        (void)f_close(&file);
    fatfs_leave();
    return rc == PROS_VFS_OK ? PROS_VFS_OK : -1000 - stage;
}


static int fatfs_mount_selftest(struct pros_vfs_provider *provider)
{
    static const uint8_t payload[] = {'F','A','T','3','2','-','O','K','!'};
    struct pros_block_device device;
    struct pros_partition partition;
    struct pros_partition_device view;
    struct pros_vfs_mount mount = {0};
    struct pros_vfs_provider *detected = 0;
    struct fatfs_volume *test_volume = 0;
    struct pros_vfs_file file;
    struct pros_vfs_dir dir;
    struct pros_vfs_dirent entry;
    uint8_t readback[sizeof(payload)];
    uint8_t extension_payload[512], extension_readback[512];
    uint32_t transferred = 0u;
    uint32_t flushes_before_open_sync = 0u;
    uint32_t flushes_before_close_sync = 0u;
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
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == &mount) {
            test_volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!test_volume) {
        failure_stage = __LINE__;
        goto cleanup;
    }

    rc = vfs_kernel_file_open(&mount, "/FATTEST.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        /* Preserve the provider/VFS error for the boot log while diagnosing
         * the first create/open path; the call site is already known. */
        failure_stage = 3300 + fatfs_mount_test_error_site;
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
        vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_seek(&file, sizeof(payload) + 64u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, 1u, &transferred) != PROS_VFS_OK ||
        transferred != 0u ||
        vfs_file_seek(&file, (uint64_t)UINT32_MAX + 1u) != PROS_VFS_OK ||
        vfs_file_read(&file, readback, 1u, &transferred) !=
            PROS_VFS_EOVERFLOW ||
        vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
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

    /*
     * Extend an existing one-cluster FAT32 file and fail the new cluster's
     * data-sector write. The previous FAT tail must remain EOC, the new
     * cluster must be free, and the original file bytes must remain readable.
     *
     * The fixture allocates cluster 3 to FATTEST.TXT, so this file receives
     * cluster 4 and its attempted extension receives cluster 5 (LBA 1059).
     */
    for (uint32_t i = 0u; i < sizeof(extension_payload); ++i)
        extension_payload[i] = (uint8_t)((i * 19u) ^ (i >> 2) ^ 0xA7u);
    rc = vfs_kernel_file_open(&mount, "/FAT-EXTEND.BIN",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK ||
        vfs_file_write(&file, extension_payload, sizeof(extension_payload),
                       &transferred) != PROS_VFS_OK ||
        transferred != sizeof(extension_payload) ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    rc = vfs_kernel_file_open(&mount, "/FAT-EXTEND.BIN",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE, &file);
    if (rc != PROS_VFS_OK ||
        vfs_file_seek(&file, sizeof(extension_payload)) != PROS_VFS_OK) {
        if (rc == PROS_VFS_OK)
            (void)vfs_file_close(&file);
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    fatfs_mount_test_media.fail_write_lba = 1059u; /* cluster 5 data */
    fatfs_mount_test_media.fail_write_lba_enabled = 1u;
    rc = vfs_file_write(&file, extension_payload, sizeof(extension_payload),
                        &transferred);
    fatfs_mount_test_media.fail_write_lba_enabled = 0u;
    if (rc != PROS_VFS_EIO || transferred != 0u) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    {
        const uint8_t *fat_sector = 0;
        for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
            if (fatfs_mount_test_media.cache[i].valid &&
                fatfs_mount_test_media.cache[i].lba == 32u) {
                fat_sector = fatfs_mount_test_media.cache[i].data;
                break;
            }
        }
        /*
         * Require the persisted FAT-sector cache entry. Falling back to the
         * fixture's original FAT bytes could hide a failed metadata flush.
         */
        if (!fat_sector) {
            failure_stage = __LINE__;
            goto cleanup;
        }
        uint32_t previous_entry =
            (uint32_t)fat_sector[16] |
            ((uint32_t)fat_sector[17] << 8) |
            ((uint32_t)fat_sector[18] << 16) |
            ((uint32_t)fat_sector[19] << 24);
        uint32_t new_entry =
            (uint32_t)fat_sector[20] |
            ((uint32_t)fat_sector[21] << 8) |
            ((uint32_t)fat_sector[22] << 16) |
            ((uint32_t)fat_sector[23] << 24);
        if ((previous_entry & 0x0FFFFFFFu) != 0x0FFFFFFFu ||
            (new_entry & 0x0FFFFFFFu) != 0u) {
            failure_stage = __LINE__;
            goto cleanup;
        }
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        file_open = 0u;
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    rc = vfs_kernel_file_open(&mount, "/FAT-EXTEND.BIN", PROS_VFS_O_READ,
                              &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_read(&file, extension_readback, sizeof(extension_readback),
                      &transferred) != PROS_VFS_OK ||
        transferred != sizeof(extension_payload) ||
        memcmp(extension_payload, extension_readback,
               sizeof(extension_payload)) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    if (vfs_kernel_unlink(&mount, "/FAT-EXTEND.BIN") != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }

    rc = vfs_kernel_file_open(&mount, "/BADCHAIN.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    if (fatfs_mount_test_expect_fat32_bad_chain(&mount, payload,
                                                sizeof(payload)) !=
        PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    if (vfs_kernel_unlink(&mount, "/BADCHAIN.TXT") != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }

    /*
     * A two-cluster file whose first FAT link is a bad-cluster marker must
     * fail after returning the valid first sector, without filling the
     * caller's unread buffer tail. Restoring the FAT link must recover reads.
     */
    {
        static uint8_t chain_payload[1024];
        for (uint32_t i = 0u; i < sizeof(chain_payload); ++i)
            chain_payload[i] = (uint8_t)((i * 23u) ^ (i >> 1) ^ 0x6Bu);
        rc = vfs_kernel_file_open(&mount, "/CHAINBAD.BIN",
                                  PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                                  PROS_VFS_O_CREATE, &file);
        if (rc != PROS_VFS_OK) {
            failure_stage = __LINE__;
            goto cleanup;
        }
        file_open = 1u;
        if (vfs_file_write(&file, chain_payload, sizeof(chain_payload),
                           &transferred) != PROS_VFS_OK ||
            transferred != sizeof(chain_payload) ||
            vfs_file_close(&file) != PROS_VFS_OK) {
            failure_stage = __LINE__;
            goto cleanup;
        }
        file_open = 0u;
        if (fatfs_mount_test_expect_fat32_bad_link(
                &mount, chain_payload, sizeof(chain_payload)) !=
            PROS_VFS_OK) {
            failure_stage = __LINE__;
            goto cleanup;
        }
        if (vfs_kernel_unlink(&mount, "/CHAINBAD.BIN") != PROS_VFS_OK) {
            failure_stage = __LINE__;
            goto cleanup;
        }
    }

    /* A failed block write must be surfaced and must not poison later I/O. */
    rc = vfs_kernel_file_open(&mount, "/WRITEERR.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload)) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    /*
     * The short write may remain in FatFs' sector buffer until close. Inject
     * the media error at that persistence boundary and require close to
     * report it while retaining a retryable FIL.
     */
    fatfs_mount_test_media.fail_writes = 1u;
    rc = vfs_file_close(&file);
    fatfs_mount_test_media.fail_writes = 0u;
    if (rc != PROS_VFS_EIO || vfs_file_close(&file) != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;

    /*
     * The retrying close must leave a durable FAT32 file, not merely clear
     * the in-memory FIL error. Unmount and remount the same media before
     * verifying the directory entry, file size, and contents.
     */
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    mounted = 0u;
    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 0u);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    mounted = 1u;
    rc = vfs_kernel_file_open(&mount, "/WRITEERR.TXT", PROS_VFS_O_READ,
                              &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK ||
        transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    if (vfs_kernel_unlink(&mount, "/WRITEERR.TXT") != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }

    /*
     * Fail the FAT32 root-directory sector write after file data succeeds.
     * The failed close must retain dirty metadata so retry writes the entry
     * again and completes the durable flush.
     */
    rc = vfs_kernel_file_open(&mount, "/METAERR.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload)) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    flushes_before_close_sync = fatfs_mount_test_media.flushes;
    fatfs_mount_test_media.fail_write_lba = 1056u;
    fatfs_mount_test_media.fail_write_lba_enabled = 1u;
    rc = vfs_file_close(&file);
    fatfs_mount_test_media.fail_write_lba_enabled = 0u;
    if (rc != PROS_VFS_EIO ||
        fatfs_mount_test_media.flushes != flushes_before_close_sync) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK ||
        fatfs_mount_test_media.flushes != flushes_before_close_sync + 1u) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    rc = vfs_kernel_file_open(&mount, "/METAERR.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    if (vfs_kernel_unlink(&mount, "/METAERR.TXT") != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }

    /*
     * FSInfo is a separate FAT32 metadata sector. A failed write must be
     * returned to the caller and leave fsi_flag set so close can retry it.
     */
    rc = vfs_kernel_file_open(&mount, "/FSINFOERR.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload)) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    /*
     * Confirm that the allocation update is still pending before close.
     * Otherwise this test can pass through f_close without attempting FSInfo.
     */
    if (test_volume->fs.fsi_flag != 1u) {
        failure_stage = 110000 + test_volume->fs.fsi_flag;
        goto cleanup;
    }
    if (test_volume->fs.volbase != 0u) {
        failure_stage = 120000 +
            (int)(test_volume->fs.volbase > 100000u ? 100000u :
                  test_volume->fs.volbase);
        goto cleanup;
    }
    flushes_before_close_sync = fatfs_mount_test_media.flushes;
    fatfs_mount_test_last_io_error = 0u;
    fatfs_mount_test_media.fail_write_lba = test_volume->fs.volbase + 1u;
    fatfs_mount_test_media.fail_write_lba_enabled = 1u;
    rc = vfs_file_close(&file);
    fatfs_mount_test_media.fail_write_lba_enabled = 0u;
    if (rc != PROS_VFS_EIO) {
        /* Include the I/O hook result: 0 means the injected write was missed. */
        failure_stage = 130000 + fatfs_mount_test_last_io_error;
        goto cleanup;
    }
    if (fatfs_mount_test_last_io_error != 2u) {
        failure_stage = 140000 + fatfs_mount_test_last_io_error;
        goto cleanup;
    }
    if (fatfs_mount_test_media.flushes != flushes_before_close_sync) {
        failure_stage = 80000 +
            (int)(fatfs_mount_test_media.flushes - flushes_before_close_sync);
        goto cleanup;
    }
    rc = vfs_file_close(&file);
    if (rc != PROS_VFS_OK) {
        failure_stage = 90000 + (rc < 0 ? -rc : rc);
        goto cleanup;
    }
    if (fatfs_mount_test_media.flushes != flushes_before_close_sync + 1u) {
        failure_stage = 100000 +
            (int)(fatfs_mount_test_media.flushes - flushes_before_close_sync);
        goto cleanup;
    }
    file_open = 0u;
    {
        struct fatfs_mount_test_sector *fsinfo_sector = 0;
        for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
            if (fatfs_mount_test_media.cache[i].valid &&
                fatfs_mount_test_media.cache[i].lba == 1u) {
                fsinfo_sector = &fatfs_mount_test_media.cache[i];
                break;
            }
        }
        uint32_t persisted_free_count;
        uint32_t persisted_next_free;
        if (!fsinfo_sector ||
            fsinfo_sector->data[0] != 0x52u ||
            fsinfo_sector->data[1] != 0x52u ||
            fsinfo_sector->data[2] != 0x61u ||
            fsinfo_sector->data[3] != 0x41u ||
            fsinfo_sector->data[484] != 0x72u ||
            fsinfo_sector->data[485] != 0x72u ||
            fsinfo_sector->data[486] != 0x41u ||
            fsinfo_sector->data[487] != 0x61u ||
            fsinfo_sector->data[508] != 0x00u ||
            fsinfo_sector->data[509] != 0x00u ||
            fsinfo_sector->data[510] != 0x55u ||
            fsinfo_sector->data[511] != 0xAAu) {
            failure_stage = __LINE__;
            goto cleanup;
        }
        persisted_free_count =
            (uint32_t)fsinfo_sector->data[488] |
            ((uint32_t)fsinfo_sector->data[489] << 8) |
            ((uint32_t)fsinfo_sector->data[490] << 16) |
            ((uint32_t)fsinfo_sector->data[491] << 24);
        persisted_next_free =
            (uint32_t)fsinfo_sector->data[492] |
            ((uint32_t)fsinfo_sector->data[493] << 8) |
            ((uint32_t)fsinfo_sector->data[494] << 16) |
            ((uint32_t)fsinfo_sector->data[495] << 24);
        /*
         * The fixture starts with an unknown free count (0xFFFFFFFF), which
         * FatFs is allowed to preserve. Compare both persisted hints with the
         * live allocation state, including that valid unknown-count value.
         */
        if (persisted_free_count != test_volume->fs.free_clst ||
            persisted_next_free != test_volume->fs.last_clst) {
            failure_stage = __LINE__;
            goto cleanup;
        }
    }
    rc = vfs_kernel_file_open(&mount, "/FSINFOERR.TXT", PROS_VFS_O_READ,
                              &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    if (vfs_kernel_unlink(&mount, "/FSINFOERR.TXT") != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }

    /*
     * A failed CTRL_SYNC during FAT32 f_close must retain FA_MODIFIED so the
     * next close retries the durable flush rather than silently succeeding.
     */
    rc = vfs_kernel_file_open(&mount, "/FLUSHERR.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload)) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    flushes_before_close_sync = fatfs_mount_test_media.flushes;
    fatfs_mount_test_media.fail_flush = 1u;
    rc = vfs_file_close(&file);
    fatfs_mount_test_media.fail_flush = 0u;
    if (rc != PROS_VFS_EIO ||
        fatfs_mount_test_media.flushes != flushes_before_close_sync + 1u) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK ||
        fatfs_mount_test_media.flushes != flushes_before_close_sync + 2u) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    if (vfs_kernel_unlink(&mount, "/FLUSHERR.TXT") != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }

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

    /*
     * Fail the parent-directory entry write at f_mkdir's final sync. FatFs
     * has already registered the new entry in its dirty window, so recovery
     * must be able to remove that incomplete operation and create it again.
     */
    fatfs_mount_test_media.fail_write_lba = 1056u; /* FAT32 root directory */
    fatfs_mount_test_media.fail_write_lba_enabled = 1u;
    rc = vfs_kernel_mkdir(&mount, "/MKDIRERR");
    fatfs_mount_test_media.fail_write_lba_enabled = 0u;
    if (rc != PROS_VFS_EIO) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    if (vfs_kernel_rmdir(&mount, "/MKDIRERR") != PROS_VFS_OK ||
        vfs_kernel_mkdir(&mount, "/MKDIRERR") != PROS_VFS_OK ||
        vfs_kernel_rmdir(&mount, "/MKDIRERR") != PROS_VFS_OK) {
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

    rc = fatfs_mount_test_fat_mirror_retry(&mount);
    if (rc != PROS_VFS_OK) {
        /* Preserve the helper's stage-specific diagnostic for the boot log. */
        failure_stage = 0;
        goto cleanup;
    }

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

    /*
     * Inject the read fault at the end of the mount test. A failed FatFs FIL
     * may retain its error state, so close it and prove recovery with a fresh
     * handle after the block device starts responding again.
     */
    rc = vfs_kernel_file_open(&mount, "/RENAMED.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_seek(&file, 0u) != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    fatfs_mount_test_media.fail_reads = 1u;
    rc = vfs_file_read(&file, readback, sizeof(payload), &transferred);
    fatfs_mount_test_media.fail_reads = 0u;
    if (rc != PROS_VFS_EIO) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 0u;
    rc = vfs_kernel_file_open(&mount, "/RENAMED.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK) {
        failure_stage = __LINE__;
        goto cleanup;
    }
    file_open = 1u;
    if (vfs_file_read(&file, readback, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload) ||
        memcmp(payload, readback, sizeof(payload)) != 0 ||
        vfs_file_close(&file) != PROS_VFS_OK) {
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


/*
 * Persist a two-cluster FAT32 file, corrupt the second cluster's FAT entry,
 * and verify that the provider surfaces the chain corruption on the next
 * read rather than returning fabricated/zero-filled bytes.
 */
static void fatfs_mount_test_corrupt_fat_entry(uint32_t cluster,
                                                uint32_t value)
{
    uint32_t offset = cluster * 4u;
    fatfs_mount_test_put32(&fatfs_mount_test_media.fat[offset], value);
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        struct fatfs_mount_test_sector *slot =
            &fatfs_mount_test_media.cache[i];
        if (slot->valid &&
            (slot->lba == 32u || slot->lba == 233u ||
             slot->lba == 544u))
            fatfs_mount_test_put32(&slot->data[offset], value);
    }
}

static int fatfs_corrupt_chain_selftest(struct pros_vfs_provider *provider)
{
    /* Keep multi-cluster test buffers out of the 4-KiB kernel thread stack. */
    static uint8_t payload[600];
    static uint8_t readback[512];
    struct pros_block_device device;
    struct pros_partition partition;
    struct pros_partition_device view;
    struct pros_vfs_mount mount = {0};
    struct pros_vfs_file file;
    uint32_t transferred = 0u;
    uint8_t mounted = 0u;
    uint8_t file_open = 0u;
    int rc;

    for (uint32_t i = 0u; i < sizeof(payload); ++i)
        payload[i] = (uint8_t)((i * 37u) ^ (i >> 1) ^ 0xA5u);

    fatfs_mount_test_init_media();
    if (block_device_init(&device, 512u, FATFS_MOUNT_TEST_SECTORS,
                          fatfs_mount_test_read, fatfs_mount_test_write,
                          &fatfs_mount_test_media) != PROS_BLOCK_OK ||
        block_device_set_flush(&device, fatfs_mount_test_flush) !=
            PROS_BLOCK_OK)
        return -1;

    memset(&partition, 0, sizeof(partition));
    partition.first_lba = 0u;
    partition.sector_count = FATFS_MOUNT_TEST_SECTORS;
    partition.mbr_type = 0x0Cu;
    partition.table_type = PROS_PARTITION_MBR;
    if (partition_device_init(&view, &device, &partition) !=
        PROS_PARTITION_OK)
        return -2;

    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 0u);
    if (rc != PROS_VFS_OK)
        return -3;
    mounted = 1u;

    rc = vfs_kernel_file_open(&mount, "/CHAIN.TXT",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    file_open = 1u;
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload)) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        file_open = 0u;
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    file_open = 0u;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    mounted = 0u;

    /*
     * The fixture reserves clusters 0, 1 and root cluster 2. The first file
     * therefore starts at cluster 3 and its 600-byte payload allocates
     * cluster 4 as the second cluster. Turn FAT[3] into the reserved bad
     * cluster marker in both mirrored FAT copies (and the fixture backing).
     */
    fatfs_mount_test_corrupt_fat_entry(3u, 0x0FFFFFF7u);

    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 1u);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    mounted = 1u;

    rc = vfs_kernel_file_open(&mount, "/CHAIN.TXT", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    file_open = 1u;
    if (vfs_file_read(&file, readback, sizeof(readback), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(readback) ||
        memcmp(payload, readback, sizeof(readback)) != 0) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    transferred = 0u;
    rc = vfs_file_read(&file, readback, 88u, &transferred);
    if (rc != PROS_VFS_ECORRUPT) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        file_open = 0u;
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    file_open = 0u;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    mounted = 0u;
    return PROS_VFS_OK;

cleanup:
    if (file_open)
        (void)vfs_file_close(&file);
    if (mounted)
        (void)pros_vfs_provider_unmount_device(&mount);
    return rc < 0 ? rc : PROS_VFS_EIO;
}


/*
 * Corrupt a persisted FAT32 short-directory entry's start cluster. The
 * provider must reject the out-of-range cluster as corruption, not follow it
 * into arbitrary sectors. Restore the original bytes and verify remount/read
 * recovery so this fixture never leaves a damaged medium behind.
 */
static int fatfs_corrupt_directory_entry_selftest(
    struct pros_vfs_provider *provider)
{
    static uint8_t payload[600];
    static uint8_t readback[32];
    static uint8_t size_readback[2048];
    struct pros_block_device device;
    struct pros_partition partition;
    struct pros_partition_device view;
    struct pros_vfs_mount mount = {0};
    struct pros_vfs_file file;
    struct fatfs_mount_test_sector *root_sector = 0;
    struct fatfs_volume *volume = 0;
    static const uint8_t short_name[11] = {
        'D','I','R','E','N','T',' ',' ','B','I','N'
    };
    uint8_t saved_cluster[4];
    uint8_t saved_size[4];
    uint32_t entry_offset = 0u, transferred = 0u;
    uint8_t found = 0u;
    uint8_t mounted = 0u;
    uint8_t file_open = 0u;
    uint8_t size_corrupted = 0u;
    uint8_t saved_attr = 0u;
    uint8_t attr_corrupted = 0u;
    int rc = PROS_VFS_EIO;

    for (uint32_t i = 0u; i < sizeof(payload); ++i)
        payload[i] = (uint8_t)((i * 29u) ^ (i >> 2) ^ 0xC3u);

    fatfs_mount_test_init_media();
    if (block_device_init(&device, 512u, FATFS_MOUNT_TEST_SECTORS,
                          fatfs_mount_test_read, fatfs_mount_test_write,
                          &fatfs_mount_test_media) != PROS_BLOCK_OK ||
        block_device_set_flush(&device, fatfs_mount_test_flush) !=
            PROS_BLOCK_OK)
        return -1;

    memset(&partition, 0, sizeof(partition));
    partition.first_lba = 0u;
    partition.sector_count = FATFS_MOUNT_TEST_SECTORS;
    partition.mbr_type = 0x0Cu;
    partition.table_type = PROS_PARTITION_MBR;
    if (partition_device_init(&view, &device, &partition) !=
        PROS_PARTITION_OK)
        return -2;

    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 0u);
    if (rc != PROS_VFS_OK)
        return -3;
    mounted = 1u;

    rc = vfs_kernel_file_open(&mount, "/DIRENT.BIN",
                              PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                              PROS_VFS_O_CREATE, &file);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    file_open = 1u;
    if (vfs_file_write(&file, payload, sizeof(payload), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(payload)) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    rc = vfs_file_close(&file);
    file_open = 0u;
    if (rc != PROS_VFS_OK)
        goto cleanup;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    mounted = 0u;

    /*
     * The fixture's FAT32 root directory is cluster 2 at LBA 1056. Locate
     * DIRENT.BIN by its 11-byte short name rather than assuming a slot index.
     * FAT32 stores the high and low start-cluster words at offsets 20 and 26.
     */
    for (uint32_t i = 0u; i < FATFS_MOUNT_TEST_CACHE; ++i) {
        if (fatfs_mount_test_media.cache[i].valid &&
            fatfs_mount_test_media.cache[i].lba == 1056u) {
            root_sector = &fatfs_mount_test_media.cache[i];
            break;
        }
    }
    if (!root_sector)
        return -4;
    for (uint32_t offset = 0u; offset + 32u <= 512u; offset += 32u) {
        if (memcmp(root_sector->data + offset, short_name,
                   sizeof(short_name)) == 0) {
            entry_offset = offset;
            found = 1u;
            break;
        }
    }
    if (!found)
        return -5;
    memcpy(saved_cluster, &root_sector->data[entry_offset + 20u], 2u);
    memcpy(saved_cluster + 2u, &root_sector->data[entry_offset + 26u], 2u);
    fatfs_mount_test_put16(&root_sector->data[entry_offset + 20u], 0x0FFFu);
    fatfs_mount_test_put16(&root_sector->data[entry_offset + 26u], 0xFFF0u);

    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 1u);
    if (rc != PROS_VFS_OK)
        goto restore_failure;
    mounted = 1u;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == &mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume) {
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }

    rc = vfs_kernel_file_open(&mount, "/DIRENT.BIN", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK)
        goto restore_failure;
    file_open = 1u;
    /*
     * FatFs may accept the directory entry at open time and validate the
     * start cluster only when the first data sector is requested.
     */
    rc = vfs_file_read(&file, readback, 1u, &transferred);
    if (rc != PROS_VFS_ECORRUPT) {
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_EBUSY) {
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }
    memcpy(&root_sector->data[entry_offset + 20u], saved_cluster, 2u);
    memcpy(&root_sector->data[entry_offset + 26u], saved_cluster + 2u, 2u);
    if (volume->fs.winsect == 1056u) {
        memcpy(volume->fs.win + entry_offset + 20u, saved_cluster, 2u);
        memcpy(volume->fs.win + entry_offset + 26u, saved_cluster + 2u, 2u);
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        file_open = 0u;
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    file_open = 0u;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }
    mounted = 0u;

    /* Restore both words before attempting any healthy remount/read. */
    memcpy(&root_sector->data[entry_offset + 20u], saved_cluster, 2u);
    memcpy(&root_sector->data[entry_offset + 26u], saved_cluster + 2u, 2u);
    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 1u);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    mounted = 1u;
    rc = vfs_kernel_file_open(&mount, "/DIRENT.BIN", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    file_open = 1u;
    if (vfs_file_read(&file, readback, sizeof(readback), &transferred) !=
            PROS_VFS_OK || transferred != sizeof(readback) ||
        memcmp(payload, readback, sizeof(readback)) != 0) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    rc = vfs_file_close(&file);
    file_open = 0u;
    if (rc != PROS_VFS_OK)
        goto cleanup;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    mounted = 0u;
    volume = 0;

    /*
     * A file size larger than its allocated cluster chain is malformed
     * directory metadata. FatFs must stop at the premature end-of-chain,
     * report corruption, and never fabricate zero-filled tail data.
     */
    memcpy(saved_size, &root_sector->data[entry_offset + 28u],
           sizeof(saved_size));
    fatfs_mount_test_put32(&root_sector->data[entry_offset + 28u], 2048u);
    size_corrupted = 1u;
    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 1u);
    if (rc != PROS_VFS_OK)
        goto restore_failure;
    mounted = 1u;
    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == &mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume) {
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }
    rc = vfs_kernel_file_open(&mount, "/DIRENT.BIN", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK)
        goto restore_failure;
    file_open = 1u;
    rc = vfs_file_read(&file, size_readback, sizeof(size_readback),
                       &transferred);
    if (rc != PROS_VFS_ECORRUPT) {
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }

    /* Restore the persisted and in-memory directory entry before closing. */
    memcpy(&root_sector->data[entry_offset + 28u], saved_size,
           sizeof(saved_size));
    if (volume->fs.winsect == 1056u)
        memcpy(volume->fs.win + entry_offset + 28u, saved_size,
               sizeof(saved_size));
    size_corrupted = 0u;
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }
    file_open = 0u;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }
    mounted = 0u;
    volume = 0;

    /*
     * FAT directory attribute bits 6 and 7 are reserved. FatFs normally
     * masks them before interpreting an entry; reject them instead so a
     * malformed entry cannot silently become an ordinary file/directory.
     */
    saved_attr = root_sector->data[entry_offset + 11u];
    root_sector->data[entry_offset + 11u] |= 0x80u;
    attr_corrupted = 1u;
    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 1u);
    if (rc != PROS_VFS_OK)
        goto restore_failure;
    mounted = 1u;
    rc = vfs_kernel_file_open(&mount, "/DIRENT.BIN", PROS_VFS_O_READ, &file);
    if (rc == PROS_VFS_OK) {
        file_open = 1u;
        (void)vfs_file_close(&file);
        file_open = 0u;
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }
    if (rc != PROS_VFS_ECORRUPT)
        goto restore_failure;

    for (uint32_t i = 0u; i < FATFS_VOLUME_MAX; ++i) {
        if (fatfs_volumes[i].allocated && fatfs_volumes[i].mount == &mount) {
            volume = &fatfs_volumes[i];
            break;
        }
    }
    if (!volume) {
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }
    root_sector->data[entry_offset + 11u] = saved_attr;
    if (volume->fs.winsect == 1056u)
        volume->fs.win[entry_offset + 11u] = saved_attr;
    attr_corrupted = 0u;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {
        rc = PROS_VFS_EIO;
        goto restore_failure;
    }
    mounted = 0u;
    volume = 0;

    /* A healthy remount must still expose the original file and contents. */
    rc = pros_vfs_provider_mount_device(provider, &mount, &device, &view,
                                        &partition, 1u);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    mounted = 1u;
    rc = vfs_kernel_file_open(&mount, "/DIRENT.BIN", PROS_VFS_O_READ, &file);
    if (rc != PROS_VFS_OK)
        goto cleanup;
    file_open = 1u;
    if (vfs_file_read(&file, readback, 32u, &transferred) != PROS_VFS_OK ||
        transferred != 32u || memcmp(payload, readback, 32u) != 0) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    if (vfs_file_close(&file) != PROS_VFS_OK) {
        file_open = 0u;
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    file_open = 0u;
    if (pros_vfs_provider_unmount_device(&mount) != PROS_VFS_OK) {
        rc = PROS_VFS_EIO;
        goto cleanup;
    }
    mounted = 0u;
    return PROS_VFS_OK;

restore_failure:
    /*
     * Restore the backing sector and FatFs window before closing the handle;
     * then release the file pin before attempting provider unmount.
     */
    memcpy(&root_sector->data[entry_offset + 20u], saved_cluster, 2u);
    memcpy(&root_sector->data[entry_offset + 26u], saved_cluster + 2u, 2u);
    if (size_corrupted)
        memcpy(&root_sector->data[entry_offset + 28u], saved_size,
               sizeof(saved_size));
    if (attr_corrupted)
        root_sector->data[entry_offset + 11u] = saved_attr;
    if (volume && volume->fs.winsect == 1056u) {
        memcpy(volume->fs.win + entry_offset + 20u, saved_cluster, 2u);
        memcpy(volume->fs.win + entry_offset + 26u, saved_cluster + 2u, 2u);
        if (size_corrupted)
            memcpy(volume->fs.win + entry_offset + 28u, saved_size,
                   sizeof(saved_size));
        if (attr_corrupted)
            volume->fs.win[entry_offset + 11u] = saved_attr;
    }
    if (file_open) {
        (void)vfs_file_close(&file);
        file_open = 0u;
    }
    if (mounted &&
        pros_vfs_provider_unmount_device(&mount) == PROS_VFS_OK)
        mounted = 0u;
    goto cleanup;

cleanup:
    if (file_open)
        (void)vfs_file_close(&file);
    if (mounted)
        (void)pros_vfs_provider_unmount_device(&mount);
    return rc < 0 ? rc : PROS_VFS_EIO;
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
    rc = fatfs_exfat_mount_selftest(&fatfs_provider);
    if (rc != PROS_VFS_OK)
        return -2500 + rc;
    rc = fatfs_mount_selftest(&fatfs_provider);
    if (rc != PROS_VFS_OK)
        return -3000 + rc;
    rc = fatfs_corrupt_chain_selftest(&fatfs_provider);
    if (rc != PROS_VFS_OK)
        return -4000 + rc;
    rc = fatfs_corrupt_directory_entry_selftest(&fatfs_provider);
    if (rc != PROS_VFS_OK)
        return -5000 + rc;
    return PROS_VFS_OK;
}

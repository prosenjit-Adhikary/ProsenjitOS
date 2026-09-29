#include "vfs.h"
#include "partition.h"
#include "prosenjitfs.h"

#define PROS_VFS_PROBE_SECTOR_MAX 4096u

static int vfs_selftest_read(struct pros_block_device *device,
                             uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    (void)buffer;
    return PROS_BLOCK_OK;
}

int vfs_mount_init(struct pros_vfs_mount *mount,
                   enum pros_filesystem_type type,
                   struct pros_block_device *device,
                   void *private_data,
                   uint8_t read_only)
{
    if (!mount || !device || type == PROS_FS_UNKNOWN)
        return PROS_VFS_EINVAL;
    mount->type = type;
    mount->device = device;
    mount->private_data = private_data;
    mount->root = 0;
    mount->read_only = read_only ? 1u : 0u;
    mount->active = 0;
    return PROS_VFS_OK;
}

int vfs_mount_partition(struct pros_vfs_mount *mount,
                        enum pros_filesystem_type type,
                        struct pros_partition_device *view,
                        struct pros_block_device *parent,
                        const struct pros_partition *partition,
                        void *private_data, uint8_t read_only)
{
    if (!mount || !view || !parent || !partition)
        return PROS_VFS_EINVAL;
    if (partition_device_init(view, parent, partition) != PROS_PARTITION_OK)
        return PROS_VFS_EINVAL;
    return vfs_mount_init(mount, type, &view->device, private_data, read_only);
}

static uint16_t vfs_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int vfs_power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

int vfs_probe_filesystem(struct pros_block_device *device,
                         enum pros_filesystem_type *type)
{
    uint8_t sector[PROS_VFS_PROBE_SECTOR_MAX];
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;

    if (!device || !type || device->sector_size < 512u ||
        device->sector_size > PROS_VFS_PROBE_SECTOR_MAX)
        return PROS_VFS_EINVAL;
    if (block_device_read(device, 0, 1, sector) != PROS_BLOCK_OK)
        return PROS_VFS_EIO;

    *type = PROS_FS_UNKNOWN;

    /* ProsenjitFS has its own versioned/checksummed superblock format and
     * must be validated independently of legacy BPB signatures. */
    if (prosenjitfs_probe(device) == PROSENJITFS_OK) {
        *type = PROS_FS_PROSENJIT;
        return PROS_VFS_OK;
    }

    if (sector[510] != 0x55u || sector[511] != 0xAAu)
        return PROS_VFS_ENOSYS;

    bytes_per_sector = vfs_le16(&sector[11]);
    sectors_per_cluster = sector[13];
    if (!vfs_power_of_two(bytes_per_sector) ||
        bytes_per_sector != device->sector_size ||
        bytes_per_sector > PROS_VFS_PROBE_SECTOR_MAX ||
        !vfs_power_of_two(sectors_per_cluster))
        return PROS_VFS_ENOSYS;

    /* NTFS: OEM ID plus a sane BPB geometry. */
    if (sector[3] == 'N' && sector[4] == 'T' && sector[5] == 'F' &&
        sector[6] == 'S' && sector[7] == ' ' && sector[8] == ' ' &&
        sector[9] == ' ' && sector[10] == ' ') {
        *type = PROS_FS_NTFS;
        return PROS_VFS_OK;
    }

    /* exFAT: the complete eight-byte OEM ID is "EXFAT   ". */
    if (sector[3] == 'E' && sector[4] == 'X' && sector[5] == 'F' &&
        sector[6] == 'A' && sector[7] == 'T' && sector[8] == ' ' &&
        sector[9] == ' ' && sector[10] == ' ') {
        *type = PROS_FS_EXFAT;
        return PROS_VFS_OK;
    }

    /* FAT32: filesystem type label is at BPB offset 82. */
    if (sector[82] == 'F' && sector[83] == 'A' && sector[84] == 'T' &&
        sector[85] == '3' && sector[86] == '2') {
        *type = PROS_FS_FAT32;
        return PROS_VFS_OK;
    }

    return PROS_VFS_ENOSYS;
}

int vfs_mount_set_root(struct pros_vfs_mount *mount,
                       struct pros_vfs_vnode *root)
{
    if (!mount || !root || !root->initialized || root->mount != mount ||
        root->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    if (vfs_vnode_get(root) != PROS_VFS_OK)
        return PROS_VFS_EINVAL;
    mount->root = root;
    return PROS_VFS_OK;
}

int vfs_mount_activate(struct pros_vfs_mount *mount)
{
    if (!mount || !mount->device)
        return PROS_VFS_EINVAL;
    if (mount->active)
        return PROS_VFS_EBUSY;
    mount->active = 1;
    return PROS_VFS_OK;
}

int vfs_mount_deactivate(struct pros_vfs_mount *mount)
{
    if (!mount)
        return PROS_VFS_EINVAL;
    if (mount->active)
        mount->active = 0;
    if (mount->root) {
        vfs_vnode_put(mount->root);
        mount->root = 0;
    }
    return PROS_VFS_OK;
}

int vfs_mount_is_read_only(const struct pros_vfs_mount *mount)
{
    if (!mount)
        return PROS_VFS_EINVAL;
    return mount->read_only ? 1 : 0;
}

int vfs_file_init(struct pros_vfs_file *file,
                  struct pros_vfs_mount *mount,
                  const struct pros_vfs_file_ops *ops,
                  void *private_data, uint32_t flags)
{
    if (!file || !mount || !mount->active || !ops || !ops->read ||
        !ops->close)
        return PROS_VFS_EINVAL;
    file->mount = mount;
    file->vnode = 0;
    file->ops = ops;
    file->private_data = private_data;
    file->position = 0;
    file->flags = flags;
    file->open = 1u;
    return PROS_VFS_OK;
}

int vfs_file_read(struct pros_vfs_file *file, void *buffer,
                  uint32_t length, uint32_t *bytes_read)
{
    int rc;
    if (!file || !file->open || !file->mount || !file->mount->active ||
        !file->ops || !file->ops->read || !buffer || !bytes_read ||
        !file->vnode || file->position > 0xFFFFFFFFULL)
        return PROS_VFS_EINVAL;

    /*
     * VFS read semantics must make EOF a successful zero-byte read. Do not
     * delegate a zero-length request to filesystem drivers that may reject
     * size == 0 as an invalid internal operation.
     */
    *bytes_read = 0u;
    if (length == 0u || file->position >= file->vnode->size)
        return PROS_VFS_OK;

    rc = file->ops->read(file, (uint32_t)file->position, length, buffer);
    if (rc < 0) return rc;
    if ((uint32_t)rc > length)
        return PROS_VFS_EIO;
    file->position += (uint32_t)rc;
    *bytes_read = (uint32_t)rc;
    return PROS_VFS_OK;
}

int vfs_file_write(struct pros_vfs_file *file, const void *buffer,
                   uint32_t length, uint32_t *bytes_written)
{
    int rc;
    if (!file || !file->open || !file->mount || !file->mount->active ||
        !file->ops || !file->ops->write || !buffer || !bytes_written ||
        (file->flags & PROS_VFS_O_WRITE) == 0u ||
        file->mount->read_only || file->position > 0xFFFFFFFFULL)
        return PROS_VFS_EPERM;
    rc = file->ops->write(file, (uint32_t)file->position, length, buffer);
    if (rc < 0)
        return rc;
    if ((uint32_t)rc > length)
        return PROS_VFS_EIO;
    file->position += (uint32_t)rc;
    *bytes_written = (uint32_t)rc;
    return PROS_VFS_OK;
}

int vfs_file_truncate(struct pros_vfs_file *file, uint64_t size)
{
    if (!file || !file->open || !file->mount || !file->mount->active ||
        !file->vnode || file->vnode->type != PROS_VFS_NODE_FILE ||
        !file->vnode->ops || !file->vnode->ops->truncate ||
        (file->flags & PROS_VFS_O_WRITE) == 0u ||
        file->mount->read_only)
        return PROS_VFS_EPERM;
    return file->vnode->ops->truncate(file->vnode, size);
}

int vfs_file_close(struct pros_vfs_file *file)
{
    int rc;
    if (!file || !file->open || !file->ops || !file->ops->close)
        return PROS_VFS_EINVAL;
    rc = file->ops->close(file);
    if (rc != PROS_VFS_OK)
        return rc;
    file->open = 0u;
    if (file->vnode)
        vfs_vnode_put(file->vnode);
    file->vnode = 0;
    file->mount = 0;
    file->ops = 0;
    file->private_data = 0;
    file->position = 0;
    return PROS_VFS_OK;
}

int vfs_file_seek(struct pros_vfs_file *file, uint64_t position)
{
    if (!file || !file->open || !file->vnode)
        return PROS_VFS_EINVAL;
    if (position > file->vnode->size)
        return PROS_VFS_EOVERFLOW;
    file->position = position;
    return PROS_VFS_OK;
}

int vfs_file_admit(const struct pros_vfs_mount *mount,
                   const uint8_t *data,
                   uint64_t length,
                   const struct pros_file_security_metadata *metadata,
                   struct pros_file_admission_record *record)
{
    struct pros_file_admission_input input;

    if (!mount || !mount->device || !mount->active ||
        !data || length == 0 || !metadata || !record)
        return PROS_VFS_EINVAL;

    /*
     * The VFS deliberately does not inspect or reinterpret filesystem
     * formats here. The filesystem driver owns format/provenance/signature
     * evidence; the security admission broker owns hashing and policy.
     */
    input.data = data;
    input.length = length;
    input.metadata = *metadata;

    return pros_file_admission_evaluate(&input, record);
}

static int vfs_security_admission_selftest(void)
{
    struct pros_block_device device;
    struct pros_vfs_mount mount;
    struct pros_file_admission_record record;
    struct pros_file_security_metadata metadata = {
        .format_valid = 1,
        .signature_valid = 1,
        .signature_present = 1,
        .local_trusted = 0,
        .local_denied = 0,
        .reputation = PROS_FILE_REPUTATION_UNAVAILABLE,
        .provenance_known = 1
    };
    static const uint8_t data[] = { 'a', 'b', 'c' };

    if (block_device_init(&device, 512, 1,
                          vfs_selftest_read, 0, 0) != PROS_BLOCK_OK)
        return -1;
    if (vfs_mount_init(&mount, PROS_FS_PROSENJIT, &device, 0, 1) !=
        PROS_VFS_OK)
        return -1;

    if (vfs_file_admit(&mount, data, sizeof(data), &metadata, &record) !=
        PROS_VFS_EINVAL)
        return -1;

    if (vfs_mount_activate(&mount) != PROS_VFS_OK ||
        vfs_file_admit(&mount, data, sizeof(data), &metadata, &record) != 0 ||
        record.security.state != PROS_FILE_VERIFIED ||
        record.security.action != PROS_FILE_ALLOW)
        return -1;

    if (vfs_mount_activate(&mount) != PROS_VFS_EBUSY ||
        vfs_mount_deactivate(&mount) != PROS_VFS_OK ||
        vfs_file_admit(&mount, data, sizeof(data), &metadata, &record) !=
            PROS_VFS_EINVAL)
        return -1;

    return 0;
}

int vfs_vnode_init(struct pros_vfs_vnode *vnode,
                   struct pros_vfs_mount *mount,
                   struct pros_vfs_vnode *parent,
                   enum pros_vfs_node_type type,
                   const struct pros_vfs_vnode_ops *ops,
                   const struct pros_vfs_file_ops *file_ops,
                   void *private_data,
                   uint64_t size)
{
    if (!vnode || !mount || !ops ||
        (type != PROS_VFS_NODE_FILE && type != PROS_VFS_NODE_DIRECTORY))
        return PROS_VFS_EINVAL;
    if (type == PROS_VFS_NODE_DIRECTORY && (!ops->lookup || !ops->readdir))
        return PROS_VFS_EINVAL;
    if (type == PROS_VFS_NODE_FILE && (!file_ops || !file_ops->read ||
                                       !file_ops->close))
        return PROS_VFS_EINVAL;
    vnode->mount = mount;
    vnode->parent = parent;
    vnode->ops = ops;
    vnode->file_ops = file_ops;
    vnode->private_data = private_data;
    vnode->type = type;
    vnode->size = size;
    vnode->refcount = 1u;
    vnode->initialized = 1u;
    return PROS_VFS_OK;
}

int vfs_vnode_get(struct pros_vfs_vnode *vnode)
{
    if (!vnode || !vnode->initialized || vnode->refcount == 0u)
        return PROS_VFS_EINVAL;
    if (vnode->refcount == 0xFFFFFFFFu)
        return PROS_VFS_EOVERFLOW;
    ++vnode->refcount;
    return PROS_VFS_OK;
}

int vfs_vnode_put(struct pros_vfs_vnode *vnode)
{
    if (!vnode || !vnode->initialized || vnode->refcount == 0u)
        return PROS_VFS_EINVAL;
    --vnode->refcount;
    if (vnode->refcount == 0u) {
        void (*release)(struct pros_vfs_vnode *) = vnode->ops->release;
        if (release)
            release(vnode);
        else
            vnode->initialized = 0u;
    }
    return PROS_VFS_OK;
}

static int vfs_next_component(const char **cursor, char *component)
{
    const char *p = *cursor;
    uint32_t length = 0;
    while (*p == '/')
        ++p;
    if (*p == '\0') {
        *cursor = p;
        return 0;
    }
    while (*p != '\0' && *p != '/') {
        if (length >= PROS_VFS_NAME_MAX)
            return PROS_VFS_EOVERFLOW;
        component[length++] = *p++;
    }
    component[length] = '\0';
    *cursor = p;
    return 1;
}

int vfs_path_lookup(struct pros_vfs_mount *mount, const char *path,
                    struct pros_vfs_vnode **result)
{
    struct pros_vfs_vnode *current;
    const char *cursor;
    char component[PROS_VFS_NAME_MAX + 1u];
    uint32_t components = 0;

    if (!mount || !path || !result || !mount->active || !mount->root ||
        path[0] == '\0')
        return PROS_VFS_EINVAL;
    current = mount->root;
    if (vfs_vnode_get(current) != PROS_VFS_OK)
        return PROS_VFS_EINVAL;
    cursor = path;

    while (1) {
        int rc = vfs_next_component(&cursor, component);
        if (rc < 0) {
            vfs_vnode_put(current);
            return rc;
        }
        if (rc == 0)
            break;
        if (++components > PROS_VFS_PATH_MAX / 2u) {
            vfs_vnode_put(current);
            return PROS_VFS_ELOOP;
        }
        if (component[0] == '.' && component[1] == '\0')
            continue;
        if (component[0] == '.' && component[1] == '.' &&
            component[2] == '\0') {
            struct pros_vfs_vnode *parent =
                current->parent ? current->parent : current;
            if (parent != current && vfs_vnode_get(parent) != PROS_VFS_OK) {
                vfs_vnode_put(current);
                return PROS_VFS_EINVAL;
            }
            if (parent != current)
                vfs_vnode_put(current);
            current = parent;
            continue;
        }
        if (current->type != PROS_VFS_NODE_DIRECTORY ||
            !current->ops->lookup) {
            vfs_vnode_put(current);
            return PROS_VFS_ENOTDIR;
        }
        struct pros_vfs_vnode *next = 0;
        int lookup_rc = current->ops->lookup(current, component, &next);
        if (lookup_rc != PROS_VFS_OK || !next || !next->initialized ||
            next->mount != mount) {
            vfs_vnode_put(current);
            return lookup_rc == PROS_VFS_OK ? PROS_VFS_EINVAL : lookup_rc;
        }
        /* lookup() transfers one owned vnode reference to the caller. */
        vfs_vnode_put(current);
        current = next;
    }
    *result = current;
    return PROS_VFS_OK;
}

static int vfs_split_parent_path(const char *path, char *parent_path,
                                  char *name)
{
    uint32_t length = 0u;
    uint32_t last_slash = 0xFFFFFFFFu;
    uint32_t name_length;

    if (!path || !parent_path || !name || path[0] == '\0')
        return PROS_VFS_EINVAL;
    while (length < PROS_VFS_PATH_MAX && path[length] != '\0') {
        if (path[length] == '/')
            last_slash = length;
        ++length;
    }
    if (path[length] != '\0' || length == 0u ||
        last_slash == 0xFFFFFFFFu || last_slash + 1u >= length)
        return PROS_VFS_EINVAL;
    name_length = length - last_slash - 1u;
    if (name_length > PROS_VFS_NAME_MAX)
        return PROS_VFS_EOVERFLOW;
    for (uint32_t i = 0u; i < name_length; ++i)
        name[i] = path[last_slash + 1u + i];
    name[name_length] = '\0';
    if (last_slash == 0u) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        for (uint32_t i = 0u; i < last_slash; ++i)
            parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
    }
    return PROS_VFS_OK;
}

int vfs_path_unlink(struct pros_vfs_mount *mount, const char *path)
{
    struct pros_vfs_vnode *parent = 0;
    char parent_path[PROS_VFS_PATH_MAX + 1u];
    char name[PROS_VFS_NAME_MAX + 1u];
    int rc;

    if (!mount || !mount->active || !path)
        return PROS_VFS_EINVAL;
    rc = vfs_split_parent_path(path, parent_path, name);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_path_lookup(mount, parent_path, &parent);
    if (rc != PROS_VFS_OK)
        return rc;
    if (parent->type != PROS_VFS_NODE_DIRECTORY ||
        !parent->ops || !parent->ops->unlink) {
        vfs_vnode_put(parent);
        return PROS_VFS_ENOSYS;
    }
    rc = parent->ops->unlink(parent, name);
    vfs_vnode_put(parent);
    return rc;
}

int vfs_path_rename(struct pros_vfs_mount *mount, const char *old_path,
                    const char *new_path)
{
    struct pros_vfs_vnode *old_parent = 0;
    struct pros_vfs_vnode *new_parent = 0;
    char old_parent_path[PROS_VFS_PATH_MAX + 1u];
    char new_parent_path[PROS_VFS_PATH_MAX + 1u];
    char old_name[PROS_VFS_NAME_MAX + 1u];
    char new_name[PROS_VFS_NAME_MAX + 1u];
    int rc;

    if (!mount || !mount->active || !old_path || !new_path)
        return PROS_VFS_EINVAL;
    rc = vfs_split_parent_path(old_path, old_parent_path, old_name);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_split_parent_path(new_path, new_parent_path, new_name);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_path_lookup(mount, old_parent_path, &old_parent);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_path_lookup(mount, new_parent_path, &new_parent);
    if (rc != PROS_VFS_OK) {
        vfs_vnode_put(old_parent);
        return rc;
    }
    if (old_parent->type != PROS_VFS_NODE_DIRECTORY ||
        new_parent->type != PROS_VFS_NODE_DIRECTORY ||
        !old_parent->ops || !old_parent->ops->rename) {
        vfs_vnode_put(new_parent);
        vfs_vnode_put(old_parent);
        return PROS_VFS_ENOSYS;
    }
    rc = old_parent->ops->rename(old_parent, old_name, new_parent, new_name);
    vfs_vnode_put(new_parent);
    vfs_vnode_put(old_parent);
    return rc;
}

int vfs_file_open(struct pros_vfs_mount *mount, const char *path,
                  uint32_t flags, struct pros_vfs_file *file)
{
    struct pros_vfs_vnode *vnode = 0;
    int rc;
    if (!file || (flags & ~(PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                            PROS_VFS_O_DIRECTORY | PROS_VFS_O_CREATE)) != 0u ||
        (flags & (PROS_VFS_O_READ | PROS_VFS_O_WRITE)) == 0u ||
        (flags & PROS_VFS_O_DIRECTORY) != 0u)
        return PROS_VFS_EINVAL;

    rc = vfs_path_lookup(mount, path, &vnode);
    if (rc == PROS_VFS_ENOENT && (flags & PROS_VFS_O_CREATE) != 0u) {
        char parent_path[PROS_VFS_PATH_MAX + 1u];
        char name[PROS_VFS_NAME_MAX + 1u];
        uint32_t length = 0u, last_slash = 0xFFFFFFFFu;
        while (length < PROS_VFS_PATH_MAX && path[length] != '\0') {
            if (path[length] == '/') last_slash = length;
            ++length;
        }
        if (path[length] != '\0' || length == 0u ||
            last_slash == 0xFFFFFFFFu || last_slash + 1u >= length)
            return PROS_VFS_EINVAL;
        uint32_t name_length = length - last_slash - 1u;
        if (name_length > PROS_VFS_NAME_MAX)
            return PROS_VFS_EOVERFLOW;
        for (uint32_t i = 0u; i < name_length; ++i)
            name[i] = path[last_slash + 1u + i];
        name[name_length] = '\0';
        if (last_slash == 0u) {
            parent_path[0] = '/'; parent_path[1] = '\0';
        } else {
            for (uint32_t i = 0u; i < last_slash; ++i)
                parent_path[i] = path[i];
            parent_path[last_slash] = '\0';
        }
        rc = vfs_path_lookup(mount, parent_path, &vnode);
        if (rc != PROS_VFS_OK) return rc;
        if (vnode->type != PROS_VFS_NODE_DIRECTORY || !vnode->ops->create) {
            vfs_vnode_put(vnode); return PROS_VFS_ENOSYS;
        }
        struct pros_vfs_vnode *created = 0;
        rc = vnode->ops->create(vnode, name, &created);
        vfs_vnode_put(vnode);
        if (rc != PROS_VFS_OK) return rc;
        vnode = created;
    } else if (rc != PROS_VFS_OK) {
        return rc;
    }
    if (vnode->type != PROS_VFS_NODE_FILE) {
        vfs_vnode_put(vnode); return PROS_VFS_EISDIR;
    }
    if ((flags & PROS_VFS_O_WRITE) != 0u && mount->read_only) {
        vfs_vnode_put(vnode); return PROS_VFS_EPERM;
    }
    if (vfs_file_init(file, mount, vnode->file_ops, vnode->private_data,
                      flags) != PROS_VFS_OK) {
        vfs_vnode_put(vnode); return PROS_VFS_EINVAL;
    }
    file->vnode = vnode;
    return PROS_VFS_OK;
}

int vfs_path_mkdir(struct pros_vfs_mount *mount, const char *path,
                   struct pros_vfs_vnode **result)
{
    struct pros_vfs_vnode *parent = 0;
    struct pros_vfs_vnode *created = 0;
    char parent_path[PROS_VFS_PATH_MAX + 1u];
    char name[PROS_VFS_NAME_MAX + 1u];
    uint32_t length = 0u, last_slash = 0xFFFFFFFFu;
    uint32_t name_length;
    int rc;

    if (!mount || !path || !result || !mount->active || path[0] == '\0')
        return PROS_VFS_EINVAL;
    *result = 0;

    while (length < PROS_VFS_PATH_MAX && path[length] != '\0') {
        if (path[length] == '/')
            last_slash = length;
        ++length;
    }
    if (path[length] != '\0' || length == 0u ||
        last_slash == 0xFFFFFFFFu || last_slash + 1u >= length)
        return PROS_VFS_EINVAL;

    name_length = length - last_slash - 1u;
    if (name_length > PROS_VFS_NAME_MAX)
        return PROS_VFS_EOVERFLOW;
    for (uint32_t i = 0u; i < name_length; ++i)
        name[i] = path[last_slash + 1u + i];
    name[name_length] = '\0';

    if (last_slash == 0u) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        for (uint32_t i = 0u; i < last_slash; ++i)
            parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
    }

    rc = vfs_path_lookup(mount, parent_path, &parent);
    if (rc != PROS_VFS_OK)
        return rc;
    if (parent->type != PROS_VFS_NODE_DIRECTORY || !parent->ops->mkdir) {
        vfs_vnode_put(parent);
        return PROS_VFS_ENOSYS;
    }

    rc = parent->ops->mkdir(parent, name, &created);
    vfs_vnode_put(parent);
    if (rc != PROS_VFS_OK)
        return rc;
    if (!created || created->type != PROS_VFS_NODE_DIRECTORY) {
        if (created)
            vfs_vnode_put(created);
        return PROS_VFS_EIO;
    }
    *result = created;
    return PROS_VFS_OK;
}

int vfs_dir_open(struct pros_vfs_mount *mount, const char *path,
                 struct pros_vfs_dir *dir)
{
    struct pros_vfs_vnode *vnode;
    int rc;
    if (!dir)
        return PROS_VFS_EINVAL;
    rc = vfs_path_lookup(mount, path, &vnode);
    if (rc != PROS_VFS_OK)
        return rc;
    if (vnode->type != PROS_VFS_NODE_DIRECTORY) {
        vfs_vnode_put(vnode);
        return PROS_VFS_ENOTDIR;
    }
    if (!vnode->ops->readdir) {
        vfs_vnode_put(vnode);
        return PROS_VFS_ENOSYS;
    }
    dir->vnode = vnode;
    dir->cookie = 0;
    dir->open = 1u;
    return PROS_VFS_OK;
}

int vfs_dir_read(struct pros_vfs_dir *dir, struct pros_vfs_dirent *entry)
{
    if (!dir || !dir->open || !dir->vnode || !entry ||
        !dir->vnode->ops->readdir)
        return PROS_VFS_EINVAL;
    return dir->vnode->ops->readdir(dir->vnode, &dir->cookie, entry);
}

int vfs_dir_close(struct pros_vfs_dir *dir)
{
    if (!dir || !dir->open || !dir->vnode)
        return PROS_VFS_EINVAL;
    vfs_vnode_put(dir->vnode);
    dir->vnode = 0;
    dir->cookie = 0;
    dir->open = 0;
    return PROS_VFS_OK;
}

struct vfs_selftest_node {
    const char *name;
    enum pros_vfs_node_type type;
    const uint8_t *data;
    uint32_t size;
    struct pros_vfs_vnode *child0;
    struct pros_vfs_vnode *child1;
};

static int vfs_selftest_lookup(struct pros_vfs_vnode *parent, const char *name,
                               struct pros_vfs_vnode **result)
{
    struct vfs_selftest_node *dir =
        (struct vfs_selftest_node *)parent->private_data;
    struct pros_vfs_vnode *children[2];
    if (!dir || !name || !result)
        return PROS_VFS_EINVAL;
    children[0] = dir->child0;
    children[1] = dir->child1;
    for (uint32_t n = 0; n < 2u; ++n) {
        if (!children[n])
            continue;
        struct vfs_selftest_node *child =
            (struct vfs_selftest_node *)children[n]->private_data;
        uint32_t i = 0;
        while (child && child->name[i] && name[i] &&
               child->name[i] == name[i])
            ++i;
        if (child && child->name[i] == name[i]) {
            if (vfs_vnode_get(children[n]) != PROS_VFS_OK)
                return PROS_VFS_EINVAL;
            *result = children[n];
            return PROS_VFS_OK;
        }
    }
    return PROS_VFS_ENOENT;
}

static int vfs_selftest_readdir(struct pros_vfs_vnode *vnode,
                                uint64_t *cookie,
                                struct pros_vfs_dirent *entry)
{
    struct vfs_selftest_node *dir =
        (struct vfs_selftest_node *)vnode->private_data;
    struct pros_vfs_vnode *child = 0;
    if (!dir || !cookie || !entry)
        return PROS_VFS_EINVAL;
    if (*cookie == 0u)
        child = dir->child0;
    else if (*cookie == 1u)
        child = dir->child1;
    else
        return PROS_VFS_ENOENT;
    if (!child)
        return PROS_VFS_ENOENT;
    struct vfs_selftest_node *node =
        (struct vfs_selftest_node *)child->private_data;
    uint32_t i = 0;
    while (node->name[i] && i < PROS_VFS_NAME_MAX) {
        entry->name[i] = node->name[i];
        ++i;
    }
    entry->name[i] = '\0';
    entry->type = child->type;
    entry->size = child->size;
    ++(*cookie);
    return PROS_VFS_OK;
}

static int vfs_selftest_close_file(struct pros_vfs_file *file)
{
    (void)file;
    return PROS_VFS_OK;
}

static int vfs_selftest_read_file(struct pros_vfs_file *file, uint32_t offset,
                                  uint32_t length, void *buffer)
{
    struct vfs_selftest_node *node =
        (struct vfs_selftest_node *)file->private_data;
    if (!node || !buffer || offset > node->size)
        return PROS_VFS_EINVAL;
    uint32_t available = node->size - offset;
    uint32_t take = length < available ? length : available;
    for (uint32_t i = 0; i < take; ++i)
        ((uint8_t *)buffer)[i] = node->data[offset + i];
    return (int)take;
}

static const struct pros_vfs_vnode_ops vfs_selftest_vnode_ops = {
    .lookup = vfs_selftest_lookup,
    .readdir = vfs_selftest_readdir
};

static const struct pros_vfs_file_ops vfs_selftest_file_ops = {
    .read = vfs_selftest_read_file,
    .close = vfs_selftest_close_file
};

static int vfs_vnode_api_selftest(void)
{
    static const uint8_t hello[] = "hello-vfs";
    static const uint8_t config[] = "config";
    static struct pros_vfs_vnode root, hello_node, etc_node, config_node;
    static struct vfs_selftest_node root_data, hello_data, etc_data, config_data;
    struct pros_block_device device;
    struct pros_vfs_mount mount;
    struct pros_vfs_file file;
    struct pros_vfs_dir dir;
    struct pros_vfs_dirent entry;
    struct pros_vfs_vnode *found = 0;
    uint8_t output[16];
    uint32_t done;

    root_data = (struct vfs_selftest_node){
        "root", PROS_VFS_NODE_DIRECTORY, 0, 0, &hello_node, &etc_node
    };
    hello_data = (struct vfs_selftest_node){
        "hello.txt", PROS_VFS_NODE_FILE, hello, sizeof(hello) - 1u, 0, 0
    };
    etc_data = (struct vfs_selftest_node){
        "etc", PROS_VFS_NODE_DIRECTORY, 0, 0, &config_node, 0
    };
    config_data = (struct vfs_selftest_node){
        "config", PROS_VFS_NODE_FILE, config, sizeof(config) - 1u, 0, 0
    };

    if (block_device_init(&device, 512, 1, vfs_selftest_read, 0, 0) !=
            PROS_BLOCK_OK ||
        vfs_mount_init(&mount, PROS_FS_PROSENJIT, &device, 0, 1) !=
            PROS_VFS_OK)
        return -1;
    if (vfs_vnode_init(&root, &mount, &root, PROS_VFS_NODE_DIRECTORY,
                       &vfs_selftest_vnode_ops, 0, &root_data, 0) !=
            PROS_VFS_OK ||
        vfs_vnode_init(&hello_node, &mount, &root, PROS_VFS_NODE_FILE,
                       &vfs_selftest_vnode_ops, &vfs_selftest_file_ops,
                       &hello_data, sizeof(hello) - 1u) != PROS_VFS_OK ||
        vfs_vnode_init(&etc_node, &mount, &root, PROS_VFS_NODE_DIRECTORY,
                       &vfs_selftest_vnode_ops, 0, &etc_data, 0) !=
            PROS_VFS_OK ||
        vfs_vnode_init(&config_node, &mount, &etc_node, PROS_VFS_NODE_FILE,
                       &vfs_selftest_vnode_ops, &vfs_selftest_file_ops,
                       &config_data, sizeof(config) - 1u) != PROS_VFS_OK ||
        vfs_mount_set_root(&mount, &root) != PROS_VFS_OK ||
        vfs_mount_activate(&mount) != PROS_VFS_OK)
        return -1;

    if (vfs_path_lookup(&mount, "/etc/../hello.txt", &found) != PROS_VFS_OK ||
        found != &hello_node)
        return -1;
    vfs_vnode_put(found);

    if (vfs_file_open(&mount, "/hello.txt", PROS_VFS_O_READ, &file) !=
            PROS_VFS_OK ||
        vfs_file_read(&file, output, 5, &done) != PROS_VFS_OK ||
        done != 5u || output[0] != 'h' || output[4] != 'o' ||
        vfs_file_seek(&file, 6) != PROS_VFS_OK ||
        vfs_file_read(&file, output, 16, &done) != PROS_VFS_OK ||
        done != 3u || output[0] != 'v' ||
        vfs_file_close(&file) != PROS_VFS_OK)
        return -1;

    if (vfs_dir_open(&mount, "/etc", &dir) != PROS_VFS_OK ||
        vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
        entry.type != PROS_VFS_NODE_FILE ||
        entry.name[0] != 'c' ||
        vfs_dir_read(&dir, &entry) != PROS_VFS_ENOENT ||
        vfs_dir_close(&dir) != PROS_VFS_OK)
        return -1;

    if (vfs_file_open(&mount, "/etc", PROS_VFS_O_READ, &file) !=
            PROS_VFS_EISDIR ||
        vfs_dir_open(&mount, "/hello.txt", &dir) != PROS_VFS_ENOTDIR)
        return -1;

    if (vfs_mount_deactivate(&mount) != PROS_VFS_OK)
        return -1;
    return 0;
}

static uint8_t vfs_probe_test_sector[512];


static uint8_t vfs_probe_test_disk[3u * 512u];

static int vfs_probe_test_disk_read(struct pros_block_device *device,
                                    uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    if (!buffer || lba > 3u || count > 3u - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t i = 0u; i < count * 512u; ++i)
        ((uint8_t *)buffer)[i] = vfs_probe_test_disk[lba * 512u + i];
    return PROS_BLOCK_OK;
}

static int vfs_probe_selftest(void)
{
    struct pros_block_device device;
    struct pros_partition_device partition;
    struct pros_partition part;
    enum pros_filesystem_type type;

    if (block_device_init(&device, 512u, 3u, vfs_probe_test_disk_read, 0, 0) !=
        PROS_BLOCK_OK)
        return -1;

    for (uint32_t i = 0u; i < sizeof(vfs_probe_test_disk); ++i)
        vfs_probe_test_disk[i] = 0u;
    vfs_probe_test_disk[510] = 0x55u;
    vfs_probe_test_disk[511] = 0xAAu;
    vfs_probe_test_disk[446 + 4] = 0x0Bu;
    vfs_probe_test_disk[446 + 8] = 1u;
    vfs_probe_test_disk[446 + 12] = 2u;

    uint32_t count = 0u;
    struct pros_partition partitions[4];
    if (partition_probe(&device, &(enum pros_partition_type){PROS_PARTITION_NONE}) !=
        PROS_PARTITION_OK)
        return -2;
    if (partition_read_mbr(&device, partitions, 4u, &count) !=
            PROS_PARTITION_OK ||
        count != 1u || partitions[0].first_lba != 1u ||
        partitions[0].sector_count != 2u)
        return -3;
    part = partitions[0];
    if (partition_device_init(&partition, &device, &part) != PROS_PARTITION_OK)
        return -4;

    uint8_t *boot = vfs_probe_test_disk + 512u;
    boot[510] = 0x55u;
    boot[511] = 0xAAu;
    boot[11] = 0x00u;
    boot[12] = 0x02u;
    boot[13] = 0x01u;
    boot[82] = 'F';
    boot[83] = 'A';
    boot[84] = 'T';
    boot[85] = '3';
    boot[86] = '2';

    if (vfs_probe_filesystem(&partition.device, &type) != PROS_VFS_OK ||
        type != PROS_FS_FAT32)
        return -5;
    if (partition.device.sector_count != 2u)
        return -6;

    /* Keep the signature test on a standalone sector as well. */
    for (uint32_t i = 0u; i < sizeof(vfs_probe_test_sector); ++i)
        vfs_probe_test_sector[i] = 0u;
    vfs_probe_test_sector[510] = 0x55u;
    vfs_probe_test_sector[511] = 0xAAu;
    vfs_probe_test_sector[11] = 0x00u;
    vfs_probe_test_sector[12] = 0x02u;
    vfs_probe_test_sector[13] = 0x01u;
    const char ntfs_oem[8] = {'N','T','F','S',' ',' ',' ',' '};
    for (uint32_t i = 0u; i < 8u; ++i)
        vfs_probe_test_sector[3u + i] = (uint8_t)ntfs_oem[i];
    for (uint32_t i = 0u; i < sizeof(vfs_probe_test_sector); ++i)
        vfs_probe_test_disk[i] = vfs_probe_test_sector[i];
    if (vfs_probe_filesystem(&device, &type) != PROS_VFS_OK ||
        type != PROS_FS_NTFS)
        return -7;

    const char exfat_oem[8] = {'E','X','F','A','T',' ',' ',' '};
    for (uint32_t i = 0u; i < 8u; ++i)
        vfs_probe_test_sector[3u + i] = (uint8_t)exfat_oem[i];
    /* exFAT geometry lives in the shift fields, not BPB byte 13. */
    vfs_probe_test_sector[72] = 3u;
    vfs_probe_test_sector[108] = 9u;  /* 512-byte sectors */
    vfs_probe_test_sector[109] = 0u;  /* one sector per cluster */
    for (uint32_t i = 0u; i < sizeof(vfs_probe_test_sector); ++i)
        vfs_probe_test_disk[i] = vfs_probe_test_sector[i];
    if (vfs_probe_filesystem(&device, &type) != PROS_VFS_OK ||
        type != PROS_FS_EXFAT)
        return -8;

    for (uint32_t i = 0u; i < 8u; ++i)
        vfs_probe_test_sector[3u + i] = 0u;
    vfs_probe_test_sector[82] = 'F';
    vfs_probe_test_sector[83] = 'A';
    vfs_probe_test_sector[84] = 'T';
    vfs_probe_test_sector[85] = '3';
    vfs_probe_test_sector[86] = '2';
    for (uint32_t i = 0u; i < sizeof(vfs_probe_test_sector); ++i)
        vfs_probe_test_disk[i] = vfs_probe_test_sector[i];
    if (vfs_probe_filesystem(&device, &type) != PROS_VFS_OK ||
        type != PROS_FS_FAT32)
        return -9;

    vfs_probe_test_sector[11] = 0x00u;
    vfs_probe_test_sector[12] = 0x04u;
    for (uint32_t i = 0u; i < sizeof(vfs_probe_test_sector); ++i)
        vfs_probe_test_disk[i] = vfs_probe_test_sector[i];
    if (vfs_probe_filesystem(&device, &type) != PROS_VFS_ENOSYS ||
        type != PROS_FS_UNKNOWN)
        return -10;
    vfs_probe_test_sector[11] = 0x00u;
    vfs_probe_test_sector[12] = 0x02u;
    vfs_probe_test_sector[13] = 0u;
    for (uint32_t i = 0u; i < sizeof(vfs_probe_test_sector); ++i)
        vfs_probe_test_disk[i] = vfs_probe_test_sector[i];
    if (vfs_probe_filesystem(&device, &type) != PROS_VFS_ENOSYS ||
        type != PROS_FS_UNKNOWN)
        return -11;

    return 0;
}

int vfs_security_selftest(void)
{
    if (vfs_probe_selftest() != 0)
        return -40;
    if (vfs_security_admission_selftest() != 0)
        return -41;
    if (vfs_vnode_api_selftest() != 0)
        return -42;
    return 0;
}

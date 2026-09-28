#include "vfs.h"
#include "partition.h"

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
    mount->active = 0;
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
        file->position > 0xFFFFFFFFULL)
        return PROS_VFS_EINVAL;
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
        if (vfs_vnode_get(next) != PROS_VFS_OK) {
            vfs_vnode_put(current);
            return PROS_VFS_EINVAL;
        }
        vfs_vnode_put(current);
        current = next;
    }
    *result = current;
    return PROS_VFS_OK;
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

int vfs_security_selftest(void)
{
    if (vfs_security_admission_selftest() != 0)
        return -41;
    if (vfs_vnode_api_selftest() != 0)
        return -42;
    return 0;
}

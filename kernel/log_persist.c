#include "log_persist.h"

static struct pros_vfs_mount *persist_mount;
static struct pros_vfs_file persist_file;
static struct pros_log_persist_policy persist_policy;
static uint8_t initialized;
static uint8_t attached;
static uint32_t bytes_written;
static char persist_path[PROS_LOG_PERSIST_PATH_MAX];

static uint32_t bounded_strlen(const char *s, uint32_t max) {
    uint32_t n = 0u;
    if (!s) return 0u;
    while (n < max && s[n]) ++n;
    return n;
}

int pros_log_persist_init(void) {
    persist_mount = 0;
    persist_file = (struct pros_vfs_file){0};
    persist_policy = (struct pros_log_persist_policy){
        .max_bytes = PROS_LOG_PERSIST_MAX_BYTES,
        .rotate_bytes = PROS_LOG_PERSIST_ROTATE_BYTES,
        .max_files = 3u
    };
    initialized = 1u;
    attached = 0u;
    bytes_written = 0u;
    persist_path[0] = '\0';
    return PROS_LOG_PERSIST_OK;
}

int pros_log_persist_attach(struct pros_vfs_mount *mount,
                            const char *path,
                            const struct pros_log_persist_policy *policy) {
    uint32_t length;
    if (!initialized || !mount || !mount->active || !mount->root ||
        !path || !policy)
        return PROS_LOG_PERSIST_EINVAL;
    if (mount->read_only || policy->max_bytes == 0u ||
        policy->max_bytes > PROS_LOG_PERSIST_MAX_BYTES ||
        policy->rotate_bytes == 0u ||
        policy->rotate_bytes > policy->max_bytes ||
        policy->max_files == 0u || policy->max_files > 8u)
        return PROS_LOG_PERSIST_EINVAL;

    length = bounded_strlen(path, PROS_LOG_PERSIST_PATH_MAX);
    if (length == 0u || length >= PROS_LOG_PERSIST_PATH_MAX)
        return PROS_LOG_PERSIST_EINVAL;

    if (attached)
        (void)vfs_file_close(&persist_file);

    if (vfs_file_open(mount, path, PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                      &persist_file) != PROS_VFS_OK)
        return PROS_LOG_PERSIST_EIO;

    persist_mount = mount;
    persist_policy = *policy;
    attached = 1u;
    bytes_written = persist_file.vnode ? persist_file.vnode->size : 0u;
    for (uint32_t i = 0u; i <= length; ++i)
        persist_path[i] = path[i];
    return PROS_LOG_PERSIST_OK;
}

static uint32_t append_u64(char *buffer, uint32_t capacity, uint32_t pos, uint64_t value) {
    char tmp[20];
    uint32_t n = 0u;
    if (value == 0u) {
        if (pos >= capacity) return UINT32_MAX;
        buffer[pos++] = '0';
        return pos;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    if (pos + n > capacity) return UINT32_MAX;
    while (n) buffer[pos++] = tmp[--n];
    return pos;
}

static uint32_t append_u32(char *buffer, uint32_t capacity, uint32_t pos, uint32_t value) {
    return append_u64(buffer, capacity, pos, value);
}


static int persist_path_with_suffix(const char *base, uint32_t suffix,
                                    char *out, uint32_t capacity) {
    uint32_t length = bounded_strlen(base, PROS_LOG_PERSIST_PATH_MAX);
    char digits[10];
    uint32_t n = 0u, pos = 0u;

    if (!base || !out || capacity == 0u || length == 0u ||
        length >= PROS_LOG_PERSIST_PATH_MAX || suffix == 0u)
        return PROS_LOG_PERSIST_EINVAL;
    while (suffix && n < sizeof(digits)) {
        digits[n++] = (char)('0' + (suffix % 10u));
        suffix /= 10u;
    }
    if (suffix != 0u || length + n + 3u > capacity)
        return PROS_LOG_PERSIST_EINVAL;
    while (pos < length) { out[pos] = base[pos]; ++pos; }
    out[pos++] = '.';
    while (n) out[pos++] = digits[--n];
    out[pos] = '\0';
    return PROS_LOG_PERSIST_OK;
}

static int persist_path_exists(struct pros_vfs_mount *mount, const char *path) {
    struct pros_vfs_file file;
    int rc;
    if (!mount || !path) return PROS_LOG_PERSIST_EINVAL;
    rc = vfs_file_open(mount, path, PROS_VFS_O_READ, &file);
    if (rc == PROS_VFS_OK) {
        (void)vfs_file_close(&file);
        return 1;
    }
    if (rc == PROS_VFS_ENOENT) return 0;
    return PROS_LOG_PERSIST_EIO;
}

static int persist_open_current(const char *path) {
    int rc;
    if (!path || bounded_strlen(path, PROS_LOG_PERSIST_PATH_MAX) == 0u)
        return PROS_LOG_PERSIST_EINVAL;
    rc = vfs_file_open(persist_mount, path,
                       PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                       &persist_file);
    if (rc != PROS_VFS_OK) return PROS_LOG_PERSIST_EIO;
    attached = 1u;
    bytes_written = persist_file.vnode ? persist_file.vnode->size : 0u;
    return PROS_LOG_PERSIST_OK;
}

static int persist_rotate(const char *path) {
    char src[PROS_LOG_PERSIST_PATH_MAX];
    char dst[PROS_LOG_PERSIST_PATH_MAX];
    uint32_t index;
    int exists, rc;

    if (!attached || !persist_mount || !path)
        return PROS_LOG_PERSIST_EINVAL;
    if (vfs_file_close(&persist_file) != PROS_VFS_OK)
        return PROS_LOG_PERSIST_EIO;
    attached = 0u;

    if (persist_policy.max_files <= 1u) {
        rc = vfs_unlink(persist_mount, path);
        if (rc != PROS_VFS_OK && rc != PROS_VFS_ENOENT)
            return PROS_LOG_PERSIST_EIO;
        return persist_open_current(path);
    }

    if (persist_path_with_suffix(path, persist_policy.max_files - 1u,
                                 src, sizeof(src)) != PROS_LOG_PERSIST_OK)
        return PROS_LOG_PERSIST_EINVAL;
    exists = persist_path_exists(persist_mount, src);
    if (exists < 0) return PROS_LOG_PERSIST_EIO;
    if (exists > 0 && vfs_unlink(persist_mount, src) != PROS_VFS_OK)
        return PROS_LOG_PERSIST_EIO;

    for (index = persist_policy.max_files - 2u; index >= 1u; --index) {
        if (persist_path_with_suffix(path, index, src, sizeof(src)) !=
                PROS_LOG_PERSIST_OK ||
            persist_path_with_suffix(path, index + 1u, dst, sizeof(dst)) !=
                PROS_LOG_PERSIST_OK)
            return PROS_LOG_PERSIST_EINVAL;
        exists = persist_path_exists(persist_mount, src);
        if (exists < 0) return PROS_LOG_PERSIST_EIO;
        if (exists == 0) continue;
        if (vfs_rename(persist_mount, src, dst) != PROS_VFS_OK)
            return PROS_LOG_PERSIST_EIO;
        if (index == 1u) break;
    }

    if (persist_path_with_suffix(path, 1u, dst, sizeof(dst)) !=
        PROS_LOG_PERSIST_OK)
        return PROS_LOG_PERSIST_EINVAL;
    if (vfs_rename(persist_mount, path, dst) != PROS_VFS_OK)
        return PROS_LOG_PERSIST_EIO;

    return persist_open_current(path);
}

int pros_log_persist_emit(const struct pros_health_event *event) {
    char line[96];
    uint32_t pos = 0u, next, written = 0u;

    if (!initialized || !event)
        return PROS_LOG_PERSIST_EINVAL;
    if (!attached)
        return PROS_LOG_PERSIST_ENOTSUP;

    next = append_u64(line, sizeof(line), pos, event->sequence);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u64(line, sizeof(line), pos, event->timestamp);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u32(line, sizeof(line), pos, event->severity);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u32(line, sizeof(line), pos, event->category);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u32(line, sizeof(line), pos, event->source);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u32(line, sizeof(line), pos, event->code);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u64(line, sizeof(line), pos, event->value);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = '\n';

    if (bytes_written + pos > persist_policy.max_bytes)
        return PROS_LOG_PERSIST_ENOTSUP;
    if (bytes_written != 0u &&
        bytes_written + pos > persist_policy.rotate_bytes) {
        int rotate_rc = persist_rotate(persist_path);
        if (rotate_rc != PROS_LOG_PERSIST_OK)
            return rotate_rc;
    }
    if (vfs_file_seek(&persist_file, bytes_written) != PROS_VFS_OK ||
        vfs_file_write(&persist_file, line, pos, &written) != PROS_VFS_OK ||
        written != pos)
        return PROS_LOG_PERSIST_EIO;

    bytes_written += written;
    return PROS_LOG_PERSIST_OK;
}

int pros_log_persist_flush(void) {
    if (!initialized)
        return PROS_LOG_PERSIST_EINVAL;
    if (!attached)
        return PROS_LOG_PERSIST_ENOTSUP;
    return PROS_LOG_PERSIST_OK;
}

int pros_log_persist_detach(void) {
    if (!initialized)
        return PROS_LOG_PERSIST_EINVAL;
    if (attached) {
        if (vfs_file_close(&persist_file) != PROS_VFS_OK)
            return PROS_LOG_PERSIST_EIO;
    }
    persist_mount = 0;
    persist_file = (struct pros_vfs_file){0};
    attached = 0u;
    bytes_written = 0u;
    persist_path[0] = '\0';
    return PROS_LOG_PERSIST_OK;
}

int pros_log_persist_available(void) {
    return initialized && attached;
}

int pros_log_persist_selftest(void) {
    char rotated[PROS_LOG_PERSIST_PATH_MAX];
    if (pros_log_persist_init() != PROS_LOG_PERSIST_OK)
        return PROS_LOG_PERSIST_EIO;
    if (pros_log_persist_available())
        return PROS_LOG_PERSIST_EIO;
    if (persist_path_with_suffix("/var/log/prosenjitos.log", 1u,
                                 rotated, sizeof(rotated)) != PROS_LOG_PERSIST_OK)
        return PROS_LOG_PERSIST_EIO;
    if (bounded_strlen(rotated, sizeof(rotated)) == 0u)
        return PROS_LOG_PERSIST_EIO;
    return PROS_LOG_PERSIST_OK;
}

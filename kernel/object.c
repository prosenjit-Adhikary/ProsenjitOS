#include "object.h"

void object_init(struct pros_object *object, uint32_t type) {
    if (!object) return;
    object->type = type;
    object->refcount = 1;
}

int object_retain(struct pros_object *object) {
    if (!object || object->refcount == 0 || object->refcount == UINT32_MAX) return -1;
    object->refcount++;
    return 0;
}

int object_release(struct pros_object *object) {
    if (!object || object->refcount == 0) return -1;
    object->refcount--;
    return 0;
}

void handle_table_init(struct pros_handle_table *table) {
    if (!table) return;
    for (uint32_t i = 0; i < PROSENJIT_HANDLE_MAX; ++i) {
        table->entries[i].object = 0;
        table->entries[i].rights = 0;
    }
}

uint64_t handle_alloc(struct pros_handle_table *table,
                      struct pros_object *object,
                      uint32_t rights) {
    if (!table || !object || object->refcount == 0) return PROSENJIT_HANDLE_INVALID;
    for (uint32_t i = 1; i < PROSENJIT_HANDLE_MAX; ++i) {
        if (!table->entries[i].object) {
            if (object_retain(object) != 0) return PROSENJIT_HANDLE_INVALID;
            table->entries[i].object = object;
            table->entries[i].rights = rights;
            return i;
        }
    }
    return PROSENJIT_HANDLE_INVALID;
}

int handle_check(const struct pros_handle_table *table,
                 uint64_t handle,
                 uint32_t required_rights,
                 uint32_t expected_type) {
    if (!table || handle == PROSENJIT_HANDLE_INVALID || handle >= PROSENJIT_HANDLE_MAX)
        return -1;
    const struct pros_handle_entry *entry = &table->entries[handle];
    if (!entry->object || entry->object->refcount == 0) return -1;
    if (expected_type && entry->object->type != expected_type) return -1;
    if ((entry->rights & required_rights) != required_rights) return -1;
    return 0;
}

int handle_close(struct pros_handle_table *table, uint64_t handle) {
    if (!table || handle == PROSENJIT_HANDLE_INVALID || handle >= PROSENJIT_HANDLE_MAX)
        return -1;
    struct pros_handle_entry *entry = &table->entries[handle];
    if (!entry->object) return -1;
    struct pros_object *object = entry->object;
    entry->object = 0;
    entry->rights = 0;
    return object_release(object);
}

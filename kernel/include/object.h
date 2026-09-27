#ifndef PROSENJIT_OBJECT_H
#define PROSENJIT_OBJECT_H

#include <stdint.h>

#define PROSENJIT_HANDLE_INVALID 0ULL
#define PROSENJIT_HANDLE_MAX 256u

enum pros_object_type {
    PROS_OBJECT_NONE = 0,
    PROS_OBJECT_PROCESS = 1,
    PROS_OBJECT_THREAD = 2,
    PROS_OBJECT_ADDRESS_SPACE = 3,
};

enum pros_object_rights {
    PROS_RIGHT_NONE  = 0,
    PROS_RIGHT_READ  = 1u << 0,
    PROS_RIGHT_WRITE = 1u << 1,
    PROS_RIGHT_EXEC  = 1u << 2,
    PROS_RIGHT_DUP   = 1u << 3,
    PROS_RIGHT_ADMIN = 1u << 4,
};

struct pros_object {
    uint32_t type;
    uint32_t refcount;
};

struct pros_handle_entry {
    struct pros_object *object;
    uint32_t rights;
};

struct pros_handle_table {
    struct pros_handle_entry entries[PROSENJIT_HANDLE_MAX];
};

void object_init(struct pros_object *object, uint32_t type);
int object_retain(struct pros_object *object);
int object_release(struct pros_object *object);

void handle_table_init(struct pros_handle_table *table);
uint64_t handle_alloc(struct pros_handle_table *table,
                      struct pros_object *object,
                      uint32_t rights);
int handle_check(const struct pros_handle_table *table,
                 uint64_t handle,
                 uint32_t required_rights,
                 uint32_t expected_type);
int handle_close(struct pros_handle_table *table, uint64_t handle);

#endif

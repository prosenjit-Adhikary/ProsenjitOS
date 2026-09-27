#ifndef PROSENJIT_OBJECT_H
#define PROSENJIT_OBJECT_H

#include <stdint.h>
#include "../../include/abi/capability.h"

#define PROSENJIT_HANDLE_INVALID 0ULL
#define PROSENJIT_HANDLE_MAX 256u

#define PROS_OBJECT_NONE PROSENJIT_OBJECT_NONE
#define PROS_OBJECT_PROCESS PROSENJIT_OBJECT_PROCESS
#define PROS_OBJECT_THREAD PROSENJIT_OBJECT_THREAD
#define PROS_OBJECT_ADDRESS_SPACE PROSENJIT_OBJECT_MEMORY
#define PROS_OBJECT_FILE PROSENJIT_OBJECT_FILE
#define PROS_OBJECT_DEVICE PROSENJIT_OBJECT_DEVICE
#define PROS_OBJECT_SOCKET PROSENJIT_OBJECT_SOCKET
#define PROS_OBJECT_IPC PROSENJIT_OBJECT_IPC

struct pros_object {
    uint16_t type;
    uint16_t reserved;
    uint32_t refcount;
};

struct pros_handle_entry {
    struct pros_object *object;
    uint16_t generation;
    uint16_t reserved;
    uint64_t rights;
};

struct pros_handle_table {
    struct pros_handle_entry entries[PROSENJIT_HANDLE_MAX];
};

void object_init(struct pros_object *object, uint16_t type);
int object_retain(struct pros_object *object);
int object_release(struct pros_object *object);

void handle_table_init(struct pros_handle_table *table);
uint64_t handle_alloc(struct pros_handle_table *table,
                      struct pros_object *object,
                      uint64_t rights);
int handle_check(const struct pros_handle_table *table,
                 uint64_t handle,
                 uint64_t required_rights,
                 uint16_t expected_type);
int handle_close(struct pros_handle_table *table, uint64_t handle);
uint64_t handle_duplicate(struct pros_handle_table *table,
                          uint64_t source_handle,
                          uint64_t requested_rights);
uint64_t handle_transfer_copy(struct pros_handle_table *source_table,
                              struct pros_handle_table *target_table,
                              uint64_t source_handle,
                              uint64_t requested_rights);
uint64_t handle_transfer_move(struct pros_handle_table *source_table,
                              struct pros_handle_table *target_table,
                              uint64_t source_handle,
                              uint64_t requested_rights);

#endif

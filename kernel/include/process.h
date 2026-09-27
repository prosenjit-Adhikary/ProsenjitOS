#ifndef PROSENJIT_PROCESS_H
#define PROSENJIT_PROCESS_H

#include <stdint.h>
#include "object.h"
#include "address_space.h"

enum pros_process_state {
    PROS_PROCESS_NEW = 0,
    PROS_PROCESS_READY = 1,
    PROS_PROCESS_RUNNING = 2,
    PROS_PROCESS_BLOCKED = 3,
    PROS_PROCESS_ZOMBIE = 4,
    PROS_PROCESS_DEAD = 5,
};

enum pros_security_level {
    PROS_SECURITY_LIMITED = 1,
    PROS_SECURITY_SUPER = 2,
};

struct pros_process {
    struct pros_object object;
    uint64_t pid;
    uint64_t parent_pid;
    uint32_t security_level;
    uint32_t state;
    int32_t exit_code;
    uint64_t capability_policy;
    struct pros_address_space address_space;
    struct pros_handle_table handles;
    struct pros_process *parent;
    struct pros_process *next;
};

int process_init(struct pros_process *process,
                 uint64_t pid,
                 uint32_t security_level);
int process_manager_init(void);
struct pros_process *process_lookup(uint64_t pid);
int process_register(struct pros_process *process);
int process_unregister(struct pros_process *process);
int process_exit(struct pros_process *process, int32_t exit_code);
int process_wait(struct pros_process *parent, uint64_t pid, int32_t *exit_code);

int process_create(struct pros_process *parent,
                   struct pros_process *child,
                   uint64_t pid,
                   uint32_t security_level);
int process_set_state(struct pros_process *process, uint32_t state);
uint32_t process_state(const struct pros_process *process);
uint64_t process_allowed_capabilities(const struct pros_process *process);

int process_grant_handle(struct pros_process *process,
                         struct pros_object *object,
                         uint64_t rights,
                         uint64_t *handle_out);
int process_check_handle(const struct pros_process *process,
                         uint64_t handle,
                         uint64_t required_rights,
                         uint16_t expected_type);
int process_close_handle(struct pros_process *process, uint64_t handle);
uint64_t process_duplicate_handle(struct pros_process *process,
                                  uint64_t source_handle,
                                  uint64_t requested_rights);
uint64_t process_transfer_handle_copy(struct pros_process *source,
                                      struct pros_process *target,
                                      uint64_t source_handle,
                                      uint64_t requested_rights);
uint64_t process_transfer_handle_move(struct pros_process *source,
                                      struct pros_process *target,
                                      uint64_t source_handle,
                                      uint64_t requested_rights);
void process_set_current(struct pros_process *process);
struct pros_process *process_current(void);

#endif

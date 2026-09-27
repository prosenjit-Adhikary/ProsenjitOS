#ifndef PROSENJIT_PROCESS_H
#define PROSENJIT_PROCESS_H

#include <stdint.h>
#include "object.h"
#include "address_space.h"

enum pros_security_level {
    PROS_SECURITY_LIMITED = 1,
    PROS_SECURITY_SUPER = 2,
};

struct pros_process {
    struct pros_object object;
    uint64_t pid;
    uint32_t security_level;
    struct pros_address_space address_space;
    struct pros_handle_table handles;
};

int process_init(struct pros_process *process,
                 uint64_t pid,
                 uint32_t security_level);
int process_grant_handle(struct pros_process *process,
                         struct pros_object *object,
                         uint64_t rights,
                         uint64_t *handle_out);
int process_check_handle(const struct pros_process *process,
                         uint64_t handle,
                         uint64_t required_rights,
                         uint16_t expected_type);
int process_close_handle(struct pros_process *process, uint64_t handle);
void process_set_current(struct pros_process *process);
struct pros_process *process_current(void);

#endif

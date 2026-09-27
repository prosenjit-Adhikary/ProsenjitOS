#include "process.h"

static struct pros_process *current_process;

int process_init(struct pros_process *process,
                 uint64_t pid,
                 uint32_t security_level) {
    if (!process) return -1;
    if (security_level != PROS_SECURITY_LIMITED &&
        security_level != PROS_SECURITY_SUPER) return -1;
    if (address_space_create(&process->address_space) != 0) return -1;

    object_init(&process->object, PROS_OBJECT_PROCESS);
    process->pid = pid;
    process->security_level = security_level;
    handle_table_init(&process->handles);
    return 0;
}

int process_grant_handle(struct pros_process *process,
                         struct pros_object *object,
                         uint64_t rights,
                         uint64_t *handle_out) {
    if (!process || !object || !handle_out) return -1;
    uint64_t handle = handle_alloc(&process->handles, object, rights);
    if (handle == PROSENJIT_HANDLE_INVALID) return -1;
    *handle_out = handle;
    return 0;
}


int process_check_handle(const struct pros_process *process,
                         uint64_t handle,
                         uint64_t required_rights,
                         uint16_t expected_type) {
    if (!process) return -1;
    return handle_check(&process->handles, handle, required_rights,
                        expected_type);
}

int process_close_handle(struct pros_process *process, uint64_t handle) {
    if (!process) return -1;
    return handle_close(&process->handles, handle);
}

void process_set_current(struct pros_process *process) {
    current_process = process;
}

struct pros_process *process_current(void) {
    return current_process;
}

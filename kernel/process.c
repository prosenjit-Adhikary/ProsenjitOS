#include "process.h"

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
                         uint32_t rights,
                         uint64_t *handle_out) {
    if (!process || !object || !handle_out) return -1;
    uint64_t handle = handle_alloc(&process->handles, object, rights);
    if (handle == PROSENJIT_HANDLE_INVALID) return -1;
    *handle_out = handle;
    return 0;
}

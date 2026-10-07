#include "process.h"
#include "../../include/abi/syscall.h"
#include "paging.h"
#include "thread.h"
#include "security_monitor.h"

extern int paging_switch_kernel(void);

static struct pros_process *current_process;
static int process_is_live(const struct pros_process *process);
static int process_is_active(const struct pros_process *process);
static int process_release_resources(struct pros_process *process);
static int process_has_children(const struct pros_process *process);
static struct pros_process *process_list;
static uint64_t next_pid = 1;

int process_manager_init(void) {
    process_list = 0;
    current_process = 0;
    next_pid = 1;
    return 0;
}

struct pros_process *process_lookup(uint64_t pid) {
    for (struct pros_process *p = process_list; p; p = p->next)
        if (p->pid == pid && p->state != PROS_PROCESS_DEAD)
            return p;
    return 0;
}

int process_register(struct pros_process *process) {
    if (!process_is_live(process) || process->pid == 0 ||
        process_lookup(process->pid) != 0) return -1;
    process->next = process_list;
    process_list = process;
    if (process->pid >= next_pid) {
        if (process->pid == UINT64_MAX)
            next_pid = UINT64_MAX;
        else
            next_pid = process->pid + 1;
    }
    return 0;
}

int process_unregister(struct pros_process *process) {
    if (!process || process->state != PROS_PROCESS_DEAD ||
        process->address_space.root != 0 || process->object.refcount != 1 ||
        thread_process_has_live(process) || thread_process_is_current(process))
        return -1;
    struct pros_process **link = &process_list;
    while (*link) {
        if (*link == process) {
            *link = process->next;
            process->next = 0;
            if (current_process == process) {
                current_process = 0;
                (void)paging_switch_kernel();
            }

            /*
             * Security telemetry is lifetime-scoped. At final reap, discard
             * the PID's monitor state before the process object disappears,
             * so no historical score/action can leak into a later lifetime.
             */
            (void)pros_security_monitor_clear(process->pid);

            if (object_release(&process->object) != 0)
                return -1;
            return 0;
        }
        link = &(*link)->next;
    }
    return -1;
}


static int process_is_live(const struct pros_process *process) {
    return process && process->object.type == PROS_OBJECT_PROCESS &&
           process->object.refcount != 0;
}

static int process_is_active(const struct pros_process *process) {
    return process_is_live(process) &&
           process->state != PROS_PROCESS_ZOMBIE &&
           process->state != PROS_PROCESS_DEAD;
}

static int process_has_children(const struct pros_process *process) {
    if (!process) return 0;
    for (struct pros_process *child = process_list; child; child = child->next)
        if (child->parent == process && child->state != PROS_PROCESS_DEAD)
            return 1;
    return 0;
}

static int process_release_resources(struct pros_process *process) {
    if (!process) return -1;
    if (handle_table_close_all(&process->handles) != 0) return -1;
    return address_space_destroy(&process->address_space);
}

int process_init(struct pros_process *process,
                 uint64_t pid,
                 uint32_t security_level) {
    if (!process) return -1;
    if (security_level != PROS_SECURITY_LIMITED &&
        security_level != PROS_SECURITY_SUPER) return -1;
    if (address_space_create(&process->address_space) != 0) return -1;

    object_init(&process->object, PROS_OBJECT_PROCESS);
    process->pid = pid;
    process->parent_pid = 0;
    process->security_level = security_level;
    process->state = PROS_PROCESS_NEW;
    process->exit_code = 0;
    process->quarantined = 0;
    process->quarantine_previous_state = 0;
    process->user_entry = 0;
    process->user_stack_top = 0;
    process->parent = 0;
    process->next = 0;
    process->capability_policy = (security_level == PROS_SECURITY_SUPER)
        ? PROSENJIT_CAP_RIGHTS_ALL
        : (PROSENJIT_CAP_RIGHTS_ALL & ~PROSENJIT_CAP_ADMIN);
    process->syscall_policy = PROSENJIT_SYSCALL_POLICY_ALL;
    handle_table_init(&process->handles);
    return 0;
}

int process_create(struct pros_process *parent,
                   struct pros_process *child,
                   uint64_t pid,
                   uint32_t security_level) {
    if (!child || pid == 0) return -1;
    if (parent && !process_is_active(parent)) return -1;
    if (process_lookup(pid) != 0) return -1;
    if (process_init(child, pid, security_level) != 0) return -1;

    child->parent_pid = parent ? parent->pid : 0;
    child->parent = parent;
    child->state = PROS_PROCESS_READY;
    if (process_register(child) != 0) {
        (void)process_release_resources(child);
        return -1;
    }

    /*
     * Security-monitor state is keyed by PID. A PID may be reused only
     * after the previous process has been fully reaped, so discard any
     * historical monitor score before the new lifetime becomes observable.
     * This prevents inherited quarantine/block scores across PID reuse.
     */
    (void)pros_security_monitor_clear(child->pid);
    return 0;
}

int process_exit(struct pros_process *process, int32_t exit_code) {
    if (!process_is_live(process) ||
        process->state == PROS_PROCESS_DEAD ||
        process->state == PROS_PROCESS_ZOMBIE ||
        process_has_children(process))
        return -1;

    if (current_process == process) {
        /*
         * Return to the kernel address space before terminating the current
         * process. A failed CR3 transition leaves both process and thread
         * state untouched, so the exit remains retryable.
         */
        if (paging_switch_kernel() != 0)
            return -1;
        current_process = 0;
    }

    if (thread_terminate_process(process) != 0)
        return -1;

    /* Publish the exit result only after the irreversible thread transition
     * has succeeded, keeping failed exits observationally non-destructive. */
    process->exit_code = exit_code;
    process->quarantined = 0u;
    process->quarantine_previous_state = 0u;
    process->state = PROS_PROCESS_ZOMBIE;
    return 0;
}
int process_wait(struct pros_process *parent, uint64_t pid, int32_t *exit_code) {
    if (!process_is_active(parent) || !exit_code || pid == 0) return -1;
    struct pros_process *child = process_lookup(pid);
    if (!child || child->parent != parent) return -1;
    if (child->state != PROS_PROCESS_ZOMBIE) return -2;
    if (thread_process_has_live(child) || thread_process_is_current(child)) return -3;
    *exit_code = child->exit_code;
    if (process_release_resources(child) != 0) return -4;
    child->state = PROS_PROCESS_DEAD;
    if (process_unregister(child) != 0) {
        /* Keep a fully reaped process visible if final unlinking fails. */
        child->state = PROS_PROCESS_ZOMBIE;
        return -5;
    }
    return 0;
}

int process_set_state(struct pros_process *process, uint32_t state) {
    if (!process_is_live(process) || state > PROS_PROCESS_DEAD) return -1;
    if (process->state == PROS_PROCESS_DEAD &&
        state != PROS_PROCESS_DEAD) return -1;
    process->state = state;
    return 0;
}

uint32_t process_state(const struct pros_process *process) {
    return process_is_live(process) ? process->state : PROS_PROCESS_DEAD;
}

uint64_t process_allowed_capabilities(const struct pros_process *process) {
    return process_is_live(process) ? process->capability_policy : 0;
}

uint64_t process_allowed_syscalls(const struct pros_process *process) {
    return process_is_live(process) ? process->syscall_policy : 0;
}

int process_set_syscall_policy(struct pros_process *process, uint64_t policy) {
    if (!process_is_active(process) ||
        (policy & ~PROSENJIT_SYSCALL_POLICY_ALL) != 0)
        return -1;
    /* Only a Super process may tighten its own policy at this layer. */
    if (process->security_level != PROS_SECURITY_SUPER)
        return -1;
    process->syscall_policy = policy;
    return 0;
}

int process_apply_syscall_policy(struct pros_process *actor,
                                 struct pros_process *target,
                                 uint64_t policy) {
    if (!process_is_active(actor) || !process_is_active(target) ||
        (policy & ~PROSENJIT_SYSCALL_POLICY_ALL) != 0 ||
        actor->security_level != PROS_SECURITY_SUPER)
        return -1;
    target->syscall_policy = policy;
    return 0;
}

int process_grant_handle(struct pros_process *process,
                         struct pros_object *object,
                         uint64_t rights,
                         uint64_t *handle_out) {
    if (!process_is_active(process) || !object || !handle_out || rights == 0 ||
        (rights & ~process->capability_policy) != 0) return -1;
    uint64_t handle = handle_alloc(&process->handles, object, rights);
    if (handle == PROSENJIT_HANDLE_INVALID) return -1;
    *handle_out = handle;
    return 0;
}


int process_check_handle(const struct pros_process *process,
                         uint64_t handle,
                         uint64_t required_rights,
                         uint16_t expected_type) {
    if (!process_is_active(process) ||
        (required_rights & ~process->capability_policy) != 0) return -1;
    return handle_check(&process->handles, handle, required_rights,
                        expected_type);
}

int process_handle_type(const struct pros_process *process,
                        uint64_t handle,
                        uint16_t *type_out) {
    if (!process_is_active(process) || !type_out || handle == PROSENJIT_HANDLE_INVALID)
        return -1;

    uint32_t slot = (uint32_t)(handle & 0xFFULL);
    uint16_t generation = (uint16_t)(handle >> 8);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return -1;

    const struct pros_handle_entry *entry = &process->handles.entries[slot];
    if (!entry->object || entry->generation != generation ||
        entry->object->refcount == 0)
        return -1;

    *type_out = entry->object->type;
    return 0;
}

int process_close_handle(struct pros_process *process, uint64_t handle) {
    if (!process_is_active(process)) return -1;
    return handle_close(&process->handles, handle);
}

uint64_t process_duplicate_handle(struct pros_process *process,
                                  uint64_t source_handle,
                                  uint64_t requested_rights) {
    if (!process_is_active(process) || requested_rights == 0 ||
        (requested_rights & ~process->capability_policy) != 0)
        return PROSENJIT_HANDLE_INVALID;
    return handle_duplicate(&process->handles, source_handle, requested_rights);
}

uint64_t process_transfer_handle_copy(struct pros_process *source,
                                      struct pros_process *target,
                                      uint64_t source_handle,
                                      uint64_t requested_rights) {
    if (!process_is_active(source) || !process_is_active(target) ||
        source == target || requested_rights == 0 ||
        (requested_rights & ~target->capability_policy) != 0)
        return PROSENJIT_HANDLE_INVALID;

    /*
     * Transfer is an explicit authority: the source handle must carry
     * TRANSFER, while the new handle may only retain rights already held
     * by that source handle. The source handle itself remains valid.
     */
    if (handle_check(&source->handles, source_handle,
                     PROSENJIT_CAP_TRANSFER, PROS_OBJECT_NONE) != 0)
        return PROSENJIT_HANDLE_INVALID;

    uint32_t slot = (uint32_t)(source_handle & 0xFFULL);
    uint16_t generation = (uint16_t)(source_handle >> 8);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return PROSENJIT_HANDLE_INVALID;

    struct pros_handle_entry *entry = &source->handles.entries[slot];
    if (!entry->object || entry->generation != generation ||
        (requested_rights & ~entry->rights) != 0)
        return PROSENJIT_HANDLE_INVALID;

    return handle_alloc(&target->handles, entry->object, requested_rights);
}


uint64_t process_transfer_handle_move(struct pros_process *source,
                                      struct pros_process *target,
                                      uint64_t source_handle,
                                      uint64_t requested_rights) {
    if (!process_is_active(source) || !process_is_active(target) ||
        source == target || requested_rights == 0 ||
        (requested_rights & ~target->capability_policy) != 0)
        return PROSENJIT_HANDLE_INVALID;

    /*
     * The object layer performs the source capability check, attenuates
     * rights, allocates the target handle first, and consumes the source
     * only after the target handle exists.
     */
    return handle_transfer_move(&source->handles, &target->handles,
                                source_handle, requested_rights);
}

void process_set_current(struct pros_process *process) {
    struct pros_process *next = process_is_active(process) ? process : 0;
    if (next == current_process) return;
    if (next) {
        if (address_space_switch(&next->address_space) != 0) return;
        current_process = next;
    } else {
        if (paging_switch_kernel() != 0) return;
        current_process = 0;
    }
}

struct pros_process *process_current(void) {
    return current_process;
}

    if (process_create(&parent, &child, 1002u, PROS_SECURITY_LIMITED) == 0)
        return -6;

    /* Security quarantine metadata must be cleared when a process exits. */
    child.quarantined = 1u;
    child.quarantine_previous_state = (uint8_t)PROS_PROCESS_READY + 1u;
    if (process_exit(&parent, 7) == 0)
        return -7;
    if (parent.quarantined || parent.quarantine_previous_state != 0u)
        return -8;

    /* Re-test child exit after explicitly seeding quarantine metadata. */
    child.state = PROS_PROCESS_READY;
    child.quarantined = 1u;
    child.quarantine_previous_state = (uint8_t)PROS_PROCESS_READY + 1u;
    if (process_exit(&child, 42) != 0 ||
        process_state(&child) != PROS_PROCESS_ZOMBIE ||
        child.quarantined ||
        child.quarantine_previous_state != 0u)
        return -9;
    if (process_wait(&parent, child.pid, &exit_code) != 0 ||
        exit_code != 42 ||
        process_lookup(child.pid) != 0)
        return -10;
    if (process_exit(&parent, 7) != 0 ||
        process_wait(&parent, child.pid, &exit_code) == 0)
        return -11;
    if (process_state(&parent) != PROS_PROCESS_ZOMBIE)
        return -12;
    if (process_wait(&parent, parent.pid, &exit_code) == 0)
        return -13;
    if (process_release_resources(&parent) != 0)
        return -14;
    parent.state = PROS_PROCESS_DEAD;
    if (process_unregister(&parent) != 0)
        return -15;
    return 0;
}


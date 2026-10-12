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

static int process_object_type_valid(uint16_t type)
{
    switch (type) {
    case PROS_OBJECT_PROCESS:
    case PROS_OBJECT_THREAD:
    case PROS_OBJECT_ADDRESS_SPACE:
    case PROS_OBJECT_FILE:
    case PROS_OBJECT_DEVICE:
    case PROS_OBJECT_SOCKET:
    case PROS_OBJECT_IPC:
        return 1;
    default:
        return 0;
    }
}

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
        process->state > PROS_PROCESS_BLOCKED ||
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
    if (!process || process->object.type != PROS_OBJECT_PROCESS ||
        process->object.reserved != 0u || process->pid == 0u ||
        process->state != PROS_PROCESS_DEAD ||
        process->address_space.root != 0 ||
        process->address_space.owned_pages != 0 ||
        process->object.refcount != 1u ||
        process->object.handle_refs != 0u ||
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
           process->object.reserved == 0u &&
           process->object.refcount != 0u &&
           process->object.handle_refs <= process->object.refcount;
}

static int process_is_active(const struct pros_process *process) {
    return process_is_live(process) &&
           process->state <= PROS_PROCESS_BLOCKED;
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
    if (!process || pid == 0u) return -1;
    if (security_level != PROS_SECURITY_LIMITED &&
        security_level != PROS_SECURITY_SUPER) return -1;
    if (address_space_create(&process->address_space) != 0) return -1;

    object_init(&process->object, PROS_OBJECT_PROCESS);
    process->pid = pid;
    process->parent_pid = 0;
    process->security_level = security_level;
    process->state = PROS_PROCESS_NEW;
    process->exit_code = 0;
    process->quarantined = 0u;
    process->quarantine_previous_state = 0u;
    for (uint32_t i = 0u; i < sizeof(process->security_reserved); ++i)
        process->security_reserved[i] = 0u;
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
    /*
     * Validate every unregister invariant before destructive resource release.
     * Otherwise corrupted object metadata could make unregister fail after
     * the address space was already destroyed, leaving an unreapable zombie.
     */
    if (child->object.type != PROS_OBJECT_PROCESS ||
        child->object.reserved != 0u || child->object.refcount != 1u ||
        child->pid == 0u || child->address_space.root == 0 ||
        process_lookup(pid) != child)
        return -5;
    int32_t saved_exit_code = child->exit_code;
    if (process_release_resources(child) != 0) return -4;
    child->state = PROS_PROCESS_DEAD;
    if (process_unregister(child) != 0) {
        /* All unregister invariants were preflighted before teardown. */
        child->state = PROS_PROCESS_ZOMBIE;
        return -6;
    }
    *exit_code = saved_exit_code;
    return 0;
}

int process_set_state(struct pros_process *process, uint32_t state) {
    if (!process_is_live(process) || state > PROS_PROCESS_BLOCKED)
        return -1;
    /*
     * This API is for scheduler-visible states only. Terminal lifecycle
     * transitions are owned by process_exit()/process_wait(); otherwise a
     * caller could fabricate a zombie without terminating its threads, or
     * revive a process whose threads have already exited.
     */
    if (process->state >= PROS_PROCESS_ZOMBIE)
        return -1;
    /* NEW is an initialization state, not a state an active process may regain. */
    if (state == PROS_PROCESS_NEW && process->state != PROS_PROCESS_NEW)
        return -1;
    /* A quarantined process must remain scheduler-blocked until the security
     * quarantine is explicitly cleared. */
    if (process->quarantined && state != PROS_PROCESS_BLOCKED)
        return -1;
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
    if (process->security_level != PROS_SECURITY_SUPER ||
        (policy & ~process->syscall_policy) != 0u)
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
    if (!process_is_active(process) || !type_out ||
        handle == PROSENJIT_HANDLE_INVALID ||
        (handle >> 24) != 0u)
        return -1;

    uint32_t slot = (uint32_t)(handle & 0xFFULL);
    uint16_t generation = (uint16_t)(handle >> 8);
    if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX || generation == 0)
        return -1;

    const struct pros_handle_entry *entry = &process->handles.entries[slot];
    if (!entry->object || entry->generation != generation ||
        handle_check(&process->handles, handle, entry->rights,
                     PROS_OBJECT_NONE) != 0)
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

int process_selftest(void)
{
    struct pros_process parent;
    struct pros_process child;
    int32_t exit_code = 0;

    if (process_manager_init() != 0)
        return -1;
    /*
     * Direct close must reject an under-counted object shared by two local
     * capability slots without mutating either slot or releasing the object.
     */
    {
        struct pros_handle_table table;
        struct pros_handle_table target_table;
        struct pros_object object;
        uint64_t first_handle, second_handle, target_handle;

        handle_table_init(&table);
        handle_table_init(&target_table);
        object_init(&object, PROS_OBJECT_FILE);
        first_handle = handle_alloc(&table, &object, PROSENJIT_CAP_RIGHTS_ALL);
        target_handle = handle_alloc(&target_table, &object,
                                     PROSENJIT_CAP_RIGHTS_ALL);
        if (first_handle == PROSENJIT_HANDLE_INVALID ||
            target_handle == PROSENJIT_HANDLE_INVALID)
            return -23;
        table.entries[2] = table.entries[1];
        table.entries[2].generation = 1u;
        second_handle = (UINT64_C(1) << 8) | UINT64_C(2);
        object.refcount = 2u;
        object.handle_refs = 3u;
        if (handle_check(&table, first_handle, PROSENJIT_CAP_DUPLICATE,
                         PROS_OBJECT_FILE) == 0 ||
            handle_alloc(&table, &object, PROSENJIT_CAP_DUPLICATE) !=
                PROSENJIT_HANDLE_INVALID ||
            handle_duplicate(&table, first_handle,
                             PROSENJIT_CAP_DUPLICATE) !=
                PROSENJIT_HANDLE_INVALID ||
            handle_transfer_copy(&table, &target_table, first_handle,
                                 PROSENJIT_CAP_DUPLICATE) !=
                PROSENJIT_HANDLE_INVALID ||
            handle_transfer_move(&table, &target_table, first_handle,
                                 PROSENJIT_CAP_DUPLICATE) !=
                PROSENJIT_HANDLE_INVALID ||
            handle_close(&table, first_handle) == 0 ||
            table.entries[1].object != &object ||
            table.entries[2].object != &object ||
            object.refcount != 2u || object.handle_refs != 3u)
            return -23;
        object.refcount = 4u;
        object.handle_refs = 3u;
        if (handle_close(&table, first_handle) != 0 ||
            handle_close(&table, second_handle) != 0 ||
            handle_close(&target_table, target_handle) != 0 ||
            object.refcount != 1u || object.handle_refs != 0u ||
            object.type != PROS_OBJECT_FILE)
            return -23;

        /* An owner reference cannot be released out from under a live handle. */
        first_handle = handle_alloc(&table, &object, PROSENJIT_CAP_READ);
        if (first_handle == PROSENJIT_HANDLE_INVALID ||
            object_release(&object) != 0 ||
            object.refcount != 1u || object.handle_refs != 1u ||
            object_release(&object) == 0 ||
            object.refcount != 1u || object.type != PROS_OBJECT_FILE ||
            handle_close(&table, first_handle) != 0 ||
            object.refcount != 0u || object.handle_refs != 0u ||
            object.type != PROS_OBJECT_NONE)
            return -24;
    }
    if (process_init(&parent, 0u, PROS_SECURITY_SUPER) == 0)
        return -19;
    if (process_create(0, &parent, 1001u, PROS_SECURITY_SUPER) != 0)
        return -2;
    if (parent.state != PROS_PROCESS_READY ||
        process_lookup(1001u) != &parent)
        return -3;
    for (uint32_t i = 0u; i < sizeof(parent.security_reserved); ++i) {
        if (parent.security_reserved[i] != 0u)
            return -15;
    }
    if (process_create(&parent, &child, 1002u, PROS_SECURITY_LIMITED) != 0)
        return -4;
    if (child.parent != &parent || child.parent_pid != parent.pid)
        return -5;
    if (process_set_state(&child, PROS_PROCESS_NEW) == 0 ||
        process_state(&child) != PROS_PROCESS_READY)
        return -18;
    if (process_create(&parent, &child, 1002u, PROS_SECURITY_LIMITED) == 0)
        return -6;

    /* Self-policy updates may reduce authority but must never restore bits. */
    uint64_t initial_syscall_policy = parent.syscall_policy;
    if (process_set_syscall_policy(&parent, UINT64_C(1)) != 0 ||
        process_set_syscall_policy(&parent, initial_syscall_policy) == 0 ||
        parent.syscall_policy != UINT64_C(1))
        return -20;

    /* Quarantine must prevent an ordinary state transition from escaping
     * the security-enforced BLOCKED state. */
    child.quarantined = 1u;
    child.state = PROS_PROCESS_BLOCKED;
    child.quarantine_previous_state = (uint8_t)PROS_PROCESS_READY + 1u;
    if (process_set_state(&child, PROS_PROCESS_READY) == 0 ||
        process_state(&child) != PROS_PROCESS_BLOCKED)
        return -7;
    child.quarantined = 0u;
    child.quarantine_previous_state = 0u;
    child.state = PROS_PROCESS_READY;

    /* Parent cannot exit while it still has a live child. */
    if (process_exit(&parent, 7) == 0)
        return -7;
    /* Generic state changes cannot bypass process_exit() to fabricate a zombie. */
    if (process_set_state(&child, PROS_PROCESS_ZOMBIE) == 0 ||
        process_state(&child) != PROS_PROCESS_READY)
        return -17;

    /* Security quarantine metadata must be cleared when a process exits. */
    child.quarantined = 1u;
    child.quarantine_previous_state = (uint8_t)PROS_PROCESS_READY + 1u;
    if (process_exit(&child, 42) != 0 ||
        process_state(&child) != PROS_PROCESS_ZOMBIE ||
        child.quarantined ||
        child.quarantine_previous_state != 0u)
        return -8;
    if (process_set_state(&child, PROS_PROCESS_READY) == 0 ||
        process_state(&child) != PROS_PROCESS_ZOMBIE)
        return -16;
    child.object.reserved = 1u;
    if (process_wait(&parent, child.pid, &exit_code) == 0 ||
        child.address_space.root == 0 ||
        child.state != PROS_PROCESS_ZOMBIE)
        return -22;
    child.object.reserved = 0u;
    if (process_wait(&parent, child.pid, &exit_code) != 0 ||
        exit_code != 42 ||
        process_lookup(child.pid) != 0)
        return -9;

    parent.quarantined = 1u;
    parent.quarantine_previous_state = (uint8_t)PROS_PROCESS_READY + 1u;
    if (process_exit(&parent, 7) != 0 ||
        parent.quarantined ||
        parent.quarantine_previous_state != 0u ||
        process_state(&parent) != PROS_PROCESS_ZOMBIE)
        return -10;

    if (process_wait(&parent, child.pid, &exit_code) == 0)
        return -11;
    if (process_wait(&parent, parent.pid, &exit_code) == 0)
        return -12;
    if (process_release_resources(&parent) != 0)
        return -13;
    parent.state = PROS_PROCESS_DEAD;
    /* Refuse corrupt object metadata without unlinking the process from PID lookup. */
    parent.object.reserved = 1u;
    if (process_unregister(&parent) == 0 ||
        process_list != &parent || parent.next != 0)
        return -21;
    parent.object.reserved = 0u;
    if (process_unregister(&parent) != 0)
        return -14;
    return 0;
}

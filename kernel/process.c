#include "process.h"
#include "../../include/abi/syscall.h"
#include "paging.h"
#include "thread.h"
#include "elf_loader.h"
#include "security_monitor.h"

extern int paging_switch_kernel(void);

static struct pros_process *current_process;
static int process_is_live(const struct pros_process *process);
static int process_is_active(const struct pros_process *process);
static int process_release_resources(struct pros_process *process);
static int process_has_children(const struct pros_process *process);
static uint64_t process_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static void process_irq_restore(uint64_t flags) {
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}
static struct pros_process *process_list;
static uint64_t next_pid = 1;

/*
 * The process list is a singly linked ownership registry. Establish that it
 * is acyclic before any lookup/unlink traversal, then verify the target's
 * exact membership. Interrupt exclusion is supplied by lifecycle callers.
 */
static int process_registry_validate(const struct pros_process *target,
                                     int expected_present) {
    struct pros_process *slow = process_list;
    struct pros_process *fast = process_list;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast)
            return -1;
    }

    uint32_t matches = 0u;
    for (struct pros_process *it = process_list; it; it = it->next) {
        if (it == target)
            ++matches;
        if (matches > 1u)
            return -1;
    }
    return matches == (expected_present ? 1u : 0u) ? 0 : -1;
}

/*
 * PID ownership lasts until the process is physically unlinked, even after
 * its state becomes terminal. process_lookup() intentionally hides DEAD
 * entries from callers, so registration needs this stricter registry scan.
 */
static int process_pid_registered(uint64_t pid) {
    if (pid == 0u || process_registry_validate(0, 0) != 0)
        return -1;
    for (struct pros_process *it = process_list; it; it = it->next)
        if (it->pid == pid)
            return 1;
    return 0;
}

int process_manager_init(void) {
    uint64_t irq_flags = process_irq_save();
    process_list = 0;
    current_process = 0;
    next_pid = 1;
    process_irq_restore(irq_flags);
    return 0;
}

struct pros_process *process_lookup(uint64_t pid) {
    uint64_t irq_flags = process_irq_save();
    if (process_registry_validate(0, 0) != 0) {
        process_irq_restore(irq_flags);
        return 0;
    }
    for (struct pros_process *p = process_list; p; p = p->next) {
        if (p->pid == pid && p->state != PROS_PROCESS_DEAD) {
            process_irq_restore(irq_flags);
            return p;
        }
    }
    process_irq_restore(irq_flags);
    return 0;
}

int process_register(struct pros_process *process) {
    uint64_t irq_flags = process_irq_save();
    if (!process_is_live(process) || process->pid == 0 ||
        (process->security_level != PROS_SECURITY_LIMITED &&
         process->security_level != PROS_SECURITY_SUPER) ||
        process->state != PROS_PROCESS_READY ||
        process_registry_validate(process, 0) != 0 ||
        process_pid_registered(process->pid) != 0 ||
        (process->parent &&
         (!process_is_active(process->parent) ||
          process_registry_validate(process->parent, 1) != 0 ||
          process->parent->quarantined ||
          process->parent->state == PROS_PROCESS_BLOCKED ||
          process->parent->security_level < process->security_level ||
          process->parent_pid != process->parent->pid)) ||
        (!process->parent && process->parent_pid != 0u)) {
        process_irq_restore(irq_flags);
        return -1;
    }
    process->next = process_list;
    process_list = process;
    if (process->pid >= next_pid) {
        if (process->pid == UINT64_MAX)
            next_pid = UINT64_MAX;
        else
            next_pid = process->pid + 1;
    }
    process_irq_restore(irq_flags);
    return 0;
}

int process_unregister(struct pros_process *process) {
    if (!process) return -1;
    uint64_t irq_flags = process_irq_save();
    if (process_registry_validate(process, 1) != 0 ||
        process->state != PROS_PROCESS_DEAD ||
        process->object.type != PROS_OBJECT_PROCESS ||
        process->object.reserved != 0u || process->object.refcount != 1u ||
        process->address_space.root != 0 ||
        process->address_space.owned_pages != 0 ||
        process_has_children(process) ||
        handle_table_close_all_preflight(&process->handles) != 0 ||
        thread_process_has_live(process) ||
        thread_process_is_current(process) ||
        thread_process_reapable(process) != 1) {
        process_irq_restore(irq_flags);
        return -1;
    }

    /* A process may be unregistered only after every owned handle is closed. */
    for (uint32_t i = 1u; i < PROSENJIT_HANDLE_MAX; ++i) {
        if (process->handles.entries[i].object != 0 ||
            process->handles.entries[i].rights != 0u) {
            process_irq_restore(irq_flags);
            return -1;
        }
    }
    struct pros_process **link = &process_list;
    while (*link != process)
        link = &(*link)->next;

    if (current_process == process && paging_switch_kernel() != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }
    if (thread_reap_process(process) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }
    if (object_release(&process->object) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }

    *link = process->next;
    process->next = 0;
    if (current_process == process)
        current_process = 0;

    /* Security telemetry is scoped to one PID lifetime. */
    (void)pros_security_monitor_clear(process->pid);
    process_irq_restore(irq_flags);
    return 0;
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
    if (process_registry_validate(0, 0) != 0) return 1;
    for (struct pros_process *child = process_list; child; child = child->next)
        if (child->parent == process && child->state != PROS_PROCESS_DEAD)
            return 1;
    return 0;
}

static int process_release_resources(struct pros_process *process) {
    uint64_t irq_flags = process_irq_save();
    if (!process ||
        handle_table_close_all_preflight(&process->handles) != 0 ||
        address_space_destroy_preflight(&process->address_space) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }

    /* Both destructive operations have passed their complete read-only gate. */
    if (handle_table_close_all(&process->handles) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }
    int rc = address_space_destroy(&process->address_space);
    process_irq_restore(irq_flags);
    return rc;
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
    /*
     * Bootstrap identity: privileged processes map to root; limited
     * processes use their kernel-assigned PID only while it fits the v1
     * 32-bit on-disk UID/GID fields. Unknown identity fails closed.
     */
    process->uid = security_level == PROS_SECURITY_SUPER ? 0u :
        ((pid != 0u && pid < UINT32_MAX) ? (uint32_t)pid : UINT32_MAX);
    process->gid = process->uid;
    /*
     * Filesystem authority is not implied by the legacy security-level label.
     * Until a user/group database exists, use the kernel-assigned nonzero PID
     * as a bootstrap identity; values outside v1 on-disk range fail closed.
     */
    process->fs_identity_valid =
        (pid != 0u && pid < UINT32_MAX) ? 1u : 0u;
    process->fs_uid = process->fs_identity_valid ? (uint32_t)pid : UINT32_MAX;
    process->fs_gid = process->fs_identity_valid ? (uint32_t)pid : UINT32_MAX;
    process->fs_identity_reserved[0] = 0u;
    process->fs_identity_reserved[1] = 0u;
    process->fs_identity_reserved[2] = 0u;
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
    /*
     * Serialize parent validation, PID uniqueness, initialization and registry
     * publication as one lifecycle operation on the supported single-CPU
     * kernel. process_spawn() already holds this exclusion across its longer
     * load/rollback transaction; keep the simpler creation API equally safe.
     */
    uint64_t irq_flags = process_irq_save();
    if (!child || child == parent || pid == 0 ||
        process_registry_validate(child, 0) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }
    if (parent && (!process_is_active(parent) ||
                   process_registry_validate(parent, 1) != 0 ||
                   parent->quarantined ||
                   parent->state == PROS_PROCESS_BLOCKED ||
                   parent->security_level < security_level)) {
        process_irq_restore(irq_flags);
        return -1;
    }
    if (process_pid_registered(pid) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }
    if (process_init(child, pid, security_level) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }

    child->parent_pid = parent ? parent->pid : 0;
    child->parent = parent;
    child->state = PROS_PROCESS_READY;
    if (process_register(child) != 0) {
        /*
         * process_init() established the process object's initial reference.
         * If registration fails, release all initialized resources and that
         * reference as one rollback path; otherwise an unregistered process
         * leaks a live object even though its address space has been destroyed.
         */
        if (process_release_resources(child) == 0) {
            child->state = PROS_PROCESS_DEAD;
            (void)object_release(&child->object);
        }
        process_irq_restore(irq_flags);
        return -1;
    }

    /*
     * Security-monitor state is keyed by PID. A PID may be reused only
     * after the previous process has been fully reaped, so discard any
     * historical monitor score before the new lifetime becomes observable.
     * This prevents inherited quarantine/block scores across PID reuse.
     */
    (void)pros_security_monitor_clear(child->pid);
    process_irq_restore(irq_flags);
    return 0;
}



static uint32_t process_self_handle_references(
    const struct pros_process *process) {
    uint32_t references = 0u;
    for (uint32_t i = 1u; i < PROSENJIT_HANDLE_MAX; ++i)
        if (process->handles.entries[i].object == &process->object)
            ++references;
    return references;
}

/*
 * Compose the thread, handle-table, address-space, object-reference and
 * process-registry gates before spawn rollback performs its first mutation.
 * The caller holds process_irq_save() exclusion across preflight and commit.
 */
static int process_spawn_discard_unregistered(struct pros_process *child,
                                               struct pros_thread *thread,
                                               int thread_initialized) {
    if (!child || child->state != PROS_PROCESS_NEW ||
        current_process == child ||
        process_registry_validate(child, 0) != 0 ||
        child->object.refcount != process_self_handle_references(child) + 1u ||
        handle_table_close_all_preflight(&child->handles) != 0 ||
        address_space_destroy_preflight(&child->address_space) != 0 ||
        (thread_initialized &&
         (thread->process != child ||
          thread_spawn_rollback_preflight(thread, 0) != 0)))
        return -1;

    if (thread_initialized && thread_abort_unpublished(thread) != 0)
        return -1;
    if (handle_table_close_all(&child->handles) != 0 ||
        address_space_destroy(&child->address_space) != 0)
        return -1;
    return object_release(&child->object);
}

static int process_spawn_discard_registered(struct pros_process *child,
                                             struct pros_thread *thread,
                                             int queued) {
    if (!child || !thread || child->state != PROS_PROCESS_READY ||
        current_process == child || thread->process != child ||
        process_registry_validate(child, 1) != 0 ||
        child->object.refcount != process_self_handle_references(child) + 1u ||
        handle_table_close_all_preflight(&child->handles) != 0 ||
        address_space_destroy_preflight(&child->address_space) != 0 ||
        thread_spawn_rollback_preflight(thread, queued) != 0)
        return -1;

    if (queued && thread_remove_queued(thread) != 0)
        return -1;
    thread->state = PROS_THREAD_DEAD;
    child->state = PROS_PROCESS_DEAD;
    if (handle_table_close_all(&child->handles) != 0 ||
        address_space_destroy(&child->address_space) != 0 ||
        process_unregister(child) != 0)
        return -1;
    return 0;
}

static int process_spawn_internal(struct pros_process *parent,
                                  struct pros_process *child,
                                  struct pros_thread *thread,
                                  uint64_t pid, uint64_t tid,
                                  uint32_t security_level,
                                  const uint8_t *image, uint64_t image_size,
                                  const char *const *argv, uint32_t argc,
                                  const char *const *envp, uint32_t envc,
                                  uint32_t fail_stage) {
    uint64_t irq_flags = process_irq_save();
    int thread_initialized = 0;
    int queued = 0;
    int failure_code = 0;
    int loader_rc = PROS_ELF_OK;
    struct pros_elf_load_result loaded;

    if (!child || !thread || child == parent || pid == 0 || tid == 0 ||
        !image || image_size == 0 ||
        (security_level != PROS_SECURITY_LIMITED &&
         security_level != PROS_SECURITY_SUPER) ||
        process_registry_validate(child, 0) != 0 ||
        (parent && (!process_is_active(parent) ||
                    process_registry_validate(parent, 1) != 0 ||
                    parent->quarantined ||
                    parent->state == PROS_PROCESS_BLOCKED ||
                    parent->security_level < security_level)) ||
        process_pid_registered(pid) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }
    if (process_init(child, pid, security_level) != 0) {
        process_irq_restore(irq_flags);
        return -2;
    }
    child->parent = parent;
    child->parent_pid = parent ? parent->pid : 0;

    if (fail_stage == 1u)
        goto rollback_unregistered;
    loader_rc = pros_elf_load_with_args(child, image, image_size,
                                         argv, argc, envp, envc, &loaded);
    if (loader_rc != PROS_ELF_OK) {
        /* Preserve both the loader stage and exact ELF error for boot diagnostics. */
        failure_code = -1000 - (pros_elf_last_error_stage() * 100) + loader_rc;
        goto rollback_unregistered;
    }
    if (fail_stage == 2u)
        goto rollback_unregistered;

    if (thread_init_user(thread, child, tid, loaded.entry,
                         loaded.stack_top) != 0) {
        failure_code = -2001;
        goto rollback_unregistered;
    }
    thread_initialized = 1;
    if (fail_stage == 3u)
        goto rollback_unregistered;

    child->state = PROS_PROCESS_READY;
    if (process_register(child) != 0) {
        failure_code = -3001;
        goto rollback_unregistered;
    }
    if (fail_stage == 4u)
        goto rollback_registered;

    if (thread_enqueue(thread) != 0)
        goto rollback_registered;
    queued = 1;
    if (fail_stage == 5u)
        goto rollback_registered;

    process_irq_restore(irq_flags);
    return 0;

rollback_registered:
    if (process_spawn_discard_registered(child, thread, queued) != 0) {
        process_irq_restore(irq_flags);
        return -5;
    }
    process_irq_restore(irq_flags);
    return fail_stage ? -100 - (int)fail_stage : -4;

rollback_unregistered:
    if (process_spawn_discard_unregistered(child, thread,
                                          thread_initialized) != 0) {
        process_irq_restore(irq_flags);
        return -6;
    }
    process_irq_restore(irq_flags);
    return fail_stage ? -100 - (int)fail_stage : (failure_code ? failure_code : -7);
}

int process_spawn(struct pros_process *parent,
                  struct pros_process *child,
                  struct pros_thread *thread,
                  uint64_t pid, uint64_t tid, uint32_t security_level,
                  const uint8_t *image, uint64_t image_size,
                  const char *const *argv, uint32_t argc,
                  const char *const *envp, uint32_t envc) {
    return process_spawn_internal(parent, child, thread, pid, tid,
                                  security_level, image, image_size,
                                  argv, argc, envp, envc, 0u);
}

int process_spawn_rollback_selftest(struct pros_process *parent,
                                    const uint8_t *image, uint64_t image_size,
                                    const char *const *argv, uint32_t argc,
                                    const char *const *envp, uint32_t envc) {
    static struct pros_process child;
    static struct pros_thread thread;

    if (!parent || !image || image_size == 0)
        return -1;
    for (uint32_t stage = 1u; stage <= 5u; ++stage) {
        uint64_t pid = 0xC100u + stage;
        int rc = process_spawn_internal(parent, &child, &thread, pid, pid,
                                        PROS_SECURITY_LIMITED, image,
                                        image_size, argv, argc, envp, envc,
                                        stage);
        if (rc != -100 - (int)stage || process_lookup(pid) != 0 ||
            child.address_space.root != 0 || child.object.refcount != 0 ||
            child.next != 0 || thread.kernel_stack_base != 0 ||
            thread.process != 0 || thread.registry_next != 0 ||
            thread.next != 0 || thread.prev != 0)
            return -2;
    }
    return 0;
}

int process_exit(struct pros_process *process, int32_t exit_code) {
    uint64_t irq_flags = process_irq_save();
    if (!process_is_live(process) ||
        process_registry_validate(process, 1) != 0 ||
        process->state == PROS_PROCESS_DEAD ||
        process->state == PROS_PROCESS_ZOMBIE ||
        process_has_children(process) ||
        thread_process_terminate_preflight(process) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }

    if (current_process == process) {
        /*
         * Return to the kernel address space before terminating the current
         * process. A failed CR3 transition leaves process/thread state intact.
         */
        if (paging_switch_kernel() != 0) {
            process_irq_restore(irq_flags);
            return -1;
        }
        current_process = 0;
    }

    if (thread_terminate_process(process) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }

    process->exit_code = exit_code;
    process->quarantined = 0u;
    process->quarantine_previous_state = 0u;
    process->state = PROS_PROCESS_ZOMBIE;
    process_irq_restore(irq_flags);
    return 0;
}
int process_wait(struct pros_process *parent, uint64_t pid, int32_t *exit_code) {
    uint64_t irq_flags = process_irq_save();
    if (!process_is_active(parent) ||
        process_registry_validate(parent, 1) != 0 ||
        !exit_code || pid == 0) {
        process_irq_restore(irq_flags);
        return -1;
    }
    struct pros_process *child = process_lookup(pid);
    if (!child || child->parent != parent) {
        process_irq_restore(irq_flags);
        return -1;
    }
    if (child->state != PROS_PROCESS_ZOMBIE) {
        process_irq_restore(irq_flags);
        return -2;
    }

    /*
     * The process's own handle table may contain handles to the process
     * object itself. Those references are released by this teardown;
     * references held by other processes must be closed before wait/reap.
     */
    uint32_t self_handle_refs = 0u;
    for (uint32_t i = 1; i < PROSENJIT_HANDLE_MAX; ++i)
        if (child->handles.entries[i].object == &child->object)
            ++self_handle_refs;
    /*
     * Aggregate the read-only gates before the first destructive operation.
     * In particular, process-list membership/cycle checks must not be left
     * until process_unregister(), which runs after handles and mappings have
     * already been released.
     */
    if (child->object.refcount != self_handle_refs + 1u ||
        current_process == child ||
        process_registry_validate(child, 1) != 0 ||
        handle_table_close_all_preflight(&child->handles) != 0 ||
        address_space_destroy_preflight(&child->address_space) != 0 ||
        !thread_process_reapable(child)) {
        process_irq_restore(irq_flags);
        return -3;
    }

    int32_t child_exit_code = child->exit_code;
    if (process_release_resources(child) != 0) {
        process_irq_restore(irq_flags);
        return -4;
    }
    child->state = PROS_PROCESS_DEAD;
    if (process_unregister(child) != 0) {
        /* Keep a fully reaped process visible if final unlinking fails. */
        child->state = PROS_PROCESS_ZOMBIE;
        process_irq_restore(irq_flags);
        return -5;
    }
    *exit_code = child_exit_code;
    process_irq_restore(irq_flags);
    return 0;
}

int process_set_state(struct pros_process *process, uint32_t state) {
    uint64_t irq_flags = process_irq_save();
    if (!process_is_active(process) || state > PROS_PROCESS_DEAD ||
        state == PROS_PROCESS_ZOMBIE || state == PROS_PROCESS_DEAD ||
        (process->quarantined && state != PROS_PROCESS_BLOCKED)) {
        process_irq_restore(irq_flags);
        return -1;
    }
    process->state = state;
    process_irq_restore(irq_flags);
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
    uint64_t irq_flags = process_irq_save();
    if (process && (!process_is_active(process) ||
                    process_registry_validate(process, 1) != 0)) {
        process_irq_restore(irq_flags);
        return;
    }
    struct pros_process *next = process;
    if (next == current_process) {
        process_irq_restore(irq_flags);
        return;
    }
    if (next) {
        if (address_space_switch(&next->address_space) != 0) {
            process_irq_restore(irq_flags);
            return;
        }
        current_process = next;
    } else {
        if (paging_switch_kernel() != 0) {
            process_irq_restore(irq_flags);
            return;
        }
        current_process = 0;
    }
    process_irq_restore(irq_flags);
}

struct pros_process *process_current(void) {
    return current_process;
}

static int process_selftest_run(void)
{
    struct pros_process parent;
    struct pros_process child;
    int32_t exit_code = 0;

    if (process_manager_init() != 0)
        return -1;
    if (process_create(0, &parent, 1001u, PROS_SECURITY_SUPER) != 0)
        return -2;
    if (parent.state != PROS_PROCESS_READY ||
        process_lookup(1001u) != &parent)
        return -3;

    /* An initialized but unpublished object cannot act as a process parent. */
    struct pros_process orphan_parent;
    struct pros_process orphan_child;
    if (process_init(&orphan_parent, 1003u, PROS_SECURITY_LIMITED) != 0)
        return -32;
    process_set_current(&orphan_parent);
    if (process_current() != 0)
        return -36;
    if (process_register(&orphan_parent) == 0)
        return -35;
    if (process_create(&orphan_parent, &orphan_child, 1004u,
                       PROS_SECURITY_LIMITED) == 0 ||
        process_exit(&orphan_parent, 99) == 0)
        return -33;
    if (address_space_destroy(&orphan_parent.address_space) != 0 ||
        object_release(&orphan_parent.object) != 0)
        return -34;

    if (process_create(&parent, &child, 1002u, PROS_SECURITY_LIMITED) != 0)
        return -4;
    if (parent.uid != 0u || parent.gid != 0u ||
        child.uid != 1002u || child.gid != 1002u)
        return -30;
    if (!parent.fs_identity_valid || parent.fs_uid != 1001u ||
        parent.fs_gid != 1001u || !child.fs_identity_valid ||
        child.fs_uid != 1002u || child.fs_gid != 1002u)
        return -31;
    if (child.parent != &parent || child.parent_pid != parent.pid)
        return -5;
    if (process_create(&parent, &child, 1002u, PROS_SECURITY_LIMITED) == 0)
        return -6;

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

    /* Security quarantine metadata must be cleared when a process exits. */
    child.quarantined = 1u;
    child.quarantine_previous_state = (uint8_t)PROS_PROCESS_READY + 1u;
    if (process_exit(&child, 42) != 0 ||
        process_state(&child) != PROS_PROCESS_ZOMBIE ||
        child.quarantined ||
        child.quarantine_previous_state != 0u)
        return -8;
    if (process_wait(&parent, child.pid, &exit_code) != 0 ||
        exit_code != 42 ||
        process_lookup(child.pid) != 0)
        return -9;

    /*
     * A corrupted process-list cycle must reject wait before any child
     * resource or lifecycle state changes; restoring the link must permit
     * the exact same zombie to be reaped successfully.
     */
    struct pros_process cycle_child;
    if (process_create(&parent, &cycle_child, 1003u,
                       PROS_SECURITY_LIMITED) != 0 ||
        process_exit(&cycle_child, 17) != 0)
        return -15;
    struct pros_process *saved_parent_next = parent.next;
    parent.next = &parent;
    if (process_registry_validate(&cycle_child, 1) == 0 ||
        process_lookup(cycle_child.pid) != 0 ||
        process_wait(&parent, cycle_child.pid, &exit_code) == 0 ||
        cycle_child.state != PROS_PROCESS_ZOMBIE ||
        cycle_child.address_space.root == 0 ||
        cycle_child.object.refcount != 1u) {
        parent.next = saved_parent_next;
        return -16;
    }
    parent.next = saved_parent_next;
    if (process_wait(&parent, cycle_child.pid, &exit_code) != 0 ||
        exit_code != 17 || process_lookup(cycle_child.pid) != 0)
        return -17;

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
    if (process_unregister(&parent) != 0)
        return -14;
    return 0;
}

/*
 * Run lifecycle tests against a temporary process registry. Preserve the
 * live kernel registry/current-process identity and keep local interrupts
 * disabled so no scheduler path can observe the temporary test registry.
 */
int process_selftest(void) {
    uint64_t irq_flags = process_irq_save();
    struct pros_process *saved_process_list = process_list;
    struct pros_process *saved_current_process = current_process;
    uint64_t saved_next_pid = next_pid;

    int rc = process_selftest_run();

    process_list = saved_process_list;
    current_process = saved_current_process;
    next_pid = saved_next_pid;
    process_irq_restore(irq_flags);
    return rc;
}

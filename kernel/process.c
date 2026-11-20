#include "process.h"
#include "memory.h"
#include "../../include/abi/syscall.h"
#include "paging.h"
#include "thread.h"
#include "elf_loader.h"
#include "security_monitor.h"

extern int paging_switch_kernel(void);
extern int cpu_set_kernel_stack(uint64_t stack_top);
extern uint64_t cpu_get_kernel_stack(void);

static struct pros_process *current_process;
static int process_is_live(const struct pros_process *process);
static int process_is_active(const struct pros_process *process);
static int process_release_resources(struct pros_process *process);
static int process_has_children(const struct pros_process *process);
static int process_pid_registered(uint64_t pid);
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
 * Validate a parent relationship using pointer identity before dereferencing
 * the parent. A parent must remain in the process registry for the complete
 * lifetime of every registered child, including DEAD children awaiting reap.
 * Root processes have neither a parent pointer nor a parent PID.
 */
static int process_parent_validate(const struct pros_process *process) {
    if (!process)
        return -1;
    if (!process->parent)
        return process->parent_pid == 0u ? 0 : -1;
    if (process->parent == process)
        return -1;

    struct pros_process *registered_parent = 0;
    uint32_t matches = 0u;
    for (struct pros_process *it = process_list; it; it = it->next) {
        if (it == process->parent) {
            registered_parent = it;
            ++matches;
        }
    }
    if (matches != 1u || !registered_parent)
        return -1;

    return process->parent_pid == registered_parent->pid ? 0 : -1;
}

/*
 * The process list is a singly linked ownership registry. Establish that it
 * is acyclic before any lookup/unlink traversal, then verify exact target
 * membership and every registered parent edge. Interrupt exclusion is
 * supplied by lifecycle callers.
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
        if (matches > 1u || process_parent_validate(it) != 0)
            return -1;
    }
    return matches == (expected_present ? 1u : 0u) ? 0 : -1;
}

int process_manager_init(void) {
    uint64_t irq_flags = process_irq_save();
    /*
     * Manager initialization is valid only before the first publication or
     * after every process has been fully unregistered. Never discard live
     * registry links or a current-process reference during reinitialization.
     */
    if (process_list || current_process ||
        process_registry_validate(0, 0) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }
    next_pid = 1;
    process_irq_restore(irq_flags);
    return 0;
}

struct pros_process *process_lookup(uint64_t pid) {
    if (process_registry_validate(0, 0) != 0)
        return 0;
    for (struct pros_process *p = process_list; p; p = p->next)
        if (p->pid == pid && p->state != PROS_PROCESS_DEAD)
            return p;
    return 0;
}

int process_register(struct pros_process *process) {
    if (!process_is_live(process) || process->pid == 0 ||
        process_registry_validate(process, 0) != 0 ||
        process_parent_validate(process) != 0 ||
        process_pid_registered(process->pid)) return -1;
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
    if (!process) return -1;
    uint64_t irq_flags = process_irq_save();
    if (process_registry_validate(process, 1) != 0 ||
        process_has_children(process) ||
        process->state != PROS_PROCESS_DEAD ||
        process->address_space.root != 0 || process->object.refcount != 1 ||
        thread_process_has_live(process) || thread_process_is_current(process)) {
        process_irq_restore(irq_flags);
        return -1;
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
        if (child->parent == process)
            return 1;
    return 0;
}

/*
 * PID uniqueness is a registry-lifetime invariant, not a lookup visibility
 * rule. DEAD entries can remain registered after a failed final teardown and
 * must continue reserving their PID until process_unregister() unlinks them.
 */
static int process_pid_registered(uint64_t pid) {
    if (pid == 0u || process_registry_validate(0, 0) != 0)
        return 1;
    for (struct pros_process *it = process_list; it; it = it->next)
        if (it->pid == pid)
            return 1;
    return 0;
}

static int process_release_resources(struct pros_process *process) {
    uint64_t irq_flags = process_irq_save();
    if (!process ||
        handle_table_close_all_preflight(&process->handles) != 0 ||
        address_space_destroy_preflight(&process->address_space) != 0 ||
        thread_address_space_disjoint_preflight(&process->address_space) != 0) {
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
     * Reject storage that is already published before process_init() can
     * overwrite its object, handle table, address space, or registry links.
     * process_register() also checks membership, but that is too late: init
     * has already mutated the caller's live process by then.
     */
    if (!child || pid == 0 || child == parent ||
        process_registry_validate(child, 0) != 0)
        return -1;
    if (parent && !process_is_active(parent)) return -1;
    if (process_pid_registered(pid)) return -1;
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
        thread_address_space_disjoint_preflight(&child->address_space) != 0 ||
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
        thread_address_space_disjoint_preflight(&child->address_space) != 0 ||
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
        process_registry_validate(child, 0) != 0 ||
        (security_level != PROS_SECURITY_LIMITED &&
         security_level != PROS_SECURITY_SUPER) ||
        (parent && (!process_is_active(parent) || parent->quarantined ||
                    parent->state == PROS_PROCESS_BLOCKED ||
                    parent->security_level < security_level)) ||
        process_lookup(pid) != 0) {
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
    /*
     * Test-only fault injection: make aggregate rollback preflight reject
     * before its first mutation. The selftest restores this field and retries
     * the same cleanup transaction.
     */
    if (fail_stage == 6u) {
        thread->wait_channel = (void *)(uintptr_t)1u;
        goto rollback_unregistered;
    }
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
    /*
     * Test-only fault injection: corrupt a preflight-only invariant after
     * registration and enqueue. The registered rollback must reject without
     * unlinking the run queue or destroying any child resource.
     */
    if (fail_stage == 7u) {
        thread->wait_channel = (void *)(uintptr_t)1u;
        goto rollback_registered;
    }
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

    /*
     * Force the aggregate rollback preflight to reject on a live unpublished
     * thread. Rejection must leave the process, address space, registry node,
     * stack ownership and thread state intact. Restore the injected field and
     * retry the exact cleanup path to prove the transaction remains recoverable.
     */
    const uint64_t preflight_pid = 0xC106u;
    int rejected = process_spawn_internal(parent, &child, &thread,
                                          preflight_pid, preflight_pid,
                                          PROS_SECURITY_LIMITED, image,
                                          image_size, argv, argc, envp, envc,
                                          6u);
    if (rejected != -6 || process_lookup(preflight_pid) != 0 ||
        child.state != PROS_PROCESS_NEW || child.address_space.root == 0 ||
        child.object.refcount == 0 || thread.process != &child ||
        thread.state != PROS_THREAD_NEW ||
        thread.wait_channel != (void *)(uintptr_t)1u ||
        thread.kernel_stack_base == 0 || thread.kernel_stack_size == 0)
        return -3;

    thread.wait_channel = 0;
    if (process_spawn_discard_unregistered(&child, &thread, 1) != 0 ||
        child.address_space.root != 0 || child.object.refcount != 0 ||
        thread.kernel_stack_base != 0 || thread.process != 0 ||
        thread.registry_next != 0 || thread.wait_channel != 0)
        return -4;

    /*
     * Repeat the aggregate-preflight rejection after the child is registered
     * and queued. Rejection must preserve process lookup, run-queue integrity,
     * address space, handle/object lifetime and the complete thread stack.
     */
    const uint64_t registered_pid = 0xC107u;
    int registered_rejected =
        process_spawn_internal(parent, &child, &thread, registered_pid,
                               registered_pid, PROS_SECURITY_LIMITED, image,
                               image_size, argv, argc, envp, envc, 7u);
    if (registered_rejected != -5 ||
        process_lookup(registered_pid) != &child ||
        child.state != PROS_PROCESS_READY || child.address_space.root == 0 ||
        child.object.refcount == 0 || thread.process != &child ||
        thread.state != PROS_THREAD_READY ||
        thread.wait_channel != (void *)(uintptr_t)1u ||
        thread.kernel_stack_base == 0 || thread.kernel_stack_size == 0 ||
        thread_spawn_rollback_preflight(&thread, 1) == 0)
        return -5;

    thread.wait_channel = 0;
    if (thread_spawn_rollback_preflight(&thread, 1) != 0 ||
        process_spawn_discard_registered(&child, &thread, 1) != 0 ||
        process_lookup(registered_pid) != 0 ||
        child.address_space.root != 0 || child.object.refcount != 0 ||
        child.next != 0 || thread.kernel_stack_base != 0 ||
        thread.process != 0 || thread.registry_next != 0 ||
        thread.next != 0 || thread.prev != 0 ||
        thread.wait_channel != 0)
        return -6;
    return 0;
}

int process_exit(struct pros_process *process, int32_t exit_code) {
    uint64_t irq_flags = process_irq_save();
    if (!process_is_live(process) ||
        process->state == PROS_PROCESS_DEAD ||
        process->state == PROS_PROCESS_ZOMBIE ||
        process_has_children(process)) {
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
    if (!process_is_active(parent) || !exit_code || pid == 0) {
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
        process_has_children(child) ||
        handle_table_close_all_preflight(&child->handles) != 0 ||
        address_space_destroy_preflight(&child->address_space) != 0 ||
        thread_address_space_disjoint_preflight(&child->address_space) != 0 ||
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
    /*
     * Terminal transitions belong exclusively to process_exit()/process_wait().
     * A generic state setter must not bypass thread termination, zombie
     * publication, or the transactional reap path, nor resurrect a terminal
     * process. Keep the state check and update inside the lifecycle exclusion.
     */
    if (!process_is_live(process) ||
        process->state > PROS_PROCESS_BLOCKED ||
        state > PROS_PROCESS_BLOCKED) {
        process_irq_restore(irq_flags);
        return -1;
    }
    /* A quarantined process must remain scheduler-blocked until the security
     * quarantine is explicitly cleared. */
    if (process->quarantined && state != PROS_PROCESS_BLOCKED) {
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

    /*
     * The kernel boot path has already initialized the manager and registered
     * PID 1. A self-test must not reset or discard that live process registry.
     */
    if (!process_lookup(1u))
        return -1;
    if (process_create(0, &parent, 1001u, PROS_SECURITY_SUPER) != 0)
        return -2;
    if (parent.state != PROS_PROCESS_READY ||
        process_lookup(1001u) != &parent)
        return -3;
    if (process_create(&parent, &child, 1002u, PROS_SECURITY_LIMITED) != 0)
        return -4;

    /*
     * A registered process object must never be reused as process_create()
     * storage. Admission must reject before process_init() can overwrite its
     * live object, handle table, address-space root, or parent/list metadata.
     */
    uint64_t parent_root_before = (uint64_t)(uintptr_t)parent.address_space.root;
    uint32_t parent_refs_before = parent.object.refcount;
    if (process_create(&parent, &parent, 1003u, PROS_SECURITY_LIMITED) == 0 ||
        parent.pid != 1001u || parent.state != PROS_PROCESS_READY ||
        parent.address_space.root != (void *)(uintptr_t)parent_root_before ||
        parent.object.refcount != parent_refs_before ||
        parent.parent != 0 || parent.parent_pid != 0u ||
        process_lookup(1001u) != &parent)
        return -65;

    uint64_t child_root_before = (uint64_t)(uintptr_t)child.address_space.root;
    uint32_t child_refs_before = child.object.refcount;
    if (process_create(&parent, &child, 1003u, PROS_SECURITY_LIMITED) == 0 ||
        child.pid != 1002u || child.state != PROS_PROCESS_READY ||
        child.address_space.root != (void *)(uintptr_t)child_root_before ||
        child.object.refcount != child_refs_before ||
        child.parent != &parent || child.parent_pid != parent.pid ||
        process_lookup(1002u) != &child)
        return -66;

    /*
     * Reinitialization must not drop already-published process ownership.
     * Both parent and child must remain discoverable after rejection.
     */
    if (process_manager_init() == 0 ||
        process_lookup(parent.pid) != &parent ||
        process_lookup(child.pid) != &child)
        return -47;

    if (parent.uid != 0u || parent.gid != 0u ||
        child.uid != 1002u || child.gid != 1002u)
        return -30;
    if (!parent.fs_identity_valid || parent.fs_uid != 1001u ||
        parent.fs_gid != 1001u || !child.fs_identity_valid ||
        child.fs_uid != 1002u || child.fs_gid != 1002u)
        return -31;
    if (child.parent != &parent || child.parent_pid != parent.pid)
        return -5;

    /*
     * Lookup intentionally hides DEAD processes, but a still-registered DEAD
     * record continues to own its PID until final unlink. Creating another
     * process with that PID must fail rather than publish a duplicate ID.
     */
    struct pros_process duplicate_pid_probe;
    child.state = PROS_PROCESS_DEAD;
    int duplicate_dead_pid_rc =
        process_create(&parent, &duplicate_pid_probe, child.pid,
                       PROS_SECURITY_LIMITED);
    child.state = PROS_PROCESS_READY;
    if (duplicate_dead_pid_rc == 0 ||
        process_registry_validate(&child, 1) != 0 ||
        process_lookup(child.pid) != &child)
        return -64;

    /* Parent pointers and parent PIDs must agree with registry membership. */
    uint64_t saved_child_parent_pid = child.parent_pid;
    child.parent_pid = parent.pid + 1u;
    if (process_registry_validate(&child, 1) == 0) {
        child.parent_pid = saved_child_parent_pid;
        return -48;
    }
    child.parent_pid = saved_child_parent_pid;
    struct pros_process *saved_child_parent = child.parent;
    child.parent = 0;
    if (process_registry_validate(&child, 1) == 0) {
        child.parent = saved_child_parent;
        return -49;
    }
    child.parent = saved_child_parent;
    uint64_t saved_root_parent_pid = parent.parent_pid;
    parent.parent_pid = 1u;
    if (process_registry_validate(&child, 1) == 0) {
        parent.parent_pid = saved_root_parent_pid;
        return -50;
    }
    parent.parent_pid = saved_root_parent_pid;

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

    /* A registered DEAD child still owns a parent pointer until it is reaped. */
    uint32_t saved_child_state = child.state;
    child.state = PROS_PROCESS_DEAD;
    if (process_exit(&parent, 7) == 0) {
        child.state = saved_child_state;
        return -51;
    }
    child.state = saved_child_state;

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
     * process_unregister() refuses to unlink a process that still has
     * registered children. process_wait() must make the same check before
     * closing handles or destroying the address space; otherwise unregister
     * can fail only after the destructive half of reap has committed.
     *
     * Construct the otherwise-unreachable zombie-with-child state directly
     * as a corruption fixture, then prove wait rejects without mutation.
     */
    struct pros_process nested_child;
    struct pros_process nested_grandchild;
    if (process_create(&parent, &nested_child, 1008u,
                       PROS_SECURITY_LIMITED) != 0 ||
        process_create(&nested_child, &nested_grandchild, 1009u,
                       PROS_SECURITY_LIMITED) != 0)
        return -61;
    uint64_t nested_root =
        (uint64_t)(uintptr_t)nested_child.address_space.root;
    uint32_t nested_refs = nested_child.object.refcount;
    int32_t nested_exit = 0x71717171;
    nested_child.state = PROS_PROCESS_ZOMBIE;
    int nested_wait_rc =
        process_wait(&parent, nested_child.pid, &nested_exit);
    if (nested_wait_rc != -3 || nested_exit != 0x71717171 ||
        nested_child.state != PROS_PROCESS_ZOMBIE ||
        (uint64_t)(uintptr_t)nested_child.address_space.root != nested_root ||
        nested_child.object.refcount != nested_refs ||
        process_lookup(nested_child.pid) != &nested_child ||
        process_lookup(nested_grandchild.pid) != &nested_grandchild) {
        nested_child.state = PROS_PROCESS_READY;
        return -62;
    }

    /* Restore a valid lifecycle and remove the fixture grandchild cleanly. */
    nested_child.state = PROS_PROCESS_READY;
    nested_grandchild.state = PROS_PROCESS_DEAD;
    if (address_space_destroy(&nested_grandchild.address_space) != 0 ||
        process_unregister(&nested_grandchild) != 0 ||
        process_exit(&nested_child, 29) != 0 ||
        process_wait(&parent, nested_child.pid, &nested_exit) != 0 ||
        nested_exit != 29 || process_lookup(nested_child.pid) != 0)
        return -63;

    /*
     * Exercise independent wait/reap preflight boundaries against the same
     * zombie. Every rejected attempt must leave the exit-code output,
     * lifecycle state, address-space root, object references and registry
     * membership unchanged; restoring the corrupted field must permit retry.
     */
    struct pros_process matrix_child;
    if (process_create(&parent, &matrix_child, 1004u,
                       PROS_SECURITY_LIMITED) != 0 ||
        process_exit(&matrix_child, 23) != 0)
        return -32;
    /*
     * Exercise both run-queue endpoint corruption cases through the actual
     * wait/reap path, including a complete resource/registry zero-mutation
     * snapshot. Keep the same zombie for the remaining retry matrix.
     */
    if (thread_runq_corruption_wait_selftest(&parent, &matrix_child) != 0)
        return -45;
    const uint64_t matrix_root = (uint64_t)(uintptr_t)matrix_child.address_space.root;
    const uint32_t matrix_refs = matrix_child.object.refcount;
    int32_t matrix_exit = 0x5A5A5A5A;

    struct pros_object *saved_reserved_handle =
        matrix_child.handles.entries[0].object;
    matrix_child.handles.entries[0].object = &matrix_child.object;
    if (process_wait(&parent, matrix_child.pid, &matrix_exit) != -3 ||
        matrix_exit != 0x5A5A5A5A ||
        matrix_child.state != PROS_PROCESS_ZOMBIE ||
        (uint64_t)(uintptr_t)matrix_child.address_space.root != matrix_root ||
        matrix_child.object.refcount != matrix_refs ||
        process_lookup(matrix_child.pid) != &matrix_child) {
        matrix_child.handles.entries[0].object = saved_reserved_handle;
        return -33;
    }
    matrix_child.handles.entries[0].object = saved_reserved_handle;

    matrix_child.address_space.root = 0;
    if (process_wait(&parent, matrix_child.pid, &matrix_exit) != -3 ||
        matrix_exit != 0x5A5A5A5A ||
        matrix_child.state != PROS_PROCESS_ZOMBIE ||
        matrix_child.object.refcount != matrix_refs ||
        process_lookup(matrix_child.pid) != &matrix_child) {
        matrix_child.address_space.root = (uint64_t *)(uintptr_t)matrix_root;
        return -34;
    }
    matrix_child.address_space.root = (uint64_t *)(uintptr_t)matrix_root;

    if (process_wait(&parent, matrix_child.pid, &matrix_exit) != 0 ||
        matrix_exit != 23 || process_lookup(matrix_child.pid) != 0 ||
        matrix_child.address_space.root != 0 ||
        matrix_child.object.refcount != 0)
        return -35;

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

    /*
     * Direct process_unregister() must reject a cyclic registry before
     * searching for an absent target. Without the bounded registry preflight,
     * the pointer-to-pointer membership walk would never terminate.
     */
    struct pros_process unregister_probe;
    if (process_init(&unregister_probe, 0x1006u,
                     PROS_SECURITY_LIMITED) != 0 ||
        address_space_destroy(&unregister_probe.address_space) != 0)
        return -58;
    unregister_probe.state = PROS_PROCESS_DEAD;
    struct pros_process *registry_tail = process_list;
    if (!registry_tail)
        return -59;
    while (registry_tail->next)
        registry_tail = registry_tail->next;
    registry_tail->next = process_list;
    int unregister_cycle_rc = process_unregister(&unregister_probe);
    registry_tail->next = 0;
    if (unregister_cycle_rc == 0 ||
        unregister_probe.object.refcount != 1u ||
        unregister_probe.address_space.root != 0 ||
        process_registry_validate(0, 0) != 0 ||
        object_release(&unregister_probe.object) != 0)
        return -60;

    /*
     * A dead process thread owns a real guarded kernel stack. Corrupt each
     * independently validated stack descriptor and the active TSS RSP0;
     * every rejected wait must leave the zombie and its address space intact.
     */
    static struct pros_process stack_child;
    static struct pros_thread stack_thread;
    static struct pros_thread stack_peer_thread;
    if (process_create(&parent, &stack_child, 1005u,
                       PROS_SECURITY_LIMITED) != 0 ||
        thread_init(&stack_thread, &stack_child, 1005u,
                    (uint64_t)(uintptr_t)process_selftest) != 0 ||
        thread_init(&stack_peer_thread, &stack_child, 1006u,
                    (uint64_t)(uintptr_t)process_selftest) != 0 ||
        process_exit(&stack_child, 29) != 0)
        return -36;

    const uint64_t stack_child_root =
        (uint64_t)(uintptr_t)stack_child.address_space.root;
    const uint32_t stack_child_refs = stack_child.object.refcount;
    const uint64_t stack_base = (uint64_t)(uintptr_t)stack_thread.kernel_stack_base;
    const uint64_t stack_size = stack_thread.kernel_stack_size;
    const uint32_t stack_slot = stack_thread.kernel_stack_slot;
    const uint64_t stack_page0 = stack_thread.kernel_stack_pages[0];
    enum { REAP_TEST_STACK_PAGE_COUNT =
        sizeof(((struct pros_thread *)0)->kernel_stack_pages) / sizeof(uint64_t) };
    int32_t stack_exit = 0x4B4B4B4B;

    /*
     * Exercise the aggregate paging transaction with two complete stacks.
     * Corrupt only the final expected mapping: rejection must happen before
     * any earlier PTE is cleared or any owned page is freed.
     */
    uint64_t reap_test_virtual[2u * REAP_TEST_STACK_PAGE_COUNT];
    uint64_t reap_test_expected[2u * REAP_TEST_STACK_PAGE_COUNT];
    uint64_t reap_test_allocated[2u * REAP_TEST_STACK_PAGE_COUNT];
    struct pros_thread *reap_test_threads[2] = {
        &stack_thread, &stack_peer_thread
    };
    uint32_t reap_test_index = 0u;
    for (uint32_t t = 0u; t < 2u; ++t) {
        uint64_t base = (uint64_t)(uintptr_t)reap_test_threads[t]->kernel_stack_base;
        for (uint32_t p = 0u; p < REAP_TEST_STACK_PAGE_COUNT; ++p) {
            reap_test_virtual[reap_test_index] =
                base + (uint64_t)p * PROSENJIT_PAGE_SIZE;
            reap_test_expected[reap_test_index] =
                reap_test_threads[t]->kernel_stack_pages[p];
            reap_test_allocated[reap_test_index] =
                reap_test_threads[t]->kernel_stack_pages[p];
            ++reap_test_index;
        }
    }
    const uint64_t final_expected =
        reap_test_expected[reap_test_index - 1u];
    reap_test_expected[reap_test_index - 1u] =
        final_expected ^ PROSENJIT_PAGE_SIZE;
    if (paging_unmap_pages_expected_and_free(
            reap_test_virtual, reap_test_expected, reap_test_index,
            reap_test_allocated, reap_test_index) == 0) {
        reap_test_expected[reap_test_index - 1u] = final_expected;
        return -56;
    }
    reap_test_expected[reap_test_index - 1u] = final_expected;
    for (uint32_t i = 0u; i < reap_test_index; ++i) {
        uint64_t actual = 0u;
        if (paging_query_physical_checked(reap_test_virtual[i], &actual) != 0 ||
            actual != reap_test_expected[i])
            return -57;
    }

    /* Inject both live-RSP and TSS.RSP0 aliases into the read-only reap gate. */
    if (thread_process_reap_context_selftest(&stack_child) != 0)
        return -44;

    stack_thread.kernel_stack_slot = 64u;
    if (process_wait(&parent, stack_child.pid, &stack_exit) != -3 ||
        stack_exit != 0x4B4B4B4B ||
        stack_child.state != PROS_PROCESS_ZOMBIE ||
        (uint64_t)(uintptr_t)stack_child.address_space.root != stack_child_root ||
        stack_child.object.refcount != stack_child_refs ||
        process_lookup(stack_child.pid) != &stack_child) {
        stack_thread.kernel_stack_slot = stack_slot;
        return -37;
    }
    stack_thread.kernel_stack_slot = stack_slot;

    stack_thread.kernel_stack_pages[0] = 0u;
    if (process_wait(&parent, stack_child.pid, &stack_exit) != -3 ||
        stack_exit != 0x4B4B4B4B ||
        stack_child.state != PROS_PROCESS_ZOMBIE ||
        (uint64_t)(uintptr_t)stack_child.address_space.root != stack_child_root ||
        stack_child.object.refcount != stack_child_refs ||
        process_lookup(stack_child.pid) != &stack_child) {
        stack_thread.kernel_stack_pages[0] = stack_page0;
        return -38;
    }
    stack_thread.kernel_stack_pages[0] = stack_page0;

    /*
     * Two distinct thread records must never claim the same physical stack
     * page. The global registry preflight must reject this cross-record alias
     * before either stack can be released.
     */
    const uint64_t peer_page0 = stack_peer_thread.kernel_stack_pages[0];
    stack_peer_thread.kernel_stack_pages[0] = stack_page0;
    if (process_wait(&parent, stack_child.pid, &stack_exit) != -3 ||
        stack_exit != 0x4B4B4B4B ||
        stack_child.state != PROS_PROCESS_ZOMBIE ||
        (uint64_t)(uintptr_t)stack_child.address_space.root != stack_child_root ||
        stack_child.object.refcount != stack_child_refs ||
        process_lookup(stack_child.pid) != &stack_child) {
        stack_peer_thread.kernel_stack_pages[0] = peer_page0;
        return -43;
    }
    stack_peer_thread.kernel_stack_pages[0] = peer_page0;

    stack_thread.kernel_stack_size = UINT64_MAX;
    if (process_wait(&parent, stack_child.pid, &stack_exit) != -3 ||
        stack_exit != 0x4B4B4B4B ||
        stack_child.state != PROS_PROCESS_ZOMBIE ||
        (uint64_t)(uintptr_t)stack_child.address_space.root != stack_child_root ||
        stack_child.object.refcount != stack_child_refs ||
        process_lookup(stack_child.pid) != &stack_child) {
        stack_thread.kernel_stack_size = stack_size;
        return -39;
    }
    stack_thread.kernel_stack_size = stack_size;

    uint64_t saved_rsp0 = cpu_get_kernel_stack();
    uint64_t child_stack_top = stack_base + stack_size;
    if (!saved_rsp0 || cpu_set_kernel_stack(child_stack_top) != 0)
        return -40;
    int32_t tss_wait_rc = process_wait(&parent, stack_child.pid, &stack_exit);
    int tss_restore_rc = cpu_set_kernel_stack(saved_rsp0);
    if (tss_wait_rc != -3 || tss_restore_rc != 0 ||
        stack_exit != 0x4B4B4B4B ||
        stack_child.state != PROS_PROCESS_ZOMBIE ||
        (uint64_t)(uintptr_t)stack_child.address_space.root != stack_child_root ||
        stack_child.object.refcount != stack_child_refs ||
        process_lookup(stack_child.pid) != &stack_child)
        return -41;

    if (process_wait(&parent, stack_child.pid, &stack_exit) != 0 ||
        stack_exit != 29 || process_lookup(stack_child.pid) != 0 ||
        stack_child.address_space.root != 0 ||
        stack_child.object.refcount != 0 ||
        stack_thread.kernel_stack_base != 0 ||
        stack_thread.kernel_stack_pages[0] != 0u ||
        stack_thread.registry_next != 0 ||
        stack_peer_thread.kernel_stack_base != 0 ||
        stack_peer_thread.kernel_stack_pages[0] != 0u ||
        stack_peer_thread.registry_next != 0)
        return -42;

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

    /*
     * Direct unregister must not bypass the parent-lifetime rule, even when
     * the child is already DEAD. The child may then be unregistered first.
     */
    struct pros_process unregister_parent;
    struct pros_process unregister_child;
    if (process_create(0, &unregister_parent, 1010u,
                       PROS_SECURITY_SUPER) != 0 ||
        process_create(&unregister_parent, &unregister_child, 1011u,
                       PROS_SECURITY_LIMITED) != 0)
        return -52;
    unregister_parent.state = PROS_PROCESS_DEAD;
    unregister_child.state = PROS_PROCESS_DEAD;
    if (process_release_resources(&unregister_parent) != 0 ||
        process_unregister(&unregister_parent) == 0)
        return -53;
    if (process_release_resources(&unregister_child) != 0 ||
        process_unregister(&unregister_child) != 0)
        return -54;
    if (process_unregister(&unregister_parent) != 0)
        return -55;

    /*
     * Direct unregister must reject a cyclic global thread registry without
     * consuming the DEAD process's final object reference. A clean retry
     * after the fixture restores the registry must then succeed.
     */
    struct pros_process registry_cycle_process;
    if (process_create(0, &registry_cycle_process, 1012u,
                       PROS_SECURITY_SUPER) != 0)
        return -56;
    registry_cycle_process.state = PROS_PROCESS_DEAD;
    if (process_release_resources(&registry_cycle_process) != 0 ||
        thread_process_unregister_registry_cycle_selftest(
            &registry_cycle_process) != 0)
        return -57;
    if (process_unregister(&registry_cycle_process) != 0)
        return -58;

    return 0;
}

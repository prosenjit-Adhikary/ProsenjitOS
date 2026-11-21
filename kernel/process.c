#include "process.h"
#include "memory.h"
#include "../../include/abi/syscall.h"
#include "paging.h"
#include "thread.h"
#include "ipc.h"
#include "elf_loader.h"
#include "security_monitor.h"
#include "heap.h"

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
/*
 * A private sidecar binds every published process address to a kernel-heap
 * allocation. The caller-owned pros_process storage remains compatible with
 * static/stack fixtures, but a copied or substituted supervisor-mapped object
 * cannot impersonate a published registry member merely by copying its PID,
 * type, refcount and list fields.
 */
struct process_identity_record {
    struct pros_process *owner;
    struct process_identity_record *next;
};
static struct process_identity_record *process_identity_list;
/* Trusted cardinality bounds every traversal without imposing a PID/process cap. */
static uint64_t process_registry_count;
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
    uint64_t visited = 0u;
    struct pros_process *it = process_list;
    for (; it && visited < process_registry_count;
         it = it->next, ++visited) {
        if (it == process->parent) {
            registered_parent = it;
            ++matches;
        }
    }
    if (it || visited != process_registry_count ||
        matches != 1u || !registered_parent)
        return -1;

    return process->parent_pid == registered_parent->pid ? 0 : -1;
}

/*
 * A registry link is untrusted until its complete process object is mapped
 * supervisor memory. Check every page before reading fields such as next;
 * this makes non-canonical, unmapped and user-mapped corrupt links fail
 * closed instead of faulting during cycle detection.
 */
static int process_registry_node_readable(const struct pros_process *process) {
    if (!process)
        return 0;

    uint64_t start = (uint64_t)(uintptr_t)process;
    if (start > UINT64_MAX - (uint64_t)sizeof(*process) + 1u)
        return 0;
    uint64_t end = start + (uint64_t)sizeof(*process) - 1u;
    uint64_t page = start & ~(PROSENJIT_PAGE_SIZE - 1u);
    uint64_t last_page = end & ~(PROSENJIT_PAGE_SIZE - 1u);

    for (;;) {
        uint64_t flags = 0u;
        if (paging_query_page(page, &flags) != 0 ||
            !(flags & PROSENJIT_PAGE_PRESENT) ||
            (flags & PROSENJIT_PAGE_USER))
            return 0;
        if (page == last_page)
            break;
        if (page > UINT64_MAX - PROSENJIT_PAGE_SIZE)
            return 0;
        page += PROSENJIT_PAGE_SIZE;
    }
    return 1;
}

/* Identity records are private heap allocations; validate before following a
 * possibly corrupted sidecar link. */
static int process_identity_record_readable(
    const struct process_identity_record *record) {
    /*
     * Identity records are allocated only by process_register(). A readable
     * supervisor mapping is not sufficient authentication: a forged pointer
     * to an unrelated kernel object or stack fixture could otherwise provide
     * plausible owner/next fields. Require the exact live heap payload.
     */
    return heap_is_live_allocation(record, sizeof(*record));
}

/*
 * The process list is a singly linked ownership registry. Establish that it
 * is acyclic before any lookup/unlink traversal, then verify exact target
 * membership and every registered parent edge. Interrupt exclusion is
 * supplied by lifecycle callers.
 */
static int process_registry_validate(const struct pros_process *target,
                                     int expected_present) {
    const uint64_t expected_count = process_registry_count;
    if ((expected_count == 0u) != (process_list == 0) ||
        (expected_count == 0u) != (process_identity_list == 0))
        return -1;

    /* Validate the private sidecar chain with the same trusted cardinality. */
    uint64_t identity_visited = 0u;
    struct process_identity_record *identity = process_identity_list;
    for (; identity && identity_visited < expected_count;
         identity = identity->next, ++identity_visited) {
        if (!process_identity_record_readable(identity) || !identity->owner)
            return -1;
    }
    if (identity || identity_visited != expected_count)
        return -1;

    struct pros_process *slow = process_list;
    struct pros_process *fast = process_list;
    uint64_t cycle_steps = 0u;
    while (fast && cycle_steps < expected_count) {
        if (!process_registry_node_readable(fast))
            return -1;
        struct pros_process *fast_next = fast->next;
        if (!fast_next) {
            fast = 0;
            break;
        }
        if (!process_registry_node_readable(fast_next) ||
            !process_registry_node_readable(slow))
            return -1;
        slow = slow->next;
        fast = fast_next->next;
        if (slow == fast)
            return -1;
        ++cycle_steps;
    }
    /* More links than the trusted publication count means corruption. */
    if (fast)
        return -1;

    uint32_t matches = 0u;
    uint64_t visited = 0u;
    struct pros_process *it = process_list;
    struct process_identity_record *identity_it = process_identity_list;
    for (; it && visited < expected_count;
         it = it->next, ++visited) {
        if (!process_registry_node_readable(it) ||
            !identity_it || !process_identity_record_readable(identity_it) ||
            identity_it->owner != it)
            return -1;
        identity_it = identity_it->next;
        if (it->object.type != PROS_OBJECT_PROCESS ||
            it->object.refcount == 0u || it->pid == 0u ||
            it->state > PROS_PROCESS_DEAD)
            return -1;
        if (it == target)
            ++matches;
        if (matches > 1u || process_parent_validate(it) != 0)
            return -1;
    }
    if (it || identity_it || visited != expected_count)
        return -1;
    return matches == (expected_present ? 1u : 0u) ? 0 : -1;
}

int process_manager_init(void) {
    uint64_t irq_flags = process_irq_save();
    /*
     * Manager initialization is valid only before the first publication or
     * after every process has been fully unregistered. Never discard live
     * registry links or a current-process reference during reinitialization.
     */
    if (process_list || process_identity_list || current_process ||
        process_registry_validate(0, 0) != 0) {
        process_irq_restore(irq_flags);
        return -1;
    }
    process_registry_count = 0u;
    next_pid = 1;
    process_irq_restore(irq_flags);
    return 0;
}

struct pros_process *process_lookup(uint64_t pid) {
    /*
     * Lookup is also used outside lifecycle transactions. Keep the registry
     * validation and pointer traversal in one IRQ-excluded snapshot so an
     * interrupt-side publication or reap cannot change links mid-walk.
     */
    uint64_t irq_flags = process_irq_save();
    if (process_registry_validate(0, 0) != 0) {
        process_irq_restore(irq_flags);
        return 0;
    }
    struct pros_process *found = 0;
    for (struct pros_process *p = process_list; p; p = p->next) {
        if (p->pid == pid && p->state != PROS_PROCESS_DEAD) {
            found = p;
            break;
        }
    }
    process_irq_restore(irq_flags);
    return found;
}

int process_register(struct pros_process *process) {
    /*
     * Publication is a single registry transaction. Lookup, parent validation,
     * PID uniqueness, and list insertion must observe one IRQ-excluded state;
     * callers may already hold the same exclusion, so save/restore preserves
     * the caller's original IF state.
     */
    uint64_t irq_flags = process_irq_save();
    if (!process_registry_node_readable(process) ||
        !process_is_live(process) || process->pid == 0 ||
        process_registry_count == UINT64_MAX ||
        process_registry_validate(process, 0) != 0 ||
        process_parent_validate(process) != 0 ||
        process_pid_registered(process->pid)) {
        process_irq_restore(irq_flags);
        return -1;
    }
    struct process_identity_record *identity =
        (struct process_identity_record *)kmalloc(sizeof(*identity));
    if (!identity) {
        process_irq_restore(irq_flags);
        return -1;
    }
    identity->owner = process;
    identity->next = process_identity_list;
    process_identity_list = identity;
    process->next = process_list;
    process_list = process;
    ++process_registry_count;
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
        process_has_children(process) ||
        process->state != PROS_PROCESS_DEAD ||
        process->address_space.root != 0 || process->object.refcount != 1 ||
        thread_process_has_live(process) || thread_process_is_current(process) ||
        !thread_process_reapable(process)) {
        process_irq_restore(irq_flags);
        return -1;
    }
    struct pros_process **link = &process_list;
    while (*link != process)
        link = &(*link)->next;
    struct process_identity_record **identity_link = &process_identity_list;
    while (*identity_link && (*identity_link)->owner != process)
        identity_link = &(*identity_link)->next;
    if (!*identity_link) {
        process_irq_restore(irq_flags);
        return -1;
    }
    struct process_identity_record *identity = *identity_link;

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
    *identity_link = identity->next;
    identity->owner = 0;
    identity->next = 0;
    kfree(identity);
    --process_registry_count;
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
    /*
     * Public process operations may receive a caller-supplied pointer.
     * Authenticate registry identity before process_is_live() reads object
     * fields; mapped kernel memory alone does not establish process identity.
     */
    return process && process_registry_validate(process, 1) == 0 &&
           process_is_live(process) &&
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
     * Keep admission, initialization, and publication in one lifecycle
     * snapshot. In particular, an apparently active but unregistered parent
     * must be rejected before child storage or address-space state is changed.
     */
    uint64_t irq_flags = process_irq_save();
    if (!child || pid == 0 || child == parent ||
        process_registry_validate(child, 0) != 0 ||
        (parent && (process_registry_validate(parent, 1) != 0 ||
                    !process_is_active(parent))) ||
        process_pid_registered(pid)) {
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
        if (process_release_resources(child) == 0)
            (void)object_release(&child->object);
        process_irq_restore(irq_flags);
        return -1;
    }

    /*
     * Security-monitor state is keyed by PID. A PID may be reused only
     * after the previous process has been fully reaped, so discard any
     * historical monitor score before the new lifetime becomes observable.
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

    /*
     * Keep admission checks individually diagnosable. The staged rollback
     * regression reuses static child/thread storage across failpoints; a
     * single aggregate -1 concealed which lifecycle invariant rejected it.
     */
    int admission_error = 0;
    if (!child || !thread || child == parent || pid == 0 || tid == 0)
        admission_error = 1;
    else if (!image || image_size == 0)
        admission_error = 2;
    else if (process_registry_validate(child, 0) != 0)
        admission_error = 3;
    else if (security_level != PROS_SECURITY_LIMITED &&
             security_level != PROS_SECURITY_SUPER)
        admission_error = 4;
    else if (parent && process_registry_validate(parent, 1) != 0)
        admission_error = 5;
    else if (parent && !process_is_active(parent))
        admission_error = 6;
    else if (parent && parent->quarantined)
        admission_error = 7;
    else if (parent && parent->state == PROS_PROCESS_BLOCKED)
        admission_error = 8;
    else if (parent && parent->security_level < security_level)
        admission_error = 9;
    else if (process_lookup(pid) != 0)
        admission_error = 10;
    if (admission_error) {
        process_irq_restore(irq_flags);
        /*
         * Failpoint-6 probe: distinguish an unexpected zero stage argument
         * from ordinary admission rejection. This PID is reserved exclusively
         * by process_spawn_rollback_selftest().
         */
        if (fail_stage == 0u) {
            if (pid == 0xC106u)
                return -6800 - admission_error;
            return -6900 - admission_error;
        }
        return -6000 - (int)(fail_stage * 100u) - admission_error;
    }
    if (process_init(child, pid, security_level) != 0) {
        process_irq_restore(irq_flags);
        return fail_stage == 6u ? -6200 : -2;
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
        return fail_stage == 6u ? -6300 : -5;
    }
    process_irq_restore(irq_flags);
    return fail_stage == 6u ? -6400 :
           (fail_stage ? -100 - (int)fail_stage : -4);

rollback_unregistered:
    if (process_spawn_discard_unregistered(child, thread,
                                          thread_initialized) != 0) {
        process_irq_restore(irq_flags);
        return fail_stage == 6u ? -6301 : -6;
    }
    process_irq_restore(irq_flags);
    /* Never let a requested failpoint hide an earlier setup failure. */
    if (fail_stage == 6u)
        return -6401;
    return failure_code ? failure_code :
           (fail_stage ? -100 - (int)fail_stage : -7);
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

    if (!parent)
        return -7101;
    if (!image)
        return -7102;
    if (image_size == 0)
        return -7103;

    /*
     * The direct spawn entry point must reject an already-registered process
     * destination before process_init() can overwrite its live resources.
     * Use a distinct registered child (not child == parent) to exercise the
     * registry-membership guard, then prove it remains normally reapable.
     */
    static struct pros_process registered_probe;
    static struct pros_thread registered_probe_thread;
    const uint64_t registered_probe_pid = 0xC0F0u;
    if (process_create(parent, &registered_probe, registered_probe_pid,
                       PROS_SECURITY_LIMITED) != 0)
        return -7;
    uint64_t registered_probe_root =
        (uint64_t)(uintptr_t)registered_probe.address_space.root;
    uint32_t registered_probe_refs = registered_probe.object.refcount;
    int spawn_reuse_rc = process_spawn_internal(
        parent, &registered_probe, &registered_probe_thread,
        registered_probe_pid + 1u, registered_probe_pid + 1u,
        PROS_SECURITY_LIMITED, image, image_size, argv, argc, envp, envc, 1u);
    if (spawn_reuse_rc == 0 ||
        registered_probe.pid != registered_probe_pid ||
        registered_probe.state != PROS_PROCESS_READY ||
        registered_probe.address_space.root !=
            (void *)(uintptr_t)registered_probe_root ||
        registered_probe.object.refcount != registered_probe_refs ||
        registered_probe.parent != parent ||
        registered_probe.parent_pid != parent->pid ||
        process_lookup(registered_probe_pid) != &registered_probe)
        return -8;
    int32_t registered_probe_exit = 0;
    if (process_exit(&registered_probe, 0) != 0 ||
        process_wait(parent, registered_probe_pid,
                     &registered_probe_exit) != 0 ||
        registered_probe_exit != 0 ||
        process_lookup(registered_probe_pid) != 0)
        return -9;

    for (uint32_t stage = 1u; stage <= 5u; ++stage) {
        uint64_t pid = 0xC100u + stage;
        int rc = process_spawn_internal(parent, &child, &thread, pid, pid,
                                        PROS_SECURITY_LIMITED, image,
                                        image_size, argv, argc, envp, envc,
                                        stage);
        if (rc != -100 - (int)stage)
            return -2000 - (int)(stage * 100u) - rc;
        if (process_lookup(pid) != 0)
            return -2100 - (int)stage;
        if (child.address_space.root != 0)
            return -2200 - (int)stage;
        if (child.object.refcount != 0)
            return -2300 - (int)stage;
        if (child.next != 0)
            return -2400 - (int)stage;
        if (thread.kernel_stack_base != 0)
            return -2500 - (int)stage;
        if (thread.process != 0)
            return -2600 - (int)stage;
        if (thread.registry_next != 0)
            return -2700 - (int)stage;
        if (thread.next != 0 || thread.prev != 0)
            return -2800 - (int)stage;
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
    if (rejected != -6)
        return ((rejected <= -6200 && rejected >= -6610) ||
                (rejected <= -6800 && rejected >= -6810) ||
                (rejected <= -6900 && rejected >= -6910)) ? rejected :
               -300 - rejected;
    if (process_lookup(preflight_pid) != 0)
        return -32;
    if (child.state != PROS_PROCESS_NEW)
        return -33;
    if (child.address_space.root == 0)
        return -34;
    if (child.object.refcount == 0)
        return -35;
    if (thread.process != &child)
        return -36;
    if (thread.state != PROS_THREAD_NEW)
        return -37;
    if (thread.wait_channel != (void *)(uintptr_t)1u)
        return -38;
    if (thread.kernel_stack_base == 0)
        return -39;
    if (thread.kernel_stack_size == 0)
        return -40;

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
    /*
     * Only a published process may enter the terminal lifecycle. A merely
     * initialized caller-owned object can be live/refcounted without owning a
     * registry identity; transitioning it to ZOMBIE would create an orphan
     * that no parent can wait for or unregister through the normal path.
     */
    if (!process || process_registry_validate(process, 1) != 0 ||
        !process_is_live(process) ||
        process->state == PROS_PROCESS_DEAD ||
        process->state == PROS_PROCESS_ZOMBIE ||
        process_has_children(process)) {
        process_irq_restore(irq_flags);
        return -1;
    }

    /*
     * Validate every process-owned thread and external IPC waiter link before
     * changing CR3/current_process. A failed termination preflight must leave
     * the still-running process fully active in its original address space.
     */
    if (thread_terminate_process_preflight(process) != 0) {
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
    if (!parent || process_registry_validate(parent, 1) != 0 ||
        !process_is_active(parent) || !exit_code || pid == 0) {
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
    if (!process || process_registry_validate(process, 1) != 0 ||
        !process_is_live(process) ||
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
    return process && process_registry_validate(process, 1) == 0 &&
                   process_is_live(process)
               ? process->state
               : PROS_PROCESS_DEAD;
}

uint64_t process_allowed_capabilities(const struct pros_process *process) {
    return process && process_registry_validate(process, 1) == 0 &&
                   process_is_live(process)
               ? process->capability_policy
               : 0;
}

uint64_t process_allowed_syscalls(const struct pros_process *process) {
    return process && process_registry_validate(process, 1) == 0 &&
                   process_is_live(process)
               ? process->syscall_policy
               : 0;
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
    /* A forged non-NULL pointer must not be treated as a request to clear the
     * current process, which would unnecessarily switch the active CR3. */
    if (process && process_registry_validate(process, 1) != 0)
        return;
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

/*
 * Verify that a failed IPC waiter preflight leaves process_exit's current
 * process and CR3 untouched. The child is temporarily named current_process
 * without switching CR3; the deliberately missing waiter entry must reject
 * before process_exit attempts that transition.
 */
static int process_exit_ipc_preflight_selftest(struct pros_process *parent) {
    static struct pros_process child;
    static struct pros_thread thread;
    static struct pros_ipc_channel channel;
    if (!parent || child.object.refcount || child.address_space.root ||
        thread.kernel_stack_base || thread.registry_next)
        return -1;

    if (process_create(parent, &child, 0xC1F0u,
                       PROS_SECURITY_LIMITED) != 0)
        return -2;
    if (thread_init(&thread, &child, 0xC1F0u,
                    (uint64_t)(uintptr_t)process_selftest) != 0)
        return -3;
    if (ipc_channel_init(&channel) != PROSENJIT_IPC_OK)
        return -4;

    thread.state = PROS_THREAD_BLOCKED;
    thread.wait_channel = &channel;

    /*
     * A non-null but unregistered pointer is not a valid channel waiter even
     * when the target thread is also present exactly once. The registered
     * target with a matching BLOCKED/channel back-pointer must pass.
     */
    channel.waiters[0] = (struct pros_thread *)(uintptr_t)1u;
    channel.waiters[1] = &thread;
    channel.waiter_count = 2u;
    if (ipc_channel_cancel_waiter_preflight(&channel, &thread) !=
        PROSENJIT_IPC_EINVAL)
        return -9;

    channel.waiters[0] = &thread;
    channel.waiters[1] = 0;
    channel.waiter_count = 1u;
    if (ipc_channel_cancel_waiter_preflight(&channel, &thread) !=
        PROSENJIT_IPC_OK)
        return -10;

    channel.waiters[0] = 0;
    channel.waiter_count = 0u;
    struct pros_process *saved_current = current_process;
    current_process = &child;

    uint64_t cr3_before = 0u;
    uint64_t cr3_after = 0u;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_before));
    int exit_rc = process_exit(&child, 0x51);
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_after));

    int rejected_without_mutation =
        exit_rc != 0 && current_process == &child &&
        child.state == PROS_PROCESS_READY &&
        thread.state == PROS_THREAD_BLOCKED &&
        thread.wait_channel == &channel &&
        channel.waiter_count == 0u && cr3_after == cr3_before;
    current_process = saved_current;
    if (!rejected_without_mutation)
        return -5;

    thread.wait_channel = 0;
    thread.state = PROS_THREAD_NEW;
    if (thread_abort_unpublished(&thread) != 0)
        return -6;
    if (process_release_resources(&child) != 0)
        return -7;
    child.state = PROS_PROCESS_DEAD;
    if (process_unregister(&child) != 0)
        return -8;
    return 0;
}

/*
 * Direct process_unregister() must prove that every dead thread stack is
 * reclaimable before it switches away from current_process. A synthetic TSS
 * RSP0 alias makes the aggregate thread preflight reject; the CR3 and current
 * process must remain unchanged, and a clean retry must then reap successfully.
 */
static int process_unregister_stack_preflight_selftest(
    struct pros_process *parent) {
    static struct pros_process child;
    static struct pros_thread thread;
    if (!parent || child.object.refcount || child.address_space.root ||
        thread.kernel_stack_base || thread.registry_next)
        return -1;

    if (process_create(parent, &child, 0xC1F1u,
                       PROS_SECURITY_LIMITED) != 0)
        return -2;
    if (thread_init(&thread, &child, 0xC1F1u,
                    (uint64_t)(uintptr_t)process_selftest) != 0)
        return -3;
    if (thread_terminate_process(&child) != 0 ||
        process_release_resources(&child) != 0)
        return -4;
    child.state = PROS_PROCESS_DEAD;

    struct pros_process *saved_current = process_current();
    uint64_t saved_tss_stack = cpu_get_kernel_stack();
    process_set_current(parent);
    if (process_current() != parent)
        return -5;

    current_process = &child;
    uint64_t cr3_before = 0u;
    uint64_t cr3_after = 0u;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_before));
    uint64_t child_stack_top =
        (uint64_t)(uintptr_t)thread.kernel_stack_base +
        thread.kernel_stack_size;
    if (!child_stack_top || cpu_set_kernel_stack(child_stack_top) != 0)
        return -6;

    int unregister_rc = process_unregister(&child);
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_after));
    int rejected_without_switch =
        unregister_rc != 0 && current_process == &child &&
        cr3_after == cr3_before &&
        process_registry_validate(&child, 1) == 0 &&
        child.object.refcount == 1u &&
        thread.state == PROS_THREAD_DEAD && thread.kernel_stack_base != 0;

    (void)cpu_set_kernel_stack(saved_tss_stack);
    process_set_current(saved_current);
    if (!rejected_without_switch)
        return -7;
    if (process_current() != saved_current)
        return -8;
    if (process_unregister(&child) != 0)
        return -9;
    return 0;
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

    /*
     * process_init() creates a live object/address space but does not publish
     * it. Such an object must not enter ZOMBIE: it has no registry identity,
     * parent-visible PID lifetime, or supported wait/unregister path.
     */
    static struct pros_process unpublished;
    if (unpublished.object.refcount || unpublished.address_space.root)
        return -75;
    if (process_init(&unpublished, 0xC1EFu, PROS_SECURITY_LIMITED) != 0)
        return -76;
    int unpublished_state_rc =
        process_set_state(&unpublished, PROS_PROCESS_READY);
    int unpublished_exit_rc = process_exit(&unpublished, 0xE1);
    int unpublished_exit_rejected =
        unpublished_state_rc != 0 && unpublished_exit_rc != 0 &&
        unpublished.state == PROS_PROCESS_NEW &&
        process_registry_validate(&unpublished, 0) == 0;
    if (process_release_resources(&unpublished) != 0 ||
        object_release(&unpublished.object) != 0 ||
        !unpublished_exit_rejected)
        return -77;

    /*
     * A corrupt handle object pointer must be rejected by the teardown
     * preflight without dereferencing a non-canonical address.
     */
    static struct pros_handle_table forged_handle_table;
    handle_table_init(&forged_handle_table);
    forged_handle_table.entries[1].object =
        (struct pros_object *)(uintptr_t)0x0000800000000000ULL;
    forged_handle_table.entries[1].rights = 1u;
    if (handle_table_close_all_preflight(&forged_handle_table) == 0)
        return -80;
    handle_table_init(&forged_handle_table);

    if (process_create(0, &parent, 1001u, PROS_SECURITY_SUPER) != 0)
        return -2;
    const uint64_t registry_count_before_child = process_registry_count;
    if (parent.state != PROS_PROCESS_READY ||
        process_lookup(1001u) != &parent)
        return -3;
    if (process_create(&parent, &child, 1002u, PROS_SECURITY_LIMITED) != 0)
        return -4;

    /* Reject an untrusted parent address before reading process fields. */
    static struct pros_process invalid_parent_child;
    struct pros_process *invalid_parent =
        (struct pros_process *)(uintptr_t)0x0000800000000000ULL;
    if (process_create(invalid_parent, &invalid_parent_child, 0x100Au,
                       PROS_SECURITY_LIMITED) == 0 ||
        invalid_parent_child.object.refcount != 0u ||
        invalid_parent_child.address_space.root != 0 ||
        process_wait(invalid_parent, child.pid, &exit_code) == 0 ||
        process_exit(invalid_parent, 0xE2) == 0 ||
        process_set_state(invalid_parent, PROS_PROCESS_READY) == 0 ||
        process_state(invalid_parent) != PROS_PROCESS_DEAD ||
        process_allowed_capabilities(invalid_parent) != 0u ||
        process_allowed_syscalls(invalid_parent) != 0u ||
        process_set_syscall_policy(invalid_parent, 0u) == 0 ||
        process_apply_syscall_policy(&parent, invalid_parent, 0u) == 0)
        return -81;

    /* Active-process APIs and scheduler selection must authenticate before
     * dereferencing a caller-supplied process pointer. A forged pointer must
     * not change the current process or active address-space root. */
    struct pros_process *saved_current = process_current();
    uint64_t cr3_before_invalid_current = 0u;
    uint64_t cr3_after_invalid_current = 0u;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_before_invalid_current));
    process_set_current(invalid_parent);
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_after_invalid_current));
    if (process_current() != saved_current ||
        cr3_after_invalid_current != cr3_before_invalid_current)
        return -82;

    /*
     * Registry membership is bound to a private kernel-heap identity record,
     * not merely copied process fields. Substitute a byte-for-byte process
     * copy at the same list position: the list cardinality and links remain
     * plausible, but the copied object must not authenticate as the parent.
     */
    struct pros_process forged_parent_copy = parent;
    struct pros_process *saved_child_next = child.next;
    forged_parent_copy.next = parent.next;
    child.next = &forged_parent_copy;
    int forged_copy_accepted =
        process_registry_validate(&parent, 1) == 0;
    child.next = saved_child_next;
    if (forged_copy_accepted ||
        process_registry_validate(&parent, 1) != 0)
        return -74;

    /* A cycle in the private identity chain must reject within its count bound. */
    struct process_identity_record *saved_identity_next =
        process_identity_list->next;
    process_identity_list->next = process_identity_list;
    int identity_cycle_accepted = process_registry_validate(0, 0) == 0;
    process_identity_list->next = saved_identity_next;
    if (identity_cycle_accepted || process_registry_validate(0, 0) != 0)
        return -78;

    /*
     * A supervisor-readable stack object is not a valid identity sidecar.
     * Rebuild a structurally plausible chain with forged stack records and
     * require heap-allocation identity to reject it before owner/next use.
     */
    struct process_identity_record forged_parent_identity;
    struct process_identity_record forged_child_identity;
    forged_child_identity.owner = &child;
    forged_child_identity.next = &forged_parent_identity;
    forged_parent_identity.owner = &parent;
    forged_parent_identity.next = 0;
    struct process_identity_record *saved_identity_head =
        process_identity_list;
    process_identity_list = &forged_child_identity;
    int forged_identity_accepted = process_registry_validate(0, 0) == 0;
    process_identity_list = saved_identity_head;
    if (forged_identity_accepted || process_registry_validate(0, 0) != 0)
        return -79;

    /*
     * If the private identity sidecar cannot be allocated, process_create()
     * must roll back the already-created address space and object without
     * publishing a PID or leaving a registry record behind.
     */
    /* Keep this large handle-table-bearing fixture off the boot stack. */
    static struct pros_process identity_alloc_failure;
    if (identity_alloc_failure.object.refcount != 0u ||
        identity_alloc_failure.address_space.root != 0)
        return -77;
    heap_test_fail_after(0u);
    int identity_alloc_rc =
        process_create(&parent, &identity_alloc_failure, 0x1009u,
                       PROS_SECURITY_LIMITED);
    int identity_alloc_failed = heap_test_fail_was_triggered();
    heap_test_fail_clear();
    if (identity_alloc_rc == 0 || !identity_alloc_failed ||
        identity_alloc_failure.object.refcount != 0u ||
        identity_alloc_failure.address_space.root != 0 ||
        process_lookup(0x1009u) != 0 ||
        process_registry_validate(0, 0) != 0)
        return -76;

    /*
     * An initialized but unpublished process is not a valid parent. Rejection
     * must happen before process_create mutates the supplied child storage.
     */
    struct pros_process unregistered_parent;
    struct pros_process rejected_child = {0};
    if (process_init(&unregistered_parent, 0x1007u,
                     PROS_SECURITY_LIMITED) != 0)
        return -69;
    if (process_create(&unregistered_parent, &rejected_child, 0x1008u,
                       PROS_SECURITY_LIMITED) == 0 ||
        rejected_child.object.refcount != 0u ||
        rejected_child.address_space.root != 0 ||
        rejected_child.next != 0 || rejected_child.parent != 0) {
        (void)address_space_destroy(&unregistered_parent.address_space);
        (void)object_release(&unregistered_parent.object);
        return -70;
    }
    if (address_space_destroy(&unregistered_parent.address_space) != 0 ||
        object_release(&unregistered_parent.object) != 0)
        return -71;

    int ipc_preflight_rc = process_exit_ipc_preflight_selftest(&parent);
    if (ipc_preflight_rc != 0)
        return -6700 + ipc_preflight_rc;
    int unregister_stack_rc =
        process_unregister_stack_preflight_selftest(&parent);
    if (unregister_stack_rc != 0)
        return -6800 + unregister_stack_rc;

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

    /*
     * The trusted registry cardinality is the traversal bound. A mismatched
     * count must reject an otherwise acyclic list before any lifecycle action.
     */
    uint64_t saved_registry_count = process_registry_count;
    if (saved_registry_count == 0u)
        return -72;
    process_registry_count = saved_registry_count - 1u;
    int count_mismatch_accepted =
        process_registry_validate(0, 0) == 0;
    process_registry_count = saved_registry_count;
    if (count_mismatch_accepted ||
        process_registry_validate(0, 0) != 0)
        return -73;

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
        process_lookup(child.pid) != 0 ||
        process_registry_count != registry_count_before_child ||
        process_registry_validate(0, 0) != 0)
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

    /*
     * An arbitrary non-canonical next pointer must be rejected by the
     * mapping preflight before cycle detection dereferences it. The same
     * zombie must remain intact and be reapable after restoring the link.
     */
    parent.next = (struct pros_process *)(uintptr_t)0x0000800000000000ULL;
    if (process_registry_validate(&cycle_child, 1) == 0 ||
        process_lookup(cycle_child.pid) != 0 ||
        process_wait(&parent, cycle_child.pid, &exit_code) == 0 ||
        cycle_child.state != PROS_PROCESS_ZOMBIE ||
        cycle_child.address_space.root == 0 ||
        cycle_child.object.refcount != 1u) {
        parent.next = saved_parent_next;
        return -64;
    }
    parent.next = saved_parent_next;

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

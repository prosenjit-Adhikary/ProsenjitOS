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
static uint64_t process_irq_save(void);
static void process_irq_restore(uint64_t flags);
#define PROS_PROCESS_REGISTRY_CAPACITY 64u
static struct pros_process *process_list;
/*
 * Trusted identity slots bound registry traversal and authenticate each
 * linked-list node before reading its next pointer. Only register/unregister
 * update this side table.
 */
static struct pros_process *process_registry_slots[PROS_PROCESS_REGISTRY_CAPACITY];
static uint32_t process_registry_count;
static uint64_t next_pid = 1;

/* The trusted slot table authenticates identity, not memory accessibility. */
static int process_registry_node_writable(const struct pros_process *process) {
    if (!process)
        return 0;
    uint64_t start = (uint64_t)(uintptr_t)process;
    if (start > UINT64_MAX - sizeof(*process) + 1u)
        return 0;
    uint64_t end = start + sizeof(*process) - 1u;
    uint64_t page = start & ~(PROSENJIT_PAGE_SIZE - 1u);
    uint64_t last_page = end & ~(PROSENJIT_PAGE_SIZE - 1u);
    for (;;) {
        uint64_t flags = 0u;
        if (paging_query_effective_page(page, &flags) != 0 ||
            !(flags & PROSENJIT_PAGE_PRESENT) ||
            !(flags & PROSENJIT_PAGE_WRITABLE) ||
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

static int process_registry_validate(void) {
    uint8_t seen[PROS_PROCESS_REGISTRY_CAPACITY] = {0};
    struct pros_process *nodes[PROS_PROCESS_REGISTRY_CAPACITY];
    uint32_t traversed = 0u;

    for (struct pros_process *it = process_list; it;) {
        uint32_t slot = PROS_PROCESS_REGISTRY_CAPACITY;
        for (uint32_t i = 0u; i < PROS_PROCESS_REGISTRY_CAPACITY; ++i) {
            if (process_registry_slots[i] == it) {
                if (slot != PROS_PROCESS_REGISTRY_CAPACITY)
                    return -1;
                slot = i;
            }
        }
        if (slot == PROS_PROCESS_REGISTRY_CAPACITY ||
            seen[slot] || traversed >= PROS_PROCESS_REGISTRY_CAPACITY ||
            !process_registry_node_writable(it) ||
            !object_validate_header(&it->object) ||
            it->object.type != PROS_OBJECT_PROCESS ||
            it->pid == 0u || it->state > PROS_PROCESS_DEAD)
            return -1;
        for (uint32_t i = 0u; i < traversed; ++i)
            if (nodes[i]->pid == it->pid)
                return -1;
        seen[slot] = 1u;
        nodes[traversed++] = it;
        it = it->next;
    }

    if (traversed != process_registry_count)
        return -1;

    /* Parent links are trusted only when they resolve to another registered
     * process and the duplicated parent PID agrees with that identity. */
    for (uint32_t i = 0u; i < traversed; ++i) {
        struct pros_process *node = nodes[i];
        if (!node->parent) {
            if (node->parent_pid != 0u)
                return -1;
            continue;
        }
        if (node->parent == node || node->parent_pid == 0u)
            return -1;
        uint32_t parent_index = traversed;
        for (uint32_t j = 0u; j < traversed; ++j)
            if (nodes[j] == node->parent) {
                parent_index = j;
                break;
            }
        if (parent_index == traversed ||
            nodes[parent_index]->pid != node->parent_pid)
            return -1;

        const struct pros_process *ancestor = node;
        for (uint32_t depth = 0u; ancestor->parent; ++depth) {
            if (depth >= PROS_PROCESS_REGISTRY_CAPACITY)
                return -1;
            const struct pros_process *next = ancestor->parent;
            uint32_t next_index = traversed;
            for (uint32_t j = 0u; j < traversed; ++j)
                if (nodes[j] == next) {
                    next_index = j;
                    break;
                }
            if (next_index == traversed || next == node ||
                ancestor->parent_pid != nodes[next_index]->pid)
                return -1;
            ancestor = next;
        }
    }

    for (uint32_t i = 0u; i < PROS_PROCESS_REGISTRY_CAPACITY; ++i)
        if ((process_registry_slots[i] != 0) != (seen[i] != 0u))
            return -1;
    return 0;
}

int process_manager_init(void) {
    process_list = 0;
    current_process = 0;
    process_registry_count = 0u;
    for (uint32_t i = 0u; i < PROS_PROCESS_REGISTRY_CAPACITY; ++i)
        process_registry_slots[i] = 0;
    next_pid = 1;
    return 0;
}

struct pros_process *process_lookup(uint64_t pid) {
    if (process_registry_validate() != 0)
        return 0;
    for (struct pros_process *p = process_list; p; p = p->next)
        if (p->pid == pid && p->state != PROS_PROCESS_DEAD)
            return p;
    return 0;
}

int process_register(struct pros_process *process) {
    if (process_registry_validate() != 0 ||
        process_registry_count >= PROS_PROCESS_REGISTRY_CAPACITY ||
        !process_is_live(process) ||
        process->state > PROS_PROCESS_BLOCKED || process->pid == 0 ||
        process_lookup(process->pid) != 0)
        return -1;
    /* A DEAD entry still owns its PID until unregister removes its slot. */
    for (struct pros_process *it = process_list; it; it = it->next)
        if (it->pid == process->pid)
            return -1;
    if (!process->parent) {
        if (process->parent_pid != 0u)
            return -1;
    } else if (!process_registry_node_writable(process->parent) ||
               !object_validate_header(&process->parent->object) ||
               process->parent->object.type != PROS_OBJECT_PROCESS ||
               process->parent->state == PROS_PROCESS_ZOMBIE ||
               process->parent->state == PROS_PROCESS_DEAD ||
               process->parent->pid != process->parent_pid ||
               process_lookup(process->parent->pid) != process->parent) {
        return -1;
    }
    uint32_t slot = PROS_PROCESS_REGISTRY_CAPACITY;
    for (uint32_t i = 0u; i < PROS_PROCESS_REGISTRY_CAPACITY; ++i) {
        if (process_registry_slots[i] == process)
            return -1;
        if (!process_registry_slots[i] &&
            slot == PROS_PROCESS_REGISTRY_CAPACITY)
            slot = i;
    }
    if (slot == PROS_PROCESS_REGISTRY_CAPACITY)
        return -1;
    process->next = process_list;
    process_list = process;
    process_registry_slots[slot] = process;
    ++process_registry_count;
    if (process->pid >= next_pid) {
        if (process->pid == UINT64_MAX)
            next_pid = UINT64_MAX;
        else
            next_pid = process->pid + 1;
    }
    return 0;
}

static int process_unregister_unlocked(struct pros_process *process) {
    if (!process || process_registry_validate() != 0)
        return -1;
    struct pros_process **link = &process_list;
    uint32_t traversed = 0u;
    while (*link && *link != process &&
           traversed < PROS_PROCESS_REGISTRY_CAPACITY) {
        link = &(*link)->next;
        ++traversed;
    }
    if (!*link || traversed >= PROS_PROCESS_REGISTRY_CAPACITY ||
        process->state != PROS_PROCESS_DEAD ||
        process->address_space.root != 0 ||
        process->address_space.owned_pages != 0 ||
        process->object.type != PROS_OBJECT_PROCESS ||
        !object_validate_header(&process->object) ||
        process->object.refcount != 1 ||
        thread_process_reap_preflight(process) != 0)
        return -1;

    /* A parent remains registered until every child, including dead-but-not-
     * yet-unregistered children, has left the registry. */
    for (struct pros_process *it = process_list; it; it = it->next)
        if (it != process && it->parent == process)
            return -1;

    uint32_t slot = PROS_PROCESS_REGISTRY_CAPACITY;
    for (uint32_t i = 0u; i < PROS_PROCESS_REGISTRY_CAPACITY; ++i)
        if (process_registry_slots[i] == process) {
            slot = i;
            break;
        }
    if (slot == PROS_PROCESS_REGISTRY_CAPACITY)
        return -1;

    if (current_process == process) {
        if (paging_switch_kernel() != 0)
            return -1;
        current_process = 0;
    }

    if (thread_reap_process(process) != 0)
        return -1;

    /*
     * Release the final object reference while the process remains registered.
     * If release rejects, the dead process remains discoverable and a retry
     * can finish unregister after its already-reaped thread set is empty.
     * Once the object is released, the following list/slot unlink is bounded
     * metadata-only and cannot fail.
     */
    (void)pros_security_monitor_clear(process->pid);
    if (object_release(&process->object) != 0)
        return -1;

    *link = process->next;
    process_registry_slots[slot] = 0;
    --process_registry_count;
    process->next = 0;
    return 0;
}

int process_unregister(struct pros_process *process) {
    uint64_t irq_flags = process_irq_save();
    int rc = process_unregister_unlocked(process);
    process_irq_restore(irq_flags);
    return rc;
}

int process_registry_descriptors_outside(
    const uint64_t *range_bases, const uint64_t *range_tops,
    uint32_t range_count) {
    if ((range_count != 0u && (!range_bases || !range_tops)) ||
        range_count > 64u ||
        process_registry_validate() != 0)
        return -1;
    for (uint32_t i = 0u; i < range_count; ++i)
        if (range_bases[i] >= range_tops[i])
            return -1;
    for (const struct pros_process *it = process_list; it; it = it->next) {
        uint64_t start = (uint64_t)(uintptr_t)it;
        if (start > UINT64_MAX - sizeof(*it))
            return -1;
        uint64_t end = start + sizeof(*it);
        for (uint32_t i = 0u; i < range_count; ++i)
            if (start < range_tops[i] && range_bases[i] < end)
                return -1;
    }
    return 0;
}

static int process_is_live(const struct pros_process *process) {
    return process_registry_node_writable(process) &&
           object_validate_header(&process->object) &&
           process->object.type == PROS_OBJECT_PROCESS &&
           process->state <= PROS_PROCESS_DEAD;
}

static int process_is_active(const struct pros_process *process) {
    return process_is_live(process) &&
           process->state <= PROS_PROCESS_BLOCKED;
}

/* process_wait() is a kernel-internal API: never write an unchecked user VA. */
static int process_wait_output_writable(const int32_t *output) {
    if (!output)
        return 0;
    uint64_t start = (uint64_t)(uintptr_t)output;
    if (start > UINT64_MAX - sizeof(*output) + 1u)
        return 0;
    uint64_t end = start + sizeof(*output) - 1u;
    uint64_t page = start & ~(PROSENJIT_PAGE_SIZE - 1u);
    uint64_t last_page = end & ~(PROSENJIT_PAGE_SIZE - 1u);
    for (;;) {
        uint64_t flags = 0u;
        if (paging_query_effective_page(page, &flags) != 0 ||
            !(flags & PROSENJIT_PAGE_PRESENT) ||
            !(flags & PROSENJIT_PAGE_WRITABLE) ||
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

static int process_has_children(const struct pros_process *process) {
    if (!process) return 0;
    if (process_registry_validate() != 0) return 1;
    /* A dead child still owns a registry parent link until it is reaped. */
    for (struct pros_process *child = process_list; child; child = child->next)
        if (child->parent == process)
            return 1;
    return 0;
}

static int process_address_space_destroy_preflight(
    const struct pros_process *target) {
    if (!target || process_registry_validate() != 0)
        return -1;
    const struct pros_address_space *others[PROS_PROCESS_REGISTRY_CAPACITY];
    uint32_t other_count = 0u;
    for (const struct pros_process *it = process_list; it; it = it->next) {
        if (it == target)
            continue;
        if (other_count >= PROS_PROCESS_REGISTRY_CAPACITY)
            return -1;
        others[other_count++] = &it->address_space;
    }
    return address_space_destroy_preflight_against(
        &target->address_space, others, other_count);
}

static int process_release_resources(struct pros_process *process) {
    if (!process) return -1;
    if (handle_table_close_all(&process->handles) != 0) return -1;
    return address_space_destroy(&process->address_space);
}

int process_init(struct pros_process *process,
                 uint64_t pid,
                 uint32_t security_level) {
    if (!process_registry_node_writable(process)) return -1;
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
    if (!child || pid == 0) return -1;
    if (parent && (!process_is_active(parent) ||
                    process_lookup(parent->pid) != parent))
        return -1;
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
    uint64_t irq_flags = process_irq_save();
    if (!process_is_live(process) ||
        process->state == PROS_PROCESS_DEAD ||
        process->state == PROS_PROCESS_ZOMBIE ||
        process_lookup(process->pid) != process ||
        process_has_children(process) ||
        thread_process_is_current(process) ||
        thread_terminate_process_preflight(process) != 0 ||
        (thread_process_is_current(process) && current_process != process)) {
        process_irq_restore(irq_flags);
        return -1;
    }

    if (current_process == process) {
        /*
         * Complete all fallible thread/IPC checks before switching CR3.
         * IRQ exclusion keeps that preflight valid through the address-space
         * transition and the subsequent no-allocation termination commit.
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

    /* Publish the exit result only after the irreversible thread transition
     * has succeeded, keeping failed exits observationally non-destructive. */
    process->exit_code = exit_code;
    process->quarantined = 0u;
    process->quarantine_previous_state = 0u;
    process->state = PROS_PROCESS_ZOMBIE;
    process_irq_restore(irq_flags);
    return 0;
}
static uint64_t process_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void process_irq_restore(uint64_t flags) {
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

int process_wait(struct pros_process *parent, uint64_t pid, int32_t *exit_code) {
    uint64_t irq_flags = process_irq_save();
    int result = -1;
    if (!parent || process_registry_validate() != 0 ||
        !process_is_active(parent) || !object_validate_header(&parent->object) ||
        !process_wait_output_writable(exit_code) || pid == 0)
        goto out;

    struct pros_process *child = process_lookup(pid);
    if (!child || child->parent != parent || child->parent_pid != parent->pid)
        goto out;
    if (child->state != PROS_PROCESS_ZOMBIE) {
        result = -2;
        goto out;
    }

    /*
     * The process-owned handle table may contain handles to the process
     * object itself. Those references are discharged by close-all; references
     * from any other owner must be gone before resources are released.
     */
    if (!object_validate_header(&child->object) ||
        handle_table_close_all_preflight(&child->handles) != 0) {
        result = -3;
        goto out;
    }
    uint32_t self_handle_refs = 0u;
    for (uint32_t i = 1u; i < PROSENJIT_HANDLE_MAX; ++i)
        if (child->handles.entries[i].object == &child->object)
            ++self_handle_refs;
    /*
     * Establish every subsystem's complete read-only release gate before the
     * first handle reference, address-space mapping, stack page, or registry
     * link is changed. IRQ exclusion keeps this snapshot stable on the
     * current single-CPU lifecycle model.
     */
    if (child->object.refcount != self_handle_refs + 1u ||
        process_has_children(child) || thread_process_is_current(child) ||
        thread_process_reap_preflight(child) != 0 ||
        process_address_space_destroy_preflight(child) != 0) {
        result = -3;
        goto out;
    }

    int32_t child_exit_code = child->exit_code;
    if (process_release_resources(child) != 0) {
        result = -4;
        goto out;
    }
    child->state = PROS_PROCESS_DEAD;
    if (process_unregister(child) != 0) {
        /* Keep a fully reaped process visible if final unlinking fails. */
        child->state = PROS_PROCESS_ZOMBIE;
        result = -5;
        goto out;
    }
    *exit_code = child_exit_code;
    result = 0;
out:
    process_irq_restore(irq_flags);
    return result;
}

int process_set_state(struct pros_process *process, uint32_t state) {
    uint64_t irq_flags = process_irq_save();
    if (!process_is_live(process) || state > PROS_PROCESS_BLOCKED ||
        process->state >= PROS_PROCESS_ZOMBIE) {
        process_irq_restore(irq_flags);
        return -1;
    }
    /* A quarantined process must remain scheduler-blocked until the security
     * quarantine is explicitly cleared. Terminal transitions are owned only
     * by process_exit()/process_unregister(), never by generic state updates. */
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
    struct pros_process *saved_process_list = process_list;
    struct pros_process *saved_current_process = current_process;
    struct pros_process *saved_registry_slots[PROS_PROCESS_REGISTRY_CAPACITY];
    uint32_t saved_registry_count = process_registry_count;
    uint64_t saved_next_pid = next_pid;
    for (uint32_t i = 0u; i < PROS_PROCESS_REGISTRY_CAPACITY; ++i)
        saved_registry_slots[i] = process_registry_slots[i];

    if (process_manager_init() != 0)
        return -1;
    if (process_create(0, &parent, 1001u, PROS_SECURITY_SUPER) != 0)
        return -2;
    uint64_t registered_parent_pid = parent.pid;
    uint32_t registered_parent_state = parent.state;
    parent.pid = 1003u;
    parent.state = PROS_PROCESS_READY;
    int duplicate_identity_rejected = process_register(&parent) != 0;
    parent.pid = registered_parent_pid;
    parent.state = registered_parent_state;
    if (!duplicate_identity_rejected || process_registry_count != 1u ||
        process_list != &parent || process_registry_validate() != 0)
        return -36;
    if (parent.state != PROS_PROCESS_READY ||
        process_lookup(1001u) != &parent)
        return -3;
    if (process_create(&parent, &child, 1002u, PROS_SECURITY_LIMITED) != 0)
        return -4;
    if (process_set_state(&child, PROS_PROCESS_ZOMBIE) == 0 ||
        process_set_state(&child, PROS_PROCESS_DEAD) == 0 ||
        child.state != PROS_PROCESS_READY)
        return -42;

    /*
     * The trusted identity table must not turn an arbitrary canonical or
     * noncanonical address into a safe registry node. Reject it before the
     * validator reads the candidate node's next pointer.
     */
    struct pros_process *saved_process_head = process_list;
    process_list = (struct pros_process *)(uintptr_t)0x0000800000000000ULL;
    process_registry_slots[2] = process_list;
    int unreadable_node_rejected = process_registry_validate() != 0;
    process_list = saved_process_head;
    process_registry_slots[2] = 0;
    if (!unreadable_node_rejected || process_registry_validate() != 0)
        return -37;

    /*
     * Corrupt registry links must fail closed without dereferencing an
     * unregistered address or looping forever. Restoring the saved link must
     * make the same lookup succeed, proving rejection did not mutate state.
     */
    struct pros_process *saved_child_next = child.next;
    child.next = (struct pros_process *)(uintptr_t)1u;
    if (process_lookup(parent.pid) != 0 ||
        process_registry_count != 2u || process_registry_slots[0] != &parent ||
        process_registry_slots[1] != &child) {
        child.next = saved_child_next;
        return -32;
    }
    child.next = &child;
    if (process_lookup(parent.pid) != 0 ||
        process_registry_count != 2u || process_registry_slots[0] != &parent ||
        process_registry_slots[1] != &child) {
        child.next = saved_child_next;
        return -33;
    }
    child.next = saved_child_next;
    if (process_lookup(parent.pid) != &parent)
        return -34;
    if (parent.uid != 0u || parent.gid != 0u ||
        child.uid != 1002u || child.gid != 1002u)
        return -30;
    if (!parent.fs_identity_valid || parent.fs_uid != 1001u ||
        parent.fs_gid != 1001u || !child.fs_identity_valid ||
        child.fs_uid != 1002u || child.fs_gid != 1002u)
        return -31;
    if (child.parent != &parent || child.parent_pid != parent.pid)
        return -5;
    uint32_t saved_child_state = child.state;
    child.state = PROS_PROCESS_DEAD;
    struct pros_process duplicate_dead_pid;
    if (process_exit(&parent, 99) == 0 ||
        parent.state != PROS_PROCESS_READY ||
        process_create(&parent, &duplicate_dead_pid, child.pid,
                       PROS_SECURITY_LIMITED) == 0 ||
        process_registry_validate() != 0)
        return -41;
    child.state = saved_child_state;
    struct pros_process *saved_parent_link = child.parent;
    child.parent = (struct pros_process *)(uintptr_t)0x0000800000000000ULL;
    int invalid_parent_link_rejected = process_registry_validate() != 0;
    child.parent = saved_parent_link;
    if (!invalid_parent_link_rejected || process_registry_validate() != 0)
        return -40;
    uint64_t saved_child_pid = child.pid;
    child.pid = parent.pid;
    int duplicate_live_pid_rejected = process_registry_validate() != 0;
    child.pid = saved_child_pid;
    if (!duplicate_live_pid_rejected || process_registry_validate() != 0)
        return -39;
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
    if (process_wait(&parent, child.pid,
                     (int32_t *)(uintptr_t)0x0000800000000000ULL) == 0 ||
        child.state != PROS_PROCESS_ZOMBIE ||
        process_lookup(child.pid) != &child)
        return -38;
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

    /*
     * A malformed object header must be rejected before thread reap or PID
     * unlink. Restoring the header must allow the same unregister to retry.
     */
    parent.object.reserved = 1u;
    if (process_unregister(&parent) == 0 ||
        process_lookup(parent.pid) != 0 ||
        parent.next != 0 ||
        process_registry_count != 1u ||
        process_registry_slots[0] != &parent) {
        parent.object.reserved = 0u;
        return -35;
    }
    parent.object.reserved = 0u;
    if (process_registry_validate() != 0 ||
        process_lookup(parent.pid) != &parent ||
        process_unregister(&parent) != 0)
        return -14;

    /* Restore the live kernel registry after exercising a private test set. */
    process_list = saved_process_list;
    current_process = saved_current_process;
    process_registry_count = saved_registry_count;
    next_pid = saved_next_pid;
    for (uint32_t i = 0u; i < PROS_PROCESS_REGISTRY_CAPACITY; ++i)
        process_registry_slots[i] = saved_registry_slots[i];
    if (process_registry_validate() != 0 ||
        (saved_current_process &&
         process_lookup(saved_current_process->pid) != saved_current_process))
        return -15;
    return 0;
}

#include <stddef.h>
#include "thread.h"
#include "ipc.h"
#include "heap.h"
#include "memory.h"
#include "paging.h"

static struct pros_thread *runq_head;
static struct pros_thread *runq_tail;
static struct pros_thread *current_thread;
static struct pros_thread *thread_registry;

extern int cpu_set_kernel_stack(uint64_t stack_top);
extern uint64_t cpu_get_kernel_stack(void);
extern char stack_top[];

#define PROS_THREAD_STACK_ARENA_BASE 0xFFFF800800000000ULL
#define PROS_THREAD_STACK_SLOT_PAGES 6ULL
#define PROS_THREAD_STACK_SLOT_COUNT 64u
#define PROS_THREAD_STACK_PAGE_COUNT 4u

/* Single-CPU allocator until scheduler/run-queue locking is implemented. */
static uint64_t thread_stack_slots[(PROS_THREAD_STACK_SLOT_COUNT + 63u) / 64u];

static int thread_kernel_stack_allocate(struct pros_thread *thread) {
    uint32_t slot = PROS_THREAD_STACK_SLOT_COUNT;
    for (uint32_t i = 0; i < PROS_THREAD_STACK_SLOT_COUNT; ++i) {
        uint64_t mask = 1ULL << (i & 63u);
        if (!(thread_stack_slots[i >> 6] & mask)) {
            thread_stack_slots[i >> 6] |= mask;
            slot = i;
            break;
        }
    }
    if (slot == PROS_THREAD_STACK_SLOT_COUNT) return -1;

    uint64_t slot_base = PROS_THREAD_STACK_ARENA_BASE +
                         (uint64_t)slot * PROS_THREAD_STACK_SLOT_PAGES *
                         PROSENJIT_PAGE_SIZE;
    uint64_t stack_base = slot_base + PROSENJIT_PAGE_SIZE;
    uint64_t pages[PROS_THREAD_STACK_PAGE_COUNT] = {0};
    uint32_t mapped = 0u;

    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i) {
        pages[i] = physical_page_alloc();
        if (!pages[i]) goto rollback;
        if (paging_map_page(stack_base + (uint64_t)i * PROSENJIT_PAGE_SIZE,
                            pages[i],
                            PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0)
            goto rollback;
        ++mapped;
    }

    thread->kernel_stack_base = (void *)(uintptr_t)stack_base;
    thread->kernel_stack_size = PROS_THREAD_KERNEL_STACK_SIZE;
    thread->kernel_stack_slot = slot;
    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i)
        thread->kernel_stack_pages[i] = pages[i];
    return 0;

rollback:
    while (mapped != 0u) {
        --mapped;
        paging_unmap_page(stack_base + (uint64_t)mapped * PROSENJIT_PAGE_SIZE);
    }
    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i)
        if (pages[i]) physical_page_free(pages[i]);
    thread_stack_slots[slot >> 6] &= ~(1ULL << (slot & 63u));
    return -1;
}

static int thread_kernel_stack_valid(const struct pros_thread *thread) {
    if (!thread || thread->kernel_stack_slot >= PROS_THREAD_STACK_SLOT_COUNT ||
        !thread->kernel_stack_base ||
        thread->kernel_stack_size != PROS_THREAD_KERNEL_STACK_SIZE)
        return 0;

    uint32_t slot = thread->kernel_stack_slot;
    if (!(thread_stack_slots[slot >> 6] & (1ULL << (slot & 63u))))
        return 0;

    uint64_t slot_base = PROS_THREAD_STACK_ARENA_BASE +
                         (uint64_t)slot * PROS_THREAD_STACK_SLOT_PAGES *
                         PROSENJIT_PAGE_SIZE;
    uint64_t expected_base = slot_base + PROSENJIT_PAGE_SIZE;
    if ((uint64_t)(uintptr_t)thread->kernel_stack_base != expected_base ||
        paging_translate(slot_base) != 0u ||
        paging_translate(slot_base + 5u * PROSENJIT_PAGE_SIZE) != 0u)
        return 0;

    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i) {
        uint64_t va = expected_base + (uint64_t)i * PROSENJIT_PAGE_SIZE;
        if (!thread->kernel_stack_pages[i] ||
            paging_translate(va) != thread->kernel_stack_pages[i] ||
            !physical_page_is_allocated(thread->kernel_stack_pages[i]))
            return 0;
    }
    return 1;
}

static int thread_kernel_stack_release(struct pros_thread *thread) {
    if (!thread_kernel_stack_valid(thread)) return -1;
    uint32_t slot = thread->kernel_stack_slot;
    uint64_t stack_base = (uint64_t)(uintptr_t)thread->kernel_stack_base;

    for (uint32_t i = 0; i < PROS_THREAD_STACK_PAGE_COUNT; ++i) {
        uint64_t va = stack_base + (uint64_t)i * PROSENJIT_PAGE_SIZE;
        paging_unmap_page(va);
        physical_page_free(thread->kernel_stack_pages[i]);
        thread->kernel_stack_pages[i] = 0u;
    }
    thread_stack_slots[slot >> 6] &= ~(1ULL << (slot & 63u));
    thread->kernel_stack_base = 0;
    thread->kernel_stack_size = 0;
    thread->kernel_stack_slot = PROS_THREAD_STACK_SLOT_COUNT;
    return 0;
}

/*
 * Publish only a validated, owned supervisor stack as the TSS privilege-entry
 * stack. CPL3 INT 0x80 now uses IST=0 and therefore enters through this RSP0;
 * the timer IRQ remains on its current interrupt-entry path.
 */
static int thread_activate_kernel_stack(const struct pros_thread *thread) {
    if (!thread || !thread->kernel_stack_base ||
        thread->kernel_stack_size < PROS_THREAD_KERNEL_STACK_SIZE)
        return -1;

    uint64_t base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
    if (base > UINT64_MAX - thread->kernel_stack_size)
        return -1;
    uint64_t top = base + thread->kernel_stack_size;
    /* Guarded kernel stack mappings live in the canonical high-half supervisor range. */
    if (top <= base || (top & 0xFULL) != 0 ||
        top < 0xFFFF800000000000ULL)
        return -1;

    return cpu_set_kernel_stack(top);
}

static int thread_activate_bootstrap_stack(void) {
    uint64_t top = (uint64_t)(uintptr_t)stack_top;
    if (!top || (top & 0xFULL) != 0)
        return -1;
    return cpu_set_kernel_stack(top);
}

static void thread_restore_kernel_stack(const struct pros_thread *thread) {
    if (thread && thread_activate_kernel_stack(thread) == 0)
        return;
    (void)thread_activate_bootstrap_stack();
}

/* Queue membership and thread state must change as one scheduler operation. */
static int thread_is_queued(const struct pros_thread *thread) {
    return thread && (thread == runq_head || thread->next || thread->prev);
}

/* Unlink a specific queued thread without disturbing other runnable entries. */
int thread_remove_queued(struct pros_thread *thread) {
    if (!thread) return -1;
    struct pros_thread *it = runq_head;
    while (it && it != thread) it = it->next;
    if (!it) return -1;

    if (it->prev) it->prev->next = it->next;
    else runq_head = it->next;
    if (it->next) it->next->prev = it->prev;
    else runq_tail = it->prev;
    it->next = 0;
    it->prev = 0;
    return 0;
}

static int thread_process_runnable(const struct pros_thread *thread) {
    if (!thread) return 0;
    if (thread->owner_kind == PROS_THREAD_OWNER_KERNEL)
        return thread->process == 0;
    if (thread->owner_kind != PROS_THREAD_OWNER_PROCESS || !thread->process)
        return 0;

    const struct pros_process *process = thread->process;
    uint32_t state = process_state(process);

    /* Only live, scheduler-eligible processes may own runnable threads.
     * In particular, BLOCKED and quarantined processes must never execute. */
    if (process->quarantined ||
        (state != PROS_PROCESS_READY && state != PROS_PROCESS_RUNNING))
        return 0;
    return 1;
}

/* Preserve a kernel worker's BLOCKED state so a later wake can requeue it. */
static uint32_t thread_ineligible_state(const struct pros_thread *thread) {
    if (thread && thread->owner_kind == PROS_THREAD_OWNER_KERNEL &&
        !thread->process)
        return PROS_THREAD_BLOCKED;
    if (!thread || thread->owner_kind != PROS_THREAD_OWNER_PROCESS ||
        !thread->process)
        return PROS_THREAD_DEAD;
    uint32_t state = process_state(thread->process);
    return (thread->process->quarantined ||
            state == PROS_PROCESS_BLOCKED)
        ? PROS_THREAD_BLOCKED : PROS_THREAD_DEAD;
}


/* Only the still-running interrupted thread may resume after a failed switch. */
static int thread_can_resume_interrupted(const struct pros_thread *thread) {
    return thread && thread->state == PROS_THREAD_RUNNING &&
           thread_process_runnable(thread);
}

/*
 * Keep the interrupt scheduler's no-successor and failed-switch park
 * decisions independently testable. These helpers select policy only; the
 * production caller still owns the non-returning interruptible idle loop.
 */
static int thread_interrupt_should_park_without_successor(
    const struct pros_thread *thread) {
    return thread && (thread->state != PROS_THREAD_RUNNING ||
                      !thread_process_runnable(thread));
}

static int thread_interrupt_should_park_after_switch_failure(
    const struct pros_thread *thread) {
    return thread && !thread_can_resume_interrupted(thread);
}

void thread_bootstrap(struct pros_thread *thread) {
    if (!thread || !thread->entry) for (;;) __asm__ volatile ("cli; hlt");
    ((void (*)(void))(uintptr_t)thread->entry)();
    thread->state = PROS_THREAD_DEAD;
    current_thread = 0;
    (void)thread_activate_bootstrap_stack();
    for (;;) __asm__ volatile ("cli; hlt");
}
extern void thread_start_trampoline(void);

int thread_manager_init(void) {
    runq_head = 0;
    runq_tail = 0;
    current_thread = 0;
    thread_registry = 0;
    for (uint32_t i = 0; i < (PROS_THREAD_STACK_SLOT_COUNT + 63u) / 64u; ++i)
        thread_stack_slots[i] = 0u;
    return 0;
}

static int thread_initialize(struct pros_thread *thread,
                              struct pros_process *process,
                              uint32_t owner_kind,
                              uint64_t tid, uint64_t entry) {
    if (!thread || tid == 0 || entry == 0 ||
        (owner_kind != PROS_THREAD_OWNER_PROCESS &&
         owner_kind != PROS_THREAD_OWNER_KERNEL) ||
        (owner_kind == PROS_THREAD_OWNER_PROCESS && !process) ||
        (owner_kind == PROS_THREAD_OWNER_KERNEL && process))
        return -1;
    if (process && (process_state(process) == PROS_PROCESS_DEAD ||
                    process_state(process) == PROS_PROCESS_ZOMBIE))
        return -1;
    for (struct pros_thread *existing = thread_registry; existing; existing = existing->registry_next) {
        /* Registry nodes are embedded in caller-owned thread objects. Reusing
         * an already-registered object would overwrite registry_next and can
         * detach the remainder of the registry (or create a self-cycle). */
        if (existing == thread)
            return -1;
        if (existing->tid == tid && existing->state != PROS_THREAD_DEAD)
            return -1;
    }

    if (thread_kernel_stack_allocate(thread) != 0) return -1;
    uint64_t stack_top = (uint64_t)(uintptr_t)thread->kernel_stack_base +
                         thread->kernel_stack_size;

    thread->tid = tid;
    thread->process = process;
    thread->owner_kind = owner_kind;
    thread->state = PROS_THREAD_NEW;
    thread->priority = 0;
    thread->entry = entry;
    thread->interrupt_frame = 0;
    thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    for (size_t i = 0; i < sizeof(thread->user_interrupt_frame_storage) / sizeof(uint64_t); ++i)
        ((uint64_t *)&thread->user_interrupt_frame_storage)[i] = 0;
    thread->wait_channel = 0;
    thread->blocked_syscall = 0;
    thread->context.r15 = 0;
    thread->context.r14 = 0;
    thread->context.r13 = 0;
    thread->context.r12 = 0;
    thread->context.rbx = 0;
    thread->context.rbp = 0;
    uint64_t *stack = (uint64_t *)(uintptr_t)stack_top;
    /*
     * thread_context_switch restores in this order:
     *   r15, r14, r13, r12, rbp, rbx, ret
     * Build the initial stack in reverse so the first POP sees r15.
     */
    *(--stack) = (uint64_t)(uintptr_t)thread_start_trampoline; /* ret */
    *(--stack) = 0; /* rbx */
    *(--stack) = 0; /* rbp */
    *(--stack) = (uint64_t)(uintptr_t)thread; /* r12 */
    *(--stack) = 0; /* r13 */
    *(--stack) = 0; /* r14 */
    *(--stack) = 0; /* r15 */
    thread->context.rsp = (uint64_t)(uintptr_t)stack;
    thread->context.rip = entry;
    thread->context.rflags = 0x202;
    thread->next = 0;
    thread->prev = 0;
    thread->registry_next = thread_registry;
    thread_registry = thread;
    return 0;
}

int thread_init(struct pros_thread *thread, struct pros_process *process,
                uint64_t tid, uint64_t entry) {
    return thread_initialize(thread, process, PROS_THREAD_OWNER_PROCESS,
                             tid, entry);
}

int thread_init_kernel(struct pros_thread *thread, uint64_t tid,
                       uint64_t entry) {
    return thread_initialize(thread, 0, PROS_THREAD_OWNER_KERNEL, tid, entry);
}


int thread_init_user(struct pros_thread *thread, struct pros_process *process,
                     uint64_t tid, uint64_t entry, uint64_t user_rsp) {
    uint64_t physical = 0, flags = 0;
    if (!thread || !process || !entry || user_rsp == 0 ||
        user_rsp <= PROSENJIT_USER_STACK_VA ||
        user_rsp > PROSENJIT_USER_STACK_TOP || (user_rsp & 0xFULL) != 0 ||
        address_space_translate_user(&process->address_space, entry,
                                     &physical, &flags) != 0 ||
        !(flags & PROSENJIT_PAGE_USER) ||
        (flags & PROSENJIT_PAGE_NX) != 0 ||
        user_rsp <= PROSENJIT_USER_CODE_VA ||
        address_space_validate_user_range(&process->address_space,
                                          user_rsp - 1u, 1u, 1) != 0)
        return -1;

    if (thread_initialize(thread, process, PROS_THREAD_OWNER_PROCESS,
                          tid, entry) != 0)
        return -1;

    struct pros_interrupt_frame *frame = &thread->user_interrupt_frame_storage;
    frame->rip = entry;
    frame->cs = 0x1Bu;
    /* Keep IF clear: CPL3 timer preemption remains disabled until its full gate passes. */
    frame->rflags = 0x2u;
    frame->user_rsp = user_rsp;
    frame->user_ss = 0x23u;
    thread->interrupt_frame = frame;
    thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_USER;
    return 0;
}

int thread_abort_unpublished(struct pros_thread *thread) {
    if (!thread || thread->state != PROS_THREAD_NEW ||
        thread == current_thread || thread_is_queued(thread) ||
        !thread_kernel_stack_valid(thread))
        return -1;

    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    uint64_t base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
    if (base > UINT64_MAX - thread->kernel_stack_size)
        return -1;
    uint64_t top = base + thread->kernel_stack_size;
    uint64_t active_tss_stack = cpu_get_kernel_stack();
    if ((active_tss_stack >= base && active_tss_stack <= top) ||
        (rsp >= base && rsp < top))
        return -1;

    struct pros_thread **link = &thread_registry;
    while (*link && *link != thread) link = &(*link)->registry_next;
    if (!*link || thread_kernel_stack_release(thread) != 0)
        return -1;
    *link = thread->registry_next;
    thread->registry_next = 0;
    thread->process = 0;
    thread->interrupt_frame = 0;
    thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    thread->wait_channel = 0;
    thread->blocked_syscall = 0;
    thread->state = PROS_THREAD_DEAD;
    return 0;
}


int thread_process_is_current(const struct pros_process *process) {
    return process && current_thread && current_thread->process == process;
}

int thread_process_has_live(const struct pros_process *process) {
    if (!process) return 0;
    for (struct pros_thread *thread = thread_registry; thread; thread = thread->registry_next)
        if (thread->process == process && thread->state != PROS_THREAD_DEAD)
            return 1;
    return 0;
}

int thread_process_reapable(const struct pros_process *process) {
    if (!process || thread_process_is_current(process) ||
        thread_process_has_live(process))
        return 0;

    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    uint64_t active_tss_stack = cpu_get_kernel_stack();

    for (struct pros_thread *thread = thread_registry; thread;
         thread = thread->registry_next) {
        if (thread->process != process) continue;
        if (thread->state != PROS_THREAD_DEAD || thread->wait_channel ||
            thread_is_queued(thread) || !thread_kernel_stack_valid(thread))
            return 0;

        uint64_t stack_base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
        if (thread->kernel_stack_size != PROS_THREAD_KERNEL_STACK_SIZE ||
            stack_base > UINT64_MAX - thread->kernel_stack_size)
            return 0;
        uint64_t stack_top = stack_base + thread->kernel_stack_size;

        /* RSP0 may not point anywhere into a stack that is about to be freed. */
        if ((active_tss_stack >= stack_base && active_tss_stack <= stack_top) ||
            (rsp >= stack_base && rsp < stack_top))
            return 0;
    }
    return 1;
}

int thread_terminate_process(struct pros_process *process) {
    if (!process) return -1;

    int terminated_current = 0;

    /* Remove process-owned threads from the run queue before clearing their
     * next/prev links. These fields are the run-queue links, so clearing them
     * first can truncate traversal of unrelated runnable threads. */
    struct pros_thread *prev = 0;
    struct pros_thread *it = runq_head;
    while (it) {
        struct pros_thread *next = it->next;
        if (it->process == process) {
            if (prev) prev->next = next; else runq_head = next;
            if (next) next->prev = prev; else runq_tail = prev;
            it->next = 0;
            it->prev = 0;
        } else {
            prev = it;
        }
        it = next;
    }

    for (struct pros_thread *thread = thread_registry; thread; thread = thread->registry_next) {
        if (thread->process != process || thread->state == PROS_THREAD_DEAD) continue;
        if (thread == current_thread)
            terminated_current = 1;
        if (thread->wait_channel) {
            (void)ipc_channel_cancel_waiter((struct pros_ipc_channel *)thread->wait_channel, thread);
            thread->wait_channel = 0;
        }
        thread->blocked_syscall = 0;
        thread->interrupt_frame = 0;
        thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
        thread->next = 0;
        thread->prev = 0;
        thread->state = PROS_THREAD_DEAD;
    }

    /* No dead process thread may remain visible as the current thread. */
    if (terminated_current) {
        current_thread = 0;
        /* The dead thread may be reaped after this transition. */
        (void)thread_activate_bootstrap_stack();
    }

    return 0;
}

int thread_reap_process(struct pros_process *process) {
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    if (!thread_process_reapable(process)) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
        return -1;
    }

    /*
     * The full read-only preflight validates every owned stack, guard/mapping
     * pages, TSS RSP0 and current RSP before any registry entry or stack changes.
     * Interrupts remain disabled through release/unlink; SMP is not supported.
     */
    struct pros_thread **link = &thread_registry;
    while (*link) {
        struct pros_thread *thread = *link;
        if (thread->process != process) {
            link = &thread->registry_next;
            continue;
        }
        if (thread_kernel_stack_release(thread) != 0) {
            __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
            return -1;
        }
        *link = thread->registry_next;
        thread->registry_next = 0;
        thread->process = 0;
        thread->interrupt_frame = 0;
        thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
        thread->wait_channel = 0;
        thread->blocked_syscall = 0;
    }
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory", "cc");
    return 0;
}
int thread_reap_kernel(struct pros_thread *thread) {
    if (!thread || thread->owner_kind != PROS_THREAD_OWNER_KERNEL ||
        thread->process || thread == current_thread ||
        thread->state != PROS_THREAD_DEAD || thread->wait_channel ||
        thread_is_queued(thread) || !thread_kernel_stack_valid(thread))
        return -1;

    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    uint64_t stack_base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
    if (stack_base > UINT64_MAX - thread->kernel_stack_size)
        return -1;
    uint64_t stack_top = stack_base + thread->kernel_stack_size;
    if (cpu_get_kernel_stack() == stack_top ||
        (rsp >= stack_base && rsp < stack_top))
        return -1;

    struct pros_thread **link = &thread_registry;
    while (*link && *link != thread) link = &(*link)->registry_next;
    if (!*link) return -1;
    if (thread_kernel_stack_release(thread) != 0) return -1;
    *link = thread->registry_next;
    thread->registry_next = 0;
    thread->interrupt_frame = 0;
    thread->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    thread->wait_channel = 0;
    thread->blocked_syscall = 0;
    return 0;
}

int thread_set_state(struct pros_thread *thread, uint32_t state) {
    if (!thread || state > PROS_THREAD_DEAD) return -1;
    if (thread->state == PROS_THREAD_DEAD && state != PROS_THREAD_DEAD)
        return -1;
    /* A queued thread cannot become BLOCKED/DEAD behind the run queue's back.
     * Callers must use the scheduler/wait/termination paths to unlink it first. */
    if (state != thread->state && thread_is_queued(thread))
        return -1;
    thread->state = state;
    return 0;
}

uint32_t thread_state(const struct pros_thread *thread) {
    return thread ? thread->state : PROS_THREAD_DEAD;
}

int thread_enqueue(struct pros_thread *thread) {
    if (!thread || thread->state == PROS_THREAD_DEAD ||
        thread->state == PROS_THREAD_BLOCKED ||
        !thread_process_runnable(thread))
        return -1;
    if (thread->next || thread->prev || runq_head == thread)
        return -1;

    thread->state = PROS_THREAD_READY;
    thread->next = 0;
    thread->prev = runq_tail;
    if (runq_tail) runq_tail->next = thread;
    else runq_head = thread;
    runq_tail = thread;
    return 0;
}

struct pros_thread *thread_dequeue(void) {
    struct pros_thread *thread = runq_head;
    if (!thread) return 0;
    runq_head = thread->next;
    if (runq_head) runq_head->prev = 0;
    else runq_tail = 0;
    thread->next = 0;
    thread->prev = 0;
    return thread;
}

struct pros_thread *thread_current(void) {
    return current_thread;
}

int thread_schedule(void) {
    struct pros_thread *next;
    struct pros_thread *prev = current_thread;

    do {
        next = thread_dequeue();
        if (!next) return -1;
        if (!thread_process_runnable(next)) {
            /* A blocked kernel worker remains wakeable; invalid owners die. */
            next->state = thread_ineligible_state(next);
        }
    } while (next->state == PROS_THREAD_DEAD ||
             next->state == PROS_THREAD_BLOCKED);

    /*
     * Queue the previous runnable thread before changing address spaces.
     * This keeps the old current thread/address space authoritative if either
     * queueing or the subsequent CR3 switch fails.
     */
    if (prev && prev != next && prev->state == PROS_THREAD_RUNNING) {
        if (thread_enqueue(prev) != 0) {
            (void)thread_enqueue(next);
            return -1;
        }
    }

    if (thread_activate_kernel_stack(next) != 0) {
        /* A malformed/unowned stack must never become the CPU's RSP0. */
        if (prev && prev != next && prev->state == PROS_THREAD_READY &&
            thread_remove_queued(prev) == 0)
            prev->state = PROS_THREAD_RUNNING;
        (void)thread_enqueue(next);
        return -1;
    }

    process_set_current(next->process);
    if (process_current() != next->process) {
        /* The candidate address-space switch failed. Restore the old RSP0
         * before rolling the outgoing thread back to RUNNING. */
        thread_restore_kernel_stack(prev);
        if (prev && prev != next && prev->state == PROS_THREAD_READY &&
            thread_remove_queued(prev) == 0)
            prev->state = PROS_THREAD_RUNNING;
        (void)thread_enqueue(next);
        return -1;
    }

    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;
    return 0;
}

/*
 * A CPL3 IRET frame is copied into storage owned by the thread object.
 * Its user RSP/SS tail is consumed by IRETQ, so it need not remain on the
 * transient syscall-entry stack. CPL0 frames stay on the kernel stack because
 * ring-0 IRET does not switch RSP.
 */
static struct pros_interrupt_frame *thread_capture_interrupt_frame(
    struct pros_thread *thread, struct pros_interrupt_frame *frame,
    enum pros_interrupt_frame_kind frame_kind) {
    if (!thread || !frame) return 0;
    if (frame_kind != PROS_INTERRUPT_FRAME_USER) return frame;

    uint64_t *destination = (uint64_t *)&thread->user_interrupt_frame_storage;
    const uint64_t *source = (const uint64_t *)frame;
    for (size_t i = 0; i < sizeof(thread->user_interrupt_frame_storage) / sizeof(uint64_t); ++i)
        destination[i] = source[i];
    return &thread->user_interrupt_frame_storage;
}

static int thread_interrupt_frame_valid(
    const struct pros_interrupt_frame *frame,
    enum pros_interrupt_frame_kind frame_kind) {
    if (!frame) return 0;
    if (frame_kind == PROS_INTERRUPT_FRAME_KERNEL)
        return frame->cs == 0x08u;
    if (frame_kind == PROS_INTERRUPT_FRAME_USER)
        return frame->cs == 0x1Bu &&
               frame->user_ss == 0x23u &&
               frame->user_rsp != 0u &&
               frame->user_rsp < 0x0000800000000000ULL &&
               (frame->rflags & 0x2u) != 0u &&
               (frame->rflags & ((3ULL << 12) | (1ULL << 14) |
                                 (1ULL << 17))) == 0u;
    return 0;
}

struct pros_interrupt_frame *thread_schedule_interrupt(
    struct pros_interrupt_frame *current_frame,
    enum pros_interrupt_frame_kind frame_kind) {
    struct pros_thread *prev = current_thread;
    if (!current_frame) return current_frame;

    /* The selector privilege level must agree with the entry stub's tag. */
    if (!thread_interrupt_frame_valid(current_frame, frame_kind)) {
        /*
         * Never IRET through a frame whose kind/selectors disagree. Returning
         * it unchanged would turn a failed validation into an unsafe resume.
         * Detach the thread, cancel any IPC waiter, and park on the bootstrap
         * privilege stack until another eligible thread can be scheduled.
         */
        if (prev) {
            if (prev->wait_channel) {
                (void)ipc_channel_cancel_waiter(
                    (struct pros_ipc_channel *)prev->wait_channel, prev);
            }
            prev->wait_channel = 0;
            prev->blocked_syscall = 0;
            prev->interrupt_frame = 0;
            prev->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
            prev->state = PROS_THREAD_DEAD;
        }
        current_thread = 0;
        (void)thread_activate_bootstrap_stack();
        for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
    }

    /*
     * current_thread is NULL only while the scheduler is parked in its
     * interruptible idle loop because no runnable thread existed. In that
     * case this frame belongs to the idle loop, not to a blocked thread.
     */
    if (prev) {
        prev->interrupt_frame = thread_capture_interrupt_frame(
            prev, current_frame, frame_kind);
        if (!prev->interrupt_frame) {
            prev->state = PROS_THREAD_DEAD;
            current_thread = 0;
            (void)thread_activate_bootstrap_stack();
            for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
        }
        prev->interrupt_frame_kind = (uint32_t)frame_kind;

        /*
         * A sender may have queued a message while this thread's old frame
         * was invalidated but before this syscall-entry frame was saved.
         * Such an early wake cannot safely rewind RIP and leaves the waiter
         * registered. Retry only after publishing the fresh USER frame.
         */
        if (prev->state == PROS_THREAD_BLOCKED &&
            prev->blocked_syscall != 0 &&
            prev->wait_channel != 0 &&
            frame_kind == PROS_INTERRUPT_FRAME_USER) {
            (void)ipc_channel_wake_one(
                (struct pros_ipc_channel *)prev->wait_channel);
        }
    }
    struct pros_thread *next;
    do {
        next = thread_dequeue();
        if (!next) {
            /*
             * There is not yet a dedicated idle thread. Returning the saved
             * interrupt frame is safe only while the interrupted thread is
             * still eligible to execute. A process may have been blocked or
             * quarantined while its thread was running; resuming that frame
             * would bypass the process policy. Park the CPU until an interrupt
             * makes a runnable thread available instead of returning to it.
             */
            if (thread_interrupt_should_park_without_successor(prev)) {
                if (prev->state == PROS_THREAD_RUNNING) {
                    prev->state = thread_ineligible_state(prev);
                }
                /*
                 * A BLOCKED thread must never fall through to IRET merely
                 * because its process remains runnable. Detach it before
                 * enabling interrupts so an idle IRQ cannot overwrite its
                 * saved syscall frame.
                 */
                current_thread = 0;
                (void)thread_activate_bootstrap_stack();
                for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
            }
            return current_frame;
        }
        if (!thread_process_runnable(next)) {
            /* Match the cooperative scheduler's owner-specific policy. */
            next->state = thread_ineligible_state(next);
        }
        if (next->state != PROS_THREAD_DEAD &&
            next->state != PROS_THREAD_BLOCKED &&
            ((next->interrupt_frame &&
              !thread_interrupt_frame_valid(
                  next->interrupt_frame,
                  (enum pros_interrupt_frame_kind)next->interrupt_frame_kind)) ||
             (!next->interrupt_frame &&
              next->interrupt_frame_kind != PROS_INTERRUPT_FRAME_NONE))) {
            /* Never restore a stale/mistagged frame belonging to another ABI. */
            next->interrupt_frame = 0;
            next->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
            next->blocked_syscall = 0;
            next->state = PROS_THREAD_DEAD;
        }
    } while (next->state == PROS_THREAD_DEAD ||
             next->state == PROS_THREAD_BLOCKED);

    /*
     * Queue the interrupted thread before changing address spaces. If the
     * queue operation fails, the interrupted frame and current address space
     * remain authoritative. If the CR3 switch fails, the previous thread is
     * already safely queued and the candidate is requeued.
     */
    /*
     * BLOCKED predecessors are not queued. A predecessor that became READY
     * through the post-publication IPC wake retry is already on the run queue;
     * only the still-running interrupted predecessor needs enqueueing here.
     */
    if (prev && prev->state == PROS_THREAD_RUNNING) {
        if (!thread_process_runnable(prev)) {
            prev->state = thread_ineligible_state(prev);
        } else if (thread_enqueue(prev) != 0) {
            (void)thread_enqueue(next);
            return current_frame;
        }
    }

    if (thread_activate_kernel_stack(next) != 0) {
        /* Reject a candidate whose supervisor stack cannot safely own RSP0. */
        if (prev && prev->state == PROS_THREAD_READY &&
            thread_remove_queued(prev) == 0)
            prev->state = PROS_THREAD_RUNNING;
        (void)thread_enqueue(next);
        if (thread_interrupt_should_park_after_switch_failure(prev)) {
            prev->state = thread_ineligible_state(prev);
            current_thread = 0;
            (void)thread_activate_bootstrap_stack();
            for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
        }
        thread_restore_kernel_stack(prev);
        return current_frame;
    }

    process_set_current(next->process);
    if (process_current() != next->process) {
        /* If the interrupted thread was requeued, undo that transition before
         * returning its frame; otherwise it would be both executing and READY. */
        if (prev && prev->state == PROS_THREAD_READY &&
            thread_remove_queued(prev) == 0)
            prev->state = PROS_THREAD_RUNNING;
        (void)thread_enqueue(next);

        /* Never iret back into a process that became blocked/quarantined while
         * running, even if switching to its replacement failed. Keep the CPU
         * in an interruptible idle loop; a later IRQ can retry scheduling. */
        if (thread_interrupt_should_park_after_switch_failure(prev)) {
            prev->state = thread_ineligible_state(prev);
            current_thread = 0;
            (void)thread_activate_bootstrap_stack();
            for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
        }
        thread_restore_kernel_stack(prev);
        return current_frame;
    }

    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;

    if (next->interrupt_frame)
        return next->interrupt_frame;

    /* First interrupt-driven run: synthesize a CPL0 iret frame. */
    /*
     * A first-run kernel frame has only the 15-GPR prefix plus the
     * three-qword CPL0 IRET tail. Do not reserve the CPL3-only RSP/SS words:
     * IRETQ will not consume them for a ring-0 return.
     */
    const size_t kernel_frame_size =
        __builtin_offsetof(struct pros_interrupt_frame, user_rsp);
    struct pros_interrupt_frame *frame =
        (struct pros_interrupt_frame *)(uintptr_t)(next->context.rsp -
                                                   kernel_frame_size);
    frame->rax = frame->rbx = frame->rcx = frame->rdx = 0;
    frame->rsi = frame->rdi = frame->rbp = 0;
    frame->r8 = frame->r9 = frame->r10 = frame->r11 = 0;
    frame->r12 = (uint64_t)(uintptr_t)next;
    frame->r13 = frame->r14 = frame->r15 = 0;
    frame->rip = (uint64_t)(uintptr_t)thread_start_trampoline;
    frame->cs = 0x08;
    frame->rflags = 0x202;
    next->interrupt_frame = frame;
    next->interrupt_frame_kind = PROS_INTERRUPT_FRAME_KERNEL;
    return frame;
}

int thread_block(void *wait_channel) {
    if (!current_thread || !wait_channel ||
        current_thread->state != PROS_THREAD_RUNNING)
        return -1;
    current_thread->wait_channel = wait_channel;
    current_thread->state = PROS_THREAD_BLOCKED;
    return 0;
}

int thread_wake(struct pros_thread *thread) {
    if (!thread || thread->state != PROS_THREAD_BLOCKED ||
        !thread_process_runnable(thread))
        return -1;

    if (thread->blocked_syscall != 0 &&
        (thread->interrupt_frame_kind != PROS_INTERRUPT_FRAME_USER ||
         !thread_interrupt_frame_valid(
             thread->interrupt_frame, PROS_INTERRUPT_FRAME_USER) ||
         thread->interrupt_frame->rip < 2u))
        return -1;

    /*
     * Publish the completed syscall continuation before exposing this thread
     * on the run queue. Save the old words so a failed enqueue leaves the
     * blocked receiver's frame and wake state unchanged.
     */
    uint64_t blocked_syscall = thread->blocked_syscall;
    uint64_t original_rax = 0;
    uint64_t original_rip = 0;
    if (blocked_syscall != 0) {
        original_rax = thread->interrupt_frame->rax;
        original_rip = thread->interrupt_frame->rip;
        thread->interrupt_frame->rax = blocked_syscall;
        thread->interrupt_frame->rip = original_rip - 2u;
    }

    thread->state = PROS_THREAD_READY;
    if (thread_enqueue(thread) != 0) {
        thread->state = PROS_THREAD_BLOCKED;
        if (blocked_syscall != 0) {
            thread->interrupt_frame->rax = original_rax;
            thread->interrupt_frame->rip = original_rip;
        }
        return -1;
    }
    if (blocked_syscall != 0)
        thread->blocked_syscall = 0;
    thread->wait_channel = 0;
    return 0;
}

static void thread_kernel_selftest_entry(void) { }

int thread_kernel_selftest(void) {
    static struct pros_thread probe;
    if (probe.registry_next || probe.kernel_stack_base) return -1;
    if (thread_init_kernel(&probe, UINT64_MAX,
                           (uint64_t)(uintptr_t)thread_kernel_selftest_entry) != 0)
        return -2;
    if (probe.process || probe.owner_kind != PROS_THREAD_OWNER_KERNEL ||
        probe.state != PROS_THREAD_NEW ||
        !thread_kernel_stack_valid(&probe))
        return -3;
    /* Reject owner/process mismatches before they can enter the run queue. */
    struct pros_process *saved_process = process_current();
    if (!saved_process) return -4;
    probe.process = saved_process;
    if (thread_process_runnable(&probe)) return -4;
    probe.process = 0;
    probe.owner_kind = UINT32_MAX;
    if (thread_process_runnable(&probe)) return -4;
    probe.owner_kind = PROS_THREAD_OWNER_KERNEL;
    if (thread_init(&probe, 0, UINT64_MAX - 1u,
                    (uint64_t)(uintptr_t)thread_kernel_selftest_entry) == 0)
        return -4;
    if (thread_set_state(&probe, PROS_THREAD_READY) != 0 ||
        !thread_process_runnable(&probe) ||
        thread_enqueue(&probe) != 0 ||
        !thread_is_queued(&probe) ||
        thread_remove_queued(&probe) != 0 ||
        thread_state(&probe) != PROS_THREAD_READY)
        return -4;

    /* Both scheduler rollback paths must preserve a blocked kernel owner. */
    probe.state = PROS_THREAD_BLOCKED;
    if (thread_ineligible_state(&probe) != PROS_THREAD_BLOCKED ||
        thread_can_resume_interrupted(&probe))
        return -5;
    probe.state = PROS_THREAD_RUNNING;
    if (!thread_can_resume_interrupted(&probe))
        return -5;
    if (thread_interrupt_should_park_without_successor(&probe) ||
        thread_interrupt_should_park_after_switch_failure(&probe))
        return -5;
    probe.state = PROS_THREAD_BLOCKED;
    if (!thread_interrupt_should_park_without_successor(&probe) ||
        !thread_interrupt_should_park_after_switch_failure(&probe))
        return -5;
    probe.state = PROS_THREAD_RUNNING;
    probe.owner_kind = UINT32_MAX;
    if (!thread_interrupt_should_park_without_successor(&probe) ||
        !thread_interrupt_should_park_after_switch_failure(&probe))
        return -5;
    probe.state = PROS_THREAD_BLOCKED;
    probe.owner_kind = UINT32_MAX;
    if (thread_ineligible_state(&probe) != PROS_THREAD_DEAD)
        return -5;
    probe.owner_kind = PROS_THREAD_OWNER_KERNEL;

    /* A blocked kernel worker must remain eligible for a later wake. */
    probe.state = PROS_THREAD_BLOCKED;
    if (thread_wake(&probe) != 0 ||
        probe.state != PROS_THREAD_READY ||
        !thread_is_queued(&probe) ||
        thread_remove_queued(&probe) != 0)
        return -5;

    /*
     * A blocked user syscall must be repaired before queue publication, and
     * an enqueue rejection must restore the original continuation exactly.
     */
    static struct pros_interrupt_frame wake_frame;
    probe.owner_kind = PROS_THREAD_OWNER_PROCESS;
    probe.process = saved_process;
    probe.state = PROS_THREAD_BLOCKED;
    probe.interrupt_frame = &wake_frame;
    probe.interrupt_frame_kind = PROS_INTERRUPT_FRAME_USER;
    probe.wait_channel = &probe;
    probe.blocked_syscall = 0x55u;
    wake_frame = (struct pros_interrupt_frame){0};
    wake_frame.rip = 0x1002u;
    wake_frame.cs = 0x1Bu;
    wake_frame.rflags = 0x202u;
    wake_frame.user_rsp = 0x70000000u;
    wake_frame.user_ss = 0x23u;
    wake_frame.rax = 0xDEADBEEFu;
    probe.next = &probe; /* Force thread_enqueue() to reject this attempt. */
    if (thread_wake(&probe) == 0 ||
        probe.state != PROS_THREAD_BLOCKED ||
        probe.blocked_syscall != 0x55u ||
        wake_frame.rax != 0xDEADBEEFu ||
        wake_frame.rip != 0x1002u ||
        probe.wait_channel != &probe)
        return -6;
    probe.next = 0;
    if (thread_wake(&probe) != 0 ||
        probe.state != PROS_THREAD_READY ||
        probe.blocked_syscall != 0 ||
        wake_frame.rax != 0x55u ||
        wake_frame.rip != 0x1000u ||
        probe.wait_channel != 0 ||
        !thread_is_queued(&probe) ||
        thread_remove_queued(&probe) != 0)
        return -6;
    probe.owner_kind = PROS_THREAD_OWNER_KERNEL;
    probe.process = 0;
    probe.state = PROS_THREAD_BLOCKED;
    probe.interrupt_frame = 0;
    probe.interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
    probe.wait_channel = 0;
    probe.blocked_syscall = 0;

    /*
     * Kernel workers must execute with the kernel page tables, not retain the
     * outgoing user process context. Restore the original process afterwards.
     */
    process_set_current(0);
    if (process_current() != 0) return -6;
    process_set_current(saved_process);
    if (process_current() != saved_process) return -6;

    probe.state = PROS_THREAD_DEAD;
    if (cpu_get_kernel_stack() ==
        (uint64_t)(uintptr_t)probe.kernel_stack_base + probe.kernel_stack_size)
        return -7;
    if (thread_reap_kernel(&probe) != 0 ||
        probe.registry_next || probe.kernel_stack_base ||
        probe.kernel_stack_size)
        return -8;
    return 0;
}

uint64_t thread_runnable_count(void) {
    uint64_t count = 0;
    for (struct pros_thread *thread = thread_registry; thread; thread = thread->registry_next)
        if (thread_process_runnable(thread) &&
            (thread->state == PROS_THREAD_READY || thread->state == PROS_THREAD_RUNNING))
            ++count;
    return count;
}

uint64_t thread_blocked_count(void) {
    uint64_t count = 0;
    for (struct pros_thread *thread = thread_registry; thread; thread = thread->registry_next)
        if (thread->state == PROS_THREAD_BLOCKED)
            ++count;
    return count;
}

uint64_t thread_dead_count(void) {
    uint64_t count = 0;
    for (struct pros_thread *thread = thread_registry; thread; thread = thread->registry_next)
        if (thread->state == PROS_THREAD_DEAD)
            ++count;
    return count;
}

int thread_has_runnable(void) {
    for (struct pros_thread *thread = runq_head; thread; thread = thread->next)
        if (thread_process_runnable(thread))
            return 1;
    return 0;
}

int thread_schedule_preempt(void) {
    struct pros_thread *prev = current_thread;
    if (thread_schedule() != 0) return -1;
    if (prev && prev != current_thread)
        thread_context_switch(prev, current_thread);
    return 0;
}

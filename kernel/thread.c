#include "thread.h"
#include "ipc.h"
#include "paging.h"
#include "../include/abi/syscall.h"

static struct pros_thread *runq_head;
static struct pros_thread *runq_tail;
static struct pros_thread *current_thread;
static struct pros_thread *thread_registry;
static int thread_process_runnable(const struct pros_thread *thread) {
    uint32_t state;

    if (!thread || !thread->process || thread->process->quarantined)
        return 0;

    state = process_state(thread->process);
    /* Only admitted READY/RUNNING processes may execute. In particular,
     * BLOCKED/quarantined processes must never become runnable just because
     * they still have a thread in the scheduler queue. */
    return state == PROS_PROCESS_READY || state == PROS_PROCESS_RUNNING;
}

/* There is no dedicated idle thread yet. If the current process is no longer
 * eligible and no safe successor can be installed, fail closed rather than
 * returning to potentially quarantined CPL3 code. */
static void thread_scheduler_fail_closed(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}


void thread_bootstrap(struct pros_thread *thread) {
    if (!thread || !thread->entry) for (;;) __asm__ volatile ("cli; hlt");
    ((void (*)(void))(uintptr_t)thread->entry)();
    thread->state = PROS_THREAD_DEAD;
    current_thread = 0;
    for (;;) __asm__ volatile ("cli; hlt");
}
extern void thread_start_trampoline(void);

int thread_manager_init(void) {
    runq_head = 0;
    runq_tail = 0;
    current_thread = 0;
    thread_registry = 0;
    return 0;
}

int thread_init(struct pros_thread *thread, struct pros_process *process,
                uint64_t tid, uint64_t entry, uint64_t stack_top) {
    if (!thread || !process || tid == 0 || entry == 0 || stack_top == 0 ||
        (stack_top & 0xFULL) != 0u)
        return -1;
    if (process_state(process) == PROS_PROCESS_DEAD ||
        process_state(process) == PROS_PROCESS_ZOMBIE)
        return -1;
    for (struct pros_thread *existing = thread_registry; existing; existing = existing->registry_next)
        if (existing->tid == tid && existing->state != PROS_THREAD_DEAD)
            return -1;

    thread->tid = tid;
    thread->process = process;
    thread->state = PROS_THREAD_NEW;
    thread->priority = 0;
    thread->entry = entry;
    thread->interrupt_frame = 0;
    thread->wait_channel = 0;
    thread->blocked_syscall = 0;
    thread->context.r15 = 0;
    thread->context.r14 = 0;
    thread->context.r13 = 0;
    thread->context.r12 = 0;
    thread->context.rbx = 0;
    thread->context.rbp = 0;
    thread->kernel_stack_top = stack_top;
    thread->user_mode = 0u;
    for (uint32_t i = 0u; i < sizeof(thread->reserved_flags); ++i)
        thread->reserved_flags[i] = 0u;
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

int thread_init_user(struct pros_thread *thread, struct pros_process *process,
                     uint64_t tid, uint64_t user_entry, uint64_t user_stack_top,
                     uint64_t kernel_stack_bottom, uint64_t kernel_stack_top)
{
    uint64_t entry_physical = 0u, entry_flags = 0u;
    uint64_t stack_physical = 0u, stack_flags = 0u;
    struct pros_user_interrupt_frame *frame;

    if (!thread || !process || tid == 0u || user_entry == 0u ||
        user_stack_top == 0u || (user_stack_top & 0xFULL) != 0u ||
        !kernel_stack_bottom || kernel_stack_top <= kernel_stack_bottom ||
        (kernel_stack_bottom & 0xFULL) != 0u ||
        (kernel_stack_top & 0xFULL) != 0u ||
        kernel_stack_top - kernel_stack_bottom < 4096u ||
        kernel_stack_top - kernel_stack_bottom <
            sizeof(struct pros_user_interrupt_frame) + 7u * sizeof(uint64_t) ||
        !process->address_space.root ||
        address_space_translate_user(&process->address_space, user_entry,
                                     &entry_physical, &entry_flags) != 0 ||
        !(entry_flags & PROSENJIT_PAGE_USER) ||
        (entry_flags & PROSENJIT_PAGE_NX) != 0u ||
        address_space_validate_user_range(&process->address_space,
                                          user_stack_top - 1u, 1u, 1) != 0 ||
        address_space_translate_user(&process->address_space,
                                     user_stack_top - 1u,
                                     &stack_physical, &stack_flags) != 0 ||
        !(stack_flags & PROSENJIT_PAGE_USER) ||
        !(stack_flags & PROSENJIT_PAGE_WRITABLE) ||
        !(stack_flags & PROSENJIT_PAGE_NX))
        return -1;

    if (thread_init(thread, process, tid, user_entry, kernel_stack_top) != 0)
        return -1;

    /* Place the CPL3 IRET frame below the unused kernel-context bootstrap frame. */
    frame = (struct pros_user_interrupt_frame *)(uintptr_t)
        (thread->context.rsp - sizeof(struct pros_user_interrupt_frame));
    for (uint32_t i = 0u; i < 15u; ++i)
        ((uint64_t *)&frame->registers)[i] = 0u;
    frame->registers.rip = user_entry;
    frame->registers.cs = 0x1Bu;
    /* Scheduler-launched CPL3 threads use the validated IRQ-frame path. */
    frame->registers.rflags = 0x202u;
    frame->user_rsp = user_stack_top;
    frame->user_ss = 0x23u;
    thread->interrupt_frame = &frame->registers;
    thread->kernel_stack_top = kernel_stack_top;
    thread->user_mode = 1u;
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

/*
 * Detach dead thread records before a parent is allowed to release the
 * caller-owned thread/stack storage. Validate the complete set first so an
 * unexpected live/queued/waiting thread cannot leave a half-detached list.
 */
int thread_process_unregister_dead(struct pros_process *process) {
    if (!process || thread_process_is_current(process) ||
        thread_process_has_live(process))
        return -1;

    for (struct pros_thread *thread = thread_registry; thread;
         thread = thread->registry_next) {
        if (thread->process != process) continue;
        if (thread->state != PROS_THREAD_DEAD || thread->wait_channel ||
            thread->next || thread->prev)
            return -1;
    }

    struct pros_thread **link = &thread_registry;
    while (*link) {
        struct pros_thread *thread = *link;
        if (thread->process != process) {
            link = &thread->registry_next;
            continue;
        }
        *link = thread->registry_next;
        thread->registry_next = 0;
        thread->process = 0;
        thread->interrupt_frame = 0;
        thread->kernel_stack_top = 0;
    }
    return 0;
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
        thread->next = 0;
        thread->prev = 0;
        thread->state = PROS_THREAD_DEAD;
    }

    /* No dead process thread may remain visible as the current thread. */
    if (terminated_current)
        current_thread = 0;

    return 0;
}
int thread_set_state(struct pros_thread *thread, uint32_t state) {
    if (!thread || state > PROS_THREAD_DEAD) return -1;
    if (thread->state == PROS_THREAD_DEAD && state != PROS_THREAD_DEAD)
        return -1;
    thread->state = state;
    return 0;
}

uint32_t thread_state(const struct pros_thread *thread) {
    return thread ? thread->state : PROS_THREAD_DEAD;
}

int thread_enqueue(struct pros_thread *thread) {
    if (!thread || thread->state == PROS_THREAD_DEAD ||
        thread->state == PROS_THREAD_BLOCKED)
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
    struct pros_thread *next = 0;
    struct pros_thread *prev = current_thread;
    uint32_t queued = 0u;
    uint32_t attempts = 0u;

    for (struct pros_thread *it = runq_head; it; it = it->next)
        ++queued;
    while (attempts < queued) {
        struct pros_thread *candidate = thread_dequeue();
        uint32_t state;
        if (!candidate)
            break;
        ++attempts;
        state = candidate->process ? process_state(candidate->process)
                                   : PROS_PROCESS_DEAD;
        if (state == PROS_PROCESS_ZOMBIE || state == PROS_PROCESS_DEAD) {
            candidate->state = PROS_THREAD_DEAD;
            continue;
        }
        /* A blocked/quarantined process must be skipped, not destroyed. */
        if (!thread_process_runnable(candidate) ||
            (candidate->user_mode && !candidate->interrupt_frame)) {
            (void)thread_enqueue(candidate);
            continue;
        }
        /* A CPL3 frame must be restored through IRET, never a kernel RET. */
        if (candidate->user_mode) {
            (void)thread_enqueue(candidate);
            continue;
        }
        next = candidate;
        break;
    }
    if (!next)
        return -1;

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

    process_set_current(next->process);
    if (process_current() != next->process) {
        (void)thread_enqueue(next);
        return -1;
    }

    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;
    return 0;
}

struct pros_interrupt_frame *thread_schedule_interrupt(
    struct pros_interrupt_frame *current_frame) {
    struct pros_thread *prev = current_thread;
    if (!prev || !current_frame) return current_frame;

    prev->interrupt_frame = current_frame;
    struct pros_thread *next = 0;
    uint32_t queued = 0u;
    uint32_t attempts = 0u;

    for (struct pros_thread *it = runq_head; it; it = it->next)
        ++queued;
    while (attempts < queued) {
        uint32_t state;
        next = thread_dequeue();
        if (!next)
            break;
        ++attempts;
        state = next->process ? process_state(next->process)
                              : PROS_PROCESS_DEAD;
        if (state == PROS_PROCESS_ZOMBIE || state == PROS_PROCESS_DEAD) {
            next->state = PROS_THREAD_DEAD;
            next = 0;
            continue;
        }
        /* Preserve blocked/quarantined threads in the queue, but never run
         * them. The bounded scan avoids spinning if every queued thread is
         * blocked. A CPL3 thread without its validated IRET frame is invalid. */
        if (!thread_process_runnable(next)) {
            (void)thread_enqueue(next);
            next = 0;
            continue;
        }
        if (next->user_mode && !next->interrupt_frame) {
            next->state = PROS_THREAD_DEAD;
            next = 0;
            continue;
        }
        break;
    }
    if (!next) {
        /*
         * Returning the same interrupt frame is safe only while the current
         * thread itself remains runnable. A process can be READY/RUNNING
         * while its only thread has blocked; checking only process eligibility
         * would then return to a blocked syscall/thread with no successor.
         * Until an idle thread exists, fail closed for every non-running
         * current thread rather than resuming an invalid execution context.
         */
        if (prev->state != PROS_THREAD_RUNNING ||
            !thread_process_runnable(prev))
            thread_scheduler_fail_closed();
        return current_frame;
    }

    /*
     * Queue the interrupted thread before changing address spaces. If the
     * queue operation fails, the interrupted frame and current address space
     * remain authoritative. If the CR3 switch fails, the previous thread is
     * already safely queued and the candidate is requeued.
     */
    if (prev->state != PROS_THREAD_BLOCKED) {
        uint32_t prev_process_state = process_state(prev->process);
        if (prev_process_state == PROS_PROCESS_ZOMBIE ||
            prev_process_state == PROS_PROCESS_DEAD) {
            prev->state = PROS_THREAD_DEAD;
        } else if (!thread_process_runnable(prev)) {
            /* The process is blocked/quarantined; preserve its thread state. */
            prev->state = PROS_THREAD_BLOCKED;
        } else if (thread_enqueue(prev) != 0) {
            (void)thread_enqueue(next);
            return current_frame;
        }
    }

    process_set_current(next->process);
    if (process_current() != next->process) {
        (void)thread_enqueue(next);
        /*
         * The interrupted frame can be resumed only if its thread remained
         * RUNNING. A runnable process does not make a BLOCKED thread runnable.
         */
        if (prev->state != PROS_THREAD_RUNNING ||
            !thread_process_runnable(prev))
            thread_scheduler_fail_closed();
        return current_frame;
    }

    if (cpu_set_kernel_stack(next->kernel_stack_top) != 0) {
        process_set_current(prev->process);
        (void)thread_enqueue(next);
        if (prev->state != PROS_THREAD_RUNNING ||
            !thread_process_runnable(prev))
            thread_scheduler_fail_closed();
        return current_frame;
    }
    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;

    if (next->interrupt_frame)
        return next->interrupt_frame;

    /* First interrupt-driven run: synthesize a CPL0 iret frame. */
    struct pros_interrupt_frame *frame =
        (struct pros_interrupt_frame *)(uintptr_t)(next->context.rsp -
                                                   sizeof(*frame));
    frame->rax = frame->rbx = frame->rcx = frame->rdx = 0;
    frame->rsi = frame->rdi = frame->rbp = 0;
    frame->r8 = frame->r9 = frame->r10 = frame->r11 = 0;
    frame->r12 = (uint64_t)(uintptr_t)next;
    frame->r13 = frame->r14 = frame->r15 = 0;
    frame->rip = (uint64_t)(uintptr_t)thread_start_trampoline;
    frame->cs = 0x08;
    frame->rflags = 0x202;
    next->interrupt_frame = frame;
    return frame;
}

/*
 * Complete SYS_EXIT while still on the interrupt-created kernel stack.
 * The old user frame is never resumed after a successful process_exit().
 * There is no idle thread yet, so lack of a runnable successor is fatal.
 */
struct pros_interrupt_frame *thread_exit_schedule_interrupt(
    struct pros_interrupt_frame *current_frame, int32_t exit_code) {
    struct pros_thread *prev = current_thread;
    struct pros_process *exiting_process;
    struct pros_thread *next = 0;
    uint32_t queued = 0u;
    uint32_t attempts = 0u;

    if (!prev || !current_frame || !prev->user_mode || !prev->process) {
        if (current_frame)
            current_frame->rax = PROSENJIT_SYSCALL_EPERM;
        return current_frame;
    }

    prev->interrupt_frame = current_frame;
    exiting_process = prev->process;
    if (process_exit(exiting_process, exit_code) != 0) {
        current_frame->rax = PROSENJIT_SYSCALL_EPERM;
        return current_frame;
    }

    for (struct pros_thread *it = runq_head; it; it = it->next)
        ++queued;
    while (attempts < queued) {
        uint32_t state;
        next = thread_dequeue();
        if (!next)
            break;
        ++attempts;
        state = next->process ? process_state(next->process)
                              : PROS_PROCESS_DEAD;
        if (state == PROS_PROCESS_ZOMBIE || state == PROS_PROCESS_DEAD) {
            next->state = PROS_THREAD_DEAD;
            next = 0;
            continue;
        }
        if (!thread_process_runnable(next)) {
            (void)thread_enqueue(next);
            next = 0;
            continue;
        }
        if (next->user_mode && !next->interrupt_frame) {
            next->state = PROS_THREAD_DEAD;
            next = 0;
            continue;
        }
        break;
    }

    /* No idle thread exists yet; never return to the terminated CPL3 frame. */
    if (!next)
        thread_scheduler_fail_closed();

    process_set_current(next->process);
    if (process_current() != next->process)
        thread_scheduler_fail_closed();
    if (cpu_set_kernel_stack(next->kernel_stack_top) != 0)
        thread_scheduler_fail_closed();

    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;
    if (next->interrupt_frame)
        return next->interrupt_frame;

    /* First interrupt-driven run of a kernel thread: synthesize its IRET frame. */
    struct pros_interrupt_frame *frame =
        (struct pros_interrupt_frame *)(uintptr_t)(next->context.rsp -
                                                   sizeof(*frame));
    frame->rax = frame->rbx = frame->rcx = frame->rdx = 0;
    frame->rsi = frame->rdi = frame->rbp = 0;
    frame->r8 = frame->r9 = frame->r10 = frame->r11 = 0;
    frame->r12 = (uint64_t)(uintptr_t)next;
    frame->r13 = frame->r14 = frame->r15 = 0;
    frame->rip = (uint64_t)(uintptr_t)thread_start_trampoline;
    frame->cs = 0x08;
    frame->rflags = 0x202;
    next->interrupt_frame = frame;
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
        (!thread->interrupt_frame || thread->interrupt_frame->rip < 2u))
        return -1;

    thread->state = PROS_THREAD_READY;
    if (thread_enqueue(thread) != 0) {
        thread->state = PROS_THREAD_BLOCKED;
        return -1;
    }
    if (thread->blocked_syscall != 0 && thread->interrupt_frame) {
        thread->interrupt_frame->rax = thread->blocked_syscall;
        thread->interrupt_frame->rip -= 2;
        thread->blocked_syscall = 0;
    }
    thread->wait_channel = 0;
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

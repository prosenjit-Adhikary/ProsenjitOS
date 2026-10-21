#include "thread.h"
#include "ipc.h"

static struct pros_thread *runq_head;
static struct pros_thread *runq_tail;
static struct pros_thread *current_thread;
static struct pros_thread *thread_registry;

/* Queue membership and thread state must change as one scheduler operation. */
static int thread_is_queued(const struct pros_thread *thread) {
    return thread && (thread == runq_head || thread->next || thread->prev);
}

/* Unlink a specific queued thread without disturbing other runnable entries. */
static int thread_remove_queued(struct pros_thread *thread) {
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
    if (!thread || !thread->process) return 0;

    const struct pros_process *process = thread->process;
    uint32_t state = process_state(process);

    /* Only live, scheduler-eligible processes may own runnable threads.
     * In particular, BLOCKED and quarantined processes must never execute. */
    if (process->quarantined ||
        (state != PROS_PROCESS_READY && state != PROS_PROCESS_RUNNING))
        return 0;
    return 1;
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
    if (!thread || !process || tid == 0 || entry == 0 || stack_top == 0)
        return -1;
    if (process_state(process) == PROS_PROCESS_DEAD ||
        process_state(process) == PROS_PROCESS_ZOMBIE)
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
            uint32_t process_status = next->process
                ? process_state(next->process) : PROS_PROCESS_DEAD;
            /* A process becoming blocked/quarantined after enqueue is not a
             * thread exit. Retire the stale queue entry without killing it. */
            next->state = (next->process &&
                           (next->process->quarantined ||
                            process_status == PROS_PROCESS_BLOCKED))
                ? PROS_THREAD_BLOCKED : PROS_THREAD_DEAD;
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

    process_set_current(next->process);
    if (process_current() != next->process) {
        /* The candidate address-space switch failed. Roll back the outgoing
         * thread's queue/state transition so current_thread remains RUNNING. */
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

struct pros_interrupt_frame *thread_schedule_interrupt(
    struct pros_interrupt_frame *current_frame) {
    struct pros_thread *prev = current_thread;
    if (!current_frame) return current_frame;

    /*
     * current_thread is NULL only while the scheduler is parked in its
     * interruptible idle loop because no runnable thread existed. In that
     * case this frame belongs to the idle loop, not to a blocked thread.
     */
    if (prev)
        prev->interrupt_frame = current_frame;
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
            if (prev && !thread_process_runnable(prev)) {
                uint32_t process_status = prev->process
                    ? process_state(prev->process) : PROS_PROCESS_DEAD;
                prev->state = (prev->process &&
                               (prev->process->quarantined ||
                                process_status == PROS_PROCESS_BLOCKED))
                    ? PROS_THREAD_BLOCKED : PROS_THREAD_DEAD;
                /*
                 * Detach the blocked thread before enabling interrupts. This
                 * prevents timer interrupts in the idle loop from overwriting
                 * its saved interrupt frame with an idle-loop frame.
                 */
                current_thread = 0;
                for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
            }
            return current_frame;
        }
        if (!thread_process_runnable(next)) {
            uint32_t process_status = next->process
                ? process_state(next->process) : PROS_PROCESS_DEAD;
            /* Match the cooperative scheduler: a blocked/quarantined owner
             * blocks its queued thread; only an invalid/dead owner retires it. */
            next->state = (next->process &&
                           (next->process->quarantined ||
                            process_status == PROS_PROCESS_BLOCKED))
                ? PROS_THREAD_BLOCKED : PROS_THREAD_DEAD;
        }
    } while (next->state == PROS_THREAD_DEAD ||
             next->state == PROS_THREAD_BLOCKED);

    /*
     * Queue the interrupted thread before changing address spaces. If the
     * queue operation fails, the interrupted frame and current address space
     * remain authoritative. If the CR3 switch fails, the previous thread is
     * already safely queued and the candidate is requeued.
     */
    if (prev && prev->state != PROS_THREAD_BLOCKED) {
        if (!thread_process_runnable(prev)) {
            uint32_t process_status = prev->process
                ? process_state(prev->process) : PROS_PROCESS_DEAD;
            prev->state = (prev->process &&
                           (prev->process->quarantined ||
                            process_status == PROS_PROCESS_BLOCKED))
                ? PROS_THREAD_BLOCKED : PROS_THREAD_DEAD;
        } else if (thread_enqueue(prev) != 0) {
            (void)thread_enqueue(next);
            return current_frame;
        }
    }

    process_set_current(next->process);
    if (process_current() != next->process) {
        /* If the interrupted thread was requeued, undo that transition before
         * returning its frame; otherwise it would be both executing and READY. */
        if (prev && prev->state == PROS_THREAD_READY &&
            thread_remove_queued(prev) == 0)
            prev->state = PROS_THREAD_RUNNING;
        (void)thread_enqueue(next);
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
    if (thread_schedule() != 0) {
        /*
         * Until an explicit idle thread exists, a blocked/dead/ineligible
         * current thread must not continue past a failed schedule attempt.
         * Park with interrupts enabled so timer/device wakeups can enqueue a
         * runnable thread; an interrupt-driven schedule can then switch away.
         */
        if (prev && (prev->state != PROS_THREAD_RUNNING ||
                     !thread_process_runnable(prev))) {
            for (;;) __asm__ volatile ("sti; hlt" ::: "memory");
        }
        return -1;
    }
    if (prev && prev != current_thread)
        thread_context_switch(prev, current_thread);
    return 0;
}

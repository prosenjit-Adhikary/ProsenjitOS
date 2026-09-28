#include "thread.h"

static struct pros_thread *runq_head;
static struct pros_thread *runq_tail;
static struct pros_thread *current_thread;
static int thread_process_runnable(const struct pros_thread *thread) {
    return thread && thread->process &&
           process_state(thread->process) != PROS_PROCESS_ZOMBIE &&
           process_state(thread->process) != PROS_PROCESS_DEAD;
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
    return 0;
}

int thread_init(struct pros_thread *thread, struct pros_process *process,
                uint64_t tid, uint64_t entry, uint64_t stack_top) {
    if (!thread || !process || tid == 0 || entry == 0 || stack_top == 0)
        return -1;
    if (process_state(process) == PROS_PROCESS_DEAD ||
        process_state(process) == PROS_PROCESS_ZOMBIE)
        return -1;

    thread->tid = tid;
    thread->process = process;
    thread->state = PROS_THREAD_NEW;
    thread->priority = 0;
    thread->entry = entry;
    thread->interrupt_frame = 0;
    thread->wait_channel = 0;
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
    struct pros_thread *next;
    do {
        next = thread_dequeue();
        if (!next) return -1;
        if (!thread_process_runnable(next)) {
            next->state = PROS_THREAD_DEAD;
        }
    } while (next->state == PROS_THREAD_DEAD);

    if (current_thread && current_thread != next &&
        current_thread->state == PROS_THREAD_RUNNING) {
        current_thread->state = PROS_THREAD_READY;
        if (thread_enqueue(current_thread) != 0) return -1;
    }

    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;
    if (current_thread->process)
        process_set_current(current_thread->process);
    return 0;
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
    if (!thread || thread->state != PROS_THREAD_BLOCKED)
        return -1;
    thread->wait_channel = 0;
    return thread_enqueue(thread);
}

int thread_schedule_preempt(void) {
    struct pros_thread *prev = current_thread;
    if (thread_schedule() != 0) return -1;
    if (prev && prev != current_thread)
        thread_context_switch(prev, current_thread);
    return 0;
}


struct pros_interrupt_frame *thread_schedule_interrupt(
    struct pros_interrupt_frame *current_frame) {
    struct pros_thread *prev = current_thread;
    if (!prev || !current_frame) return current_frame;

    prev->interrupt_frame = current_frame;
    struct pros_thread *next;
    do {
        next = thread_dequeue();
        if (!next) return current_frame;
        if (!thread_process_runnable(next))
            next->state = PROS_THREAD_DEAD;
    } while (next->state == PROS_THREAD_DEAD);

    prev->state = PROS_THREAD_READY;
    if (!thread_process_runnable(prev)) {
        prev->state = PROS_THREAD_DEAD;
    } else if (thread_enqueue(prev) != 0) {
        prev->state = PROS_THREAD_RUNNING;
        thread_enqueue(next);
        return current_frame;
    }

    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;
    if (current_thread->process)
        process_set_current(current_thread->process);

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

#include "thread.h"
#include "spinlock.h"

static struct pros_thread *runq_head;
static struct pros_thread *runq_tail;
static struct pros_thread *current_thread;
static struct pros_spinlock runq_lock = PROS_SPINLOCK_INIT;

static int thread_process_runnable(const struct pros_thread *thread) {
    return thread && thread->process &&
           process_state(thread->process) != PROS_PROCESS_ZOMBIE &&
           process_state(thread->process) != PROS_PROCESS_DEAD;
}

static int thread_enqueue_locked(struct pros_thread *thread) {
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

static struct pros_thread *thread_dequeue_locked(void) {
    struct pros_thread *thread = runq_head;
    if (!thread) return 0;
    runq_head = thread->next;
    if (runq_head) runq_head->prev = 0;
    else runq_tail = 0;
    thread->next = 0;
    thread->prev = 0;
    return thread;
}

void thread_bootstrap(struct pros_thread *thread) {
    if (!thread || !thread->entry) for (;;) __asm__ volatile ("cli; hlt");
    ((void (*)(void))(uintptr_t)thread->entry)();
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    thread->state = PROS_THREAD_DEAD;
    current_thread = 0;
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    for (;;) __asm__ volatile ("cli; hlt");
}
extern void thread_start_trampoline(void);

int thread_manager_init(void) {
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    runq_head = 0;
    runq_tail = 0;
    current_thread = 0;
    pros_spin_unlock_irqrestore(&runq_lock, flags);
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
    thread->blocked_syscall = 0;
    thread->context.r15 = 0;
    thread->context.r14 = 0;
    thread->context.r13 = 0;
    thread->context.r12 = 0;
    thread->context.rbx = 0;
    thread->context.rbp = 0;
    uint64_t *stack = (uint64_t *)(uintptr_t)stack_top;
    *(--stack) = (uint64_t)(uintptr_t)thread_start_trampoline;
    *(--stack) = 0;
    *(--stack) = 0;
    *(--stack) = (uint64_t)(uintptr_t)thread;
    *(--stack) = 0;
    *(--stack) = 0;
    *(--stack) = 0;
    thread->context.rsp = (uint64_t)(uintptr_t)stack;
    thread->context.rip = entry;
    thread->context.rflags = 0x202;
    thread->next = 0;
    thread->prev = 0;
    return 0;
}

int thread_set_state(struct pros_thread *thread, uint32_t state) {
    if (!thread || state > PROS_THREAD_DEAD) return -1;
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    if (thread->state == PROS_THREAD_DEAD && state != PROS_THREAD_DEAD) {
        pros_spin_unlock_irqrestore(&runq_lock, flags);
        return -1;
    }
    thread->state = state;
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    return 0;
}

uint32_t thread_state(const struct pros_thread *thread) {
    if (!thread) return PROS_THREAD_DEAD;
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    uint32_t state = thread->state;
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    return state;
}

int thread_enqueue(struct pros_thread *thread) {
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    int result = thread_enqueue_locked(thread);
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    return result;
}

struct pros_thread *thread_dequeue(void) {
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    struct pros_thread *thread = thread_dequeue_locked();
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    return thread;
}

struct pros_thread *thread_current(void) {
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    struct pros_thread *thread = current_thread;
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    return thread;
}

int thread_schedule(void) {
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    struct pros_thread *next;
    do {
        next = thread_dequeue_locked();
        if (!next) {
            pros_spin_unlock_irqrestore(&runq_lock, flags);
            return -1;
        }
        if (!thread_process_runnable(next))
            next->state = PROS_THREAD_DEAD;
    } while (next->state == PROS_THREAD_DEAD);

    if (current_thread && current_thread != next &&
        current_thread->state == PROS_THREAD_RUNNING) {
        current_thread->state = PROS_THREAD_READY;
        if (thread_enqueue_locked(current_thread) != 0) {
            pros_spin_unlock_irqrestore(&runq_lock, flags);
            return -1;
        }
    }

    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;
    if (current_thread->process)
        process_set_current(current_thread->process);
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    return 0;
}

int thread_block(void *wait_channel) {
    if (!wait_channel) return -1;
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    if (!current_thread || current_thread->state != PROS_THREAD_RUNNING) {
        pros_spin_unlock_irqrestore(&runq_lock, flags);
        return -1;
    }
    current_thread->wait_channel = wait_channel;
    current_thread->state = PROS_THREAD_BLOCKED;
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    return 0;
}

int thread_wake(struct pros_thread *thread) {
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    if (!thread || thread->state != PROS_THREAD_BLOCKED ||
        !thread_process_runnable(thread)) {
        pros_spin_unlock_irqrestore(&runq_lock, flags);
        return -1;
    }
    thread->state = PROS_THREAD_READY;
    if (thread_enqueue_locked(thread) != 0) {
        thread->state = PROS_THREAD_BLOCKED;
        pros_spin_unlock_irqrestore(&runq_lock, flags);
        return -1;
    }
    if (thread->blocked_syscall != 0 && thread->interrupt_frame) {
        thread->interrupt_frame->rax = thread->blocked_syscall;
        thread->interrupt_frame->rip -= 2;
        thread->blocked_syscall = 0;
    }
    thread->wait_channel = 0;
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    return 0;
}

int thread_has_runnable(void) {
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    int runnable = 0;
    for (struct pros_thread *thread = runq_head; thread; thread = thread->next)
        if (thread_process_runnable(thread)) {
            runnable = 1;
            break;
        }
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    return runnable;
}

int thread_schedule_preempt(void) {
    struct pros_thread *prev = current_thread;
    if (thread_schedule() != 0) return -1;
    struct pros_thread *next = thread_current();
    if (prev && prev != next)
        thread_context_switch(prev, next);
    return 0;
}

struct pros_interrupt_frame *thread_schedule_interrupt(
    struct pros_interrupt_frame *current_frame) {
    if (!current_frame) return current_frame;
    uint64_t flags = pros_spin_lock_irqsave(&runq_lock);
    struct pros_thread *prev = current_thread;
    prev->interrupt_frame = current_frame;
    struct pros_thread *next;
    do {
        next = thread_dequeue_locked();
        if (!next) {
            pros_spin_unlock_irqrestore(&runq_lock, flags);
            return current_frame;
        }
        if (!thread_process_runnable(next))
            next->state = PROS_THREAD_DEAD;
    } while (next->state == PROS_THREAD_DEAD);

    if (prev->state != PROS_THREAD_BLOCKED) {
        prev->state = PROS_THREAD_READY;
        if (!thread_process_runnable(prev)) {
            prev->state = PROS_THREAD_DEAD;
        } else if (thread_enqueue_locked(prev) != 0) {
            prev->state = PROS_THREAD_RUNNING;
            (void)thread_enqueue_locked(next);
            pros_spin_unlock_irqrestore(&runq_lock, flags);
            return current_frame;
        }
    }

    current_thread = next;
    current_thread->state = PROS_THREAD_RUNNING;
    if (current_thread->process)
        process_set_current(current_thread->process);

    struct pros_interrupt_frame *result = next->interrupt_frame;
    if (!result) {
        result = (struct pros_interrupt_frame *)(uintptr_t)(next->context.rsp -
                                                             sizeof(*result));
        result->rax = result->rbx = result->rcx = result->rdx = 0;
        result->rsi = result->rdi = result->rbp = 0;
        result->r8 = result->r9 = result->r10 = result->r11 = 0;
        result->r12 = (uint64_t)(uintptr_t)next;
        result->r13 = result->r14 = result->r15 = 0;
        result->rip = (uint64_t)(uintptr_t)thread_start_trampoline;
        result->cs = 0x08;
        result->rflags = 0x202;
        next->interrupt_frame = result;
    }
    pros_spin_unlock_irqrestore(&runq_lock, flags);
    return result;
}

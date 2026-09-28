#ifndef PROSENJIT_THREAD_H
#define PROSENJIT_THREAD_H

#include <stdint.h>
#include "process.h"

enum pros_thread_state {
    PROS_THREAD_NEW = 0,
    PROS_THREAD_READY = 1,
    PROS_THREAD_RUNNING = 2,
    PROS_THREAD_BLOCKED = 3,
    PROS_THREAD_DEAD = 4,
};

struct pros_cpu_context {
    uint64_t r15, r14, r13, r12;
    uint64_t rbx, rbp;
    uint64_t rsp, rip;
    uint64_t rflags;
};

/* Exact memory layout at the RAX pointer passed by isr32.
 * The CPU's vector/error words live immediately before this pointer.
 * At the frame pointer itself the layout is RAX..R15, then the CPU's
 * kernel-mode IRET frame (RIP, CS, RFLAGS).
 */
struct pros_interrupt_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, cs, rflags;
};

struct pros_thread {
    uint64_t tid;
    struct pros_process *process;
    uint32_t state;
    uint32_t priority;
    uint64_t entry;
    struct pros_cpu_context context;
    struct pros_interrupt_frame *interrupt_frame;
    void *wait_channel;
    uint64_t blocked_syscall;
    struct pros_thread *next;
    struct pros_thread *prev;
};

int thread_manager_init(void);
int thread_init(struct pros_thread *thread, struct pros_process *process,
                uint64_t tid, uint64_t entry, uint64_t stack_top);
int thread_set_state(struct pros_thread *thread, uint32_t state);
uint32_t thread_state(const struct pros_thread *thread);
int thread_enqueue(struct pros_thread *thread);
struct pros_thread *thread_dequeue(void);
struct pros_thread *thread_current(void);
int thread_schedule(void);
int thread_schedule_preempt(void);
int thread_block(void *wait_channel);
int thread_wake(struct pros_thread *thread);
int thread_has_runnable(void);

/* Save the current IRQ frame and select the next runnable thread frame. */
struct pros_interrupt_frame *thread_schedule_interrupt(
    struct pros_interrupt_frame *current_frame);

/* Switches CPU execution from prev to next using their saved kernel stacks. */
void thread_context_switch(struct pros_thread *prev,
                           struct pros_thread *next);

#endif

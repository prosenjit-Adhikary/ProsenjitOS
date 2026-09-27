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

struct pros_thread {
    uint64_t tid;
    struct pros_process *process;
    uint32_t state;
    uint32_t priority;
    struct pros_cpu_context context;
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

/* Switches CPU execution from prev to next using their saved kernel stacks. */
void thread_context_switch(struct pros_thread *prev,
                           struct pros_thread *next);

#endif

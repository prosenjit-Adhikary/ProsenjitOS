#ifndef PROSENJIT_IPC_H
#define PROSENJIT_IPC_H

#include <stdint.h>
#include "object.h"
struct pros_thread;

#define PROSENJIT_IPC_CAPACITY 8u
#define PROSENJIT_IPC_MESSAGE_MAX 64u

#define PROSENJIT_IPC_OK 0
#define PROSENJIT_IPC_EINVAL -1
#define PROSENJIT_IPC_EFULL -2
#define PROSENJIT_IPC_EEMPTY -3
#define PROSENJIT_IPC_EMSGSIZE -4
#define PROSENJIT_IPC_EALREADY -5

struct pros_ipc_message {
    uint32_t length;
    uint8_t data[PROSENJIT_IPC_MESSAGE_MAX];
};

struct pros_ipc_channel {
    struct pros_object object;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    struct pros_ipc_message messages[PROSENJIT_IPC_CAPACITY];
    struct pros_thread *waiters[PROSENJIT_IPC_CAPACITY];
    uint32_t waiter_count;
    volatile uint8_t lock;
};

int ipc_channel_init(struct pros_ipc_channel *channel);
int ipc_channel_integrity_selftest(void);
int ipc_channel_send(struct pros_ipc_channel *channel, const void *data, uint32_t length);
int ipc_channel_receive(struct pros_ipc_channel *channel, void *data, uint32_t capacity, uint32_t *length_out);
uint32_t ipc_channel_count(struct pros_ipc_channel *channel);
uint32_t ipc_channel_capacity(const struct pros_ipc_channel *channel);
int ipc_channel_wait(struct pros_ipc_channel *channel, struct pros_thread *thread);
int ipc_channel_wake_one(struct pros_ipc_channel *channel);
int ipc_channel_cancel_waiter_preflight(struct pros_ipc_channel *channel,
                                         struct pros_thread *thread);
int ipc_channel_cancel_waiter(struct pros_ipc_channel *channel, struct pros_thread *thread);

#endif

#include "ipc.h"
#include "thread.h"

static void ipc_lock(struct pros_ipc_channel *channel) {
    while (__atomic_test_and_set(&channel->lock, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void ipc_unlock(struct pros_ipc_channel *channel) {
    __atomic_clear(&channel->lock, __ATOMIC_RELEASE);
}

static int ipc_channel_valid(const struct pros_ipc_channel *channel) {
    return channel &&
           channel->object.type == PROS_OBJECT_IPC &&
           channel->object.refcount != 0 &&
           channel->head < PROSENJIT_IPC_CAPACITY &&
           channel->tail < PROSENJIT_IPC_CAPACITY &&
           channel->count <= PROSENJIT_IPC_CAPACITY;
}

int ipc_channel_init(struct pros_ipc_channel *channel) {
    if (!channel) return PROSENJIT_IPC_EINVAL;
    object_init(&channel->object, PROS_OBJECT_IPC);
    channel->head = 0;
    channel->tail = 0;
    channel->count = 0;
    for (uint32_t i = 0; i < PROSENJIT_IPC_CAPACITY; ++i) {
        channel->messages[i].length = 0;
        channel->waiters[i] = 0;
    }
    channel->waiter_count = 0;
    channel->lock = 0;
    return PROSENJIT_IPC_OK;
}

int ipc_channel_send(struct pros_ipc_channel *channel,
                     const void *data, uint32_t length) {
    if (!ipc_channel_valid(channel)) return PROSENJIT_IPC_EINVAL;
    if (length > PROSENJIT_IPC_MESSAGE_MAX) return PROSENJIT_IPC_EMSGSIZE;
    if (length != 0 && !data) return PROSENJIT_IPC_EINVAL;

    ipc_lock(channel);
    if (channel->count == PROSENJIT_IPC_CAPACITY) {
        ipc_unlock(channel);
        return PROSENJIT_IPC_EFULL;
    }

    struct pros_ipc_message *message = &channel->messages[channel->tail];
    message->length = length;
    for (uint32_t i = 0; i < length; ++i)
        message->data[i] = ((const uint8_t *)data)[i];

    channel->tail = (channel->tail + 1u) % PROSENJIT_IPC_CAPACITY;
    ++channel->count;

    /*
     * Wake while holding the channel lock so publishing the message and
     * removing a receiver waiter are one atomic state transition.  The
     * wake path does not touch IPC state, so it is safe to run here.
     */
    if (channel->waiter_count != 0) {
        struct pros_thread *thread = channel->waiters[0];
        if (thread_wake(thread) == 0) {
            for (uint32_t i = 1; i < channel->waiter_count; ++i)
                channel->waiters[i - 1] = channel->waiters[i];
            --channel->waiter_count;
            channel->waiters[channel->waiter_count] = 0;
        }
    }
    ipc_unlock(channel);
    return PROSENJIT_IPC_OK;
}

int ipc_channel_receive(struct pros_ipc_channel *channel,
                        void *data, uint32_t capacity,
                        uint32_t *length_out) {
    if (!ipc_channel_valid(channel) || !length_out)
        return PROSENJIT_IPC_EINVAL;

    ipc_lock(channel);
    if (channel->count == 0) {
        ipc_unlock(channel);
        return PROSENJIT_IPC_EEMPTY;
    }

    struct pros_ipc_message *message = &channel->messages[channel->head];
    if (message->length > capacity) {
        ipc_unlock(channel);
        return PROSENJIT_IPC_EMSGSIZE;
    }
    if (message->length != 0 && !data) {
        ipc_unlock(channel);
        return PROSENJIT_IPC_EINVAL;
    }

    for (uint32_t i = 0; i < message->length; ++i)
        ((uint8_t *)data)[i] = message->data[i];

    *length_out = message->length;
    message->length = 0;
    channel->head = (channel->head + 1u) % PROSENJIT_IPC_CAPACITY;
    --channel->count;
    ipc_unlock(channel);
    return PROSENJIT_IPC_OK;
}

uint32_t ipc_channel_count(const struct pros_ipc_channel *channel) {
    return ipc_channel_valid(channel) ? channel->count : 0;
}

uint32_t ipc_channel_capacity(const struct pros_ipc_channel *channel) {
    return channel ? PROSENJIT_IPC_CAPACITY : 0;
}

int ipc_channel_wait(struct pros_ipc_channel *channel, struct pros_thread *thread) {
    if (!ipc_channel_valid(channel) || !thread)
        return PROSENJIT_IPC_EINVAL;
    ipc_lock(channel);
    if (channel->waiter_count >= PROSENJIT_IPC_CAPACITY) {
        ipc_unlock(channel);
        return PROSENJIT_IPC_EINVAL;
    }
    if (channel->count != 0) {
        ipc_unlock(channel);
        return PROSENJIT_IPC_OK;
    }
    for (uint32_t i = 0; i < channel->waiter_count; ++i) {
        if (channel->waiters[i] == thread) {
            ipc_unlock(channel);
            return PROSENJIT_IPC_EINVAL;
        }
    }
    if (thread_block(channel) != 0) {
        ipc_unlock(channel);
        return PROSENJIT_IPC_EINVAL;
    }
    channel->waiters[channel->waiter_count++] = thread;
    ipc_unlock(channel);
    return PROSENJIT_IPC_OK;
}

int ipc_channel_cancel_waiter(struct pros_ipc_channel *channel,
                                struct pros_thread *thread) {
    if (!ipc_channel_valid(channel) || !thread) return PROSENJIT_IPC_EINVAL;
    ipc_lock(channel);
    for (uint32_t i = 0; i < channel->waiter_count; ++i) {
        if (channel->waiters[i] != thread) continue;
        for (uint32_t j = i + 1; j < channel->waiter_count; ++j)
            channel->waiters[j - 1] = channel->waiters[j];
        --channel->waiter_count;
        channel->waiters[channel->waiter_count] = 0;
        ipc_unlock(channel);
        return PROSENJIT_IPC_OK;
    }
    ipc_unlock(channel);
    return PROSENJIT_IPC_EEMPTY;
}

int ipc_channel_wake_one(struct pros_ipc_channel *channel) {
    if (!ipc_channel_valid(channel)) return PROSENJIT_IPC_EINVAL;
    ipc_lock(channel);
    if (channel->waiter_count == 0) {
        ipc_unlock(channel);
        return PROSENJIT_IPC_EEMPTY;
    }
    struct pros_thread *thread = channel->waiters[0];
    int rc = thread_wake(thread);
    if (rc == 0) {
        for (uint32_t i = 1; i < channel->waiter_count; ++i)
            channel->waiters[i - 1] = channel->waiters[i];
        --channel->waiter_count;
        channel->waiters[channel->waiter_count] = 0;
    }
    ipc_unlock(channel);
    return rc == 0 ? PROSENJIT_IPC_OK : PROSENJIT_IPC_EINVAL;
}

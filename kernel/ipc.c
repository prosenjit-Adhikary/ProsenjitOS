#include "ipc.h"

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
    for (uint32_t i = 0; i < PROSENJIT_IPC_CAPACITY; ++i)
        channel->messages[i].length = 0;
    return PROSENJIT_IPC_OK;
}

int ipc_channel_send(struct pros_ipc_channel *channel,
                     const void *data, uint32_t length) {
    if (!ipc_channel_valid(channel)) return PROSENJIT_IPC_EINVAL;
    if (length > PROSENJIT_IPC_MESSAGE_MAX) return PROSENJIT_IPC_EMSGSIZE;
    if (length != 0 && !data) return PROSENJIT_IPC_EINVAL;
    if (channel->count == PROSENJIT_IPC_CAPACITY) return PROSENJIT_IPC_EFULL;

    struct pros_ipc_message *message = &channel->messages[channel->tail];
    message->length = length;
    for (uint32_t i = 0; i < length; ++i)
        message->data[i] = ((const uint8_t *)data)[i];

    channel->tail = (channel->tail + 1u) % PROSENJIT_IPC_CAPACITY;
    ++channel->count;
    return PROSENJIT_IPC_OK;
}

int ipc_channel_receive(struct pros_ipc_channel *channel,
                        void *data, uint32_t capacity,
                        uint32_t *length_out) {
    if (!ipc_channel_valid(channel) || !length_out)
        return PROSENJIT_IPC_EINVAL;
    if (channel->count == 0) return PROSENJIT_IPC_EEMPTY;

    struct pros_ipc_message *message = &channel->messages[channel->head];
    if (message->length > capacity) return PROSENJIT_IPC_EMSGSIZE;
    if (message->length != 0 && !data) return PROSENJIT_IPC_EINVAL;

    for (uint32_t i = 0; i < message->length; ++i)
        ((uint8_t *)data)[i] = message->data[i];

    *length_out = message->length;
    message->length = 0;
    channel->head = (channel->head + 1u) % PROSENJIT_IPC_CAPACITY;
    --channel->count;
    return PROSENJIT_IPC_OK;
}

uint32_t ipc_channel_count(const struct pros_ipc_channel *channel) {
    return ipc_channel_valid(channel) ? channel->count : 0;
}

uint32_t ipc_channel_capacity(const struct pros_ipc_channel *channel) {
    return channel ? PROSENJIT_IPC_CAPACITY : 0;
}

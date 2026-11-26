#include "ipc.h"
#include "thread.h"
#include "paging.h"

/*
 * Prevent the local timer interrupt from scheduling another thread while
 * this CPU owns the channel lock.  The atomic flag still serializes access
 * between CPUs; restoring only IF preserves callers that entered with IRQs
 * already disabled.
 */
static uint64_t ipc_lock(struct pros_ipc_channel *channel) {
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli"
                      : "=r"(irq_flags) :: "memory", "cc");
    while (__atomic_test_and_set(&channel->lock, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
    return irq_flags;
}

static void ipc_unlock(struct pros_ipc_channel *channel, uint64_t irq_flags) {
    __atomic_clear(&channel->lock, __ATOMIC_RELEASE);
    if (irq_flags & (1ull << 9))
        __asm__ volatile ("sti" ::: "memory");
}

/*
 * wait_channel is a kernel pointer stored in a thread descriptor. A corrupted
 * value must be rejected before ipc_lock() or object-field access can fault.
 * Prove the complete channel object is mapped supervisor-writable memory.
 */
static int ipc_channel_pointer_writable(
    const struct pros_ipc_channel *channel) {
    if (!channel)
        return 0;
    uint64_t start = (uint64_t)(uintptr_t)channel;
    if (start > UINT64_MAX - (uint64_t)sizeof(*channel) + 1u)
        return 0;
    uint64_t end = start + (uint64_t)sizeof(*channel) - 1u;
    uint64_t page = start & ~0xFFFULL;
    uint64_t last_page = end & ~0xFFFULL;
    for (;;) {
        uint64_t flags = 0u;
        if (paging_query_effective_page(page, &flags) != 0 ||
            !(flags & PROSENJIT_PAGE_PRESENT) ||
            !(flags & PROSENJIT_PAGE_WRITABLE) ||
            (flags & PROSENJIT_PAGE_USER))
            return 0;
        if (page == last_page)
            break;
        if (page > UINT64_MAX - 0x1000ULL)
            return 0;
        page += 0x1000ULL;
    }
    return 1;
}

static int ipc_channel_valid(const struct pros_ipc_channel *channel) {
    return ipc_channel_pointer_writable(channel) &&
           channel->object.type == PROS_OBJECT_IPC &&
           channel->object.reserved == 0 &&
           channel->object.refcount != 0 &&
           channel->head < PROSENJIT_IPC_CAPACITY &&
           channel->tail < PROSENJIT_IPC_CAPACITY &&
           channel->count <= PROSENJIT_IPC_CAPACITY &&
           channel->waiter_count <= PROSENJIT_IPC_CAPACITY &&
           channel->receive_reserved <= 1u &&
           (channel->receive_reserved
                ? (channel->count != 0u &&
                   channel->receive_reservation_token != 0u)
                : channel->receive_reservation_token == 0u);
}

/*
 * Validate the complete waiter ownership set while the channel lock is held.
 * Active entries must be registered BLOCKED threads pointing back to this
 * channel; duplicates and non-NULL unused slots are corruption.
 */
static int ipc_channel_waiters_valid_locked(
    const struct pros_ipc_channel *channel) {
    if (!channel || channel->waiter_count > PROSENJIT_IPC_CAPACITY)
        return -1;
    for (uint32_t i = 0u; i < channel->waiter_count; ++i) {
        struct pros_thread *waiter = channel->waiters[i];
        if (!waiter || thread_ipc_waiter_validate(waiter, channel) != 0)
            return -1;
        for (uint32_t j = 0u; j < i; ++j)
            if (channel->waiters[j] == waiter)
                return -1;
    }
    for (uint32_t i = channel->waiter_count;
         i < PROSENJIT_IPC_CAPACITY; ++i)
        if (channel->waiters[i])
            return -1;
    return 0;
}

/*
 * Validate ring geometry while the channel lock is held. The producer index
 * must equal head + count modulo capacity, and every slot outside the active
 * ring must have a cleared length. Active message lengths are checked by
 * receive immediately before copying, so that its corruption regression
 * exercises the actual copy boundary.
 */
static int ipc_channel_queue_valid_locked(
    const struct pros_ipc_channel *channel) {
    if (!channel || channel->head >= PROSENJIT_IPC_CAPACITY ||
        channel->tail >= PROSENJIT_IPC_CAPACITY ||
        channel->count > PROSENJIT_IPC_CAPACITY ||
        channel->tail !=
            (channel->head + channel->count) % PROSENJIT_IPC_CAPACITY)
        return -1;

    for (uint32_t offset = channel->count;
         offset < PROSENJIT_IPC_CAPACITY; ++offset) {
        uint32_t index =
            (channel->head + offset) % PROSENJIT_IPC_CAPACITY;
        if (channel->messages[index].length != 0u)
            return -1;
    }
    return 0;
}

static int ipc_channel_state_valid_locked(
    const struct pros_ipc_channel *channel) {
    return ipc_channel_queue_valid_locked(channel) == 0 &&
           ipc_channel_waiters_valid_locked(channel) == 0 ? 0 : -1;
}

int ipc_channel_init(struct pros_ipc_channel *channel) {
    if (!ipc_channel_pointer_writable(channel))
        return PROSENJIT_IPC_EINVAL;
    object_init(&channel->object, PROS_OBJECT_IPC);
    channel->head = 0;
    channel->tail = 0;
    channel->count = 0;
    for (uint32_t i = 0; i < PROSENJIT_IPC_CAPACITY; ++i) {
        channel->messages[i].length = 0;
        channel->waiters[i] = 0;
    }
    channel->waiter_count = 0;
    channel->receive_reserved = 0u;
    channel->receive_reservation_sequence = 0u;
    channel->receive_reservation_token = 0u;
    channel->lock = 0;
    return PROSENJIT_IPC_OK;
}

int ipc_channel_send(struct pros_ipc_channel *channel,
                     const void *data, uint32_t length) {
    if (!ipc_channel_valid(channel)) return PROSENJIT_IPC_EINVAL;
    if (length > PROSENJIT_IPC_MESSAGE_MAX) return PROSENJIT_IPC_EMSGSIZE;
    if (length != 0 && !data) return PROSENJIT_IPC_EINVAL;

    uint64_t irq_flags = ipc_lock(channel);
    if (!ipc_channel_valid(channel) ||
        ipc_channel_state_valid_locked(channel) != 0) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    if (channel->count == PROSENJIT_IPC_CAPACITY) {
        ipc_unlock(channel, irq_flags);
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
    ipc_unlock(channel, irq_flags);
    return PROSENJIT_IPC_OK;
}

int ipc_channel_receive(struct pros_ipc_channel *channel,
                        void *data, uint32_t capacity,
                        uint32_t *length_out) {
    if (!ipc_channel_valid(channel) || !length_out)
        return PROSENJIT_IPC_EINVAL;

    uint64_t irq_flags = ipc_lock(channel);
    if (!ipc_channel_valid(channel) ||
        ipc_channel_state_valid_locked(channel) != 0) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    if (channel->receive_reserved) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EBUSY;
    }
    if (channel->count == 0) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EEMPTY;
    }

    struct pros_ipc_message *message = &channel->messages[channel->head];
    if (message->length > PROSENJIT_IPC_MESSAGE_MAX) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    if (message->length > capacity) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EMSGSIZE;
    }
    if (message->length != 0 && !data) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }

    for (uint32_t i = 0; i < message->length; ++i)
        ((uint8_t *)data)[i] = message->data[i];

    *length_out = message->length;
    message->length = 0;
    channel->head = (channel->head + 1u) % PROSENJIT_IPC_CAPACITY;
    --channel->count;
    ipc_unlock(channel, irq_flags);
    return PROSENJIT_IPC_OK;
}

/*
 * Reserve the current head message without removing it. The caller may then
 * copy the staged bytes to user memory and either commit (consume) or cancel
 * (leave the message queued). A monotonically increasing nonzero token keeps
 * stale completion attempts from consuming a later message.
 */
int ipc_channel_receive_reserve(struct pros_ipc_channel *channel, void *data,
                                uint32_t capacity, uint32_t *length_out,
                                uint64_t *token_out) {
    if (!ipc_channel_valid(channel) || !length_out || !token_out)
        return PROSENJIT_IPC_EINVAL;

    uint64_t irq_flags = ipc_lock(channel);
    if (!ipc_channel_valid(channel) ||
        ipc_channel_state_valid_locked(channel) != 0) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    if (channel->receive_reserved) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EBUSY;
    }
    if (channel->count == 0u) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EEMPTY;
    }

    struct pros_ipc_message *message = &channel->messages[channel->head];
    if (message->length > PROSENJIT_IPC_MESSAGE_MAX) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    if (message->length > capacity) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EMSGSIZE;
    }
    if (message->length != 0u && !data) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    if (channel->receive_reservation_sequence == UINT64_MAX) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }

    uint64_t token = channel->receive_reservation_sequence + 1u;
    for (uint32_t i = 0u; i < message->length; ++i)
        ((uint8_t *)data)[i] = message->data[i];

    channel->receive_reservation_sequence = token;
    channel->receive_reservation_token = token;
    channel->receive_reserved = 1u;
    *length_out = message->length;
    *token_out = token;
    ipc_unlock(channel, irq_flags);
    return PROSENJIT_IPC_OK;
}

/* Commit only the exact outstanding reservation; no other receiver can
 * consume the head slot between reserve and commit/cancel. */
int ipc_channel_receive_commit(struct pros_ipc_channel *channel,
                               uint64_t token) {
    if (!ipc_channel_valid(channel) || token == 0u)
        return PROSENJIT_IPC_EINVAL;

    uint64_t irq_flags = ipc_lock(channel);
    if (!ipc_channel_valid(channel) ||
        ipc_channel_state_valid_locked(channel) != 0 ||
        !channel->receive_reserved ||
        channel->receive_reservation_token != token ||
        channel->count == 0u) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }

    struct pros_ipc_message *message = &channel->messages[channel->head];
    if (message->length > PROSENJIT_IPC_MESSAGE_MAX) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    message->length = 0u;
    channel->head = (channel->head + 1u) % PROSENJIT_IPC_CAPACITY;
    --channel->count;
    channel->receive_reserved = 0u;
    channel->receive_reservation_token = 0u;
    ipc_unlock(channel, irq_flags);
    return PROSENJIT_IPC_OK;
}

/* Cancel is idempotence-resistant: only the live token can release the slot. */
int ipc_channel_receive_cancel(struct pros_ipc_channel *channel,
                               uint64_t token) {
    if (!ipc_channel_valid(channel) || token == 0u)
        return PROSENJIT_IPC_EINVAL;

    uint64_t irq_flags = ipc_lock(channel);
    if (!ipc_channel_valid(channel) ||
        ipc_channel_state_valid_locked(channel) != 0 ||
        !channel->receive_reserved ||
        channel->receive_reservation_token != token) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    channel->receive_reserved = 0u;
    channel->receive_reservation_token = 0u;
    ipc_unlock(channel, irq_flags);
    return PROSENJIT_IPC_OK;
}

uint32_t ipc_channel_count(struct pros_ipc_channel *channel) {
    if (!ipc_channel_pointer_writable(channel)) return 0;
    uint64_t irq_flags = ipc_lock(channel);
    uint32_t count = 0;
    if (ipc_channel_valid(channel) &&
        ipc_channel_state_valid_locked(channel) == 0)
        count = channel->count;
    ipc_unlock(channel, irq_flags);
    return count;
}

uint32_t ipc_channel_capacity(const struct pros_ipc_channel *channel) {
    return channel ? PROSENJIT_IPC_CAPACITY : 0;
}

int ipc_channel_wait(struct pros_ipc_channel *channel, struct pros_thread *thread) {
    if (!ipc_channel_valid(channel) || !thread || thread_current() != thread)
        return PROSENJIT_IPC_EINVAL;
    uint64_t irq_flags = ipc_lock(channel);
    if (!ipc_channel_valid(channel) ||
        ipc_channel_state_valid_locked(channel) != 0) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    /*
     * A queued message takes precedence over waiter capacity: callers must
     * retry receive instead of blocking after a sender has already won.
     */
    if (channel->count != 0) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EALREADY;
    }
    if (channel->waiter_count >= PROSENJIT_IPC_CAPACITY) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    for (uint32_t i = 0; i < channel->waiter_count; ++i) {
        if (channel->waiters[i] == thread) {
            ipc_unlock(channel, irq_flags);
            return PROSENJIT_IPC_EINVAL;
        }
    }
    if (thread_block(channel) != 0) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    channel->waiters[channel->waiter_count++] = thread;
    ipc_unlock(channel, irq_flags);
    return PROSENJIT_IPC_OK;
}

/*
 * Read-only cancellation gate used by process termination before it mutates
 * any run-queue or thread lifecycle state. The bounded waiter array must be
 * internally consistent and contain the target exactly once.
 */
int ipc_channel_cancel_waiter_preflight(struct pros_ipc_channel *channel,
                                         struct pros_thread *thread) {
    if (!ipc_channel_pointer_writable(channel) || !thread)
        return PROSENJIT_IPC_EINVAL;

    uint64_t irq_flags = ipc_lock(channel);
    int result = PROSENJIT_IPC_EINVAL;
    if (!ipc_channel_valid(channel) ||
        ipc_channel_state_valid_locked(channel) != 0)
        goto out;

    uint32_t matches = 0u;
    for (uint32_t i = 0u; i < channel->waiter_count; ++i)
        if (channel->waiters[i] == thread)
            ++matches;
    result = matches == 1u ? PROSENJIT_IPC_OK : PROSENJIT_IPC_EEMPTY;
out:
    ipc_unlock(channel, irq_flags);
    return result;
}

int ipc_channel_cancel_waiter(struct pros_ipc_channel *channel,
                                struct pros_thread *thread) {
    if (!ipc_channel_pointer_writable(channel) || !thread)
        return PROSENJIT_IPC_EINVAL;
    uint64_t irq_flags = ipc_lock(channel);
    if (!ipc_channel_valid(channel) ||
        ipc_channel_state_valid_locked(channel) != 0) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    for (uint32_t i = 0; i < channel->waiter_count; ++i) {
        if (channel->waiters[i] != thread) continue;
        for (uint32_t j = i + 1; j < channel->waiter_count; ++j)
            channel->waiters[j - 1] = channel->waiters[j];
        --channel->waiter_count;
        channel->waiters[channel->waiter_count] = 0;
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_OK;
    }
    ipc_unlock(channel, irq_flags);
    return PROSENJIT_IPC_EEMPTY;
}

int ipc_channel_wake_one(struct pros_ipc_channel *channel) {
    if (!ipc_channel_valid(channel)) return PROSENJIT_IPC_EINVAL;
    uint64_t irq_flags = ipc_lock(channel);
    if (!ipc_channel_valid(channel) ||
        ipc_channel_state_valid_locked(channel) != 0) {
        ipc_unlock(channel, irq_flags);
        return PROSENJIT_IPC_EINVAL;
    }
    /*
     * A waiter is resumed only when a message is available. This also lets
     * the scheduler retry a wakeup that arrived before its fresh interrupt
     * frame had been published.
     */
    if (channel->waiter_count == 0 || channel->count == 0) {
        ipc_unlock(channel, irq_flags);
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
    ipc_unlock(channel, irq_flags);
    return rc == 0 ? PROSENJIT_IPC_OK : PROSENJIT_IPC_EINVAL;
}

/*
 * Reject a corrupted waiter count before any public operation can index the
 * fixed-size waiter array. The synthetic thread pointer is safe here because
 * every tested operation must reject the channel before dereferencing it.
 */
int ipc_channel_integrity_selftest(void) {
    struct pros_ipc_channel channel;
    uint32_t received = 0;
    struct pros_thread *synthetic = (struct pros_thread *)(uintptr_t)1u;

    if (ipc_channel_init(&channel) != PROSENJIT_IPC_OK)
        return -1;

    struct pros_ipc_channel *invalid_channel =
        (struct pros_ipc_channel *)(uintptr_t)UINT64_MAX;
    if (ipc_channel_init(invalid_channel) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_send(invalid_channel, 0, 0) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_receive(invalid_channel, 0, 0, &received) !=
            PROSENJIT_IPC_EINVAL ||
        ipc_channel_cancel_waiter_preflight(invalid_channel, synthetic) !=
            PROSENJIT_IPC_EINVAL ||
        ipc_channel_cancel_waiter(invalid_channel, synthetic) !=
            PROSENJIT_IPC_EINVAL ||
        ipc_channel_wake_one(invalid_channel) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_count(invalid_channel) != 0u)
        return -12;

    /* Ring geometry, inactive-slot clearing, and object header are trusted. */
    channel.tail = 1u;
    if (ipc_channel_send(&channel, 0, 0) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_receive(&channel, 0, 0, &received) != PROSENJIT_IPC_EINVAL)
        return -9;
    channel.tail = 0u;
    channel.messages[0].length = 1u;
    if (ipc_channel_send(&channel, 0, 0) != PROSENJIT_IPC_EINVAL)
        return -10;
    channel.messages[0].length = 0u;
    channel.object.reserved = 1u;
    if (ipc_channel_send(&channel, 0, 0) != PROSENJIT_IPC_EINVAL)
        return -11;
    channel.object.reserved = 0u;

    channel.waiter_count = PROSENJIT_IPC_CAPACITY + 1u;
    if (ipc_channel_send(&channel, 0, 0) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_receive(&channel, 0, 0, &received) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_wait(&channel, synthetic) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_cancel_waiter(&channel, synthetic) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_cancel_waiter_preflight(&channel, synthetic) !=
            PROSENJIT_IPC_EINVAL ||
        ipc_channel_wake_one(&channel) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_count(&channel) != 0u)
        return -2;

    channel.waiter_count = 0u;
    if (ipc_channel_cancel_waiter_preflight(&channel, synthetic) !=
        PROSENJIT_IPC_EEMPTY)
        return -3;

    channel.waiters[0] = synthetic;
    channel.waiter_count = 1u;
    channel.count = 1u;
    channel.tail = (channel.head + 1u) % PROSENJIT_IPC_CAPACITY;
    channel.messages[channel.head].length = 0u;
    if (ipc_channel_send(&channel, 0, 0) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_receive(&channel, 0, 0, &received) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_count(&channel) != 0u ||
        ipc_channel_wait(&channel, synthetic) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_cancel_waiter(&channel, synthetic) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_wake_one(&channel) != PROSENJIT_IPC_EINVAL ||
        ipc_channel_cancel_waiter_preflight(&channel, synthetic) !=
            PROSENJIT_IPC_EINVAL)
        return -4;
    channel.count = 0u;
    channel.tail = channel.head;
    channel.messages[channel.head].length = 0u;

    channel.waiters[1] = synthetic;
    channel.waiter_count = 2u;
    if (ipc_channel_cancel_waiter_preflight(&channel, synthetic) !=
        PROSENJIT_IPC_EINVAL)
        return -5;

    channel.waiters[1] = 0;
    channel.waiter_count = 0u;
    if (ipc_channel_cancel_waiter_preflight(&channel, synthetic) !=
        PROSENJIT_IPC_EINVAL)
        return -6;

    channel.waiters[0] = 0;
    if (ipc_channel_cancel_waiter_preflight(&channel, synthetic) !=
        PROSENJIT_IPC_EEMPTY)
        return -7;

    /*
     * A corrupted message length must be rejected before copying, publishing
     * length_out, or consuming the queued slot.
     */
    uint32_t original_head = channel.head;
    received = 0xA5A5A5A5u;
    channel.count = 1u;
    channel.tail = (channel.head + 1u) % PROSENJIT_IPC_CAPACITY;
    channel.messages[channel.head].length =
        PROSENJIT_IPC_MESSAGE_MAX + 1u;
    if (ipc_channel_receive(&channel, 0, 0, &received) !=
            PROSENJIT_IPC_EINVAL ||
        channel.count != 1u || channel.head != original_head ||
        received != 0xA5A5A5A5u)
        return -8;

    channel.count = 0u;
    channel.tail = channel.head;
    channel.messages[channel.head].length = 0u;

    /* Copy-out failure can cancel a reservation without losing the message. */
    const uint8_t payload[3] = {0x31u, 0x52u, 0x73u};
    uint8_t staged[3] = {0u, 0u, 0u};
    uint32_t staged_length = 0u;
    uint64_t token = 0u;
    if (ipc_channel_send(&channel, payload, sizeof(payload)) !=
            PROSENJIT_IPC_OK ||
        ipc_channel_receive_reserve(&channel, staged, sizeof(staged),
                                    &staged_length, &token) !=
            PROSENJIT_IPC_OK ||
        token == 0u || staged_length != sizeof(payload) ||
        staged[0] != payload[0] || staged[1] != payload[1] ||
        staged[2] != payload[2] || channel.count != 1u ||
        ipc_channel_receive(&channel, staged, sizeof(staged),
                            &staged_length) != PROSENJIT_IPC_EBUSY)
        return -13;
    if (ipc_channel_receive_cancel(&channel, token) != PROSENJIT_IPC_OK ||
        ipc_channel_receive_commit(&channel, token) != PROSENJIT_IPC_EINVAL ||
        channel.count != 1u)
        return -14;

    token = 0u;
    if (ipc_channel_receive_reserve(&channel, staged, sizeof(staged),
                                    &staged_length, &token) !=
            PROSENJIT_IPC_OK ||
        ipc_channel_receive_reserve(&channel, staged, sizeof(staged),
                                    &staged_length, &received) !=
            PROSENJIT_IPC_EBUSY ||
        ipc_channel_receive_commit(&channel, token) != PROSENJIT_IPC_OK ||
        channel.count != 0u || channel.receive_reserved != 0u)
        return -15;
    return 0;
}

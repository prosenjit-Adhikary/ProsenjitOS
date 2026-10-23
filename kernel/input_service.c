#include "input_service.h"
#include "ps2_keyboard.h"
#include "process.h"
#include "object.h"

#define INPUT_QUEUE_MASK (PROS_INPUT_SERVICE_QUEUE_CAPACITY - 1u)

#if (PROS_INPUT_SERVICE_QUEUE_CAPACITY & (PROS_INPUT_SERVICE_QUEUE_CAPACITY - 1u)) != 0
#error "input service queue capacity must be a power of two"
#endif

struct input_service_state {
    struct pros_hw_input_event events[PROS_INPUT_SERVICE_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t dropped;
    uint8_t ready;
};
static struct input_service_state input_state;
static struct pros_object input_object;
static uint8_t input_object_initialized;
static uint32_t input_lock;

static uint64_t input_irq_save(void)
{
#if __STDC_HOSTED__
    return 0u;
#else
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
#endif
}

static void input_irq_restore(uint64_t flags)
{
#if __STDC_HOSTED__
    (void)flags;
#else
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
#endif
}

static uint64_t input_lock_acquire(void)
{
    uint64_t flags = input_irq_save();
    while (__atomic_exchange_n(&input_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
    return flags;
}

static void input_lock_release(uint64_t flags)
{
    __atomic_store_n(&input_lock, 0u, __ATOMIC_RELEASE);
    input_irq_restore(flags);
}

static int input_endpoint_init(void *context)
{
    if (context != &input_state) return PROS_DRIVER_EINVAL;
    uint64_t flags = input_lock_acquire();
    input_state.head = 0u;
    input_state.tail = 0u;
    input_state.dropped = 0u;
    input_state.ready = 1u;
    input_lock_release(flags);
    return PROS_DRIVER_OK;
}

static int input_endpoint_transfer(void *context, const void *buffer,
                                   uint32_t length)
{
    if (context != &input_state || !buffer ||
        length != (uint32_t)sizeof(struct pros_hw_input_event))
        return PROS_DRIVER_EINVAL;

    const struct pros_hw_input_event *event =
        (const struct pros_hw_input_event *)buffer;
    if (event->version != PROS_HW_STACK_ABI_VERSION ||
        (event->type != PROS_HW_INPUT_KEY &&
         event->type != PROS_HW_INPUT_POINTER))
        return PROS_DRIVER_EINVAL;

    uint64_t flags = input_lock_acquire();
    if (!input_state.ready) {
        input_lock_release(flags);
        return PROS_DRIVER_ENODEV;
    }
    if ((uint32_t)(input_state.head - input_state.tail) >=
        PROS_INPUT_SERVICE_QUEUE_CAPACITY) {
        ++input_state.dropped;
        input_lock_release(flags);
        return PROS_DRIVER_ENOSPC;
    }

    input_state.events[input_state.head & INPUT_QUEUE_MASK] = *event;
    ++input_state.head;
    input_lock_release(flags);
    return PROS_DRIVER_OK;
}

static int input_endpoint_shutdown(void *context)
{
    if (context != &input_state) return PROS_DRIVER_EINVAL;
    uint64_t flags = input_lock_acquire();
    input_state.ready = 0u;
    input_lock_release(flags);
    return PROS_DRIVER_OK;
}

int pros_input_service_init(void)
{
    static const struct pros_hw_endpoint_ops ops = {
        input_endpoint_init,
        input_endpoint_transfer,
        input_endpoint_shutdown
    };

    uint64_t flags = input_lock_acquire();
    if (input_state.ready) {
        input_lock_release(flags);
        return PROS_DRIVER_EBUSY;
    }
    input_state.head = 0u;
    input_state.tail = 0u;
    input_state.dropped = 0u;
    if (!input_object_initialized) {
        object_init(&input_object, PROS_OBJECT_DEVICE);
        input_object_initialized = 1u;
    }
    input_lock_release(flags);

    return pros_hw_endpoint_register(PROS_INPUT_SERVICE_ENDPOINT_ID,
                                     PROS_HW_ENDPOINT_INPUT,
                                     &ops, &input_state);
}

int pros_input_service_grant_read(struct pros_process *process,
                                  uint64_t *handle_out)
{
    if (!process || !handle_out) return PROS_DRIVER_EINVAL;
    uint64_t flags = input_irq_save();
    if (!input_state.ready || !input_object_initialized) {
        input_irq_restore(flags);
        return PROS_DRIVER_ENODEV;
    }

    /* Read-only authority: no write/admin/duplicate/transfer rights. */
    int rc = process_grant_handle(process, &input_object,
                                  PROSENJIT_CAP_READ, handle_out);
    input_irq_restore(flags);
    return rc == 0 ? PROS_DRIVER_OK : PROS_DRIVER_EPERM;
}

int pros_input_service_read_capability(const struct pros_process *process,
                                       uint64_t handle,
                                       struct pros_hw_input_event *out)
{
    if (!process || !out || handle == PROSENJIT_HANDLE_INVALID)
        return PROS_DRIVER_EINVAL;

    /* Prevent a timer preemption between handle validation and queue access.
     * The current kernel is single-CPU; SMP will need process-handle locking. */
    uint64_t irq_flags = input_irq_save();
    if (!input_state.ready || !input_object_initialized ||
        process_check_handle(process, handle, PROSENJIT_CAP_READ,
                             PROS_OBJECT_DEVICE) != 0) {
        input_irq_restore(irq_flags);
        return PROS_DRIVER_EPERM;
    }

    uint32_t slot = (uint32_t)(handle & 0xFFULL);
    uint16_t generation = (uint16_t)(handle >> 8);
    if (slot == 0u || slot >= PROSENJIT_HANDLE_MAX || generation == 0u ||
        process->handles.entries[slot].generation != generation ||
        process->handles.entries[slot].object != &input_object) {
        input_irq_restore(irq_flags);
        return PROS_DRIVER_EPERM;
    }

    int rc = pros_input_service_read(out);
    input_irq_restore(irq_flags);
    return rc;
}

int pros_input_service_poll_hardware(void)
{
    if (!pros_input_service_is_ready()) return PROS_DRIVER_ENODEV;
    return pros_ps2_keyboard_poll(PROS_INPUT_SERVICE_ENDPOINT_ID);
}

int pros_input_service_read(struct pros_hw_input_event *out)
{
    if (!out) return PROS_DRIVER_EINVAL;
    uint64_t flags = input_lock_acquire();
    if (!input_state.ready) {
        input_lock_release(flags);
        return PROS_DRIVER_ENODEV;
    }
    if (input_state.tail == input_state.head) {
        input_lock_release(flags);
        return PROS_DRIVER_EBUSY;
    }

    *out = input_state.events[input_state.tail & INPUT_QUEUE_MASK];
    ++input_state.tail;
    input_lock_release(flags);
    return PROS_DRIVER_OK;
}

uint32_t pros_input_service_dropped_events(void)
{
    uint64_t flags = input_lock_acquire();
    uint32_t dropped = input_state.dropped;
    input_lock_release(flags);
    return dropped;
}

int pros_input_service_is_ready(void)
{
    uint64_t flags = input_lock_acquire();
    int ready = input_state.ready ? 1 : 0;
    input_lock_release(flags);
    return ready;
}

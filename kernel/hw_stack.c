#include "hw_stack.h"
struct endpoint_slot {
    uint32_t device_id;
    enum pros_hw_endpoint_type type;
    const struct pros_hw_endpoint_ops *ops;
    void *context;
    int32_t last_error;
    uint8_t used, initialized, state;
    uint32_t in_flight;
};
static struct endpoint_slot endpoints[PROS_HW_ENDPOINT_MAX];
static uint8_t stack_ready;
static uint32_t endpoint_lock;

enum endpoint_state {
    ENDPOINT_FREE = 0,
    ENDPOINT_INITIALIZING,
    ENDPOINT_ACTIVE,
    ENDPOINT_CLEANUP_PENDING,
    ENDPOINT_SHUTTING_DOWN
};

/*
 * Protect the registry against scheduler preemption and concurrent CPUs.
 * Host-side contract tests are hosted processes and must not execute CLI/STI;
 * freestanding kernel builds save/restore RFLAGS around the same atomic lock.
 */
static uint64_t endpoint_irq_save(void) {
#if __STDC_HOSTED__
    return 0u;
#else
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
#endif
}
static void endpoint_irq_restore(uint64_t flags) {
#if __STDC_HOSTED__
    (void)flags;
#else
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
#endif
}
static uint64_t endpoint_lock_acquire(void) {
    uint64_t flags = endpoint_irq_save();
    while (__atomic_exchange_n(&endpoint_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
    return flags;
}
static void endpoint_lock_release(uint64_t flags) {
    __atomic_store_n(&endpoint_lock, 0u, __ATOMIC_RELEASE);
    endpoint_irq_restore(flags);
}
static void endpoint_clear(struct endpoint_slot *s) {
    s->device_id=0u; s->type=0; s->ops=0; s->context=0;
    s->last_error=PROS_DRIVER_OK; s->used=0u; s->initialized=0u;
    s->state=ENDPOINT_FREE; s->in_flight=0u;
}
static int valid_type(enum pros_hw_endpoint_type t) {
    return t==PROS_HW_ENDPOINT_DISPLAY || t==PROS_HW_ENDPOINT_INPUT || t==PROS_HW_ENDPOINT_AUDIO;
}
static int endpoint_find(uint32_t id, enum pros_hw_endpoint_type t) {
    for (uint32_t i=0;i<PROS_HW_ENDPOINT_MAX;++i)
        if (endpoints[i].used && endpoints[i].device_id==id && endpoints[i].type==t) return (int)i;
    return -1;
}
int pros_hw_classify_pci(const struct pros_pci_device *d, enum pros_device_class *out) {
    if (!d || !out || d->vendor_id==0xFFFFu) return PROS_DRIVER_EINVAL;
    switch (d->class_code) {
    case 0x01u: *out=PROS_DEVICE_CLASS_STORAGE; return PROS_DRIVER_OK;
    case 0x02u: *out=PROS_DEVICE_CLASS_NETWORK; return PROS_DRIVER_OK;
    case 0x03u: *out=PROS_DEVICE_CLASS_DISPLAY; return PROS_DRIVER_OK;
    case 0x04u: *out=PROS_DEVICE_CLASS_AUDIO; return PROS_DRIVER_OK;
    case 0x09u: *out=PROS_DEVICE_CLASS_INPUT; return PROS_DRIVER_OK;
    case 0x0Cu:
        if (d->subclass==0x03u) { *out=PROS_DEVICE_CLASS_USB; return PROS_DRIVER_OK; }
        break;
    default: break;
    }
    *out=PROS_DEVICE_CLASS_OTHER;
    return PROS_DRIVER_ENOTSUP;
}
int pros_hw_resource_validate(const struct pros_hw_resource *r) {
    if (!r || r->version!=PROS_HW_STACK_ABI_VERSION || r->flags!=0u ||
        r->length==0u || r->length>PROS_HW_RESOURCE_MAX ||
        r->length-1u>UINT64_MAX-r->base)
        return PROS_DRIVER_EINVAL;
    switch (r->type) {
    case PROS_HW_RESOURCE_MMIO:
        if ((r->base&0xFFFu)!=0u || r->irq!=0u ||
            r->base>0x000FFFFFFFFFFFFFULL ||
            r->length-1u>0x000FFFFFFFFFFFFFULL-r->base)
            return PROS_DRIVER_EINVAL;
        return PROS_DRIVER_OK;
    case PROS_HW_RESOURCE_IOPORT:
        return (r->base<=0xFFFFu && r->length<=0x10000u-r->base && r->irq==0u)
            ? PROS_DRIVER_OK : PROS_DRIVER_EINVAL;
    case PROS_HW_RESOURCE_IRQ:
        return (r->base==0u && r->length==1u && r->irq<=255u)
            ? PROS_DRIVER_OK : PROS_DRIVER_EINVAL;
    case PROS_HW_RESOURCE_DMA:
        /* Metadata only: no DMA allocation, mapping, or bus mastering. */
        if (r->irq!=0u || (r->base&0xFu)!=0u) return PROS_DRIVER_EINVAL;
        return PROS_DRIVER_ENOTSUP;
    default: return PROS_DRIVER_EINVAL;
    }
}
int pros_hw_stack_init(void) {
    uint64_t flags = endpoint_lock_acquire();
    for (uint32_t i=0;i<PROS_HW_ENDPOINT_MAX;++i) {
        if (endpoints[i].used) {
            endpoint_lock_release(flags);
            return PROS_DRIVER_EBUSY;
        }
    }
    for (uint32_t i=0;i<PROS_HW_ENDPOINT_MAX;++i) endpoint_clear(&endpoints[i]);
    stack_ready=1u;
    endpoint_lock_release(flags);
    return PROS_DRIVER_OK;
}
int pros_hw_endpoint_register(uint32_t id, enum pros_hw_endpoint_type type,
                              const struct pros_hw_endpoint_ops *ops, void *ctx) {
    if (id==0u || !valid_type(type) || !ops ||
        !ops->init || !ops->transfer || !ops->shutdown) return PROS_DRIVER_EINVAL;
    uint64_t flags = endpoint_lock_acquire();
    if (!stack_ready) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_EINVAL;
    }
    if (endpoint_find(id,type)>=0) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_EBUSY;
    }
    uint32_t n=PROS_HW_ENDPOINT_MAX;
    for (uint32_t i=0;i<PROS_HW_ENDPOINT_MAX;++i)
        if (!endpoints[i].used) { n=i; break; }
    if (n==PROS_HW_ENDPOINT_MAX) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_ENOSPC;
    }
    struct endpoint_slot *s=&endpoints[n];
    s->device_id=id; s->type=type; s->ops=ops; s->context=ctx;
    s->last_error=PROS_DRIVER_OK; s->used=1u; s->initialized=0u;
    s->state=ENDPOINT_INITIALIZING; s->in_flight=0u;
    endpoint_lock_release(flags);

    /* Backend callbacks must run outside the registry lock. */
    int rc=ops->init(ctx);
    if (rc==PROS_DRIVER_OK) {
        flags=endpoint_lock_acquire();
        s->initialized=1u; s->state=ENDPOINT_ACTIVE; s->last_error=PROS_DRIVER_OK;
        endpoint_lock_release(flags);
        return PROS_DRIVER_OK;
    }

    flags=endpoint_lock_acquire();
    s->last_error=rc; s->state=ENDPOINT_SHUTTING_DOWN;
    endpoint_lock_release(flags);
    int cleanup=ops->shutdown(ctx);
    flags=endpoint_lock_acquire();
    if (cleanup==PROS_DRIVER_OK) {
        endpoint_clear(s);
        endpoint_lock_release(flags);
        return rc;
    }
    /* Keep the resource lease so a caller can retry cleanup explicitly. */
    s->last_error=cleanup; s->initialized=0u; s->state=ENDPOINT_CLEANUP_PENDING;
    endpoint_lock_release(flags);
    return PROS_DRIVER_EIO;
}
int pros_hw_endpoint_submit(uint32_t id, enum pros_hw_endpoint_type type,
                            const void *buffer, uint32_t length) {
    if (id==0u || !valid_type(type) || !buffer ||
        length==0u || length>PROS_HW_TRANSFER_MAX) return PROS_DRIVER_EINVAL;
    uint64_t flags=endpoint_lock_acquire();
    if (!stack_ready) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_EINVAL;
    }
    int n=endpoint_find(id,type);
    if (n<0) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_ENODEV;
    }
    struct endpoint_slot *s=&endpoints[(uint32_t)n];
    if (s->state!=ENDPOINT_ACTIVE || !s->initialized || !s->ops || !s->ops->transfer) {
        int rc=(s->state==ENDPOINT_INITIALIZING || s->state==ENDPOINT_SHUTTING_DOWN)
            ? PROS_DRIVER_EBUSY : PROS_DRIVER_ENODEV;
        endpoint_lock_release(flags);
        return rc;
    }
    if (s->in_flight==UINT32_MAX) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_EBUSY;
    }
    ++s->in_flight;
    const struct pros_hw_endpoint_ops *ops=s->ops;
    void *context=s->context;
    endpoint_lock_release(flags);

    int rc=ops->transfer(context,buffer,length);

    flags=endpoint_lock_acquire();
    s->last_error=rc;
    if (s->in_flight!=0u) --s->in_flight;
    endpoint_lock_release(flags);
    return rc;
}
int pros_hw_endpoint_status(uint32_t id, enum pros_hw_endpoint_type type,
                            struct pros_hw_endpoint_status *out) {
    if (!out || id==0u || !valid_type(type)) return PROS_DRIVER_EINVAL;
    uint64_t flags=endpoint_lock_acquire();
    if (!stack_ready) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_EINVAL;
    }
    int n=endpoint_find(id,type);
    if (n<0) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_ENODEV;
    }
    const struct endpoint_slot *s=&endpoints[(uint32_t)n];
    out->version=PROS_HW_STACK_ABI_VERSION; out->type=(uint32_t)s->type;
    out->device_id=s->device_id;
    out->initialized=(s->state==ENDPOINT_ACTIVE && s->initialized) ? 1u : 0u;
    out->last_error=s->last_error;
    endpoint_lock_release(flags);
    return PROS_DRIVER_OK;
}
int pros_hw_endpoint_unregister(uint32_t id, enum pros_hw_endpoint_type type) {
    if (id==0u || !valid_type(type)) return PROS_DRIVER_EINVAL;
    uint64_t flags=endpoint_lock_acquire();
    if (!stack_ready) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_EINVAL;
    }
    int n=endpoint_find(id,type);
    if (n<0) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_ENODEV;
    }
    struct endpoint_slot *s=&endpoints[(uint32_t)n];
    if (s->state==ENDPOINT_INITIALIZING || s->state==ENDPOINT_SHUTTING_DOWN ||
        s->in_flight!=0u) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_EBUSY;
    }
    if (s->state!=ENDPOINT_ACTIVE && s->state!=ENDPOINT_CLEANUP_PENDING) {
        endpoint_lock_release(flags);
        return PROS_DRIVER_ENODEV;
    }
    s->state=ENDPOINT_SHUTTING_DOWN;
    s->initialized=0u;
    const struct pros_hw_endpoint_ops *ops=s->ops;
    void *context=s->context;
    endpoint_lock_release(flags);

    int rc=ops->shutdown(context);

    flags=endpoint_lock_acquire();
    if (rc==PROS_DRIVER_OK) {
        endpoint_clear(s);
        endpoint_lock_release(flags);
        return PROS_DRIVER_OK;
    }
    s->last_error=rc;
    s->state=ENDPOINT_CLEANUP_PENDING;
    endpoint_lock_release(flags);
    return rc;
}
int pros_hw_display_present(uint32_t id, uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t format,
                            const void *pixels, uint32_t length) {
    uint64_t row_bytes, expected;
    if (!pixels || width==0u || height==0u ||
        width>PROS_HW_FRAME_MAX_WIDTH || height>PROS_HW_FRAME_MAX_HEIGHT ||
        format!=PROS_HW_DISPLAY_XRGB8888)
        return PROS_DRIVER_EINVAL;
    row_bytes=(uint64_t)width*4u;
    if ((uint64_t)stride<row_bytes || height>UINT64_MAX/(uint64_t)stride)
        return PROS_DRIVER_EINVAL;
    expected=(uint64_t)stride*height;
    if (expected>PROS_HW_TRANSFER_MAX || expected!=length)
        return PROS_DRIVER_EINVAL;
    struct pros_hw_display_frame frame = {
        PROS_HW_STACK_ABI_VERSION, width, height, stride, format, pixels, length
    };
    return pros_hw_endpoint_submit(id,PROS_HW_ENDPOINT_DISPLAY,&frame,
                                   (uint32_t)sizeof(frame));
}
int pros_hw_input_send(uint32_t id, const struct pros_hw_input_event *e) {
    uint32_t edge;
    if (!e || e->version!=PROS_HW_STACK_ABI_VERSION || e->reserved!=0u ||
        (e->flags&~(PROS_HW_INPUT_PRESS|PROS_HW_INPUT_RELEASE|PROS_HW_INPUT_RELATIVE))!=0u)
        return PROS_DRIVER_EINVAL;
    edge=e->flags&(PROS_HW_INPUT_PRESS|PROS_HW_INPUT_RELEASE);
    if (e->type==PROS_HW_INPUT_KEY) {
        if ((edge!=PROS_HW_INPUT_PRESS && edge!=PROS_HW_INPUT_RELEASE) ||
            (e->flags&PROS_HW_INPUT_RELATIVE)!=0u)
            return PROS_DRIVER_EINVAL;
    } else if (e->type==PROS_HW_INPUT_POINTER) {
        /* Pointer motion and button transitions are distinct event forms. */
        if ((e->flags&PROS_HW_INPUT_RELATIVE)!=0u) {
            if (edge!=0u) return PROS_DRIVER_EINVAL;
        } else if (edge!=PROS_HW_INPUT_PRESS && edge!=PROS_HW_INPUT_RELEASE) {
            return PROS_DRIVER_EINVAL;
        }
    } else return PROS_DRIVER_EINVAL;
    return pros_hw_endpoint_submit(id,PROS_HW_ENDPOINT_INPUT,e,(uint32_t)sizeof(*e));
}
int pros_hw_audio_write(uint32_t id, const struct pros_hw_audio_format *f,
                        const void *samples, uint32_t length) {
    uint32_t bytes_per_sample, frame_bytes;
    if (!f || f->version!=PROS_HW_STACK_ABI_VERSION || !samples ||
        f->sample_rate<8000u || f->sample_rate>192000u ||
        f->channels==0u || f->channels>8u || length==0u ||
        length>PROS_HW_TRANSFER_MAX)
        return PROS_DRIVER_EINVAL;
    if (f->sample_format==PROS_HW_AUDIO_S16_LE) bytes_per_sample=2u;
    else if (f->sample_format==PROS_HW_AUDIO_S32_LE) bytes_per_sample=4u;
    else return PROS_DRIVER_ENOTSUP;
    frame_bytes=f->channels*bytes_per_sample;
    if (length%frame_bytes!=0u) return PROS_DRIVER_EINVAL;
    struct pros_hw_audio_buffer buffer = {*f,samples,length};
    return pros_hw_endpoint_submit(id,PROS_HW_ENDPOINT_AUDIO,&buffer,
                                   (uint32_t)sizeof(buffer));
}
struct selftest_context { int init_rc, transfer_rc, shutdown_rc; uint32_t calls; };
static uint8_t selftest_reenter_unregister;
static int selftest_reenter_unregister_rc;
static int selftest_init(void *p) { struct selftest_context *c=p; if(!c)return PROS_DRIVER_EINVAL; ++c->calls; return c->init_rc; }
static int selftest_transfer(void *p,const void *b,uint32_t n) {
    struct selftest_context *c=p;
    if(!c||!b||!n)return PROS_DRIVER_EINVAL;
    ++c->calls;
    if (selftest_reenter_unregister) {
        selftest_reenter_unregister=0u;
        selftest_reenter_unregister_rc=pros_hw_endpoint_unregister(1u,PROS_HW_ENDPOINT_INPUT);
    }
    return c->transfer_rc;
}
static int selftest_shutdown(void *p) { struct selftest_context *c=p; if(!c)return PROS_DRIVER_EINVAL; ++c->calls; return c->shutdown_rc; }
int pros_hw_stack_selftest(void) {
    static const struct pros_hw_endpoint_ops ops={selftest_init,selftest_transfer,selftest_shutdown};
    struct selftest_context c={0,0,0,0u};
    struct pros_hw_endpoint_status st;
    struct pros_pci_device d={0u,1u,0u,0x1234u,0x5678u,0x03u,0u,0u,0u};
    enum pros_device_class cls=PROS_DEVICE_CLASS_OTHER;
    uint8_t payload[8]={0};
    struct pros_hw_resource r={PROS_HW_STACK_ABI_VERSION,PROS_HW_RESOURCE_MMIO,0xF0000000u,4096u,0u,0u};
    struct pros_hw_input_event event={PROS_HW_STACK_ABI_VERSION,PROS_HW_INPUT_KEY,
        PROS_HW_INPUT_PRESS|PROS_HW_INPUT_RELEASE,0u,0u,0,0};
    struct pros_hw_audio_format audio={PROS_HW_STACK_ABI_VERSION,48000u,2u,PROS_HW_AUDIO_S16_LE};
    if(pros_hw_stack_init()!=0)return -1;
    if(pros_hw_classify_pci(&d,&cls)!=0||cls!=PROS_DEVICE_CLASS_DISPLAY)return -2;
    d.vendor_id=0xFFFFu; if(pros_hw_classify_pci(&d,&cls)!=PROS_DRIVER_EINVAL)return -3;
    if(pros_hw_resource_validate(&r)!=0)return -4;
    r.base=UINT64_MAX-7u; if(pros_hw_resource_validate(&r)!=PROS_DRIVER_EINVAL)return -5;
    r.base=0x000FFFFFFFFFF000ULL; r.length=0x2000u;
    if(pros_hw_resource_validate(&r)!=PROS_DRIVER_EINVAL)return -31;
    r.base=0x1000u; r.length=4096u; r.type=PROS_HW_RESOURCE_DMA;
    if(pros_hw_resource_validate(&r)!=PROS_DRIVER_ENOTSUP)return -6;
    r.base=0x1001u;
    if(pros_hw_resource_validate(&r)!=PROS_DRIVER_EINVAL)return -38;
    r.base=0x1000u; r.irq=1u;
    if(pros_hw_resource_validate(&r)!=PROS_DRIVER_EINVAL)return -39;
    r.irq=0u;
    if(pros_hw_resource_validate(&r)!=PROS_DRIVER_ENOTSUP)return -40;
    if(pros_hw_endpoint_submit(1u,PROS_HW_ENDPOINT_INPUT,payload,sizeof(payload))!=PROS_DRIVER_ENODEV)return -7;
    if(pros_hw_display_present(1u,640u,480u,2560u,PROS_HW_DISPLAY_XRGB8888,
                               payload,sizeof(payload))!=PROS_DRIVER_EINVAL)return -24;
    if(pros_hw_input_send(1u,&event)!=PROS_DRIVER_EINVAL)return -25;
    event.flags=PROS_HW_INPUT_PRESS;
    if(pros_hw_input_send(1u,&event)!=PROS_DRIVER_ENODEV)return -26;
    event.type=PROS_HW_INPUT_POINTER;
    event.flags=PROS_HW_INPUT_PRESS|PROS_HW_INPUT_RELATIVE;
    if(pros_hw_input_send(1u,&event)!=PROS_DRIVER_EINVAL)return -35;
    event.flags=0u;
    if(pros_hw_input_send(1u,&event)!=PROS_DRIVER_EINVAL)return -36;
    event.flags=PROS_HW_INPUT_RELATIVE;
    if(pros_hw_input_send(1u,&event)!=PROS_DRIVER_ENODEV)return -37;
    event.type=PROS_HW_INPUT_KEY;
    event.flags=PROS_HW_INPUT_PRESS;
    audio.sample_rate=1000u;
    if(pros_hw_audio_write(1u,&audio,payload,sizeof(payload))!=PROS_DRIVER_EINVAL)return -27;
    audio.sample_rate=48000u;
    if(pros_hw_audio_write(1u,&audio,payload,3u)!=PROS_DRIVER_EINVAL)return -28;
    if(pros_hw_audio_write(1u,&audio,payload,sizeof(payload))!=PROS_DRIVER_ENODEV)return -29;
    if(pros_hw_endpoint_register(0u,PROS_HW_ENDPOINT_INPUT,&ops,&c)!=PROS_DRIVER_EINVAL)return -8;
    c.init_rc=PROS_DRIVER_EIO;
    if(pros_hw_endpoint_register(1u,PROS_HW_ENDPOINT_INPUT,&ops,&c)!=PROS_DRIVER_EIO||c.calls!=2u)return -9;
    if(pros_hw_endpoint_status(1u,PROS_HW_ENDPOINT_INPUT,&st)!=PROS_DRIVER_ENODEV)return -10;
    c.calls=0u; c.shutdown_rc=PROS_DRIVER_EIO;
    if(pros_hw_endpoint_register(2u,PROS_HW_ENDPOINT_INPUT,&ops,&c)!=PROS_DRIVER_EIO||c.calls!=2u)return -20;
    if(pros_hw_endpoint_status(2u,PROS_HW_ENDPOINT_INPUT,&st)!=0||st.initialized)return -21;
    c.shutdown_rc=0;
    if(pros_hw_endpoint_unregister(2u,PROS_HW_ENDPOINT_INPUT)!=0)return -22;
    if(pros_hw_endpoint_status(2u,PROS_HW_ENDPOINT_INPUT,&st)!=PROS_DRIVER_ENODEV)return -23;
    c.init_rc=0; c.calls=0u;
    if(pros_hw_endpoint_register(1u,PROS_HW_ENDPOINT_INPUT,&ops,&c)!=0)return -11;
    if(pros_hw_endpoint_register(1u,PROS_HW_ENDPOINT_INPUT,&ops,&c)!=PROS_DRIVER_EBUSY)return -12;
    if(pros_hw_stack_init()!=PROS_DRIVER_EBUSY)return -33;
    selftest_reenter_unregister=1u; selftest_reenter_unregister_rc=PROS_DRIVER_OK;
    if(pros_hw_endpoint_submit(1u,PROS_HW_ENDPOINT_INPUT,payload,sizeof(payload))!=0)return -13;
    if(selftest_reenter_unregister_rc!=PROS_DRIVER_EBUSY)return -32;
    if(pros_hw_input_send(1u,&event)!=0)return -30;
    if(pros_hw_endpoint_status(1u,PROS_HW_ENDPOINT_INPUT,&st)!=0||!st.initialized||st.last_error!=0)return -14;
    c.shutdown_rc=PROS_DRIVER_EIO;
    if(pros_hw_endpoint_unregister(1u,PROS_HW_ENDPOINT_INPUT)!=PROS_DRIVER_EIO)return -15;
    if(pros_hw_endpoint_status(1u,PROS_HW_ENDPOINT_INPUT,&st)!=0||st.initialized||st.last_error!=PROS_DRIVER_EIO)return -16;
    if(pros_hw_endpoint_submit(1u,PROS_HW_ENDPOINT_INPUT,payload,sizeof(payload))!=PROS_DRIVER_ENODEV)return -34;
    c.shutdown_rc=0; if(pros_hw_endpoint_unregister(1u,PROS_HW_ENDPOINT_INPUT)!=0)return -17;
    if(pros_hw_endpoint_status(1u,PROS_HW_ENDPOINT_INPUT,&st)!=PROS_DRIVER_ENODEV)return -18;
    if(pros_hw_endpoint_submit(1u,PROS_HW_ENDPOINT_AUDIO,payload,sizeof(payload))!=PROS_DRIVER_ENODEV)return -19;
    return PROS_DRIVER_OK;
}

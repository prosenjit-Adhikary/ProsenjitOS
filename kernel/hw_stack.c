#include "hw_stack.h"
struct endpoint_slot {
    uint32_t device_id;
    enum pros_hw_endpoint_type type;
    const struct pros_hw_endpoint_ops *ops;
    void *context;
    int32_t last_error;
    uint8_t used, initialized;
};
static struct endpoint_slot endpoints[PROS_HW_ENDPOINT_MAX];
static uint8_t stack_ready;
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
        r->length==0u || r->length>PROS_HW_RESOURCE_MAX || r->base>UINT64_MAX-r->length)
        return PROS_DRIVER_EINVAL;
    switch (r->type) {
    case PROS_HW_RESOURCE_MMIO:
        return ((r->base&0xFFFu)==0u && r->irq==0u) ? PROS_DRIVER_OK : PROS_DRIVER_EINVAL;
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
    if (stack_ready) for (uint32_t i=0;i<PROS_HW_ENDPOINT_MAX;++i)
        if (endpoints[i].used) return PROS_DRIVER_EBUSY;
    for (uint32_t i=0;i<PROS_HW_ENDPOINT_MAX;++i) {
        endpoints[i].device_id=0u; endpoints[i].type=0; endpoints[i].ops=0;
        endpoints[i].context=0; endpoints[i].last_error=PROS_DRIVER_OK;
        endpoints[i].used=0u; endpoints[i].initialized=0u;
    }
    stack_ready=1u;
    return PROS_DRIVER_OK;
}
int pros_hw_endpoint_register(uint32_t id, enum pros_hw_endpoint_type type,
                              const struct pros_hw_endpoint_ops *ops, void *ctx) {
    if (!stack_ready || id==0u || !valid_type(type) || !ops ||
        !ops->init || !ops->transfer || !ops->shutdown) return PROS_DRIVER_EINVAL;
    if (endpoint_find(id,type)>=0) return PROS_DRIVER_EBUSY;
    uint32_t n=PROS_HW_ENDPOINT_MAX;
    for (uint32_t i=0;i<PROS_HW_ENDPOINT_MAX;++i) if (!endpoints[i].used) { n=i; break; }
    if (n==PROS_HW_ENDPOINT_MAX) return PROS_DRIVER_ENOSPC;
    endpoints[n].device_id=id; endpoints[n].type=type; endpoints[n].ops=ops;
    endpoints[n].context=ctx; endpoints[n].last_error=PROS_DRIVER_OK;
    endpoints[n].used=1u; endpoints[n].initialized=0u;
    int rc=ops->init(ctx);
    if (rc!=PROS_DRIVER_OK) {
        endpoints[n].last_error=rc;
        int cleanup=ops->shutdown(ctx);
        if (cleanup!=PROS_DRIVER_OK) {
            /* Preserve ownership so a failed partial-init cleanup can be retried. */
            endpoints[n].last_error=cleanup;
            return PROS_DRIVER_EIO;
        }
        endpoints[n].used=0u; endpoints[n].ops=0; endpoints[n].context=0;
        endpoints[n].device_id=0u; endpoints[n].initialized=0u;
        return rc;
    }
    endpoints[n].initialized=1u;
    return PROS_DRIVER_OK;
}
int pros_hw_endpoint_submit(uint32_t id, enum pros_hw_endpoint_type type,
                            const void *buffer, uint32_t length) {
    if (!stack_ready || id==0u || !valid_type(type) || !buffer ||
        length==0u || length>PROS_HW_TRANSFER_MAX) return PROS_DRIVER_EINVAL;
    int n=endpoint_find(id,type);
    if (n<0) return PROS_DRIVER_ENODEV;
    struct endpoint_slot *s=&endpoints[(uint32_t)n];
    if (!s->initialized || !s->ops || !s->ops->transfer) return PROS_DRIVER_ENODEV;
    int rc=s->ops->transfer(s->context,buffer,length);
    s->last_error=rc;
    return rc;
}
int pros_hw_endpoint_status(uint32_t id, enum pros_hw_endpoint_type type,
                            struct pros_hw_endpoint_status *out) {
    if (!stack_ready || !out || id==0u || !valid_type(type)) return PROS_DRIVER_EINVAL;
    int n=endpoint_find(id,type);
    if (n<0) return PROS_DRIVER_ENODEV;
    const struct endpoint_slot *s=&endpoints[(uint32_t)n];
    out->version=PROS_HW_STACK_ABI_VERSION; out->type=(uint32_t)s->type;
    out->device_id=s->device_id; out->initialized=s->initialized; out->last_error=s->last_error;
    return PROS_DRIVER_OK;
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
    return pros_hw_endpoint_submit(id,PROS_HW_ENDPOINT_DISPLAY,pixels,length);
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
        if (edge==(PROS_HW_INPUT_PRESS|PROS_HW_INPUT_RELEASE))
            return PROS_DRIVER_EINVAL;
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
    return pros_hw_endpoint_submit(id,PROS_HW_ENDPOINT_AUDIO,samples,length);
}
int pros_hw_endpoint_unregister(uint32_t id, enum pros_hw_endpoint_type type) {
    if (!stack_ready || id==0u || !valid_type(type)) return PROS_DRIVER_EINVAL;
    int n=endpoint_find(id,type);
    if (n<0) return PROS_DRIVER_ENODEV;
    struct endpoint_slot *s=&endpoints[(uint32_t)n];
    int rc=s->ops->shutdown(s->context);
    s->last_error=rc;
    if (rc!=PROS_DRIVER_OK) return rc;
    s->device_id=0u; s->type=0; s->ops=0; s->context=0;
    s->used=0u; s->initialized=0u;
    return PROS_DRIVER_OK;
}
struct selftest_context { int init_rc, transfer_rc, shutdown_rc; uint32_t calls; };
static int selftest_init(void *p) { struct selftest_context *c=p; if(!c)return PROS_DRIVER_EINVAL; ++c->calls; return c->init_rc; }
static int selftest_transfer(void *p,const void *b,uint32_t n) { struct selftest_context *c=p; if(!c||!b||!n)return PROS_DRIVER_EINVAL; ++c->calls; return c->transfer_rc; }
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
    r.base=0x1000u; r.type=PROS_HW_RESOURCE_DMA;
    if(pros_hw_resource_validate(&r)!=PROS_DRIVER_ENOTSUP)return -6;
    if(pros_hw_endpoint_submit(1u,PROS_HW_ENDPOINT_INPUT,payload,sizeof(payload))!=PROS_DRIVER_ENODEV)return -7;
    if(pros_hw_display_present(1u,640u,480u,2560u,PROS_HW_DISPLAY_XRGB8888,
                               payload,sizeof(payload))!=PROS_DRIVER_EINVAL)return -24;
    if(pros_hw_input_send(1u,&event)!=PROS_DRIVER_EINVAL)return -25;
    event.flags=PROS_HW_INPUT_PRESS;
    if(pros_hw_input_send(1u,&event)!=PROS_DRIVER_ENODEV)return -26;
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
    if(pros_hw_endpoint_submit(1u,PROS_HW_ENDPOINT_INPUT,payload,sizeof(payload))!=0)return -13;
    if(pros_hw_input_send(1u,&event)!=0)return -30;
    if(pros_hw_endpoint_status(1u,PROS_HW_ENDPOINT_INPUT,&st)!=0||!st.initialized||st.last_error!=0)return -14;
    c.shutdown_rc=PROS_DRIVER_EIO;
    if(pros_hw_endpoint_unregister(1u,PROS_HW_ENDPOINT_INPUT)!=PROS_DRIVER_EIO)return -15;
    if(pros_hw_endpoint_status(1u,PROS_HW_ENDPOINT_INPUT,&st)!=0||!st.initialized||st.last_error!=PROS_DRIVER_EIO)return -16;
    c.shutdown_rc=0; if(pros_hw_endpoint_unregister(1u,PROS_HW_ENDPOINT_INPUT)!=0)return -17;
    if(pros_hw_endpoint_status(1u,PROS_HW_ENDPOINT_INPUT,&st)!=PROS_DRIVER_ENODEV)return -18;
    if(pros_hw_endpoint_submit(1u,PROS_HW_ENDPOINT_AUDIO,payload,sizeof(payload))!=PROS_DRIVER_ENODEV)return -19;
    return PROS_DRIVER_OK;
}

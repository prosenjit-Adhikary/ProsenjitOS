#include "display.h"

static uint8_t display_ready;

/*
 * Validate the device-reported geometry before any consumer uses it.
 * This contract currently supports XRGB8888 (4 bytes per pixel). Pitch is
 * measured in bytes, and framebuffer_size must cover every advertised row.
 */
static int display_info_valid(const struct pros_kernel_display_info *info)
{
    uint64_t minimum_pitch;
    uint64_t required_bytes;

    if (!info || !info->width || !info->height ||
        info->width > PROS_DISPLAY_MAX_WIDTH ||
        info->height > PROS_DISPLAY_MAX_HEIGHT ||
        info->format != 1u || !info->framebuffer || !info->framebuffer_size)
        return 0;

    minimum_pitch = (uint64_t)info->width * 4u;
    if ((uint64_t)info->pitch < minimum_pitch)
        return 0;

    required_bytes = (uint64_t)info->pitch * (uint64_t)info->height;
    if (required_bytes > info->framebuffer_size)
        return 0;

    /* Reject address ranges that wrap the 64-bit physical address space. */
    if (info->framebuffer > UINT64_MAX - info->framebuffer_size)
        return 0;

    return 1;
}

static const struct pros_display_backend *backend_of(const struct pros_driver_device *d)
{
    if (!d || d->class_id != PROS_DEVICE_CLASS_DISPLAY ||
        d->state != PROS_DEVICE_BOUND || !d->driver_data) return 0;
    const struct pros_display_backend *b =
        (const struct pros_display_backend *)d->driver_data;
    if (b->magic != PROS_DISPLAY_BACKEND_MAGIC ||
        b->abi_version != PROS_DISPLAY_ABI_VERSION ||
        !b->get_info || !b->present) return 0;
    return b;
}

int pros_display_init(void) { display_ready = 1u; return PROS_DRIVER_OK; }

int pros_display_bind(uint32_t id)
{
    if (!display_ready || id == 0u) return PROS_DRIVER_EINVAL;
    const struct pros_driver_device *d = pros_device_get(id);
    if (!d) return PROS_DRIVER_ENODEV;
    if (d->class_id != PROS_DEVICE_CLASS_DISPLAY) return PROS_DRIVER_ENOTSUP;
    if (!d->driver || d->state != PROS_DEVICE_BOUND) return PROS_DRIVER_ENODEV;
    return backend_of(d) ? PROS_DRIVER_OK : PROS_DRIVER_ENOTSUP;
}

int pros_display_unbind(uint32_t id)
{
    if (!display_ready || id == 0u) return PROS_DRIVER_EINVAL;
    const struct pros_driver_device *d = pros_device_get(id);
    if (!d) return PROS_DRIVER_ENODEV;
    return d->class_id == PROS_DEVICE_CLASS_DISPLAY ?
        PROS_DRIVER_OK : PROS_DRIVER_ENOTSUP;
}

int pros_display_get_info(uint32_t id, struct pros_kernel_display_info *info)
{
    if (!display_ready || !info || id == 0u) return PROS_DRIVER_EINVAL;
    const struct pros_display_backend *b = backend_of(pros_device_get(id));
    if (!b) return PROS_DRIVER_ENODEV;
    int rc = b->get_info(b->ctx, info);
    if (rc != PROS_DRIVER_OK) return rc;
    if (!display_info_valid(info)) return PROS_DRIVER_EIO;
    return PROS_DRIVER_OK;
}

int pros_display_present(uint32_t id, const void *pixels, uint64_t size)
{
    if (!display_ready || !pixels || !size || id == 0u) return PROS_DRIVER_EINVAL;
    const struct pros_display_backend *b = backend_of(pros_device_get(id));
    if (!b) return PROS_DRIVER_ENODEV;
    struct pros_kernel_display_info info;
    if (b->get_info(b->ctx, &info) != PROS_DRIVER_OK ||
        !display_info_valid(&info) || size > info.framebuffer_size)
        return PROS_DRIVER_EINVAL;
    return b->present(b->ctx, pixels, size);
}

struct display_test_state { uint32_t presents; struct pros_kernel_display_info info; };
static int test_info(void *ctx, struct pros_kernel_display_info *i)
{ struct display_test_state *s=ctx; if(!s||!i)return PROS_DRIVER_EINVAL;*i=s->info;return 0; }
static int test_present(void *ctx, const void *p, uint64_t n)
{ struct display_test_state *s=ctx; if(!s||!p||!n)return PROS_DRIVER_EINVAL;++s->presents;return 0; }
static int test_probe(struct pros_driver_device *d)
{
    static struct display_test_state s;
    static struct pros_display_backend b;
    if(!d)return PROS_DRIVER_EINVAL;
    s.presents=0u; s.info.width=640u; s.info.height=480u; s.info.pitch=2560u;
    s.info.format=1u; s.info.framebuffer=0xE0000000ULL; s.info.framebuffer_size=1228800ULL;
    b.magic=PROS_DISPLAY_BACKEND_MAGIC;b.abi_version=PROS_DISPLAY_ABI_VERSION;
    b.get_info=test_info;b.present=test_present;b.ctx=&s;d->driver_data=&b;return 0;
}
static int test_remove(struct pros_driver_device *d)
{ if(!d)return PROS_DRIVER_EINVAL;d->driver_data=0;return 0; }

int pros_display_selftest(void)
{
    static const struct pros_driver_id ids[]={{PROS_DRIVER_BUS_VIRTUAL,0xD15Au,1u,3u,0u,0u}};
    static const struct pros_driver_ops ops={test_probe,test_remove,0,0};
    static const struct pros_driver drv={
        "display-selftest","1.0.0",PROS_DRIVER_ABI_VERSION,PROS_DRIVER_FLAG_NATIVE,
        PROS_DRIVER_ORIGIN_NATIVE,ids,1u,&ops,0,{0}
    };
    static struct pros_driver_device dev={
        0,PROS_DRIVER_BUS_VIRTUAL,PROS_DEVICE_CLASS_DISPLAY,PROS_DEVICE_DISCOVERED,
        {PROS_DRIVER_BUS_VIRTUAL,0xD15Au,1u,3u,0u,0u},"display-selftest",0,0,0
    };
    uint8_t px[4]={1,2,3,4}; struct pros_kernel_display_info i;
    if(pros_display_init()!=0)return -1;
    if(pros_driver_register(&drv)!=0)return -2;
    if(pros_device_register(&dev)!=0)return -3;
    if(pros_driver_bind(dev.id)!=0)return -4;
    if(pros_display_bind(dev.id)!=0)return -5;
    if(pros_display_get_info(dev.id,&i)!=0||i.width!=640u||i.height!=480u)return -6;
    {
        struct pros_display_backend *backend =
            (struct pros_display_backend *)dev.driver_data;
        struct display_test_state *state = backend ? backend->ctx : 0;
        if (!state) return -7;

        state->info.pitch = 2559u;
        if (pros_display_get_info(dev.id, &i) != PROS_DRIVER_EIO) return -8;
        state->info.pitch = 2560u;

        state->info.framebuffer_size = 1228799ULL;
        if (pros_display_get_info(dev.id, &i) != PROS_DRIVER_EIO) return -9;
        state->info.framebuffer_size = 1228800ULL;

        state->info.framebuffer = UINT64_MAX - 100u;
        if (pros_display_get_info(dev.id, &i) != PROS_DRIVER_EIO) return -10;
        state->info.framebuffer = 0xE0000000ULL;
    }
    if(pros_display_present(dev.id,px,sizeof(px))!=0)return -11;
    if(pros_display_present(dev.id,px,1228801ULL)!=PROS_DRIVER_EINVAL)return -12;
    if(pros_driver_unbind(dev.id)!=0)return -13;
    if(pros_display_bind(dev.id)!=PROS_DRIVER_ENODEV)return -14;
    if(pros_device_unregister(dev.id)!=0)return -15;
    if(pros_driver_unregister(&drv)!=0)return -16;
    return 0;
}

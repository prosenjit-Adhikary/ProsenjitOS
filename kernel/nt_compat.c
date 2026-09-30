#include "nt_compat.h"

static struct pros_nt_compat_image images[PROS_NT_COMPAT_MAX_HANDLES];
static struct pros_nt_compat_dispatch_table dispatch[PROS_NT_COMPAT_MAX_HANDLES];

int pros_nt_compat_init(void)
{
    uint32_t i;
    for (i = 0; i < PROS_NT_COMPAT_MAX_HANDLES; ++i) {
        images[i].package_id = 0;
        images[i].state = PROS_NT_COMPAT_EMPTY;
        images[i].capabilities = 0;
        images[i].image_size = 0;
        images[i].image_entry = 0;
        { uint32_t j; for (j = 0; j < PROS_NT_IRP_MAX; ++j) dispatch[i].major[j] = 0; }
    }
    return PROS_DRIVER_OK;
}

int pros_nt_compat_inspect(uint32_t package_id,
                           struct pros_nt_compat_image *out)
{
    if (!out || package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES)
        return PROS_DRIVER_EINVAL;

    /*
     * Deliberately do not parse/execute PE code here. The image inspection
     * stage will only become executable after a PE loader and NT ABI are
     * explicitly implemented and policy-approved.
     */
    if (images[package_id - 1u].state == PROS_NT_COMPAT_EMPTY) {
        images[package_id - 1u].package_id = package_id;
        images[package_id - 1u].state = PROS_NT_COMPAT_INSPECTED;
    }
    *out = images[package_id - 1u];
    return PROS_DRIVER_OK;
}

int pros_nt_compat_register_dispatch(uint32_t package_id,
                                      uint32_t irp_major,
                                      pros_nt_dispatch_fn handler)
{
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES ||
        irp_major >= PROS_NT_IRP_MAX || !handler)
        return PROS_DRIVER_EINVAL;
    if (images[package_id - 1u].state == PROS_NT_COMPAT_EMPTY ||
        images[package_id - 1u].state == PROS_NT_COMPAT_QUARANTINED)
        return PROS_DRIVER_EPERM;
    dispatch[package_id - 1u].major[irp_major] = handler;
    images[package_id - 1u].capabilities |= PROS_NT_CAP_IO_DISPATCH;
    return PROS_DRIVER_OK;
}

int pros_nt_compat_dispatch(uint32_t package_id, uint32_t irp_major,
                            void *context)
{
    pros_nt_dispatch_fn fn;
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES ||
        irp_major >= PROS_NT_IRP_MAX)
        return PROS_DRIVER_EINVAL;
    if (images[package_id - 1u].state == PROS_NT_COMPAT_QUARANTINED)
        return PROS_DRIVER_EPERM;
    fn = dispatch[package_id - 1u].major[irp_major];
    if (!fn) return PROS_DRIVER_ENOTSUP;
    return fn(package_id, irp_major, context);
}

int pros_nt_compat_stage(uint32_t package_id)
{
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES)
        return PROS_DRIVER_EINVAL;
    if (images[package_id - 1u].state != PROS_NT_COMPAT_INSPECTED)
        return PROS_DRIVER_EPERM;
    return PROS_DRIVER_ENOTSUP;
}

int pros_nt_compat_pnp(uint32_t package_id, uint32_t event)
{
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES ||
        event >= PROS_NT_PNP_MAX)
        return PROS_DRIVER_EINVAL;
    if (images[package_id - 1u].state == PROS_NT_COMPAT_QUARANTINED)
        return PROS_DRIVER_EPERM;
    if (dispatch[package_id - 1u].major[PROS_NT_IRP_PNP])
        return dispatch[package_id - 1u].major[PROS_NT_IRP_PNP](
            package_id, PROS_NT_IRP_PNP, (void *)(uintptr_t)event);
    return PROS_DRIVER_ENOTSUP;
}

int pros_nt_compat_power(uint32_t package_id, uint32_t event)
{
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES ||
        event >= PROS_NT_POWER_MAX)
        return PROS_DRIVER_EINVAL;
    if (images[package_id - 1u].state == PROS_NT_COMPAT_QUARANTINED)
        return PROS_DRIVER_EPERM;
    if (dispatch[package_id - 1u].major[PROS_NT_IRP_POWER])
        return dispatch[package_id - 1u].major[PROS_NT_IRP_POWER](
            package_id, PROS_NT_IRP_POWER, (void *)(uintptr_t)event);
    return PROS_DRIVER_ENOTSUP;
}

int pros_nt_compat_health_check(uint32_t package_id)
{
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES)
        return PROS_DRIVER_EINVAL;
    return PROS_DRIVER_ENOTSUP;
}

int pros_nt_compat_quarantine(uint32_t package_id)
{
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES)
        return PROS_DRIVER_EINVAL;
    images[package_id - 1u].package_id = package_id;
    images[package_id - 1u].state = PROS_NT_COMPAT_QUARANTINED;
    return PROS_DRIVER_OK;
}

int pros_nt_compat_rollback(uint32_t package_id)
{
    uint32_t i;
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES)
        return PROS_DRIVER_EINVAL;
    images[package_id - 1u].state = PROS_NT_COMPAT_EMPTY;
    images[package_id - 1u].package_id = 0;
    images[package_id - 1u].capabilities = 0;
    images[package_id - 1u].machine = 0;
    images[package_id - 1u].entry_rva = 0;
    images[package_id - 1u].image_size = 0;
    for (i = 0; i < PROS_NT_COMPAT_MAX_DISPATCH; ++i)
        dispatch_tables[package_id - 1u].handlers[i] = 0;
    return PROS_DRIVER_OK;
}

int pros_nt_compat_selftest(void)
{
    struct pros_nt_compat_image image;

    if (pros_nt_compat_init() != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_nt_compat_inspect(1u, &image) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (image.state != PROS_NT_COMPAT_INSPECTED)
        return PROS_DRIVER_EIO;
    if (pros_nt_compat_dispatch(1u, PROS_NT_IRP_READ, 0)
        != PROS_DRIVER_ENOTSUP)
        return PROS_DRIVER_EIO;
    if (pros_nt_compat_stage(1u) != PROS_DRIVER_ENOTSUP)
        return PROS_DRIVER_EIO;
    if (pros_nt_compat_quarantine(1u) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_nt_compat_rollback(1u) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    return PROS_DRIVER_OK;
}

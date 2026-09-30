#include "nt_compat.h"

static struct pros_nt_compat_image images[PROS_NT_COMPAT_MAX_HANDLES];

int pros_nt_compat_init(void)
{
    uint32_t i;
    for (i = 0; i < PROS_NT_COMPAT_MAX_HANDLES; ++i) {
        images[i].package_id = 0;
        images[i].state = PROS_NT_COMPAT_EMPTY;
        images[i].capabilities = 0;
        images[i].image_size = 0;
        images[i].image_entry = 0;
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

int pros_nt_compat_stage(uint32_t package_id)
{
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES)
        return PROS_DRIVER_EINVAL;
    if (images[package_id - 1u].state != PROS_NT_COMPAT_INSPECTED)
        return PROS_DRIVER_EPERM;
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
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES)
        return PROS_DRIVER_EINVAL;
    images[package_id - 1u].state = PROS_NT_COMPAT_EMPTY;
    images[package_id - 1u].package_id = 0;
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
    if (pros_nt_compat_stage(1u) != PROS_DRIVER_ENOTSUP)
        return PROS_DRIVER_EIO;
    if (pros_nt_compat_quarantine(1u) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_nt_compat_rollback(1u) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    return PROS_DRIVER_OK;
}

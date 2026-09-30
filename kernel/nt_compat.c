#include "nt_compat.h"

static int nt_compat_validate_resources(const struct pros_nt_device_object *device)
{
    uint32_t i, j;
    if (!device || device->resource_count > PROS_NT_COMPAT_MAX_RESOURCES)
        return PROS_DRIVER_EINVAL;
    for (i = 0; i < device->resource_count; ++i) {
        const struct pros_nt_device_resource *a = &device->resources[i];
        uint64_t a_end;
        if (a->type < PROS_NT_RESOURCE_IOPORT || a->type > PROS_NT_RESOURCE_DMA ||
            a->length == 0 || a->start > UINT64_MAX - (a->length - 1u))
            return PROS_DRIVER_EINVAL;
        a_end = a->start + a->length - 1u;
        (void)a_end;
        for (j = i + 1u; j < device->resource_count; ++j) {
            const struct pros_nt_device_resource *b = &device->resources[j];
            uint64_t b_end;
            if (a->type != b->type)
                continue;
            if (b->length == 0 || b->start > UINT64_MAX - (b->length - 1u))
                return PROS_DRIVER_EINVAL;
            b_end = b->start + b->length - 1u;
            if (a->start <= b_end && b->start <= a_end)
                return PROS_DRIVER_EBUSY;
        }
    }
    return PROS_DRIVER_OK;
}

static int nt_compat_resource_conflicts(uint32_t package_id,
                                        const struct pros_nt_device_object *device)
{
    uint32_t i, j;
    for (i = 0; i < PROS_NT_COMPAT_MAX_HANDLES; ++i) {
        if (i + 1u == package_id ||
            devices[i].package_id == 0 ||
            devices[i].state == PROS_NT_DEVICE_REMOVED)
            continue;
        for (j = 0; j < device->resource_count; ++j) {
            uint32_t k;
            const struct pros_nt_device_resource *a = &device->resources[j];
            for (k = 0; k < devices[i].resource_count; ++k) {
                const struct pros_nt_device_resource *b = &devices[i].resources[k];
                uint64_t a_end = a->start + a->length - 1u;
                uint64_t b_end = b->start + b->length - 1u;
                if (a->type == b->type &&
                    a->start <= b_end && b->start <= a_end)
                    return PROS_DRIVER_EBUSY;
            }
        }
    }
    return PROS_DRIVER_OK;
}

int pros_nt_compat_pnp(uint32_t package_id, uint32_t event)
{
    enum pros_nt_compat_state state;
    uint32_t device_state;
    int rc;
    if (package_id == 0 || package_id > PROS_NT_COMPAT_MAX_HANDLES ||
        event >= PROS_NT_PNP_MAX)
        return PROS_DRIVER_EINVAL;
    state = images[package_id - 1u].state;
    device_state = devices[package_id - 1u].state;
    if (state == PROS_NT_COMPAT_QUARANTINED)
        return PROS_DRIVER_EPERM;
    if (device_state == PROS_NT_DEVICE_EMPTY ||
        device_state == PROS_NT_DEVICE_REMOVED)
        return PROS_DRIVER_ENODEV;
    if (event == PROS_NT_PNP_START &&
        state != PROS_NT_COMPAT_STAGED && state != PROS_NT_COMPAT_HEALTHY)
        return PROS_DRIVER_EPERM;
    if (event == PROS_NT_PNP_START &&
        device_state != PROS_NT_DEVICE_PRESENT &&
        device_state != PROS_NT_DEVICE_STOPPED)
        return PROS_DRIVER_EPERM;
    if (event == PROS_NT_PNP_QUERY_REMOVE &&
        device_state != PROS_NT_DEVICE_STARTED)
        return PROS_DRIVER_EPERM;
    if (event == PROS_NT_PNP_STOP &&
        device_state != PROS_NT_DEVICE_STARTED &&
        device_state != PROS_NT_DEVICE_QUERY_REMOVE)
        return PROS_DRIVER_EPERM;
    if ((event == PROS_NT_PNP_REMOVE || event == PROS_NT_PNP_SURPRISE_REMOVE) &&
        device_state != PROS_NT_DEVICE_STOPPED &&
        device_state != PROS_NT_DEVICE_QUERY_REMOVE &&
        device_state != PROS_NT_DEVICE_STARTED)
        return PROS_DRIVER_EPERM;
    if (dispatch[package_id - 1u].major[PROS_NT_IRP_PNP]) {
        rc = dispatch[package_id - 1u].major[PROS_NT_IRP_PNP](
            package_id, PROS_NT_IRP_PNP, (void *)(uintptr_t)event);
        if (rc != PROS_DRIVER_OK)
            return rc;
    } else {
        return PROS_DRIVER_ENOTSUP;
    }
    if (event == PROS_NT_PNP_START)
        devices[package_id - 1u].state = PROS_NT_DEVICE_STARTED;
    else if (event == PROS_NT_PNP_QUERY_REMOVE)
        devices[package_id - 1u].state = PROS_NT_DEVICE_QUERY_REMOVE;
    else if (event == PROS_NT_PNP_STOP)
        devices[package_id - 1u].state = PROS_NT_DEVICE_STOPPED;
    else
        devices[package_id - 1u].state = PROS_NT_DEVICE_REMOVED;
    return PROS_DRIVER_OK;
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
    images[package_id - 1u].image_entry = 0;
    images[package_id - 1u].image_size = 0;
    devices[package_id - 1u].size = 0;
    devices[package_id - 1u].package_id = 0;
    devices[package_id - 1u].state = PROS_NT_DEVICE_EMPTY;
    devices[package_id - 1u].resource_count = 0;
    for (i = 0; i < PROS_NT_IRP_MAX; ++i)
        dispatch[package_id - 1u].major[i] = 0;
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
    {
        struct pros_nt_device_object device;
        struct pros_nt_device_object returned;
        device.size = sizeof(device);
        device.package_id = 0;
        device.bus = 0;
        device.location = 0x0100u;
        device.vendor_id = 0x1234u;
        device.device_id = 0x5678u;
        device.class_code = 1u;
        device.subclass = 6u;
        device.prog_if = 1u;
        device.resource_count = 1u;
        device.resources[0].type = PROS_NT_RESOURCE_MMIO;
        device.resources[0].flags = 0;
        device.resources[0].start = 0x100000u;
        device.resources[0].length = 0x1000u;
        if (pros_nt_compat_set_device(1u, &device) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        returned.size = sizeof(returned);
        if (pros_nt_compat_get_device(1u, &returned) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (returned.package_id != 1u ||
            returned.resource_count != 1u ||
            returned.resources[0].type != PROS_NT_RESOURCE_MMIO)
            return PROS_DRIVER_EIO;
        device.resources[0].start = 0x100800u;
        if (pros_nt_compat_set_device(1u, &device) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        device.resource_count = 2u;
        device.resources[1] = device.resources[0];
        if (pros_nt_compat_set_device(1u, &device) != PROS_DRIVER_EBUSY)
            return PROS_DRIVER_EIO;
        images[0].state = PROS_NT_COMPAT_STAGED;
        if (pros_nt_compat_register_dispatch(1u, PROS_NT_IRP_PNP,
                                             nt_compat_pnp_test_handler)
            != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
        if (pros_nt_compat_pnp(1u, PROS_NT_PNP_START) != PROS_DRIVER_OK ||
            devices[0].state != PROS_NT_DEVICE_STARTED)
            return PROS_DRIVER_EIO;
        if (pros_nt_compat_pnp(1u, PROS_NT_PNP_QUERY_REMOVE) != PROS_DRIVER_OK ||
            devices[0].state != PROS_NT_DEVICE_QUERY_REMOVE)
            return PROS_DRIVER_EIO;
        if (pros_nt_compat_pnp(1u, PROS_NT_PNP_STOP) != PROS_DRIVER_OK ||
            devices[0].state != PROS_NT_DEVICE_STOPPED)
            return PROS_DRIVER_EIO;
        if (pros_nt_compat_pnp(1u, PROS_NT_PNP_START) != PROS_DRIVER_OK ||
            devices[0].state != PROS_NT_DEVICE_STARTED)
            return PROS_DRIVER_EIO;
        if (pros_nt_compat_pnp(1u, PROS_NT_PNP_REMOVE) != PROS_DRIVER_EPERM)
            return PROS_DRIVER_EIO;
        if (pros_nt_compat_pnp(1u, PROS_NT_PNP_STOP) != PROS_DRIVER_OK ||
            devices[0].state != PROS_NT_DEVICE_STOPPED)
            return PROS_DRIVER_EIO;
        if (pros_nt_compat_pnp(1u, PROS_NT_PNP_REMOVE) != PROS_DRIVER_OK ||
            devices[0].state != PROS_NT_DEVICE_REMOVED)
            return PROS_DRIVER_EIO;
    }
    if (pros_nt_compat_register_dispatch(1u, PROS_NT_IRP_READ,
                                               nt_compat_test_handler)
        != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    {
        struct pros_nt_io_request request;
        request.size = sizeof(request);
        request.type = PROS_NT_IO_READ;
        request.major = PROS_NT_IRP_READ;
        request.control_code = 0;
        request.buffer_size = 0;
        request.buffer = 0;
        request.status = PROS_DRIVER_EIO;
        request.information = 0;
        if (pros_nt_compat_submit_io(1u, &request) != PROS_DRIVER_OK)
            return PROS_DRIVER_EIO;
    }
    if (pros_nt_compat_quarantine(1u) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_nt_compat_dispatch(1u, PROS_NT_IRP_READ, 0)
        != PROS_DRIVER_EPERM)
        return PROS_DRIVER_EIO;
    if (pros_nt_compat_rollback(1u) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;
    if (pros_nt_compat_dispatch(1u, PROS_NT_IRP_READ, 0)
        != PROS_DRIVER_ENOTSUP)
        return PROS_DRIVER_EIO;
    return PROS_DRIVER_OK;
}

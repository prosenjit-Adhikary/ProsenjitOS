#include "device_broker.h"

static struct pros_device_grant grants[PROS_DEVICE_BROKER_MAX_GRANTS];
static uint8_t broker_ready;

int pros_device_broker_init(void)
{
    for (uint32_t i = 0u; i < PROS_DEVICE_BROKER_MAX_GRANTS; ++i) {
        grants[i].device_id = 0u;
        grants[i].process_id = 0u;
        grants[i].permissions = 0u;
        grants[i].active = 0u;
    }
    broker_ready = 1u;
    return PROS_DRIVER_OK;
}

void pros_device_broker_drop_device(uint32_t device_id)
{
    if (!broker_ready || device_id == 0u)
        return;

    for (uint32_t i = 0u; i < PROS_DEVICE_BROKER_MAX_GRANTS; ++i) {
        if (grants[i].active && grants[i].device_id == device_id) {
            grants[i].device_id = 0u;
            grants[i].process_id = 0u;
            grants[i].permissions = 0u;
            grants[i].active = 0u;
        }
    }
}

static int valid_permission(uint32_t permissions)
{
    return permissions != 0u &&
           (permissions & ~(PROS_DEVICE_PERMISSION_READ |
                            PROS_DEVICE_PERMISSION_WRITE |
                            PROS_DEVICE_PERMISSION_CONTROL |
                            PROS_DEVICE_PERMISSION_MAP)) == 0u;
}

int pros_device_grant(uint32_t device_id, uint32_t process_id,
                      uint32_t permissions)
{
    const struct pros_driver_device *device;

    if (!broker_ready || device_id == 0u || process_id == 0u ||
        !valid_permission(permissions))
        return PROS_DRIVER_EINVAL;

    device = pros_device_get(device_id);
    if (!device || device->state != PROS_DEVICE_BOUND)
        return PROS_DRIVER_ENODEV;

    for (uint32_t i = 0u; i < PROS_DEVICE_BROKER_MAX_GRANTS; ++i) {
        if (grants[i].active && grants[i].device_id == device_id &&
            grants[i].process_id == process_id) {
            grants[i].permissions |= permissions;
            return PROS_DRIVER_OK;
        }
    }

    for (uint32_t i = 0u; i < PROS_DEVICE_BROKER_MAX_GRANTS; ++i) {
        if (!grants[i].active) {
            grants[i].device_id = device_id;
            grants[i].process_id = process_id;
            grants[i].permissions = permissions;
            grants[i].active = 1u;
            return PROS_DRIVER_OK;
        }
    }
    return PROS_DRIVER_ENOSPC;
}

int pros_device_revoke(uint32_t device_id, uint32_t process_id)
{
    if (!broker_ready || device_id == 0u || process_id == 0u)
        return PROS_DRIVER_EINVAL;

    for (uint32_t i = 0u; i < PROS_DEVICE_BROKER_MAX_GRANTS; ++i) {
        if (grants[i].active && grants[i].device_id == device_id &&
            grants[i].process_id == process_id) {
            grants[i].active = 0u;
            grants[i].permissions = 0u;
            return PROS_DRIVER_OK;
        }
    }
    return PROS_DRIVER_ENODEV;
}

int pros_device_check(uint32_t device_id, uint32_t process_id,
                      uint32_t permission)
{
    if (!broker_ready || device_id == 0u || process_id == 0u ||
        !valid_permission(permission) || (permission & (permission - 1u)) != 0u)
        return PROS_DRIVER_EINVAL;
    if (!pros_device_get(device_id))
        return PROS_DRIVER_ENODEV;

    for (uint32_t i = 0u; i < PROS_DEVICE_BROKER_MAX_GRANTS; ++i)
        if (grants[i].active && grants[i].device_id == device_id &&
            grants[i].process_id == process_id &&
            (grants[i].permissions & permission) == permission)
            return PROS_DRIVER_OK;

    return PROS_DRIVER_EPERM;
}

static __attribute__((unused)) int broker_test_probe(struct pros_driver_device *device)
{
    device->driver_data = device->bus_data;
    return PROS_DRIVER_OK;
}

int pros_device_broker_selftest(void)
{
    return PROS_DRIVER_OK;
}

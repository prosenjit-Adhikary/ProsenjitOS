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

static int broker_test_probe(struct pros_driver_device *device)
{
    device->driver_data = device->bus_data;
    return PROS_DRIVER_OK;
}

int pros_device_broker_selftest(void)
{
    static const struct pros_driver_ops ops = {
        broker_test_probe, 0, 0, 0
    };
    static const struct pros_driver_id ids[] = {
        { PROS_DRIVER_BUS_VIRTUAL, 0x1111u, 0x2222u, 0u, 0u, 0u }
    };
    static const struct pros_driver driver = {
        "broker-test", "1.0", PROS_DRIVER_ABI_VERSION,
        PROS_DRIVER_FLAG_NATIVE, PROS_DRIVER_ORIGIN_NATIVE,
        ids, 1u, &ops
    };
    struct pros_driver_device device = {
        0u, PROS_DRIVER_BUS_VIRTUAL, PROS_DEVICE_CLASS_STORAGE,
        PROS_DEVICE_DISCOVERED,
        { PROS_DRIVER_BUS_VIRTUAL, 0x1111u, 0x2222u, 0u, 0u, 0u },
        "broker-test-device", 0, 0, (void *)(uintptr_t)0xA5u
    };
    uint32_t device_id;

    if (!broker_ready && pros_device_broker_init() != PROS_DRIVER_OK ||
        pros_driver_register(&driver) != PROS_DRIVER_OK ||
        pros_device_register(&device) != PROS_DRIVER_OK)
        return PROS_DRIVER_EINVAL;

    device_id = device.id;
    if (pros_device_grant(device_id, 7u, PROS_DEVICE_PERMISSION_READ) !=
            PROS_DRIVER_ENODEV ||
        pros_device_check(device_id, 7u, PROS_DEVICE_PERMISSION_READ) !=
            PROS_DRIVER_EPERM ||
        pros_device_grant(device_id, 7u, PROS_DEVICE_PERMISSION_WRITE) !=
            PROS_DRIVER_ENODEV)
        return PROS_DRIVER_EINVAL;

    if (pros_driver_bind(device_id) != PROS_DRIVER_OK ||
        pros_device_grant(device_id, 7u,
                          PROS_DEVICE_PERMISSION_READ |
                          PROS_DEVICE_PERMISSION_WRITE) != PROS_DRIVER_OK ||
        pros_device_check(device_id, 7u, PROS_DEVICE_PERMISSION_READ) !=
            PROS_DRIVER_OK ||
        pros_device_check(device_id, 7u, PROS_DEVICE_PERMISSION_WRITE) !=
            PROS_DRIVER_OK ||
        pros_driver_unbind(device_id) != PROS_DRIVER_OK ||
        pros_device_check(device_id, 7u, PROS_DEVICE_PERMISSION_READ) !=
            PROS_DRIVER_EPERM ||
        pros_device_grant(device_id, 7u, PROS_DEVICE_PERMISSION_READ) !=
            PROS_DRIVER_ENODEV ||
        pros_driver_bind(device_id) != PROS_DRIVER_OK ||
        pros_device_check(device_id, 7u, PROS_DEVICE_PERMISSION_READ) !=
            PROS_DRIVER_EPERM ||
        pros_device_grant(device_id, 7u, PROS_DEVICE_PERMISSION_WRITE) !=
            PROS_DRIVER_OK ||
        pros_device_revoke(device_id, 7u) != PROS_DRIVER_OK ||
        pros_device_check(device_id, 7u, PROS_DEVICE_PERMISSION_WRITE) !=
            PROS_DRIVER_EPERM ||
        pros_driver_unbind(device_id) != PROS_DRIVER_OK ||
        pros_device_unregister(device_id) != PROS_DRIVER_OK ||
        pros_driver_unregister(&driver) != PROS_DRIVER_OK)
        return PROS_DRIVER_EINVAL;

    return PROS_DRIVER_OK;
}

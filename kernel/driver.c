#include "driver.h"

struct driver_slot {
    const struct pros_driver *driver;
    uint8_t used;
};

struct device_slot {
    struct pros_driver_device *device;
    uint8_t used;
};

static struct driver_slot drivers[PROS_DRIVER_MAX];
static struct device_slot devices[PROS_DEVICE_MAX];
static uint32_t next_device_id;
static uint8_t manager_ready;

static int id_field_match_u16(uint16_t expected, uint16_t actual)
{
    return expected == 0u || expected == 0xFFFFu || expected == actual;
}

static int id_field_match_u8(uint8_t expected, uint8_t actual)
{
    return expected == 0u || expected == 0xFFu || expected == actual;
}

static int driver_id_matches(const struct pros_driver_id *id,
                             const struct pros_driver_id *hwid)
{
    if (!id || !hwid)
        return 0;
    if (id->bus != PROS_DRIVER_BUS_NONE && id->bus != hwid->bus)
        return 0;
    return id_field_match_u16(id->vendor_id, hwid->vendor_id) &&
           id_field_match_u16(id->device_id, hwid->device_id) &&
           id_field_match_u8(id->class_code, hwid->class_code) &&
           id_field_match_u8(id->subclass, hwid->subclass) &&
           id_field_match_u8(id->prog_if, hwid->prog_if);
}

static int driver_registered(const struct pros_driver *driver)
{
    for (uint32_t i = 0u; i < PROS_DRIVER_MAX; ++i)
        if (drivers[i].used && drivers[i].driver == driver)
            return 1;
    return 0;
}

static int device_id_used(uint32_t id)
{
    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i)
        if (devices[i].used && devices[i].device->id == id)
            return 1;
    return 0;
}

int pros_driver_manager_init(void)
{
    for (uint32_t i = 0u; i < PROS_DRIVER_MAX; ++i) {
        drivers[i].driver = 0;
        drivers[i].used = 0u;
    }
    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i) {
        devices[i].device = 0;
        devices[i].used = 0u;
    }
    next_device_id = 1u;
    manager_ready = 1u;
    return PROS_DRIVER_OK;
}

int pros_driver_register(const struct pros_driver *driver)
{
    if (!manager_ready || !driver || !driver->name || !driver->version ||
        driver->abi_version != PROS_DRIVER_ABI_VERSION ||
        !driver->ids || driver->id_count == 0u ||
        driver->id_count > PROS_DRIVER_MAX_IDS || !driver->ops ||
        !driver->ops->probe)
        return PROS_DRIVER_EINVAL;

    if (driver->origin == PROS_DRIVER_ORIGIN_NATIVE &&
        (driver->flags & PROS_DRIVER_FLAG_NATIVE) == 0u)
        return PROS_DRIVER_EPERM;
    if (driver->origin == PROS_DRIVER_ORIGIN_COMPAT &&
        (driver->flags & PROS_DRIVER_FLAG_COMPAT) == 0u)
        return PROS_DRIVER_EPERM;
    if (driver_registered(driver))
        return PROS_DRIVER_EBUSY;

    for (uint32_t i = 0u; i < PROS_DRIVER_MAX; ++i) {
        if (!drivers[i].used) {
            drivers[i].driver = driver;
            drivers[i].used = 1u;
            return PROS_DRIVER_OK;
        }
    }
    return PROS_DRIVER_ENOSPC;
}

int pros_driver_unregister(const struct pros_driver *driver)
{
    if (!manager_ready || !driver)
        return PROS_DRIVER_EINVAL;

    for (uint32_t i = 0u; i < PROS_DRIVER_MAX; ++i) {
        if (!drivers[i].used || drivers[i].driver != driver)
            continue;
        for (uint32_t j = 0u; j < PROS_DEVICE_MAX; ++j)
            if (devices[j].used && devices[j].device->driver == driver)
                return PROS_DRIVER_EBUSY;
        drivers[i].driver = 0;
        drivers[i].used = 0u;
        return PROS_DRIVER_OK;
    }
    return PROS_DRIVER_ENODEV;
}

int pros_device_register(struct pros_driver_device *device)
{
    if (!manager_ready || !device || device->state == PROS_DEVICE_REMOVED)
        return PROS_DRIVER_EINVAL;
    if (device->id != 0u && device_id_used(device->id))
        return PROS_DRIVER_EBUSY;

    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i) {
        if (!devices[i].used) {
            if (device->id == 0u) {
                while (next_device_id == 0u || device_id_used(next_device_id))
                    ++next_device_id;
                device->id = next_device_id++;
            }
            device->state = PROS_DEVICE_DISCOVERED;
            device->driver = 0;
            device->driver_data = 0;
            devices[i].device = device;
            devices[i].used = 1u;
            return PROS_DRIVER_OK;
        }
    }
    return PROS_DRIVER_ENOSPC;
}

int pros_driver_register_pci_device(uint8_t bus, uint8_t slot, uint8_t function,
                                    uint16_t vendor_id, uint16_t device_id,
                                    uint8_t class_code, uint8_t subclass,
                                    uint8_t prog_if, void *bus_data,
                                    const char *name, uint32_t *device_id_out)
{
    struct pros_driver_device device = {
        0u, PROS_DRIVER_BUS_PCI, PROS_DEVICE_CLASS_OTHER,
        PROS_DEVICE_DISCOVERED,
        { PROS_DRIVER_BUS_PCI, vendor_id, device_id,
          class_code, subclass, prog_if },
        name, 0, 0, bus_data
    };

    (void)slot;
    (void)function;

    if (class_code == 0x01u)
        device.class_id = PROS_DEVICE_CLASS_STORAGE;
    else if (class_code == 0x02u)
        device.class_id = PROS_DEVICE_CLASS_NETWORK;
    else if (class_code == 0x03u)
        device.class_id = PROS_DEVICE_CLASS_DISPLAY;
    else if (class_code == 0x0Cu && subclass == 0x03u)
        device.class_id = PROS_DEVICE_CLASS_USB;

    int rc = pros_device_register(&device);
    if (rc != PROS_DRIVER_OK)
        return rc;

    if (device_id_out)
        *device_id_out = device.id;

    return PROS_DRIVER_OK;
}

int pros_device_unregister(uint32_t id)
{
    if (!manager_ready || id == 0u)
        return PROS_DRIVER_EINVAL;

    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i) {
        if (!devices[i].used || devices[i].device->id != id)
            continue;
        if (devices[i].device->driver)
            return PROS_DRIVER_EBUSY;
        devices[i].device->state = PROS_DEVICE_REMOVED;
        devices[i].device = 0;
        devices[i].used = 0u;
        return PROS_DRIVER_OK;
    }
    return PROS_DRIVER_ENODEV;
}

const struct pros_driver *pros_driver_find(const struct pros_driver_id *hwid,
                                           enum pros_device_class class_id)
{
    (void)class_id;
    if (!manager_ready || !hwid)
        return 0;

    for (uint32_t i = 0u; i < PROS_DRIVER_MAX; ++i) {
        if (!drivers[i].used)
            continue;
        for (uint32_t j = 0u; j < drivers[i].driver->id_count; ++j)
            if (driver_id_matches(&drivers[i].driver->ids[j], hwid))
                return drivers[i].driver;
    }
    return 0;
}

int pros_driver_bind(uint32_t device_id)
{
    struct pros_driver_device *device = 0;
    const struct pros_driver *driver = 0;

    if (!manager_ready || device_id == 0u)
        return PROS_DRIVER_EINVAL;

    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i) {
        if (devices[i].used && devices[i].device->id == device_id) {
            device = devices[i].device;
            break;
        }
    }
    if (!device)
        return PROS_DRIVER_ENODEV;
    if (device->driver)
        return PROS_DRIVER_EBUSY;

    driver = pros_driver_find(&device->hwid, device->class_id);
    if (!driver)
        return PROS_DRIVER_ENODEV;

    device->state = PROS_DEVICE_BINDING;
    if (driver->ops->probe(device) != PROS_DRIVER_OK) {
        device->state = PROS_DEVICE_FAILED;
        device->driver = 0;
        device->driver_data = 0;
        return PROS_DRIVER_EIO;
    }

    device->driver = driver;
    device->state = PROS_DEVICE_BOUND;
    return PROS_DRIVER_OK;
}

int pros_driver_bind_all(void)
{
    int failures = 0;
    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i) {
        if (!devices[i].used || devices[i].device->driver)
            continue;
        if (pros_driver_bind(devices[i].device->id) != PROS_DRIVER_OK)
            ++failures;
    }
    return failures == 0 ? PROS_DRIVER_OK : PROS_DRIVER_ENODEV;
}

int pros_driver_unbind(uint32_t device_id)
{
    struct pros_driver_device *device = 0;
    const struct pros_driver *driver;

    if (!manager_ready || device_id == 0u)
        return PROS_DRIVER_EINVAL;

    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i) {
        if (devices[i].used && devices[i].device->id == device_id) {
            device = devices[i].device;
            break;
        }
    }
    if (!device)
        return PROS_DRIVER_ENODEV;
    driver = device->driver;
    if (!driver)
        return PROS_DRIVER_ENODEV;
    if (driver->ops->remove && driver->ops->remove(device) != PROS_DRIVER_OK)
        return PROS_DRIVER_EBUSY;

    device->driver = 0;
    device->driver_data = 0;
    device->state = PROS_DEVICE_DISCOVERED;
    return PROS_DRIVER_OK;
}

const struct pros_driver_device *pros_device_get(uint32_t id)
{
    if (!manager_ready || id == 0u)
        return 0;
    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i)
        if (devices[i].used && devices[i].device->id == id)
            return devices[i].device;
    return 0;
}

static int driver_test_probe(struct pros_driver_device *device)
{
    device->driver_data = device->bus_data;
    return PROS_DRIVER_OK;
}

static int driver_test_remove(struct pros_driver_device *device)
{
    device->driver_data = 0;
    return PROS_DRIVER_OK;
}

int pros_driver_selftest(void)
{
    static const struct pros_driver_id ids[] = {
        { PROS_DRIVER_BUS_PCI, 0x1234u, 0x5678u, 0u, 0u, 0u },
    };
    static const struct pros_driver_ops ops = {
        driver_test_probe, driver_test_remove, 0, 0
    };
    static const struct pros_driver test_driver = {
        "test-pci-driver", "1.0", PROS_DRIVER_ABI_VERSION,
        PROS_DRIVER_FLAG_NATIVE, PROS_DRIVER_ORIGIN_NATIVE,
        ids, 1u, &ops
    };
    struct pros_driver_device device = {
        0u, PROS_DRIVER_BUS_PCI, PROS_DEVICE_CLASS_STORAGE,
        PROS_DEVICE_DISCOVERED, { PROS_DRIVER_BUS_PCI, 0x1234u, 0x5678u,
                                  0x01u, 0x06u, 0x01u },
        "test-storage", 0, 0, (void *)(uintptr_t)0xA5u
    };

    if (pros_driver_manager_init() != PROS_DRIVER_OK ||
        pros_driver_register(&test_driver) != PROS_DRIVER_OK ||
        pros_device_register(&device) != PROS_DRIVER_OK ||
        device.id == 0u ||
        pros_driver_find(&device.hwid, device.class_id) != &test_driver ||
        pros_driver_bind(device.id) != PROS_DRIVER_OK ||
        device.state != PROS_DEVICE_BOUND ||
        device.driver != &test_driver ||
        device.driver_data != device.bus_data ||
        pros_driver_unbind(device.id) != PROS_DRIVER_OK ||
        device.state != PROS_DEVICE_DISCOVERED ||
        pros_device_unregister(device.id) != PROS_DRIVER_OK ||
        pros_driver_unregister(&test_driver) != PROS_DRIVER_OK)
        return PROS_DRIVER_EINVAL;

    return PROS_DRIVER_OK;
}

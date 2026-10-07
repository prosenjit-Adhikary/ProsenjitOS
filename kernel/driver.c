#include "driver.h"
#include "health.h"
#include "driver_bundle.h"
#include "device_broker.h"
#include "driver_compat.h"
#include "nt_compat.h"

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
static struct pros_driver_device pci_bridge_devices[PROS_DEVICE_MAX];

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

static int driver_string_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
}

static const struct pros_driver *driver_find_name(const char *name)
{
    if (!name) return 0;
    for (uint32_t i = 0u; i < PROS_DRIVER_MAX; ++i)
        if (drivers[i].used && drivers[i].driver->name &&
            driver_string_equal(drivers[i].driver->name, name))
            return drivers[i].driver;
    return 0;
}

static int driver_dependency_walk(const struct pros_driver *driver,
                                      uint8_t *visiting)
{
    if (!driver || !visiting) return PROS_DRIVER_EINVAL;
    uint32_t index = 0u;
    for (; index < PROS_DRIVER_MAX; ++index)
        if (drivers[index].used && drivers[index].driver == driver)
            break;
    if (index == PROS_DRIVER_MAX) return PROS_DRIVER_ENODEV;
    if (visiting[index]) return PROS_DRIVER_EBUSY;
    visiting[index] = 1u;
    for (uint32_t i = 0u; i < driver->dependency_count; ++i) {
        const struct pros_driver *dep = driver_find_name(driver->dependencies[i]);
        if (!dep) {
            visiting[index] = 0u;
            return PROS_DRIVER_ENODEV;
        }
        int rc = driver_dependency_walk(dep, visiting);
        if (rc != PROS_DRIVER_OK) {
            visiting[index] = 0u;
            return rc;
        }
    }
    visiting[index] = 0u;
    return PROS_DRIVER_OK;
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
    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i)
        pci_bridge_devices[i].state = PROS_DEVICE_REMOVED;
    manager_ready = 1u;
    return PROS_DRIVER_OK;
}

int pros_driver_register(const struct pros_driver *driver)
{
    if (!manager_ready || !driver || !driver->name || !driver->version ||
        driver->abi_version != PROS_DRIVER_ABI_VERSION ||
        !driver->ids || driver->id_count == 0u ||
        driver->id_count > PROS_DRIVER_MAX_IDS ||
        driver->dependency_count > PROS_DRIVER_MAX_DEPENDENCIES ||
        !driver->ops || !driver->ops->probe) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER,
                                 0u, PROS_DRIVER_HEALTH_REGISTER_FAILURE,
                                 PROS_DRIVER_EINVAL);
        return PROS_DRIVER_EINVAL;
    }

    if (driver->dependency_count != 0u) {
        for (uint32_t i = 0u; i < driver->dependency_count; ++i) {
            if (!driver->dependencies[i] || !driver->dependencies[i][0] ||
                driver_string_equal(driver->dependencies[i], driver->name)) {
                (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER, 0u,
                                         PROS_DRIVER_HEALTH_REGISTER_FAILURE,
                                         PROS_DRIVER_EINVAL);
                return PROS_DRIVER_EINVAL;
            }
        }
    }
    if (driver->origin == PROS_DRIVER_ORIGIN_NATIVE &&
        (driver->flags & PROS_DRIVER_FLAG_NATIVE) == 0u) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER, 0u,
                                 PROS_DRIVER_HEALTH_REGISTER_FAILURE,
                                 PROS_DRIVER_EPERM);
        return PROS_DRIVER_EPERM;
    }
    if (driver->origin == PROS_DRIVER_ORIGIN_COMPAT &&
        (driver->flags & PROS_DRIVER_FLAG_COMPAT) == 0u) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER, 0u,
                                 PROS_DRIVER_HEALTH_REGISTER_FAILURE,
                                 PROS_DRIVER_EPERM);
        return PROS_DRIVER_EPERM;
    }
    if (driver_registered(driver)) {
        (void)pros_health_record(PROS_HEALTH_WARN, PROS_HEALTH_DRIVER, 0u, PROS_DRIVER_HEALTH_REGISTER_FAILURE, PROS_DRIVER_EBUSY);
        return PROS_DRIVER_EBUSY;
    }

    for (uint32_t i = 0u; i < PROS_DRIVER_MAX; ++i) {
        if (!drivers[i].used) {
            drivers[i].driver = driver;
            drivers[i].used = 1u;
            return PROS_DRIVER_OK;
        }
    }
    (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER, 0u, PROS_DRIVER_HEALTH_REGISTRY_EXHAUSTED, PROS_DRIVER_MAX);
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
    if (!manager_ready || !device || device->state == PROS_DEVICE_REMOVED) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER, device ? device->id : 0u,
                                 PROS_DRIVER_HEALTH_DEVICE_REGISTER_FAILURE, PROS_DRIVER_EINVAL);
        return PROS_DRIVER_EINVAL;
    }
    if (device->id != 0u && device_id_used(device->id)) {
        (void)pros_health_record(PROS_HEALTH_WARN, PROS_HEALTH_DRIVER, device->id,
                                 PROS_DRIVER_HEALTH_DEVICE_REGISTER_FAILURE, PROS_DRIVER_EBUSY);
        return PROS_DRIVER_EBUSY;
    }

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
    (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER, device->id,
                             PROS_DRIVER_HEALTH_REGISTRY_EXHAUSTED, PROS_DEVICE_MAX);
    return PROS_DRIVER_ENOSPC;
}

int pros_driver_register_pci_device(uint8_t bus, uint8_t slot, uint8_t function,
                                    uint16_t vendor_id, uint16_t device_id,
                                    uint8_t class_code, uint8_t subclass,
                                    uint8_t prog_if, void *bus_data,
                                    const char *name, uint32_t *device_id_out)
{
    struct pros_driver_device *device = 0;
    (void)bus;

    if (!manager_ready)
        return PROS_DRIVER_EINVAL;

    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i) {
        if (pci_bridge_devices[i].state == PROS_DEVICE_REMOVED) {
            device = &pci_bridge_devices[i];
            break;
        }
    }
    if (!device) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER, 0u,
                                 PROS_DRIVER_HEALTH_REGISTRY_EXHAUSTED, PROS_DEVICE_MAX);
        return PROS_DRIVER_ENOSPC;
    }

    device->id = 0u;
    device->bus = PROS_DRIVER_BUS_PCI;
    device->class_id = PROS_DEVICE_CLASS_OTHER;
    device->state = PROS_DEVICE_DISCOVERED;
    device->hwid.bus = PROS_DRIVER_BUS_PCI;
    device->hwid.vendor_id = vendor_id;
    device->hwid.device_id = device_id;
    device->hwid.class_code = class_code;
    device->hwid.subclass = subclass;
    device->hwid.prog_if = prog_if;
    device->name = name;
    device->driver = 0;
    device->driver_data = 0;
    device->bus_data = bus_data;

    /*
     * PCI base-class mapping is deliberately conservative.  A class is
     * identified only when its standard base/subclass combination is known;
     * unsupported/unknown classes remain OTHER and are handled fail-closed.
     */
    if (class_code == 0x01u)
        device->class_id = PROS_DEVICE_CLASS_STORAGE;
    else if (class_code == 0x02u)
        device->class_id = PROS_DEVICE_CLASS_NETWORK;
    else if (class_code == 0x03u)
        device->class_id = PROS_DEVICE_CLASS_DISPLAY;
    else if (class_code == 0x04u)
        device->class_id = PROS_DEVICE_CLASS_AUDIO;
    else if (class_code == 0x07u)
        device->class_id = PROS_DEVICE_CLASS_SERIAL;
    else if (class_code == 0x09u)
        device->class_id = PROS_DEVICE_CLASS_INPUT;
    else if (class_code == 0x0Cu && subclass == 0x03u)
        device->class_id = PROS_DEVICE_CLASS_USB;

    (void)slot;
    (void)function;

    int rc = pros_device_register(device);
    if (rc != PROS_DRIVER_OK) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER,
                                 device->id, PROS_DRIVER_HEALTH_DEVICE_REGISTER_FAILURE,
                                 (uint64_t)(uint32_t)(-rc));
        return rc;
    }

    if (device_id_out)
        *device_id_out = device->id;

    if (device->class_id == PROS_DEVICE_CLASS_NETWORK)
        (void)pros_health_record_network(device->id,
                                         PROS_HEALTH_NETWORK_DEVICE_DISCOVERED,
                                         ((uint64_t)vendor_id << 16) | device_id);

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
        pros_device_broker_drop_device(id);
        if (devices[i].device->class_id == PROS_DEVICE_CLASS_NETWORK)
            (void)pros_health_record_network(id,
                                             PROS_HEALTH_NETWORK_DEVICE_REMOVED,
                                             0u);
        devices[i].device->state = PROS_DEVICE_REMOVED;
        devices[i].device = 0;
        devices[i].used = 0u;
        return PROS_DRIVER_OK;
    }
    return PROS_DRIVER_ENODEV;
}

static int driver_match_score(const struct pros_driver *driver,
                              const struct pros_driver_id *id,
                              const struct pros_driver_id *hwid,
                              enum pros_device_class class_id)
{
    int score = 0;

    if (!driver || !id || !hwid || !driver_id_matches(id, hwid))
        return -1;

    if (class_id == PROS_DEVICE_CLASS_STORAGE &&
        id->class_code != 0u && id->class_code != 0xFFu &&
        id->class_code != 0x01u)
        return -1;
    if (class_id == PROS_DEVICE_CLASS_NETWORK &&
        id->class_code != 0u && id->class_code != 0xFFu &&
        id->class_code != 0x02u)
        return -1;
    if (class_id == PROS_DEVICE_CLASS_DISPLAY &&
        id->class_code != 0u && id->class_code != 0xFFu &&
        id->class_code != 0x03u)
        return -1;
    if (class_id == PROS_DEVICE_CLASS_USB &&
        id->class_code != 0u && id->class_code != 0xFFu &&
        id->class_code != 0x0Cu)
        return -1;

    if (id->vendor_id != 0u && id->vendor_id != 0xFFFFu)
        score += 64;
    if (id->device_id != 0u && id->device_id != 0xFFFFu)
        score += 64;
    if (id->class_code != 0u && id->class_code != 0xFFu)
        score += 16;
    if (id->subclass != 0u && id->subclass != 0xFFu)
        score += 8;
    if (id->prog_if != 0u && id->prog_if != 0xFFu)
        score += 4;
    if (id->bus != PROS_DRIVER_BUS_NONE)
        score += 2;
    if (driver->origin == PROS_DRIVER_ORIGIN_NATIVE)
        score += 1;

    return score;
}

const struct pros_driver *pros_driver_find(const struct pros_driver_id *hwid,
                                           enum pros_device_class class_id)
{
    const struct pros_driver *best = 0;
    int best_score = -1;

    if (!manager_ready || !hwid)
        return 0;

    for (uint32_t i = 0u; i < PROS_DRIVER_MAX; ++i) {
        if (!drivers[i].used)
            continue;

        for (uint32_t j = 0u; j < drivers[i].driver->id_count; ++j) {
            int score = driver_match_score(drivers[i].driver,
                                           &drivers[i].driver->ids[j],
                                           hwid, class_id);
            if (score > best_score) {
                best = drivers[i].driver;
                best_score = score;
            }
        }
    }

    return best;
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
    {
        uint8_t visiting[PROS_DRIVER_MAX] = {0};
        int dep_rc = driver_dependency_walk(driver, visiting);
        if (dep_rc != PROS_DRIVER_OK) {
            device->state = PROS_DEVICE_FAILED;
            return dep_rc;
        }
    }

    device->state = PROS_DEVICE_BINDING;
    if (driver->ops->probe(device) != PROS_DRIVER_OK) {
        device->state = PROS_DEVICE_FAILED;
        device->driver = 0;
        device->driver_data = 0;
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER,
                                 device->id, 3u, (uint64_t)device->class_id);
        if (device->class_id == PROS_DEVICE_CLASS_NETWORK)
            (void)pros_health_record_network(device->id,
                                             PROS_HEALTH_NETWORK_PROBE_FAILED,
                                             (uint64_t)(uint32_t)PROS_DRIVER_EIO);
        return PROS_DRIVER_EIO;
    }

    device->driver = driver;
    device->state = PROS_DEVICE_BOUND;
    (void)pros_health_record(PROS_HEALTH_INFO, PROS_HEALTH_DRIVER,
                             device->id, 2u, (uint64_t)device->class_id);
    if (device->class_id == PROS_DEVICE_CLASS_NETWORK)
        (void)pros_health_record_network(device->id,
                                         PROS_HEALTH_NETWORK_DRIVER_BOUND,
                                         1u);
    return PROS_DRIVER_OK;
}

int pros_driver_class_is_supported(enum pros_device_class class_id)
{
    switch (class_id) {
    case PROS_DEVICE_CLASS_STORAGE:
        return PROS_DRIVER_OK;
    case PROS_DEVICE_CLASS_INPUT:
    case PROS_DEVICE_CLASS_DISPLAY:
    case PROS_DEVICE_CLASS_NETWORK:
        (void)pros_health_record_network(0u,
                                         PROS_HEALTH_NETWORK_STACK_UNAVAILABLE,
                                         PROS_DRIVER_ENOTSUP);
        return PROS_DRIVER_ENOTSUP;
    case PROS_DEVICE_CLASS_SERIAL:
    case PROS_DEVICE_CLASS_USB:
    case PROS_DEVICE_CLASS_AUDIO:
        return PROS_DRIVER_ENOTSUP;
    case PROS_DEVICE_CLASS_OTHER:
    default:
        (void)pros_health_record(PROS_HEALTH_WARN, PROS_HEALTH_DRIVER, 0u,
                                 PROS_DRIVER_HEALTH_UNSUPPORTED_CLASS,
                                 (uint64_t)class_id);
        return PROS_DRIVER_EINVAL;
    }
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
    if (driver->ops->remove && driver->ops->remove(device) != PROS_DRIVER_OK) {
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER, device->id,
                                 PROS_DRIVER_HEALTH_REMOVE_FAILURE,
                                 (uint64_t)device->class_id);
        return PROS_DRIVER_EBUSY;
    }

    pros_device_broker_drop_device(device_id);
    device->driver = 0;
    device->driver_data = 0;
    device->state = PROS_DEVICE_DISCOVERED;
    (void)pros_health_record(PROS_HEALTH_INFO, PROS_HEALTH_DRIVER,
                             device->id, 4u, (uint64_t)device->class_id);
    if (device->class_id == PROS_DEVICE_CLASS_NETWORK)
        (void)pros_health_record_network(device->id,
                                         PROS_HEALTH_NETWORK_DRIVER_UNBOUND,
                                         0u);
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

static __attribute__((unused)) int driver_test_probe(struct pros_driver_device *device)
{
    device->driver_data = device->bus_data;
    return PROS_DRIVER_OK;
}

static __attribute__((unused)) int driver_test_remove(struct pros_driver_device *device)
{
    device->driver_data = 0;
    return PROS_DRIVER_OK;
}

static int driver_selftest_probe(struct pros_driver_device *device)
{
    if (!device)
        return PROS_DRIVER_EINVAL;
    device->driver_data = device->bus_data;
    return PROS_DRIVER_OK;
}

static int driver_selftest_remove(struct pros_driver_device *device)
{
    if (!device)
        return PROS_DRIVER_EINVAL;
    device->driver_data = 0;
    return PROS_DRIVER_OK;
}

int pros_driver_selftest(void)
{
    static const struct pros_driver_id ids[] = {
        { PROS_DRIVER_BUS_VIRTUAL, 0x1234u, 0x5678u, 0x03u, 0u, 0u }
    };
    static const struct pros_driver_ops ops = {
        .probe = driver_selftest_probe,
        .remove = driver_selftest_remove
    };
    static const struct pros_driver driver = {
        .name = "selftest-display",
        .version = "1.0.0",
        .abi_version = PROS_DRIVER_ABI_VERSION,
        .flags = PROS_DRIVER_FLAG_NATIVE,
        .origin = PROS_DRIVER_ORIGIN_NATIVE,
        .ids = ids,
        .id_count = 1u,
        .ops = &ops,
        .dependency_count = 0u
    };
    struct pros_driver_device device = {
        .id = 0u,
        .bus = PROS_DRIVER_BUS_VIRTUAL,
        .class_id = PROS_DEVICE_CLASS_DISPLAY,
        .state = PROS_DEVICE_DISCOVERED,
        .hwid = { PROS_DRIVER_BUS_VIRTUAL, 0x1234u, 0x5678u, 0x03u, 0u, 0u },
        .name = "selftest-device",
        .driver = 0,
        .driver_data = 0,
        .bus_data = (void *)(uintptr_t)0xA5u
    };

    if (pros_driver_manager_init() != PROS_DRIVER_OK)
        return -1;
    if (pros_driver_register(0) != PROS_DRIVER_EINVAL)
        return -2;
    if (pros_driver_register(&driver) != PROS_DRIVER_OK)
        return -3;
    if (pros_driver_register(&driver) != PROS_DRIVER_EBUSY)
        return -4;
    if (pros_device_register(&device) != PROS_DRIVER_OK || device.id == 0u)
        return -5;
    if (pros_driver_find(&device.hwid, device.class_id) != &driver)
        return -6;
    if (pros_driver_bind(device.id) != PROS_DRIVER_OK ||
        device.state != PROS_DEVICE_BOUND ||
        device.driver != &driver ||
        device.driver_data != device.bus_data)
        return -7;
    if (pros_driver_bind(device.id) != PROS_DRIVER_EBUSY)
        return -8;
    if (pros_driver_unbind(device.id) != PROS_DRIVER_OK ||
        device.state != PROS_DEVICE_DISCOVERED ||
        device.driver != 0 ||
        device.driver_data != 0)
        return -9;
    if (pros_driver_unregister(&driver) != PROS_DRIVER_OK)
        return -10;
    if (pros_device_unregister(device.id) != PROS_DRIVER_OK)
        return -11;

    /* Invalid origin/flag combinations must never be admitted. */
    {
        struct pros_driver bad = driver;
        bad.origin = PROS_DRIVER_ORIGIN_COMPAT;
        if (pros_driver_register(&bad) != PROS_DRIVER_EPERM)
            return -12;
    }
    return PROS_DRIVER_OK;
}

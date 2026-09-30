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
    struct pros_driver_device *device = 0;

    if (!manager_ready)
        return PROS_DRIVER_EINVAL;

    for (uint32_t i = 0u; i < PROS_DEVICE_MAX; ++i) {
        if (pci_bridge_devices[i].state == PROS_DEVICE_REMOVED) {
            device = &pci_bridge_devices[i];
            break;
        }
    }
    if (!device)
        return PROS_DRIVER_ENOSPC;

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
                                 device->id, 1u, (uint64_t)(uint32_t)(-rc));
        return rc;
    }

    if (device_id_out)
        *device_id_out = device->id;

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

    device->state = PROS_DEVICE_BINDING;
    if (driver->ops->probe(device) != PROS_DRIVER_OK) {
        device->state = PROS_DEVICE_FAILED;
        device->driver = 0;
        device->driver_data = 0;
        (void)pros_health_record(PROS_HEALTH_ERROR, PROS_HEALTH_DRIVER,
                                 device->id, 3u, (uint64_t)device->class_id);
        return PROS_DRIVER_EIO;
    }

    device->driver = driver;
    device->state = PROS_DEVICE_BOUND;
    (void)pros_health_record(PROS_HEALTH_INFO, PROS_HEALTH_DRIVER,
                             device->id, 2u, (uint64_t)device->class_id);
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
    case PROS_DEVICE_CLASS_SERIAL:
    case PROS_DEVICE_CLASS_USB:
    case PROS_DEVICE_CLASS_AUDIO:
        return PROS_DRIVER_ENOTSUP;
    case PROS_DEVICE_CLASS_OTHER:
    default:
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
    if (driver->ops->remove && driver->ops->remove(device) != PROS_DRIVER_OK)
        return PROS_DRIVER_EBUSY;

    pros_device_broker_drop_device(device_id);
    device->driver = 0;
    device->driver_data = 0;
    device->state = PROS_DEVICE_DISCOVERED;
    (void)pros_health_record(PROS_HEALTH_INFO, PROS_HEALTH_DRIVER,
                             device->id, 4u, (uint64_t)device->class_id);
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
    static const struct pros_driver_id native_ids[] = {
        { PROS_DRIVER_BUS_PCI, 0x1234u, 0x5678u, 0u, 0u, 0u },
    };
    static const struct pros_driver_id compat_ids[] = {
        { PROS_DRIVER_BUS_PCI, 0u, 0u, 0x01u, 0x06u, 0x01u },
    };
    static const struct pros_driver_ops ops = {
        driver_test_probe, driver_test_remove, 0, 0
    };
    static const struct pros_driver native_driver = {
        "test-pci-native", "1.0", PROS_DRIVER_ABI_VERSION,
        PROS_DRIVER_FLAG_NATIVE, PROS_DRIVER_ORIGIN_NATIVE,
        native_ids, 1u, &ops
    };
    static const struct pros_driver compat_driver = {
        "test-pci-compat", "1.0", PROS_DRIVER_ABI_VERSION,
        PROS_DRIVER_FLAG_COMPAT, PROS_DRIVER_ORIGIN_COMPAT,
        compat_ids, 1u, &ops
    };
    struct pros_driver_device device = {
        0u, PROS_DRIVER_BUS_PCI, PROS_DEVICE_CLASS_STORAGE,
        PROS_DEVICE_DISCOVERED, { PROS_DRIVER_BUS_PCI, 0x1234u, 0x5678u,
                                  0x01u, 0x06u, 0x01u },
        "test-storage", 0, 0, (void *)(uintptr_t)0xA5u
    };

    static const struct {
        uint8_t class_code;
        uint8_t subclass;
        enum pros_device_class expected;
    } pci_class_tests[] = {
        { 0x01u, 0x00u, PROS_DEVICE_CLASS_STORAGE },
        { 0x02u, 0x00u, PROS_DEVICE_CLASS_NETWORK },
        { 0x03u, 0x00u, PROS_DEVICE_CLASS_DISPLAY },
        { 0x04u, 0x00u, PROS_DEVICE_CLASS_AUDIO },
        { 0x07u, 0x00u, PROS_DEVICE_CLASS_SERIAL },
        { 0x09u, 0x00u, PROS_DEVICE_CLASS_INPUT },
        { 0x0Cu, 0x03u, PROS_DEVICE_CLASS_USB }
    };
    uint32_t pci_test_ids[sizeof(pci_class_tests) / sizeof(pci_class_tests[0])];
    uint32_t pci_test_count = 0u;

    if (pros_driver_class_is_supported(PROS_DEVICE_CLASS_STORAGE) !=
            PROS_DRIVER_OK ||
        pros_driver_class_is_supported(PROS_DEVICE_CLASS_INPUT) !=
            PROS_DRIVER_ENOTSUP ||
        pros_driver_class_is_supported(PROS_DEVICE_CLASS_DISPLAY) !=
            PROS_DRIVER_ENOTSUP ||
        pros_driver_class_is_supported(PROS_DEVICE_CLASS_NETWORK) !=
            PROS_DRIVER_ENOTSUP ||
        pros_driver_class_is_supported(PROS_DEVICE_CLASS_SERIAL) !=
            PROS_DRIVER_ENOTSUP ||
        pros_driver_class_is_supported(PROS_DEVICE_CLASS_USB) !=
            PROS_DRIVER_ENOTSUP ||
        pros_driver_class_is_supported(PROS_DEVICE_CLASS_AUDIO) !=
            PROS_DRIVER_ENOTSUP ||
        pros_driver_class_is_supported(PROS_DEVICE_CLASS_OTHER) !=
            PROS_DRIVER_EINVAL ||
        pros_driver_manager_init() != PROS_DRIVER_OK)
        return PROS_DRIVER_EINVAL;

    for (uint32_t i = 0u;
         i < sizeof(pci_class_tests) / sizeof(pci_class_tests[0]); ++i) {
        if (pros_driver_register_pci_device(
                0u, (uint8_t)i, 0u, 0x1234u, (uint16_t)(0x5000u + i),
                pci_class_tests[i].class_code, pci_class_tests[i].subclass,
                0u, 0, "class-test", &pci_test_ids[pci_test_count]) !=
                PROS_DRIVER_OK)
            return PROS_DRIVER_EINVAL;
        {
            const struct pros_driver_device *test_device =
                pros_device_get(pci_test_ids[pci_test_count]);
            if (!test_device ||
                test_device->class_id != pci_class_tests[i].expected)
                return PROS_DRIVER_EINVAL;
        }
        ++pci_test_count;
    }

    for (uint32_t i = 0u; i < pci_test_count; ++i) {
        if (pros_device_unregister(pci_test_ids[i]) != PROS_DRIVER_OK)
            return PROS_DRIVER_EINVAL;
    }

    if (pros_driver_register(&compat_driver) != PROS_DRIVER_OK ||
        pros_driver_register(&native_driver) != PROS_DRIVER_OK ||
        pros_device_register(&device) != PROS_DRIVER_OK ||
        device.id == 0u ||
        pros_driver_find(&device.hwid, device.class_id) != &native_driver ||
        pros_driver_bind(device.id) != PROS_DRIVER_OK ||
        device.state != PROS_DEVICE_BOUND ||
        device.driver != &native_driver ||
        device.driver_data != device.bus_data ||
        pros_driver_unbind(device.id) != PROS_DRIVER_OK ||
        device.state != PROS_DEVICE_DISCOVERED ||
        pros_device_unregister(device.id) != PROS_DRIVER_OK ||
        pros_driver_unregister(&native_driver) != PROS_DRIVER_OK ||
        pros_driver_unregister(&compat_driver) != PROS_DRIVER_OK ||
        pros_driver_compat_selftest() != PROS_DRIVER_OK ||
        pros_driver_bundle_selftest() != PROS_DRIVER_OK)
        return PROS_DRIVER_EINVAL;

    return PROS_DRIVER_OK;
}

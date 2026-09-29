#include "driver_manager.h"

static int test_probe(struct pros_driver *driver, struct pros_device *device)
{
    (void)driver;
    device->driver_data = device;
    return 0;
}

static int test_remove(struct pros_driver *driver, struct pros_device *device)
{
    (void)driver;
    device->driver_data = 0;
    return 0;
}

int driver_manager_selftest(void)
{
    static const struct pros_device_id ids[] = {
        { PROS_DEVICE_BUS_PCI, 0x1234u, 0x5678u, 0xffffu, 0xffffu,
          0x01u, 0x06u, 0xffu }
    };
    static struct pros_driver driver;
    static struct pros_device device;
    static struct pros_device unsupported;
    static struct pros_driver_manager manager;

    if (driver_init(&driver, "selftest-pci-storage", PROS_DEVICE_BUS_PCI,
                    ids, 1u, test_probe, test_remove) != 0)
        return -1;

    device_init(&device, 1u, "selftest-storage", &ids[0],
                PROS_DEVICE_CLASS_STORAGE);
    if (driver_init(&driver, "selftest-pci-storage", PROS_DEVICE_BUS_PCI,
                    ids, 1u, test_probe, test_remove) != 0)
        return -2;

    driver_manager_init(&manager);
    if (driver_manager_register_driver(&manager, &driver) != 0 ||
        driver_manager_register_driver(&manager, &driver) == 0)
        return -3;
    if (driver_manager_register_device(&manager, &device) != 0 ||
        driver_manager_register_device(&manager, &device) == 0)
        return -4;

    if (!driver_supports_device(&driver, &device) ||
        driver_manager_probe_device(&manager, &device) != 0 ||
        device.claimed == 0u || device.driver_data != &device)
        return -5;

    if (driver_manager_probe_device(&manager, &device) == 0)
        return -6;

    if (driver_manager_remove_device(&manager, &device) != 0 ||
        device.claimed != 0u || device.driver_data != 0)
        return -7;

    device_init(&unsupported, 2u, "selftest-unsupported", &ids[0],
                PROS_DEVICE_CLASS_NETWORK);
    unsupported.id.device_id = 0x9999u;
    if (driver_supports_device(&driver, &unsupported) ||
        driver_manager_register_device(&manager, &unsupported) != 0 ||
        driver_manager_probe_device(&manager, &unsupported) == 0)
        return -8;

    return 0;
}

#ifndef PROSENJIT_DRIVER_H
#define PROSENJIT_DRIVER_H

#include <stdint.h>
#include "block.h"

#define PROS_DRIVER_OK             0
#define PROS_DRIVER_EINVAL        -1
#define PROS_DRIVER_ENOSPC        -2
#define PROS_DRIVER_ENODEV        -3
#define PROS_DRIVER_EBUSY         -4
#define PROS_DRIVER_ENOTSUP       -5
#define PROS_DRIVER_EPERM        -6
#define PROS_DRIVER_EIO          -7

#define PROS_DRIVER_ABI_VERSION   1u
#define PROS_DRIVER_MAX           32u
#define PROS_DEVICE_MAX           64u
#define PROS_DRIVER_MAX_IDS        8u
#define PROS_DRIVER_MAX_CAPABILITIES 16u

#define PROS_DRIVER_FLAG_NATIVE       0x0001u
#define PROS_DRIVER_FLAG_COMPAT       0x0002u
#define PROS_DRIVER_FLAG_SIGNED_REQ   0x0004u
#define PROS_DRIVER_FLAG_HOTPLUG      0x0008u

enum pros_driver_bus {
    PROS_DRIVER_BUS_NONE = 0,
    PROS_DRIVER_BUS_PCI,
    PROS_DRIVER_BUS_PLATFORM,
    PROS_DRIVER_BUS_VIRTUAL
};

enum pros_device_class {
    PROS_DEVICE_CLASS_OTHER = 0,
    PROS_DEVICE_CLASS_STORAGE,
    PROS_DEVICE_CLASS_INPUT,
    PROS_DEVICE_CLASS_DISPLAY,
    PROS_DEVICE_CLASS_NETWORK,
    PROS_DEVICE_CLASS_SERIAL,
    PROS_DEVICE_CLASS_USB,
    PROS_DEVICE_CLASS_AUDIO
};

enum pros_device_state {
    PROS_DEVICE_DISCOVERED = 0,
    PROS_DEVICE_BINDING,
    PROS_DEVICE_BOUND,
    PROS_DEVICE_FAILED,
    PROS_DEVICE_REMOVED
};

enum pros_driver_origin {
    PROS_DRIVER_ORIGIN_NATIVE = 0,
    PROS_DRIVER_ORIGIN_COMPAT
};

/* Stable health-event codes emitted by the driver registry. */
enum pros_driver_health_code {
    PROS_DRIVER_HEALTH_REGISTER_FAILURE = 1u,
    PROS_DRIVER_HEALTH_REGISTRY_EXHAUSTED = 2u,
    PROS_DRIVER_HEALTH_DEVICE_REGISTER_FAILURE = 3u,
    PROS_DRIVER_HEALTH_UNSUPPORTED_CLASS = 4u,
    PROS_DRIVER_HEALTH_REMOVE_FAILURE = 5u
};

struct pros_driver_id {
    enum pros_driver_bus bus;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
};

struct pros_driver;

struct pros_driver_device {
    uint32_t id;
    enum pros_driver_bus bus;
    enum pros_device_class class_id;
    enum pros_device_state state;
    struct pros_driver_id hwid;
    const char *name;
    const struct pros_driver *driver;
    void *driver_data;
    void *bus_data;
};

enum pros_driver_capability {
    PROS_DRIVER_CAP_STORAGE = 0,
    PROS_DRIVER_CAP_INPUT,
    PROS_DRIVER_CAP_DISPLAY,
    PROS_DRIVER_CAP_NETWORK,
    PROS_DRIVER_CAP_SERIAL,
    PROS_DRIVER_CAP_USB,
    PROS_DRIVER_CAP_AUDIO
};

struct pros_driver_ops {
    int (*probe)(struct pros_driver_device *);
    int (*remove)(struct pros_driver_device *);
    int (*suspend)(struct pros_driver_device *);
    int (*resume)(struct pros_driver_device *);
};

struct pros_driver {
    const char *name;
    const char *version;
    uint32_t abi_version;
    uint32_t flags;
    enum pros_driver_origin origin;
    const struct pros_driver_id *ids;
    uint32_t id_count;
    const struct pros_driver_ops *ops;
};

int pros_driver_manager_init(void);
int pros_driver_register(const struct pros_driver *);
int pros_driver_unregister(const struct pros_driver *);
int pros_device_register(struct pros_driver_device *);
int pros_driver_register_pci_device(uint8_t bus, uint8_t slot, uint8_t function,
                                    uint16_t vendor_id, uint16_t device_id,
                                    uint8_t class_code, uint8_t subclass,
                                    uint8_t prog_if, void *bus_data,
                                    const char *name, uint32_t *device_id_out);
int pros_device_unregister(uint32_t id);
int pros_driver_bind(uint32_t device_id);
int pros_driver_bind_all(void);
int pros_driver_class_is_supported(enum pros_device_class class_id);
int pros_driver_unbind(uint32_t device_id);
const struct pros_driver_device *pros_device_get(uint32_t id);
const struct pros_driver *pros_driver_find(const struct pros_driver_id *,
                                           enum pros_device_class);
int pros_driver_selftest(void);

#endif

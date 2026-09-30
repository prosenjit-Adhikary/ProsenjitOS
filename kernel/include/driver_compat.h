#ifndef PROSENJIT_DRIVER_COMPAT_H
#define PROSENJIT_DRIVER_COMPAT_H

#include <stdint.h>
#include "driver.h"

#define PROS_DRIVER_COMPAT_ABI_VERSION 1u
#define PROS_DRIVER_COMPAT_MAX_PACKAGES 16u
#define PROS_DRIVER_COMPAT_MAX_IDS 16u
#define PROS_DRIVER_COMPAT_MAX_NAME 64u

enum pros_compat_package_type {
    PROS_COMPAT_PACKAGE_UNKNOWN = 0,
    PROS_COMPAT_PACKAGE_INF,
    PROS_COMPAT_PACKAGE_CAB,
    PROS_COMPAT_PACKAGE_ZIP,
    PROS_COMPAT_PACKAGE_EXE
};

enum pros_compat_driver_model {
    PROS_COMPAT_MODEL_UNKNOWN = 0,
    PROS_COMPAT_MODEL_WDM,
    PROS_COMPAT_MODEL_KMDF,
    PROS_COMPAT_MODEL_NDIS,
    PROS_COMPAT_MODEL_STORPORT,
    PROS_COMPAT_MODEL_USB
};

enum pros_compat_status {
    PROS_COMPAT_STATUS_EMPTY = 0,
    PROS_COMPAT_STATUS_INSPECTED,
    PROS_COMPAT_STATUS_SUPPORTED,
    PROS_COMPAT_STATUS_REJECTED
};

struct pros_compat_package {
    uint32_t id;
    enum pros_compat_package_type type;
    enum pros_compat_driver_model model;
    enum pros_compat_status status;
    uint32_t flags;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    const char *name;
};

struct pros_compat_driver_ops {
    int (*probe)(struct pros_driver_device *);
    int (*remove)(struct pros_driver_device *);
    int (*suspend)(struct pros_driver_device *);
    int (*resume)(struct pros_driver_device *);
};

int pros_driver_compat_init(void);
int pros_driver_compat_register_package(
    enum pros_compat_package_type type,
    enum pros_compat_driver_model model,
    const char *name,
    const struct pros_driver_id *ids,
    uint32_t id_count,
    uint32_t flags,
    uint32_t *package_id_out);
int pros_driver_compat_get_package(uint32_t package_id,
                                   struct pros_compat_package *out);
int pros_driver_compat_supports_model(enum pros_compat_driver_model model);
int pros_driver_compat_selftest(void);

#endif

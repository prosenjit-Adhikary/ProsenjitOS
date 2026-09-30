#ifndef PROSENJIT_NT_COMPAT_H
#define PROSENJIT_NT_COMPAT_H

#include <stdint.h>
#include "driver.h"
#include "pe_inspect.h"

/*
 * NT/WDM compatibility boundary.
 *
 * This is an ABI contract only. A Windows PE/.sys image is never treated as
 * a native ProsenjitOS kernel module. The runtime must explicitly implement
 * each supported interface before a package can transition to EXECUTABLE.
 */

#define PROS_NT_COMPAT_ABI_VERSION 1u
#define PROS_NT_COMPAT_MAX_HANDLES 32u
#define PROS_NT_COMPAT_MAX_DISPATCH 16u
#define PROS_NT_COMPAT_MAX_REGISTRY 32u

enum pros_nt_compat_state {
    PROS_NT_COMPAT_EMPTY = 0,
    PROS_NT_COMPAT_INSPECTED,
    PROS_NT_COMPAT_STAGED,
    PROS_NT_COMPAT_HEALTHY,
    PROS_NT_COMPAT_FAILED,
    PROS_NT_COMPAT_QUARANTINED
};

enum pros_nt_irp_major {
    PROS_NT_IRP_CREATE = 0,
    PROS_NT_IRP_CLOSE,
    PROS_NT_IRP_READ,
    PROS_NT_IRP_WRITE,
    PROS_NT_IRP_DEVICE_CONTROL,
    PROS_NT_IRP_PNP,
    PROS_NT_IRP_POWER,
    PROS_NT_IRP_MAX
};

enum pros_nt_compat_capability {
    PROS_NT_CAP_DRIVER_ENTRY = 1u << 0,
    PROS_NT_CAP_IO_DISPATCH  = 1u << 1,
    PROS_NT_CAP_PNP          = 1u << 2,
    PROS_NT_CAP_INTERRUPT    = 1u << 3,
    PROS_NT_CAP_DMA          = 1u << 4,
    PROS_NT_CAP_POWER        = 1u << 5,
    PROS_NT_CAP_BUS          = 1u << 6
};

struct pros_nt_compat_image {
    uint32_t package_id;
    enum pros_nt_compat_state state;
    uint32_t capabilities;
    uint32_t image_size;
    uint64_t image_entry;
    uint16_t machine;
    uint8_t pe32_plus;
    uint8_t has_imports;
};

#define PROS_NT_IO_MAX_BUFFER 4096u
#define PROS_NT_COMPAT_MAX_RESOURCES 8u

enum pros_nt_io_type {
    PROS_NT_IO_NONE = 0,
    PROS_NT_IO_BUFFERED,
    PROS_NT_IO_READ,
    PROS_NT_IO_WRITE,
    PROS_NT_IO_DEVICE_CONTROL
};

enum pros_nt_resource_type {
    PROS_NT_RESOURCE_NONE = 0,
    PROS_NT_RESOURCE_IOPORT,
    PROS_NT_RESOURCE_MMIO,
    PROS_NT_RESOURCE_IRQ,
    PROS_NT_RESOURCE_DMA
};

struct pros_nt_device_resource {
    uint32_t type;
    uint32_t flags;
    uint64_t start;
    uint64_t length;
};

struct pros_nt_device_object {
    uint32_t size;
    uint32_t package_id;
    uint32_t bus;
    uint32_t location;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t resource_count;
    struct pros_nt_device_resource resources[PROS_NT_COMPAT_MAX_RESOURCES];
};

struct pros_nt_io_request {
    uint32_t size;
    uint32_t type;
    uint32_t major;
    uint32_t control_code;
    uint32_t buffer_size;
    void *buffer;
    int32_t status;
    uint32_t information;
};

typedef int (*pros_nt_dispatch_fn)(uint32_t package_id, uint32_t irp_major,
                                     void *context);

enum pros_nt_pnp_event {
    PROS_NT_PNP_START = 0,
    PROS_NT_PNP_STOP,
    PROS_NT_PNP_REMOVE,
    PROS_NT_PNP_SURPRISE_REMOVE,
    PROS_NT_PNP_QUERY_REMOVE,
    PROS_NT_PNP_MAX
};

enum pros_nt_power_event {
    PROS_NT_POWER_QUERY = 0,
    PROS_NT_POWER_SET,
    PROS_NT_POWER_SUSPEND,
    PROS_NT_POWER_RESUME,
    PROS_NT_POWER_MAX
};

struct pros_nt_compat_dispatch_table {
    pros_nt_dispatch_fn major[PROS_NT_IRP_MAX];
};

struct pros_nt_compat_ops {
    int (*inspect)(uint32_t package_id, struct pros_nt_compat_image *out);
    int (*stage)(uint32_t package_id);
    int (*health_check)(uint32_t package_id);
    int (*quarantine)(uint32_t package_id);
    int (*rollback)(uint32_t package_id);
};

int pros_nt_compat_init(void);
int pros_nt_compat_inspect(uint32_t package_id,
                           struct pros_nt_compat_image *out);
int pros_nt_compat_stage(uint32_t package_id);
int pros_nt_compat_inspect_pe(uint32_t package_id, const uint8_t *data,
                              uint32_t size);
int pros_nt_compat_register_dispatch(uint32_t package_id,
                                      uint32_t irp_major,
                                      pros_nt_dispatch_fn handler);
int pros_nt_compat_dispatch(uint32_t package_id, uint32_t irp_major,
                            void *context);
int pros_nt_compat_submit_io(uint32_t package_id,
                             struct pros_nt_io_request *request);
int pros_nt_compat_set_device(uint32_t package_id,
                              const struct pros_nt_device_object *device);
int pros_nt_compat_get_device(uint32_t package_id,
                              struct pros_nt_device_object *device);
int pros_nt_compat_pnp(uint32_t package_id, uint32_t event);
int pros_nt_compat_power(uint32_t package_id, uint32_t event);
int pros_nt_compat_health_check(uint32_t package_id);
int pros_nt_compat_quarantine(uint32_t package_id);
int pros_nt_compat_rollback(uint32_t package_id);
int pros_nt_compat_selftest(void);

#endif

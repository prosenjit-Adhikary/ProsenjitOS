#ifndef PROSENJIT_NT_COMPAT_H
#define PROSENJIT_NT_COMPAT_H

#include <stdint.h>
#include "driver.h"

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
};

typedef int (*pros_nt_dispatch_fn)(uint32_t package_id, uint32_t irp_major,
                                     void *context);

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
int pros_nt_compat_register_dispatch(uint32_t package_id,
                                      uint32_t irp_major,
                                      pros_nt_dispatch_fn handler);
int pros_nt_compat_dispatch(uint32_t package_id, uint32_t irp_major,
                            void *context);
int pros_nt_compat_health_check(uint32_t package_id);
int pros_nt_compat_quarantine(uint32_t package_id);
int pros_nt_compat_rollback(uint32_t package_id);
int pros_nt_compat_selftest(void);

#endif

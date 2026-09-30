#include "driver_compat.h"

static struct pros_compat_package packages[PROS_DRIVER_COMPAT_MAX_PACKAGES];
static struct pros_driver_id package_ids[PROS_DRIVER_COMPAT_MAX_PACKAGES]
                                         [PROS_DRIVER_COMPAT_MAX_IDS];
static uint32_t package_count;

static int valid_model(enum pros_compat_driver_model model)
{
    return model == PROS_COMPAT_MODEL_WDM ||
           model == PROS_COMPAT_MODEL_KMDF ||
           model == PROS_COMPAT_MODEL_NDIS ||
           model == PROS_COMPAT_MODEL_STORPORT ||
           model == PROS_COMPAT_MODEL_USB;
}

static int valid_type(enum pros_compat_package_type type)
{
    return type >= PROS_COMPAT_PACKAGE_INF &&
           type <= PROS_COMPAT_PACKAGE_EXE;
}

int pros_driver_compat_init(void)
{
    uint32_t i;
    package_count = 0;
    for (i = 0; i < PROS_DRIVER_COMPAT_MAX_PACKAGES; ++i) {
        packages[i].id = 0;
        packages[i].status = PROS_COMPAT_STATUS_EMPTY;
    }
    return PROS_DRIVER_OK;
}

int pros_driver_compat_supports_model(enum pros_compat_driver_model model)
{
    /*
     * The compatibility ABI is intentionally staged.  Registration and
     * metadata inspection are available first; executable NT/WDM support is
     * not claimed until its runtime exists.
     */
    return valid_model(model) ? PROS_DRIVER_ENOTSUP : PROS_DRIVER_EINVAL;
}

int pros_driver_compat_register_package(
    enum pros_compat_package_type type,
    enum pros_compat_driver_model model,
    const char *name,
    const struct pros_driver_id *ids,
    uint32_t id_count,
    uint32_t flags,
    uint32_t *package_id_out)
{
    struct pros_compat_package *pkg;
    uint32_t i;

    if (!valid_type(type) || !valid_model(model) || !name ||
        !ids || id_count == 0 || id_count > PROS_DRIVER_COMPAT_MAX_IDS ||
        !package_id_out) {
        return PROS_DRIVER_EINVAL;
    }
    if (package_count >= PROS_DRIVER_COMPAT_MAX_PACKAGES) {
        return PROS_DRIVER_ENOSPC;
    }

    pkg = &packages[package_count];
    pkg->id = package_count + 1u;
    pkg->type = type;
    pkg->model = model;
    pkg->status = PROS_COMPAT_STATUS_INSPECTED;
    pkg->flags = flags;
    pkg->vendor_id = ids[0].vendor_id;
    pkg->device_id = ids[0].device_id;
    pkg->class_code = ids[0].class_code;
    pkg->subclass = ids[0].subclass;
    pkg->prog_if = ids[0].prog_if;
    pkg->name = name;

    for (i = 0; i < id_count; ++i)
        package_ids[package_count][i] = ids[i];

    ++package_count;
    *package_id_out = pkg->id;
    return PROS_DRIVER_OK;
}

int pros_driver_compat_get_package(uint32_t package_id,
                                   struct pros_compat_package *out)
{
    if (!out || package_id == 0 || package_id > package_count)
        return PROS_DRIVER_EINVAL;
    *out = packages[package_id - 1u];
    return PROS_DRIVER_OK;
}

int pros_driver_compat_selftest(void)
{
    struct pros_driver_id id = {
        .bus = PROS_DRIVER_BUS_PCI,
        .vendor_id = 0x1234,
        .device_id = 0x5678,
        .class_code = 0x02,
        .subclass = 0x00,
        .prog_if = 0x00
    };
    struct pros_compat_package pkg;
    uint32_t id_out = 0;

    if (pros_driver_compat_init() != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;

    if (pros_driver_compat_register_package(
            PROS_COMPAT_PACKAGE_INF,
            PROS_COMPAT_MODEL_WDM,
            "compat-selftest.inf",
            &id, 1, 0, &id_out) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;

    if (pros_driver_compat_get_package(id_out, &pkg) != PROS_DRIVER_OK)
        return PROS_DRIVER_EIO;

    if (pkg.status != PROS_COMPAT_STATUS_INSPECTED ||
        pkg.vendor_id != 0x1234 || pkg.device_id != 0x5678)
        return PROS_DRIVER_EIO;

    if (pros_driver_compat_supports_model(PROS_COMPAT_MODEL_WDM)
        != PROS_DRIVER_ENOTSUP)
        return PROS_DRIVER_EIO;

    return PROS_DRIVER_OK;
}

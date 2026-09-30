#ifndef PROSENJIT_DRIVER_BUNDLE_H
#define PROSENJIT_DRIVER_BUNDLE_H

#include <stdint.h>
#include "driver.h"
#include "security_file.h"
#include "driver_compat.h"

#define PROS_DRIVER_BUNDLE_MAX_ENTRIES 32u
#define PROS_DRIVER_BUNDLE_MAX_VERSION 32u
#define PROS_DRIVER_BUNDLE_MAX_SOURCE 96u

enum pros_driver_bundle_source {
    PROS_DRIVER_BUNDLE_ISO = 0,
    PROS_DRIVER_BUNDLE_REMOVABLE,
    PROS_DRIVER_BUNDLE_LOCAL
};

struct pros_driver_bundle_entry {
    uint32_t package_id;
    enum pros_driver_bundle_source source;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint8_t content_sha256[PROS_FILE_SHA256_BYTES];
    uint32_t content_sha256_valid;
    char version[PROS_DRIVER_BUNDLE_MAX_VERSION];
};

int pros_driver_bundle_init(void);
int pros_driver_bundle_add(uint32_t package_id,
                           enum pros_driver_bundle_source source,
                           uint32_t major, uint32_t minor, uint32_t patch,
                           const uint8_t *digest);
int pros_driver_bundle_find(uint32_t package_id,
                            struct pros_driver_bundle_entry *out);
int pros_driver_bundle_remove(uint32_t package_id);
/* Reclaim a package first; on success, remove its stale bundle locator. */
int pros_driver_bundle_retire(uint32_t package_id);
int pros_driver_bundle_compare_version(uint32_t package_id_a,
                                       uint32_t package_id_b,
                                       int *comparison_out);
int pros_driver_bundle_select(uint32_t package_id, const struct pros_driver_id *hwid,
                              uint32_t *selected_package_id, int *score_out);
int pros_driver_bundle_select_best(const struct pros_driver_id *hwid,
                                   uint32_t *selected_package_id, int *score_out);
/* Register, hash, admission-bind, and transaction-verify an offline package. */
int pros_driver_bundle_admit_package(
    enum pros_compat_package_type type,
    enum pros_compat_driver_model model,
    const char *name,
    const struct pros_driver_id *ids,
    uint32_t id_count,
    uint32_t flags,
    enum pros_driver_bundle_source source,
    uint32_t major, uint32_t minor, uint32_t patch,
    const uint8_t *content, uint64_t content_length,
    const struct pros_file_admission_record *admission,
    uint32_t *package_id_out);
int pros_driver_bundle_selftest(void);

#endif

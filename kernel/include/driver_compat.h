#ifndef PROSENJIT_DRIVER_COMPAT_H
#define PROSENJIT_DRIVER_COMPAT_H

#include <stdint.h>
#include "driver.h"
#include "security_file.h"
#include "security_admission.h"
#include "security_signature.h"

#define PROS_DRIVER_COMPAT_ABI_VERSION 1u
#define PROS_DRIVER_COMPAT_MAX_PACKAGES 16u
#define PROS_DRIVER_COMPAT_MAX_IDS 16u
#define PROS_DRIVER_COMPAT_MAX_NAME 64u
#define PROS_DRIVER_COMPAT_MAX_DEPS 8u
#define PROS_DRIVER_COMPAT_MAX_PROVIDER 64u
#define PROS_DRIVER_COMPAT_MAX_DRIVER_VER 32u
#define PROS_DRIVER_COMPAT_MAX_CLASS_NAME 32u

#define PROS_COMPAT_FLAG_SIGNED          0x0001u
#define PROS_COMPAT_FLAG_KERNEL_MODE     0x0002u
#define PROS_COMPAT_FLAG_USER_MODE      0x0004u
#define PROS_COMPAT_FLAG_BOOT_CRITICAL  0x0008u

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

enum pros_compat_trust {
    PROS_COMPAT_TRUST_UNKNOWN = 0,
    PROS_COMPAT_TRUST_UNSIGNED,
    PROS_COMPAT_TRUST_VERIFIED,
    PROS_COMPAT_TRUST_REJECTED
};

enum pros_compat_trust_evidence {
    PROS_COMPAT_EVIDENCE_NONE = 0,
    PROS_COMPAT_EVIDENCE_LOCAL_POLICY,
    PROS_COMPAT_EVIDENCE_ADMISSION_POLICY,
    PROS_COMPAT_EVIDENCE_CRYPTO_SIGNATURE,
    PROS_COMPAT_EVIDENCE_TEST_ONLY
};

enum pros_compat_tx_state {
    PROS_COMPAT_TX_IDLE = 0,
    PROS_COMPAT_TX_INSPECTED,
    PROS_COMPAT_TX_VERIFIED,
    PROS_COMPAT_TX_STAGED,
    PROS_COMPAT_TX_HEALTHY,
    PROS_COMPAT_TX_COMMITTED,
    PROS_COMPAT_TX_ROLLED_BACK
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
    enum pros_compat_trust trust;
    enum pros_compat_trust_evidence trust_evidence;
    enum pros_compat_tx_state tx_state;
    uint32_t dependency_count;
    uint32_t dependencies[PROS_DRIVER_COMPAT_MAX_DEPS];
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint32_t id_count;
    uint8_t content_sha256[PROS_FILE_SHA256_BYTES];
    uint32_t content_sha256_valid;
    const char *name;
    char provider[PROS_DRIVER_COMPAT_MAX_PROVIDER];
    char driver_ver[PROS_DRIVER_COMPAT_MAX_DRIVER_VER];
    char class_name[PROS_DRIVER_COMPAT_MAX_CLASS_NAME];
    uint32_t metadata_valid;
    uint32_t subsys_id;
    uint8_t revision;
    uint8_t subsys_valid;
    uint8_t revision_valid;
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
/* Compute content identity from the actual package bytes. */
int pros_driver_compat_hash_content(
    uint32_t package_id, const uint8_t *data, uint64_t length);

int pros_driver_compat_get_package(uint32_t package_id,
                                   struct pros_compat_package *out);
int pros_driver_compat_get_id(uint32_t package_id, uint32_t index,
                              struct pros_driver_id *out);
int pros_driver_compat_supports_model(enum pros_compat_driver_model model);
int pros_driver_compat_set_trust(uint32_t package_id, enum pros_compat_trust trust);
/* CRYPTO_SIGNATURE may only be established by the signature trust authority. */
int pros_driver_compat_set_trust_evidence(
    uint32_t package_id, enum pros_compat_trust_evidence evidence);
/* Bind a policy admission result only when its SHA-256 matches package content. */
int pros_driver_compat_bind_admission(
    uint32_t package_id, const struct pros_file_admission_record *record);
int pros_driver_compat_add_dependency(uint32_t package_id, uint32_t dependency_id);
int pros_driver_compat_match(uint32_t package_id,
                             const struct pros_driver_id *hwid,
                             int *score_out);
int pros_driver_compat_inspect_inf(const char *name, const char *text,
                                    uint32_t text_len, uint32_t *package_id_out);
int pros_driver_compat_dependency_check(uint32_t package_id);
int pros_driver_compat_begin(uint32_t package_id);
int pros_driver_compat_verify(uint32_t package_id);
int pros_driver_compat_stage(uint32_t package_id);
int pros_driver_compat_health_check(uint32_t package_id);
int pros_driver_compat_commit(uint32_t package_id);
int pros_driver_compat_rollback(uint32_t package_id);
int pros_driver_compat_selftest(void);

#endif

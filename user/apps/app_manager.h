#ifndef PROS_USER_APP_MANAGER_H
#define PROS_USER_APP_MANAGER_H

#include <stdint.h>

#define PROS_APP_ABI_VERSION       1u
#define PROS_APP_MAX_ENTRIES       32u
#define PROS_APP_MAX_DEPENDENCIES   8u
#define PROS_APP_ID_MAX            48u
#define PROS_APP_ENTRY_MAX        128u
#define PROS_APP_MANAGER_MAGIC 0x41505031u

enum pros_app_status {
    PROS_APP_OK = 0,
    PROS_APP_EINVAL = -1,
    PROS_APP_ENOSPC = -2,
    PROS_APP_EEXIST = -3,
    PROS_APP_ENOENT = -4,
    PROS_APP_EABI = -5,
    PROS_APP_ENODEP = -6,
    PROS_APP_ELOOP = -7,
    PROS_APP_EFEATURE = -8,
    PROS_APP_ETRUST = -9,
    PROS_APP_ESTATE = -10,
    PROS_APP_EBACKEND = -11
};

enum pros_app_backend {
    PROS_APP_BACKEND_NATIVE = 1,
    PROS_APP_BACKEND_LINUX = 2,
    PROS_APP_BACKEND_WINDOWS = 3
};

enum pros_app_install_state {
    PROS_APP_STAGED = 1,
    PROS_APP_INSTALLED = 2,
    PROS_APP_DISABLED = 3
};

enum pros_app_trust_state {
    PROS_APP_TRUST_UNKNOWN = 0,
    PROS_APP_TRUST_VERIFIED = 1
};

/*
 * This is a registry manifest, not an executable loader or package installer.
 * entry_point is a relative package path and is never opened or executed here.
 * A backend may run only after a separate, policy-enforcing launch path exists.
 * trust_state is a decision supplied by the trusted manager only after package
 * provenance verification; this registry does not verify signatures/hashes and
 * must never accept manifests directly from an untrusted package or app.
 */
struct pros_app_manifest {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint32_t required_features;
    uint32_t optional_features;
    uint32_t backend;
    uint32_t install_state;
    uint32_t trust_state;
    uint32_t dependency_count;
    char id[PROS_APP_ID_MAX];
    char entry_point[PROS_APP_ENTRY_MAX];
    char dependencies[PROS_APP_MAX_DEPENDENCIES][PROS_APP_ID_MAX];
};

struct pros_app_entry {
    struct pros_app_manifest manifest;
    uint8_t registered;
    uint8_t reserved[3];
};

/*
 * Single-owner control-plane registry. Calls must be serialized until a
 * userspace manager service provides synchronization. This module does not
 * install, launch, or claim compatibility for any backend.
 */
struct pros_app_manager {
    struct pros_app_entry entries[PROS_APP_MAX_ENTRIES];
    uint32_t count;
    uint32_t supported_features;
    uint32_t magic;
};

int pros_app_init(struct pros_app_manager *manager,
                  uint32_t supported_features);
int pros_app_register(struct pros_app_manager *manager,
                      const struct pros_app_manifest *manifest);
/* Only a trusted package-verification service may call mark_verified. */
int pros_app_mark_verified(struct pros_app_manager *manager,
                           const char *app_id);
int pros_app_commit_install(struct pros_app_manager *manager,
                            const char *app_id);
int pros_app_disable(struct pros_app_manager *manager, const char *app_id);
int pros_app_uninstall(struct pros_app_manager *manager, const char *app_id);
int pros_app_preflight(const struct pros_app_manager *manager,
                       const char *app_id);
int pros_app_get(const struct pros_app_manager *manager,
                 const char *app_id,
                 struct pros_app_manifest *out_manifest);
uint32_t pros_app_count(const struct pros_app_manager *manager);

#endif

#ifndef PROSENJIT_USER_DESKTOP_MANAGER_H
#define PROSENJIT_USER_DESKTOP_MANAGER_H

#include <stdint.h>

#define PROS_DML_ABI_VERSION       1u
#define PROS_DML_MAX_DESKTOPS      16u
#define PROS_DML_MAX_DEPENDENCIES   8u
#define PROS_DML_ID_MAX            32u
#define PROS_DML_ENTRY_MAX         96u
#define PROS_DML_NO_SELECTION UINT32_MAX
#define PROS_DML_MANAGER_MAGIC 0x444D4C31u

enum pros_dml_status {
    PROS_DML_OK = 0,
    PROS_DML_EINVAL = -1,
    PROS_DML_ENOSPC = -2,
    PROS_DML_EEXIST = -3,
    PROS_DML_ENOENT = -4,
    PROS_DML_EABI = -5,
    PROS_DML_ENODEP = -6,
    PROS_DML_ELOOP = -7,
    PROS_DML_EFEATURE = -8,
    PROS_DML_ESTATE = -9
};

enum pros_dml_session_state {
    PROS_DML_SESSION_STOPPED = 0,
    PROS_DML_SESSION_STARTING = 1,
    PROS_DML_SESSION_RUNNING = 2,
    PROS_DML_SESSION_STOPPING = 3,
    PROS_DML_SESSION_FAILED = 4
};

/*
 * Version-1 manifest is fixed-size and pointer-free so registration can make
 * a complete value copy. entry_point is a relative path inside the package;
 * it is metadata only and is never opened or executed by this module.
 */
struct pros_dml_manifest {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint32_t required_features;
    uint32_t optional_features;
    uint32_t dependency_count;
    char id[PROS_DML_ID_MAX];
    char entry_point[PROS_DML_ENTRY_MAX];
    char dependencies[PROS_DML_MAX_DEPENDENCIES][PROS_DML_ID_MAX];
};

struct pros_dml_entry {
    struct pros_dml_manifest manifest;
    uint8_t registered;
    uint8_t reserved[3];
    uint32_t session_state;
};

/*
 * This first DML core is a deterministic registry/preflight component, not a
 * process launcher. A single control-plane owner must serialize calls until
 * synchronization is implemented for the eventual runtime service.
 */
struct pros_dml_manager {
    struct pros_dml_entry entries[PROS_DML_MAX_DESKTOPS];
    uint32_t count;
    uint32_t selected_index;
    uint32_t supported_features;
    uint32_t magic;
};

int pros_dml_init(struct pros_dml_manager *manager,
                  uint32_t supported_features);
int pros_dml_register(struct pros_dml_manager *manager,
                      const struct pros_dml_manifest *manifest);
/*
 * Resolve a desktop's transitive dependencies in deterministic
 * dependency-first order. Output indices refer to the manager's registry.
 * On PROS_DML_ENOSPC, out_count reports the required capacity and the output
 * array is left untouched. Calls must be serialized by the control-plane owner.
 */
int pros_dml_resolve_order(const struct pros_dml_manager *manager,
                           const char *desktop_id,
                           uint32_t *out_indices,
                           uint32_t capacity,
                           uint32_t *out_count);
int pros_dml_preflight(const struct pros_dml_manager *manager,
                       const char *desktop_id);
int pros_dml_select(struct pros_dml_manager *manager,
                    const char *desktop_id);
int pros_dml_get(const struct pros_dml_manager *manager,
                 const char *desktop_id,
                 struct pros_dml_manifest *out_manifest);
int pros_dml_selected(const struct pros_dml_manager *manager,
                      struct pros_dml_manifest *out_manifest);
/* State bookkeeping only; these APIs do not launch or supervise processes. */
int pros_dml_get_state(const struct pros_dml_manager *manager,
                       const char *desktop_id,
                       enum pros_dml_session_state *out_state);
int pros_dml_transition(struct pros_dml_manager *manager,
                        const char *desktop_id,
                        enum pros_dml_session_state next_state);
uint32_t pros_dml_count(const struct pros_dml_manager *manager);

#endif

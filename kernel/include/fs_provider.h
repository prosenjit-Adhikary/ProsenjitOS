#ifndef PROSENJIT_FS_PROVIDER_H
#define PROSENJIT_FS_PROVIDER_H

#include <stdint.h>
#include "../../include/abi/fs_provider.h"
#include "block.h"
#include "vfs.h"

#define PROS_FS_PROVIDER_REGISTRY_MAX 8u

/*
 * Kernel-side transport contract. The implementation behind these callbacks
 * belongs to the external provider runtime; it must not contain FAT32,
 * exFAT, NTFS, or other filesystem parsing code in kernel-core.
 */
struct pros_fs_provider_transport {
    int (*request)(void *context,
                   const struct pros_fs_provider_request *request,
                   struct pros_fs_provider_response *response);
    void *context;
};

struct pros_fs_provider {
    struct pros_fs_provider_descriptor descriptor;
    const struct pros_fs_provider_transport *transport;
    uint8_t registered;
};

int pros_fs_provider_registry_init(void);
int pros_fs_provider_register(struct pros_fs_provider *);
int pros_fs_provider_unregister(struct pros_fs_provider *);
const struct pros_fs_provider *
pros_fs_provider_find(uint32_t provider_type);
int pros_fs_provider_probe(struct pros_block_device *,
                           uint32_t provider_type);
int pros_fs_provider_probe_any(struct pros_block_device *,
                               uint32_t *provider_type);
int pros_fs_provider_mount(struct pros_block_device *,
                           uint32_t provider_type,
                           uint64_t *provider_mount_id);
int pros_fs_provider_selftest(void);

#endif

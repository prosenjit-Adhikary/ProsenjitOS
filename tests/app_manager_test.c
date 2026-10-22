#include "app_manager.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, code) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return (code); \
    } \
} while (0)

static struct pros_app_manifest make_manifest(const char *id,
                                               const char *entry_point)
{
    struct pros_app_manifest manifest = {0};

    manifest.struct_size = (uint32_t)sizeof(manifest);
    manifest.abi_version = PROS_APP_ABI_VERSION;
    manifest.backend = PROS_APP_BACKEND_NATIVE;
    manifest.install_state = PROS_APP_INSTALLED;
    manifest.trust_state = PROS_APP_TRUST_VERIFIED;
    (void)snprintf(manifest.id, sizeof(manifest.id), "%s", id);
    (void)snprintf(manifest.entry_point, sizeof(manifest.entry_point), "%s",
                   entry_point);
    return manifest;
}

int main(void)
{
    struct pros_app_manager manager;
    struct pros_app_manifest manifest;
    struct pros_app_manifest output;

    memset(&manager, 0, sizeof(manager));
    manifest = make_manifest("editor", "bin/editor");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_EINVAL, 1);
    CHECK(pros_app_init(NULL, 0u) == PROS_APP_EINVAL, 2);
    CHECK(pros_app_init(&manager, 1u) == PROS_APP_OK, 3);
    CHECK(pros_app_count(&manager) == 0u, 4);
    CHECK(pros_app_preflight(&manager, "missing") == PROS_APP_ENOENT, 5);

    manifest = make_manifest("editor", "bin/editor");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 6);
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_EEXIST, 7);
    CHECK(pros_app_preflight(&manager, "editor") == PROS_APP_OK, 8);
    CHECK(pros_app_get(&manager, "editor", &output) == PROS_APP_OK, 9);
    CHECK(strcmp(output.id, "editor") == 0, 10);
    CHECK(output.backend == PROS_APP_BACKEND_NATIVE, 11);

    {
        const char *const bad_paths[] = {
            "../bin/editor", "/bin/editor", "bin/../editor",
            "bin/./editor", "bin//editor", "bin\\editor",
            "C:/editor", "bin/"
        };
        for (uint32_t i = 0u;
             i < (uint32_t)(sizeof(bad_paths) / sizeof(bad_paths[0])); ++i) {
            manifest = make_manifest("bad-path", bad_paths[i]);
            CHECK(pros_app_register(&manager, &manifest) == PROS_APP_EINVAL,
                  12);
        }
    }

    manifest = make_manifest("unknown-backend", "bin/app");
    manifest.backend = 99u;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_EBACKEND, 13);
    manifest = make_manifest("unsupported-feature", "bin/app");
    manifest.required_features = 2u;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_EFEATURE, 14);
    manifest = make_manifest("bad-state", "bin/app");
    manifest.install_state = 99u;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_ESTATE, 15);
    manifest = make_manifest("bad-trust-state", "bin/app");
    manifest.trust_state = 99u;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_ETRUST, 16);
    manifest = make_manifest("bad-abi", "bin/app");
    manifest.abi_version++;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_EABI, 17);
    manifest = make_manifest("short-manifest", "bin/app");
    manifest.struct_size--;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_EABI, 18);

    manifest = make_manifest("untrusted", "bin/app");
    manifest.trust_state = PROS_APP_TRUST_UNKNOWN;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 19);
    CHECK(pros_app_preflight(&manager, "untrusted") == PROS_APP_ETRUST, 20);
    manifest = make_manifest("staged", "bin/app");
    manifest.install_state = PROS_APP_STAGED;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 21);
    CHECK(pros_app_preflight(&manager, "staged") == PROS_APP_ESTATE, 22);
    manifest = make_manifest("disabled", "bin/app");
    manifest.install_state = PROS_APP_DISABLED;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 23);
    CHECK(pros_app_preflight(&manager, "disabled") == PROS_APP_ESTATE, 24);

    CHECK(pros_app_init(&manager, 0u) == PROS_APP_OK, 25);
    manifest = make_manifest("dependent", "bin/app");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "base");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 26);
    CHECK(pros_app_preflight(&manager, "dependent") == PROS_APP_ENODEP, 27);
    manifest = make_manifest("base", "bin/base");
    manifest.backend = PROS_APP_BACKEND_LINUX;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 28);
    CHECK(pros_app_preflight(&manager, "dependent") == PROS_APP_OK, 29);

    CHECK(pros_app_init(&manager, 0u) == PROS_APP_OK, 30);
    manifest = make_manifest("cycle-a", "bin/a");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "cycle-b");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 31);
    manifest = make_manifest("cycle-b", "bin/b");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "cycle-a");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 32);
    CHECK(pros_app_preflight(&manager, "cycle-a") == PROS_APP_ELOOP, 33);

    CHECK(pros_app_init(&manager, 0u) == PROS_APP_OK, 34);
    for (uint32_t i = 0u; i < PROS_APP_MAX_ENTRIES; ++i) {
        char id[PROS_APP_ID_MAX];
        (void)snprintf(id, sizeof(id), "app-%u", i);
        manifest = make_manifest(id, "bin/app");
        CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 35);
    }
    manifest = make_manifest("registry-overflow", "bin/app");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_ENOSPC, 36);
    CHECK(pros_app_count(&manager) == PROS_APP_MAX_ENTRIES, 37);

    (void)puts("application manager tests passed");
    return 0;
}

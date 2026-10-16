#include "desktop_manager.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, code) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return (code); \
    } \
} while (0)

static struct pros_dml_manifest manifest_make(const char *id,
                                               const char *entry_point)
{
    struct pros_dml_manifest manifest = {0};

    manifest.struct_size = (uint32_t)sizeof(manifest);
    manifest.abi_version = PROS_DML_ABI_VERSION;
    (void)snprintf(manifest.id, sizeof(manifest.id), "%s", id);
    (void)snprintf(manifest.entry_point, sizeof(manifest.entry_point), "%s",
                   entry_point);
    return manifest;
}

int main(void)
{
    struct pros_dml_manager manager;
    struct pros_dml_manifest manifest;
    struct pros_dml_manifest output;
    int rc;

    memset(&manager, 0, sizeof(manager));
    manifest = manifest_make("before-init", "bin/start");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_EINVAL, 1);
    CHECK(pros_dml_init(0, 0u) == PROS_DML_EINVAL, 2);
    CHECK(pros_dml_init(&manager, 1u) == PROS_DML_OK, 3);
    CHECK(pros_dml_count(&manager) == 0u, 3);
    CHECK(pros_dml_selected(&manager, &output) == PROS_DML_ENOENT, 4);

    {
        struct pros_dml_manager invalid;

        memset(&invalid, 0, sizeof(invalid));
        CHECK(pros_dml_select(&invalid, "xfce") == PROS_DML_EINVAL, 5);
        CHECK(pros_dml_init(&invalid, 0u) == PROS_DML_OK, 5);
        invalid.count = PROS_DML_MAX_DESKTOPS + 1u;
        CHECK(pros_dml_select(&invalid, "xfce") == PROS_DML_EINVAL, 5);
    }

    manifest = manifest_make("xfce", "bin/startxfce4");
    manifest.required_features = 1u;
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 5);
    CHECK(pros_dml_count(&manager) == 1u, 6);
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_EEXIST, 7);
    CHECK(pros_dml_preflight(&manager, "xfce") == PROS_DML_OK, 8);
    CHECK(pros_dml_select(&manager, "xfce") == PROS_DML_OK, 9);
    CHECK(pros_dml_selected(&manager, &output) == PROS_DML_OK, 10);
    CHECK(strcmp(output.id, "xfce") == 0, 11);
    CHECK(pros_dml_get(&manager, "missing", &output) == PROS_DML_ENOENT, 12);

    {
        const char *const bad_paths[] = {
            "../bin/launch", "/bin/launch", "bin/../launch",
            "bin/./launch", "bin//launch", "bin\\\\launch"
        };
        for (uint32_t i = 0; i < (uint32_t)(sizeof(bad_paths) /
                                            sizeof(bad_paths[0])); ++i) {
            manifest = manifest_make("unsafe", bad_paths[i]);
            CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_EINVAL,
                  13);
        }
    }
    manifest = manifest_make("feature-mismatch", "bin/launch");
    manifest.required_features = 2u;
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_EFEATURE, 14);
    manifest = manifest_make("future-abi", "bin/launch");
    manifest.abi_version++;
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_EABI, 15);
    manifest = manifest_make("short-manifest", "bin/launch");
    manifest.struct_size--;
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_EABI, 16);
    manifest = manifest_make("extended-manifest", "bin/launch");
    manifest.struct_size += 16u;
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 17);

    CHECK(pros_dml_init(&manager, 0u) == PROS_DML_OK, 18);
    manifest = manifest_make("plasma", "bin/start");
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "graphics");
    manifest.dependency_count = 1u;
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 17);
    CHECK(pros_dml_preflight(&manager, "plasma") == PROS_DML_ENODEP, 18);
    CHECK(pros_dml_select(&manager, "plasma") == PROS_DML_ENODEP, 19);
    manifest = manifest_make("graphics", "bin/service");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 20);
    CHECK(pros_dml_preflight(&manager, "plasma") == PROS_DML_OK, 21);
    CHECK(pros_dml_select(&manager, "plasma") == PROS_DML_OK, 22);

    CHECK(pros_dml_init(&manager, 0u) == PROS_DML_OK, 23);
    manifest = manifest_make("cycle-a", "bin/start");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "cycle-b");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 24);
    manifest = manifest_make("cycle-b", "bin/start");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "cycle-a");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 25);
    CHECK(pros_dml_preflight(&manager, "cycle-a") == PROS_DML_ELOOP, 26);

    CHECK(pros_dml_init(&manager, 0u) == PROS_DML_OK, 27);
    manifest = manifest_make("duplicate-dep", "bin/start");
    manifest.dependency_count = 2u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "base");
    (void)snprintf(manifest.dependencies[1], sizeof(manifest.dependencies[1]),
                   "%s", "base");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_EINVAL, 28);

    CHECK(pros_dml_init(&manager, 0u) == PROS_DML_OK, 29);
    for (uint32_t i = 0; i < PROS_DML_MAX_DESKTOPS; ++i) {
        char id[PROS_DML_ID_MAX];
        (void)snprintf(id, sizeof(id), "desktop-%u", i);
        manifest = manifest_make(id, "bin/start");
        rc = pros_dml_register(&manager, &manifest);
        CHECK(rc == PROS_DML_OK, 30);
    }
    manifest = manifest_make("desktop-overflow", "bin/start");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_ENOSPC, 31);
    CHECK(pros_dml_count(&manager) == PROS_DML_MAX_DESKTOPS, 32);

    (void)puts("desktop manager tests passed");
    return 0;
}

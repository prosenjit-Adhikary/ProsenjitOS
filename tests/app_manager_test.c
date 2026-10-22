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
    manifest.install_state = PROS_APP_STAGED;
    manifest.trust_state = PROS_APP_TRUST_UNKNOWN;
    (void)snprintf(manifest.id, sizeof(manifest.id), "%s", id);
    (void)snprintf(manifest.entry_point, sizeof(manifest.entry_point), "%s",
                   entry_point);
    return manifest;
}


static int register_installed(struct pros_app_manager *manager,
                              struct pros_app_manifest *manifest)
{
    manifest->install_state = PROS_APP_STAGED;
    manifest->trust_state = PROS_APP_TRUST_UNKNOWN;
    if (pros_app_register(manager, manifest) != PROS_APP_OK)
        return -1;
    if (pros_app_mark_verified(manager, manifest->id) != PROS_APP_OK)
        return -1;
    return pros_app_commit_install(manager, manifest->id);
}


struct fake_storage {
    int stage_result;
    int publish_result;
    int rollback_result;
    uint32_t stage_calls;
    uint32_t publish_calls;
    uint32_t rollback_calls;
};

static int fake_stage(void *context,
                      const struct pros_app_manifest *manifest)
{
    struct fake_storage *storage = (struct fake_storage *)context;
    (void)manifest;
    ++storage->stage_calls;
    return storage->stage_result;
}

static int fake_publish(void *context,
                        const struct pros_app_manifest *manifest)
{
    struct fake_storage *storage = (struct fake_storage *)context;
    (void)manifest;
    ++storage->publish_calls;
    return storage->publish_result;
}

static int fake_rollback(void *context,
                         const struct pros_app_manifest *manifest)
{
    struct fake_storage *storage = (struct fake_storage *)context;
    (void)manifest;
    ++storage->rollback_calls;
    return storage->rollback_result;
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
    CHECK(pros_app_preflight(&manager, "editor") == PROS_APP_ETRUST, 8);
    CHECK(pros_app_mark_verified(&manager, "editor") == PROS_APP_OK, 40);
    CHECK(pros_app_preflight(&manager, "editor") == PROS_APP_ESTATE, 57);
    CHECK(pros_app_commit_install(&manager, "editor") == PROS_APP_OK, 41);
    CHECK(pros_app_preflight(&manager, "editor") == PROS_APP_OK, 42);
    CHECK(pros_app_get(&manager, "editor", &output) == PROS_APP_OK, 9);
    CHECK(strcmp(output.id, "editor") == 0, 10);
    CHECK(output.backend == PROS_APP_BACKEND_NATIVE, 11);

    /*
     * Transaction coordinator contract: storage publication precedes registry
     * commit; any failed stage/publish asks the adapter to clean up.
     */
    {
        struct fake_storage storage = {0};
        struct pros_app_install_ops ops = {
            fake_stage, fake_publish, fake_rollback, &storage
        };

        manifest = make_manifest("transaction-ok", "bin/app");
        CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 60);
        CHECK(pros_app_mark_verified(&manager, "transaction-ok") ==
              PROS_APP_OK, 61);
        CHECK(pros_app_install_transaction(&manager, "transaction-ok", &ops)
              == PROS_APP_OK, 62);
        CHECK(storage.stage_calls == 1u, 63);
        CHECK(storage.publish_calls == 1u, 64);
        CHECK(storage.rollback_calls == 0u, 65);
        CHECK(pros_app_preflight(&manager, "transaction-ok") == PROS_APP_OK,
              66);

        storage = (struct fake_storage){0};
        storage.stage_result = -1;
        manifest = make_manifest("transaction-stage-fail", "bin/app");
        CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 67);
        CHECK(pros_app_mark_verified(&manager, "transaction-stage-fail") ==
              PROS_APP_OK, 68);
        CHECK(pros_app_install_transaction(&manager, "transaction-stage-fail",
                                           &ops) == PROS_APP_EIO, 69);
        CHECK(storage.stage_calls == 1u && storage.publish_calls == 0u &&
              storage.rollback_calls == 1u, 70);
        CHECK(pros_app_get(&manager, "transaction-stage-fail", &output) ==
              PROS_APP_OK && output.install_state == PROS_APP_STAGED, 71);

        storage = (struct fake_storage){0};
        storage.publish_result = -1;
        manifest = make_manifest("transaction-publish-fail", "bin/app");
        CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 72);
        CHECK(pros_app_mark_verified(&manager, "transaction-publish-fail") ==
              PROS_APP_OK, 73);
        CHECK(pros_app_install_transaction(&manager, "transaction-publish-fail",
                                           &ops) == PROS_APP_EIO, 74);
        CHECK(storage.stage_calls == 1u && storage.publish_calls == 1u &&
              storage.rollback_calls == 1u, 75);
        CHECK(pros_app_get(&manager, "transaction-publish-fail", &output) ==
              PROS_APP_OK && output.install_state == PROS_APP_STAGED, 76);

        storage = (struct fake_storage){0};
        storage.publish_result = -1;
        storage.rollback_result = -1;
        manifest = make_manifest("transaction-rollback-fail", "bin/app");
        CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 77);
        CHECK(pros_app_mark_verified(&manager, "transaction-rollback-fail") ==
              PROS_APP_OK, 78);
        CHECK(pros_app_install_transaction(&manager, "transaction-rollback-fail",
                                           &ops) == PROS_APP_EROLLBACK, 79);
    }

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
    manifest = make_manifest("duplicate-dependency", "bin/app");
    manifest.dependency_count = 2u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "base");
    (void)snprintf(manifest.dependencies[1], sizeof(manifest.dependencies[1]),
                   "%s", "base");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_EINVAL, 58);
    manifest = make_manifest("self-dependency", "bin/app");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "self-dependency");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_EINVAL, 59);
    manifest = make_manifest("short-manifest", "bin/app");
    manifest.struct_size--;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_EABI, 18);

    manifest = make_manifest("untrusted", "bin/app");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 19);
    CHECK(pros_app_commit_install(&manager, "untrusted") == PROS_APP_ETRUST, 20);
    manifest = make_manifest("bad-trust-injection", "bin/app");
    manifest.trust_state = PROS_APP_TRUST_VERIFIED;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_ETRUST, 21);
    manifest = make_manifest("bad-state", "bin/app");
    manifest.install_state = PROS_APP_DISABLED;
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_ESTATE, 22);

    CHECK(pros_app_init(&manager, 0u) == PROS_APP_OK, 25);
    manifest = make_manifest("dependent", "bin/app");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "base");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 26);
    CHECK(pros_app_mark_verified(&manager, "dependent") == PROS_APP_OK, 43);
    CHECK(pros_app_commit_install(&manager, "dependent") == PROS_APP_ENODEP, 27);
    manifest = make_manifest("base", "bin/base");
    CHECK(register_installed(&manager, &manifest) == PROS_APP_OK, 28);
    CHECK(pros_app_commit_install(&manager, "dependent") == PROS_APP_OK, 29);
    CHECK(pros_app_preflight(&manager, "dependent") == PROS_APP_OK, 44);
    manifest = make_manifest("linux-dependent", "bin/app");
    manifest.backend = PROS_APP_BACKEND_LINUX;
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "base");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 30);
    CHECK(pros_app_mark_verified(&manager, "linux-dependent") == PROS_APP_OK, 45);
    CHECK(pros_app_commit_install(&manager, "linux-dependent") ==
          PROS_APP_EBACKEND, 31);
    CHECK(pros_app_uninstall(&manager, "linux-dependent") == PROS_APP_OK, 49);
    CHECK(pros_app_disable(&manager, "base") == PROS_APP_EBUSY, 56);
    CHECK(pros_app_disable(&manager, "dependent") == PROS_APP_OK, 50);
    CHECK(pros_app_preflight(&manager, "dependent") == PROS_APP_ESTATE, 51);
    CHECK(pros_app_uninstall(&manager, "base") == PROS_APP_EBUSY, 52);
    CHECK(pros_app_uninstall(&manager, "dependent") == PROS_APP_OK, 53);
    CHECK(pros_app_uninstall(&manager, "base") == PROS_APP_OK, 54);
    CHECK(pros_app_uninstall(&manager, "base") == PROS_APP_ENOENT, 55);

    CHECK(pros_app_init(&manager, 0u) == PROS_APP_OK, 32);
    manifest = make_manifest("cycle-a", "bin/a");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "cycle-b");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 33);
    CHECK(pros_app_mark_verified(&manager, "cycle-a") == PROS_APP_OK, 46);
    manifest = make_manifest("cycle-b", "bin/b");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "cycle-a");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_OK, 34);
    CHECK(pros_app_mark_verified(&manager, "cycle-b") == PROS_APP_OK, 47);
    CHECK(pros_app_commit_install(&manager, "cycle-b") == PROS_APP_ESTATE, 48);
    /* Simulate a corrupt persisted registry to exercise preflight cycle defense. */
    manager.entries[0].manifest.dependency_count = 1u;
    (void)snprintf(manager.entries[0].manifest.dependencies[0],
                   sizeof(manager.entries[0].manifest.dependencies[0]),
                   "%s", "cycle-b");
    manager.entries[0].manifest.install_state = PROS_APP_INSTALLED;
    manager.entries[1].manifest.install_state = PROS_APP_INSTALLED;
    CHECK(pros_app_preflight(&manager, "cycle-a") == PROS_APP_ELOOP, 35);

    CHECK(pros_app_init(&manager, 0u) == PROS_APP_OK, 36);
    for (uint32_t i = 0u; i < PROS_APP_MAX_ENTRIES; ++i) {
        char id[PROS_APP_ID_MAX];
        (void)snprintf(id, sizeof(id), "app-%u", i);
        manifest = make_manifest(id, "bin/app");
        CHECK(register_installed(&manager, &manifest) == PROS_APP_OK, 37);
    }
    manifest = make_manifest("registry-overflow", "bin/app");
    CHECK(pros_app_register(&manager, &manifest) == PROS_APP_ENOSPC, 38);
    CHECK(pros_app_count(&manager) == PROS_APP_MAX_ENTRIES, 39);

    (void)puts("application manager tests passed");
    return 0;
}

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

    /* Reject holes and hidden registrations in the public registry storage. */
    manager.entries[1].registered = 1u;
    CHECK(pros_dml_count(&manager) == 0u, 4);
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_EINVAL, 4);
    manager.entries[1].registered = 0u;
    manager.count = 1u;
    CHECK(pros_dml_select(&manager, "before-init") == PROS_DML_EINVAL, 4);
    manager.count = 0u;

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
        enum pros_dml_session_state state = PROS_DML_SESSION_FAILED;

        CHECK(pros_dml_get_state(&manager, "xfce", &state) == PROS_DML_OK,
              12);
        CHECK(state == PROS_DML_SESSION_STOPPED, 12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_RUNNING) == PROS_DML_ESTATE,
              12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_STARTING) == PROS_DML_OK,
              12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_RUNNING) == PROS_DML_OK,
              12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_STOPPING) == PROS_DML_OK,
              12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_STOPPED) == PROS_DML_OK,
              12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_STARTING) == PROS_DML_OK,
              12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_FAILED) == PROS_DML_OK,
              12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_RUNNING) == PROS_DML_ESTATE,
              12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_STOPPING) == PROS_DML_OK,
              12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_FAILED) == PROS_DML_OK,
              12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  (enum pros_dml_session_state)99) ==
              PROS_DML_EINVAL, 12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  (enum pros_dml_session_state)-1) ==
              PROS_DML_EINVAL, 12);
        CHECK(pros_dml_transition(&manager, "xfce",
                                  PROS_DML_SESSION_STOPPED) == PROS_DML_OK,
              12);
        CHECK(pros_dml_get_state(&manager, "not-installed", &state) ==
              PROS_DML_ENOENT, 12);

        /* Corrupt public storage must not be surfaced as a valid enum value. */
        manager.entries[0].session_state = UINT32_MAX;
        CHECK(pros_dml_get_state(&manager, "xfce", &state) == PROS_DML_ESTATE,
              12);
        manager.entries[0].session_state = PROS_DML_SESSION_STOPPED;
    }

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

    {
        uint32_t order[PROS_DML_MAX_DESKTOPS];
        uint32_t order_count = 0u;
        uint32_t sentinel = 0xA5A5A5A5u;

        CHECK(pros_dml_resolve_order(&manager, "plasma", order,
                                     PROS_DML_MAX_DESKTOPS,
                                     &order_count) == PROS_DML_OK, 22);
        CHECK(order_count == 2u, 22);
        CHECK(strcmp(manager.entries[order[0]].manifest.id, "graphics") == 0,
              22);
        CHECK(strcmp(manager.entries[order[1]].manifest.id, "plasma") == 0,
              22);
        order[0] = sentinel;
        order_count = 0u;
        CHECK(pros_dml_resolve_order(&manager, "plasma", order, 1u,
                                     &order_count) == PROS_DML_ENOSPC, 22);
        CHECK(order_count == 2u && order[0] == sentinel, 22);
        CHECK(pros_dml_resolve_order(&manager, "missing", order,
                                     PROS_DML_MAX_DESKTOPS,
                                     &order_count) == PROS_DML_ENOENT, 22);
        CHECK(order_count == 0u, 22);
    }

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
    {
        uint32_t order[PROS_DML_MAX_DESKTOPS];
        uint32_t order_count = 99u;

        CHECK(pros_dml_resolve_order(&manager, "cycle-a", order,
                                     PROS_DML_MAX_DESKTOPS,
                                     &order_count) == PROS_DML_ELOOP, 26);
        CHECK(order_count == 0u, 26);
    }

    /* Shared dependencies are emitted once in dependency-first order. */
    CHECK(pros_dml_init(&manager, 0u) == PROS_DML_OK, 26);
    manifest = manifest_make("session", "bin/session");
    manifest.dependency_count = 2u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "base");
    (void)snprintf(manifest.dependencies[1], sizeof(manifest.dependencies[1]),
                   "%s", "graphics");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 26);
    manifest = manifest_make("graphics", "bin/graphics");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "base");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 26);
    manifest = manifest_make("base", "bin/base");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 26);
    {
        uint32_t order[PROS_DML_MAX_DESKTOPS];
        uint32_t order_count = 0u;

        CHECK(pros_dml_resolve_order(&manager, "session", order,
                                     PROS_DML_MAX_DESKTOPS,
                                     &order_count) == PROS_DML_OK, 26);
        CHECK(order_count == 3u, 26);
        CHECK(strcmp(manager.entries[order[0]].manifest.id, "base") == 0, 26);
        CHECK(strcmp(manager.entries[order[1]].manifest.id, "graphics") == 0,
              26);
        CHECK(strcmp(manager.entries[order[2]].manifest.id, "session") == 0,
              26);
    }
    /* Public manager storage can be corrupted; reject unsafe dependency counts. */
    manager.entries[0].manifest.dependency_count =
        PROS_DML_MAX_DEPENDENCIES + 1u;
    {
        uint32_t order[PROS_DML_MAX_DESKTOPS];
        uint32_t order_count = 0u;

        CHECK(pros_dml_preflight(&manager, "session") == PROS_DML_EINVAL, 26);
        CHECK(pros_dml_resolve_order(&manager, "session", order,
                                     PROS_DML_MAX_DESKTOPS,
                                     &order_count) == PROS_DML_EINVAL, 26);
        CHECK(order_count == 0u, 26);
    }

    /* Corruption in a transitive dependency must also stop traversal safely. */
    CHECK(pros_dml_init(&manager, 0u) == PROS_DML_OK, 26);
    manifest = manifest_make("session", "bin/session");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "graphics");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 26);
    manifest = manifest_make("graphics", "bin/graphics");
    manifest.dependency_count = 1u;
    (void)snprintf(manifest.dependencies[0], sizeof(manifest.dependencies[0]),
                   "%s", "base");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 26);
    manifest = manifest_make("base", "bin/base");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 26);
    manager.entries[1].manifest.dependency_count =
        PROS_DML_MAX_DEPENDENCIES + 1u;
    {
        uint32_t order[PROS_DML_MAX_DESKTOPS];
        uint32_t order_count = 0u;

        CHECK(pros_dml_preflight(&manager, "session") == PROS_DML_EINVAL, 26);
        CHECK(pros_dml_resolve_order(&manager, "session", order,
                                     PROS_DML_MAX_DESKTOPS,
                                     &order_count) == PROS_DML_EINVAL, 26);
        CHECK(order_count == 0u, 26);
    }

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

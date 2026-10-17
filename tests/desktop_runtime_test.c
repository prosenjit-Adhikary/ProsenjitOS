#include "dml_runtime.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, code) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return (code); \
    } \
} while (0)

struct mock_backend {
    char starts[16][PROS_DML_ID_MAX];
    char stops[16][PROS_DML_ID_MAX];
    uint32_t start_count;
    uint32_t stop_count;
    uint64_t next_handle;
    const char *fail_start_id;
    const char *fail_stop_id;
    int fail_start_once;
    int fail_stop_once;
};

static int mock_start(const struct pros_dml_manifest *manifest,
                      uint64_t *handle, void *opaque)
{
    struct mock_backend *mock = opaque;

    if (!manifest || !handle || !mock ||
        mock->start_count >= 16u)
        return -77;
    (void)snprintf(mock->starts[mock->start_count],
                   sizeof(mock->starts[mock->start_count]), "%s",
                   manifest->id);
    ++mock->start_count;
    if (mock->fail_start_once && mock->fail_start_id &&
        strcmp(mock->fail_start_id, manifest->id) == 0) {
        mock->fail_start_once = 0;
        return -77;
    }
    *handle = ++mock->next_handle;
    return 0;
}

static int mock_stop(const struct pros_dml_manifest *manifest,
                     uint64_t handle, void *opaque)
{
    struct mock_backend *mock = opaque;

    (void)handle;
    if (!manifest || !mock || mock->stop_count >= 16u)
        return -77;
    (void)snprintf(mock->stops[mock->stop_count],
                   sizeof(mock->stops[mock->stop_count]), "%s",
                   manifest->id);
    ++mock->stop_count;
    if (mock->fail_stop_once && mock->fail_stop_id &&
        strcmp(mock->fail_stop_id, manifest->id) == 0) {
        mock->fail_stop_once = 0;
        return -77;
    }
    return 0;
}

static struct pros_dml_manifest make_manifest(const char *id,
                                               const char *dependency)
{
    struct pros_dml_manifest manifest = {0};

    manifest.struct_size = (uint32_t)sizeof(manifest);
    manifest.abi_version = PROS_DML_ABI_VERSION;
    (void)snprintf(manifest.id, sizeof(manifest.id), "%s", id);
    (void)snprintf(manifest.entry_point, sizeof(manifest.entry_point),
                   "bin/%s", id);
    if (dependency) {
        manifest.dependency_count = 1u;
        (void)snprintf(manifest.dependencies[0],
                       sizeof(manifest.dependencies[0]), "%s", dependency);
    }
    return manifest;
}

int main(void)
{
    struct pros_dml_manager manager;
    struct pros_dml_runtime runtime;
    struct pros_dml_runtime_ops ops = { mock_start, mock_stop };
    struct mock_backend mock = {0};
    struct pros_dml_manifest manifest;
    const char *active = 0;
    enum pros_dml_session_state state;

    CHECK(pros_dml_init(&manager, 0u) == PROS_DML_OK, 1);
    manifest = make_manifest("session", "graphics");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 2);
    manifest = make_manifest("graphics", "base");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 3);
    manifest = make_manifest("base", 0);
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 4);
    CHECK(pros_dml_runtime_init(&runtime, &manager, &ops, &mock) ==
          PROS_DML_OK, 5);

    /* A failed middle dependency must roll back already-started prerequisites. */
    mock.fail_start_id = "graphics";
    mock.fail_start_once = 1;
    CHECK(pros_dml_runtime_start(&runtime, "session") == -77, 6);
    CHECK(mock.start_count == 2u, 6);
    CHECK(strcmp(mock.starts[0], "base") == 0, 6);
    CHECK(strcmp(mock.starts[1], "graphics") == 0, 6);
    CHECK(mock.stop_count == 1u &&
          strcmp(mock.stops[0], "base") == 0, 6);
    CHECK(pros_dml_get_state(&manager, "base", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_STOPPED, 6);
    CHECK(pros_dml_get_state(&manager, "graphics", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_FAILED, 6);
    CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_ENOENT, 6);

    /* A cleanly rolled-back launch can be retried. */
    mock.fail_start_id = 0;
    CHECK(pros_dml_runtime_start(&runtime, "session") == PROS_DML_OK, 7);
    CHECK(mock.start_count == 5u, 7);
    CHECK(strcmp(mock.starts[2], "base") == 0, 7);
    CHECK(strcmp(mock.starts[3], "graphics") == 0, 7);
    CHECK(strcmp(mock.starts[4], "session") == 0, 7);
    CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_OK &&
          strcmp(active, "session") == 0, 7);

    /* Failed shutdown keeps the remaining session tracked for a safe retry. */
    mock.fail_stop_id = "session";
    mock.fail_stop_once = 1;
    CHECK(pros_dml_runtime_stop(&runtime) == -77, 8);
    CHECK(pros_dml_get_state(&manager, "session", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_FAILED, 8);
    CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_ESTATE, 8);
    CHECK(pros_dml_runtime_stop(&runtime) == PROS_DML_OK, 9);
    CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_ENOENT, 9);
    CHECK(pros_dml_get_state(&manager, "session", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_STOPPED, 9);
    CHECK(pros_dml_get_state(&manager, "graphics", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_STOPPED, 9);
    CHECK(pros_dml_get_state(&manager, "base", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_STOPPED, 9);
    CHECK(mock.stop_count == 5u, 9);
    CHECK(strcmp(mock.stops[1], "session") == 0, 9);
    CHECK(strcmp(mock.stops[2], "session") == 0, 9);
    CHECK(strcmp(mock.stops[3], "graphics") == 0, 9);
    CHECK(strcmp(mock.stops[4], "base") == 0, 9);

    (void)puts("desktop runtime tests passed");
    return 0;
}

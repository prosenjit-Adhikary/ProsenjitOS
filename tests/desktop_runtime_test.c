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
    char starts[32][PROS_DML_ID_MAX];
    char stops[32][PROS_DML_ID_MAX];
    uint32_t start_count;
    uint32_t stop_count;
    uint64_t next_handle;
    const char *fail_start_id;
    const char *fail_stop_id;
    int fail_start_once;
    int fail_stop_once;
    int return_zero_handle_once;
    int publish_handle_on_failed_start;
    struct pros_dml_runtime *reentry_runtime;
    int reenter_start_once;
    int reenter_stop_once;
    int reentry_result;
};

static int mock_start(const struct pros_dml_manifest *manifest,
                      uint64_t *handle, void *opaque)
{
    struct mock_backend *mock = opaque;

    if (!manifest || !handle || !mock ||
        mock->start_count >= 32u)
        return -77;
    (void)snprintf(mock->starts[mock->start_count],
                   sizeof(mock->starts[mock->start_count]), "%s",
                   manifest->id);
    ++mock->start_count;
    if (mock->reenter_start_once && mock->reentry_runtime) {
        mock->reenter_start_once = 0;
        mock->reentry_result =
            pros_dml_runtime_stop(mock->reentry_runtime);
    }
    if (mock->fail_start_once && mock->fail_start_id &&
        strcmp(mock->fail_start_id, manifest->id) == 0) {
        mock->fail_start_once = 0;
        if (mock->publish_handle_on_failed_start) {
            mock->publish_handle_on_failed_start = 0;
            *handle = ++mock->next_handle;
        }
        return -77;
    }
    if (mock->return_zero_handle_once) {
        mock->return_zero_handle_once = 0;
        return 0;
    }
    *handle = ++mock->next_handle;
    return 0;
}

static int mock_stop(const struct pros_dml_manifest *manifest,
                     uint64_t handle, void *opaque)
{
    struct mock_backend *mock = opaque;

    (void)handle;
    if (!manifest || !mock || mock->stop_count >= 32u)
        return -77;
    (void)snprintf(mock->stops[mock->stop_count],
                   sizeof(mock->stops[mock->stop_count]), "%s",
                   manifest->id);
    ++mock->stop_count;
    if (mock->reenter_stop_once && mock->reentry_runtime) {
        mock->reenter_stop_once = 0;
        mock->reentry_result =
            pros_dml_runtime_start(mock->reentry_runtime, "session");
    }
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
    {
        struct pros_dml_manager malformed_manager = manager;
        struct pros_dml_runtime malformed_runtime;

        /* A claimed registry slot must actually be registered. */
        malformed_manager.count = 1u;
        CHECK(pros_dml_runtime_init(&malformed_runtime, &malformed_manager,
                                    &ops, &mock) == PROS_DML_EINVAL, 1);

        /* No registration may exist beyond the published count. */
        malformed_manager = manager;
        malformed_manager.entries[PROS_DML_MAX_DESKTOPS - 1u].registered = 1u;
        CHECK(pros_dml_runtime_init(&malformed_runtime, &malformed_manager,
                                    &ops, &mock) == PROS_DML_EINVAL, 1);
    }
    manifest = make_manifest("session", "graphics");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 2);
    manifest = make_manifest("graphics", "base");
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 3);
    manifest = make_manifest("base", 0);
    CHECK(pros_dml_register(&manager, &manifest) == PROS_DML_OK, 4);
    manifest = make_manifest("unused", 0);
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
    {
        struct pros_dml_manager saved_manager = manager;

        /* A manager mutated after runtime init must not be acted upon. */
        manager.count--;
        CHECK(pros_dml_runtime_stop(&runtime) == PROS_DML_EINVAL, 7);
        manager = saved_manager;
    }
    {
        struct pros_dml_runtime corrupt_runtime = runtime;
        corrupt_runtime.active_root = 3u; /* Registered, but not in this session. */
        CHECK(pros_dml_runtime_active(&corrupt_runtime, &active) ==
              PROS_DML_EINVAL, 7);
    }
    {
        struct pros_dml_runtime corrupt_runtime = runtime;
        uint32_t inactive = PROS_DML_MAX_DESKTOPS - 1u;
        uint32_t stops_before = mock.stop_count;
        corrupt_runtime.process_handles[inactive] = 0xBADu;
        CHECK(pros_dml_runtime_stop(&corrupt_runtime) == PROS_DML_EINVAL, 7);
        CHECK(mock.stop_count == stops_before, 7);
    }
    {
        struct pros_dml_runtime corrupt_runtime = runtime;
        corrupt_runtime.active_order[0] = PROS_DML_MAX_DESKTOPS;
        CHECK(pros_dml_runtime_stop(&corrupt_runtime) == PROS_DML_EINVAL, 7);
    }
    {
        struct pros_dml_runtime corrupt_runtime = runtime;
        uint32_t stops_before = mock.stop_count;
        corrupt_runtime.process_handles[corrupt_runtime.active_order[0]] =
            PROS_DML_INVALID_PROCESS_HANDLE;
        CHECK(pros_dml_runtime_stop(&corrupt_runtime) == PROS_DML_EINVAL, 7);
        CHECK(mock.stop_count == stops_before, 7);
    }
    {
        struct pros_dml_runtime second_runtime;
        CHECK(pros_dml_runtime_init(&second_runtime, &manager, &ops, &mock) ==
              PROS_DML_ESTATE, 7);
    }

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

    /* A rollback-stop failure must retain the live prerequisite for retry. */
    mock.fail_start_id = "graphics";
    mock.fail_start_once = 1;
    mock.fail_stop_id = "base";
    mock.fail_stop_once = 1;
    CHECK(pros_dml_runtime_start(&runtime, "session") == PROS_DML_EROLLBACK, 10);
    CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_ESTATE, 10);
    CHECK(pros_dml_get_state(&manager, "base", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_FAILED, 10);
    CHECK(pros_dml_get_state(&manager, "graphics", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_FAILED, 10);

    /* Cleanup retries the retained handle; a subsequent full launch is safe. */
    CHECK(pros_dml_runtime_stop(&runtime) == PROS_DML_OK, 11);
    CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_ENOENT, 11);
    mock.fail_start_id = 0;
    mock.fail_stop_id = 0;
    CHECK(pros_dml_runtime_start(&runtime, "session") == PROS_DML_OK, 12);
    CHECK(pros_dml_runtime_stop(&runtime) == PROS_DML_OK, 12);
    CHECK(pros_dml_get_state(&manager, "base", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_STOPPED, 12);
    CHECK(pros_dml_get_state(&manager, "graphics", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_STOPPED, 12);
    CHECK(pros_dml_get_state(&manager, "session", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_STOPPED, 12);

    /* A backend must not report success without publishing a valid handle. */
    mock.return_zero_handle_once = 1;
    CHECK(pros_dml_runtime_start(&runtime, "session") == PROS_DML_EBACKEND, 13);
    CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_ENOENT, 13);
    CHECK(pros_dml_get_state(&manager, "base", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_FAILED, 13);
    CHECK(pros_dml_runtime_start(&runtime, "session") == PROS_DML_OK, 14);
    CHECK(pros_dml_runtime_stop(&runtime) == PROS_DML_OK, 14);
    {
        struct pros_dml_runtime corrupt_runtime = runtime;
        corrupt_runtime.active_root = 0u; /* Root must clear with the last handle. */
        CHECK(pros_dml_runtime_active(&corrupt_runtime, &active) ==
              PROS_DML_EINVAL, 14);
    }
    CHECK(pros_dml_get_state(&manager, "base", &state) == PROS_DML_OK &&
          state == PROS_DML_SESSION_STOPPED, 14);

    /*
     * If a backend publishes a child handle and then reports a later launch
     * failure, the runtime must not orphan that child. Roll back the partial
     * child first, then its already-started prerequisites.
     */
    {
        uint32_t stops_before = mock.stop_count;
        mock.fail_start_id = "graphics";
        mock.fail_start_once = 1;
        mock.publish_handle_on_failed_start = 1;
        CHECK(pros_dml_runtime_start(&runtime, "session") == -77, 16);
        CHECK(mock.stop_count == stops_before + 2u, 16);
        CHECK(strcmp(mock.stops[stops_before], "graphics") == 0, 16);
        CHECK(strcmp(mock.stops[stops_before + 1u], "base") == 0, 16);
        CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_ENOENT, 16);
        CHECK(pros_dml_get_state(&manager, "graphics", &state) == PROS_DML_OK &&
              state == PROS_DML_SESSION_STOPPED, 16);
        CHECK(pros_dml_get_state(&manager, "base", &state) == PROS_DML_OK &&
              state == PROS_DML_SESSION_STOPPED, 16);
        mock.fail_start_id = 0;
    }

    /*
     * If cleanup of a partially launched child itself fails, retain that
     * handle and every still-live prerequisite. A later stop must retry the
     * failed child first, then clean up the prerequisite.
     */
    {
        uint32_t stops_before = mock.stop_count;
        mock.fail_start_id = "graphics";
        mock.fail_start_once = 1;
        mock.publish_handle_on_failed_start = 1;
        mock.fail_stop_id = "graphics";
        mock.fail_stop_once = 1;
        CHECK(pros_dml_runtime_start(&runtime, "session") ==
              PROS_DML_EROLLBACK, 17);
        CHECK(mock.stop_count == stops_before + 1u, 17);
        CHECK(strcmp(mock.stops[stops_before], "graphics") == 0, 17);
        CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_ESTATE, 17);
        CHECK(pros_dml_get_state(&manager, "graphics", &state) == PROS_DML_OK &&
              state == PROS_DML_SESSION_FAILED, 17);
        CHECK(pros_dml_runtime_stop(&runtime) == PROS_DML_OK, 17);
        CHECK(mock.stop_count == stops_before + 3u, 17);
        CHECK(strcmp(mock.stops[stops_before + 1u], "graphics") == 0, 17);
        CHECK(strcmp(mock.stops[stops_before + 2u], "base") == 0, 17);
        CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_ENOENT, 17);
        CHECK(pros_dml_get_state(&manager, "graphics", &state) == PROS_DML_OK &&
              state == PROS_DML_SESSION_STOPPED, 17);
        CHECK(pros_dml_get_state(&manager, "base", &state) == PROS_DML_OK &&
              state == PROS_DML_SESSION_STOPPED, 17);
        mock.fail_start_id = 0;
        mock.fail_stop_id = 0;
    }

    /*
     * The backend contract is synchronous and non-reentrant. Verify a backend
     * that accidentally calls back into the orchestrator cannot corrupt the
     * active order or reset the runtime while a callback is in flight.
     */
    mock.reentry_runtime = &runtime;
    mock.reenter_start_once = 1;
    CHECK(pros_dml_runtime_start(&runtime, "session") == PROS_DML_OK, 15);
    CHECK(mock.reentry_result == PROS_DML_ESTATE, 15);
    mock.reenter_stop_once = 1;
    CHECK(pros_dml_runtime_stop(&runtime) == PROS_DML_OK, 15);
    CHECK(mock.reentry_result == PROS_DML_ESTATE, 15);
    CHECK(pros_dml_runtime_active(&runtime, &active) == PROS_DML_ENOENT, 15);

    (void)puts("desktop runtime tests passed");
    return 0;
}

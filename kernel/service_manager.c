#include "service_manager.h"

static struct pros_service services[PROS_SERVICE_MAX];
static uint32_t service_count;

static int service_name_valid(const char *name)
{
    if (!name || name[0] == '\0')
        return 0;
    for (uint32_t i = 0; i < 128u; ++i) {
        if (name[i] == '\0')
            return 1;
        if (name[i] == '/' || name[i] == ' ')
            return 0;
    }
    return 0;
}

static int service_name_equal(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (*a != *b)
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static int service_find(const char *name)
{
    if (!name)
        return -1;
    for (uint32_t i = 0; i < service_count; ++i)
        if (services[i].registered &&
            services[i].definition.name &&
            service_name_equal(services[i].definition.name, name))
            return (int)i;
    return -1;
}

static int service_start_index(uint32_t index, uint8_t *visiting)
{
    struct pros_service *service;
    if (index >= service_count || !visiting)
        return PROS_SERVICE_EINVAL;
    service = &services[index];

    if (service->state == PROS_SERVICE_RUNNING)
        return PROS_SERVICE_OK;
    if (service->state == PROS_SERVICE_STARTING)
        return PROS_SERVICE_ELOOP;
    if (visiting[index])
        return PROS_SERVICE_ELOOP;
    if (!service->definition.start)
        return PROS_SERVICE_EINVAL;

    visiting[index] = 1u;
    service->state = PROS_SERVICE_STARTING;

    for (uint32_t d = 0; d < service->definition.dependency_count; ++d) {
        int dep = service_find(service->definition.dependencies[d]);
        int rc;
        if (dep < 0) {
            service->state = PROS_SERVICE_FAILED;
            visiting[index] = 0u;
            return PROS_SERVICE_ENOENT;
        }
        rc = service_start_index((uint32_t)dep, visiting);
        if (rc != PROS_SERVICE_OK) {
            service->state = PROS_SERVICE_FAILED;
            visiting[index] = 0u;
            return rc;
        }
    }

    {
        uint32_t limit = service->definition.max_retries;
        if (limit > PROS_SERVICE_MAX_RETRIES)
            limit = PROS_SERVICE_MAX_RETRIES;

        for (uint32_t attempt = 0; attempt <= limit; ++attempt) {
            int rc = service->definition.start(service->definition.context);
            if (rc == 0) {
                service->state = PROS_SERVICE_RUNNING;
                visiting[index] = 0u;
                return PROS_SERVICE_OK;
            }
            if (service->restart_count != UINT32_MAX)
                ++service->restart_count;
            if (attempt == limit)
                break;
        }
    }

    service->state = PROS_SERVICE_FAILED;
    visiting[index] = 0u;
    return PROS_SERVICE_EFAILED;
}

static int service_stop_index(uint32_t index, uint8_t *visiting)
{
    if (index >= service_count || !visiting)
        return PROS_SERVICE_EINVAL;
    if (services[index].state == PROS_SERVICE_STOPPED ||
        services[index].state == PROS_SERVICE_DEFINED)
        return PROS_SERVICE_OK;
    if (visiting[index])
        return PROS_SERVICE_ELOOP;
    if (!services[index].definition.stop)
        return PROS_SERVICE_EINVAL;

    visiting[index] = 1u;
    services[index].state = PROS_SERVICE_STOPPING;

    /* Stop dependants first, preserving dependency shutdown ordering. */
    for (uint32_t i = 0; i < service_count; ++i) {
        if (!services[i].registered)
            continue;
        for (uint32_t d = 0; d < services[i].definition.dependency_count; ++d) {
            const char *dep = services[i].definition.dependencies[d];
            if (dep && service_name_equal(dep, services[index].definition.name)) {
                int rc = service_stop_index(i, visiting);
                if (rc != PROS_SERVICE_OK) {
                    services[index].state = PROS_SERVICE_FAILED;
                    visiting[index] = 0u;
                    return rc;
                }
            }
        }
    }

    if (services[index].definition.stop(services[index].definition.context) != 0) {
        services[index].state = PROS_SERVICE_FAILED;
        visiting[index] = 0u;
        return PROS_SERVICE_EFAILED;
    }

    services[index].state = PROS_SERVICE_STOPPED;
    visiting[index] = 0u;
    return PROS_SERVICE_OK;
}

int service_manager_init(void)
{
    for (uint32_t i = 0; i < PROS_SERVICE_MAX; ++i)
        services[i] = (struct pros_service){0};
    service_count = 0;
    return PROS_SERVICE_OK;
}

int service_register(const struct pros_service_definition *definition)
{
    if (!definition || !service_name_valid(definition->name) ||
        !definition->start || !definition->stop ||
        definition->dependency_count > PROS_SERVICE_MAX_DEPS)
        return PROS_SERVICE_EINVAL;
    if (service_find(definition->name) >= 0)
        return PROS_SERVICE_EBUSY;
    for (uint32_t d = 0; d < definition->dependency_count; ++d) {
        const char *dependency = definition->dependencies[d];
        if (!service_name_valid(dependency) ||
            service_name_equal(dependency, definition->name))
            return PROS_SERVICE_EINVAL;
    }
    if (service_count >= PROS_SERVICE_MAX)
        return PROS_SERVICE_ENOSPC;

    services[service_count].definition = *definition;
    services[service_count].state = PROS_SERVICE_DEFINED;
    services[service_count].restart_count = 0;
    services[service_count].registered = 1u;
    ++service_count;
    return PROS_SERVICE_OK;
}

int service_start(const char *name)
{
    int index = service_find(name);
    uint8_t visiting[PROS_SERVICE_MAX] = {0};
    if (index < 0)
        return PROS_SERVICE_ENOENT;
    return service_start_index((uint32_t)index, visiting);
}

int service_stop(const char *name)
{
    int index = service_find(name);
    uint8_t visiting[PROS_SERVICE_MAX] = {0};
    if (index < 0)
        return PROS_SERVICE_ENOENT;
    return service_stop_index((uint32_t)index, visiting);
}

int service_restart(const char *name)
{
    int rc = service_stop(name);
    if (rc != PROS_SERVICE_OK && rc != PROS_SERVICE_EINVAL)
        return rc;
    return service_start(name);
}

int service_start_all(void)
{
    for (uint32_t i = 0; i < service_count; ++i) {
        int rc = service_start(services[i].definition.name);
        if (rc != PROS_SERVICE_OK)
            return rc;
    }
    return PROS_SERVICE_OK;
}

int service_stop_all(void)
{
    for (uint32_t i = service_count; i > 0; --i) {
        int rc = service_stop(services[i - 1u].definition.name);
        if (rc != PROS_SERVICE_OK)
            return rc;
    }
    return PROS_SERVICE_OK;
}

int service_status(const char *name, enum pros_service_state *state,
                   uint32_t *restart_count)
{
    int index = service_find(name);
    if (index < 0 || !state || !restart_count)
        return PROS_SERVICE_EINVAL;
    *state = services[index].state;
    *restart_count = services[index].restart_count;
    return PROS_SERVICE_OK;
}

static int sm_test_start(void *context)
{
    uint32_t *value = (uint32_t *)context;
    if (!value)
        return -1;
    ++*value;
    return 0;
}

static int sm_test_fail(void *context)
{
    (void)context;
    return -1;
}

static int sm_test_stop(void *context)
{
    uint32_t *value = (uint32_t *)context;
    if (!value)
        return -1;
    if (*value == 0)
        return -1;
    --*value;
    return 0;
}

int service_manager_selftest(void)
{
    uint32_t a = 0, b = 0;
    enum pros_service_state state;
    uint32_t retries;

    if (service_manager_init() != PROS_SERVICE_OK)
        return -1;

    {
        const struct pros_service_definition bad_self = {
            .name = "bad-self", .start = sm_test_start, .stop = sm_test_stop,
            .context = &a, .dependency_count = 1,
            .dependencies = {"bad-self"}, .max_retries = 0
        };
        const struct pros_service_definition bad_name = {
            .name = "bad/name", .start = sm_test_start, .stop = sm_test_stop,
            .context = &a, .dependency_count = 0, .max_retries = 0
        };
        if (service_register(&bad_self) != PROS_SERVICE_EINVAL ||
            service_register(&bad_name) != PROS_SERVICE_EINVAL)
            return -14;
    }

    {
        const struct pros_service_definition base = {
            .name = "base", .start = sm_test_start, .stop = sm_test_stop,
            .context = &a, .dependency_count = 0, .max_retries = 0
        };
        const struct pros_service_definition child = {
            .name = "child", .start = sm_test_start, .stop = sm_test_stop,
            .context = &b, .dependency_count = 1,
            .dependencies = {"base"}, .max_retries = 1
        };
        if (service_register(&base) != PROS_SERVICE_OK ||
            service_register(&child) != PROS_SERVICE_OK ||
            service_register(&base) != PROS_SERVICE_EBUSY)
            return -2;
    }

    if (service_start("child") != PROS_SERVICE_OK || a != 1 || b != 1)
        return -3;
    if (service_status("child", &state, &retries) != PROS_SERVICE_OK ||
        state != PROS_SERVICE_RUNNING || retries != 0)
        return -4;
    if (service_stop("base") != PROS_SERVICE_OK || a != 0 || b != 0)
        return -5;
    if (service_start("missing") != PROS_SERVICE_ENOENT)
        return -6;
    if (service_restart("child") != PROS_SERVICE_OK ||
        service_status("child", &state, &retries) != PROS_SERVICE_OK ||
        state != PROS_SERVICE_RUNNING || a != 1u || b != 1u)
        return -7;

    if (service_manager_init() != PROS_SERVICE_OK)
        return -8;
    {
        const struct pros_service_definition loop_a = {
            .name = "loop-a", .start = sm_test_start, .stop = sm_test_stop,
            .context = &a, .dependency_count = 1,
            .dependencies = {"loop-b"}, .max_retries = 0
        };
        const struct pros_service_definition loop_b = {
            .name = "loop-b", .start = sm_test_start, .stop = sm_test_stop,
            .context = &b, .dependency_count = 1,
            .dependencies = {"loop-a"}, .max_retries = 0
        };
        if (service_register(&loop_a) != PROS_SERVICE_OK ||
            service_register(&loop_b) != PROS_SERVICE_OK ||
            service_start("loop-a") != PROS_SERVICE_ELOOP)
            return -9;
    }

    /* Missing dependencies must fail closed rather than partially starting. */
    if (service_manager_init() != PROS_SERVICE_OK)
        return -10;
    {
        const struct pros_service_definition missing_dep = {
            .name = "missing-dep", .start = sm_test_start, .stop = sm_test_stop,
            .context = &a, .dependency_count = 1,
            .dependencies = {"does-not-exist"}, .max_retries = 0
        };
        if (service_register(&missing_dep) != PROS_SERVICE_OK ||
            service_start("missing-dep") != PROS_SERVICE_ENOENT ||
            service_status("missing-dep", &state, &retries) != PROS_SERVICE_OK ||
            state != PROS_SERVICE_FAILED || a != 0)
            return -11;
    }

    /* Retry exhaustion is bounded: max_retries=2 means exactly three starts. */
    if (service_manager_init() != PROS_SERVICE_OK)
        return -12;
    {
        const struct pros_service_definition failing = {
            .name = "failing", .start = sm_test_fail, .stop = sm_test_stop,
            .context = &a, .dependency_count = 0, .max_retries = 2
        };
        if (service_register(&failing) != PROS_SERVICE_OK ||
            service_start("failing") != PROS_SERVICE_EFAILED ||
            service_status("failing", &state, &retries) != PROS_SERVICE_OK ||
            state != PROS_SERVICE_FAILED || retries != 3u)
            return -13;
    }
    return PROS_SERVICE_OK;
}

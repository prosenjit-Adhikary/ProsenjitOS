#include "driver.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    static const uint32_t registry_codes[] = {
        PROS_DRIVER_HEALTH_REGISTER_FAILURE,
        PROS_DRIVER_HEALTH_REGISTRY_EXHAUSTED,
        PROS_DRIVER_HEALTH_DEVICE_REGISTER_FAILURE,
        PROS_DRIVER_HEALTH_UNSUPPORTED_CLASS,
        PROS_DRIVER_HEALTH_REMOVE_FAILURE
    };
    static const uint32_t lifecycle_codes[] = {
        PROS_DRIVER_HEALTH_PROBE_FAILURE,
        PROS_DRIVER_HEALTH_BIND_SUCCESS,
        PROS_DRIVER_HEALTH_UNBIND_SUCCESS
    };

    for (size_t i = 0; i < sizeof(registry_codes) / sizeof(registry_codes[0]); ++i) {
        for (size_t j = 0; j < sizeof(lifecycle_codes) / sizeof(lifecycle_codes[0]); ++j)
            assert(registry_codes[i] != lifecycle_codes[j]);
    }
    for (size_t i = 0; i < sizeof(lifecycle_codes) / sizeof(lifecycle_codes[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(lifecycle_codes) / sizeof(lifecycle_codes[0]); ++j)
            assert(lifecycle_codes[i] != lifecycle_codes[j]);
    }

    assert(PROS_DRIVER_HEALTH_PROBE_FAILURE == 0x100u);
    assert(PROS_DRIVER_HEALTH_BIND_SUCCESS == 0x101u);
    assert(PROS_DRIVER_HEALTH_UNBIND_SUCCESS == 0x102u);
    puts("driver telemetry code namespace tests: PASS");
    return 0;
}

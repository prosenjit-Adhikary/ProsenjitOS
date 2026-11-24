#ifndef PROSENJIT_SECURITY_PROVIDER_H
#define PROSENJIT_SECURITY_PROVIDER_H

#include <stdint.h>
#include "security_file.h"

#define PROS_SECURITY_MAX_PROVIDERS 4u

enum pros_security_provider_status {
    PROS_SECURITY_PROVIDER_OK = 0,
    PROS_SECURITY_PROVIDER_UNAVAILABLE = 1,
    PROS_SECURITY_PROVIDER_ERROR = 2,
};

typedef enum pros_security_provider_status
(*pros_security_provider_query_fn)(
    const uint8_t digest[PROS_FILE_SHA256_BYTES],
    enum pros_file_reputation *reputation);

struct pros_security_provider {
    const char *name;
    pros_security_provider_query_fn query;
    uint32_t enabled;
};

int pros_security_provider_register(const struct pros_security_provider *provider);
int pros_security_provider_set_enabled(uint32_t index, uint32_t enabled);

enum pros_file_reputation pros_security_provider_reputation(
    const uint8_t digest[PROS_FILE_SHA256_BYTES]);

uint32_t pros_security_provider_count(void);
int pros_security_provider_selftest(void);

#endif

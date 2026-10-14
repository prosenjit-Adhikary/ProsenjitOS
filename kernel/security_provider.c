#include "security_provider.h"
#include "security_admission.h"
#include "health.h"

#include <stdint.h>

static struct pros_security_provider providers[PROS_SECURITY_MAX_PROVIDERS];
static uint32_t provider_count;

static enum pros_file_reputation merge_reputation(
    enum pros_file_reputation current,
    enum pros_file_reputation candidate) {
    if (candidate == PROS_FILE_REPUTATION_MALICIOUS ||
        current == PROS_FILE_REPUTATION_MALICIOUS)
        return PROS_FILE_REPUTATION_MALICIOUS;
    if (candidate == PROS_FILE_REPUTATION_SUSPICIOUS ||
        current == PROS_FILE_REPUTATION_SUSPICIOUS)
        return PROS_FILE_REPUTATION_SUSPICIOUS;
    if (candidate == PROS_FILE_REPUTATION_CLEAN ||
        current == PROS_FILE_REPUTATION_CLEAN)
        return PROS_FILE_REPUTATION_CLEAN;
    return PROS_FILE_REPUTATION_UNAVAILABLE;
}

int pros_security_provider_register(
    const struct pros_security_provider *provider) {
    if (!provider || !provider->name || !provider->query ||
        provider->enabled > 1u ||
        provider_count >= PROS_SECURITY_MAX_PROVIDERS) {
        (void)pros_health_record_security(0x53454350u,
                                          PROS_SECURITY_HEALTH_PROVIDER, 0u);
        return -1;
    }
    providers[provider_count] = *provider;
    ++provider_count;
    return (int)(provider_count - 1u);
}

int pros_security_provider_set_enabled(uint32_t index, uint32_t enabled) {
    if (index >= provider_count)
        return -1;
    providers[index].enabled = enabled ? 1u : 0u;
    return 0;
}

enum pros_file_reputation pros_security_provider_reputation(
    const uint8_t digest[PROS_FILE_SHA256_BYTES]) {
    enum pros_file_reputation result = PROS_FILE_REPUTATION_UNAVAILABLE;

    if (!digest)
        return result;

    for (uint32_t i = 0; i < provider_count; ++i) {
        enum pros_file_reputation candidate =
            PROS_FILE_REPUTATION_UNAVAILABLE;
        if (!providers[i].enabled)
            continue;
        if (providers[i].query(digest, &candidate) !=
            PROS_SECURITY_PROVIDER_OK) {
            (void)pros_health_record_security(0x53454350u,
                                              PROS_SECURITY_HEALTH_PROVIDER,
                                              i);
            continue;
        }
        if (candidate > PROS_FILE_REPUTATION_MALICIOUS)
            candidate = PROS_FILE_REPUTATION_UNAVAILABLE;
        result = merge_reputation(result, candidate);
        if (result == PROS_FILE_REPUTATION_MALICIOUS)
            break;
    }
    return result;
}

uint32_t pros_security_provider_count(void) {
    return provider_count;
}

static enum pros_security_provider_status selftest_clean(
    const uint8_t digest[PROS_FILE_SHA256_BYTES],
    enum pros_file_reputation *reputation) {
    (void)digest;
    if (!reputation)
        return PROS_SECURITY_PROVIDER_ERROR;
    *reputation = PROS_FILE_REPUTATION_CLEAN;
    return PROS_SECURITY_PROVIDER_OK;
}

static enum pros_security_provider_status selftest_suspicious(
    const uint8_t digest[PROS_FILE_SHA256_BYTES],
    enum pros_file_reputation *reputation) {
    (void)digest;
    if (!reputation)
        return PROS_SECURITY_PROVIDER_ERROR;
    *reputation = PROS_FILE_REPUTATION_SUSPICIOUS;
    return PROS_SECURITY_PROVIDER_OK;
}

int pros_security_provider_selftest(void) {
    static const uint8_t digest[PROS_FILE_SHA256_BYTES] = { 0 };
    struct pros_security_provider clean = {
        .name = "selftest-clean", .query = selftest_clean, .enabled = 1
    };
    struct pros_security_provider suspicious = {
        .name = "selftest-suspicious", .query = selftest_suspicious, .enabled = 1
    };
    uint32_t before = provider_count;
    struct pros_security_provider invalid = {
        .name = "selftest-invalid-enabled", .query = selftest_clean, .enabled = 2u
    };

    if (pros_security_provider_register(&invalid) >= 0 ||
        provider_count != before)
        return -1;
    if (pros_security_provider_register(&clean) < 0)
        return -1;
    if (pros_security_provider_reputation(digest) !=
        PROS_FILE_REPUTATION_CLEAN)
        return -1;
    if (pros_security_provider_register(&suspicious) < 0)
        return -1;
    if (pros_security_provider_reputation(digest) !=
        PROS_FILE_REPUTATION_SUSPICIOUS)
        return -1;

    provider_count = before;
    return 0;
}

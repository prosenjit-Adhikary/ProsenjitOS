#ifndef PROSENJIT_SECURITY_TELEMETRY_SERVICE_H
#define PROSENJIT_SECURITY_TELEMETRY_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include "telemetry_protocol.h"

/*
 * Host-testable request dispatcher for the future Security Control Center
 * broker. The caller must supply authorization established by the OS-owned
 * endpoint/transport; this library cannot authenticate a peer itself.
 * Backend callbacks must return only bounded, redacted, public wire records.
 */
struct scc_service_backend {
    void *context;
    int (*get_status)(void *context, uint8_t payload[24]);
    int (*list_events)(void *context, uint64_t after_sequence,
                       uint32_t category_mask, uint16_t max_records,
                       uint8_t payload[SCC_RESPONSE_PAYLOAD_MAX],
                       uint16_t *payload_length, uint64_t *event_cursor);
    int (*get_event)(void *context, uint64_t sequence,
                     uint8_t payload[SCC_EVENT_RECORD_SIZE]);
};

/* Backend callback results map to protocol status values, not errno values. */
int scc_service_dispatch(const struct scc_service_backend *backend,
                         int peer_authorized,
                         const uint8_t *request, size_t request_length,
                         uint8_t *response, size_t response_capacity,
                         size_t *response_length);

#endif

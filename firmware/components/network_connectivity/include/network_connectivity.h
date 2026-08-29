#ifndef NETWORK_CONNECTIVITY_H
#define NETWORK_CONNECTIVITY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct network_connectivity network_connectivity_t;

typedef enum {
    NETWORK_CONNECTIVITY_RESULT_OK = 0,
    NETWORK_CONNECTIVITY_RESULT_INVALID_ARGUMENT,
    NETWORK_CONNECTIVITY_RESULT_NO_MEMORY,
    NETWORK_CONNECTIVITY_RESULT_INVALID_STATE,
    NETWORK_CONNECTIVITY_RESULT_STORAGE_ERROR,
    NETWORK_CONNECTIVITY_RESULT_PLATFORM_ERROR,
} network_connectivity_result_t;

typedef enum {
    NETWORK_CONNECTIVITY_STATE_STOPPED = 0,
    NETWORK_CONNECTIVITY_STATE_UNCONFIGURED,
    NETWORK_CONNECTIVITY_STATE_CONNECTING,
    NETWORK_CONNECTIVITY_STATE_READY,
    NETWORK_CONNECTIVITY_STATE_BACKOFF,
    NETWORK_CONNECTIVITY_STATE_ERROR,
} network_connectivity_state_t;

typedef enum {
    NETWORK_CONNECTIVITY_EVENT_STATE_CHANGED = 0,
    NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED,
    NETWORK_CONNECTIVITY_EVENT_IP_LOST,
} network_connectivity_event_type_t;

typedef struct {
    network_connectivity_event_type_t type;
    network_connectivity_state_t state;
    uint64_t ip_epoch;
    uint32_t ipv4_address;
    uint32_t reconnect_delay_ms;
    uint16_t disconnect_reason;
} network_connectivity_event_t;

/*
 * Events are delivered synchronously after the component releases its internal
 * lock. The callback must not block and may call network_connectivity_get_status().
 */
typedef void (*network_connectivity_event_callback_t)(
    const network_connectivity_event_t *event,
    void *context);

typedef struct {
    network_connectivity_event_callback_t event_callback;
    void *event_context;
} network_connectivity_config_t;

typedef struct {
    network_connectivity_state_t state;
    bool has_credentials;
    bool has_ip;
    /* Monotonic for the current boot; zero means no IP has been acquired. */
    uint64_t ip_epoch;
    /* IPv4 address in the same network byte order used by esp_ip4_addr_t.addr. */
    uint32_t ipv4_address;
    uint32_t reconnect_attempt;
    uint32_t disconnect_count;
    uint32_t current_reconnect_delay_ms;
    uint16_t last_disconnect_reason;
} network_connectivity_status_t;

network_connectivity_result_t network_connectivity_create(
    const network_connectivity_config_t *config,
    network_connectivity_t **connectivity);

network_connectivity_result_t network_connectivity_start(
    network_connectivity_t *connectivity);

void network_connectivity_stop(network_connectivity_t *connectivity);

void network_connectivity_destroy(network_connectivity_t *connectivity);

void network_connectivity_get_status(
    network_connectivity_t *connectivity,
    network_connectivity_status_t *status);

#endif

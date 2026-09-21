#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../interface/gateway_protocol.h"
#include "ble_types.h"

typedef enum {
    SCHEDULER_STATE_IDLE = 0,
    SCHEDULER_STATE_DISCOVERING,
    SCHEDULER_STATE_CONNECTING,
    SCHEDULER_STATE_CONFIGURING,
    SCHEDULER_STATE_READY,
    SCHEDULER_STATE_STREAMING,
    SCHEDULER_STATE_RECOVERING,
    SCHEDULER_STATE_FAILED,
} ble_scheduler_state_t;

typedef struct {
    uint8_t max_parallel_connects;
    uint32_t connect_gap_ms;
    uint32_t post_connect_settle_ms;
    uint32_t startup_health_window_ms;
    uint32_t min_rate_hz;
    bool stagger_connections;
    bool stagger_stream_start;
} ble_scheduler_policy_t;

int ble_scheduler_init(void);

int ble_scheduler_start_scan(const char *request_id, uint32_t timeout_ms);
int ble_scheduler_stop_scan(void);
int is_scan_active(void);
int ble_scheduler_connect_addresses(
    const char *request_id,
    const gateway_connect_sensor_t *sensors,
    uint8_t sensor_count
);
int ble_scheduler_disconnect_addresses(
    const char *request_id,
    const char addresses[][GATEWAY_MAX_ADDRESS_LEN],
    uint8_t address_count
);
int ble_scheduler_subscribe(
    const char *request_id,
    const char *address,
    const char *characteristic_uuid,
    bool binary_notifications,
    bool indicate
);
int ble_scheduler_unsubscribe(
    const char *address,
    const char *characteristic_uuid
);
int ble_scheduler_gatt_write(
    const char *request_id,
    const char *address,
    const char *characteristic_uuid,
    const uint8_t *data,
    uint16_t data_len,
    bool without_response
);
int ble_scheduler_gatt_read(
    const char *request_id,
    const char *address,
    const char *characteristic_uuid
);
int ble_scheduler_disconnect_all(void);
int ble_scheduler_reset_session(void);
int ble_scheduler_get_status(void);

void ble_scheduler_tick(void);

/*
 * BLE interface callbacks feed into scheduler.
 */
void ble_scheduler_on_sensor_found(const ble_discovered_sensor_t *sensor);
void ble_scheduler_on_connected(const char *address, uint16_t conn_handle);
void ble_scheduler_on_disconnected(const char *address, int reason);
void ble_scheduler_on_notification(
    const char *address,
    const char *characteristic_uuid,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_time_us
);
void ble_scheduler_report_notification_rx_stats(const char *request_id);

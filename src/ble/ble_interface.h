#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ble_types.h"

typedef void (*ble_sensor_found_cb_t)(const ble_discovered_sensor_t *sensor);
typedef void (*ble_connected_cb_t)(const char *address, uint16_t conn_handle);
typedef void (*ble_disconnected_cb_t)(const char *address, int reason);
typedef void (*ble_notification_cb_t)(
    const char *address,
    const char *characteristic_uuid,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_time_us,
    uint32_t receive_sequence
);

typedef struct {
    ble_sensor_found_cb_t on_sensor_found;
    ble_connected_cb_t on_connected;
    ble_disconnected_cb_t on_disconnected;
    ble_notification_cb_t on_notification;
} ble_interface_callbacks_t;

int ble_interface_init(const ble_interface_callbacks_t *callbacks);

int ble_interface_start_scan(uint32_t timeout_ms);
int ble_interface_stop_scan(void);

int ble_interface_connect(const char *address);
int ble_interface_disconnect(const char *address);
int ble_interface_disconnect_all(void);
int ble_interface_reset_state(void);

int ble_interface_discover_gatt(const char *address);
int ble_interface_subscribe(const char *address, const char *characteristic_uuid, bool indicate);
int ble_interface_read(
    const char *address,
    const char *characteristic_uuid,
    uint8_t *data_out,
    size_t *data_len_in_out
);
int ble_interface_write(
    const char *address,
    const char *characteristic_uuid,
    const uint8_t *data,
    size_t data_len,
    bool without_response
);

/*
 * Writes with response are asynchronous. ble_interface_write() accepts and
 * submits (or starts handle discovery for) the operation, while this function
 * advances timeout/cancellation handling and reports completion from main-loop
 * context. It returns 1 when a result was consumed, 0 while pending, or a
 * negative errno for invalid arguments.
 */
int ble_interface_write_poll(int *result_out);
bool ble_interface_write_is_busy(void);
void ble_interface_write_cancel(int reason);

int ble_interface_get_rssi(const char *address, int8_t *rssi_out);
uint8_t ble_interface_active_connection_count(void);

/*
 * Optional tuning hooks. Some stacks expose these, some do not.
 */
int ble_interface_request_connection_params(
    const char *address,
    uint16_t min_interval_units,
    uint16_t max_interval_units,
    uint16_t latency,
    uint16_t supervision_timeout_units
);

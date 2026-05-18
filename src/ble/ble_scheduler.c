/*

This is the most important part to refine
Right now it is synchronous-looking and simplified. The real version should be event-driven:

connection queue
  connect one sensor
  wait for connected callback
  discover GATT
  subscribe
  configure
  move to next sensor

stream supervisor
  check per-sensor frame rate
  detect weak links
  report health
  reconnect if needed

buffering
  notification callback writes into ring buffer
  interface layer drains ring buffer to host

*/
#include "ble_scheduler.h"
#include "ble_interface.h"
//#include "../sensors/sensor_spec.h"
#include "../interface/gateway_interface.h"
#include "../config/gateway_config.h"
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>

static ble_scheduler_state_t g_state = SCHEDULER_STATE_IDLE;
static int64_t g_scan_deadline_ms;
static bool g_scan_active;
static char g_scan_request_id[GATEWAY_MAX_REQUEST_ID_LEN];
static char g_disconnect_request_id[GATEWAY_MAX_REQUEST_ID_LEN];
static char g_connect_request_id[GATEWAY_MAX_REQUEST_ID_LEN];
static gateway_sensor_t g_sensors[GATEWAY_MAX_SENSORS];
static uint8_t g_sensor_count = 0;

static ble_scheduler_policy_t g_policy = {
    .max_parallel_connects = 1,
    .connect_gap_ms = 500,
    .post_connect_settle_ms = 5000,
    .startup_health_window_ms = GATEWAY_HEALTH_WINDOW_MS,
    .min_rate_hz = GATEWAY_MIN_HEALTH_RATE_HZ,
    .stagger_connections = true,
    .stagger_stream_start = false,
};

static gateway_sensor_t *find_sensor_by_address(const char *address)
{
    for (uint8_t i = 0; i < g_sensor_count; i++) {
        if (strcmp(g_sensors[i].address, address) == 0) {
            return &g_sensors[i];
        }
    }
    return NULL;
}

static gateway_sensor_t *allocate_sensor(const ble_discovered_sensor_t *found)
{
    gateway_sensor_t *existing = find_sensor_by_address(found->address);
    if (existing != NULL) {
        return existing;
    }

    if (g_sensor_count >= GATEWAY_MAX_SENSORS) {
        return NULL;
    }

    gateway_sensor_t *sensor = &g_sensors[g_sensor_count++];
    memset(sensor, 0, sizeof(*sensor));

    strncpy(sensor->address, found->address, sizeof(sensor->address) - 1);
    sensor->sensor_type = found->sensor_type;
    sensor->state = SENSOR_STATE_FOUND;
    sensor->rssi = found->rssi;
    sensor->expected_rate_hz = GATEWAY_DEFAULT_STREAM_RATE_HZ;
    sensor->is_required = true;

    return sensor;
}

int ble_scheduler_init(void)
{
    memset(g_sensors, 0, sizeof(g_sensors));
    g_sensor_count = 0;
    g_state = SCHEDULER_STATE_IDLE;
    return 0;
}

int ble_scheduler_start_scan(const char *request_id, uint32_t timeout_ms)
{
    g_state = SCHEDULER_STATE_DISCOVERING;

    if (timeout_ms == 0) {
        timeout_ms = GATEWAY_DEFAULT_SCAN_TIMEOUT_MS;
    }

    memset(g_scan_request_id, 0, sizeof(g_scan_request_id));

    if (request_id != NULL) {
        strncpy(
            g_scan_request_id,
            request_id,
            sizeof(g_scan_request_id) - 1
        );
    }

    g_scan_deadline_ms = k_uptime_get() + timeout_ms;
    g_scan_active = true;

    return ble_interface_start_scan(timeout_ms);
}

int ble_scheduler_stop_scan(void)
{
    int rc = ble_interface_stop_scan();

    if (g_scan_active) {
        g_scan_active = false;
        g_state = SCHEDULER_STATE_IDLE;
        gateway_interface_send_scan_complete(g_scan_request_id);
    }

    return rc;
}

int ble_scheduler_connect_addresses(
    const char *request_id,
    const gateway_connect_sensor_t *sensors,
    uint8_t sensor_count
)
{
    if (sensors == NULL || sensor_count == 0) {
        return -1;
    }

    memset(g_connect_request_id, 0, sizeof(g_connect_request_id));

    if (request_id != NULL) {
        strncpy(
            g_connect_request_id,
            request_id,
            sizeof(g_connect_request_id) - 1
        );
    }

    /*
     * Smallest milestone:
     * connect only the first requested address.
     * Multi-sensor queue comes later.
     */
    return ble_interface_connect(sensors[0].address);
}

int ble_scheduler_disconnect_addresses(
    const char *request_id,
    const char addresses[][GATEWAY_MAX_ADDRESS_LEN],
    uint8_t address_count
)
{
    if (addresses == NULL || address_count == 0) {
        return -1;
    }

    memset(g_disconnect_request_id, 0, sizeof(g_disconnect_request_id));

    if (request_id != NULL) {
        strncpy(
            g_disconnect_request_id,
            request_id,
            sizeof(g_disconnect_request_id) - 1
        );
    }

    for (uint8_t i = 0; i < address_count; i++) {
        int rc = ble_interface_disconnect(addresses[i]);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

int ble_scheduler_subscribe(
    const char *address,
    const char *characteristic_uuid
)
{
    if (address == NULL || characteristic_uuid == NULL) {
        return -1;
    }

    return ble_interface_subscribe(address, characteristic_uuid);
}

int ble_scheduler_unsubscribe(
    const char *address,
    const char *characteristic_uuid
)
{
    (void)address;
    (void)characteristic_uuid;

    /*
     * TODO:
     * - add BLE unsubscribe support if required by active plugins
     */
    return 0;
}

int ble_scheduler_gatt_write(
    const char *address,
    const char *characteristic_uuid,
    const uint8_t *data,
    uint16_t data_len,
    bool without_response
)
{
    if (address == NULL || characteristic_uuid == NULL || data == NULL) {
        return -1;
    }

    return ble_interface_write(
        address,
        characteristic_uuid,
        data,
        data_len,
        without_response
    );
}

int ble_scheduler_gatt_read(
    const char *address,
    const char *characteristic_uuid
)
{
    uint8_t buffer[GATEWAY_MAX_FRAME_PAYLOAD];
    size_t data_len = sizeof(buffer);

    if (address == NULL || characteristic_uuid == NULL) {
        return -1;
    }

    return ble_interface_read(
        address,
        characteristic_uuid,
        buffer,
        &data_len
    );
}

int ble_scheduler_disconnect_all(void)
{
    for (uint8_t i = 0; i < g_sensor_count; i++) {
        if (g_sensors[i].is_connected) {
            ble_interface_disconnect(g_sensors[i].address);
        }
    }

    g_state = SCHEDULER_STATE_IDLE;
    return 0;
}

int ble_scheduler_get_status(void)
{
    /*
     * TODO:
     * emit one health event per sensor.
     */
    gateway_interface_send_json_line("{\"type\":\"status\"}");
    return 0;
}

void ble_scheduler_tick(void)
{
    if (g_scan_active && k_uptime_get() >= g_scan_deadline_ms) {
        ble_scheduler_stop_scan();
    }
}

void ble_scheduler_on_sensor_found(const ble_discovered_sensor_t *found)
{
    if (found == NULL) {
        return;
    }

    gateway_interface_send_scan_result(
        g_scan_request_id,
        found->address,
        found->name,
        found->rssi
    );
}

void ble_scheduler_on_connected(const char *address, uint16_t conn_handle)
{
    gateway_sensor_t *sensor = find_sensor_by_address(address);

    if (sensor != NULL) {
        sensor->conn_handle = conn_handle;
        sensor->state = SENSOR_STATE_CONNECTED;
        sensor->is_connected = true;
    }

    char line[224];

    snprintf(
        line,
        sizeof(line),
        "{\"type\":\"sensor_connected\","
        "\"request_id\":\"%s\","
        "\"address\":\"%s\"}",
        g_connect_request_id,
        address != NULL ? address : ""
    );

    gateway_interface_send_json_line(line);
}

void ble_scheduler_on_disconnected(const char *address, int reason)
{
    gateway_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor != NULL) {
        sensor->state = SENSOR_STATE_DISCONNECTED;
        sensor->is_connected = false;
        sensor->is_streaming = false;
    }

    char line[192];
    const char *request_id =
    g_disconnect_request_id[0] != '\0'
        ? g_disconnect_request_id
        : g_connect_request_id;

    snprintf(
        line,
        sizeof(line),
        "{\"type\":\"sensor_disconnected\","
        "\"request_id\":\"%s\","
        "\"address\":\"%s\","
        "\"reason\":%d}",
        request_id,
        address != NULL ? address : "",
        reason
    );
    gateway_interface_send_json_line(line);
    g_disconnect_request_id[0] = '\0';
}

void ble_scheduler_on_notification(
    const char *address,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_time_us
)
{
    gateway_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor == NULL || payload == NULL) {
        return;
    }

    sensor->frames_received++;
    sensor->last_frame_gateway_time_us = gateway_time_us;
    if (sensor->first_frame_gateway_time_us == 0) {
        sensor->first_frame_gateway_time_us = gateway_time_us;
    }

    gateway_sensor_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    strncpy(frame.address, address, sizeof(frame.address) - 1);
    frame.sensor_type = sensor->sensor_type;
    frame.gateway_time_us = gateway_time_us;
    frame.sequence = sensor->frames_received;

    if (payload_len > GATEWAY_MAX_FRAME_PAYLOAD) {
        payload_len = GATEWAY_MAX_FRAME_PAYLOAD;
    }

    frame.payload_len = (uint16_t)payload_len;
    memcpy(frame.payload, payload, payload_len);

    gateway_interface_send_frame(&frame);
}

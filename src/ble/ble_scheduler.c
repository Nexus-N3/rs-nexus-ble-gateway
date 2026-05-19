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
static int64_t g_connect_deadline_ms;
static bool g_scan_active;
static char g_scan_request_id[GATEWAY_MAX_REQUEST_ID_LEN];
static char g_disconnect_request_id[GATEWAY_MAX_REQUEST_ID_LEN];
static char g_connect_request_id[GATEWAY_MAX_REQUEST_ID_LEN];
static char g_active_connect_address[GATEWAY_MAX_ADDRESS_LEN];
static char g_connect_queue[GATEWAY_MAX_SENSORS][GATEWAY_MAX_ADDRESS_LEN];
static uint8_t g_connect_queue_count;
static uint8_t g_connect_queue_index;
static uint8_t g_disconnect_pending_count;
static gateway_sensor_t g_sensors[GATEWAY_MAX_SENSORS];
static uint8_t g_sensor_count = 0;
#define NOTIFICATION_QUEUE_DEPTH 128
#define NOTIFICATION_FLUSH_BUDGET 2
#define ACTIVE_SUBSCRIPTION_MAX 16

typedef enum {
    GATT_OP_NONE = 0,
    GATT_OP_READ,
    GATT_OP_WRITE,
    GATT_OP_SUBSCRIBE,
    GATT_OP_UNSUBSCRIBE,
} gatt_op_type_t;

typedef struct {
    bool pending;
    gatt_op_type_t type;
    char request_id[GATEWAY_MAX_REQUEST_ID_LEN];
    char address[GATEWAY_MAX_ADDRESS_LEN];
    char characteristic_uuid[GATEWAY_MAX_UUID_LEN];
    bool without_response;
    bool binary_notifications;
    uint16_t payload_len;
    uint8_t payload[GATEWAY_MAX_FRAME_PAYLOAD];
} pending_gatt_op_t;

static pending_gatt_op_t g_pending_gatt_op;

typedef struct {
    char address[GATEWAY_MAX_ADDRESS_LEN];
    char characteristic_uuid[GATEWAY_MAX_UUID_LEN];
    bool binary_notifications;
    uint8_t sensor_id;
    uint16_t payload_len;
    uint8_t payload[GATEWAY_MAX_FRAME_PAYLOAD];
    uint64_t gateway_time_us;
} pending_notification_t;

static pending_notification_t g_notification_queue[NOTIFICATION_QUEUE_DEPTH];
static uint16_t g_notification_head;
static uint16_t g_notification_tail;
static uint16_t g_notification_count;
static uint32_t g_notification_drop_count;

typedef struct {
    bool used;
    char address[GATEWAY_MAX_ADDRESS_LEN];
    char characteristic_uuid[GATEWAY_MAX_UUID_LEN];
    bool binary_notifications;
    uint8_t sensor_id;
} active_subscription_t;

static active_subscription_t g_active_subscriptions[ACTIVE_SUBSCRIPTION_MAX];

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

static gateway_sensor_t *ensure_sensor_by_address(const char *address)
{
    gateway_sensor_t *existing;

    if (address == NULL || address[0] == '\0') {
        return NULL;
    }

    existing = find_sensor_by_address(address);
    if (existing != NULL) {
        return existing;
    }

    if (g_sensor_count >= GATEWAY_MAX_SENSORS) {
        return NULL;
    }

    existing = &g_sensors[g_sensor_count++];
    memset(existing, 0, sizeof(*existing));
    strncpy(existing->address, address, sizeof(existing->address) - 1);
    existing->state = SENSOR_STATE_IDLE;
    existing->expected_rate_hz = GATEWAY_DEFAULT_STREAM_RATE_HZ;
    existing->is_required = true;
    return existing;
}

static int sensor_id_for_address(const char *address)
{
    for (uint8_t i = 0; i < g_sensor_count; i++) {
        if (strcmp(g_sensors[i].address, address) == 0) {
            return i;
        }
    }
    return -1;
}

static void clear_active_subscriptions_for_address(const char *address)
{
    if (address == NULL) {
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_active_subscriptions); i++) {
        if (g_active_subscriptions[i].used &&
            strcmp(g_active_subscriptions[i].address, address) == 0) {
            memset(&g_active_subscriptions[i], 0, sizeof(g_active_subscriptions[i]));
        }
    }
}

static void register_active_subscription(
    const char *address,
    const char *characteristic_uuid,
    bool binary_notifications
)
{
    int sensor_id = sensor_id_for_address(address);

    if (sensor_id < 0) {
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_active_subscriptions); i++) {
        if (g_active_subscriptions[i].used &&
            strcmp(g_active_subscriptions[i].address, address) == 0 &&
            strcmp(g_active_subscriptions[i].characteristic_uuid, characteristic_uuid) == 0) {
            g_active_subscriptions[i].binary_notifications = binary_notifications;
            g_active_subscriptions[i].sensor_id = (uint8_t)sensor_id;
            return;
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_active_subscriptions); i++) {
        if (!g_active_subscriptions[i].used) {
            memset(&g_active_subscriptions[i], 0, sizeof(g_active_subscriptions[i]));
            g_active_subscriptions[i].used = true;
            g_active_subscriptions[i].binary_notifications = binary_notifications;
            g_active_subscriptions[i].sensor_id = (uint8_t)sensor_id;
            strncpy(g_active_subscriptions[i].address, address, sizeof(g_active_subscriptions[i].address) - 1);
            strncpy(
                g_active_subscriptions[i].characteristic_uuid,
                characteristic_uuid,
                sizeof(g_active_subscriptions[i].characteristic_uuid) - 1
            );
            return;
        }
    }
}

static active_subscription_t *find_active_subscription(
    const char *address,
    const char *characteristic_uuid
)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_active_subscriptions); i++) {
        if (g_active_subscriptions[i].used &&
            strcmp(g_active_subscriptions[i].address, address) == 0 &&
            strcmp(g_active_subscriptions[i].characteristic_uuid, characteristic_uuid) == 0) {
            return &g_active_subscriptions[i];
        }
    }
    return NULL;
}

static void emit_connect_failure(const char *address, int reason)
{
    char line[192];

    snprintf(
        line,
        sizeof(line),
        "{\"type\":\"sensor_disconnected\","
        "\"request_id\":\"%s\","
        "\"address\":\"%s\","
        "\"reason\":%d}",
        g_connect_request_id,
        address != NULL ? address : "",
        reason
    );

    gateway_interface_send_json_line(line);
}

static bool remove_address_from_connect_queue(const char *address)
{
    if (address == NULL || address[0] == '\0') {
        return false;
    }

    for (uint8_t i = g_connect_queue_index; i < g_connect_queue_count; i++) {
        if (strcmp(g_connect_queue[i], address) == 0) {
            for (uint8_t j = i; j + 1 < g_connect_queue_count; j++) {
                memset(g_connect_queue[j], 0, sizeof(g_connect_queue[j]));
                strncpy(
                    g_connect_queue[j],
                    g_connect_queue[j + 1],
                    sizeof(g_connect_queue[j]) - 1
                );
            }

            g_connect_queue_count--;

            if (g_connect_queue_count < GATEWAY_MAX_SENSORS) {
                memset(
                    g_connect_queue[g_connect_queue_count],
                    0,
                    sizeof(g_connect_queue[g_connect_queue_count])
                );
            }

            return true;
        }
    }

    return false;
}

static int start_next_connect(void)
{
    int rc;

    if (g_connect_queue_index >= g_connect_queue_count) {
        g_state = SCHEDULER_STATE_READY;
        g_connect_queue_count = 0;
        g_connect_queue_index = 0;
        return 0;
    }

    g_state = SCHEDULER_STATE_CONNECTING;
    memset(g_active_connect_address, 0, sizeof(g_active_connect_address));
    strncpy(
        g_active_connect_address,
        g_connect_queue[g_connect_queue_index],
        sizeof(g_active_connect_address) - 1
    );
    g_connect_deadline_ms = k_uptime_get() + GATEWAY_DEFAULT_CONNECT_TIMEOUT_MS;
    rc = ble_interface_connect(g_connect_queue[g_connect_queue_index]);
    if (rc != 0) {
        g_state = SCHEDULER_STATE_FAILED;
        g_active_connect_address[0] = '\0';
        g_connect_deadline_ms = 0;
        gateway_interface_send_error(
            g_connect_request_id[0] != '\0' ? g_connect_request_id : NULL,
            rc == -3 ? "sensor_not_found" : "connect_failed",
            rc
        );
        g_connect_queue_count = 0;
        g_connect_queue_index = 0;
        return rc;
    }

    return 0;
}

int ble_scheduler_init(void)
{
    memset(g_sensors, 0, sizeof(g_sensors));
    memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));
    memset(g_notification_queue, 0, sizeof(g_notification_queue));
    memset(g_active_subscriptions, 0, sizeof(g_active_subscriptions));
    g_sensor_count = 0;
    g_connect_queue_count = 0;
    g_connect_queue_index = 0;
    g_disconnect_pending_count = 0;
    g_notification_head = 0;
    g_notification_tail = 0;
    g_notification_count = 0;
    g_notification_drop_count = 0;
    g_connect_deadline_ms = 0;
    g_active_connect_address[0] = '\0';
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

int is_scan_active(void)
{
    return g_scan_active;
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

    g_connect_queue_count = 0;
    g_connect_queue_index = 0;

    for (uint8_t i = 0; i < sensor_count && i < GATEWAY_MAX_SENSORS; i++) {
        if (sensors[i].address[0] == '\0') {
            return -1;
        }

        strncpy(
            g_connect_queue[g_connect_queue_count],
            sensors[i].address,
            sizeof(g_connect_queue[g_connect_queue_count]) - 1
        );
        ensure_sensor_by_address(sensors[i].address);
        g_connect_queue_count++;
    }

    if (g_connect_queue_count == 0) {
        return -1;
    }

    return start_next_connect();
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

    g_disconnect_pending_count = address_count;

    for (uint8_t i = 0; i < address_count; i++) {
        int rc = ble_interface_disconnect(addresses[i]);
        if (rc != 0) {
            g_disconnect_pending_count = 0;
            return rc;
        }
    }

    return 0;
}

int ble_scheduler_subscribe(
        const char *request_id,
        const char *address,
        const char *characteristic_uuid,
        bool binary_notifications
)
{
    if (address == NULL || characteristic_uuid == NULL) {
        return -1;
    }

    if (g_pending_gatt_op.pending) {
        return -16;
    }

    memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));

    g_pending_gatt_op.pending = true;
    g_pending_gatt_op.type = GATT_OP_SUBSCRIBE;
    g_pending_gatt_op.binary_notifications = binary_notifications;

    strncpy(
        g_pending_gatt_op.address,
        address,
        sizeof(g_pending_gatt_op.address) - 1
    );

    strncpy(
        g_pending_gatt_op.characteristic_uuid,
        characteristic_uuid,
        sizeof(g_pending_gatt_op.characteristic_uuid) - 1
    );

    if (request_id != NULL) {
        strncpy(
            g_pending_gatt_op.request_id,
            request_id,
            sizeof(g_pending_gatt_op.request_id) - 1
        );
    }

    return 0;
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
    const char *request_id,
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

    if (g_pending_gatt_op.pending) {
        return -16;
    }

    memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));
    g_pending_gatt_op.pending = true;
    g_pending_gatt_op.type = GATT_OP_WRITE;
    g_pending_gatt_op.without_response = without_response;
    g_pending_gatt_op.payload_len = data_len;
    strncpy(g_pending_gatt_op.address, address, sizeof(g_pending_gatt_op.address) - 1);
    strncpy(
        g_pending_gatt_op.characteristic_uuid,
        characteristic_uuid,
        sizeof(g_pending_gatt_op.characteristic_uuid) - 1
    );
    if (request_id != NULL) {
        strncpy(
            g_pending_gatt_op.request_id,
            request_id,
            sizeof(g_pending_gatt_op.request_id) - 1
        );
    }
    if (data_len > sizeof(g_pending_gatt_op.payload)) {
        data_len = sizeof(g_pending_gatt_op.payload);
        g_pending_gatt_op.payload_len = data_len;
    }
    memcpy(g_pending_gatt_op.payload, data, data_len);
    return 0;
}

int ble_scheduler_gatt_read(
    const char *request_id,
    const char *address,
    const char *characteristic_uuid
)
{
    if (address == NULL || characteristic_uuid == NULL) {
        return -1;
    }

    if (g_pending_gatt_op.pending) {
        return -16;
    }

    memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));
    g_pending_gatt_op.pending = true;
    g_pending_gatt_op.type = GATT_OP_READ;
    strncpy(g_pending_gatt_op.address, address, sizeof(g_pending_gatt_op.address) - 1);
    strncpy(
        g_pending_gatt_op.characteristic_uuid,
        characteristic_uuid,
        sizeof(g_pending_gatt_op.characteristic_uuid) - 1
    );
    if (request_id != NULL) {
        strncpy(
            g_pending_gatt_op.request_id,
            request_id,
            sizeof(g_pending_gatt_op.request_id) - 1
        );
    }
    return 0;
}

int ble_scheduler_disconnect_all(void)
{
    for (uint8_t i = 0; i < g_sensor_count; i++) {
        if (g_sensors[i].is_connected) {
            ble_interface_disconnect(g_sensors[i].address);
        }
    }

    g_disconnect_pending_count = 0;
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

static void flush_notification_queue(uint8_t budget)
{
    uint8_t flushed = 0;

    while (g_notification_count > 0 && flushed < budget) {
        pending_notification_t item;
        unsigned int key = irq_lock();

        if (g_notification_count == 0) {
            irq_unlock(key);
            return;
        }

        item = g_notification_queue[g_notification_head];
        memset(&g_notification_queue[g_notification_head], 0, sizeof(g_notification_queue[g_notification_head]));
        g_notification_head = (uint16_t)((g_notification_head + 1U) % NOTIFICATION_QUEUE_DEPTH);
        g_notification_count--;
        irq_unlock(key);

        if (item.binary_notifications) {
            gateway_interface_send_stream_frame(
                item.sensor_id,
                item.payload,
                item.payload_len,
                item.gateway_time_us
            );
        } else {
            static const char hex_chars[] = "0123456789ABCDEF";
            char payload_hex[(GATEWAY_MAX_FRAME_PAYLOAD * 2) + 1];
            char line[768];

            for (size_t i = 0; i < item.payload_len; i++) {
                payload_hex[i * 2] = hex_chars[(item.payload[i] >> 4) & 0x0F];
                payload_hex[i * 2 + 1] = hex_chars[item.payload[i] & 0x0F];
            }
            payload_hex[item.payload_len * 2] = '\0';

            snprintf(
                line,
                sizeof(line),
                "{\"type\":\"notification\","
                "\"address\":\"%s\","
                "\"characteristic_uuid\":\"%s\","
                "\"payload_hex\":\"%s\","
                "\"payload_len\":%u,"
                "\"gateway_timestamp_us\":%llu}",
                item.address,
                item.characteristic_uuid,
                payload_hex,
                (unsigned int)item.payload_len,
                (unsigned long long)item.gateway_time_us
            );
            gateway_interface_send_json_line(line);
        }
        flushed++;
    }
}

void ble_scheduler_tick(void)
{
    bool handled_gatt_op = false;

    if (g_scan_active && k_uptime_get() >= g_scan_deadline_ms) {
        ble_scheduler_stop_scan();
    }

    if (g_state == SCHEDULER_STATE_CONNECTING &&
        g_active_connect_address[0] != '\0' &&
        g_connect_deadline_ms > 0 &&
        k_uptime_get() >= g_connect_deadline_ms) {
        char timed_out_address[GATEWAY_MAX_ADDRESS_LEN];

        strncpy(
            timed_out_address,
            g_active_connect_address,
            sizeof(timed_out_address) - 1
        );
        timed_out_address[sizeof(timed_out_address) - 1] = '\0';

        (void)ble_interface_disconnect(timed_out_address);
        emit_connect_failure(timed_out_address, -110);

        g_active_connect_address[0] = '\0';
        g_connect_deadline_ms = 0;

        if (remove_address_from_connect_queue(timed_out_address)) {
            start_next_connect();
        }

        return;
    }

    
    if (g_pending_gatt_op.pending && g_pending_gatt_op.type == GATT_OP_READ) {
        uint8_t buffer[GATEWAY_MAX_FRAME_PAYLOAD];
        size_t data_len = sizeof(buffer);
        int rc = ble_interface_read(
            g_pending_gatt_op.address,
            g_pending_gatt_op.characteristic_uuid,
            buffer,
            &data_len
        );

        if (rc != 0) {
            gateway_interface_send_error(
                g_pending_gatt_op.request_id[0] != '\0' ? g_pending_gatt_op.request_id : NULL,
                "gatt_read_failed",
                rc
            );
        } else {
            static const char hex_chars[] = "0123456789ABCDEF";
            char payload_hex[(GATEWAY_MAX_FRAME_PAYLOAD * 2) + 1];
            char line[768];

            for (size_t i = 0; i < data_len; i++) {
                payload_hex[i * 2] = hex_chars[(buffer[i] >> 4) & 0x0F];
                payload_hex[i * 2 + 1] = hex_chars[buffer[i] & 0x0F];
            }
            payload_hex[data_len * 2] = '\0';

            snprintf(
                line,
                sizeof(line),
                "{\"type\":\"read_result\",\"request_id\":\"%s\","
                "\"address\":\"%s\",\"characteristic_uuid\":\"%s\","
                "\"payload_hex\":\"%s\",\"ok\":true}",
                g_pending_gatt_op.request_id,
                g_pending_gatt_op.address,
                g_pending_gatt_op.characteristic_uuid,
                payload_hex
            );
            gateway_interface_send_json_line(line);
        }
        memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));
        handled_gatt_op = true;
    } else if (g_pending_gatt_op.pending && g_pending_gatt_op.type == GATT_OP_WRITE) {
        int rc = ble_interface_write(
            g_pending_gatt_op.address,
            g_pending_gatt_op.characteristic_uuid,
            g_pending_gatt_op.payload,
            g_pending_gatt_op.payload_len,
            g_pending_gatt_op.without_response
        );

        if (rc != 0) {
            gateway_interface_send_error(
                g_pending_gatt_op.request_id[0] != '\0' ? g_pending_gatt_op.request_id : NULL,
                "gatt_write_failed",
                rc
            );
        } else {
            char line[256];
            snprintf(
                line,
                sizeof(line),
                "{\"type\":\"write_complete\",\"request_id\":\"%s\","
                "\"address\":\"%s\",\"characteristic_uuid\":\"%s\",\"ok\":true}",
                g_pending_gatt_op.request_id,
                g_pending_gatt_op.address,
                g_pending_gatt_op.characteristic_uuid
            );
            gateway_interface_send_json_line(line);
        }
        memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));
        handled_gatt_op = true;
    } else if (g_pending_gatt_op.pending && g_pending_gatt_op.type == GATT_OP_SUBSCRIBE) {
        int rc = ble_interface_subscribe(
            g_pending_gatt_op.address,
            g_pending_gatt_op.characteristic_uuid
        );

        if (rc != 0) {
            gateway_interface_send_error(
                g_pending_gatt_op.request_id[0] != '\0'
                    ? g_pending_gatt_op.request_id
                    : NULL,
                "subscribe_failed",
                rc
            );
        } else {
            char line[256];

            snprintf(
                line,
                sizeof(line),
                "{\"type\":\"subscribe_complete\","
                "\"request_id\":\"%s\","
                "\"address\":\"%s\","
                "\"sensor_id\":%u,"
                "\"characteristic_uuid\":\"%s\","
                "\"ok\":true}",
                g_pending_gatt_op.request_id,
                g_pending_gatt_op.address,
                (unsigned int)sensor_id_for_address(g_pending_gatt_op.address),
                g_pending_gatt_op.characteristic_uuid
            );

            register_active_subscription(
                g_pending_gatt_op.address,
                g_pending_gatt_op.characteristic_uuid,
                g_pending_gatt_op.binary_notifications
            );
            gateway_interface_send_json_line(line);
        }

        memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));
        handled_gatt_op = true;
    }

    if (handled_gatt_op) {
        return;
    }

    flush_notification_queue(NOTIFICATION_FLUSH_BUDGET);
}

void ble_scheduler_on_sensor_found(const ble_discovered_sensor_t *found)
{
    if (found == NULL) {
        return;
    }

    gateway_sensor_t *sensor = allocate_sensor(found);
    if (sensor != NULL) {
        sensor->rssi = found->rssi;
        sensor->state = SENSOR_STATE_FOUND;
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
    gateway_sensor_t *sensor = ensure_sensor_by_address(address);

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
        "\"sensor_id\":%u,"
        "\"address\":\"%s\"}",
        g_connect_request_id,
        (unsigned int)sensor_id_for_address(address),
        address != NULL ? address : ""
    );

    gateway_interface_send_json_line(line);

    if (g_connect_queue_count > 0 &&
        g_connect_queue_index < g_connect_queue_count &&
        strcmp(g_connect_queue[g_connect_queue_index], address) == 0) {
        if (strcmp(g_active_connect_address, address) == 0) {
            g_active_connect_address[0] = '\0';
            g_connect_deadline_ms = 0;
        }
        g_connect_queue_index++;
        start_next_connect();
    }
}

void ble_scheduler_on_disconnected(const char *address, int reason)
{
    gateway_sensor_t *sensor = find_sensor_by_address(address);
    if (sensor != NULL) {
        sensor->state = SENSOR_STATE_DISCONNECTED;
        sensor->is_connected = false;
        sensor->is_streaming = false;
    }
    clear_active_subscriptions_for_address(address);

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

    if (strcmp(g_active_connect_address, address) == 0) {
        g_active_connect_address[0] = '\0';
        g_connect_deadline_ms = 0;
    }

    if (g_disconnect_pending_count > 0) {
        g_disconnect_pending_count--;
        if (g_disconnect_pending_count == 0) {
            g_disconnect_request_id[0] = '\0';
        }
    } else if (g_connect_queue_count > 0 &&
            g_connect_queue_index < g_connect_queue_count) {
        if (strcmp(g_connect_queue[g_connect_queue_index], address) == 0) {
            g_connect_queue_index++;
            start_next_connect();
        } else if (remove_address_from_connect_queue(address)) {
            start_next_connect();
        } else {
            g_disconnect_request_id[0] = '\0';
        }
    } else {
        g_disconnect_request_id[0] = '\0';
    }
}

void ble_scheduler_on_notification(
    const char *address,
    const char *characteristic_uuid,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_time_us
)
{
    pending_notification_t *slot;
    unsigned int key;
    active_subscription_t *subscription;

    if (address == NULL || characteristic_uuid == NULL || payload == NULL) {
        return;
    }

    if (payload_len > GATEWAY_MAX_FRAME_PAYLOAD) {
        payload_len = GATEWAY_MAX_FRAME_PAYLOAD;
    }

    subscription = find_active_subscription(address, characteristic_uuid);

    key = irq_lock();
    if (g_notification_count >= NOTIFICATION_QUEUE_DEPTH) {
        g_notification_drop_count++;
        irq_unlock(key);
        return;
    }

    slot = &g_notification_queue[g_notification_tail];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->address, address, sizeof(slot->address) - 1);
    strncpy(slot->characteristic_uuid, characteristic_uuid, sizeof(slot->characteristic_uuid) - 1);
    slot->binary_notifications = subscription != NULL ? subscription->binary_notifications : false;
    slot->sensor_id = subscription != NULL ? subscription->sensor_id : 0xFF;
    slot->payload_len = (uint16_t)payload_len;
    memcpy(slot->payload, payload, payload_len);
    slot->gateway_time_us = gateway_time_us;

    g_notification_tail = (uint16_t)((g_notification_tail + 1U) % NOTIFICATION_QUEUE_DEPTH);
    g_notification_count++;
    irq_unlock(key);
}

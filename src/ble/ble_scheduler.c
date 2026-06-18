#include "ble_scheduler.h"
#include "ble_interface.h"
#include "../interface/gateway_interface.h"
#include "../config/gateway_config.h"
#include <string.h>
#include <strings.h>
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
#define NOTIFICATION_FLUSH_BUDGET 8
#define ACTIVE_SUBSCRIPTION_MAX 32

typedef struct {
    bool used;
    char address[GATEWAY_MAX_ADDRESS_LEN];
    uint32_t notification_count;
    uint64_t first_gateway_timestamp_us;
    uint64_t last_gateway_timestamp_us;
    bool has_gateway_timestamp;
    uint32_t subscription_lookup_misses;
    uint32_t json_fallback_notifications;
    uint32_t notification_queue_accepted;
    uint32_t notification_queue_dropped;
    uint32_t notification_queue_flushed;
    uint32_t stream_enqueue_success;
    uint32_t stream_enqueue_dropped;
    uint32_t json_forward_success;
    uint32_t json_forward_dropped;
} notification_rx_stats_t;

static notification_rx_stats_t g_notification_rx_stats[GATEWAY_MAX_SENSORS];

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
static uint32_t g_subscription_table_overflow_count;

typedef struct {
    bool used;
    char address[GATEWAY_MAX_ADDRESS_LEN];
    char characteristic_uuid[GATEWAY_MAX_UUID_LEN];
} emitted_unmapped_warning_t;

static emitted_unmapped_warning_t g_emitted_unmapped_warnings[ACTIVE_SUBSCRIPTION_MAX];

static notification_rx_stats_t *find_or_alloc_notification_rx_stats(
    const char *address
)
{
    notification_rx_stats_t *free_slot = NULL;

    if (address == NULL || address[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_notification_rx_stats); i++) {
        if (g_notification_rx_stats[i].used &&
            strcmp(g_notification_rx_stats[i].address, address) == 0) {
            return &g_notification_rx_stats[i];
        }

        if (!g_notification_rx_stats[i].used && free_slot == NULL) {
            free_slot = &g_notification_rx_stats[i];
        }
    }

    if (free_slot == NULL) {
        return NULL;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    strncpy(free_slot->address, address, sizeof(free_slot->address) - 1);

    return free_slot;
}

static bool uuid_equals(const char *lhs, const char *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return false;
    }

    return strcasecmp(lhs, rhs) == 0;
}

static bool should_emit_unmapped_warning(
    const char *address,
    const char *characteristic_uuid
)
{
    if (address == NULL || characteristic_uuid == NULL) {
        return false;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_emitted_unmapped_warnings); i++) {
        if (g_emitted_unmapped_warnings[i].used &&
            strcmp(g_emitted_unmapped_warnings[i].address, address) == 0 &&
            uuid_equals(g_emitted_unmapped_warnings[i].characteristic_uuid, characteristic_uuid)) {
            return false;
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_emitted_unmapped_warnings); i++) {
        if (!g_emitted_unmapped_warnings[i].used) {
            memset(&g_emitted_unmapped_warnings[i], 0, sizeof(g_emitted_unmapped_warnings[i]));
            g_emitted_unmapped_warnings[i].used = true;
            strncpy(
                g_emitted_unmapped_warnings[i].address,
                address,
                sizeof(g_emitted_unmapped_warnings[i].address) - 1
            );
            strncpy(
                g_emitted_unmapped_warnings[i].characteristic_uuid,
                characteristic_uuid,
                sizeof(g_emitted_unmapped_warnings[i].characteristic_uuid) - 1
            );
            return true;
        }
    }

    return false;
}

static void update_notification_rx_stats(
    const char *address,
    uint64_t gateway_time_us
)
{
    notification_rx_stats_t *stats;

    stats = find_or_alloc_notification_rx_stats(address);
    if (stats == NULL) {
        return;
    }

    stats->notification_count++;
    if (!stats->has_gateway_timestamp) {
        stats->first_gateway_timestamp_us = gateway_time_us;
        stats->has_gateway_timestamp = true;
    }
    stats->last_gateway_timestamp_us = gateway_time_us;
}



static gateway_sensor_t *find_sensor_by_address(const char *address)
{
    for (uint8_t i = 0; i < g_sensor_count; i++) {
        if (strcmp(g_sensors[i].address, address) == 0) {
            return &g_sensors[i];
        }
    }
    return NULL;
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

static int register_active_subscription(
    const char *address,
    const char *characteristic_uuid,
    bool binary_notifications,
    uint8_t *sensor_id_out
)
{
    gateway_sensor_t *sensor;
    int sensor_id;

    if (address == NULL || characteristic_uuid == NULL) {
        return -1;
    }

    sensor = ensure_sensor_by_address(address);
    sensor_id = sensor_id_for_address(address);

    if (sensor_id_out != NULL) {
        *sensor_id_out = 0U;
    }

    if (sensor_id < 0) {
        char line[320];

        snprintf(
            line,
            sizeof(line),
            "{\"type\":\"subscription_register_failed\","
            "\"address\":\"%s\","
            "\"characteristic_uuid\":\"%s\","
            "\"reason\":\"sensor_id_not_found\","
            "\"sensor_slot_allocated\":%s,"
            "\"sensor_count\":%u}",
            address,
            characteristic_uuid,
            sensor != NULL ? "true" : "false",
            (unsigned int)g_sensor_count
        );
        gateway_interface_send_json_line(line);
        return -2;
    }

    if (sensor_id_out != NULL) {
        *sensor_id_out = (uint8_t)sensor_id;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_active_subscriptions); i++) {
        if (g_active_subscriptions[i].used &&
            strcmp(g_active_subscriptions[i].address, address) == 0 &&
            uuid_equals(g_active_subscriptions[i].characteristic_uuid, characteristic_uuid)) {
            g_active_subscriptions[i].binary_notifications = binary_notifications;
            g_active_subscriptions[i].sensor_id = (uint8_t)sensor_id;
            return 0;
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_active_subscriptions); i++) {
        if (!g_active_subscriptions[i].used) {
            memset(&g_active_subscriptions[i], 0, sizeof(g_active_subscriptions[i]));
            g_active_subscriptions[i].used = true;
            g_active_subscriptions[i].binary_notifications = binary_notifications;
            g_active_subscriptions[i].sensor_id = (uint8_t)sensor_id;
            strncpy(
                g_active_subscriptions[i].address,
                address,
                sizeof(g_active_subscriptions[i].address) - 1
            );
            strncpy(
                g_active_subscriptions[i].characteristic_uuid,
                characteristic_uuid,
                sizeof(g_active_subscriptions[i].characteristic_uuid) - 1
            );
            return 0;
        }
    }

    g_subscription_table_overflow_count++;
    return -12;
}

static active_subscription_t *find_active_subscription(
    const char *address,
    const char *characteristic_uuid
)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_active_subscriptions); i++) {
        if (g_active_subscriptions[i].used &&
            strcmp(g_active_subscriptions[i].address, address) == 0 &&
            uuid_equals(g_active_subscriptions[i].characteristic_uuid, characteristic_uuid)) {
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
    memset(g_notification_rx_stats, 0, sizeof(g_notification_rx_stats));
    memset(g_sensors, 0, sizeof(g_sensors));
    memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));
    memset(g_notification_queue, 0, sizeof(g_notification_queue));
    memset(g_active_subscriptions, 0, sizeof(g_active_subscriptions));
    memset(g_emitted_unmapped_warnings, 0, sizeof(g_emitted_unmapped_warnings));

    g_subscription_table_overflow_count = 0;
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

int ble_scheduler_start_rf_survey_scan(const char *request_id, uint32_t timeout_ms)
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

    return ble_interface_start_scan(timeout_ms, true);
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

    return ble_interface_start_scan(timeout_ms, false);
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
    int rc = ble_interface_disconnect_all();

    g_disconnect_pending_count = 0;
    g_disconnect_request_id[0] = '\0';
    g_connect_request_id[0] = '\0';
    g_active_connect_address[0] = '\0';
    g_connect_deadline_ms = 0;
    g_connect_queue_count = 0;
    g_connect_queue_index = 0;
    memset(g_connect_queue, 0, sizeof(g_connect_queue));
    memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));
    memset(g_notification_queue, 0, sizeof(g_notification_queue));
    g_notification_head = 0;
    g_notification_tail = 0;
    g_notification_count = 0;
    g_notification_drop_count = 0;
    memset(g_active_subscriptions, 0, sizeof(g_active_subscriptions));
    memset(g_emitted_unmapped_warnings, 0, sizeof(g_emitted_unmapped_warnings));
    g_subscription_table_overflow_count = 0;
    g_state = SCHEDULER_STATE_IDLE;
    return rc;
}

int ble_scheduler_reset_session(void)
{
    int rc = ble_interface_disconnect_all();

    ble_interface_reset_state();

    memset(g_sensors, 0, sizeof(g_sensors));
    g_sensor_count = 0;
    g_scan_active = false;
    g_scan_deadline_ms = 0;
    g_scan_request_id[0] = '\0';
    g_disconnect_pending_count = 0;
    g_disconnect_request_id[0] = '\0';
    g_connect_request_id[0] = '\0';
    g_active_connect_address[0] = '\0';
    g_connect_deadline_ms = 0;
    g_connect_queue_count = 0;
    g_connect_queue_index = 0;
    memset(g_connect_queue, 0, sizeof(g_connect_queue));
    memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));
    memset(g_notification_queue, 0, sizeof(g_notification_queue));
    g_notification_head = 0;
    g_notification_tail = 0;
    g_notification_count = 0;
    g_notification_drop_count = 0;
    memset(g_active_subscriptions, 0, sizeof(g_active_subscriptions));
    memset(g_emitted_unmapped_warnings, 0, sizeof(g_emitted_unmapped_warnings));
    g_subscription_table_overflow_count = 0;
    g_state = SCHEDULER_STATE_IDLE;
    memset(g_notification_rx_stats, 0, sizeof(g_notification_rx_stats));
    return rc;
}

int ble_scheduler_get_status(void)
{
    gateway_interface_send_json_line("{\"type\":\"status\"}");
    ble_scheduler_report_notification_rx_stats(NULL);
    return 0;
}

static void flush_notification_queue(uint8_t budget)
{
    uint8_t flushed = 0;

    while (g_notification_count > 0 && flushed < budget) {
        notification_rx_stats_t *stats;
        pending_notification_t item;
        int rc;
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

        stats = find_or_alloc_notification_rx_stats(item.address);
        if (stats != NULL) {
            stats->notification_queue_flushed++;
        }

        if (item.binary_notifications) {
            rc = gateway_interface_send_stream_frame(
                item.sensor_id,
                item.payload,
                item.payload_len,
                item.gateway_time_us
            );

            if (stats != NULL) {
                if (rc == 0) {
                    stats->stream_enqueue_success++;
                } else {
                    stats->stream_enqueue_dropped++;
                }
            }
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
            rc = gateway_interface_send_json_line(line);

            if (stats != NULL) {
                if (rc == 0) {
                    stats->json_forward_success++;
                } else {
                    stats->json_forward_dropped++;
                }
            }
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
        uint8_t sensor_id = 0U;
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
            int reg_rc = register_active_subscription(
                g_pending_gatt_op.address,
                g_pending_gatt_op.characteristic_uuid,
                g_pending_gatt_op.binary_notifications,
                &sensor_id
            );

            if (reg_rc != 0) {
                gateway_interface_send_error(
                    g_pending_gatt_op.request_id[0] != '\0'
                        ? g_pending_gatt_op.request_id
                        : NULL,
                    "subscription_register_failed",
                    reg_rc
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
                    (unsigned int)sensor_id,
                    g_pending_gatt_op.characteristic_uuid
                );

                gateway_interface_send_json_line(line);
            }
        }

        memset(&g_pending_gatt_op, 0, sizeof(g_pending_gatt_op));
        handled_gatt_op = true;
    }

    if (handled_gatt_op) {
        flush_notification_queue(NOTIFICATION_FLUSH_BUDGET);
        return;
    }

    flush_notification_queue(NOTIFICATION_FLUSH_BUDGET);
}

#define BLE_RX_STATS_LINE_MAX 1024

void ble_scheduler_report_notification_rx_stats(const char *request_id)
{
    char summary_line[256];
    char complete_line[192];
    uint32_t attempted = 0;
    uint32_t sent = 0;
    uint32_t dropped = 0;
    uint32_t truncated = 0;
    bool summary_enqueued = false;
    bool completion_enqueued = false;

    for (size_t i = 0; i < ARRAY_SIZE(g_notification_rx_stats); i++) {
        char line[BLE_RX_STATS_LINE_MAX];
        int n;

        if (!g_notification_rx_stats[i].used) {
            continue;
        }

        attempted++;

        n = snprintf(
            line,
            sizeof(line),
            "{\"type\":\"ble_notification_rx_stats\","
            "\"request_id\":\"%s\","
            "\"address\":\"%s\","
            "\"notification_count\":%u,"
            "\"first_gateway_timestamp_us\":%llu,"
            "\"last_gateway_timestamp_us\":%llu,"
            "\"subscription_lookup_misses\":%u,"
            "\"json_fallback_notifications\":%u,"
            "\"notification_queue_accepted\":%u,"
            "\"notification_queue_dropped\":%u,"
            "\"notification_queue_flushed\":%u,"
            "\"stream_enqueue_success\":%u,"
            "\"stream_enqueue_dropped\":%u,"
            "\"json_forward_success\":%u,"
            "\"json_forward_dropped\":%u}",
            request_id != NULL ? request_id : "",
            g_notification_rx_stats[i].address,
            (unsigned int)g_notification_rx_stats[i].notification_count,
            (unsigned long long)g_notification_rx_stats[i].first_gateway_timestamp_us,
            (unsigned long long)g_notification_rx_stats[i].last_gateway_timestamp_us,
            (unsigned int)g_notification_rx_stats[i].subscription_lookup_misses,
            (unsigned int)g_notification_rx_stats[i].json_fallback_notifications,
            (unsigned int)g_notification_rx_stats[i].notification_queue_accepted,
            (unsigned int)g_notification_rx_stats[i].notification_queue_dropped,
            (unsigned int)g_notification_rx_stats[i].notification_queue_flushed,
            (unsigned int)g_notification_rx_stats[i].stream_enqueue_success,
            (unsigned int)g_notification_rx_stats[i].stream_enqueue_dropped,
            (unsigned int)g_notification_rx_stats[i].json_forward_success,
            (unsigned int)g_notification_rx_stats[i].json_forward_dropped
        );

        if (n < 0 || n >= (int)sizeof(line)) {
            truncated++;
            dropped++;
            continue;
        }

        if (gateway_interface_send_json_line(line) == 0) {
            sent++;
        } else {
            dropped++;
        }
    }

    snprintf(
        summary_line,
        sizeof(summary_line),
        "{\"type\":\"ble_notification_rx_stats_summary\","
        "\"request_id\":\"%s\","
        "\"attempted\":%u,"
        "\"sent\":%u,"
        "\"dropped\":%u,"
        "\"truncated\":%u}",
        request_id != NULL ? request_id : "",
        (unsigned int)attempted,
        (unsigned int)sent,
        (unsigned int)dropped,
        (unsigned int)truncated
    );

    summary_enqueued = gateway_interface_send_json_line(summary_line) == 0;

    snprintf(
        complete_line,
        sizeof(complete_line),
        "{\"type\":\"ble_notification_rx_stats_complete\","
        "\"request_id\":\"%s\","
        "\"summary_enqueued\":%s}",
        request_id != NULL ? request_id : "",
        summary_enqueued ? "true" : "false"
    );

    completion_enqueued = gateway_interface_send_json_line(complete_line) == 0;
    (void)completion_enqueued;
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
    uint8_t active_connection_count = ble_interface_active_connection_count();
    if (sensor != NULL) {
        sensor->state = SENSOR_STATE_DISCONNECTED;
        sensor->is_connected = false;
        sensor->is_streaming = false;
    }
    //clear_active_subscriptions_for_address(address);

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
        "\"active_connection_count\":%u,"
        "\"reason\":%d}",
        request_id,
        address != NULL ? address : "",
        (unsigned int)active_connection_count,
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
    notification_rx_stats_t *stats;

    if (address == NULL || characteristic_uuid == NULL || payload == NULL) {
        return;
    }

    if (payload_len > GATEWAY_MAX_FRAME_PAYLOAD) {
        payload_len = GATEWAY_MAX_FRAME_PAYLOAD;
    }

    update_notification_rx_stats(address, gateway_time_us);

    subscription = find_active_subscription(address, characteristic_uuid);
    stats = find_or_alloc_notification_rx_stats(address);

    if (subscription == NULL) {
        if (stats != NULL) {
            stats->subscription_lookup_misses++;

            /*
             * Keep using this existing field for now so the host summary
             * still exposes the issue, but this is now an unmapped drop,
             * not a JSON fallback.
             */
            stats->json_fallback_notifications++;
        }

        if (should_emit_unmapped_warning(address, characteristic_uuid)) {
            char line[256];

            snprintf(
                line,
                sizeof(line),
                "{\"type\":\"subscription_lookup_miss\","
                "\"address\":\"%s\","
                "\"characteristic_uuid\":\"%s\","
                "\"subscription_table_overflow_count\":%u}",
                address,
                characteristic_uuid,
                (unsigned int)g_subscription_table_overflow_count
            );
            gateway_interface_send_json_line(line);
        }

        return;
    }

    key = irq_lock();
    if (g_notification_count >= NOTIFICATION_QUEUE_DEPTH) {
        g_notification_drop_count++;
        irq_unlock(key);

        if (stats != NULL) {
            stats->notification_queue_dropped++;
        }

        return;
    }

    slot = &g_notification_queue[g_notification_tail];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->address, address, sizeof(slot->address) - 1);
    strncpy(
        slot->characteristic_uuid,
        characteristic_uuid,
        sizeof(slot->characteristic_uuid) - 1
    );

    slot->binary_notifications = subscription->binary_notifications;
    slot->sensor_id = subscription->sensor_id;
    slot->payload_len = (uint16_t)payload_len;
    memcpy(slot->payload, payload, payload_len);
    slot->gateway_time_us = gateway_time_us;

    g_notification_tail =
        (uint16_t)((g_notification_tail + 1U) % NOTIFICATION_QUEUE_DEPTH);
    g_notification_count++;
    irq_unlock(key);

    if (stats != NULL) {
        stats->notification_queue_accepted++;
    }
}

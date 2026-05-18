#include "ble_interface.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/kernel.h>

#include <stdio.h>
#include <string.h>

static ble_interface_callbacks_t g_callbacks;
static bool g_ble_ready;
static bool g_scanning;

typedef struct {
    char name[32];
} adv_parse_ctx_t;

static bool parse_advertising_data(
    struct bt_data *data,
    void *user_data
)
{
    adv_parse_ctx_t *ctx = (adv_parse_ctx_t *)user_data;

    if (data->type == BT_DATA_NAME_COMPLETE ||
        data->type == BT_DATA_NAME_SHORTENED) {
        size_t len = data->data_len;

        if (len >= sizeof(ctx->name)) {
            len = sizeof(ctx->name) - 1;
        }

        memcpy(ctx->name, data->data, len);
        ctx->name[len] = '\0';

        return false;
    }

    return true;
}

static void device_found(
    const bt_addr_le_t *addr,
    int8_t rssi,
    uint8_t type,
    struct net_buf_simple *ad
)
{
    ARG_UNUSED(type);

    if (g_callbacks.on_sensor_found == NULL) {
        return;
    }

    ble_discovered_sensor_t sensor;
    adv_parse_ctx_t parse_ctx;

    memset(&sensor, 0, sizeof(sensor));
    memset(&parse_ctx, 0, sizeof(parse_ctx));

    snprintf(
        sensor.address,
        sizeof(sensor.address),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        addr->a.val[5],
        addr->a.val[4],
        addr->a.val[3],
        addr->a.val[2],
        addr->a.val[1],
        addr->a.val[0]
    );

    sensor.rssi = rssi;
    sensor.sensor_type = SENSOR_TYPE_UNKNOWN;

    bt_data_parse(ad, parse_advertising_data, &parse_ctx);

    if (parse_ctx.name[0] != '\0') {
        strncpy(sensor.name, parse_ctx.name, sizeof(sensor.name) - 1);
    } else {
        sensor.name[0] = '\0';
    }

    g_callbacks.on_sensor_found(&sensor);
}

int ble_interface_init(const ble_interface_callbacks_t *callbacks)
{
    if (callbacks != NULL) {
        g_callbacks = *callbacks;
    }

    if (g_ble_ready) {
        return 0;
    }

    int rc = bt_enable(NULL);
    if (rc != 0) {
        return rc;
    }

    g_ble_ready = true;
    return 0;
}

int ble_interface_start_scan(const sensor_spec_t *spec, uint32_t timeout_ms)
{
    ARG_UNUSED(spec);
    ARG_UNUSED(timeout_ms);

    if (!g_ble_ready) {
        return -1;
    }

    if (g_scanning) {
        return 0;
    }

    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };

    int rc = bt_le_scan_start(&scan_param, device_found);
    if (rc != 0) {
        return rc;
    }

    g_scanning = true;
    return 0;
}

int ble_interface_stop_scan(void)
{
    if (!g_scanning) {
        return 0;
    }

    int rc = bt_le_scan_stop();
    if (rc != 0) {
        return rc;
    }

    g_scanning = false;
    return 0;
}

int ble_interface_connect(const char *address, const sensor_spec_t *spec)
{
    (void)address;
    (void)spec;

    /*
     * TODO:
     * - create BLE connection
     * - configure preferred connection params if possible
     * - on success call g_callbacks.on_connected()
     */
    return -2;
}

int ble_interface_disconnect(const char *address)
{
    (void)address;

    /*
     * TODO: disconnect.
     */
    return -2;
}

int ble_interface_discover_gatt(const char *address, const sensor_spec_t *spec)
{
    (void)address;
    (void)spec;

    /*
     * TODO:
     * - discover required services/chars from spec
     * - cache handles internally
     */
    return -2;
}

int ble_interface_subscribe(const char *address, const char *characteristic_uuid)
{
    (void)address;
    (void)characteristic_uuid;

    /*
     * TODO:
     * - enable notifications/indications
     * - write CCCD
     */
    return -2;
}

int ble_interface_read(
    const char *address,
    const char *characteristic_uuid,
    uint8_t *data_out,
    size_t *data_len_in_out
)
{
    (void)address;
    (void)characteristic_uuid;
    (void)data_out;
    (void)data_len_in_out;

    /*
     * TODO:
     * - read characteristic
     * - copy bytes to caller buffer
     * - update returned length
     */
    return -2;
}

int ble_interface_write(
    const char *address,
    const char *characteristic_uuid,
    const uint8_t *data,
    size_t data_len,
    bool without_response
)
{
    (void)address;
    (void)characteristic_uuid;
    (void)data;
    (void)data_len;
    (void)without_response;

    /*
     * TODO:
     * - write characteristic
     */
    return -2;
}

int ble_interface_get_rssi(const char *address, int8_t *rssi_out)
{
    (void)address;

    if (rssi_out != NULL) {
        *rssi_out = 0;
    }

    return -2;
}

int ble_interface_request_connection_params(
    const char *address,
    uint16_t min_interval_units,
    uint16_t max_interval_units,
    uint16_t latency,
    uint16_t supervision_timeout_units
)
{
    (void)address;
    (void)min_interval_units;
    (void)max_interval_units;
    (void)latency;
    (void)supervision_timeout_units;

    /*
     * TODO:
     * - vendor-specific connection parameter update request.
     */
    return -2;
}
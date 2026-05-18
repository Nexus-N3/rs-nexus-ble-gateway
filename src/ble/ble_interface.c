#include "ble_interface.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/kernel.h>

#include <stdio.h>
#include <string.h>

#define BLE_MAX_DISCOVERED_PEERS 32

static ble_interface_callbacks_t g_callbacks;
static bool g_ble_ready;
static bool g_scanning;

typedef struct {
    char name[32];
} adv_parse_ctx_t;

typedef struct {
    bool used;
    char address[GATEWAY_MAX_ADDRESS_LEN];
    bt_addr_le_t addr;
} known_peer_t;

typedef struct {
    bool used;
    char address[GATEWAY_MAX_ADDRESS_LEN];
    struct bt_conn *conn;
} active_conn_t;

static active_conn_t g_active_conns[GATEWAY_MAX_SENSORS];

static known_peer_t g_known_peers[BLE_MAX_DISCOVERED_PEERS];

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

static void format_address(
    const bt_addr_le_t *addr,
    char *out,
    size_t out_size
)
{
    if (addr == NULL || out == NULL || out_size == 0) {
        return;
    }

    snprintf(
        out,
        out_size,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        addr->a.val[5],
        addr->a.val[4],
        addr->a.val[3],
        addr->a.val[2],
        addr->a.val[1],
        addr->a.val[0]
    );
}

static active_conn_t *find_active_conn_by_address(const char *address)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_active_conns); i++) {
        if (g_active_conns[i].used &&
            strcmp(g_active_conns[i].address, address) == 0) {
            return &g_active_conns[i];
        }
    }

    return NULL;
}

static active_conn_t *find_active_conn_by_conn(const struct bt_conn *conn)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_active_conns); i++) {
        if (g_active_conns[i].used && g_active_conns[i].conn == conn) {
            return &g_active_conns[i];
        }
    }

    return NULL;
}

static active_conn_t *allocate_active_conn(const char *address)
{
    active_conn_t *existing = find_active_conn_by_address(address);
    if (existing != NULL) {
        return existing;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_active_conns); i++) {
        if (!g_active_conns[i].used) {
            memset(&g_active_conns[i], 0, sizeof(g_active_conns[i]));
            g_active_conns[i].used = true;
            strncpy(
                g_active_conns[i].address,
                address,
                sizeof(g_active_conns[i].address) - 1
            );
            return &g_active_conns[i];
        }
    }

    return NULL;
}

static void release_active_conn(active_conn_t *entry)
{
    if (entry == NULL) {
        return;
    }

    if (entry->conn != NULL) {
        bt_conn_unref(entry->conn);
    }

    memset(entry, 0, sizeof(*entry));
}

static known_peer_t *find_known_peer(const char *address)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_known_peers); i++) {
        if (g_known_peers[i].used &&
            strcmp(g_known_peers[i].address, address) == 0) {
            return &g_known_peers[i];
        }
    }

    return NULL;
}

static known_peer_t *upsert_known_peer(const bt_addr_le_t *addr)
{
    char address[GATEWAY_MAX_ADDRESS_LEN];
    known_peer_t *free_slot = NULL;

    format_address(addr, address, sizeof(address));

    for (size_t i = 0; i < ARRAY_SIZE(g_known_peers); i++) {
        if (g_known_peers[i].used &&
            strcmp(g_known_peers[i].address, address) == 0) {
            g_known_peers[i].addr = *addr;
            return &g_known_peers[i];
        }

        if (!g_known_peers[i].used && free_slot == NULL) {
            free_slot = &g_known_peers[i];
        }
    }

    if (free_slot == NULL) {
        return NULL;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    free_slot->addr = *addr;
    strncpy(free_slot->address, address, sizeof(free_slot->address) - 1);

    return free_slot;
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    char address[GATEWAY_MAX_ADDRESS_LEN];

    format_address(bt_conn_get_dst(conn), address, sizeof(address));

    if (err != 0) {
        if (g_callbacks.on_disconnected != NULL) {
            g_callbacks.on_disconnected(address, -(int)err);
        }
        return;
    }

    if (g_callbacks.on_connected != NULL) {
        g_callbacks.on_connected(address, (uint16_t)bt_conn_index(conn));
    }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    char address[GATEWAY_MAX_ADDRESS_LEN];
    active_conn_t *entry = find_active_conn_by_conn(conn);

    format_address(bt_conn_get_dst(conn), address, sizeof(address));

    if (entry != NULL) {
        release_active_conn(entry);
    }

    if (g_callbacks.on_disconnected != NULL) {
        g_callbacks.on_disconnected(address, (int)reason);
    }
}

static struct bt_conn_cb g_conn_callbacks = {
    .connected = on_connected,
    .disconnected = on_disconnected,
};

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

    known_peer_t *peer = upsert_known_peer(addr);
    if (peer == NULL) {
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
    bt_conn_cb_register(&g_conn_callbacks);
    if (rc != 0) {
        return rc;
    }

    g_ble_ready = true;
    return 0;
}

int ble_interface_start_scan(uint32_t timeout_ms)
{
    //(void)spec;
    ARG_UNUSED(timeout_ms);

    if (!g_ble_ready) {
        return -1;
    }

    if (g_scanning) {
        return 0;
    }

    memset(g_known_peers, 0, sizeof(g_known_peers));

    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_ACTIVE, //BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
        //.options = BT_LE_SCAN_OPT_NONE,
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

int ble_interface_connect(const char *address)
{
    if (!g_ble_ready || address == NULL || address[0] == '\0') {
        return -1;
    }

    known_peer_t *peer = find_known_peer(address);
    if (peer == NULL) {
        return -3;
    }

    if (g_scanning) {
        int scan_rc = ble_interface_stop_scan();
        if (scan_rc != 0) {
            return scan_rc;
        }
    }

    active_conn_t *entry = allocate_active_conn(address);
    if (entry == NULL) {
        return -4;
    }

    if (entry->conn != NULL) {
        return 0;
    }

    struct bt_conn *conn = NULL;

    int rc = bt_conn_le_create(
        &peer->addr,
        BT_CONN_LE_CREATE_CONN,
        BT_LE_CONN_PARAM_DEFAULT,
        &conn
    );

    if (rc != 0) {
        memset(entry, 0, sizeof(*entry));
        return rc;
    }

    entry->conn = conn;

    return 0;
}

int ble_interface_disconnect(const char *address)
{
    active_conn_t *entry;

    if (address == NULL || address[0] == '\0') {
        return -1;
    }

    entry = find_active_conn_by_address(address);
    if (entry == NULL || entry->conn == NULL) {
        return -3;
    }

    return bt_conn_disconnect(entry->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

int ble_interface_discover_gatt(const char *address)
{
    (void)address;
    //(void)spec;

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
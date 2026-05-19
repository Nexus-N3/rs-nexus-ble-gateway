#include "ble_interface.h"
#include "../interface/gateway_interface.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define BLE_MAX_DISCOVERED_PEERS 32

static ble_interface_callbacks_t g_callbacks;
static bool g_ble_ready;
static bool g_scanning;

typedef struct {
    struct bt_gatt_discover_params params;
    struct bt_uuid_any uuid;
    struct k_sem done;
    uint16_t value_handle;
    uint16_t ccc_handle;
    int err;
} gatt_discover_ctx_t;

typedef struct {
    struct bt_gatt_write_params params;
    struct k_sem done;
    int err;
} gatt_write_ctx_t;

typedef struct {
    struct bt_gatt_read_params params;
    struct k_sem done;
    uint8_t *data_out;
    size_t *data_len_in_out;
    size_t bytes_copied;
    int err;
} gatt_read_ctx_t;

typedef struct {
    bool used;
    char address[GATEWAY_MAX_ADDRESS_LEN];
    char characteristic_uuid[GATEWAY_MAX_UUID_LEN];
    struct bt_gatt_subscribe_params params;
} gatt_subscribe_ctx_t;

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
    struct bt_gatt_exchange_params mtu_params;
    bool mtu_exchange_in_progress;
} active_conn_t;

static active_conn_t g_active_conns[GATEWAY_MAX_SENSORS];

static known_peer_t g_known_peers[BLE_MAX_DISCOVERED_PEERS];
static gatt_discover_ctx_t g_discover_ctx;
static gatt_write_ctx_t g_write_ctx;
static gatt_read_ctx_t g_read_ctx;
static gatt_subscribe_ctx_t g_subscribe_ctxs[GATEWAY_MAX_SENSORS];

static void emit_gatt_debug(
    const char *phase,
    const char *address,
    const char *characteristic_uuid,
    uint16_t handle,
    uint16_t mtu,
    size_t data_len,
    int rc,
    bool without_response
)
{
    char line[512];

    snprintf(
        line,
        sizeof(line),
        "{\"type\":\"gatt_debug\","
        "\"phase\":\"%s\","
        "\"address\":\"%s\","
        "\"characteristic_uuid\":\"%s\","
        "\"handle\":%u,"
        "\"mtu\":%u,"
        "\"data_len\":%u,"
        "\"without_response\":%s,"
        "\"rc\":%d}",
        phase != NULL ? phase : "",
        address != NULL ? address : "",
        characteristic_uuid != NULL ? characteristic_uuid : "",
        handle,
        mtu,
        (unsigned int)data_len,
        without_response ? "true" : "false",
        rc
    );

    gateway_interface_send_log(line);
}

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

static gatt_subscribe_ctx_t *find_subscribe_ctx(
    const char *address,
    const char *characteristic_uuid
)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_subscribe_ctxs); i++) {
        if (g_subscribe_ctxs[i].used &&
            strcmp(g_subscribe_ctxs[i].address, address) == 0 &&
            strcmp(g_subscribe_ctxs[i].characteristic_uuid, characteristic_uuid) == 0) {
            return &g_subscribe_ctxs[i];
        }
    }

    return NULL;
}

static gatt_subscribe_ctx_t *allocate_subscribe_ctx(
    const char *address,
    const char *characteristic_uuid
)
{
    gatt_subscribe_ctx_t *existing =
        find_subscribe_ctx(address, characteristic_uuid);

    if (existing != NULL) {
        return existing;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_subscribe_ctxs); i++) {
        if (!g_subscribe_ctxs[i].used) {
            memset(&g_subscribe_ctxs[i], 0, sizeof(g_subscribe_ctxs[i]));

            g_subscribe_ctxs[i].used = true;

            strncpy(
                g_subscribe_ctxs[i].address,
                address,
                sizeof(g_subscribe_ctxs[i].address) - 1
            );

            strncpy(
                g_subscribe_ctxs[i].characteristic_uuid,
                characteristic_uuid,
                sizeof(g_subscribe_ctxs[i].characteristic_uuid) - 1
            );

            return &g_subscribe_ctxs[i];
        }
    }

    return NULL;
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

static void finalize_connected(struct bt_conn *conn)
{
    char address[GATEWAY_MAX_ADDRESS_LEN];

    format_address(bt_conn_get_dst(conn), address, sizeof(address));

    emit_gatt_debug(
        "connected_ready",
        address,
        "",
        0,
        bt_gatt_get_mtu(conn),
        0,
        0,
        false
    );

    if (g_callbacks.on_connected != NULL) {
        g_callbacks.on_connected(address, (uint16_t)bt_conn_index(conn));
    }
}

static void mtu_exchange_cb(
    struct bt_conn *conn,
    uint8_t err,
    struct bt_gatt_exchange_params *params
)
{
    active_conn_t *entry = find_active_conn_by_conn(conn);
    char address[GATEWAY_MAX_ADDRESS_LEN];

    ARG_UNUSED(params);

    if (entry != NULL) {
        entry->mtu_exchange_in_progress = false;
    }

    format_address(bt_conn_get_dst(conn), address, sizeof(address));

    emit_gatt_debug(
        "mtu_exchange_complete",
        address,
        "",
        0,
        bt_gatt_get_mtu(conn),
        0,
        err == 0 ? 0 : -(int)err,
        false
    );

    finalize_connected(conn);
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

static void release_subscribe_ctxs_for_address(const char *address)
{
    if (address == NULL) {
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_subscribe_ctxs); i++) {
        if (g_subscribe_ctxs[i].used &&
            strcmp(g_subscribe_ctxs[i].address, address) == 0) {
            memset(&g_subscribe_ctxs[i], 0, sizeof(g_subscribe_ctxs[i]));
        }
    }
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

static uint8_t discover_characteristic_cb(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    struct bt_gatt_discover_params *params
)
{
    gatt_discover_ctx_t *ctx =
        CONTAINER_OF(params, gatt_discover_ctx_t, params);

    ARG_UNUSED(conn);

    if (attr == NULL) {
        k_sem_give(&ctx->done);
        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        const struct bt_gatt_chrc *chrc =
            (const struct bt_gatt_chrc *)attr->user_data;

        if (chrc != NULL) {
            ctx->value_handle = chrc->value_handle;
            k_sem_give(&ctx->done);
            return BT_GATT_ITER_STOP;
        }
    }

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t discover_ccc_cb(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    struct bt_gatt_discover_params *params
)
{
    gatt_discover_ctx_t *ctx =
        CONTAINER_OF(params, gatt_discover_ctx_t, params);

    ARG_UNUSED(conn);

    if (attr == NULL) {
        k_sem_give(&ctx->done);
        return BT_GATT_ITER_STOP;
    }

    if (bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CCC) == 0) {
        ctx->ccc_handle = attr->handle;
        k_sem_give(&ctx->done);
        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_CONTINUE;
}

static void write_complete_cb(
    struct bt_conn *conn,
    uint8_t err,
    struct bt_gatt_write_params *params
)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(params);

    g_write_ctx.err = err == 0 ? 0 : -(int)err;
    k_sem_give(&g_write_ctx.done);
}

static uint8_t read_complete_cb(
    struct bt_conn *conn,
    uint8_t err,
    struct bt_gatt_read_params *params,
    const void *data,
    uint16_t length
)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(params);

    if (err != 0U) {
        g_read_ctx.err = -(int)err;
        k_sem_give(&g_read_ctx.done);
        return BT_GATT_ITER_STOP;
    }

    if (data == NULL) {
        if (g_read_ctx.data_len_in_out != NULL) {
            *g_read_ctx.data_len_in_out = g_read_ctx.bytes_copied;
        }
        g_read_ctx.err = 0;
        k_sem_give(&g_read_ctx.done);
        return BT_GATT_ITER_STOP;
    }

    if (g_read_ctx.data_out == NULL || g_read_ctx.data_len_in_out == NULL) {
        g_read_ctx.err = -1;
        k_sem_give(&g_read_ctx.done);
        return BT_GATT_ITER_STOP;
    }

    size_t capacity = *g_read_ctx.data_len_in_out;
    size_t remaining = capacity > g_read_ctx.bytes_copied
        ? capacity - g_read_ctx.bytes_copied
        : 0;
    size_t to_copy = length;

    if (to_copy > remaining) {
        to_copy = remaining;
    }

    memcpy(g_read_ctx.data_out + g_read_ctx.bytes_copied, data, to_copy);
    g_read_ctx.bytes_copied += to_copy;

    return BT_GATT_ITER_CONTINUE;
}

static int discover_characteristic_handle(
    struct bt_conn *conn,
    const char *characteristic_uuid,
    uint16_t *handle_out
)
{
    int rc;
    char address[GATEWAY_MAX_ADDRESS_LEN];

    if (conn == NULL || characteristic_uuid == NULL || handle_out == NULL) {
        return -1;
    }

    format_address(bt_conn_get_dst(conn), address, sizeof(address));

    memset(&g_discover_ctx, 0, sizeof(g_discover_ctx));
    k_sem_init(&g_discover_ctx.done, 0, 1);

    rc = bt_uuid_from_str(characteristic_uuid, &g_discover_ctx.uuid);
    if (rc < 0) {
        return rc;
    }

    g_discover_ctx.params.uuid = &g_discover_ctx.uuid.uuid;
    g_discover_ctx.params.func = discover_characteristic_cb;
    g_discover_ctx.params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    g_discover_ctx.params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    g_discover_ctx.params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    emit_gatt_debug(
        "discover_start",
        address,
        characteristic_uuid,
        0,
        bt_gatt_get_mtu(conn),
        0,
        0,
        false
    );

    rc = bt_gatt_discover(conn, &g_discover_ctx.params);
    if (rc != 0) {
        emit_gatt_debug(
            "discover_submit_failed",
            address,
            characteristic_uuid,
            0,
            bt_gatt_get_mtu(conn),
            0,
            rc,
            false
        );
        return rc;
    }

    if (k_sem_take(&g_discover_ctx.done, K_SECONDS(5)) != 0) {
        emit_gatt_debug(
            "discover_timeout",
            address,
            characteristic_uuid,
            0,
            bt_gatt_get_mtu(conn),
            0,
            -110,
            false
        );
        return -110;
    }

    if (g_discover_ctx.value_handle == 0) {
        emit_gatt_debug(
            "discover_characteristic",
            address,
            characteristic_uuid,
            0,
            bt_gatt_get_mtu(conn),
            0,
            -2,
            false
        );
        return -2;
    }

    *handle_out = g_discover_ctx.value_handle;
    emit_gatt_debug(
        "discover_characteristic",
        address,
        characteristic_uuid,
        *handle_out,
        bt_gatt_get_mtu(conn),
        0,
        0,
        false
    );
    return 0;
}

static int discover_ccc_handle(
    struct bt_conn *conn,
    const char *characteristic_uuid,
    uint16_t value_handle,
    uint16_t *ccc_handle_out
)
{
    int rc;
    char address[GATEWAY_MAX_ADDRESS_LEN];

    if (conn == NULL || characteristic_uuid == NULL || value_handle == 0 || ccc_handle_out == NULL) {
        return -1;
    }

    format_address(bt_conn_get_dst(conn), address, sizeof(address));

    memset(&g_discover_ctx, 0, sizeof(g_discover_ctx));
    k_sem_init(&g_discover_ctx.done, 0, 1);

    g_discover_ctx.params.uuid = BT_UUID_GATT_CCC;
    g_discover_ctx.params.func = discover_ccc_cb;
    g_discover_ctx.params.start_handle = value_handle + 1U;
    g_discover_ctx.params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    g_discover_ctx.params.type = BT_GATT_DISCOVER_DESCRIPTOR;

    emit_gatt_debug(
        "discover_ccc_start",
        address,
        characteristic_uuid,
        value_handle,
        bt_gatt_get_mtu(conn),
        0,
        0,
        false
    );

    rc = bt_gatt_discover(conn, &g_discover_ctx.params);
    if (rc != 0) {
        emit_gatt_debug(
            "discover_ccc_submit_failed",
            address,
            characteristic_uuid,
            value_handle,
            bt_gatt_get_mtu(conn),
            0,
            rc,
            false
        );
        return rc;
    }

    if (k_sem_take(&g_discover_ctx.done, K_SECONDS(5)) != 0) {
        emit_gatt_debug(
            "discover_ccc_timeout",
            address,
            characteristic_uuid,
            value_handle,
            bt_gatt_get_mtu(conn),
            0,
            -110,
            false
        );
        return -110;
    }

    if (g_discover_ctx.ccc_handle == 0) {
        emit_gatt_debug(
            "discover_ccc_missing",
            address,
            characteristic_uuid,
            value_handle,
            bt_gatt_get_mtu(conn),
            0,
            -2,
            false
        );
        return -2;
    }

    *ccc_handle_out = g_discover_ctx.ccc_handle;
    emit_gatt_debug(
        "discover_ccc_complete",
        address,
        characteristic_uuid,
        *ccc_handle_out,
        bt_gatt_get_mtu(conn),
        0,
        0,
        false
    );

    return 0;
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    char address[GATEWAY_MAX_ADDRESS_LEN];
    active_conn_t *entry = find_active_conn_by_conn(conn);
    int mtu_rc;

    format_address(bt_conn_get_dst(conn), address, sizeof(address));

    if (err != 0) {
        if (g_callbacks.on_disconnected != NULL) {
            g_callbacks.on_disconnected(address, -(int)err);
        }
        return;
    }

    if (entry == NULL) {
        finalize_connected(conn);
        return;
    }

    entry->mtu_params.func = mtu_exchange_cb;
    entry->mtu_exchange_in_progress = true;
    mtu_rc = bt_gatt_exchange_mtu(conn, &entry->mtu_params);

    if (mtu_rc == -EALREADY) {
        entry->mtu_exchange_in_progress = false;
        finalize_connected(conn);
        return;
    }

    if (mtu_rc != 0) {
        entry->mtu_exchange_in_progress = false;
        finalize_connected(conn);
    }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    char address[GATEWAY_MAX_ADDRESS_LEN];
    active_conn_t *entry = find_active_conn_by_conn(conn);

    format_address(bt_conn_get_dst(conn), address, sizeof(address));

    if (entry != NULL) {
        release_subscribe_ctxs_for_address(address);
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

    memset(g_subscribe_ctxs, 0, sizeof(g_subscribe_ctxs));

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

static uint8_t notify_cb(
    struct bt_conn *conn,
    struct bt_gatt_subscribe_params *params,
    const void *data,
    uint16_t length
)
{
    gatt_subscribe_ctx_t *ctx =
        CONTAINER_OF(params, gatt_subscribe_ctx_t, params);

    ARG_UNUSED(conn);

    if (data == NULL) {
        //ctx->used = false;
        memset(ctx, 0, sizeof(*ctx));
        return BT_GATT_ITER_STOP;
    }

    if (g_callbacks.on_notification != NULL) {
        g_callbacks.on_notification(
            ctx->address,
            ctx->characteristic_uuid,
            data,
            length,
            k_ticks_to_us_floor64(k_uptime_ticks())
        );
    }

    return BT_GATT_ITER_CONTINUE;
}

int ble_interface_subscribe(const char *address, const char *characteristic_uuid)
{
    active_conn_t *entry;
    gatt_subscribe_ctx_t *ctx;
    uint16_t value_handle = 0;
    uint16_t ccc_handle = 0;
    int rc;

    if (address == NULL || characteristic_uuid == NULL) {
        return -1;
    }

    entry = find_active_conn_by_address(address);
    if (entry == NULL || entry->conn == NULL) {
        return -3;
    }

    ctx = find_subscribe_ctx(address, characteristic_uuid);
    if (ctx != NULL) {
        emit_gatt_debug(
            "subscribe_already_active",
            address,
            characteristic_uuid,
            ctx->params.value_handle,
            bt_gatt_get_mtu(entry->conn),
            0,
            0,
            false
        );

        return 0;
    }

    ctx = allocate_subscribe_ctx(address, characteristic_uuid);
    if (ctx == NULL) {
        return -12;
    }

    rc = discover_characteristic_handle(
        entry->conn,
        characteristic_uuid,
        &value_handle
    );

    if (rc != 0) {
        emit_gatt_debug(
            "subscribe_failed_details",
            address,
            characteristic_uuid,
            ccc_handle,
            bt_gatt_get_mtu(entry->conn),
            value_handle,
            rc,
            false
        );
    }

    if (rc != 0) {
        emit_gatt_debug(
            "subscribe_discover_failed",
            address,
            characteristic_uuid,
            0,
            bt_gatt_get_mtu(entry->conn),
            0,
            rc,
            false
        );
        //ctx->used = false;
        memset(ctx, 0, sizeof(*ctx));
        return rc;
    }

    rc = discover_ccc_handle(
        entry->conn,
        characteristic_uuid,
        value_handle,
        &ccc_handle
    );

    if (rc != 0) {
        //ctx->used = false;
        memset(ctx, 0, sizeof(*ctx));
        return rc;
    }

    memset(&ctx->params, 0, sizeof(ctx->params));

    ctx->params.notify = notify_cb;
    ctx->params.value = BT_GATT_CCC_NOTIFY;
    ctx->params.value_handle = value_handle;
    ctx->params.ccc_handle = ccc_handle;

    emit_gatt_debug(
        "subscribe_start",
        address,
        characteristic_uuid,
        value_handle,
        bt_gatt_get_mtu(entry->conn),
        0,
        0,
        false
    );

    rc = bt_gatt_subscribe(entry->conn, &ctx->params);

    if (rc == -EALREADY) {
        rc = 0;
    }

    emit_gatt_debug(
        "subscribe_complete",
        address,
        characteristic_uuid,
        value_handle,
        bt_gatt_get_mtu(entry->conn),
        0,
        rc,
        false
    );

    if (rc != 0) {
        //ctx->used = false;
        memset(ctx, 0, sizeof(*ctx));
        return rc;
    }

    return 0;
}

int ble_interface_read(
    const char *address,
    const char *characteristic_uuid,
    uint8_t *data_out,
    size_t *data_len_in_out
)
{
    active_conn_t *entry;
    uint16_t handle;
    int rc;

    if (address == NULL || characteristic_uuid == NULL ||
        data_out == NULL || data_len_in_out == NULL ||
        *data_len_in_out == 0) {
        return -1;
    }

    entry = find_active_conn_by_address(address);
    if (entry == NULL || entry->conn == NULL) {
        return -3;
    }

    rc = discover_characteristic_handle(entry->conn, characteristic_uuid, &handle);
    if (rc != 0) {
        emit_gatt_debug(
            "read_discover_failed",
            address,
            characteristic_uuid,
            0,
            bt_gatt_get_mtu(entry->conn),
            0,
            rc,
            false
        );
        return rc;
    }

    emit_gatt_debug(
        "read_start",
        address,
        characteristic_uuid,
        handle,
        bt_gatt_get_mtu(entry->conn),
        *data_len_in_out,
        0,
        false
    );

    memset(&g_read_ctx, 0, sizeof(g_read_ctx));
    k_sem_init(&g_read_ctx.done, 0, 1);
    g_read_ctx.data_out = data_out;
    g_read_ctx.data_len_in_out = data_len_in_out;
    g_read_ctx.params.func = read_complete_cb;
    g_read_ctx.params.handle_count = 1;
    g_read_ctx.params.single.handle = handle;
    g_read_ctx.params.single.offset = 0U;

    rc = bt_gatt_read(entry->conn, &g_read_ctx.params);
    if (rc != 0) {
        emit_gatt_debug(
            "read_submit_failed",
            address,
            characteristic_uuid,
            handle,
            bt_gatt_get_mtu(entry->conn),
            *data_len_in_out,
            rc,
            false
        );
        return rc;
    }

    if (k_sem_take(&g_read_ctx.done, K_SECONDS(5)) != 0) {
        emit_gatt_debug(
            "read_timeout",
            address,
            characteristic_uuid,
            handle,
            bt_gatt_get_mtu(entry->conn),
            *data_len_in_out,
            -110,
            false
        );
        return -110;
    }

    emit_gatt_debug(
        "read_complete",
        address,
        characteristic_uuid,
        handle,
        bt_gatt_get_mtu(entry->conn),
        *data_len_in_out,
        g_read_ctx.err,
        false
    );
    return g_read_ctx.err;
}

int ble_interface_write(
    const char *address,
    const char *characteristic_uuid,
    const uint8_t *data,
    size_t data_len,
    bool without_response
)
{
    active_conn_t *entry;
    uint16_t handle;
    int rc;

    if (address == NULL || characteristic_uuid == NULL || data == NULL ||
        data_len == 0) {
        return -1;
    }

    entry = find_active_conn_by_address(address);
    if (entry == NULL || entry->conn == NULL) {
        return -3;
    }

    rc = discover_characteristic_handle(entry->conn, characteristic_uuid, &handle);
    if (rc != 0) {
        emit_gatt_debug(
            "write_discover_failed",
            address,
            characteristic_uuid,
            0,
            bt_gatt_get_mtu(entry->conn),
            data_len,
            rc,
            without_response
        );
        return rc;
    }

    emit_gatt_debug(
        "write_start",
        address,
        characteristic_uuid,
        handle,
        bt_gatt_get_mtu(entry->conn),
        data_len,
        0,
        without_response
    );

    if (without_response) {
        rc = bt_gatt_write_without_response(
            entry->conn,
            handle,
            data,
            (uint16_t)data_len,
            false
        );
        emit_gatt_debug(
            "write_complete",
            address,
            characteristic_uuid,
            handle,
            bt_gatt_get_mtu(entry->conn),
            data_len,
            rc,
            true
        );
        return rc;
    }

    memset(&g_write_ctx, 0, sizeof(g_write_ctx));
    k_sem_init(&g_write_ctx.done, 0, 1);
    g_write_ctx.params.handle = handle;
    g_write_ctx.params.offset = 0;
    g_write_ctx.params.data = data;
    g_write_ctx.params.length = (uint16_t)data_len;
    g_write_ctx.params.func = write_complete_cb;

    rc = bt_gatt_write(entry->conn, &g_write_ctx.params);
    if (rc != 0) {
        emit_gatt_debug(
            "write_submit_failed",
            address,
            characteristic_uuid,
            handle,
            bt_gatt_get_mtu(entry->conn),
            data_len,
            rc,
            false
        );
        return rc;
    }

    if (k_sem_take(&g_write_ctx.done, K_SECONDS(5)) != 0) {
        emit_gatt_debug(
            "write_timeout",
            address,
            characteristic_uuid,
            handle,
            bt_gatt_get_mtu(entry->conn),
            data_len,
            -110,
            false
        );
        return -110;
    }

    emit_gatt_debug(
        "write_complete",
        address,
        characteristic_uuid,
        handle,
        bt_gatt_get_mtu(entry->conn),
        data_len,
        g_write_ctx.err,
        false
    );
    return g_write_ctx.err;
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

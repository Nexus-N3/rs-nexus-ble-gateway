#include "gateway.h"
#include "../interface/gateway_interface.h"
#include "../ble/ble_interface.h"
#include "../ble/ble_scheduler.h"

#include <stddef.h>

static const char *request_id_or_null(const gateway_command_t *command)
{
    if (command == NULL || command->request_id[0] == '\0') {
        return NULL;
    }

    return command->request_id;
}

static void send_not_implemented(
    const gateway_command_t *command,
    const char *command_name
)
{
    gateway_interface_send_not_implemented(
        request_id_or_null(command),
        command_name
    );
}

static void on_gateway_command(const gateway_command_t *command)
{
    if (command == NULL) {
        return;
    }

    switch (command->type) {
    case GW_CMD_HELLO:
        gateway_interface_send_hello_ack(request_id_or_null(command));
        break;

    case GW_CMD_GET_STATUS:
        gateway_interface_send_status(request_id_or_null(command));
        break;

    case GW_CMD_SCAN_START:
        ble_scheduler_start_scan(
            request_id_or_null(command),
            command->timeout_ms
        );
        break;

    case GW_CMD_SCAN_STOP:
        ble_scheduler_stop_scan();
        break;

    case GW_CMD_CONNECT_ADDRESSES:
        send_not_implemented(command, "connect_addresses");
        break;

    case GW_CMD_DISCONNECT_ADDRESSES:
        send_not_implemented(command, "disconnect_addresses");
        break;

    case GW_CMD_SUBSCRIBE:
        send_not_implemented(command, "subscribe");
        break;

    case GW_CMD_UNSUBSCRIBE:
        send_not_implemented(command, "unsubscribe");
        break;

    case GW_CMD_GATT_WRITE:
        send_not_implemented(command, "gatt_write");
        break;

    case GW_CMD_GATT_READ:
        send_not_implemented(command, "gatt_read");
        break;

    case GW_CMD_DISCONNECT_ALL:
        send_not_implemented(command, "disconnect_all");
        break;

    default:
        gateway_interface_send_error(
            request_id_or_null(command),
            "unknown_command",
            -1
        );
        break;
    }
}

static void on_ble_sensor_found(const ble_discovered_sensor_t *sensor)
{
    ble_scheduler_on_sensor_found(sensor);
}

static void on_ble_connected(const char *address, uint16_t conn_handle)
{
    ble_scheduler_on_connected(address, conn_handle);
}

static void on_ble_disconnected(const char *address, int reason)
{
    ble_scheduler_on_disconnected(address, reason);
}

static void on_ble_notification(
    const char *address,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_time_us
)
{
    ble_scheduler_on_notification(address, payload, payload_len, gateway_time_us);
}

int gateway_app_init(void)
{
    gateway_interface_callbacks_t interface_callbacks = {
        .on_command = on_gateway_command,
    };

    ble_interface_callbacks_t ble_callbacks = {
        .on_sensor_found = on_ble_sensor_found,
        .on_connected = on_ble_connected,
        .on_disconnected = on_ble_disconnected,
        .on_notification = on_ble_notification,
    };

    int rc = gateway_interface_init(&interface_callbacks);
    if (rc != 0) {
        return rc;
    }

    rc = ble_interface_init(&ble_callbacks);
    if (rc != 0) {
        gateway_interface_send_error(NULL, "ble_init_failed", rc);
        return rc;
    }

    ble_scheduler_init();

    gateway_interface_send_ready();

    return 0;
}

void gateway_app_run_once(void)
{
    gateway_interface_poll();
    ble_scheduler_tick();
}
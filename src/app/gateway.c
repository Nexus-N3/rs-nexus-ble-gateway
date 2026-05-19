#include "gateway.h"
#include "../interface/gateway_interface.h"
#include "../ble/ble_interface.h"
#include "../ble/ble_scheduler.h"
#include "../hardware/led.h"

#include <stddef.h>
#include <stdio.h>


// Helper function to get the request ID from a command, or return NULL if it's not set.
static const char *request_id_or_null(const gateway_command_t *command)
{
    if (command == NULL || command->request_id[0] == '\0') {
        return NULL;
    }

    return command->request_id;
}

// Helper function to send a "not implemented" error for a given command and command name.
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

// Main handler for incoming gateway commands. This function is called by the gateway interface
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
        // turn on scan led
        led_on(APP_LED_SCAN);

        ble_scheduler_start_scan(
            request_id_or_null(command),
            command->timeout_ms
        );
        break;

    case GW_CMD_SCAN_STOP:
        // turn off scan led
        bool rc = is_scan_active();
        if(rc){
            led_off(APP_LED_SCAN);
            ble_scheduler_stop_scan();
        }
        break;

    case GW_CMD_CONNECT_ADDRESSES: {
        //scan may not be stopped? 
        led_off(APP_LED_SCAN);
        ble_scheduler_stop_scan();

        int rc = ble_scheduler_connect_addresses(
            request_id_or_null(command),
            command->sensors,
            command->sensor_count
        );

        if (rc != 0) {
            gateway_interface_send_error(
                request_id_or_null(command),
                rc == -3 ? "sensor_not_found" : "connect_addresses_failed",
                rc
            );
        }

        break;
    }

    case GW_CMD_DISCONNECT_ADDRESSES: {
        int rc = ble_scheduler_disconnect_addresses(
            request_id_or_null(command),
            command->addresses,
            command->address_count
        );

        if (rc != 0) {
            gateway_interface_send_error(
                request_id_or_null(command),
                "disconnect_addresses_failed",
                rc
            );
        }

        break;
}

    case GW_CMD_SUBSCRIBE:
    {
        int rc = ble_scheduler_subscribe(
            request_id_or_null(command),
            command->address,
            command->characteristic_uuid,
            command->binary_notifications
        );

        if (rc != 0) {
            gateway_interface_send_error(
                request_id_or_null(command),
                "subscribe_failed",
                rc
            );
        }

        break;
    }

    case GW_CMD_UNSUBSCRIBE:
    {
        int rc = ble_scheduler_unsubscribe(
            command->address,
            command->characteristic_uuid
        );

        if (rc != 0) {
            gateway_interface_send_error(
                request_id_or_null(command),
                "unsubscribe_failed",
                rc
            );
        } else {
            char line[256];

            snprintf(
                line,
                sizeof(line),
                "{\"type\":\"unsubscribe_complete\","
                "\"request_id\":\"%s\","
                "\"address\":\"%s\","
                "\"characteristic_uuid\":\"%s\","
                "\"ok\":true}",
                command->request_id,
                command->address,
                command->characteristic_uuid
            );

            gateway_interface_send_json_line(line);
        }

        break;
    }

    case GW_CMD_GATT_WRITE:
    {
        int rc = ble_scheduler_gatt_write(
                request_id_or_null(command),
                command->address,
                command->characteristic_uuid,
                command->payload,
                command->payload_len,
                command->without_response
            );

        if (rc != 0) {
            gateway_interface_send_error(
                request_id_or_null(command),
                "gatt_write_failed",
                rc
            );
        }
        break;
    }

    case GW_CMD_GATT_READ:
    {
        int rc = ble_scheduler_gatt_read(
            request_id_or_null(command),
            command->address,
            command->characteristic_uuid
        );

        if (rc != 0) {
            gateway_interface_send_error(
                request_id_or_null(command),
                "gatt_read_failed",
                rc
            );
        }
        break;
    }

    case GW_CMD_DISCONNECT_ALL:
        if (ble_scheduler_disconnect_all() != 0) {
            gateway_interface_send_error(
                request_id_or_null(command),
                "disconnect_all_failed",
                -1
            );
        }
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

//callbacks for BLE events, which simply forward the events to the BLE scheduler. 
//The scheduler will handle the events and update its internal state accordingly.

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
    const char *characteristic_uuid,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_time_us
)
{
    ble_scheduler_on_notification(address, characteristic_uuid, payload, payload_len, gateway_time_us);
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

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../ble/ble_types.h"
//#include "../config/gateway_config.h"  //isnt this what config is for instead of reaching into ble types?

// commands that the gateway accepts from the host
typedef enum {
    GW_CMD_NONE = 0,
    GW_CMD_HELLO,
    GW_CMD_SCAN_START,
    GW_CMD_SCAN_STOP,
    GW_CMD_CONNECT_ADDRESSES,
    GW_CMD_DISCONNECT_ADDRESSES,
    GW_CMD_DISCONNECT_ALL,
    GW_CMD_SUBSCRIBE,
    GW_CMD_UNSUBSCRIBE,
    GW_CMD_GATT_WRITE,
    GW_CMD_GATT_READ,
    GW_CMD_GET_STATUS,
    GW_CMD_RESET_SESSION,

    GW_CMD_RF_SURVEY_START,
    GW_CMD_RF_SURVEY_STATUS,
    GW_CMD_RF_SURVEY_STOP,
} gateway_command_type_t;

//events that the gateway emits
//not every event goes through here for example send hello or scan result
//these events are more high level and simple.
typedef enum {
    GW_EVT_READY = 0,
    GW_EVT_SCAN_RESULT,
    GW_EVT_SCAN_COMPLETE,
    GW_EVT_SENSOR_CONNECTED,
    GW_EVT_SENSOR_DISCONNECTED,
    GW_EVT_SUBSCRIBE_COMPLETE,
    GW_EVT_WRITE_COMPLETE,
    GW_EVT_READ_RESULT,
    GW_EVT_NOTIFICATION,
    GW_EVT_STATUS,
    GW_EVT_ERROR,
    GW_EVT_SENSOR_FOUND,
    GW_EVT_SENSOR_CONFIGURED,
    GW_EVT_SENSOR_STREAMING,
    GW_EVT_SENSOR_FRAME,
    GW_EVT_SENSOR_HEALTH,
    GW_EVT_SESSION_READY,
    GW_EVT_SESSION_FAILED,
} gateway_event_type_t;

// data structure definitions 
typedef struct {
    char address[GATEWAY_MAX_ADDRESS_LEN];
    char sensor_key[GATEWAY_MAX_SENSOR_KEY_LEN];
    char sensor_type_name[GATEWAY_MAX_SENSOR_NAME_LEN];
} gateway_connect_sensor_t;

typedef struct {
    gateway_command_type_t type;
    char request_id[GATEWAY_MAX_REQUEST_ID_LEN];
    uint32_t timeout_ms;
    uint32_t window_ms; // rf survey
    uint32_t duration_ms; // rf survey
    bool without_response;
    bool binary_notifications;
    char address[GATEWAY_MAX_ADDRESS_LEN];
    char characteristic_uuid[GATEWAY_MAX_UUID_LEN];
    uint8_t payload[GATEWAY_MAX_FRAME_PAYLOAD];
    uint16_t payload_len;
    uint8_t address_count;
    char addresses[GATEWAY_MAX_SENSORS][GATEWAY_MAX_ADDRESS_LEN];
    uint8_t sensor_count;
    gateway_connect_sensor_t sensors[GATEWAY_MAX_SENSORS];
} gateway_command_t;

typedef struct {
    gateway_event_type_t type;
    char request_id[GATEWAY_MAX_REQUEST_ID_LEN];
    int error_code;
    const char *message;
    char address[GATEWAY_MAX_ADDRESS_LEN];
    char characteristic_uuid[GATEWAY_MAX_UUID_LEN];
    bool ok;
    uint8_t payload[GATEWAY_MAX_FRAME_PAYLOAD];
    uint16_t payload_len;
    uint64_t gateway_timestamp_us;
    gateway_sensor_t sensor;
    gateway_sensor_frame_t frame;
} gateway_event_t;

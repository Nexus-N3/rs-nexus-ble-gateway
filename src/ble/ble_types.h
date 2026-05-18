#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../config/gateway_config.h"

typedef enum {
    SENSOR_TYPE_UNKNOWN = 0,
    SENSOR_TYPE_MOVELLA_DOT,
    SENSOR_TYPE_MOVESENSE,
} sensor_type_t;

typedef enum {
    SENSOR_STATE_IDLE = 0,
    SENSOR_STATE_DISCOVERING,
    SENSOR_STATE_FOUND,
    SENSOR_STATE_CONNECTING,
    SENSOR_STATE_CONNECTED,
    SENSOR_STATE_DISCOVERING_GATT,
    SENSOR_STATE_CONFIGURING,
    SENSOR_STATE_SUBSCRIBING,
    SENSOR_STATE_READY,
    SENSOR_STATE_STREAMING,
    SENSOR_STATE_UNSTABLE,
    SENSOR_STATE_DISCONNECTING,
    SENSOR_STATE_DISCONNECTED,
    SENSOR_STATE_FAILED,
} sensor_state_t;

typedef struct {
    char address[GATEWAY_MAX_ADDRESS_LEN];
    int8_t rssi;
    sensor_type_t sensor_type;
    char name[32];
    uint8_t service_uuid_count;
    char service_uuids[GATEWAY_MAX_SERVICE_UUIDS][GATEWAY_MAX_UUID_LEN];
} ble_discovered_sensor_t;

typedef struct {
    char address[GATEWAY_MAX_ADDRESS_LEN];
    sensor_type_t sensor_type;
    char location[32];

    sensor_state_t state;

    uint16_t conn_handle;
    int8_t rssi;

    uint32_t expected_rate_hz;
    uint32_t frames_received;
    uint32_t dropped_estimate;
    uint32_t gap_events;

    uint64_t last_frame_gateway_time_us;
    uint64_t first_frame_gateway_time_us;

    bool is_required;
    bool is_connected;
    bool is_streaming;
} gateway_sensor_t;

typedef struct {
    char address[GATEWAY_MAX_ADDRESS_LEN];
    sensor_type_t sensor_type;
    uint64_t gateway_time_us;
    uint32_t sensor_time_us;
    uint32_t sequence;
    uint16_t payload_len;
    uint8_t payload[GATEWAY_MAX_FRAME_PAYLOAD];
} gateway_sensor_frame_t;

typedef struct {
    sensor_type_t sensor_type;
    uint8_t count;
    uint32_t expected_rate_hz;
} gateway_sensor_requirement_t;

typedef struct {
    char session_id[32];
    uint8_t requirement_count;
    gateway_sensor_requirement_t requirements[GATEWAY_MAX_SENSOR_TYPES];
} gateway_session_config_t;

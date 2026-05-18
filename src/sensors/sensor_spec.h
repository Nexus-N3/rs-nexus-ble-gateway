#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../ble/ble_types.h"

typedef struct {
    const char *uuid;
    const char *name;
} sensor_uuid_ref_t;

typedef struct {
    const char *name_contains;
    const char *service_uuid_hint;
} sensor_advertising_match_t;

typedef struct {
    const char *service_uuid;
    const char *characteristic_uuid;
    bool notify;
    bool write;
    bool write_without_response;
    bool read;
} sensor_characteristic_spec_t;

typedef struct {
    const char *name;
    const uint8_t *data;
    size_t data_len;
    const char *target_characteristic_uuid;
    bool write_without_response;
} sensor_setup_command_t;

typedef struct {
    sensor_type_t sensor_type;
    const char *sensor_type_name;

    sensor_advertising_match_t advertising;

    const sensor_characteristic_spec_t *characteristics;
    size_t characteristic_count;

    const sensor_setup_command_t *setup_commands;
    size_t setup_command_count;

    const char *notify_characteristic_uuid;
    const char *control_characteristic_uuid;

    uint32_t default_rate_hz;
    bool forward_raw_payload;
} sensor_spec_t;

const sensor_spec_t *sensor_spec_get(sensor_type_t sensor_type);
bool sensor_spec_matches_advertisement(
    const sensor_spec_t *spec,
    const char *name,
    const char **service_uuids,
    size_t service_uuid_count
);
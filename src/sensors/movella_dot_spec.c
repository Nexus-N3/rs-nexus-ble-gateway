#include "movella_dot_spec.h"

#define MOVELLA_DOT_BATTERY_UUID        "15173001-4947-11e9-8646-d663bd873d93"
#define MOVELLA_DOT_DEVICE_CONTROL_UUID "15171002-4947-11e9-8646-d663bd873d93"
#define MOVELLA_DOT_DEVICE_REPORT_UUID  "15171004-4947-11e9-8646-d663bd873d93"
#define MOVELLA_DOT_MEASUREMENT_UUID    "15172001-4947-11e9-8646-d663bd873d93"
#define MOVELLA_DOT_PAYLOAD_SHORT_UUID  "15172004-4947-11e9-8646-d663bd873d93"
#define MOVELLA_DOT_PAYLOAD_MEDIUM_UUID "15172003-4947-11e9-8646-d663bd873d93"
#define MOVELLA_DOT_PAYLOAD_LONG_UUID   "15172002-4947-11e9-8646-d663bd873d93"

static const sensor_characteristic_spec_t movella_dot_characteristics[] = {
    {
        .service_uuid = "15173000-4947-11e9-8646-d663bd873d93",
        .characteristic_uuid = MOVELLA_DOT_BATTERY_UUID,
        .notify = true,
        .write = false,
        .write_without_response = false,
        .read = true,
    },
    {
        .service_uuid = "15171000-4947-11e9-8646-d663bd873d93",
        .characteristic_uuid = MOVELLA_DOT_DEVICE_CONTROL_UUID,
        .notify = false,
        .write = true,
        .write_without_response = true,
        .read = true,
    },
    {
        .service_uuid = "15171000-4947-11e9-8646-d663bd873d93",
        .characteristic_uuid = MOVELLA_DOT_DEVICE_REPORT_UUID,
        .notify = true,
        .write = false,
        .write_without_response = false,
        .read = false,
    },
    {
        .service_uuid = "15172000-4947-11e9-8646-d663bd873d93",
        .characteristic_uuid = MOVELLA_DOT_MEASUREMENT_UUID,
        .notify = false,
        .write = true,
        .write_without_response = true,
        .read = false,
    },
    {
        .service_uuid = "15172000-4947-11e9-8646-d663bd873d93",
        .characteristic_uuid = MOVELLA_DOT_PAYLOAD_SHORT_UUID,
        .notify = true,
        .write = false,
        .write_without_response = false,
        .read = false,
    },
    {
        .service_uuid = "15172000-4947-11e9-8646-d663bd873d93",
        .characteristic_uuid = MOVELLA_DOT_PAYLOAD_MEDIUM_UUID,
        .notify = true,
        .write = false,
        .write_without_response = false,
        .read = false,
    },
    {
        .service_uuid = "15172000-4947-11e9-8646-d663bd873d93",
        .characteristic_uuid = MOVELLA_DOT_PAYLOAD_LONG_UUID,
        .notify = true,
        .write = false,
        .write_without_response = false,
        .read = false,
    },
};

static const uint8_t movella_dot_identify[] = {
    0x01, 0x01, 0x02,
};

static const uint8_t movella_dot_set_rate_20[] = {
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B,
    0x4D, 0x6F, 0x76, 0x65, 0x6C, 0x6C, 0x61, 0x20,
    0x44, 0x4F, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t movella_dot_set_rate_60[] = {
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B,
    0x4D, 0x6F, 0x76, 0x65, 0x6C, 0x6C, 0x61, 0x20,
    0x44, 0x4F, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t movella_dot_start_stream[] = {
    0x01, 0x01, 0x1A,
};

static const uint8_t movella_dot_stop_stream[] = {
    0x01, 0x00, 0x1A,
};

static const sensor_setup_command_t movella_dot_setup_commands[] = {
    {
        .name = "identify",
        .data = movella_dot_identify,
        .data_len = sizeof(movella_dot_identify),
        .target_characteristic_uuid = MOVELLA_DOT_DEVICE_CONTROL_UUID,
        .write_without_response = true,
    },
    {
        .name = "set_rate_20",
        .data = movella_dot_set_rate_20,
        .data_len = sizeof(movella_dot_set_rate_20),
        .target_characteristic_uuid = MOVELLA_DOT_DEVICE_CONTROL_UUID,
        .write_without_response = true,
    },
    {
        .name = "set_rate_60",
        .data = movella_dot_set_rate_60,
        .data_len = sizeof(movella_dot_set_rate_60),
        .target_characteristic_uuid = MOVELLA_DOT_DEVICE_CONTROL_UUID,
        .write_without_response = true,
    },
    {
        .name = "start_stream",
        .data = movella_dot_start_stream,
        .data_len = sizeof(movella_dot_start_stream),
        .target_characteristic_uuid = MOVELLA_DOT_MEASUREMENT_UUID,
        .write_without_response = true,
    },
    {
        .name = "stop_stream",
        .data = movella_dot_stop_stream,
        .data_len = sizeof(movella_dot_stop_stream),
        .target_characteristic_uuid = MOVELLA_DOT_MEASUREMENT_UUID,
        .write_without_response = true,
    },
};

static const sensor_spec_t movella_dot_spec = {
    .sensor_type = SENSOR_TYPE_MOVELLA_DOT,
    .sensor_type_name = "movella_dot",
    .advertising = {
        .name_contains = "Movella DOT",
        .service_uuid_hint = NULL,
    },
    .characteristics = movella_dot_characteristics,
    .characteristic_count = sizeof(movella_dot_characteristics) /
                            sizeof(movella_dot_characteristics[0]),
    .setup_commands = movella_dot_setup_commands,
    .setup_command_count = sizeof(movella_dot_setup_commands) /
                           sizeof(movella_dot_setup_commands[0]),
    .notify_characteristic_uuid = MOVELLA_DOT_PAYLOAD_LONG_UUID,
    .control_characteristic_uuid = MOVELLA_DOT_DEVICE_CONTROL_UUID,
    .default_rate_hz = 60,
    .forward_raw_payload = true,
};

const sensor_spec_t *movella_dot_get_spec(void)
{
    return &movella_dot_spec;
}

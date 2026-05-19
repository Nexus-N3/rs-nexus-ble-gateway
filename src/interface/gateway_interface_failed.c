#include "gateway_interface.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../hardware/led.h"

#define RX_LINE_MAX 256
#define TX_RING_SIZE 8192

static const struct device *uart_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static gateway_interface_callbacks_t g_callbacks;

static char rx_line[RX_LINE_MAX];
static size_t rx_len;

static char pending_line[RX_LINE_MAX];
static volatile bool pending_line_ready;

static uint8_t tx_ring[TX_RING_SIZE];
static volatile size_t tx_head;
static volatile size_t tx_tail;
static volatile size_t tx_count;
static volatile uint32_t tx_drop_count;

static void tx_kick(void)
{
    uart_irq_tx_enable(uart_dev);
}

static int tx_enqueue_byte(uint8_t byte)
{
    unsigned int key = irq_lock();

    if (tx_count >= TX_RING_SIZE) {
        tx_drop_count++;
        irq_unlock(key);
        return -1;
    }

    tx_ring[tx_tail] = byte;
    tx_tail = (tx_tail + 1U) % TX_RING_SIZE;
    tx_count++;

    irq_unlock(key);
    tx_kick();
    return 0;
}

static int tx_enqueue_bytes(const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return -1;
    }

    for (size_t i = 0; i < len; i++) {
        if (tx_enqueue_byte(data[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

static void transport_write_str(const char *s)
{
    if (s == NULL) {
        return;
    }

    while (*s != '\0') {
        if (tx_enqueue_byte((uint8_t)*s++) != 0) {
            return;
        }
    }
}

static void transport_write_bytes(const uint8_t *data, size_t len)
{
    (void)tx_enqueue_bytes(data, len);
}

int gateway_interface_send_scan_result(
    const char *request_id,
    const char *address,
    const char *name,
    int rssi
)
{
    char line[256];

    snprintf(
        line,
        sizeof(line),
        "{\"type\":\"scan_result\","
        "\"request_id\":\"%s\","
        "\"address\":\"%s\","
        "\"name\":\"%s\","
        "\"rssi\":%d,"
        "\"service_uuids\":[]}",
        request_id != NULL ? request_id : "",
        address != NULL ? address : "",
        name != NULL ? name : "",
        rssi
    );

    return gateway_interface_send_json_line(line);
}

int gateway_interface_send_scan_complete(const char *request_id)
{
    char line[128];

    snprintf(
        line,
        sizeof(line),
        "{\"type\":\"scan_complete\","
        "\"request_id\":\"%s\"}",
        request_id != NULL ? request_id : ""
    );

    return gateway_interface_send_json_line(line);
}

static uint32_t extract_uint32_field(
    const char *line,
    const char *field_name,
    uint32_t default_value
)
{
    char key[48];
    const char *p;

    snprintf(key, sizeof(key), "\"%s\"", field_name);

    p = strstr(line, key);
    if (p == NULL) {
        return default_value;
    }

    p = strchr(p, ':');
    if (p == NULL) {
        return default_value;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    return (uint32_t)strtoul(p, NULL, 10);
}

static bool extract_bool_field(
    const char *line,
    const char *field_name,
    bool default_value
)
{
    char key[48];
    const char *p;

    snprintf(key, sizeof(key), "\"%s\"", field_name);

    p = strstr(line, key);
    if (p == NULL) {
        return default_value;
    }

    p = strchr(p, ':');
    if (p == NULL) {
        return default_value;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (strncmp(p, "true", 4) == 0) {
        return true;
    }

    if (strncmp(p, "false", 5) == 0) {
        return false;
    }

    return default_value;
}

static void extract_string_field(
    const char *line,
    const char *field_name,
    char *out,
    size_t out_size
)
{
    char key[48];
    const char *p;
    const char *end;
    size_t len;

    if (out == NULL || out_size == 0) {
        return;
    }

    out[0] = '\0';

    snprintf(key, sizeof(key), "\"%s\"", field_name);
    p = strstr(line, key);
    if (p == NULL) {
        return;
    }

    p = strchr(p, ':');
    if (p == NULL) {
        return;
    }

    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p != '"') {
        return;
    }

    p++;
    end = strchr(p, '"');
    if (end == NULL) {
        return;
    }

    len = (size_t)(end - p);
    if (len >= out_size) {
        len = out_size - 1;
    }

    memcpy(out, p, len);
    out[len] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static void extract_hex_payload_field(
    const char *line,
    const char *field_name,
    uint8_t *out,
    uint16_t *out_len,
    uint16_t max_len
)
{
    char hex[2 * GATEWAY_MAX_FRAME_PAYLOAD + 1];
    size_t hex_len;
    uint16_t byte_len = 0;

    if (out == NULL || out_len == NULL) {
        return;
    }

    *out_len = 0;
    extract_string_field(line, field_name, hex, sizeof(hex));

    hex_len = strlen(hex);
    if (hex_len == 0 || (hex_len % 2) != 0) {
        return;
    }

    for (size_t i = 0; i + 1 < hex_len && byte_len < max_len; i += 2) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1]);

        if (hi < 0 || lo < 0) {
            *out_len = 0;
            return;
        }

        out[byte_len++] = (uint8_t)((hi << 4) | lo);
    }

    *out_len = byte_len;
}

static void extract_request_id(const char *line, char *out, size_t out_size)
{
    const char *key = "\"request_id\"";
    const char *p;

    if (out_size == 0) {
        return;
    }

    out[0] = '\0';

    p = strstr(line, key);
    if (p == NULL) {
        return;
    }

    p += strlen(key);

    p = strchr(p, ':');
    if (p == NULL) {
        return;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p != '"') {
        return;
    }

    p++;

    const char *end = strchr(p, '"');
    if (end == NULL) {
        return;
    }

    size_t len = (size_t)(end - p);
    if (len >= out_size) {
        len = out_size - 1;
    }

    memcpy(out, p, len);
    out[len] = '\0';
}

static int json_type_is(const char *line, const char *type)
{
    char compact[64];
    char spaced[64];

    snprintf(compact, sizeof(compact), "\"type\":\"%s\"", type);
    snprintf(spaced, sizeof(spaced), "\"type\": \"%s\"", type);

    return strstr(line, compact) != NULL || strstr(line, spaced) != NULL;
}

static void extract_address_array(
    const char *line,
    const char *field_name,
    char out[][GATEWAY_MAX_ADDRESS_LEN],
    uint8_t *out_count,
    uint8_t max_count
)
{
    char key[48];
    const char *p;
    uint8_t count = 0;

    if (out_count == NULL || max_count == 0) {
        return;
    }

    *out_count = 0;

    snprintf(key, sizeof(key), "\"%s\"", field_name);

    p = strstr(line, key);
    if (p == NULL) {
        return;
    }

    p = strchr(p, '[');
    if (p == NULL) {
        return;
    }

    p++;

    while (*p != '\0' && *p != ']' && count < max_count) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }

        if (*p != '"') {
            break;
        }

        p++;

        const char *end = strchr(p, '"');
        if (end == NULL) {
            break;
        }

        size_t len = (size_t)(end - p);
        if (len >= GATEWAY_MAX_ADDRESS_LEN) {
            len = GATEWAY_MAX_ADDRESS_LEN - 1;
        }

        memcpy(out[count], p, len);
        out[count][len] = '\0';

        count++;
        p = end + 1;
    }

    *out_count = count;
}

//
static void parse_command_line(const char *line, gateway_command_t *command)
{
    memset(command, 0, sizeof(*command));
    command->type = GW_CMD_NONE;

    extract_request_id(line, command->request_id, sizeof(command->request_id));

    if (json_type_is(line, "hello")) {
        command->type = GW_CMD_HELLO;
        return;
    }

    if (json_type_is(line, "get_status")) {
        command->type = GW_CMD_GET_STATUS;
        return;
    }

    if (json_type_is(line, "scan_start")) {
        command->type = GW_CMD_SCAN_START;
        command->timeout_ms = extract_uint32_field(
            line,
            "timeout_ms",
            5000
        );
        return;
    }

    if (json_type_is(line, "scan_stop")) {
        command->type = GW_CMD_SCAN_STOP;
        return;
    }

    if (json_type_is(line, "connect_addresses")) {
        command->type = GW_CMD_CONNECT_ADDRESSES;

        extract_address_array(
            line,
            "addresses",
            command->addresses,
            &command->address_count,
            GATEWAY_MAX_SENSORS
        );

        command->sensor_count = command->address_count;

        for (uint8_t i = 0; i < command->address_count; i++) {
            strncpy(
                command->sensors[i].address,
                command->addresses[i],
                sizeof(command->sensors[i].address) - 1
            );
        }

        return;
    }

    if (json_type_is(line, "disconnect_addresses")) {
        command->type = GW_CMD_DISCONNECT_ADDRESSES;

        extract_address_array(
            line,
            "addresses",
            command->addresses,
            &command->address_count,
            GATEWAY_MAX_SENSORS
        );

        return;
    }

    if (json_type_is(line, "disconnect_all")) {
        command->type = GW_CMD_DISCONNECT_ALL;
        return;
    }

    if (json_type_is(line, "subscribe")) {
        command->type = GW_CMD_SUBSCRIBE;
        extract_string_field(
            line,
            "address",
            command->address,
            sizeof(command->address)
        );
        extract_string_field(
            line,
            "characteristic_uuid",
            command->characteristic_uuid,
            sizeof(command->characteristic_uuid)
        );
        command->binary_notifications = extract_bool_field(
            line,
            "binary_notifications",
            false
        );
        return;
    }

    if (json_type_is(line, "unsubscribe")) {
        command->type = GW_CMD_UNSUBSCRIBE;
        extract_string_field(
            line,
            "address",
            command->address,
            sizeof(command->address)
        );
        extract_string_field(
            line,
            "characteristic_uuid",
            command->characteristic_uuid,
            sizeof(command->characteristic_uuid)
        );
        return;
    }

    if (json_type_is(line, "gatt_write")) {
        command->type = GW_CMD_GATT_WRITE;
        extract_string_field(
            line,
            "address",
            command->address,
            sizeof(command->address)
        );
        extract_string_field(
            line,
            "characteristic_uuid",
            command->characteristic_uuid,
            sizeof(command->characteristic_uuid)
        );
        command->without_response = extract_bool_field(
            line,
            "without_response",
            false
        );
        extract_hex_payload_field(
            line,
            "payload_hex",
            command->payload,
            &command->payload_len,
            sizeof(command->payload)
        );
        return;
    }

    if (json_type_is(line, "gatt_read")) {
        command->type = GW_CMD_GATT_READ;
        extract_string_field(
            line,
            "address",
            command->address,
            sizeof(command->address)
        );
        extract_string_field(
            line,
            "characteristic_uuid",
            command->characteristic_uuid,
            sizeof(command->characteristic_uuid)
        );
        return;
    }
}

static void uart_cb(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t buf[32];
        int len = uart_fifo_read(dev, buf, sizeof(buf));

        for (int i = 0; i < len; i++) {
            unsigned char c = buf[i];

            if (c == '\r') {
                continue;
            }

            if (c == '\n') {
                rx_line[rx_len] = '\0';

                if (rx_len > 0 && !pending_line_ready) {
                    memcpy(pending_line, rx_line, rx_len + 1);
                    pending_line_ready = true;
                }

                rx_len = 0;
                continue;
            }

            if (rx_len < RX_LINE_MAX - 1) {
                rx_line[rx_len++] = (char)c;
            } else {
                rx_len = 0;
            }
        }
    }

    while (uart_irq_update(dev) && uart_irq_tx_ready(dev)) {
        uint8_t buf[64];
        int len = 0;
        unsigned int key = irq_lock();

        while (tx_count > 0 && len < (int)sizeof(buf)) {
            buf[len++] = tx_ring[tx_head];
            tx_head = (tx_head + 1U) % TX_RING_SIZE;
            tx_count--;
        }

        irq_unlock(key);

        if (len > 0) {
            (void)uart_fifo_fill(dev, buf, len);
        }

        if (tx_count == 0) {
            uart_irq_tx_disable(dev);
            break;
        }
    }
}

int gateway_interface_init(const gateway_interface_callbacks_t *callbacks)
{
    if (callbacks != NULL) {
        g_callbacks = *callbacks;
    }

    if (!device_is_ready(uart_dev)) {
        return -1;
    }

    tx_head = 0;
    tx_tail = 0;
    tx_count = 0;
    tx_drop_count = 0;

    uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    return 0;
}

//This function should be called periodically from the main loop to process incoming commands. 
//It checks if a complete line of input has been received, parses it as a command, and 
//invokes the appropriate callback.
int gateway_interface_poll(void)
{
    char line[RX_LINE_MAX];

    if (!pending_line_ready) {
        return 0;
    }

    unsigned int key = irq_lock();

    if (!pending_line_ready) {
        irq_unlock(key);
        return 0;
    }

    memcpy(line, pending_line, sizeof(line));
    pending_line_ready = false;

    irq_unlock(key);

    gateway_command_t command;
    parse_command_line(line, &command);

    // Invoike the callback that is listening on gateway.c
    if (g_callbacks.on_command != NULL) {
        g_callbacks.on_command(&command);
    }

    return 0;
}

int gateway_interface_send_event(const gateway_event_t *event)
{
    if (event == NULL) {
        return -1;
    }

    return 0;
}

int gateway_interface_send_frame(const gateway_sensor_frame_t *frame)
{
    if (frame == NULL) {
        return -1;
    }

    return 0;
}

int gateway_interface_send_log(const char *message)
{
    if (message == NULL) {
        return -1;
    }

    return gateway_interface_send_json_line(message);
}

int gateway_interface_send_ready(void)
{
    return gateway_interface_send_json_line(
        "{\"type\":\"ready\","
        "\"protocol_version\":1,"
        "\"device\":\"nrf54l15-dk\","
        "\"app\":\"rs-nexus-ble-gateway\"}"
    );
}

int gateway_interface_send_hello_ack(const char *request_id)
{
    char line[192];

    if (request_id != NULL && request_id[0] != '\0') {
        snprintf(
            line,
            sizeof(line),
            "{\"type\":\"hello_ack\","
            "\"request_id\":\"%s\","
            "\"ok\":true,"
            "\"protocol_version\":1}",
            request_id
        );
    } else {
        snprintf(
            line,
            sizeof(line),
            "{\"type\":\"hello_ack\","
            "\"ok\":true,"
            "\"protocol_version\":1}"
        );
    }

    return gateway_interface_send_json_line(line);
}

int gateway_interface_send_status(const char *request_id)
{
    char line[256];

    if (request_id != NULL && request_id[0] != '\0') {
        snprintf(
            line,
            sizeof(line),
            "{\"type\":\"status\","
            "\"request_id\":\"%s\","
            "\"gateway\":{"
            "\"state\":\"ready\","
            "\"backend\":\"stub\","
            "\"connected_count\":0"
            "}}",
            request_id
        );
    } else {
        snprintf(
            line,
            sizeof(line),
            "{\"type\":\"status\","
            "\"gateway\":{"
            "\"state\":\"ready\","
            "\"backend\":\"stub\","
            "\"connected_count\":0"
            "}}"
        );
    }

    return gateway_interface_send_json_line(line);
}

int gateway_interface_send_error(
    const char *request_id,
    const char *message,
    int error_code
)
{
    char line[256];

    if (message == NULL) {
        return -1;
    }

    if (request_id != NULL && request_id[0] != '\0') {
        snprintf(
            line,
            sizeof(line),
            "{\"type\":\"error\","
            "\"request_id\":\"%s\","
            "\"error_code\":%d,"
            "\"message\":\"%s\"}",
            request_id,
            error_code,
            message
        );
    } else {
        snprintf(
            line,
            sizeof(line),
            "{\"type\":\"error\","
            "\"error_code\":%d,"
            "\"message\":\"%s\"}",
            error_code,
            message
        );
    }

    return gateway_interface_send_json_line(line);
}

int gateway_interface_send_not_implemented(
    const char *request_id,
    const char *command_name
)
{
    char message[128];

    if (command_name == NULL) {
        return gateway_interface_send_error(
            request_id,
            "not_implemented",
            -2
        );
    }

    snprintf(
        message,
        sizeof(message),
        "%s_not_implemented",
        command_name
    );

    return gateway_interface_send_error(request_id, message, -2);
}

int gateway_interface_send_json_line(const char *json)
{
    if (json == NULL) {
        return -1;
    }

    transport_write_str(json);
    transport_write_str("\n");

    return 0;
}

int gateway_interface_send_stream_frame(
    uint8_t sensor_id,
    const uint8_t *payload,
    uint16_t payload_len,
    uint64_t gateway_timestamp_us
)
{
    uint8_t header[13];
    uint8_t checksum = 0;

    if (payload == NULL || payload_len > GATEWAY_MAX_FRAME_PAYLOAD) {
        return -1;
    }

    header[0] = 0xA5;
    header[1] = 0x5A;
    header[2] = 0x01;
    header[3] = sensor_id;
    header[4] = (uint8_t)(gateway_timestamp_us & 0xFF);
    header[5] = (uint8_t)((gateway_timestamp_us >> 8) & 0xFF);
    header[6] = (uint8_t)((gateway_timestamp_us >> 16) & 0xFF);
    header[7] = (uint8_t)((gateway_timestamp_us >> 24) & 0xFF);
    header[8] = (uint8_t)((gateway_timestamp_us >> 32) & 0xFF);
    header[9] = (uint8_t)((gateway_timestamp_us >> 40) & 0xFF);
    header[10] = (uint8_t)((gateway_timestamp_us >> 48) & 0xFF);
    header[11] = (uint8_t)((gateway_timestamp_us >> 56) & 0xFF);
    header[12] = (uint8_t)payload_len;

    for (size_t i = 2; i < sizeof(header); i++) {
        checksum = (uint8_t)(checksum + header[i]);
    }

    for (uint16_t i = 0; i < payload_len; i++) {
        checksum = (uint8_t)(checksum + payload[i]);
    }

    transport_write_bytes(header, sizeof(header));
    transport_write_bytes(payload, payload_len);
    transport_write_bytes(&checksum, 1);

    return 0;
}

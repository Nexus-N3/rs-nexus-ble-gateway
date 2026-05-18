#include "gateway_interface.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define RX_LINE_MAX 256

static const struct device *uart_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static gateway_interface_callbacks_t g_callbacks;

static char rx_line[RX_LINE_MAX];
static size_t rx_len;

static char pending_line[RX_LINE_MAX];
static volatile bool pending_line_ready;

static void transport_write_str(const char *s)
{
    if (s == NULL) {
        return;
    }

    while (*s != '\0') {
        uart_poll_out(uart_dev, *s++);
    }
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
        return;
    }

    if (json_type_is(line, "disconnect_all")) {
        command->type = GW_CMD_DISCONNECT_ALL;
        return;
    }

    if (json_type_is(line, "subscribe")) {
        command->type = GW_CMD_SUBSCRIBE;
        return;
    }

    if (json_type_is(line, "unsubscribe")) {
        command->type = GW_CMD_UNSUBSCRIBE;
        return;
    }

    if (json_type_is(line, "gatt_write")) {
        command->type = GW_CMD_GATT_WRITE;
        return;
    }

    if (json_type_is(line, "gatt_read")) {
        command->type = GW_CMD_GATT_READ;
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
}

int gateway_interface_init(const gateway_interface_callbacks_t *callbacks)
{
    if (callbacks != NULL) {
        g_callbacks = *callbacks;
    }

    if (!device_is_ready(uart_dev)) {
        return -1;
    }

    uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    return 0;
}

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
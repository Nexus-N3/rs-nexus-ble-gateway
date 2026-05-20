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

#define TX_CHUNK_SIZE 256
#define RX_LINE_MAX 256
#define UART_RX_BUF_SIZE 256
#define TX_CONTROL_RING_SIZE 8192
#define TX_STREAM_RING_SIZE 16384

static const struct device *uart_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static gateway_interface_callbacks_t g_callbacks;

static char rx_line[RX_LINE_MAX];
static size_t rx_len;

static char pending_line[RX_LINE_MAX];
static volatile bool pending_line_ready;
static uint8_t uart_rx_buf_a[UART_RX_BUF_SIZE];
static uint8_t uart_rx_buf_b[UART_RX_BUF_SIZE];
static bool uart_rx_buf_a_in_use;
static bool uart_rx_buf_b_in_use;

static uint8_t tx_control_ring[TX_CONTROL_RING_SIZE];
static volatile size_t tx_control_head;
static volatile size_t tx_control_tail;
static volatile size_t tx_control_count;
static volatile uint32_t tx_control_drop_count;

static uint8_t tx_stream_ring[TX_STREAM_RING_SIZE];
static volatile size_t tx_stream_head;
static volatile size_t tx_stream_tail;
static volatile size_t tx_stream_count;
static volatile uint32_t tx_stream_drop_count;
static volatile uint32_t tx_control_enqueue_success_count;
static volatile uint32_t tx_control_enqueue_drop_count;
static volatile uint32_t tx_stream_enqueue_success_count;
static volatile uint32_t tx_stream_enqueue_drop_count;
static volatile uint32_t tx_control_bytes_enqueued;
static volatile uint32_t tx_stream_bytes_enqueued;
static volatile uint32_t tx_control_bytes_dequeued;
static volatile uint32_t tx_stream_bytes_dequeued;
static volatile uint32_t tx_control_tx_done_count;
static volatile uint32_t tx_stream_tx_done_count;
static volatile uint32_t tx_control_tx_aborted_count;
static volatile uint32_t tx_stream_tx_aborted_count;
static volatile uint32_t tx_control_tx_start_failures;
static volatile uint32_t tx_stream_tx_start_failures;

typedef enum {
    TX_QUEUE_CONTROL = 0,
    TX_QUEUE_STREAM,
} tx_queue_kind_t;

static uint8_t tx_chunk_buf[TX_CHUNK_SIZE];
static volatile bool tx_in_progress;
static volatile tx_queue_kind_t tx_active_queue_kind;
static volatile size_t tx_active_len;
static void transport_try_start_tx(void);

static void process_rx_bytes(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
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

static int tx_enqueue_bytes(
    tx_queue_kind_t queue_kind,
    const uint8_t *data,
    size_t len
)
{
    uint8_t *ring;
    volatile size_t *tail;
    volatile size_t *count;
    size_t ring_size;
    volatile uint32_t *drop_count;

    if (data == NULL) {
        return -1;
    }

    if (queue_kind == TX_QUEUE_CONTROL) {
        ring = tx_control_ring;
        tail = &tx_control_tail;
        count = &tx_control_count;
        ring_size = TX_CONTROL_RING_SIZE;
        drop_count = &tx_control_drop_count;
    } else {
        ring = tx_stream_ring;
        tail = &tx_stream_tail;
        count = &tx_stream_count;
        ring_size = TX_STREAM_RING_SIZE;
        drop_count = &tx_stream_drop_count;
    }

    unsigned int key = irq_lock();

    if ((ring_size - *count) < len) {
        *drop_count += 1U;
        if (queue_kind == TX_QUEUE_CONTROL) {
            tx_control_enqueue_drop_count++;
        } else {
            tx_stream_enqueue_drop_count++;
        }
        irq_unlock(key);
        return -12;
    }

    for (size_t i = 0; i < len; i++) {
        ring[*tail] = data[i];
        *tail = (*tail + 1U) % ring_size;
    }

    *count += len;
    if (queue_kind == TX_QUEUE_CONTROL) {
        tx_control_enqueue_success_count++;
        tx_control_bytes_enqueued += (uint32_t)len;
    } else {
        tx_stream_enqueue_success_count++;
        tx_stream_bytes_enqueued += (uint32_t)len;
    }
    irq_unlock(key);

    transport_try_start_tx();
    return 0;
}

static int transport_write_str(tx_queue_kind_t queue_kind, const char *s)
{
    if (s == NULL) {
        return -1;
    }

    return tx_enqueue_bytes(
        queue_kind,
        (const uint8_t *)s,
        strlen(s)
    );
}

static int transport_write_bytes(
    tx_queue_kind_t queue_kind,
    const uint8_t *data,
    size_t len
)
{
    return tx_enqueue_bytes(queue_kind, data, len);
}

static int tx_dequeue_into_buf(
    uint8_t *buf,
    size_t buf_size,
    tx_queue_kind_t *queue_kind_out
)
{
    size_t len = 0;
    unsigned int key = irq_lock();

    if (tx_control_count > 0) {
        if (queue_kind_out != NULL) {
            *queue_kind_out = TX_QUEUE_CONTROL;
        }
        while (tx_control_count > 0 && len < buf_size) {
            buf[len++] = tx_control_ring[tx_control_head];
            tx_control_head = (tx_control_head + 1U) % TX_CONTROL_RING_SIZE;
            tx_control_count--;
        }
        tx_control_bytes_dequeued += (uint32_t)len;
    } else {
        if (queue_kind_out != NULL) {
            *queue_kind_out = TX_QUEUE_STREAM;
        }
        while (tx_stream_count > 0 && len < buf_size) {
            buf[len++] = tx_stream_ring[tx_stream_head];
            tx_stream_head = (tx_stream_head + 1U) % TX_STREAM_RING_SIZE;
            tx_stream_count--;
        }
        tx_stream_bytes_dequeued += (uint32_t)len;
    }

    irq_unlock(key);
    return (int)len;
}

static void mark_rx_buf_free(const uint8_t *buf)
{
    if (buf == uart_rx_buf_a) {
        uart_rx_buf_a_in_use = false;
    } else if (buf == uart_rx_buf_b) {
        uart_rx_buf_b_in_use = false;
    }
}

static uint8_t *claim_rx_buf(void)
{
    if (!uart_rx_buf_a_in_use) {
        uart_rx_buf_a_in_use = true;
        return uart_rx_buf_a;
    }

    if (!uart_rx_buf_b_in_use) {
        uart_rx_buf_b_in_use = true;
        return uart_rx_buf_b;
    }

    return NULL;
}

static void transport_try_start_tx(void)
{
    int len;

    if (!device_is_ready(uart_dev)) {
        return;
    }

    unsigned int key = irq_lock();
    if (tx_in_progress) {
        irq_unlock(key);
        return;
    }
    tx_in_progress = true;
    irq_unlock(key);

    tx_queue_kind_t queue_kind = TX_QUEUE_CONTROL;
    len = tx_dequeue_into_buf(tx_chunk_buf, sizeof(tx_chunk_buf), &queue_kind);
    if (len <= 0) {
        key = irq_lock();
        tx_in_progress = false;
        irq_unlock(key);
        return;
    }

    key = irq_lock();
    tx_active_queue_kind = queue_kind;
    tx_active_len = (size_t)len;
    irq_unlock(key);

    int rc = uart_tx(uart_dev, tx_chunk_buf, len, SYS_FOREVER_US);
    if (rc != 0) {
        key = irq_lock();
        if (tx_active_queue_kind == TX_QUEUE_CONTROL) {
            tx_control_tx_start_failures++;
        } else {
            tx_stream_tx_start_failures++;
        }
        tx_active_len = 0;
        tx_in_progress = false;
        irq_unlock(key);
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

    if (json_type_is(line, "reset_session")) {
        command->type = GW_CMD_RESET_SESSION;
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

static void uart_cb(
    const struct device *dev,
    struct uart_event *evt,
    void *user_data
)
{
    ARG_UNUSED(user_data);

    switch (evt->type) {
    case UART_TX_DONE: {
        unsigned int key = irq_lock();
        if (tx_active_queue_kind == TX_QUEUE_CONTROL) {
            tx_control_tx_done_count++;
        } else {
            tx_stream_tx_done_count++;
        }
        tx_active_len = 0;
        tx_in_progress = false;
        irq_unlock(key);
        transport_try_start_tx();
        break;
    }

    case UART_TX_ABORTED: {
        unsigned int key = irq_lock();
        if (tx_active_queue_kind == TX_QUEUE_CONTROL) {
            tx_control_tx_aborted_count++;
        } else {
            tx_stream_tx_aborted_count++;
        }
        tx_active_len = 0;
        tx_in_progress = false;
        irq_unlock(key);
        transport_try_start_tx();
        break;
    }

    case UART_RX_RDY:
        process_rx_bytes(
            &evt->data.rx.buf[evt->data.rx.offset],
            evt->data.rx.len
        );
        break;

    case UART_RX_BUF_REQUEST: {
        uint8_t *buf = claim_rx_buf();
        if (buf != NULL) {
            (void)uart_rx_buf_rsp(dev, buf, UART_RX_BUF_SIZE);
        }
        break;
    }

    case UART_RX_BUF_RELEASED:
        mark_rx_buf_free(evt->data.rx_buf.buf);
        break;

    case UART_RX_DISABLED: {
        uint8_t *buf = claim_rx_buf();
        if (buf != NULL) {
            (void)uart_rx_enable(dev, buf, UART_RX_BUF_SIZE, 5000);
        }
        break;
    }

    case UART_RX_STOPPED:
        (void)uart_rx_disable(dev);
        break;

    default:
        break;
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

    tx_control_head = 0;
    tx_control_tail = 0;
    tx_control_count = 0;
    tx_control_drop_count = 0;
    tx_stream_head = 0;
    tx_stream_tail = 0;
    tx_stream_count = 0;
    tx_stream_drop_count = 0;
    tx_control_enqueue_success_count = 0;
    tx_control_enqueue_drop_count = 0;
    tx_stream_enqueue_success_count = 0;
    tx_stream_enqueue_drop_count = 0;
    tx_control_bytes_enqueued = 0;
    tx_stream_bytes_enqueued = 0;
    tx_control_bytes_dequeued = 0;
    tx_stream_bytes_dequeued = 0;
    tx_control_tx_done_count = 0;
    tx_stream_tx_done_count = 0;
    tx_control_tx_aborted_count = 0;
    tx_stream_tx_aborted_count = 0;
    tx_control_tx_start_failures = 0;
    tx_stream_tx_start_failures = 0;

    uart_rx_buf_a_in_use = false;
    uart_rx_buf_b_in_use = false;
    tx_in_progress = false;
    tx_active_queue_kind = TX_QUEUE_CONTROL;
    tx_active_len = 0;

    if (uart_callback_set(uart_dev, uart_cb, NULL) != 0) {
        return -1;
    }

    uint8_t *buf = claim_rx_buf();
    if (buf == NULL) {
        return -1;
    }

    if (uart_rx_enable(uart_dev, buf, UART_RX_BUF_SIZE, 5000) != 0) {
        return -1;
    }

    return 0;
}

int gateway_interface_reset_transport_state(void)
{
    unsigned int key;

    if (!device_is_ready(uart_dev)) {
        return -1;
    }

    key = irq_lock();
    rx_len = 0;
    pending_line_ready = false;
    pending_line[0] = '\0';
    tx_control_head = 0;
    tx_control_tail = 0;
    tx_control_count = 0;
    tx_control_drop_count = 0;
    tx_stream_head = 0;
    tx_stream_tail = 0;
    tx_stream_count = 0;
    tx_stream_drop_count = 0;
    tx_control_enqueue_success_count = 0;
    tx_control_enqueue_drop_count = 0;
    tx_stream_enqueue_success_count = 0;
    tx_stream_enqueue_drop_count = 0;
    tx_control_bytes_enqueued = 0;
    tx_stream_bytes_enqueued = 0;
    tx_control_bytes_dequeued = 0;
    tx_stream_bytes_dequeued = 0;
    tx_control_tx_done_count = 0;
    tx_stream_tx_done_count = 0;
    tx_control_tx_aborted_count = 0;
    tx_stream_tx_aborted_count = 0;
    tx_control_tx_start_failures = 0;
    tx_stream_tx_start_failures = 0;
    tx_active_queue_kind = TX_QUEUE_CONTROL;
    tx_active_len = 0;
    tx_in_progress = false;
    irq_unlock(key);

    return 0;
}

int gateway_interface_poll(void)
{
    char line[RX_LINE_MAX];

    transport_try_start_tx();

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

int gateway_interface_send_transport_stats(void)
{
    char line[768];
    unsigned int key = irq_lock();
    const size_t control_ring_count = tx_control_count;
    const size_t stream_ring_count = tx_stream_count;
    const uint32_t control_ring_drops = tx_control_drop_count;
    const uint32_t stream_ring_drops = tx_stream_drop_count;
    const uint32_t control_enqueue_success = tx_control_enqueue_success_count;
    const uint32_t control_enqueue_drops = tx_control_enqueue_drop_count;
    const uint32_t stream_enqueue_success = tx_stream_enqueue_success_count;
    const uint32_t stream_enqueue_drops = tx_stream_enqueue_drop_count;
    const uint32_t control_enqueued = tx_control_bytes_enqueued;
    const uint32_t stream_enqueued = tx_stream_bytes_enqueued;
    const uint32_t control_dequeued = tx_control_bytes_dequeued;
    const uint32_t stream_dequeued = tx_stream_bytes_dequeued;
    const uint32_t control_tx_done = tx_control_tx_done_count;
    const uint32_t stream_tx_done = tx_stream_tx_done_count;
    const uint32_t control_tx_aborted = tx_control_tx_aborted_count;
    const uint32_t stream_tx_aborted = tx_stream_tx_aborted_count;
    const uint32_t control_tx_failures = tx_control_tx_start_failures;
    const uint32_t stream_tx_failures = tx_stream_tx_start_failures;
    const bool in_progress = tx_in_progress;
    const unsigned int active_queue_kind = (unsigned int)tx_active_queue_kind;
    const unsigned int active_len = (unsigned int)tx_active_len;
    irq_unlock(key);

    snprintf(
        line,
        sizeof(line),
        "{\"type\":\"gateway_transport_stats\","
        "\"control_ring_bytes\":%u,"
        "\"stream_ring_bytes\":%u,"
        "\"control_ring_drops\":%u,"
        "\"stream_ring_drops\":%u,"
        "\"control_enqueue_success\":%u,"
        "\"control_enqueue_drops\":%u,"
        "\"stream_enqueue_success\":%u,"
        "\"stream_enqueue_drops\":%u,"
        "\"control_bytes_enqueued\":%u,"
        "\"stream_bytes_enqueued\":%u,"
        "\"control_bytes_dequeued\":%u,"
        "\"stream_bytes_dequeued\":%u,"
        "\"control_tx_done\":%u,"
        "\"stream_tx_done\":%u,"
        "\"control_tx_aborted\":%u,"
        "\"stream_tx_aborted\":%u,"
        "\"control_tx_start_failures\":%u,"
        "\"stream_tx_start_failures\":%u,"
        "\"tx_in_progress\":%s,"
        "\"active_queue_kind\":%u,"
        "\"active_len\":%u}",
        (unsigned int)control_ring_count,
        (unsigned int)stream_ring_count,
        (unsigned int)control_ring_drops,
        (unsigned int)stream_ring_drops,
        (unsigned int)control_enqueue_success,
        (unsigned int)control_enqueue_drops,
        (unsigned int)stream_enqueue_success,
        (unsigned int)stream_enqueue_drops,
        (unsigned int)control_enqueued,
        (unsigned int)stream_enqueued,
        (unsigned int)control_dequeued,
        (unsigned int)stream_dequeued,
        (unsigned int)control_tx_done,
        (unsigned int)stream_tx_done,
        (unsigned int)control_tx_aborted,
        (unsigned int)stream_tx_aborted,
        (unsigned int)control_tx_failures,
        (unsigned int)stream_tx_failures,
        in_progress ? "true" : "false",
        active_queue_kind,
        active_len
    );

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

    if (transport_write_str(TX_QUEUE_CONTROL, json) != 0) {
        return -12;
    }

    if (transport_write_str(TX_QUEUE_CONTROL, "\n") != 0) {
        return -12;
    }

    return 0;
}

int gateway_interface_send_stream_frame(
    uint8_t sensor_id,
    const uint8_t *payload,
    uint16_t payload_len,
    uint64_t gateway_timestamp_us
)
{
    uint8_t frame[13 + GATEWAY_MAX_FRAME_PAYLOAD + 1];
    size_t index = 0;
    uint8_t checksum = 0;

    if (payload == NULL) {
        return -1;
    }

    if (payload_len > GATEWAY_MAX_FRAME_PAYLOAD) {
        payload_len = GATEWAY_MAX_FRAME_PAYLOAD;
    }

    frame[index++] = 0xA5;
    frame[index++] = 0x5A;
    frame[index++] = 0x01;
    frame[index++] = sensor_id;

    for (int i = 0; i < 8; i++) {
        frame[index++] = (uint8_t)((gateway_timestamp_us >> (8 * i)) & 0xFF);
    }

    frame[index++] = (uint8_t)payload_len;
    memcpy(&frame[index], payload, payload_len);
    index += payload_len;

    for (size_t i = 2; i < index; i++) {
        checksum = (uint8_t)(checksum + frame[i]);
    }

    frame[index++] = checksum;
    return transport_write_bytes(TX_QUEUE_STREAM, frame, index);
}

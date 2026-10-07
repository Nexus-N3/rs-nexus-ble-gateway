#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/interface/gateway_interface.c"

struct device fake_uart_device;
int64_t fake_uptime_ms;

static uart_callback_t fake_callback;
static void *fake_callback_data;
static bool fake_hw_tx_active;
static bool fake_abort_synchronously;
static unsigned int fake_abort_without_callback_on_call;
static const uint8_t *fake_tx_buf;
static size_t fake_tx_len;
static unsigned int fake_tx_calls;
static unsigned int fake_abort_calls;
static unsigned int fake_overlapping_tx_calls;
static uint8_t fake_tx_first_bytes[4096];
static int fake_tx_start_error;

bool device_is_ready(const struct device *dev)
{
    return dev == &fake_uart_device;
}

int uart_callback_set(
    const struct device *dev,
    uart_callback_t callback,
    void *user_data
)
{
    assert(dev == &fake_uart_device);
    fake_callback = callback;
    fake_callback_data = user_data;
    return 0;
}

int uart_tx(
    const struct device *dev,
    const uint8_t *buf,
    size_t len,
    int32_t timeout
)
{
    assert(dev == &fake_uart_device);
    assert(timeout == SYS_FOREVER_US);

    if (fake_hw_tx_active) {
        fake_overlapping_tx_calls++;
        return -EBUSY;
    }

    if (fake_tx_start_error != 0) {
        return fake_tx_start_error;
    }

    assert(fake_tx_calls < sizeof(fake_tx_first_bytes));
    fake_tx_first_bytes[fake_tx_calls] = buf[0];
    fake_tx_calls++;
    fake_hw_tx_active = true;
    fake_tx_buf = buf;
    fake_tx_len = len;
    return 0;
}

static void fake_emit_tx_event(enum uart_event_type type)
{
    struct uart_event event = {
        .type = type,
        .data.tx = {
            .buf = fake_tx_buf,
            .len = fake_tx_len,
        },
    };

    fake_hw_tx_active = false;
    fake_callback(&fake_uart_device, &event, fake_callback_data);
}

int uart_tx_abort(const struct device *dev)
{
    assert(dev == &fake_uart_device);
    fake_abort_calls++;

    if (!fake_hw_tx_active) {
        return -EFAULT;
    }

    if (fake_abort_calls == fake_abort_without_callback_on_call) {
        fake_hw_tx_active = false;
    } else if (fake_abort_synchronously) {
        fake_emit_tx_event(UART_TX_ABORTED);
    }

    return 0;
}

int uart_rx_enable(
    const struct device *dev,
    uint8_t *buf,
    size_t len,
    int32_t timeout
)
{
    (void)dev;
    (void)buf;
    (void)len;
    (void)timeout;
    return 0;
}

int uart_rx_buf_rsp(const struct device *dev, uint8_t *buf, size_t len)
{
    (void)dev;
    (void)buf;
    (void)len;
    return 0;
}

int uart_rx_disable(const struct device *dev)
{
    (void)dev;
    return 0;
}

int led_set(enum app_led led, bool value)
{
    (void)led;
    (void)value;
    return 0;
}

static void reset_fixture(void)
{
    fake_uptime_ms = 1;
    fake_tx_start_error = 0;
    fake_callback = NULL;
    fake_callback_data = NULL;
    fake_hw_tx_active = false;
    fake_abort_synchronously = false;
    fake_abort_without_callback_on_call = 0;
    fake_tx_buf = NULL;
    fake_tx_len = 0;
    fake_tx_calls = 0;
    fake_abort_calls = 0;
    fake_overlapping_tx_calls = 0;
    memset(fake_tx_first_bytes, 0, sizeof(fake_tx_first_bytes));
    tx_abort_report_pending = false;

    assert(gateway_interface_init(NULL) == 0);
}

static void enqueue_control(uint8_t byte)
{
    assert(tx_enqueue_bytes(TX_QUEUE_CONTROL, &byte, 1) == 0);
}

static void test_normal_tx_done_starts_next(void)
{
    reset_fixture();
    enqueue_control('A');
    enqueue_control('B');

    assert(fake_tx_calls == 1);
    fake_emit_tx_event(UART_TX_DONE);

    assert(tx_control_tx_done_count == 1);
    assert(fake_tx_calls == 2);
    assert(fake_tx_first_bytes[1] == 'B');
    assert(fake_overlapping_tx_calls == 0);
}

static void test_normal_tx_aborted_starts_next(void)
{
    reset_fixture();
    enqueue_control('A');
    enqueue_control('B');

    fake_emit_tx_event(UART_TX_ABORTED);

    assert(tx_control_tx_aborted_count == 1);
    assert(fake_tx_calls == 2);
    assert(fake_tx_first_bytes[1] == 'B');
    assert(fake_overlapping_tx_calls == 0);
}

static void test_stuck_tx_recovers_when_driver_is_idle(void)
{
    reset_fixture();
    enqueue_control('A');
    enqueue_control('B');

    /* Model hardware completion with a lost UART_TX_DONE callback. */
    fake_hw_tx_active = false;
    fake_uptime_ms = 102;
    assert(gateway_interface_poll() == 0);

    assert(tx_stuck_count == 1);
    assert(fake_abort_calls == 1);
    assert(fake_tx_calls == 2);
    assert(fake_tx_first_bytes[1] == 'B');
    assert(fake_overlapping_tx_calls == 0);
}

static void test_stuck_tx_recovers_after_async_abort(void)
{
    reset_fixture();
    enqueue_control('A');
    enqueue_control('B');

    fake_uptime_ms = 102;
    assert(gateway_interface_poll() == 0);

    assert(tx_stuck_count == 1);
    assert(fake_abort_calls == 1);
    assert(fake_tx_calls == 1);
    assert(tx_in_progress);

    fake_emit_tx_event(UART_TX_ABORTED);

    assert(fake_tx_calls == 2);
    assert(fake_tx_first_bytes[1] == 'B');
    assert(fake_overlapping_tx_calls == 0);
}

static void test_successful_abort_without_callback_is_retried(void)
{
    reset_fixture();
    enqueue_control('A');
    enqueue_control('B');
    fake_abort_without_callback_on_call = 1;

    fake_uptime_ms = 102;
    assert(gateway_interface_poll() == 0);

    assert(fake_abort_calls == 1);
    assert(tx_in_progress);
    assert(!tx_stuck_reported);
    assert(fake_tx_calls == 1);

    fake_uptime_ms = 203;
    assert(gateway_interface_poll() == 0);

    assert(fake_abort_calls == 2);
    assert(fake_tx_calls == 2);
    assert(fake_tx_first_bytes[1] == 'B');
    assert(fake_overlapping_tx_calls == 0);
}

static void test_recovery_does_not_start_tx_inside_abort(void)
{
    reset_fixture();
    enqueue_control('A');
    enqueue_control('B');
    fake_abort_synchronously = true;

    fake_uptime_ms = 102;
    assert(gateway_interface_poll() == 0);

    assert(tx_stuck_count == 1);
    assert(fake_abort_calls == 1);
    assert(tx_control_tx_aborted_count == 1);
    assert(fake_tx_calls == 2);
    assert(fake_tx_first_bytes[1] == 'B');
    assert(fake_overlapping_tx_calls == 0);
}

static void enqueue_stream(uint8_t sensor, uint16_t payload_len)
{
    uint8_t payload[GATEWAY_MAX_FRAME_PAYLOAD];
    memset(payload, sensor, sizeof(payload));
    assert(gateway_interface_send_stream_frame(sensor, payload, payload_len, 123) == 0);
}

static void test_stream_gap_and_control_priority(void)
{
    reset_fixture();
    enqueue_stream(1, 244);
    enqueue_stream(2, 244);
    assert(fake_tx_len == 258);
    fake_emit_tx_event(UART_TX_DONE);
    assert(fake_tx_calls == 1);
    assert(tx_stream_count == 258);
    assert(tx_stream_pacing_delays == 1);

    /* Even repeated polling cannot consume a paced frame early. */
    fake_uptime_ms = 2;
    gateway_interface_poll();
    assert(fake_tx_calls == 1);
    enqueue_control('C');
    assert(fake_tx_calls == 2);
    assert(fake_tx_buf[0] == 'C');
    fake_emit_tx_event(UART_TX_DONE);
    assert(fake_tx_calls == 2);
    fake_uptime_ms = 3;
    gateway_interface_poll();
    assert(fake_tx_calls == 2);
    fake_uptime_ms = 4;
    gateway_interface_poll();
    assert(fake_tx_calls == 3);
    assert(fake_tx_buf[3] == 2);
    assert(tx_stream_count == 0);
    assert(tx_stream_high_water_bytes == 258);
    assert(tx_stream_backlog_max_ms == 3);
    assert(fake_overlapping_tx_calls == 0);
}

static void test_complete_frame_batches_and_ring_wrap(void)
{
    reset_fixture();
    tx_stream_head = tx_stream_tail = TX_STREAM_RING_SIZE - 10;
    enqueue_control('C');
    enqueue_stream(1, 100);
    enqueue_stream(2, 100);
    enqueue_stream(3, 100);
    fake_emit_tx_event(UART_TX_DONE);
    fake_uptime_ms += GATEWAY_STREAM_TX_GAP_MS;
    gateway_interface_poll();
    assert(fake_tx_len == 228);
    assert(fake_tx_buf[3] == 1);
    assert(fake_tx_buf[114 + 3] == 2);
    assert(tx_stream_count == 114);
    fake_emit_tx_event(UART_TX_DONE);
    fake_uptime_ms += GATEWAY_STREAM_TX_GAP_MS;
    gateway_interface_poll();
    assert(fake_tx_len == 114);
    assert(fake_tx_buf[3] == 3);
    assert(tx_stream_count == 0);
    assert(tx_stream_bytes_enqueued == tx_stream_bytes_dequeued);
}

static void test_start_failure_retains_stream_frame(void)
{
    reset_fixture();
    fake_tx_start_error = -EBUSY;
    enqueue_stream(7, 244);
    assert(!tx_in_progress);
    assert(tx_stream_tx_start_failures == 1);
    assert(tx_stream_count == 258);
    assert(tx_stream_bytes_dequeued == 0);
    fake_tx_start_error = 0;
    fake_uptime_ms += GATEWAY_STREAM_TX_GAP_MS;
    gateway_interface_poll();
    assert(fake_tx_len == 258);
    assert(fake_tx_buf[3] == 7);
    assert(tx_stream_count == 0);
    assert(tx_stream_bytes_dequeued == 258);
}

static void test_pressure_and_overflow_are_visible(void)
{
    reset_fixture();
    enqueue_stream(1, 244);
    for (int i = 0; i < 63; i++) {
        enqueue_stream(2, 244);
    }
    assert(tx_stream_high_water_bytes == 63 * 258);
    assert(tx_stream_pressure_events == 1);
    uint8_t payload[244] = {0};
    assert(gateway_interface_send_stream_frame(3, payload, sizeof(payload), 123) == -12);
    assert(tx_stream_enqueue_drop_count == 1);
    assert(tx_stream_drop_count == 1);
    assert(tx_stream_count == 63 * 258);
    fake_uptime_ms += 50;
    gateway_interface_poll();
    assert(tx_stream_backlog_max_ms == 50);
    assert(gateway_interface_reset_transport_state() == 0);
    assert(tx_stream_high_water_bytes == 0);
    assert(tx_stream_pressure_events == 0);
    assert(tx_stream_backlog_max_ms == 0);
    assert(tx_stream_pacing_delays == 0);
}

static void test_four_dot_capacity(void)
{
    reset_fixture();
    /* Movella DOT decoded IMU prefix: 44 bytes, 60 Hz each. Model synchronized
     * arrival from four sensors and integer-ms wire-time rounding upward.
     */
    int64_t completion_ms = 0;
    for (fake_uptime_ms = 1; fake_uptime_ms <= 1001; fake_uptime_ms++) {
        if (fake_hw_tx_active && fake_uptime_ms >= completion_ms) {
            fake_emit_tx_event(UART_TX_DONE);
            if (fake_hw_tx_active) {
                completion_ms = fake_uptime_ms + (fake_tx_len + 99) / 100;
            }
        }
        if ((fake_uptime_ms - 1) % 17 == 0) {
            bool was_active = fake_hw_tx_active;
            for (int sensor = 1; sensor <= 4; sensor++) {
                enqueue_stream(sensor, 44);
            }
            if (!was_active && fake_hw_tx_active) {
                completion_ms = fake_uptime_ms + (fake_tx_len + 99) / 100;
            }
        }
        bool was_active = fake_hw_tx_active;
        gateway_interface_poll();
        if (!was_active && fake_hw_tx_active) {
            completion_ms = fake_uptime_ms + (fake_tx_len + 99) / 100;
        }
    }
    assert(tx_stream_enqueue_drop_count == 0);
    assert(tx_stream_pressure_events == 0);
    assert(tx_stream_high_water_bytes <= 4 * 58);
    assert(tx_stream_backlog_max_ms < 17);
}

static void test_transport_stats_fit(void)
{
    reset_fixture();
    /* Exercise the longest decimal representations, not just zeros. */
    tx_control_drop_count = tx_stream_drop_count = UINT32_MAX;
    tx_control_enqueue_success_count = tx_control_enqueue_drop_count = UINT32_MAX;
    tx_stream_enqueue_success_count = tx_stream_enqueue_drop_count = UINT32_MAX;
    tx_control_bytes_enqueued = tx_stream_bytes_enqueued = UINT32_MAX;
    tx_control_bytes_dequeued = tx_stream_bytes_dequeued = UINT32_MAX;
    tx_control_tx_done_count = tx_stream_tx_done_count = UINT32_MAX;
    tx_control_tx_aborted_count = tx_stream_tx_aborted_count = UINT32_MAX;
    tx_control_tx_start_failures = tx_stream_tx_start_failures = UINT32_MAX;
    tx_done_len_mismatch_count = tx_done_zero_len_count = UINT32_MAX;
    tx_done_buffer_pointer_mismatches = tx_stuck_count = UINT32_MAX;
    tx_stream_high_water_bytes = TX_STREAM_RING_SIZE;
    tx_stream_pacing_delays = UINT32_MAX;
    tx_stream_pressure_events = UINT32_MAX;
    tx_stream_backlog_max_ms = UINT32_MAX;
    assert(gateway_interface_send_transport_stats() == 0);
    /* The first control chunk is active; the remainder ends in valid JSON. */
    assert(tx_control_ring[(tx_control_tail + TX_CONTROL_RING_SIZE - 1) % TX_CONTROL_RING_SIZE] == '\n');
    assert(tx_control_ring[(tx_control_tail + TX_CONTROL_RING_SIZE - 2) % TX_CONTROL_RING_SIZE] == '}');
}

int main(void)
{
    test_normal_tx_done_starts_next();
    test_normal_tx_aborted_starts_next();
    test_stuck_tx_recovers_when_driver_is_idle();
    test_stuck_tx_recovers_after_async_abort();
    test_successful_abort_without_callback_is_retried();
    test_recovery_does_not_start_tx_inside_abort();
    test_stream_gap_and_control_priority();
    test_complete_frame_batches_and_ring_wrap();
    test_start_failure_retains_stream_frame();
    test_pressure_and_overflow_are_visible();
    test_four_dot_capacity();
    test_transport_stats_fit();
    puts("UART TX state tests passed");
    return 0;
}

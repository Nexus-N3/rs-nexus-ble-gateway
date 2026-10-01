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
static uint8_t fake_tx_first_bytes[8];

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

int main(void)
{
    test_normal_tx_done_starts_next();
    test_normal_tx_aborted_starts_next();
    test_stuck_tx_recovers_when_driver_is_idle();
    test_stuck_tx_recovers_after_async_abort();
    test_successful_abort_without_callback_is_retried();
    test_recovery_does_not_start_tx_inside_abort();
    puts("UART TX state tests passed");
    return 0;
}

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

enum uart_event_type {
    UART_TX_DONE,
    UART_TX_ABORTED,
    UART_RX_RDY,
    UART_RX_BUF_REQUEST,
    UART_RX_BUF_RELEASED,
    UART_RX_DISABLED,
    UART_RX_STOPPED,
};

struct uart_event {
    enum uart_event_type type;
    union {
        struct {
            const uint8_t *buf;
            size_t len;
        } tx;
        struct {
            uint8_t *buf;
            size_t offset;
            size_t len;
        } rx;
        struct {
            uint8_t *buf;
        } rx_buf;
    } data;
};

typedef void (*uart_callback_t)(
    const struct device *dev,
    struct uart_event *evt,
    void *user_data
);

int uart_callback_set(
    const struct device *dev,
    uart_callback_t callback,
    void *user_data
);
int uart_tx(
    const struct device *dev,
    const uint8_t *buf,
    size_t len,
    int32_t timeout
);
int uart_tx_abort(const struct device *dev);
int uart_rx_enable(
    const struct device *dev,
    uint8_t *buf,
    size_t len,
    int32_t timeout
);
int uart_rx_buf_rsp(const struct device *dev, uint8_t *buf, size_t len);
int uart_rx_disable(const struct device *dev);

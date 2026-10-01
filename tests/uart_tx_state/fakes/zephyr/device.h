#pragma once

#include <stdbool.h>

struct device {
    int unused;
};

extern struct device fake_uart_device;

#define DT_CHOSEN(node_id) 0
#define DEVICE_DT_GET(node_id) (&fake_uart_device)

bool device_is_ready(const struct device *dev);

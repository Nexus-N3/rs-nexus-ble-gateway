#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#include "app/gateway.h"

#define LED0_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
#endif

static void heartbeat_init(void)
{
#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
    if (gpio_is_ready_dt(&led0)) {
        gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    }
#endif
}

static void heartbeat_tick(void)
{
    static int64_t last_blink_ms;
    int64_t now_ms = k_uptime_get();

    if (now_ms - last_blink_ms < 500) {
        return;
    }

    last_blink_ms = now_ms;

#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
    if (gpio_is_ready_dt(&led0)) {
        gpio_pin_toggle_dt(&led0);
    }
#endif
}

int main(void)
{
    heartbeat_init();

    gateway_app_init();

    while (1) {
        gateway_app_run_once();
        heartbeat_tick();
        k_sleep(K_MSEC(1));
    }

    return 0;
}
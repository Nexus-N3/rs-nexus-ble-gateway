#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include "hardware/led.h"
#include "app/gateway.h"

#define GATEWAY_WATCHDOG_TIMEOUT_MS 10000

#if DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
static const struct device *wdt_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
#else
static const struct device *wdt_dev = NULL;
#endif

static int wdt_channel_id = -1;

#define LED0_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
#endif

static int gateway_watchdog_init(void)
{
    struct wdt_timeout_cfg wdt_config = {
        .window = {
            .min = 0,
            .max = GATEWAY_WATCHDOG_TIMEOUT_MS,
        },
        .flags = WDT_FLAG_RESET_SOC,
    };

    if (wdt_dev == NULL || !device_is_ready(wdt_dev)) {
        return -1;
    }

    wdt_channel_id = wdt_install_timeout(wdt_dev, &wdt_config);
    if (wdt_channel_id < 0) {
        return wdt_channel_id;
    }

    return wdt_setup(wdt_dev, 0);
}

static void gateway_watchdog_feed(void)
{
    if (wdt_dev != NULL && wdt_channel_id >= 0) {
        (void)wdt_feed(wdt_dev, wdt_channel_id);
    }
}

static void heartbeat_init(void)
{
#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
    if (gpio_is_ready_dt(&led0)) {
        gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    }
#endif
}

// This function is called periodically to toggle the heartbeat LED. It uses a simple time check to toggle the LED every 500ms.
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

// This is the main entry point of the application. It initializes the heartbeat and gateway app, 
// then enters an infinite loop where it runs the gateway app and ticks the heartbeat.
int main(void)
{
    heartbeat_init();

    gateway_app_init();

    int rc = leds_init();
    if (rc) {
        return rc;
    }

    (void)gateway_watchdog_init();

    while (1) {
        gateway_app_run_once();
        heartbeat_tick();

        /*
         * Feed only after gateway_app_run_once() returns.
         * If BLE/GATT/control processing wedges, this is not reached
         * and the watchdog will reset the board.
         */
        gateway_watchdog_feed();

        k_sleep(K_MSEC(1));
    }

    return 0;
}
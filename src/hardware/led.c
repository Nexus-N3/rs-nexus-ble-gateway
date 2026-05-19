#include "led.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

//#define LED0_NODE DT_ALIAS(led0) - leave this one as the main app uses it for the heartbeat
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)
#define LED3_NODE DT_ALIAS(led3)

// LED_COUUNT is defined in the enum in th header last (here it equals 3)
static const struct gpio_dt_spec leds[APP_LED_COUNT] = {
	//[APP_LED_HEARTBEAT] = GPIO_DT_SPEC_GET(LED0_NODE, gpios),
	[APP_LED_SCAN]      = GPIO_DT_SPEC_GET(LED1_NODE, gpios),
	[APP_LED_STATUS]    = GPIO_DT_SPEC_GET(LED2_NODE, gpios),
	[APP_LED_ERROR]     = GPIO_DT_SPEC_GET(LED3_NODE, gpios),
};

static bool initialised;

int leds_init(void)
{
    if (initialised) {
        return 0;
    }

    for (size_t i = 0; i < APP_LED_COUNT; i++) {
        if (!device_is_ready(leds[i].port)) {
            return -ENODEV;
        }

        int rc = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
        if (rc != 0) {
            return rc;
        }
    }

    initialised = true;

    return 0;
}

int led_check(enum app_led led)
{
    if (led < 0 || led >= APP_LED_COUNT) {
        return -EINVAL;
    }

    if (!initialised) {
        return -EIO;
    }

    return 0;
}

int led_set(enum app_led led, bool value)
{
    int rc = led_check(led);
    if (rc != 0) {
        return rc;
    }

    return gpio_pin_set_dt(&leds[led], value);
}

int led_on(enum app_led led)
{
    return led_set(led, 1);
}

int led_off(enum app_led led)
{
    return led_set(led, 0);
}

int led_toggle(enum app_led led)
{
    int rc = led_check(led);
    if (rc != 0) {
        return rc;
    }

    return gpio_pin_toggle_dt(&leds[led]);
}
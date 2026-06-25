#include "button.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>


// specific to the maker diary connect board
#define BUTTON_NODE DT_ALIAS(sw0)
#define BUTTON_DEBOUNCE_MS 50

#if !DT_NODE_HAS_STATUS(BUTTON_NODE, okay)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);
static struct gpio_callback button_gpio_cb;

static button_callback_t app_callback;
static int64_t last_event_ms;

//button presses will trigger an interrupt
//this function definition is a zephr callback
static void button_gpio_handler(
    const struct device *port,
    struct gpio_callback *cb,
    uint32_t pins
){
    int64_t now_ms = k_uptime_get();  // from zephr kernal
    int value;

    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    if ((now_ms - last_event_ms) < BUTTON_DEBOUNCE_MS) {
        return;
    }

    last_event_ms = now_ms;

    value = gpio_pin_get_dt(&button);
    if (value < 0) {
        return;
    }

    if (app_callback == NULL) {
        return;
    }

    if (value == 1) {
        app_callback(BUTTON_EVENT_PRESSED, now_ms);
    } else {
        app_callback(BUTTON_EVENT_RELEASED, now_ms);
    }

}

//button initialisation
int button_init(button_callback_t callback){
    int rc;

    if (!gpio_is_ready_dt(&button)) {
        return -ENODEV;
    }

    //assign the callback
    app_callback = callback;
    last_event_ms = 0;

    //configure the button as an input by passing the address of the button
    rc = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (rc != 0) {
        return rc;
    }

    //configure the interrupt on both edges for pressed and released (we only need pressed at the moment)
    rc = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
    if (rc != 0) {
        return rc;
    }

    //init the interrupt handler callback
    gpio_init_callback(
        &button_gpio_cb,
        button_gpio_handler,
        BIT(button.pin)
    );
    //add the interrupt handler callback
    rc = gpio_add_callback(button.port, &button_gpio_cb);
    if (rc != 0) {
        return rc;
    }

    return 0;
}
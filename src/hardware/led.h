#pragma once

#include <stdbool.h>

enum app_led {
	//APP_LED_HEARTBEAT = 0,
	APP_LED_SCAN = 0, // this makes the rest 1,2,3 so LED_COUNT is 3
	APP_LED_STATUS,
	APP_LED_ERROR,
	APP_LED_COUNT
};

int leds_init(void);
int led_check(enum app_led led);
int led_on(enum app_led led);
int led_off(enum app_led led);
int led_toggle(enum app_led led);
int led_set(enum app_led led, bool value);
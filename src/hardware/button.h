#pragma once

#include <stdint.h>


//button events 
typedef enum {
    BUTTON_EVENT_PRESSED = 0,
    BUTTON_EVENT_RELEASED,
} button_event_t;

// callback function pointer
typedef void (*button_callback_t)(button_event_t event, int64_t timestamp_ms);

// initialise the button in hardware and pass in the callback function pointer
int button_init(button_callback_t callback);



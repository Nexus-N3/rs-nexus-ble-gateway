#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "gateway_protocol.h"

typedef void (*gateway_command_callback_t)(const gateway_command_t *command);

typedef struct {
    gateway_command_callback_t on_command;
} gateway_interface_callbacks_t;

int gateway_interface_init(const gateway_interface_callbacks_t *callbacks);
int gateway_interface_poll(void);

int gateway_interface_send_event(const gateway_event_t *event);
int gateway_interface_send_frame(const gateway_sensor_frame_t *frame);
int gateway_interface_send_log(const char *message);

/*
 * First-pass JSON-lines gateway events.
 */
int gateway_interface_send_ready(void);
int gateway_interface_send_hello_ack(const char *request_id);
int gateway_interface_send_status(const char *request_id);

int gateway_interface_send_error(
    const char *request_id,
    const char *message,
    int error_code
);

int gateway_interface_send_not_implemented(
    const char *request_id,
    const char *command_name
);

/*
 * Temporary debug/helper API for JSON-lines prototype.
 * Production should move to structured event serialization or binary framing.
 */
int gateway_interface_send_json_line(const char *json);
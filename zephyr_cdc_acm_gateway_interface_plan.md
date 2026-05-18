# Zephyr CDC ACM Gateway Interface Plan

## Purpose

Define how `rs-nexus-ble-gateway` should expose its first-pass host interface on
the nRF54L15 DK using Zephyr and USB CDC ACM.

This document complements the host-side serial transport plan in
`rs-nexus-os` and describes the gateway-facing implementation model only.

## Chosen Platform And Interface

Platform:

- nRF54L15 DK
- Zephyr SDK and Zephyr USB device support

Chosen host link:

- USB CDC ACM
- newline-delimited JSON messages

This means the gateway firmware should appear to the host as a serial device,
for example `/dev/ttyACM0`.

## Why CDC ACM

- directly supported by Zephyr
- natural fit for bring-up on a development kit
- easy to debug manually and from Python
- fits the adapter-driven JSON-lines scaffold already defined

## Zephyr Behavior Reference

Zephyr CDC ACM provides a UART-like interface to the application and presents a
serial device to the host.

This implementation should use the CDC ACM UART model, not a custom USB bulk
interface for the first pass.

## First-Pass Responsibilities Of `gateway_interface`

The gateway interface layer should:

- initialize the CDC ACM UART device
- receive host bytes
- assemble newline-delimited messages
- parse JSON command messages
- populate `gateway_command_t`
- dispatch to `gateway_app`
- serialize outbound events as JSON-lines

It should not:

- execute BLE logic
- manage sensor session semantics
- parse sensor payloads

## Layer Split

### `gateway_interface`

Responsibilities:

- CDC ACM UART setup
- RX/TX buffering
- line assembly
- JSON parse/serialize
- callback dispatch

### `gateway_app`

Responsibilities:

- command switch
- handoff to scheduler/backend layer

### BLE scheduler/backend

Responsibilities:

- scan
- connect
- disconnect
- subscribe
- read
- write
- notification forwarding

## Initialization Model

At startup:

1. initialize USB CDC ACM UART
2. wait until the host opens the port if required by the chosen flow
3. initialize interface callbacks
4. initialize BLE stack/backend
5. emit boot/ready messages

The scaffold already emits a boot line from `gateway_app`. The first transport
milestone is to ensure `hello -> ready` works over the real CDC ACM link.

## RX Model

### Recommended first-pass approach

Use a static line buffer with byte-wise accumulation.

State needed:

- RX buffer array
- RX write index
- overflow flag or reset behavior

Behavior:

- read available bytes from UART
- append bytes until newline
- on newline:
  - terminate line as C string
  - parse line
  - dispatch command
  - clear buffer
- on overflow:
  - discard current partial line
  - emit structured `error` if practical
  - reset state

## TX Model

All outbound messages should be emitted as:

- one compact JSON object
- followed by `\n`

This applies to:

- `ready`
- `error`
- `scan_result`
- `scan_complete`
- `sensor_connected`
- `sensor_disconnected`
- `subscribe_complete`
- `write_complete`
- `read_result`
- `notification`
- `status`

## `gateway_interface_poll()` Model

`gateway_interface_poll()` should be the central pump for first-pass RX work.

It should:

1. poll the CDC ACM UART for available bytes
2. move bytes into the line buffer
3. detect completed lines
4. parse one line into a `gateway_command_t`
5. call `g_callbacks.on_command(&command)`

It should return quickly and be safe to call repeatedly from the main loop.

## Parsing Scope

First-pass parsing only needs to support the adapter-driven command set:

- `hello`
- `scan_start`
- `scan_stop`
- `connect_addresses`
- `disconnect_addresses`
- `disconnect_all`
- `subscribe`
- `unsubscribe`
- `gatt_write`
- `gatt_read`
- `get_status`

Legacy session-oriented command parsing is no longer required.

## Required Parsed Fields

Depending on command:

- `type`
- `request_id`
- `timeout_ms`
- `address`
- `characteristic_uuid`
- `without_response`
- `payload`
- `addresses`
- `sensor_count`
- per-sensor:
  - `address`
  - `sensor_key`
  - `sensor_type`

## Suggested First-Pass Parser Strategy

Keep parsing intentionally narrow.

Recommended approach:

- use a small JSON parser or tightly scoped field extraction
- reject malformed or unknown commands with structured `error`

The first pass does not need a fully generic document model.

## Command Handling Expectations

After parse, `gateway_interface` passes the command to `gateway_app`, which
already routes the adapter-driven command set to scheduler functions.

Examples:

- `scan_start` -> `ble_scheduler_start_scan(...)`
- `connect_addresses` -> `ble_scheduler_connect_addresses(...)`
- `subscribe` -> `ble_scheduler_subscribe(...)`
- `gatt_write` -> `ble_scheduler_gatt_write(...)`
- `gatt_read` -> `ble_scheduler_gatt_read(...)`

## Completion And Error Expectations

For command-driven operations:

- the gateway should eventually emit a completion event or an `error`
- if a request includes `request_id`, the completion/error should echo it

Examples:

- `hello` -> `ready`
- `connect_addresses` -> `sensor_connected` or `error`
- `subscribe` -> `subscribe_complete` or `error`
- `gatt_write` -> `write_complete` or `error`
- `gatt_read` -> `read_result` or `error`

Streaming notifications and unsolicited disconnects may not include
`request_id`.

## Recommended CDC ACM Runtime Behavior

First-pass guidance:

- tolerate host reconnects cleanly
- do not assume data is available immediately after boot
- if required, wait for host terminal/open state before heavy logging

The gateway should be robust to:

- no host connected yet
- host opening late
- partial lines
- repeated reconnect during development

## Suggested First Implementation Milestones

### Milestone 1

- CDC ACM enumerates
- `hello` command can be received
- `ready` response can be sent

### Milestone 2

- `scan_start` command parsed
- gateway emits `scan_complete`

### Milestone 3

- `connect_addresses` parsed
- stub success/error events emitted

### Milestone 4

- `subscribe`, `gatt_write`, and `gatt_read` parsed
- stub completion events emitted

### Milestone 5

- real BLE operations wired behind those commands

## Suggested Initial Zephyr Project Needs

The gateway project will need normal Zephyr project support for CDC ACM:

- USB device support enabled
- CDC ACM UART device configured
- UART access in the application layer

This should be implemented in a way that keeps the rest of the gateway logic
transport-agnostic.

## Debugging Model

The interface should be testable without full BLE support.

Examples:

- open the enumerated serial device from the host
- send `{"type":"hello","request_id":"1"}`
- confirm `{"type":"ready"}` response

This gives a clean bring-up path before BLE functionality is added.

## Recommended Next Implementation Steps

1. Add Zephyr CDC ACM UART initialization in `gateway_interface`.
2. Implement byte ingestion and line buffering in `gateway_interface_poll()`.
3. Implement `hello` parsing and `ready` response.
4. Then add `scan_start` parsing and `scan_complete`.
5. Only after transport confidence, add real BLE command execution.

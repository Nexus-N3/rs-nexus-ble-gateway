# Nexus BLE Gateway

## Overview

`rs-nexus-ble-gateway` is a BLE-to-host streaming gateway. It runs on the gateway board, manages multiple BLE sensor links, and exposes a host-facing serial API so a PC-side application can:

- discover supported BLE sensors
- connect to one or more sensor addresses
- subscribe to sensor notification characteristics
- issue GATT reads and writes
- receive sensor data as JSON notifications or compact binary stream frames
- query gateway transport and BLE RX statistics

The current validated target for the host tooling is up to 9 simultaneous Movella DOT links at 60 Hz.

## What The Gateway Does

At a system level, the gateway has three jobs:

1. Manage BLE central behavior on the embedded side.
2. Bridge data and control over a UART serial link to the host.
3. Keep the host protocol simple enough that a Python SDK or customer sample can run immediately.

The host never talks to Zephyr directly. It talks to the gateway over JSON-lines commands on the serial port, and receives a mix of JSON events plus optional binary stream frames.

## Key Capabilities

- BLE scanning for nearby sensors
- explicit connect by sensor address
- explicit disconnect by sensor address or disconnect-all
- GATT subscribe, unsubscribe, read, and write
- binary streaming mode for high-rate notification paths
- per-sensor BLE RX accounting and drop diagnostics
- gateway transport queue statistics for host-side throughput debugging

## Host Transport

The host transport is a serial UART connection. In the current host tools, the Python side uses:

- baud rate: `1000000`
- newline-delimited JSON for commands and control/status events
- a binary frame format for high-rate streaming notifications when `binary_notifications=true`

The default host serial path in the Python tools is:

```text
/dev/serial/by-id/usb-SEGGER_J-Link_001057755524-if02
```

## Host Protocol Model

### Request Model

The gateway accepts one JSON object per line. Most commands include a `request_id` string so the host can correlate responses.

Example:

```json
{"type":"hello","request_id":"hello_1","protocol_version":1,"client":"example"}
```

### Response Model

The gateway emits:

- command acknowledgements such as `hello_ack`, `subscribe_complete`, `write_complete`
- streaming data such as `notification` or binary stream frames
- async lifecycle events such as `sensor_connected` and `sensor_disconnected`
- diagnostics such as `gateway_transport_stats` and `ble_notification_rx_stats`
- `error` responses when a command fails

## Supported Commands

The embedded parser currently accepts these command types:

- `hello`
- `get_status`
- `reset_session`
- `scan_start`
- `scan_stop`
- `connect_addresses`
- `disconnect_addresses`
- `disconnect_all`
- `subscribe`
- `unsubscribe`
- `gatt_write`
- `gatt_read`

## Python Helper Snippet

This is the basic pattern a customer application can use before moving to the SDK:

```python
import json
import serial

ser = serial.Serial(
    port="/dev/serial/by-id/usb-SEGGER_J-Link_001057755524-if02",
    baudrate=1_000_000,
    timeout=0.2,
    write_timeout=1.0,
)

def send_json(obj: dict) -> None:
    ser.write((json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8"))
    ser.flush()

def read_line() -> str:
    return ser.readline().decode("utf-8", errors="replace").strip()
```

## Command Reference

### `hello`

Use this first to confirm that the host and gateway agree on the protocol.

Request:

```python
send_json({
    "type": "hello",
    "request_id": "hello_1",
    "protocol_version": 1,
    "client": "customer_example",
})
```

Example response:

```json
{"type":"hello_ack","request_id":"hello_1","ok":true,"protocol_version":1}
```

### `reset_session`

Clears scheduler session state before a new run.

Request:

```python
send_json({
    "type": "reset_session",
    "request_id": "reset_1",
})
```

Example response:

```json
{"type":"reset_session_complete","request_id":"reset_1","ok":true}
```

### `scan_start`

Starts BLE discovery for a host-selected timeout.

Request:

```python
send_json({
    "type": "scan_start",
    "request_id": "scan_1",
    "timeout_ms": 5000,
})
```

Example responses:

```json
{"type":"scan_result","request_id":"scan_1","address":"D4:22:CD:00:11:22","name":"Movella DOT","rssi":-51,"service_uuids":[]}
{"type":"scan_complete","request_id":"scan_1"}
```

### `scan_stop`

Stops scanning early if the host no longer needs discovery results.

Request:

```python
send_json({
    "type": "scan_stop",
    "request_id": "scan_stop_1",
})
```

### `connect_addresses`

Requests one or more explicit address connections.

Request:

```python
send_json({
    "type": "connect_addresses",
    "request_id": "connect_1",
    "addresses": [
        "D4:22:CD:00:11:22",
        "D4:22:CD:00:11:23",
    ],
})
```

Example responses:

```json
{"type":"sensor_connected","request_id":"connect_1","sensor_id":1,"address":"D4:22:CD:00:11:22"}
{"type":"sensor_connected","request_id":"connect_1","sensor_id":2,"address":"D4:22:CD:00:11:23"}
```

A failed connect may appear as:

```json
{"type":"sensor_disconnected","request_id":"connect_1","address":"D4:22:CD:00:11:22","reason":-110}
```

Or as a command error:

```json
{"type":"error","request_id":"connect_1","error_code":-3,"message":"sensor_not_found"}
```

### `disconnect_addresses`

Disconnects a selected set of active links.

Request:

```python
send_json({
    "type": "disconnect_addresses",
    "request_id": "disconnect_1",
    "addresses": [
        "D4:22:CD:00:11:22",
    ],
})
```

Example response:

```json
{"type":"sensor_disconnected","request_id":"disconnect_1","address":"D4:22:CD:00:11:22","active_connection_count":0,"reason":19}
```

### `disconnect_all`

Disconnects every active BLE sensor link.

Request:

```python
send_json({
    "type": "disconnect_all",
    "request_id": "disconnect_all_1",
})
```

### `subscribe`

Subscribes to a characteristic on an active sensor connection.

Important field:

- `binary_notifications`
  - `false`: gateway forwards notifications as JSON
  - `true`: gateway forwards notifications as compact binary stream frames

Request:

```python
send_json({
    "type": "subscribe",
    "request_id": "subscribe_1",
    "address": "D4:22:CD:00:11:22",
    "characteristic_uuid": "15172002-4947-11e9-8646-d663bd873d93",
    "binary_notifications": True,
})
```

Example response:

```json
{"type":"subscribe_complete","request_id":"subscribe_1","address":"D4:22:CD:00:11:22","sensor_id":1,"characteristic_uuid":"15172002-4947-11e9-8646-d663bd873d93","ok":true}
```

### `unsubscribe`

Stops a prior subscription.

Request:

```python
send_json({
    "type": "unsubscribe",
    "request_id": "unsubscribe_1",
    "address": "D4:22:CD:00:11:22",
    "characteristic_uuid": "15172002-4947-11e9-8646-d663bd873d93",
})
```

Example response:

```json
{"type":"unsubscribe_complete","request_id":"unsubscribe_1","address":"D4:22:CD:00:11:22","characteristic_uuid":"15172002-4947-11e9-8646-d663bd873d93","ok":true}
```

### `gatt_write`

Writes a hex payload to a GATT characteristic.

Request:

```python
send_json({
    "type": "gatt_write",
    "request_id": "write_1",
    "address": "D4:22:CD:00:11:22",
    "characteristic_uuid": "15172001-4947-11e9-8646-d663bd873d93",
    "payload_hex": "01011A",
    "without_response": True,
})
```

Example response:

```json
{"type":"write_complete","request_id":"write_1","address":"D4:22:CD:00:11:22","characteristic_uuid":"15172001-4947-11e9-8646-d663bd873d93","ok":true}
```

### `gatt_read`

Reads a characteristic and returns a hex payload.

Request:

```python
send_json({
    "type": "gatt_read",
    "request_id": "read_1",
    "address": "D4:22:CD:00:11:22",
    "characteristic_uuid": "15173001-4947-11e9-8646-d663bd873d93",
})
```

Example response:

```json
{"type":"read_result","request_id":"read_1","address":"D4:22:CD:00:11:22","characteristic_uuid":"15173001-4947-11e9-8646-d663bd873d93","payload_hex":"6400","ok":true}
```

### `get_status`

Requests gateway status plus transport and BLE RX diagnostics.

Request:

```python
send_json({
    "type": "get_status",
    "request_id": "status_1",
})
```

Example response sequence:

```json
{"type":"status","request_id":"status_1","gateway":{"state":"ready","backend":"stub","connected_count":0}}
{"type":"gateway_transport_stats","control_ring_bytes":0,"stream_ring_bytes":0,"control_ring_drops":0,"stream_ring_drops":0,"control_enqueue_success":0,"control_enqueue_drops":0,"stream_enqueue_success":0,"stream_enqueue_drops":0,"control_bytes_enqueued":0,"stream_bytes_enqueued":0,"control_bytes_dequeued":0,"stream_bytes_dequeued":0,"control_tx_done":0,"stream_tx_done":0,"control_tx_aborted":0,"stream_tx_aborted":0,"control_tx_start_failures":0,"stream_tx_start_failures":0,"tx_in_progress":false,"active_queue_kind":0,"active_len":0}
{"type":"ble_notification_rx_stats_complete","request_id":"status_1","summary_enqueued":true}
```

When sensor stats are available, the gateway may also emit one or more:

- `ble_notification_rx_stats`
- `ble_notification_rx_stats_summary`

## Streaming Modes

### JSON Notification Mode

If a subscription uses `binary_notifications=false`, the host receives JSON notification events:

```json
{"type":"notification","address":"D4:22:CD:00:11:22","characteristic_uuid":"15172002-4947-11e9-8646-d663bd873d93","payload_hex":"78563412","payload_len":4,"gateway_timestamp_us":123456789}
```

This mode is easier to debug, but it is less efficient for high-throughput multi-sensor streaming.

### Binary Stream Mode

If a subscription uses `binary_notifications=true`, the gateway emits compact binary frames:

```text
0-1   magic                 0xA5 0x5A
2     version               0x01
3     sensor_id             uint8
4-11  gateway_timestamp_us  uint64 little-endian
12    payload_len           uint8
13..  payload bytes
last  checksum              sum(frame[2:last-1]) & 0xFF
```

This is the mode used by the current high-rate Movella DOT host sample.

## Gateway Statistics

The gateway exposes two main diagnostic groups.

### Transport Statistics

`gateway_transport_stats` reports host-link queue and transmit behavior, including:

- control ring occupancy and drops
- stream ring occupancy and drops
- enqueue success and drop counts
- bytes enqueued and dequeued
- transmit completions and aborts
- transmit start failures

These values are useful when validating high-rate streaming or diagnosing host bottlenecks.

### BLE RX Statistics

`ble_notification_rx_stats` reports per-sensor notification health, including:

- `notification_count`
- `timestamp_gap_events`
- `estimated_dropped_packets`
- `timestamp_reset_events`
- `timestamp_discontinuity_events`
- `last_sensor_timestamp_us`
- `subscription_lookup_misses`
- `json_fallback_notifications`
- `notification_queue_accepted`
- `notification_queue_dropped`
- `notification_queue_flushed`
- `stream_enqueue_success`
- `stream_enqueue_dropped`
- `json_forward_success`
- `json_forward_dropped`

These are the main counters to watch when pushing multiple sensors at 60 Hz.

## Error Handling

Command failures are returned as JSON `error` objects:

```json
{"type":"error","request_id":"write_1","error_code":-3,"message":"gatt_write_failed"}
```

Common failure conditions include:

- sensor address not present in current scan cache
- GATT write/read/subscribe failure
- disconnect request for sensors that are already gone
- subscription registration failure
- host command parse mismatch or unsupported command

## Engineering Notes

- The protocol is intentionally simple enough to drive from Python.
- The control plane is JSON-lines for readability and debugging.
- The streaming plane supports binary framing to reduce host overhead for multi-sensor data paths.
- The current host SDK and sample clients live under [tools/host](/home/mike/Desktop/apps/dev/rs-nexus-project/rs-nexus-ble-gateway/tools/host).

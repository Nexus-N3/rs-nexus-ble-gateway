# BLE Gateway Host Interface Contract

## Purpose

Define the first-pass interface between `rs-nexus-os` and `rs-nexus-ble-gateway`.

This contract is focused on preserving the current sensor plugin model in
`rs-nexus-os` while moving all low-level BLE execution to the gateway.

The intended split is:

- `rs-nexus-os` owns sensor inventory, subject mapping, orchestration, parsing,
  storage, compute, and higher-level product behavior.
- `rs-nexus-ble-gateway` owns scanning, connecting, GATT discovery,
  subscription, writes, stream control, notification receipt, and BLE health.

## Goals

- Keep BLE sensor plugins in `rs-nexus-os` as unchanged as possible.
- Allow `SensorManager` to choose which BLE adapter implementation is active at
  runtime.
- Keep the gateway generic at the transport/interface level.
- Keep raw payload parsing on the host side in the sensor plugins.
- Support mixed sessions:
  - multiple BLE sensor types, such as Movella DOT and Movesense
  - BLE sensors plus non-BLE sensors, such as USB cameras

## Non-Goals For First Pass

- Moving sensor parsing logic into the gateway
- Moving subject/location assignment into the gateway
- Replacing the current direct `Bleak` adapter
- Implementing a production binary framing protocol
- Moving discovery matching to the gateway

## First-Pass Ownership

### `rs-nexus-os`

- Select active BLE adapter implementation at runtime
- Instantiate sensor plugins
- Load sensor BLE spec from plugin YAML/spec files
- Match discovered devices to requested sensors
- Decide which discovered addresses should connect
- Trigger setup, identify, stream start, and stream stop through the adapter
- Parse raw notification payloads into sensor-specific data models
- Route parsed data into subjects, storage, compute, and events

### `rs-nexus-ble-gateway`

- Scan for BLE devices
- Report discovered devices to host
- Connect and disconnect devices by address
- Discover required GATT services and characteristics
- Subscribe to notifications
- Execute characteristic writes and reads
- Execute identify/start/stop/configuration commands
- Receive BLE notification payloads
- Forward raw notification payloads and BLE state changes to host

## Design Principle

The new gateway-backed BLE adapter should preserve the current plugin-facing
adapter behavior as much as possible.

From a plugin perspective:

- it still receives "an adapter"
- it still calls adapter methods during `setup(...)`, `identify(...)`,
  `start_stream(...)`, and `stop_stream(...)`
- it still receives raw notification payloads through the same callback path

The difference is that the new adapter does not execute BLE locally. It
translates those operations into gateway protocol commands.

## First-Pass Discovery Model

For the first pass, discovery matching stays on the host in `rs-nexus-os`.

Flow:

1. Host requests gateway scan.
2. Gateway returns discovered BLE devices with advertisement metadata.
3. Host `SensorManager` matches discovered devices to requested sensor plugins.
4. Host selects the addresses that should connect.
5. Host sends connect requests for those addresses.

Reasons:

- preserves current sensor-manager matching behavior
- keeps the gateway generic initially
- minimizes first-pass behavior changes in `rs-nexus-os`

Future option:

- move matching into the gateway once the host/gateway contract is stable

## Gateway Adapter Model In `rs-nexus-os`

The runtime should continue to resolve sensors by adapter family.

Example:

- `BLE` -> either host `Bleak` adapter or gateway BLE adapter
- `USB_CAMERA` -> existing USB camera adapter

This means:

- multiple BLE sensor types can share one active BLE backend
- BLE and camera sensors can coexist in the same session

The new gateway adapter should be a new implementation behind the BLE adapter
family, not a rewrite of sensor plugins.

## Required Host-Side Runtime Selection

`rs-nexus-os` should support selecting the BLE backend at launch.

Example conceptual modes:

- `BLE_BACKEND=host`
- `BLE_BACKEND=gateway`

The exact config mechanism can be CLI, env var, config file, or startup
settings, but the outcome should be:

- `SensorManager` remains adapter-driven
- `AdapterPool` resolves `BLE` to the chosen implementation

## First-Pass Interface Style

Use JSON-lines for the prototype interface.

Each message is one JSON object per line.

Reasons:

- simple to inspect and debug
- easy to implement on both host and gateway
- aligns with the existing gateway plan direction

Future option:

- replace with binary framing once the behavior contract is stable

## Protocol Derivation Rule

This document is derived from the Python-side
`GatewayBLEAdapter` contract in `rs-nexus-os`.

That means:

- the Python adapter-facing behavior is the compatibility source of truth
- this gateway protocol exists to satisfy that adapter contract
- gateway firmware implementation should conform to this protocol

Order of authority:

1. `GatewayBLEAdapter` behavior contract in `rs-nexus-os`
2. host/gateway wire protocol in this document
3. gateway firmware implementation details

## Adapter Method To Protocol Mapping

This section makes the contract explicit by mapping each required
`GatewayBLEAdapter` behavior onto gateway protocol commands and events.

### `discover_devices(names, timeout)`

Host behavior:

- adapter requests remote scan
- adapter collects discovered devices
- adapter returns enough metadata for host-side matching

Protocol mapping:

- host -> `scan_start`
- optional host -> `scan_stop`
- gateway -> repeated `scan_result`
- gateway -> `scan_complete`

### `create_transport_client(address, loop, disconnected_callback)`

Host behavior:

- adapter creates a local proxy transport client
- no gateway command is required at creation time

Protocol mapping:

- no required wire command
- host stores local proxy state keyed by address

### `connect(transport_client)`

Host behavior:

- adapter requests remote connect by address
- adapter waits for success/failure

Protocol mapping:

- host -> `connect_addresses`
- gateway -> `sensor_connected` or `error`

### `disconnect(transport_client)`

Host behavior:

- adapter requests remote disconnect by address
- adapter waits for success/failure or disconnect event

Protocol mapping:

- host -> `disconnect_addresses`
- gateway -> `sensor_disconnected` or `error`

### `connect_to_device(device, adapter, timeout)`

Host behavior:

- compatibility wrapper over remote connect for one sensor

Protocol mapping:

- same as `connect(transport_client)`

### `connect_all(devices, adapter, timeout)`

Host behavior:

- adapter requests remote connect for multiple addresses
- may be implemented sequentially or in batches

Protocol mapping:

- host -> `connect_addresses`
- gateway -> one or more `sensor_connected`
- gateway -> optional `error`

### `set_notify_callback(transport_client, uuid, callback_func)`

Host behavior:

- adapter registers callback locally
- adapter requests remote notify subscription
- raw gateway notifications are routed back through the stored callback

Protocol mapping:

- host -> `subscribe`
- gateway -> `subscribe_complete` or `error`
- gateway -> repeated `notification`

### `write(transport_client, uuid, char)`

Host behavior:

- adapter requests remote write
- adapter waits for completion/failure

Protocol mapping:

- host -> `gatt_write`
- gateway -> `write_complete` or `error`

### `read(transport_client, uuid)`

Host behavior:

- adapter requests remote read
- adapter waits for payload/failure

Protocol mapping:

- host -> `gatt_read`
- gateway -> `read_result` or `error`

### Disconnect callback routing

Host behavior:

- proxy transport client disconnect callback must fire when the gateway reports
  loss of a sensor connection

Protocol mapping:

- gateway -> `sensor_disconnected`

### Notification callback routing

Host behavior:

- sensor/plugin callback path must receive the same raw notification behavior
  expected from the current local BLE path

Protocol mapping:

- gateway -> `notification`

## Host To Gateway Commands

### `hello`

Purpose:

- verify link is up
- negotiate protocol version/capabilities

Example:

```json
{
  "type": "hello",
  "protocol_version": 1,
  "client": "rs-nexus-os"
}
```

### `scan_start`

Purpose:

- request a scan for BLE devices

Fields:

- `request_id`
- `timeout_ms`
- optional host-side filtering hints

Example:

```json
{
  "type": "scan_start",
  "request_id": "scan_001",
  "timeout_ms": 5000
}
```

### `scan_stop`

Purpose:

- stop an in-progress scan

Example:

```json
{
  "type": "scan_stop",
  "request_id": "scan_001"
}
```

### `connect_addresses`

Purpose:

- request connection to one or more already-selected sensor addresses

Fields:

- `request_id`
- `sensors`

Each sensor entry should include:

- `address`
- `sensor_key`
- `sensor_type`
- optional serialized BLE spec payload

This command exists to satisfy:

- `connect(transport_client)`
- `connect_to_device(...)`
- `connect_all(...)`

Example:

```json
{
  "type": "connect_addresses",
  "request_id": "connect_001",
  "sensors": [
    {
      "address": "D4:22:CD:07:B4:5E",
      "sensor_key": "subject_1_left_ankle",
      "sensor_type": "movella_dot"
    }
  ]
}
```

### `disconnect_addresses`

Purpose:

- disconnect specific sensor addresses

This command exists to satisfy:

- `disconnect(transport_client)`

Example:

```json
{
  "type": "disconnect_addresses",
  "request_id": "disconnect_001",
  "addresses": [
    "D4:22:CD:07:B4:5E"
  ]
}
```

### `disconnect_all`

Purpose:

- disconnect all active BLE sensors

Example:

```json
{
  "type": "disconnect_all",
  "request_id": "disconnect_all_001"
}
```

### `gatt_write`

Purpose:

- perform a remote characteristic write

Used by:

- sensor plugin `setup(...)`
- sensor plugin `identify(...)`
- sensor plugin `start_stream(...)`
- sensor plugin `stop_stream(...)`

This command exists to satisfy:

- `write(transport_client, uuid, char)`

Fields:

- `request_id`
- `address`
- `characteristic_uuid`
- `payload`
- `payload_encoding`
- `without_response`

Example:

```json
{
  "type": "gatt_write",
  "request_id": "write_001",
  "address": "D4:22:CD:07:B4:5E",
  "characteristic_uuid": "15174001-4947-11e9-8646-d663bd873d93",
  "payload": "AQI=",
  "payload_encoding": "base64",
  "without_response": true
}
```

### `gatt_read`

Purpose:

- perform a remote characteristic read when needed

This command exists to satisfy:

- `read(transport_client, uuid)`

Example:

```json
{
  "type": "gatt_read",
  "request_id": "read_001",
  "address": "D4:22:CD:07:B4:5E",
  "characteristic_uuid": "00002a19-0000-1000-8000-00805f9b34fb"
}
```

### `subscribe`

Purpose:

- subscribe to a remote characteristic notification stream

Fields:

- `request_id`
- `address`
- `characteristic_uuid`

This command exists to satisfy:

- `set_notify_callback(transport_client, uuid, callback_func)`

Example:

```json
{
  "type": "subscribe",
  "request_id": "sub_001",
  "address": "D4:22:CD:07:B4:5E",
  "characteristic_uuid": "15172002-4947-11e9-8646-d663bd873d93"
}
```

### `unsubscribe`

Purpose:

- stop notifications for a characteristic if needed

Example:

```json
{
  "type": "unsubscribe",
  "request_id": "unsub_001",
  "address": "D4:22:CD:07:B4:5E",
  "characteristic_uuid": "15172002-4947-11e9-8646-d663bd873d93"
}
```

### `get_status`

Purpose:

- request gateway and sensor health/status

Example:

```json
{
  "type": "get_status",
  "request_id": "status_001"
}
```

## Gateway To Host Events

### `ready`

Purpose:

- indicate gateway interface is available

### `scan_result`

Purpose:

- report one discovered device

Fields:

- `request_id`
- `address`
- `name`
- `rssi`
- optional advertisement metadata

Example:

```json
{
  "type": "scan_result",
  "request_id": "scan_001",
  "address": "D4:22:CD:07:B4:5E",
  "name": "Movella DOT",
  "rssi": -58,
  "service_uuids": []
}
```

### `scan_complete`

Purpose:

- indicate scan is complete

### `sensor_connected`

Purpose:

- report successful connection

Fields:

- `request_id`
- `address`

This event exists to satisfy completion for:

- `connect(transport_client)`
- `connect_to_device(...)`
- `connect_all(...)`

### `sensor_disconnected`

Purpose:

- report disconnect

Fields:

- `address`
- `reason`

This event exists to satisfy:

- remote disconnect reporting
- proxy client disconnect callback routing
- sensor-manager disconnect propagation

### `write_complete`

Purpose:

- acknowledge a `gatt_write`

Fields:

- `request_id`
- `address`
- `ok`
- optional `error`

This event exists to satisfy completion for:

- `write(transport_client, uuid, char)`

### `read_result`

Purpose:

- return a `gatt_read` payload

Fields:

- `request_id`
- `address`
- `characteristic_uuid`
- `payload`
- `payload_encoding`

This event exists to satisfy completion for:

- `read(transport_client, uuid)`

### `subscribe_complete`

Purpose:

- acknowledge a `subscribe`

This event exists to satisfy completion for:

- `set_notify_callback(transport_client, uuid, callback_func)`

### `notification`

Purpose:

- deliver raw BLE notification payload to host

Fields:

- `address`
- `characteristic_uuid`
- `gateway_timestamp_us`
- `payload`
- `payload_encoding`

This event exists to satisfy:

- remote raw notify delivery
- sensor/plugin callback routing

Example:

```json
{
  "type": "notification",
  "address": "D4:22:CD:07:B4:5E",
  "characteristic_uuid": "15172002-4947-11e9-8646-d663bd873d93",
  "gateway_timestamp_us": 1234567890,
  "payload": "AAECAwQ=",
  "payload_encoding": "base64"
}
```

### `status`

Purpose:

- return gateway or per-sensor health

Fields may include:

- `address`
- `connected`
- `streaming`
- `rssi`
- `frames_received`
- `drops_estimate`
- `last_frame_gateway_time_us`

### `error`

Purpose:

- report command or sensor failure

Fields:

- `request_id`
- optional `address`
- `error_code`
- `message`

## First-Pass Sensor Spec Handling

For the first pass, the gateway should not need the full plugin spec for
discovery matching because matching remains on the host.

However, the gateway may still need BLE spec details to execute remote
operations reliably.

The host should be prepared to provide a normalized BLE operation spec with
fields such as:

- sensor type
- service UUIDs
- characteristic UUIDs
- notify characteristic UUID
- write/control characteristic UUID
- command payloads
- write mode flags

First-pass rule:

- the gateway should not require full sensor plugin semantics
- the gateway should only require the normalized BLE operation details needed to
  execute the requested remote operation

Recommended normalized spec areas:

- advertisement hints
- service UUIDs
- characteristic UUIDs
- notify/write/read role metadata
- command payload bytes
- write-with-response vs write-without-response flags

This should be normalized into one host-side schema instead of sending raw
plugin-internal structures directly.

Recommended first-pass transport rule:

- include only the BLE operation detail actually needed for the command being
  executed
- do not force the gateway to ingest full plugin specs up front unless that
  proves necessary

## First-Pass Discovery Decision

### Keep On Host

Recommended for first implementation.

Pros:

- preserves current `SensorManager` matching model
- simpler gateway
- fewer gateway assumptions about sensor classes/plugins

Cons:

- host still owns the matching function
- two-step flow: scan then connect selection

### Move To Gateway Later

Possible future step after first-pass success.

Pros:

- more self-contained BLE gateway
- lower host-side matching responsibility

Cons:

- gateway must understand more sensor/plugin semantics

## Callback Routing Requirement

The gateway-backed BLE adapter must preserve the current event path as closely
as possible.

That means:

- gateway `notification` events should be routed into the same sensor/plugin
  callback behavior currently triggered by the direct BLE adapter
- gateway disconnect events should trigger the same disconnect callback path
- battery/button/identify-related notification flows should continue to emit the
  same sensor events where supported

Compatibility target:

- sensor plugins should not need to know whether BLE is local (`Bleak`) or
  remote (`BLE gateway`)

Concrete requirement:

- the gateway must emit enough information on each `notification` event for the
  host adapter to:
  - find the proxy client by address
  - find the registered callback by characteristic UUID
  - invoke that callback with the raw payload

Concrete requirement for disconnect:

- the gateway must emit enough information on each `sensor_disconnected` event
  for the host adapter to:
  - find the proxy client by address
  - mark it disconnected
  - invoke its stored disconnect callback

## Gateway State Requirements

The gateway must maintain enough per-address state to fulfill the
`GatewayBLEAdapter` contract.

Minimum first-pass gateway state per connected or discovered device:

- address
- advertised name if known
- connection state
- subscribed characteristic UUIDs
- cached characteristic handle mapping if required by the BLE stack
- last known RSSI if available

The gateway does not need to own subject mapping or parsed sensor state.

## Host State Requirements

The host adapter must maintain:

- proxy transport clients keyed by address
- pending request correlation keyed by `request_id`
- registered notify callbacks per address and characteristic UUID
- disconnect callbacks per proxy client

This split is intentional:

- gateway owns BLE execution state
- host owns plugin callback and orchestration state

## Request Correlation Requirement

All command/response style operations must support host-side correlation through
`request_id`.

This is required for:

- connect
- disconnect
- subscribe
- write
- read
- status

The gateway should echo the originating `request_id` in completion or error
messages where applicable.

Asynchronous events that are not direct command completions may omit
`request_id`, for example:

- unsolicited `sensor_disconnected`
- streaming `notification`

## Expected `rs-nexus-os` Changes

- add a new gateway-backed BLE adapter implementation
- keep current `Bleak` adapter unchanged
- allow BLE backend selection at runtime
- preserve `SensorManager` orchestration model
- preserve plugin parsing of raw binary notification payloads
- add a host-side gateway client/transport
- add request/response correlation handling for remote adapter operations

## Expected `rs-nexus-ble-gateway` Changes

- implement the host transport for JSON-lines commands/events
- support scan/connect/disconnect/read/write/subscribe operations
- emit raw notification events back to host
- maintain per-address connection state
- expose status and error reporting

More specifically, the gateway implementation must satisfy the Python adapter
contract by:

- accepting command-driven BLE execution requests from the host adapter
- returning explicit completion/error events for awaited operations
- preserving per-address BLE state long enough to service later write/subscribe
  calls
- emitting raw notifications without parsing them into sensor-specific payloads

## Open Questions

1. Should `connect_addresses` include normalized BLE spec every time, or should
   the host register a spec once and refer to it by ID afterward?
2. Does any current plugin require descriptor writes or operations beyond
   read/write/subscribe?
3. Should battery and button flows be first-pass requirements, or can they
   follow streaming support?
4. Should notification events include only raw payload and UUID, or also a
   gateway-assigned sensor handle?
5. Should host and gateway maintain both address and host-defined `sensor_key`
   for correlation?

## Recommended First Implementation Scope

Implement first-pass support for:

- `hello`
- `scan_start`
- `scan_result`
- `scan_complete`
- `connect_addresses`
- `sensor_connected`
- `disconnect_all`
- `disconnect_addresses`
- `sensor_disconnected`
- `subscribe`
- `subscribe_complete`
- `gatt_write`
- `write_complete`
- `gatt_read` if required by active plugins
- `read_result` if `gatt_read` is implemented
- `notification`
- `get_status`
- `status`
- `error`

This is enough to support:

- host-side discovery matching
- gateway-side BLE execution
- plugin-driven setup/start/stop behavior
- raw payload delivery back to host parsing

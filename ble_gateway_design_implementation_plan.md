# BLE Gateway Design and Implementation Plan

## 0. Board Selection

Phase 1:
  develop on nRF5340 DK first
  prove 1, 4, 8 DOTs with hardcoded Movella support

Phase 2:
  port same app to BL5340PA DVK or Lyra 24 453-00142-K1 DK
  focus on RF/FEM/external antenna behavior

Phase 3:
  port same app to nRF54L15 DK
  evaluate whether newer platform improves capacity/margin

## 1. Context and Motivation

The current BLE architecture uses the host computer or Raspberry Pi as the BLE central through Linux, BlueZ, Bleak, and Python. External USB dongles such as the BL654 currently operate as HCI controllers only. In that model, the application still depends on the host operating system for scanning, connection management, GATT discovery, notification subscription, and data delivery.

Testing showed that this model is highly host- and controller-dependent:

- Raspberry Pi 5 internal Bluetooth can handle 8 sensors when RF conditions are good.
- Raspberry Pi 5 with BL654 running latest Zephyr `hci_usb` reaches a practical limit of 5 sensors.
- Jetson with the same BL654 firmware performs much worse, with approximately 2 sensors as a practical limit.
- TP-Link UB5A failed at lower counts than BL654.
- RPi internal Bluetooth capacity drops when the case attenuates or detunes RF.

The conclusion is that the BLE workload itself is achievable, but the host/HCI dongle architecture has too much variability and insufficient control. A purpose-built BLE gateway should take full responsibility for BLE central behavior and expose a higher-level sensor data interface to the host.

## 2. Goal

Build a BLE gateway that offloads all BLE responsibilities from the host while preserving the existing Python-side product logic.

The gateway should:

- Scan for required sensors.
- Connect to the requested sensor set.
- Run GATT discovery and subscriptions.
- Apply sensor setup commands.
- Start and stop streams.
- Receive BLE notifications.
- Timestamp, buffer, and forward sensor frames to the host.
- Report per-sensor health, rate, drops, RSSI, disconnects, and errors.
- Hide BLE stack details from the Python application.

The Python host should continue to own:

- Subjects.
- Sensor requirements.
- Location mapping.
- Session orchestration.
- Data decoding and normalization where practical.
- Storage, networking, UI, and higher-level product behavior.

## 3. Non-Goals for the First Prototype

The first prototype should not attempt to be a fully generic product gateway. It should not immediately support every sensor type, every protocol option, or a production binary protocol.

The first prototype should focus on proving the hardest technical question:

> Can the selected gateway hardware and BLE stack sustain 8 to 10 Movella DOT sensors streaming at approximately 60 Hz?

Only after that is proven should the design be generalized for arbitrary sensor specs and production protocol details.

## 4. High-Level Architecture

### Current HCI Dongle Model

```text
Python application
  -> Bleak
  -> BlueZ
  -> Linux btusb/bluetooth kernel stack
  -> USB HCI dongle
  -> BLE sensors
```

### Proposed BLE Gateway Model

```text
Python application
  -> Gateway protocol over USB/UART/etc.
  -> BLE gateway firmware
  -> BLE sensors
```

The gateway firmware owns the BLE central role. The host no longer talks to individual BLE devices directly.

## 5. Layer Responsibilities

### Python Host Layer

The Python application remains the product brain. It sends session requirements to the gateway and receives sensor frames and health updates.

Responsibilities:

- Define required sensor types and counts.
- Assign discovered sensors to subject locations.
- Start and stop sessions.
- Receive raw or decoded sensor frames.
- Run existing data pipeline logic.
- Decide how to handle gateway-reported failures.

Example host request:

```json
{
  "type": "configure_session",
  "session_id": "session_001",
  "requirements": [
    {
      "sensor_type": "movella_dot",
      "count": 8,
      "sampling_rate_hz": 60,
      "locations": [
        "LEFT_ANKLE",
        "RIGHT_ANKLE",
        "LEFT_THIGH",
        "RIGHT_THIGH",
        "CHEST",
        "LOWER_BACK",
        "HEAD",
        "UPPER_BACK"
      ]
    }
  ]
}
```

### Gateway Application Layer

The gateway application receives host commands, maintains session state, and drives the BLE scheduler.

Responsibilities:

- Parse host commands.
- Maintain gateway session state.
- Track required sensors.
- Route discovered sensors to session requirements.
- Emit gateway events and sensor frames.
- Coordinate BLE scheduler and host interface.

### BLE Scheduler Layer

The BLE scheduler is the core of the gateway. It does not implement vendor-specific BLE APIs directly. It manages the high-level plan for scanning, connecting, configuring, streaming, health checking, and recovery.

Responsibilities:

- Decide when to scan.
- Decide which sensors to connect.
- Pace or stagger connections if needed.
- Run per-sensor state machines.
- Request GATT discovery and subscriptions.
- Send setup commands.
- Start streams.
- Monitor per-sensor rates, gaps, RSSI, and disconnects.
- Retry or mark sensors unhealthy.

The scheduler should be backend-agnostic.

### BLE Backend Layer

The BLE backend wraps the selected vendor SDK or BLE stack.

Potential backends:

- Zephyr / Nordic nRF Connect SDK backend.
- Silicon Labs backend.
- Mock backend for host-side testing.

Responsibilities:

- Start and stop scanning.
- Parse advertisements.
- Connect and disconnect.
- Discover GATT services and characteristics.
- Subscribe to notifications.
- Write setup commands.
- Receive notification callbacks.
- Query RSSI.
- Request connection parameters if supported.

Vendor-specific types should not leak out of this layer.

### Sensor Spec Layer

Sensor specs define how to identify, configure, and subscribe to a sensor type.

A sensor spec should contain:

- Sensor type name.
- Advertisement matching rules.
- Service UUIDs.
- Characteristic UUIDs.
- Notification characteristic.
- Control/write characteristic.
- Setup commands.
- Expected sampling rate.
- Payload forwarding mode.

The first implementation should hardcode Movella DOT. Later, specs can be serialized or generated from existing Python definitions.

### Host Interface Layer

The host interface transports commands, events, and frames between gateway and Python.

Possible transports:

- USB CDC serial for first prototype.
- USB bulk for production.
- UART for embedded integration.
- Ethernet or TCP if the gateway is a Linux-class device.

First prototype can use JSON-lines. Production should use binary framing with sequence numbers and CRC.

## 6. Backend Provisioning Strategy

The gateway should be designed around a common BLE backend contract. The selected backend should be chosen at build time.

Recommended shape:

```text
backend/
  ble_backend.h
  ble_backend_types.h
  zephyr/
    ble_backend_zephyr.c
  silabs/
    ble_backend_silabs.c
  mock/
    ble_backend_mock.c
```

Shared gateway code should call only `ble_backend.h`.

### Backend Contract

The backend should expose functions such as:

```c
int ble_backend_init(const ble_backend_callbacks_t *callbacks);
int ble_backend_get_capabilities(ble_backend_capabilities_t *out);

int ble_backend_start_scan(const sensor_spec_t *spec, uint32_t timeout_ms);
int ble_backend_stop_scan(void);

int ble_backend_assign_sensor(
    gateway_sensor_id_t sensor_id,
    const char *address,
    const sensor_spec_t *spec
);

int ble_backend_connect(gateway_sensor_id_t sensor_id);
int ble_backend_disconnect(gateway_sensor_id_t sensor_id);

int ble_backend_discover_gatt(gateway_sensor_id_t sensor_id);
int ble_backend_subscribe(
    gateway_sensor_id_t sensor_id,
    const char *characteristic_uuid
);

int ble_backend_write(
    gateway_sensor_id_t sensor_id,
    const char *characteristic_uuid,
    const uint8_t *data,
    size_t data_len,
    bool without_response
);

int ble_backend_request_connection_params(
    gateway_sensor_id_t sensor_id,
    const ble_conn_params_t *params
);

int ble_backend_get_rssi(gateway_sensor_id_t sensor_id, int8_t *rssi_out);

void ble_backend_poll(void);
```

The backend should emit callbacks such as:

```c
sensor_found(...)
connected(...)
gatt_ready(...)
subscribed(...)
notification(...)
disconnected(...)
error(...)
```

### Backend Selection

For Zephyr, backend selection can eventually be done with Kconfig:

```kconfig
choice RS_GATEWAY_BLE_BACKEND
    prompt "BLE backend"

config RS_GATEWAY_BLE_BACKEND_ZEPHYR
    bool "Zephyr/NCS backend"

config RS_GATEWAY_BLE_BACKEND_SILABS
    bool "Silicon Labs backend"

config RS_GATEWAY_BLE_BACKEND_MOCK
    bool "Mock backend"

endchoice
```

Early prototype can simply use a compile-time define in `gateway_config.h`.

## 7. Recommended Repository Layout

```text
rs-ble-gateway/
  app/
    gateway_app.c
    gateway_app.h

  scheduler/
    ble_scheduler.c
    ble_scheduler.h
    sensor_session.c
    sensor_session.h

  backend/
    ble_backend.h
    ble_backend_types.h

    zephyr/
      ble_backend_zephyr.c
      ble_backend_zephyr.h

    silabs/
      ble_backend_silabs.c
      ble_backend_silabs.h

    mock/
      ble_backend_mock.c
      ble_backend_mock.h

  sensors/
    sensor_spec.h
    sensor_spec.c
    movella_dot_spec.c
    movella_dot_spec.h
    movesense_spec.c
    movesense_spec.h

  interface/
    gateway_interface.c
    gateway_interface.h
    gateway_protocol.h
    frame_codec.c
    frame_codec.h

  config/
    gateway_config.h

  ports/
    zephyr/
      CMakeLists.txt
      prj.conf
      Kconfig
      boards/

    silabs/
      project files / slcp project
```

## 8. Zephyr Project Requirements

If Zephyr or Nordic nRF Connect SDK is used, the gateway still needs normal Zephyr project files.

### Zephyr `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rs_ble_gateway)

target_sources(app PRIVATE
  ../../app/gateway_app.c
  ../../scheduler/ble_scheduler.c
  ../../scheduler/sensor_session.c
  ../../backend/zephyr/ble_backend_zephyr.c
  ../../sensors/sensor_spec.c
  ../../sensors/movella_dot_spec.c
  ../../interface/gateway_interface.c
  ../../interface/frame_codec.c
)

target_include_directories(app PRIVATE
  ../..
  ../../app
  ../../scheduler
  ../../backend
  ../../backend/zephyr
  ../../sensors
  ../../interface
  ../../config
)
```

### Prototype `prj.conf`

The gateway is not an HCI USB dongle, so it should not use:

```conf
CONFIG_BT_HCI_RAW=y
CONFIG_USBD_BT_HCI=y
```

Instead, the gateway firmware itself is the BLE host and central application.

Illustrative starting point:

```conf
CONFIG_BT=y
CONFIG_BT_CENTRAL=y
CONFIG_BT_OBSERVER=y
CONFIG_BT_GATT_CLIENT=y

CONFIG_BT_MAX_CONN=10

CONFIG_LOG=y
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096

# Prototype host interface over USB CDC serial
CONFIG_USB_DEVICE_STACK=y
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_UART_LINE_CTRL=y
CONFIG_USB_CDC_ACM=y

# Adjust as needed per board
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
```

Exact Kconfig symbols may differ depending on upstream Zephyr, Nordic NCS, board support, and Zephyr version.

## 9. Gateway Protocol

### Prototype Protocol

Use JSON-lines initially because it is easy to debug.

Host commands:

```text
HELLO
CONFIGURE_SESSION
START_DISCOVERY
CONNECT_REQUIREMENTS
START_STREAM
STOP_STREAM
DISCONNECT_ALL
GET_STATUS
RESET_SESSION
```

Gateway events:

```text
READY
SENSOR_FOUND
SENSOR_CONNECTED
SENSOR_CONFIGURED
SENSOR_STREAMING
SENSOR_FRAME
SENSOR_HEALTH
SENSOR_DISCONNECTED
SESSION_READY
SESSION_FAILED
ERROR
```

Example frame event:

```json
{
  "type": "sensor_frame",
  "session_id": "session_001",
  "sensor_id": 3,
  "address": "D4:22:CD:07:B4:5E",
  "gateway_time_us": 1234567890,
  "sensor_time_us": 987654321,
  "sequence": 1442,
  "payload": "base64_payload"
}
```

### Production Protocol

Production should move to binary framing:

```text
magic
version
message_type
sequence
sensor_id
gateway_timestamp_us
payload_len
payload
crc
```

The production protocol should support:

- Sequence numbers.
- CRC.
- Heartbeats.
- Health events.
- Backpressure or overflow reporting.
- Gateway firmware version and backend capability reporting.

## 10. Sensor Spec Model

A backend-neutral sensor spec should contain plain data only.

Example conceptual fields:

```c
typedef struct {
    const char *sensor_type_name;
    const char *advertised_name_contains;
    const char *control_service_uuid;
    const char *control_characteristic_uuid;
    const char *notify_service_uuid;
    const char *notify_characteristic_uuid;
    const sensor_setup_command_t *setup_commands;
    size_t setup_command_count;
    uint32_t expected_rate_hz;
    bool forward_raw_payload;
} sensor_spec_t;
```

The backend converts UUID strings into SDK-specific UUID types.

For the first prototype, Movella DOT can be hardcoded. Later, the gateway can load or receive specs generated from existing Python-side definitions.

## 11. BLE Scheduling Design

The BLE scheduler should be an application-level scheduler. It does not replace the BLE link-layer scheduler inside the vendor stack. It controls the order and timing of higher-level BLE operations.

It should manage:

- Scan windows.
- Connection queue.
- Connection pacing.
- GATT discovery queue.
- Subscription queue.
- Setup command queue.
- Stream start timing.
- Stream health monitoring.
- Reconnect policy.

### Per-Sensor State Machine

Each sensor should have an independent state machine:

```text
IDLE
DISCOVERING
FOUND
CONNECTING
CONNECTED
DISCOVERING_GATT
CONFIGURING
SUBSCRIBING
READY
STREAMING
UNSTABLE
RECONNECTING
DISCONNECTING
DISCONNECTED
FAILED
```

### Scheduler Policy

The scheduler should have configurable policy fields:

```c
typedef struct {
    uint8_t max_parallel_connects;
    uint32_t connect_gap_ms;
    uint32_t post_connect_settle_ms;
    uint32_t startup_health_window_ms;
    uint32_t min_rate_hz;
    bool stagger_connections;
    bool stagger_stream_start;
} ble_scheduler_policy_t;
```

The first prototype can keep this simple and connect sequentially. Later, it can support more advanced connection pacing and retry strategies.

## 12. Buffering and Timing Requirements

The gateway must not depend on the host reading frames at exactly the BLE notification rate.

Notification callbacks should be short:

1. Timestamp frame.
2. Copy payload into a ring buffer.
3. Update per-sensor counters.
4. Return quickly.

A separate interface task should drain the frame queue to the host.

Required metrics:

- Frames received per sensor.
- Observed rate per sensor.
- Gap events.
- Estimated drops if packet sequence or sensor timestamp allows it.
- Gateway queue overflow count.
- Host interface backpressure count.
- RSSI if available.
- Disconnect/reconnect count.

## 13. Hardware/SDK Evaluation Plan

The architecture should support a backend bake-off.

Candidate families:

- Nordic nRF5340 or nRF54-class board/module using NCS/Zephyr.
- Silicon Labs BG24/BG29/Lyra-class board/module using Silicon Labs stack.
- Mock backend for host-side testing.

Do not choose final hardware based on marketing connection count. The meaningful test is sustained high-rate GATT notifications.

### Test Workload

For each candidate backend:

1. Connect 1 Movella DOT and forward raw notifications.
2. Verify 60 Hz for 60 seconds.
3. Test 4 DOTs.
4. Test 6 DOTs.
5. Test 8 DOTs.
6. Test 10 DOTs if available or once enough devices are available.
7. Repeat cold-start tests.
8. Test at product-relevant distance and enclosure geometry.

Success criteria:

- All required sensors connect.
- All required sensors stream at or above stability threshold.
- No persistent rate collapse.
- No unexplained queue overflow.
- Recovery behavior is deterministic.
- Gateway reports useful health information.

## 14. Implementation Milestones

### Milestone 0: Architecture Stub

- Create repository layout.
- Add shared headers.
- Add mock backend.
- Add host interface stub.
- Add Movella DOT spec stub.
- Add scheduler skeleton.

Output: firmware builds with mock backend and emits fake sensor frames.

### Milestone 1: One-DOT Gateway

- Implement one BLE backend.
- Scan for Movella DOT.
- Connect to one DOT.
- Discover GATT.
- Subscribe to notification characteristic.
- Start stream.
- Forward raw frames to host.
- Report rate and health.

Output: one DOT streams at 60 Hz through gateway interface.

### Milestone 2: Multi-DOT Proof

- Extend scheduler to multiple sensors.
- Connect 2, then 4 sensors.
- Add per-sensor health.
- Add frame queue.
- Add basic reconnect handling.

Output: 4 DOTs stream reliably.

### Milestone 3: Capacity Test

- Test 6, 8, and eventually 10 sensors.
- Run repeated cold starts.
- Compare Nordic/NCS and Silicon Labs backend if needed.

Output: hardware/backend decision based on measured sustained streaming.

### Milestone 4: Host Integration

- Implement Python gateway client.
- Map gateway sensor frames into existing Python sensor data pipeline.
- Keep subject/location/session management in Python.
- Replace Bleak path behind existing sensor manager abstraction.

Output: Python application can run a session through gateway instead of BlueZ/Bleak.

### Milestone 5: Production Protocol

- Replace JSON-lines frame path with binary framing.
- Add CRC.
- Add sequence numbers.
- Add firmware/version/capability reporting.
- Add structured errors.
- Add health telemetry.

Output: robust gateway-host protocol.

### Milestone 6: Product Hardening

- RF testing in enclosure.
- Long-duration soak tests.
- Reconnect testing.
- Power-cycle testing.
- Sensor dropout testing.
- Firmware update strategy.
- Manufacturing/provisioning plan.

Output: production-ready gateway subsystem.

## 15. Key Risks

### BLE Stack Capacity Risk

A vendor stack may claim many simultaneous connections but still fail the high-rate notification workload.

Mitigation: test hardcoded Movella DOT streaming before building generic features.

### RF Risk

The RPi internal tests showed RF margin can dominate performance. Gateway hardware must support suitable antenna placement.

Mitigation: prefer modules/dev boards with external antenna options and test in product-like enclosure geometry early.

### Host Interface Bottleneck

If the gateway forwards all raw frames, the host interface must handle bursts without dropping.

Mitigation: ring buffers, sequence numbers, queue overflow reporting, and eventually binary framing.

### Over-Generalization Risk

A generic sensor-spec engine could consume time before core BLE capacity is proven.

Mitigation: hardcode Movella DOT first, generalize later.

## 16. Immediate Next Steps

1. Create the repository skeleton.
2. Implement mock backend and JSON-lines host interface.
3. Implement Movella DOT spec stub.
4. Select first gateway board/backend candidate.
5. Implement one-DOT scan/connect/subscribe/forward.
6. Measure one-DOT rate at the gateway before scaling.
7. Scale to 4, 6, and 8 DOTs.
8. If the first backend cannot reach target, implement the second backend using the same scheduler and host protocol.

## 17. Decision Criteria

The gateway architecture is successful if one selected board/backend can repeatedly demonstrate:

- 8 sensors at 60 Hz.
- Ideally 10 sensors for margin.
- Reliable cold starts.
- Product-relevant RF placement.
- Clean host interface with no BLE dependency on the RPi.

If no single gateway can achieve this, the fallback architecture should be two gateway radios split across sensor groups, for example 4+4.

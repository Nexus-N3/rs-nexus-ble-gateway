# Host SDK And Movella DOT Sample

## Overview

This repository includes a host-side Python SDK and a customer-facing Movella DOT sample client.

The host tooling is designed so a customer can:

- connect a gateway board over USB
- run a supported sample immediately
- understand the gateway lifecycle from the console output
- reuse the same SDK for future supported sensors

The current host code lives under [tools/host](/home/mike/Desktop/apps/dev/rs-nexus-project/rs-nexus-ble-gateway/tools/host).

## Directory Layout

### `tools/host/NexusBLESdk`

This is the shared host SDK layer. It contains:

- serial transport setup
- JSON command send/receive handling
- mixed JSON and binary stream parsing
- gateway request helpers for scan, connect, subscribe, read, write, status, and disconnect
- generic startup-gate logic
- generic stream statistics and summary reporting

This layer is sensor-agnostic. It knows how to talk to the gateway, but it does not know how a specific sensor should be configured or how a sensor payload should be interpreted.

### `tools/host/MovellaDot`

This is the first customer-facing sensor integration. It contains:

- Movella DOT UUIDs and command payloads
- Movella timestamp parsing
- a `MovellaDotClient` built on top of `NexusBLESdk`
- `stream_client.py`, which is the runnable sample CLI for customers

## Design Split

The host code is intentionally split into two layers.

### Shared SDK Responsibilities

`NexusBLESdk` owns:

- serial transport
- gateway protocol handling
- command correlation by `request_id`
- stream frame parsing
- per-sensor stream accounting
- startup-gate evaluation
- generic summary and debug output

### Sensor-Specific Responsibilities

`MovellaDot` owns:

- sensor discovery filter
- sensor-specific GATT UUIDs
- rate configuration payloads
- payload timestamp parsing
- the user-facing CLI arguments for Movella DOT workflows

This lets future sensors reuse the gateway SDK without forcing all sensors into one generic CLI.

## Main Customer Entry Point

The recommended customer entry point today is:

```text
tools/host/MovellaDot/stream_client.py
```

This sample is built for direct operator use. It prints progress through the run, including:

- scanning
- matched devices
- connection progress
- BLE link settling
- configuration progress
- startup-gate state
- official measurement activation
- stream stop
- final stream and gateway summary

## Movella DOT Runtime Flow

For a normal run, the sample performs this sequence:

1. Open the gateway serial port.
2. Reset the gateway session.
3. Send `hello`.
4. Scan for Movella DOT sensors.
5. Select the requested number of addresses.
6. Connect the requested addresses.
7. Wait for BLE links and parameters to settle.
8. Pre-stop any residual stream state on the sensors.
9. Subscribe to the Movella streaming characteristic in binary mode.
10. Configure the sampling rate.
11. Wait briefly after configuration.
12. Start streaming on each connected sensor.
13. Apply a startup stability gate before official measurement, if enabled.
14. Collect frames and compute stream statistics.
15. Stop streams.
16. Request gateway status and BLE RX summaries.
17. Disconnect sensors.
18. Print final results.

## Startup Gate

The startup gate is implemented in the shared SDK because the concept is generic even though the thresholds are sensor-specific.

The gate can enforce:

- minimum packet count
- minimum observation window
- minimum observed rate
- maximum gap events
- grace period before gap detection starts

For Movella DOT, the sample provides sensor-specific defaults for those thresholds.

Once the gate passes, the client prints:

```text
Startup stability gate passed. Official measurement is now active.
```

## Generic Stream Statistics

The SDK summary is designed to be reusable across supported streaming sensors. It currently reports:

- stream frame count
- unknown sensor-id frames
- host parser checksum and resync counters
- gateway transport counters
- per-sensor startup packets
- per-sensor startup-gate packets
- time to first packet
- measurement packet count
- observed rate
- estimated packet drops from timestamp gaps
- gateway-side BLE RX counters

This summary is generic in structure, while the payload parser and sensor defaults remain sensor-specific.

## Running The Movella DOT Sample

From the repository root, a basic single-sensor run is:

```bash
python tools/host/MovellaDot/stream_client.py \
  --sensor-count 1 \
  --stream-seconds 10
```

The full customer-facing usage notes for Movella DOT are in [tools/host/MovellaDot/README.md](/home/mike/Desktop/apps/dev/rs-nexus-project/rs-nexus-ble-gateway/tools/host/MovellaDot/README.md).

## Extending The SDK For Additional Sensors

To add another supported sensor, the intended pattern is:

1. Create a new directory beside `MovellaDot/`.
2. Add sensor-specific UUIDs, payload parsing, and configuration logic there.
3. Reuse `NexusBLESdk` for transport, command handling, startup gating, and summary reporting.
4. Provide a sensor-specific CLI sample for customer use.

This keeps the gateway integration consistent while allowing each sensor to expose the right workflow and arguments.

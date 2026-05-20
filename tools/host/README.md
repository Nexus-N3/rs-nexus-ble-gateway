# Host Tools

This directory contains the customer-facing host software for the Nexus BLE gateway.

The host tools are organized so that supported sensors can ship with ready-to-run sample clients. The intended model is:

- `NexusBLESdk/`: shared gateway transport, command handling, stream monitoring, startup-gate logic, and generic stream statistics
- `MovellaDot/`: the Movella DOT sample client built on top of `NexusBLESdk`

Over time, additional supported sensors or sensor combinations can be added beside `MovellaDot/` using the same structure.

## Directory Layout

- `NexusBLESdk/`
  Reusable Python code for talking to the gateway over the host serial link. This layer handles gateway protocol messages, mixed JSON and binary stream parsing, connection lifecycle operations, and generic monitoring utilities that can be reused across supported sensors.

- `MovellaDot/`
  The first customer-facing sensor sample. This directory contains Movella DOT-specific constants, payload parsing, sensor operations, and a runnable `stream_client.py` example.

## Getting Started

If you want to run a supported sensor sample immediately, start with the Movella DOT sample:

- see [MovellaDot/README.md](/home/mike/Desktop/apps/dev/rs-nexus-project/rs-nexus-ble-gateway/tools/host/MovellaDot/README.md)
- run `python tools/host/MovellaDot/stream_client.py --sensor-count 1 --stream-seconds 10`

## Design Intent

The goal of this layout is to let a customer receive a gateway board and begin working with supported sensors immediately:

- use `NexusBLESdk` for shared gateway behavior
- keep each sensor CLI customer-facing and sensor-specific
- keep reusable monitoring, startup-gate, and stream-health reporting generic where possible

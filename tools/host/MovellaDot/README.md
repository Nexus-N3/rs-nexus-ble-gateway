# Movella DOT

This directory contains the customer-facing Movella DOT host sample for the Nexus BLE gateway.

`stream_client.py` is the recommended starting point for customers who want to connect a supported gateway board, discover Movella DOT sensors, start streaming, and verify that data is flowing correctly.

## What This Sample Does

The sample client:

- opens the gateway serial interface
- resets the gateway host session
- scans for Movella DOT sensors
- connects the requested number of sensors
- configures the sampling rate
- subscribes to the Movella streaming characteristic
- starts streaming
- applies a startup stability gate before measurement, if enabled
- prints a stream and gateway summary at the end

During a run, the client prints operator-facing progress messages for the main phases, including scanning, connecting, link settling, configuration, stream start, startup-gate status, official measurement activation, stream stop, and final summary output.

## Requirements

- a programmed Nexus BLE gateway board connected over USB
- at least one powered Movella DOT sensor advertising nearby
- Python 3
- the Python `pyserial` package available in your environment

## Single-Sensor Test

Run the customer-facing sample with one sensor:

```bash
python tools/host/MovellaDot/stream_client.py \
  --sensor-count 1 \
  --stream-seconds 10
```

To explicitly keep the startup gate enabled:

```bash
python tools/host/MovellaDot/stream_client.py \
  --sensor-count 1 \
  --stream-seconds 10 \
  --use-startup-gate
```

To test at 20 Hz instead of the default 60 Hz:

```bash
python tools/host/MovellaDot/stream_client.py \
  --sensor-count 1 \
  --sampling-rate-hz 20 \
  --stream-seconds 10
```

If your gateway appears at a different serial path:

```bash
python tools/host/MovellaDot/stream_client.py \
  --port /dev/serial/by-id/your-gateway-device \
  --sensor-count 1
```

## Expected Output

For a healthy single-sensor run, you should see:

- `Scanning for up to ...`
- `SCAN MATCH: ...` and `SCAN COMPLETE: ...`
- `CONNECTED: ...`
- `All sensors connected. Waiting ... for BLE links/params to settle.`
- `CONFIG ...: pre-stop`, `subscribe`, and `set-rate ...`
- `All sensors configured. Waiting ... before stream start.`
- `Starting stream. Total stream budget: ...`
- `Waiting for startup stability gate: ...` when the startup gate is enabled
- `Startup stability gate passed. Official measurement is now active.` once the stream is stable
- a `Selected addresses: [...]` line
- no disconnect or timeout failure during configure or streaming
- `STOP STREAM ...` lines during shutdown
- summary output with non-zero startup and measurement packet counts
- `observed_rate_hz` close to the configured sample rate

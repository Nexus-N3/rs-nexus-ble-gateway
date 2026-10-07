# UART TX pacing A/B test

Physical UART: **1,000,000 baud, 8N1**. Configure the host serial port to
1,000,000 too: IFMCU applies CDC line coding to its physical UART.

## Evidence and likely failure

Gateway baseline: `82e26dc`. IFMCU reviewed directly with `git show` at
`0ccc004c89e73a3935cf1e8ed2350bf128188e3f` in the local Makerdiary checkout:

- `applications/ifmcu_firmware/src/modules/uart_handler.c`
- `applications/ifmcu_firmware/src/modules/usb_cdc_handler.c`
- `applications/ifmcu_firmware/src/modules/Kconfig`
- `applications/ifmcu_firmware/prj.conf`
- `boards/makerdiary/nrf54l15_connectkit/nrf54l15_connectkit_nrf52820.dts`

UART RX uses three 512-byte slab blocks. RX events retain references until the
app event subscribers finish. Continuous 1 Mbps input fills a block in 5.12 ms,
versus 11.11 ms at 460800. A delayed event worker can hold all three blocks.
`UART_RX_BUF_REQUEST` then fails allocation and supplies no next buffer.
`UART_RX_DISABLED` only re-enables RX for the separate `enable_rx_retry` flag;
allocation failure does not set that flag. Otherwise it powers the UART down
when PM is enabled. This is a plausible permanent RX stop while the separate
IFMCU shell and application MCU remain alive. Hardware traces are still needed
to prove which event occurred in the reported failures.

Separately, the CDC ring is 1024 bytes. `uart_fifo_fill()` is called once per RX
event; partial writes are only logged, with no retry of the remainder. Logging
is disabled in the supplied IFMCU configuration. This directly permits missing
bytes and parser corruption even when the gateway reports no drops.

Gateway TX previously chained DONE/ABORTED callbacks directly to the next TX.
With stream backlog, complete-frame batches (up to 258 bytes) could run at line
rate indefinitely. Individual full-size frames occupied 2.58 ms each with no
intentional idle interval. The nRF54's counters cannot detect receiver-side loss.

## Patch and tuning

`GATEWAY_STREAM_TX_GAP_MS` in `src/config/gateway_config.h` defaults to **2**.
STREAM waits until this deadline after any TX completion or abort; CONTROL
bypasses the deadline and retains priority. Control completion refreshes the
stream deadline. Existing approximately 1 ms main-loop polling resumes STREAM.
There are no sleeps or additional timers. Millisecond uptime quantization means
the actual minimum gap approaches 1 ms; polling can extend it. This gives the
IFMCU's 1000 us idle timeout and event/USB workers a recovery opportunity.

The existing maximum 258-byte batch and complete-frame packing remain intact.
Small frames can share a batch. An idle transmitter starts the first batch
immediately. Pacing never dequeues bytes while waiting, never accumulates credit
for a later catch-up burst, and never changes payloads. STREAM dequeue commits
only after `uart_tx()` accepts the batch; failed starts keep bytes for retry.
The short nonblocking driver start is IRQ-protected through queue accounting.

At full batches, nominal capacity is 258 / (2.58 + 2) ms = **56.3 kB/s**;
allowing another 1 ms polling delay gives **46.2 kB/s**. Four Movella DOTs at
60 Hz with the local parser's 44-byte IMU prefix need **13.92 kB/s** including
14-byte gateway framing. This is over 3x headroom in the conservative full-batch
estimate; the host test also models synchronized four-sensor arrivals. Actual
payload sizes, polling load, control volume and sensor rates must be checked
in the hardware run. Four maximum-size 244-byte notifications at 60 Hz need
61.92 kB/s and exceed nominal default capacity. This is not an unconditional
capacity guarantee for every possible sensor configuration.

Set the macro to **0** for the unpaced 1 Mbps baseline, or try **3** for a longer
recovery interval. Rebuild after changing it. Keep the physical baud unchanged.

## Diagnostics

Request existing `get_status` to receive `gateway_transport_stats` (periodic
stats remain disabled). New fields:

- `stream_tx_gap_ms`: active configured nominal gap.
- `stream_pacing_delays`: number of attempted starts deferred by the gate;
  this counts attempts, not elapsed milliseconds or unique frames.
- `stream_high_water_bytes`: maximum queued stream bytes since init/reset.
- `stream_pressure_events`: upward crossings of 12288 bytes (75% of the ring).
- `stream_backlog_age_ms`: how long the queued stream ring has remained nonempty.
- `stream_backlog_max_ms`: maximum observed nonempty duration.

Growing occupancy/backlog, pressure crossings, or existing stream enqueue/ring
drop counters indicate inadequate drain capacity. Overflow rejects the entire
new frame with `-12`; existing scheduler enqueue-drop diagnostics also record it.
Finite buffers cannot guarantee losslessness under arbitrary overload. Accepted
TX aborts remain visible through existing abort counters/failure reports; pacing
does not add retransmission of partially transmitted frames. Stats storage grows
to 1280 bytes so the extended JSON does not truncate at large counter values.

## Validate and flash (operator only)

```bash
cd ~/Desktop/apps/dev/rs-nexus-project/rs-nexus-ble-gateway
git status --short
git diff --check
git diff 82e26dc -- src/config/gateway_config.h src/interface/gateway_interface.c tests/uart_tx_state/test_gateway_interface_uart_tx.c ports/zephyr/app.overlay
cat UART_TX_PACING.md
bash tests/uart_tx_state/run.sh
source ./env.sh
CCACHE_DIR=/tmp/nexus-gateway-ccache CCACHE_TEMPDIR=/tmp/nexus-gateway-ccache-tmp west build -p always -b nrf54l15_connectkit/nrf54l15/cpuapp -s "$GATEWAY/ports/zephyr" -d "$GATEWAY/build" -- -DBOARD_ROOT="$HOME/Desktop/nrf54l15-connectkit" -DUSER_CACHE_DIR=/tmp/nexus-gateway-zephyr-cache -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
# Run only when ready to flash the application MCU:
west flash --build-dir "$GATEWAY/build"
```

Run identical four-DOT sessions for gap 0 and gap 2, longer than the previously
observed failure window. Record host checksum/resync/lost-frame counters and
status snapshots, plus nRF54 heartbeat and IFMCU bridge LED/shell behavior.
The bridge has no RTS/CTS, so a sufficiently long USB/worker stall or a large
unpaced control burst can still overflow it. This is gateway-side mitigation,
not a repair of IFMCU's recovery or partial-write handling.

#!/usr/bin/env python3

import argparse
import json
import struct
import time
from dataclasses import dataclass, replace
from types import SimpleNamespace

from gateway_discover_connect import (
    DEFAULT_PORT,
    MOVELLA_DEVICE_CONTROL_UUID,
    MOVELLA_START_STOP_STREAM_UUID,
    command_hello,
    identify_address,
    json_objects_from_line,
    open_gateway_serial,
    select_discovered_addresses,
)


DEFAULT_LOCATIONS = [
    "LEFT_ANKLE",
    "RIGHT_ANKLE",
    "LEFT_THIGH",
    "RIGHT_THIGH",
    "CHEST",
    "LOWER_BACK",
    "HEAD",
    "UPPER_BACK",
]

MOVELLA_LONG_PAYLOAD_UUID = "15172002-4947-11e9-8646-d663bd873d93"
MOVELLA_SET_RATE_HEX = {
    20: "100000000000000B4D6F76656C6C6120444F5400000000001400000000000000",
    60: "100000000000000B4D6F76656C6C6120444F5400000000003C00000000000000",
}
MOVELLA_START_HEX = "01011A"
MOVELLA_STOP_HEX = "01001A"
MOVELLA_MIN_PACKET_LEN = 4
STREAM_FRAME_MAGIC = b"\xA5\x5A"


class GatewayTransportReader:
    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def send_jsonl(self, obj):
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        self.ser.write(line.encode("utf-8"))
        self.ser.flush()

    def read_item(self, timeout_s=10):
        deadline = time.time() + timeout_s

        while time.time() < deadline:
            item = self._extract_item()
            if item is not None:
                return item

            chunk = self.ser.read(256)
            if chunk:
                self.buf.extend(chunk)

        raise TimeoutError("Timed out waiting for gateway item")

    def _extract_item(self):
        while self.buf:
            if self.buf[0] == ord("{"):
                newline_index = self.buf.find(b"\n")
                if newline_index < 0:
                    return None
                line = self.buf[:newline_index].decode("utf-8", errors="replace").strip()
                del self.buf[: newline_index + 1]
                if not line:
                    continue
                for msg in json_objects_from_line(line):
                    return ("json", msg)
                continue

            if len(self.buf) >= 2 and self.buf[:2] == STREAM_FRAME_MAGIC:
                if len(self.buf) < 14:
                    return None
                version = self.buf[2]
                sensor_id = self.buf[3]
                gateway_timestamp_us = int.from_bytes(self.buf[4:12], "little")
                payload_len = self.buf[12]
                total_len = 13 + payload_len + 1
                if len(self.buf) < total_len:
                    return None
                payload = bytes(self.buf[13 : 13 + payload_len])
                checksum = self.buf[13 + payload_len]
                computed = sum(self.buf[2 : 13 + payload_len]) & 0xFF
                del self.buf[:total_len]
                if checksum != computed:
                    continue
                return (
                    "stream_frame",
                    {
                        "version": version,
                        "sensor_id": sensor_id,
                        "gateway_timestamp_us": gateway_timestamp_us,
                        "payload": payload,
                    },
                )

            next_json = self.buf.find(b"{")
            next_bin = self.buf.find(STREAM_FRAME_MAGIC)
            candidates = [idx for idx in (next_json, next_bin) if idx >= 0]
            if not candidates:
                self.buf.clear()
                return None
            del self.buf[: min(candidates)]

        return None


def read_json_any_quiet(reader, timeout_s=10):
    deadline = time.time() + timeout_s

    while time.time() < deadline:
        item_type, item = reader.read_item(timeout_s=max(0.1, deadline - time.time()))
        if item_type == "json":
            return item

    raise TimeoutError("Timed out waiting for JSON")


def send_jsonl_quiet(reader, obj):
    reader.send_jsonl(obj)


def gatt_subscribe_address_quiet(reader, address, characteristic_uuid, timeout_s=10):
    request_id = f"subscribe_{int(time.time() * 1000)}"
    send_jsonl_quiet(
        reader,
        {
            "type": "subscribe",
            "request_id": request_id,
            "address": address,
            "characteristic_uuid": characteristic_uuid,
            "binary_notifications": True,
        },
    )

    deadline = time.time() + timeout_s
    while time.time() < deadline:
        msg = read_json_any_quiet(reader, timeout_s=max(0.1, deadline - time.time()))
        msg_type = msg.get("type")

        if msg_type == "subscribe_complete" and msg.get("request_id") == request_id:
            print(f"SUBSCRIBE COMPLETE: {address} uuid={characteristic_uuid}")
            return True

        if msg_type == "error" and msg.get("request_id") == request_id:
            raise RuntimeError(
                f"Gateway subscribe failed: {msg.get('message')} "
                f"({msg.get('error_code')})"
            )

    raise TimeoutError(f"Timed out waiting for subscribe on {address}")


def gatt_write_address_quiet(
    reader,
    address,
    characteristic_uuid,
    payload_hex,
    without_response=False,
    timeout_s=10,
):
    request_id = f"write_{int(time.time() * 1000)}"
    send_jsonl_quiet(
        reader,
        {
            "type": "gatt_write",
            "request_id": request_id,
            "address": address,
            "characteristic_uuid": characteristic_uuid,
            "payload_hex": payload_hex,
            "without_response": without_response,
        },
    )

    deadline = time.time() + timeout_s
    while time.time() < deadline:
        msg = read_json_any_quiet(reader, timeout_s=max(0.1, deadline - time.time()))
        msg_type = msg.get("type")

        if msg_type == "write_complete" and msg.get("request_id") == request_id:
            return True

        if msg_type == "error" and msg.get("request_id") == request_id:
            raise RuntimeError(
                f"Gateway gatt_write failed: {msg.get('message')} "
                f"({msg.get('error_code')})"
            )

    raise TimeoutError(f"Timed out waiting for gatt_write on {address}")


def connect_addresses_quiet(
    reader,
    addresses,
    attempt_timeout_s,
    retry_attempts=0,
    retry_delay_s=20.0,
):
    remaining = list(addresses)
    connected = []
    sensor_id_by_address = {}

    for attempt in range(retry_attempts + 1):
        if not remaining:
            break

        request_id = f"connect_{int(time.time() * 1000)}_{attempt}"
        pending = list(remaining)
        failed_this_attempt = []

        send_jsonl_quiet(
            reader,
            {
                "type": "connect_addresses",
                "request_id": request_id,
                "addresses": pending,
            },
        )

        deadline = time.time() + attempt_timeout_s
        while time.time() < deadline and pending:
            try:
                msg = read_json_any_quiet(
                    reader,
                    timeout_s=max(0.1, deadline - time.time()),
                )
            except TimeoutError:
                break

            msg_type = msg.get("type")

            if msg_type == "sensor_connected" and msg.get("request_id") == request_id:
                address = msg.get("address")
                if address in pending:
                    pending.remove(address)
                    if address not in connected:
                        connected.append(address)
                    if isinstance(msg.get("sensor_id"), int):
                        sensor_id_by_address[address] = msg["sensor_id"]
                    print(f"CONNECTED: {address}")
                continue

            if msg_type == "sensor_disconnected" and msg.get("request_id") == request_id:
                address = msg.get("address")
                if address in pending:
                    pending.remove(address)
                    failed_this_attempt.append(address)
                    print(f"CONNECT FAILED: {address} reason={msg.get('reason')}")
                continue

            if msg_type == "error" and msg.get("request_id") == request_id:
                code = msg.get("error_code")
                message = msg.get("message", "unknown_error")

                if message == "sensor_not_found" or code == -3:
                    raise RuntimeError(
                        "Gateway could not connect because one or more requested sensors "
                        "were not found in the gateway's current discovery cache."
                    )

                raise RuntimeError(f"Gateway connect failed: {message} ({code})")

        remaining = failed_this_attempt + pending

        if remaining and attempt < retry_attempts:
            print(f"Retrying connect in {retry_delay_s:.1f}s for addresses: {remaining}")
            time.sleep(retry_delay_s)

    if remaining:
        raise TimeoutError(
            f"Failed to connect after {retry_attempts + 1} attempt(s): "
            f"{', '.join(remaining)}"
        )

    return connected, sensor_id_by_address


def disconnect_addresses_quiet(reader, addresses, timeout_s=10):
    request_id = f"disconnect_{int(time.time() * 1000)}"
    pending = list(addresses)
    disconnected = []

    send_jsonl_quiet(
        reader,
        {
            "type": "disconnect_addresses",
            "request_id": request_id,
            "addresses": pending,
        },
    )

    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            msg = read_json_any_quiet(reader, timeout_s=max(0.5, deadline - time.time()))
        except TimeoutError as exc:
            raise TimeoutError(
                f"Timed out waiting for disconnect confirmation for: "
                f"{', '.join(pending)}"
            ) from exc

        msg_type = msg.get("type")

        if msg_type == "sensor_disconnected" and msg.get("request_id") == request_id:
            address = msg.get("address")
            if address in pending:
                pending.remove(address)
                disconnected.append(address)
                print(f"DISCONNECTED: {address}")
            if not pending:
                return disconnected

        if msg_type == "error" and msg.get("request_id") == request_id:
            raise RuntimeError(
                f"Gateway disconnect failed: {msg.get('message')} "
                f"({msg.get('error_code')})"
            )

    raise TimeoutError(f"Timed out waiting for disconnect of: {', '.join(pending)}")


def discover_movella_quiet(reader, timeout_ms):
    request_id = f"scan_{int(time.time() * 1000)}"
    matches = {}

    send_jsonl_quiet(
        reader,
        {
            "type": "scan_start",
            "request_id": request_id,
            "timeout_ms": timeout_ms,
        },
    )

    while True:
        msg = read_json_any_quiet(reader, timeout_s=max(10, timeout_ms / 1000 + 5))
        msg_type = msg.get("type")

        if msg_type == "scan_result" and msg.get("request_id") == request_id:
            if msg.get("name") != "Movella DOT":
                continue
            address = msg.get("address")
            if address and address not in matches:
                matches[address] = {
                    "address": address,
                    "name": msg.get("name", ""),
                    "rssi": msg.get("rssi"),
                }
            continue

        if msg_type == "scan_complete" and msg.get("request_id") == request_id:
            return list(matches.values())


@dataclass
class SensorStats:
    address: str
    location: str | None
    expected_rate_hz: int | None
    connect_time: float | None = None
    stream_start_command_time: float | None = None
    first_packet_time: float | None = None
    startup_first_sensor_timestamp: int | None = None
    startup_last_sensor_timestamp: int | None = None
    startup_first_wall_time: float | None = None
    startup_last_wall_time: float | None = None
    startup_packets_received: int = 0
    startup_gap_events: int = 0
    startup_estimated_dropped_packets: int = 0
    measurement_first_sensor_timestamp: int | None = None
    measurement_last_sensor_timestamp: int | None = None
    measurement_first_wall_time: float | None = None
    measurement_last_wall_time: float | None = None
    measurement_packets_received: int = 0
    gap_events: int = 0
    estimated_dropped_packets: int = 0
    gap_detection_start_wall_time: float | None = None

    def record_sample(self, timestamp: int | None, wall_time: float, measurement_active: bool):
        if self.first_packet_time is None:
            self.first_packet_time = wall_time
            self.gap_detection_start_wall_time = wall_time + 1.0

        if measurement_active:
            self._record_measurement_sample(timestamp, wall_time)
        else:
            self._record_startup_sample(timestamp, wall_time)

    def _record_startup_sample(self, timestamp: int | None, wall_time: float):
        if self.startup_first_sensor_timestamp is None:
            self.startup_first_sensor_timestamp = timestamp
            self.startup_first_wall_time = wall_time
        else:
            if (
                self.gap_detection_start_wall_time is not None
                and wall_time >= self.gap_detection_start_wall_time
            ):
                self._record_startup_gap_if_needed(timestamp)

                self.startup_last_sensor_timestamp = timestamp
                self.startup_last_wall_time = wall_time
                self.startup_packets_received += 1

    def _record_measurement_sample(self, timestamp: int | None, wall_time: float):
        if self.measurement_first_sensor_timestamp is None:
            self.measurement_first_sensor_timestamp = timestamp
            self.measurement_first_wall_time = wall_time
        else:
            self._record_measurement_gap_if_needed(timestamp)

        self.measurement_last_sensor_timestamp = timestamp
        self.measurement_last_wall_time = wall_time
        self.measurement_packets_received += 1

    def _record_startup_gap_if_needed(self, timestamp: int | None):
        if timestamp is None or self.startup_last_sensor_timestamp is None:
            return
        expected_delta_us = self.expected_delta_us
        if expected_delta_us is None:
            return
        observed_delta_us = timestamp - self.startup_last_sensor_timestamp
        if observed_delta_us <= int(expected_delta_us * 1.5):
            return
        missing_packets = max(int(round(observed_delta_us / expected_delta_us)) - 1, 0)
        if missing_packets <= 0:
            return
        self.startup_gap_events += 1
        self.startup_estimated_dropped_packets += missing_packets

    def _record_measurement_gap_if_needed(self, timestamp: int | None):
        if timestamp is None or self.measurement_last_sensor_timestamp is None:
            return
        expected_delta_us = self.expected_delta_us
        if expected_delta_us is None:
            return
        observed_delta_us = timestamp - self.measurement_last_sensor_timestamp
        if observed_delta_us <= int(expected_delta_us * 1.5):
            return
        missing_packets = max(int(round(observed_delta_us / expected_delta_us)) - 1, 0)
        if missing_packets <= 0:
            return
        self.gap_events += 1
        self.estimated_dropped_packets += missing_packets

    def reset_measurement(self):
        self.measurement_first_sensor_timestamp = None
        self.measurement_last_sensor_timestamp = None
        self.measurement_first_wall_time = None
        self.measurement_last_wall_time = None
        self.measurement_packets_received = 0
        self.gap_events = 0
        self.estimated_dropped_packets = 0

    @property
    def expected_delta_us(self) -> float | None:
        if not self.expected_rate_hz:
            return None
        return 1_000_000.0 / float(self.expected_rate_hz)

    @property
    def startup_duration_seconds(self) -> float:
        if self.startup_first_wall_time is None or self.startup_last_wall_time is None:
            return 0.0
        return max(self.startup_last_wall_time - self.startup_first_wall_time, 0.0)

    @property
    def startup_observed_rate_hz(self) -> float:
        duration = self.startup_duration_seconds
        if duration <= 0:
            return 0.0
        return self.startup_packets_received / duration

    @property
    def measurement_duration_seconds(self) -> float:
        if self.measurement_first_wall_time is None or self.measurement_last_wall_time is None:
            return 0.0
        return max(self.measurement_last_wall_time - self.measurement_first_wall_time, 0.0)

    @property
    def observed_rate_hz(self) -> float:
        duration = self.measurement_duration_seconds
        if duration <= 0:
            return 0.0
        return self.measurement_packets_received / duration

    @property
    def time_to_first_packet_ms(self) -> float | None:
        if self.stream_start_command_time is None or self.first_packet_time is None:
            return None
        return max((self.first_packet_time - self.stream_start_command_time) * 1000.0, 0.0)


class GatewayStartupRetryMonitorTest:
    def __init__(
        self,
        *,
        port: str,
        sensor_count: int,
        stream_seconds: int,
        timeout_seconds: int,
        scan_timeout_ms: int,
        connect_attempt_timeout_s: float,
        connect_retry_attempts: int,
        connect_retry_delay_s: float,
        subscribe_timeout_s: float,
        write_timeout_s: float,
        disconnect_timeout_s: float,
        post_connect_settle_seconds: float,
        sampling_rate_hz: int,
        use_startup_gate: bool,
        startup_stability_window_seconds: float,
        startup_packets_required: int,
        startup_min_rate_hz: float,
        startup_min_observation_seconds: float,
        retry_delay_seconds: float,
        max_start_attempts: int,
        identify_retry_attempts: int,
        identify_retry_delay_s: float,
        pre_identify_delay_s: float,
        without_response: bool,
    ):
        self.port = port
        self.sensor_count = sensor_count
        self.stream_seconds = stream_seconds
        self.timeout_seconds = timeout_seconds
        self.scan_timeout_ms = scan_timeout_ms
        self.connect_attempt_timeout_s = connect_attempt_timeout_s
        self.connect_retry_attempts = connect_retry_attempts
        self.connect_retry_delay_s = connect_retry_delay_s
        self.subscribe_timeout_s = subscribe_timeout_s
        self.write_timeout_s = write_timeout_s
        self.disconnect_timeout_s = disconnect_timeout_s
        self.post_connect_settle_seconds = post_connect_settle_seconds
        self.sampling_rate_hz = sampling_rate_hz
        self.use_startup_gate = use_startup_gate
        self.startup_stability_window_seconds = startup_stability_window_seconds
        self.startup_packets_required = startup_packets_required
        self.startup_min_rate_hz = startup_min_rate_hz
        self.startup_min_observation_seconds = startup_min_observation_seconds
        self.retry_delay_seconds = retry_delay_seconds
        self.max_start_attempts = max_start_attempts
        self.identify_retry_attempts = identify_retry_attempts
        self.identify_retry_delay_s = identify_retry_delay_s
        self.pre_identify_delay_s = pre_identify_delay_s
        self.without_response = without_response

        self.attempt_number = 0
        self.attempt_outcome = ""
        self.attempt_reason = ""
        self.connected_addresses: set[str] = set()
        self.disconnected_addresses: set[str] = set()
        self.stats_by_address: dict[str, SensorStats] = {}
        self.attempt_summaries: list[dict] = []
        self.measurement_active = False
        self.measurement_started_at: float | None = None
        self.stream_started_at: float | None = None
        self.sensor_id_by_address: dict[str, int] = {}
        self.address_by_sensor_id: dict[int, str] = {}

    def run(self) -> int:
        final_success = False

        for attempt in range(1, self.max_start_attempts + 1):
            self._reset_attempt_state(attempt)
            print("")
            print(
                f"Attempt {attempt}/{self.max_start_attempts}: "
                f"starting gateway startup-retry monitor with {self.sensor_count} sensor(s)."
            )

            try:
                self._run_attempt()
            except (RuntimeError, TimeoutError) as exc:
                if not self.attempt_outcome:
                    self.attempt_outcome = "retry"
                    self.attempt_reason = str(exc)
                print(f"FAILED: {exc}")
            except Exception as exc:
                if not self.attempt_outcome:
                    self.attempt_outcome = "retry"
                    self.attempt_reason = f"{type(exc).__name__}: {exc}"
                print(f"FAILED: {type(exc).__name__}: {exc}")

            self._print_attempt_summary()
            self.attempt_summaries.append(
                {
                    "attempt": attempt,
                    "outcome": self.attempt_outcome,
                    "reason": self.attempt_reason,
                    "stats": {
                        address: replace(stats) for address, stats in self.stats_by_address.items()
                    },
                }
            )

            if self.attempt_outcome == "success":
                final_success = True
                break

            if attempt < self.max_start_attempts:
                print(
                    f"Retrying startup after {self.retry_delay_seconds:.1f}s "
                    f"(attempt {attempt + 1}/{self.max_start_attempts})."
                )
                time.sleep(self.retry_delay_seconds)

        self._print_overall_summary()
        return 0 if final_success else 1

    def _run_attempt(self):
        with open_gateway_serial(self.port) as ser:
            reader = GatewayTransportReader(ser)
            send_jsonl_quiet(
                reader,
                {
                    "type": "hello",
                    "request_id": "hello_host_tool",
                    "protocol_version": 1,
                    "client": "gateway_startup_retry_monitor",
                },
            )
            hello = read_json_any_quiet(reader, timeout_s=5)
            if hello.get("type") != "hello_ack":
                raise RuntimeError(f"Expected hello_ack, got: {hello}")
            connected: list[str] = []
            streams_started = False
            cleanup_completed = False

            try:
                print(f"Scanning for up to {self.scan_timeout_ms}ms...")
                matches = discover_movella_quiet(reader, timeout_ms=self.scan_timeout_ms)
                selected = select_discovered_addresses(matches, self.sensor_count)
                address_locations = {
                    address: DEFAULT_LOCATIONS[index] if index < len(DEFAULT_LOCATIONS) else None
                    for index, address in enumerate(selected)
                }

                print(f"Selected addresses: {selected}")
                connected, sensor_id_by_address = connect_addresses_quiet(
                    reader,
                    selected,
                    attempt_timeout_s=self.connect_attempt_timeout_s,
                    retry_attempts=self.connect_retry_attempts,
                    retry_delay_s=self.connect_retry_delay_s,
                )
                self.connected_addresses = set(connected)
                self.sensor_id_by_address = dict(sensor_id_by_address)
                self.address_by_sensor_id = {
                    sensor_id: address for address, sensor_id in sensor_id_by_address.items()
                }

                connected_time = time.monotonic()
                for address in connected:
                    stats = self._get_or_create_stats(address, address_locations.get(address))
                    if stats.connect_time is None:
                        stats.connect_time = connected_time

                if len(connected) < self.sensor_count:
                    raise RuntimeError(
                        f"Only {len(connected)} of {self.sensor_count} sensors connected."
                    )

                if self.post_connect_settle_seconds > 0:
                    print(
                        "All sensors connected. "
                        f"Waiting {self.post_connect_settle_seconds:.1f}s for BLE links to settle."
                    )
                    time.sleep(self.post_connect_settle_seconds)

                self._configure_sensors(reader, connected)
                self._start_streams(reader, connected)
                streams_started = True

                if self.use_startup_gate:
                    print(
                        "Waiting for startup stability gate:",
                        f"up to {self.startup_stability_window_seconds:.1f}s.",
                    )
                else:
                    self.attempt_outcome = "success"
                    self.measurement_started_at = time.monotonic()
                    self.measurement_active = True
                    print(
                        "Startup gate disabled. "
                        f"Starting official measurement window for {self.stream_seconds}s."
                    )

                self._monitor_stream(reader)
                self._identify_drops_then_disconnect(reader, connected)
                cleanup_completed = True

                if not self.attempt_outcome:
                    self.attempt_outcome = "success"
            finally:
                if connected and not cleanup_completed:
                    self._best_effort_cleanup(reader, connected, streams_started)

    def _configure_sensors(self, reader, addresses: list[str]):
        for address in addresses:
            # Force a known idle state in case a previous run left the sensor streaming.
            try:
                gatt_write_address_quiet(
                    reader,
                    address,
                    MOVELLA_START_STOP_STREAM_UUID,
                    MOVELLA_STOP_HEX,
                    without_response=self.without_response,
                    timeout_s=self.write_timeout_s,
                )
                time.sleep(0.25)
            except (RuntimeError, TimeoutError) as exc:
                print(f"PRE-STOP WARNING: {address}: {exc}")

            gatt_subscribe_address_quiet(
                reader,
                address,
                MOVELLA_LONG_PAYLOAD_UUID,
                timeout_s=self.subscribe_timeout_s,
            )
            gatt_write_address_quiet(
                reader,
                address,
                MOVELLA_DEVICE_CONTROL_UUID,
                MOVELLA_SET_RATE_HEX[self.sampling_rate_hz],
                without_response=self.without_response,
                timeout_s=self.write_timeout_s,
            )

    def _start_streams(self, reader, addresses: list[str]):
        start_command_time = time.monotonic()
        self.stream_started_at = start_command_time
        for address in addresses:
            self.stats_by_address[address].stream_start_command_time = start_command_time

        print(f"Starting stream. Total stream budget: {self.stream_seconds}s.")
        for address in addresses:
            gatt_write_address_quiet(
                reader,
                address,
                MOVELLA_START_STOP_STREAM_UUID,
                MOVELLA_START_HEX,
                without_response=self.without_response,
                timeout_s=self.write_timeout_s,
            )

    def _stop_streams(self, reader, addresses: list[str]):
        print("Stopping stream now.")
        for address in addresses:
            try:
                gatt_write_address_quiet(
                    reader,
                    address,
                    MOVELLA_START_STOP_STREAM_UUID,
                    MOVELLA_STOP_HEX,
                    without_response=self.without_response,
                    timeout_s=self.write_timeout_s,
                )
            except (RuntimeError, TimeoutError) as exc:
                print(f"STOP STREAM FAILED: {address}: {exc}")

    def _monitor_stream(self, reader):
        attempt_deadline = time.monotonic() + self.timeout_seconds
        startup_deadline = time.monotonic() + self.startup_stability_window_seconds
        stream_deadline = (
            self.stream_started_at + self.stream_seconds
            if self.stream_started_at is not None
            else None
        )

        while time.monotonic() < attempt_deadline:
            now = time.monotonic()

            if stream_deadline is not None and now >= stream_deadline:
                self._stop_streams(reader, sorted(self.connected_addresses))
                if self.use_startup_gate and not self.measurement_active:
                    self.attempt_outcome = "retry"
                    self.attempt_reason = (
                        f"Startup stability gate did not pass within total stream budget "
                        f"of {self.stream_seconds}s."
                    )
                    raise RuntimeError(self.attempt_reason)
                return

            if self.use_startup_gate and not self.measurement_active and now >= startup_deadline:
                _stable, unstable = self._evaluate_current_stability(sorted(self.connected_addresses))
                details = ", ".join(unstable) if unstable else "unknown startup instability"
                self.attempt_outcome = "retry"
                self.attempt_reason = f"Startup stability gate failed: {details}"
                raise RuntimeError(self.attempt_reason)

            try:
                item_type, item = reader.read_item(timeout_s=0.2)
            except TimeoutError:
                continue

            if item_type == "stream_frame":
                address = self.address_by_sensor_id.get(item.get("sensor_id"))
                if address not in self.connected_addresses:
                    continue
                timestamp = self._parse_movella_timestamp_bytes(item.get("payload", b""))
                wall_time = time.monotonic()
                self._get_or_create_stats(address, None).record_sample(
                    timestamp,
                    wall_time,
                    measurement_active=self.measurement_active,
                )

                if self.use_startup_gate and not self.measurement_active:
                    stable, _unstable = self._evaluate_current_stability(
                        sorted(self.connected_addresses)
                    )
                    if stable:
                        for sensor_address in sorted(self.connected_addresses):
                            self.stats_by_address[sensor_address].reset_measurement()
                        self.measurement_active = True
                        self.measurement_started_at = time.monotonic()
                        self.attempt_outcome = "success"
                        print(
                            "Startup stability gate passed. "
                            "Official measurement is now active until the total stream budget ends."
                        )
                continue

            msg = item
            msg_type = msg.get("type")

            if msg_type == "sensor_disconnected":
                address = msg.get("address")
                if address:
                    self.disconnected_addresses.add(address)
                self.attempt_outcome = "retry"
                self.attempt_reason = (
                    f"Unexpected disconnect during stream: {address} reason={msg.get('reason')}"
                )
                raise RuntimeError(self.attempt_reason)

            if msg_type == "error":
                print("Gateway error while streaming:")
                print(msg)
                continue

        if not self.measurement_active:
            self.attempt_outcome = "retry"
            self.attempt_reason = (
                f"Attempt {self.attempt_number} timed out after {self.timeout_seconds}s."
            )
            raise TimeoutError(self.attempt_reason)

        self.attempt_outcome = "retry"
        self.attempt_reason = (
            f"Measurement timed out after {self.timeout_seconds}s."
        )
        raise TimeoutError(self.attempt_reason)

    def _identify_drops_then_disconnect(self, reader, connected: list[str]):
        identify_args = SimpleNamespace(
            pre_identify_delay_s=self.pre_identify_delay_s,
            identify_retry_attempts=self.identify_retry_attempts,
            identify_retry_delay_s=self.identify_retry_delay_s,
            read_timeout_s=self.write_timeout_s,
            write_timeout_s=self.write_timeout_s,
            without_response=self.without_response,
        )
        addresses_to_identify = [
            address
            for address, stats in sorted(self.stats_by_address.items())
            if stats.estimated_dropped_packets > 0
        ]
        if addresses_to_identify:
            print(
                "Identifying sensors with packet drops before disconnect:",
                addresses_to_identify,
            )
            for address in addresses_to_identify:
                print(f"IDENTIFY: {address}")
                try:
                        identify_address(reader.ser, address, identify_args)
                except (RuntimeError, TimeoutError) as exc:
                    print(f"IDENTIFY FAILED: {address}: {exc}")

        disconnected = disconnect_addresses_quiet(
            reader,
            connected,
            timeout_s=self.disconnect_timeout_s,
        )
        self.disconnected_addresses.update(disconnected)

    def _evaluate_current_stability(self, addresses: list[str]):
        unstable = []
        for address in addresses:
            stats = self.stats_by_address[address]
            if stats.first_packet_time is None:
                unstable.append(f"{address}: no_first_packet")
                continue
            if stats.startup_packets_received < self.startup_packets_required:
                unstable.append(f"{address}: packets={stats.startup_packets_received}")
                continue
            if stats.startup_duration_seconds < self.startup_min_observation_seconds:
                unstable.append(
                    f"{address}: warmup_window={stats.startup_duration_seconds:.2f}s"
                )
                continue
            if stats.startup_observed_rate_hz < self.startup_min_rate_hz:
                unstable.append(f"{address}: rate={stats.startup_observed_rate_hz:.2f}Hz")
                continue
            if stats.startup_gap_events > 0:
                unstable.append(
                    f"{address}: startup_gap_events={stats.startup_gap_events} "
                    f"startup_drops={stats.startup_estimated_dropped_packets}"
                )
                continue
        return len(unstable) == 0, unstable

    def _parse_movella_timestamp_bytes(self, data: bytes) -> int:
        if len(data) < MOVELLA_MIN_PACKET_LEN:
            raise ValueError(f"Movella payload too short: {len(data)} bytes")
        return int(struct.unpack_from("<I", data, 0)[0])

    def _get_or_create_stats(self, address: str, location: str | None):
        stats = self.stats_by_address.get(address)
        if stats is None:
            stats = SensorStats(
                address=address,
                location=location,
                expected_rate_hz=self.sampling_rate_hz,
            )
            self.stats_by_address[address] = stats
            return stats
        if stats.location is None and location is not None:
            stats.location = location
        if stats.expected_rate_hz is None:
            stats.expected_rate_hz = self.sampling_rate_hz
        return stats

    def _reset_attempt_state(self, attempt_number: int):
        self.attempt_number = attempt_number
        self.attempt_outcome = ""
        self.attempt_reason = ""
        self.connected_addresses = set()
        self.disconnected_addresses = set()
        self.stats_by_address = {}
        self.measurement_active = False
        self.measurement_started_at = None
        self.stream_started_at = None
        self.sensor_id_by_address = {}
        self.address_by_sensor_id = {}

    def _best_effort_cleanup(self, reader, connected: list[str], streams_started: bool):
        if not connected:
            return

        if streams_started:
            try:
                print("Best-effort cleanup: stopping streams.")
                self._stop_streams(reader, connected)
            except Exception as exc:
                print(f"Best-effort stop skipped/failed: {exc}")

        try:
            print("Best-effort cleanup: disconnecting sensors.")
            disconnected = disconnect_addresses_quiet(
                reader,
                connected,
                timeout_s=min(self.disconnect_timeout_s, 5.0),
            )
            self.disconnected_addresses.update(disconnected)
        except Exception as exc:
            print(f"Best-effort disconnect incomplete: {exc}")
            self.attempt_reason = (
                f"{self.attempt_reason}; cleanup_incomplete={exc}"
                if self.attempt_reason
                else f"cleanup_incomplete={exc}"
            )

    def _print_attempt_summary(self):
        print("")
        print(
            f"Attempt {self.attempt_number} summary "
            f"outcome={self.attempt_outcome or 'unknown'}"
        )
        print(f"Reason: {self.attempt_reason or 'n/a'}")
        for address in sorted(self.stats_by_address):
            stats = self.stats_by_address[address]
            time_to_first_packet_ms = (
                "n/a"
                if stats.time_to_first_packet_ms is None
                else f"{stats.time_to_first_packet_ms:.1f}"
            )
            print(
                f"{address} location={stats.location} "
                f"startup_packets={stats.startup_packets_received} "
                f"startup_rate_hz={stats.startup_observed_rate_hz:.2f} "
                f"startup_gap_events={stats.startup_gap_events} "
                f"startup_drops={stats.startup_estimated_dropped_packets} "
                f"time_to_first_packet_ms={time_to_first_packet_ms} "
                f"packets={stats.measurement_packets_received} "
                f"observed_rate_hz={stats.observed_rate_hz:.2f} "
                f"expected_rate_hz={stats.expected_rate_hz} "
                f"gap_events={stats.gap_events} "
                f"estimated_dropped_packets={stats.estimated_dropped_packets}"
            )

    def _print_overall_summary(self):
        print("")
        print("Overall summary")
        for attempt in self.attempt_summaries:
            print(
                f"attempt={attempt['attempt']} "
                f"outcome={attempt['outcome'] or 'unknown'} "
                f"reason={attempt['reason'] or 'n/a'}"
            )


def parse_sensor_counts(raw: str) -> list[int]:
    values = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            start_str, end_str = part.split("-", 1)
            start = int(start_str)
            end = int(end_str)
            if start > end:
                raise ValueError(f"Invalid sensor count range: {part}")
            values.extend(range(start, end + 1))
        else:
            values.append(int(part))

    deduped = []
    seen = set()
    for value in values:
        if value < 1 or value > 8:
            raise ValueError(f"Sensor count out of range: {value}")
        if value not in seen:
            seen.add(value)
            deduped.append(value)
    if not deduped:
        raise ValueError("No sensor counts selected")
    return deduped


def build_parser():
    parser = argparse.ArgumentParser(
        description=(
            "Gateway-backed startup retry monitor for Movella DOT notifications. "
            "Supports a startup gate or immediate measurement mode."
        )
    )
    parser.add_argument("--port", default=DEFAULT_PORT, help="Serial port path.")
    parser.add_argument(
        "--sensor-counts",
        default="1-8",
        help="Comma-separated list and/or ranges of sensor counts to test, for example 1-8 or 2,4,8.",
    )
    parser.add_argument(
        "--stream-seconds",
        type=int,
        default=10,
        help="How long the official measurement capture should run.",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=int,
        default=90,
        help="Per-attempt timeout for discover/connect/stream/disconnect flow.",
    )
    parser.add_argument("--scan-timeout-ms", type=int, default=5000)
    parser.add_argument(
        "--connect-attempt-timeout-s",
        type=float,
        default=30.0,
        help="Seconds to wait for a connect attempt before retrying pending sensors.",
    )
    parser.add_argument(
        "--connect-retry-attempts",
        type=int,
        default=0,
        help="Number of retry attempts after the initial connect attempt.",
    )
    parser.add_argument(
        "--connect-retry-delay-s",
        type=float,
        default=2.0,
        help="Seconds to wait before retrying failed or pending connections.",
    )
    parser.add_argument(
        "--subscribe-timeout-s",
        type=float,
        default=10.0,
        help="Seconds to wait for each subscribe_complete.",
    )
    parser.add_argument(
        "--write-timeout-s",
        type=float,
        default=10.0,
        help="Seconds to wait for each write_complete.",
    )
    parser.add_argument(
        "--disconnect-timeout-s",
        type=float,
        default=5.0,
        help="Seconds to wait for disconnect confirmation.",
    )
    parser.add_argument(
        "--post-connect-settle-seconds",
        type=float,
        default=2.0,
        help="Delay after all sensors connect before subscribing and starting streams.",
    )
    parser.add_argument(
        "--sampling-rate-hz",
        type=int,
        choices=[20, 60],
        default=60,
        help="Movella DOT sampling rate to configure before streaming.",
    )
    parser.add_argument(
        "--use-startup-gate",
        dest="use_startup_gate",
        action="store_true",
        help="Require startup stability before official measurement.",
    )
    parser.add_argument(
        "--no-startup-gate",
        dest="use_startup_gate",
        action="store_false",
        help="Start official measurement immediately after streams start.",
    )
    parser.set_defaults(use_startup_gate=True)
    parser.add_argument(
        "--startup-stability-window-seconds",
        type=float,
        default=5.0,
        help="How long to wait for all sensors to pass the startup stability gate.",
    )
    parser.add_argument(
        "--startup-packets-required",
        type=int,
        default=60,
        help="Minimum startup packets required per sensor before evaluating rate.",
    )
    parser.add_argument(
        "--startup-min-rate-hz",
        type=float,
        default=58.0,
        help="Minimum accepted startup packet rate per sensor.",
    )
    parser.add_argument(
        "--startup-min-observation-seconds",
        type=float,
        default=2.0,
        help="Minimum startup observation window per sensor before accepting stability.",
    )
    parser.add_argument(
        "--retry-delay-seconds",
        type=float,
        default=5.0,
        help="Delay between failed startup attempts.",
    )
    parser.add_argument(
        "--max-start-attempts",
        type=int,
        default=2,
        help="How many startup attempts to allow per sensor count.",
    )
    parser.add_argument("--identify-retry-attempts", type=int, default=1)
    parser.add_argument("--identify-retry-delay-s", type=float, default=1.0)
    parser.add_argument("--pre-identify-delay-s", type=float, default=2.0)
    parser.add_argument(
        "--without-response",
        action="store_true",
        help="Use write without response for Movella write commands.",
    )
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()

    if args.timeout_seconds <= args.stream_seconds:
        raise SystemExit("--timeout-seconds must be greater than --stream-seconds")

    try:
        sensor_counts = parse_sensor_counts(args.sensor_counts)
    except ValueError as exc:
        raise SystemExit(str(exc))

    overall_exit_code = 0
    for sensor_count in sensor_counts:
        print("")
        print("=" * 72)
        print(
            f"Gateway startup-retry monitor: sensor_count={sensor_count} "
            f"startup_gate={'on' if args.use_startup_gate else 'off'}"
        )
        print("=" * 72)

        exit_code = GatewayStartupRetryMonitorTest(
            port=args.port,
            sensor_count=sensor_count,
            stream_seconds=args.stream_seconds,
            timeout_seconds=args.timeout_seconds,
            scan_timeout_ms=args.scan_timeout_ms,
            connect_attempt_timeout_s=args.connect_attempt_timeout_s,
            connect_retry_attempts=args.connect_retry_attempts,
            connect_retry_delay_s=args.connect_retry_delay_s,
            subscribe_timeout_s=args.subscribe_timeout_s,
            write_timeout_s=args.write_timeout_s,
            disconnect_timeout_s=args.disconnect_timeout_s,
            post_connect_settle_seconds=args.post_connect_settle_seconds,
            sampling_rate_hz=args.sampling_rate_hz,
            use_startup_gate=args.use_startup_gate,
            startup_stability_window_seconds=args.startup_stability_window_seconds,
            startup_packets_required=args.startup_packets_required,
            startup_min_rate_hz=args.startup_min_rate_hz,
            startup_min_observation_seconds=args.startup_min_observation_seconds,
            retry_delay_seconds=args.retry_delay_seconds,
            max_start_attempts=args.max_start_attempts,
            identify_retry_attempts=args.identify_retry_attempts,
            identify_retry_delay_s=args.identify_retry_delay_s,
            pre_identify_delay_s=args.pre_identify_delay_s,
            without_response=args.without_response,
        ).run()

        if exit_code != 0:
            overall_exit_code = exit_code

    raise SystemExit(overall_exit_code)


if __name__ == "__main__":
    main()

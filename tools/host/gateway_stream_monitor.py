#!/usr/bin/env python3
"""
Clean gateway-backed Movella DOT stream monitor.

Flow per attempt:
  scan -> connect -> configure -> subscribe -> start stream -> monitor -> stop stream -> disconnect

Design goals:
  - Keep the test process obvious.
  - One GatewayClient owns all reads so JSON events are not accidentally lost.
  - Disconnect events observed during stop/write are cached and reused by disconnect().
  - Binary stream frames are parsed in one place.
  - Startup gate is optional and separate from the official measurement window.
"""

import argparse
import json
import struct
import time
from dataclasses import dataclass, replace
from typing import Any

from gateway_discover_connect import (
    DEFAULT_PORT,
    MOVELLA_DEVICE_CONTROL_UUID,
    MOVELLA_START_STOP_STREAM_UUID,
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

MOVELLA_NAME = "Movella DOT"
MOVELLA_LONG_PAYLOAD_UUID = "15172002-4947-11e9-8646-d663bd873d93"
MOVELLA_SET_RATE_HEX = {
    20: "100000000000000B4D6F76656C6C6120444F5400000000001400000000000000",
    60: "100000000000000B4D6F76656C6C6120444F5400000000003C00000000000000",
}
MOVELLA_START_HEX = "01011A"
MOVELLA_STOP_HEX = "01001A"
MOVELLA_MIN_PACKET_LEN = 4
STREAM_FRAME_MAGIC = b"\xA5\x5A"


@dataclass
class StreamFrame:
    sensor_id: int
    gateway_timestamp_us: int
    payload: bytes


@dataclass
class SensorStats:
    address: str
    location: str | None
    expected_rate_hz: int
    stream_start_command_time: float | None = None
    first_packet_time: float | None = None

    startup_first_sensor_timestamp: int | None = None
    startup_last_sensor_timestamp: int | None = None
    startup_first_wall_time: float | None = None
    startup_last_wall_time: float | None = None
    startup_packets_received: int = 0
    startup_gap_events: int = 0
    startup_estimated_dropped_packets: int = 0
    startup_gap_detection_start_wall_time: float | None = None

    measurement_first_sensor_timestamp: int | None = None
    measurement_last_sensor_timestamp: int | None = None
    measurement_first_wall_time: float | None = None
    measurement_last_wall_time: float | None = None
    measurement_packets_received: int = 0
    gap_events: int = 0
    estimated_dropped_packets: int = 0

    @property
    def expected_delta_us(self) -> float:
        return 1_000_000.0 / float(self.expected_rate_hz)

    @property
    def startup_duration_seconds(self) -> float:
        if self.startup_first_wall_time is None or self.startup_last_wall_time is None:
            return 0.0
        return max(self.startup_last_wall_time - self.startup_first_wall_time, 0.0)

    @property
    def startup_observed_rate_hz(self) -> float:
        duration = self.startup_duration_seconds
        return 0.0 if duration <= 0 else self.startup_packets_received / duration

    @property
    def measurement_duration_seconds(self) -> float:
        if self.measurement_first_wall_time is None or self.measurement_last_wall_time is None:
            return 0.0
        return max(self.measurement_last_wall_time - self.measurement_first_wall_time, 0.0)

    @property
    def observed_rate_hz(self) -> float:
        duration = self.measurement_duration_seconds
        return 0.0 if duration <= 0 else self.measurement_packets_received / duration

    @property
    def time_to_first_packet_ms(self) -> float | None:
        if self.stream_start_command_time is None or self.first_packet_time is None:
            return None
        return max((self.first_packet_time - self.stream_start_command_time) * 1000.0, 0.0)

    def record_sample(self, timestamp: int | None, wall_time: float, measurement_active: bool):
        if self.first_packet_time is None:
            self.first_packet_time = wall_time
            self.startup_gap_detection_start_wall_time = wall_time + 1.0

        if measurement_active:
            self._record_measurement_sample(timestamp, wall_time)
        else:
            self._record_startup_sample(timestamp, wall_time)

    def _record_startup_sample(self, timestamp: int | None, wall_time: float):
        if self.startup_first_sensor_timestamp is None:
            self.startup_first_sensor_timestamp = timestamp
            self.startup_first_wall_time = wall_time
        elif (
            self.startup_gap_detection_start_wall_time is not None
            and wall_time >= self.startup_gap_detection_start_wall_time
        ):
            self._record_gap_if_needed(
                timestamp=timestamp,
                previous_timestamp=self.startup_last_sensor_timestamp,
                startup=True,
            )

        self.startup_last_sensor_timestamp = timestamp
        self.startup_last_wall_time = wall_time
        self.startup_packets_received += 1

    def _record_measurement_sample(self, timestamp: int | None, wall_time: float):
        if self.measurement_first_sensor_timestamp is None:
            self.measurement_first_sensor_timestamp = timestamp
            self.measurement_first_wall_time = wall_time
        else:
            self._record_gap_if_needed(
                timestamp=timestamp,
                previous_timestamp=self.measurement_last_sensor_timestamp,
                startup=False,
            )

        self.measurement_last_sensor_timestamp = timestamp
        self.measurement_last_wall_time = wall_time
        self.measurement_packets_received += 1

    def _record_gap_if_needed(self, timestamp: int | None, previous_timestamp: int | None, startup: bool):
        if timestamp is None or previous_timestamp is None:
            return

        observed_delta_us = timestamp - previous_timestamp
        if observed_delta_us <= int(self.expected_delta_us * 1.5):
            return

        missing_packets = max(int(round(observed_delta_us / self.expected_delta_us)) - 1, 0)
        if missing_packets <= 0:
            return

        if startup:
            self.startup_gap_events += 1
            self.startup_estimated_dropped_packets += missing_packets
        else:
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


class GatewayClient:
    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()
        self.cached_json: list[dict[str, Any]] = []
        self.cached_stream_frames: list[StreamFrame] = []
        self.disconnected_addresses: set[str] = set()
        self.notification_drop_count: int = 0

    def send(self, obj: dict[str, Any]):
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        self.ser.write(line.encode("utf-8"))
        self.ser.flush()

    def read_item(self, timeout_s: float = 10.0):
        if self.cached_stream_frames:
            return ("stream_frame", self.cached_stream_frames.pop(0))
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            item = self._extract_item()
            if item is not None:
                item_type, payload = item
                if item_type == "json":
                    self._observe_json(payload)
                return item

            chunk = self.ser.read(256)
            if chunk:
                self.buf.extend(chunk)

        raise TimeoutError("Timed out waiting for gateway item")

    def read_json(self, timeout_s: float = 10.0) -> dict[str, Any]:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.cached_json:
                return self.cached_json.pop(0)

            item_type, payload = self.read_item(timeout_s=max(0.1, deadline - time.time()))

            if item_type == "json":
                return payload

            if item_type == "stream_frame":
                self.cached_stream_frames.append(payload)
                continue

        raise TimeoutError("Timed out waiting for JSON")

    def wait_for_request(self, request_id: str, success_type: str, timeout_s: float):
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            msg = self.read_json(timeout_s=max(0.1, deadline - time.time()))
            msg_type = msg.get("type")

            if msg_type == success_type and msg.get("request_id") == request_id:
                return msg

            if msg_type == "error" and msg.get("request_id") == request_id:
                raise RuntimeError(
                    f"Gateway command failed: {msg.get('message')} ({msg.get('error_code')})"
                )

            # Preserve unrelated JSON for higher-level waits only if it is not a noisy status/debug event.
            if msg_type not in {"gatt_debug"}:
                self.cached_json.append(msg)

        raise TimeoutError(f"Timed out waiting for {success_type} request_id={request_id}")

    def hello(self):
        request_id = "hello_host_tool"
        self.send(
            {
                "type": "hello",
                "request_id": request_id,
                "protocol_version": 1,
                "client": "clean_gateway_stream_monitor",
            }
        )
        self.wait_for_request(request_id, "hello_ack", timeout_s=5.0)

    def discover_movella(self, timeout_ms: int) -> list[dict[str, Any]]:
        request_id = f"scan_{int(time.time() * 1000)}"
        self.send({"type": "scan_start", "request_id": request_id, "timeout_ms": timeout_ms})
        matches: dict[str, dict[str, Any]] = {}

        while True:
            msg = self.read_json(timeout_s=max(10.0, timeout_ms / 1000.0 + 5.0))
            msg_type = msg.get("type")

            if msg_type == "scan_result" and msg.get("request_id") == request_id:
                if msg.get("name") != MOVELLA_NAME:
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

    def connect(self, addresses: list[str], timeout_s: float):
        request_id = f"connect_{int(time.time() * 1000)}"
        pending = list(addresses)
        connected: list[str] = []
        sensor_id_by_address: dict[str, int] = {}

        self.send({"type": "connect_addresses", "request_id": request_id, "addresses": pending})

        deadline = time.time() + timeout_s
        while time.time() < deadline and pending:
            msg = self.read_json(timeout_s=max(0.1, deadline - time.time()))
            msg_type = msg.get("type")

            if msg_type == "sensor_connected":
                address = msg.get("address")
                if address in pending:
                    pending.remove(address)
                    connected.append(address)
                    if isinstance(msg.get("sensor_id"), int):
                        sensor_id_by_address[address] = msg["sensor_id"]
                    print(f"CONNECTED: {address}")
                continue

            if msg_type == "sensor_disconnected":
                address = msg.get("address")
                if address in pending:
                    pending.remove(address)
                    print(f"CONNECT FAILED: {address} reason={msg.get('reason')}")
                continue

            if msg_type == "error" and msg.get("request_id") == request_id:
                raise RuntimeError(
                    f"Gateway connect failed: {msg.get('message')} ({msg.get('error_code')})"
                )

        if pending:
            raise TimeoutError("Failed to connect: " + ", ".join(pending))

        return connected, sensor_id_by_address

    def subscribe_binary(self, address: str, uuid: str, timeout_s: float):
        request_id = f"subscribe_{int(time.time() * 1000)}"
        self.send(
            {
                "type": "subscribe",
                "request_id": request_id,
                "address": address,
                "characteristic_uuid": uuid,
                "binary_notifications": True,
            }
        )
        self.wait_for_request(request_id, "subscribe_complete", timeout_s)
        print(f"SUBSCRIBE COMPLETE: {address} uuid={uuid}")

    def write(self, address: str, uuid: str, payload_hex: str, timeout_s: float, without_response: bool):
        request_id = f"write_{int(time.time() * 1000)}"
        self.send(
            {
                "type": "gatt_write",
                "request_id": request_id,
                "address": address,
                "characteristic_uuid": uuid,
                "payload_hex": payload_hex,
                "without_response": without_response,
            }
        )

        try:
            self.wait_for_request(request_id, "write_complete", timeout_s)
        except Exception as exc:
            raise RuntimeError(
                f"gatt_write failed address={address} "
                f"uuid={uuid} payload_hex={payload_hex} "
                f"without_response={without_response}: {exc}"
            ) from exc

    def disconnect(self, addresses: list[str], timeout_s: float) -> list[str]:
        request_id = f"disconnect_{int(time.time() * 1000)}"
        pending = [address for address in addresses if address not in self.disconnected_addresses]
        disconnected = [address for address in addresses if address in self.disconnected_addresses]

        if not pending:
            return disconnected

        self.send({"type": "disconnect_addresses", "request_id": request_id, "addresses": pending})
        deadline = time.time() + timeout_s

        while time.time() < deadline and pending:
            msg = self.read_json(timeout_s=max(0.1, deadline - time.time()))
            msg_type = msg.get("type")

            if msg_type == "sensor_disconnected":
                address = msg.get("address")
                if address in pending:
                    pending.remove(address)
                    disconnected.append(address)
                    self.disconnected_addresses.add(address)
                    print(f"DISCONNECTED: {address}")
                continue

            if msg_type == "error" and msg.get("request_id") == request_id:
                # If the gateway says an address is already gone, treat cleanup as best-effort complete.
                if msg.get("error_code") == -3:
                    print("DISCONNECT WARNING: gateway reported one or more sensors already disconnected")
                    break
                raise RuntimeError(
                    f"Gateway disconnect failed: {msg.get('message')} ({msg.get('error_code')})"
                )

        if pending:
            print("DISCONNECT INCOMPLETE:", pending)

        return disconnected

    def _observe_json(self, msg: dict[str, Any]):
        msg_type = msg.get("type")

        if msg_type == "sensor_disconnected":
            address = msg.get("address")
            if address:
                self.disconnected_addresses.add(address)

        elif msg_type == "notification_drops":
            value = msg.get("drop_count")
            if isinstance(value, int):
                self.notification_drop_count = value
                print(f"GATEWAY NOTIFICATION DROPS: {value}")

        elif msg_type == "notification_seen":
            print(
                "GATEWAY NOTIFICATION SEEN: "
                f"seen_count={msg.get('seen_count')} "
                f"drop_count={msg.get('drop_count')}"
            )

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
                    StreamFrame(
                        sensor_id=sensor_id,
                        gateway_timestamp_us=gateway_timestamp_us,
                        payload=payload,
                    ),
                )

            next_json = self.buf.find(b"{")
            next_bin = self.buf.find(STREAM_FRAME_MAGIC)
            candidates = [idx for idx in (next_json, next_bin) if idx >= 0]
            if not candidates:
                self.buf.clear()
                return None
            del self.buf[: min(candidates)]

        return None


def parse_movella_timestamp(data: bytes) -> int:
    if len(data) < MOVELLA_MIN_PACKET_LEN:
        raise ValueError(f"Movella payload too short: {len(data)} bytes")
    return int(struct.unpack_from("<I", data, 0)[0])


def parse_sensor_counts(raw: str) -> list[int]:
    values: list[int] = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            start_str, end_str = part.split("-", 1)
            values.extend(range(int(start_str), int(end_str) + 1))
        else:
            values.append(int(part))

    deduped: list[int] = []
    seen: set[int] = set()
    for value in values:
        if value < 1 or value > 8:
            raise ValueError(f"Sensor count out of range: {value}")
        if value not in seen:
            seen.add(value)
            deduped.append(value)
    if not deduped:
        raise ValueError("No sensor counts selected")
    return deduped


class StreamAttempt:
    def __init__(self, client: GatewayClient, args, sensor_count: int, attempt_number: int):
        self.client = client
        self.args = args
        self.sensor_count = sensor_count
        self.attempt_number = attempt_number
        self.outcome = ""
        self.reason = ""
        self.connected: list[str] = []
        self.address_by_sensor_id: dict[int, str] = {}
        self.stats: dict[str, SensorStats] = {}
        self.measurement_active = False
        self.stream_started_at: float | None = None
        self.stream_frames_seen = 0
        self.stream_frames_unknown_sensor_id = 0
        self.unknown_sensor_ids = {}

    def run(self):
        self.client.hello()
        print(f"Scanning for up to {self.args.scan_timeout_ms}ms...")
        matches = self.client.discover_movella(self.args.scan_timeout_ms)
        selected = select_discovered_addresses(matches, self.sensor_count)
        print(f"Selected addresses: {selected}")

        locations = {
            address: DEFAULT_LOCATIONS[index] if index < len(DEFAULT_LOCATIONS) else None
            for index, address in enumerate(selected)
        }

        self.connected, sensor_id_by_address = self.client.connect(
            selected,
            timeout_s=self.args.connect_attempt_timeout_s,
        )
        self.address_by_sensor_id = {sensor_id: address for address, sensor_id in sensor_id_by_address.items()}
        print(f"SENSOR ID MAP: {self.address_by_sensor_id}")
        for address in self.connected:
            self.stats[address] = SensorStats(
                address=address,
                location=locations.get(address),
                expected_rate_hz=self.args.sampling_rate_hz,
            )

        if self.args.post_connect_settle_seconds > 0:
            print(
                "All sensors connected. "
                f"Waiting {self.args.post_connect_settle_seconds:.1f}s for BLE links to settle."
            )
            time.sleep(self.args.post_connect_settle_seconds)

        try:
            self.configure()
            self.start_streams()
            self.monitor()
            self.stop_streams()
            self.client.disconnect(self.connected, timeout_s=self.args.disconnect_timeout_s)
            if not self.outcome:
                self.outcome = "success"
        except Exception as exc:
            if not self.outcome:
                self.outcome = "retry"
                self.reason = str(exc)
            print(f"FAILED: {exc}")
            self.cleanup()

    def configure(self):
        for address in self.connected:
            print(f"CONFIG {address}: pre-stop")
            try:
                self.client.write(
                    address,
                    MOVELLA_START_STOP_STREAM_UUID,
                    MOVELLA_STOP_HEX,
                    timeout_s=self.args.write_timeout_s,
                    without_response=self.args.without_response,
                )
                time.sleep(0.25)
            except Exception as exc:
                print(f"PRE-STOP WARNING: {address}: {exc}")

        for address in self.connected:
            print(f"CONFIG {address}: subscribe")
            self.client.subscribe_binary(
                address,
                MOVELLA_LONG_PAYLOAD_UUID,
                timeout_s=self.args.subscribe_timeout_s,
            )
            time.sleep(0.25)

        for address in self.connected:
            print(f"CONFIG {address}: set-rate {self.args.sampling_rate_hz}Hz")
            self.client.write(
                address,
                MOVELLA_DEVICE_CONTROL_UUID,
                MOVELLA_SET_RATE_HEX[self.args.sampling_rate_hz],
                timeout_s=self.args.write_timeout_s,
                without_response=self.args.without_response,
            )
            time.sleep(0.25)

    def start_streams(self):
        start_time = time.monotonic()
        self.stream_started_at = start_time

        for stats in self.stats.values():
            stats.stream_start_command_time = start_time

        print(f"Starting stream. Total stream budget: {self.args.stream_seconds}s.")

        for address in self.connected:
            print(f"START STREAM: {address}")
            self.client.write(
                address,
                MOVELLA_START_STOP_STREAM_UUID,
                MOVELLA_START_HEX,
                timeout_s=self.args.write_timeout_s,
                without_response=True,
            )
            time.sleep(0.1)

        if self.args.use_startup_gate:
            print(
                "Waiting for startup stability gate: "
                f"up to {self.args.startup_stability_window_seconds:.1f}s."
            )
        else:
            self.measurement_active = True
            self.outcome = "success"
            print("Startup gate disabled. Official measurement is active immediately.")

    def stop_streams(self):
        print("Stopping stream now.")

        for address in self.connected:
            print(f"STOP STREAM: {address}")
            try:
                self.client.write(
                    address,
                    MOVELLA_START_STOP_STREAM_UUID,
                    MOVELLA_STOP_HEX,
                    timeout_s=self.args.write_timeout_s,
                    without_response=True,
                )
                time.sleep(0.1)
            except Exception as exc:
                print(f"STOP STREAM FAILED: {address}: {exc}")
    def cleanup(self):
        print("Cleanup: stop stream, then disconnect.")

        try:
            self.stop_streams()
        except Exception as exc:
            print(f"Cleanup stop incomplete: {exc}")

        try:
            self.client.disconnect(self.connected, timeout_s=self.args.disconnect_timeout_s)
        except Exception as exc:
            print(f"Cleanup disconnect incomplete: {exc}")
            if self.reason:
                self.reason = f"{self.reason}; cleanup_disconnect_incomplete={exc}"
            else:
                self.reason = f"cleanup_disconnect_incomplete={exc}"

    def monitor(self):
        if self.stream_started_at is None:
            raise RuntimeError("Stream was not started")

        attempt_deadline = time.monotonic() + self.args.timeout_seconds
        startup_deadline = time.monotonic() + self.args.startup_stability_window_seconds
        stream_deadline = self.stream_started_at + self.args.stream_seconds

        while time.monotonic() < attempt_deadline:
            now = time.monotonic()

            if now >= stream_deadline:
                if self.args.use_startup_gate and not self.measurement_active:
                    self._fail_startup_gate("Startup stability gate did not pass within stream budget")
                return

            if self.args.use_startup_gate and not self.measurement_active and now >= startup_deadline:
                stable, unstable = self.evaluate_startup_stability()
                if stable:
                    self.activate_measurement()
                else:
                    self._fail_startup_gate(", ".join(unstable) if unstable else "unknown startup instability")

            try:
                item_type, item = self.client.read_item(timeout_s=0.2)
            except TimeoutError:
                continue

            if item_type == "stream_frame":
                frame: StreamFrame = item
                self.stream_frames_seen += 1

                address = self.address_by_sensor_id.get(frame.sensor_id)

                if address not in self.stats:
                    self.stream_frames_unknown_sensor_id += 1
                    self.unknown_sensor_ids[frame.sensor_id] = (
                        self.unknown_sensor_ids.get(frame.sensor_id, 0) + 1
                    )

                    if self.stream_frames_unknown_sensor_id <= 10:
                        print(
                            "IGNORING STREAM FRAME: "
                            f"unknown sensor_id={frame.sensor_id} "
                            f"payload_len={len(frame.payload)} "
                            f"known_ids={self.address_by_sensor_id}"
                        )

                    continue

                timestamp = parse_movella_timestamp(frame.payload)
                self.stats[address].record_sample(
                    timestamp,
                    time.monotonic(),
                    self.measurement_active,
                )

            msg: dict[str, Any] = item
            msg_type = msg.get("type")
            if msg_type == "notification":
                address = msg.get("address")
                characteristic_uuid = str(msg.get("characteristic_uuid", "")).lower()

                if characteristic_uuid == MOVELLA_LONG_PAYLOAD_UUID.lower() and address in self.stats:
                    payload_hex = msg.get("payload_hex", "")

                    try:
                        payload = bytes.fromhex(payload_hex)
                        timestamp = parse_movella_timestamp(payload)
                    except Exception as exc:
                        print(f"IGNORING JSON NOTIFICATION: parse failed address={address}: {exc}")
                        continue

                    print(f"JSON STREAM NOTIFICATION: {address} payload_len={len(payload)}")

                    self.stats[address].record_sample(
                        timestamp,
                        time.monotonic(),
                        self.measurement_active,
                    )

                    if self.args.use_startup_gate and not self.measurement_active:
                        stable, _unstable = self.evaluate_startup_stability()
                        if stable:
                            self.activate_measurement()

                continue
            if msg.get("type") == "sensor_disconnected":
                address = msg.get("address")
                self.outcome = "retry"
                self.reason = f"Unexpected disconnect during stream: {address} reason={msg.get('reason')}"
                raise RuntimeError(self.reason)

            if msg.get("type") == "error":
                print("Gateway error while streaming:", msg)

        raise TimeoutError(f"Attempt timed out after {self.args.timeout_seconds}s")

    def evaluate_startup_stability(self):
        unstable: list[str] = []
        for address in sorted(self.stats):
            stats = self.stats[address]
            if stats.first_packet_time is None:
                unstable.append(f"{address}: no_first_packet")
            elif stats.startup_packets_received < self.args.startup_packets_required:
                unstable.append(f"{address}: packets={stats.startup_packets_received}")
            elif stats.startup_duration_seconds < self.args.startup_min_observation_seconds:
                unstable.append(f"{address}: warmup_window={stats.startup_duration_seconds:.2f}s")
            elif stats.startup_observed_rate_hz < self.args.startup_min_rate_hz:
                unstable.append(f"{address}: rate={stats.startup_observed_rate_hz:.2f}Hz")
            elif stats.startup_gap_events > self.args.startup_max_gap_events:
                unstable.append(
                    f"{address}: startup_gap_events={stats.startup_gap_events} "
                    f"startup_drops={stats.startup_estimated_dropped_packets}"
                )
        return len(unstable) == 0, unstable

    def activate_measurement(self):
        for stats in self.stats.values():
            stats.reset_measurement()
        self.measurement_active = True
        self.outcome = "success"
        print("Startup stability gate passed. Official measurement is now active.")

    def _fail_startup_gate(self, reason: str):
        self.outcome = "retry"
        self.reason = f"Startup stability gate failed: {reason}"
        raise RuntimeError(self.reason)

    def summary(self):
        print("")
        print(f"Attempt {self.attempt_number} summary outcome={self.outcome or 'unknown'}")
        print(f"Reason: {self.reason or 'n/a'}")
        for address in sorted(self.stats):
            stats = self.stats[address]
            ttff = "n/a" if stats.time_to_first_packet_ms is None else f"{stats.time_to_first_packet_ms:.1f}"
            print(
                f"{address} location={stats.location} "
                f"startup_packets={stats.startup_packets_received} "
                f"startup_rate_hz={stats.startup_observed_rate_hz:.2f} "
                f"startup_gap_events={stats.startup_gap_events} "
                f"startup_drops={stats.startup_estimated_dropped_packets} "
                f"time_to_first_packet_ms={ttff} "
                f"packets={stats.measurement_packets_received} "
                f"observed_rate_hz={stats.observed_rate_hz:.2f} "
                f"expected_rate_hz={stats.expected_rate_hz} "
                f"gap_events={stats.gap_events} "
                f"estimated_dropped_packets={stats.estimated_dropped_packets}"
            )


def run_count(args, sensor_count: int) -> int:
    final_success = False
    attempts: list[dict[str, Any]] = []

    for attempt_number in range(1, args.max_start_attempts + 1):
        print("")
        print(
            f"Attempt {attempt_number}/{args.max_start_attempts}: "
            f"starting clean gateway monitor with {sensor_count} sensor(s)."
        )

        with open_gateway_serial(args.port) as ser:
            client = GatewayClient(ser)
            attempt = StreamAttempt(client, args, sensor_count, attempt_number)
            attempt.run()
            attempt.summary()
            attempts.append(
                {
                    "attempt": attempt_number,
                    "outcome": attempt.outcome,
                    "reason": attempt.reason,
                    "stats": {address: replace(stats) for address, stats in attempt.stats.items()},
                }
            )
            if attempt.outcome == "success":
                final_success = True
                break

        if attempt_number < args.max_start_attempts:
            print(f"Retrying startup after {args.retry_delay_seconds:.1f}s.")
            time.sleep(args.retry_delay_seconds)

    print("")
    print("Overall summary")
    for item in attempts:
        print(
            f"attempt={item['attempt']} "
            f"outcome={item['outcome'] or 'unknown'} "
            f"reason={item['reason'] or 'n/a'}"
        )

    return 0 if final_success else 1


def build_parser():
    parser = argparse.ArgumentParser(description="Clean gateway stream monitor for Movella DOT sensors.")
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--sensor-counts", default="1-8")
    parser.add_argument("--stream-seconds", type=int, default=10)
    parser.add_argument("--timeout-seconds", type=int, default=90)
    parser.add_argument("--scan-timeout-ms", type=int, default=5000)
    parser.add_argument("--connect-attempt-timeout-s", type=float, default=30.0)
    parser.add_argument("--subscribe-timeout-s", type=float, default=10.0)
    parser.add_argument("--write-timeout-s", type=float, default=10.0)
    parser.add_argument("--disconnect-timeout-s", type=float, default=5.0)
    parser.add_argument("--post-connect-settle-seconds", type=float, default=2.0)
    parser.add_argument("--sampling-rate-hz", type=int, choices=[20, 60], default=60)
    parser.add_argument("--use-startup-gate", dest="use_startup_gate", action="store_true")
    parser.add_argument("--no-startup-gate", dest="use_startup_gate", action="store_false")
    parser.set_defaults(use_startup_gate=True)
    parser.add_argument("--startup-stability-window-seconds", type=float, default=5.0)
    parser.add_argument("--startup-packets-required", type=int, default=60)
    parser.add_argument("--startup-min-rate-hz", type=float, default=58.0)
    parser.add_argument("--startup-min-observation-seconds", type=float, default=2.0)
    parser.add_argument("--startup-max-gap-events", type=int, default=0)
    parser.add_argument("--retry-delay-seconds", type=float, default=5.0)
    parser.add_argument("--max-start-attempts", type=int, default=2)
    parser.add_argument("--without-response", action="store_true")
    return parser


def main():
    args = build_parser().parse_args()
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
            f"Clean gateway monitor: sensor_count={sensor_count} "
            f"startup_gate={'on' if args.use_startup_gate else 'off'}"
        )
        print("=" * 72)
        exit_code = run_count(args, sensor_count)
        if exit_code != 0:
            overall_exit_code = exit_code

    raise SystemExit(overall_exit_code)


if __name__ == "__main__":
    main()

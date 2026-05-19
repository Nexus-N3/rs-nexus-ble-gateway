#!/usr/bin/env python3

import argparse
import json
import time
from typing import List, Tuple
from dataclasses import dataclass
import serial

@dataclass
class GatewayStreamStats:
    address: str
    expected_rate_hz: float
    stream_start_command_time: float | None = None
    first_packet_wall_time: float | None = None
    first_gateway_time_us: int | None = None
    last_gateway_time_us: int | None = None
    first_sensor_timestamp: int | None = None
    last_sensor_timestamp: int | None = None
    first_wall_time: float | None = None
    last_wall_time: float | None = None
    packets_received: int = 0
    gap_events: int = 0
    estimated_dropped_packets: int = 0

    @property
    def expected_delta_us(self):
        if self.expected_rate_hz <= 0:
            return None
        return 1_000_000.0 / self.expected_rate_hz

    @property
    def duration_seconds(self):
        if self.first_wall_time is None or self.last_wall_time is None:
            return 0.0
        return max(self.last_wall_time - self.first_wall_time, 0.0)

    @property
    def observed_rate_hz(self):
        duration = self.duration_seconds
        if duration <= 0:
            return 0.0
        return self.packets_received / duration

    @property
    def time_to_first_packet_ms(self):
        if self.stream_start_command_time is None or self.first_packet_wall_time is None:
            return None
        return max(
            (self.first_packet_wall_time - self.stream_start_command_time) * 1000.0,
            0.0,
        )

    def record_packet(self, sensor_timestamp, gateway_time_us, wall_time):
        if self.first_packet_wall_time is None:
            self.first_packet_wall_time = wall_time

        if self.first_wall_time is None:
            self.first_wall_time = wall_time
            self.first_gateway_time_us = gateway_time_us
            self.first_sensor_timestamp = sensor_timestamp
        else:
            self._record_gap_if_needed(sensor_timestamp)

        self.last_wall_time = wall_time
        self.last_gateway_time_us = gateway_time_us
        self.last_sensor_timestamp = sensor_timestamp
        self.packets_received += 1

    def _record_gap_if_needed(self, sensor_timestamp):
        if sensor_timestamp is None or self.last_sensor_timestamp is None:
            return

        expected_delta_us = self.expected_delta_us
        if expected_delta_us is None:
            return

        observed_delta_us = sensor_timestamp - self.last_sensor_timestamp

        if observed_delta_us <= int(expected_delta_us * 1.5):
            return

        missing_packets = max(int(round(observed_delta_us / expected_delta_us)) - 1, 0)

        if missing_packets <= 0:
            return

        self.gap_events += 1
        self.estimated_dropped_packets += missing_packets

DEFAULT_PORT = "/dev/serial/by-id/usb-SEGGER_J-Link_001057755524-if02"
BAUD = 1000000
MOVELLA_NAME = "Movella DOT"
MOVELLA_UUID_HINTS = {
    "15173001-4947-11e9-8646-d663bd873d93",
    "15171002-4947-11e9-8646-d663bd873d93",
    "15171004-4947-11e9-8646-d663bd873d93",
    "15172001-4947-11e9-8646-d663bd873d93",
    "15172002-4947-11e9-8646-d663bd873d93",
    "15172003-4947-11e9-8646-d663bd873d93",
    "15172004-4947-11e9-8646-d663bd873d93",
}
MOVELLA_DEVICE_CONTROL_UUID = "15171002-4947-11e9-8646-d663bd873d93"
MOVELLA_IDENTIFY_HEX = "010102"
MOVELLA_BATTERY_UUID = "15173001-4947-11e9-8646-d663bd873d93"
MOVELLA_START_STOP_STREAM_UUID = "15172001-4947-11e9-8646-d663bd873d93"
MOVELLA_START_HEX = "01011A"
MOVELLA_STOP_HEX = "01001A"

def disconnect_addresses(ser, addresses, timeout_s=10):
    request_id = f"disconnect_{int(time.time() * 1000)}"
    pending = list(addresses)
    disconnected = []

    send_jsonl(
        ser,
        {
            "type": "disconnect_addresses",
            "request_id": request_id,
            "addresses": pending,
        },
    )

    deadline = time.time() + timeout_s

    while time.time() < deadline:
        try:
            msg = read_json_any(ser, timeout_s=max(0.5, deadline - time.time()))
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
            code = msg.get("error_code")
            message = msg.get("message", "unknown_error")

            if code == -3:
                raise RuntimeError(
                    "Gateway could not disconnect one or more sensors because they "
                    "are not connected."
                )

            raise RuntimeError(f"Gateway disconnect failed: {message} ({code})")

        print("Ignoring JSON message:")
        print(json.dumps(msg, indent=2))

    raise TimeoutError(f"Timed out waiting for disconnect of: {', '.join(pending)}")


def gatt_write_address(
    ser,
    address,
    characteristic_uuid,
    payload_hex,
    without_response=False,
    timeout_s=10,
):
    request_id = f"write_{int(time.time() * 1000)}"

    send_jsonl(
        ser,
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
        msg = read_json_any(ser, timeout_s=max(0.1, deadline - time.time()))
        msg_type = msg.get("type")

        if msg_type == "write_complete" and msg.get("request_id") == request_id:
            print(
                f"WRITE COMPLETE: {address} uuid={characteristic_uuid}"
            )
            return True

        if msg_type == "error" and msg.get("request_id") == request_id:
            raise RuntimeError(
                f"Gateway gatt_write failed: {msg.get('message')} "
                f"({msg.get('error_code')})"
            )

    raise TimeoutError(f"Timed out waiting for gatt_write on {address}")


def gatt_read_address(
    ser,
    address,
    characteristic_uuid,
    timeout_s=10,
):
    request_id = f"read_{int(time.time() * 1000)}"

    send_jsonl(
        ser,
        {
            "type": "gatt_read",
            "request_id": request_id,
            "address": address,
            "characteristic_uuid": characteristic_uuid,
        },
    )

    deadline = time.time() + timeout_s
    while time.time() < deadline:
        msg = read_json_any(ser, timeout_s=max(0.1, deadline - time.time()))
        msg_type = msg.get("type")

        if msg_type == "read_result" and msg.get("request_id") == request_id:
            payload_hex = msg.get("payload_hex", "")
            print(
                f"READ COMPLETE: {address} uuid={characteristic_uuid} "
                f"payload_hex={payload_hex}"
            )
            return payload_hex

        if msg_type == "error" and msg.get("request_id") == request_id:
            raise RuntimeError(
                f"Gateway gatt_read failed: {msg.get('message')} "
                f"({msg.get('error_code')})"
            )

    raise TimeoutError(f"Timed out waiting for gatt_read on {address}")

def gatt_subscribe_address(
    ser,
    address,
    characteristic_uuid,
    timeout_s=10,
):
    request_id = f"subscribe_{int(time.time() * 1000)}"

    send_jsonl(
        ser,
        {
            "type": "subscribe",
            "request_id": request_id,
            "address": address,
            "characteristic_uuid": characteristic_uuid,
        },
    )

    deadline = time.time() + timeout_s
    while time.time() < deadline:
        msg = read_json_any(ser, timeout_s=max(0.1, deadline - time.time()))
        msg_type = msg.get("type")

        if msg_type == "subscribe_complete" and msg.get("request_id") == request_id:
            print(
                f"SUBSCRIBE COMPLETE: {address} uuid={characteristic_uuid}"
            )
            return True

        if msg_type == "error" and msg.get("request_id") == request_id:
            raise RuntimeError(
                f"Gateway subscribe failed: {msg.get('message')} "
                f"({msg.get('error_code')})"
            )

        print("Ignoring JSON message:")
        print(json.dumps(msg, indent=2))

    raise TimeoutError(f"Timed out waiting for subscribe on {address}")

def identify_address(ser, address, args):
    last_error = None

    for attempt in range(args.identify_retry_attempts + 1):
        if args.pre_identify_delay_s > 0:
            print(
                f"Waiting {args.pre_identify_delay_s:.1f}s before identify on "
                f"{address} (attempt {attempt + 1})..."
            )
            time.sleep(args.pre_identify_delay_s)

        try:
            current_hex = gatt_read_address(
                ser,
                address,
                MOVELLA_DEVICE_CONTROL_UUID,
                timeout_s=args.read_timeout_s,
            )
            if len(current_hex) < 6:
                raise RuntimeError(
                    f"Device control read too short for identify on {address}: "
                    f"{current_hex}"
                )

            identify_hex = MOVELLA_IDENTIFY_HEX + current_hex[6:]
            gatt_write_address(
                ser,
                address,
                MOVELLA_DEVICE_CONTROL_UUID,
                identify_hex,
                without_response=args.without_response,
                timeout_s=args.write_timeout_s,
            )
            return
        except (RuntimeError, TimeoutError) as exc:
            last_error = exc
            if attempt < args.identify_retry_attempts:
                print(
                    f"Identify attempt {attempt + 1} failed for {address}: {exc}"
                )
                print(
                    f"Retrying identify in {args.identify_retry_delay_s:.1f}s..."
                )
                time.sleep(args.identify_retry_delay_s)

    raise RuntimeError(f"Identify failed for {address}: {last_error}")

def start_stream_address(ser, address, args):
    print(f"START STREAM: {address}")
    return gatt_write_address(
        ser,
        address,
        MOVELLA_START_STOP_STREAM_UUID,
        MOVELLA_START_HEX,
        without_response=args.without_response,
        timeout_s=args.write_timeout_s,
    )


def stop_stream_address(ser, address, args):
    print(f"STOP STREAM: {address}")
    return gatt_write_address(
        ser,
        address,
        MOVELLA_START_STOP_STREAM_UUID,
        MOVELLA_STOP_HEX,
        without_response=args.without_response,
        timeout_s=args.write_timeout_s,
    )


def match_sensor_name(
    local_name: str,
    names_sorted: List[str],
    names_sorted_lower: List[Tuple[str, str]],
):
    """Copied from rs-nexus-os host-side BLE matching behavior."""
    if not local_name:
        return None
    for name in names_sorted:
        if local_name == name or local_name.startswith(name):
            return name
    local_lower = local_name.lower()
    for name, lower_name in names_sorted_lower:
        if local_lower == lower_name or local_lower.startswith(lower_name):
            return name
    return None


def send_jsonl(ser, obj):
    line = json.dumps(obj, separators=(",", ":")) + "\n"
    print("HOST ->", line.strip())
    ser.write(line.encode("utf-8"))
    ser.flush()


def normalize_uuids(service_uuids):
    if not isinstance(service_uuids, list):
        return []
    return [str(uuid).lower() for uuid in service_uuids if uuid]

def parse_battery_percent(payload_hex):
    if not payload_hex:
        return None

    try:
        data = bytes.fromhex(payload_hex)
    except ValueError:
        return None

    if not data:
        return None

    return data[0]

def match_movella(msg, names_sorted, names_sorted_lower):
    matched_name = match_sensor_name(
        msg.get("name", ""),
        names_sorted,
        names_sorted_lower,
    )
    if matched_name is not None:
        return matched_name, "name"

    service_uuids = normalize_uuids(msg.get("service_uuids", []))
    for uuid in service_uuids:
        if uuid in MOVELLA_UUID_HINTS:
            return MOVELLA_NAME, "service_uuid"

    return None, None


def read_json_any(ser, timeout_s=10):
    deadline = time.time() + timeout_s
    line_buf = bytearray()

    while time.time() < deadline:
        b = ser.read(1)

        if not b:
            continue

        if b == b"\r":
            continue

        if b != b"\n":
            line_buf.extend(b)
            continue

        line = line_buf.decode("utf-8", errors="replace").strip()
        line_buf.clear()

        if not line:
            continue

        print("BOARD <-", line)

        for msg in json_objects_from_line(line):
            return msg

    raise TimeoutError("Timed out waiting for JSON")

def json_objects_from_line(line):
    decoder = json.JSONDecoder()

    for i, ch in enumerate(line):
        if ch != "{":
            continue

        try:
            obj, _ = decoder.raw_decode(line[i:])
            yield obj
        except json.JSONDecodeError:
            continue

def read_json_until(ser, wanted_type, request_id=None, timeout_s=10):
    deadline = time.time() + timeout_s

    while time.time() < deadline:
        msg = read_json_any(ser, timeout_s=max(0.1, deadline - time.time()))
        if msg.get("type") != wanted_type:
            continue
        if request_id is not None and msg.get("request_id") != request_id:
            continue
        return msg

    raise TimeoutError(f"Timed out waiting for {wanted_type}")


def open_gateway_serial(port):
    ser = serial.Serial(
        port=port,
        baudrate=BAUD,
        timeout=0.1,
        write_timeout=1.0,
        dsrdtr=False,
        rtscts=False,
    )
    ser.setDTR(True)
    ser.setRTS(True)
    time.sleep(0.5)
    ser.reset_input_buffer()
    return ser

def command_hello(ser):
    request_id = "hello_host_tool"
    send_jsonl(
        ser,
        {
            "type": "hello",
            "request_id": request_id,
            "protocol_version": 1,
            "client": "gateway_discover_connect",
        },
    )
    read_json_until(ser, "hello_ack", request_id=request_id, timeout_s=5)


def discover_movella(ser, timeout_ms):
    request_id = f"scan_{int(time.time() * 1000)}"
    names_sorted = [MOVELLA_NAME]
    names_sorted_lower = [(MOVELLA_NAME, MOVELLA_NAME.lower())]
    matches = {}

    send_jsonl(
        ser,
        {
            "type": "scan_start",
            "request_id": request_id,
            "timeout_ms": timeout_ms,
        },
    )

    while True:
        msg = read_json_any(ser, timeout_s=max(10, timeout_ms / 1000 + 5))
        msg_type = msg.get("type")

        if msg_type == "scan_result" and msg.get("request_id") == request_id:
            matched_name, matched_by = match_movella(
                msg,
                names_sorted,
                names_sorted_lower,
            )
            if matched_name is None:
                continue

            address = msg.get("address")
            if address and address not in matches:
                service_uuids = normalize_uuids(msg.get("service_uuids", []))
                matches[address] = {
                    "address": address,
                    "name": msg.get("name", ""),
                    "rssi": msg.get("rssi"),
                    "matched_name": matched_name,
                    "matched_by": matched_by,
                    "service_uuids": service_uuids,
                }
                print(
                    f"MATCH: {address}  name={msg.get('name', '')}  "
                    f"rssi={msg.get('rssi')}  by={matched_by}  "
                    f"uuids={service_uuids}"
                )
            continue

        if msg_type == "scan_complete" and msg.get("request_id") == request_id:
            return list(matches.values())


def connect_addresses(
    ser,
    addresses,
    attempt_timeout_s,
    retry_attempts=1,
    retry_delay_s=2.0,
):
    remaining = list(addresses)
    connected = []

    for attempt in range(retry_attempts + 1):
        if not remaining:
            break

        request_id = f"connect_{int(time.time() * 1000)}_{attempt}"
        pending = list(remaining)
        failed_this_attempt = []

        send_jsonl(
            ser,
            {
                "type": "connect_addresses",
                "request_id": request_id,
                "addresses": pending,
            },
        )

        deadline = time.time() + attempt_timeout_s
        while time.time() < deadline and pending:
            try:
                msg = read_json_any(ser, timeout_s=max(0.1, deadline - time.time()))
            except TimeoutError:
                break

            msg_type = msg.get("type")

            if msg_type == "sensor_connected":
                address = msg.get("address")
                if address in pending:
                    pending.remove(address)
                    if address not in connected:
                        connected.append(address)
                    event_request_id = msg.get("request_id")
                    if event_request_id != request_id:
                        print(
                            f"CONNECTED: {address} "
                            f"(late/stale request_id={event_request_id}, current={request_id})"
                        )
                    else:
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
                        "were not found in the gateway's current discovery cache. "
                        "Run without --skip-discover, rescan, or check the address."
                    )

                raise RuntimeError(f"Gateway connect failed: {message} ({code})")

        remaining = failed_this_attempt + pending

        if remaining and attempt < retry_attempts:
            print(
                f"Retrying in {retry_delay_s:.1f}s for addresses: {remaining}"
            )
            time.sleep(retry_delay_s)

    if remaining:
        raise TimeoutError(
            f"Failed to connect after {retry_attempts + 1} attempt(s): "
            f"{', '.join(remaining)}"
        )

    return connected


def select_discovered_addresses(matches, count):
    if len(matches) < count:
        raise RuntimeError(
            f"Requested {count} Movella DOT sensors, found {len(matches)}"
        )

    return [entry["address"] for entry in matches[:count]]


def add_connect_retry_args(parser):
    parser.add_argument(
        "--connect-attempt-timeout-s",
        type=float,
        default=20.0,
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


def run_discover(args):
    with open_gateway_serial(args.port) as ser:
        command_hello(ser)
        matches = discover_movella(ser, timeout_ms=args.timeout_ms)
        print(json.dumps(matches, indent=2))


def run_connect(args):
    with open_gateway_serial(args.port) as ser:
        command_hello(ser)
        if not args.skip_discover:
            matches = discover_movella(ser, timeout_ms=args.timeout_ms)
            matched_addresses = {entry["address"] for entry in matches}
            missing = [address for address in args.address if address not in matched_addresses]
            if missing:
                raise RuntimeError(
                    "Requested addresses were not found in current Movella scan: "
                    + ", ".join(missing)
                )
        connected = connect_addresses(
            ser,
            args.address,
            attempt_timeout_s=args.connect_attempt_timeout_s,
            retry_attempts=args.connect_retry_attempts,
            retry_delay_s=args.connect_retry_delay_s,
        )
        print(json.dumps({"connected": connected}, indent=2))


def run_auto_connect(args):
    with open_gateway_serial(args.port) as ser:
        command_hello(ser)
        matches = discover_movella(ser, timeout_ms=args.timeout_ms)
        selected = select_discovered_addresses(matches, args.count)
        print(f"AUTO-CONNECT addresses: {selected}")
        connected = connect_addresses(
            ser,
            selected,
            attempt_timeout_s=args.connect_attempt_timeout_s,
            retry_attempts=args.connect_retry_attempts,
            retry_delay_s=args.connect_retry_delay_s,
        )
        print(
            json.dumps(
                {
                    "matched": matches,
                    "selected": selected,
                    "connected": connected,
                },
                indent=2,
            )
        )


def run_connect_disconnect(args):
    with open_gateway_serial(args.port) as ser:
        command_hello(ser)

        addresses = list(args.address)

        if not args.skip_discover:
            matches = discover_movella(ser, timeout_ms=args.timeout_ms)
            matched_addresses = {entry["address"] for entry in matches}
            missing = [address for address in addresses if address not in matched_addresses]

            if missing:
                raise RuntimeError(
                    "Requested addresses were not found in current Movella scan: "
                    + ", ".join(missing)
                )

        connected = connect_addresses(
            ser,
            addresses,
            attempt_timeout_s=args.connect_attempt_timeout_s,
            retry_attempts=args.connect_retry_attempts,
            retry_delay_s=args.connect_retry_delay_s,
        )

        if not connected:
            raise RuntimeError(f"Failed to connect to: {', '.join(addresses)}")

        print(f"Waiting {args.hold_s:.1f} seconds before disconnect...")
        time.sleep(args.hold_s)

        disconnected = disconnect_addresses(
            ser,
            connected,
            timeout_s=args.timeout_s,
        )

        print(
            json.dumps(
                {
                    "connected_then_disconnected": disconnected,
                },
                indent=2,
            )
        )


def run_auto_connect_disconnect(args):
    with open_gateway_serial(args.port) as ser:
        command_hello(ser)
        matches = discover_movella(ser, timeout_ms=args.timeout_ms)
        selected = select_discovered_addresses(matches, args.count)

        print(f"AUTO-CONNECT-DISCONNECT addresses: {selected}")
        connected = connect_addresses(
            ser,
            selected,
            attempt_timeout_s=args.connect_attempt_timeout_s,
            retry_attempts=args.connect_retry_attempts,
            retry_delay_s=args.connect_retry_delay_s,
        )

        print(f"Waiting {args.hold_s:.1f} seconds before disconnect...")
        time.sleep(args.hold_s)

        disconnected = disconnect_addresses(
            ser,
            connected,
            timeout_s=args.disconnect_timeout_s,
        )

        print(
            json.dumps(
                {
                    "matched": matches,
                    "selected": selected,
                    "connected": connected,
                    "disconnected": disconnected,
                },
                indent=2,
            )
        )

def run_battery(args):
    with open_gateway_serial(args.port) as ser:
        command_hello(ser)

        if args.count is not None:
            matches = discover_movella(ser, timeout_ms=args.timeout_ms)
            selected = select_discovered_addresses(matches, args.count)
            addresses = selected
            print(f"AUTO-BATTERY addresses: {addresses}")
        else:
            addresses = list(args.address)

            if not args.skip_discover:
                matches = discover_movella(ser, timeout_ms=args.timeout_ms)
                matched_addresses = {entry["address"] for entry in matches}
                missing = [
                    address for address in addresses
                    if address not in matched_addresses
                ]

                if missing:
                    raise RuntimeError(
                        "Requested addresses were not found in current Movella scan: "
                        + ", ".join(missing)
                    )

        connected = connect_addresses(
            ser,
            addresses,
            attempt_timeout_s=args.connect_attempt_timeout_s,
            retry_attempts=args.connect_retry_attempts,
            retry_delay_s=args.connect_retry_delay_s,
        )

        if not connected:
            raise RuntimeError(f"Failed to connect to: {', '.join(addresses)}")

        for address in connected:
            gatt_subscribe_address(
                ser,
                address,
                MOVELLA_BATTERY_UUID,
                timeout_s=args.subscribe_timeout_s,
            )

            payload_hex = gatt_read_address(
                ser,
                address,
                MOVELLA_BATTERY_UUID,
                timeout_s=args.read_timeout_s,
            )

            battery_percent = parse_battery_percent(payload_hex)
            if battery_percent is None:
                print(
                    f"INITIAL BATTERY: {address} "
                    f"payload_hex={payload_hex} could not parse"
                )
            else:
                print(f"INITIAL BATTERY: {address} {battery_percent}%")

        deadline = time.time() + args.listen_s
        print(f"Listening for battery notifications for {args.listen_s:.1f}s...")

        while time.time() < deadline:
            try:
                msg = read_json_any(
                    ser,
                    timeout_s=max(0.1, deadline - time.time()),
                )
            except TimeoutError:
                break

            msg_type = msg.get("type")

            if msg_type == "notification":
                address = msg.get("address")
                characteristic_uuid = str(
                    msg.get("characteristic_uuid", "")
                ).lower()

                if characteristic_uuid != MOVELLA_BATTERY_UUID.lower():
                    print("Ignoring non-battery notification:")
                    print(json.dumps(msg, indent=2))
                    continue

                payload_hex = msg.get("payload_hex", "")
                battery_percent = parse_battery_percent(payload_hex)

                if battery_percent is None:
                    print(
                        f"BATTERY NOTIFICATION: {address} "
                        f"payload_hex={payload_hex} could not parse"
                    )
                else:
                    print(f"BATTERY NOTIFICATION: {address} {battery_percent}%")
                continue

            if msg_type == "error":
                print("Gateway error while listening:")
                print(json.dumps(msg, indent=2))
                continue

            print("Ignoring JSON message:")
            print(json.dumps(msg, indent=2))

        if args.disconnect:
            disconnected = disconnect_addresses(
                ser,
                connected,
                timeout_s=args.disconnect_timeout_s,
            )
        else:
            disconnected = []

        print(
            json.dumps(
                {
                    "connected": connected,
                    "disconnected": disconnected,
                },
                indent=2,
            )
        )

def run_identify(args):
    with open_gateway_serial(args.port) as ser:
        command_hello(ser)

        addresses = list(args.address)

        if not args.skip_discover:
            matches = discover_movella(ser, timeout_ms=args.timeout_ms)
            matched_addresses = {entry["address"] for entry in matches}
            missing = [address for address in addresses if address not in matched_addresses]
            if missing:
                raise RuntimeError(
                    "Requested addresses were not found in current Movella scan: "
                    + ", ".join(missing)
                )

        connected = connect_addresses(
            ser,
            addresses,
            attempt_timeout_s=args.connect_attempt_timeout_s,
            retry_attempts=args.connect_retry_attempts,
            retry_delay_s=args.connect_retry_delay_s,
        )

        for address in connected:
            identify_address(ser, address, args)

        if args.hold_s > 0:
            print(f"Waiting {args.hold_s:.1f} seconds before disconnect...")
            time.sleep(args.hold_s)

        disconnected = disconnect_addresses(
            ser,
            connected,
            timeout_s=args.disconnect_timeout_s,
        )

        print(
            json.dumps(
                {
                    "identified": connected,
                    "disconnected": disconnected,
                },
                indent=2,
            )
        )


def run_auto_identify(args):
    with open_gateway_serial(args.port) as ser:
        command_hello(ser)
        matches = discover_movella(ser, timeout_ms=args.timeout_ms)
        selected = select_discovered_addresses(matches, args.count)

        print(f"AUTO-IDENTIFY addresses: {selected}")
        connected = connect_addresses(
            ser,
            selected,
            attempt_timeout_s=args.connect_attempt_timeout_s,
            retry_attempts=args.connect_retry_attempts,
            retry_delay_s=args.connect_retry_delay_s,
        )

        for address in connected:
            identify_address(ser, address, args)

        if args.hold_s > 0:
            print(f"Waiting {args.hold_s:.1f} seconds before disconnect...")
            time.sleep(args.hold_s)

        disconnected = disconnect_addresses(
            ser,
            connected,
            timeout_s=args.disconnect_timeout_s,
        )

        print(
            json.dumps(
                {
                    "matched": matches,
                    "selected": selected,
                    "identified": connected,
                    "disconnected": disconnected,
                },
                indent=2,
            )
        )

def main():
    parser = argparse.ArgumentParser(
        description="Discover and connect Movella DOT sensors through rs-nexus-ble-gateway."
    )
    parser.add_argument("--port", default=DEFAULT_PORT, help="Serial port path.")

    subparsers = parser.add_subparsers(dest="command", required=True)

    discover_parser = subparsers.add_parser(
        "discover",
        help="Scan and print only Movella DOT matches.",
    )
    discover_parser.add_argument("--timeout-ms", type=int, default=5000)
    discover_parser.set_defaults(func=run_discover)

    connect_parser = subparsers.add_parser(
        "connect",
        help="Connect explicit sensor addresses.",
    )
    connect_parser.add_argument(
        "--address",
        action="append",
        required=True,
        help="BLE address to connect. Repeat for multiple sensors.",
    )
    connect_parser.add_argument("--timeout-ms", type=int, default=5000)
    connect_parser.add_argument(
        "--skip-discover",
        action="store_true",
        help="Skip the pre-connect discovery pass.",
    )
    add_connect_retry_args(connect_parser)
    connect_parser.set_defaults(func=run_connect)

    connect_disconnect_parser = subparsers.add_parser(
        "connect-disconnect",
        help="Connect explicit sensor addresses, wait, then disconnect.",
    )
    connect_disconnect_parser.add_argument(
        "--address",
        action="append",
        required=True,
        help="BLE address to connect and disconnect. Repeat for multiple sensors.",
    )
    connect_disconnect_parser.add_argument("--timeout-ms", type=int, default=5000)
    connect_disconnect_parser.add_argument(
        "--skip-discover",
        action="store_true",
        help="Skip the pre-connect discovery pass.",
    )
    add_connect_retry_args(connect_disconnect_parser)
    connect_disconnect_parser.add_argument("--timeout-s", type=float, default=30.0)
    connect_disconnect_parser.add_argument(
        "--hold-s",
        type=float,
        default=1.0,
        help="Seconds to wait after connecting before disconnecting.",
    )
    connect_disconnect_parser.set_defaults(func=run_connect_disconnect)

    auto_connect_parser = subparsers.add_parser(
        "auto-connect",
        help="Scan for Movella DOT sensors and connect the first N matches.",
    )
    auto_connect_parser.add_argument("--count", type=int, required=True)
    auto_connect_parser.add_argument("--timeout-ms", type=int, default=5000)
    add_connect_retry_args(auto_connect_parser)
    auto_connect_parser.set_defaults(func=run_auto_connect)

    auto_connect_disconnect_parser = subparsers.add_parser(
        "auto-connect-disconnect",
        help="Scan for Movella DOT sensors, connect the first N matches, then disconnect them.",
    )
    auto_connect_disconnect_parser.add_argument("--count", type=int, required=True)
    auto_connect_disconnect_parser.add_argument("--timeout-ms", type=int, default=5000)
    add_connect_retry_args(auto_connect_disconnect_parser)
    auto_connect_disconnect_parser.add_argument(
        "--disconnect-timeout-s",
        type=float,
        default=30.0,
    )
    auto_connect_disconnect_parser.add_argument(
        "--hold-s",
        type=float,
        default=1.0,
        help="Seconds to wait after connecting before disconnecting.",
    )
    auto_connect_disconnect_parser.set_defaults(func=run_auto_connect_disconnect)

    identify_parser = subparsers.add_parser(
        "identify",
        help="Connect explicit sensor addresses, send identify, then disconnect.",
    )
    identify_parser.add_argument(
        "--address",
        action="append",
        required=True,
        help="BLE address to identify. Repeat for multiple sensors.",
    )
    identify_parser.add_argument("--timeout-ms", type=int, default=5000)
    identify_parser.add_argument(
        "--skip-discover",
        action="store_true",
        help="Skip the pre-connect discovery pass.",
    )
    add_connect_retry_args(identify_parser)
    identify_parser.add_argument("--read-timeout-s", type=float, default=15.0)
    identify_parser.add_argument("--write-timeout-s", type=float, default=15.0)
    identify_parser.add_argument("--disconnect-timeout-s", type=float, default=30.0)
    identify_parser.add_argument("--hold-s", type=float, default=10.0)
    identify_parser.add_argument("--pre-identify-delay-s", type=float, default=2.0)
    identify_parser.add_argument("--identify-retry-attempts", type=int, default=1)
    identify_parser.add_argument("--identify-retry-delay-s", type=float, default=1.0)
    identify_parser.add_argument(
        "--without-response",
        action="store_true",
        help="Use write without response for the identify command.",
    )
    identify_parser.set_defaults(func=run_identify)

    auto_identify_parser = subparsers.add_parser(
        "auto-identify",
        help="Scan for Movella DOT sensors, connect the first N matches, send identify, then disconnect.",
    )
    auto_identify_parser.add_argument("--count", type=int, required=True)
    auto_identify_parser.add_argument("--timeout-ms", type=int, default=5000)
    add_connect_retry_args(auto_identify_parser)
    auto_identify_parser.add_argument("--read-timeout-s", type=float, default=15.0)
    auto_identify_parser.add_argument("--write-timeout-s", type=float, default=15.0)
    auto_identify_parser.add_argument("--disconnect-timeout-s", type=float, default=30.0)
    auto_identify_parser.add_argument("--hold-s", type=float, default=10.0)
    auto_identify_parser.add_argument("--pre-identify-delay-s", type=float, default=2.0)
    auto_identify_parser.add_argument("--identify-retry-attempts", type=int, default=1)
    auto_identify_parser.add_argument("--identify-retry-delay-s", type=float, default=1.0)
    auto_identify_parser.add_argument(
        "--without-response",
        action="store_true",
        help="Use write without response for the identify command.",
    )
    auto_identify_parser.set_defaults(func=run_auto_identify)

    battery_parser = subparsers.add_parser(
    "battery",
    help="Connect explicit sensor addresses, subscribe to battery notifications, read initial battery, then listen.",
)
    battery_parser.add_argument(
        "--address",
        action="append",
        default=[],
        help="BLE address to test battery for. Repeat for multiple sensors.",
    )
    battery_parser.add_argument(
        "--count",
        type=int,
        default=None,
        help="Scan for Movella DOT sensors and test the first N matches.",
    )
    battery_parser.add_argument("--timeout-ms", type=int, default=5000)
    battery_parser.add_argument(
        "--skip-discover",
        action="store_true",
        help="Skip the pre-connect discovery pass.",
    )
    add_connect_retry_args(battery_parser)
    battery_parser.add_argument(
        "--subscribe-timeout-s",
        type=float,
        default=10.0,
        help="Seconds to wait for subscribe_complete.",
    )
    battery_parser.add_argument(
        "--read-timeout-s",
        type=float,
        default=10.0,
        help="Seconds to wait for the initial battery read.",
    )
    battery_parser.add_argument(
        "--listen-s",
        type=float,
        default=30.0,
        help="Seconds to listen for battery notification events.",
    )
    battery_parser.add_argument(
        "--disconnect-timeout-s",
        type=float,
        default=30.0,
        help="Seconds to wait for disconnect confirmation.",
    )
    battery_parser.add_argument(
        "--disconnect",
        action="store_true",
        help="Disconnect after the battery test finishes.",
    )
    battery_parser.set_defaults(func=run_battery)

    args = parser.parse_args()
    if args.command == "battery":
        if args.count is None and not args.address:
            parser.error("battery requires either --count N or at least one --address")
        if args.count is not None and args.address:
            parser.error("battery accepts either --count N or --address, not both")
        if args.count is not None and args.skip_discover:
            parser.error("battery --count requires discovery, so do not use --skip-discover")
    try:
        args.func(args)
    except (TimeoutError, RuntimeError) as exc:
        print(f"ERROR: {exc}")
        raise SystemExit(1)


if __name__ == "__main__":
    main()

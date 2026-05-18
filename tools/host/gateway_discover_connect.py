#!/usr/bin/env python3

import argparse
import json
import time
from typing import List, Tuple

import serial


DEFAULT_PORT = "/dev/serial/by-id/usb-SEGGER_J-Link_001057755524-if02"
BAUD = 115200
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

        start = line.find("{")
        end = line.rfind("}")
        if start == -1 or end == -1 or end <= start:
            continue

        try:
            return json.loads(line[start : end + 1])
        except json.JSONDecodeError:
            continue

    raise TimeoutError("Timed out waiting for JSON")


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
    time.sleep(0.3)
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
    print("discover_movella: Starting scan...")
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
            print(msg)
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


def connect_addresses(ser, addresses, timeout_s):
    request_id = f"connect_{int(time.time() * 1000)}"
    pending = list(addresses)
    connected = []

    send_jsonl(
        ser,
        {
            "type": "connect_addresses",
            "request_id": request_id,
            "addresses": pending,
        },
    )

    deadline = time.time() + timeout_s
    while time.time() < deadline and pending:
        msg = read_json_any(ser, timeout_s=max(0.1, deadline - time.time()))
        msg_type = msg.get("type")

        if msg_type == "sensor_connected" and msg.get("request_id") == request_id:
            address = msg.get("address")
            if address in pending:
                pending.remove(address)
                connected.append(address)
                print(f"CONNECTED: {address}")
            continue

        if msg_type == "error" and msg.get("request_id") == request_id:
            raise RuntimeError(
                f"Gateway error {msg.get('error_code')}: {msg.get('message')}"
            )

    if pending:
        raise TimeoutError(f"Timed out waiting for connections: {', '.join(pending)}")

    return connected


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
        connected = connect_addresses(ser, args.address, timeout_s=args.timeout_s)
        print(json.dumps({"connected": connected}, indent=2))


def run_auto_connect(args):
    with open_gateway_serial(args.port) as ser:
        command_hello(ser)
        matches = discover_movella(ser, timeout_ms=args.timeout_ms)
        if len(matches) < args.count:
            raise RuntimeError(
                f"Requested {args.count} Movella DOT sensors, found {len(matches)}"
            )

        selected = [entry["address"] for entry in matches[: args.count]]
        print(f"AUTO-CONNECT addresses: {selected}")
        connected = connect_addresses(ser, selected, timeout_s=args.connect_timeout_s)
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
    connect_parser.add_argument("--timeout-s", type=float, default=30.0)
    connect_parser.set_defaults(func=run_connect)

    auto_connect_parser = subparsers.add_parser(
        "auto-connect",
        help="Scan for Movella DOT sensors and connect the first N matches.",
    )
    auto_connect_parser.add_argument("--count", type=int, required=True)
    auto_connect_parser.add_argument("--timeout-ms", type=int, default=5000)
    auto_connect_parser.add_argument("--connect-timeout-s", type=float, default=30.0)
    auto_connect_parser.set_defaults(func=run_auto_connect)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

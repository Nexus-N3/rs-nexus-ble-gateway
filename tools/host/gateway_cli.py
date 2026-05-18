#!/usr/bin/env python3

import argparse
import json
import time
import serial


DEFAULT_PORT = "/dev/serial/by-id/usb-SEGGER_J-Link_001057755524-if02"
BAUD = 115200


def send_jsonl(ser, obj):
    line = json.dumps(obj, separators=(",", ":")) + "\n"
    print("HOST ->", line.strip())
    ser.write(line.encode("utf-8"))
    ser.flush()

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

        json_text = line[start : end + 1]

        try:
            return json.loads(line)
        except json.JSONDecodeError:
            continue

    raise TimeoutError("Timed out waiting for JSON")


def read_json_until(ser, wanted_type, request_id=None, timeout_s=10):
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

        json_text = line[start : end + 1]

        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

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
    request_id = "hello_001"

    send_jsonl(
        ser,
        {
            "type": "hello",
            "request_id": request_id,
            "protocol_version": 1,
            "client": "rs-nexus-os",
        },
    )

    msg = read_json_until(
        ser,
        wanted_type="hello_ack",
        request_id=request_id,
        timeout_s=10,
    )

    print("PASS: hello")
    print(json.dumps(msg, indent=2))


def command_status(ser):
    request_id = "status_001"

    send_jsonl(
        ser,
        {
            "type": "get_status",
            "request_id": request_id,
        },
    )

    msg = read_json_until(
        ser,
        wanted_type="status",
        request_id=request_id,
        timeout_s=10,
    )

    print("PASS: get_status")
    print(json.dumps(msg, indent=2))

def command_scan(ser):
    request_id = "scan_001"

    send_jsonl(
        ser,
        {
            "type": "scan_start",
            "request_id": request_id,
            "timeout_ms": 5000,
        },
    )

    while True:
        msg = read_json_any(ser, timeout_s=10)
        msg_type = msg.get("type")

        if msg_type == "scan_result":
            print("SCAN:")
            print(json.dumps(msg, indent=2))
            continue

        if msg_type == "scan_complete":
            print("PASS: scan_complete")
            print(json.dumps(msg, indent=2))
            return

        print("Ignoring JSON message:")
        print(json.dumps(msg, indent=2))

def main():
    parser = argparse.ArgumentParser(
        description="Test rs-nexus-ble-gateway JSON-lines UART transport."
    )
    parser.add_argument(
        "command",
        nargs="?",
        default="hello",
        choices=["hello", "status", "scan", "all"],
        help="Command to run.",
    )
    parser.add_argument(
        "--port",
        default=DEFAULT_PORT,
        help="Serial port path.",
    )

    args = parser.parse_args()

    with open_gateway_serial(args.port) as ser:
        if args.command == "hello":
            command_hello(ser)
        elif args.command == "status":
            command_status(ser)
        elif args.command == "scan":
            command_scan(ser)
        elif args.command == "all":
            command_hello(ser)
            command_status(ser)
            command_scan(ser)


if __name__ == "__main__":
    main()
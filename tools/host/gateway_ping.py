#!/usr/bin/env python3

import json
import sys
import time
import serial


PORT = "/dev/serial/by-id/usb-SEGGER_J-Link_001057755524-if02"
BAUD = 1000000


def send_jsonl(ser, obj):
    line = json.dumps(obj, separators=(",", ":")) + "\n"
    print("HOST ->", line.strip())
    ser.write(line.encode("utf-8"))
    ser.flush()


def read_lines_until(ser, wanted_type, timeout_s=10):
    deadline = time.time() + timeout_s
    line_buf = bytearray()
    sent_hello = False

    while time.time() < deadline:
        b = ser.read(1)

        if not b:
            if not sent_hello:
                send_jsonl(
                    ser,
                    {
                        "type": "hello",
                        "request_id": "hello_001",
                        "protocol_version": 1,
                        "client": "rs-nexus-os",
                    },
                )
                sent_hello = True
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

        if line.startswith("{"):
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue

            if msg.get("type") == wanted_type:
                return msg

            # Once the board says ready, send hello immediately.
            if msg.get("type") == "ready" and not sent_hello:
                send_jsonl(
                    ser,
                    {
                        "type": "hello",
                        "request_id": "hello_001",
                        "protocol_version": 1,
                        "client": "rs-nexus-os",
                    },
                )
                sent_hello = True

    raise TimeoutError(f"Timed out waiting for {wanted_type}")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else PORT

    with serial.Serial(
        port=port,
        baudrate=BAUD,
        timeout=0.1,
        write_timeout=1.0,
        dsrdtr=False,
        rtscts=False,
    ) as ser:
        ser.setDTR(True)
        ser.setRTS(True)

        time.sleep(0.5)

        # Ask once immediately too, in case the ready line already passed.
        send_jsonl(
            ser,
            {
                "type": "hello",
                "request_id": "hello_001",
                "protocol_version": 1,
                "client": "rs-nexus-os",
            },
        )

        msg = read_lines_until(ser, "hello_ack", timeout_s=10)

        print("PASS: gateway UART transport is alive")
        print(json.dumps(msg, indent=2))


if __name__ == "__main__":
    main()

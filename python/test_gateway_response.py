import glob
import json
import time
import serial

ports = sorted(glob.glob("/dev/serial/by-id/*-if01*"))

if len(ports) != 1:
    raise SystemExit(f"Expected one gateway data port; found: {ports}")

with serial.Serial(ports[0], 1_000_000, timeout=0.5) as ser:
    ser.setDTR(True)

    command = {
        "type": "hello",
        "request_id": "burst-check"
    }

    ser.write((json.dumps(command) + "\n").encode())

    deadline = time.monotonic() + 5

    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue

        print(raw.decode(errors="replace").strip())

        try:
            response = json.loads(raw)
        except (ValueError, UnicodeDecodeError):
            continue

        if response.get("type") == "hello_ack":
            print("Gateway communication verified")
            break
    else:
        print("No hello acknowledgment received")
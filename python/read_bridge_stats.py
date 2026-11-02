
import glob
import time
from pathlib import Path
from datetime import datetime, timezone

import serial

data_ports = sorted(glob.glob("/dev/serial/by-id/*-if01"))

if len(data_ports) != 1:
    raise SystemExit(f"Expected one IFMCU data port: {data_ports}")

shell_port = Path(data_ports[0][:-4] + "if03")

if not shell_port.exists():
    raise SystemExit(f"IFMCU shell not found: {shell_port}")


def read_prompt(port, timeout_s=2.5):
    received = bytearray()
    deadline = time.monotonic() + timeout_s

    while time.monotonic() < deadline:
        received.extend(port.read(256))
        if b"ifsh:~$" in received:
            break

    return received.decode("utf-8", errors="replace")


with serial.Serial(
    str(shell_port),
    baudrate=115200,
    timeout=0.05,
    exclusive=True,
) as port:
    port.reset_input_buffer()

    # Synchronize with the IFMCU shell.
    port.write(b"\r")
    prompt = read_prompt(port)

    if "ifsh:~$" not in prompt:
        raise SystemExit("IFMCU shell prompt not received")

    port.write(b"bridge stats\r")
    response = read_prompt(port)

    print("Captured:", datetime.now(timezone.utc).isoformat())
    print(response)

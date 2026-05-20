from __future__ import annotations

import json
import time
from contextlib import contextmanager
from typing import Iterator

import serial


DEFAULT_PORT = "/dev/serial/by-id/usb-SEGGER_J-Link_001057755524-if02"
DEFAULT_BAUD = 1_000_000
STREAM_FRAME_MAGIC = b"\xA5\x5A"


def json_objects_from_line(line: str):
    decoder = json.JSONDecoder()

    for index, character in enumerate(line):
        if character != "{":
            continue

        try:
            obj, _ = decoder.raw_decode(line[index:])
            yield obj
        except json.JSONDecodeError:
            continue


@contextmanager
def open_gateway_serial(
    port: str = DEFAULT_PORT,
    baudrate: int = DEFAULT_BAUD,
) -> Iterator[serial.Serial]:
    ser = serial.Serial(
        port=port,
        baudrate=baudrate,
        timeout=0.1,
        write_timeout=1.0,
        dsrdtr=False,
        rtscts=False,
    )
    try:
        ser.setDTR(True)
        ser.setRTS(True)
        time.sleep(0.5)
        ser.reset_input_buffer()
        yield ser
    finally:
        ser.close()

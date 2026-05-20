from __future__ import annotations

import struct


MOVELLA_NAME = "Movella DOT"
MOVELLA_DEVICE_CONTROL_UUID = "15171002-4947-11e9-8646-d663bd873d93"
MOVELLA_START_STOP_STREAM_UUID = "15172001-4947-11e9-8646-d663bd873d93"
MOVELLA_LONG_PAYLOAD_UUID = "15172002-4947-11e9-8646-d663bd873d93"
MOVELLA_SET_RATE_HEX = {
    20: "100000000000000B4D6F76656C6C6120444F5400000000001400000000000000",
    60: "100000000000000B4D6F76656C6C6120444F5400000000003C00000000000000",
}
MOVELLA_START_HEX = "01011A"
MOVELLA_STOP_HEX = "01001A"
MOVELLA_MIN_PACKET_LEN = 4
DEFAULT_STARTUP_GATE = {
    "enabled": True,
    "stability_window_seconds": 5.0,
    "packets_required": 60,
    "min_rate_hz": 58.0,
    "min_observation_seconds": 2.0,
    "max_gap_events": 0,
    "gap_grace_seconds": 2.0,
}
DEFAULT_LOCATIONS = [
    "LEFT_ANKLE",
    "RIGHT_ANKLE",
    "LEFT_THIGH",
    "RIGHT_THIGH",
    "CHEST",
    "LOWER_BACK",
    "HEAD",
    "UPPER_BACK",
    "LEFT_WRIST",
    "RIGHT_WRIST",
]


def parse_sensor_timestamp(payload: bytes) -> int:
    if len(payload) < MOVELLA_MIN_PACKET_LEN:
        raise ValueError(f"Movella payload too short: {len(payload)} bytes")
    return int(struct.unpack_from("<I", payload, 0)[0])


def select_addresses(matches, count: int) -> list[str]:
    if len(matches) < count:
        raise RuntimeError(f"Requested {count} Movella DOT sensors, found {len(matches)}")
    return [entry.address for entry in matches[:count]]

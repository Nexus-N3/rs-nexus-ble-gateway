from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class StreamFrame:
    sensor_id: int
    gateway_timestamp_us: int
    payload: bytes


@dataclass(frozen=True)
class DiscoveredDevice:
    address: str
    name: str = ""
    rssi: int | None = None
    service_uuids: tuple[str, ...] = ()
    raw: dict = field(default_factory=dict)


@dataclass(frozen=True)
class SensorConnection:
    address: str
    sensor_id: int | None = None

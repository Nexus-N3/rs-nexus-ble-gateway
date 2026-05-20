from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from typing import Callable

from .models import SensorConnection, StreamFrame


@dataclass(frozen=True)
class StartupGateConfig:
    enabled: bool = True
    stability_window_seconds: float = 5.0
    packets_required: int = 60
    min_rate_hz: float = 58.0
    min_observation_seconds: float = 2.0
    max_gap_events: int = 0
    gap_grace_seconds: float = 2.0


@dataclass
class SensorStreamStats:
    address: str
    sensor_id: int | None
    label: str | None
    expected_rate_hz: int
    stream_start_command_time: float | None = None
    first_packet_time: float | None = None

    startup_first_sensor_timestamp: int | None = None
    startup_last_sensor_timestamp: int | None = None
    startup_packets_received: int = 0
    startup_gap_events: int = 0
    startup_estimated_dropped_packets: int = 0
    startup_gap_detection_start_sensor_timestamp: int | None = None
    startup_gate_first_sensor_timestamp: int | None = None
    startup_gate_last_sensor_timestamp: int | None = None
    startup_gate_packets_received: int = 0
    startup_gate_gap_events: int = 0
    startup_gate_estimated_dropped_packets: int = 0

    measurement_first_sensor_timestamp: int | None = None
    measurement_last_sensor_timestamp: int | None = None
    measurement_packets_received: int = 0
    measurement_gap_events: int = 0
    measurement_estimated_dropped_packets: int = 0
    host_parsed_frames: int = 0

    @property
    def expected_delta_us(self) -> float:
        return 1_000_000.0 / float(self.expected_rate_hz)

    @property
    def startup_duration_seconds(self) -> float:
        if self.startup_first_sensor_timestamp is None or self.startup_last_sensor_timestamp is None:
            return 0.0
        return max(
            (self.startup_last_sensor_timestamp - self.startup_first_sensor_timestamp) / 1_000_000.0,
            0.0,
        )

    @property
    def startup_observed_rate_hz(self) -> float:
        duration = self.startup_duration_seconds
        return 0.0 if duration <= 0 or self.startup_packets_received < 2 else (self.startup_packets_received - 1) / duration

    @property
    def startup_gate_duration_seconds(self) -> float:
        if self.startup_gate_first_sensor_timestamp is None or self.startup_gate_last_sensor_timestamp is None:
            return 0.0
        return max(
            (self.startup_gate_last_sensor_timestamp - self.startup_gate_first_sensor_timestamp) / 1_000_000.0,
            0.0,
        )

    @property
    def startup_gate_rate_hz(self) -> float:
        duration = self.startup_gate_duration_seconds
        return 0.0 if duration <= 0 or self.startup_gate_packets_received < 2 else (self.startup_gate_packets_received - 1) / duration

    @property
    def measurement_duration_seconds(self) -> float:
        if self.measurement_first_sensor_timestamp is None or self.measurement_last_sensor_timestamp is None:
            return 0.0
        return max(
            (self.measurement_last_sensor_timestamp - self.measurement_first_sensor_timestamp) / 1_000_000.0,
            0.0,
        )

    @property
    def observed_rate_hz(self) -> float:
        duration = self.measurement_duration_seconds
        return 0.0 if duration <= 0 or self.measurement_packets_received < 2 else (self.measurement_packets_received - 1) / duration

    @property
    def time_to_first_packet_ms(self) -> float | None:
        if self.stream_start_command_time is None or self.first_packet_time is None:
            return None
        return max((self.first_packet_time - self.stream_start_command_time) * 1000.0, 0.0)

    def record_sample(
        self,
        timestamp: int | None,
        wall_time: float,
        *,
        measurement_active: bool,
        startup_gap_grace_seconds: float,
    ):
        if self.first_packet_time is None:
            self.first_packet_time = wall_time

        if measurement_active:
            self._record_measurement_sample(timestamp)
        else:
            self._record_startup_sample(timestamp, startup_gap_grace_seconds)

        self.host_parsed_frames += 1

    def _record_startup_sample(self, timestamp: int | None, startup_gap_grace_seconds: float):
        if self.startup_first_sensor_timestamp is None:
            self.startup_first_sensor_timestamp = timestamp
            if timestamp is not None:
                self.startup_gap_detection_start_sensor_timestamp = (
                    timestamp + int(startup_gap_grace_seconds * 1_000_000.0)
                )
        else:
            self._record_gap_if_needed(
                timestamp=timestamp,
                previous_timestamp=self.startup_last_sensor_timestamp,
                mode="startup",
            )

        self.startup_last_sensor_timestamp = timestamp
        self.startup_packets_received += 1

        if (
            timestamp is not None
            and self.startup_gap_detection_start_sensor_timestamp is not None
            and timestamp >= self.startup_gap_detection_start_sensor_timestamp
        ):
            if self.startup_gate_first_sensor_timestamp is None:
                self.startup_gate_first_sensor_timestamp = timestamp
            else:
                self._record_gap_if_needed(
                    timestamp=timestamp,
                    previous_timestamp=self.startup_gate_last_sensor_timestamp,
                    mode="startup_gate",
                )

            self.startup_gate_last_sensor_timestamp = timestamp
            self.startup_gate_packets_received += 1

    def _record_measurement_sample(self, timestamp: int | None):
        if self.measurement_first_sensor_timestamp is None:
            self.measurement_first_sensor_timestamp = timestamp
        else:
            self._record_gap_if_needed(
                timestamp=timestamp,
                previous_timestamp=self.measurement_last_sensor_timestamp,
                mode="measurement",
            )

        self.measurement_last_sensor_timestamp = timestamp
        self.measurement_packets_received += 1

    def _record_gap_if_needed(self, timestamp: int | None, previous_timestamp: int | None, mode: str):
        if timestamp is None or previous_timestamp is None:
            return

        observed_delta_us = timestamp - previous_timestamp
        if observed_delta_us <= int(self.expected_delta_us * 1.5):
            return

        missing_packets = max(int(round(observed_delta_us / self.expected_delta_us)) - 1, 0)
        if missing_packets <= 0:
            return

        if mode == "startup":
            self.startup_gap_events += 1
            self.startup_estimated_dropped_packets += missing_packets
        elif mode == "startup_gate":
            self.startup_gate_gap_events += 1
            self.startup_gate_estimated_dropped_packets += missing_packets
        else:
            self.measurement_gap_events += 1
            self.measurement_estimated_dropped_packets += missing_packets

    def reset_measurement(self):
        self.measurement_first_sensor_timestamp = None
        self.measurement_last_sensor_timestamp = None
        self.measurement_packets_received = 0
        self.measurement_gap_events = 0
        self.measurement_estimated_dropped_packets = 0


class GenericStreamMonitor:
    def __init__(
        self,
        *,
        connections: list[SensorConnection],
        labels_by_address: dict[str, str | None],
        expected_rate_hz: int,
        timestamp_parser: Callable[[bytes], int],
        startup_gate: StartupGateConfig,
        verbose: bool = True,
    ):
        self.timestamp_parser = timestamp_parser
        self.startup_gate = startup_gate
        self.verbose = verbose
        self.measurement_active = not startup_gate.enabled
        self.stream_frames_seen = 0
        self.stream_frames_unknown_sensor_id = 0
        self.unknown_sensor_ids: Counter[int] = Counter()
        self.address_by_sensor_id: dict[int, str] = {}
        self.stats_by_address: dict[str, SensorStreamStats] = {}

        for connection in connections:
            if connection.sensor_id is not None:
                self.address_by_sensor_id[connection.sensor_id] = connection.address
            self.stats_by_address[connection.address] = SensorStreamStats(
                address=connection.address,
                sensor_id=connection.sensor_id,
                label=labels_by_address.get(connection.address),
                expected_rate_hz=expected_rate_hz,
            )

        if self.startup_gate.enabled:
            self._log(
                "Waiting for startup stability gate: "
                f"up to {self.startup_gate.stability_window_seconds:.1f}s."
            )
        else:
            self._log("Startup gate disabled. Official measurement is active immediately.")

    def mark_stream_started(self, address: str, command_time: float | None):
        if address in self.stats_by_address:
            self.stats_by_address[address].stream_start_command_time = command_time

    def handle_stream_frame(self, frame: StreamFrame, wall_time: float):
        self.stream_frames_seen += 1
        address = self.address_by_sensor_id.get(frame.sensor_id)
        if address not in self.stats_by_address:
            self.stream_frames_unknown_sensor_id += 1
            self.unknown_sensor_ids[frame.sensor_id] += 1
            return

        timestamp = self.timestamp_parser(frame.payload)
        self.stats_by_address[address].record_sample(
            timestamp,
            wall_time,
            measurement_active=self.measurement_active,
            startup_gap_grace_seconds=self.startup_gate.gap_grace_seconds,
        )

        if self.startup_gate.enabled and not self.measurement_active:
            stable, _ = self.evaluate_startup_stability()
            if stable:
                self.activate_measurement()

    def evaluate_startup_stability(self) -> tuple[bool, list[str]]:
        unstable: list[str] = []
        for address in sorted(self.stats_by_address):
            stats = self.stats_by_address[address]
            if stats.first_packet_time is None:
                unstable.append(f"{address}: no_first_packet")
            elif stats.startup_gate_packets_received < self.startup_gate.packets_required:
                unstable.append(f"{address}: packets={stats.startup_gate_packets_received}")
            elif stats.startup_gate_duration_seconds < self.startup_gate.min_observation_seconds:
                unstable.append(f"{address}: warmup_window={stats.startup_gate_duration_seconds:.2f}s")
            elif stats.startup_gate_rate_hz < self.startup_gate.min_rate_hz:
                unstable.append(f"{address}: rate={stats.startup_gate_rate_hz:.2f}Hz")
            elif stats.startup_gate_gap_events > self.startup_gate.max_gap_events:
                unstable.append(
                    f"{address}: startup_gap_events={stats.startup_gate_gap_events} "
                    f"startup_drops={stats.startup_gate_estimated_dropped_packets}"
                )
        return len(unstable) == 0, unstable

    def activate_measurement(self):
        for stats in self.stats_by_address.values():
            stats.reset_measurement()
        self.measurement_active = True
        self._log("Startup stability gate passed. Official measurement is now active.")

    def _log(self, message: str):
        if self.verbose:
            print(message)

    def summary_lines(self, gateway_client) -> list[str]:
        lines = [
            "Stream summary",
            (
                f"measurement_active={int(self.measurement_active)} "
                f"stream_frames_seen={self.stream_frames_seen} "
                f"unknown_sensor_id_frames={self.stream_frames_unknown_sensor_id} "
                f"unknown_sensor_ids={dict(self.unknown_sensor_ids)}"
            ),
            (
                "Host parser summary "
                f"checksum_failures={gateway_client.stream_checksum_failures} "
                f"resync_events={gateway_client.stream_resync_events} "
                f"resync_drop_bytes={gateway_client.stream_resync_drop_bytes} "
                f"partial_json_waits={gateway_client.stream_partial_json_waits} "
                f"partial_frame_waits={gateway_client.stream_partial_frame_waits}"
            ),
        ]

        if gateway_client.gateway_transport_stats:
            transport = gateway_client.gateway_transport_stats
            lines.append(
                "Gateway transport summary "
                f"stream_ring_bytes={transport.get('stream_ring_bytes')} "
                f"stream_ring_drops={transport.get('stream_ring_drops')} "
                f"stream_enqueue_success={transport.get('stream_enqueue_success')} "
                f"stream_enqueue_drops={transport.get('stream_enqueue_drops')} "
                f"stream_tx_done={transport.get('stream_tx_done')} "
                f"stream_tx_aborted={transport.get('stream_tx_aborted')} "
                f"stream_tx_start_failures={transport.get('stream_tx_start_failures')}"
            )

        for address in sorted(self.stats_by_address):
            stats = self.stats_by_address[address]
            gateway_stats = gateway_client.gateway_ble_rx_stats.get(
                gateway_client._normalize_address(address),
                {},
            )
            ttff = "n/a" if stats.time_to_first_packet_ms is None else f"{stats.time_to_first_packet_ms:.1f}"
            lines.append(
                f"{address} label={stats.label} "
                f"sensor_id={stats.sensor_id} "
                f"startup_packets={stats.startup_packets_received} "
                f"startup_rate_hz={stats.startup_observed_rate_hz:.2f} "
                f"startup_gap_events={stats.startup_gap_events} "
                f"startup_drops={stats.startup_estimated_dropped_packets} "
                f"startup_gate_packets={stats.startup_gate_packets_received} "
                f"startup_gate_rate_hz={stats.startup_gate_rate_hz:.2f} "
                f"startup_gate_gap_events={stats.startup_gate_gap_events} "
                f"startup_gate_drops={stats.startup_gate_estimated_dropped_packets} "
                f"time_to_first_packet_ms={ttff} "
                f"packets={stats.measurement_packets_received} "
                f"host_parsed_frames={stats.host_parsed_frames} "
                f"observed_rate_hz={stats.observed_rate_hz:.2f} "
                f"expected_rate_hz={stats.expected_rate_hz} "
                f"gap_events={stats.measurement_gap_events} "
                f"estimated_dropped_packets={stats.measurement_estimated_dropped_packets} "
                f"gateway_timestamp_resets={gateway_stats.get('timestamp_reset_events')} "
                f"gateway_timestamp_discontinuities={gateway_stats.get('timestamp_discontinuity_events')} "
                f"gateway_lookup_misses={gateway_stats.get('subscription_lookup_misses')} "
                f"gateway_json_fallbacks={gateway_stats.get('json_fallback_notifications')} "
                f"gateway_queue_accepted={gateway_stats.get('notification_queue_accepted')} "
                f"gateway_queue_dropped={gateway_stats.get('notification_queue_dropped')} "
                f"gateway_queue_flushed={gateway_stats.get('notification_queue_flushed')} "
                f"gateway_stream_enqueue_success={gateway_stats.get('stream_enqueue_success')} "
                f"gateway_stream_enqueue_dropped={gateway_stats.get('stream_enqueue_dropped')}"
            )

        return lines

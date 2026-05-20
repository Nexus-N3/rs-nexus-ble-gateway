from __future__ import annotations

import json
import time
from typing import Any

from .models import DiscoveredDevice, SensorConnection, StreamFrame
from .transport import STREAM_FRAME_MAGIC, json_objects_from_line


class GatewayClient:
    def __init__(self, ser, *, client_name: str = "nexus_ble_sdk", verbose: bool = True):
        self.ser = ser
        self.client_name = client_name
        self.verbose = verbose
        self.buf = bytearray()
        self.cached_json: list[dict[str, Any]] = []
        self.cached_stream_frames: list[StreamFrame] = []
        self.disconnected_addresses: set[str] = set()
        self.notification_drop_count: int = 0
        self.gateway_transport_stats: dict[str, Any] = {}
        self.gateway_ble_rx_stats: dict[str, dict[str, Any]] = {}
        self.stream_checksum_failures: int = 0
        self.stream_resync_drop_bytes: int = 0
        self.stream_resync_events: int = 0
        self.stream_partial_json_waits: int = 0
        self.stream_partial_frame_waits: int = 0
        self._partial_block_kind: str | None = None
        self._partial_block_len: int = -1
        self.phase = "idle"

    def request_id(self, prefix: str) -> str:
        return f"{prefix}_{int(time.time() * 1000)}"

    def send(self, obj: dict[str, Any]):
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        self.ser.write(line.encode("utf-8"))
        self.ser.flush()

    def hello(self, protocol_version: int = 1):
        request_id = "hello_host_tool"
        self.send(
            {
                "type": "hello",
                "request_id": request_id,
                "protocol_version": protocol_version,
                "client": self.client_name,
            }
        )
        self.wait_for_request(request_id, "hello_ack", timeout_s=5.0)

    def reset_session(self, timeout_s: float = 5.0):
        request_id = self.request_id("reset")
        self.send({"type": "reset_session", "request_id": request_id})
        self.wait_for_request(request_id, "reset_session_complete", timeout_s)

    def scan(self, timeout_ms: int, *, name_filter: str | None = None) -> list[DiscoveredDevice]:
        request_id = self.request_id("scan")
        matches: dict[str, DiscoveredDevice] = {}
        self._log(f"Scanning for up to {timeout_ms}ms...")
        self.send({"type": "scan_start", "request_id": request_id, "timeout_ms": timeout_ms})

        while True:
            msg = self.read_json(timeout_s=max(10.0, timeout_ms / 1000.0 + 5.0))
            msg_type = msg.get("type")

            if msg_type == "scan_result" and msg.get("request_id") == request_id:
                name = str(msg.get("name", ""))
                if name_filter is not None and name != name_filter:
                    continue

                address = str(msg.get("address", ""))
                if not address or address in matches:
                    continue

                service_uuids = tuple(
                    str(value).lower()
                    for value in msg.get("service_uuids", [])
                    if isinstance(value, str)
                )
                matches[address] = DiscoveredDevice(
                    address=address,
                    name=name,
                    rssi=msg.get("rssi"),
                    service_uuids=service_uuids,
                    raw=dict(msg),
                )
                self._log(f"SCAN MATCH: {address} name={name} rssi={msg.get('rssi')}")
                continue

            if msg_type == "scan_complete" and msg.get("request_id") == request_id:
                self._log(f"SCAN COMPLETE: {len(matches)} device(s) matched")
                return list(matches.values())

    def connect(self, addresses: list[str], timeout_s: float) -> list[SensorConnection]:
        request_id = self.request_id("connect")
        pending = list(addresses)
        connected: list[SensorConnection] = []

        self.send({"type": "connect_addresses", "request_id": request_id, "addresses": pending})
        deadline = time.time() + timeout_s

        while time.time() < deadline and pending:
            msg = self.read_json(timeout_s=max(0.1, deadline - time.time()))
            msg_type = msg.get("type")

            if msg_type == "sensor_connected":
                address = msg.get("address")
                if address in pending:
                    pending.remove(address)
                    self._log(f"CONNECTED: {address}")
                    connected.append(
                        SensorConnection(
                            address=address,
                            sensor_id=msg.get("sensor_id") if isinstance(msg.get("sensor_id"), int) else None,
                        )
                    )
                continue

            if msg_type == "sensor_disconnected":
                address = msg.get("address")
                if address in pending:
                    pending.remove(address)
                    self._log(f"CONNECT FAILED: {address} reason={msg.get('reason')}")
                continue

            if msg_type == "error" and msg.get("request_id") == request_id:
                raise RuntimeError(
                    f"Gateway connect failed: {msg.get('message')} ({msg.get('error_code')})"
                )

        if pending:
            raise TimeoutError("Failed to connect: " + ", ".join(pending))

        return connected

    def subscribe(
        self,
        address: str,
        characteristic_uuid: str,
        timeout_s: float,
        *,
        binary_notifications: bool = False,
    ):
        request_id = self.request_id("subscribe")
        self.send(
            {
                "type": "subscribe",
                "request_id": request_id,
                "address": address,
                "characteristic_uuid": characteristic_uuid,
                "binary_notifications": binary_notifications,
            }
        )
        self.wait_for_request(request_id, "subscribe_complete", timeout_s)

    def write_gatt(
        self,
        address: str,
        characteristic_uuid: str,
        payload_hex: str,
        timeout_s: float,
        *,
        without_response: bool = False,
        allow_timeout: bool = False,
    ) -> float | None:
        request_id = self.request_id("write")
        self.send(
            {
                "type": "gatt_write",
                "request_id": request_id,
                "address": address,
                "characteristic_uuid": characteristic_uuid,
                "payload_hex": payload_hex,
                "without_response": without_response,
            }
        )

        try:
            self.wait_for_request(request_id, "write_complete", timeout_s)
            return time.monotonic()
        except TimeoutError:
            if allow_timeout:
                return None
            raise

    def disconnect(
        self,
        addresses: list[str],
        timeout_s: float,
        *,
        allow_timeout: bool = False,
    ) -> list[str]:
        request_id = self.request_id("disconnect")
        pending = [address for address in addresses if address not in self.disconnected_addresses]
        disconnected = [address for address in addresses if address in self.disconnected_addresses]

        if not pending:
            return disconnected

        self.send({"type": "disconnect_addresses", "request_id": request_id, "addresses": pending})
        deadline = time.time() + timeout_s

        while time.time() < deadline and pending:
            try:
                msg = self.read_json(timeout_s=max(0.1, deadline - time.time()))
            except TimeoutError:
                if allow_timeout:
                    return disconnected
                raise

            msg_type = msg.get("type")
            if msg_type == "sensor_disconnected":
                address = msg.get("address")
                if address in pending:
                    pending.remove(address)
                    disconnected.append(address)
                    self.disconnected_addresses.add(address)
                continue

            if msg_type == "error" and msg.get("request_id") == request_id:
                if msg.get("error_code") == -3:
                    break
                raise RuntimeError(
                    f"Gateway disconnect failed: {msg.get('message')} ({msg.get('error_code')})"
                )

        if pending and not allow_timeout:
            raise TimeoutError("Failed to disconnect: " + ", ".join(pending))

        return disconnected

    def get_status_snapshot(self, timeout_s: float = 10.0) -> dict[str, Any]:
        request_id = self.request_id("status")
        saw_status = False
        saw_transport_stats = False
        saw_ble_stats_complete = False
        self.gateway_transport_stats = {}
        self.gateway_ble_rx_stats = {}
        self.send({"type": "get_status", "request_id": request_id})

        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                msg = self.read_json(timeout_s=max(0.1, deadline - time.time()))
            except TimeoutError:
                continue

            msg_type = msg.get("type")
            if msg_type == "status" and msg.get("request_id") == request_id:
                saw_status = True
            elif msg_type == "gateway_transport_stats":
                saw_transport_stats = True
            elif msg_type == "ble_notification_rx_stats_complete" and msg.get("request_id") == request_id:
                saw_ble_stats_complete = True

            if saw_status and saw_transport_stats and saw_ble_stats_complete:
                return {
                    "transport": dict(self.gateway_transport_stats),
                    "ble_rx": dict(self.gateway_ble_rx_stats),
                }

        raise TimeoutError(
            "Timed out waiting for complete status snapshot: "
            f"saw_status={saw_status} "
            f"saw_transport_stats={saw_transport_stats} "
            f"saw_ble_stats_complete={saw_ble_stats_complete}"
        )

    def wait_for_request(self, request_id: str, success_type: str, timeout_s: float):
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            msg = self.read_json(timeout_s=max(0.1, deadline - time.time()))
            msg_type = msg.get("type")

            if msg_type == success_type and msg.get("request_id") == request_id:
                return msg

            if msg_type == "error" and msg.get("request_id") == request_id:
                raise RuntimeError(
                    f"Gateway command failed: {msg.get('message')} ({msg.get('error_code')})"
                )

            if msg_type not in {"gatt_debug"}:
                self.cached_json.append(msg)

        raise TimeoutError(f"Timed out waiting for {success_type} request_id={request_id}")

    def read_item(self, timeout_s: float = 10.0):
        if self.cached_stream_frames:
            return ("stream_frame", self.cached_stream_frames.pop(0))

        return self._read_uncached_item(timeout_s=timeout_s)

    def read_json(self, timeout_s: float = 10.0) -> dict[str, Any]:
        deadline = time.time() + timeout_s

        while time.time() < deadline:
            if self.cached_json:
                return self.cached_json.pop(0)

            item_type, payload = self._read_uncached_item(timeout_s=max(0.1, deadline - time.time()))
            if item_type == "json":
                return payload

            if item_type == "stream_frame":
                self.cached_stream_frames.append(payload)

        raise TimeoutError("Timed out waiting for JSON")

    def _read_uncached_item(self, timeout_s: float = 10.0):
        deadline = time.time() + timeout_s

        while time.time() < deadline:
            item = self._extract_item()
            if item is not None:
                item_type, payload = item
                if item_type == "json":
                    self._observe_json(payload)
                return item

            chunk = self.ser.read(256)
            if chunk:
                self.buf.extend(chunk)

        raise TimeoutError("Timed out waiting for gateway item")

    def _extract_item(self):
        while self.buf:
            if self.buf[0] == ord("{"):
                newline_index = self.buf.find(b"\n")
                if newline_index < 0:
                    self._record_partial_block("json")
                    return None

                line = self.buf[:newline_index].decode("utf-8", errors="replace").strip()
                del self.buf[: newline_index + 1]
                self._clear_partial_block()
                if not line:
                    continue
                for msg in json_objects_from_line(line):
                    return ("json", msg)
                continue

            if len(self.buf) >= 2 and self.buf[:2] == STREAM_FRAME_MAGIC:
                if len(self.buf) < 14:
                    self._record_partial_block("frame")
                    return None

                version = self.buf[2]
                if version != 0x01:
                    self._drop_and_resync(1)
                    continue

                sensor_id = self.buf[3]
                gateway_timestamp_us = int.from_bytes(self.buf[4:12], "little")
                payload_len = self.buf[12]
                total_len = 13 + payload_len + 1
                if len(self.buf) < total_len:
                    self._record_partial_block("frame")
                    return None

                payload = bytes(self.buf[13 : 13 + payload_len])
                checksum = self.buf[13 + payload_len]
                computed = sum(self.buf[2 : 13 + payload_len]) & 0xFF
                if checksum != computed:
                    self.stream_checksum_failures += 1
                    self._drop_and_resync(1)
                    continue

                del self.buf[:total_len]
                self._clear_partial_block()
                return (
                    "stream_frame",
                    StreamFrame(
                        sensor_id=sensor_id,
                        gateway_timestamp_us=gateway_timestamp_us,
                        payload=payload,
                    ),
                )

            next_json = self.buf.find(b"{")
            next_frame = self.buf.find(STREAM_FRAME_MAGIC)
            candidates = [index for index in (next_json, next_frame) if index >= 0]
            if not candidates:
                keep_len = 1 if self.buf[-1:] == STREAM_FRAME_MAGIC[:1] else 0
                self._drop_and_resync(len(self.buf) - keep_len)
                return None

            drop_len = min(candidates)
            if drop_len > 0:
                self._drop_and_resync(drop_len)
            else:
                self._clear_partial_block()

        return None

    def _observe_json(self, msg: dict[str, Any]):
        msg_type = msg.get("type")

        if msg_type == "sensor_disconnected":
            address = msg.get("address")
            if address:
                self.disconnected_addresses.add(address)
            self._log(
                "SENSOR DISCONNECTED: "
                f"{msg.get('address')} phase={self.phase} request_id={msg.get('request_id')} "
                f"reason={msg.get('reason')}"
            )
            return

        if msg_type == "notification_drops":
            value = msg.get("drop_count")
            if isinstance(value, int):
                self.notification_drop_count = value
            return

        if msg_type == "gateway_transport_stats":
            self.gateway_transport_stats = dict(msg)
            return

        if msg_type == "ble_notification_rx_stats":
            address = self._normalize_address(str(msg.get("address", "")))
            if address:
                normalized = dict(msg)
                normalized["address"] = address
                self.gateway_ble_rx_stats[address] = normalized

    def _drop_and_resync(self, drop_len: int):
        if drop_len <= 0:
            self._clear_partial_block()
            return

        self.stream_resync_drop_bytes += drop_len
        self.stream_resync_events += 1
        del self.buf[:drop_len]
        self._clear_partial_block()

    def _record_partial_block(self, kind: str):
        current_len = len(self.buf)
        if self._partial_block_kind == kind and self._partial_block_len == current_len:
            return

        self._partial_block_kind = kind
        self._partial_block_len = current_len

        if kind == "json":
            self.stream_partial_json_waits += 1
        elif kind == "frame":
            self.stream_partial_frame_waits += 1

    def _clear_partial_block(self):
        self._partial_block_kind = None
        self._partial_block_len = -1

    def _normalize_address(self, address: str | None) -> str:
        return "" if not address else address.strip().upper()

    def _log(self, message: str):
        if self.verbose:
            print(message)

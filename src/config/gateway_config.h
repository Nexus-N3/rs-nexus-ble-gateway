#pragma once

#define GATEWAY_MAX_SENSORS              10
#define GATEWAY_MAX_SENSOR_TYPES          4
#define GATEWAY_MAX_FRAME_PAYLOAD       244
#define GATEWAY_MAX_ADDRESS_LEN          18
#define GATEWAY_MAX_REQUEST_ID_LEN       64
#define GATEWAY_MAX_UUID_LEN             40
#define GATEWAY_MAX_SERVICE_UUIDS         8
#define GATEWAY_MAX_SENSOR_KEY_LEN       64
#define GATEWAY_MAX_SENSOR_NAME_LEN      32

#define GATEWAY_DEFAULT_SCAN_TIMEOUT_MS 30000
#define GATEWAY_DEFAULT_CONNECT_TIMEOUT_MS 15000
#define GATEWAY_DEFAULT_CONFIG_TIMEOUT_MS 10000
#define GATEWAY_DEFAULT_STREAM_RATE_HZ    60

#define GATEWAY_HEALTH_WINDOW_MS        3000
#define GATEWAY_MIN_HEALTH_RATE_HZ        58

#define GATEWAY_USE_JSON_PROTOCOL          1
#define GATEWAY_ENABLE_RAW_FRAME_FORWARD   1
#define GATEWAY_ENABLE_GATEWAY_TIMESTAMPS  1

/* Stream UART batches remain <= 258 bytes and contain only complete frames.
 * Wait after any TX completion before starting another STREAM batch. Control
 * bypasses this gate. 2 ms exceeds the IFMCU's 1 ms RX idle timeout; the existing
 * 1 ms main poll resumes pending streams without sleeps in callbacks.
 * Set to 0 for the unpaced A/B baseline. Physical UART stays at 1 Mbps.
 */
#ifndef GATEWAY_STREAM_TX_GAP_MS
#define GATEWAY_STREAM_TX_GAP_MS          2U
#endif
#define GATEWAY_STREAM_PRESSURE_BYTES 12288U /* 75% of the 16 KiB stream ring */

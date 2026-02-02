/**
 * @file accel_service.h
 * @brief Coin-Cell Wireless Accelerometer GATT Service
 *
 * Architecture: Rev 3 - Burst mode with 10-byte samples, 24 samples/packet
 */

#ifndef ACCEL_SERVICE_H_
#define ACCEL_SERVICE_H_

#include <zephyr/bluetooth/conn.h>
#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * GATT UUIDs
 *===========================================================================*/

/* Custom Service UUID: 12340000-1234-5678-9ABC-DEF012345678 */
#define ACCEL_SERVICE_UUID_VAL                                                 \
  BT_UUID_128_ENCODE(0x12340000, 0x1234, 0x5678, 0x9ABC, 0xDEF012345678)

/* Acceleration Data Characteristic UUID: 12340001-... (NOTIFY) */
#define ACCEL_DATA_CHAR_UUID_VAL                                               \
  BT_UUID_128_ENCODE(0x12340001, 0x1234, 0x5678, 0x9ABC, 0xDEF012345678)

/* Timestamp Characteristic UUID: 12340002-... (READ | NOTIFY) */
#define TIMESTAMP_CHAR_UUID_VAL                                                \
  BT_UUID_128_ENCODE(0x12340002, 0x1234, 0x5678, 0x9ABC, 0xDEF012345678)

/* Sampling Rate Characteristic UUID: 12340003-... (READ) */
#define SAMPLE_RATE_CHAR_UUID_VAL                                              \
  BT_UUID_128_ENCODE(0x12340003, 0x1234, 0x5678, 0x9ABC, 0xDEF012345678)

/* Sensor Metadata (TEDS) Characteristic UUID: 12340004-... (READ) */
#define SENSOR_META_CHAR_UUID_VAL                                              \
  BT_UUID_128_ENCODE(0x12340004, 0x1234, 0x5678, 0x9ABC, 0xDEF012345678)

/* Operating Mode Characteristic UUID: 12340005-... (READ | WRITE) */
#define OPERATING_MODE_CHAR_UUID_VAL                                           \
  BT_UUID_128_ENCODE(0x12340005, 0x1234, 0x5678, 0x9ABC, 0xDEF012345678)

#define ACCEL_SERVICE_UUID BT_UUID_DECLARE_128(ACCEL_SERVICE_UUID_VAL)
#define ACCEL_DATA_CHAR_UUID BT_UUID_DECLARE_128(ACCEL_DATA_CHAR_UUID_VAL)
#define TIMESTAMP_CHAR_UUID BT_UUID_DECLARE_128(TIMESTAMP_CHAR_UUID_VAL)
#define SAMPLE_RATE_CHAR_UUID BT_UUID_DECLARE_128(SAMPLE_RATE_CHAR_UUID_VAL)
#define SENSOR_META_CHAR_UUID BT_UUID_DECLARE_128(SENSOR_META_CHAR_UUID_VAL)
#define OPERATING_MODE_CHAR_UUID                                               \
  BT_UUID_DECLARE_128(OPERATING_MODE_CHAR_UUID_VAL)

/*============================================================================
 * Operating Modes
 *===========================================================================*/

typedef enum {
  MODE_COINCELL_BURST = 0x00, /* Default: ~1s latency, power-gated network */
  MODE_CONTINUOUS_LAB = 0x01  /* Lab: ≤40ms latency, network always on */
} operating_mode_t;

/*============================================================================
 * Sample Format (10 bytes, packed)
 *
 * Per architecture Rev 3:
 * - sample_counter: 16-bit, wraps at 65535 (~65s @ 1kHz)
 * - rel_timestamp_ms: 16-bit, ms from burst start, no wrap in 1s
 * - accel_xyz: 6 bytes, signed raw counts (±16g range)
 *===========================================================================*/

typedef struct __attribute__((packed)) {
  uint16_t sample_counter;   /* 2 bytes - monotonic, loss detection */
  uint16_t rel_timestamp_ms; /* 2 bytes - ms since burst_start_ms in main.c */
  int16_t accel_x;           /* 2 bytes */
  int16_t accel_y;           /* 2 bytes */
  int16_t accel_z;           /* 2 bytes */
} accel_sample_t;            /* TOTAL = 10 bytes */

#define ACCEL_SAMPLE_SIZE sizeof(accel_sample_t) /* 10 */

/*============================================================================
 * Packet Format (243 bytes)
 *
 * Per architecture Rev 3:
 * - 24 samples per packet (24 × 10 = 240 bytes)
 * - 1 byte header (burst_id)
 * - 2 bytes CRC16
 * - Fits in BLE MTU (244 bytes)
 *===========================================================================*/

#define SAMPLES_PER_PACKET 24
#define PACKET_PAYLOAD_SIZE (SAMPLES_PER_PACKET * ACCEL_SAMPLE_SIZE) /* 240 */

typedef struct __attribute__((packed)) {
  uint8_t burst_id;                           /* 1 byte - burst sequence */
  accel_sample_t samples[SAMPLES_PER_PACKET]; /* 240 bytes */
  uint16_t crc16;                             /* 2 bytes - integrity check */
} accel_packet_t;                             /* TOTAL = 243 bytes */

#define ACCEL_PACKET_SIZE sizeof(accel_packet_t) /* 243 */

/*============================================================================
 * Ring Buffer Configuration
 *
 * 1024 samples = 1.024 seconds @ 1kHz (FFT-friendly)
 * 43 packets per burst (ceil(1024/24))
 *===========================================================================*/

#define RING_BUFFER_SAMPLES 1024
#define RING_BUFFER_MASK (RING_BUFFER_SAMPLES - 1) /* 0x3FF */
#define PACKETS_PER_BURST 43                       /* ceil(1024/24) */

/*============================================================================
 * Sensor Metadata (TEDS-like)
 *===========================================================================*/

typedef struct __attribute__((packed)) {
  char sensor_name[24];
  int16_t range_g;
  char unit[8];
} sensor_metadata_t;

/*============================================================================
 * API Functions
 *===========================================================================*/

/**
 * @brief Initialize the Accelerometer GATT Service
 * @return 0 on success, negative errno on failure
 */
int accel_service_init(void);

/**
 * @brief Send a complete burst packet (243 bytes)
 * @param conn Connection object (NULL for all connections)
 * @param packet Pointer to packet structure
 * @return 0 on success, negative errno on failure
 */
int accel_service_notify_packet(struct bt_conn *conn,
                                const accel_packet_t *packet);

/**
 * @brief Send timestamp notification
 * @param conn Connection object (NULL for all connections)
 * @param uptime_ms Current uptime in milliseconds
 * @return 0 on success, negative errno on failure
 */
int accel_service_notify_timestamp(struct bt_conn *conn, uint32_t uptime_ms);

/**
 * @brief Check if notifications are enabled for acceleration data
 * @return true if enabled, false otherwise
 */
bool accel_service_data_notify_enabled(void);

/**
 * @brief Set the current connection reference
 * @param conn Connection object
 */
void accel_service_set_conn(struct bt_conn *conn);

/**
 * @brief Get current operating mode
 * @return Current mode (MODE_COINCELL_BURST or MODE_CONTINUOUS_LAB)
 */
operating_mode_t accel_service_get_mode(void);

/**
 * @brief Set operating mode (only if external power detected)
 * @param mode Requested mode
 * @param external_power_detected true if USB/external power present
 * @return 0 on success, -EPERM if trying to set LAB mode without power
 */
int accel_service_set_mode(operating_mode_t mode, bool external_power_detected);

/**
 * @brief Update external power state for mode switching permission
 * @param detected true if USB/external power is present
 */
void accel_service_set_power_detected(bool detected);

#ifdef __cplusplus
}
#endif

#endif /* ACCEL_SERVICE_H_ */

#ifndef ACCEL_SERVICE_H_
#define ACCEL_SERVICE_H_

#include <zephyr/bluetooth/conn.h>
#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#define ACCEL_SERVICE_UUID BT_UUID_DECLARE_128(ACCEL_SERVICE_UUID_VAL)
#define ACCEL_DATA_CHAR_UUID BT_UUID_DECLARE_128(ACCEL_DATA_CHAR_UUID_VAL)
#define TIMESTAMP_CHAR_UUID BT_UUID_DECLARE_128(TIMESTAMP_CHAR_UUID_VAL)
#define SAMPLE_RATE_CHAR_UUID BT_UUID_DECLARE_128(SAMPLE_RATE_CHAR_UUID_VAL)
#define SENSOR_META_CHAR_UUID BT_UUID_DECLARE_128(SENSOR_META_CHAR_UUID_VAL)

/* Single acceleration sample (14 bytes) */
struct accel_sample {
  uint32_t sample_counter;
  uint32_t timestamp_ms; /* device uptime in ms for E2E latency */
  int16_t accel_x;
  int16_t accel_y;
  int16_t accel_z;
} __packed;

/* Batched packet configuration */
#define ACCEL_BATCH_SIZE 10 /* 10 samples per BLE notification */
/* Batch packet: 1 byte count + 10 * 14 bytes = 141 bytes (fits in 244 MTU) */

/* Batched acceleration packet structure */
struct accel_batch_packet {
  uint8_t batch_count; /* Number of valid samples in this batch */
  struct accel_sample samples[ACCEL_BATCH_SIZE];
} __packed;

/* Legacy single-sample alias for compatibility */
#define accel_data_packet accel_sample

/* Sensor metadata structure (TEDS-like) */
struct sensor_metadata {
  char sensor_name[24];
  int16_t range_g;
  char unit[8];
} __packed;

/**
 * @brief Initialize the Accelerometer GATT Service
 * @return 0 on success, negative errno on failure
 */
int accel_service_init(void);

/**
 * @brief Send batched acceleration data notification
 * @param conn Connection object (NULL for all connections)
 * @param samples Array of acceleration samples
 * @param count Number of samples in the batch (1 to ACCEL_BATCH_SIZE)
 * @return 0 on success, negative errno on failure
 */
int accel_service_notify_batch(struct bt_conn *conn,
                               const struct accel_sample *samples,
                               uint8_t count);

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

#ifdef __cplusplus
}
#endif

#endif /* ACCEL_SERVICE_H_ */

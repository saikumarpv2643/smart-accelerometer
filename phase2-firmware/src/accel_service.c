#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "accel_service.h"

LOG_MODULE_REGISTER(accel_svc, LOG_LEVEL_INF);

/* Service configuration */
#define SAMPLING_RATE_HZ 1000

/* State variables */
static bool data_notify_enabled = false;
static bool timestamp_notify_enabled = false;
static struct bt_conn *current_conn = NULL;

/* Static data for characteristics */
static uint16_t sampling_rate = SAMPLING_RATE_HZ;
static uint32_t current_timestamp = 0;

static struct sensor_metadata sensor_meta = {
    .sensor_name = "ISRO_Phase2_Accel", .range_g = 16, .unit = "g"};

/* CCC changed callback for acceleration data */
static void accel_data_ccc_changed(const struct bt_gatt_attr *attr,
                                   uint16_t value) {
  LOG_INF("CCC write received: value=0x%04x (NOTIFY=0x%04x)", value,
          BT_GATT_CCC_NOTIFY);
  data_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
  LOG_INF("Acceleration Data notifications %s",
          data_notify_enabled ? "ENABLED" : "DISABLED");
}

/* CCC changed callback for timestamp */
static void timestamp_ccc_changed(const struct bt_gatt_attr *attr,
                                  uint16_t value) {
  timestamp_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
  LOG_INF("Timestamp notifications %s",
          timestamp_notify_enabled ? "enabled" : "disabled");
}

/* Read callback for timestamp characteristic */
static ssize_t read_timestamp(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr, void *buf,
                              uint16_t len, uint16_t offset) {
  current_timestamp = (uint32_t)k_uptime_get();
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_timestamp,
                           sizeof(current_timestamp));
}

/* Read callback for sampling rate characteristic */
static ssize_t read_sampling_rate(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr, void *buf,
                                  uint16_t len, uint16_t offset) {
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &sampling_rate,
                           sizeof(sampling_rate));
}

/* Read callback for sensor metadata characteristic */
static ssize_t read_sensor_meta(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr, void *buf,
                                uint16_t len, uint16_t offset) {
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &sensor_meta,
                           sizeof(sensor_meta));
}

/* GATT Service Definition */
BT_GATT_SERVICE_DEFINE(
    accel_svc,
    /* Primary Service Declaration */
    BT_GATT_PRIMARY_SERVICE(ACCEL_SERVICE_UUID),

    /* Acceleration Data Characteristic (NOTIFY only) */
    BT_GATT_CHARACTERISTIC(ACCEL_DATA_CHAR_UUID, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(accel_data_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* Timestamp Characteristic (READ | NOTIFY) */
    BT_GATT_CHARACTERISTIC(TIMESTAMP_CHAR_UUID,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_timestamp, NULL, NULL),
    BT_GATT_CCC(timestamp_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* Sampling Rate Characteristic (READ only) */
    BT_GATT_CHARACTERISTIC(SAMPLE_RATE_CHAR_UUID, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ, read_sampling_rate, NULL, NULL),

    /* Sensor Metadata Characteristic (READ only) */
    BT_GATT_CHARACTERISTIC(SENSOR_META_CHAR_UUID, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ, read_sensor_meta, NULL, NULL), );

int accel_service_init(void) {
  LOG_INF("Accelerometer GATT Service initialized");
  LOG_INF("  Sampling Rate: %u Hz", SAMPLING_RATE_HZ);
  LOG_INF("  Sensor: %s, Range: +/-%d %s", sensor_meta.sensor_name,
          sensor_meta.range_g, sensor_meta.unit);
  return 0;
}

int accel_service_notify_batch(struct bt_conn *conn,
                               const struct accel_sample *samples,
                               uint8_t count) {
  if (!data_notify_enabled) {
    return -ENOTCONN;
  }

  struct bt_conn *target = conn ? conn : current_conn;
  if (!target) {
    return -ENOTCONN;
  }

  if (count == 0 || count > ACCEL_BATCH_SIZE) {
    return -EINVAL;
  }

  /* Build batch packet: 1 byte count + samples */
  static struct accel_batch_packet batch;
  batch.batch_count = count;
  memcpy(batch.samples, samples, count * sizeof(struct accel_sample));

  /* Send: 1 + (count * 14) bytes */
  size_t payload_size = 1 + (count * sizeof(struct accel_sample));
  return bt_gatt_notify(target, &accel_svc.attrs[1], &batch, payload_size);
}

int accel_service_notify_timestamp(struct bt_conn *conn, uint32_t uptime_ms) {
  if (!timestamp_notify_enabled) {
    return -ENOTCONN;
  }

  struct bt_conn *target = conn ? conn : current_conn;
  if (!target) {
    return -ENOTCONN;
  }

  return bt_gatt_notify(target, &accel_svc.attrs[4], &uptime_ms,
                        sizeof(uptime_ms));
}

bool accel_service_data_notify_enabled(void) { return data_notify_enabled; }

void accel_service_set_conn(struct bt_conn *conn) {
  if (current_conn) {
    bt_conn_unref(current_conn);
  }
  current_conn = conn ? bt_conn_ref(conn) : NULL;
}

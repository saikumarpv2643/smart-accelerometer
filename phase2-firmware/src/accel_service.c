/**
 * @file accel_service.c
 * @brief Coin-Cell Wireless Accelerometer GATT Service Implementation
 *
 * Architecture: Rev 3 - Supports both burst and continuous modes
 */

#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include "accel_service.h"

LOG_MODULE_REGISTER(accel_svc, LOG_LEVEL_INF);

/*============================================================================
 * Configuration
 *===========================================================================*/

#define SAMPLING_RATE_HZ 1000

/*============================================================================
 * State Variables
 *===========================================================================*/

static bool data_notify_enabled = false;
static bool timestamp_notify_enabled = false;
static struct bt_conn *current_conn = NULL;
static operating_mode_t current_mode = MODE_COINCELL_BURST;

/*============================================================================
 * Static Metadata
 *===========================================================================*/

static uint16_t sampling_rate = SAMPLING_RATE_HZ;
static uint32_t current_timestamp = 0;

static sensor_metadata_t sensor_meta = {
    .sensor_name = "ISRO_Phase3_Accel", .range_g = 16, .unit = "g"};

/*============================================================================
 * CCC Callbacks
 *===========================================================================*/

static void accel_data_ccc_changed(const struct bt_gatt_attr *attr,
                                   uint16_t value) {
  data_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
  LOG_INF("Acceleration Data notifications %s",
          data_notify_enabled ? "ENABLED" : "DISABLED");
}

static void timestamp_ccc_changed(const struct bt_gatt_attr *attr,
                                  uint16_t value) {
  timestamp_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
  LOG_INF("Timestamp notifications %s",
          timestamp_notify_enabled ? "enabled" : "disabled");
}

/*============================================================================
 * Read Callbacks
 *===========================================================================*/

static ssize_t read_timestamp(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr, void *buf,
                              uint16_t len, uint16_t offset) {
  current_timestamp = (uint32_t)k_uptime_get();
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_timestamp,
                           sizeof(current_timestamp));
}

static ssize_t read_sampling_rate(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr, void *buf,
                                  uint16_t len, uint16_t offset) {
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &sampling_rate,
                           sizeof(sampling_rate));
}

static ssize_t read_sensor_meta(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr, void *buf,
                                uint16_t len, uint16_t offset) {
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &sensor_meta,
                           sizeof(sensor_meta));
}

static ssize_t read_operating_mode(struct bt_conn *conn,
                                   const struct bt_gatt_attr *attr, void *buf,
                                   uint16_t len, uint16_t offset) {
  uint8_t mode_byte = (uint8_t)current_mode;
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &mode_byte,
                           sizeof(mode_byte));
}

static ssize_t write_operating_mode(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    const void *buf, uint16_t len,
                                    uint16_t offset, uint8_t flags) {
  if (len != 1) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }

  uint8_t requested = ((const uint8_t *)buf)[0];

  if (requested == MODE_CONTINUOUS_LAB) {
    /* TODO: Check external power via ADC before allowing */
    /* For now, allow mode switch */
    current_mode = MODE_CONTINUOUS_LAB;
    LOG_INF("Mode switched to CONTINUOUS (Lab)");
  } else {
    current_mode = MODE_COINCELL_BURST;
    LOG_INF("Mode switched to BURST (Coin-cell)");
  }

  return len;
}

/*============================================================================
 * GATT Service Definition
 *===========================================================================*/

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
                           BT_GATT_PERM_READ, read_sensor_meta, NULL, NULL),

    /* Operating Mode Characteristic (READ | WRITE) */
    BT_GATT_CHARACTERISTIC(OPERATING_MODE_CHAR_UUID,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                           read_operating_mode, write_operating_mode, NULL), );

/*============================================================================
 * API Implementation
 *===========================================================================*/

int accel_service_init(void) {
  LOG_INF("Accelerometer GATT Service initialized (Rev 3)");
  LOG_INF("  Sample size: %u bytes", ACCEL_SAMPLE_SIZE);
  LOG_INF("  Packet size: %u bytes (%u samples)", ACCEL_PACKET_SIZE,
          SAMPLES_PER_PACKET);
  LOG_INF("  Packets per burst: %u", PACKETS_PER_BURST);
  LOG_INF("  Initial mode: %s",
          current_mode == MODE_COINCELL_BURST ? "BURST" : "CONTINUOUS");
  return 0;
}

int accel_service_notify_packet(struct bt_conn *conn,
                                const accel_packet_t *packet) {
  if (!data_notify_enabled) {
    return -ENOTCONN;
  }

  struct bt_conn *target = conn ? conn : current_conn;
  if (!target) {
    return -ENOTCONN;
  }

  /* Packet already includes CRC, just send it */
  return bt_gatt_notify(target, &accel_svc.attrs[1], packet, ACCEL_PACKET_SIZE);
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

operating_mode_t accel_service_get_mode(void) { return current_mode; }

int accel_service_set_mode(operating_mode_t mode,
                           bool external_power_detected) {
  if (mode == MODE_CONTINUOUS_LAB && !external_power_detected) {
    LOG_WRN("Cannot switch to CONTINUOUS mode without external power");
    return -EPERM;
  }

  current_mode = mode;
  LOG_INF("Operating mode set to %s",
          mode == MODE_COINCELL_BURST ? "BURST" : "CONTINUOUS");
  return 0;
}

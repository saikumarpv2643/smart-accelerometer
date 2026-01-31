#include <dk_buttons_and_leds.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "accel_service.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* MPU6050 device */
static const struct device *mpu;

/* Sensor Parameters */
#define SAMPLE_FREQ_HZ 1000.0f
#define SAMPLE_PERIOD_US (1000000 / (int)SAMPLE_FREQ_HZ)
#define MPU_LSB_PER_G 2048.0f /* ±16g range */

/* Statistics timer */
static int64_t last_stats_time = 0;

/* BLE Connection Callbacks */
static void connected(struct bt_conn *conn, uint8_t err) {
  if (err) {
    LOG_ERR("Connection failed (err %u)", err);
    return;
  }

  LOG_INF("Connected");
  dk_set_led_on(DK_LED1);

  accel_service_set_conn(conn);

  /* Reset stats timer on new connection */
  last_stats_time = k_uptime_get();
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
  LOG_INF("Disconnected (reason %u)", reason);
  dk_set_led_off(DK_LED1);

  accel_service_set_conn(NULL);
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
                             uint16_t latency, uint16_t timeout) {
  LOG_INF("Connection parameters updated: interval=%u, latency=%u, timeout=%u",
          interval, latency, timeout);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .le_param_updated = le_param_updated,
};

/* MPU6050 Sensor Thread with Batch Buffering */
static void sensor_thread_fn(void *p1, void *p2, void *p3) {
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  uint32_t sample_counter = 0;
  struct sensor_value accel[3];

  /* Sample buffer for batching */
  static struct accel_sample buffer[ACCEL_BATCH_SIZE];
  uint8_t buffer_idx = 0;

  /* Batch statistics */
  static uint32_t batches_sent = 0;
  static uint32_t batches_dropped = 0;
  static uint32_t buffer_overflows = 0;

  LOG_INF("MPU6050 sensor thread started (BATCHED mode)");
  LOG_INF("  Sample Rate: %.1f Hz", (double)SAMPLE_FREQ_HZ);
  LOG_INF("  Batch Size: %d samples", ACCEL_BATCH_SIZE);

  while (1) {
    /* Fetch sensor data */
    int ret = sensor_sample_fetch(mpu);
    if (ret < 0) {
      LOG_WRN("Sensor fetch failed: %d", ret);
      k_busy_wait(SAMPLE_PERIOD_US);
      continue;
    }

    /* Get tri-axial acceleration */
    sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_XYZ, accel);

    /* Convert from m/s² to g, then to raw counts */
    float ax_g = (float)sensor_value_to_double(&accel[0]) / 9.81f;
    float ay_g = (float)sensor_value_to_double(&accel[1]) / 9.81f;
    float az_g = (float)sensor_value_to_double(&accel[2]) / 9.81f;

    int16_t ax_raw = (int16_t)(ax_g * MPU_LSB_PER_G);
    int16_t ay_raw = (int16_t)(ay_g * MPU_LSB_PER_G);
    int16_t az_raw = (int16_t)(az_g * MPU_LSB_PER_G);

    sample_counter++;

    /* Store sample in buffer with individual timestamp */
    buffer[buffer_idx].sample_counter = sample_counter;
    buffer[buffer_idx].timestamp_ms = (uint32_t)k_uptime_get();
    buffer[buffer_idx].accel_x = ax_raw;
    buffer[buffer_idx].accel_y = ay_raw;
    buffer[buffer_idx].accel_z = az_raw;
    buffer_idx++;

    /* When batch is full, send it */
    if (buffer_idx >= ACCEL_BATCH_SIZE) {
      if (accel_service_data_notify_enabled()) {
        int err = accel_service_notify_batch(NULL, buffer, ACCEL_BATCH_SIZE);
        if (err == 0) {
          batches_sent++;
        } else if (err == -ENOMEM || err == -EAGAIN) {
          batches_dropped++;
        } else if (err != -ENOTCONN) {
          batches_dropped++;
        }
      } else {
        /* Not connected - buffer overflow, drop oldest */
        buffer_overflows++;
      }
      buffer_idx = 0; /* Reset buffer */
    }

    /* Log statistics every 5 seconds */
    int64_t now = k_uptime_get();
    if (now - last_stats_time >= 5000) {
      float batch_rate =
          (float)batches_sent / ((now - last_stats_time) / 1000.0f);
      float sample_rate = batch_rate * ACCEL_BATCH_SIZE;
      LOG_INF("Stats: batches=%u, dropped=%u, overflow=%u", batches_sent,
              batches_dropped, buffer_overflows);
      LOG_INF("  Rate: %.1f batches/s → %.1f samples/s", (double)batch_rate,
              (double)sample_rate);
      batches_sent = 0;
      batches_dropped = 0;
      buffer_overflows = 0;
      last_stats_time = now;
    }

    /* Precise timing at 1kHz */
    k_busy_wait(SAMPLE_PERIOD_US);
  }
}

K_THREAD_DEFINE(sensor_thread, 2048, sensor_thread_fn, NULL, NULL, NULL, 7, 0,
                0);

/* Advertising Parameters: Connectable, Fast interval */
#define BT_LE_ADV_CONN_CUSTOM                                                  \
  BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,               \
                  BT_GAP_ADV_FAST_INT_MAX_2, NULL)

/* Advertising Data */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Scan Response Data with Service UUID */
static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, ACCEL_SERVICE_UUID_VAL),
};

int main(void) {
  int err;

  LOG_INF("===========================================");
  LOG_INF("ISRO Phase-2 MPU6050 Accelerometer Service");
  LOG_INF("===========================================");

  /* Get MPU6050 device */
  mpu = DEVICE_DT_GET_ONE(invensense_mpu6050);
  if (!device_is_ready(mpu)) {
    LOG_ERR("MPU6050 not ready!");
    return -ENODEV;
  }
  LOG_INF("MPU6050 ready");

  /* Set sampling rate to 1000 Hz */
  struct sensor_value odr = {.val1 = 1000, .val2 = 0};
  err = sensor_attr_set(mpu, SENSOR_CHAN_ACCEL_XYZ,
                        SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
  if (err) {
    LOG_WRN("Could not set ODR (err %d)", err);
  }

  /* Initialize LEDs */
  err = dk_leds_init();
  if (err) {
    LOG_ERR("LED init failed (err %d)", err);
  }

  /* Initialize Bluetooth */
  err = bt_enable(NULL);
  if (err) {
    LOG_ERR("Bluetooth init failed (err %d)", err);
    return err;
  }
  LOG_INF("Bluetooth initialized");

  /* Load settings */
  if (IS_ENABLED(CONFIG_SETTINGS)) {
    settings_load();
  }

  /* Initialize accelerometer service */
  err = accel_service_init();
  if (err) {
    LOG_ERR("Accel service init failed (err %d)", err);
    return err;
  }

  /* Start advertising */
  err = bt_le_adv_start(BT_LE_ADV_CONN_CUSTOM, ad, ARRAY_SIZE(ad), sd,
                        ARRAY_SIZE(sd));
  if (err) {
    LOG_ERR("Advertising failed to start (err %d)", err);
    return err;
  }
  LOG_INF("Advertising started as '%s'", CONFIG_BT_DEVICE_NAME);

  /* Blink LED2 to indicate running */
  while (1) {
    dk_set_led(DK_LED2, 1);
    k_sleep(K_MSEC(500));
    dk_set_led(DK_LED2, 0);
    k_sleep(K_MSEC(500));
  }

  return 0;
}

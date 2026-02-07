#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#endif
#if defined(CONFIG_SENSOR)
#include <zephyr/drivers/sensor.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>

#include "accel_service.h"

/* Conditional logging - disabled in release builds */
#if defined(CONFIG_LOG)
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);
#define LOG_INF_SAFE(...) LOG_INF(__VA_ARGS__)
#define LOG_ERR_SAFE(...) LOG_ERR(__VA_ARGS__)
#define LOG_WRN_SAFE(...) LOG_WRN(__VA_ARGS__)
#else
#define LOG_INF_SAFE(...) do {} while(0)
#define LOG_ERR_SAFE(...) do {} while(0)
#define LOG_WRN_SAFE(...) do {} while(0)
#endif

/* Conditional LED support - disabled in release builds */
#if defined(CONFIG_DK_LIBRARY)
#include <dk_buttons_and_leds.h>
#define LED_ON(led)  dk_set_led_on(led)
#define LED_OFF(led) dk_set_led_off(led)
#define LED_SET(led, val) dk_set_led(led, val)
#define LEDS_INIT() dk_leds_init()
#else
#define LED_ON(led)  do {} while(0)
#define LED_OFF(led) do {} while(0)
#define LED_SET(led, val) do {} while(0)
#define LEDS_INIT() 0
#endif

/* MPU6050 device */
static const struct device *mpu;

/* Sensor Parameters */
#define SAMPLE_FREQ_HZ 1000.0f   /* 1 kHz high-rate sampling */
#define SAMPLE_PERIOD_US 1000    /* 1 ms between samples */
#define BATCH_PERIOD_MS 17       /* Send batch every 17 ms (17 samples) */
#define MPU_LSB_PER_G 2048.0f    /* ±16g range */

/* Statistics timer (only used in debug builds) */
#if defined(CONFIG_LOG)
static int64_t last_stats_time = 0;
#endif

/* BLE Connection Callbacks */
static void connected(struct bt_conn *conn, uint8_t err) {
  if (err) {
    LOG_ERR_SAFE("Connection failed (err %u)", err);
    return;
  }

  // LOG_INF_SAFE("Connected");
  // Turn off LED for power saving when connected
  LED_OFF(DK_LED1);

  accel_service_set_conn(conn);

#if defined(CONFIG_LOG)
  /* Reset stats timer on new connection */
  last_stats_time = k_uptime_get();
#endif
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
  // LOG_INF_SAFE("Disconnected (reason %u)", reason);
  LED_OFF(DK_LED1);

  accel_service_set_conn(NULL);
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
                             uint16_t latency, uint16_t timeout) {
  LOG_INF_SAFE("Connection parameters updated: interval=%u, latency=%u, timeout=%u",
          interval, latency, timeout);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .le_param_updated = le_param_updated,
};

/* MPU6050 Sensor Thread with 1 kHz Sampling */
static void sensor_thread_fn(void *p1, void *p2, void *p3) {
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  uint32_t sample_counter = 0;
  struct sensor_value accel[3];

  /* Sample buffer for batching (17 samples = 17ms at 1kHz) */
  static struct accel_sample buffer[ACCEL_BATCH_SIZE];
  uint8_t buffer_idx = 0;

  /* Timing variables for precise 1 kHz */
  int64_t next_sample_time = k_uptime_get();
  
  // Debug/statistics code removed for power saving

  while (1) {
    /* Wait until next sample time (absolute timing) */
    next_sample_time += 1; /* 1 ms interval */
    int64_t now = k_uptime_get();
    if (next_sample_time > now) {
      k_sleep(K_TIMEOUT_ABS_MS(next_sample_time));
    }

    /* Fetch sensor data */
    int ret = sensor_sample_fetch(mpu);
    if (ret < 0) {
      LOG_WRN_SAFE("Sensor fetch failed: %d", ret);
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

    /* When batch is full (17 samples = 17ms at 1kHz), send it */
    if (buffer_idx >= ACCEL_BATCH_SIZE) {
      if (accel_service_data_notify_enabled()) {
        int err = accel_service_notify_batch(NULL, buffer, ACCEL_BATCH_SIZE);
#if defined(CONFIG_LOG)
        if (err == 0) {
          batches_sent++;
        } else if (err == -ENOMEM || err == -EAGAIN) {
          batches_dropped++;
        } else if (err != -ENOTCONN) {
          batches_dropped++;
        }
#else
        (void)err;
#endif
      } else {
#if defined(CONFIG_LOG)
        /* Not connected - buffer overflow, drop oldest */
        buffer_overflows++;
#endif
      }
      buffer_idx = 0; /* Reset buffer */
    }

#if defined(CONFIG_LOG)
    /* Log statistics every 5 seconds (debug only) */
    int64_t stats_now = k_uptime_get();
    if (stats_now - last_stats_time >= 5000) {
      float batch_rate =
          (float)batches_sent / ((stats_now - last_stats_time) / 1000.0f);
      float sample_rate = batch_rate * ACCEL_BATCH_SIZE;
      LOG_INF_SAFE("Stats: batches=%u, dropped=%u, overflow=%u", batches_sent,
              batches_dropped, buffer_overflows);
      LOG_INF_SAFE("  Rate: %.1f batches/s → %.1f samples/s", (double)batch_rate,
              (double)sample_rate);
      batches_sent = 0;
      batches_dropped = 0;
      buffer_overflows = 0;
      last_stats_time = stats_now;
    }
#endif

    /* Timing is handled at top of loop with absolute time */
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

  // LOG_INF_SAFE("===========================================");
  // LOG_INF_SAFE("ISRO Phase-2 MPU6050 Accelerometer Service");
  // LOG_INF_SAFE("1 kHz Sampling - Power Optimized");
  // LOG_INF_SAFE("===========================================");

  /* Get MPU6050 device */
  mpu = DEVICE_DT_GET_ONE(invensense_mpu6050);
  if (!device_is_ready(mpu)) {
    LOG_ERR_SAFE("MPU6050 not ready!");
    return -ENODEV;
  }
  LOG_INF_SAFE("MPU6050 ready");

  /* Set sampling rate to 1000 Hz for high-rate sampling */
  struct sensor_value odr = {.val1 = 1000, .val2 = 0};
  err = sensor_attr_set(mpu, SENSOR_CHAN_ACCEL_XYZ,
                        SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
  if (err) {
    LOG_WRN_SAFE("Could not set ODR (err %d)", err);
  }

  /* Initialize LEDs (only if enabled) */
  err = LEDS_INIT();
  if (err) {
    // LOG_ERR_SAFE("LED init failed (err %d)", err);
  }

  /* Initialize Bluetooth */
  err = bt_enable(NULL);
  if (err) {
    LOG_ERR_SAFE("Bluetooth init failed (err %d)", err);
    return err;
  }
  LOG_INF_SAFE("Bluetooth initialized");

  /* Load settings */
  if (IS_ENABLED(CONFIG_SETTINGS)) {
    settings_load();
  }

  /* Initialize accelerometer service */
  err = accel_service_init();
  if (err) {
    LOG_ERR_SAFE("Accel service init failed (err %d)", err);
    return err;
  }

  /* Start advertising */
  err = bt_le_adv_start(BT_LE_ADV_CONN_CUSTOM, ad, ARRAY_SIZE(ad), sd,
                        ARRAY_SIZE(sd));
  if (err) {
    LOG_ERR_SAFE("Advertising failed to start (err %d)", err);
    return err;
  }
  LOG_INF_SAFE("Advertising started as '%s'", CONFIG_BT_DEVICE_NAME);

  /* Main loop - sleep for power efficiency */
  /* Sensor thread handles data collection */
  /* Power management will put CPU to sleep between wakeups */
  while (1) {
    /* Sleep indefinitely - sensor thread and BLE stack handle everything */
    /* PM subsystem will enter low power state automatically */
    k_sleep(K_FOREVER);
  }

  return 0;
}

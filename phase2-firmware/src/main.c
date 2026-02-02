/**
 * @file main.c
 * @brief Coin-Cell Wireless Accelerometer Firmware
 *
 * Architecture: Rev 3
 * - ISR-driven 1 kHz sampling with minimal work in ISR
 * - High-priority reader thread for I2C reads
 * - Ring buffer for 1024 samples
 * - Burst transmission every ~1 second (coin-cell mode)
 * - Continuous streaming (lab mode with external power)
 */

#include <dk_buttons_and_leds.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>

#include "accel_service.h"

/* Coin-cell mode: reduce logging to save power */
#ifdef CONFIG_COINCELL_MODE
LOG_MODULE_REGISTER(main, LOG_LEVEL_WRN);
#else
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);
#endif

/*============================================================================
 * Configuration
 *===========================================================================*/

#define SAMPLE_FREQ_HZ 1000
#define SAMPLE_PERIOD_US (1000000 / SAMPLE_FREQ_HZ) /* 1000 µs */

/* MPU6050 Registers */
#define MPU6050_ADDR 0x68
#define MPU6050_ACCEL_XOUT_H 0x3B
#define MPU6050_PWR_MGMT_1 0x6B
#define MPU6050_SMPLRT_DIV 0x19
#define MPU6050_CONFIG 0x1A
#define MPU6050_ACCEL_CONFIG 0x1C

/* Burst mode timing */
#define SAMPLES_BEFORE_BURST RING_BUFFER_SAMPLES /* 1024 */
#define INTER_PACKET_DELAY_MS 2 /* Prevent coin-cell brownout */

/*============================================================================
 * Hardware Devices
 *===========================================================================*/

static const struct device *i2c_dev;

/*============================================================================
 * Ring Buffer (Shared between ISR/Thread and Burst Controller)
 *===========================================================================*/

static accel_sample_t ring_buffer[RING_BUFFER_SAMPLES];
static volatile uint16_t write_idx = 0;
static volatile uint16_t read_idx = 0;
static volatile uint16_t sample_counter = 0;
static volatile uint32_t burst_start_ms = 0;

/*============================================================================
 * Synchronization
 *===========================================================================*/

/* Semaphore depth must be unbounded to prevent losing ISR signals */
K_SEM_DEFINE(sample_ready_sem, 0, K_SEM_MAX_LIMIT);
K_SEM_DEFINE(burst_ready_sem, 0, K_SEM_MAX_LIMIT);

static volatile uint16_t pending_timestamp_ms = 0;

/*============================================================================
 * Statistics
 *===========================================================================*/

static uint32_t total_samples = 0;
static volatile uint32_t samples_overflowed =
    0; /* Ring buffer overflow count */
static uint32_t total_bursts = 0;
static uint32_t packets_sent = 0;
static uint32_t packets_failed = 0;

/* MTU tracking - need >= 246 for 243-byte packets */
static volatile bool mtu_ready = false;
static volatile uint16_t current_mtu = 23; /* Default BLE MTU */
#define REQUIRED_MTU 246 /* 243 payload + 3 ATT header (opcode + handle) */

/*============================================================================
 * Timer ISR - Minimal work, deterministic timing
 *===========================================================================*/

static void sample_timer_handler(struct k_timer *timer) {
  ARG_UNUSED(timer);

  /* Capture timestamp BEFORE any variable latency */
  pending_timestamp_ms = (uint16_t)(k_uptime_get_32() - burst_start_ms);

  /* Signal reader thread - NO I2C work here! */
  k_sem_give(&sample_ready_sem);
}

K_TIMER_DEFINE(sample_timer, sample_timer_handler, NULL);

/*============================================================================
 * High-Priority Sample Reader Thread
 *
 * Does I2C read OUTSIDE of ISR to avoid blocking and jitter.
 * Runs at priority 0 (highest) to minimize latency after ISR signal.
 *===========================================================================*/

static void sample_reader_thread_fn(void *p1, void *p2, void *p3) {
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  uint8_t raw_data[6];
  int ret;

  LOG_INF("Sample reader thread started");

  while (1) {
    /* Wait for ISR signal */
    k_sem_take(&sample_ready_sem, K_FOREVER);

    /* Capture timestamp locally to avoid race with ISR */
    uint16_t local_timestamp = pending_timestamp_ms;

    /* Ring buffer overflow protection: drop sample if full */
    uint16_t samples_pending = write_idx - read_idx;
    if (samples_pending >= RING_BUFFER_SAMPLES) {
      samples_overflowed++;
      continue; /* Drop THIS sample - never block ISR/sampling */
    }

    /* Read accelerometer data via I2C - this takes ~300µs */
    ret = i2c_burst_read(i2c_dev, MPU6050_ADDR, MPU6050_ACCEL_XOUT_H, raw_data,
                         6);
    if (ret < 0) {
      LOG_WRN("I2C read failed: %d", ret);
      continue;
    }

    /* Pack sample into ring buffer */
    uint16_t idx = write_idx & RING_BUFFER_MASK;
    ring_buffer[idx].sample_counter = sample_counter;
    ring_buffer[idx].rel_timestamp_ms = local_timestamp;
    ring_buffer[idx].accel_x = (int16_t)((raw_data[0] << 8) | raw_data[1]);
    ring_buffer[idx].accel_y = (int16_t)((raw_data[2] << 8) | raw_data[3]);
    ring_buffer[idx].accel_z = (int16_t)((raw_data[4] << 8) | raw_data[5]);

    write_idx++;
    sample_counter++;
    total_samples++;

    /* Check if buffer is full (time for burst) */
    samples_pending = write_idx - read_idx;
    if (samples_pending >= SAMPLES_BEFORE_BURST) {
      k_sem_give(&burst_ready_sem);
    }
  }
}

K_THREAD_DEFINE(sample_reader, 1024, sample_reader_thread_fn, NULL, NULL, NULL,
                0, 0, 0); /* Priority 0 = highest */

/*============================================================================
 * Burst Controller Thread
 *
 * Waits for buffer to fill, then transmits all packets.
 * In coin-cell mode, this fires every ~1 second.
 * In lab mode, this fires more frequently (smaller batches).
 *===========================================================================*/

static accel_packet_t tx_packet; /* Static to avoid stack allocation */

static void burst_controller_thread_fn(void *p1, void *p2, void *p3) {
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  static uint8_t burst_id = 0;
  int err;

  LOG_INF("Burst controller thread started");

  while (1) {
    /* Wait for buffer to fill */
    k_sem_take(&burst_ready_sem, K_FOREVER);

    if (!accel_service_data_notify_enabled()) {
      /* Not connected or notifications not enabled - silently drain buffer */
      read_idx = write_idx;
      continue;
    }

    if (!mtu_ready) {
      /* MTU exchange not complete yet - drain buffer and wait */
      LOG_WRN("MTU not ready (current: %u, need: %u), draining %u samples",
              current_mtu, REQUIRED_MTU, write_idx - read_idx);
      read_idx = write_idx;
      continue;
    }

    LOG_INF("Starting burst %u: %u samples buffered", burst_id,
            (uint16_t)(write_idx - read_idx));

    /*
     * Robustness fix: Only send full packets available in buffer.
     * Prevents read_idx from overshooting write_idx (1024 is not divisible by
     * 24).
     */
    uint16_t samples_available = write_idx - read_idx;
    uint16_t packets_to_send = samples_available / SAMPLES_PER_PACKET;

    if (packets_to_send > PACKETS_PER_BURST) {
      packets_to_send = PACKETS_PER_BURST;
    }

    if (packets_to_send == 0) {
      continue;
    }

    for (uint16_t p = 0; p < packets_to_send; p++) {
      /* Re-check connection status before each packet */
      if (!accel_service_data_notify_enabled()) {
        LOG_WRN("Connection lost mid-burst, aborting");
        break;
      }

      /* Build packet */
      tx_packet.burst_id = burst_id;

      for (uint16_t s = 0; s < SAMPLES_PER_PACKET; s++) {
        uint16_t src_idx =
            (read_idx + (p * SAMPLES_PER_PACKET) + s) & RING_BUFFER_MASK;
        tx_packet.samples[s] = ring_buffer[src_idx];
      }

      /* Calculate CRC16 over header + samples (all but the last 2 bytes, which
       * are the CRC itself) */
      tx_packet.crc16 = crc16_ccitt(0xFFFF, (const uint8_t *)&tx_packet,
                                    ACCEL_PACKET_SIZE - 2);

      /* Send packet */
      err = accel_service_notify_packet(NULL, &tx_packet);
      if (err == 0) {
        packets_sent++;
      } else {
        packets_failed++;
        LOG_WRN("Packet %u failed: %d", p, err);
      }

      /* Inter-packet delay to prevent coin-cell brownout */
      if (accel_service_get_mode() == MODE_COINCELL_BURST) {
        k_sleep(K_MSEC(INTER_PACKET_DELAY_MS));
      }
    }

    /* Advance read pointer */
    read_idx += packets_to_send * SAMPLES_PER_PACKET;

    /* Reset burst start time for next window */
    burst_start_ms = k_uptime_get_32();

    burst_id++;
    total_bursts++;

    LOG_INF("Burst complete: %u packets sent, %u failed%s", packets_sent,
            packets_failed,
            samples_overflowed > 0 ? " (OVERFLOW detected)" : "");
    if (samples_overflowed > 0) {
      LOG_WRN("Total dropped samples due to overflow: %u", samples_overflowed);
    }
  }
}

K_THREAD_DEFINE(burst_controller, 2048, burst_controller_thread_fn, NULL, NULL,
                NULL, 5, 0, 0); /* Priority 5 = lower than reader */

/*============================================================================
 * MPU6050 Initialization
 *===========================================================================*/

static int mpu6050_init(void) {
  uint8_t buf[2];
  int ret;

  /* Wake up MPU6050 */
  buf[0] = MPU6050_PWR_MGMT_1;
  buf[1] = 0x00; /* Clear sleep bit */
  ret = i2c_write(i2c_dev, buf, 2, MPU6050_ADDR);
  if (ret < 0) {
    LOG_ERR("Failed to wake MPU6050: %d", ret);
    return ret;
  }

  k_sleep(K_MSEC(100)); /* Wait for wake */

  /* Set sample rate divider (8kHz / (1 + 7) = 1kHz) */
  buf[0] = MPU6050_SMPLRT_DIV;
  buf[1] = 7;
  ret = i2c_write(i2c_dev, buf, 2, MPU6050_ADDR);
  if (ret < 0) {
    LOG_ERR("Failed to set sample rate: %d", ret);
    return ret;
  }

  /* Set DLPF to 44Hz bandwidth */
  buf[0] = MPU6050_CONFIG;
  buf[1] = 0x03;
  ret = i2c_write(i2c_dev, buf, 2, MPU6050_ADDR);
  if (ret < 0) {
    LOG_ERR("Failed to set DLPF: %d", ret);
    return ret;
  }

  /* Set accelerometer range to ±16g */
  buf[0] = MPU6050_ACCEL_CONFIG;
  buf[1] = 0x18; /* AFS_SEL = 3 → ±16g */
  ret = i2c_write(i2c_dev, buf, 2, MPU6050_ADDR);
  if (ret < 0) {
    LOG_ERR("Failed to set accel range: %d", ret);
    return ret;
  }

  LOG_INF("MPU6050 initialized: ±16g, 1kHz ODR, 44Hz DLPF");
  return 0;
}

/*============================================================================
 * BLE Connection Callbacks
 *===========================================================================*/

/* Forward declare MTU exchange callback and params */
static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
                            struct bt_gatt_exchange_params *params);

static struct bt_gatt_exchange_params mtu_exchange_params = {
    .func = mtu_exchange_cb,
};

static void connected(struct bt_conn *conn, uint8_t err) {
  if (err) {
    LOG_ERR("Connection failed (err %u)", err);
    return;
  }

  LOG_INF("Connected");

  /* LED only in lab mode to save coin-cell power */
  if (accel_service_get_mode() == MODE_CONTINUOUS_LAB) {
    dk_set_led_on(DK_LED1);
  }

  accel_service_set_conn(conn);

  /* Reset MTU state */
  mtu_ready = false;
  current_mtu = 23;

  /* Request MTU exchange for large packets */
  int ret = bt_gatt_exchange_mtu(conn, &mtu_exchange_params);
  if (ret) {
    LOG_ERR("MTU exchange request failed (err %d)", ret);
  } else {
    LOG_INF("MTU exchange requested");
  }

  /* Reset burst timing on new connection */
  burst_start_ms = k_uptime_get_32();
  sample_counter = 0;
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
  LOG_INF("Disconnected (reason %u)", reason);
  dk_set_led_off(DK_LED1); /* Always safe to turn off */

  accel_service_set_conn(NULL); /* Resets CCC state */
  mtu_ready = false;
  current_mtu = 23;
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
                             uint16_t latency, uint16_t timeout) {
  LOG_INF("Connection params: interval=%u (%.2f ms), latency=%u, timeout=%u",
          interval, interval * 1.25, latency, timeout);
}

/* GATT MTU exchange callback implementation */
static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
                            struct bt_gatt_exchange_params *params) {
  if (err) {
    LOG_ERR("MTU exchange failed (err %u)", err);
    return;
  }

  current_mtu = bt_gatt_get_mtu(conn);
  LOG_INF("MTU exchanged: %u bytes", current_mtu);

  if (current_mtu >= REQUIRED_MTU) {
    mtu_ready = true;
    LOG_INF("MTU ready for 243-byte packets");
  } else {
    LOG_WRN("MTU %u too small for 243-byte packets (need %u)", current_mtu,
            REQUIRED_MTU);
  }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .le_param_updated = le_param_updated,
};

/*============================================================================
 * Advertising
 *===========================================================================*/

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, ACCEL_SERVICE_UUID_VAL),
};

/* Connectable advertising parameters */
static struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
    BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2, /* 100 ms */
    BT_GAP_ADV_FAST_INT_MAX_2,                     /* 150 ms */
    NULL);

/*============================================================================
 * Main Entry Point
 *===========================================================================*/

int main(void) {
  int err;

  /* Power-safe enforcement: Ensure structures match BLE packet requirements */
  BUILD_ASSERT(sizeof(accel_sample_t) == 10, "Sample must be 10 bytes");
  BUILD_ASSERT(sizeof(accel_packet_t) == 243, "Packet must be 243 bytes");
  BUILD_ASSERT(RING_BUFFER_SAMPLES == 1024, "Buffer must remain 1024 for FFT");

  LOG_INF("=========================================");
  LOG_INF("ISRO Coin-Cell Accelerometer Rev 3");
  LOG_INF("=========================================");
  LOG_INF("Sample format: %u bytes", ACCEL_SAMPLE_SIZE);
  LOG_INF("Packet format: %u bytes (%u samples)", ACCEL_PACKET_SIZE,
          SAMPLES_PER_PACKET);
  LOG_INF("Buffer depth: %u samples", RING_BUFFER_SAMPLES);
  LOG_INF("Packets per burst: %u", PACKETS_PER_BURST);

  /* Get I2C device */
  i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
  if (!device_is_ready(i2c_dev)) {
    LOG_ERR("I2C device not ready!");
    return -ENODEV;
  }
  LOG_INF("I2C device ready");

  /* Initialize MPU6050 */
  err = mpu6050_init();
  if (err) {
    LOG_ERR("MPU6050 init failed: %d", err);
    return err;
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
  err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
  if (err) {
    LOG_ERR("Advertising failed to start (err %d)", err);
    return err;
  }
  LOG_INF("Advertising started as '%s'", CONFIG_BT_DEVICE_NAME);

  /* Initialize burst start time */
  burst_start_ms = k_uptime_get_32();

  /* Start sample timer at 1 kHz */
  k_timer_start(&sample_timer, K_USEC(SAMPLE_PERIOD_US),
                K_USEC(SAMPLE_PERIOD_US));
  LOG_INF("Sampling started at %u Hz", SAMPLE_FREQ_HZ);

  /* Main loop - LED heartbeat only in lab mode */
  while (1) {
    if (accel_service_get_mode() == MODE_CONTINUOUS_LAB) {
      dk_set_led(DK_LED2, 1);
      k_sleep(K_MSEC(500));
      dk_set_led(DK_LED2, 0);
      k_sleep(K_MSEC(500));
    } else {
      /* Coin-cell mode: sleep longer, no LED blink */
      k_sleep(K_MSEC(1000));
    }
  }

  return 0;
}

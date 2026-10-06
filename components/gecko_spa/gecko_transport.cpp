#include "gecko_transport.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef GECKO_SPA_DIRECT_I2C
#include <driver/gpio.h>
#include <esp_attr.h>
#ifdef USE_GECKO_SPA_I2C_ARDUINO_HAL
#include <esp32-hal-i2c.h>
#include <esp32-hal-i2c-slave.h>
#endif
#endif

namespace esphome {
namespace gecko_spa {

static const char *const TAG = "gecko_spa.transport";

#ifdef USE_GECKO_SPA_UART

// The proxy holds RST low for us; 100ms is well past the ATmega reset pulse
// minimum and short enough that the spa does not notice the gap.
static const uint32_t RESET_PULSE_MS = 100;

static uint8_t hex_nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return 0;
}

void GeckoUartTransport::setup_transport() {
  if (reset_pin_ != nullptr) {
    reset_pin_->setup();
    reset_pin_->digital_write(true);  // RST is active LOW, keep HIGH
  }
}

void GeckoUartTransport::loop_transport() {
  // Finish a non-blocking reset pulse
  if (reset_in_progress_ && (millis() - reset_start_time_ > RESET_PULSE_MS)) {
    if (reset_pin_ != nullptr) {
      reset_pin_->digital_write(true);  // Release reset (HIGH)
    }
    reset_in_progress_ = false;
    ESP_LOGI(TAG, "Arduino reset complete");
  }

  while (this->available()) {
    char c = this->read();
    if (c == '\n' || c == '\r') {
      if (line_pos_ > 0) {
        line_buffer_[line_pos_] = '\0';
        this->process_proxy_line_(line_buffer_);
        line_pos_ = 0;
      }
    } else if (line_pos_ < sizeof(line_buffer_) - 1) {
      line_buffer_[line_pos_++] = c;
    }
  }
}

void GeckoUartTransport::process_proxy_line_(const char *line) {
  ESP_LOGD(TAG, "Proxy: %s", line);

  // RX:<len>:<hex>
  if (strncmp(line, "RX:", 3) == 0) {
    const char *p = line + 3;
    int len = atoi(p);

    // Find the colon after the length
    while (*p != '\0' && *p != ':')
      p++;
    if (*p == ':')
      p++;

    if (len > GECKO_MAX_FRAME_LEN)
      len = GECKO_MAX_FRAME_LEN;

    uint8_t data[GECKO_MAX_FRAME_LEN];
    for (int i = 0; i < len; i++) {
      data[i] = (hex_nibble(p[i * 2]) << 4) | hex_nibble(p[i * 2 + 1]);
    }

    if (len > 0 && frame_callback_)
      frame_callback_(data, (uint8_t) len);
  } else if (strcmp(line, "READY") == 0) {
    ESP_LOGI(TAG, "Arduino proxy ready");
    proxy_ready_ = true;
  } else if (strcmp(line, "I2C_PROXY:V1") == 0) {
    ESP_LOGI(TAG, "Arduino proxy version 1");
  } else if (strcmp(line, "TX:OK") == 0) {
    ESP_LOGD(TAG, "I2C TX acknowledged");
  } else if (strcmp(line, "PONG") == 0) {
    ESP_LOGD(TAG, "Proxy ping OK");
  }
}

void GeckoUartTransport::send_frame(const uint8_t *data, uint8_t len) {
  this->write_str("TX:");
  for (uint8_t i = 0; i < len; i++) {
    char hex[3];
    sprintf(hex, "%02X", data[i]);
    this->write_str(hex);
  }
  this->write_str("\n");
}

void GeckoUartTransport::recover_link() {
  if (reset_pin_ == nullptr) {
    ESP_LOGW(TAG, "Reset pin not configured, cannot reset Arduino");
    return;
  }
  if (reset_in_progress_) {
    ESP_LOGD(TAG, "Reset already in progress");
    return;
  }
  ESP_LOGI(TAG, "Resetting Arduino");
  proxy_ready_ = false;
  reset_pin_->digital_write(false);  // Pull LOW to reset
  reset_start_time_ = millis();
  reset_in_progress_ = true;
}

void GeckoUartTransport::dump_transport_config() {
  ESP_LOGCONFIG(TAG, "  Transport: %s", this->transport_name());
  if (reset_pin_ != nullptr) {
    LOG_PIN("  Reset Pin: ", reset_pin_);
  } else {
    ESP_LOGCONFIG(TAG, "  Reset Pin: not configured");
  }
}

#endif  // USE_GECKO_SPA_UART

#ifdef GECKO_SPA_DIRECT_I2C

// What we hand back when the spa does a repeated-start read of two bytes after
// addressing us. The Arduino proxy answered the same way; the spa only cares
// that something ACKs.
static const uint8_t SLAVE_READ_RESPONSE[2] = {0x00, 0x00};

// ---------------------------------------------------------------------------
// Shared by both backends
// ---------------------------------------------------------------------------

void GeckoI2cTransport::setup_transport() {
  rx_queue_ = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxFrame));
  if (rx_queue_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate I2C receive queue");
    return;
  }
#ifdef USE_GECKO_SPA_I2C_IDF
  slave_lock_ = xSemaphoreCreateMutex();
  if (slave_lock_ == nullptr ||
      xTaskCreate(request_task_, "gecko_i2c_req", 2048, this, REQUEST_TASK_PRIORITY, &request_task_handle_) != pdPASS) {
    ESP_LOGE(TAG, "Could not start the I2C request task");
    return;
  }
#endif
  if (!this->enter_slave_mode_()) {
    ESP_LOGW(TAG, "I2C slave mode not up yet, retrying in the background");
  }
}

bool GeckoI2cTransport::slave_mode_result_(bool ok) {
  slave_active_ = ok;
  if (ok) {
    slave_retry_interval_ = 50;
    ESP_LOGD(TAG, "I2C slave listening on 0x%02X", address_);
  } else {
    // Starting slave mode can fail while the spa is mid-transaction. Back off
    // and try again rather than giving up on the bus.
    slave_retry_interval_ = std::min<uint32_t>(slave_retry_interval_ * 2, 2000);
    last_slave_retry_ = millis();
  }
  return ok;
}

bool GeckoI2cTransport::bus_is_idle_() {
  // Both lines released means no master currently owns the bus. Both backends
  // configure the pins as open-drain input/output, so this reads the wire.
  return gpio_get_level((gpio_num_t) sda_pin_) != 0 && gpio_get_level((gpio_num_t) scl_pin_) != 0;
}

void GeckoI2cTransport::send_frame(const uint8_t *data, uint8_t len) {
  if (len == 0)
    return;
  if (len > TX_FRAME_LEN) {
    ESP_LOGE(TAG, "Refusing to send %u byte frame (max %u)", len, TX_FRAME_LEN);
    return;
  }
  if (tx_count_ >= TX_QUEUE_DEPTH) {
    ESP_LOGW(TAG, "Transmit queue full, dropping frame");
    tx_errors_++;
    return;
  }

  memcpy(tx_queue_[tx_count_], data, len);
  tx_lengths_[tx_count_] = len;
  if (tx_count_ == 0)
    tx_queued_at_ = millis();
  tx_count_++;
}

void GeckoI2cTransport::flush_tx_queue_() {
  // One frame per trip off the bus. The spa answers a command with traffic of
  // its own, which it cannot deliver while we are master; a second frame sent
  // straight after collided with it and was lost - the second of a standby
  // change's three writes, the one that sets the state, most of all. Back in
  // slave mode we take the spa's frames, and the next one goes out later.
  uint32_t started = millis();
  this->leave_slave_mode_();

  esp_err_t err = this->master_begin_();
  if (err == ESP_OK) {
    err = this->master_transfer_(tx_queue_[0], tx_lengths_[0]);
    if (err != ESP_OK) {
      // The spa is another master on this bus, so losing arbitration now and
      // then is normal. One retry after a short backoff often clears it.
      delay(2);
      err = this->master_transfer_(tx_queue_[0], tx_lengths_[0]);
    }
    this->master_end_();
  }

  bool sent = err == ESP_OK;
  tx_attempts_++;
  if (!sent)
    ESP_LOGW(TAG, "I2C transmit failed: %s (try %u of %u)", esp_err_to_name(err), tx_attempts_, TX_MAX_ATTEMPTS);
  bool dropped = !sent && tx_attempts_ >= TX_MAX_ATTEMPTS;
  if (sent || dropped) {
    if (dropped)
      tx_errors_++;
    // Done with the head of the queue: sent, or out of tries
    tx_count_--;
    for (uint8_t i = 0; i < tx_count_; i++) {
      memcpy(tx_queue_[i], tx_queue_[i + 1], tx_lengths_[i + 1]);
      tx_lengths_[i] = tx_lengths_[i + 1];
    }
    tx_attempts_ = 0;
  }
  last_tx_at_ = millis();
  tx_queued_at_ = last_tx_at_;

  bool back_up = this->enter_slave_mode_();
  // The spa is deaf to us for the whole round trip, so it is worth being able
  // to see how long that window actually is on real hardware.
  ESP_LOGD(TAG, "%s 1 frame, off the bus for %" PRIu32 " ms, %u queued",
           sent ? "Sent" : (dropped ? "Dropped" : "Will retry"), last_tx_at_ - started, tx_count_);
  if (!back_up)
    ESP_LOGW(TAG, "Could not return to slave mode after transmit, retrying");
}

void GeckoI2cTransport::loop_transport() {
  // Hand over frames captured by the slave driver
  if (rx_queue_ != nullptr) {
    RxFrame frame;
    while (xQueueReceive(rx_queue_, &frame, 0) == pdTRUE) {
      // The callback may queue a reply (handshake ACKs do); that is flushed
      // below, still within this loop iteration.
      if (frame_callback_)
        frame_callback_(frame.data, frame.len);
    }
  }

  if (tx_count_ > 0 && millis() - last_tx_at_ >= TX_GAP_MS &&
      (this->bus_is_idle_() || millis() - tx_queued_at_ > TX_MAX_DEFER_MS)) {
    this->flush_tx_queue_();
  } else if (!slave_active_ && millis() - last_slave_retry_ >= slave_retry_interval_) {
    this->enter_slave_mode_();
  }
}

void GeckoI2cTransport::recover_link() {
  ESP_LOGI(TAG, "Reinitialising I2C bus");
  this->leave_slave_mode_();
  this->recover_bus_();
  if (!this->enter_slave_mode_())
    ESP_LOGW(TAG, "I2C bus still busy, retrying in the background");
}

void GeckoI2cTransport::dump_transport_config() {
  ESP_LOGCONFIG(TAG, "  Transport: %s", this->transport_name());
  ESP_LOGCONFIG(TAG, "  I2C bus: %u  SDA: GPIO%u  SCL: GPIO%u", bus_num_, sda_pin_, scl_pin_);
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X  Frequency: %" PRIu32 " Hz", address_, frequency_);
  ESP_LOGCONFIG(TAG, "  Slave mode: %s", slave_active_ ? "listening" : "DOWN");
  if (rx_dropped_ != 0 || tx_errors_ != 0) {
    ESP_LOGCONFIG(TAG, "  Dropped frames: %" PRIu32 " rx, %" PRIu32 " tx", rx_dropped_, tx_errors_);
  }
}

#ifdef USE_GECKO_SPA_I2C_ARDUINO_HAL
// ---------------------------------------------------------------------------
// Arduino core HAL backend
// ---------------------------------------------------------------------------

void GeckoI2cTransport::receive_callback_(uint8_t bus_num, uint8_t *data, size_t len, bool stop, void *arg) {
  auto *self = (GeckoI2cTransport *) arg;
  if (self->rx_queue_ == nullptr || data == nullptr || len == 0)
    return;

  RxFrame frame;
  frame.len = (len > GECKO_MAX_FRAME_LEN) ? GECKO_MAX_FRAME_LEN : (uint8_t) len;
  memcpy(frame.data, data, frame.len);

  if (xQueueSend(self->rx_queue_, &frame, 0) != pdTRUE)
    self->rx_dropped_++;
}

void GeckoI2cTransport::request_callback_(uint8_t bus_num, void *arg) {
  auto *self = (GeckoI2cTransport *) arg;
  i2cSlaveWrite(self->bus_num_, SLAVE_READ_RESPONSE, sizeof(SLAVE_READ_RESPONSE), 10);
}

bool GeckoI2cTransport::enter_slave_mode_() {
  i2cSlaveAttachCallbacks(bus_num_, request_callback_, receive_callback_, this);
  esp_err_t err = i2cSlaveInit(bus_num_, sda_pin_, scl_pin_, address_, frequency_, SLAVE_RX_BUFFER_LEN, TX_FRAME_LEN);
  return this->slave_mode_result_(err == ESP_OK);
}

void GeckoI2cTransport::leave_slave_mode_() {
  if (!slave_active_)
    return;
  i2cSlaveDeinit(bus_num_);
  slave_active_ = false;
}

esp_err_t GeckoI2cTransport::master_begin_() { return i2cInit(bus_num_, sda_pin_, scl_pin_, frequency_); }

esp_err_t GeckoI2cTransport::master_transfer_(const uint8_t *data, uint8_t len) {
  // Write then repeated-start read of two bytes: what the spa expects, and
  // what the Arduino proxy did.
  uint8_t response[sizeof(SLAVE_READ_RESPONSE)];
  size_t read_count = 0;
  return i2cWriteReadNonStop(bus_num_, address_, data, len, response, sizeof(response), TX_TIMEOUT_MS, &read_count);
}

void GeckoI2cTransport::master_end_() { i2cDeinit(bus_num_); }

void GeckoI2cTransport::recover_bus_() {
  // Nothing to do here: i2cSlaveInit() checks the lines itself and clocks out
  // the standard nine SCL pulses when it finds them stuck low.
}

#endif  // USE_GECKO_SPA_I2C_ARDUINO_HAL

#ifdef USE_GECKO_SPA_I2C_IDF
// ---------------------------------------------------------------------------
// ESP-IDF I2C slave driver (version 2) backend
// ---------------------------------------------------------------------------

bool IRAM_ATTR GeckoI2cTransport::idf_on_receive_(i2c_slave_dev_handle_t slave,
                                                  const i2c_slave_rx_done_event_data_t *evt, void *arg) {
  auto *self = static_cast<GeckoI2cTransport *>(arg);
  if (self->rx_queue_ == nullptr || evt->buffer == nullptr || evt->length == 0)
    return false;

  // Fires once per transaction: at STOP, or at the repeated START before the
  // spa's two-byte read. The driver reuses evt->buffer, so copy it now.
  RxFrame frame;
  frame.len = (evt->length > GECKO_MAX_FRAME_LEN) ? GECKO_MAX_FRAME_LEN : (uint8_t) evt->length;
  memcpy(frame.data, evt->buffer, frame.len);

  BaseType_t woken = pdFALSE;
  if (xQueueSendFromISR(self->rx_queue_, &frame, &woken) != pdTRUE)
    self->rx_dropped_++;
  return woken == pdTRUE;
}

bool IRAM_ATTR GeckoI2cTransport::idf_on_request_(i2c_slave_dev_handle_t slave,
                                                  const i2c_slave_request_event_data_t *evt, void *arg) {
  auto *self = static_cast<GeckoI2cTransport *>(arg);
  BaseType_t woken = pdFALSE;
  if (self->request_task_handle_ != nullptr)
    vTaskNotifyGiveFromISR(self->request_task_handle_, &woken);
  return woken == pdTRUE;
}

void GeckoI2cTransport::request_task_(void *arg) {
  auto *self = static_cast<GeckoI2cTransport *>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xSemaphoreTake(self->slave_lock_, portMAX_DELAY);
    if (self->slave_handle_ != nullptr) {
      uint32_t written = 0;
      i2c_slave_write(self->slave_handle_, SLAVE_READ_RESPONSE, sizeof(SLAVE_READ_RESPONSE), &written, 10);
    }
    xSemaphoreGive(self->slave_lock_);
  }
}

bool GeckoI2cTransport::enter_slave_mode_() {
  // Without request_task_ nothing would answer the spa's reads, and on chips
  // that stretch SCL at a read that would hold the whole bus. Stay off it.
  if (slave_lock_ == nullptr || request_task_handle_ == nullptr)
    return this->slave_mode_result_(false);

  i2c_slave_config_t config = {};
  config.i2c_port = (i2c_port_num_t) bus_num_;
  config.sda_io_num = (gpio_num_t) sda_pin_;
  config.scl_io_num = (gpio_num_t) scl_pin_;
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.send_buf_depth = SLAVE_TX_BUFFER_LEN;
  config.receive_buf_depth = SLAVE_RX_BUFFER_LEN;
  config.slave_addr = address_;
  config.addr_bit_len = I2C_ADDR_BIT_LEN_7;
  config.flags.enable_internal_pullup = true;

  i2c_slave_dev_handle_t handle = nullptr;
  esp_err_t err = i2c_new_slave_device(&config, &handle);
  if (err == ESP_OK) {
    i2c_slave_event_callbacks_t callbacks = {};
    callbacks.on_request = idf_on_request_;
    callbacks.on_receive = idf_on_receive_;
    err = i2c_slave_register_event_callbacks(handle, &callbacks, this);
    if (err != ESP_OK)
      i2c_del_slave_device(handle);
  }
  if (err != ESP_OK) {
    ESP_LOGD(TAG, "I2C slave start failed: %s", esp_err_to_name(err));
    return this->slave_mode_result_(false);
  }

  xSemaphoreTake(slave_lock_, portMAX_DELAY);
  slave_handle_ = handle;
  // Preload the answer to the spa's first read. Chips without stretch-cause
  // support (the original ESP32) cannot hold the clock while we respond, so
  // the bytes must already be waiting. On the others this is harmless, and it
  // also releases the clock if a read arrived before slave_handle_ was set.
  uint32_t written = 0;
  i2c_slave_write(handle, SLAVE_READ_RESPONSE, sizeof(SLAVE_READ_RESPONSE), &written, 10);
  xSemaphoreGive(slave_lock_);

  return this->slave_mode_result_(true);
}

void GeckoI2cTransport::leave_slave_mode_() {
  if (!slave_active_)
    return;
  xSemaphoreTake(slave_lock_, portMAX_DELAY);
  i2c_slave_dev_handle_t handle = slave_handle_;
  slave_handle_ = nullptr;
  xSemaphoreGive(slave_lock_);
  if (handle != nullptr)
    i2c_del_slave_device(handle);
  slave_active_ = false;
}

esp_err_t GeckoI2cTransport::master_begin_() {
  i2c_master_bus_config_t bus_config = {};
  bus_config.i2c_port = (i2c_port_num_t) bus_num_;
  bus_config.sda_io_num = (gpio_num_t) sda_pin_;
  bus_config.scl_io_num = (gpio_num_t) scl_pin_;
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt = 7;
  bus_config.flags.enable_internal_pullup = true;

  esp_err_t err = i2c_new_master_bus(&bus_config, &master_bus_);
  if (err != ESP_OK) {
    master_bus_ = nullptr;
    return err;
  }

  i2c_device_config_t device_config = {};
  device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device_config.device_address = address_;
  device_config.scl_speed_hz = frequency_;
  err = i2c_master_bus_add_device(master_bus_, &device_config, &master_dev_);
  if (err != ESP_OK) {
    master_dev_ = nullptr;
    this->master_end_();
  }
  return err;
}

esp_err_t GeckoI2cTransport::master_transfer_(const uint8_t *data, uint8_t len) {
  // Write then repeated-start read of two bytes: what the spa expects, and
  // what the Arduino proxy did.
  uint8_t response[sizeof(SLAVE_READ_RESPONSE)];
  return i2c_master_transmit_receive(master_dev_, data, len, response, sizeof(response), (int) TX_TIMEOUT_MS);
}

void GeckoI2cTransport::master_end_() {
  if (master_dev_ != nullptr) {
    i2c_master_bus_rm_device(master_dev_);
    master_dev_ = nullptr;
  }
  if (master_bus_ != nullptr) {
    i2c_del_master_bus(master_bus_);
    master_bus_ = nullptr;
  }
}

void GeckoI2cTransport::recover_bus_() {
  // Unlike the Arduino HAL, the IDF slave driver does not check the lines when
  // it starts. Borrow a master bus for its reset, which clocks out the
  // standard nine SCL pulses to free a slave stuck holding SDA low.
  if (this->master_begin_() != ESP_OK)
    return;
  esp_err_t err = i2c_master_bus_reset(master_bus_);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "I2C bus reset failed: %s", esp_err_to_name(err));
  this->master_end_();
}

#endif  // USE_GECKO_SPA_I2C_IDF

#endif  // GECKO_SPA_DIRECT_I2C

}  // namespace gecko_spa
}  // namespace esphome

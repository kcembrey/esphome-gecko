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
#include <esp32-hal-i2c.h>
#include <esp32-hal-i2c-slave.h>
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

void GeckoI2cTransport::setup_transport() {
  rx_queue_ = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxFrame));
  if (rx_queue_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate I2C receive queue");
    return;
  }
  if (!this->enter_slave_mode_()) {
    ESP_LOGW(TAG, "I2C slave mode not up yet, retrying in the background");
  }
}

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
  slave_active_ = (err == ESP_OK);
  if (slave_active_) {
    slave_retry_interval_ = 50;
    ESP_LOGD(TAG, "I2C slave listening on 0x%02X", address_);
  } else {
    // The HAL refuses to start while the spa is mid-transaction. Back off and
    // try again rather than giving up on the bus.
    slave_retry_interval_ = std::min<uint32_t>(slave_retry_interval_ * 2, 2000);
    last_slave_retry_ = millis();
  }
  return slave_active_;
}

void GeckoI2cTransport::leave_slave_mode_() {
  if (!slave_active_)
    return;
  i2cSlaveDeinit(bus_num_);
  slave_active_ = false;
}

bool GeckoI2cTransport::bus_is_idle_() {
  // Both lines released means no master currently owns the bus. The slave HAL
  // configures the pins as open-drain input/output, so this reads the wire.
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
  uint8_t pending = tx_count_;
  uint32_t started = millis();
  tx_count_ = 0;

  this->leave_slave_mode_();

  esp_err_t err = i2cInit(bus_num_, sda_pin_, scl_pin_, frequency_);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "I2C master init failed (%s), dropped %u frame(s)", esp_err_to_name(err), pending);
    tx_errors_ += pending;
  } else {
    uint8_t response[sizeof(SLAVE_READ_RESPONSE)];
    for (uint8_t i = 0; i < pending; i++) {
      size_t read_count = 0;
      // Write then repeated-start read of two bytes: what the spa expects, and
      // what the Arduino proxy did.
      err = i2cWriteReadNonStop(bus_num_, address_, tx_queue_[i], tx_lengths_[i], response, sizeof(response),
                                TX_TIMEOUT_MS, &read_count);
      if (err != ESP_OK) {
        // The spa is another master on this bus, so losing arbitration now and
        // then is normal. One retry after a short backoff clears it.
        delay(2);
        err = i2cWriteReadNonStop(bus_num_, address_, tx_queue_[i], tx_lengths_[i], response, sizeof(response),
                                  TX_TIMEOUT_MS, &read_count);
      }
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C transmit failed: %s", esp_err_to_name(err));
        tx_errors_++;
      }
    }
    i2cDeinit(bus_num_);
  }

  bool back_up = this->enter_slave_mode_();
  // The spa is deaf to us for the whole round trip, so it is worth being able
  // to see how long that window actually is on real hardware.
  ESP_LOGD(TAG, "Sent %u frame(s), off the bus for %" PRIu32 " ms", pending, millis() - started);
  if (!back_up)
    ESP_LOGW(TAG, "Could not return to slave mode after transmit, retrying");
}

void GeckoI2cTransport::loop_transport() {
  // Hand over frames captured by the HAL's I2C slave task
  if (rx_queue_ != nullptr) {
    RxFrame frame;
    while (xQueueReceive(rx_queue_, &frame, 0) == pdTRUE) {
      // The callback may queue a reply (handshake ACKs do); that is flushed
      // below, still within this loop iteration.
      if (frame_callback_)
        frame_callback_(frame.data, frame.len);
    }
  }

  if (tx_count_ > 0 && (this->bus_is_idle_() || millis() - tx_queued_at_ > TX_MAX_DEFER_MS)) {
    this->flush_tx_queue_();
  } else if (!slave_active_ && millis() - last_slave_retry_ >= slave_retry_interval_) {
    this->enter_slave_mode_();
  }
}

void GeckoI2cTransport::recover_link() {
  ESP_LOGI(TAG, "Reinitialising I2C bus");
  this->leave_slave_mode_();
  // enter_slave_mode_() runs the HAL's bus recovery (nine SCL pulses) when it
  // finds the lines stuck low, which is what a wedged bus needs.
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

#endif  // GECKO_SPA_DIRECT_I2C

}  // namespace gecko_spa
}  // namespace esphome

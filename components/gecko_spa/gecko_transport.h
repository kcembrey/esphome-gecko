#pragma once

#include <cstdint>
#include <functional>

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"

#ifdef USE_GECKO_SPA_UART
#include "esphome/components/uart/uart.h"
#endif

// Talking to the spa bus directly needs the ESP32 I2C peripheral in slave mode,
// which is only exposed by the Arduino framework's HAL.
#if defined(USE_GECKO_SPA_I2C) && defined(USE_ESP32) && defined(USE_ARDUINO)
#define GECKO_SPA_DIRECT_I2C
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#endif

namespace esphome {
namespace gecko_spa {

// The longest frame the spa puts on the wire in one transaction is 78 bytes;
// 128 matches the buffer the Arduino proxy used and leaves plenty of headroom.
static const uint8_t GECKO_MAX_FRAME_LEN = 128;

// A transport moves complete I2C frames between the protocol layer (GeckoSpa)
// and the spa, and hides how they get there: through an Arduino proxy over
// UART, or straight off the ESP32's own I2C peripheral.
class GeckoTransport {
 public:
  // Called with each complete frame received from the spa.
  using FrameCallback = std::function<void(const uint8_t *, uint8_t)>;

  virtual ~GeckoTransport() = default;

  virtual void setup_transport() = 0;
  virtual void loop_transport() = 0;
  virtual void dump_transport_config() = 0;

  // Queue a frame for transmission to the spa.
  virtual void send_frame(const uint8_t *data, uint8_t len) = 0;

  // Re-establish the link after the spa (or the link itself) has gone away.
  virtual void recover_link() = 0;

  // True while recover_link() is still working, so the retry logic in GeckoSpa
  // does not stack recoveries on top of each other.
  virtual bool recovery_in_progress() { return false; }

  // Human readable name used in logs and dump_config().
  virtual const char *transport_name() const = 0;

  void set_frame_callback(FrameCallback callback) { frame_callback_ = std::move(callback); }

 protected:
  FrameCallback frame_callback_{};
};

#ifdef USE_GECKO_SPA_UART

// Legacy transport: an Arduino Nano running the I2C proxy sketch, connected
// over UART. Frames are exchanged as ASCII hex lines ("TX:<hex>", "RX:<len>:<hex>").
class GeckoUartTransport : public GeckoTransport, public uart::UARTDevice {
 public:
  void set_reset_pin(GPIOPin *pin) { reset_pin_ = pin; }

  void setup_transport() override;
  void loop_transport() override;
  void dump_transport_config() override;
  void send_frame(const uint8_t *data, uint8_t len) override;
  void recover_link() override;
  bool recovery_in_progress() override { return reset_in_progress_; }
  const char *transport_name() const override { return "Arduino proxy (UART)"; }

 protected:
  void process_proxy_line_(const char *line);

  GPIOPin *reset_pin_{nullptr};
  bool reset_in_progress_{false};
  uint32_t reset_start_time_{0};
  bool proxy_ready_{false};

  char line_buffer_[512];
  uint16_t line_pos_{0};
};

#endif  // USE_GECKO_SPA_UART

#ifdef GECKO_SPA_DIRECT_I2C

// Direct transport: the ESP32 drives the spa's I2C bus itself through a
// bidirectional level shifter. No Arduino in the middle.
//
// The Gecko bus is multi-master and both ends answer to the same address
// (0x17), so the peripheral cannot stay in one role. It sits in slave mode
// listening to the spa and briefly flips to master mode to push our own frames
// out - the same dance the Arduino proxy performed, just without the Arduino.
class GeckoI2cTransport : public GeckoTransport {
 public:
  void set_sda_pin(uint8_t pin) { sda_pin_ = pin; }
  void set_scl_pin(uint8_t pin) { scl_pin_ = pin; }
  void set_bus_num(uint8_t num) { bus_num_ = num; }
  void set_address(uint8_t address) { address_ = address; }
  void set_frequency(uint32_t frequency) { frequency_ = frequency; }

  void setup_transport() override;
  void loop_transport() override;
  void dump_transport_config() override;
  void send_frame(const uint8_t *data, uint8_t len) override;
  void recover_link() override;
  bool recovery_in_progress() override { return !slave_active_; }
  const char *transport_name() const override { return "Direct I2C"; }

 protected:
  // Frames we send are short - the longest command is 21 bytes - and a
  // handshake burst never queues more than a couple at a time.
  static const uint8_t TX_QUEUE_DEPTH = 6;
  static const uint8_t TX_FRAME_LEN = 32;
  static const uint8_t RX_QUEUE_DEPTH = 6;
  // Slave RX ring inside the HAL. Two max-length frames of headroom.
  static const uint16_t SLAVE_RX_BUFFER_LEN = 256;
  // Never sit on a queued frame longer than this waiting for an idle bus.
  static const uint32_t TX_MAX_DEFER_MS = 500;
  // A 21 byte transaction at 100kHz takes ~2.5ms; this is generous but keeps a
  // dead bus from stalling the ESPHome loop.
  static const uint32_t TX_TIMEOUT_MS = 20;

  struct RxFrame {
    uint8_t len;
    uint8_t data[GECKO_MAX_FRAME_LEN];
  };

  // These run on the HAL's I2C slave task, not on the ESPHome loop task.
  static void receive_callback_(uint8_t bus_num, uint8_t *data, size_t len, bool stop, void *arg);
  static void request_callback_(uint8_t bus_num, void *arg);

  bool enter_slave_mode_();
  void leave_slave_mode_();
  bool bus_is_idle_();
  void flush_tx_queue_();

  uint8_t sda_pin_{0};
  uint8_t scl_pin_{0};
  uint8_t bus_num_{0};
  uint8_t address_{0x17};
  uint32_t frequency_{100000};

  QueueHandle_t rx_queue_{nullptr};
  bool slave_active_{false};
  uint32_t last_slave_retry_{0};
  uint32_t slave_retry_interval_{50};

  uint8_t tx_queue_[TX_QUEUE_DEPTH][TX_FRAME_LEN];
  uint8_t tx_lengths_[TX_QUEUE_DEPTH]{};
  uint8_t tx_count_{0};
  uint32_t tx_queued_at_{0};

  // Diagnostics, surfaced in dump_config().
  uint32_t rx_dropped_{0};
  uint32_t tx_errors_{0};
};

#endif  // GECKO_SPA_DIRECT_I2C

}  // namespace gecko_spa
}  // namespace esphome

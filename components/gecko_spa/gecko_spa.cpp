#include "gecko_spa.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <ctime>

namespace esphome {
namespace gecko_spa {

static const char *const TAG = "gecko_spa";

// GO keep-alive message
const uint8_t GeckoSpa::GO_MESSAGE[15] = {
    0x17, 0x00, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x47, 0x4F  // "GO"
};

void GeckoSpa::setup() {
  ESP_LOGI(TAG, "GeckoSpa starting");
  if (transport_ == nullptr) {
    ESP_LOGE(TAG, "No transport configured");
    this->mark_failed();
    return;
  }
  transport_->set_frame_callback([this](const uint8_t *data, uint8_t len) { this->process_i2c_message(data, len); });
  transport_->setup_transport();
}

void GeckoSpa::dump_config() {
  ESP_LOGCONFIG(TAG, "Gecko Spa:");
  if (transport_ != nullptr)
    transport_->dump_transport_config();
  ESP_LOGCONFIG(TAG, "  Notification date format: %s",
                notif_date_format_ == NotifDateFormat::Y_M_D ? "Y-M-D" : "D-M-Y");
  ESP_LOGCONFIG(TAG, "  Max temperature: %.1f C", max_temperature_);
  if (quiet_configured_) {
    ESP_LOGCONFIG(TAG, "  Quiet time: %02u:%02u-%02u:%02u (released below %.1f C water)", quiet_start_ / 60,
                  quiet_start_ % 60, quiet_end_ / 60, quiet_end_ % 60, quiet_min_water_temp_);
  }
}

void GeckoSpa::loop() {
  if (transport_ == nullptr)
    return;

  transport_->loop_transport();

  // Check connection timeout (1 minute without I2C traffic)
  if (millis() - last_i2c_time_ > 60000) {
    if (connected_) {
      connected_ = false;
      if (connected_sensor_)
        connected_sensor_->publish_state(false);
      ESP_LOGW(TAG, "Spa connection lost (timeout)");
      recovery_retry_count_ = 0;
      this->recover_link();
    } else if (!transport_->recovery_in_progress()) {
      // Not connected and not mid-recovery: retry with backoff
      // Retry intervals: 30s, 60s, 120s, then every 120s (max 5 retries before giving up)
      uint32_t backoff = 30000UL * (1 << std::min(recovery_retry_count_, (uint8_t) 2));
      if (recovery_retry_count_ < 5 && (millis() - last_recovery_time_ > backoff)) {
        ESP_LOGW(TAG, "Link recovery retry %d/%d (backoff %" PRIu32 "s)", recovery_retry_count_ + 1, 5,
                 backoff / 1000);
        this->recover_link();
      }
    }
  }

  this->update_quiet_time_();

  // Send GO keep-alive every 23 seconds (triggers handshake sequence)
  if (millis() - last_go_send_time_ > 23000) {
    last_go_send_time_ = millis();
    send_i2c_message(GO_MESSAGE, 15);
    ESP_LOGD(TAG, "Sent GO keep-alive");
  }
}

void GeckoSpa::send_light_command(bool on) {
  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x33, (uint8_t)(on ? 0x01 : 0x00), 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent light %s command", on ? "ON" : "OFF");
}

void GeckoSpa::send_circ_command(bool on) {
  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x6B, (uint8_t)(on ? 0x01 : 0x00), 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent circ %s command", on ? "ON" : "OFF");
}

void GeckoSpa::send_pump1_command(uint8_t state) {
  // P1 function ID: 0x03
  // State: 0=OFF, 2=ON/HIGH (P1 uses 0x02 for ON, not 0x01)
  uint8_t state_val = (user_demand_state_ & 0xFC) | ((state == 0) ? 0x00 : 0x02);

  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x03, state_val, 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent P1 state=%d command (val=0x%02X)", state, state_val);
}

void GeckoSpa::send_pump2_command(uint8_t state) {
  // P2 function ID: 0x03
  // State: 0=OFF, 8=ON/HIGH (P2 uses 0x08 for ON, not 0x01)
  uint8_t state_val = (user_demand_state_ & 0xF3) | ((state == 0) ? 0x00 : 0x08);

  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x03, state_val, 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent P2 state=%d command (val=0x%02X)", state, state_val);
}

void GeckoSpa::send_pump3_command(uint8_t state) {
  // P3 function ID: 0x03
  // State: 0=OFF, 32=ON/HIGH (P3 uses 0x20 for ON, not 0x01)
  uint8_t state_val = (user_demand_state_ & 0xCF) | ((state == 0) ? 0x00 : 0x20);

  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x03, state_val, 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent P3 state=%d command (val=0x%02X) [EXPERIMENTAL]", state, state_val);
}

void GeckoSpa::send_pump4_command(uint8_t state) {
  // P4 function ID: 0x03
  // State: 0=OFF, 128=ON/HIGH (P4 uses 0x80 for ON, not 0x01)
  uint8_t state_val = (user_demand_state_ & 0x3F) | ((state == 0) ? 0x00 : 0x80);

  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      0x01, 0x03, state_val, 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
  ESP_LOGI(TAG, "Sent P4 state=%d command (val=0x%02X) [EXPERIMENTAL]", state, state_val);
}


void GeckoSpa::send_program_command(uint8_t prog) {
  if (prog > 4)
    return;
  uint8_t cmd[18] = {
      0x17, 0x0B, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x04, 0x4E, 0x03, 0xD0,
      prog, 0x00};
  cmd[17] = calc_checksum(cmd, 18);
  send_i2c_message(cmd, 18);
  ESP_LOGI(TAG, "Sent program %d command", prog);
}

void GeckoSpa::send_temperature_command(float temp_c) {
  if (temp_c < MIN_TEMPERATURE || temp_c > max_temperature_) {
    ESP_LOGW(TAG, "Ignoring setpoint %.1f C, outside %.1f-%.1f C", temp_c, MIN_TEMPERATURE, max_temperature_);
    return;
  }
  // Writes SetpointG (config offset 0x0001): a big-endian word in 1/18 C.
  // The high byte is not always 0x02 - anything under 28.5 C is 0x01xx.
  uint16_t raw = (uint16_t) lroundf(temp_c * 18.0f);
  uint8_t cmd[21] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x07, 0x46, config_version_, status_version_,
      0x00, 0x01, (uint8_t) (raw >> 8), (uint8_t) (raw & 0xFF), 0x00};
  cmd[20] = calc_checksum(cmd, 21);
  send_i2c_message(cmd, 21);
  ESP_LOGI(TAG, "Sent temperature %.1f command (raw=%04X)", temp_c, raw);
}

void GeckoSpa::request_status() {
  // A GO makes the spa restart the handshake and push a fresh status dump.
  last_go_send_time_ = millis();
  send_i2c_message(GO_MESSAGE, 15);
  ESP_LOGI(TAG, "Requested status refresh");
}

void GeckoSpa::recover_link() {
  if (transport_ == nullptr)
    return;
  last_recovery_time_ = millis();
  recovery_retry_count_++;
  transport_->recover_link();
}

uint8_t GeckoSpa::calc_checksum(const uint8_t *data, uint8_t len) {
  uint8_t xor_val = 0;
  for (uint8_t i = 0; i < len - 1; i++) {
    xor_val ^= data[i];
  }
  return xor_val;
}

void GeckoSpa::send_i2c_message(const uint8_t *data, uint8_t len) {
  if (transport_ != nullptr)
    transport_->send_frame(data, len);
}

void GeckoSpa::process_i2c_message(const uint8_t *data, uint8_t len) {
  // Any I2C message means we're connected
  last_i2c_time_ = millis();
  if (!connected_) {
    connected_ = true;
    recovery_retry_count_ = 0;
    if (connected_sensor_)
      connected_sensor_->publish_state(true);
    ESP_LOGI(TAG, "Spa connected (I2C traffic detected)");
  }

  // Log standalone messages as FULL-RX (not continuation parts of multi-part messages)
  // Continuation flag is byte[9]: 0x01 = more coming
  bool is_continuation = (len >= 10 && data[9] == 0x01);
  if (!is_continuation && msg_buffer_len_ == 0 && len > 0) {
    // Split into 32 bytes per line (64 hex characters)
    const int CHUNK_BYTES = 32;
    char hex_str[68];
    ESP_LOGI(TAG, "FULL-RX:%d bytes", len);
    for (int offset = 0; offset < len; offset += CHUNK_BYTES) {
      int chunk_len = (len - offset < CHUNK_BYTES) ? (len - offset) : CHUNK_BYTES;
      int pos = 0;
      for (int i = 0; i < chunk_len; i++) {
        pos += sprintf(hex_str + pos, "%02X", data[offset + i]);
      }
      ESP_LOGI(TAG, "  %03d: %s", offset, hex_str);
    }
  }

  // GO message (15 bytes, ends with "GO") - just log it
  if (len == 15 && data[13] == 0x47 && data[14] == 0x4F) {
    ESP_LOGD(TAG, "Received GO message from spa");
    return;
  }

  // Handshake acknowledgment response
  static const uint8_t ACK_MESSAGE[15] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02
  };

  // 33-byte config file message - parse XML filename and send acknowledgment
  if (len == 33) {
    // Extract XML filename from bytes 16-28 (null-terminated string)
    char xml_name[16];
    int pos = 0;
    for (int i = 16; i < 29 && data[i] != 0 && pos < 15; i++) {
      xml_name[pos++] = (char)data[i];
    }
    xml_name[pos] = '\0';

    ESP_LOGI(TAG, "Handshake XML: %s", xml_name);

    // Parse version number from filename (e.g., inYT_C82.xml -> 82)
    // Look for _C or _S followed by digits
    const char *ver_ptr = strstr(xml_name, "_C");
    if (ver_ptr == nullptr) ver_ptr = strstr(xml_name, "_S");
    if (ver_ptr != nullptr) {
      int version = atoi(ver_ptr + 2);

      // Publish to appropriate sensor and store version
      if (strstr(xml_name, "_C") != nullptr) {
        config_version_ = version;
        if (config_version_sensor_)
          config_version_sensor_->publish_state(xml_name);
        ESP_LOGI(TAG, "Config version: %d", config_version_);
      } else if (strstr(xml_name, "_S") != nullptr) {
        status_version_ = version;
        if (status_version_sensor_)
          status_version_sensor_->publish_state(xml_name);
        ESP_LOGI(TAG, "Status version: %d", status_version_);

        // Select appropriate offsets based on status version
        if (status_version_ <= 50) {
          log_offsets_ = &GECKO_LOG_OFFSETS_V50;
          ESP_LOGI(TAG, "Using v50 log offsets");
        } else {
          log_offsets_ = &GECKO_LOG_OFFSETS_V51;
          ESP_LOGI(TAG, "Using v51+ log offsets");
        }
      }
    }

    send_i2c_message(ACK_MESSAGE, 15);
    return;
  }

  // 22-byte clock message - parse time and send acknowledgment
  if (len == 22 && data[13] == 0x4B) {  // 0x4B = 'K'
    // Time format: [15]=Day [16]=Month [17]=DayOfWeek [18]=Hour [19]=Min [20]=Sec
    uint8_t day = data[15];
    uint8_t month = data[16];
    uint8_t hour = data[18];
    uint8_t minute = data[19];
    uint8_t second = data[20];

    ESP_LOGD(TAG, "Spa clock: %02d/%02d %02d:%02d:%02d", day, month, hour, minute, second);

    if (spa_time_sensor_) {
      char time_str[20];
      // Some packs (an inYT C65/S66) keep no date and send 0xFF for it
      if (day == 0xFF || month == 0xFF) {
        snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", hour, minute, second);
      } else {
        snprintf(time_str, sizeof(time_str), "%02d/%02d %02d:%02d:%02d", day, month, hour, minute, second);
      }
      spa_time_sensor_->publish_state(time_str);
    }

    send_i2c_message(ACK_MESSAGE, 15);
    return;
  }

  // 15-byte "LO" message - handshake complete
  if (len == 15 && data[13] == 0x4C && data[14] == 0x4F) {
    ESP_LOGI(TAG, "Received LO message - handshake complete");
    return;
  }

  // Notification message (77 bytes with byte[6]=0x0B)
  if (len == 77 && data[6] == 0x0B) {
    ESP_LOGD(TAG, "77-byte notification message");
    parse_notification_message(data);
    return;
  }

  // Program status (18 bytes)
  if (len == 18) {
    ESP_LOGI(TAG, "18-byte msg: [1]=%02X [16]=%02X", data[1], data[16]);
    uint8_t prog = data[16];
    if (prog <= 4 && prog != program_id_) {
      program_id_ = prog;
      ESP_LOGI(TAG, "Program from spa: %d", prog);
      if (program_select_) {
        static const char *prog_names[] = {"Away", "Standard", "Energy", "Super Energy", "Weekend"};
        program_select_->publish_state(prog_names[prog]);
      }
    }
    return;
  }

  // Multi-part message handling using byte[9] as continuation flag
  // byte[9] == 0x01: more parts coming, byte[9] == 0x00: last part
  // Header is 16 bytes: [0-13]=protocol header + [14-15]=config/status versions
  // from the handshake (41 42 for inYT_C65/S66; the 52 51 an inYT_C82/S81 pack
  // sends happens to read as ASCII "RQ")
  // Stripping 16 bytes aligns payload with geckolib struct offsets
  // Only concatenate messages with byte[1]=0x09 (config/status type)
  static const int HEADER_LEN = 16;

  if (len >= HEADER_LEN && data[1] == 0x09) {
    bool more_coming = (data[9] == 0x01);

    // Some packs send the same part several times in a row (an inYT C65/S66
    // repeats one part about ten times). Appending every copy inflates the
    // message past any size the parser recognises, so the whole message was
    // thrown away. Skip a part identical to the one just appended. Only
    // mid-message: standalone messages are always handled, so a resent
    // handshake message still gets its ACK.
    if (msg_buffer_len_ > 0 && len == last_part_len_ && memcmp(data, last_part_, len) == 0) {
      repeats_dropped_++;
      return;
    }
    memcpy(last_part_, data, len);
    last_part_len_ = len;

    char header_hex[HEADER_LEN * 2 + 1];
    for (int i = 0; i < HEADER_LEN; i++)
      sprintf(header_hex + i * 2, "%02X", data[i]);

    // Add this part to buffer (strip 16-byte header)
    int payload_start = HEADER_LEN;
    int payload_len = len - payload_start;
    if (msg_buffer_len_ + payload_len <= sizeof(msg_buffer_)) {
      if (part_count_ < MAX_PARTS)
        part_starts_[part_count_++] = msg_buffer_len_;
      memcpy(msg_buffer_ + msg_buffer_len_, data + payload_start, payload_len);
      msg_buffer_len_ += payload_len;
    }

    if (more_coming) {
      ESP_LOGD(TAG, "Message part (%d bytes), more coming. Buffer now %d bytes. Header %s", len, msg_buffer_len_,
               header_hex);
      return;
    }
    ESP_LOGD(TAG, "Last message part (%d bytes). Header %s", len, header_hex);
    if (repeats_dropped_ > 0)
      ESP_LOGD(TAG, "Skipped %u repeated part(s) of this message", repeats_dropped_);

    // Last part received - log complete message in FULL-RX format
    // Split into 32 bytes per line (64 hex characters)
    const int CHUNK_BYTES = 32;
    char hex_str[68];
    int total_bytes = msg_buffer_len_;
    ESP_LOGI(TAG, "FULL-RX:%d bytes", total_bytes);
    for (int offset = 0; offset < total_bytes; offset += CHUNK_BYTES) {
      int chunk_len = (total_bytes - offset < CHUNK_BYTES) ? (total_bytes - offset) : CHUNK_BYTES;
      int pos = 0;
      for (int i = 0; i < chunk_len; i++) {
        pos += sprintf(hex_str + pos, "%02X", msg_buffer_[offset + i]);
      }
      ESP_LOGI(TAG, "  %03d: %s", offset, hex_str);
    }

    // Check message type by size
    // ~162 bytes = status-only message (3 parts: 78+78+54 - 3*16 headers)
    // ~390 bytes = config+status message (8 parts with log section starting at offset 230)

    // We expect status message to be around 150 bytes long, but it varies per spa pack model
    // and software version.  The length will be consistent per pack, so once we've detected
    // it we can assume that all future messages at that length are status messages.  Assume
    // that anything between MIN_STATUS_MSG_LEN and 170 bytes long, with a 0 byte at offset 1,
    // is a status message if we don't yet know the correct length.
    if (status_msg_len_ == 0) {
      if ((msg_buffer_len_ >= MIN_STATUS_MSG_LEN) &&
          (msg_buffer_len_ <= 170) &&
          (msg_buffer_[1] == 0x00)) {
        ESP_LOGI(TAG, "Auto-detect %d as the standard status message length", msg_buffer_len_);
        status_msg_len_ = msg_buffer_len_;
      }
    }

    // Check message type by size
    // 162 bytes = status-only message (3 parts: 78+78+54 - 3*16 headers)
    // ~390 bytes = config+status message (8 parts with log section starting at offset 230)
    if ((msg_buffer_len_ == status_msg_len_) &&
        (status_msg_len_ != 0) &&
        (msg_buffer_[1] == 0x00)) {
      // Status-only message (162 bytes)
      ESP_LOGI(TAG, "Status msg (%db): [3]=%02X [5]=%02X [21-24]=%02X%02X%02X%02X [53]=%02X",
               msg_buffer_len_,
               msg_buffer_[3], msg_buffer_[5],
               msg_buffer_[21], msg_buffer_[22], msg_buffer_[23], msg_buffer_[24], msg_buffer_[53]);
      parse_status_message(msg_buffer_);
    } else if (msg_buffer_len_ >= 300 && msg_buffer_len_ <= 400) {
      // Config+status message (~390 bytes)
      // Config section has +2 byte offset (geckolib offset N → message byte N+2)
      static const int CFG_OFFSET = 2;  // Config struct offset

      // Parse config section (with +2 offset from geckolib struct definitions)
      // Geckolib offsets → message bytes: N → N+2
      uint8_t config_num = msg_buffer_[CFG_OFFSET + 0];
      uint16_t setpoint_raw = (msg_buffer_[CFG_OFFSET + 1] << 8) | msg_buffer_[CFG_OFFSET + 2];
      float setpoint_c = setpoint_raw / 18.0f;
      uint8_t filt_freq = msg_buffer_[CFG_OFFSET + 3];
      uint8_t temp_units = msg_buffer_[CFG_OFFSET + 33];  // 0=F, 1=C
      uint8_t time_format = msg_buffer_[CFG_OFFSET + 34]; // 0=NA, 1=AmPm, 2=24h
      uint8_t pump_timeout = msg_buffer_[CFG_OFFSET + 54];
      uint8_t light_timeout = msg_buffer_[CFG_OFFSET + 55];
      uint8_t econ_type = msg_buffer_[CFG_OFFSET + 70];   // 0=Standard, 1=Night
      uint8_t customer_id = msg_buffer_[CFG_OFFSET + 111];
      uint8_t num_zones = msg_buffer_[CFG_OFFSET + 127];
      uint8_t silent_mode = msg_buffer_[CFG_OFFSET + 157]; // 0=NA, 1=OFF, 2=ECONOMY, 3=SLEEP, 4=NIGHT

      static const char* time_fmt_str[] = {"NA", "AmPm", "24h"};
      static const char* silent_str[] = {"NA", "OFF", "ECONOMY", "SLEEP", "NIGHT"};
      static const char* econ_str[] = {"Standard", "Night"};

      ESP_LOGI(TAG, "Config: Ver=%d Setpoint=%.1f%s FiltFreq=%d TimeFormat=%s",
               config_num, setpoint_c, temp_units == 1 ? "C" : "F", filt_freq,
               time_format < 3 ? time_fmt_str[time_format] : "?");
      ESP_LOGI(TAG, "Config: PumpTimeout=%dmin LightTimeout=%dmin EconType=%s",
               pump_timeout, light_timeout,
               econ_type < 2 ? econ_str[econ_type] : "?");
      ESP_LOGI(TAG, "Config: CustomerID=%d Zones=%d SilentMode=%s",
               customer_id, num_zones,
               silent_mode < 5 ? silent_str[silent_mode] : "?");

      // The spa appends a full status block to this message, so the keep-alive
      // refreshes status even when nothing has been commanded. If we have not
      // seen a standalone status yet, learn its length here: it is the tail
      // that starts at a part boundary and looks like a status message.
      if (status_msg_len_ == 0) {
        for (int i = part_count_ - 1; i >= 0; i--) {
          uint16_t tail = msg_buffer_len_ - part_starts_[i];
          if (tail > 170)
            break;
          if (tail >= MIN_STATUS_MSG_LEN && msg_buffer_[part_starts_[i] + 1] == 0x00) {
            ESP_LOGI(TAG, "Auto-detect %d as the standard status message length (from config message)", tail);
            status_msg_len_ = tail;
            break;
          }
        }
      }

      if (status_msg_len_ != 0 && msg_buffer_len_ > status_msg_len_) {
        // Status portion is the very end of the message
        int STATUS_OFFSET = msg_buffer_len_ - status_msg_len_;
        // It starts a part of its own. An inYT C82/S81 pack also always leaves
        // 0x3B just before it - really the previous part's checksum, which
        // varies on other packs (0xD4/0xD5 on an inYT C65/S66) - so accept
        // either sign.
        bool starts_part = this->is_part_start_(STATUS_OFFSET);
        if ((starts_part || msg_buffer_[STATUS_OFFSET - 1] == 0x3B) && msg_buffer_[STATUS_OFFSET + 1] == 0) {
          parse_status_message(&msg_buffer_[STATUS_OFFSET]);
        }
      }
    }

    // Reset buffer for next message
    msg_buffer_len_ = 0;
    last_part_len_ = 0;
    repeats_dropped_ = 0;
    part_count_ = 0;
    return;
  }

  // Short messages (< 11 bytes) - log them
  if (len > 2) {
    char hex_str[64];
    int pos = 0;
    for (int i = 0; i < len && pos < 60; i++) {
      pos += sprintf(hex_str + pos, "%02X", data[i]);
    }
    ESP_LOGI(TAG, "Short msg (%d bytes): %s", len, hex_str);
  }
}

void GeckoSpa::parse_status_message(const uint8_t *data) {
  // Convert geckolib offset to message byte: byte = geckolib_offset - 254
  // This accounts for the +2 byte misalignment between geckolib structs and actual message
  auto toB = [](uint16_t geckoOffset) -> uint16_t { return geckoOffset - 254; };

  // Get offsets from version-specific struct
  const GeckoLogOffsets &off = *log_offsets_;

  // Calculate message byte positions
  uint16_t b_hours = toB(off.hours);
  uint16_t b_quietState = toB(off.quietState);
  uint16_t b_udP1 = toB(off.udP1);
  uint16_t b_deviceStatus = toB(off.deviceStatus);
  uint16_t b_p1 = toB(off.p1);
  uint16_t b_udLi = toB(off.udLi);
  uint16_t b_realSetPoint = toB(off.realSetPointG);
  uint16_t b_displayedTemp = toB(off.displayedTempG);
  uint16_t b_lockMode = toB(off.lockMode);
  uint16_t b_packType = toB(off.packType);
  uint16_t b_udPumpTime = toB(off.udPumpTime);

  // === Decode all fields from geckolib-compatible offsets ===

  // Hours counter
  uint8_t hours = data[b_hours];

  // QuietState: 0=NOT_SET, 1=DRAIN, 2=SOAK, 3=OFF
  uint8_t quietState = data[b_quietState];
  static const char* quiet_str[] = {"NOT_SET", "DRAIN", "SOAK", "OFF"};

  // User demand P1-P4 (2-bit fields in UdP1 byte)
  user_demand_state_ = data[b_udP1];
  uint8_t udP1 = (user_demand_state_ >> 0) & 0x03;  // bits 0-1
  uint8_t udP2 = (user_demand_state_ >> 2) & 0x03;  // bits 2-3
  uint8_t udP3 = (user_demand_state_ >> 4) & 0x03;  // bits 4-5
  uint8_t udP4 = (user_demand_state_ >> 6) & 0x03;  // bits 6-7
  static const char* pump_ud_str[] = {"OFF", "LO", "HI", "?"};

  // Device status byte (CP, BL, Heater, Waterfall)
  uint8_t devStatus = data[b_deviceStatus];
  bool cp_on = (devStatus >> 2) & 0x01;      // bit 2: CP (circulation pump)
  bool bl_on = (devStatus >> 1) & 0x01;      // bit 1: BL (blower)
  bool heater_on = (devStatus >> 5) & 0x01;  // bit 5: MSTR_HEATER
  bool waterfall = (devStatus >> 7) & 0x01;  // bit 7: Waterfall

  // P1-P4 device status (2-bit fields)
  uint8_t p1_raw = data[b_p1];
  uint8_t p1_state = (p1_raw >> 0) & 0x03;  // bits 0-1: 0=OFF, 1=HIGH, 2=LOW
  uint8_t p2_state = (p1_raw >> 2) & 0x03;  // bits 2-3
  uint8_t p3_state = (p1_raw >> 4) & 0x03;  // bits 4-5
  uint8_t p4_state = (p1_raw >> 6) & 0x03;  // bits 6-7
  static const char* pump_state_str[] = {"OFF", "HIGH", "LOW", "?"};

  // Light user demand
  uint8_t udLi = data[b_udLi];

  // Lock mode: 0=UNLOCK, 1=PARTIAL, 2=FULL
  uint8_t lockMode = data[b_lockMode];
  static const char* lock_str[] = {"UNLOCK", "PARTIAL", "FULL"};

  // Pack type
  uint8_t packType = data[b_packType];
  static const char* pack_str[] = {"Unknown", "inXE", "MasIBC", "MIA", "DJS4", "inClear", "inXM", "K600", "inTerface", "inTouch", "inYT"};

  // Pump timer countdown
  uint8_t pumpTime = data[b_udPumpTime];

  // Temperature (word values, big-endian)
  uint16_t target_raw = (data[b_realSetPoint] << 8) | data[b_realSetPoint + 1];
  uint16_t actual_raw = (data[b_displayedTemp] << 8) | data[b_displayedTemp + 1];
  float target_temp = target_raw / 18.0f;
  float actual_temp = actual_raw / 18.0f;

  if (off.udQuietTime != 0)
    ud_quiet_time_ = data[toB(off.udQuietTime)];

  // === Log decoded status (geckolib format) ===
  ESP_LOGI(TAG, "Status[v%d]: Hours=%d QuietState=%s (%u min) LockMode=%s PackType=%s",
           status_version_, hours,
           quietState < 4 ? quiet_str[quietState] : "?", ud_quiet_time_,
           lockMode < 3 ? lock_str[lockMode] : "?",
           packType < 11 ? pack_str[packType] : "?");

  ESP_LOGI(TAG, "Status: Temp=%.1f/%.1f°C Heater=%s CP=%s BL=%s Waterfall=%s",
           target_temp, actual_temp,
           heater_on ? "ON" : "OFF",
           cp_on ? "ON" : "OFF",
           bl_on ? "ON" : "OFF",
           waterfall ? "ON" : "OFF");

  ESP_LOGI(TAG, "Status: P1=%s P2=%s P3=%s P4=%s PumpTimer=%dmin",
           pump_state_str[p1_state], pump_state_str[p2_state],
           pump_state_str[p3_state], pump_state_str[p4_state],
           pumpTime);

  ESP_LOGI(TAG, "Status: UdP1=%s UdP2=%s UdP3=%s UdP4=%s UdLi=%s",
           pump_ud_str[udP1], pump_ud_str[udP2],
           pump_ud_str[udP3], pump_ud_str[udP4],
           udLi ? "ON" : "OFF");

  // === Update internal state and entities ===

  // Derive states for Home Assistant entities
  bool new_standby = (quietState == 0x03);  // OFF in QuietState means standby
  bool new_circ = cp_on;              // Circulation pump from device status (CP)
  bool new_waterfall = waterfall;     // Waterfall from device status
  bool new_blower = bl_on;            // Blower from device status (BL)
  bool new_heating = heater_on;       // Heater from device status
  bool new_light = (udLi != 0);       // Light from UdLi (user demand light)

  // P1-P4 individual pump states (0=OFF, 1=HIGH, 2=LOW)
  uint8_t new_p1 = p1_state;
  uint8_t new_p2 = p2_state;
  uint8_t new_p3 = p3_state;
  uint8_t new_p4 = p4_state;

  float new_target = target_temp;
  float new_actual = actual_temp;
  bool temp_valid = (target_raw != 0 || actual_raw != 0);

  // On first status message, publish all states
  bool first = !first_status_received_;
  if (first) {
    first_status_received_ = true;
    ESP_LOGI(TAG, "First status received, publishing all states");
  }

  // Update entities on change (or first message)
  if (first || new_light != light_state_) {
    light_state_ = new_light;
    ESP_LOGI(TAG, "Light: %s", light_state_ ? "ON" : "OFF");
    if (light_switch_)
      light_switch_->publish_state(light_state_);
  }

  if (first || new_circ != circ_state_) {
    circ_state_ = new_circ;
    ESP_LOGI(TAG, "Circulation: %s", circ_state_ ? "ON" : "OFF");
    if (circ_switch_)
      circ_switch_->publish_state(circ_state_);
  }

  if (first || new_waterfall != waterfall_state_) {
    waterfall_state_ = new_waterfall;
    ESP_LOGI(TAG, "Waterfall: %s", waterfall_state_ ? "ON" : "OFF");
    if (waterfall_sensor_)
      waterfall_sensor_->publish_state(waterfall_state_);
  }

  if (first || new_blower != blower_state_) {
    blower_state_ = new_blower;
    ESP_LOGI(TAG, "Blower: %s", blower_state_ ? "ON" : "OFF");
    if (blower_sensor_)
      blower_sensor_->publish_state(blower_state_);
  }

  if (first || new_heating != heating_state_) {
    heating_state_ = new_heating;
    ESP_LOGI(TAG, "Heating: %s", heating_state_ ? "ON" : "OFF");
    update_climate_state();
  }

  if (first || new_standby != standby_state_) {
    standby_state_ = new_standby;
    ESP_LOGI(TAG, "Standby: %s", standby_state_ ? "ON" : "OFF");
    if (standby_sensor_)
      standby_sensor_->publish_state(standby_state_);
  }

  // Update LockMode sensor
  if (first || lockMode != lock_mode_) {
    lock_mode_ = lockMode;
    if (lock_mode_sensor_) {
      lock_mode_sensor_->publish_state(lockMode < 3 ? lock_str[lockMode] : "?");
    }
  }

  // Update PackType sensor
  if (first || packType != pack_type_) {
    pack_type_ = packType;
    if (pack_type_sensor_) {
      pack_type_sensor_->publish_state(packType < 11 ? pack_str[packType] : "?");
    }
  }

  // Update PumpTimer sensor
  if (first || pumpTime != pump_timer_) {
    pump_timer_ = pumpTime;
    if (pump_timer_sensor_) {
      pump_timer_sensor_->publish_state(pumpTime);
    }
  }

  // What the active program is doing. With FilterAccess = REMOTE (a pack
  // whose filtration a water-care program runs) the program starts and
  // stops filtration through these fields.
  if (off.remoteFiltAction != 0) {
    static const char *const filt_str[] = {"Idle", "Stop", "Start", "New", "Active"};
    uint8_t action = data[toB(off.remoteFiltAction)];
    uint16_t dur_at = toB(off.remoteFiltDur);
    uint16_t time_left = data[dur_at] * 60 + data[dur_at + 1];
    bool econ = data[toB(off.econActive)] & 0x04;

    if (first || action != filt_action_) {
      filt_action_ = action;
      ESP_LOGI(TAG, "Filtration: %s", action < 5 ? filt_str[action] : "?");
      if (filtration_sensor_)
        filtration_sensor_->publish_state(action < 5 ? filt_str[action] : "?");
    }
    if (first || time_left != filt_time_left_) {
      filt_time_left_ = time_left;
      if (filter_time_left_sensor_)
        filter_time_left_sensor_->publish_state(time_left);
    }
    if (first || econ != econ_active_) {
      econ_active_ = econ;
      ESP_LOGI(TAG, "Economy: %s", econ ? "ON" : "OFF");
      if (economy_sensor_)
        economy_sensor_->publish_state(econ);
    }
  }

  // Only update temperature if valid data was received
  // std::fabs: a bare abs() can resolve to the int overload and truncate, so
  // changes under a whole degree would never be published.
  if (temp_valid &&
      (first || std::fabs(new_target - target_temp_) > 0.1f || std::fabs(new_actual - actual_temp_) > 0.1f)) {
    target_temp_ = new_target;
    actual_temp_ = new_actual;
    temps_known_ = true;
    ESP_LOGI(TAG, "Temp: target=%.1f actual=%.1f", target_temp_, actual_temp_);
    update_climate_state();
  }

  // Update P1-P4 pump states (all controllable switches)
  if (first || new_p1 != pump1_state_) {
    pump1_state_ = new_p1;
    if (pump1_switch_)
      pump1_switch_->publish_state(pump1_state_ != 0);
  }
  if (first || new_p2 != pump2_state_) {
    pump2_state_ = new_p2;
    if (pump2_switch_)
      pump2_switch_->publish_state(pump2_state_ != 0);
  }
  if (first || new_p3 != pump3_state_) {
    pump3_state_ = new_p3;
    if (pump3_switch_)
      pump3_switch_->publish_state(pump3_state_ != 0);
  }
  if (first || new_p4 != pump4_state_) {
    pump4_state_ = new_p4;
    if (pump4_switch_)
      pump4_switch_->publish_state(pump4_state_ != 0);
  }
}

void GeckoSpa::update_climate_state() {
  // Until a status message has given us real temperatures there is nothing
  // true to publish (the members still hold 0 C).
  if (!climate_ || !temps_known_)
    return;

  climate_->target_temperature = target_temp_;
  climate_->current_temperature = actual_temp_;

  // A spa only heats: HEAT is its one mode, and the action says whether the
  // heater is running right now.
  climate_->mode = climate::CLIMATE_MODE_HEAT;
  climate_->action = heating_state_ ? climate::CLIMATE_ACTION_HEATING : climate::CLIMATE_ACTION_IDLE;

  climate_->publish_state();
}

bool GeckoSpa::is_part_start_(uint16_t offset) const {
  for (uint8_t i = 0; i < part_count_; i++) {
    if (part_starts_[i] == offset)
      return true;
  }
  return false;
}

void GeckoSpa::write_value_(uint16_t position, uint8_t value) {
  // geckolib's SET_VALUE (0x46): write one byte at a pack memory position -
  // the same command the light, pump and setpoint use.
  uint8_t cmd[20] = {
      0x17, 0x0A, 0x00, 0x00, 0x00, 0x17, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x06, 0x46, config_version_, status_version_,
      (uint8_t) (position >> 8), (uint8_t) (position & 0xFF), value, 0x00};
  cmd[19] = calc_checksum(cmd, 20);
  send_i2c_message(cmd, 20);
}

void GeckoSpa::pause_quiet_time(uint32_t minutes) {
  if (!quiet_configured_) {
    ESP_LOGW(TAG, "Quiet time is not configured");
    return;
  }
  if (minutes == 0) {
    quiet_paused_ = false;
    ESP_LOGI(TAG, "Quiet time resumed");
  } else {
    quiet_paused_ = true;
    quiet_pause_until_ = millis() + minutes * 60000UL;
    ESP_LOGI(TAG, "Quiet time paused for %" PRIu32 " min", minutes);
  }
  // Act now rather than on the next periodic check
  last_quiet_check_ = 0;
  last_quiet_command_ = 0;
  this->update_quiet_time_();
}

bool GeckoSpa::in_quiet_window_() {
#ifdef USE_GECKO_SPA_QUIET_TIME
  if (quiet_clock_ == nullptr)
    return false;
  auto now = quiet_clock_->now();
  // No valid time yet: do not hold the spa off on a guess
  if (!now.is_valid())
    return false;
  uint16_t minute = now.hour * 60 + now.minute;
  if (quiet_start_ < quiet_end_)
    return minute >= quiet_start_ && minute < quiet_end_;
  return minute >= quiet_start_ || minute < quiet_end_;  // Window crosses midnight
#else
  return false;
#endif
}

void GeckoSpa::update_quiet_time_() {
  if (!quiet_configured_)
    return;
  uint32_t now = millis();
  if (last_quiet_check_ != 0 && now - last_quiet_check_ < 5000)
    return;
  last_quiet_check_ = now;

  if (quiet_paused_ && (int32_t) (quiet_pause_until_ - now) <= 0) {
    quiet_paused_ = false;
    ESP_LOGI(TAG, "Quiet time pause over");
  }

  bool in_window = this->in_quiet_window_();
  if (!in_window) {
    quiet_cold_hold_ = false;  // Each night starts fresh
  } else if (!quiet_cold_hold_ && temps_known_ && actual_temp_ < quiet_min_water_temp_) {
    // Standby also stops the heater, so let the spa look after itself
    quiet_cold_hold_ = true;
    ESP_LOGW(TAG, "Water at %.1f C is below %.1f C: quiet time off for the rest of tonight", actual_temp_,
             quiet_min_water_temp_);
  }

  bool want = in_window && !quiet_paused_ && !quiet_cold_hold_;
  if (want != quiet_wanted_) {
    quiet_wanted_ = want;
    ESP_LOGI(TAG, "Quiet time %s", want ? "on" : "off");
    if (quiet_time_sensor_)
      quiet_time_sensor_->publish_state(want);
  }

  const GeckoLogOffsets &off = *log_offsets_;
  if (off.udQuietTime == 0) {
    if (!quiet_unmapped_warned_) {
      ESP_LOGW(TAG, "Quiet time is not supported for status version %d", status_version_);
      quiet_unmapped_warned_ = true;
    }
    return;
  }

  // Only act on what the spa has reported, and once the handshake has given
  // us the versions every write must carry
  if (!connected_ || !first_status_received_ || config_version_ == 0 || status_version_ == 0)
    return;
  // Give the spa time to report back before sending anything else. If it
  // keeps refusing standby, stop asking so often.
  uint32_t gap = (want && !standby_state_ && quiet_enter_attempts_ >= 3) ? 600000UL : 60000UL;
  if (last_quiet_command_ != 0 && now - last_quiet_command_ < gap)
    return;

  if (want) {
    if (!standby_state_) {
      if (quiet_enter_attempts_ == 3)
        ESP_LOGW(TAG, "Spa has not gone into standby after 3 tries; retrying every 10 min");
      ESP_LOGI(TAG, "Quiet time: putting the spa in standby for %u min", QUIET_STANDBY_MINUTES);
      // Timer either side of the state, so it sticks whether the spa resets
      // the timer when standby starts or wants it set beforehand
      this->write_value_(off.udQuietTime, QUIET_STANDBY_MINUTES);
      this->write_value_(off.quietState, QUIET_STATE_OFF);
      this->write_value_(off.udQuietTime, QUIET_STANDBY_MINUTES);
      quiet_owned_ = true;
      if (quiet_enter_attempts_ < 255)
        quiet_enter_attempts_++;
      last_quiet_command_ = now;
    } else {
      // In standby - ours, or a Maintenance run from the panel we adopt
      quiet_enter_attempts_ = 0;
      quiet_owned_ = true;
      if (ud_quiet_time_ <= QUIET_EXTEND_BELOW_MINUTES) {
        ESP_LOGI(TAG, "Quiet time: extending standby (%u min left)", ud_quiet_time_);
        this->write_value_(off.udQuietTime, QUIET_STANDBY_MINUTES);
        last_quiet_command_ = now;
      }
    }
  } else if (quiet_owned_) {
    if (standby_state_) {
      ESP_LOGI(TAG, "Quiet time: taking the spa out of standby");
      this->write_value_(off.quietState, QUIET_STATE_NOT_SET);
      this->write_value_(off.udQuietTime, 0);
      last_quiet_command_ = now;
    } else {
      // Spa confirms it is out of standby; anything from here on is not ours
      quiet_owned_ = false;
      quiet_enter_attempts_ = 0;
    }
  }
}

int GeckoSpa::days_since_2000(int day, int month, int year) {
  // Calculate days since Jan 1, 2000
  struct tm tm = {};
  tm.tm_year = 100 + year;  // years since 1900, so 2000 = 100
  tm.tm_mon = month - 1;    // 0-based month
  tm.tm_mday = day;
  time_t target = mktime(&tm);

  struct tm epoch = {};
  epoch.tm_year = 100;  // 2000
  epoch.tm_mon = 0;     // January
  epoch.tm_mday = 1;
  time_t base = mktime(&epoch);

  return (int)((target - base) / 86400);
}

void GeckoSpa::parse_notification_message(const uint8_t *data) {
  // Notification entries start at byte 16, each entry is 6 bytes:
  // [ID] [DD] [MM] [YY] [INTERVAL_LO] [INTERVAL_HI]
  // ID: 0x01=Rinse Filter, 0x02=Clean Filter, 0x03=Change Water, 0x04=Spa Checkup
  for (int i = 0; i < 4; i++) {
    int offset = 16 + (i * 6);
    uint8_t id = data[offset];
    uint8_t reset_day, reset_month, reset_year;
    if (notif_date_format_ == NotifDateFormat::D_M_Y) {
      reset_day = data[offset + 1];
      reset_month = data[offset + 2];
      reset_year = data[offset + 3];  // 2-digit year
    } else {
      reset_year = data[offset + 1];  // 2-digit year
      reset_month = data[offset + 2];
      reset_day = data[offset + 3];
    }
    uint16_t interval = data[offset + 4] | (data[offset + 5] << 8);

    if (id == 0 || interval == 0)
      continue;

    // Calculate due date: reset_date + interval days
    struct tm reset_tm = {};
    reset_tm.tm_year = 100 + reset_year;  // years since 1900
    reset_tm.tm_mon = reset_month - 1;     // 0-based month
    reset_tm.tm_mday = reset_day + interval;  // mktime normalizes this
    mktime(&reset_tm);  // Normalize the date

    // Format as ISO date string (YYYY-MM-DD)
    char date_str[12];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d",
             1900 + reset_tm.tm_year, reset_tm.tm_mon + 1, reset_tm.tm_mday);

    ESP_LOGI(TAG, "Notification %d: reset=%02d/%02d/%02d interval=%d due=%s",
             id, reset_day, reset_month, reset_year, interval, date_str);

    text_sensor::TextSensor *sensor = nullptr;
    switch (id) {
      case 0x01:
        sensor = rinse_filter_sensor_;
        break;
      case 0x02:
        sensor = clean_filter_sensor_;
        break;
      case 0x03:
        sensor = change_water_sensor_;
        break;
      case 0x04:
        sensor = spa_checkup_sensor_;
        break;
    }

    // Publish state if it has changed
    if (strcmp(date_str, notification_date_[id - 1]))
    {
      ESP_LOGI(TAG, "Publish changed notification %d : %s", id, date_str);
      if (sensor) {
        sensor->publish_state(date_str);
      }
      strcpy(notification_date_[id - 1], date_str);
    }
  }
}

// GeckoSpaClimate implementation
void GeckoSpaClimate::setup() {
  this->mode = climate::CLIMATE_MODE_HEAT;
  this->action = climate::CLIMATE_ACTION_IDLE;
  this->target_temperature = 37.0;
  this->current_temperature = NAN;
  this->publish_state();
}

climate::ClimateTraits GeckoSpaClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.set_supported_modes({climate::CLIMATE_MODE_HEAT});
  // Without these ESPHome does not report the water temperature or whether
  // the heater is running, and Home Assistant shows neither.
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
  traits.set_visual_min_temperature(GeckoSpa::MIN_TEMPERATURE);
  traits.set_visual_max_temperature(parent_->get_max_temperature());
  traits.set_visual_temperature_step(0.5);
  return traits;
}

void GeckoSpaClimate::control(const climate::ClimateCall &call) {
  if (call.get_target_temperature().has_value()) {
    float temp = *call.get_target_temperature();
    this->target_temperature = temp;
    parent_->send_temperature_command(temp);
  }
  this->publish_state();
}

// GeckoSpaSwitch implementation
void GeckoSpaSwitch::write_state(bool state) {
  if (switch_type_ == "light") {
    parent_->send_light_command(state);
  } else if (switch_type_ == "circulation") {
    parent_->send_circ_command(state);
  } else if (switch_type_ == "pump1") {
    parent_->send_pump1_command(state ? 1 : 0);  // 1=HIGH, 0=OFF
  } else if (switch_type_ == "pump2") {
    parent_->send_pump2_command(state ? 1 : 0);  // EXPERIMENTAL
  } else if (switch_type_ == "pump3") {
    parent_->send_pump3_command(state ? 1 : 0);  // EXPERIMENTAL
  } else if (switch_type_ == "pump4") {
    parent_->send_pump4_command(state ? 1 : 0);  // EXPERIMENTAL
  }
  // State will be published when spa confirms the change
}

// GeckoSpaSelect implementation
void GeckoSpaSelect::setup() {
  // Initialize preferences
  this->pref_ = global_preferences->make_preference<uint8_t>(this->get_object_id_hash());

  // Restore saved state on boot
  if (this->pref_.load(&this->saved_index_) && this->saved_index_ < 5) {
    static const char *prog_names[] = {"Away", "Standard", "Energy", "Super Energy", "Weekend"};
    this->publish_state(prog_names[this->saved_index_]);
    ESP_LOGI("gecko_spa", "Restored program state: %s (index %d)", prog_names[this->saved_index_], this->saved_index_);
  }
}

void GeckoSpaSelect::control(const std::string &value) {
  uint8_t prog = 1;
  if (value == "Away")
    prog = 0;
  else if (value == "Standard")
    prog = 1;
  else if (value == "Energy")
    prog = 2;
  else if (value == "Super Energy")
    prog = 3;
  else if (value == "Weekend")
    prog = 4;

  // Save the state
  this->saved_index_ = prog;
  this->pref_.save(&this->saved_index_);

  parent_->send_program_command(prog);
  this->publish_state(value);
}

}  // namespace gecko_spa
}  // namespace esphome

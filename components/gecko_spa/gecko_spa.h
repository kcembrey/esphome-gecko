#pragma once

#include <cstdint>
#include <string>
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/preferences.h"
#include "gecko_transport.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/select/select.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/sensor/sensor.h"
#ifdef USE_GECKO_SPA_QUIET_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

namespace esphome {
namespace gecko_spa {

// Geckolib-compatible offset structure for log/status messages
// Offsets are geckolib offsets (base 256), convert to message byte with: byte = offset - 254
struct GeckoLogOffsets {
  uint16_t hours;           // Operating hours
  uint16_t quietState;      // Quiet/drain/soak mode
  uint16_t udP1;            // User demand P1-P4 (2-bit fields)
  uint16_t deviceStatus;    // CP, BL, Heater, Waterfall bits
  uint16_t p1;              // P1-P4 device status (2-bit fields)
  uint16_t udLi;            // Light user demand
  uint16_t realSetPointG;   // Target temperature (word)
  uint16_t displayedTempG;  // Actual temperature (word)
  uint16_t lockMode;        // Keypad lock status
  uint16_t packType;        // Pack type identifier
  uint16_t udPumpTime;      // Pump timer countdown
  uint16_t udQuietTime;     // Standby (Maintenance) timer in minutes; 0 = position unknown
  // Remote filtration control: on packs whose filtration is driven by a
  // water-care program, these show what the program is doing. 0 = unknown.
  uint16_t remoteFiltAction;  // IDLE, STOP, START, NEW, ACTIVE
  uint16_t remoteFiltDur;     // Time word: hours, minutes left
  uint16_t econActive;        // Bit 2: economy (lower setpoint) in effect
};

// Default offsets for inYT v51+ (most common)
static const GeckoLogOffsets GECKO_LOG_OFFSETS_V51 = {
  .hours = 256,
  .quietState = 257,
  .udP1 = 259,
  .deviceStatus = 260,
  .p1 = 261,
  .udLi = 307,
  .realSetPointG = 275,
  .displayedTempG = 277,
  .lockMode = 310,
  .packType = 289,
  .udPumpTime = 303,
  .udQuietTime = 304,
  // geckolib puts these at the same positions in every inYT status version
  // from 51 through 83
  .remoteFiltAction = 263,
  .remoteFiltDur = 264,
  .econActive = 281,
};

// Offsets for inYT v50 (older version with shifted offsets)
static const GeckoLogOffsets GECKO_LOG_OFFSETS_V50 = {
  .hours = 284,
  .quietState = 285,
  .udP1 = 258,
  .deviceStatus = 259,
  .p1 = 260,
  .udLi = 307,
  .realSetPointG = 274,
  .displayedTempG = 276,
  .lockMode = 309,
  .packType = 288,
  .udPumpTime = 302,
  .udQuietTime = 0,  // Not mapped for v50, so quiet time stays off
  .remoteFiltAction = 0,
  .remoteFiltDur = 0,
  .econActive = 0,
};

class GeckoSpaClimate;

enum class NotifDateFormat : uint8_t {
  Y_M_D = 0,
  D_M_Y = 1
};

class GeckoSpa : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // The transport owns the link to the spa: either an Arduino proxy over UART
  // or the ESP32's own I2C peripheral. Everything above this line is identical
  // either way.
  void set_transport(GeckoTransport *transport) { transport_ = transport; }

  // Entity setters - switches (controllable)
  void set_light_switch(switch_::Switch *sw) { light_switch_ = sw; }
  void set_circ_switch(switch_::Switch *sw) { circ_switch_ = sw; }
  void set_pump1_switch(switch_::Switch *sw) { pump1_switch_ = sw; }
  void set_pump2_switch(switch_::Switch *sw) { pump2_switch_ = sw; }
  void set_pump3_switch(switch_::Switch *sw) { pump3_switch_ = sw; }
  void set_pump4_switch(switch_::Switch *sw) { pump4_switch_ = sw; }
  // Entity setters - binary sensors (read-only status)
  void set_waterfall_sensor(binary_sensor::BinarySensor *bs) { waterfall_sensor_ = bs; }
  void set_blower_sensor(binary_sensor::BinarySensor *bs) { blower_sensor_ = bs; }
  void set_program_select(select::Select *sel) { program_select_ = sel; }
  void set_standby_sensor(binary_sensor::BinarySensor *bs) {
    standby_sensor_ = bs;
    bs->publish_state(standby_state_);
  }
  void set_connected_sensor(binary_sensor::BinarySensor *bs) {
    connected_sensor_ = bs;
    bs->publish_state(connected_);
  }
  void set_climate(climate::Climate *cl) { climate_ = cl; }
  void set_rinse_filter_sensor(text_sensor::TextSensor *s) { rinse_filter_sensor_ = s; }
  void set_clean_filter_sensor(text_sensor::TextSensor *s) { clean_filter_sensor_ = s; }
  void set_change_water_sensor(text_sensor::TextSensor *s) { change_water_sensor_ = s; }
  void set_spa_checkup_sensor(text_sensor::TextSensor *s) { spa_checkup_sensor_ = s; }
  void set_spa_time_sensor(text_sensor::TextSensor *s) { spa_time_sensor_ = s; }
  void set_config_version_sensor(text_sensor::TextSensor *s) { config_version_sensor_ = s; }
  void set_status_version_sensor(text_sensor::TextSensor *s) { status_version_sensor_ = s; }
  void set_lock_mode_sensor(text_sensor::TextSensor *s) { lock_mode_sensor_ = s; }
  void set_pack_type_sensor(text_sensor::TextSensor *s) { pack_type_sensor_ = s; }
  void set_pump_timer_sensor(sensor::Sensor *s) { pump_timer_sensor_ = s; }
  // What the active program is doing (read-only)
  void set_filtration_sensor(text_sensor::TextSensor *s) { filtration_sensor_ = s; }
  void set_filter_time_left_sensor(sensor::Sensor *s) { filter_time_left_sensor_ = s; }
  void set_economy_sensor(binary_sensor::BinarySensor *bs) { economy_sensor_ = bs; }
  void set_notif_date_format(NotifDateFormat format) { notif_date_format_ = format; }
  // Setpoint range accepted from Home Assistant. Gecko panels stop at 40 C
  // unless the up key is held.
  void set_min_temperature(float temp_c) { min_temperature_ = temp_c; }
  void set_max_temperature(float temp_c) { max_temperature_ = temp_c; }

  // Quiet time: every night between start and end (minutes after midnight),
  // hold the spa in standby - the panel's Maintenance mode, all pumps off.
#ifdef USE_GECKO_SPA_QUIET_TIME
  void set_quiet_time(time::RealTimeClock *clock, uint16_t start_minute, uint16_t end_minute, float min_water_temp) {
    quiet_clock_ = clock;
    quiet_start_ = start_minute;
    quiet_end_ = end_minute;
    quiet_min_water_temp_ = min_water_temp;
    quiet_configured_ = true;
  }
#endif
  void set_quiet_time_sensor(binary_sensor::BinarySensor *bs) {
    quiet_time_sensor_ = bs;
    bs->publish_state(quiet_wanted_);
  }
  // Let the pumps run for the next `minutes` even inside quiet time.
  // 0 ends the pause and resumes quiet time straight away.
  void pause_quiet_time(uint32_t minutes = 120);
  float get_min_temperature() const { return min_temperature_; }
  float get_max_temperature() const { return max_temperature_; }

  // Command methods
  void send_light_command(bool on);
  void send_circ_command(bool on);
  void send_pump1_command(uint8_t state);  // state: 0=OFF, 1=HIGH, 2=LOW
  void send_pump2_command(uint8_t state);  // Experimental: func ID 0x04
  void send_pump3_command(uint8_t state);  // Experimental: func ID 0x05
  void send_pump4_command(uint8_t state);  // Experimental: func ID 0x06
  void send_program_command(uint8_t prog);
  void send_temperature_command(float temp_c);
  void request_status();
  // Force the link back up: resets the Arduino proxy, or reinitialises the I2C
  // peripheral when talking to the spa directly.
  void recover_link();
  // Kept so existing configurations that call id(spa).reset_arduino() still work.
  void reset_arduino() { this->recover_link(); }

  // State getters
  bool get_light_state() { return light_state_; }
  bool get_circ_state() { return circ_state_; }
  bool get_waterfall_state() { return waterfall_state_; }
  uint8_t get_pump1_state() { return pump1_state_; }  // 0=OFF, 1=HIGH, 2=LOW
  uint8_t get_pump2_state() { return pump2_state_; }
  uint8_t get_pump3_state() { return pump3_state_; }
  uint8_t get_pump4_state() { return pump4_state_; }
  float get_target_temp() { return target_temp_; }
  float get_actual_temp() { return actual_temp_; }
  bool is_heating() { return heating_state_; }

 protected:
  // Entity pointers - switches (controllable)
  switch_::Switch *light_switch_{nullptr};
  switch_::Switch *circ_switch_{nullptr};
  switch_::Switch *pump1_switch_{nullptr};
  switch_::Switch *pump2_switch_{nullptr};
  switch_::Switch *pump3_switch_{nullptr};
  switch_::Switch *pump4_switch_{nullptr};
  // Entity pointers - binary sensors (read-only)
  binary_sensor::BinarySensor *waterfall_sensor_{nullptr};
  binary_sensor::BinarySensor *blower_sensor_{nullptr};
  select::Select *program_select_{nullptr};
  binary_sensor::BinarySensor *standby_sensor_{nullptr};
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
  climate::Climate *climate_{nullptr};
  text_sensor::TextSensor *rinse_filter_sensor_{nullptr};
  text_sensor::TextSensor *clean_filter_sensor_{nullptr};
  text_sensor::TextSensor *change_water_sensor_{nullptr};
  text_sensor::TextSensor *spa_checkup_sensor_{nullptr};
  text_sensor::TextSensor *spa_time_sensor_{nullptr};
  text_sensor::TextSensor *config_version_sensor_{nullptr};
  text_sensor::TextSensor *status_version_sensor_{nullptr};
  text_sensor::TextSensor *lock_mode_sensor_{nullptr};
  text_sensor::TextSensor *pack_type_sensor_{nullptr};
  sensor::Sensor *pump_timer_sensor_{nullptr};
  text_sensor::TextSensor *filtration_sensor_{nullptr};
  sensor::Sensor *filter_time_left_sensor_{nullptr};
  binary_sensor::BinarySensor *economy_sensor_{nullptr};
  GeckoTransport *transport_{nullptr};
  NotifDateFormat notif_date_format_{NotifDateFormat::D_M_Y};
  float min_temperature_{26.0f};
  float max_temperature_{40.0f};
  // The pack's own setpoint range (MinSetpointG/MaxSetpointG) in its 1/18 C
  // units, as last read from the config message. 0 until read.
  uint16_t spa_min_setpoint_{0};
  uint16_t spa_max_setpoint_{0};
  void check_setpoint_range_(uint16_t spa_min, uint16_t spa_max);

  // Quiet time
  binary_sensor::BinarySensor *quiet_time_sensor_{nullptr};
#ifdef USE_GECKO_SPA_QUIET_TIME
  time::RealTimeClock *quiet_clock_{nullptr};
#endif
  bool quiet_configured_{false};
  uint16_t quiet_start_{0};
  uint16_t quiet_end_{0};
  float quiet_min_water_temp_{10.0f};
  bool quiet_wanted_{false};       // Quiet time is in force right now
  bool quiet_owned_{false};        // The spa's current standby is ours to end
  bool quiet_cold_hold_{false};    // Water too cold: quiet time off until the window ends
  bool quiet_paused_{false};
  bool quiet_unmapped_warned_{false};
  uint32_t quiet_pause_until_{0};
  uint32_t last_quiet_check_{0};
  uint32_t last_quiet_command_{0};
  uint8_t quiet_enter_attempts_{0};
  uint8_t ud_quiet_time_{0};       // Minutes left on the spa's standby timer, from status
  uint8_t filt_action_{0xFF};      // RemoteFiltAction last published
  uint16_t filt_time_left_{0xFFFF};
  bool econ_active_{false};
  // Each standby lasts this long on the spa's own timer and is topped up
  // while quiet time holds, so a crash or reboot can never keep the pumps
  // off for longer than this.
  static constexpr uint8_t QUIET_STANDBY_MINUTES = 60;
  static constexpr uint8_t QUIET_EXTEND_BELOW_MINUTES = 15;
  static constexpr uint8_t QUIET_STATE_NOT_SET = 0;
  static constexpr uint8_t QUIET_STATE_OFF = 3;  // Standby: what the panel's Maintenance mode uses
  void update_quiet_time_();
  bool in_quiet_window_();
  void write_value_(uint16_t position, uint8_t value);

  // State
  bool light_state_{false};
  bool circ_state_{false};
  bool waterfall_state_{false};
  bool blower_state_{false};
  bool heating_state_{false};
  bool standby_state_{false};
  bool connected_{false};
  bool first_status_received_{false};
  bool temps_known_{false};  // Set once a status message has given us real temperatures
  uint8_t user_demand_state_{0};  // Bitfield from udP1-udP4 (P1-P4 user demand)
  uint8_t pump1_state_{0};   // 0=OFF, 1=HIGH, 2=LOW
  uint8_t pump2_state_{0};   // Read-only
  uint8_t pump3_state_{0};   // Read-only
  uint8_t pump4_state_{0};   // Read-only
  uint8_t lock_mode_{0};
  uint8_t pack_type_{0};
  uint8_t pump_timer_{0};
  uint8_t program_id_{0xFF};
  float target_temp_{0};
  float actual_temp_{0};
  uint32_t last_i2c_time_{0};
  uint32_t last_go_send_time_{0};
  uint32_t last_recovery_time_{0};
  uint8_t recovery_retry_count_{0};     // Consecutive recovery attempts without traffic
  char notification_date_[4][12]{ "", "", "", ""};

  // Version tracking (parsed from handshake XML filenames)
  uint8_t config_version_{0};   // e.g., 82 from inYT_C82.xml
  uint8_t status_version_{0};   // e.g., 81 from inYT_S81.xml
  const GeckoLogOffsets *log_offsets_{&GECKO_LOG_OFFSETS_V51};  // Default to v51+

  // Multi-part message buffer (byte[10]=0x01 means more coming, 0x00 means last)
  uint8_t msg_buffer_[512];
  uint16_t msg_buffer_len_{0};
  // The last part appended to msg_buffer_, so a part the spa resends is not
  // appended twice.
  uint8_t last_part_[GECKO_MAX_FRAME_LEN];
  uint8_t last_part_len_{0};
  uint8_t repeats_dropped_{0};
  // The final part of the last multi-part message, which the spa resends
  // several times once the message is complete.
  uint8_t last_tail_[GECKO_MAX_FRAME_LEN];
  uint8_t last_tail_len_{0};
  // Where each part of the message being reassembled starts in msg_buffer_.
  // A status block embedded in a config message starts a part of its own.
  static const uint8_t MAX_PARTS = 16;
  uint16_t part_starts_[MAX_PARTS];
  uint8_t part_count_{0};
  bool is_part_start_(uint16_t offset) const;
  // The byte at pack memory `position` in the message just reassembled, or
  // -1 if no part of it covers that position.
  int byte_at_position_(uint16_t position) const;
  // Which part of the message being reassembled covers `position` as its
  // first byte, or -1.
  int part_index_for_position_(uint16_t position) const;

  // GO keep-alive message
  static const uint8_t GO_MESSAGE[15];

  // Autodetected status message length
  // Currently we don't parse beyond offset 112, so a min length of 120 is safe.
  uint16_t status_msg_len_{0};
  static const uint16_t MIN_STATUS_MSG_LEN{120};

  uint8_t calc_checksum(const uint8_t *data, uint8_t len);
  void send_i2c_message(const uint8_t *data, uint8_t len);
  void process_i2c_message(const uint8_t *data, uint8_t len);
  void parse_status_message(const uint8_t *data);
  void parse_notification_message(const uint8_t *data);
  int days_since_2000(int day, int month, int year);
  void update_climate_state();
};

class GeckoSpaClimate : public Component, public climate::Climate {
 public:
  GeckoSpaClimate(GeckoSpa *parent) : parent_(parent) {}

  void setup() override;
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

 protected:
  GeckoSpa *parent_;
};

class GeckoSpaSwitch : public Component, public switch_::Switch {
 public:
  void set_parent(GeckoSpa *parent) { parent_ = parent; }
  void set_switch_type(const std::string &type) { switch_type_ = type; }

  void write_state(bool state) override;

 protected:
  GeckoSpa *parent_{nullptr};
  std::string switch_type_;
};

class GeckoSpaSelect : public Component, public select::Select {
 public:
  void set_parent(GeckoSpa *parent) { parent_ = parent; }
  void setup() override;

  void control(const std::string &value) override;

 protected:
  GeckoSpa *parent_{nullptr};
  ESPPreferenceObject pref_;
  uint8_t saved_index_{0xFF};
};

}  // namespace gecko_spa
}  // namespace esphome

<p align="center"><img src="https://github.com/zteifel/esphome-gecko/blob/master/logo_small.png" alt="Smart Spa Heating" width="400"><img src="https://github.com/zteifel/smart-spa-heating/blob/main/logo_small.png" alt="Smart Spa Heating" width="400"></p>


Home Assistant integration for Gecko spa systems. The ESP32 drives the spa's 5V I2C bus directly through a bidirectional logic level converter - no Arduino in the middle. Tested together with a Gecko IN.YE-3-H3.0 (YE-3-CE) spa controller. Feel free to combine with my home assistant integration for smart heating based on electricty price [Smart Spa Heating Integration](https://github.com/zteifel/smart-spa-heating).

## Architecture

```
┌──────────────────┐              ┌──────────────────┐             ┌─────────────┐
│      ESP32       │   3.3V I2C   │  Level Converter │   5V I2C    │  Gecko Spa  │
│                  │◄────────────►│    (BSS138 x2)   │◄───────────►│             │
│ - I2C slave      │  SDA / SCL   │                  │   (0x17)    │             │
│   (listening)    │              │  Bidirectional,  │             │             │
│ - I2C master     │              │  open-drain safe │             │             │
│   (short bursts) │              │                  │             │             │
│ - Protocol logic │              │                  │             │             │
│ - Home Assistant │              │                  │             │             │
│ - OTA updates    │              │                  │             │             │
└──────────────────┘              └──────────────────┘             └─────────────┘
```

**Key Design Decision:** The Gecko bus is multi-master and both ends answer to address `0x17`, so the ESP32 cannot stay in one role. It sits in I2C **slave** mode listening to the spa, and flips to **master** mode for the few milliseconds it takes to push a command or a handshake ACK out, then goes straight back to listening. That is exactly what the Arduino proxy used to do internally - it now happens on the ESP32 itself, and all protocol encoding/decoding stays where it always was, on a chip you can update over WiFi (OTA).

Dropping the Arduino also removes the failure mode where the spa restarts but the Arduino does not (or the other way round) and the link stays dead until something power-cycles the proxy. There is no second processor to fall out of sync: when the bus goes quiet the ESP32 reinitialises its own I2C peripheral, which includes the standard nine-clock bus recovery.

> **Still running the Arduino proxy?** It is still supported - see [Legacy: Arduino I2C Proxy](#legacy-arduino-i2c-proxy).

## Table of Contents

1. [Installation](#installation)
2. [Hardware Build](#hardware-build)
3. [Configuration Reference](#configuration-reference)
4. [I2C Protocol](#i2c-protocol)
5. [Legacy: Arduino I2C Proxy](#legacy-arduino-i2c-proxy)
6. [Buy me a coffee](#buy-me-a-coffee)
7. [TODO](#todo)
8. [Troubleshooting](#troubleshooting)
9. [Credits](#credits)

---

## Installation

### Quick Start

1. **Set up hardware** - See [Hardware Build](#hardware-build) section below

2. **Nothing to flash but the ESP32** - the level converter is passive. If you are migrating from the Arduino proxy, unplug the Arduino and wire the converter in its place; see [Hardware Build](#hardware-build).

3. **Create a secrets.yaml file** with your credentials:
   ```yaml
   wifi_ssid: "YourWiFiName"
   wifi_password: "YourWiFiPassword"
   api_encryption_key: "generate-with-openssl-rand-base64-32"
   ota_password: "your-ota-password"
   ```
   Generate the API key with: `openssl rand -base64 32`

4. **Create your ESPHome configuration** - This component can be installed directly from GitHub:

```yaml
substitutions:
  device_name: spa-controller

esphome:
  name: ${device_name}
  friendly_name: Spa Controller

esp32:
  board: featheresp32-s2
  framework:
    type: arduino

# Import Gecko Spa component from GitHub
external_components:
  - source: github://zteifel/esphome-gecko
    components: [gecko_spa]

logger:
  level: DEBUG
  baud_rate: 0

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret api_encryption_key

ota:
  platform: esphome
  password: !secret ota_password

# Gecko Spa component - direct connection to the spa I2C bus.
# GPIO3/GPIO4 are the pins labelled SDA/SCL on the Feather ESP32-S2.
gecko_spa:
  id: spa
  sda: GPIO3
  scl: GPIO4

# Climate control
climate:
  - platform: gecko_spa
    gecko_spa_id: spa
    name: "Spa"

# Switches
switch:
  - platform: gecko_spa
    gecko_spa_id: spa
    type: light
    name: "Spa Light"
    icon: "mdi:lightbulb"

  - platform: gecko_spa
    gecko_spa_id: spa
    type: pump1
    name: "Spa Pump"
    icon: "mdi:pump"

  - platform: gecko_spa
    gecko_spa_id: spa
    type: circulation
    name: "Spa Circulation"
    icon: "mdi:rotate-3d-variant"

# Program selector
select:
  - platform: gecko_spa
    gecko_spa_id: spa
    name: "Spa Program"
    icon: "mdi:format-list-bulleted"

# Status sensors
binary_sensor:
  - platform: gecko_spa
    gecko_spa_id: spa
    type: standby
    name: "Spa Standby"
    icon: "mdi:power-standby"

  - platform: gecko_spa
    gecko_spa_id: spa
    type: connected
    name: "Spa Connected"
    device_class: connectivity

# Spa internal clock and maintenance reminder due dates
text_sensor:
  - platform: gecko_spa
    gecko_spa_id: spa
    type: spa_time
    name: "Spa Time"
    icon: "mdi:clock-outline"

  - platform: gecko_spa
    gecko_spa_id: spa
    type: rinse_filter
    name: "Rinse Filter Due"
    icon: "mdi:air-filter"

  - platform: gecko_spa
    gecko_spa_id: spa
    type: clean_filter
    name: "Clean Filter Due"
    icon: "mdi:air-filter"

  - platform: gecko_spa
    gecko_spa_id: spa
    type: change_water
    name: "Change Water Due"
    icon: "mdi:water-sync"

  - platform: gecko_spa
    gecko_spa_id: spa
    type: spa_checkup
    name: "Spa Checkup Due"
    icon: "mdi:wrench-clock"
```

5. **Flash and add to Home Assistant**:
   ```bash
   esphome run your-config.yaml
   ```

6. The device will appear in Home Assistant under **Settings → Devices & Services → ESPHome**

### Home Assistant Entities

After installation, you'll have these entities:

| Entity | Type | Description |
|--------|------|-------------|
| Spa | Climate | Temperature control with current/target display |
| Spa Light | Switch | Control spa light |
| Spa Pump 1 | Switch | Control main pump |
| Spa Circulation | Switch | Control circulation pump |
| Spa Program | Select | Choose program (Away, Standard, Energy, Super Energy, Weekend) |
| Spa Standby | Binary Sensor | Standby mode status |
| Spa Connected | Binary Sensor | Connection status to spa |
| Spa Time | Text Sensor | Spa's internal clock (DD/MM HH:MM:SS) |
| Spa Rinse Filter Due | Text Sensor | Due date for filter rinse (YYYY-MM-DD) |
| Spa Clean Filter Due | Text Sensor | Due date for filter clean (YYYY-MM-DD) |
| Spa Change Water Due | Text Sensor | Due date for water change (YYYY-MM-DD) |
| Spa Checkup Due | Text Sensor | Due date for spa checkup (YYYY-MM-DD) |
| Refresh Spa Status | Button | Manually request status update |
| Reconnect Spa Bus | Button | Reinitialise the I2C peripheral and recover the bus |

---

## Hardware Build

### Components Required

| Component | Description | Notes |
|-----------|-------------|-------|
| ESP32 board | WiFi/Ethernet microcontroller | Tested with Adafruit Feather ESP32-S2 and Olimex ESP32-POE-ISO |
| Bidirectional logic level converter | 3.3V <-> 5V, 2 channels minimum | **Must be the MOSFET (BSS138) type.** See the warning below |
| Dupont wires | Various | Connections between board, converter and spa |

> **The converter type matters.** I2C is an open-drain bus: both ends pull the line low and pull-up resistors bring it back high. Only the passive MOSFET converters (BSS138, or a TXS010x) can pass that. A push-pull converter such as the **TXB0104 will not work** on I2C, and neither will a resistor voltage divider - that is fine for one-way UART, but it cannot pass a line the far end is holding down.

### Pin Connections

#### ESP32 to Level Converter (low-voltage side)

| ESP32 Pin | Converter Pin | Notes |
|-----------|---------------|-------|
| SDA GPIO (GPIO3 on the Feather S2) | LV1 | I2C data, 3.3V side |
| SCL GPIO (GPIO4 on the Feather S2) | LV2 | I2C clock, 3.3V side |
| 3V3 | LV | Reference voltage for the low side |
| GND | GND | Common ground |

#### Level Converter to Spa (high-voltage side)

| Converter Pin | Spa Connector | Notes |
|---------------|---------------|-------|
| HV1 | SDA | I2C data, 5V side |
| HV2 | SCL | I2C clock, 5V side |
| HV | 5V | Reference voltage for the high side |
| GND | GND | Common ground required |

**Pull-ups:** the spa bus already carries roughly 4.7k pull-ups to 5V, and BSS138 breakout boards carry 10k pull-ups on both sides. Do not add more.

**Power:** the spa's 5V rail can supply the converter's HV reference, but do not run the whole ESP32 off it - use USB, an external supply, or PoE.

> **Adafruit Feather ESP32-S2 only:** the board gates power to its on-board I2C pull-ups behind GPIO7. The example config drives that high on boot with an internal `gpio` switch. It is harmless when the converter supplies its own pull-ups, and required if it does not.

### Wiring Diagram
Credits to agittins for the pictures

<img src="./pictures/spa_pinouts.png" width="500"><img src="./pictures/spa_power.png" width="400">
<img src="./pictures/adafruit_esp32s2.png" width="550">

```
                    ┌─────────────────┐
                    │   Gecko Spa     │
                    │   Motherboard   │
                    │                 │
                    │ SDA  SCL  5V GND│
                    └──┬────┬────┬──┬─┘
                       │    │    │  │
    ┌──────────────────┼────┼────┼──┼───────────────────────┐
    │                  │    │    │  │                       │
    │  ┌───────────────┴────┴────┴──┴────────────────────┐  │
    │  │  HV1  HV2   HV  GND                             │  │
    │  │              Logic Level Converter              │  │
    │  │                (BSS138, bidirectional)          │  │
    │  │  LV1  LV2   LV  GND                             │  │
    │  └───┬────┬─────┬───┬────────────────────────────-─┘  │
    │      │    │     │   │                                 │
    │  ┌───┴────┴─────┴───┴─────────────────────────────┐   │
    │  │ GPIO3 GPIO4  3V3 GND                           │   │
    │  │ (SDA) (SCL)                                    │   │
    │  │               Adafruit ESP32-S2                │   │
    │  └────────────────────────────────────────────────┘   │
    │                                                       │
    └───────────────────────────────────────────────────────┘
```

---

## Configuration Reference

### `gecko_spa` options

| Option | Default | Description |
|--------|---------|-------------|
| `sda` | *(required for direct I2C)* | GPIO carrying I2C data to the level converter |
| `scl` | *(required for direct I2C)* | GPIO carrying I2C clock to the level converter |
| `i2c_bus` | `0` | Which of the ESP32's two I2C peripherals to use. Change it only if something else in your config already claims bus 0 |
| `address` | `0x17` | Bus address. Both the spa and the controller answer to this |
| `frequency` | `100kHz` | Bus speed. The spa runs standard mode; leave it alone unless you know otherwise |
| `notif_date_format` | `D-M-Y` | Swap to `Y-M-D` if the maintenance reminder dates look wrong |
| `uart_id` | - | Legacy Arduino proxy mode. Mutually exclusive with `sda`/`scl` |
| `reset_pin` | - | Legacy Arduino proxy mode only - the GPIO wired to the Arduino's RST pin |

Direct I2C mode needs an ESP32 with `framework: type: arduino`, because I2C slave mode is only reachable through the Arduino core's HAL. Config validation will tell you if that is not the case.

### Lambdas

| Call | Description |
|------|-------------|
| `id(spa).request_status()` | Send a GO immediately, prompting a fresh handshake and status dump |
| `id(spa).recover_link()` | Reinitialise the link: I2C peripheral reset (direct mode) or Arduino reset pulse (proxy mode) |
| `id(spa).reset_arduino()` | Alias of `recover_link()`, kept so older configs keep working |

---

## I2C Protocol

### Overview

The Gecko spa uses I2C for communication between components. The spa motherboard and external controllers share address **0x17** in a multi-master configuration.

### Bus Configuration

| Parameter | Value |
|-----------|-------|
| I2C Address | 0x17 (23 decimal) |
| Bus Speed | 100kHz (standard mode) |
| Configuration | Multi-master |
| Pull-ups | 4.7kΩ (typically on spa bus) |

### Multi-Master Operation

Both the spa motherboard and the Arduino controller use address 0x17. The spa sends status updates to this address, and the controller sends commands to this address. This allows bidirectional communication without address conflicts.

### Message Checksums

Most messages use XOR checksum of bytes 0 to (length-2), stored in the last byte.

```c
uint8_t calcChecksum(uint8_t* data, uint8_t len) {
    uint8_t xorVal = 0;
    for (uint8_t i = 0; i < len - 1; i++) {
        xorVal ^= data[i];
    }
    return xorVal;
}
```

---

### Messages FROM Spa (Status Updates)

#### GO Keep-Alive & Handshake Protocol

The controller sends GO every 60 seconds to trigger a handshake sequence.

**GO Message (15 bytes):**
```
17 00 00 00 00 17 09 00 00 00 00 00 01 47 4F
                                       ^^^^
                                       "GO" ASCII
```

**Handshake Sequence:**
After GO is sent, the spa responds with messages that must be acknowledged:

1. **Spa sends 33-byte config message** (contains XML filename like `inYT_C82.xml`)
2. **Controller replies with ACK** (15 bytes): `17 0A 00 00 00 17 09 00 00 00 00 00 01 00 02`
3. **Spa sends another 33-byte config message**
4. **Controller replies with ACK**
5. **Spa sends 22-byte clock message** (byte[13] = 0x4B = 'K', contains date/time)
   - Byte 15: Day (decimal)
   - Byte 16: Month (decimal)
   - Byte 17: Day of week (0=Sun)
   - Byte 18: Hour (hex, 24h format)
   - Byte 19: Minutes (hex)
   - Byte 20: Seconds (hex)
   - Byte 21: Checksum
6. **Controller replies with ACK**
7. **Spa sends 15-byte "LO" message**: bytes[13-14] = 0x4C 0x4F = "LO"
8. **Handshake complete** - status messages now flow normally

**Important:** Without proper handshake acknowledgment, commands may not receive immediate status responses.

#### Status Message (Multi-Part, 162 bytes concatenated)

Status data is sent as a **multi-part message** split across 3 I2C transmissions.

**Multi-Part Message Structure:**

| Part | Raw Size | Header | Payload | Description |
|------|----------|--------|---------|-------------|
| 1 | 78 bytes | 16 bytes | 62 bytes | First status part |
| 2 | 78 bytes | 16 bytes | 62 bytes | Second status part |
| 3 | 54 bytes | 16 bytes | 38 bytes | Final status part |
| **Total** | 210 bytes | 48 bytes | **162 bytes** | Concatenated payload |

**Continuation Flag (Byte 9 in raw message):**
- `0x01` = More parts coming
- `0x00` = Last part of message

**Header (16 bytes, stripped from each part):**
```
17 09 00 00 00 17 0A 01 00 XX 00 00 YY ZZ 52 51
```
Where XX = continuation flag (byte 9), YY ZZ = length/type info

**Message Identification (in concatenated 162-byte payload):**
- Byte[1] = 0x00 indicates status data

**Key Byte Positions (VERIFIED in 162-byte concatenated payload):**

| Byte | Description | Values |
|------|-------------|--------|
| 1 | Data type | 0x00 = Status data |
| 3 | Standby | 0x03 = Standby ON |
| 5 | Pump state | 0x02 = Pump ON |
| 6 | Heat flags | Bit 7 (0x80) = Circ during heating, Bit 5 (0x20) = Heating |
| 7 | Pump flag | 0x01 = Pump ON |
| 21-22 | Target temp (raw) | Big-endian, divide by 18.0 for °C |
| 23-24 | Actual temp (raw) | Big-endian, divide by 18.0 for °C |
| 26 | Heating flag | Bit 2 (0x04) = Heating |
| 53 | Light state | 0x01 = Light ON |
| 112 | Circulation | 0x01 = Circulation ON (manual toggle) |

**Example:** Raw temp bytes `02 9A` = 0x029A = 666 / 18.0 = **37.0°C**

#### Configuration Message (Multi-Part, 405 bytes concatenated)

Periodic configuration/settings dump sent by the spa, typically after handshake.

**Multi-Part Message Structure:**

Similar to status messages, this is sent as multiple I2C transmissions with 16-byte headers stripped and payloads concatenated to form a 405-byte message.

**Known Byte Positions (PARTIALLY DECODED):**

| Byte | Description | Values |
|------|-------------|--------|
| 3-4 | Target temperature (raw) | Big-endian, divide by 18.0 for °C |

**Example:** Bytes 3-4 = `02 9A` = 0x029A = 666 / 18.0 = **37.0°C**

**Note:** This message likely contains program schedules, filter cycle settings, notification intervals, and other configuration data. Further reverse engineering is needed to decode additional fields.

---

#### Program Status Message (18 bytes)

Indicates current program selection.

```
17 0B 00 00 00 17 09 00 00 00 00 00 04 4E 03 D0 [PROG] [CHK]
                                                  ^^^^
                                                  Program ID
```

**Program IDs:**

| ID | Program |
|----|---------|
| 0x00 | Away |
| 0x01 | Standard |
| 0x02 | Energy |
| 0x03 | Super Energy |
| 0x04 | Weekend |

---

#### Notification Message (77 bytes)

Maintenance reminder data, sent with byte[6] = 0x0B.

**Entry Format (6 bytes each, starting at byte 16):**

```
[ID] [DD] [MM] [YY] [INTERVAL_LO] [INTERVAL_HI]
```

| Byte | Description |
|------|-------------|
| ID | Notification type (see below) |
| DD | Reset day (decimal, 1-31) |
| MM | Reset month (decimal, 1-12) |
| YY | Reset year (2-digit, e.g., 25 for 2025) |
| INTERVAL_LO | Interval low byte |
| INTERVAL_HI | Interval high byte |

**Notification IDs:**

| ID | Notification |
|----|--------------|
| 0x01 | Rinse Filter |
| 0x02 | Clean Filter |
| 0x03 | Change Water |
| 0x04 | Spa Checkup |

**Interval:** 16-bit little-endian value representing days between reminders.

**Due Date Calculation:** `due_date = reset_date + interval_days`

**Example:** Entry `01 09 12 25 1E 00` = Rinse Filter, reset Dec 9 2025, interval 30 days → due Jan 8 2026

---

### Messages TO Spa (Commands)

#### On/Off Command (20 bytes)

Controls light, pump, and circulation.

```
17 0A 00 00 00 17 09 00 00 00 00 00 06 46 52 51 01 [FUNC] [STATE] [CHK]
                                                    ^^^^   ^^^^^   ^^^
                                                    Function ID    Checksum
```

**Function IDs:**

| ID | Function | ON State | OFF State |
|----|----------|----------|-----------|
| 0x33 | Light | 0x01 | 0x00 |
| 0x03 | Pump | 0x02 | 0x00 |
| 0x6B | Circulation | 0x01 | 0x00 |

**Note:** Pump uses 0x02 for ON state, not 0x01.

**Example - Light ON:**
```
17 0A 00 00 00 17 09 00 00 00 00 00 06 46 52 51 01 33 01 [CHK]
```

#### Program Select Command (18 bytes)

Changes the spa program.

```
17 0B 00 00 00 17 09 00 00 00 00 00 04 4E 03 D0 [PROG] [CHK]
```

**Pre-calculated Commands:**

| Program | Command (hex) |
|---------|---------------|
| Away | `17 0B 00 00 00 17 09 00 00 00 00 00 04 4E 03 D0 00 9B` |
| Standard | `17 0B 00 00 00 17 09 00 00 00 00 00 04 4E 03 D0 01 9A` |
| Energy | `17 0B 00 00 00 17 09 00 00 00 00 00 04 4E 03 D0 02 99` |
| Super Energy | `17 0B 00 00 00 17 09 00 00 00 00 00 04 4E 03 D0 03 98` |
| Weekend | `17 0B 00 00 00 17 09 00 00 00 00 00 04 4E 03 D0 04 9F` |

#### Temperature Set Command (21 bytes)

Sets the target temperature for the spa.

```
17 0A 00 00 00 17 09 00 00 00 00 00 07 46 52 51 00 01 02 [TEMP] [CHK]
                                                         ^^^^   ^^^
                                                         Raw temp value
```

**Temperature Encoding:**

```
TEMP_RAW = (temperature_celsius × 18) - 512
```

| Temperature | Calculation | Raw Value |
|-------------|-------------|-----------|
| 26.0°C | (26 × 18) - 512 = -44 | 0xD4 |
| 36.5°C | (36.5 × 18) - 512 = 145 | 0x91 |
| 37.0°C | (37 × 18) - 512 = 154 | 0x9A |
| 40.0°C | (40 × 18) - 512 = 208 | 0xD0 |

**Example - Set 37°C:**
```
17 0A 00 00 00 17 09 00 00 00 00 00 07 46 52 51 00 01 02 9A [CHK]
```

---

## Legacy: Arduino I2C Proxy

The original build put an Arduino Nano between the ESP32 and the spa: the Nano did the I2C, the ESP32 did the protocol, and the two talked over UART. That still works and is still supported by the component - `esphome/spa-controller-arduino-proxy.yaml` is a ready-to-use config.

The direct wiring exists because the proxy has one structural weakness: two processors that must agree on the state of the bus. If the spa restarts and the Arduino does not (or the other way round), the link stays dead until something resets the proxy. With the ESP32 on the bus itself there is no second processor to fall out of sync.

### Migrating to direct I2C

1. Remove the `uart:` block and the `reset_pin:` line from your config.
2. Add `sda:` and `scl:` to `gecko_spa:`.
3. Rewire: the level converter's HV side goes where the Arduino's A4/A5/GND went, its LV side to the ESP32's SDA/SCL/3V3/GND.
4. Flash and unplug the Arduino.

Buttons calling `id(spa).reset_arduino()` keep working - it is now an alias for `recover_link()`.

### Flashing the Arduino Nano

**Option A: Use the precompiled binary (recommended)**

Download `arduino-i2c-proxy-atmega328p.hex` from the [GitHub Releases](https://github.com/zteifel/esphome-gecko/releases) page (or from the `arduino/` folder).

```bash
# Install avrdude (Ubuntu/Debian)
sudo apt install avrdude

# For Nano Clone (new bootloader) - 115200 baud
avrdude -v -patmega328p -carduino -P/dev/ttyUSB0 -b115200 -D -Uflash:w:arduino-i2c-proxy-atmega328p.hex:i

# For Original Arduino Nano (old bootloader) - 57600 baud
avrdude -v -patmega328p -carduino -P/dev/ttyUSB0 -b57600 -D -Uflash:w:arduino-i2c-proxy-atmega328p.hex:i
```

> **Tip:** Most cheap Nano clones from AliExpress/Amazon use the new bootloader (115200 baud). If upload fails, try 57600 baud for original Nanos with the old bootloader.

**Option B: Build from source with PlatformIO**

The Arduino Wire library has a default 32-byte I2C buffer, but the spa sends messages up to 78 bytes. You **must** patch the Wire library to increase this buffer.

1. `pip install platformio`
2. `cd arduino && pio run` to download dependencies
3. Edit `~/.platformio/packages/framework-arduino-avr/libraries/Wire/src/utility/twi.h` and change `#define TWI_BUFFER_LENGTH 32` to `128`
4. `pio run -t upload`

> **Why is patching needed?** The `-DTWI_BUFFER_LENGTH=128` build flag in `platformio.ini` should override this value, but some PlatformIO versions do not apply it correctly. Patching the source file directly ensures the buffer is always 128 bytes.

### Proxy Wiring

#### ESP32-S2 to Arduino Nano Clone (UART + Reset)

| ESP32-S2 Pin | Arduino Nano Clone Pin | Notes |
|--------------|------------------|-------|
| GPIO5 (TX) | RX (D0) | Direct connection (3.3V -> 5V tolerant) |
| GPIO16 (RX) | TX (D1) | Via voltage divider (5V -> 3.3V) |
| GPIO17 | RST | Arduino reset (directly, no resistor needed) |
| GND | GND | Common ground required |

```
Arduino TX (D1) ----[2.7kΩ]----+---- ESP32 GPIO16 (RX)
                               |
                            [5.6kΩ]
                               |
                              GND
```

Output voltage: ~2.7V (within ESP32 3.3V logic threshold)

#### Arduino Nano Clone to Spa I2C Bus

| Arduino Nano Clone Pin | Spa Connector | Notes |
|------------------|---------------|-------|
| A4 (SDA) | SDA | I2C Data |
| A5 (SCL) | SCL | I2C Clock |
| GND | GND | Common ground |

**Important:** Do NOT connect Arduino VCC to spa - power the Arduino separately via USB or an external supply.

<img src="./pictures/arduino_nano_pinout.webp" width="350">

### Proxy Configuration

```yaml
uart:
  id: arduino_uart
  tx_pin: GPIO5
  rx_pin: GPIO16
  baud_rate: 115200
  rx_buffer_size: 512

gecko_spa:
  id: spa
  uart_id: arduino_uart
  reset_pin: GPIO17  # Resets the Arduino automatically on disconnect
```

### UART Proxy Protocol

#### Overview

The Arduino acts as a transparent I2C proxy. The ESP32 sends raw I2C bytes as hex strings, and the Arduino forwards them to the I2C bus. Similarly, I2C messages received by the Arduino are sent to the ESP32 as hex strings.

#### Message Format

- **Baud Rate:** 115200
- **Data Bits:** 8
- **Parity:** None
- **Stop Bits:** 1
- **Line Ending:** `\n` (LF)

#### Commands (ESP32 → Arduino)

| Command | Description |
|---------|-------------|
| `TX:<hex>\n` | Send hex bytes to I2C bus at address 0x17 |
| `PING\n` | Health check |

**Example - Send light ON command:**
```
TX:170A0000001709000000000646525101330163\n
```

#### Responses (Arduino → ESP32)

| Message | Description |
|---------|-------------|
| `I2C_PROXY:V1\n` | Firmware version on boot |
| `READY\n` | Arduino ready for commands |
| `RX:<len>:<hex>\n` | Received I2C message (length in decimal, data in hex) |
| `TX:OK\n` | I2C transmission acknowledged |
| `TX:ERR:INVALID_HEX\n` | Invalid hex string |
| `TX:ERR:TOO_LONG\n` | Message exceeds 128 bytes |
| `PONG\n` | Response to PING |

**Example - Received 78-byte status message:**
```
RX:78:17090000001709...4F\n
```

#### Protocol Logic

All spa protocol logic (GO responses, command encoding, status parsing) runs on the ESP32 in `spa_protocol.h`. This allows OTA updates without physical access to the spa.

---


## TODO

- Scheduling of economy intervals and filter cycles in programs (decoding of i2c protocol complete)
- Cleanup notifications fix


## Troubleshooting

Set `logger: level: DEBUG` and watch the boot log. `dump_config()` prints the transport in use, the pins, the address, and whether I2C slave mode came up.

### "Spa Connected" never turns on

- Check SDA and SCL are not swapped, both through the converter and at the spa connector.
- Confirm the converter is the MOSFET/BSS138 type. A TXB0104 or a resistor divider cannot drive an open-drain bus and will silently fail here.
- Confirm HV is tied to the spa's 5V and LV to the ESP32's 3V3. Without both references a BSS138 board passes nothing.
- Check the ground is common between the ESP32 and the spa.
- On the Adafruit Feather ESP32-S2, make sure GPIO7 (I2C power) is being driven high - the example config does this.

### `I2C slave mode not up yet` or `bad pin state` in the log

The HAL refuses to start slave mode while SDA or SCL is held low. The component keeps retrying with a backoff and runs the standard nine-clock bus recovery each time. If it never clears:

- Something is holding a line low - check for a shorted or miswired converter channel.
- Verify the pull-ups exist on both sides (the spa supplies ~4.7k, the converter board usually 10k).

### `I2C transmit failed` warnings

Occasional failures are normal: the spa is another master on the bus, so arbitration is sometimes lost. The component retries once and the spa resends status regularly. A constant stream of them means the spa is not ACKing address `0x17` - re-check wiring and the `address:` option.

### Commands are accepted but nothing happens

- Ensure the handshake completed: the log should show `Handshake XML: inYT_Cxx.xml` and `Received LO message`. Commands embed the config/status versions learned there.
- Press **Refresh Spa Status** to force a GO and a fresh handshake.

### Spa Not Responding at all

- Ensure the GO keep-alive is being sent (the log prints `Sent GO keep-alive` every 23s)
- Verify I2C address 0x17
- Check I2C pull-up resistors

### Arduino proxy issues (legacy wiring)

- **Arduino hangs after receiving I2C:** the Wire library's 32-byte buffer is too small for the spa's 78-byte messages. See [Flashing the Arduino Nano](#flashing-the-arduino-nano), or use the precompiled binary. After patching `twi.h` the line should read `#define TWI_BUFFER_LENGTH 128`.
- Do NOT use `digitalRead()` on SDA/SCL pins, and do NOT enable the hardware watchdog.
- **ESP32 not receiving UART:** verify voltage divider output is >2.5V, check the common ground, and verify the GPIO pins.

---

## Credits
Credit to https://github.com/agittins for the pictures and initial research.

## Buy me a coffee
https://www.buymeacoffee.com/zteifel
## License

MIT License - See LICENSE file for details.

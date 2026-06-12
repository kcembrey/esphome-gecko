# Olimex ESP32-POE-ISO-IND Setup Guide

This is an addon guide for using the **Olimex ESP32-POE-ISO-IND** instead of the Adafruit ESP32-S2. For general setup (Arduino flashing, secrets, Home Assistant entities, protocol details), see the [main README](../README.md).

## Why Olimex?

The Olimex ESP32-POE-ISO-IND provides **Power over Ethernet (PoE)** with galvanic isolation, eliminating the need for a separate power supply and WiFi. This can be more reliable in environments where WiFi signal is weak (e.g., outdoor spa enclosures).

| | Adafruit ESP32-S2 | Olimex ESP32-POE-ISO-IND |
|---|---|---|
| Network | WiFi | Ethernet (PoE) + WiFi |
| Power | USB / external | PoE (via Ethernet cable) or external |
| Chip | ESP32-S2 | ESP32 (WROVER) |
| I/O Voltage | 3.3V | 3.3V |

## Components Required

| Component | Description | Notes |
|-----------|-------------|-------|
| Olimex ESP32-POE-ISO-IND | PoE Ethernet microcontroller | Replaces Adafruit ESP32-S2 |
| Arduino Nano Clone | I2C bridge | Same as main guide |
| Voltage Divider Resistors | 2.7k + 5.6k | Level shifting Arduino TX to Olimex RX |
| Ethernet Cable | Cat5e or better | Connects to PoE switch/injector |
| PoE Switch or Injector | 802.3af | Powers the Olimex board |

## Pin Connections

Several GPIOs on the Olimex board are reserved for the Ethernet PHY (LAN8720) and PSRAM. The pin assignments differ from the ESP32-S2 setup.

**Reserved pins (do NOT use):** GPIO0, GPIO5, GPIO12, GPIO17, GPIO18, GPIO19, GPIO21, GPIO22, GPIO23, GPIO25, GPIO26, GPIO27

### Olimex to Arduino Nano (UART + Reset)

| Olimex Pin | Arduino Nano Pin | Notes |
|------------|------------------|-------|
| GPIO4 (TX) | RX (D0) | Direct connection (3.3V is 5V tolerant on Arduino) |
| GPIO36 (RX) | TX (D1) | Via voltage divider (5V to 3.3V) |
| GPIO14 (RST) | RST | Arduino reset (active-LOW, has internal pull-up) |
| GND | GND | Common ground required |

### Voltage Divider Circuit (Arduino TX to Olimex RX)

```
Arduino TX (D1) ----[2.7k]----+---- Olimex GPIO36 (RX)
                               |
                            [5.6k]
                               |
                              GND
```

Output voltage: ~2.7V (within ESP32 3.3V logic threshold)

### Arduino Nano to Spa I2C Bus

Same as the main guide - see [Hardware Build](../README.md#hardware-build).

| Arduino Nano Pin | Spa Connector | Notes |
|------------------|---------------|-------|
| A4 (SDA) | SDA | I2C Data |
| A5 (SCL) | SCL | I2C Clock |
| GND | GND | Common ground |

## Wiring Diagram

```
                    +------------------+
                    |    Gecko Spa     |
                    |   Motherboard    |
                    |                  |
                    |  SDA  SCL  GND   |
                    +---+----+----+---+
                        |    |    |
    +-------------------+----+----+---------------------+
    |                   |    |    |                      |
    |  +----------------+----+----+-------------------+  |
    |  |             Arduino Nano Clone               |  |
    |  |                                              |  |
    |  |  A4(SDA)  A5(SCL)  GND   TX(D1)  RX(D0) RST |  |
    |  +-------------------------------+------+----+--+  |
    |                          |       |      |    |     |
    |                          |    [2.7k]    |    |     |
    |                          |       |      |    |     |
    |                          +-[5.6k]+      |    |     |
    |                          |       |      |    |     |
    |  +-----------------------+-------+------+----+--+  |
    |  |                      GND  GPIO36  GPIO4 GPIO14|  |
    |  |                                               |  |
    |  |          Olimex ESP32-POE-ISO-IND             |  |
    |  |               [Ethernet/PoE]                  |  |
    |  +-----------------------------------------------+  |
    |                                                     |
    +-----------------------------------------------------+
```

## ESPHome Configuration

Use the provided `esphome/spa-controller-olimex.yaml` or create your own. The key differences from the ESP32-S2 config:

### Board and Ethernet (replaces WiFi)

```yaml
esp32:
  board: esp32-poe-iso
  framework:
    type: arduino

# Ethernet (PoE) - replaces wifi: section
ethernet:
  type: LAN8720
  mdc_pin: GPIO23
  mdio_pin: GPIO18
  clk_mode: GPIO17_OUT
  phy_addr: 0
  power_pin: GPIO12
```

### UART Pins

```yaml
uart:
  id: arduino_uart
  tx_pin: GPIO4      # Was GPIO5 on ESP32-S2
  rx_pin: GPIO36     # Was GPIO16 on ESP32-S2
  baud_rate: 115200
  rx_buffer_size: 512
```

### Reset Pin

```yaml
gecko_spa:
  id: spa
  uart_id: arduino_uart
  reset_pin: GPIO14   # Was GPIO17 on ESP32-S2
```

### Full Config

A complete ready-to-use configuration is provided at [`esphome/spa-controller-olimex.yaml`](../esphome/spa-controller-olimex.yaml).

## Flashing

First flash requires USB connection to the Olimex board. After the initial flash, OTA updates work over Ethernet.

```bash
# Initial flash via USB
esphome run spa-controller-olimex.yaml

# Subsequent updates via OTA (Ethernet)
esphome upload spa-controller-olimex.yaml
```

## Pin Reference Summary

| Function | ESP32-S2 (Adafruit) | ESP32-POE-ISO (Olimex) | Why changed |
|----------|---------------------|------------------------|-------------|
| UART TX | GPIO5 | GPIO4 | GPIO5 used by Ethernet PHY reset |
| UART RX | GPIO16 | GPIO36 | GPIO16 used by PSRAM (WROVER) |
| Arduino RST | GPIO17 | GPIO14 | GPIO17 used by Ethernet PHY clock |
| Network | WiFi | Ethernet (PoE) | Board feature |

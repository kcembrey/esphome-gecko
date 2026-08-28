# Olimex ESP32-POE-ISO-IND Setup Guide

This is an addon guide for using the **Olimex ESP32-POE-ISO-IND** instead of the Adafruit ESP32-S2. For general setup (level converter choice, secrets, Home Assistant entities, protocol details), see the [main README](../README.md).

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
| Bidirectional logic level converter | 3.3V <-> 5V, MOSFET/BSS138 type | Same as main guide |
| Ethernet Cable | Cat5e or better | Connects to PoE switch/injector |
| PoE Switch or Injector | 802.3af | Powers the Olimex board |

## Pin Connections

Several GPIOs on the Olimex board are reserved for the Ethernet PHY (LAN8720) and PSRAM. The pin assignments differ from the ESP32-S2 setup.

**Reserved pins (do NOT use):** GPIO0, GPIO5, GPIO12, GPIO17, GPIO18, GPIO19, GPIO21, GPIO22, GPIO23, GPIO25, GPIO26, GPIO27

### Olimex to Level Converter (low-voltage side)

The UEXT connector's I2C pair is GPIO13/GPIO16, but GPIO16 is taken by PSRAM on the WROVER module. GPIO13 and GPIO14 are both free on the extension header and both can drive open-drain outputs, so those are what the example config uses.

| Olimex Pin | Converter Pin | Notes |
|------------|---------------|-------|
| GPIO13 (SDA) | LV1 | I2C data, 3.3V side |
| GPIO14 (SCL) | LV2 | I2C clock, 3.3V side |
| 3.3V | LV | Reference voltage for the low side |
| GND | GND | Common ground required |

### Level Converter to Spa (high-voltage side)

Same as the main guide - see [Hardware Build](../README.md#hardware-build).

| Converter Pin | Spa Connector | Notes |
|---------------|---------------|-------|
| HV1 | SDA | I2C Data |
| HV2 | SCL | I2C Clock |
| HV | 5V | Reference voltage for the high side |
| GND | GND | Common ground |

## Wiring Diagram

```
                    +-------------------+
                    |    Gecko Spa      |
                    |   Motherboard     |
                    |                   |
                    |  SDA  SCL  5V GND |
                    +---+----+----+--+--+
                        |    |    |  |
    +-------------------+----+----+--+--------------------+
    |                   |    |    |  |                    |
    |  +----------------+----+----+--+-----------------+  |
    |  |  HV1  HV2   HV  GND                           |  |
    |  |            Logic Level Converter              |  |
    |  |              (BSS138, bidirectional)          |  |
    |  |  LV1  LV2   LV  GND                           |  |
    |  +---+----+-----+---+---------------------------+   |
    |      |    |     |   |                               |
    |  +---+----+-----+---+---------------------------+   |
    |  |GPIO13 GPIO14 3.3V GND                        |   |
    |  |(SDA)  (SCL)                                  |   |
    |  |          Olimex ESP32-POE-ISO-IND            |   |
    |  |               [Ethernet/PoE]                 |   |
    |  +----------------------------------------------+   |
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

### I2C Pins

```yaml
gecko_spa:
  id: spa
  sda: GPIO13   # Was GPIO3 on ESP32-S2
  scl: GPIO14   # Was GPIO4 on ESP32-S2
```

Unlike the ESP32-S2 config, no GPIO7 I2C-power switch is needed here - the Olimex board has no such gate.

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
| I2C SDA | GPIO3 | GPIO13 | Different board pinout; GPIO13 is free on the extension header |
| I2C SCL | GPIO4 | GPIO14 | GPIO16 (the UEXT SCL) is used by PSRAM (WROVER) |
| I2C pull-up power | GPIO7 | n/a | Feather-specific gate, no equivalent on the Olimex |
| Network | WiFi | Ethernet (PoE) | Board feature |

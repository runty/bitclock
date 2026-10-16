<img src="https://github.com/goat-hill/bitclock/assets/220799/5121ca8b-19c7-4db0-9a81-38fae36ac5df" width="250" alt="bitclock" />

# bitclock
Open source e-ink desk companion, clock, and air quality monitor.

Available for purchase in USA @ https://bitclock.io

<img src="https://github.com/user-attachments/assets/a1be3895-0399-4e95-8b5f-d52c59792a22" width="500" alt="Bitclock photo" />

Visit https://bitclock.io/connect to configure your existing device

## Home Assistant

Bitclock can report air quality readings to Home Assistant over MQTT. In Home
Assistant, enable the MQTT integration with discovery enabled, then use the
Bitclock configurator at `/connect`.

1. Flash firmware with Home Assistant MQTT support.
2. Open the configurator in Chrome and connect to the device named `bclk`.
3. Enable MQTT reporting.
4. Enter the broker URI, for example `mqtt://homeassistant.local:1883`.
5. Enter MQTT broker credentials. Home Assistant long-lived access tokens are
   not MQTT credentials.

The firmware publishes Home Assistant MQTT discovery for:

- Sensors: temperature, humidity, CO2, VOC index, NOx index and the air quality
  alert the display shows (`Clear`, `CO2`, `VOC`, `NOx`, `Humid`, `Dry`, ...).
- Diagnostics: Wi-Fi signal, uptime, the last CO2 calibration correction, and
  the firmware version on the device page.
- Controls: a temperature offset (-10.0 to +10.0 °C, saved on the device and
  applied to temperature with humidity compensated), a button that calibrates
  CO2 to outdoor air (420 ppm), the clock face (Clock or Weather), and a status
  line.

Values are rounded (temperature and humidity to 0.1) and state is sent every 30
seconds and right after any command.

### Pushing data to the clock

Topics are under `bitclock/<wifi mac, lower-case hex>/`:

| Topic | Payload | Effect |
| --- | --- | --- |
| `status/set` | text, up to 24 characters | Replaces the date under the clock; empty restores it. Upper-cased; the font has A-Z, 0-9 and `. , : ; - + % ° / ! ? & ( ) # '`. Publish retained so it survives a reboot. |
| `weather/set` | `{"high": 12, "low": 6, "condition": "rainy"}` | Forecast for the weather face, in Celsius with Home Assistant condition names. While one is less than 3 hours old the clock stops polling weather.gov (which only covers the US). Publish retained. |
| `temperature_offset/set` | e.g. `-0.8` | Same as the Home Assistant number. |
| `clock_face/set` | `Clock` or `Weather` | Same as the Home Assistant select. |
| `co2_calibrate/press` | anything | Forced recalibration to 420 ppm. Put the clock in outdoor air (an open window) for a few minutes first; the sensor refuses until it has been measuring for 3 minutes. |

A Home Assistant automation that keeps the weather face current:

```yaml
triggers:
  - trigger: time_pattern
    minutes: "/30"
  - trigger: homeassistant
    event: start
actions:
  - action: weather.get_forecasts
    target:
      entity_id: weather.forecast_home
    data:
      type: daily
    response_variable: forecast
  - action: mqtt.publish
    data:
      topic: bitclock/<mac>/weather/set
      retain: true
      payload: >-
        {% set today = forecast['weather.forecast_home'].forecast[0] %}
        {"high": {{ today.temperature }}, "low": {{ today.templow }},
         "condition": "{{ today.condition }}"}
```

Known compatibility notes:

- The current Home Assistant configurator discovers devices by the new Home
  Assistant BLE service UUID. Stock firmware that only advertises the original
  Bitclock service may not appear in the local `/connect` device picker.
- The current Home Assistant firmware advertises the new Home Assistant service
  UUID to avoid stale browser GATT caches. Older configurators that filter only
  for the original Bitclock service may not discover updated devices.
- The connect page can still show `0` for unavailable or malformed sensor BLE
  values. Check the BLE reads panel before treating `0` CO2, humidity, VOC, or
  NOx values as real readings.

## Firmware

Source code for ESP32 using the ESP-IDF development framework.

See [bitclock-fw/](bitclock-fw/)

## Website

Next.js web app for [homepage](https://bitclock.io) and [device configurator](https://bitclock.io/connect).

See [bitclock-web/](bitclock-web/)

<img src="https://github.com/goat-hill/bitclock/assets/220799/9b72ab4b-d259-4e13-9049-c8f1ba85664d" width="500" alt="Web configurator screenshot" />

## Enclosure

OnShape design, STEP files, and instructions for 3D printing.

See [bitclock-enclosure/](bitclock-enclosure/)

<img src="https://github.com/goat-hill/bitclock/assets/220799/b83af06f-92e7-4f3a-9e44-477fad33d226" width="500" alt="Device CAD screenshot" />

## PCB

KiCad schematic, board, and JLCPCB ordering files.

See [bitclock-pcb/](bitclock-pcb/)

<img src="https://github.com/user-attachments/assets/40ddb658-e42d-4617-83b4-a5637c75a250" width="500" alt="Bitclock PCB" />

# Blog posts

- [Advanced ESP32 development with ESP-IDF](https://bitclock.io/blog/esp-idf-vscode)

# Contributing

## Cloning the repo

This repo uses git submodules. Make sure to use `git clone --recursive`.

```sh
git clone --recursive git@github.com:goat-hill/bitclock.git
```

If the repo was previously checked out without `--recursive`, initialize the submodules.

```sh
git submodule init
git submodule update
```

## Pre-commit linting

A single pre-commit config is created for this monorepo. Set up the hooks to get linting on every commit.

```fish
brew install pre-commit
pre-commit install
```

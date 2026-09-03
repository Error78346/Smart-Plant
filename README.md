# SMART_FICUS 🌱

An Arduino-based automatic plant watering system with an OLED display, rotary
encoder menu, and per-plant moisture profiles. Built around an SSD1306
display, a DHT11 temperature/humidity sensor, a capacitive soil moisture
sensor, and a relay-driven water pump.

## Features

- **Live OLED dashboard** — cycle through pages showing an animated plant
  graphic (with weather effects for hot/cold conditions), soil moisture %,
  temperature/humidity, raw sensor readout, and the active plant's settings.
- **Rotary encoder navigation** — interrupt-driven for responsive menu
  control; press to enter the settings menu, turn to adjust values.
- **Per-plant profiles** — Ficus, Succulent, and Fern each have their own
  dry/wet moisture thresholds, pulse duration, and rest interval, all
  editable from the on-device menu and persisted to EEPROM.
- **Pulsed watering** — waters in short bursts with a rest interval between
  them instead of one continuous pour, so slow-draining soil has time to
  absorb moisture before the next check.
- **Safety cutoffs**
  - A maximum-duration timeout aborts a watering cycle (and flags it on the
    display) if the moisture reading never reaches the wet threshold —
    guards against an empty reservoir, a dislodged probe, or a clogged line.
  - Skips new watering pulses below 15°C, since cold soil absorbs water more
    slowly and would otherwise waterlog rather than hydrate.
  - A hardware watchdog timer force-resets the board if the main loop ever
    stalls, with the relay pin defaulting to OFF as the very first action on
    boot/reset.

## Hardware

| Component | Notes |
|---|---|
| Arduino Uno (or compatible) | ATmega328p-based |
| SSD1306 128x64 OLED | I2C, address `0x3C` |
| Capacitive soil moisture sensor | Analog output → `A0` |
| DHT11 temperature/humidity sensor | Digital pin `5` |
| Rotary encoder (KY-040 or similar) | `CLK`→2, `DT`→3, `SW`→4 |
| Relay module | Signal → pin `7`; **active-LOW** (drive LOW to energize) |
| Small DC water pump | Powered through the relay's switched contacts |

### Wiring notes

- Power the pump from a **separate supply** from the Arduino/OLED, sharing
  only a common ground through the relay's switched side — this avoids the
  pump's current draw sagging the same rail powering your logic.
- Put a **flyback/rectifier diode across the pump motor terminals**. Brushed
  DC pump motors generate continuous electrical noise while running, which
  can corrupt the I2C bus and hang the display indefinitely (the standard
  `Wire` library has no timeout). This one part fixes it.
- If you still see occasional display corruption, also try a bulk
  decoupling capacitor (100–470µF) across 5V/GND near the pump/relay, and
  make sure the relay module has its own flyback diode across the coil.

## Calibration

Every capacitive moisture sensor reads differently. Before relying on this
for an actual plant:

1. Open the settings menu and note the raw ADC value with the probe
   completely dry (out of soil/water) and fully submerged in water.
2. Set each plant profile's `dryLimit` a bit below your dry reading and
   `wetLimit` a bit above your wet reading, leaving margin at both physical
   extremes.
3. Watch the live moisture % for a few days and nudge the thresholds to
   taste — a Ficus, Succulent, and Fern all want very different watering
   behavior.

## License

_Add a license of your choice before publishing (e.g. MIT)._

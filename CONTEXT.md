# Project Context for Claude Code Sessions

This file captures the development context and decisions made during this project so future sessions can pick up where we left off.

## Project Overview

Automatic Nut & Water Dispensing System — an Arduino-based device that dispenses nuts (via stepper motor) and water (via MOSFET-controlled pump) on a user-configured daily schedule. The interface uses a 1.3" OLED display with an EC11 rotary encoder module.

## Hardware Summary

| Component | Model/Driver | Interface |
|-----------|-------------|-----------|
| Microcontroller | Arduino Pro Micro (ATmega32U4, 5V/16MHz) | USB |
| Display | 1.3" OLED, **SH1106** (not SSD1306), 128x64 | I2C @ 0x3C |
| Encoder module | EC11 + OLED combo board (Amazon B0DMYQHM9J) | Digital pins |
| Nut dispenser | 28BYJ-48 stepper via ULN2003 | Pins 9,10,14,15 |
| Water pump | MOSFET on pin 16 (active HIGH) | Digital |

## Pin Allocation

| Pin | Assignment |
|-----|-----------|
| 2 | SDA (OLED I2C) |
| 3 | SCL (OLED I2C) |
| 4 | CON button |
| 5 | Encoder TRB |
| 6 | Encoder PSH (push) |
| 7 | Encoder TRA (INT6 interrupt) |
| 8 | BAK button |
| 9 | ULN2003 IN1 |
| 10 | ULN2003 IN2 |
| 14 | ULN2003 IN3 |
| 15 | ULN2003 IN4 |
| 16 | MOSFET gate (water pump) |
| **Free:** | 0, 1, A0, A1, A2, A3 |

## Key Technical Decisions & Lessons Learned

### Display
- The 1.3" OLED uses **SH1106**, not SSD1306. SSD1306 caused garbage pixels on the right edge due to 132 vs 128 pixel memory width.
- Adafruit SSD1306 library used too much RAM (1KB buffer). Switched to **U8g2** with page buffering (128 bytes) to fit in 2.5KB RAM.

### Encoder
- Pin 4 does NOT support hardware interrupts on Pro Micro. Only pins 0, 1, 2, 3, 7 do.
- Pins 2 and 3 are used for I2C, so encoder TRA was moved to **pin 7 (INT6)**.
- CON button was swapped from pin 7 to pin 4 to free up the interrupt pin.

### Stepper Motor
- Half-step mode was jerky. Switched to **full-step** for smoother operation and more torque.
- Step delay: **5ms** between steps works well. 2ms was too fast and caused missed steps.
- **Critical:** OLED page-buffer redraws take ~200-300ms and block the main loop. During stepper dispensing, display redraws are **throttled to once per second** so the stepper gets stepped at full speed.
- Coils are de-energized when idle (`stopMotor()`) to save power and reduce heat.

### Water Pump
- Originally simulated with TX LED (pin 30, active LOW).
- Now uses a **MOSFET on pin 16** (active HIGH) with 220Ω gate resistor and 10kΩ pull-down.

### Button Mapping
- **PSH** (encoder push): select/confirm everywhere, open menu from home
- **BAK**: go back
- **CON**: only used on home screen to re-arm schedule after dispensing completes
- This mapping evolved through user feedback — PSH is the primary action button.

### Time Keeping
- Software clock using `millis()` offset. No RTC module.
- Time must be set on every boot. Will drift over days.

## Dispensing Sequence
1. At scheduled time: stepper spins (nuts) for set duration
2. Stepper stops, coils off
3. MOSFET turns on (water pump) for same duration
4. Display shows "Done! CON to restart"
5. User presses CON to re-arm for next day

## Planned Features
- **HX711 + Load Cell** (pins A0, A1): verify cup is placed, confirm nuts dispensed, confirm water dispensed by reading weight changes.

## Build & Upload
```bash
arduino-cli compile --fqbn arduino:avr:leonardo .
arduino-cli upload -p /dev/cu.usbmodem11101 --fqbn arduino:avr:leonardo .
```
Note: USB port changes frequently on reconnect. Always run `arduino-cli board list` to find the current port.

## Repository
GitHub: https://github.com/himesh1729/ans2

## File Structure
- `ans2.ino` — main application sketch
- `stepper_test/stepper_test.ino` — standalone stepper motor test
- `dependencies.yaml` — hardware and library dependencies
- `README.md` — user-facing documentation
- `CONTEXT.md` — this file (development context for AI sessions)

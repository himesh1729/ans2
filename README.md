# Automatic Nut & Water Dispensing System

A scheduled dispensing system built with an Arduino Pro Micro and a 1.3" OLED + EC11 rotary encoder module. At a user-configured time each day, the system dispenses nuts for a set duration, then immediately dispenses water for the same duration.

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | Arduino Pro Micro (ATmega32U4, 5V/16MHz) |
| Display | 1.3" OLED, SH1106 driver, 128x64, I2C (0x3C) |
| Encoder | EC11 rotary encoder with push button |
| Buttons | CON (confirm) and BAK (back) on the module |
| Outputs | Water pump relay + Nut dispenser motor relay (currently simulated by onboard TX/RX LEDs) |

## Wiring

| Module Pin | Pro Micro Pin | Function |
|-----------|---------------|----------|
| VCC | VCC (5V) | Power |
| GND | GND | Ground |
| SDA | Pin 2 | I2C data (OLED) |
| SCL | Pin 3 | I2C clock (OLED) |
| TRA | Pin 7 | Encoder channel A (INT6) |
| TRB | Pin 5 | Encoder channel B |
| PSH | Pin 6 | Encoder push button |
| CON | Pin 4 | Confirm button |
| BAK | Pin 8 | Back button |

**Output pins (accent LEDs for now):**
| Signal | Pin | Notes |
|--------|-----|-------|
| Nut dispenser | Pin 17 (RX LED) | Active LOW, replace with relay GPIO |
| Water pump | Pin 30 (TX LED) | Active LOW, replace with relay GPIO |

## Dependencies

Install via Arduino CLI or Arduino IDE Library Manager:

```bash
arduino-cli lib install "U8g2"
```

Core: `arduino:avr` (Board: `arduino:avr:leonardo`)

## Build & Upload

```bash
# Compile
arduino-cli compile --fqbn arduino:avr:leonardo .

# Upload (adjust port as needed)
arduino-cli upload -p /dev/cu.usbmodem214401 --fqbn arduino:avr:leonardo .
```

## User Interface

### Controls

| Button | Action |
|--------|--------|
| **Rotary encoder** | Scroll through values / menu items |
| **PSH** (encoder push) | Select / confirm in all screens; open menu from home |
| **BAK** | Go back to previous screen |
| **CON** | Re-arm schedule after dispensing completes (home screen only) |

### Screens

1. **Set Clock** (shown on boot)
   - Set hour (1-12) -> minute (0-59) -> AM/PM
   - Uses PSH to confirm each step, BAK to go back

2. **Home Screen**
   - Shows current time (12h format with AM/PM)
   - Shows scheduled time and duration (if set)
   - Shows dispense status: Idle / Dispensing NUTS / Dispensing WATER / Done
   - PSH opens menu, CON re-arms schedule after dispensing

3. **Main Menu**
   - Set Schedule — configure dispense time and duration
   - Set Clock — re-set the current time
   - Back — return to home

4. **Set Schedule**
   - Set hour (1-12) -> minute (0-59) -> AM/PM -> duration (1-120 seconds)
   - Schedule persists until changed; no need to re-enter daily

### Dispensing Sequence

When the clock reaches the scheduled time:

1. **Nut dispenser** turns ON for the set duration
2. **Water pump** turns ON for the same duration (immediately after nuts finish)
3. Display shows **"Done! CON to restart"**
4. Press **CON** to re-arm the schedule for the next trigger

## Time Keeping

The system uses a software clock based on `millis()`. The user sets the current time on boot. There is no RTC module, so:

- Time resets on power loss (user must re-set)
- Time may drift slightly over days (~1-2 seconds/day typical for a crystal oscillator)

## Replacing LEDs with Real Hardware

To connect actual pump and motor relays, change the pin definitions and wiring:

```cpp
#define PUMP_LED <your_pump_relay_pin>
#define NUT_LED  <your_nut_relay_pin>
```

The outputs are **active LOW** — `digitalWrite(pin, LOW)` turns ON, `HIGH` turns OFF. If your relay module is active HIGH, invert the logic in `checkDispensing()` and `setup()`.

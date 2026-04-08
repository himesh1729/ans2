# Automatic Nut & Water Dispensing System

A scheduled dispensing system built with an Arduino Pro Micro and a 1.3" OLED + EC11 rotary encoder module. At a user-configured time each day, the system dispenses nuts via a stepper motor for a set duration, then immediately runs a water pump for the same duration.

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | Arduino Pro Micro (ATmega32U4, 5V/16MHz) |
| Display | 1.3" OLED, SH1106 driver, 128x64, I2C (0x3C) |
| Encoder | EC11 rotary encoder with push button |
| Buttons | CON (confirm) and BAK (back) on the module |
| Nut Dispenser | 28BYJ-48 stepper motor via ULN2003 driver board |
| Water Pump | Relay module (currently simulated by onboard TX LED) |

## Wiring

### OLED + Encoder Module

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

### ULN2003 Stepper Driver (Nut Dispenser)

| ULN2003 Pin | Pro Micro Pin |
|-------------|---------------|
| IN1 | Pin 9 |
| IN2 | Pin 10 |
| IN3 | Pin 14 |
| IN4 | Pin 15 |
| VCC | 5V (external supply recommended for motor current) |
| GND | GND (shared with Pro Micro) |

The 28BYJ-48 motor plugs into the ULN2003 board's 5-pin white connector.

### Water Pump

| Signal | Pin | Notes |
|--------|-----|-------|
| Water pump | Pin 30 (TX LED) | Active LOW; replace with relay GPIO for real pump |

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
arduino-cli upload -p /dev/cu.usbmodem11101 --fqbn arduino:avr:leonardo .
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

1. **Stepper motor** spins for the set duration (nut dispensing)
2. Motor stops and coils are de-energized
3. **Water pump** turns ON for the same duration (immediately after nuts finish)
4. Display shows **"Done! CON to restart"**
5. Press **CON** to re-arm the schedule for the next trigger

## Technical Notes

### Stepper Motor
- Uses full-step sequence (4 phases) for maximum torque
- Step interval: 5ms between steps
- Coils are de-energized when idle to save power and reduce heat
- During dispensing, OLED redraws are throttled to once per second so the main loop stays fast enough to drive the stepper smoothly

### Time Keeping
The system uses a software clock based on `millis()`. The user sets the current time on boot. There is no RTC module, so:

- Time resets on power loss (user must re-set)
- Time may drift slightly over days (~1-2 seconds/day typical for a crystal oscillator)

### Memory Usage
U8g2 page buffering mode is used (128 bytes per page) to fit within the Pro Micro's 2.5KB RAM. The sketch uses ~44% of dynamic memory.

## Replacing TX LED with Real Water Pump

To connect an actual pump relay, change the pin definition:

```cpp
#define PUMP_LED <your_pump_relay_pin>
```

The output is **active LOW** — `digitalWrite(pin, LOW)` turns ON, `HIGH` turns OFF. If your relay module is active HIGH, invert the logic in `checkDispensing()` and `setup()`.

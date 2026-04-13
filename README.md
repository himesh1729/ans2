# Automatic Nut & Water Dispensing System

A scheduled dispensing system built with an Arduino Pro Micro and a 1.3" OLED + EC11 rotary encoder module. At a user-configured time each day, the system dispenses nuts via a stepper motor, then immediately runs a water pump. Each has its own configurable duration.

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | Arduino Pro Micro (ATmega32U4, 5V/16MHz) |
| Display | 1.3" OLED, SH1106 driver, 128x64, I2C (0x3C) |
| Encoder | EC11 rotary encoder with push button |
| Buttons | CON (confirm) and BAK (back) on the module |
| Nut Dispenser | 28BYJ-48 stepper motor via ULN2003 driver board |
| Water Pump | MOSFET-controlled DC pump |
| RTC | DS3231 module (I2C at 0x68) for accurate timekeeping |

## Wiring

### OLED + Encoder Module

| Module Pin | Pro Micro Pin | Function |
|-----------|---------------|----------|
| VCC | VCC (5V) | Power |
| GND | GND | Ground |
| SDA | Pin 2 | I2C data (OLED & RTC) |
| SCL | Pin 3 | I2C clock (OLED & RTC) |
| TRA | Pin 7 | Encoder channel A (INT6) |
| TRB | Pin 5 | Encoder channel B |
| PSH | Pin 6 | Encoder push button |
| CON | Pin 4 | Confirm button |
| BAK | Pin 8 | Back button |

### DS3231 RTC Module

| RTC Pin | Pro Micro Pin |
|---------|---------------|
| + (VCC) | VCC (5V) |
| D (SDA) | Pin 2 |
| C (SCL) | Pin 3 |
| - (GND) | GND |
| NC | Not connected |

### ULN2003 Stepper Driver (Nut Dispenser)

| ULN2003 Pin | Pro Micro Pin |
|-------------|---------------|
| IN1 | Pin 10 |
| IN2 | Pin 16 |
| IN3 | Pin 14 |
| IN4 | Pin 15 |
| VCC | 5V (external supply recommended for motor current) |
| GND | GND (shared with Pro Micro) |

The 28BYJ-48 motor plugs into the ULN2003 board's 5-pin white connector.

### Water Pump (MOSFET)

| Connection | Wiring |
|-----------|--------|
| MOSFET Gate | Pin 9 (via 220Ω resistor) |
| MOSFET Source | GND |
| MOSFET Drain | Pump negative terminal |
| Pump positive | 5V (external supply) |
| Gate pull-down | 10kΩ resistor between Gate and GND |

The MOSFET is **active HIGH** — `digitalWrite(pin, HIGH)` turns the pump on.

## Dependencies

Install via Arduino CLI or Arduino IDE Library Manager:

```bash
arduino-cli lib install "U8g2" "RTClib"
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
   - Set hour (1-12) -> minute (0-59) -> AM/PM -> nut duration (1-120s) -> water duration (1-120s)
   - Schedule persists in EEPROM; no need to re-enter after power loss

### Dispensing Sequence

When the clock reaches the scheduled time:

1. **Stepper motor** spins for the nut duration (nut dispensing)
2. Motor stops and coils are de-energized
3. **Water pump** turns ON for the water duration (immediately after nuts finish)
4. Display shows **"Done! CON to restart"**
5. Press **CON** to re-arm the schedule for the next trigger

## Technical Notes

### Stepper Motor
- Uses full-step sequence (4 phases) for maximum torque
- Step interval: 5ms between steps
- Coils are de-energized when idle to save power and reduce heat
- During dispensing, stepper runs continuously (blocking) for smooth operation

### Time Keeping
The system uses a DS3231 RTC module for accurate timekeeping:

- Time persists across power cycles (battery backup on RTC module)
- Accuracy: ±2 ppm (~1 minute/year drift)
- If RTC has valid time on boot, clock setup is skipped

### Schedule Persistence
Schedule is saved to EEPROM and restored on boot. Both time and durations persist across power cycles.

### Memory Usage
U8g2 page buffering mode is used (128 bytes per page) to fit within the Pro Micro's 2.5KB RAM. The sketch uses ~45% of dynamic memory.

## Available Pins

| Pin | Status |
|-----|--------|
| A0 | Free |
| A1 | Free |
| A2 | Free |
| A3 | Free |
| 0 (TX) | Free (avoid if using Serial) |
| 1 (RX) | Free (avoid if using Serial) |

Planned: HX711 load cell (A0, A1) for cup/dispense verification.

/*
 * Automatic Nut & Water Dispensing System
 * =======================================
 *
 * Hardware:
 *   - Arduino Pro Micro (ATmega32U4)
 *   - 1.3" OLED display (SH1106, 128x64, I2C at 0x3C)
 *   - EC11 rotary encoder with push button
 *   - CON (confirm) and BAK (back) buttons on the module
 *   - Water pump via MOSFET on pin 9
 *   - 28BYJ-48 stepper motor via ULN2003 driver (nut dispenser)
 *   - DS3231 RTC module (I2C at 0x68)
 *
 * Pin Mapping:
 *   TRA (Encoder A) -> Pin 7  (interrupt-capable INT6)
 *   TRB (Encoder B) -> Pin 5
 *   PSH (Encoder push) -> Pin 6
 *   CON (Confirm btn) -> Pin 4
 *   BAK (Back btn)    -> Pin 8
 *   SDA -> Pin 2 (I2C data)
 *   SCL -> Pin 3 (I2C clock)
 *   IN1 (ULN2003) -> Pin 10
 *   IN2 (ULN2003) -> Pin 16
 *   IN3 (ULN2003) -> Pin 14
 *   IN4 (ULN2003) -> Pin 15
 *   PUMP (MOSFET gate) -> Pin 9
 *
 * Button Roles:
 *   PSH  - Select/confirm in all menus; open menu from home screen
 *   BAK  - Go back to previous screen
 *   CON  - Re-arm schedule after dispensing completes (home screen only)
 *
 * How It Works:
 *   1. On boot, user sets the current time (hour, minute, AM/PM).
 *   2. From the home screen, press PSH to open the menu.
 *   3. "Set Schedule" lets user pick a dispense time and duration.
 *   4. At the scheduled time each day:
 *      - Nut dispenser runs for the set duration
 *      - Immediately after, water pump runs for the same duration
 *   5. After both finish, display shows "Done! CON to restart".
 *   6. Press CON to re-arm the same schedule for the next trigger.
 *   7. Schedule persists — no need to re-enter time/duration each day.
 *
 * Time Keeping:
 *   DS3231 RTC module keeps accurate time with battery backup.
 *   Time persists across power cycles. If RTC has valid time, clock
 *   setup is skipped on boot.
 *
 * Display Library:
 *   U8g2 with page buffering (128-byte pages) to fit within the
 *   Pro Micro's 2.5KB RAM. During stepper dispensing, display redraws
 *   are throttled to once per second so the main loop runs fast enough
 *   to step the motor at 5ms intervals.
 *
 * Stepper Motor:
 *   28BYJ-48 driven via ULN2003 (full-step sequence, 5ms step delay).
 *   Coils are de-energized when idle to save power and reduce heat.
 */

#include <U8g2lib.h>
#include <Wire.h>
#include <RTClib.h>
#include <EEPROM.h>

// --- Display: SH1106 128x64 OLED over hardware I2C ---
U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// --- DS3231 RTC ---
RTC_DS3231 rtc;

// --- Pin definitions ---
#define TRA 7   // Encoder channel A (uses INT6 for hardware interrupt)
#define TRB 5   // Encoder channel B
#define PSH 6   // Encoder push button (select/confirm)
#define CON 4   // Confirm button (re-arm schedule only)
#define BAK 8   // Back button

#define PUMP_PIN 9   // MOSFET gate for water pump (HIGH = on)

// --- Stepper motor (28BYJ-48 via ULN2003) for nut dispenser ---
#define IN1 10
#define IN2 16
#define IN3 14
#define IN4 15

// Full-step sequence (4 steps, good torque)
const uint8_t stepSeq[4][4] = {
  {1,1,0,0}, {0,1,1,0}, {0,0,1,1}, {1,0,0,1}
};
int stepIdx = 0;
unsigned long lastStepTime = 0;
#define STEP_DELAY_MS 5  // milliseconds between steps

// Advance stepper one step (dir: 1=CW, -1=CCW). Non-blocking.
void stepMotor(int dir) {
  stepIdx = (stepIdx + dir + 4) % 4;
  digitalWrite(IN1, stepSeq[stepIdx][0]);
  digitalWrite(IN2, stepSeq[stepIdx][1]);
  digitalWrite(IN3, stepSeq[stepIdx][2]);
  digitalWrite(IN4, stepSeq[stepIdx][3]);
}

// Turn off all coils (saves power when idle)
void stopMotor() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

// =====================================================================
// Rotary Encoder (interrupt-driven)
// =====================================================================
// TRA is on pin 7 (INT6), so we use a hardware interrupt on FALLING edge.
// On each detent, we compare TRB to TRA to determine rotation direction.

volatile int encDelta = 0;

void encoderISR() {
  if (digitalRead(TRB) != digitalRead(TRA)) encDelta++;
  else encDelta--;
}

// Read and reset accumulated encoder ticks since last call
int readEncoder() {
  int d = encDelta;
  encDelta = 0;
  return d;
}

// =====================================================================
// Button Debounce
// =====================================================================
// Simple edge detector: returns true once per HIGH->LOW transition.
// Uses internal pull-ups, so pressed = LOW.

bool btnPressed(int pin, int &last) {
  int v = digitalRead(pin);
  bool p = (v == LOW && last == HIGH);
  last = v;
  return p;
}

int lastPsh = 1, lastCon = 1, lastBak = 1;

// =====================================================================
// RTC Time Functions
// =====================================================================

DateTime now() { return rtc.now(); }

// Current seconds since midnight
long nowSecs() {
  DateTime n = now();
  return (long)n.hour() * 3600L + (long)n.minute() * 60L + n.second();
}

// 12-hour format helpers
int nowHour12() {
  int h = now().hour() % 12;
  return h == 0 ? 12 : h;
}
int nowMin()    { return now().minute(); }
int nowSecond() { return now().second(); }
bool nowIsPM()  { return now().hour() >= 12; }

// Convert 12-hour + AM/PM to 24-hour format
int to24(int h12, bool pm) {
  if (pm) return h12 == 12 ? 12 : h12 + 12;
  else return h12 == 12 ? 0 : h12;
}

// =====================================================================
// Schedule & Dispensing State Machine
// =====================================================================

struct Schedule {
  int hour24;         // scheduled hour in 24h format (0-23)
  uint8_t minute;     // scheduled minute (0-59)
  uint8_t nutDur;     // nut dispense duration in seconds (1-120)
  uint8_t waterDur;   // water dispense duration in seconds (1-120)
  bool set;           // true once user has configured a schedule
};

Schedule sched = {0, 0, 10, 10, false};

// EEPROM addresses for schedule persistence
#define EEPROM_MAGIC_ADDR 0      // 1 byte: magic number to detect valid data
#define EEPROM_HOUR_ADDR  1      // 1 byte: hour24 (0-23)
#define EEPROM_MIN_ADDR   2      // 1 byte: minute (0-59)
#define EEPROM_NUT_DUR_ADDR   3  // 1 byte: nut duration (1-120)
#define EEPROM_WATER_DUR_ADDR 4  // 1 byte: water duration (1-120)
#define EEPROM_MAGIC_VAL  0xA6   // Magic value (changed to invalidate old format)

// Save schedule to EEPROM
void saveSchedule() {
  EEPROM.update(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  EEPROM.update(EEPROM_HOUR_ADDR, (uint8_t)sched.hour24);
  EEPROM.update(EEPROM_MIN_ADDR, sched.minute);
  EEPROM.update(EEPROM_NUT_DUR_ADDR, sched.nutDur);
  EEPROM.update(EEPROM_WATER_DUR_ADDR, sched.waterDur);
}

// Load schedule from EEPROM (returns true if valid data found)
bool loadSchedule() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) return false;
  sched.hour24 = EEPROM.read(EEPROM_HOUR_ADDR);
  sched.minute = EEPROM.read(EEPROM_MIN_ADDR);
  sched.nutDur = EEPROM.read(EEPROM_NUT_DUR_ADDR);
  sched.waterDur = EEPROM.read(EEPROM_WATER_DUR_ADDR);
  sched.set = true;
  return true;
}

// Dispense state machine:
//   0 = idle (waiting for scheduled time)
//   1 = nut dispenser running
//   2 = water pump running (starts immediately after nuts finish)
//   3 = done, waiting for user to press CON to re-arm
uint8_t dispState = 0;
unsigned long dispStarted = 0;
int lastTriggerMin = -1;  // prevents re-triggering within the same minute

// Forward declaration (Arduino auto-prototype doesn't handle struct refs)
void checkDispensing();

// =====================================================================
// Menu / Screen State
// =====================================================================

enum Screen {
  CLOCK_HOUR, CLOCK_MIN, CLOCK_AMPM,   // Initial clock setup
  HOME,                                  // Home screen with time & status
  MAIN_MENU,                             // Menu: Set Schedule / Set Clock / Back
  SCHED_HOUR, SCHED_MIN, SCHED_AMPM, SCHED_NUT_DUR, SCHED_WATER_DUR  // Schedule setup wizard
};

Screen screen = CLOCK_HOUR;  // Boot into clock setup
int menuPos = 0;             // Highlighted menu item index
int editVal = 12;            // Current value being edited by encoder
bool editPM = false;         // AM/PM toggle state during editing
int savedHour = 12;          // Stashed hour value across edit screens
int savedMin = 0;            // Stashed minute value across edit screens
int schedHour = 12;          // Stashed hour for schedule wizard
int schedMin = 0;            // Stashed minute for schedule wizard

// =====================================================================
// Display Drawing Functions
// =====================================================================

// Draw a centered title with underline
void drawTitle(const char *t) {
  u8g2.setFont(u8g2_font_ncenB10_tr);
  int w = u8g2.getStrWidth(t);
  u8g2.drawStr((128 - w) / 2, 14, t);
  u8g2.drawHLine(0, 17, 128);
}

// Generic edit screen: title, label, and centered value string
void drawValScreen(const char *title, const char *label, const char *valStr) {
  drawTitle(title);
  u8g2.setFont(u8g2_font_ncenB10_tr);
  u8g2.drawStr(10, 38, label);
  int w = u8g2.getStrWidth(valStr);
  u8g2.drawStr((128 - w) / 2, 58, valStr);
}

// Home screen: current time, schedule info, dispense status, button hints
void drawHome() {
  char buf[24];

  // Current time (large, centered)
  u8g2.setFont(u8g2_font_ncenB12_tr);
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s",
           nowHour12(), nowMin(), nowSecond(), nowIsPM() ? "PM" : "AM");
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - w) / 2, 16, buf);
  u8g2.drawHLine(0, 19, 128);

  // Schedule info
  u8g2.setFont(u8g2_font_6x10_tr);
  if (sched.set) {
    int sh = sched.hour24 % 12;
    if (sh == 0) sh = 12;
    bool spm = sched.hour24 >= 12;
    snprintf(buf, sizeof(buf), "%02d:%02d%s N%ds W%ds",
             sh, sched.minute, spm ? "P" : "A", sched.nutDur, sched.waterDur);
    u8g2.drawStr(4, 36, buf);
  } else {
    u8g2.drawStr(4, 36, "No schedule set");
  }

  // Dispense state
  if (dispState == 1)
    u8g2.drawStr(4, 50, ">> Dispensing NUTS");
  else if (dispState == 2)
    u8g2.drawStr(4, 50, ">> Dispensing WATER");
  else if (dispState == 3)
    u8g2.drawStr(4, 50, "Done! CON to restart");
  else
    u8g2.drawStr(4, 50, "Idle");

  // Button hints
  u8g2.drawStr(4, 64, "PSH:menu  CON:restart");
}

// Main menu with highlighted selection
void drawMainMenu() {
  drawTitle("Menu");
  u8g2.setFont(u8g2_font_ncenB10_tr);
  const char *items[] = {"Set Schedule", "Set Clock", "Back"};
  for (int i = 0; i < 3; i++) {
    int y = 34 + i * 16;
    if (i == menuPos) {
      // Inverted highlight for selected item
      u8g2.drawBox(0, y - 12, 128, 15);
      u8g2.setDrawColor(0);
      u8g2.drawStr(10, y, items[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(10, y, items[i]);
    }
  }
}

// =====================================================================
// Setup
// =====================================================================

void setup() {
  // Configure button pins with internal pull-ups
  pinMode(TRA, INPUT_PULLUP);
  pinMode(TRB, INPUT_PULLUP);
  pinMode(PSH, INPUT_PULLUP);
  pinMode(CON, INPUT_PULLUP);
  pinMode(BAK, INPUT_PULLUP);

  // Water pump MOSFET (active HIGH — LOW = off)
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);

  // Stepper motor pins
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  stopMotor();

  // Encoder interrupt on pin 7 (INT6)
  attachInterrupt(digitalPinToInterrupt(TRA), encoderISR, FALLING);

  u8g2.begin();

  // Initialize RTC
  if (!rtc.begin()) {
    // RTC not found — show error and halt
    u8g2.firstPage();
    do {
      u8g2.setFont(u8g2_font_ncenB10_tr);
      u8g2.drawStr(10, 40, "RTC not found!");
    } while (u8g2.nextPage());
    while (1);  // halt
  }

  // Skip clock setup if RTC has valid time (battery maintained)
  if (!rtc.lostPower()) {
    screen = HOME;
  }

  // Load saved schedule from EEPROM
  loadSchedule();

  editVal = 12;
  editPM = false;
}

// =====================================================================
// Dispensing Logic
// =====================================================================
// Called every loop iteration. Checks if current time matches schedule
// and manages the nuts -> water -> done state transitions.

void checkDispensing() {
  // Check if it's time to trigger
  if (sched.set && dispState == 0) {
    int curMin = (nowSecs() / 60) % 1440;
    int schedMin = sched.hour24 * 60 + sched.minute;
    if (curMin == schedMin && lastTriggerMin != curMin) {
      dispState = 1;
      dispStarted = millis();
      lastTriggerMin = curMin;
    }
    // Reset trigger guard once we've moved past the scheduled minute
    if (curMin != schedMin) {
      lastTriggerMin = -1;
    }
  }

  // Nut dispenser running: run motor continuously (blocking) for nutDur seconds
  if (dispState == 1) {
    unsigned long endTime = dispStarted + (unsigned long)sched.nutDur * 1000UL;
    while (millis() < endTime) {
      stepMotor(-1);
      delayMicroseconds(STEP_DELAY_MS * 1000UL);
    }
    stopMotor();
    dispState = 2;
    dispStarted = millis();
    digitalWrite(PUMP_PIN, HIGH);  // Turn on water pump
  }

  // Water duration elapsed -> done, wait for user acknowledgment
  if (dispState == 2 && millis() - dispStarted >= (unsigned long)sched.waterDur * 1000UL) {
    digitalWrite(PUMP_PIN, LOW);   // Turn off water pump
    dispState = 3;
  }
}

// =====================================================================
// Main Loop
// =====================================================================

void loop() {
  int enc = readEncoder();
  bool con = btnPressed(CON, lastCon);
  bool bak = btnPressed(BAK, lastBak);
  bool psh = btnPressed(PSH, lastPsh);
  char buf[24];

  // --- Input handling per screen ---
  switch (screen) {

    // --- Initial clock setup (shown on boot) ---
    case CLOCK_HOUR:
      editVal = constrain(editVal + enc, 1, 12);
      if (psh) { savedHour = editVal; editVal = 0; screen = CLOCK_MIN; }
      break;

    case CLOCK_MIN:
      editVal = constrain(editVal + enc, 0, 59);
      if (psh) { savedMin = editVal; editPM = false; screen = CLOCK_AMPM; }
      if (bak) { editVal = savedHour; screen = CLOCK_HOUR; }
      break;

    case CLOCK_AMPM:
      if (enc != 0) editPM = !editPM;
      if (psh) {
        // Commit the user-set time to the RTC
        int h24 = to24(savedHour, editPM);
        rtc.adjust(DateTime(2024, 1, 1, h24, savedMin, 0));
        screen = HOME;
      }
      if (bak) { editVal = savedMin; screen = CLOCK_MIN; }
      break;

    // --- Home screen ---
    case HOME:
      if (con && dispState == 3) {
        dispState = 0;  // Re-arm schedule with same settings
      }
      if (psh) { screen = MAIN_MENU; menuPos = 0; }
      break;

    // --- Main menu ---
    case MAIN_MENU:
      menuPos = constrain(menuPos + enc, 0, 2);
      if (psh) {
        if (menuPos == 0)      { screen = SCHED_HOUR; editVal = 12; editPM = false; }
        else if (menuPos == 1) { screen = CLOCK_HOUR; editVal = 12; editPM = false; }
        else                   { screen = HOME; }
      }
      if (bak) screen = HOME;
      break;

    // --- Schedule setup wizard ---
    case SCHED_HOUR:
      editVal = constrain(editVal + enc, 1, 12);
      if (psh) { schedHour = editVal; editVal = 0; screen = SCHED_MIN; }
      if (bak) screen = MAIN_MENU;
      break;

    case SCHED_MIN:
      editVal = constrain(editVal + enc, 0, 59);
      if (psh) { schedMin = editVal; editPM = false; screen = SCHED_AMPM; }
      if (bak) { editVal = schedHour; screen = SCHED_HOUR; }
      break;

    case SCHED_AMPM:
      if (enc != 0) editPM = !editPM;
      if (psh) {
        sched.hour24 = to24(schedHour, editPM);
        editVal = sched.nutDur;
        screen = SCHED_NUT_DUR;
      }
      if (bak) { editVal = schedMin; screen = SCHED_MIN; }
      break;

    case SCHED_NUT_DUR:
      editVal = constrain(editVal + enc, 1, 120);
      if (psh) {
        sched.nutDur = editVal;
        editVal = sched.waterDur;
        screen = SCHED_WATER_DUR;
      }
      if (bak) { screen = SCHED_AMPM; }
      break;

    case SCHED_WATER_DUR:
      editVal = constrain(editVal + enc, 1, 120);
      if (psh) {
        sched.waterDur = editVal;
        sched.set = true;
        sched.minute = schedMin;
        saveSchedule();  // Persist to EEPROM
        dispState = 0;
        lastTriggerMin = -1;
        screen = HOME;
      }
      if (bak) { editVal = sched.nutDur; screen = SCHED_NUT_DUR; }
      break;
  }

  checkDispensing();

  // Display refresh timing
  static unsigned long lastDraw = 0;
  bool needDraw = (millis() - lastDraw >= 50);

  if (needDraw) {
    lastDraw = millis();
    u8g2.firstPage();
    do {
      switch (screen) {
        case CLOCK_HOUR:
          snprintf(buf, sizeof(buf), "< %d h >", editVal);
          drawValScreen("Set Clock", "Hour:", buf);
          break;
        case CLOCK_MIN:
          snprintf(buf, sizeof(buf), "< %d m >", editVal);
          drawValScreen("Set Clock", "Minute:", buf);
          break;
        case CLOCK_AMPM:
          snprintf(buf, sizeof(buf), "< %s >", editPM ? "PM" : "AM");
          drawValScreen("Set Clock", "AM / PM:", buf);
          break;
        case HOME:        drawHome(); break;
        case MAIN_MENU:   drawMainMenu(); break;
        case SCHED_HOUR:
          snprintf(buf, sizeof(buf), "< %d h >", editVal);
          drawValScreen("Set Schedule", "Hour:", buf);
          break;
        case SCHED_MIN:
          snprintf(buf, sizeof(buf), "< %d m >", editVal);
          drawValScreen("Set Schedule", "Minute:", buf);
          break;
        case SCHED_AMPM:
          snprintf(buf, sizeof(buf), "< %s >", editPM ? "PM" : "AM");
          drawValScreen("Set Schedule", "AM / PM:", buf);
          break;
        case SCHED_NUT_DUR:
          snprintf(buf, sizeof(buf), "< %d s >", editVal);
          drawValScreen("Set Schedule", "Nuts:", buf);
          break;
        case SCHED_WATER_DUR:
          snprintf(buf, sizeof(buf), "< %d s >", editVal);
          drawValScreen("Set Schedule", "Water:", buf);
          break;
      }
    } while (u8g2.nextPage());
  }
}

/*
 * Automatic Nut & Water Dispensing System - Pico W Version
 * =========================================================
 *
 * Hardware:
 *   - Raspberry Pi Pico W
 *   - 1.3" OLED display (SH1106, 128x64, I2C at 0x3C)
 *   - EC11 rotary encoder with push button
 *   - CON (confirm) and BAK (back) buttons
 *   - Water pump via MOSFET on GP15
 *   - 28BYJ-48 stepper motor via ULN2003 driver (nut dispenser)
 *
 * Pin Mapping (Pico W):
 *   CON (Confirm btn) -> GP1
 *   SDA (OLED) -> GP2 (I2C1 data)
 *   SCL (OLED) -> GP3 (I2C1 clock)
 *   PSH (Encoder push) -> GP4
 *   TRA (Encoder A) -> GP5
 *   TRB (Encoder B) -> GP6
 *   BAK (Back btn) -> GP7
 *   IN1 (ULN2003) -> GP10
 *   IN2 (ULN2003) -> GP11
 *   IN3 (ULN2003) -> GP12
 *   IN4 (ULN2003) -> GP13
 *   PUMP (MOSFET gate) -> GP14
 *   SDA (RTC) -> GP16 (I2C0 data)
 *   SCL (RTC) -> GP17 (I2C0 clock)
 *   HX711 DOUT -> GP18
 *   HX711 SCK -> GP19
 *
 * Advantages over Pro Micro version:
 *   - More RAM: Full framebuffer display mode possible
 *   - Dual core: Could run stepper on second core (future)
 *   - 3.3V logic: Works directly with most modern sensors
 *
 * Time Keeping:
 *   DS3231 RTC module on I2C0 (GP16/GP17) for accurate timekeeping.
 *   Time persists across power cycles.
 *
 * Note: This is 3.3V logic. Ensure your MOSFET is logic-level
 *       compatible (e.g., IRLML6344) or use a gate driver.
 */

#include <U8g2lib.h>
#include <Wire.h>
#include <EEPROM.h>
#include <RTClib.h>
#include <HX711.h>

// --- RTC: DS3231 on I2C0 (GP16=SDA, GP17=SCL) ---
RTC_DS3231 rtc;

// --- HX711 Load Cell (GP18=DOUT, GP19=SCK) ---
#define HX711_DOUT 18
#define HX711_SCK  19
HX711 scale;

// --- Display: SH1106 128x64 OLED over I2C1 (GP2=SDA, GP3=SCL) ---
// Using full buffer mode since Pico W has plenty of RAM (264KB vs 2.5KB)
U8G2_SH1106_128X64_NONAME_F_2ND_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// --- Pin definitions (Pico W GPIO) ---
#define CON 1   // Confirm button (re-arm schedule only)
#define PSH 4   // Encoder push button (select/confirm)
#define TRA 5   // Encoder channel A
#define TRB 6   // Encoder channel B
#define BAK 7   // Back button

#define PUMP_PIN 14  // MOSFET gate for water pump (HIGH = on)

// --- Stepper motor (28BYJ-48 via ULN2003) for nut dispenser ---
#define IN1 10
#define IN2 11
#define IN3 12
#define IN4 13

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
// Rotary Encoder (simple polling, like Pro Micro)
// =====================================================================

volatile int encDelta = 0;
int lastA = 1;

// Call this frequently in loop()
void updateEncoder() {
  int a = digitalRead(TRA);
  if (a != lastA) {
    lastA = a;
    if (a == LOW) {  // Falling edge only
      if (digitalRead(TRB) != a) encDelta++;
      else encDelta--;
    }
  }
}

// Read and reset encoder delta
int readEncoder() {
  updateEncoder();
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
// Time Functions (DS3231 RTC)
// =====================================================================

// Get current hour (24h format)
int nowHour24() {
  DateTime now = rtc.now();
  return now.hour();
}

// Get current minute
int nowMin() {
  DateTime now = rtc.now();
  return now.minute();
}

// Get current second
int nowSecond() {
  DateTime now = rtc.now();
  return now.second();
}

// Current seconds since midnight
long nowSecs() {
  return (long)nowHour24() * 3600L + (long)nowMin() * 60L + nowSecond();
}

// 12-hour format helpers
int nowHour12() {
  int h = nowHour24() % 12;
  return h == 0 ? 12 : h;
}

bool nowIsPM() {
  return nowHour24() >= 12;
}

// Convert 12-hour + AM/PM to 24-hour format
int to24(int h12, bool pm) {
  if (pm) return h12 == 12 ? 12 : h12 + 12;
  else return h12 == 12 ? 0 : h12;
}

// Set time on RTC
void setTime(int h24, int m) {
  DateTime now = rtc.now();
  rtc.adjust(DateTime(now.year(), now.month(), now.day(), h24, m, 0));
}

// =====================================================================
// Schedule & Dispensing State Machine
// =====================================================================

struct Schedule {
  int hour24;         // scheduled hour in 24h format (0-23)
  uint8_t minute;     // scheduled minute (0-59)
  uint8_t nutGrams;   // nut dispense amount in grams (1-200)
  uint8_t waterDur;   // water dispense duration in seconds (1-120)
  bool set;           // true once user has configured a schedule
};

Schedule sched = {0, 0, 50, 10, false};  // Default 50g nuts

// HX711 calibration data
float scaleCalibration = 1.0;  // Scale factor (raw units per gram)
long scaleOffset = 0;          // Tare offset
bool scaleCalibrated = false;
#define KNOWN_WEIGHT_GRAMS 100  // Weight used for calibration (100g)

// EEPROM addresses for persistence
// Note: Pico W uses flash-emulated EEPROM
#define EEPROM_MAGIC_ADDR     0   // 1 byte: magic number
#define EEPROM_HOUR_ADDR      1   // 1 byte: hour24 (0-23)
#define EEPROM_MIN_ADDR       2   // 1 byte: minute (0-59)
#define EEPROM_NUT_GRAMS_ADDR 3   // 1 byte: nut grams (1-200)
#define EEPROM_WATER_DUR_ADDR 4   // 1 byte: water duration (1-120)
#define EEPROM_SCALE_CAL_ADDR 5   // 4 bytes: scale calibration (float)
#define EEPROM_SCALE_OFF_ADDR 9   // 4 bytes: scale offset (long)
#define EEPROM_MAGIC_VAL      0xA8  // New magic value (changed due to new format)

// Save schedule to EEPROM
void saveSchedule() {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  EEPROM.write(EEPROM_HOUR_ADDR, (uint8_t)sched.hour24);
  EEPROM.write(EEPROM_MIN_ADDR, sched.minute);
  EEPROM.write(EEPROM_NUT_GRAMS_ADDR, sched.nutGrams);
  EEPROM.write(EEPROM_WATER_DUR_ADDR, sched.waterDur);
  EEPROM.commit();
}

// Save scale calibration to EEPROM
void saveScaleCalibration() {
  EEPROM.put(EEPROM_SCALE_CAL_ADDR, scaleCalibration);
  EEPROM.put(EEPROM_SCALE_OFF_ADDR, scaleOffset);
  EEPROM.commit();
}

// Load schedule from EEPROM (returns true if valid data found)
bool loadSchedule() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) return false;
  sched.hour24 = EEPROM.read(EEPROM_HOUR_ADDR);
  sched.minute = EEPROM.read(EEPROM_MIN_ADDR);
  sched.nutGrams = EEPROM.read(EEPROM_NUT_GRAMS_ADDR);
  sched.waterDur = EEPROM.read(EEPROM_WATER_DUR_ADDR);
  sched.set = true;
  
  // Load scale calibration
  EEPROM.get(EEPROM_SCALE_CAL_ADDR, scaleCalibration);
  EEPROM.get(EEPROM_SCALE_OFF_ADDR, scaleOffset);
  if (scaleCalibration > 0.1 && scaleCalibration < 10000) {
    scaleCalibrated = true;
    scale.set_scale(scaleCalibration);
    scale.set_offset(scaleOffset);
  }
  return true;
}

// Get current weight in grams
float getWeight() {
  if (!scaleCalibrated) return 0;
  return scale.get_units(3);  // Average of 3 readings
}

// Tare the scale (set current weight as zero)
void tareScale() {
  scale.tare(10);  // Average of 10 readings
  scaleOffset = scale.get_offset();
}

// Dispense state machine:
//   0 = idle (waiting for scheduled time)
//   1 = nut dispenser running
//   2 = water pump running (starts immediately after nuts finish)
//   3 = done, waiting for user to press CON to re-arm
uint8_t dispState = 0;
unsigned long dispStarted = 0;
int lastTriggerMin = -1;  // prevents re-triggering within the same minute

// Forward declaration
void checkDispensing();

// =====================================================================
// Menu / Screen State
// =====================================================================

enum Screen {
  CLOCK_HOUR, CLOCK_MIN, CLOCK_AMPM,     // Clock setup on boot
  HOME,                                   // Home screen with time & status
  MAIN_MENU,                              // Menu: Set Schedule / Set Clock / Calibrate / Nuts / Water / Back
  INSTANT_NUTS, INSTANT_WATER,            // Instant dispense (hold PSH)
  SCHED_HOUR, SCHED_MIN, SCHED_AMPM, SCHED_NUT_GRAMS, SCHED_WATER_DUR,  // Schedule setup
  CALIB_TARE, CALIB_PLACE, CALIB_DONE     // Scale calibration wizard
};

Screen screen = CLOCK_HOUR;  // Boot into clock setup
int menuPos = 0;                  // Highlighted menu item index
int editVal = 12;                 // Current value being edited by encoder
bool editPM = false;              // AM/PM toggle state during editing
int savedHour = 12;               // Stashed hour value across edit screens
int savedMin = 0;                 // Stashed minute value across edit screens
int schedHour = 12;               // Stashed hour for schedule wizard
int schedMin = 0;                 // Stashed minute for schedule wizard

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
  char buf[32];

  // Current time (large, centered)
  u8g2.setFont(u8g2_font_ncenB12_tr);
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s",
           nowHour12(), nowMin(), nowSecond(), nowIsPM() ? "PM" : "AM");
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - w) / 2, 16, buf);
  u8g2.drawHLine(0, 19, 128);

  u8g2.setFont(u8g2_font_6x10_tr);

  // Schedule info
  if (sched.set) {
    int sh = sched.hour24 % 12;
    if (sh == 0) sh = 12;
    bool spm = sched.hour24 >= 12;
    snprintf(buf, sizeof(buf), "%02d:%02d%s N%dg W%ds",
             sh, sched.minute, spm ? "P" : "A", sched.nutGrams, sched.waterDur);
    u8g2.drawStr(4, 34, buf);
  } else {
    u8g2.drawStr(4, 34, "No schedule set");
  }

  // Current weight (if calibrated)
  if (scaleCalibrated) {
    snprintf(buf, sizeof(buf), "Weight: %.1fg", getWeight());
    u8g2.drawStr(4, 44, buf);
  }

  // Dispense state
  if (dispState == 1)
    u8g2.drawStr(4, 54, ">> Dispensing NUTS");
  else if (dispState == 2)
    u8g2.drawStr(4, 54, ">> Dispensing WATER");
  else if (dispState == 3)
    u8g2.drawStr(4, 54, "Done! CON to restart");
  else if (!scaleCalibrated)
    u8g2.drawStr(4, 54, "Scale not calibrated");
  else
    u8g2.drawStr(4, 54, "Idle");

  // Button hints
  u8g2.drawStr(4, 64, "PSH:menu  CON:restart");
}

// Main menu with highlighted selection
void drawMainMenu() {
  drawTitle("Menu");
  u8g2.setFont(u8g2_font_6x10_tr);
  const char *items[] = {"Schedule", "Clock", "Calibrate", "Nuts", "Water", "Back"};
  for (int i = 0; i < 6; i++) {
    int y = 24 + i * 8;
    if (i == menuPos) {
      // Inverted highlight for selected item
      u8g2.drawBox(0, y - 7, 128, 9);
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

bool rtcOK = false;

void setup() {
  // Initialize EEPROM (Pico W requires explicit size)
  EEPROM.begin(32);  // Increased for scale calibration data

  // Configure I2C0 for RTC (GP16=SDA, GP17=SCL)
  Wire.setSDA(16);
  Wire.setSCL(17);
  Wire.begin();

  // Configure I2C1 for OLED (GP2=SDA, GP3=SCL)
  Wire1.setSDA(2);
  Wire1.setSCL(3);
  Wire1.begin();

  // Initialize RTC
  if (rtc.begin(&Wire)) {
    rtcOK = true;
    if (rtc.lostPower()) {
      // RTC lost power, set to compile time as fallback
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  // Initialize HX711 load cell
  scale.begin(HX711_DOUT, HX711_SCK);

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

  // Initialize encoder state
  lastA = digitalRead(TRA);

  u8g2.begin();

  // Load saved schedule from EEPROM
  loadSchedule();

  // Skip clock setup if RTC has valid time
  if (rtcOK && !rtc.lostPower()) {
    screen = HOME;
  } else {
    editVal = 12;
    editPM = false;
  }
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
    int schedMinVal = sched.hour24 * 60 + sched.minute;
    if (curMin == schedMinVal && lastTriggerMin != curMin) {
      dispState = 1;
      dispStarted = millis();
      lastTriggerMin = curMin;
    }
    // Reset trigger guard once we've moved past the scheduled minute
    if (curMin != schedMinVal) {
      lastTriggerMin = -1;
    }
  }

  // Nut dispenser running: dispense until target weight reached
  if (dispState == 1) {
    // Show "Dispensing NUTS" before blocking
    u8g2.clearBuffer();
    drawHome();
    u8g2.sendBuffer();
    
    float targetWeight = (float)sched.nutGrams;
    float startWeight = scaleCalibrated ? getWeight() : 0;
    unsigned long timeout = millis() + 60000UL;  // 60 second timeout
    
    if (scaleCalibrated) {
      // Weight-based dispensing
      while (millis() < timeout) {
        float currentWeight = getWeight();
        if (currentWeight - startWeight >= targetWeight) break;
        stepMotor(-1);
        delayMicroseconds(STEP_DELAY_MS * 1000UL);
      }
    } else {
      // Fallback: time-based (10 seconds per 50g estimate)
      unsigned long duration = (unsigned long)sched.nutGrams * 200UL;  // ~200ms per gram
      unsigned long endTime = millis() + duration;
      while (millis() < endTime) {
        stepMotor(-1);
        delayMicroseconds(STEP_DELAY_MS * 1000UL);
      }
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

    // --- Clock setup (shown on boot) ---
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
        // Set time
        int h24 = to24(savedHour, editPM);
        setTime(h24, savedMin);
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
      menuPos = constrain(menuPos + enc, 0, 5);
      if (psh) {
        if (menuPos == 0)      { screen = SCHED_HOUR; editVal = 12; editPM = false; }
        else if (menuPos == 1) { screen = CLOCK_HOUR; editVal = 12; editPM = false; }
        else if (menuPos == 2) { screen = CALIB_TARE; }
        else if (menuPos == 3) { screen = INSTANT_NUTS; }
        else if (menuPos == 4) { screen = INSTANT_WATER; }
        else                   { screen = HOME; }
      }
      if (bak) screen = HOME;
      break;

    // --- Calibration wizard ---
    case CALIB_TARE:
      // User should remove all weight, press PSH to tare
      if (psh) {
        tareScale();
        screen = CALIB_PLACE;
      }
      if (bak) screen = MAIN_MENU;
      break;

    case CALIB_PLACE:
      // User places known weight (100g), press PSH to calibrate
      if (psh) {
        long rawValue = scale.get_units(10);  // Average of 10 readings (raw after tare)
        if (rawValue != 0) {
          scaleCalibration = (float)rawValue / KNOWN_WEIGHT_GRAMS;
          scale.set_scale(scaleCalibration);
          scaleCalibrated = true;
          saveScaleCalibration();
        }
        screen = CALIB_DONE;
      }
      if (bak) screen = CALIB_TARE;
      break;

    case CALIB_DONE:
      if (psh || bak) screen = HOME;
      break;

    // --- Instant dispense modes (hold PSH to dispense) ---
    case INSTANT_NUTS:
      // Dispense while PSH is held (check raw pin, not edge)
      if (digitalRead(PSH) == LOW) {
        stepMotor(-1);
        delayMicroseconds(STEP_DELAY_MS * 1000UL);
      } else {
        stopMotor();
      }
      if (bak) { stopMotor(); screen = MAIN_MENU; }
      break;

    case INSTANT_WATER:
      // Pump while PSH is held
      digitalWrite(PUMP_PIN, digitalRead(PSH) == LOW ? HIGH : LOW);
      if (bak) { digitalWrite(PUMP_PIN, LOW); screen = MAIN_MENU; }
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
        editVal = sched.nutGrams;
        screen = SCHED_NUT_GRAMS;
      }
      if (bak) { editVal = schedMin; screen = SCHED_MIN; }
      break;

    case SCHED_NUT_GRAMS:
      editVal = constrain(editVal + enc * 5, 5, 200);  // 5g increments, 5-200g
      if (psh) {
        sched.nutGrams = editVal;
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
      if (bak) { editVal = sched.nutGrams; screen = SCHED_NUT_GRAMS; }
      break;
  }

  checkDispensing();

  // --- Display refresh (using full buffer since Pico W has plenty of RAM) ---
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw >= 50) {
    lastDraw = millis();
    u8g2.clearBuffer();
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
      case HOME:
        drawHome();
        break;
      case MAIN_MENU:
        drawMainMenu();
        break;
      case INSTANT_NUTS:
        drawValScreen("Manual", "Hold PSH for", "NUTS");
        break;
      case INSTANT_WATER:
        drawValScreen("Manual", "Hold PSH for", "WATER");
        break;
      case CALIB_TARE:
        drawValScreen("Calibrate", "Remove weight", "Press PSH");
        break;
      case CALIB_PLACE:
        snprintf(buf, sizeof(buf), "Place %dg", KNOWN_WEIGHT_GRAMS);
        drawValScreen("Calibrate", buf, "Press PSH");
        break;
      case CALIB_DONE:
        drawValScreen("Calibrate", "Done!", "Press PSH");
        break;
      case SCHED_HOUR:
        snprintf(buf, sizeof(buf), "< %d h >", editVal);
        drawValScreen("Schedule", "Hour:", buf);
        break;
      case SCHED_MIN:
        snprintf(buf, sizeof(buf), "< %d m >", editVal);
        drawValScreen("Schedule", "Minute:", buf);
        break;
      case SCHED_AMPM:
        snprintf(buf, sizeof(buf), "< %s >", editPM ? "PM" : "AM");
        drawValScreen("Schedule", "AM / PM:", buf);
        break;
      case SCHED_NUT_GRAMS:
        snprintf(buf, sizeof(buf), "< %d g >", editVal);
        drawValScreen("Schedule", "Nuts:", buf);
        break;
      case SCHED_WATER_DUR:
        snprintf(buf, sizeof(buf), "< %d s >", editVal);
        drawValScreen("Schedule", "Water:", buf);
        break;
    }
    u8g2.sendBuffer();
  }
}

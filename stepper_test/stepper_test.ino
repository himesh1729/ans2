// Quick test: spin the 28BYJ-48 stepper via ULN2003
// IN1=9, IN2=10, IN3=14, IN4=15

#include <U8g2lib.h>
#include <Wire.h>

U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

#define IN1 9
#define IN2 10
#define IN3 14
#define IN4 15

// Full-step sequence for 28BYJ-48 (4 steps, more torque)
const uint8_t steps[4][4] = {
  {1,1,0,0}, {0,1,1,0}, {0,0,1,1}, {1,0,0,1}
};

int stepIdx = 0;

void stepMotor(int dir) {
  stepIdx = (stepIdx + dir + 4) % 4;
  digitalWrite(IN1, steps[stepIdx][0]);
  digitalWrite(IN2, steps[stepIdx][1]);
  digitalWrite(IN3, steps[stepIdx][2]);
  digitalWrite(IN4, steps[stepIdx][3]);
}

void setup() {
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  u8g2.begin();
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(10, 40, "Stepper Test");
  } while (u8g2.nextPage());
}

void loop() {
  // Spin clockwise continuously
  stepMotor(1);
  delay(5);
}

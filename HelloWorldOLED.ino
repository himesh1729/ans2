#include <U8g2lib.h>
#include <Wire.h>

U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

void setup() {
  u8g2.begin();
  u8g2.firstPage();
  do {
    u8g2.drawFrame(0, 0, 128, 64);
  } while (u8g2.nextPage());
}

void loop() {
}

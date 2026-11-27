#include "display_ui.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

static Adafruit_SSD1306 oled(128, 64, &SPI,
                             Config::PIN_OLED_DC,
                             Config::PIN_OLED_RST,
                             Config::PIN_OLED_CS);

bool DisplayUI::begin() {
  SPI.begin();
  ready_ = oled.begin(SSD1306_SWITCHCAPVCC);
  if (ready_) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("Group 16 Laundry Mon");
    oled.println("ThingSpeak TLS Mode");
    oled.println("Booting...");
    oled.display();
  }
  return ready_;
}

void DisplayUI::showError(const char* line1, const char* line2) {
  if (!ready_) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 10);
  oled.println("HARDWARE ERROR:");
  oled.println(line1);
  oled.println(line2);
  oled.display();
}

void DisplayUI::render(WasherState state,
                       float vib,
                       float startThresh,
                       bool thrLocked,
                       bool wifiOk,
                       bool mqttOk,
                       size_t bufferedCount,
                       uint32_t cyclesCompleted) {
  if (!ready_) return;

  oled.clearDisplay();

  // Top Header: Large State Name
  oled.setTextSize(2);
  oled.setCursor(0, 0);
  oled.print(stateToString(state));

  // Line 2: Live Vibration
  oled.setTextSize(1);
  oled.setCursor(0, 22);
  oled.printf("Vib: %.2f m/s2", vib);

  // Line 3: Threshold (with [L] lock indicator) & Completed Cycles
  oled.setCursor(0, 34);
  oled.printf("Thr:%.2f%s Cyc:%lu",
              startThresh,
              thrLocked ? "[L]" : "   ",
              (unsigned long)cyclesCompleted);

  // Bottom Line: Connectivity & Offline Queue Depth
  oled.setCursor(0, 50);
  oled.printf("W:%s M:%s Q:%u",
              wifiOk ? "OK" : "--",
              mqttOk ? "OK" : "--",
              (unsigned int)bufferedCount);

  oled.display();
}
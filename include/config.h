#pragma once
#include <Arduino.h>

namespace Config {
  // Device & MQTT Topics
  constexpr const char* DEVICE_ID       = "T16";
  constexpr const char* TOPIC_VIBRATION = "iot/T16/vibration";
  constexpr const char* TOPIC_STATE     = "iot/T16/state";
  constexpr const char* TOPIC_STATUS    = "iot/T16/status";

  // Fall 2026 Kit Pin Map
  constexpr int PIN_LED_RED     = 2;
  constexpr int PIN_LED_YELLOW  = 4;
  constexpr int PIN_LED_GREEN   = 15;
  constexpr int PIN_BUTTON      = 32;  // INPUT_PULLUP, active LOW
  constexpr int PIN_POT_ADC     = 34;  // Analog sensitivity dial
  constexpr int PIN_BUZZER      = 27;  // Passive piezo (LEDC ch 0)
  constexpr int PIN_SERVO_PWM   = 13;  // Hobby servo (LEDC ch 2)

  constexpr int PIN_OLED_CS     = 5;
  constexpr int PIN_OLED_DC     = 16;
  constexpr int PIN_OLED_RST    = 17;

  constexpr int PIN_I2C_SDA     = 21;
  constexpr int PIN_I2C_SCL     = 22;
  constexpr uint8_t ADDR_MPU6050   = 0x68;
  constexpr uint8_t ADDR_ATECC608A = 0x60;

  // Servo PWM Configuration (50 Hz, 16-bit)
  constexpr int SERVO_CH         = 2;
  constexpr int SERVO_HZ         = 50;
  constexpr int SERVO_RES_BITS   = 16;
  constexpr int SERVO_BUSY_US    = 600;   // 0 deg -> points to "BUSY"
  constexpr int SERVO_DONE_US    = 2400;  // 180 deg -> points to "DONE / FREE"

  // Sampling & Telemetry Intervals
  constexpr uint32_t SAMPLE_MS           = 20;     // 50 Hz accelerometer sampling
  constexpr uint32_t PUBLISH_INTERVAL_MS = 30000;  // 30 s for 72h run (set to 2000 for fast demo)
  constexpr uint32_t OLED_REFRESH_MS     = 250;    // 4 Hz display update
  constexpr uint32_t WIFI_RETRY_MS       = 10000;  // Non-blocking Wi-Fi retry
  constexpr uint32_t MQTT_RETRY_MS       = 5000;   // Non-blocking MQTT retry
  constexpr uint32_t WDT_TIMEOUT_SEC     = 15;     // Hardware watchdog timeout

  // State Machine Debounce & Cycle Timings
  constexpr uint32_t START_CONFIRM_MS      = 3000;    // 3 s sustained shake to start cycle
  constexpr uint32_t UNBALANCED_CONFIRM_MS = 2000;    // 2 s violent shake for alarm
  constexpr uint32_t DONE_QUIET_MS         = 120000;  // 2 min quiet to ignore rinse pauses
  constexpr uint32_t BUTTON_DEBOUNCE_MS    = 50;

  // Potentiometer Sensitivity Mapping (m/s^2 standard deviation)
  constexpr float MIN_START_THRESH = 0.15f;
  constexpr float MAX_START_THRESH = 1.00f;
  constexpr float UNBALANCED_MULT  = 5.5f;   // Unbalanced = 5.5x start threshold

  // Offline Ring Buffer Size (120 readings = 60 min at 30 s intervals)
  constexpr size_t RING_BUFFER_CAPACITY = 120;
}

enum class WasherState : uint8_t {
  IDLE = 0,
  RUNNING,
  UNBALANCED,
  DONE
};

inline const char* stateToString(WasherState s) {
  switch (s) {
    case WasherState::IDLE:       return "IDLE";
    case WasherState::RUNNING:    return "RUNNING";
    case WasherState::UNBALANCED: return "UNBALANCED";
    case WasherState::DONE:       return "DONE";
    default:                      return "UNKNOWN";
  }
}
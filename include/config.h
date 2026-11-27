#pragma once
#include <Arduino.h>

// Set to 1 for quick bench testing (16 s publish, 10 s quiet-to-DONE).
// Set to 0 before starting the 72-hour unattended reliability run.
#define DRY_RUN_MODE 0

namespace Config {
  constexpr const char* DEVICE_ID = "T16";

  // ThingSpeak MQTT Broker (TLS)
  constexpr const char* MQTT_HOST = "mqtt3.thingspeak.com";
  constexpr uint16_t    MQTT_PORT = 8883;

  // Fall 2026 Kit Pin Map (Servo removed)
  constexpr int PIN_LED_RED     = 2;   // Blinks on MPU6050 hardware error
  constexpr int PIN_LED_YELLOW  = 4;   // Solid ON while RUNNING
  constexpr int PIN_LED_GREEN   = 15;  // Solid ON while IDLE; blinks when DONE
  constexpr int PIN_BUTTON      = 32;  // INPUT_PULLUP, active LOW (short: reset DONE; 2s hold: lock pot)
  constexpr int PIN_POT_ADC     = 34;  // Analog sensitivity dial
  constexpr int PIN_BUZZER      = 27;  // Passive piezo (3-beep chime when DONE)

  constexpr int PIN_OLED_CS     = 5;
  constexpr int PIN_OLED_DC     = 16;
  constexpr int PIN_OLED_RST    = 17;

  constexpr int PIN_I2C_SDA     = 21;
  constexpr int PIN_I2C_SCL     = 22;
  constexpr uint8_t ADDR_MPU6050   = 0x68;
  constexpr uint8_t ADDR_ATECC608A = 0x60;

  // Sampling & Telemetry Intervals
  constexpr uint32_t SAMPLE_MS            = 20;     // 50 Hz accelerometer sampling
  constexpr uint32_t OLED_REFRESH_MS      = 250;    // 4 Hz display update
  constexpr uint32_t WIFI_RETRY_MS        = 10000;  // Non-blocking Wi-Fi retry
  constexpr uint32_t MQTT_RETRY_MS        = 5000;   // Non-blocking MQTT retry
  constexpr uint32_t MIN_PUBLISH_GAP_MS   = 16000;  // ThingSpeak enforces >= 15 s (1 s safety margin)
  constexpr uint32_t TLS_HANDSHAKE_SEC    = 10;     // Bound TLS stall below WDT timeout
  constexpr uint32_t SOCKET_TIMEOUT_SEC   = 5;
  constexpr uint32_t WDT_TIMEOUT_SEC      = 30;     // Hardware watchdog timeout

#if DRY_RUN_MODE
  constexpr uint32_t PUBLISH_INTERVAL_MS  = 16000;  // 16 s fast test
  constexpr uint32_t START_CONFIRM_MS     = 2000;   // 2 s shake to start
  constexpr uint32_t DONE_QUIET_MS        = 10000;  // 10 s quiet to finish
#else
  // 30 s = 8,640 msgs / 72 h (use 35000 if you want all 72 h inside 1 ThingSpeak 8,000-pt chart)
  constexpr uint32_t PUBLISH_INTERVAL_MS  = 30000;
  constexpr uint32_t START_CONFIRM_MS     = 3000;   // 3 s sustained shake to start cycle
  constexpr uint32_t DONE_QUIET_MS        = 180000; // 3 min quiet to ignore rinse/drain pauses
#endif

  // Button Timings
  constexpr uint32_t BUTTON_DEBOUNCE_MS   = 50;
  constexpr uint32_t KNOB_LOCK_HOLD_MS    = 2000;   // Hold 2 s in IDLE to lock/unlock threshold knob

  // Potentiometer Sensitivity Mapping (m/s^2 standard deviation)
  constexpr float MIN_START_THRESH = 0.15f;
  constexpr float MAX_START_THRESH = 1.00f;

  // Offline Ring Buffer Size (240 readings = 2 full hours at 30 s intervals)
  constexpr size_t RING_BUFFER_CAPACITY = 240;

  // Sensor Health Guard
  constexpr uint8_t MAX_CONSECUTIVE_I2C_ERRORS = 25; // 500 ms at 50 Hz before I2C bus reset
}

enum class WasherState : uint8_t {
  IDLE = 0,
  RUNNING = 1,
  DONE = 2
};

inline const char* stateToString(WasherState s) {
  switch (s) {
    case WasherState::IDLE:    return "IDLE";
    case WasherState::RUNNING: return "RUNNING";
    case WasherState::DONE:    return "DONE";
    default:                   return "UNKNOWN";
  }
}
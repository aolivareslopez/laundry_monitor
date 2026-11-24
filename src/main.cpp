#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

#include "config.h"
#include "secrets.h"
#include "ring_buffer.h"
#include "security.h"
#include "display_ui.h"

// Hardware & Network Objects
Adafruit_MPU6050 mpu;
WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);
PayloadSigner signer;
DisplayUI ui;
TelemetryRingBuffer ringBuf;

// State Machine & Timing Variables
WasherState currentState = WasherState::IDLE;
uint32_t stateEnteredMs = 0;
uint32_t lastSampleMs = 0;
uint32_t lastPublishMs = 0;
uint32_t lastOledMs = 0;
uint32_t lastWifiTryMs = 0;
uint32_t lastMqttTryMs = 0;
uint32_t lastOutageStartMs = 0;

// Vibration Window Accumulators (50 Hz)
double accelSum = 0.0;
double accelSqSum = 0.0;
uint32_t sampleCount = 0;
float latestVib = 0.0f;
float startThreshold = 0.35f;

// State Debounce & Cycle Tracking
uint32_t aboveStartSinceMs = 0;
uint32_t lastActiveVibMs = 0;
uint32_t completedCycles = 0;
uint32_t msgSequence = 0;
bool wasWifiConnected = false;
bool wasMqttConnected = false;

// Button Debounce
int lastRawBtn = HIGH;
int stableBtn = HIGH;
uint32_t lastBtnDebounceMs = 0;

// Non-blocking Done Chime
uint8_t doneChimeStep = 0;
uint32_t doneChimeTimerMs = 0;

const char* resetReasonString() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_SW:       return "SOFTWARE";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default:               return "OTHER";
  }
}

String buildSignedJson(const TelemetryRecord& rec) {
  char body[192];
  snprintf(body, sizeof(body),
           "{\"device\":\"%s\",\"seq\":%lu,\"state\":\"%s\",\"vib\":%.3f,"
           "\"thr\":%.2f,\"cycles\":%lu,\"ms\":%lu,\"buf\":%s}",
           Config::DEVICE_ID,
           (unsigned long)rec.seq,
           stateToString(rec.state),
           rec.vib,
           rec.threshold,
           (unsigned long)completedCycles,
           (unsigned long)rec.uptimeMs,
           rec.buffered ? "true" : "false");

  String rawBody(body);
  String sig = signer.signMessage(rawBody);

  // Insert signature before closing brace
  rawBody.remove(rawBody.length() - 1);
  rawBody += ",\"sig\":\"" + sig + "\"}";
  return rawBody;
}

void publishOrBuffer(const char* topic, const TelemetryRecord& rec, bool immediateState) {
  if (mqtt.connected()) {
    String json = buildSignedJson(rec);
    if (mqtt.publish(topic, json.c_str())) {
      Serial.printf("[MQTT -> %s] %s\n", topic, json.c_str());
      return;
    }
  }
  // Network unavailable: queue in RAM ring buffer for 72h reliability
  if (!immediateState) {
    TelemetryRecord queued = rec;
    queued.buffered = true;
    ringBuf.push(queued);
    Serial.printf("[BUF] Queued seq=%lu (size=%u)\n",
                  (unsigned long)rec.seq, (unsigned int)ringBuf.size());
  }
}

void enterState(WasherState nextState) {
  if (currentState == nextState) return;
  currentState = nextState;
  stateEnteredMs = millis();

  noTone(Config::PIN_BUZZER);
  digitalWrite(Config::PIN_LED_RED,    LOW);
  digitalWrite(Config::PIN_LED_YELLOW, LOW);
  digitalWrite(Config::PIN_LED_GREEN,  LOW);

  switch (currentState) {
    case WasherState::IDLE:
      digitalWrite(Config::PIN_LED_GREEN, HIGH);
      break;

    case WasherState::RUNNING:
      digitalWrite(Config::PIN_LED_YELLOW, HIGH);
      lastActiveVibMs = millis();
      break;

    case WasherState::DONE:
      completedCycles++;
      doneChimeStep = 1;
      doneChimeTimerMs = millis();
      break;
  }

  Serial.printf("[STATE] -> %s\n", stateToString(currentState));

  // Publish state transition event immediately
  TelemetryRecord eventRec{
    ++msgSequence, millis(), latestVib, startThreshold, currentState, false
  };
  publishOrBuffer(Config::TOPIC_STATE, eventRec, true);
}

void updatePotentiometerThreshold() {
  static float filteredAdc = analogRead(Config::PIN_POT_ADC);
  int raw = analogRead(Config::PIN_POT_ADC);
  filteredAdc = 0.90f * filteredAdc + 0.10f * raw;

  float ratio = filteredAdc / 4095.0f;
  startThreshold = Config::MIN_START_THRESH +
                   ratio * (Config::MAX_START_THRESH - Config::MIN_START_THRESH);
}

bool checkResetButtonPressed() {
  int reading = digitalRead(Config::PIN_BUTTON);
  if (reading != lastRawBtn) {
    lastBtnDebounceMs = millis();
  }
  lastRawBtn = reading;

  if ((millis() - lastBtnDebounceMs) > Config::BUTTON_DEBOUNCE_MS) {
    if (reading != stableBtn) {
      stableBtn = reading;
      if (stableBtn == LOW) {
        return true;  // Falling edge detected
      }
    }
  }
  return false;
}

void updateActuatorsNonBlocking(uint32_t now) {
  if (currentState == WasherState::DONE) {
    // Blink green LED (500 ms ON / 500 ms OFF) to notify roommates
    digitalWrite(Config::PIN_LED_GREEN, (((now - stateEnteredMs) % 1000) < 500) ? HIGH : LOW);

    // 3-beep completion chime when entering DONE
    if (doneChimeStep > 0) {
      uint32_t dt = now - doneChimeTimerMs;
      if (doneChimeStep <= 5) {
        if (dt >= 180) {
          doneChimeTimerMs = now;
          doneChimeStep++;
          if (doneChimeStep % 2 == 1 && doneChimeStep <= 5) {
            tone(Config::PIN_BUZZER, 1500);
          } else {
            noTone(Config::PIN_BUZZER);
          }
        } else if (doneChimeStep == 1) {
          tone(Config::PIN_BUZZER, 1500);
        }
      } else {
        noTone(Config::PIN_BUZZER);
        doneChimeStep = 0;
      }
    }
  }
}

void connectWifiNonBlocking(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wasWifiConnected) {
      wasWifiConnected = true;
      Serial.printf("[WIFI] Connected, IP=%s\n", WiFi.localIP().toString().c_str());
    }
    return;
  }
  wasWifiConnected = false;
  if (lastWifiTryMs != 0 && (now - lastWifiTryMs) < Config::WIFI_RETRY_MS) return;

  lastWifiTryMs = now;
  Serial.println("[WIFI] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void connectMqttNonBlocking(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) return;

  if (mqtt.connected()) {
    if (!wasMqttConnected) {
      wasMqttConnected = true;
      uint32_t gapMs = (lastOutageStartMs > 0) ? (now - lastOutageStartMs) : 0;
      lastOutageStartMs = 0;

      char statusMsg[128];
      snprintf(statusMsg, sizeof(statusMsg),
               "{\"device\":\"%s\",\"status\":\"online\",\"reset\":\"%s\",\"gap_ms\":%lu,\"dropped\":%lu}",
               Config::DEVICE_ID, resetReasonString(),
               (unsigned long)gapMs, (unsigned long)ringBuf.droppedCount());
      mqtt.publish(Config::TOPIC_STATUS, statusMsg, true);
      Serial.printf("[MQTT] Connected -> %s\n", statusMsg);
    }
    return;
  }

  if (wasMqttConnected) {
    wasMqttConnected = false;
    lastOutageStartMs = now;
    Serial.println("[MQTT] Disconnected! Buffering telemetry...");
  }

  if (lastMqttTryMs != 0 && (now - lastMqttTryMs) < Config::MQTT_RETRY_MS) return;
  lastMqttTryMs = now;

  Serial.print("[MQTT] Connecting to EMQX... ");
  // Configure Last Will and Testament (LWT) on iot/T16/status
  const char* lwtPayload = "{\"device\":\"T16\",\"status\":\"offline\"}";
  if (mqtt.connect("esp32-t16", MQTT_USER, MQTT_PASS,
                   Config::TOPIC_STATUS, 1, true, lwtPayload)) {
    Serial.println("OK");
  } else {
    Serial.printf("FAILED rc=%d\n", mqtt.state());
  }
}

void flushOneBufferedRecord() {
  if (!mqtt.connected() || ringBuf.size() == 0) return;

  TelemetryRecord rec;
  if (ringBuf.peek(rec)) {
    String json = buildSignedJson(rec);
    if (mqtt.publish(Config::TOPIC_VIBRATION, json.c_str())) {
      ringBuf.pop();
      Serial.printf("[FLUSH] Sent buffered seq=%lu (remaining=%u)\n",
                    (unsigned long)rec.seq, (unsigned int)ringBuf.size());
    }
  }
}

void initHardwareWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms = Config::WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdtCfg);
#else
  esp_task_wdt_init(Config::WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== Group 16 Laundry Monitor (Reset: %s) ===\n", resetReasonString());

  // Configure GPIO Outputs & Inputs
  pinMode(Config::PIN_LED_RED, OUTPUT);
  pinMode(Config::PIN_LED_YELLOW, OUTPUT);
  pinMode(Config::PIN_LED_GREEN, OUTPUT);
  pinMode(Config::PIN_BUZZER, OUTPUT);
  pinMode(Config::PIN_BUTTON, INPUT_PULLUP);
  pinMode(Config::PIN_POT_ADC, INPUT);

  // Initialize SPI OLED
  ui.begin();

  // Initialize I2C Bus (MPU-6050 + ATECC608A)
  Wire.begin(Config::PIN_I2C_SDA, Config::PIN_I2C_SCL);
  if (!mpu.begin(Config::ADDR_MPU6050, &Wire)) {
    Serial.println("[ERR] MPU6050 not found at 0x68! Check I2C wiring.");
    ui.showError("MPU6050 not found", "Check SDA=21 SCL=22");
    while (true) {
      digitalWrite(Config::PIN_LED_RED, !digitalRead(Config::PIN_LED_RED));
      delay(300);
    }
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);
  Serial.println("[OK] MPU6050 initialized");

  signer.begin(Wire);

  // Configure TLS with pinned EMQX Root CA & larger MQTT buffer for signed JSON
  tlsClient.setCACert(EMQX_ROOT_CA);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);

  // Start in safest initial state (IDLE / Free) and enable Watchdog
  digitalWrite(Config::PIN_LED_GREEN, HIGH);
  initHardwareWatchdog();

  connectWifiNonBlocking(millis());
}

void loop() {
  esp_task_wdt_reset();  // Feed hardware watchdog
  uint32_t now = millis();

  connectWifiNonBlocking(now);
  connectMqttNonBlocking(now);
  mqtt.loop();

  // Drain 1 buffered offline reading per loop when online
  flushOneBufferedRecord();

  // Read potentiometer & check reset button
  updatePotentiometerThreshold();
  if (checkResetButtonPressed()) {
    if (currentState == WasherState::DONE) {
      Serial.println("[BTN] User reset -> IDLE");
      enterState(WasherState::IDLE);
    }
  }

  // 1. SENSE: Sample accelerometer at 50 Hz (every 20 ms)
  if (now - lastSampleMs >= Config::SAMPLE_MS) {
    lastSampleMs = now;
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);

    double mag = sqrt(a.acceleration.x * a.acceleration.x +
                      a.acceleration.y * a.acceleration.y +
                      a.acceleration.z * a.acceleration.z);
    accelSum += mag;
    accelSqSum += mag * mag;
    sampleCount++;
  }

  // Compute vibration intensity every 1 second (50 samples) for state decisions
  if (sampleCount >= 50) {
    double mean = accelSum / sampleCount;
    latestVib = (float)sqrt(max(0.0, (accelSqSum / sampleCount) - (mean * mean)));
    accelSum = 0.0;
    accelSqSum = 0.0;
    sampleCount = 0;

    // 2. DECIDE: 3-State Machine Transitions (IDLE -> RUNNING -> DONE)
    switch (currentState) {
      case WasherState::IDLE:
        if (latestVib >= startThreshold) {
          if (aboveStartSinceMs == 0) aboveStartSinceMs = now;
          if (now - aboveStartSinceMs >= Config::START_CONFIRM_MS) {
            aboveStartSinceMs = 0;
            enterState(WasherState::RUNNING);
          }
        } else {
          aboveStartSinceMs = 0;
        }
        break;

      case WasherState::RUNNING:
        if (latestVib >= startThreshold) {
          lastActiveVibMs = now;
        }
        if (now - lastActiveVibMs >= Config::DONE_QUIET_MS) {
          enterState(WasherState::DONE);
        }
        break;

      case WasherState::DONE:
        // Wait for user button press to reset to IDLE
        break;
    }
  }

  // 3. ACT: Update non-blocking DONE LED blink & chime
  updateActuatorsNonBlocking(now);

  // 4. CONNECT: Periodic telemetry upload (or buffer if offline)
  if (now - lastPublishMs >= Config::PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;
    TelemetryRecord rec{
      ++msgSequence, now, latestVib, startThreshold, currentState, false
    };
    publishOrBuffer(Config::TOPIC_VIBRATION, rec, false);
  }

  // 5. LOCAL UI: Refresh OLED at 4 Hz
  if (now - lastOledMs >= Config::OLED_REFRESH_MS) {
    lastOledMs = now;
    ui.render(currentState,
              latestVib,
              startThreshold,
              WiFi.status() == WL_CONNECTED,
              mqtt.connected(),
              ringBuf.size(),
              completedCycles);
  }
}
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <time.h>
#include <cmath>

#include "config.h"
#include "secrets.h"
#include "ring_buffer.h"
#include "security.h"
#include "display_ui.h"

// Hardware & Network Objects
Adafruit_MPU6050    mpu;
WiFiClientSecure    tlsClient;
PubSubClient        mqtt(tlsClient);
PayloadSigner       signer;
DisplayUI           ui;
TelemetryRingBuffer ringBuf;

// Preformatted ThingSpeak MQTT publish topic: "channels/<ID>/publish"
char tsPublishTopic[48];

// State Machine & Timers
WasherState currentState      = WasherState::IDLE;
uint32_t    stateEnteredMs    = 0;
uint32_t    lastSampleMs      = 0;
uint32_t    lastPublishMs     = 0;
uint32_t    lastTxMs          = 0;
uint32_t    lastOledMs        = 0;
uint32_t    lastWifiTryMs     = 0;
uint32_t    lastMqttTryMs     = 0;
bool        hasFirstPublished = false;

// Vibration Window Accumulators (50 Hz -> 1 s window)
double   accelSum            = 0.0;
double   accelSqSum          = 0.0;
uint32_t sampleCount         = 0;
uint8_t  consecutiveI2cErrs  = 0;
float    latestVib           = 0.0f;
float    startThreshold      = 0.35f;
bool     thresholdLocked     = false;

// Cycle & Sequence Tracking
uint32_t aboveStartSinceMs = 0;
uint32_t lastActiveVibMs   = 0;
uint32_t completedCycles   = 0;
uint32_t msgSequence       = 0;
int64_t  lastEpochSec      = 0;
bool     wasWifiConnected  = false;
bool     wasMqttConnected  = false;
bool     ntpConfigured     = false;

// Button Debounce & Hold Detection
int      lastRawBtn        = HIGH;
int      stableBtn         = HIGH;
uint32_t lastBtnDebounceMs = 0;
uint32_t btnPressedAtMs    = 0;
bool     holdActionFired   = false;

// Non-blocking 3-Beep Chime State
uint8_t  doneChimeStep     = 0;
uint32_t doneChimeTimerMs  = 0;

// ---------------------------------------------------------------------------
// Diagnostics & Time Helpers
// ---------------------------------------------------------------------------
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

int64_t getMonotonicEpochSec() {
  time_t nowSec = time(nullptr);
  if (nowSec < 1700000000) {
    return 0;  // NTP not synced yet
  }
  int64_t ts = static_cast<int64_t>(nowSec);
  // Guarantee strictly unique timestamps so ThingSpeak never rejects a duplicate created_at
  if (ts <= lastEpochSec) {
    ts = lastEpochSec + 1;
  }
  lastEpochSec = ts;
  return ts;
}

void formatIsoUtc(int64_t epochSec, char* out, size_t outSize) {
  time_t t = static_cast<time_t>(epochSec);
  struct tm utcTm;
  gmtime_r(&t, &utcTm);
  strftime(out, outSize, "%Y-%m-%dT%H:%M:%SZ", &utcTm);
}

// ---------------------------------------------------------------------------
// Telemetry Queue (Producer-Consumer respecting ThingSpeak's 15 s Rate Limit)
// ---------------------------------------------------------------------------
void enqueueTelemetry(uint32_t now) {
  TelemetryRecord rec;
  rec.seq              = ++msgSequence;
  rec.uptimeSec        = now / 1000UL;
  rec.epochSec         = getMonotonicEpochSec();
  rec.vib              = latestVib;
  rec.threshold        = startThreshold;
  rec.state            = currentState;
  rec.cycles           = completedCycles;
  rec.rssi             = (WiFi.status() == WL_CONNECTED)
                           ? static_cast<int8_t>(WiFi.RSSI())
                           : static_cast<int8_t>(-127);
  rec.backlogAtCapture = static_cast<uint16_t>(ringBuf.size());
  rec.buffered         = (!mqtt.connected() || !ringBuf.isEmpty());

  ringBuf.push(rec);

  if (rec.buffered) {
    Serial.printf("[BUF] Queued seq=%lu state=%s vib=%.3f (queue=%u dropped=%lu)\n",
                  (unsigned long)rec.seq,
                  stateToString(rec.state),
                  rec.vib,
                  (unsigned int)ringBuf.size(),
                  (unsigned long)ringBuf.droppedCount());
  }
}

size_t buildThingSpeakPayload(const TelemetryRecord& rec, char* out, size_t outSize) {
  // Canonical fields string (signed by ATECC608A / HMAC fallback before appending &status=)
  char fields[224];
  int len = snprintf(fields, sizeof(fields),
                     "field1=%.3f&field2=%u&field3=%.2f&field4=%lu"
                     "&field5=%lu&field6=%d&field7=%u&field8=%lu",
                     rec.vib,
                     static_cast<unsigned int>(rec.state),
                     rec.threshold,
                     (unsigned long)rec.cycles,
                     (unsigned long)rec.seq,
                     static_cast<int>(rec.rssi),
                     static_cast<unsigned int>(rec.backlogAtCapture),
                     (unsigned long)rec.uptimeSec);

  if (len < 0 || static_cast<size_t>(len) >= sizeof(fields)) {
    return 0;
  }

  if (rec.epochSec > 0) {
    char isoTime[24];
    formatIsoUtc(rec.epochSec, isoTime, sizeof(isoTime));
    len += snprintf(fields + len, sizeof(fields) - len, "&created_at=%s", isoTime);
  }

  String sig = signer.signMessage(fields, static_cast<size_t>(len));

  // ThingSpeak status field supports up to 255 chars; avoid '&', '=', or spaces inside status
  int total = snprintf(out, outSize,
                       "%s&status=S:%s|B:%u|D:%lu|R:%s|M:%s|SIG:%s",
                       fields,
                       stateToString(rec.state),
                       rec.buffered ? 1U : 0U,
                       (unsigned long)ringBuf.droppedCount(),
                       resetReasonString(),
                       signer.methodTag(),
                       sig.c_str());

  if (total < 0 || static_cast<size_t>(total) >= outSize) {
    return 0;
  }
  return static_cast<size_t>(total);
}

void processTelemetryQueue(uint32_t now) {
  if (!mqtt.connected() || ringBuf.isEmpty()) return;

  // Enforce >= 16 s spacing between ALL ThingSpeak publishes (live, state changes, & backlog)
  if (lastTxMs != 0 && (now - lastTxMs) < Config::MIN_PUBLISH_GAP_MS) {
    return;
  }

  TelemetryRecord rec;
  if (!ringBuf.peek(rec)) return;

  char payload[440];
  size_t payloadLen = buildThingSpeakPayload(rec, payload, sizeof(payload));
  if (payloadLen == 0) {
    Serial.println("[ERR] Payload exceeded buffer; dropping malformed record.");
    ringBuf.pop();
    return;
  }

  if (mqtt.publish(tsPublishTopic, payload)) {
    ringBuf.pop();
    lastTxMs = now;
    Serial.printf("[TX] seq=%lu state=%s vib=%.3f rssi=%d buf=%u rem=%u\n",
                  (unsigned long)rec.seq,
                  stateToString(rec.state),
                  rec.vib,
                  static_cast<int>(rec.rssi),
                  rec.buffered ? 1U : 0U,
                  (unsigned int)ringBuf.size());
  } else {
    Serial.printf("[TX] Publish failed (rc=%d), keeping seq=%lu in queue\n",
                  mqtt.state(), (unsigned long)rec.seq);
    lastTxMs = now;  // Back off before retrying
  }
}

// ---------------------------------------------------------------------------
// State Machine, Inputs & Actuators
// ---------------------------------------------------------------------------
void enterState(WasherState nextState, uint32_t now) {
  if (currentState == nextState) return;
  currentState   = nextState;
  stateEnteredMs = now;

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
      lastActiveVibMs = now;
      break;

    case WasherState::DONE:
      completedCycles++;
      doneChimeStep    = 1;
      doneChimeTimerMs = now;
      tone(Config::PIN_BUZZER, 1500);  // Start beep 1 once (no per-loop retrigger)
      break;
  }

  Serial.printf("[STATE] -> %s (cycles=%lu)\n",
                stateToString(currentState), (unsigned long)completedCycles);

  // Always enqueue state transition so it is never lost offline or rate-limited out
  enqueueTelemetry(now);
}

void updatePotentiometerThreshold() {
  static float filteredAdc = static_cast<float>(analogRead(Config::PIN_POT_ADC));
  if (thresholdLocked) return;

  int raw = analogRead(Config::PIN_POT_ADC);
  filteredAdc = 0.90f * filteredAdc + 0.10f * static_cast<float>(raw);

  float ratio = filteredAdc / 4095.0f;
  startThreshold = Config::MIN_START_THRESH +
                   ratio * (Config::MAX_START_THRESH - Config::MIN_START_THRESH);
}

void handleButton(uint32_t now) {
  int reading = digitalRead(Config::PIN_BUTTON);
  if (reading != lastRawBtn) {
    lastBtnDebounceMs = now;
  }
  lastRawBtn = reading;

  if ((now - lastBtnDebounceMs) > Config::BUTTON_DEBOUNCE_MS) {
    if (reading != stableBtn) {
      stableBtn = reading;
      if (stableBtn == LOW) {
        btnPressedAtMs  = now;
        holdActionFired = false;
      } else {
        // Button released: treat short press in DONE as cycle reset
        if (!holdActionFired && currentState == WasherState::DONE) {
          Serial.println("[BTN] Short press -> Reset DONE to IDLE");
          enterState(WasherState::IDLE, now);
        }
      }
    }
  }

  // Long hold (2 s) in IDLE toggles potentiometer lock (STRIDE Tampering mitigation)
  if (stableBtn == LOW && !holdActionFired && currentState == WasherState::IDLE) {
    if ((now - btnPressedAtMs) >= Config::KNOB_LOCK_HOLD_MS) {
      holdActionFired = true;
      thresholdLocked = !thresholdLocked;
      Serial.printf("[BTN] Threshold knob %s (%.2f m/s2)\n",
                    thresholdLocked ? "LOCKED" : "UNLOCKED", startThreshold);
    }
  }
}

void updateActuatorsNonBlocking(uint32_t now) {
  if (currentState != WasherState::DONE) return;

  // Blink green LED (500 ms ON / 500 ms OFF)
  digitalWrite(Config::PIN_LED_GREEN, (((now - stateEnteredMs) % 1000) < 500) ? HIGH : LOW);

  // 3-beep completion chime (steps: 1=ON, 2=OFF, 3=ON, 4=OFF, 5=ON, 6=DONE)
  if (doneChimeStep > 0 && (now - doneChimeTimerMs) >= 180) {
    doneChimeTimerMs = now;
    doneChimeStep++;
    if (doneChimeStep == 3 || doneChimeStep == 5) {
      tone(Config::PIN_BUZZER, 1500);
    } else {
      noTone(Config::PIN_BUZZER);
      if (doneChimeStep >= 6) {
        doneChimeStep = 0;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Networking (Non-blocking Wi-Fi, NTP, and ThingSpeak MQTT over TLS)
// ---------------------------------------------------------------------------
void connectWifiNonBlocking(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wasWifiConnected) {
      wasWifiConnected = true;
      Serial.printf("[WIFI] Connected, IP=%s RSSI=%d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (!ntpConfigured) {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        ntpConfigured = true;
        Serial.println("[NTP] Sync requested (UTC)");
      }
    }
    return;
  }

  if (wasWifiConnected) {
    wasWifiConnected = false;
    Serial.println("[WIFI] Disconnected; buffering telemetry...");
  }

  if (lastWifiTryMs != 0 && (now - lastWifiTryMs) < Config::WIFI_RETRY_MS) return;

  lastWifiTryMs = now;
  Serial.println("[WIFI] Reconnecting...");
  WiFi.disconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void connectMqttNonBlocking(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) return;

  if (mqtt.connected()) {
    if (!wasMqttConnected) {
      wasMqttConnected = true;
      Serial.println("[MQTT] Connected to ThingSpeak (TLS 8883)");
    }
    return;
  }

  if (wasMqttConnected) {
    wasMqttConnected = false;
    Serial.println("[MQTT] Disconnected from ThingSpeak; buffering telemetry...");
  }

  if (lastMqttTryMs != 0 && (now - lastMqttTryMs) < Config::MQTT_RETRY_MS) return;
  lastMqttTryMs = now;

  Serial.print("[MQTT] Connecting to ThingSpeak... ");
  esp_task_wdt_reset();  // Prevent WDT trip during bounded TLS handshake

  // ThingSpeak MQTT requires exact Device ClientID/Username/Password and CleanSession=true (no LWT)
  bool ok = mqtt.connect(TS_MQTT_CLIENT_ID, TS_MQTT_USER, TS_MQTT_PASS);
  esp_task_wdt_reset();

  if (ok) {
    Serial.println("OK");
  } else {
    Serial.printf("FAILED rc=%d\n", mqtt.state());
  }
}

// ---------------------------------------------------------------------------
// Watchdog & Sensor Sampling
// ---------------------------------------------------------------------------
void initHardwareWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms     = Config::WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic  = true
  };
  esp_task_wdt_init(&wdtCfg);
#else
  esp_task_wdt_init(Config::WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);
}

bool initMpu6050() {
  if (!mpu.begin(Config::ADDR_MPU6050, &Wire)) {
    return false;
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);
  return true;
}

void sampleAccelerometer(uint32_t now) {
  if ((now - lastSampleMs) < Config::SAMPLE_MS) return;
  lastSampleMs = now;

  sensors_event_t a, g, t;
  bool ok = mpu.getEvent(&a, &g, &t);

  bool valid = ok &&
               !std::isnan(a.acceleration.x) &&
               !std::isnan(a.acceleration.y) &&
               !std::isnan(a.acceleration.z);

  if (!valid) {
    consecutiveI2cErrs++;
    if (consecutiveI2cErrs >= Config::MAX_CONSECUTIVE_I2C_ERRORS) {
      Serial.println("[WARN] MPU6050 I2C glitch; attempting bus recovery...");
      digitalWrite(Config::PIN_LED_RED, HIGH);
      Wire.end();
      Wire.begin(Config::PIN_I2C_SDA, Config::PIN_I2C_SCL);
      if (initMpu6050()) {
        Serial.println("[OK] MPU6050 recovered");
        digitalWrite(Config::PIN_LED_RED, LOW);
      }
      consecutiveI2cErrs = 0;
    }
    return;
  }

  consecutiveI2cErrs = 0;
  double mag = sqrt(static_cast<double>(a.acceleration.x) * a.acceleration.x +
                    static_cast<double>(a.acceleration.y) * a.acceleration.y +
                    static_cast<double>(a.acceleration.z) * a.acceleration.z);
  accelSum   += mag;
  accelSqSum += mag * mag;
  sampleCount++;
}

// ---------------------------------------------------------------------------
// Arduino Setup & Loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== Group 16 Laundry Monitor (Reset: %s) ===\n", resetReasonString());

  snprintf(tsPublishTopic, sizeof(tsPublishTopic), "channels/%s/publish", TS_CHANNEL_ID);

  // GPIO
  pinMode(Config::PIN_LED_RED,    OUTPUT);
  pinMode(Config::PIN_LED_YELLOW, OUTPUT);
  pinMode(Config::PIN_LED_GREEN,  OUTPUT);
  pinMode(Config::PIN_BUZZER,     OUTPUT);
  pinMode(Config::PIN_BUTTON,     INPUT_PULLUP);
  pinMode(Config::PIN_POT_ADC,    INPUT);

  ui.begin();

  // I2C Bus (MPU-6050 + ATECC608A)
  Wire.begin(Config::PIN_I2C_SDA, Config::PIN_I2C_SCL);
  if (!initMpu6050()) {
    Serial.println("[ERR] MPU6050 not found at 0x68! Check I2C wiring.");
    ui.showError("MPU6050 not found", "Check SDA=21 SCL=22");
    while (true) {
      digitalWrite(Config::PIN_LED_RED, !digitalRead(Config::PIN_LED_RED));
      delay(300);
    }
  }
  Serial.println("[OK] MPU6050 initialized");

  signer.begin(Wire);

  // Wi-Fi configuration (prevent flash wear across 72 h)
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // TLS with pinned ThingSpeak Root CA & bounded timeouts (publish-only, OTA disabled)
  tlsClient.setCACert(THINGSPEAK_ROOT_CA);
  tlsClient.setHandshakeTimeout(Config::TLS_HANDSHAKE_SEC);
  tlsClient.setTimeout(Config::SOCKET_TIMEOUT_SEC);

  mqtt.setServer(Config::MQTT_HOST, Config::MQTT_PORT);
  mqtt.setSocketTimeout(Config::SOCKET_TIMEOUT_SEC);
  mqtt.setBufferSize(512);

  // Initialize threshold from current knob reading and enter IDLE
  updatePotentiometerThreshold();
  digitalWrite(Config::PIN_LED_GREEN, HIGH);

  initHardwareWatchdog();
  connectWifiNonBlocking(millis());
  lastPublishMs = millis();
}

void loop() {
  esp_task_wdt_reset();
  uint32_t now = millis();

  // Connectivity & rate-limited ThingSpeak queue consumer
  connectWifiNonBlocking(now);
  connectMqttNonBlocking(now);
  if (mqtt.connected()) {
    mqtt.loop();
  }
  processTelemetryQueue(now);

  // Local inputs
  updatePotentiometerThreshold();
  handleButton(now);

  // 1. SENSE: 50 Hz accelerometer sampling
  sampleAccelerometer(now);

  // Compute vibration intensity every 1 s (50 samples) for state decisions
  if (sampleCount >= 50) {
    double mean = accelSum / sampleCount;
    latestVib   = static_cast<float>(sqrt(max(0.0, (accelSqSum / sampleCount) - (mean * mean))));
    accelSum    = 0.0;
    accelSqSum  = 0.0;
    sampleCount = 0;

    // Enqueue an immediate boot confirmation reading once first 1 s sample is ready
    if (!hasFirstPublished && mqtt.connected()) {
      hasFirstPublished = true;
      lastPublishMs     = now;
      enqueueTelemetry(now);
    }

    // 2. DECIDE: 3-State Machine (IDLE -> RUNNING -> DONE)
    switch (currentState) {
      case WasherState::IDLE:
        if (latestVib >= startThreshold) {
          if (aboveStartSinceMs == 0) aboveStartSinceMs = now;
          if ((now - aboveStartSinceMs) >= Config::START_CONFIRM_MS) {
            aboveStartSinceMs = 0;
            enterState(WasherState::RUNNING, now);
          }
        } else {
          aboveStartSinceMs = 0;
        }
        break;

      case WasherState::RUNNING:
        if (latestVib >= startThreshold) {
          lastActiveVibMs = now;
        }
        if ((now - lastActiveVibMs) >= Config::DONE_QUIET_MS) {
          enterState(WasherState::DONE, now);
        }
        break;

      case WasherState::DONE:
        // Stay in DONE until user short-presses the reset button
        break;
    }
  }

  // 3. ACT: Non-blocking DONE LED blink & 3-beep chime
  updateActuatorsNonBlocking(now);

  // 4. CONNECT: Periodic telemetry capture (pushed to FIFO queue)
  if ((now - lastPublishMs) >= Config::PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;
    enqueueTelemetry(now);
  }

  // 5. LOCAL UI: Refresh OLED at 4 Hz
  if ((now - lastOledMs) >= Config::OLED_REFRESH_MS) {
    lastOledMs = now;
    ui.render(currentState,
              latestVib,
              startThreshold,
              thresholdLocked,
              WiFi.status() == WL_CONNECTED,
              mqtt.connected(),
              ringBuf.size(),
              completedCycles);
  }
}
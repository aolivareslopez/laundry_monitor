// Laundry Machine Monitor - CS147 Group 16
// ESP32 DevKit V1 + MPU-6050. Publishes to EMQX over MQTT/TLS.
//
// Built on our vertical slice. Added:
//   - state machine (IDLE / RUNNING / UNBALANCED / DONE) = the decision
//   - NTP timestamps, sequence numbers, boot counter (to explain gaps)
//   - ring buffer: readings are kept while offline and sent later with "bf":1
//   - HMAC-SHA256 signature on every message
//   - heartbeat, last-will "offline" message, task watchdog

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "mbedtls/md.h"
#include "secrets.h"

const int I2C_SDA = 21, I2C_SCL = 22;
const int LED_GREEN = 15;

Adafruit_MPU6050 mpu;
WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

// ---------- Sampling ----------
const uint32_t SAMPLE_MS = 20;    // 50 Hz sampling
const uint32_t PUBLISH_MS = 2000; // one reading every 2 s

// ---------- Decision thresholds: tune these from a real wash cycle ----------
const float RUN_THR = 0.15;        // m/s^2, above this the machine counts as running
const float UNBAL_THR = 2.00;      // m/s^2, above this the load is unbalanced
const int START_WINDOWS = 15;      // ~30 s of vibration before RUNNING
const int UNBAL_WINDOWS = 3;       // 6 s of heavy shaking before UNBALANCED
const int CALM_WINDOWS = 15;       // 30 s below UNBAL_THR clears UNBALANCED
const uint32_t DONE_QUIET_S = 300; // quiet this long while RUNNING => DONE
const uint32_t DONE_HOLD_S = 1800; // DONE shows for 30 min, then back to IDLE

// ---------- MQTT ----------
const char *CLIENT_ID = "esp32-t16";
const char *TOPIC_DATA = "iot/T16/vibration";
const char *TOPIC_HB = "iot/T16/heartbeat";
const char *TOPIC_STATUS = "iot/T16/status";
const char *WILL_MSG = "{\"device\":\"T16\",\"online\":false}";
const uint32_t HEARTBEAT_MS = 60000;
const int WDT_TIMEOUT_S = 60; // reboot if loop() is stuck this long

enum State : uint8_t
{
  IDLE,
  RUNNING,
  UNBALANCED,
  DONE
};
const char *STATE_NAMES[] = {"IDLE", "RUNNING", "UNBALANCED", "DONE"};

// ---------- Ring buffer of unsent readings ----------
struct Record
{
  uint32_t seq, ms, epoch;
  float vib;
  uint8_t state;
};
const int BUF_LEN = 1800; // 1 hour at one reading per 2 s
Record buf[BUF_LEN];
int bufHead = 0, bufCount = 0;
uint32_t dropped = 0; // readings lost because the buffer was full

void bufPush(const Record &r)
{
  if (bufCount == BUF_LEN)
  { // full: drop the oldest
    bufHead = (bufHead + 1) % BUF_LEN;
    bufCount--;
    dropped++;
  }
  buf[(bufHead + bufCount) % BUF_LEN] = r;
  bufCount++;
}

// ---------- Globals ----------
uint32_t lastSample = 0, lastPublish = 0, lastMqttTry = 0, lastWifiOk = 0, lastHeartbeat = 0;
double sum = 0, sumSq = 0;
uint32_t n = 0;
bool wasWifi = false;
uint32_t seq = 0, bootCount = 0, wifiDrops = 0, mqttConnects = 0;

State state = IDLE;
int runScore = 0, unbalCount = 0, calmCount = 0;
uint32_t quietMs = 0, doneSince = 0;

// ---------- Helpers ----------
uint32_t epochNow()
{
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0; // 0 until NTP has synced
}

const char *resetReason()
{
  switch (esp_reset_reason())
  {
  case ESP_RST_POWERON:
    return "power-on";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "crash";
  case ESP_RST_INT_WDT:
    return "int-watchdog";
  case ESP_RST_TASK_WDT:
    return "task-watchdog";
  case ESP_RST_WDT:
    return "watchdog";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_EXT:
    return "reset-pin";
  default:
    return "other";
  }
}

// HMAC-SHA256 of msg as 64 hex chars
void hmacHex(const char *msg, size_t len, char out[65])
{
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY),
                  (const uint8_t *)msg, len, mac);
  for (int i = 0; i < 32; i++)
    sprintf(out + 2 * i, "%02x", mac[i]);
}

// body = JSON object WITHOUT its closing brace. Appends ,"sig":"..."} and publishes.
// Server check: split at the last ,"sig":" -> the left part is exactly what was signed.
bool publishSigned(const char *topic, const char *body, int len, bool retained = false)
{
  char sig[65], payload[400];
  hmacHex(body, len, sig);
  snprintf(payload, sizeof payload, "%s,\"sig\":\"%s\"}", body, sig);
  bool ok = mqtt.publish(topic, payload, retained);
  Serial.printf("%s  %s\n", payload, ok ? "[sent]" : "[NOT sent]");
  return ok;
}

// ---------- Decision ----------
void updateState(float vib)
{
  bool active = vib > RUN_THR;
  runScore = active ? min(runScore + 1, START_WINDOWS) : max(runScore - 1, 0);
  quietMs = active ? 0 : quietMs + PUBLISH_MS;
  unbalCount = vib > UNBAL_THR ? unbalCount + 1 : 0;
  calmCount = vib > UNBAL_THR ? 0 : calmCount + 1;

  State prev = state;
  switch (state)
  {
  case IDLE:
    if (runScore >= START_WINDOWS)
      state = RUNNING;
    break;
  case RUNNING:
    if (unbalCount >= UNBAL_WINDOWS)
      state = UNBALANCED;
    else if (quietMs >= DONE_QUIET_S * 1000UL)
    {
      state = DONE;
      doneSince = millis();
      runScore = 0;
    }
    break;
  case UNBALANCED:
    if (calmCount >= CALM_WINDOWS)
    {
      state = RUNNING;
      quietMs = 0;
    }
    break;
  case DONE:
    if (runScore >= START_WINDOWS)
      state = RUNNING; // new load started
    else if (millis() - doneSince >= DONE_HOLD_S * 1000UL)
      state = IDLE;
    break;
  }
  if (state != prev)
    Serial.printf("State: %s -> %s\n", STATE_NAMES[prev], STATE_NAMES[state]);
}

// ---------- Network ----------
void connectWifi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (!wasWifi)
    {
      wasWifi = true;
      Serial.printf("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
    }
    lastWifiOk = millis();
    return;
  }
  if (wasWifi)
  {
    wasWifi = false;
    wifiDrops++;
    Serial.println("WiFi lost");
  }
  if (millis() - lastWifiOk < 30000)
    return; // give auto-reconnect 30 s, then restart Wi-Fi
  lastWifiOk = millis();
  Serial.println("WiFi reconnecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void connectMqtt()
{
  if (WiFi.status() != WL_CONNECTED || mqtt.connected())
    return;
  if (lastMqttTry != 0 && millis() - lastMqttTry < 5000)
    return; // non-blocking retry
  lastMqttTry = millis();
  esp_task_wdt_reset();
  Serial.print("MQTT connecting... ");
  if (mqtt.connect(CLIENT_ID, MQTT_USER, MQTT_PASS, TOPIC_STATUS, 1, true, WILL_MSG))
  {
    Serial.println("ok");
    mqttConnects++;
    char body[200];
    int len = snprintf(body, sizeof body,
                       "{\"device\":\"T16\",\"online\":true,\"boot\":%lu,\"reset\":\"%s\",\"uptime\":%lu,\"ts\":%lu",
                       (unsigned long)bootCount, resetReason(), (unsigned long)(millis() / 1000),
                       (unsigned long)epochNow());
    publishSigned(TOPIC_STATUS, body, len, true);
  }
  else
  {
    Serial.printf("failed rc=%d\n", mqtt.state());
  }
}

// Send buffered readings, oldest first, up to 10 per loop pass.
void flushBuffer()
{
  for (int i = 0; i < 10 && bufCount > 0 && mqtt.connected(); i++)
  {
    const Record &r = buf[bufHead];
    uint32_t now = millis();
    uint32_t ts = r.epoch;
    if (ts == 0)
    { // recorded before NTP synced: back-calculate
      uint32_t e = epochNow();
      if (e)
        ts = e - (now - r.ms) / 1000;
    }
    char body[256];
    int len = snprintf(body, sizeof body,
                       "{\"device\":\"T16\",\"boot\":%lu,\"seq\":%lu,\"ts\":%lu,\"ms\":%lu,\"vib\":%.3f,"
                       "\"state\":\"%s\",\"bf\":%d",
                       (unsigned long)bootCount, (unsigned long)r.seq, (unsigned long)ts, (unsigned long)r.ms,
                       r.vib, STATE_NAMES[r.state], (now - r.ms > 5000) ? 1 : 0);
    bool sent = publishSigned(TOPIC_DATA, body, len);
    digitalWrite(LED_GREEN, sent ? HIGH : LOW); // green = last publish succeeded
    if (!sent)
      return; // keep it, retry next pass
    bufHead = (bufHead + 1) % BUF_LEN;
    bufCount--;
  }
}

void sendHeartbeat()
{
  if (!mqtt.connected() || (lastHeartbeat != 0 && millis() - lastHeartbeat < HEARTBEAT_MS))
    return;
  lastHeartbeat = millis();
  char body[300];
  int len = snprintf(body, sizeof body,
                     "{\"device\":\"T16\",\"boot\":%lu,\"ts\":%lu,\"uptime\":%lu,\"rssi\":%d,\"heap\":%u,"
                     "\"wifiDrops\":%lu,\"mqttConnects\":%lu,\"buffered\":%d,\"dropped\":%lu,\"reset\":\"%s\","
                     "\"runThr\":%.2f,\"unbalThr\":%.2f",
                     (unsigned long)bootCount, (unsigned long)epochNow(), (unsigned long)(millis() / 1000),
                     (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), (unsigned long)wifiDrops,
                     (unsigned long)mqttConnects, bufCount, (unsigned long)dropped, resetReason(),
                     RUN_THR, UNBAL_THR);
  publishSigned(TOPIC_HB, body, len);
}

// ---------- Setup / loop ----------
void setup()
{
  Serial.begin(115200);
  delay(500);
  pinMode(LED_GREEN, OUTPUT);

  Preferences prefs; // boot counter survives power loss
  prefs.begin("t16", false);
  bootCount = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", bootCount);
  prefs.end();
  Serial.printf("\nBoot #%lu, reset reason: %s\n", (unsigned long)bootCount, resetReason());

  Wire.begin(I2C_SDA, I2C_SCL);
  while (!mpu.begin(0x68, &Wire))
  { // retry instead of hanging forever
    Serial.println("MPU6050 not found, check wiring");
    delay(2000);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);
  Serial.println("MPU6050 ready");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiOk = millis();
  configTime(0, 0, "pool.ntp.org", "time.google.com"); // UTC

#ifdef MQTT_ROOT_CA
  tlsClient.setCACert(MQTT_ROOT_CA); // verify the EMQX server certificate
#else
  tlsClient.setInsecure(); // encrypted but unverified: add the CA before the demo
#endif
  tlsClient.setHandshakeTimeout(15);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512); // default 256 is too small for signed payloads
  mqtt.setKeepAlive(30);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt = {.timeout_ms = WDT_TIMEOUT_S * 1000, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

void loop()
{
  esp_task_wdt_reset();
  connectWifi();
  connectMqtt();
  mqtt.loop();

  uint32_t now = millis();

  if (now - lastSample >= SAMPLE_MS)
  {
    lastSample = now;
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    double mag = sqrt(a.acceleration.x * a.acceleration.x +
                      a.acceleration.y * a.acceleration.y +
                      a.acceleration.z * a.acceleration.z);
    sum += mag;
    sumSq += mag * mag;
    n++;
  }

  if (now - lastPublish >= PUBLISH_MS && n > 0)
  {
    lastPublish = now;
    // Std deviation of |a| = vibration intensity (gravity's constant offset drops out)
    double mean = sum / n;
    float vib = sqrt(max(0.0, sumSq / n - mean * mean));
    sum = sumSq = 0;
    n = 0;

    updateState(vib);
    bufPush({++seq, now, epochNow(), vib, (uint8_t)state});
  }

  flushBuffer();
  sendHeartbeat();
}

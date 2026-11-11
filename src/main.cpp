#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "secrets.h"

const int I2C_SDA = 21, I2C_SCL = 22;
const int LED_GREEN = 15;

Adafruit_MPU6050 mpu;
WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

const uint32_t SAMPLE_MS = 20;      // 50 Hz sampling
const uint32_t PUBLISH_MS = 2000;   // publish every 2 s
uint32_t lastSample = 0, lastPublish = 0, lastMqttTry = 0, lastWifiTry = 0;
double sum = 0, sumSq = 0;
uint32_t n = 0;
bool wasWifi = false;

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wasWifi) {
      wasWifi = true;
      Serial.printf("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
    }
    return;
  }
  wasWifi = false;
  if (millis() - lastWifiTry < 10000 && lastWifiTry != 0) return;  // retry every 10 s
  lastWifiTry = millis();
  Serial.println("WiFi connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - lastMqttTry < 5000) return;   // non-blocking retry
  lastMqttTry = millis();
  Serial.print("MQTT connecting... ");
  if (mqtt.connect("esp32-t16", MQTT_USER, MQTT_PASS)) {
    Serial.println("ok");
  } else {
    Serial.printf("failed rc=%d\n", mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_GREEN, OUTPUT);

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!mpu.begin(0x68, &Wire)) {
    Serial.println("MPU6050 not found, check wiring");
    while (true) delay(1000);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);
  Serial.println("MPU6050 ready");

  tlsClient.setInsecure();   // fine for the slice; pin the HiveMQ root CA later
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  connectWifi();
}

void loop() {
  connectWifi();
  connectMqtt();
  mqtt.loop();

  uint32_t now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    double mag = sqrt(a.acceleration.x * a.acceleration.x +
                      a.acceleration.y * a.acceleration.y +
                      a.acceleration.z * a.acceleration.z);
    sum += mag; sumSq += mag * mag; n++;
  }

  if (now - lastPublish >= PUBLISH_MS && n > 0) {
    lastPublish = now;
    // Std deviation of |a| = vibration intensity (gravity's constant offset drops out)
    double mean = sum / n;
    double vib = sqrt(max(0.0, sumSq / n - mean * mean));
    sum = sumSq = 0; n = 0;

    char payload[96];
    snprintf(payload, sizeof payload,
             "{\"device\":\"T16\",\"vib\":%.3f,\"ms\":%lu}", vib, (unsigned long)now);

    bool sent = mqtt.connected() && mqtt.publish("iot/T16/vibration", payload);
    Serial.printf("%s  %s\n", payload, sent ? "[sent]" : "[NOT sent]");
    digitalWrite(LED_GREEN, sent ? HIGH : LOW);   // green = last publish succeeded
  }
}
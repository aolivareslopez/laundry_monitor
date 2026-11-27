#pragma once
#include <Arduino.h>
#include <Wire.h>

class PayloadSigner {
public:
  bool begin(TwoWire& wire = Wire);
  String signMessage(const char* data, size_t len);
  String signMessage(const String& message);
  bool isHardwareReady() const { return hwReady_; }
  const char* methodTag() const { return hwReady_ ? "ATECC" : "HMAC"; }

private:
  bool hwReady_ = false;
  static String toHex(const uint8_t* bytes, size_t len);
};
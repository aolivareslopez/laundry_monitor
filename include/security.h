#pragma once
#include <Arduino.h>
#include <Wire.h>

class PayloadSigner {
public:
  bool begin(TwoWire& wire = Wire);
  String signMessage(const String& message);
  bool isHardwareReady() const { return hwReady_; }

private:
  bool hwReady_ = false;
  static String toHex(const uint8_t* bytes, size_t len);
};
#pragma once
#include <Arduino.h>
#include "config.h"

class DisplayUI {
public:
  bool begin();
  void render(WasherState state,
              float vib,
              float startThresh,
              bool wifiOk,
              bool mqttOk,
              size_t bufferedCount,
              uint32_t cyclesCompleted);
  void showError(const char* line1, const char* line2);

private:
  bool ready_ = false;
};
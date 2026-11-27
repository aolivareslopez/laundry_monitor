#pragma once
#include <Arduino.h>
#include "config.h"

struct TelemetryRecord {
  uint32_t    seq;
  uint32_t    uptimeSec;
  int64_t     epochSec;       // UTC epoch seconds from NTP (0 if not synced yet)
  float       vib;
  float       threshold;
  WasherState state;
  uint32_t    cycles;         // Captured at sample time (never mutated later)
  int8_t      rssi;           // Wi-Fi RSSI in dBm (-127 if disconnected)
  uint16_t    backlogAtCapture;
  bool        buffered;       // True if captured while MQTT was offline or backlogged
};

class TelemetryRingBuffer {
public:
  void push(const TelemetryRecord& rec) {
    buffer_[head_] = rec;
    head_ = (head_ + 1) % Config::RING_BUFFER_CAPACITY;
    if (count_ < Config::RING_BUFFER_CAPACITY) {
      count_++;
    } else {
      tail_ = (tail_ + 1) % Config::RING_BUFFER_CAPACITY;
      dropped_++;
    }
  }

  bool peek(TelemetryRecord& out) const {
    if (count_ == 0) return false;
    out = buffer_[tail_];
    return true;
  }

  void pop() {
    if (count_ == 0) return;
    tail_ = (tail_ + 1) % Config::RING_BUFFER_CAPACITY;
    count_--;
  }

  bool     isEmpty()      const { return count_ == 0; }
  size_t   size()         const { return count_; }
  uint32_t droppedCount() const { return dropped_; }

private:
  TelemetryRecord buffer_[Config::RING_BUFFER_CAPACITY];
  size_t   head_    = 0;
  size_t   tail_    = 0;
  size_t   count_   = 0;
  uint32_t dropped_ = 0;
};